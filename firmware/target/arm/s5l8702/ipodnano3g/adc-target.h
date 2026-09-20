/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") ADC channel driver public interface.
 *
 * Original implementation for this project. There is no separate ADC
 * peripheral on this target: analog inputs are muxed through the PMU
 * (D1671) chip's own ADC, driven over I2C by pmu-nano3g.c. This header
 * only declares which logical channels exist.
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
#ifndef _ADC_TARGET_H_
#define _ADC_TARGET_H_

#include <stdbool.h>
#include "config.h"

enum
{
    ADC_BATTERY = 0,
    ADC_USBDATA,
    ADC_ACCESSORY,
    NUM_ADC_CHANNELS
};

#define ADC_UNREG_POWER ADC_BATTERY /* For compatibility */

unsigned short adc_read_millivolts(int channel);
unsigned int adc_read_battery_voltage(void);
unsigned int adc_read_usbdata_voltage(bool dp);
int adc_read_accessory_resistor(void);
const char *adc_name(int channel);

#endif
