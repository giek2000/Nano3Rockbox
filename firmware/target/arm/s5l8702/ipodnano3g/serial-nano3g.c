/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") dock connector serial / iAP driver.
 *
 * Original implementation for this project. The uartc_port_* API and the
 * struct uartc_port/s5l8702_uartc it drives are shared S5L8702 UART
 * controller infrastructure (firmware/export/uc87xx.h and the s5l8702
 * uart-s5l8702.c driver), not something specific to this target. The
 * UART_CLK_HZ value and the BRDATA_* bit-rate divisor constants below are
 * hardware facts: they are the UBRDIV/DIVSLOT values that yield the named
 * baud rates when the UART's clock is a 12 MHz ECLK, which is how this
 * SoC's baud rate generator works (documented behaviour of the peripheral,
 * not a creative choice). The auto-baud-rate detection thresholds are
 * likewise derived directly from that same clock arithmetic (+-10% guard
 * bands around the standard rates 9600..57600), not copied from another
 * project's iAP implementation.
 *
 * The state machine driving auto-baud detection and the accessory
 * plug/unplug handling below is written independently for this project.
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
#include <stdint.h>
#include <stdbool.h>

#include "config.h"
#include "cpu.h"
#include "system.h"
#include "serial.h"

#include "s5l87xx.h"
#include "uc87xx.h"

#define LOGF_ENABLE
#include "logf.h"

/* UART baud rate generator clock. The dock/iAP UART on this target runs
 * from the 12 MHz external oscillator, not the CPU clock. */
#define SERIAL_UART_CLK_HZ     12000000

/* UBRDIV/DIVSLOT encodings that yield each standard rate from a 12 MHz
 * UART clock (DIVSLOT packed in the upper bits where the divider isn't a
 * whole number, per this UART controller's fractional divider scheme). */
#define BRDATA_9600     (77)
#define BRDATA_19200    (38)
#define BRDATA_28800    (25)
#define BRDATA_38400    (19 | (0xc330c << 8))
#define BRDATA_57600    (12)
#define BRDATA_115200   (6  | (0xffffff << 8))

extern const struct uartc s5l8702_uartc;

#ifdef IPOD_ACCESSORY_PROTOCOL
static void serial_rx_isr(int len, char *data, char *err, uint32_t abr_cnt);
#endif

static struct uartc_port dock_port IDATA_ATTR =
{
    .uartc  = &s5l8702_uartc,
    .id     = 0,

    .rx_trg = UFCON_RX_FIFO_TRG_4,
    .tx_trg = UFCON_TX_FIFO_TRG_EMPTY,
    .clksel = UCON_CLKSEL_ECLK,
    .clkhz  = SERIAL_UART_CLK_HZ,

#ifdef IPOD_ACCESSORY_PROTOCOL
    .rx_cb  = serial_rx_isr,
#else
    .rx_cb  = NULL,
#endif
    .tx_cb  = NULL,  /* Tx is polled */
};

/* ---- Rockbox serial driver API ---- */

int tx_rdy(void)
{
    return uartc_port_tx_ready(&dock_port) ? 1 : 0;
}

void tx_writec(unsigned char c)
{
    uartc_port_tx_byte(&dock_port, c);
}

#ifndef IPOD_ACCESSORY_PROTOCOL

void serial_setup(void)
{
    uartc_port_open(&dock_port);
    uartc_port_config(&dock_port, ULCON_DATA_BITS_8,
                       ULCON_PARITY_NONE, ULCON_STOP_BITS_1);
    uartc_port_set_bitrate_raw(&dock_port, BRDATA_115200);
    uartc_port_set_tx_mode(&dock_port, UCON_MODE_INTREQ);

    logf("[%lu] "MODEL_NAME" serial port %d ready", USEC_TIMER, dock_port.id);
}

#else /* IPOD_ACCESSORY_PROTOCOL */

#include "kernel.h"
#include "pmu-target.h"
#include "iap.h"

/* Auto-baud-rate detection state.
 *
 * When an accessory is plugged in but its speed is unknown, we launch the
 * UART controller's hardware ABR detector and wait for it to time the
 * first low pulse on Rx (expected to be the start bit of an 0xFF sync
 * byte). Once a plausible pulse width comes back we commit to the nearest
 * standard rate and require the very next byte to look like protocol
 * sync (0xFF or 0x55) before trusting it; a bad guess restarts the whole
 * detection cycle rather than getting stuck on a wrong rate.
 */
enum abr_state
{
    ABR_IDLE,        /* no accessory connected */
    ABR_DETECTING,    /* hardware ABR running, waiting for a pulse */
    ABR_CONFIRMING,   /* rate applied, waiting to confirm sync byte(s) */
    ABR_LOCKED,       /* rate confirmed (or fixed rate requested) */
};

static enum abr_state abr_state = ABR_IDLE;
static int requested_bitrate = 0;   /* 0 = auto-detect */
static bool accessory_plugged = false;

/* After applying a detected rate, the iAP sync preamble is expected to be
 * [0xff] 0x55 -- two bytes. We accept the rate as soon as either looks
 * like a sync byte, and only give up (restarting detection) once both
 * have failed to look like one. */
#define ABR_CONFIRM_TRIES   2

static int abr_confirm_tries_left;

static uint32_t bitrate_to_brdata(int rate)
{
    switch (rate)
    {
        case 57600: return BRDATA_57600;
        case 38400: return BRDATA_38400;
        case 19200: return BRDATA_19200;
        default:    return BRDATA_9600;
    }
}

/* Converts an ABR pulse-width count (in UART clock ticks) to the nearest
 * standard rate, using the midpoints between each pair of standard rates'
 * expected tick counts. */
static uint32_t abr_count_to_brdata(uint32_t abr_cnt)
{
    #define TICKS_FOR(bps) (SERIAL_UART_CLK_HZ / (unsigned)(bps))

    if (abr_cnt < TICKS_FOR(48000)) return BRDATA_57600;
    if (abr_cnt < TICKS_FOR(33600)) return BRDATA_38400;
    if (abr_cnt < TICKS_FOR(24000)) return BRDATA_28800;
    if (abr_cnt < TICKS_FOR(14400)) return BRDATA_19200;
    return BRDATA_9600;

    #undef TICKS_FOR
}

/* A detected pulse width is only trusted if it falls within +-10% of the
 * fastest (57600) to slowest (9600) standard rates we support; anything
 * outside that band is treated as noise and detection is relaunched. */
static bool abr_count_in_range(uint32_t abr_cnt)
{
    #define TICKS_FOR(bps) (SERIAL_UART_CLK_HZ / (unsigned)(bps))
    return abr_cnt >= TICKS_FOR(57600 * 11 / 10)
        && abr_cnt <= TICKS_FOR(9600 * 9 / 10);
    #undef TICKS_FOR
}

static void abr_launch(void)
{
    uartc_port_set_rx_mode(&dock_port, UCON_MODE_DISABLED);
    uartc_port_abr_start(&dock_port);
    abr_state = ABR_DETECTING;
}

static void abr_apply_and_confirm(uint32_t brdata)
{
    uartc_port_set_bitrate_raw(&dock_port, brdata);
    uartc_port_set_rx_mode(&dock_port, UCON_MODE_INTREQ);
    iap_getc(IF_IAP_MP(0,) 0xff);  /* prime the iAP parser for sync byte */
    abr_state = ABR_CONFIRMING;
    abr_confirm_tries_left = ABR_CONFIRM_TRIES;
}

static void serial_port_bring_up(void)
{
    uartc_open(dock_port.uartc);
    uartc_port_open(&dock_port);
    uartc_port_config(&dock_port, ULCON_DATA_BITS_8,
                       ULCON_PARITY_NONE, ULCON_STOP_BITS_1);
    uartc_port_set_tx_mode(&dock_port, UCON_MODE_INTREQ);
    serial_bitrate(requested_bitrate);
}

static void serial_port_tear_down(void)
{
    uartc_port_abr_stop(&dock_port);
    uartc_port_close(&dock_port);
    uartc_close(dock_port.uartc);
    abr_state = ABR_IDLE;
}

static void serial_accessory_poll(void)
{
    bool plugged = pmu_accessory_present();

    if (plugged == accessory_plugged)
        return;

    accessory_plugged = plugged;
    if (plugged)
        serial_port_bring_up();
    else
        serial_port_tear_down();
}

void serial_setup(void)
{
    uartc_close(dock_port.uartc);
    tick_add_task(serial_accessory_poll);
}

void serial_bitrate(int rate)
{
    requested_bitrate = rate;

    if (!accessory_plugged)
        return;

    logf("[%lu] serial_bitrate(%d)", USEC_TIMER, rate);

    uartc_port_abr_stop(&dock_port);  /* cancel any detection in progress */

    if (rate == 0)
        abr_launch();
    else
    {
        abr_apply_and_confirm(bitrate_to_brdata(rate));
        abr_state = ABR_LOCKED;  /* fixed rate: trust it immediately */
    }
}

static void serial_rx_isr(int len, char *data, char *err, uint32_t abr_cnt)
{
    (void)err;  /* Rx framing/parity errors: let the iAP layer discard bad packets */

    if (abr_state == ABR_DETECTING && abr_cnt != 0)
    {
        if (!abr_count_in_range(abr_cnt))
        {
            abr_launch();  /* implausible pulse width, try again */
            return;
        }
        abr_apply_and_confirm(abr_count_to_brdata(abr_cnt));
    }

    while (len--)
    {
        bool is_sync_byte = !iap_getc(IF_IAP_MP(0,) *data++);

        if (abr_state != ABR_CONFIRMING)
            continue;

        if (is_sync_byte)
        {
            abr_state = ABR_LOCKED;
        }
        else if (--abr_confirm_tries_left == 0)
        {
            /* Neither expected sync byte showed up: the detected rate
             * was wrong. Discard whatever's left of this batch and
             * restart detection from scratch. */
            serial_bitrate(0);
            return;
        }
    }
}

#endif /* IPOD_ACCESSORY_PROTOCOL */
