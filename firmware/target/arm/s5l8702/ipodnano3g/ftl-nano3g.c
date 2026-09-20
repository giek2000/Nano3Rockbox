/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") flash translation layer -- implementation.
 *
 * Original design and implementation; see ftl-target.h for the on-flash
 * layout rationale and how it differs deliberately from (rather than
 * attempts to reproduce) Apple's proprietary Whimory FTL/VFL.
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

#include <string.h>
#include "config.h"
#include "nand-target.h"
#include "nand_vendor.h"
#include "ftl-target.h"

/* Every page/spare buffer handed to nand_hw_read_page()/nand_hw_write_page()
 * must carry this.
 *
 * Two independent reasons, both confirmed against real hardware:
 *
 *  1. The FMC DMAs straight into/out of the buffer and ignores the low bits
 *     of the address. A buffer at an address 1-3 bytes off a word boundary
 *     therefore transfers *shifted by that offset* while still reporting
 *     success. This was a real bug: a plain `static uint8_t[]` has no
 *     alignment guarantee, and the linker happened to place the FTL's page
 *     buffer on an odd address in the main-firmware build but an aligned one
 *     in the bootloader build. The result was that the bootloader mounted the
 *     filesystem and loaded the firmware from it perfectly, while that same
 *     firmware read every sector displaced by one byte -- so the FAT boot
 *     sector's 0xAA55 signature appeared as 0x00AA and it reported "No
 *     partition found" on a filesystem that was completely intact.
 *
 *  2. The driver performs cache maintenance over these buffers
 *     (commit_dcache_range()/DISCARD_DCACHE_RANGE()), which operates on whole
 *     cache lines; a buffer that doesn't start on a cache-line boundary can
 *     have adjacent data flushed or invalidated along with it.
 *
 * 32 bytes satisfies both (ARM926 cache line). Defined in nand-target.h as
 * NAND_DMA_BUF_ATTR, next to the hardware facts that motivate it; aliased
 * here for brevity at the many declarations below. */
#define FTL_DMA_BUF_ATTR  NAND_DMA_BUF_ATTR

/* Upper bound on blocks-per-bank we size static tables for. 8Gb SLC parts
 * (the largest density plausible in a 2007/08 4-8GB flash player) top out
 * at a few thousand 128-256KiB blocks per die; 8192 leaves comfortable
 * headroom without resorting to runtime allocation, which keeps this
 * usable from bootloader context where a heap may not be set up. */
#define FTL_MAX_BLOCKS_PER_BANK  8192
#define FTL_MAX_PHYSICAL_BLOCKS  (NAND_MAX_BANKS * FTL_MAX_BLOCKS_PER_BANK)

/* Fraction of total blocks held back as spare/free capacity for wear
 * leveling headroom and runtime-discovered bad blocks. Our own choice,
 * not extracted from any reference implementation; 1/32 (~3%) plus a
 * fixed minimum is a common, easily-justified rule of thumb for
 * log-structured flash stores. */
#define FTL_SPARE_FRACTION_SHIFT 5
#define FTL_SPARE_MIN_BLOCKS     4

/* Two magic values, not a magic-plus-flags-bit scheme: this doubles as
 * the "is this block bad" signal (mark_bad_best_effort() always stamps
 * FTL_HEADER_MAGIC_BAD; every other write always uses FTL_HEADER_MAGIC),
 * so a separate flags field would carry no information the magic value
 * doesn't already carry. That redundancy is what was dropped to fit this
 * header in NAND_SPARE_META_BYTES (see below) -- not a reduction in what
 * the on-flash format can express. Narrowed to 16 bits (from the
 * original 32) as part of that same size reduction; 'FT'/'FB' collide
 * with real content far less often than blank (0xFFFF) or zeroed
 * (0x0000) flash would, which is all this needs to guard against. */
#define FTL_HEADER_MAGIC        ((uint16_t)0x4654u) /* 'FT' */
#define FTL_HEADER_MAGIC_BAD    ((uint16_t)0x4642u) /* 'FB' */

/* On-flash per-page header, stored in the leading bytes of each page's
 * spare/OOB area. Sized and packed to fit within NAND_SPARE_META_BYTES
 * (nand-target.h) -- the real, confirmed-on-hardware limit on how many
 * bytes of per-page metadata this controller's transfer mechanism
 * actually round-trips (see that macro's comment), NOT geo->spare_size,
 * which is a chip's *total* OOB/spare area and is mostly consumed by the
 * controller's own ECC parity rather than available to software.
 *
 * An earlier version of this header was 20 bytes (uint32_t magic/
 * logical_block/generation/erase_count plus uint16_t page_index/flags);
 * real hardware testing of the low-level driver revealed the 12-byte
 * hardware limit only after this header had already been designed and
 * tested purely in simulation, so it had to be redesigned to fit once
 * that was known:
 *   - magic narrowed to uint16_t (see above).
 *   - erase_count is dropped from the on-flash format entirely. It was
 *     only ever read into the in-RAM erase_count[] array for wear-
 *     leveling statistics (see scan_and_mount()); no code path uses a
 *     block's on-flash erase_count for correctness (which physical
 *     block "wins" for a logical block is decided purely by magic/
 *     logical_block/generation/page_index). Losing it just means wear
 *     tracking restarts from 0 for every already-written block after a
 *     remount, which biases the least-erased-first picker no worse than
 *     a freshly-flashed device already does -- it does not affect
 *     correctness or crash-safety.
 *   - flags is dropped as redundant with the two-magic-value scheme
 *     (see above): FTL_FLAG_BAD is retired along with it.
 *   - logical_block narrowed to uint16_t: FTL_MAX_PHYSICAL_BLOCKS above
 *     (32768) fits comfortably under 65536, and ftl_init() below now
 *     explicitly refuses to mount (FTL_ERR_TOO_SMALL... see its own
 *     bound check) rather than silently truncate if a future chip ever
 *     pushed num_logical_blocks past that.
 *   - page_index narrowed to uint8_t (0-255 pages per block): this
 *     chip's own pages_per_block is 128. FTL_MAX_PAGES_PER_BLOCK below
 *     is checked explicitly in ftl_init(), so a chip with more pages
 *     per block than this format can address fails loudly at mount
 *     time instead of silently aliasing two page indices onto the same
 *     on-flash byte value. */
#define FTL_MAX_PAGES_PER_BLOCK  256

struct ftl_page_header
{
    uint16_t magic;            /* FTL_HEADER_MAGIC or _MAGIC_BAD */
    uint16_t logical_block;
    uint32_t generation;
    uint8_t  page_index;
} __attribute__((packed));

/* Confirmed to fit within the hardware's real per-page metadata budget
 * at compile time, rather than relying on every future edit to this
 * struct remembering to check by hand. */
_Static_assert(sizeof(struct ftl_page_header) <= NAND_SPARE_META_BYTES,
              "struct ftl_page_header must fit in NAND_SPARE_META_BYTES "
              "-- this controller's transfer mechanism cannot round-trip "
              "more spare bytes than that per page, regardless of a "
              "chip's total spare_size (see nand-target.h)");

/* Physical block identifier: bank * blocks_per_bank + block, using bank
 * 0's geometry as the reference (all banks are required to match bank 0's
 * ID during nand_scan_banks(), so their geometry -- if all recognised --
 * is identical; we don't currently support mixed-geometry banks). */
static const struct nand_geometry *chip_geo;
static unsigned int bank_count;
static unsigned int blocks_per_bank;
static unsigned int sectors_per_page;      /* 1, or 2 for 4KiB-page chips */
static unsigned int sectors_per_block;
static unsigned int total_physical_blocks;
static unsigned int num_logical_blocks;
static unsigned int spare_pool_target;

/* RAM state, sized against the static upper bound above. */
static uint32_t block_map[FTL_MAX_PHYSICAL_BLOCKS];   /* logical -> phys, or
                                                          FTL_UNMAPPED */
static uint32_t block_generation[FTL_MAX_PHYSICAL_BLOCKS]; /* by logical idx */
static uint32_t erase_count[FTL_MAX_PHYSICAL_BLOCKS]; /* by physical idx */
static bool     block_bad[FTL_MAX_PHYSICAL_BLOCKS];   /* by physical idx */
static uint32_t free_list[FTL_MAX_PHYSICAL_BLOCKS];
static unsigned int free_count;

#define FTL_UNMAPPED  0xFFFFFFFFu

static bool mounted;
static bool readonly_mount;
static bool write_error_latched;

/* Write-back cache reset; defined with the cache below but used by
 * ftl_init() above it. */
static void wb_reset(void);

static void phys_to_bank_block(uint32_t phys, unsigned int *bank,
                               unsigned int *block)
{
    *bank  = phys / blocks_per_bank;
    *block = phys % blocks_per_bank;
}

static int erase_physical_block(uint32_t phys)
{
    unsigned int bank, block;
    int rc;

    phys_to_bank_block(phys, &bank, &block);
    rc = nand_hw_erase_block(bank, block);
    if (rc == 0)
        erase_count[phys]++;
    return rc;
}

static void mark_bad_best_effort(uint32_t phys)
{
    unsigned int bank, block;
    struct ftl_page_header hdr;
    /* static, not stack locals: this driver's module state is already
     * non-reentrant by design (see block_map[]/erase_count[]/etc. above),
     * and this project's own nand-nano3g.c already hit a real, confirmed-
     * on-hardware stack overflow from page/spare-sized buffers as
     * ordinary stack locals, stacked across nested calls, on this
     * bootloader's fixed 8KB stack. This file (ftl-nano3g.c) has several
     * functions with the same size of buffer, some of which call each
     * other (scan_and_mount() -> verify_block_fully(), both with their
     * own page/spare buffers), and had never been run on real hardware
     * before the same failure mode showed up here too -- fixed the same
     * way, everywhere in this file, rather than only where it was first
     * observed. */
    static uint8_t spare[NAND_MAX_SPARE_SIZE] FTL_DMA_BUF_ATTR;
    static uint8_t blank[NAND_MAX_PAGE_SIZE] FTL_DMA_BUF_ATTR;

    block_bad[phys] = true;
    phys_to_bank_block(phys, &bank, &block);

    /* Best-effort persistent bad-block stamp: if the erase above failed,
     * this write will very likely also fail, and that's fine -- runtime
     * re-discovery (an unreadable/inconsistent block is never selected as
     * a valid mapping) still keeps things safe, just without persistence
     * of the "bad" fact across a reboot in that particular case. */
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = FTL_HEADER_MAGIC_BAD;
    memset(spare, 0xFF, sizeof(spare));
    memcpy(spare, &hdr, sizeof(hdr));
    memset(blank, 0xFF, sizeof(blank));

    nand_hw_write_page(bank, block * chip_geo->pages_per_block, blank, spare);
}

static int allocate_free_block(uint32_t *phys_out)
{
    unsigned int i, best_i = 0;
    uint32_t best_erase = 0xFFFFFFFFu;

    if (free_count == 0)
        return -1;

    /* Least-erased-first wear leveling among the current free list. This
     * is a simple, easily verified policy; it is not a claim of matching
     * any particular vendor's wear-leveling sophistication, just a
     * reasonable and testable baseline. */
    for (i = 0; i < free_count; i++)
    {
        uint32_t phys = free_list[i];
        if (erase_count[phys] < best_erase)
        {
            best_erase = erase_count[phys];
            best_i = i;
        }
    }

    *phys_out = free_list[best_i];
    free_list[best_i] = free_list[--free_count];
    return 0;
}

static void release_free_block(uint32_t phys)
{
    if (free_count < total_physical_blocks)
        free_list[free_count++] = phys;
}

/* nand_hw_read_page() returns a negative NAND_HWERR_* on a hardware
 * timeout, but a NON-negative enum nand_ecc_result otherwise -- and
 * NAND_ECC_FAILED (uncorrectable, "data is not trustworthy" per its own
 * doc comment in nand-target.h) is one of those non-negative values.
 * Every call site below used to check only `rc < 0`, which is correct
 * for a hardware timeout but silently treated a real, hardware-flagged
 * uncorrectable ECC failure as if the read had succeeded -- the exact
 * "trusting an uncorrected page" risk this project's own MLC-support
 * analysis (NANO3G_MLC_NAND_ANALYSIS.md) warned about, except it turned
 * out to be a live bug in the SLC-era code path too, not just a future
 * MLC concern: any chip's read can return NAND_ECC_FAILED, not only
 * MLC's higher-error-rate ones. This wraps the check once instead of
 * repeating `rc < 0 || rc == NAND_ECC_FAILED` at every call site. */
static bool nand_read_page_untrusted(int rc)
{
    return rc < 0 || rc == NAND_ECC_FAILED;
}

/* --- Mount-time scan --------------------------------------------------
 * Reads every physical block's page-0 header. A block is a valid mapping
 * candidate for its declared logical_block if:
 *   - its magic matches, its flags aren't the bad sentinel, and
 *   - logical_block < num_logical_blocks, and
 *   - it is the highest generation seen so far for that logical block.
 * We do not require re-reading every page of every block up front (that
 * would mean a full-device read on every boot); instead we verify a
 * candidate's remaining pages lazily on first read/write of that logical
 * block, and treat a verification failure as "this candidate is invalid,
 * fall back to unmapped" rather than trusting page 0 alone forever. This
 * keeps mount fast while still not blindly trusting a single header. */
static bool verify_block_fully(uint32_t phys, uint32_t expect_logical,
                               uint32_t expect_generation)
{
    unsigned int bank, block, page;
    struct ftl_page_header hdr;
    /* static: see mark_bad_best_effort()'s comment. */
    static uint8_t data[NAND_MAX_PAGE_SIZE] FTL_DMA_BUF_ATTR;
    static uint8_t spare[NAND_MAX_SPARE_SIZE] FTL_DMA_BUF_ATTR;

    phys_to_bank_block(phys, &bank, &block);

    for (page = 0; page < chip_geo->pages_per_block; page++)
    {
        int rc = nand_hw_read_page(bank, block * chip_geo->pages_per_block + page,
                                   data, spare);
        if (nand_read_page_untrusted(rc))
            return false;

        memcpy(&hdr, spare, sizeof(hdr));
        if (hdr.magic != FTL_HEADER_MAGIC)
            return false;
        if (hdr.logical_block != expect_logical)
            return false;
        if (hdr.generation != expect_generation)
            return false;
        if (hdr.page_index != page)
            return false;
    }
    return true;
}

static int scan_and_mount(void)
{
    unsigned int bank, block, page;
    struct ftl_page_header hdr;
    /* static: see mark_bad_best_effort()'s comment -- this function
     * calls verify_block_fully() below, which has its own same-sized
     * data/spare pair; both being stack locals is what actually
     * overflowed the stack on real hardware. */
    static uint8_t data[NAND_MAX_PAGE_SIZE] FTL_DMA_BUF_ATTR;
    static uint8_t spare[NAND_MAX_SPARE_SIZE] FTL_DMA_BUF_ATTR;

    for (uint32_t phys = 0; phys < total_physical_blocks; phys++)
    {
        block_bad[phys] = false;
        erase_count[phys] = 0;
    }
    for (unsigned int lb = 0; lb < num_logical_blocks; lb++)
    {
        block_map[lb] = FTL_UNMAPPED;
        block_generation[lb] = 0;
    }
    free_count = 0;

    for (uint32_t phys = 0; phys < total_physical_blocks; phys++)
    {
        phys_to_bank_block(phys, &bank, &block);
        page = 0;

        int rc = nand_hw_read_page(bank, block * chip_geo->pages_per_block + page,
                                   data, spare);
        if (nand_read_page_untrusted(rc))
        {
            /* Unreadable, or uncorrectably wrong, block: treat as bad,
             * don't offer it for reuse. */
            block_bad[phys] = true;
            continue;
        }

        memcpy(&hdr, spare, sizeof(hdr));

        /* The bad-block sentinel is now purely a distinct magic value
         * (see FTL_HEADER_MAGIC_BAD's comment) rather than a separate
         * flags bit -- mark_bad_best_effort() always writes this exact
         * magic, and no other code path does, so there's no "blank
         * flash sets the flag bit" ambiguity to guard against the way
         * the old flags-based check needed to. */
        if (hdr.magic == FTL_HEADER_MAGIC_BAD)
        {
            block_bad[phys] = true;
            continue;
        }

        if (hdr.magic != FTL_HEADER_MAGIC ||
            hdr.logical_block >= num_logical_blocks ||
            hdr.page_index != 0)
        {
            /* Blank or foreign content: assume erased/reusable. We do not
             * erase it ourselves here (avoids unnecessary wear on a mount
             * that may just be re-mounting a NAND check image); it will
             * be erased before its first reuse by allocate path. */
            erase_count[phys] = 0;
            release_free_block(phys);
            continue;
        }

        /* erase_count is no longer part of the on-flash header (see its
         * own removal comment above) -- wear-tracking simply restarts
         * from 0 for every already-written block found at mount time,
         * same as erase_count[] is already initialised to 0 for every
         * physical block just above this loop. */

        if (hdr.generation > block_generation[hdr.logical_block])
        {
            uint32_t previous = block_map[hdr.logical_block];
            block_map[hdr.logical_block] = phys;
            block_generation[hdr.logical_block] = hdr.generation;
            if (previous != FTL_UNMAPPED)
                release_free_block(previous);
        }
        else
        {
            /* Superseded by a higher-generation copy already seen (or
             * that we'll see later and reconcile via the branch above --
             * see the fix-up pass below). Provisionally free; corrected
             * below if this turns out to have been the higher one. */
            release_free_block(phys);
        }
    }

    /* The single pass above can release a block that later turns out to
     * be the true highest-generation copy if blocks aren't scanned in a
     * convenient order. Do a fix-up pass: remove from the free list any
     * physical block that ended up as the winning mapping. */
    for (unsigned int lb = 0; lb < num_logical_blocks; lb++)
    {
        uint32_t phys = block_map[lb];
        if (phys == FTL_UNMAPPED)
            continue;
        for (unsigned int i = 0; i < free_count; i++)
        {
            if (free_list[i] == phys)
            {
                free_list[i] = free_list[--free_count];
                break;
            }
        }
    }

    /* Full verification pass: page 0's header alone doesn't rule out a
     * torn write leaving later pages of the "winning" candidate
     * inconsistent -- most importantly, a power loss partway through
     * rewrite_logical_block()'s per-page loop, after only a few of a
     * fresh physical block's pages were programmed with a new, higher
     * generation number than the still-fully-intact old copy of the same
     * logical block. Page 0 of that fresh block looks valid in isolation
     * (it's written first), so the single-highest-generation scan above
     * picks it as the winner and demotes the still-good old copy to the
     * free pool -- exactly the situation this pass must recover from. */
    for (unsigned int lb = 0; lb < num_logical_blocks; lb++)
    {
        uint32_t phys = block_map[lb];
        if (phys == FTL_UNMAPPED)
            continue;
        if (verify_block_fully(phys, lb, block_generation[lb]))
            continue;

        /* The winning candidate didn't hold up under full verification.
         * Free it (it's a torn write, not a hardware fault, so it's safe
         * to erase and reuse later) and fall back to the next-best
         * candidate still sitting in the free pool: the highest-
         * generation block that still declares this logical block and
         * itself passes full verification. There should be at most one
         * such fallback in practice (our write path never leaves more
         * than one stale copy of a logical block unerased at a time),
         * but we search for the best rather than assume that. */
        release_free_block(phys);
        block_map[lb] = FTL_UNMAPPED;
        block_generation[lb] = 0;

        uint32_t best_fallback = FTL_UNMAPPED;
        uint32_t best_fallback_gen = 0;
        unsigned int best_fallback_slot = 0;

        for (unsigned int i = 0; i < free_count; i++)
        {
            uint32_t cand = free_list[i];
            unsigned int cbank, cblock;
            struct ftl_page_header chdr;
            /* static: see mark_bad_best_effort()'s comment. */
            static uint8_t cdata[NAND_MAX_PAGE_SIZE] FTL_DMA_BUF_ATTR;
            static uint8_t cspare[NAND_MAX_SPARE_SIZE] FTL_DMA_BUF_ATTR;

            if (cand == phys)
                continue; /* this is the candidate that just failed verification */

            phys_to_bank_block(cand, &cbank, &cblock);
            if (nand_read_page_untrusted(nand_hw_read_page(cbank,
                    cblock * chip_geo->pages_per_block, cdata, cspare)))
                continue;
            memcpy(&chdr, cspare, sizeof(chdr));
            if (chdr.magic != FTL_HEADER_MAGIC || chdr.logical_block != lb ||
                chdr.page_index != 0)
                continue;
            if (best_fallback == FTL_UNMAPPED || chdr.generation > best_fallback_gen)
            {
                best_fallback = cand;
                best_fallback_gen = chdr.generation;
                best_fallback_slot = i;
            }
        }

        if (best_fallback != FTL_UNMAPPED &&
            verify_block_fully(best_fallback, lb, best_fallback_gen))
        {
            block_map[lb] = best_fallback;
            block_generation[lb] = best_fallback_gen;
            free_list[best_fallback_slot] = free_list[--free_count];
        }
        /* Otherwise this logical block genuinely has no valid copy left
         * (should only occur if the fallback itself was also corrupted,
         * e.g. two overlapping faults) and stays unmapped, reading back
         * as erased. */
    }

    return 0;
}

int ftl_init(void)
{
    mounted = false;
    write_error_latched = false;
    /* Drop any stale write-back cache from a previous mount before the
     * scan below rebuilds block_map[]; a leftover wb_valid would alias a
     * logical block against freshly scanned mappings. */
    wb_reset();

    bank_count = nand_get_bank_count();
    if (bank_count == 0)
        return FTL_ERR_NO_BANKS;

    chip_geo = nand_get_bank_geometry(0);
    if (!chip_geo || !chip_geo->recognized)
        return FTL_ERR_UNRECOGNIZED;

    blocks_per_bank = chip_geo->blocks_per_bank;
    sectors_per_page = chip_geo->page_size / NAND_PAGE_SIZE;
    if (sectors_per_page == 0)
        sectors_per_page = 1;
    sectors_per_block = chip_geo->pages_per_block * sectors_per_page;

    total_physical_blocks = bank_count * blocks_per_bank;
    if (total_physical_blocks > FTL_MAX_PHYSICAL_BLOCKS)
        total_physical_blocks = FTL_MAX_PHYSICAL_BLOCKS; /* clamp, don't fault */

    spare_pool_target = total_physical_blocks >> FTL_SPARE_FRACTION_SHIFT;
    if (spare_pool_target < FTL_SPARE_MIN_BLOCKS)
        spare_pool_target = FTL_SPARE_MIN_BLOCKS;

    if (total_physical_blocks <= spare_pool_target + 1)
        return FTL_ERR_TOO_SMALL;

    /* struct ftl_page_header's logical_block field is a uint16_t (see
     * its own comment for why): refuse to mount rather than silently
     * truncate/alias logical block numbers on a chip whose geometry
     * would need more than that many logical blocks. */
    if (total_physical_blocks - spare_pool_target > 0x10000u)
        return FTL_ERR_TOO_SMALL;

    /* Same reasoning for page_index (uint8_t): a chip with more pages
     * per block than this on-flash format can address would have two
     * different real pages alias onto the same stored page_index byte,
     * which verify_block_fully()'s check could then wrongly accept. */
    if (chip_geo->pages_per_block > FTL_MAX_PAGES_PER_BLOCK)
        return FTL_ERR_TOO_SMALL;

    num_logical_blocks = total_physical_blocks - spare_pool_target;

    scan_and_mount();

#ifdef FTL_READONLY
    readonly_mount = true;
#else
    /* Writable only when every bank reported a fully recognised,
     * validated geometry -- see nand_vendor_decode()'s ->recognized flag.
     * An unusual/unidentified chip is still fully readable (whatever
     * valid-looking blocks the scan found), just not written to. */
    readonly_mount = !chip_geo->recognized;
#endif

    mounted = true;
    return 0;
}

static int read_one_sector(uint32_t sector, void *buffer)
{
    unsigned int logical_block = sector / sectors_per_block;
    unsigned int sector_in_block = sector % sectors_per_block;
    unsigned int page_in_block = sector_in_block / sectors_per_page;
    unsigned int subpage = sector_in_block % sectors_per_page;
    uint32_t phys;
    unsigned int bank, block;
    /* static: see mark_bad_best_effort()'s comment. */
    static uint8_t data[NAND_MAX_PAGE_SIZE] FTL_DMA_BUF_ATTR;

    if (logical_block >= num_logical_blocks)
        return -1;

    phys = block_map[logical_block];
    if (phys == FTL_UNMAPPED)
    {
        memset(buffer, 0xFF, NAND_PAGE_SIZE);
        return 0;
    }

    phys_to_bank_block(phys, &bank, &block);
    int rc = nand_hw_read_page(bank, block * chip_geo->pages_per_block + page_in_block,
                               data, NULL);
    if (nand_read_page_untrusted(rc))
        return rc < 0 ? rc : -1;

    memcpy(buffer, data + subpage * NAND_PAGE_SIZE, NAND_PAGE_SIZE);
    return 0;
}

/* --- Single-block write-back cache -----------------------------------
 *
 * Why this exists. rewrite_logical_block() below rewrites an *entire*
 * erase block (allocate + erase + program every one of ~128 pages, plus
 * read-back of the old block for the pages not being changed) on every
 * call, regardless of how few sectors actually changed. That is the
 * crash-safety design (the old block stays intact until the new one is
 * fully written), and it is fine for the occasional small write -- but a
 * host file copy is pathological for it: the filesystem streams the
 * sectors of one logical block as many separate storage_write_sectors()
 * calls (and interleaves small FAT/directory updates), so a single
 * logical block can be rewritten from scratch dozens of times as its
 * sectors arrive one chunk at a time. Measured result: ~7.5 KB/s and
 * multi-minute stalls that trip the host's USB write timeout and abort
 * the copy mid-stream.
 *
 * The cache collapses that. It holds exactly one logical block's worth
 * of sectors in RAM. Writes that hit the cached block just update RAM;
 * only when the write moves to a *different* logical block (or the host
 * issues SYNCHRONIZE CACHE, or the device is unmounted) is the cached
 * block committed to NAND with a single rewrite_logical_block() call. A
 * sequential file write therefore costs ~1 full-block rewrite per block
 * instead of ~128.
 *
 * Crash-safety is unchanged. The actual commit still goes through
 * rewrite_logical_block(), which never retires the old physical block
 * until the new one is completely written. The one new window is "data
 * acknowledged to the host but still only in RAM" -- which is exactly
 * what ftl_sync() (SCSI SYNCHRONIZE CACHE, and nand_close() on unplug)
 * exists to close, the same contract every write-back cache relies on.
 * A power loss with an unflushed cache loses only the not-yet-synced
 * tail, and leaves the previous generation of every block fully intact;
 * it can never corrupt an already-committed block.
 *
 * RAM cost is one logical block: sectors_per_block * NAND_PAGE_SIZE =
 * 128 * 2048 = 256 KB on this chip, out of 32 MB. The buffer is sized to
 * the static maximum so it needs no heap.
 *
 * Concurrency: like the rest of this file, this is non-reentrant and
 * relies on all storage calls being serialised on the one storage/USB
 * thread (see the module-state note on block_map[] etc.). */

/* One logical block, at the static worst case (max pages/block * max
 * sectors/page * sector size). Carries FTL_DMA_BUF_ATTR because on flush
 * its pages are handed straight to nand_hw_write_page(). */
static uint8_t  wb_data[FTL_MAX_PAGES_PER_BLOCK * NAND_MAX_PAGE_SIZE]
                    FTL_DMA_BUF_ATTR;
static unsigned int wb_logical_block;   /* which logical block is cached */
static bool         wb_valid;           /* is a block currently cached? */
static bool         wb_dirty;           /* does it differ from NAND? */

static int rewrite_logical_block(unsigned int logical_block,
                                 unsigned int first_sector_in_block,
                                 unsigned int count,
                                 const uint8_t *buffer);

static void wb_reset(void)
{
    wb_valid = false;
    wb_dirty = false;
    wb_logical_block = 0;
}

/* Commit the cached block to NAND if it has unwritten changes. Returns 0
 * on success (or if there was nothing to flush), -1 on write failure with
 * the error latched -- matching the rest of the write path. */
static int wb_flush(void)
{
    if (!wb_valid || !wb_dirty)
        return 0;

    /* Commit the whole cached block in one rewrite. Passing the entire
     * block as the changed range means rewrite_logical_block() takes its
     * data purely from wb_data and never re-reads the old block -- the
     * cache already holds the merged, up-to-date contents. */
    if (rewrite_logical_block(wb_logical_block, 0, sectors_per_block,
                              wb_data) != 0)
    {
        /* Leave the cache marked dirty so a later ftl_sync() can surface
         * the failure again rather than silently dropping the data. */
        write_error_latched = true;
        return -1;
    }

    wb_dirty = false;
    return 0;
}

/* Load `logical_block` into the cache, flushing whatever was there first.
 * The cache is filled from the block's current mapped contents (or 0xFF
 * where unmapped) so that subsequent partial writes merge correctly and
 * reads are served consistently. Returns 0, or -1 if flushing the
 * previous block failed. */
static int wb_load(unsigned int logical_block)
{
    unsigned int page;
    uint32_t phys;
    unsigned int bank, block;
    /* static: see mark_bad_best_effort()'s comment on why page/spare
     * buffers here are module-static rather than stack locals. */
    static uint8_t data[NAND_MAX_PAGE_SIZE] FTL_DMA_BUF_ATTR;

    if (wb_valid && wb_logical_block == logical_block)
        return 0;

    if (wb_flush() != 0)
        return -1;

    phys = block_map[logical_block];
    if (phys == FTL_UNMAPPED)
    {
        /* Never-written block: reads back as erased. */
        memset(wb_data, 0xFF, (size_t)sectors_per_block * NAND_PAGE_SIZE);
    }
    else
    {
        phys_to_bank_block(phys, &bank, &block);
        for (page = 0; page < chip_geo->pages_per_block; page++)
        {
            uint8_t *dst = wb_data + (size_t)page * chip_geo->page_size;
            int rc = nand_hw_read_page(bank,
                        block * chip_geo->pages_per_block + page, data, NULL);
            if (nand_read_page_untrusted(rc))
            {
                /* Unreadable/uncorrectable page: fill with the erased
                 * pattern rather than fail the load, mirroring
                 * rewrite_logical_block()'s own handling of a degraded
                 * old copy. The sectors the caller is about to write are
                 * overwritten anyway; the rest read back as erased. */
                memset(dst, 0xFF, chip_geo->page_size);
            }
            else
            {
                memcpy(dst, data, chip_geo->page_size);
            }
        }
    }

    wb_logical_block = logical_block;
    wb_valid = true;
    wb_dirty = false;
    return 0;
}

int ftl_read(uint32_t sector, uint32_t count, void *buffer)
{
    uint8_t *out = (uint8_t *)buffer;

    if (!mounted)
        return -1;

    while (count--)
    {
        unsigned int logical_block = sector / sectors_per_block;
        unsigned int sector_in_block = sector % sectors_per_block;

        /* Serve from the write-back cache when this sector belongs to the
         * currently cached block, so reads see writes that have been
         * acknowledged but not yet committed to NAND. */
        if (wb_valid && logical_block == wb_logical_block)
        {
            memcpy(out, wb_data + (size_t)sector_in_block * NAND_PAGE_SIZE,
                   NAND_PAGE_SIZE);
        }
        else
        {
            int rc = read_one_sector(sector, out);
            if (rc < 0)
                return rc;
        }
        sector++;
        out += NAND_PAGE_SIZE;
    }
    return 0;
}

/* Rewrite one whole logical block, merging `count` sectors of new data
 * starting at `first_sector_in_block` from `buffer` with the previously
 * mapped block's content for any sectors outside that range. Allocates a
 * fresh physical block, writes it completely, then reclaims the old one.
 * This keeps the old copy intact and readable until the new one is fully
 * and successfully written -- the crash-safety property the on-flash
 * format is designed around. */
static int rewrite_logical_block(unsigned int logical_block,
                                 unsigned int first_sector_in_block,
                                 unsigned int count,
                                 const uint8_t *buffer)
{
    uint32_t old_phys = block_map[logical_block];
    uint32_t new_generation = block_generation[logical_block] + 1;
    /* static: see mark_bad_best_effort()'s comment. */
    static uint8_t page_data[NAND_MAX_PAGE_SIZE] FTL_DMA_BUF_ATTR;
    static uint8_t spare[NAND_MAX_SPARE_SIZE] FTL_DMA_BUF_ATTR;
    struct ftl_page_header hdr;
    unsigned int attempt;

    /* A block picked from the free pool can itself turn out to be bad
     * (fail to erase, or fail partway through programming) -- that's
     * exactly what "wear out" and "runtime bad block discovery" look
     * like. Retry with a fresh block from the pool a bounded number of
     * times rather than giving up on the very first faulty pick; this is
     * what actually gives mark_bad_best_effort() a chance to matter
     * rather than just failing the write outright the first time a
     * marginal block is drawn. The bound is the spare pool size plus a
     * small margin: with a healthy chip we should virtually never retry
     * at all, and a pathologically bad chip (more faulty blocks than the
     * whole spare pool) is expected to eventually fail outright rather
     * than loop forever. */
    for (attempt = 0; attempt < spare_pool_target + 4; attempt++)
    {
        uint32_t new_phys;
        unsigned int bank, block, page;
        bool page_write_failed = false;

        if (allocate_free_block(&new_phys) != 0)
        {
            nand_debug_log("ftl: pool exhausted (free=%u att=%u)",
                           free_count, attempt);
            return -1; /* pool exhausted; nothing left to try */
        }

        /* Free-list invariant: entries are always pre-erased. Guard
         * against that invariant having been violated (e.g. a block
         * released after a failed program without a chance to erase it
         * yet) by erasing again defensively; a redundant erase of an
         * already-erased block is harmless. */
        int erc = erase_physical_block(new_phys);
        if (erc != 0)
        {
            if (attempt < 2)
                nand_debug_log("ftl: erase p=%lu rc=%d att=%u",
                               (unsigned long)new_phys, erc, attempt);
            /* A systematic error is not a property of this block, so
             * retrying across the whole spare pool cannot help and would
             * needlessly stamp hundreds of good blocks bad (the loop runs
             * spare_pool_target+4 times). Fail fast and leave the pool
             * intact; only genuinely block-specific failures
             * (ERASE_FAILED/PROGRAM_FAILED/TIMEOUT) justify retrying. */
            if (erc == NAND_HWERR_UNSUPPORTED_GEOMETRY ||
                erc == NAND_HWERR_NO_CHIP)
                return -1;
            mark_bad_best_effort(new_phys);
            continue; /* try the next free block instead of giving up */
        }

        phys_to_bank_block(new_phys, &bank, &block);

        for (page = 0; page < chip_geo->pages_per_block; page++)
        {
            unsigned int sp;
            for (sp = 0; sp < sectors_per_page; sp++)
            {
                unsigned int sector_in_block = page * sectors_per_page + sp;
                uint8_t *dst = page_data + sp * NAND_PAGE_SIZE;

                if (sector_in_block >= first_sector_in_block &&
                    sector_in_block < first_sector_in_block + count)
                {
                    memcpy(dst, buffer + (sector_in_block - first_sector_in_block)
                                         * NAND_PAGE_SIZE,
                           NAND_PAGE_SIZE);
                }
                else if (old_phys != FTL_UNMAPPED)
                {
                    unsigned int obank, oblock;
                    phys_to_bank_block(old_phys, &obank, &oblock);
                    /* static: see mark_bad_best_effort()'s comment. */
                    static uint8_t old_page[NAND_MAX_PAGE_SIZE] FTL_DMA_BUF_ATTR;
                    if (nand_read_page_untrusted(nand_hw_read_page(obank,
                            oblock * chip_geo->pages_per_block + page,
                            old_page, NULL)))
                    {
                        /* Old copy has degraded (hardware read failure,
                         * or a real, hardware-flagged uncorrectable ECC
                         * result -- either way not safe to trust) fill
                         * with erased pattern rather than fail the whole
                         * rewrite outright, since we still want to
                         * preserve the sectors we *can* merge in this
                         * operation. */
                        memset(old_page, 0xFF, chip_geo->page_size);
                    }
                    memcpy(dst, old_page + sp * NAND_PAGE_SIZE, NAND_PAGE_SIZE);
                }
                else
                {
                    memset(dst, 0xFF, NAND_PAGE_SIZE);
                }
            }

            memset(&hdr, 0, sizeof(hdr));
            hdr.magic = FTL_HEADER_MAGIC;
            hdr.logical_block = (uint16_t)logical_block;
            hdr.generation = new_generation;
            hdr.page_index = (uint8_t)page;
            memset(spare, 0xFF, sizeof(spare));
            memcpy(spare, &hdr, sizeof(hdr));

            int wrc = nand_hw_write_page(bank,
                                         block * chip_geo->pages_per_block + page,
                                         page_data, spare);
            if (wrc != 0)
            {
                if (attempt < 2)
                    nand_debug_log("ftl: wr p=%lu pg=%u rc=%d att=%u",
                                   (unsigned long)new_phys, page, wrc, attempt);
                /* Systematic error: see the erase path's comment above --
                 * retrying across the pool cannot help and would stamp
                 * hundreds of good blocks bad. */
                if (wrc == NAND_HWERR_UNSUPPORTED_GEOMETRY ||
                    wrc == NAND_HWERR_NO_CHIP)
                    return -1;
                mark_bad_best_effort(new_phys);
                page_write_failed = true;
                break;
            }
        }

        if (page_write_failed)
            continue; /* try the next free block instead of giving up */

        /* Only now, after the new block is fully and successfully
         * written, do we retire the old one. */
        block_map[logical_block] = new_phys;
        block_generation[logical_block] = new_generation;

        if (old_phys != FTL_UNMAPPED)
        {
            if (erase_physical_block(old_phys) == 0)
                release_free_block(old_phys);
            else
                mark_bad_best_effort(old_phys);
        }

        return 0;
    }

    return -1; /* exhausted our retry budget without a successful write */
}

int ftl_write(uint32_t sector, uint32_t count, const void *buffer)
{
    const uint8_t *in = (const uint8_t *)buffer;

    if (!mounted)
    {
        nand_debug_log("ftl_write: not mounted");
        return -1;
    }
    if (readonly_mount)
    {
        nand_debug_log("ftl_write: readonly mount");
        return -1;
    }

    while (count > 0)
    {
        unsigned int logical_block = sector / sectors_per_block;
        unsigned int sector_in_block = sector % sectors_per_block;
        unsigned int run = sectors_per_block - sector_in_block;

        if (run > count)
            run = count;
        if (logical_block >= num_logical_blocks)
        {
            nand_debug_log("ftl_write: lb %u >= %u", logical_block,
                           num_logical_blocks);
            write_error_latched = true;
            return -1;
        }

        /* Route through the write-back cache: bring this logical block
         * into RAM (flushing any previously cached block first), then
         * merge the new sectors into it. The expensive full-block NAND
         * rewrite happens only when the cache later moves to a different
         * block or is flushed by ftl_sync(). A file copy that streams the
         * sectors of one block across many calls therefore commits that
         * block once, not once per call. */
        if (wb_load(logical_block) != 0)
        {
            nand_debug_log("ftl_write: wb_load lb=%u failed", logical_block);
            write_error_latched = true;
            return -1;
        }

        memcpy(wb_data + (size_t)sector_in_block * NAND_PAGE_SIZE, in,
               (size_t)run * NAND_PAGE_SIZE);
        wb_dirty = true;

        sector += run;
        count -= run;
        in += (size_t)run * NAND_PAGE_SIZE;
    }

    return 0;
}

int ftl_sync(void)
{
    /* Commit the write-back cache. This is the point that closes the
     * "acknowledged to the host but only in RAM" window: the host issues
     * SCSI SYNCHRONIZE CACHE after writes and around format/dismount
     * (see usb_storage.c), and nand_close() calls it on unplug. After a
     * successful flush there is nothing outstanding; surface any latched
     * error either way, matching the contract in ftl-target.h. */
    if (wb_flush() != 0)
        return -1;
    return write_error_latched ? -1 : 0;
}

uint32_t ftl_num_sectors(void)
{
    if (!mounted)
        return 0;
    return (uint32_t)num_logical_blocks * sectors_per_block;
}

bool ftl_readonly_mount(void)
{
    return readonly_mount;
}
