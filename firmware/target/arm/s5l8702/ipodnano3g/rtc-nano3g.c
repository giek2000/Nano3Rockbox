/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") RTC driver, layered on the PMU's RTC registers
 * (pmu_read_rtc()/pmu_write_rtc() in pmu-nano3g.c).
 *
 * Original implementation for this project. The PMU keeps seconds,
 * minutes, hours, day, month and year-since-2000 in binary, with no
 * weekday field -- Rockbox's set_day_of_week() derives that. This is a
 * hardware fact about the PMU (see pmu-nano3g.c); everything else here is
 * a plain field-by-field translation between that layout and struct tm.
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
#include "rtc.h"
#include "kernel.h"
#include "system.h"
#include "pmu-target.h"
#include "timefuncs.h"

enum
{
    RTC_FIELD_SEC = 0,
    RTC_FIELD_MIN,
    RTC_FIELD_HOUR,
    RTC_FIELD_MDAY,
    RTC_FIELD_MON,
    RTC_FIELD_YEAR,
    RTC_NUM_FIELDS
};

void rtc_init(void)
{
    /* The PMU's RTC runs on its own regardless of Rockbox; nothing to
     * bring up here. */
}

int rtc_read_datetime(struct tm *tm)
{
    unsigned char buf[RTC_NUM_FIELDS];

    pmu_read_rtc(buf);

    tm->tm_sec  = buf[RTC_FIELD_SEC];
    tm->tm_min  = buf[RTC_FIELD_MIN];
    tm->tm_hour = buf[RTC_FIELD_HOUR];
    tm->tm_mday = buf[RTC_FIELD_MDAY];
    tm->tm_mon  = buf[RTC_FIELD_MON] - 1;
    tm->tm_year = buf[RTC_FIELD_YEAR] + 100;
    tm->tm_yday = 0; /* not implemented */

    set_day_of_week(tm);
    return 0;
}

int rtc_write_datetime(const struct tm *tm)
{
    unsigned char buf[RTC_NUM_FIELDS];

    buf[RTC_FIELD_SEC]  = tm->tm_sec;
    buf[RTC_FIELD_MIN]  = tm->tm_min;
    buf[RTC_FIELD_HOUR] = tm->tm_hour;
    buf[RTC_FIELD_MDAY] = tm->tm_mday;
    buf[RTC_FIELD_MON]  = tm->tm_mon + 1;
    buf[RTC_FIELD_YEAR] = tm->tm_year - 100;

    pmu_write_rtc(buf);
    return 0;
}
