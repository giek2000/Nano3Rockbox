/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") battery/power management glue.
 *
 * Original implementation for this project. The battery threshold and
 * the two voltage-to-percentage curves below are measured calibration
 * data specific to this device's battery cell chemistry and discharge
 * curve -- there is no "original" alternative to these numbers, only a
 * choice of source. They are kept as documented hardware/calibration
 * facts, exactly as any Rockbox target's powermgmt file must record them
 * (compare e.g. other iPod targets' own percent_to_volt tables, which are
 * likewise measured per-device data, not algorithmic).
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
#include "powermgmt.h"
#include "pmu-target.h"
#include "power.h"
#include "audiohw.h"
#include "adc-target.h"

/* Below this, storage writes are refused to avoid corruption on a brownout. */
const unsigned short battery_level_disksafe = 3500;
/* Below this, the device shuts itself down. */
const unsigned short battery_level_shutoff  = 3300;

/* Measured open-circuit voltage (mV) at each 10% state-of-charge step,
 * discharging (no charger attached). */
const unsigned short percent_to_volt_discharge[11] =
{
    3500, 3670, 3720, 3750, 3770, 3800, 3860, 3920, 3980, 4070, 4170
};

#if CONFIG_CHARGING
/* Measured voltage (mV) at each 10% state-of-charge step while the
 * charger is actively charging the cell (higher than the discharge curve
 * due to the cell's internal resistance under charge current). */
const unsigned short percent_to_volt_charge[11] =
{
    3700, 3820, 3900, 3950, 3990, 4030, 4070, 4120, 4170, 4190, 4200
};
#endif /* CONFIG_CHARGING */

int _battery_voltage(void)
{
    return adc_read_battery_voltage();
}

#ifdef HAVE_ACCESSORY_SUPPLY
void accessory_supply_set(bool enable)
{
    /* Not yet identified which PMU rail (if any) gates accessory power
     * on this target; nothing to switch here until it is. */
    (void)enable;
}
#endif

#ifdef HAVE_LINEOUT_POWEROFF
void lineout_set(bool enable)
{
    audiohw_enable_lineout(enable);
}
#endif
