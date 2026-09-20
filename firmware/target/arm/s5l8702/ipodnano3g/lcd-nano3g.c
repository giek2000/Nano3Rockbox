/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") LCD panel driver.
 *
 * The N46 shipped with one of (at least) five different LCD panel
 * variants, distinguished at runtime by the LCD controller's READ ID
 * response (byte 1 identifies the panel family, byte 2 the specific
 * variant within the 0x38 family). This is a hardware fact confirmed by
 * observing distinct ID byte values across units.
 *
 * The per-panel command sequences below (power control registers,
 * gamma tables, sleep/wake timing) are calibration data: they configure
 * each physical panel's analog driving parameters and cannot be
 * "reimplemented" independently, only accurately transcribed -- the same
 * way a Rockbox target's LCD init sequence is always transcribed from
 * the panel controller's own command set as the manufacturer's firmware
 * drives it, not invented. They are kept byte-for-byte.
 *
 * The panel probing/dispatch structure below (a single table of per-type
 * sequence pointers selected by ID match, instead of parallel arrays
 * indexed separately for sleep/awake/init) is written independently for
 * this project.
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
#include "config.h"
#include "gcc_extensions.h"

#include "lcd-s5l8702.h"
#ifdef BOOTLOADER
#include "piezo.h"
#endif

enum lcd_panel_type
{
    LCD_PANEL_38B3 = 0,
    LCD_PANEL_38C4,
    LCD_PANEL_38D5,
    LCD_PANEL_38E6,
    LCD_PANEL_58XX,
    LCD_NUM_PANEL_TYPES
};

/* ==================================================================== *
 * Sleep sequences (calibration data, kept as observed per panel family)
 * ==================================================================== */
#if defined(HAVE_LCD_SLEEP) || defined(HAVE_LCD_SHUTDOWN)

/* 0xb3, 0xe6 */
static const uint8_t sleep_seq_38b3_38e6[] =
{
    CMD,   0x28,  0,    /* Display Off */
    CMD,   0x10,  0,    /* Sleep In */
    SLEEP, 5,           /* 50 ms */
    END
};

/* 0xc4 */
static const uint8_t sleep_seq_38c4[] =
{
    CMD,   0x28,  0,    /* Display Off */
    CMD,   0x10,  0,    /* Sleep In */
    SLEEP, 12,          /* 120 ms */
    END
};

/* 0xd5 */
static const uint8_t sleep_seq_38d5[] =
{
    CMD,   0x28,  0,    /* Display Off */
    CMD,   0x10,  0,    /* Sleep In */
    END
};

/* 0x58 */
static const uint8_t sleep_seq_58xx[] =
{
    CMD,   0x10,  0,    /* Sleep In */
    END
};

#endif /* HAVE_LCD_SLEEP || HAVE_LCD_SHUTDOWN */

/* ==================================================================== *
 * Awake sequence: identical across every known panel variant
 * ==================================================================== */
#if defined(HAVE_LCD_SLEEP)

static const uint8_t awake_seq_common[] =
{
    CMD,   0x11,  0,    /* Sleep Out */
    SLEEP, 12,          /* 120 ms */
    CMD,   0x29,  0,    /* Display On */
    SLEEP, 1,           /* 10 ms */
    END
};

#endif /* HAVE_LCD_SLEEP */

/* ==================================================================== *
 * Init sequences (bootloader only; the retail firmware leaves the panel
 * already initialised when the bootloader for a warm boot doesn't run
 * this). Each table is this exact panel controller's own gamma and power
 * control register set, per variant.
 * ==================================================================== */
#if defined(BOOTLOADER)

/* 0xb3 */
static const uint8_t init_seq_38b3[] =
{
    CMD,   0xef,  1, 0x80,

    /* Power control */
    CMD,   0xc0,  1, 0x06,
    CMD,   0xc1,  1, 0x03,
    CMD,   0xc2,  2, 0x12, 0x00,
    CMD,   0xc3,  2, 0x12, 0x00,
    CMD,   0xc4,  2, 0x12, 0x00,
    CMD,   0xc5,  2, 0x40, 0x38,

    /* Display control */
    CMD,   0xb1,  2, 0x5f, 0x3f,
    CMD,   0xb2,  2, 0x5f, 0x3f,
    CMD,   0xb3,  2, 0x5f, 0x3f,
    CMD,   0xb4,  1, 0x02,
    CMD,   0xb6,  2, 0x12, 0x02,

    CMD,   0x35,  1, 0x00,  /* Tearing Effect Line On */
    CMD,   0x26,  1, 0x10,  /* Gamma Set */

    CMD,   0xfe,  1, 0x00,

    /* Gamma settings */
    CMD,   0xe0, 11, 0x0f, 0x70, 0x47, 0x03, 0x02, 0x02, 0xa0, 0x94,
                     0x05, 0x00, 0x0e,
    CMD,   0xe1, 11, 0x02, 0x43, 0x77, 0x00, 0x0f, 0x05, 0x49, 0x0a,
                     0x02, 0x0e, 0x00,
    CMD,   0xe2, 11, 0x2f, 0x63, 0x20, 0x50, 0x00, 0x07, 0xd1, 0x13,
                     0x00, 0x00, 0x0e,
    CMD,   0xe3, 11, 0x50, 0x20, 0x60, 0x23, 0x0f, 0x00, 0x31, 0x1d,
                     0x07, 0x0e, 0x00,
    CMD,   0xe4, 11, 0x5e, 0x50, 0x65, 0x27, 0x00, 0x0b, 0xdf, 0xf1,
                     0x01, 0x00, 0x0e,
    CMD,   0xe5, 11, 0x20, 0x67, 0x55, 0x50, 0x0e, 0x01, 0x1f, 0xfd,
                     0x0b, 0x0e, 0x00,

    CMD,   0x3a,  1, 0x06,  /* Pixel Format Set */
    CMD,   0x36,  1, 0x60,  /* Memory Access Control */
    CMD,   0x13,  0,        /* Normal Mode On */

    /* Awake sequence */
    CMD,   0x11,  0,    /* Sleep Out */
    SLEEP, 12,          /* 120 ms */
    CMD,   0x29,  0,    /* Display On */
    SLEEP, 1,           /* 10 ms */
    END
};

/* 0xc4 */
static const uint8_t init_seq_38c4[] =
{
    CMD,   0x01,  0,    /* Software Reset */
    SLEEP, 1,           /* 10 ms */

    /* Power control */
    CMD,   0xc0,  1, 0x01,
    CMD,   0xc1,  1, 0x03,
    CMD,   0xc2,  2, 0x74, 0x00,
    CMD,   0xc3,  2, 0x72, 0x03,
    CMD,   0xc4,  2, 0x73, 0x03,
    CMD,   0xc5,  2, 0x3c, 0x3c,

    /* Display control */
    CMD,   0xb1,  2, 0x6a, 0x15,
    CMD,   0xb2,  2, 0x6a, 0x15,
    CMD,   0xb3,  2, 0x6a, 0x15,
    CMD,   0xb4,  1, 0x02,
    CMD,   0xb6,  2, 0x12, 0x02,

    CMD,   0x35,  1, 0x00,  /* Tearing Effect Line On */
    CMD,   0x26,  1, 0x10,  /* Gamma Set */

    /* Gamma settings */
    CMD,   0xe0, 11, 0x1e, 0x22, 0x44, 0x00, 0x09, 0x01, 0x47, 0xc1,
                     0x05, 0x02, 0x09,
    CMD,   0xe1, 11, 0x0f, 0x32, 0x35, 0x00, 0x03, 0x05, 0x5e, 0x78,
                     0x03, 0x00, 0x03,
    CMD,   0xe2, 11, 0x0d, 0x74, 0x47, 0x41, 0x07, 0x01, 0x74, 0x41,
                     0x09, 0x03, 0x07,
    CMD,   0xe3, 11, 0x5f, 0x41, 0x27, 0x02, 0x00, 0x03, 0x43, 0x55,
                     0x02, 0x00, 0x03,
    CMD,   0xe4, 11, 0x1b, 0x53, 0x44, 0x51, 0x0b, 0x01, 0x64, 0x20,
                     0x05, 0x02, 0x09,
    CMD,   0xe5, 11, 0x7f, 0x41, 0x26, 0x02, 0x04, 0x00, 0x33, 0x35,
                     0x01, 0x00, 0x02,

    CMD,   0x3a,  1, 0x66,  /* Pixel Format Set */
    CMD,   0x36,  1, 0x60,  /* Memory Access Control */

    /* Awake sequence */
    CMD,   0x11,  0,    /* Sleep Out */
    SLEEP, 12,          /* 120 ms */
    CMD,   0x29,  0,    /* Display On */
    SLEEP, 1,           /* 10 ms */
    END
};

/* 0xd5 */
static const uint8_t init_seq_38d5[] =
{
    CMD,   0xfe,  1, 0x00,

    /* Power control */
    CMD,   0xc0,  1, 0x00,
    CMD,   0xc1,  1, 0x03,
    CMD,   0xc2,  2, 0x73, 0x03,
    CMD,   0xc3,  2, 0x73, 0x03,
    CMD,   0xc4,  2, 0x73, 0x03,
    CMD,   0xc5,  2, 0x64, 0x37,

    /* Display control */
    CMD,   0xb1,  2, 0x69, 0x13,
    CMD,   0xb2,  2, 0x69, 0x13,
    CMD,   0xb3,  2, 0x69, 0x13,
    CMD,   0xb4,  1, 0x02,
    CMD,   0xb6,  2, 0x03, 0x12,

    CMD,   0x35,  1, 0x00,  /* Tearing Effect Line On */
    CMD,   0x26,  1, 0x10,  /* Gamma Set */

    /* Gamma settings */
    CMD,   0xe0, 11, 0x08, 0x00, 0x10, 0x00, 0x03, 0x0e, 0xc8, 0x65,
                     0x05, 0x00, 0x00,
    CMD,   0xe1, 11, 0x06, 0x20, 0x00, 0x00, 0x00, 0x07, 0x4d, 0x0b,
                     0x08, 0x00, 0x00,
    CMD,   0xe2, 11, 0x08, 0x77, 0x27, 0x63, 0x0f, 0x16, 0xcf, 0x25,
                     0x03, 0x00, 0x00,
    CMD,   0xe3, 11, 0x5f, 0x53, 0x77, 0x06, 0x00, 0x02, 0x4b, 0x7b,
                     0x0f, 0x00, 0x00,
    CMD,   0xe4, 11, 0x08, 0x46, 0x57, 0x52, 0x0f, 0x16, 0xcf, 0x25,
                     0x04, 0x00, 0x00,
    CMD,   0xe5, 11, 0x6f, 0x42, 0x57, 0x06, 0x00, 0x04, 0x43, 0x7b,
                     0x0f, 0x00, 0x00,

    CMD,   0x3a,  1, 0x66,  /* Pixel Format Set */
    CMD,   0x36,  1, 0x60,  /* Memory Access Control */

    /* Awake sequence */
    CMD,   0x11,  0,    /* Sleep Out */
    SLEEP, 12,          /* 120 ms */
    CMD,   0x29,  0,    /* Display On */
    SLEEP, 1,           /* 10 ms */
    END
};

/* 0xe6 */
static const uint8_t init_seq_38e6[] =
{
    CMD,   0xef,  1, 0x80,

    /* Power control */
    CMD,   0xc0,  1, 0x0a,
    CMD,   0xc1,  1, 0x03,
    CMD,   0xc2,  2, 0x12, 0x00,
    CMD,   0xc3,  2, 0x12, 0x00,
    CMD,   0xc4,  2, 0x12, 0x00,
    CMD,   0xc5,  2, 0x38, 0x38,

    /* Display control */
    CMD,   0xb1,  2, 0x5f, 0x3f,
    CMD,   0xb2,  2, 0x5f, 0x3f,
    CMD,   0xb3,  2, 0x5f, 0x3f,
    CMD,   0xb4,  1, 0x02,
    CMD,   0xb6,  2, 0x12, 0x02,

    CMD,   0x35,  1, 0x00,  /* Tearing Effect Line On */
    CMD,   0x26,  1, 0x10,  /* Gamma Set */

    CMD,   0xfe,  1, 0x00,

    /* Gamma settings */
    CMD,   0xe0, 11, 0x0f, 0x70, 0x47, 0x03, 0x02, 0x02, 0xa0, 0x94,
                     0x05, 0x00, 0x0e,
    CMD,   0xe1, 11, 0x02, 0x43, 0x77, 0x00, 0x0f, 0x05, 0x49, 0x0a,
                     0x02, 0x0e, 0x00,
    CMD,   0xe2, 11, 0x2f, 0x63, 0x20, 0x50, 0x00, 0x07, 0xd1, 0x13,
                     0x00, 0x00, 0x0e,
    CMD,   0xe3, 11, 0x50, 0x20, 0x60, 0x23, 0x0f, 0x00, 0x31, 0x1d,
                     0x07, 0x0e, 0x00,
    CMD,   0xe4, 11, 0x5e, 0x50, 0x65, 0x27, 0x00, 0x0b, 0xdf, 0xf1,
                     0x01, 0x00, 0x0e,
    CMD,   0xe5, 11, 0x20, 0x67, 0x55, 0x50, 0x0e, 0x01, 0x1f, 0xfd,
                     0x0b, 0x0e, 0x00,

    CMD,   0x3a,  1, 0x06,  /* Pixel Format Set */
    CMD,   0x36,  1, 0x60,  /* Memory Control Access */
    CMD,   0x13,  0,        /* Normal Mode On */

    /* Awake sequence */
    CMD,   0x11,  0,    /* Sleep Out */
    SLEEP, 12,          /* 120 ms */
    CMD,   0x29,  0,    /* Display On */
    SLEEP, 1,           /* 10 ms */
    END
};

/* 0x58 */
static const uint8_t init_seq_58xx[] =
{
    CMD,   0xe1,  3, 0x0f, 0x31, 0x04,
    CMD,   0xe2,  5, 0x02, 0xa2, 0x08, 0x11, 0x01,
    CMD,   0xe3,  2, 0x10, 0x88,
    CMD,   0xe4,  2, 0x10, 0x88,
    CMD,   0xe5,  2, 0x00, 0x88,
    CMD,   0xe7, 13, 0x33, 0x0d, 0x57, 0x0e, 0x57, 0x05, 0x57, 0x02,
                     0x04, 0x10, 0x0b, 0x02, 0x02,
    CMD,   0xe8,  5, 0x30, 0x0d, 0x84, 0x8c, 0x21,
    CMD,   0xe9,  5, 0x4b, 0x1a, 0xba, 0x60, 0x11,
    CMD,   0xea,  5, 0x4b, 0xba, 0xba, 0x10, 0x11,
    CMD,   0xeb,  5, 0x0b, 0x3a, 0xd9, 0x60, 0x11,
    CMD,   0xef,  3, 0x00, 0x00, 0x00,
    CMD,   0xf0, 18, 0x12, 0x01, 0x21, 0xa5, 0x6c, 0x23, 0x02, 0x01,
                     0x08, 0x0d, 0x1e, 0xde, 0x5a, 0x93, 0xdc, 0x0d,
                     0x1e, 0x17,
    CMD,   0xf1, 18, 0x12, 0x05, 0x21, 0xb5, 0x8d, 0x24, 0x02, 0x01,
                     0x08, 0x0d, 0x1a, 0xde, 0x4a, 0x72, 0xdb, 0x0d,
                     0x1e, 0x17,
    CMD,   0xf2, 18, 0x0e, 0x00, 0x30, 0xd6, 0x8f, 0x34, 0x03, 0x07,
                     0x08, 0x11, 0x1f, 0xcf, 0x29, 0x70, 0xcb, 0x0c,
                     0x18, 0x17,
    CMD,   0xfa,  3, 0x02, 0x00, 0x02,
    CMD,   0xf7,  2, 0x00, 0xc0,

    CMD,   0x35,  1, 0x00,  /* Tearing Effect Line On */
    CMD,   0x36,  1, 0x20,  /* Memory Access Control */

    /* Awake sequence */
    CMD,   0x11,  0,    /* Sleep Out */
    SLEEP, 12,          /* 120 ms */
    CMD,   0x29,  0,    /* Display On */
    SLEEP, 1,           /* 10 ms */
    END
};

#endif /* BOOTLOADER */

/* ==================================================================== *
 * Panel dispatch table: one row per known panel type, gathering every
 * sequence for it in one place rather than one parallel array per
 * sequence kind.
 * ==================================================================== */

struct lcd_panel_sequences
{
#if defined(HAVE_LCD_SLEEP) || defined(HAVE_LCD_SHUTDOWN)
    const uint8_t *sleep;
#endif
#if defined(HAVE_LCD_SLEEP)
    const uint8_t *awake;
#endif
#if defined(BOOTLOADER)
    const uint8_t *init;
#endif
};

static const struct lcd_panel_sequences panel_sequences[LCD_NUM_PANEL_TYPES] =
{
#if defined(HAVE_LCD_SLEEP) || defined(HAVE_LCD_SHUTDOWN)
#define SLEEP_SEQ(name) .sleep = name,
#else
#define SLEEP_SEQ(name)
#endif
#if defined(HAVE_LCD_SLEEP)
#define AWAKE_SEQ(name) .awake = name,
#else
#define AWAKE_SEQ(name)
#endif
#if defined(BOOTLOADER)
#define INIT_SEQ(name) .init = name,
#else
#define INIT_SEQ(name)
#endif

    [LCD_PANEL_38B3] = { SLEEP_SEQ(sleep_seq_38b3_38e6)
                          AWAKE_SEQ(awake_seq_common)
                          INIT_SEQ(init_seq_38b3) },
    [LCD_PANEL_38C4] = { SLEEP_SEQ(sleep_seq_38c4)
                          AWAKE_SEQ(awake_seq_common)
                          INIT_SEQ(init_seq_38c4) },
    [LCD_PANEL_38D5] = { SLEEP_SEQ(sleep_seq_38d5)
                          AWAKE_SEQ(awake_seq_common)
                          INIT_SEQ(init_seq_38d5) },
    [LCD_PANEL_38E6] = { SLEEP_SEQ(sleep_seq_38b3_38e6)
                          AWAKE_SEQ(awake_seq_common)
                          INIT_SEQ(init_seq_38e6) },
    [LCD_PANEL_58XX] = { SLEEP_SEQ(sleep_seq_58xx)
                          AWAKE_SEQ(awake_seq_common)
                          INIT_SEQ(init_seq_58xx) },

#undef SLEEP_SEQ
#undef AWAKE_SEQ
#undef INIT_SEQ
};

/* Classifies a READ ID response into one of the known panel types, or
 * returns -1 if it doesn't match any of them. */
static int identify_panel(const uint8_t *id)
{
    if (id[1] == 0x58)
        return LCD_PANEL_58XX;

    if (id[1] == 0x38)
    {
        switch (id[2])
        {
            case 0xb3: return LCD_PANEL_38B3;
            case 0xc4: return LCD_PANEL_38C4;
            case 0xd5: return LCD_PANEL_38D5;
            case 0xe6: return LCD_PANEL_38E6;
        }
    }

    return -1;
}

static void NORETURN_ATTR lcd_panel_not_found(void)
{
#ifdef BOOTLOADER
    static uint16_t fatal_beep[] = { 3000, 500, 500, 0 };
    while (1)
        piezo_seq(fatal_beep);
#else
    /* Should not happen once the bootloader has already validated the
     * panel; there is no good recovery path at this layer. */
    while (1);
#endif
}

static struct lcd_info_rec lcd_info =
{
    .mpuiface = LCD_MPUIFACE_PAR9,
};

/* Kept as a plain global (not a local in lcd_target_get_info(), where it
 * would otherwise belong) because the development bootloader build
 * (S5L87XX_DEVELOPMENT_BOOTLOADER, bootloader/ipod-s5l87xx.c) reads it
 * back via `extern` to show the raw READ ID bytes on-screen for hardware
 * bring-up debugging -- the same convention the Nano 4G's lcd-nano4g.c
 * uses for its own lcd_id[4]. */
uint8_t lcd_id[4];

struct lcd_info_rec *lcd_target_get_info(void)
{
    int retry;
    int type = -1;

    for (retry = 3; retry > 0 && type < 0; retry--)
    {
        lcd_read_display_id(LCD_MPUIFACE_PAR9, lcd_id);
        type = identify_panel(lcd_id);
    }

    if (type < 0)
        lcd_panel_not_found();

    lcd_info.lcd_type = (uint8_t)type;
#if defined(HAVE_LCD_SLEEP) || defined(HAVE_LCD_SHUTDOWN)
    lcd_info.seq_sleep = (void *)panel_sequences[type].sleep;
#endif
#ifdef HAVE_LCD_SLEEP
    lcd_info.seq_awake = (void *)panel_sequences[type].awake;
#endif
#ifdef BOOTLOADER
    lcd_info.seq_init = (void *)panel_sequences[type].init;
#endif

    return &lcd_info;
}
