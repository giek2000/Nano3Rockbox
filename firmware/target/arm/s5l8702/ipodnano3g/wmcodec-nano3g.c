/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") Wolfson WM1870 audio codec driver.
 *
 * Original implementation for this project. The codec register write
 * protocol (7-bit register address packed with the value's 9th bit into
 * one control byte, addressed on I2C bus 0 at 0x34, write-only) and the
 * sample-rate-to-register table below are hardware facts: WM1870 has no
 * public datasheet, so these are taken from observing how the original
 * firmware configures the same codec while running from the same 12 MHz
 * MCLK this driver also uses; they describe the chip, not a design
 * choice, and are kept exact.
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
#include "audiohw.h"
#include "i2c-s5l8702.h"
#include "wmcodec.h"

#define WMCODEC_I2C_SLAVE   0x34

/* The PCM driver brings the codec up (audiohw_preinit()) and drives its
 * MCLK; nothing else to do at audiohw_init() time. */
void audiohw_init(void)
{
}

/* Rate-to-register table for a 12 MHz MCLK: these are the codec's "USB
 * mode" sample rate codes with a BCLK divider selected, matching how the
 * original firmware clocks this codec on this device. */
static const struct
{
    unsigned long rate;
    unsigned short sampctrl;
} wmcodec_rate_table[] =
{
    { SAMPR_8,  0x10d },
    { SAMPR_11, 0x133 },
    { SAMPR_12, 0x111 },
    { SAMPR_16, 0x115 },
    { SAMPR_22, 0x137 },
    { SAMPR_24, 0x139 },
    { SAMPR_32, 0x119 },
    { SAMPR_44, 0x123 },
    { SAMPR_48, 0x081 },
    { SAMPR_88, 0x0bf },
    { SAMPR_96, 0x09d },
};

unsigned short wmcodec_sampctrl(unsigned long rate)
{
    unsigned int i;

    for (i = 0; i < ARRAYLEN(wmcodec_rate_table); i++)
        if (wmcodec_rate_table[i].rate == rate)
            return wmcodec_rate_table[i].sampctrl;

    return 0;
}

/* Sends a 7-bit register address and 9-bit value as a single two-byte
 * I2C write: the register (shifted left one bit) ORed with the value's
 * bit 8, followed by the value's low 8 bits. The codec never acknowledges
 * reads, so only writes are supported. */
void wmcodec_write(int reg, int data)
{
    unsigned char low_byte = data & 0xff;
    unsigned char addr_byte = (reg << 1) | ((data >> 8) & 1);

    i2c_write(0, WMCODEC_I2C_SLAVE, addr_byte, 1, &low_byte);
}
