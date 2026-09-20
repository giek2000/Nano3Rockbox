/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") backlight driver.
 *
 * Original implementation for this project. The backlight is a PWM
 * output on the PMU (D1671) chip, controlled entirely through
 * D1671_REG_LEDCTL. The bit assignments used below (bits 0..4 = output
 * level, bit 7 = enable, bit 6 = an accompanying flag whose exact purpose
 * is unconfirmed) are hardware facts taken from how the original firmware
 * drives this register; the bit-6 behaviour is marked TBC rather than
 * given a made-up name, matching the reference driver's own uncertainty.
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
#include <stdbool.h>

#include "config.h"
#include "kernel.h"
#include "backlight.h"
#include "backlight-target.h"
#include "pmu-target.h"

#ifdef HAVE_LCD_SLEEP
#include "lcd.h"
#include "lcd-s5l8702.h"
#endif

/* brightness argument is Rockbox's 0..(brightness steps-1) setting range;
 * the original firmware writes it directly into LEDCTL bits 0..4, so it
 * is halved here to fit that 5-bit field (matching observed behaviour). */
void backlight_hw_brightness(int brightness)
{
    unsigned char ledctl = pmu_read(D1671_REG_LEDCTL);

    ledctl &= ~D1671_LEDCTL_OUT_MASK;
    ledctl |= (brightness >> 1) & D1671_LEDCTL_OUT_MASK;

    pmu_write(D1671_REG_LEDCTL, ledctl);
}

void backlight_hw_on(void)
{
#ifdef HAVE_LCD_SLEEP
    if (!lcd_active())
        lcd_awake();
#endif

    /* As the original firmware turns the backlight on: bit 7 (ENABLE)
     * and bit 6 (UNKNOWN) set, bit 5 clear. */
    unsigned char ledctl = pmu_read(D1671_REG_LEDCTL);

    ledctl &= ~0x60;
    ledctl |= D1671_LEDCTL_ENABLE | D1671_LEDCTL_UNKNOWN;

    pmu_write(D1671_REG_LEDCTL, ledctl);
}

void backlight_hw_off(void)
{
    pmu_write(D1671_REG_LEDCTL, pmu_read(D1671_REG_LEDCTL) & ~D1671_LEDCTL_ENABLE);
}

bool backlight_hw_init(void)
{
    backlight_hw_brightness(DEFAULT_BRIGHTNESS_SETTING);
    backlight_hw_on();
    return true;
}

void backlight_hw_kill(void)
{
    backlight_hw_off();
}
