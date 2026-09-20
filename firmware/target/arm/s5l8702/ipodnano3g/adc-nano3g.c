/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") ADC channel driver.
 *
 * Original implementation for this project, layered on the PMU's ADC
 * (pmu_read_adc()/pmu_adc_raw2mv() in pmu-nano3g.c). Only the battery
 * channel's PMU ADC mux selector (0x24) and its raw-to-millivolt
 * conversion (offset 2500 mV, span 2000 mV over the 10-bit range) are
 * hardware facts here, established by measurement (see pmu-nano3g.c's
 * ADC section): reading a cell at rest against a multimeter and matching
 * the raw ADC codes the PMU returned. The USB-data and accessory-resistor
 * channels' PMU mux selectors have not been identified yet -- they read
 * back 0 rather than guessing at a mux value, which would risk reporting
 * a plausible-looking but wrong voltage.
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
#include "inttypes.h"
#include "s5l87xx.h"
#include "adc.h"
#include "adc-target.h"
#include "pmu-target.h"
#include "kernel.h"

/* Channels not yet identified on the PMU ADC mux are given samples = 0,
 * which pmu_read_adc()/adc_read_millivolts() treat as "not readable". */
static const struct pmu_adc_channel adc_channels[NUM_ADC_CHANNELS] =
{
    [ADC_BATTERY] =
    {
        .name     = "Battery",
        .mux      = 0x24,
        .samples  = 4,
        .offset_mv = 2500,
        .span_mv  = 2000,
    },
    [ADC_USBDATA] =
    {
        .name = "USB data",
    },
    [ADC_ACCESSORY] =
    {
        .name = "Accessory",
    },
};

static bool channel_is_readable(int channel)
{
    return adc_channels[channel].samples != 0;
}

unsigned short adc_read_millivolts(int channel)
{
    const struct pmu_adc_channel *ch = &adc_channels[channel];

    if (!channel_is_readable(channel))
        return 0;

    return pmu_adc_raw2mv(ch, pmu_read_adc(ch));
}

unsigned int adc_read_battery_voltage(void)
{
    return adc_read_millivolts(ADC_BATTERY);
}

/* Rockbox generic ADC API: raw reading, no mV conversion */
unsigned short adc_read(int channel)
{
    if (!channel_is_readable(channel))
        return 0;

    return pmu_read_adc(&adc_channels[channel]);
}

int adc_read_accessory_resistor(void)
{
    /* Mux selector not identified: refuse to guess rather than return a
     * plausible-looking but meaningless value. */
    return 0;
}

unsigned int adc_read_usbdata_voltage(bool dp)
{
    (void)dp;
    return 0;
}

const char *adc_name(int channel)
{
    return adc_channels[channel].name;
}

void adc_init(void)
{
    /* Nothing to configure: the PMU ADC is brought up by pmu_preinit(). */
}
