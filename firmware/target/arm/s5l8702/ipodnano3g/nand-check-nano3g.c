/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 by Andrew Rice
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

/*
 * The contributor check image (a bootloader built with -DNAND_CHECK, run
 * from DFU; utils/ipodnano3g/nandcheck/build.sh builds it). It identifies
 * whatever chip is fitted, reports how the FTL mount went (or would have
 * gone), and serves a raw, read-only view of the NAND over USB, in
 * SECTOR_SIZE sectors:
 *
 *   0                              a text report, NUL padded
 *   1 + block * banks + bank       that block's per-page spare bytes and
 *                                  read result, one NAND_CHECK_RECORD-byte
 *                                  entry per page (see nand_check_record)
 *   metaend + page * secperpage    page data, bank by bank
 *
 * Nothing here writes to the NAND. This file is built only in a
 * NAND_CHECK build, where it provides the storage API that the driver
 * (nand-nano3g.c) leaves out in that configuration: the disk served is
 * the raw NAND, not the FTL's, so an owner of an unrecognised chip can
 * supply what getting it supported takes without risking their data.
 */

#include "config.h"
#include "system.h"
#include "storage.h"
#include "nand-target.h"
#include "ftl-target.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "version.h"

/* One page's worth of diagnostic record: up to the first 12 spare/OOB
 * bytes (however many the chip actually has, zero padded), followed by
 * the read result as a 4-byte signed word (a nand_ecc_result value, or
 * a negative NAND_HWERR_* on a hardware timeout). */
#define NAND_CHECK_SPARE_BYTES  12
#define NAND_CHECK_RECORD       (NAND_CHECK_SPARE_BYTES + 4)
#define NAND_CHECK_PPB          (SECTOR_SIZE / NAND_CHECK_RECORD)

static char report_text[SECTOR_SIZE];
static int report_len;

static uint32_t bank_ids[NAND_MAX_BANKS][2];  /* [bank][0]=maker|device<<8 */
static unsigned int bank_count;
static const struct nand_geometry *primary_geo;  /* bank 0, if usable */

static uint8_t row_spares[NAND_MAX_BANKS][SECTOR_SIZE] NAND_DMA_BUF_ATTR;
static uint32_t row_block = 0xffffffff;

static uint8_t page_buf[NAND_MAX_PAGE_SIZE] NAND_DMA_BUF_ATTR;
static uint32_t page_cached = 0xffffffff;
static uint32_t page_cached_bank = 0xffffffff;

/* Set when primary_geo/bank_count below describe an identified-but-not-
 * "usable" chip (unrecognised -- e.g. MLC, or any other undecoded/
 * unvalidated part) rather than one nand_scan_banks() counted normally.
 * Only bank 0 is ever dumped in that case: nand_scan_banks() never ran
 * its multi-bank consistency check for such a chip, so there's no basis
 * for trusting any other chip-enable's identity here. Purely
 * diagnostic -- this never feeds ftl_init()/the FTL's write gating,
 * which still keys off nand_get_bank_geometry()/->recognized exactly as
 * before. */
static bool diagnostic_only;

/* How many chip-enables answered when diagnostic_only probed banks 1-3
 * directly (see nand_check_init()); 1 if only bank 0 answered (or this
 * isn't a diagnostic_only case at all). Purely a report value -- see
 * that probe's own comment for why it's kept separate from bank_count,
 * which still drives the raw-dump layout below. */
static unsigned int diagbanks_seen = 1;

/* rc is nand_init()'s result: 0, a NAND_ERR_*, or NAND_ERR_FTL_BASE plus
 * ftl_init()'s result (see nand-target.h/ftl-target.h) */
void nand_check_init(int rc)
{
    unsigned int bank;

    bank_count = nand_get_bank_count();
    primary_geo = nand_get_bank_geometry(0);
    diagnostic_only = false;

    if (!primary_geo)
    {
        /* No bank was "usable", but bank 0 may still have answered and
         * decoded a plausible (if unrecognised) geometry -- read it back
         * for diagnostics only. nand_check_probe_bank_capacity() issues
         * only nand_hw_read_page() calls (no writes/erases), so this is
         * safe to run against any chip regardless of validation status. */
        const struct nand_geometry *g = nand_check_bank_geometry(0);

        if (g && g->page_size != 0)
        {
            unsigned int bank;

            nand_check_probe_bank_capacity(0);
            primary_geo = g;
            bank_count = 1;
            diagnostic_only = true;

            /* nand_scan_banks() stops at bank 0 the moment it's
             * unrecognised (see its own comment), so it never checked
             * whether further chip-enables answer too -- a multi-die
             * package (e.g. an 8GB unit built from several 2GiB dies,
             * one chip-enable each) would look exactly like this from
             * bank 0 alone. Diagnostic-only, read-only: nand_hw_reset()/
             * nand_hw_read_id() don't gate on ->recognized, so probing
             * banks 1-3 directly here doesn't touch anything the write
             * path depends on.
             *
             * Deliberately NOT folded into bank_count: that variable
             * also drives this file's raw-NAND-dump sector layout
             * (metaend()/total_sector_count()/load_row()), which
             * expects every counted bank to have real decoded geometry
             * (via nand_vendor_decode(), which this diagnostic probe
             * does not call for banks 1-3) -- bumping it here would
             * silently corrupt that dump's layout instead of actually
             * serving useful data for those banks. This is reported
             * (diagbanks_seen/diagbank_ids below) for identification
             * only; extending the dump itself to cover other banks is
             * a separate, not-yet-done piece of work. */
            for (bank = 1; bank < NAND_MAX_BANKS; bank++)
            {
                uint8_t id[8];

                if (nand_hw_reset(bank) != 0
                    || nand_hw_read_id(bank, id, sizeof(id)) != 0)
                    break;
                bank_ids[bank][1] = ((uint32_t)id[0]) | ((uint32_t)id[1] << 8)
                                   | ((uint32_t)id[2] << 16)
                                   | ((uint32_t)id[3] << 24);
                /* Record real decoded geometry for this bank too (not
                 * just its raw ID), so a later diagnostic that looks at
                 * more than one bank (e.g. nand_check_write_test_sweep())
                 * has something real to work with instead of every
                 * bank past 0 reading as page_size == 0. Still purely
                 * diagnostic -- see that function's own comment. */
                nand_check_decode_bank_geometry(bank, id, sizeof(id));
                diagbanks_seen = bank + 1;
            }
        }
    }

    for (bank = 0; bank < NAND_MAX_BANKS; bank++)
    {
        const struct nand_geometry *g = nand_check_bank_geometry(bank);
        bank_ids[bank][0] = g ? ((uint32_t)g->maker_id
                                | ((uint32_t)g->device_id << 8)) : 0;
    }

    report_len = snprintf(report_text, sizeof(report_text),
                 "nano3g-nandcheck 1\n"
                 "version %s\n"
                 "banks %u\n"
                 "ids %08lx %08lx %08lx %08lx\n"
                 "nand %d\n",
                 rbversion, bank_count,
                 (unsigned long)bank_ids[0][0], (unsigned long)bank_ids[1][0],
                 (unsigned long)bank_ids[2][0], (unsigned long)bank_ids[3][0],
                 rc);

    /* Full raw READ ID bytes for bank 0, beyond the 2-byte maker|device
     * pair above: a pure hardware read (no state change), so it's safe
     * to re-issue here regardless of how nand_scan_banks() classified
     * the chip. This is what a "not recognised" verdict can't show on
     * its own -- which extended-ID byte(s) our generic decode rejected,
     * and whether that rejection is even correct for this specific part. */
    {
        uint8_t raw_id[8] = { 0 };
        if (nand_hw_read_id(0, raw_id, sizeof(raw_id)) == 0)
        {
            report_len += snprintf(report_text + report_len,
                         sizeof(report_text) - report_len,
                         "rawid %02x %02x %02x %02x %02x %02x %02x %02x\n",
                         raw_id[0], raw_id[1], raw_id[2], raw_id[3],
                         raw_id[4], raw_id[5], raw_id[6], raw_id[7]);
        }
    }

    if (!primary_geo)
    {
        report_len += snprintf(report_text + report_len,
                                sizeof(report_text) - report_len, "row none\n");
    }
    else
    {
        int ftl_rc = (rc <= NAND_ERR_FTL_BASE) ? rc - NAND_ERR_FTL_BASE
                   : rc ? 1 : 0;

        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len,
                     "pagesize %u\n"
                     "sparesize %u\n"
                     "ppb %u\n"
                     "blocks %u\n"
                     "bitspercell %u\n"
                     "buswidth16 %d\n"
                     "recognized %d\n"
                     "diagonly %d\n"
                     "ftl %d\n"
                     "sectors %lu\n",
                     primary_geo->page_size, primary_geo->spare_size,
                     primary_geo->pages_per_block,
                     primary_geo->blocks_per_bank,
                     primary_geo->bits_per_cell, primary_geo->bus_width_16,
                     primary_geo->recognized, diagnostic_only, ftl_rc,
                     (unsigned long)ftl_num_sectors());

        /* Why the capacity probe's doubling phase stopped where it did --
         * distinguishes "genuinely ran out of chip"/"hit a real hardware
         * timeout" from a merely-bad block (which doesn't stop the probe
         * at all; see probe_bank_capacity_blocks()'s comment) for an
         * unrecognised chip's reported block count. Placed after (not
         * before, as an earlier version of this code mistakenly had it)
         * the report_len = snprintf(report_text, ...) call above, which
         * resets report_len and overwrites report_text from byte 0 --
         * anything appended before that call is silently discarded. */
        if (diagnostic_only)
        {
            const struct nand_probe_trace *t = nand_check_last_probe_trace();
            report_len += snprintf(report_text + report_len,
                         sizeof(report_text) - report_len,
                         "probestop %d block %lu page %lu rc %d\n",
                         t->stopped, (unsigned long)t->stop_block,
                         (unsigned long)t->stop_page, t->stop_rc);
            /* probestop 0 means the doubling phase ran to its cap
             * without any read ever failing -- on real hardware
             * against a chip this probe can't measure (confirmed: an
             * MLC part whose out-of-range reads alias back onto valid
             * pages instead of erroring), that means the `blocks`
             * figure above is not a real capacity, just "at least this
             * many" -- see probe_bank_capacity_blocks()'s comment. */
            if (!t->stopped)
                report_len += snprintf(report_text + report_len,
                             sizeof(report_text) - report_len,
                             "blocks_unreliable 1\n");

            /* Whether further chip-enables answered too (see the bank
             * 1-3 probe loop above): a multi-die package's other dies
             * would show up here as additional non-zero IDs. Printed
             * after the report_len reset above, same rule as
             * probestop's line -- anything appended before that reset
             * is silently discarded. */
            report_len += snprintf(report_text + report_len,
                         sizeof(report_text) - report_len,
                         "diagbanks %u ids %08lx %08lx %08lx %08lx\n",
                         diagbanks_seen, (unsigned long)bank_ids[0][1],
                         (unsigned long)bank_ids[1][1],
                         (unsigned long)bank_ids[2][1],
                         (unsigned long)bank_ids[3][1]);
        }
    }

    report_len += snprintf(report_text + report_len,
             sizeof(report_text) - report_len,
             "verdict %s\n",
             rc == NAND_ERR_NO_CHIP ? "NAND did not answer"
             : diagnostic_only ? "chip identified but not recognised; "
                                 "read-only diagnostic dump below"
             : rc == NAND_ERR_UNSUPPORTED ? "chip identified but not recognised"
             : !primary_geo ? "no usable bank"
             : rc == 0 ? "OK, mounted"
             : rc <= NAND_ERR_FTL_BASE ? "chip recognised, FTL mount failed"
             : "unexpected error");

    if (report_len >= (int)sizeof(report_text))
        report_len = (int)sizeof(report_text) - 1;
}

const char *nand_check_report(void)
{
    return report_text;
}

void nand_check_note(const char *line)
{
    report_len += snprintf(report_text + report_len,
                           sizeof(report_text) - report_len, "%s\n", line);
    if (report_len >= (int)sizeof(report_text))
        report_len = (int)sizeof(report_text) - 1;
}

/* The first sector of page data: the report, then every block's spare
 * record row (one row per block, one bank's worth of records per sector
 * within that row). */
static uint32_t metaend(void)
{
    if (!primary_geo)
        return 1;
    return 1 + primary_geo->blocks_per_bank * bank_count;
}

static uint64_t total_sector_count(void)
{
    if (!primary_geo)
        return 1;

    return metaend()
         + (uint64_t)bank_count * primary_geo->blocks_per_bank
           * primary_geo->pages_per_block
           * (primary_geo->page_size / SECTOR_SIZE);
}

/* Reads every page of one block, on every bank, filling row_spares[][].
 * Pages are read one at a time (this driver has no multi-bank parallel
 * read primitive); that trades check-image throughput for staying within
 * the low-level driver's actual, published API surface. */
static void load_row(uint32_t block)
{
    unsigned int bank;
    uint32_t page, ppb = primary_geo->pages_per_block;

    for (bank = 0; bank < bank_count; bank++)
    {
        for (page = 0; page < ppb && page * NAND_CHECK_RECORD < SECTOR_SIZE;
             page++)
        {
            uint8_t spare[NAND_MAX_SPARE_SIZE] NAND_DMA_BUF_ATTR;
            uint8_t *rec = row_spares[bank] + page * NAND_CHECK_RECORD;
            int32_t result = nand_hw_read_page(bank, block * ppb + page,
                                               NULL, spare);
            unsigned int copy = primary_geo->spare_size < NAND_CHECK_SPARE_BYTES
                               ? primary_geo->spare_size
                               : NAND_CHECK_SPARE_BYTES;

            memset(rec, 0, NAND_CHECK_RECORD);
            memcpy(rec, spare, copy);
            memcpy(rec + NAND_CHECK_SPARE_BYTES, &result, 4);
        }
    }
    row_block = block;
    page_cached = 0xffffffff;
}

static void fill_sector(uint32_t sector, uint8_t *dst)
{
    uint32_t end, spp, per_bank_pages, global_page, bank, page_in_bank;

    if (sector == 0)
    {
        memcpy(dst, report_text, SECTOR_SIZE);
        return;
    }
    if (!primary_geo)
    {
        memset(dst, 0, SECTOR_SIZE);
        return;
    }

    end = metaend();
    spp = primary_geo->page_size / SECTOR_SIZE;
    per_bank_pages = primary_geo->blocks_per_bank * primary_geo->pages_per_block;

    if (sector < end)
    {
        uint32_t block = (sector - 1) / bank_count;
        uint32_t bank = (sector - 1) % bank_count;

        if (block != row_block)
            load_row(block);
        memcpy(dst, row_spares[bank], SECTOR_SIZE);
        return;
    }

    global_page = (sector - end) / spp;
    bank = global_page / per_bank_pages;
    page_in_bank = global_page % per_bank_pages;

    if (global_page != page_cached || bank != page_cached_bank)
    {
        nand_hw_read_page(bank, page_in_bank, page_buf, NULL);
        page_cached = global_page;
        page_cached_bank = bank;
        row_block = 0xffffffff;  /* our page read may have disturbed FIFO state */
    }
    memcpy(dst, page_buf + (sector - end) % spp * SECTOR_SIZE, SECTOR_SIZE);
}

/* ---- Rockbox storage API: the check's read-only raw NAND ---- */

int nand_read_sectors(IF_MD(int drive,) sector_t start, int incount,
                      void *inbuf)
{
    IF_MD((void)drive);
    uint8_t *buf = inbuf;

    if ((uint64_t)start + incount > total_sector_count())
        return -1;
    while (incount--)
    {
        fill_sector((uint32_t)start++, buf);
        buf += SECTOR_SIZE;
    }
    nand_spin();
    return 0;
}

int nand_write_sectors(IF_MD(int drive,) sector_t start, int count,
                       const void *outbuf)
{
    IF_MD((void)drive);
    (void)start;
    (void)count;
    (void)outbuf;
    return -1;
}

#ifdef HAVE_STORAGE_READONLY
bool nand_readonly(IF_MD_NONVOID(int drive))
{
    IF_MD((void)drive);
    return true;
}
#endif

int nand_event(long id, intptr_t data)
{
    (void)id;
    (void)data;
    return 0;
}

#ifdef STORAGE_GET_INFO
void nand_get_info(IF_MD(int drive,) struct storage_info *info)
{
    IF_MD((void)drive);
    info->sector_size = SECTOR_SIZE;
    info->num_sectors = total_sector_count();
    info->vendor = "Rockbox";
    info->product = "Nano 3G NAND";
    info->revision = "chk1";
}
#endif
