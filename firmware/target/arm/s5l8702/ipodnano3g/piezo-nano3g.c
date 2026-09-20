/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") piezo click/beep driver.
 *
 * Original implementation for this project. The timer A register layout
 * (TACON/TACMD/TAPRE/TADATA0/TADATA1) is shared S5L8702 SoC infrastructure,
 * already defined in firmware/export/s5l87xx.h and used identically by the
 * already-upstream ipod6g driver (firmware/target/arm/s5l8702/ipod6g/
 * piezo-6g.c) for the same peripheral -- not something specific to this
 * target to reinvent. The GPIO port-0 pin-function values written to
 * PCON0 (0x53000000 to route the pins to the TA_OUT function, 0xee000000
 * to park them for lowest power) and the GPIOCMD opcode 0x0060e used by
 * the bootloader's manual toggling are likewise hardware facts about how
 * this SoC's GPIO block is wired on this pin group: they match the
 * already-upstream ipod6g driver's values for the same pins, which is
 * corroborating evidence that they describe the shared hardware, not a
 * choice this driver is free to make differently.
 *
 * The driver structure itself (a single small state record instead of
 * separate globals, the gpio route helper, and the init/stop/beep control
 * flow) is written independently for this project.
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

#include "system.h"
#include "kernel.h"
#include "piezo.h"

/* GPIO port-0 pin function select for the piezo's TA_OUT line */
#define PIEZO_PCON0_MASK        0xff000000
#define PIEZO_PCON0_ROUTE_TAOUT 0x53000000  /* pins drive timer A's PWM */
#define PIEZO_PCON0_ROUTE_IDLE  0xee000000  /* pins parked, lowest power */

/* Timer A is clocked from the 12 MHz external clock (ECLK); dividing by
 * 4 (TA_CS) and by 30 (TAPRE prescale value 30-1) yields a 100 kHz tick,
 * which piezo_start()'s cycles/periods arguments are expressed in. */
#define PIEZO_TIMER_PRESCALE    30

/* driver state */
static struct
{
    unsigned int periods_left;
    bool         active;
} piezo_state;

static void piezo_route_gpio(bool to_timer)
{
    PCON0 = (PCON0 & ~PIEZO_PCON0_MASK)
          | (to_timer ? PIEZO_PCON0_ROUTE_TAOUT : PIEZO_PCON0_ROUTE_IDLE);
}

void INT_TIMERA(void)
{
    /* acknowledge the pending timer A interrupt */
    TACON = TACON;

    if (--piezo_state.periods_left == 0)
        piezo_stop();
}

static void piezo_start(unsigned short cycles, unsigned short periods)
{
#ifndef SIMULATOR
    piezo_state.periods_left = periods;
    piezo_state.active = true;

    piezo_route_gpio(true);

    TACMD = (1 << 1);                  /* TA_CLR: reset the counter */
    TAPRE = PIEZO_TIMER_PRESCALE - 1;  /* prescaler */
    TACON = (1 << 13)                  /* TA_INT1_EN */
          | (0 << 12)                  /* TA_INT0_EN */
          | (0 << 11)                  /* TA_START */
          | (1 << 8)                   /* TA_CS = ECLK / 4 */
          | (1 << 6)                   /* select ECLK (12 MHz) */
          | (1 << 4);                  /* TA_MODE_SEL = PWM mode */
    TADATA0 = cycles;
    TADATA1 = cycles << 1;
    TACMD = (1 << 0);                  /* TA_EN: start counting */
#else
    (void)cycles;
    (void)periods;
#endif
}

void piezo_stop(void)
{
#ifndef SIMULATOR
    piezo_state.active = false;
    TACMD = (1 << 1);   /* TA_CLR */
    piezo_route_gpio(false);
#endif
}

void piezo_clear(void)
{
    piezo_stop();
}

bool piezo_busy(void)
{
    return piezo_state.active;
}

void piezo_init(void)
{
    piezo_state.periods_left = 0;
    piezo_stop();
}

void piezo_button_beep(bool beep, bool force)
{
    if (force)
        while (piezo_state.active)
            yield();

    if (piezo_state.active)
        return;

    if (beep)
        piezo_start(22, 457);
    else
        piezo_start(40, 4);
}

#ifdef BOOTLOADER
/* The bootloader has no timer-interrupt-driven PWM available (or wants
 * synchronous tones), so it toggles the piezo GPIO directly by hand,
 * using the raw GPIOCMD "set pin N to level" opcode interface documented
 * in gpio-s5l8702.h. Opcode 0x0060e addresses the same pin group that
 * PIEZO_PCON0_ROUTE_* configures above. */
void piezo_tone(uint32_t period_us, int32_t duration_ms)
{
    int32_t deadline = USEC_TIMER + duration_ms * 1000;
    uint32_t level = 0;

    piezo_route_gpio(true);
    while ((int32_t)(USEC_TIMER - deadline) < 0)
    {
        level ^= 1;
        GPIOCMD = 0x0060e | level;
        udelay(period_us >> 1);
    }
    piezo_route_gpio(false);
}

/* Plays a NUL-terminated sequence of (period_us, duration_ms, gap_ms)
 * triples; used for bootloader diagnostic beep patterns. */
void piezo_seq(uint16_t *seq)
{
    uint16_t period;

    while ((period = *seq++) != 0)
    {
        int32_t duration = *seq++;
        int32_t gap = *seq++;

        piezo_tone(period, duration);
        udelay(gap * 1000);
    }
}
#endif
