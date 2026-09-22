/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 by Dave Chapman
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
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "config.h"

#include "inttypes.h"
#include "cpu.h"
#include "system.h"
#include "lcd.h"
#include "../kernel-internal.h"
#include "file_internal.h"
#include "storage.h"
#include "disk.h"
#include "font.h"
#include "backlight.h"
#include "backlight-target.h"
#include "button.h"
#include "panic.h"
#include "power.h"
#include "file.h"
#include "common.h"
#include "rb-loader.h"
#include "loader_strerror.h"
#include "version.h"
#include "powermgmt.h"
#include "usb.h"
#ifdef IPOD_NANO3G
#include "nand-target.h"
#endif
#ifdef NAND_CHECK
#include "string-extra.h"
#endif
#ifdef HAVE_SERIAL
#include "serial.h"
#endif

#include "s5l87xx.h"
#include "clocking-s5l8702.h"
#include "spi-s5l8702.h"
#include "i2c-s5l8702.h"
#include "gpio-s5l8702.h"
#include "pmu-target.h"
#if defined(IPOD_6G) || defined(IPOD_NANO3G)
#include "norboot-target.h"
#endif


#define ERR_RB      0
#define ERR_OF      1
#define ERR_STORAGE 2
#define ERR_LBA28   3

/* Safety measure - maximum allowed firmware image size.
   The largest known current (October 2009) firmware is about 6.2MB so
   we set this to 8MB.
*/
#define MAX_LOADSIZE (8*1024*1024)

#define LCD_RBYELLOW    LCD_RGBPACK(255,192,0)
#define LCD_REDORANGE   LCD_RGBPACK(255,70,0)
#define LCD_GREEN       LCD_RGBPACK(0,255,0)

extern void bss_init(void);
extern uint32_t _movestart;
extern uint32_t start_loc;

extern int line;

#ifndef S5L87XX_DEVELOPMENT_BOOTLOADER
#ifdef HAVE_BOOTLOADER_USB_MODE
static void usb_mode(void)
{
    int button;

    verbose = true;

    printf("Entering USB mode...");

    powermgmt_init();

    /* The code will ask for the maximum possible value */
    usb_charging_enable(USB_CHARGING_ENABLE);

    usb_init();
    usb_start_monitoring();

    /* Wait until USB is plugged */
    while (usb_detect() != USB_INSERTED)
    {
        printf("Plug USB cable");
        line--;
        sleep(HZ/10);
    }

    while(1)
    {
        button = button_get_w_tmo(HZ/10);

        if (button == SYS_USB_CONNECTED)
            break; /* Hit */

        if (usb_detect() == USB_EXTRACTED)
            break; /* Cable pulled */

        /* Wait for threads to connect or cable is pulled */
        printf("USB: Connecting...");
        line--;
    }

    if (button == SYS_USB_CONNECTED)
    {
        /* Got the message - wait for disconnect */
        printf("Bootloader USB mode");

        /* Ack the SYS_USB_CONNECTED polled from the button queue */
        usb_acknowledge(SYS_USB_CONNECTED_ACK, button_get_data());

        while(1)
        {
            button = button_get_w_tmo(HZ/2);
            /* SYS_USB_DISCONNECTED alone does NOT mean the cable was
             * pulled: usb.c's usb_release_exclusive_storage() broadcasts
             * it during ordinary re-configuration too. Observed on real
             * hardware: Windows issues a second SET_CONFIGURATION, whose
             * usb_core_do_set_config() calls release-then-request, and
             * the release's broadcast was taken here as "session over".
             * That called usb_close(), which posts USB_QUIT, whose
             * handler in usb_thread is thread_exit() -- permanently
             * killing the USB thread while the host was still talking to
             * us. Everything afterwards then wedged: the next bus reset
             * set usb_core.c's bus_reset_pending and no thread was left
             * to run USB_NOTIFY_BUS_RESET and clear it, so every later
             * SETUP packet was silently dropped by
             * usb_core_setup_received() and the host saw the device go
             * dead mid-session. Confirm the cable is really gone before
             * tearing the stack down. */
            if (button == SYS_USB_DISCONNECTED &&
                usb_detect() == USB_EXTRACTED)
                break;
            /* A host may request exclusive storage more than once in a
             * single session (same second SET_CONFIGURATION as above,
             * which re-runs usb_request_exclusive_storage() and
             * broadcasts a fresh SYS_USB_CONNECTED). Every broadcast
             * needs its own ack, or usb.c's usb_num_acks_to_expect never
             * reaches zero and usb_slave_mode(true) is never entered for
             * that request. */
            if (button == SYS_USB_CONNECTED)
                usb_acknowledge(SYS_USB_CONNECTED_ACK, button_get_data());
        }
    }

    /* We don't want the HDD to spin up if the USB is attached again */
    usb_close();
    printf("USB mode exit     ");
}
#endif /* HAVE_BOOTLOADER_USB_MODE */

#ifdef IPOD_NANO3G
/* Paged viewer for nand_debug_log() (nand-target.h/nand-nano3g.c): shows
 * a handful of recorded lines per screen, advanced with SELECT, so the
 * user can photograph each page in turn. Exists to diagnose a real
 * hardware finding: the normal (non-NAND_CHECK) USB mass-storage path
 * fails to enumerate on the host side with no other visibility into
 * which storage call, if any, ran before the USB session ended --
 * unlike the -DNAND_CHECK build's own simpler USB serving, which works
 * and could report over USB, the failure here IS in USB serving, so
 * this has to be readable without it. */
static void show_nand_debug_log(void)
{
    unsigned int total = nand_debug_log_count();
    unsigned int lines_per_page = 8;
    unsigned int page = 0;
    unsigned int total_pages;

    if (total == 0)
        return; /* nothing was ever logged; nothing to show */

    total_pages = (total + lines_per_page - 1) / lines_per_page;

    while (1)
    {
        unsigned int start = page * lines_per_page;
        unsigned int end = start + lines_per_page;
        unsigned int i;

        if (end > total)
            end = total;

        lcd_clear_display();
        lcd_set_foreground(LCD_WHITE);
        line = 0;
        printf("debug log page %u/%u", page + 1, total_pages);
        for (i = start; i < end; i++)
        {
            char text[NAND_DEBUG_LOG_LINE_LEN];
            nand_debug_log_get(i, text, sizeof(text));
            printf("%u: %s", i, text);
        }
        lcd_set_foreground(LCD_RBYELLOW);
        printf("SELECT: next  MENU: done");
        lcd_update();

        while (button_status() != BUTTON_NONE)
            sleep(HZ / 100);
        while (1)
        {
            int btn = button_status();
            if (btn == BUTTON_SELECT)
            {
                page = (page + 1) % total_pages;
                break;
            }
            if (btn == BUTTON_MENU)
                return;
            sleep(HZ / 100);
        }
    }
}
#endif /* IPOD_NANO3G */

#if defined(NAND_CHECK) && defined(HAVE_BOOTLOADER_USB_MODE) \
    && !defined(S5L87XX_DEVELOPMENT_BOOTLOADER)
/* The contributor NAND check (build with -DNAND_CHECK, run from DFU): show
 * what the driver identified and how the read-only mount went, add the
 * unit's model and firmware version, then serve the raw NAND over USB for
 * utils/ipodnano3g/nandcheck/nandcheck.py. Nothing is written to the NAND. */
/* The bootloader's backlight calls are stubs, and the check's screen has
 * come up too dim to read: drive the backlight at full brightness */
static void nand_check_light(void)
{
    backlight_hw_brightness(MAX_BRIGHTNESS_SETTING);
    backlight_hw_on();
}

/* The report's key lines, short enough for the screen; the whole report
 * goes to nandcheck.py */
static void nand_check_print(const char *report)
{
    static const char *const keys[] = {
        "banks", "row", "mode", "pagesize", "sparesize", "blocks",
        "blocks_unreliable", "validated", "recognized", "diagonly", "ftl",
        "verdict", "model", "swvr", "rawid", "probestop", "diagbanks",
        "wtest", "wsweep", "wstatus", "wloc", "wisolate",
#ifdef FTL_APPLE_COMPAT
        /* Apple-compat read-only mount diagnostic (see ftl-apple-nano3g.c
         * and nand-check-nano3g.c's FTL_APPLE_COMPAT block). Shown on the
         * LCD so the mount result is photographable without needing the
         * sector-0 USB read, which does not reliably enumerate here. */
        "apple_dl", "apple_dl0", "apple_dl1", "apple_dl2", "apple_dl3",
        "apple_dl4",
        "apple_bd",
        "apple_bd0", "apple_bd1", "apple_bd2", "apple_bd3", "apple_bd4",
        "apple_bd5", "apple_bd6", "apple_bd7", "apple_bd8", "apple_bd9",
        "apple_bd10", "apple_bd11", "apple_bd12", "apple_bd13", "apple_bd14",
        "apple_bd15",
        "apple_tm",
        /* read-only full-mount probe (APPLE_READ_MOUNT_ONLY) */
        "apple_mnt", "apple_sec", "apple_s0", "apple_s0h", "apple_s0sig",
        "apple_scan", "apple_nzh", "apple_low", "apple_lowh", "apple_sigh",
        "apple_ftlc", "apple_ftlu", "apple_ca0", "apple_ca1", "apple_ca2",
        "apple_lay", "apple_rmp", "apple_rm", "apple_uc",
        /* compact mount summary + typemap (APPLE_TYPEMAP_ONLY build) */
        "apple_mount", "apple_ftl_mounted", "apple_banks", "apple_pagesize",
        "apple_ppblock", "apple_blocks", "apple_userblocks_provisional",
        "apple_vflusn", "apple_ftlctrl", "apple_spare0",
        "apple_devinfo_captured",
#endif
    };
    static char text[SECTOR_SIZE];
    char *p, *nl, *sp;
    size_t i;

    strlcpy(text, report, sizeof(text));
    for (p = text; (nl = strchr(p, '\n')); p = nl + 1)
    {
        *nl = '\0';
        sp = strchr(p, ' ');
        if (!sp)
            continue;
        if (!strncmp(p, "ids ", 4))
        {
            /* bank 0's id; the others are in the report */
            printf("id %.8s", sp + 1);
            continue;
        }
        for (i = 0; i < ARRAYLEN(keys); i++)
            if ((size_t)(sp - p) == strlen(keys[i])
                && !strncmp(p, keys[i], sp - p))
                printf("%s", p);
    }
}

#ifdef FTL_APPLE_COMPAT
/* Paged, photographable view of the Apple diagnostic lines. The USB
 * sector-0 read has proven unreliable on this host (the mass-storage
 * function enumerates but Windows creates no disk object), and the plain
 * nand_check_print() scrolls lines off the top. This shows every "apple_"
 * report line 8 at a time, advanced with SELECT (MENU exits), so each page
 * can be photographed. Read-only; just reformats the already-built report. */
static void show_apple_report(const char *report)
{
    static char text[SECTOR_SIZE];
    /* line offsets into text[] for lines beginning with "apple_" */
    static const char *lines[64];
    unsigned int nlines = 0;
    char *p, *nl;
    unsigned int lines_per_page = 8;
    unsigned int page = 0, total_pages;

    strlcpy(text, report, sizeof(text));
    for (p = text; (nl = strchr(p, '\n')); p = nl + 1)
    {
        *nl = '\0';
        if (!strncmp(p, "apple_", 6) && nlines < ARRAYLEN(lines))
            lines[nlines++] = p;
    }
    /* trailing line without a newline */
    if (*p && !strncmp(p, "apple_", 6) && nlines < ARRAYLEN(lines))
        lines[nlines++] = p;

    if (nlines == 0)
        return;

    total_pages = (nlines + lines_per_page - 1) / lines_per_page;

    while (1)
    {
        unsigned int start = page * lines_per_page;
        unsigned int end = start + lines_per_page;
        unsigned int i;
        int btn;

        if (end > nlines)
            end = nlines;

        lcd_clear_display();
        nand_check_light();
        lcd_set_foreground(LCD_RBYELLOW);
        line = 0;
        printf("apple report %u/%u", page + 1, total_pages);
        lcd_set_foreground(LCD_WHITE);
        for (i = start; i < end; i++)
            printf("%s", lines[i]);
        lcd_set_foreground(LCD_RBYELLOW);
        printf("SELECT: next  MENU: done");
        lcd_update();

        while (button_status() != BUTTON_NONE)
            sleep(HZ / 100);
        while (1)
        {
            btn = button_status();
            if (btn == BUTTON_SELECT)
            {
                page = (page + 1) % total_pages;
                break;
            }
            if (btn == BUTTON_MENU)
                return;
            sleep(HZ / 100);
        }
    }
}
#endif /* FTL_APPLE_COMPAT */

static void nand_check(void)
{
    static struct SysCfg syscfg;
    char line[64];
    int rc;
    ssize_t n;

    nand_check_light();
    snprintf(line, sizeof(line), "battery %dmV", _battery_voltage());
    rc = storage_init();
    /* USB mode unmounts every volume when the host configures the device,
     * through the file system's locks and object lists */
    filesystem_init();

    lcd_set_foreground(LCD_RBYELLOW);
    printf("Nano 3G NAND check");
    lcd_set_foreground(LCD_WHITE);
    printf("%s", line);
    nand_check_note(line);
    nand_check_light();
    if (rc)
        printf("storage_init %d", rc);

    /* Which unit this is, but not its serial number */
    n = syscfg_read(&syscfg);
    if (n != -1)
    {
        size_t i, count = MIN(syscfg.header.num_entries, SYSCFG_MAX_ENTRIES);

        for (i = 0; i < count; i++)
        {
            const struct SysCfgEntry *e = &syscfg.entries[i];
            const uint32_t *w = (const uint32_t *)e->data;

            if (e->tag == SYSCFG_TAG_MODN)
                snprintf(line, sizeof(line), "model %.16s", e->data);
            else if (e->tag == SYSCFG_TAG_SWVR)
                snprintf(line, sizeof(line), "swvr %.16s", e->data);
            else if (e->tag == SYSCFG_TAG_HWVR)
                snprintf(line, sizeof(line), "hwvr %08lx",
                         (unsigned long)w[1]);
            else
                continue;
            nand_check_note(line);
        }
    }

#ifdef NAND_CHECK_ALLOW_WRITE_TEST
#ifndef NAND_CHECK_ALLOW_WRITE_TEST_BANK
#define NAND_CHECK_ALLOW_WRITE_TEST_BANK  0
#endif
#ifndef NAND_CHECK_ALLOW_WRITE_TEST_BLOCK
/* Deliberately not block 0: leaving the very first block of the very
 * first bank alone in case anything (including this driver's own
 * probing) ever comes to depend on reading it. Block 8 is arbitrary,
 * chosen only to be comfortably away from block 0 while still well
 * within any plausible chip's real (ID-table-sourced, not probed --
 * see NANO3G_ORIGINAL_NAND_FTL.md) capacity. */
#define NAND_CHECK_ALLOW_WRITE_TEST_BLOCK  8
#endif
    /* Real erase+program+read-back diagnostics for an unrecognised chip.
     * The first test destroys block 8 on CE0; if it passes, the bounded sweep
     * destroys block 16 and three midpoint-adjacent blocks on every repeatedly
     * identified matching CE. Off by default (a normal -DNAND_CHECK build
     * does not define this), explicitly enabled at build time, and gated by
     * a continuous SELECT hold plus the repeated full-ID CE mask. */
    printf("DESTRUCTIVE: CE0 block 8");
    printf("Each CE: 16, mid-1,mid,mid+1");
    {
        /* Two separate windows, not one: first give the user generous time
         * to press SELECT at all -- with a VISIBLE per-second countdown so
         * it's obvious the device is waiting and how long is left (the
         * earlier version waited silently, which read as "the prompt went
         * by too quick" on real hardware) -- then require a short
         * continuous hold once they've started, so a brief accidental tap
         * still doesn't trigger a real erase/write. */
        const int WAIT_SECS = 30;   /* plenty of time to react */
        const int HOLD_TENTHS = 15; /* ~1.5s continuous hold to confirm */
        int waited = 0;
        bool pressed = false;

        /* Visible countdown so it's obvious the device is waiting and how
         * long is left. Uses the file's printf() row mechanism (which owns
         * the row cursor); we print one countdown line per second, and it's
         * fine that they scroll -- the point is that SOMETHING visibly ticks
         * down, unlike the earlier silent wait that "went by too quick". */
        lcd_set_foreground(LCD_RBYELLOW);
        int last_shown = -1;
        while (waited < WAIT_SECS * 10) /* poll at 10 Hz */
        {
            if (button_status() == BUTTON_SELECT)
            {
                pressed = true;
                break;
            }
            /* Print a milestone only every 5 s (and the final 5,4,3,2,1),
             * so the report above stays readable rather than scrolling off
             * under 30 one-per-second lines. */
            int secs_left = WAIT_SECS - waited / 10;
            if (secs_left != last_shown &&
                (secs_left <= 5 || secs_left % 5 == 0))
            {
                printf("hold SELECT now... %d s left", secs_left);
                last_shown = secs_left;
            }
            sleep(HZ / 10);
            waited++;
        }
        lcd_set_foreground(LCD_WHITE);

        bool held_through = false;
        if (pressed)
        {
            int held = 0;
            held_through = true;
            lcd_set_foreground(LCD_GREEN);
            printf("SELECT held - keep holding...");
            lcd_set_foreground(LCD_WHITE);
            while (held < HOLD_TENTHS) /* continuous hold to confirm */
            {
                if (button_status() != BUTTON_SELECT)
                {
                    held_through = false;
                    break;
                }
                sleep(HZ / 10);
                held++;
            }
        }
        else
        {
            printf("no write-test; identify only");
        }

        if (!held_through)
        {
            nand_check_note(pressed ? "wstatus cancelled_release"
                                    : "wstatus skipped_timeout");
        }
        if (held_through
            && !(nand_check_physical_bank_mask()
                 & (1u << NAND_CHECK_ALLOW_WRITE_TEST_BANK)))
        {
            nand_check_note("wstatus ineligible_id");
            held_through = false;
        }
        if (held_through)
        {
            const struct nand_write_test_result *r =
                nand_check_write_test(NAND_CHECK_ALLOW_WRITE_TEST_BANK,
                                      NAND_CHECK_ALLOW_WRITE_TEST_BLOCK);
            snprintf(line, sizeof(line), "wtest erase %d write %d read %d",
                     r->erase_rc, r->write_rc, r->read_rc);
            nand_check_note(line);
            snprintf(line, sizeof(line), "wtest data %d meta %d salt %02x",
                     r->data_match, r->meta_match, r->pattern_salt);
            nand_check_note(line);

            /* The single-block result above passing doesn't say much
             * about the other banks in a multi-die package, or whether
             * that one block was just lucky -- run the broader, still
             * bounded sweep across every bank this chip has and a
             * handful of blocks per bank. Only reached if the single
             * test above didn't already report a problem, since this
             * sweep does several more real erases/programs on top of
             * it. */
            if (r->erase_rc == 0 && r->write_rc == 0 && r->read_rc >= 0
                && r->data_match && r->meta_match)
            {
                const struct nand_write_sweep_result *s =
                    nand_check_write_test_sweep();
                snprintf(line, sizeof(line),
                         "wsweep plan %x %lu %lu %lu %lu",
                         s->bank_mask,
                         (unsigned long)s->test_blocks[0],
                         (unsigned long)s->test_blocks[1],
                         (unsigned long)s->test_blocks[2],
                         (unsigned long)s->test_blocks[3]);
                nand_check_note(line);
                {
                    unsigned int pi;
                    for (pi = 0; pi < s->preflight_count; pi++)
                    {
                        const struct nand_write_preflight *p = &s->preflight[pi];
                        snprintf(line, sizeof(line),
                                 "wpre b%u k%lu r%d,%d m%02x%02x/%02x%02x",
                                 p->bank, (unsigned long)p->block,
                                 p->first_page_rc, p->second_page_rc,
                                 p->first_meta_prefix[0], p->first_meta_prefix[1],
                                 p->second_meta_prefix[0], p->second_meta_prefix[1]);
                        nand_check_note(line);
                    }
                }
                snprintf(line, sizeof(line), "wsweep %u/%u passed",
                         s->passed, s->attempted);
                nand_check_note(line);
                snprintf(line, sizeof(line),
                         "wisolate %x/%x map %d,%d,%d,%d",
                         s->isolation_preserved_mask,
                         s->isolation_tested_mask,
                         s->isolation_source[0], s->isolation_source[1],
                         s->isolation_source[2], s->isolation_source[3]);
                nand_check_note(line);
                {
                    unsigned int expected = s->block_count
                        * ((s->bank_mask & 1u ? 1u : 0u)
                         + (s->bank_mask & 2u ? 1u : 0u)
                         + (s->bank_mask & 4u ? 1u : 0u)
                         + (s->bank_mask & 8u ? 1u : 0u));
                    if (s->passed < s->attempted)
                    {
                        nand_check_note("wstatus failed");
                    snprintf(line, sizeof(line),
                             "wsweep fail bank %u block %lu",
                             s->fail_bank, (unsigned long)s->fail_block);
                    nand_check_note(line);
                    snprintf(line, sizeof(line),
                             "wsweep erase %d write %d read %d",
                             s->fail_result.erase_rc, s->fail_result.write_rc,
                             s->fail_result.read_rc);
                    nand_check_note(line);
                    snprintf(line, sizeof(line),
                             "wsweep match data %d meta %d salt %02x",
                             s->fail_result.data_match,
                             s->fail_result.meta_match,
                             s->fail_result.pattern_salt);
                    nand_check_note(line);
                    {
                        unsigned int li;
                        for (li = 0; li < s->fail_result.locator_count; li++)
                        {
                            snprintf(line, sizeof(line), "wloc %lu data %d meta %d",
                                     (unsigned long)s->fail_result.locator_blocks[li],
                                     s->fail_result.locator_data_match[li],
                                     s->fail_result.locator_meta_match[li]);
                            nand_check_note(line);
                        }
                    }
                    }
                    else if (s->attempted == 0 || s->bank_mask == 0
                             || s->attempted != expected
                             || s->isolation_tested_mask != s->bank_mask
                             || s->isolation_preserved_mask != s->bank_mask)
                    {
                        nand_check_note("wstatus incomplete_coverage");
                    }
                    else
                    {
                        nand_check_note("wstatus passed");
                    }
                }
            }
            else
            {
                nand_check_note("wstatus single_failed");
            }
        }
    }
#endif

    nand_check_print(nand_check_report());
#ifdef FTL_APPLE_COMPAT
    /* Page through the Apple diagnostic lines (photographable) before the
     * USB step, since the sector-0 USB read has been unreliable on the host
     * (mass-storage enumerates but no disk object is created). */
    show_apple_report(nand_check_report());
#endif
    lcd_set_foreground(LCD_RBYELLOW);
    printf("Photo, then connect USB");
    printf("and run nandcheck.py");
    lcd_set_foreground(LCD_WHITE);
    /* One USB session: usb_mode() starts power management each time, so it
     * must not run twice */
    usb_mode();
    nand_check_light();
    printf("Done. Hold MENU+SELECT");
    printf("to restart");
    while (1)
        sleep(HZ);
}
#endif

void fatal_error(int err)
{
    verbose = true;

    /* System font is 6 pixels wide */
    line++;
    switch (err)
    {
        case ERR_RB:
#ifdef HAVE_BOOTLOADER_USB_MODE
            usb_mode();
#ifdef IPOD_NANO3G
            /* Show whatever the real (non-NAND_CHECK) storage glue
             * recorded during that USB session, if anything, before
             * moving on to the normal "hold to reboot" prompt -- see
             * show_nand_debug_log()'s own comment for why this exists. */
            show_nand_debug_log();
#endif
            printf("Hold MENU+SELECT to reboot");
            break;
#endif
        case ERR_STORAGE:
            printf("Hold MENU+SELECT to reboot");
            printf("then SELECT+PLAY for disk mode");
            break;
        case ERR_OF:
            printf("Hold MENU+SELECT to reboot");
            printf("and enter Rockbox firmware");
            break;
        case ERR_LBA28:
            printf("Hold MENU+SELECT to reboot");
            printf("and LEFT if you are REALLY sure");
            break;
    }

#if (CONFIG_STORAGE & STORAGE_ATA)
    if (ide_powered())
        ata_sleepnow(); /* Immediately spindown the disk. */
#endif

    line++;
    lcd_set_foreground(LCD_REDORANGE);
    while (1) {
        lcd_puts(0, line, button_hold() ? "Hold switch on!"
                                        : "               ");
        lcd_update();
    }
}

#if (CONFIG_STORAGE & STORAGE_ATA)
extern unsigned short battery_level_disksafe;
static void battery_trap(void)
{
    int vbat, old_verb;
    int th = 50;

    old_verb = verbose;
    verbose = true;

    usb_charging_maxcurrent_change(100);

    while (1)
    {
        vbat = _battery_voltage();

        /*  Two reasons to use this threshold (may require adjustments):
         *  - when USB (or wall adaptor) is plugged/unplugged, Vbat readings
         *    differ as much as more than 200 mV when charge current is at
         *    maximum (~340 mA).
         *  - RB uses some sort of average/compensation for battery voltage
         *    measurements, battery icon blinks at battery_level_disksafe,
         *    when the HDD is used heavily (large database) the level drops
         *    to battery_level_shutoff quickly.
         */
        if (vbat >= battery_level_disksafe + th)
            break;
        th = 200;

        if (power_input_status() != POWER_INPUT_NONE) {
            lcd_set_foreground(LCD_RBYELLOW);
            printf("Low battery: %d mV, charging...     ", vbat);
            sleep(HZ*3);
        }
        else {
            /* Wait for the user to insert a charger */
            int tmo = 10;
            lcd_set_foreground(LCD_REDORANGE);
            while (1) {
                vbat = _battery_voltage();
                printf("Low battery: %d mV, power off in %d ", vbat, tmo);
                if (!tmo--) {
                    /* Raise Vsysok (hyst=0.02*Vsysok) to avoid PMU
                       standby<->active looping */
                    if (vbat < 3200)
                        pmu_write(PCF5063X_REG_SVMCTL, 0xA /*3200mV*/);
                    power_off();
                }
                sleep(HZ*1);
                if (power_input_status() != POWER_INPUT_NONE)
                    break;
                line--;
            }
        }
        line--;
    }

    verbose = old_verb;
    lcd_set_foreground(LCD_WHITE);
    printf("Battery status ok: %d mV            ", vbat);
}
#endif /* CONFIG_STORAGE & STORAGE_ATA */
#endif /* S5L87XX_DEVELOPMENT_BOOTLOADER */

#ifdef NANO3G_ERASE_ALL
/* Erase every block of every bank, returning the NAND to blank silicon.
 *
 * Purpose: a Nano 3G formatted by this port cannot be restored by iTunes.
 * Apple's bootloader fails storage init before it ever offers disk mode, and
 * disk mode launched directly from NOR (see launch_flsh_image()) loads and
 * verifies but dies on the handoff. The remaining hypothesis is that Apple's
 * firmware can cope with a *blank* chip -- it has to, since that is how these
 * leave the factory -- but not with a chip carrying a foreign FTL's metadata
 * in every spare area. This puts the chip back into the state Apple's code
 * was built to initialise.
 *
 * Deliberately talks to nand_hw_* directly and never touches ftl_*: there is
 * no filesystem here to mount and nothing to preserve.
 *
 * Note on bad blocks: this does not try to honour factory bad-block markers,
 * because formatting for Rockbox has already overwritten the spare areas that
 * held them, so there is nothing left to honour. Blocks that are genuinely
 * bad announce themselves by failing to erase, and those are counted and
 * reported; Apple's firmware rediscovers them the same way.
 *
 * Meant to be run volatile from DFU (mks5lboot --mkdfu-raw).
 */
static void erase_whole_nand(void)
{
    unsigned int banks, b;
    unsigned long total = 0, failed = 0;
    int first_err = 0;
    unsigned int first_err_bank = 0;
    unsigned long first_err_block = 0;

    printf("Full NAND erase");

    if (nand_hw_init() != 0) {
        lcd_set_foreground(LCD_REDORANGE);
        printf("nand_hw_init failed");
        lcd_set_foreground(LCD_WHITE);
        return;
    }

    banks = nand_scan_banks();
    if (banks == 0) {
        lcd_set_foreground(LCD_REDORANGE);
        printf("no banks found (id 0x%lx)", (unsigned long)nand_get_id());
        lcd_set_foreground(LCD_WHITE);
        return;
    }
    printf("%u banks", banks);

    /* Counts down rather than requiring a press, so a device whose buttons
     * are not responding can still be erased -- this image is only ever sent
     * deliberately, and the DFU send is itself the confirmation. */
    lcd_set_foreground(LCD_RBYELLOW);
    for (int t = 5; t > 0; t--) {
        printf("ERASING EVERYTHING in %d  (MENU aborts)", t);
        line--;
        if (button_status() == BUTTON_MENU) {
            line++;
            lcd_set_foreground(LCD_WHITE);
            printf("aborted                          ");
            return;
        }
        sleep(HZ);
    }
    line++;
    lcd_set_foreground(LCD_WHITE);
    printf("erasing...                       ");

    for (b = 0; b < banks; b++) {
        const struct nand_geometry *geo = nand_get_bank_geometry(b);
        unsigned long blocks, blk;
        unsigned long bank_failed = 0;

        if (!geo)
            continue;
        blocks = geo->blocks_per_bank;

        for (blk = 0; blk < blocks; blk++) {
            int rc = nand_hw_erase_block(b, blk);
            total++;
            if (rc != 0) {
                failed++;
                bank_failed++;
                if (!first_err) {
                    first_err = rc;
                    first_err_bank = b;
                    first_err_block = blk;
                }
            }
            /* Progress without scrolling the display off: rewrite one line. */
            if ((blk & 0xff) == 0) {
                printf("bank %u: %lu/%lu  bad %lu",
                       b, blk, blocks, bank_failed);
                line--;
                lcd_update();
            }
        }
        printf("bank %u: %lu blocks, %lu failed  ",
               b, blocks, bank_failed);
        lcd_update();
    }

    line++;
    if (failed == 0)
        lcd_set_foreground(LCD_GREEN);
    else
        lcd_set_foreground(LCD_RBYELLOW);
    printf("erased %lu blocks, %lu failed", total, failed);
    if (failed)
        printf("first: bank %u block %lu rc %d",
               first_err_bank, first_err_block, first_err);
    lcd_set_foreground(LCD_WHITE);
    printf("NAND is now blank.");
    printf("Hold MENU+SELECT to reboot.");
    lcd_update();

    while (1)
        sleep(HZ);
}
#endif /* NANO3G_ERASE_ALL */

#ifdef NANO3G_LAUNCH_FLSH
/* Load and run one of Apple's NOR-resident images directly, bypassing
 * Apple's bootloader.
 *
 * Exists because a Nano 3G whose NAND has been formatted by Rockbox cannot
 * be restored by iTunes through the usual route: Apple's bootloader fails
 * storage init (red "X" icon) and its SELECT+PLAY disk-mode combo never
 * gets a chance to run, so iTunes never sees a device to restore. The
 * images iTunes needs are not on the NAND at all -- the 'flsh' directory in
 * the last 0x200 bytes of NOR lists 'disk' (disk mode) and 'diag' among
 * others, each with its own load address -- so they survive any amount of
 * NAND damage and can be started without Apple's bootloader agreeing to.
 *
 * Note the names in that directory are stored byte-reversed per word, which
 * is why the callers below pass "ksid" and "gaid" rather than "disk" and
 * "diag" (compare utils/ipod/flashsplit/flashsplit.c, which matches the
 * whole 8-byte "hslfksid").
 *
 * Meant to be run volatile from DFU (mks5lboot --mkdfu-raw); nothing here
 * writes to the device.
 */
static void launch_flsh_image(const char *revname, const char *label)
{
    /* flsh_load_file() verifies a SHA1 over the whole 0x200 header, so this
     * must be exactly that size and outlive the call. */
    static uint8_t hdr[FILEHDR_SZ] __attribute__((aligned(16)));
    image_t entry;
    int rc;

    printf("Looking for '%s' in NOR...", label);

    if (!flsh_find_file((char *)revname, &entry)) {
        lcd_set_foreground(LCD_REDORANGE);
        printf("not in the NOR directory");
        lcd_set_foreground(LCD_RBYELLOW);
        printf("SELECT to continue booting");
        lcd_set_foreground(LCD_WHITE);
        lcd_update();
        while (button_status() != BUTTON_SELECT)
            sleep(HZ/100);
        return;
    }

    printf(" off 0x%x len 0x%x", entry.devOffset, entry.len);
    printf(" addr 0x%x entry 0x%x", entry.addr, entry.entryOffset);
    lcd_update();

    /* Loads to entry.addr, which for 'disk' and 'diag' is DRAM, well clear
     * of this bootloader in IRAM -- unlike launch_onb(), which overwrites
     * IRAM0 and the exception vector table. */
    rc = flsh_load_file(&entry, hdr, (void *)entry.addr);
    if (rc != 0) {
        lcd_set_foreground(LCD_REDORANGE);
        /* -1 corrupt header, -2 bad size, -3 corrupt data (post-decrypt
         * SHA1 mismatch, i.e. the hardware key did not produce valid
         * plaintext) */
        printf("load failed: %d", rc);
        lcd_set_foreground(LCD_RBYELLOW);
        printf("SELECT to continue booting");
        lcd_set_foreground(LCD_WHITE);
        lcd_update();
        while (button_status() != BUTTON_SELECT)
            sleep(HZ/100);
        return;
    }

    lcd_set_foreground(LCD_GREEN);
    printf("loaded and verified OK");
    lcd_set_foreground(LCD_RBYELLOW);
    printf("SELECT: start %s", label);
    printf("MENU: skip");
    lcd_set_foreground(LCD_WHITE);
    lcd_update();

    /* Deliberately wait for a keypress rather than jumping straight away.
     * Whatever we jump to takes over the screen immediately, so without this
     * there is no way to tell "we failed to load the image" apart from "the
     * image loaded and then failed by itself" -- which are very different
     * problems when the device being recovered shows an error either way. */
    while (button_status() != BUTTON_NONE)
        sleep(HZ/100);
    while (1) {
        int btn = button_status();
        if (btn == BUTTON_SELECT)
            break;
        if (btn == BUTTON_MENU)
            return;
        sleep(HZ/100);
    }

    disable_irq();
    eint_init();
    commit_discard_idcache();

    asm volatile("mov pc, %0"::"r"(entry.addr + entry.entryOffset));
    while (1);
}
#endif /* NANO3G_LAUNCH_FLSH */

static int launch_onb(int clkdiv)
{
#if defined(IPOD_6G) || defined(IPOD_NANO3G)
    /* SPI clock = PClk/(clkdiv+1) */
    spi_clkdiv(SPI_PORT, clkdiv);

    /* Actually IRAM1_ORIG contains current RB bootloader IM3 header,
       it will be replaced by ONB IM3 header, so this function must
       be called once!!! */
    struct Im3Info *hinfo = (struct Im3Info*)IRAM1_ORIG;

    /* Loads ONB in IRAM0, exception vector table is destroyed !!! */
    int rc = im3_read(
            NORBOOT_OFF + im3_nor_sz(hinfo), hinfo, (void*)IRAM0_ORIG);

    if (rc != 0) {
        /* Restore exception vector table */
        memcpy((void*)IRAM0_ORIG, &_movestart, 4*(&start_loc-&_movestart));
        commit_discard_idcache();
        return rc;
    }

    /* Disable all external interrupts */
    eint_init();

    commit_discard_idcache();

    /* Branch to start of IRAM */
    asm volatile("mov pc, %0"::"r"(IRAM0_ORIG));
    while(1);
#elif defined(IPOD_NANO4G)
    (void) clkdiv;

    lcd_set_foreground(LCD_REDORANGE);
    printf("Not implemented");

    line++;
    lcd_set_foreground(LCD_RBYELLOW);
    printf("Press SELECT to continue");
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);

    return 0;
#endif
}

/* Launch OF when kernel mode is running */
static int kernel_launch_onb(void)
{
    disable_irq();
    int rc = launch_onb(3); /* 54/4 = 13.5 MHz. */
    enable_irq();
    return rc;
}

/*  The boot sequence is executed on power-on or reset. After power-up
 *  the device could come from a state of hibernation, OF hibernates
 *  the iPod after an inactive period of ~30 minutes, on this state the
 *  SDRAM is in self-refresh mode.
 *
 *  t0 = 0
 *     S5L8702 BOOTROM loads an IM3 image located at NOR:
 *     - IM3 header (first 0x800 bytes) is loaded at IRAM1_ORIG
 *     - IM3 body (decrypted RB bootloader) is loaded at IRAM0_ORIG
 *     The time needed to load the RB bootloader (~100 Kb) is estimated
 *     on 200~250 ms. Once executed, RB booloader moves itself from
 *     IRAM0_ORIG to IRAM1_ORIG+0x800, preserving current IM3 header
 *     that contains the NOR offset where the ONB (original NOR boot),
 *     is located (see dualboot.c for details).
 *
 *  t1 = ~250 ms.
 *     If the PMU is hibernated, decrypted ONB (size 128Kb) is loaded
 *       and executed, it takes ~120 ms. Then the ONB restores the
 *       iPod to the state prior to hibernation.
 *     If not, initialize system and RB kernel, wait for t2.
 *
 *  t2 = ~650 ms.
 *     Check user button selection.
 *     If OF, diagmode, or diskmode is selected then launch ONB.
 *     If not, wait for LCD initialization.
 *
 *  t3 = ~700,~900 ms. (lcd_type_01,lcd_type_23)
 *     LCD is initialized, baclight ON.
 *     Wait for HDD spin-up.
 *
 *  t4 = ~2600,~2800 ms.
 *     HDD is ready.
 *     If hold switch is locked, then load and launch ONB.
 *     If not, load rockbox.ipod file from HDD.
 *
 *  t5 = ~2800,~3000 ms.
 *     rockbox.ipod is executed.
 */

#ifdef S5L87XX_DEVELOPMENT_BOOTLOADER
#include "piezo.h"
#include "lcd-s5l8702.h"
extern int lcd_type;

static uint16_t alive[] = { 500,100,0, 0 };
static uint16_t alivelcd[] = { 2000,200,0, 0 };

#ifdef HAVE_LCD_SLEEP
static void sleep_test(void)
{
    int sleep_tmo = 5;
    int awake_tmo = 3;

    lcd_clear_display();
    lcd_set_foreground(LCD_WHITE);
    line = 0;

    printf("Entering LCD sleep mode in %d seconds,", sleep_tmo);
    printf("during sleep mode you will see a white");
    printf("screen for about %d seconds.", awake_tmo);
    while (sleep_tmo--) {
        printf("Sleep in %d...", sleep_tmo);
        sleep(HZ*1);
    }
    lcd_sleep();
    sleep(HZ*awake_tmo);
    lcd_awake();

    line++;
    printf("Awake!");

    line++;
    lcd_set_foreground(LCD_RBYELLOW);
    printf("Press SELECT to continue");
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);
}
#endif

static void pmu_info(void)
{
    int loop = 0;

    lcd_clear_display();
    lcd_update();
    while (button_status() != BUTTON_NONE);

    while (1)
    {
        lcd_set_foreground(LCD_WHITE);
        lcd_clear_display();
        line = 0;
        printf("loop: %d", loop++);

        for (int i = 0; i < 128; i += 8)
        {
            unsigned char buf[8];

#if defined(IPOD_NANO3G)
            if (i == 0) {
                static int flip = 0;
                if (flip) {
                    pmu_write(6, 0xff);
                    pmu_write(7, 0xff);
                }
                else {
                    pmu_write(6, 0xe7);
                    pmu_write(7, 0xfe);
                }
                flip ^= 1;
            }
#elif defined(IPOD_NANO4G)
            if (i == 120)
                for (int j = 0; j < 8; j++)
                    pmu_write(i+j, j);
#endif
            for (int j = 0; j < 8; j++)
                buf[j] = pmu_read(i+j);

            printf(" %2x: %2x %2x %2x %2x %2x %2x %2x %2x", i,
                    buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);
        }
        line++;
        printf("USB: %s    ", (usb_detect() == USB_INSERTED) ? "inserted" : "not inserted");
#if CONFIG_CHARGING
        printf("Firewire: %s    ", pmu_firewire_present() ? "inserted" : "not inserted");
#endif
#ifdef IPOD_ACCESSORY_PROTOCOL
        printf("Accessory: %s    ", pmu_accessory_present() ? "inserted" : "not inserted");
#endif
        printf("Hold Switch: %s  ", pmu_holdswitch_locked() ? "locked" : "unlocked");
        line++;
        lcd_set_foreground(LCD_RBYELLOW);
        printf("Press SELECT to continue");
        if (button_status() == BUTTON_SELECT)
            break;
        sleep(HZ/2);
    }
}

static void gpio_info(void)
{
    int loop = 0;

    lcd_clear_display();

    while (1)
    {
        lcd_set_foreground(LCD_WHITE);
        lcd_clear_display();
        line = 0;
        printf("loop: %d", loop++);
        for (int i = 0; i < GPIO_N_GROUPS; i ++)
        {
            printf(" %x: %8x %2x %4x %2x %2x", i,
                    PCON(i), PDAT(i), PUNA(i), PUNB(i), PUNC(i));
        }
        line++;
        lcd_set_foreground(LCD_RBYELLOW);
        printf("Press SELECT to continue");
        if (button_status() == BUTTON_SELECT)
            break;
        sleep(HZ/5);
    }
}

static void run_of(void)
{
    int tmo = 5;
    lcd_clear_display();
    lcd_set_foreground(LCD_WHITE);
    line = 0;
    while (tmo--) {
        printf("Booting OF in %d...", tmo);
        sleep(HZ*1);
    }

    int rc = kernel_launch_onb();
    printf("Load OF error: %d", rc);
    sleep(HZ*10);
}

#if defined(IPOD_6G) || defined(IPOD_NANO3G)
static void print_syscfg(void)
{
    lcd_clear_display();
    lcd_set_foreground(LCD_WHITE);
    line = 0;

    struct SysCfg syscfg;
    const ssize_t result = syscfg_read(&syscfg);

    if (result == -1) {
        printf("SCfg magic not found. NOR flash is corrupted.");
        goto end;
    }

    printf("Total size: %lu bytes, %lu entries", syscfg.header.size, syscfg.header.num_entries);

    if (result > 0) {
        printf("Wrong size: expected %ld, got %lu", result, syscfg.header.size);
    }

    if (syscfg.header.num_entries > SYSCFG_MAX_ENTRIES) {
        printf("Too many entries, showing only first %u", SYSCFG_MAX_ENTRIES);
    }

    const size_t syscfg_num_entries = MIN(syscfg.header.num_entries, SYSCFG_MAX_ENTRIES);

    for (size_t i = 0; i < syscfg_num_entries; i++) {
        const struct SysCfgEntry* entry = &syscfg.entries[i];
        const char* tag = (char *)&entry->tag;
        const uint32_t* data32 = (uint32_t *)entry->data;

        switch (entry->tag) {
        case SYSCFG_TAG_SRNM:
            printf("Serial number (SrNm): %s", entry->data);
            break;
        case SYSCFG_TAG_FWID:
            printf("Firmware ID (FwId): %07lX", data32[1] & 0x0FFFFFFF);
            break;
        case SYSCFG_TAG_HWID:
            printf("Hardware ID (HwId): %08lX", data32[0]);
            break;
        case SYSCFG_TAG_HWVR:
            printf("Hardware version (HwVr): %06lX", data32[1]);
            break;
        case SYSCFG_TAG_CODC:
            printf("Codec (Codc): %s", entry->data);
            break;
        case SYSCFG_TAG_SWVR:
            printf("Software version (SwVr): %s", entry->data);
            break;
        case SYSCFG_TAG_MLBN:
            printf("Logic board serial number (MLBN): %s", entry->data);
            break;
        case SYSCFG_TAG_MODN:
            printf("Model number (Mod#): %s", entry->data);
            break;
        case SYSCFG_TAG_REGN:
            printf("Sales region (Regn): %08lX %08lX", data32[0], data32[1]);
            break;
        default:
            printf("%c%c%c%c: %08lX %08lX %08lX %08lX",
                tag[3], tag[2], tag[1], tag[0],
                data32[0], data32[1], data32[2], data32[3]
            );
            break;
        }
    }

end:
    line++;
    lcd_set_foreground(LCD_RBYELLOW);
    printf("Press SELECT to continue");
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);
}

static void print_bootloader_hash(void)
{
    lcd_clear_display();
    lcd_set_foreground(LCD_WHITE);
    line = 0;

    struct Im3Info hinfo;
    int rc = im3_read(NORBOOT_OFF, &hinfo, NULL);

    if (rc != 0) {
        printf("Error loading the primary bootloader: %d", rc);
        goto end;
    }

    unsigned char primary_hash[SIGN_SZ];

    memcpy(primary_hash, hinfo.u.enc12.data_sign, SIGN_SZ);
    hwkeyaes(HWKEYAES_DECRYPT, HWKEYAES_UKEY, primary_hash, SIGN_SZ);

    unsigned bl_nor_sz = im3_nor_sz(&hinfo);
    rc = im3_read(NORBOOT_OFF + bl_nor_sz, &hinfo, NULL);

    if (rc == 0) {
        // Rockbox bootloader is installed as primary
        // Stock bootloader is backed up
        unsigned char backup_hash[SIGN_SZ];
        memcpy(backup_hash, hinfo.u.enc12.data_sign, SIGN_SZ);
        hwkeyaes(HWKEYAES_DECRYPT, HWKEYAES_UKEY, backup_hash, SIGN_SZ);

        printf("Rockbox bootloader hash:");

        for (int i = 0; i < SIGN_SZ; i++) {
            lcd_putsf(i * 2, line, "%02X", primary_hash[i]);
        }

        line += 2;
        lcd_update();

        printf("Stock bootloader hash:");

        for (int i = 0; i < SIGN_SZ; i++) {
            lcd_putsf(i * 2, line, "%02X", backup_hash[i]);
        }

        line++;
        lcd_update();
    }
    else {
        // Stock bootloader is installed as primary
        // No backup bootloader
        printf("Rockbox bootloader is not installed!");
        line++;

        printf("Stock bootloader hash:");

        for (int i = 0; i < SIGN_SZ; i++) {
            lcd_putsf(i * 2, line, "%02X", primary_hash[i]);
        }

        line++;
        lcd_update();
    }

end:
    line++;
    while (button_status() != BUTTON_NONE)
        sleep(HZ/100);
    lcd_set_foreground(LCD_RBYELLOW);
    printf("Press SELECT to continue");
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);
}

#ifdef HAVE_SERIAL

#define FLASH_PAGES (FLASH_SIZE >> 12)
#define FLASH_PAGE_SIZE (FLASH_SIZE >> 8)

static void dump_bootflash(void)
{
    lcd_clear_display();
    lcd_set_foreground(LCD_WHITE);
    line = 0;

    uint8_t page[FLASH_PAGE_SIZE];
    printf("Total pages: %d", FLASH_PAGES);

    bootflash_init(SPI_PORT);

    for (int i = 0; i < FLASH_PAGES; i++) {
        printf("Reading flash... %d", i + 1);
        bootflash_read(SPI_PORT, i << 12, FLASH_PAGE_SIZE, page);

        printf("Sending over UART... %d", i + 1);
        serial_tx_raw(page, FLASH_PAGE_SIZE);
        line -= 2;
    }

    bootflash_close(SPI_PORT);

    line += 2;
    printf("Done!");
    piezo_seq(alive);

    line++;
    lcd_set_foreground(LCD_RBYELLOW);
    printf("Press SELECT to continue");
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);
}
#endif /* HAVE_SERIAL */
#endif /* IPOD_6G || IPOD_NANO3G */

static void devel_menu(void)
{
    const char *items[] = {
#ifdef HAVE_LCD_SLEEP
        "LCD sleep/awake test",
#endif
        "PMU info",
        "GPIO info",
#if defined(IPOD_6G) || defined(IPOD_NANO3G)
        "Show SysCfg",
        "Show bootloader hash",
#ifdef HAVE_SERIAL
        "Dump bootflash to UART",
#endif
#endif
        "Launch OF",
        //"Launch Rockbox",
        "Restart",
        "Power off",
    };
    void (*handlers[])(void) = {
#ifdef HAVE_LCD_SLEEP
        sleep_test,
#endif
        pmu_info,
        gpio_info,
#if defined(IPOD_6G) || defined(IPOD_NANO3G)
        print_syscfg,
        print_bootloader_hash,
#ifdef HAVE_SERIAL
        dump_bootflash,
#endif
#endif
        run_of,
        //run_rockbox,
        system_reboot,
        power_off,
    };
    const size_t items_count = sizeof(items) / sizeof(items[0]);
    unsigned char selected_item = 0;

    while (1)
    {
        lcd_clear_display();
        lcd_set_foreground(LCD_RBYELLOW);
        line = 0;
        printf("Development menu");

        for (size_t i = 0; i < items_count; i++) {
            lcd_set_foreground(i == selected_item ? LCD_GREEN : LCD_WHITE);
            printf(items[i]);
        }

        while (button_status() != BUTTON_NONE);

        bool done = false;
        while (!done)
        {
            switch (button_status())
            {
                case BUTTON_MENU:
                case BUTTON_LEFT:
                    if (selected_item > 0) {
                        selected_item--;
                        done = true;
                    }
                    else {
                        sleep(HZ/100);
                    }
                    break;
                case BUTTON_PLAY:
                case BUTTON_RIGHT:
                    if (selected_item < items_count - 1) {
                        selected_item++;
                        done = true;
                    }
                    else {
                        sleep(HZ/100);
                    }
                    break;
                case BUTTON_SELECT:
                    handlers[selected_item]();
                    done = true;
                    break;
                default:
                    sleep(HZ/100);
                    break;
            }
        }
    }
}
#endif /* S5L87XX_DEVELOPMENT_BOOTLOADER */

void main(void)
{
    int rc = 0;

    usec_timer_init();

#ifdef S5L87XX_DEVELOPMENT_BOOTLOADER
    piezo_seq(alive);
#endif

    /* Configure I2C0 */
    i2c_preinit(0);

    if (pmu_is_hibernated()) {
        rc = launch_onb(1); /* 27/2 = 13.5 MHz. */
    }

    system_preinit();
    memory_init();
    /*
     * XXX: BSS is initialized here, do not use .bss before this line
     */
    bss_init();

    system_init();
    kernel_init();
    i2c_init();
    power_init();

    enable_irq();

#ifdef HAVE_SERIAL
    serial_setup();
#endif

    button_init();
    if (rc == 0) {
        /* User button selection timeout */
        while (USEC_TIMER < 400000);
        int btn = button_read_device();
        /* This prevents HDD spin-up when the user enters DFU */
        if (btn == (BUTTON_SELECT|BUTTON_MENU)) {
            while (button_read_device() == (BUTTON_SELECT|BUTTON_MENU))
                sleep(HZ/10);
            sleep(HZ);
            btn = button_read_device();
        }
        /* Enter OF, diagmode and diskmode using ONB */
        if ((btn == BUTTON_MENU)
                || (btn == (BUTTON_SELECT|BUTTON_LEFT))
                || (btn == (BUTTON_SELECT|BUTTON_PLAY))) {
            rc = kernel_launch_onb();
        }
    }

    lcd_init();
    lcd_set_foreground(LCD_WHITE);
    lcd_set_background(LCD_BLACK);
    lcd_clear_display();
    font_init();
    lcd_setfont(FONT_SYSFIXED);

    // TODO: see if removing this causes the nano3g LCD to initialize properly
#ifdef S5L87XX_DEVELOPMENT_BOOTLOADER
    sleep(HZ);
    for (int i = 0; i < lcd_type+1; i++) {
        sleep(HZ/2);
        piezo_seq(alivelcd);
    }
#endif

    lcd_update();
    sleep(HZ/40);  /* wait for lcd update */

    verbose = true;

    printf("Rockbox boot loader");
    printf("Version: %s", rbversion);

    backlight_init(); /* Turns on the backlight */

#ifdef NANO3G_ERASE_ALL
    /* Before storage init, which would try to mount an FTL we are about to
     * destroy. Does not return unless it could not start. */
    erase_whole_nand();
#endif

#ifdef NANO3G_LAUNCH_FLSH
    /* Before any storage init: the whole point is to reach Apple's disk mode
     * on a device whose NAND this port has made unreadable to Apple. Falls
     * through to the normal boot if the image cannot be loaded, so a failure
     * here still leaves a usable bootloader on screen. */
    launch_flsh_image(NANO3G_LAUNCH_FLSH, NANO3G_LAUNCH_FLSH_LABEL);
#endif

#ifdef S5L87XX_DEVELOPMENT_BOOTLOADER
    line++;
    printf("lcd type: %d", lcd_type);
#ifdef S5L_LCD_WITH_READID
    extern unsigned char lcd_id[4];
    uint32_t* lcd_id_32 = (uint32_t *)lcd_id;
    printf("lcd id: 0x%x", *lcd_id_32);
#endif
#ifdef IPOD_NANO4G
    printf("boot cfg: 0x%x", pmu_read(0x7f));
#endif
    line++;
    printf("Press SELECT to continue");
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);

    devel_menu();
#endif /* S5L87XX_DEVELOPMENT_BOOTLOADER */

#if defined(NAND_CHECK) && defined(HAVE_BOOTLOADER_USB_MODE) \
    && !defined(S5L87XX_DEVELOPMENT_BOOTLOADER)
    nand_check();
#endif

#ifndef S5L87XX_DEVELOPMENT_BOOTLOADER
    if (rc == 0) {
#if (CONFIG_STORAGE & STORAGE_ATA)
        /* Wait until there is enought power to spin-up HDD */
        battery_trap();
#endif

        rc = storage_init();
#ifdef IPOD_NANO3G
        if (rc == NAND_ERR_UNSUPPORTED) {
            /* Rockbox only drives NAND chips proven on hardware. Say which
             * one this is and how to get it validated, then leave the unit
             * to Apple's firmware, which is untouched. */
            lcd_set_foreground(LCD_RBYELLOW);
            printf("NAND not supported yet");
            lcd_set_foreground(LCD_WHITE);
            printf("chip %08lx x %u", (unsigned long)nand_get_id(),
                   nand_get_bank_count());
            printf("Rockbox does not write to");
            printf("chips it has not been");
            printf("tested on.");
            printf("Starting Apple firmware...");
            sleep(8 * HZ);      /* long enough to read the message */
            rc = kernel_launch_onb();
            /* Only reached if the ONB could not be read from NOR */
            printf("Apple firmware failed: %d", rc);
            printf("Hold MENU+SELECT to reboot,");
            printf("then SELECT+PLAY for disk mode");
            while (1)
                sleep(HZ);
        }
#endif
        if (rc != 0) {
            printf("Storage error: %d", rc);
            fatal_error(ERR_STORAGE);
        }

        filesystem_init();

        /* We wait until HDD spins up to check for hold button */
        if (button_hold()) {
#ifdef SYSCFG_MAX_ENTRIES
            bool lba48 = false;
            struct SysCfg syscfg;
            const ssize_t result = syscfg_read(&syscfg);
            if (result != -1) {
                const size_t syscfg_num_entries = MIN(syscfg.header.num_entries, SYSCFG_MAX_ENTRIES);
                for (size_t i = 0; i < syscfg_num_entries; i++) {
                    const struct SysCfgEntry* entry = &syscfg.entries[i];
                    const uint32_t* data32 = (uint32_t *)entry->data;
                    if (entry->tag == SYSCFG_TAG_HWVR) {
                        lba48 = (data32[1] >= 0x130200);
                        break;
                    }
                }

                int btn = button_read_device();

                struct storage_info sinfo;
                storage_get_info(0, &sinfo);
                if (sinfo.num_sectors < (1 << 28) || lba48 || btn & BUTTON_LEFT) {
                    printf("Executing OF...");
#if (CONFIG_STORAGE & STORAGE_ATA)
                    ata_sleepnow();
#endif
                    rc = kernel_launch_onb();
                } else {
                    printf("OF does not support LBA48");
                    fatal_error(ERR_LBA28);
                }
            }
#else
            printf("Executing OF...");
#if (CONFIG_STORAGE & STORAGE_ATA)
            ata_sleepnow();
#endif
            rc = kernel_launch_onb();
#endif /* SYSCFG_MAX_ENTRIES */
        }
    }

    if (rc != 0) {
        printf("Load OF error: %d", rc);
        fatal_error(ERR_OF);
    }

#ifdef HAVE_BOOTLOADER_USB_MODE
    /* Enter USB mode if SELECT+RIGHT are pressed */
    if (button_read_device() == (BUTTON_SELECT|BUTTON_RIGHT)) {
#if defined(MAX_VIRT_SECTOR_SIZE) && defined(DEFAULT_VIRT_SECTOR_SIZE)
#ifdef HAVE_MULTIDRIVE
            for (int i = 0 ; i < NUM_DRIVES ; i++)
#endif
                disk_set_sector_multiplier(IF_MD(i,) DEFAULT_VIRT_SECTOR_SIZE/SECTOR_SIZE);
#endif
        usb_mode();
    }
#endif

    rc = disk_mount_all();
    if (rc <= 0) {
#ifdef STORAGE_GET_INFO
        struct storage_info sinfo;
        storage_get_info(0, &sinfo);
#ifdef MAX_PHYS_SECTOR_SIZE
        printf("id: '%s' s:%u*%u", sinfo.product, sinfo.sector_size, sinfo.phys_sector_mult);
#else
        printf("id: '%s' s:%u", sinfo.product, sinfo.sector_size);
#endif
#endif
        struct partinfo pinfo;
        printf("No partition found");
        for (int i = 0 ; i < NUM_VOLUMES ; i++) {
            disk_partinfo(i, &pinfo);
            if (pinfo.type)
                printf("P%d T%02x S%llx",
                       i, pinfo.type, (unsigned long long)pinfo.size);
        }
#if defined(IPOD_NANO3G) && defined(DEFAULT_VIRT_SECTOR_SIZE)
        /* Nothing mounted: show USB hosts Apple's 4096-byte sectors, which
         * its partition table counts in */
        disk_set_sector_multiplier(IF_MD(0,)
                                   DEFAULT_VIRT_SECTOR_SIZE / SECTOR_SIZE);
#endif
        fatal_error(ERR_RB);
    }

    printf("Loading Rockbox...");
    unsigned char *loadbuffer = (unsigned char *)DRAM_ORIG;
    rc = load_firmware(loadbuffer, BOOTFILE, MAX_LOADSIZE);

    if (rc <= EFILE_EMPTY) {
        printf("Error!");
        printf("Can't load " BOOTFILE ": ");
        printf(loader_strerror(rc));
        fatal_error(ERR_RB);
    }

    printf("Rockbox loaded.");

    /* If we get here, we have a new firmware image at 0x08000000, run it */
    disable_irq();

    int (*kernel_entry)(void) = (void*)loadbuffer;
    commit_discard_idcache();
    rc = kernel_entry();

    /* End stop - should not get here */
    enable_irq();
    printf("ERR: Failed to boot");
    while(1);
#endif
}
