/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") PMU (Dialog D1671) driver public interface.
 *
 * Original implementation for this project. The register addresses,
 * bitfield positions and the "0x40..0x45 = RTC seconds..year, bit 6 of
 * seconds = SET flag" convention below are hardware facts about the
 * D1671 PMU chip, derived from observing how the original firmware
 * accesses them (this chip has no public datasheet). They are recorded
 * here as facts to be described accurately, not as an implementation
 * choice; anywhere the exact meaning of a bit is still unconfirmed it is
 * marked TBC (to be confirmed), matching how the original driver's
 * comments documented that same uncertainty.
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

#ifndef __PMU_TARGET_H__
#define __PMU_TARGET_H__

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

/* Undocumented D1671 PMU registers, as accessed over I2C (7-bit address
 * 0x73, i.e. slave byte 0xe6 for a write). */
enum d1671_regs
{
    D1671_REG_EVENTA    = 0x02,
    D1671_REG_EVENTB    = 0x03,
    D1671_REG_STATUSA   = 0x04,
    D1671_REG_STATUSB   = 0x05,
    D1671_REG_IRQMASKA  = 0x06,
    D1671_REG_IRQMASKB  = 0x07,
    D1671_REG_SYSCTRLA  = 0x08,
    D1671_REG_LEDCTL    = 0x20,
    D1671_REG_CHCTL     = 0x21,   /* TBC */
    D1671_REG_RTCSEC    = 0x40,   /* TBC */
};

enum d1671_reg_eventa
{
    D1671_EVENTA_VBUS     = 0x08,  /* USB: 0 -> not present */
    D1671_EVENTA_VADAPTOR = 0x10,  /* FireWire: 0 -> not present */
    D1671_EVENTA_INPUT1   = 0x20,  /* accessory: 0 -> not present */
    D1671_EVENTA_UNK6     = 0x40,  /* TBC */
};

enum d1671_reg_eventb
{
    D1671_EVENTB_INPUT2   = 0x01,  /* hold switch: 0 -> locked */
    D1671_EVENTB_WARMBOOT = 0x80,  /* TBC */
};

enum d1671_reg_statusa
{
    D1671_STATUSA_VBUS     = 0x08,
    D1671_STATUSA_VADAPTOR = 0x10,
    D1671_STATUSA_INPUT1   = 0x20,
};

enum d1671_reg_statusb
{
    D1671_STATUSB_INPUT2   = 0x01,
    D1671_STATUSB_WARMBOOT = 0x80,
};

enum d1671_reg_irqmaska
{
    D1671_IRQMASKA_VBUS     = 0x08,
    D1671_IRQMASKA_VADAPTOR = 0x10,
    D1671_IRQMASKA_INPUT1   = 0x20,
};

enum d1671_reg_irqmaskb
{
    D1671_IRQMASKB_INPUT2   = 0x01,
    D1671_IRQMASKB_WARMBOOT = 0x80,
};

enum d1671_reg_sysctrla
{
    D1671_SYSCTRLA_GOSTDBY = 0x01,
    D1671_SYSCTRLA_GOUNK   = 0x02,  /* enters an unknown state, TBC (shutdown?) */
};

enum d1671_reg_ledctl
{
    D1671_LEDCTL_UNKNOWN = 0x40,  /* TBC */
    D1671_LEDCTL_ENABLE  = 0x80,
};
#define D1671_LEDCTL_OUT_POS    0
#define D1671_LEDCTL_OUT_MASK   0x1f

enum d1671_reg_chctl
{
    D1671_CHCTL_FASTCHRG = 0x01,  /* 100/500 mA USB current limit select */
};

/* GPIO line the PMU's own IRQ output is wired to */
#define GPIO_EINT_PMU   0x7b

/* One ADC channel's PMU mux selector and mV conversion, see pmu_read_adc()
 * and adc-nano3g.c */
struct pmu_adc_channel
{
    const char *name;
    uint8_t mux;                /* value written to D1671_REG_ADCCTL */
    uint8_t samples;            /* conversions averaged; 0 = not readable */
    unsigned short offset_mv;   /* millivolts at raw ADC code 0 */
    unsigned short span_mv;     /* millivolts from raw code 0 to 1023 */
};

/* Runtime (threaded, I2C bus arbitrated) access */
void pmu_init(void);
unsigned char pmu_read(int address);
int pmu_write(int address, unsigned char val);
int pmu_read_multiple(int address, int count, unsigned char *buffer);
int pmu_write_multiple(int address, int count, unsigned char *buffer);

/* Early bring-up access (before the kernel/threads/I2C IRQ path exist) */
void pmu_preinit(void);
#ifdef BOOTLOADER
unsigned char pmu_rd(int address);
int pmu_wr(int address, unsigned char val);
int pmu_rd_multiple(int address, int count, unsigned char *buffer);
int pmu_wr_multiple(int address, int count, unsigned char *buffer);
bool pmu_is_hibernated(void);
#endif

void pmu_set_wake_condition(unsigned char condition);
void pmu_enter_standby(void);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
void pmu_set_cpu_voltage(bool high);
#endif
#if (CONFIG_RTC == RTC_NANO3G)
void pmu_read_rtc(unsigned char *buffer);
void pmu_write_rtc(unsigned char *buffer);
#endif
void pmu_set_usblimit(bool fast_charge);

unsigned short pmu_read_adc(const struct pmu_adc_channel *ch);
unsigned short pmu_adc_raw2mv(const struct pmu_adc_channel *ch,
                               unsigned short raw);

int pmu_holdswitch_locked(void);
#if CONFIG_CHARGING
int pmu_firewire_present(void);
#endif
#ifdef IPOD_ACCESSORY_PROTOCOL
int pmu_accessory_present(void);
#endif

#endif /* __PMU_TARGET_H__ */
