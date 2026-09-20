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

#ifdef FTL_APPLE_COMPAT
/* Read-only Apple VFL/FTL mount diagnostic, provided by ftl-apple-nano3g.c.
 * Runs the full read-only mount (VFL context + FTL context + block map) and
 * reports what it recovered from Apple's own on-flash structures, so this
 * can be validated against a stock (unmodified Apple) unit. */
struct apple_vfl_mount_info
{
    unsigned int banks;
    unsigned int page_size;
    unsigned int pages_per_block;
    unsigned int blocks_per_bank;
    uint32_t     vfl_usn_max;
    uint16_t     ftlctrlblocks[3];
    uint16_t     spareused[NAND_MAX_BANKS];
    uint16_t     firstspare[NAND_MAX_BANKS];
    uint16_t     sparecount[NAND_MAX_BANKS];
    unsigned int user_blocks;
    uint8_t      devinfo_head[128];
    bool         devinfo_captured;
    bool         ftl_mounted;
};
int apple_ftl_vfl_mount_info(struct apple_vfl_mount_info *out);

struct apple_rawscan_info
{
    unsigned int scanned;
    unsigned int blank;
    unsigned int okpages;
    unsigned int corrected;
    unsigned int eccfailed;
    unsigned int hwerr;
    uint32_t     first_nonblank_page;
    int          first_nonblank_rc;
    uint8_t      first_nonblank_head[16];
    int          found_first;
};
void apple_ftl_rawscan(struct apple_rawscan_info *out);

struct apple_blockdump_info
{
    uint32_t block;
    unsigned int npages;
    uint8_t  type[16];
    int      rc[16];
    uint8_t  head[16][16];
};
void apple_ftl_blockdump(uint32_t block, struct apple_blockdump_info *out);

struct apple_blockgroup_info
{
    uint32_t block;
    unsigned int ngroups;
    uint8_t  type[16];
    uint8_t  head[16][2];
    int      rc[16];
};
void apple_ftl_blockgroup_probe(uint32_t block,
                                struct apple_blockgroup_info *out);

struct apple_vfl_locator_bank
{
    uint32_t reads;
    uint32_t readable;
    uint32_t strict_hits;
    uint32_t marker_hits;
    uint32_t both_hits;
    uint16_t first_block;
    uint8_t  first_page;
    uint8_t  first_half;
    uint8_t  first_type;
    uint8_t  first_field8;
    uint32_t first_spare_usn;
    uint32_t first_payload_usn;
    uint16_t ring[4];
    uint8_t  found;
};

struct apple_vfl_locator_info
{
    unsigned int banks;
    unsigned int page_size;
    struct apple_vfl_locator_bank bank[NAND_MAX_BANKS];
};
void apple_ftl_vfl_locator(struct apple_vfl_locator_info *out);

struct apple_read_probe_info
{
    int          mount_rc;
    bool         mounted;
    uint32_t     num_sectors;
    uint32_t     usersb;
    uint32_t     nsuperblocks;
    uint32_t     sbpages;
    uint16_t     map0;
    int          sector0_rc;
    uint8_t      sector0_head[16];
    uint8_t      sector0_tail[4];
    uint16_t     ftlctrl[3];
    uint32_t     ctrl_used[3];
    int          ctrl_last[3];
    uint32_t     newblk;
    int          ftl_clean;
    uint32_t     ctrl_bank[3];
    uint32_t     ctrl_page[3];
    int          ctrl_rc[3];
    uint8_t      ctrl_type[3];
    uint8_t      layout;
    uint8_t      planes;
    uint32_t     dbg_nsuperblocks;
    uint32_t     dbg_vflspares;
    uint32_t     dbg_unit_block;
    uint32_t     dbg_phys_block;
    uint16_t     dbg_remap[6];
    uint16_t     dbg_usedcount[4];
    uint16_t     dbg_ring0;
    uint32_t     scan_firstnz;
    uint8_t      scan_nzhead[16];
    uint8_t      scan_sigfound;
    uint32_t     scan_sigsector;
    uint8_t      scan_sighead[16];
    uint32_t     scan_nzcount;
    uint32_t     scan_nzoff;
    uint32_t     low_firstnz;
    uint8_t      low_head[16];
    uint32_t     apm_sector;
    uint16_t     apm_sig;
};
void apple_ftl_read_probe(struct apple_read_probe_info *out);

struct apple_typemap_info
{
    uint32_t start;
    unsigned int count;
    uint8_t  page0_type[32];
    uint8_t  interesting[32];
    uint8_t  interesting_type[32];
    uint8_t  interesting_page[32];
};
void apple_ftl_typemap(uint32_t start, struct apple_typemap_info *out);
#endif

static uint32_t bank_ids[NAND_MAX_BANKS][2];  /* [bank][0]=maker|device<<8 */
static unsigned int bank_count;
static const struct nand_geometry *primary_geo;  /* bank 0, if usable */

/* Full 8-byte raw READ ID captured per chip-enable, read-only, with a
 * per-bank stability flag. This is the definitive identification data for
 * an odd unit: it shows every chip-enable's complete ID and whether that
 * read was repeatable, which distinguishes a genuine part from a
 * partial/aliased/failing read (e.g. a bank that stably returns all-zero,
 * or one whose bytes vary between reads). Filled by probe_all_bank_ids().
 * Purely diagnostic -- it never feeds the FTL's write gating. */
static uint8_t  bank_rawid[NAND_MAX_BANKS][8];
static int      bank_rawid_present[NAND_MAX_BANKS];  /* a read succeeded */
static int      bank_rawid_stable[NAND_MAX_BANKS];   /* reads agreed */

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

/* Read every chip-enable's full 8-byte READ ID a few times, recording the
 * bytes and whether they were stable across reads. Read-only (reset +
 * READ ID only), safe on any chip regardless of validation status. This
 * is the authoritative per-bank identity dump used to diagnose odd units
 * where the single primary read is ambiguous. */
static void probe_all_bank_ids(void)
{
    unsigned int bank;
    int attempt, i;

    for (bank = 0; bank < NAND_MAX_BANKS; bank++)
    {
        uint8_t first[8] = { 0 };
        bank_rawid_present[bank] = 0;
        bank_rawid_stable[bank] = 1;
        memset(bank_rawid[bank], 0, 8);

        for (attempt = 0; attempt < 4; attempt++)
        {
            uint8_t tmp[8] = { 0 };
            if (nand_hw_reset(bank) != 0)
                continue;
            if (nand_hw_read_id(bank, tmp, sizeof(tmp)) != 0)
                continue;
            if (!bank_rawid_present[bank])
            {
                memcpy(first, tmp, sizeof(tmp));
                memcpy(bank_rawid[bank], tmp, sizeof(tmp));
                bank_rawid_present[bank] = 1;
                continue;
            }
            if (memcmp(tmp, first, sizeof(tmp)) != 0)
            {
                int cur_zero = 1, have_zero = 1;
                bank_rawid_stable[bank] = 0;
                for (i = 0; i < 8; i++)
                {
                    if (tmp[i]) cur_zero = 0;
                    if (bank_rawid[bank][i]) have_zero = 0;
                }
                if (have_zero && !cur_zero)
                    memcpy(bank_rawid[bank], tmp, sizeof(tmp));
            }
        }
    }
}

/* rc is nand_init()'s result: 0, a NAND_ERR_*, or NAND_ERR_FTL_BASE plus
 * ftl_init()'s result (see nand-target.h/ftl-target.h) */
void nand_check_init(int rc)
{
    unsigned int bank;

    /* Capture every chip-enable's full ID first (read-only), so the report
     * can show the authoritative per-bank identity regardless of how the
     * FTL scan classified the chip. */
    probe_all_bank_ids();

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

    /* Full 8-byte READ ID for bank 0 (the "primary" chip-enable), from the
     * multi-read stability probe. Reported even when it is all-zero: a
     * stable all-zero here means bank 0 genuinely does not answer READ ID
     * on this unit (a real, diagnostic finding), distinct from a chip that
     * simply isn't in the validated table. The per-bank "rawidN" lines
     * below give every chip-enable's full ID and stability, which is the
     * authoritative identification for an ambiguous/odd unit. */
    if (bank_rawid_present[0])
    {
        const uint8_t *r = bank_rawid[0];
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len,
                     "rawid %02x %02x %02x %02x %02x %02x %02x %02x\n"
                     "rawid_stable %d\n",
                     r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
                     bank_rawid_stable[0]);
    }
    for (bank = 0; bank < NAND_MAX_BANKS; bank++)
    {
        const uint8_t *r = bank_rawid[bank];
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len,
                     "rawid%u %02x %02x %02x %02x %02x %02x %02x %02x present %d stable %d\n",
                     bank, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
                     bank_rawid_present[bank], bank_rawid_stable[bank]);
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

#ifdef FTL_APPLE_COMPAT
    /* Apple-compatible pipeline: run the read-only Apple VFL+FTL mount and
     * report what it recovered from Apple's on-flash structures. This is the
     * Phase 1/2 hardware-validation surface -- point it at a STOCK unit. */
    {
#ifdef APPLE_READ_MOUNT_ONLY
        /* Full read-only mount + a real sector-0 read. Validates the whole
         * VFL+FTL read path end to end: if the volume mounts and logical
         * sector 0 comes back as a stock MBR (ending 55 AA), the port reads
         * Apple's format correctly. */
        struct apple_read_probe_info rp;
        unsigned int b;
#if 0 /* verbose temporary diagnostics; excluded from the compact probe */

        apple_ftl_read_probe(&rp);
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len,
                     "apple_mnt rc %d m %d\n"
                     "apple_sec n %lu usb %lu nsb %lu sbp %lu map0 %04x\n"
                     "apple_s0 rc %d\n",
                     rp.mount_rc, rp.mounted,
                     (unsigned long)rp.num_sectors, (unsigned long)rp.usersb,
                     (unsigned long)rp.nsuperblocks, (unsigned long)rp.sbpages,
                     rp.map0, rp.sector0_rc);
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len, "apple_s0h");
        for (b = 0; b < 16; b++)
            report_len += snprintf(report_text + report_len,
                         sizeof(report_text) - report_len,
                         " %02x", rp.sector0_head[b]);
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len,
                     "\napple_s0sig %02x %02x\n",
                     rp.sector0_tail[2], rp.sector0_tail[3]);
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len,
                     "apple_scan nz %lu cnt %lu sig %u@%lu\n",
                     (unsigned long)rp.scan_firstnz,
                     (unsigned long)rp.scan_nzcount, rp.scan_sigfound,
                     (unsigned long)rp.scan_sigsector);
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len, "apple_nzh @%lu",
                     (unsigned long)rp.scan_nzoff);
        {
            unsigned int b;
            for (b = 0; b < 16; b++)
                report_len += snprintf(report_text + report_len,
                             sizeof(report_text) - report_len,
                             " %02x", rp.scan_nzhead[b]);
        }
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len, "\n");
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len,
                     "apple_low nz %lu apm %04x@%lu\n",
                     (unsigned long)rp.low_firstnz, rp.apm_sig,
                     (unsigned long)rp.apm_sector);
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len, "apple_lowh");
        {
            unsigned int b;
            for (b = 0; b < 16; b++)
                report_len += snprintf(report_text + report_len,
                             sizeof(report_text) - report_len,
                             " %02x", rp.low_head[b]);
        }
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len, "\n");
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len,
                     "apple_lay lo %u pl %u\n", rp.layout, rp.planes);
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len,
                     "apple_rmp nsb %lu vs %lu ub %lu pb %lu\n",
                     (unsigned long)rp.dbg_nsuperblocks,
                     (unsigned long)rp.dbg_vflspares,
                     (unsigned long)rp.dbg_unit_block,
                     (unsigned long)rp.dbg_phys_block);
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len,
                     "apple_rm %04x %04x %04x %04x %04x %04x\n",
                     rp.dbg_remap[0], rp.dbg_remap[1], rp.dbg_remap[2],
                     rp.dbg_remap[3], rp.dbg_remap[4], rp.dbg_remap[5]);
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len,
                     "apple_uc %u %u %u %u ring0 %u\n",
                     rp.dbg_usedcount[0], rp.dbg_usedcount[1],
                     rp.dbg_usedcount[2], rp.dbg_usedcount[3], rp.dbg_ring0);
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len,
                     "apple_ftlc %u %u %u clean %d nb %lu\n",
                     rp.ftlctrl[0], rp.ftlctrl[1], rp.ftlctrl[2],
                     rp.ftl_clean, (unsigned long)rp.newblk);
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len,
                     "apple_ftlu %lu:%d %lu:%d %lu:%d\n",
                     (unsigned long)rp.ctrl_used[0], rp.ctrl_last[0],
                     (unsigned long)rp.ctrl_used[1], rp.ctrl_last[1],
                     (unsigned long)rp.ctrl_used[2], rp.ctrl_last[2]);
        for (b = 0; b < 3; b++)
            report_len += snprintf(report_text + report_len,
                         sizeof(report_text) - report_len,
                         "apple_ca%u sb%u bk%lu pg%lu rc%d t%02x\n",
                         b, rp.ftlctrl[b], (unsigned long)rp.ctrl_bank[b],
                         (unsigned long)rp.ctrl_page[b], rp.ctrl_rc[b],
                         rp.ctrl_type[b]);
#endif /* compact APPLE_READ_MOUNT_ONLY report excludes verbose diagnostics */
        apple_ftl_read_probe(&rp);
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len,
                     "apple_mnt %d %d\napple_sec %lu\n"
                     "apple_s0 %d\napple_scan %lu %lu %u %lu\n"
                     "apple_low %lu %04x %lu\n",
                     rp.mount_rc, rp.mounted,
                     (unsigned long)rp.num_sectors,
                     rp.sector0_rc,
                     (unsigned long)rp.scan_firstnz,
                     (unsigned long)rp.scan_nzcount,
                     rp.scan_sigfound,
                     (unsigned long)rp.scan_sigsector,
                     (unsigned long)rp.low_firstnz,
                     rp.apm_sig,
                     (unsigned long)rp.apm_sector);
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len, "apple_lowh");
        for (b = 0; b < 16; b++)
            report_len += snprintf(report_text + report_len,
                         sizeof(report_text) - report_len,
                         " %02x", rp.low_head[b]);
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len, "\n");
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len, "apple_sigh");
        for (b = 0; b < 16; b++)
            report_len += snprintf(report_text + report_len,
                         sizeof(report_text) - report_len,
                         " %02x", rp.scan_sighead[b]);
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len, "\n");
#elif defined(APPLE_VFL_LOCATOR_ONLY)
        /* High-information read-only locator: all banks, low blocks 1..199,
         * first eight pages, strict context checksums on both 2 KiB halves
         * of a 4 KiB physical page. Marker and checksum hits stay separate
         * so a subpage-spare mismatch is visible instead of becoming a false
         * "no context" result. */
        struct apple_vfl_locator_info li;
        unsigned int bank;

        apple_ftl_vfl_locator(&li);
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len,
                     "apple_loc b%u ps%u\n", li.banks, li.page_size);
        for (bank = 0; bank < li.banks; bank++)
        {
            const struct apple_vfl_locator_bank *r = &li.bank[bank];
            report_len += snprintf(report_text + report_len,
                         sizeof(report_text) - report_len,
                         "apple_b%u r%lu o%lu s%lu m%lu x%lu\n",
                         bank, (unsigned long)r->reads,
                         (unsigned long)r->readable,
                         (unsigned long)r->strict_hits,
                         (unsigned long)r->marker_hits,
                         (unsigned long)r->both_hits);
            if (r->found)
            {
                report_len += snprintf(report_text + report_len,
                             sizeof(report_text) - report_len,
                             "apple_h%u b%u p%u h%u %02x%02x\n",
                             bank, r->first_block, r->first_page,
                             r->first_half, r->first_field8, r->first_type);
                report_len += snprintf(report_text + report_len,
                             sizeof(report_text) - report_len,
                             "apple_u%u %08lx %08lx\n", bank,
                             (unsigned long)r->first_payload_usn,
                             (unsigned long)r->first_spare_usn);
                report_len += snprintf(report_text + report_len,
                             sizeof(report_text) - report_len,
                             "apple_r%u %04x %04x %04x %04x\n", bank,
                             r->ring[0], r->ring[1],
                             r->ring[2], r->ring[3]);
            }
            if (report_len >= (int)sizeof(report_text))
            {
                report_len = (int)sizeof(report_text) - 1;
                break;
            }
        }
#elif defined(APPLE_A5_HEAD_ONLY)
        /* 3G-focused system-block probe. The devinfo list names 4094 and
         * 4091 as high system blocks, and their page-0 spare type is A5.
         * The general typemap deliberately filters A5 out, so emit their
         * actual page-0 payload signatures here. Uses apple_ftl_blockdump(),
         * which reads through nand_hw_read_page() only; it never writes NAND.
         * This small report deliberately replaces the verbose mount/typemap
         * output so it fits inside the one-sector diagnostic report. */
        {
#ifdef APPLE_LOW_SYSTEM_ONLY
            /* The working 2G VFL mount searches low physical system blocks.
             * For the 3G, show page-0 OOB types for blocks 0..31 in compact
             * pairs: l00 is blocks 0/1, l02 is 2/3, etc. This is read-only;
             * a non-FF pair identifies the next block for a payload dump. */
            struct apple_typemap_info tm;
            unsigned int i;

            apple_ftl_typemap(0, &tm);
            for (i = 0; i < tm.count; i += 2)
            {
                uint8_t next_type = i + 1 < tm.count ? tm.page0_type[i + 1]
                                                       : 0xff;
                report_len += snprintf(report_text + report_len,
                             sizeof(report_text) - report_len,
                             "apple_l%02u %02x%02x\n", i,
                             tm.page0_type[i], next_type);
                if (report_len >= (int)sizeof(report_text))
                {
                    report_len = (int)sizeof(report_text) - 1;
                    break;
                }
            }
#elif defined(APPLE_A5_GENERATIONS_ONLY)
            /* Sample the first page of each eight-page generation in block
             * 4094. Compact output keeps all 16 generations within the
             * one-sector report: gNN, OOB type, then data bytes 0..1. */
            struct apple_blockgroup_info bg;
            unsigned int g;

            apple_ftl_blockgroup_probe(4094, &bg);
            for (g = 0; g < bg.ngroups; g++)
            {
                report_len += snprintf(report_text + report_len,
                             sizeof(report_text) - report_len,
                             "apple_g%02u %02x %02x%02x\n", g,
                             bg.type[g], bg.head[g][0], bg.head[g][1]);
                if (report_len >= (int)sizeof(report_text))
                {
                    report_len = (int)sizeof(report_text) - 1;
                    break;
                }
            }
#elif defined(APPLE_A5_PAGES_ONLY)
            /* 4091 is the only listed A5 block whose page-0 payload differs
             * from DEVICEINFO. Show its first eight page signatures, using a
             * deliberately compact format (one page / one LCD-width line).
             * This reuses the existing read-only helper and makes no writes. */
            struct apple_blockdump_info bd;
            unsigned int p, b;

            apple_ftl_blockdump(4091, &bd);
            report_len += snprintf(report_text + report_len,
                         sizeof(report_text) - report_len,
                         "apple_4091 pages %u\n", bd.npages);
            for (p = 0; p < bd.npages; p++)
            {
                report_len += snprintf(report_text + report_len,
                             sizeof(report_text) - report_len,
                             "apple_p%u %02x", p, bd.type[p]);
                /* Eight bytes keeps each line readable on the Nano LCD. */
                for (b = 0; b < 8; b++)
                    report_len += snprintf(report_text + report_len,
                                 sizeof(report_text) - report_len,
                                 " %02x", bd.head[p][b]);
                report_len += snprintf(report_text + report_len,
                             sizeof(report_text) - report_len, "\n");
                if (report_len >= (int)sizeof(report_text))
                {
                    report_len = (int)sizeof(report_text) - 1;
                    break;
                }
            }
#else
            static const uint32_t probe_blocks[] = { 4094, 4091 };
            struct apple_blockdump_info bd;
            unsigned int i, b;

            for (i = 0; i < sizeof(probe_blocks) / sizeof(probe_blocks[0]); i++)
            {
                apple_ftl_blockdump(probe_blocks[i], &bd);
                report_len += snprintf(report_text + report_len,
                             sizeof(report_text) - report_len,
                             "apple_a5 blk %lu p0 t %02x rc %d d",
                             (unsigned long)bd.block, bd.type[0], bd.rc[0]);
                for (b = 0; b < 16; b++)
                    report_len += snprintf(report_text + report_len,
                                 sizeof(report_text) - report_len,
                                 " %02x", bd.head[0][b]);
                report_len += snprintf(report_text + report_len,
                             sizeof(report_text) - report_len, "\n");
                if (report_len >= (int)sizeof(report_text))
                {
                    report_len = (int)sizeof(report_text) - 1;
                    break;
                }
            }
#endif
        }
#else
        struct apple_vfl_mount_info mi;
        int amrc = apple_ftl_vfl_mount_info(&mi);
        report_len += snprintf(report_text + report_len,
                     sizeof(report_text) - report_len,
                     "apple_mount %s\n"
                     "apple_ftl_mounted %d\n"
                     "apple_banks %u\n"
                     "apple_pagesize %u\n"
                     "apple_ppblock %u\n"
                     "apple_blocks %u\n"
                     "apple_userblocks_provisional %u\n"
                     "apple_vflusn %lu\n"
                     "apple_ftlctrl %u %u %u\n"
                     "apple_spare0 used %u first %u count %u\n"
                     "apple_devinfo_captured %d\n",
                     amrc == 0 ? "mounted" : "FAILED",
                     mi.ftl_mounted,
                     mi.banks, mi.page_size, mi.pages_per_block,
                     mi.blocks_per_bank, mi.user_blocks,
                     (unsigned long)mi.vfl_usn_max,
                     mi.ftlctrlblocks[0], mi.ftlctrlblocks[1],
                     mi.ftlctrlblocks[2],
                     mi.spareused[0], mi.firstspare[0], mi.sparecount[0],
                     mi.devinfo_captured);
        if (report_len >= (int)sizeof(report_text))
            report_len = (int)sizeof(report_text) - 1;

#ifndef APPLE_TYPEMAP_ONLY
        /* Raw head of the devinfo page as hex -- lets us locate the real
         * userblocks/syshyperblocks fields we still need to decode.
         * (Suppressed in APPLE_TYPEMAP_ONLY builds: the sector-0 report is
         * only SECTOR_SIZE bytes and these verbose dumps push the typemap
         * output past the end where it gets truncated.) */
        if (mi.devinfo_captured)
        {
            /* Emit the 128 devinfo bytes as 8 short lines of 16 bytes each,
             * so each line fits on the LCD and is photographable (the single
             * long line was truncated at the screen edge, and the sector-0
             * USB read does not reliably enumerate on this controller). */
            int row, b;
            for (row = 0; row < 8; row++)
            {
                report_len += snprintf(report_text + report_len,
                             sizeof(report_text) - report_len,
                             "apple_di%d", row);
                for (b = 0; b < 16; b++)
                    report_len += snprintf(report_text + report_len,
                                 sizeof(report_text) - report_len,
                                 " %02x", mi.devinfo_head[row * 16 + b]);
                report_len += snprintf(report_text + report_len,
                             sizeof(report_text) - report_len, "\n");
            }
            if (report_len >= (int)sizeof(report_text))
                report_len = (int)sizeof(report_text) - 1;
        }

        /* Ground-truth raw scan of the devinfo search region: did we read
         * anything at all, and what does the first non-blank page contain?
         * This tells us whether the mount failure is "can't read Apple data"
         * vs "magic isn't where we looked". */
        {
            struct apple_rawscan_info rs;
            int b;
            apple_ftl_rawscan(&rs);
            report_len += snprintf(report_text + report_len,
                         sizeof(report_text) - report_len,
                         "apple_scan pages %u blank %u ok %u corr %u "
                         "eccfail %u hwerr %u\n"
                         "apple_scan_first page %lu rc %d\n",
                         rs.scanned, rs.blank, rs.okpages, rs.corrected,
                         rs.eccfailed, rs.hwerr,
                         (unsigned long)rs.first_nonblank_page,
                         rs.first_nonblank_rc);
            if (rs.found_first)
            {
                report_len += snprintf(report_text + report_len,
                             sizeof(report_text) - report_len,
                             "apple_scan_head");
                for (b = 0; b < 16; b++)
                    report_len += snprintf(report_text + report_len,
                                 sizeof(report_text) - report_len,
                                 " %02x", rs.first_nonblank_head[b]);
                report_len += snprintf(report_text + report_len,
                             sizeof(report_text) - report_len, "\n");
            }
            if (report_len >= (int)sizeof(report_text))
                report_len = (int)sizeof(report_text) - 1;
        }
#endif /* !APPLE_TYPEMAP_ONLY */

        /* Type map: block 4095 is all devinfo (A5); the VFL context (2G type
         * 0x80) was not found there. Map page-0 spare types across the top of
         * the device to locate where the 3G keeps its VFL/FTL metadata.
         * Only "interesting" (non-FF/non-A5) blocks are printed to keep the
         * report small. */
        {
            static const uint32_t tm_starts[] = { 4064, 4032, 4000, 3968 };
            struct apple_typemap_info tm;
            unsigned int s, i;
            for (s = 0; s < sizeof(tm_starts)/sizeof(tm_starts[0]); s++)
            {
                apple_ftl_typemap(tm_starts[s], &tm);
                report_len += snprintf(report_text + report_len,
                             sizeof(report_text) - report_len,
                             "apple_tm start %lu count %u\n",
                             (unsigned long)tm.start, tm.count);
                for (i = 0; i < tm.count; i++)
                {
                    if (tm.interesting[i])
                        report_len += snprintf(report_text + report_len,
                                     sizeof(report_text) - report_len,
                                     "apple_tm blk %lu p0 %02x int %02x@%u\n",
                                     (unsigned long)(tm.start + i),
                                     tm.page0_type[i],
                                     tm.interesting_type[i],
                                     tm.interesting_page[i]);
                }
                if (report_len >= (int)sizeof(report_text))
                {
                    report_len = (int)sizeof(report_text) - 1;
                    break;
                }
            }
        }
#endif /* Apple diagnostic build variant */
    }
#endif

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
