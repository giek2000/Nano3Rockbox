/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") power on/off and charge-state driver.
 *
 * Original implementation for this project. charging_state()'s bit
 * checks against D1671_REG_CHCTL/SYSCTRLA/STATUSB are hardware facts,
 * matched against how the original firmware decides the same thing;
 * measured behaviour (a 4.1 V cell charging from USB) is noted where the
 * evidence for a bit's meaning is inference rather than direct
 * documentation.
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
#include "inttypes.h"
#include "s5l87xx.h"
#include "power.h"
#include "panic.h"
#include "pmu-target.h"
#include "usb_core.h"   /* for usb_charging_maxcurrent_change */

void power_init(void)
{
    pmu_init();
    pmu_set_usblimit(false);  /* start at the conservative 100 mA limit */
}

void power_off(void)
{
    pmu_enter_standby();
    while (1);  /* pmu_enter_standby() should cut power before we get here */
}

#if CONFIG_CHARGING

#ifdef HAVE_USB_CHARGING_ENABLE
void usb_charging_maxcurrent_change(int maxcurrent)
{
    pmu_set_usblimit(maxcurrent >= 500);
}
#endif

unsigned int power_input_status(void)
{
    unsigned int status = POWER_INPUT_NONE;

    if (usb_detect() == USB_INSERTED)
        status |= POWER_INPUT_USB_CHARGER;
    if (pmu_firewire_present())
        status |= POWER_INPUT_MAIN_CHARGER;

    return status;
}

/* As the original firmware decides it: a charger must be present, the
 * charger enabled (D1671_REG_CHCTL bits 1..6 nonzero), not suspended
 * (D1671_REG_SYSCTRLA bit 2 clear), and not reporting done
 * (D1671_REG_STATUSB bits 1..2 clear). Those STATUSB bits have been
 * observed to read 0 throughout a full USB charge cycle on a 4.1 V cell,
 * so "not done" is the only behaviour confirmed by measurement; they are
 * treated as the charge-complete indication on the strength of the
 * original firmware's own logic, not direct observation of them set. */
bool charging_state(void)
{
    if (!(power_input_status() & POWER_INPUT_CHARGER))
        return false;
    if (!(pmu_read(D1671_REG_CHCTL) & 0x7e))
        return false;
    if (pmu_read(D1671_REG_SYSCTRLA) & 0x04)
        return false;
    return !(pmu_read(D1671_REG_STATUSB) & 0x06);
}

#endif /* CONFIG_CHARGING */
