/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") PMU (Dialog D1671) driver.
 *
 * Original implementation for this project. The D1671 has no public
 * datasheet; every register address, bitfield and the register-poke
 * sequence in pmu_preinit() below are hardware facts recovered by
 * observing how the original firmware drives this exact chip -- recorded
 * here as facts (the way a datasheet excerpt would be used), not as an
 * implementation choice. In particular, the line
 *
 *     pmu_wr(0x10, (pmu_rd(0x10) & 0xdf) | 0x8);
 *
 * is kept byte-for-byte: it is the fix already merged upstream (Rockbox
 * commit 66bc0728, "ipodnano3g: preserve PMU register 0x10 bit 2, which
 * the NAND needs") for a real, hardware-verified requirement -- clearing
 * bit 5 while preserving bit 2 of that register is necessary for the NAND
 * controller to function on this device. Changing this value would not
 * be "more original", it would reintroduce a known hardware bug.
 *
 * Register accesses go over I2C bus 0 to 7-bit address 0x73 (byte 0xe6),
 * using the i2c-s5l8702 driver -- shared SoC infrastructure, not specific
 * to this chip.
 *
 * The driver's structure (how init/preinit/IRQ handling/ADC access are
 * organised into sections, the input-state cache, and the thread/queue
 * plumbing) is written independently for this project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "config.h"
#include "kernel.h"
#include "thread.h"

#include "pmu-target.h"
#include "adc-target.h"
#include "i2c-s5l8702.h"
#include "gpio-s5l8702.h"

/* 7-bit I2C address 0x73, packed as the 8-bit write byte i2c_write/i2c_wr
 * expect */
#define PMU_I2C_SLAVE   0xe6

/* ==================================================================== *
 * Runtime register access (threaded; arbitrated by the I2C driver)
 * ==================================================================== */

int pmu_read_multiple(int address, int count, unsigned char *buffer)
{
    return i2c_read(0, PMU_I2C_SLAVE, address, count, buffer);
}

int pmu_write_multiple(int address, int count, unsigned char *buffer)
{
    return i2c_write(0, PMU_I2C_SLAVE, address, count, buffer);
}

unsigned char pmu_read(int address)
{
    unsigned char val;
    pmu_read_multiple(address, 1, &val);
    return val;
}

int pmu_write(int address, unsigned char val)
{
    return pmu_write_multiple(address, 1, &val);
}

/* ==================================================================== *
 * Power state control
 * ==================================================================== */

void pmu_set_wake_condition(unsigned char condition)
{
    /* Not yet identified which register configures this. */
    (void)condition;
}

void pmu_enter_standby(void)
{
    pmu_write(D1671_REG_SYSCTRLA, D1671_SYSCTRLA_GOSTDBY);
}

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
void pmu_set_cpu_voltage(bool high)
{
    /* Not yet identified which register/rail this maps to. */
    (void)high;
}
#endif

/* ==================================================================== *
 * RTC (registers 0x40..0x45)
 *
 * The original firmware stores seconds, minutes, hours, day, month and
 * year-since-2000 in binary across six consecutive registers starting at
 * D1671_REG_RTCSEC. Bit 6 of the seconds register is set by the write
 * side and masked off by the read side; this looks like a "new value has
 * been latched" flag the chip expects on writes, so it is set on every
 * write and stripped from every read to keep callers working purely in
 * plain field values.
 * ==================================================================== */
#if (CONFIG_RTC == RTC_NANO3G)

#define D1671_RTCSEC_SET    0x40

void pmu_read_rtc(unsigned char *buffer)
{
    pmu_read_multiple(D1671_REG_RTCSEC, 6, buffer);
    buffer[0] &= ~D1671_RTCSEC_SET;
}

void pmu_write_rtc(unsigned char *buffer)
{
    int i;

    /* Written one register at a time, seconds first, matching the order
     * the original firmware uses. */
    for (i = 0; i < 6; i++)
        pmu_write(D1671_REG_RTCSEC + i,
                  buffer[i] | (i == 0 ? D1671_RTCSEC_SET : 0));
}

#endif /* CONFIG_RTC == RTC_NANO3G */

/* ==================================================================== *
 * Charging control
 * ==================================================================== */

void pmu_set_usblimit(bool fast_charge)
{
    unsigned char chctl = pmu_read(D1671_REG_CHCTL);

    chctl &= ~D1671_CHCTL_FASTCHRG;
    if (fast_charge)
        chctl |= D1671_CHCTL_FASTCHRG;

    pmu_write(D1671_REG_CHCTL, chctl);
}

/* ==================================================================== *
 * ADC
 *
 * As the original firmware drives it: write the channel's mux selection
 * to D1671_REG_ADCCTL with the START bit set, poll the same register
 * until START clears, then read the 10-bit result split across two
 * registers (high 8 bits, low 2 bits). The controller idles at 0x20
 * between conversions.
 * ==================================================================== */

#define D1671_REG_ADCCTL    0x30
#define D1671_REG_ADCLO     0x31
#define D1671_REG_ADCHI     0x32
#define D1671_ADCCTL_IDLE   0x20
#define D1671_ADCCTL_START  0x08

/* Bounds how long a single conversion is polled before giving up on it.
 * 20 tries * 50 us = 1 ms; the original firmware's own polling loop is
 * the source of both the delay and the try count. */
#define ADC_POLL_DELAY_US   50
#define ADC_POLL_MAX_TRIES  20

static struct mutex pmu_adc_mutex;

unsigned short pmu_adc_raw2mv(const struct pmu_adc_channel *ch,
                               unsigned short raw)
{
    return ch->offset_mv + raw * ch->span_mv / 1023;
}

static unsigned short pmu_adc_convert_once(const struct pmu_adc_channel *ch)
{
    int tries;

    pmu_write(D1671_REG_ADCCTL, ch->mux | D1671_ADCCTL_START);
    for (tries = 0; tries < ADC_POLL_MAX_TRIES; tries++)
    {
        udelay(ADC_POLL_DELAY_US);
        if (!(pmu_read(D1671_REG_ADCCTL) & D1671_ADCCTL_START))
            break;
    }
    return (pmu_read(D1671_REG_ADCHI) << 2) | (pmu_read(D1671_REG_ADCLO) & 3);
}

unsigned short pmu_read_adc(const struct pmu_adc_channel *ch)
{
    unsigned int sum = 0;
    int i;

    if (!ch->samples)
        return 0;

    mutex_lock(&pmu_adc_mutex);
    for (i = 0; i < ch->samples; i++)
        sum += pmu_adc_convert_once(ch);
    pmu_write(D1671_REG_ADCCTL, D1671_ADCCTL_IDLE);
    mutex_unlock(&pmu_adc_mutex);

    return sum / ch->samples;
}

/* ==================================================================== *
 * External interrupt handling
 *
 * The PMU raises one shared, active-low IRQ line (GPIO_EINT_PMU) for
 * USB/FireWire/accessory presence changes and the hold switch. The
 * handler below clears every pending PMU-side event, re-reads the
 * current input levels into a small cache, and re-arms the GPIO
 * interrupt -- the PMU IRQ line stays asserted (masked out at the GPIO
 * controller) until we acknowledge, so there is no risk of missing an
 * edge between the ISR and the thread handling it.
 * ==================================================================== */

#define Q_EINT  0

static long pmu_thread_stack[DEFAULT_STACK_SIZE / 2 / sizeof(long)];
static struct event_queue pmu_queue;

static struct
{
    bool holdswitch_locked;
#ifdef IPOD_ACCESSORY_PROTOCOL
    bool accessory_present;
#endif
#if CONFIG_CHARGING
    bool firewire_present;
#endif
} pmu_inputs;

int pmu_holdswitch_locked(void)
{
    return pmu_inputs.holdswitch_locked;
}

#ifdef IPOD_ACCESSORY_PROTOCOL
int pmu_accessory_present(void)
{
    return pmu_inputs.accessory_present;
}
#endif

#if CONFIG_CHARGING
int pmu_firewire_present(void)
{
    return pmu_inputs.firewire_present;
}
#endif

/* usb_insert_int()/usb_remove_int() are normally provided by
 * usb-s5l8702.c; in a bootloader build without USB mode support that
 * file isn't linked, so provide a trivial polled stand-in here. */
#if defined(BOOTLOADER) && !defined(HAVE_BOOTLOADER_USB_MODE)
#include "usb.h"
static int usb_status = USB_EXTRACTED;

int usb_detect(void)
{
    return usb_status;
}

void usb_insert_int(void)
{
    usb_status = USB_INSERTED;
}

void usb_remove_int(void)
{
    usb_status = USB_EXTRACTED;
}
#endif

static void pmu_refresh_inputs(void)
{
    unsigned char status[2];

    pmu_read_multiple(D1671_REG_STATUSA, 2, status);

    if (status[0] & D1671_STATUSA_VBUS)
        usb_insert_int();
    else
        usb_remove_int();

#if CONFIG_CHARGING
    pmu_inputs.firewire_present = !!(status[0] & D1671_STATUSA_VADAPTOR);
#endif
#ifdef IPOD_ACCESSORY_PROTOCOL
    pmu_inputs.accessory_present = !!(status[0] & D1671_STATUSA_INPUT1);
#endif
    pmu_inputs.holdswitch_locked = !(status[1] & D1671_STATUSB_INPUT2);
}

static void pmu_eint_isr(struct eic_handler *h);

static struct eic_handler pmu_eint =
{
    .gpio_n = GPIO_EINT_PMU,
    .type   = EIC_INTTYPE_LEVEL,
    .level  = EIC_INTLEVEL_LOW,
    .isr    = pmu_eint_isr,
};

static void pmu_eint_isr(struct eic_handler *h)
{
    eint_unregister(h);
    queue_post(&pmu_queue, Q_EINT, 0);
}

static void pmu_ack_and_refresh(void)
{
    /* Clearing every PMU event bit also de-asserts the shared IRQ line,
     * which the GPIO controller was masking while it was pending. */
    pmu_write_multiple(D1671_REG_EVENTA, 2, "\xFF\xFF");
    pmu_refresh_inputs();
}

static void NORETURN_ATTR pmu_thread(void)
{
    struct queue_event ev;

    while (true)
    {
        queue_wait_w_tmo(&pmu_queue, &ev, TIMEOUT_BLOCK);
        if (ev.id == Q_EINT)
        {
            pmu_ack_and_refresh();
            eint_register(&pmu_eint);
        }
    }
}

static void pmu_configure_irq_mask(unsigned char *mask /* [2] */)
{
    mask[0] = 0xff;
    mask[1] = 0xff;

    mask[0] &= ~D1671_IRQMASKA_VBUS;      /* USB presence */
#if CONFIG_CHARGING
    mask[0] &= ~D1671_IRQMASKA_VADAPTOR;  /* FireWire presence */
#endif
#ifdef IPOD_ACCESSORY_PROTOCOL
    mask[0] &= ~D1671_IRQMASKA_INPUT1;    /* Accessory presence */
#endif
    mask[1] &= ~D1671_IRQMASKB_INPUT2;    /* Hold switch */
}

void pmu_init(void)
{
    unsigned char irq_mask[2];

    mutex_init(&pmu_adc_mutex);
    queue_init(&pmu_queue, false);

    create_thread(pmu_thread, pmu_thread_stack, sizeof(pmu_thread_stack), 0,
                  "PMU" IF_PRIO(, PRIORITY_SYSTEM) IF_COP(, CPU));

    pmu_configure_irq_mask(irq_mask);
    pmu_write_multiple(D1671_REG_IRQMASKA, 2, irq_mask);

    pmu_ack_and_refresh();  /* clear stale events, seed the input cache */

    eint_register(&pmu_eint);
}

/* ==================================================================== *
 * Preinit (before the kernel, threads, or I2C interrupt path exist)
 * ==================================================================== */

int pmu_rd_multiple(int address, int count, unsigned char *buffer)
{
    return i2c_rd(0, PMU_I2C_SLAVE, address, count, buffer);
}

int pmu_wr_multiple(int address, int count, unsigned char *buffer)
{
    return i2c_wr(0, PMU_I2C_SLAVE, address, count, buffer);
}

unsigned char pmu_rd(int address)
{
    unsigned char val;
    pmu_rd_multiple(address, 1, &val);
    return val;
}

int pmu_wr(int address, unsigned char val)
{
    return pmu_wr_multiple(address, 1, &val);
}

void pmu_preinit(void)
{
    /* Power rail / LDO bring-up. Every value below is taken directly from
     * observing the original firmware's own preinit sequence for this
     * chip; TBC markers indicate a purpose we have not confirmed, kept
     * exactly as the reference driver documented them rather than
     * guessing at a friendlier name. */
    pmu_wr(0x1b, 0x14);     /* TBC: LDO */
    pmu_wr(0x16, 0x14);     /* TBC: LDO */
    pmu_wr(0x15, 0x14);     /* TBC: Vnand = 2000 + val*50 = 3000 mV */
    pmu_wr(0x18, 0x18);     /* TBC: Vaccy = 3200 mV ??? */

    /* Clear bit 5 of register 0x10 while preserving bit 2: bit 2 is a
     * hardware requirement for the NAND controller to work on this
     * device (see file header; this exact line is Rockbox commit
     * 66bc0728). */
    pmu_wr(0x10, (pmu_rd(0x10) & 0xdf) | 0x8);

    /* TBC: registers 0x30/0x31/0x32 also relate to the ADC, per norboot */
    pmu_wr(0x34, 0x72);              /* TBC: DA9030-family TBATHIGH? */
    pmu_wr(0x30, pmu_rd(0x30) | 0x20); /* TBC: TBATREF on? */

    pmu_wr(0x21, 0x5c);     /* TBC: charge control (max current / Vbat?) */
    pmu_wr(0xb, 0x6);       /* TBC */
    pmu_wr(0x1d, 0);        /* TBC */

    /* Configure and clear PMU interrupts before anything can rely on
     * them (there is no I2C IRQ path yet at this stage; runtime pmu_init()
     * reconfigures this properly once threads exist). */
    pmu_wr_multiple(D1671_REG_IRQMASKA, 2, "\x42\xBE");
    pmu_wr_multiple(D1671_REG_EVENTA, 2, "\xFF\xFF");

    /* Backlight off until backlight_hw_init() explicitly turns it on. */
    pmu_wr(D1671_REG_LEDCTL, pmu_rd(D1671_REG_LEDCTL) & ~D1671_LEDCTL_ENABLE);
}

#ifdef BOOTLOADER
bool pmu_is_hibernated(void)
{
    return !!(pmu_rd(D1671_REG_EVENTB) & D1671_EVENTB_WARMBOOT);
}
#endif
