/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") flash translation layer -- v2, page-level and
 * log-structured.
 *
 * This is the fast successor to the original block-level FTL (ftl-nano3g.c).
 * Same public API (ftl-target.h), same on-flash *philosophy* (self-describing
 * pages, no separate superblock commit protocol, crash-safe by construction),
 * but a fundamentally different data structure that removes the original's
 * dominant cost.
 *
 * WHY IT EXISTS
 * -------------
 * The original FTL maps one logical block to one physical erase block, so any
 * write -- even a single sector -- rewrites the entire ~128-page erase block
 * (allocate + erase + copy-forward every page + program every page). That is
 * ~128x write amplification, measured at ~7.5 KB/s on real hardware, slow
 * enough that a host file copy trips the USB write timeout and aborts. This
 * FTL maps at *page* granularity and appends writes to a moving frontier, so
 * writing one page programs ~one page. Erases are deferred to a background
 * garbage collector, off the host's critical path -- the same reason Apple's
 * own FTL is fast.
 *
 * MODEL (see NANO3G_FTL_V2_DESIGN for the full writeup)
 * -----
 *   - Map granularity is one *physical NAND page* (4KiB on this chip = 2
 *     logical NAND_PAGE_SIZE sectors). page_map[LPN] -> PPN or PAGE_UNMAPPED.
 *   - Writes append to the current "open" block at a write frontier, one page
 *     at a time. The previous copy of that LPN is left in place and simply
 *     becomes stale (its block's valid-page count drops). No erase, no
 *     read-modify-write on the write path.
 *   - Each data page's 12-byte spare header carries its LPN and a device-
 *     global monotonically increasing sequence number (seq). When two pages
 *     claim the same LPN (e.g. after a crash), the higher seq wins. That one
 *     rule is the entire freshest-copy / recovery mechanism.
 *   - Garbage collection reclaims stale space: pick the block with the fewest
 *     valid pages, relocate its live pages (with fresh seq) to the frontier,
 *     then erase it and return it to the free queue.
 *
 * CRASH SAFETY
 * ------------
 * A page is only "moved" once its new copy is fully programmed AND the map
 * points there; the old copy stays valid until its whole block is erased, and
 * a block is never erased until every live page in it has been relocated. A
 * torn (partially written) page fails header validation at mount and is
 * ignored, so the older copy wins. Power loss therefore can lose only writes
 * that were never synced, never already-committed data -- the same guarantee
 * the original design gave, preserved at page granularity. ftl_sync() is the
 * durability boundary the host relies on (SCSI SYNCHRONIZE CACHE; nand_close()
 * on unplug).
 *
 * This file is selected instead of ftl-nano3g.c by defining FTL_NANO3G_V2 at
 * build time; the two never compile together. It shares nand-target.h's
 * hardware facts (DMA alignment, spare budget, ecc-result semantics) with the
 * original and deliberately keeps the same public surface so nand-nano3g.c and
 * the ftltest2 harness need no changes.
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

/* Same hardware rationale as the original: every buffer handed to
 * nand_hw_read_page()/nand_hw_write_page() must be 32-byte aligned (the FMC
 * DMAs into it ignoring the low address bits, and cache maintenance works on
 * whole lines). See nand-target.h's NAND_DMA_BUF_ATTR comment. */
#define FTL_DMA_BUF_ATTR  NAND_DMA_BUF_ATTR

/* --- Static sizing --------------------------------------------------------
 *
 * Option A (flat in-RAM map). The chip is 4 banks x 4096 blocks x 128 pages;
 * we size the static tables for the worst case we support so nothing needs a
 * heap (the bootloader may run before any allocator is up).
 */
/* Static bounds for the RAM tables. These must be sized to the real parts we
 * support, NOT to the independent worst case of every geometry dimension
 * multiplied together -- doing that (8192 blocks/bank * 256 pages/block * 4
 * banks) would demand a 33 MiB map that does not fit even in DRAM. The Nano 3G's
 * S5L8702 wires at most NAND_MAX_BANKS (4) chip-enables.
 *
 * Two real 8GB-unit geometries are supported, and the bound must cover the
 * larger block count of the two because one universal binary must mount either:
 *   - Samsung 4-die MLC: 4096 blocks/bank of 128 pages, 4096-byte pages
 *     (512KiB erase block), 2 GiB/die.
 *   - Hynix   4-die MLC: 8192 blocks/bank of 128 pages, 2048-byte pages
 *     (256KiB erase block), 2 GiB/die.
 * Both dies are the same 2 GiB, but the Hynix part's smaller physical page
 * carves the die into twice as many blocks and twice as many physical pages.
 * Because this FTL maps at physical-page granularity, the Hynix part needs
 * twice the map/bookkeeping entries of the Samsung part for the same bytes --
 * so the static bound is the Hynix block count (8192), not the Samsung one.
 *
 * ftl_init() still refuses to mount (FTL_ERR_TOO_SMALL) any geometry that
 * would exceed these bounds, so an unexpected even-larger chip fails loudly
 * rather than corrupting memory.
 *
 * page_map cost at these bounds: 4 * 8192 * 128 = 4,194,304 pages * 4 bytes
 * = 16 MiB. That is the deliberate Option-A cost for supporting the 2KB-page
 * Hynix part. It lives in the bootloader's BSS_AREA, carved out of DRAM (see
 * boot.lds: ORIGIN = DRAM_ORIG + MAX_LOADSIZE, LENGTH ~24 MiB), so 16 MiB fits
 * with headroom; it does NOT fit in IRAM. In the main firmware the same tables
 * land in .bss ahead of the elastic .audiobuf, so the extra 8 MiB (vs the old
 * 4096 bound) simply shortens the audio pre-buffer rather than failing the
 * link. The tables must still never be sized larger than the parts we support
 * actually need. */
#define FTL_MAX_BLOCKS_PER_BANK  8192
#define FTL_MAX_PHYSICAL_BLOCKS  (NAND_MAX_BANKS * FTL_MAX_BLOCKS_PER_BANK)
/* Pages per block on the parts we support. 256 remains the on-flash format
 * ceiling (page_index is byte-sized) and is checked separately in ftl_init();
 * the static tables are sized to the 128 real hardware uses. */
#define FTL_MAX_PAGES_PER_BLOCK      256   /* format ceiling (validated) */
#define FTL_MAX_PAGES_PER_BLOCK_HW   128   /* real hardware, sizes the map */

/* The logical page map is the one big table, sized at runtime against the
 * real geometry (num_logical_pages) but statically bounded so it needs no
 * heap (the bootloader runs before any allocator). */
#define FTL_MAX_LOGICAL_PAGES \
    (FTL_MAX_PHYSICAL_BLOCKS * FTL_MAX_PAGES_PER_BLOCK_HW)

/* Fraction of blocks held back as spare/free capacity (GC headroom + runtime
 * bad blocks). Same rule of thumb as the original: 1/32 (~3%), min 4. A log-
 * structured FTL needs at least a couple of free blocks to make progress (one
 * open frontier + at least one GC target). */
#define FTL_SPARE_FRACTION_SHIFT 5
#define FTL_SPARE_MIN_BLOCKS     4

/* GC low-water mark: when the free-block queue drops to this many, start
 * reclaiming opportunistically. A hard reclaim also runs if we hit zero free
 * blocks while writing. */
#define FTL_GC_LOW_WATER_MIN     2

/* --- On-flash per-page header (must fit in NAND_SPARE_META_BYTES = 12) ----
 *
 * Stored in the leading bytes of each page's spare/OOB area. Compile-time
 * checked below. Layout:
 *   magic(2) type(1) _pad(1) lpn(4) seq(4)  = 12 bytes exactly.
 */
#define FTL2_MAGIC       ((uint16_t)0x464Cu) /* 'FL' log-FTL data/GC page   */
#define FTL2_MAGIC_BAD   ((uint16_t)0x4642u) /* 'FB' bad-block sentinel     */

#define FTL2_TYPE_DATA    0x01u  /* ordinary host write                     */
#define FTL2_TYPE_GC      0x02u  /* relocated by garbage collection         */
#define FTL2_TYPE_SUMMARY 0x03u  /* block summary page (last page of block) */
#define FTL2_TYPE_CLEAN   0xCEu  /* clean-unmount marker (no mapping)       */

struct ftl2_page_header
{
    uint16_t magic;      /* FTL2_MAGIC, or FTL2_MAGIC_BAD on page 0 of a bad block */
    uint8_t  type;       /* FTL2_TYPE_*                                      */
    uint8_t  _pad;
    uint32_t lpn;        /* logical page this physical page holds (data/GC) */
    uint32_t seq;        /* device-global monotonic; higher == newer        */
} __attribute__((packed));

_Static_assert(sizeof(struct ftl2_page_header) <= NAND_SPARE_META_BYTES,
              "ftl2_page_header must fit in NAND_SPARE_META_BYTES (12) -- the "
              "controller cannot round-trip more spare metadata than that");

/* --- Per-block summary page ----------------------------------------------
 *
 * The last page of every block is reserved as a SUMMARY: it records, for each
 * data page in the block, that page's (lpn, seq). This is a pure mount-time
 * OPTIMISATION, never a correctness dependency -- every data page still
 * carries its own self-describing header, so if a summary is missing or torn
 * the mount simply falls back to full-scanning that one block.
 *
 * Why it matters: without it, mount must read every one of the ~2.1M pages on
 * an 8GB chip to rebuild the map (confirmed on hardware as a multi-minute
 * hang). With it, mount reads page 0 (to classify the block) plus the last
 * page (the summary) -- ~2 reads per block, ~32k total, i.e. seconds.
 *
 * The summary lives in the page's DATA area (not the 12-byte spare, which is
 * far too small for 127 entries): an array of ftl2_summary_entry. With 128
 * pages/block that is 127 data pages * 8 bytes = 1016 bytes, comfortably
 * inside a 2KB or 4KB page. The summary page's own spare header is type
 * FTL2_TYPE_SUMMARY with the block's highest seq, so mount can still fold the
 * block's freshest seq into the global counter from the summary alone. */
struct ftl2_summary_entry
{
    uint32_t lpn;   /* logical page this data page held, or PAGE_UNMAPPED */
    uint32_t seq;   /* that data page's seq                              */
} __attribute__((packed));

/* --- Geometry (filled by ftl_init) ---------------------------------------- */
static const struct nand_geometry *chip_geo;
static unsigned int bank_count;
static unsigned int blocks_per_bank;
static unsigned int pages_per_block;
static unsigned int data_pages_per_block;   /* pages_per_block - 1 (summary) */
static unsigned int summary_page_index;     /* == pages_per_block - 1        */
static unsigned int sectors_per_page;       /* NAND page / NAND_PAGE_SIZE (2) */
static unsigned int total_physical_blocks;
static uint32_t     total_physical_pages;
static unsigned int spare_pool_target;
static unsigned int usable_blocks;          /* blocks backing the logical space */
static uint32_t     num_logical_pages;      /* usable_blocks * pages_per_block  */

/* --- RAM state ------------------------------------------------------------ */

#define PAGE_UNMAPPED   0xFFFFFFFFu
#define BLOCK_NONE      0xFFFFFFFFu

/* The flat logical->physical page map (Option A). */
static uint32_t page_map[FTL_MAX_LOGICAL_PAGES];

/* Per physical block bookkeeping, indexed by physical block number. */
static uint16_t block_valid[FTL_MAX_PHYSICAL_BLOCKS]; /* live pages in block   */
static uint32_t block_erasectr[FTL_MAX_PHYSICAL_BLOCKS];
static uint8_t  block_state[FTL_MAX_PHYSICAL_BLOCKS];

#define BST_FREE     0u   /* erased, in the free queue, ready to open        */
#define BST_OPEN     1u   /* currently being filled (the frontier)          */
#define BST_CLOSED   2u   /* fully written, holds valid and/or stale pages   */
#define BST_BAD      3u   /* excluded permanently                           */

/* Free-block queue: physical block numbers ready to be opened. Kept as a
 * simple ring; wear-levelling picks least-erased at open time rather than
 * relying on queue order. */
static uint32_t free_queue[FTL_MAX_PHYSICAL_BLOCKS];
static unsigned int free_count;

/* Write frontier. */
static uint32_t frontier_block;       /* physical block currently open, or NONE */
static unsigned int frontier_page;    /* next free page index within it       */

/* Per-open-block record of what each data page holds, so the SUMMARY page can
 * be written when the block closes. Indexed by page-in-block (0..data_pages-1).
 * Sized to the format ceiling. */
static struct ftl2_summary_entry frontier_entries[FTL_MAX_PAGES_PER_BLOCK];

/* Device-global sequence counter; every data/GC page program stamps and then
 * increments this. Established at mount as max(seen)+1. */
static uint32_t next_seq;

static bool mounted;
static bool readonly_mount;
static bool write_error_latched;

/* A "clean" marker is laid down on sync and cleared on first write, so a
 * future checkpointed mount (v2.1) can trust a fast path after a clean
 * unmount. v2 always does the full scan, so this is currently informational,
 * but we maintain it now so the format is forward-compatible. */
static bool clean_flag;

/* --- Shared static DMA buffers -------------------------------------------
 *
 * As in the original FTL, page/spare buffers are module-static rather than
 * stack locals: the bootloader runs on a fixed 8KB stack and page-sized
 * locals across nested calls overflowed it on real hardware. This file's
 * functions call each other (write -> gc -> relocate), so the same discipline
 * applies. The driver is non-reentrant and everything runs on the one storage
 * thread, so shared static scratch is safe.
 */
static uint8_t io_data[NAND_MAX_PAGE_SIZE] FTL_DMA_BUF_ATTR;
static uint8_t io_spare[NAND_MAX_SPARE_SIZE] FTL_DMA_BUF_ATTR;
static uint8_t gc_data[NAND_MAX_PAGE_SIZE] FTL_DMA_BUF_ATTR;
static uint8_t gc_spare[NAND_MAX_SPARE_SIZE] FTL_DMA_BUF_ATTR;

/* --- Small helpers -------------------------------------------------------- */

static bool nand_read_page_untrusted(int rc)
{
    /* nand_hw_read_page returns negative NAND_HWERR_* on a hardware timeout,
     * but a NON-negative enum nand_ecc_result otherwise -- and
     * NAND_ECC_FAILED is non-negative. Both mean "do not trust these bytes".
     * (Same subtlety, and the same fix, as the original FTL.) */
    return rc < 0 || rc == NAND_ECC_FAILED;
}

static void ppn_to_bank_page(uint32_t ppn, unsigned int *bank, uint32_t *page)
{
    uint32_t pages_per_bank = (uint32_t)blocks_per_bank * pages_per_block;
    *bank = (unsigned int)(ppn / pages_per_bank);
    *page = ppn % pages_per_bank;
}

static uint32_t ppn_to_phys_block(uint32_t ppn)
{
    /* Absolute physical block index across all banks. */
    return ppn / pages_per_block;
}

static void phys_block_to_bank_block(uint32_t pblock, unsigned int *bank,
                                     uint32_t *block)
{
    *bank  = (unsigned int)(pblock / blocks_per_bank);
    *block = pblock % blocks_per_bank;
}

/* First PPN of a physical block. */
static uint32_t phys_block_first_ppn(uint32_t pblock)
{
    return pblock * pages_per_block;
}

/* Seq comparison with wrap defence: a is "newer than" b if the forward
 * distance from b to a (mod 2^32) is smaller than the reverse distance. In
 * normal operation seq never wraps within device lifetime; this just prevents
 * a corrupted far-future seq from always winning. */
static bool seq_newer(uint32_t a, uint32_t b)
{
    return (uint32_t)(a - b) < 0x80000000u;
}

/* --- Low-level block ops -------------------------------------------------- */

static int erase_phys_block(uint32_t pblock)
{
    unsigned int bank;
    uint32_t block;
    int rc;

    phys_block_to_bank_block(pblock, &bank, &block);
    rc = nand_hw_erase_block(bank, block);
    if (rc == 0)
        block_erasectr[pblock]++;
    return rc;
}

/* Stamp page 0 of a block with the bad-block sentinel, best effort. A block
 * that failed erase/program is excluded from the free queue regardless; this
 * just persists the fact across a remount when it can. */
static void mark_block_bad(uint32_t pblock)
{
    unsigned int bank;
    uint32_t block;
    struct ftl2_page_header hdr;

    block_state[pblock] = BST_BAD;
    block_valid[pblock] = 0;

    phys_block_to_bank_block(pblock, &bank, &block);
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = FTL2_MAGIC_BAD;
    memset(io_spare, 0xFF, sizeof(io_spare));
    memcpy(io_spare, &hdr, sizeof(hdr));
    memset(io_data, 0xFF, sizeof(io_data));
    nand_hw_write_page(bank, block * pages_per_block, io_data, io_spare);
}

static void free_queue_push(uint32_t pblock)
{
    if (free_count < FTL_MAX_PHYSICAL_BLOCKS)
    {
        block_state[pblock] = BST_FREE;
        free_queue[free_count++] = pblock;
    }
}

/* Pick and remove the least-erased block from the free queue (wear
 * levelling). Returns BLOCK_NONE if the queue is empty. */
static uint32_t free_queue_pop_least_worn(void)
{
    unsigned int i, best_i = 0;
    uint32_t best_ec = 0xFFFFFFFFu;

    if (free_count == 0)
        return BLOCK_NONE;

    for (i = 0; i < free_count; i++)
    {
        uint32_t pb = free_queue[i];
        if (block_erasectr[pb] < best_ec)
        {
            best_ec = block_erasectr[pb];
            best_i = i;
        }
    }

    uint32_t chosen = free_queue[best_i];
    free_queue[best_i] = free_queue[--free_count];
    return chosen;
}

/* Forward declarations for the write/GC interdependency. */
static int gc_reclaim_one(void);
static int ensure_free_blocks(unsigned int want);

/* Open a fresh frontier block from the free queue, erasing it defensively.
 * Returns 0 on success, -1 if no usable free block could be opened. */
static int open_frontier(void)
{
    for (;;)
    {
        uint32_t pb = free_queue_pop_least_worn();
        if (pb == BLOCK_NONE)
            return -1;

        /* Free-queue invariant is "erased", but erase defensively in case a
         * block was released after a failed program without a re-erase; a
         * redundant erase of an already-erased block is harmless. */
        if (erase_phys_block(pb) != 0)
        {
            mark_block_bad(pb);
            continue; /* try the next free block */
        }

        block_state[pb] = BST_OPEN;
        block_valid[pb] = 0;
        frontier_block = pb;
        frontier_page = 0;
        /* Reset the summary record for the new block. */
        for (unsigned int i = 0; i < data_pages_per_block; i++)
        {
            frontier_entries[i].lpn = PAGE_UNMAPPED;
            frontier_entries[i].seq = 0;
        }
        return 0;
    }
}

/* Write the SUMMARY page (the block's last page) recording every data page's
 * (lpn, seq), then mark the block CLOSED. Best-effort: if the summary program
 * fails, the block is still CLOSED and fully correct -- mount will just fall
 * back to full-scanning it (slower, but the data pages are self-describing).
 * A summary program failure does NOT make the block bad, since all its data
 * pages already programmed fine. */
static void write_summary_and_close(void)
{
    unsigned int bank; uint32_t block;
    struct ftl2_page_header hdr;
    uint32_t highest_seq = 0;
    unsigned int i;

    if (frontier_block == BLOCK_NONE)
        return;

    phys_block_to_bank_block(frontier_block, &bank, &block);

    /* Build the summary in io_data: the (lpn,seq) array for every data page
     * slot, padded with PAGE_UNMAPPED for any unused tail slots. */
    memset(io_data, 0xFF, chip_geo->page_size);
    struct ftl2_summary_entry *ents = (struct ftl2_summary_entry *)io_data;
    for (i = 0; i < data_pages_per_block; i++)
    {
        ents[i] = frontier_entries[i];   /* lpn/seq, or {UNMAPPED,..} if unused */
        if (frontier_entries[i].lpn != PAGE_UNMAPPED &&
            seq_newer(frontier_entries[i].seq, highest_seq))
            highest_seq = frontier_entries[i].seq;
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = FTL2_MAGIC;
    hdr.type = FTL2_TYPE_SUMMARY;
    hdr.lpn = PAGE_UNMAPPED;         /* summary is not itself a logical page */
    hdr.seq = highest_seq;           /* block's freshest seq, for mount      */
    memset(io_spare, 0xFF, sizeof(io_spare));
    memcpy(io_spare, &hdr, sizeof(hdr));

    /* Summary occupies the reserved last page (summary_page_index). */
    nand_hw_write_page(bank, block * pages_per_block + summary_page_index,
                       io_data, io_spare);

    block_state[frontier_block] = BST_CLOSED;
    frontier_block = BLOCK_NONE;
}

/* --- The write path ------------------------------------------------------- */

/* Program one logical page's worth of data (chip_geo->page_size bytes, i.e.
 * sectors_per_page NAND_PAGE_SIZE sectors) to the frontier, updating the map
 * and staleness bookkeeping. `type` distinguishes host writes from GC copies
 * (informational, for diagnostics).
 *
 * `from_gc` is true when this is a GC relocation. In that case we must NOT
 * trigger garbage collection to obtain a fresh frontier block (that would
 * re-enter GC and corrupt the shared gc_* scratch, and could pick the very
 * victim currently being drained). GC guarantees a free block is available
 * for the frontier before it starts relocating (see gc_reclaim_one), so a GC
 * relocation only ever needs to pop the free queue, never to reclaim.
 *
 * Returns 0 or -1. */
static int program_logical_page(uint32_t lpn, const uint8_t *page_bytes,
                                uint8_t type, bool from_gc)
{
    struct ftl2_page_header hdr;

    /* Make sure there is an open frontier with a free DATA slot. The last
     * page of every block is reserved for the SUMMARY, so the frontier is
     * "full" once frontier_page reaches data_pages_per_block. */
    if (frontier_block == BLOCK_NONE || frontier_page >= data_pages_per_block)
    {
        /* Close the current frontier (writing its summary) and open a new
         * one. */
        if (frontier_block != BLOCK_NONE)
            write_summary_and_close();

        /* Host writes may reclaim to make room; GC relocations must not
         * re-enter GC and instead rely on the free block GC reserved. */
        if (!from_gc)
        {
            if (ensure_free_blocks(1) != 0)
                return -1;
        }
        if (open_frontier() != 0)
            return -1;
    }

    unsigned int bank;
    uint32_t block, hw_page;
    phys_block_to_bank_block(frontier_block, &bank, &block);
    /* Hardware address is (bank, bank-relative page). */
    hw_page = block * pages_per_block + frontier_page;
    /* The stored PPN is bank-ABSOLUTE (phys_block_first_ppn already encodes
     * the bank via the flat block index), so ppn_to_bank_page() round-trips
     * it. Storing the bank-relative page here was a real bug: a frontier in a
     * higher bank read back from the wrong bank. */
    uint32_t this_ppn = phys_block_first_ppn(frontier_block) + frontier_page;

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = FTL2_MAGIC;
    hdr.type = type;
    hdr.lpn = lpn;
    hdr.seq = next_seq;

    memset(io_spare, 0xFF, sizeof(io_spare));
    memcpy(io_spare, &hdr, sizeof(hdr));

    int wrc = nand_hw_write_page(bank, hw_page, page_bytes, io_spare);
    if (wrc != 0)
    {
        /* Program failure: this block is bad. Mark it, drop what we mapped
         * into it (nothing committed for this lpn yet), and retry on a fresh
         * frontier. Live pages already written to this block earlier are lost
         * only if this was mid-block -- but a program failure mid-block is a
         * genuine hardware bad block; those earlier pages' *previous* copies
         * (lower seq, elsewhere) still exist and win at mount, because we
         * never erased them. */
        mark_block_bad(frontier_block);
        frontier_block = BLOCK_NONE;
        return -1;
    }

    /* Commit the mapping. Decrement the old block's valid count first. */
    uint32_t old_ppn = (lpn < num_logical_pages) ? page_map[lpn] : PAGE_UNMAPPED;
    if (old_ppn != PAGE_UNMAPPED)
    {
        uint32_t old_block = ppn_to_phys_block(old_ppn);
        if (block_valid[old_block] > 0)
            block_valid[old_block]--;
    }

    page_map[lpn] = this_ppn;
    block_valid[frontier_block]++;

    /* Record this data page for the block's summary (written at close). */
    frontier_entries[frontier_page].lpn = lpn;
    frontier_entries[frontier_page].seq = next_seq;

    frontier_page++;
    next_seq++;

    return 0;
}

int ftl_write(uint32_t sector, uint32_t count, const void *buffer)
{
    const uint8_t *in = (const uint8_t *)buffer;

    if (!mounted)
    {
        nand_debug_log("ftl2_write: not mounted");
        return -1;
    }
    if (readonly_mount)
    {
        nand_debug_log("ftl2_write: readonly mount");
        return -1;
    }

    /* First write since a sync: clear the clean marker. (The marker itself is
     * laid down lazily at sync; here we just note we're now dirty.) */
    clean_flag = false;

    /* The host addresses NAND_PAGE_SIZE (2048B) sectors; a logical page holds
     * sectors_per_page of them. We only ever get whole-page-aligned runs from
     * the storage layer in practice, but handle partial leading/trailing
     * sectors correctly by read-merging the affected logical page. */
    while (count > 0)
    {
        uint32_t lpn = sector / sectors_per_page;
        unsigned int sub = sector % sectors_per_page;      /* sector within page */
        unsigned int run = sectors_per_page - sub;         /* sectors to end of page */
        if (run > count)
            run = count;

        if (lpn >= num_logical_pages)
        {
            nand_debug_log("ftl2_write: lpn %lu >= %lu",
                           (unsigned long)lpn, (unsigned long)num_logical_pages);
            write_error_latched = true;
            return -1;
        }

        if (run == sectors_per_page)
        {
            /* Whole logical page written in one go: no read-merge needed. */
            if (program_logical_page(lpn, in, FTL2_TYPE_DATA, false) != 0)
            {
                write_error_latched = true;
                return -1;
            }
        }
        else
        {
            /* Partial page: read the current contents (or 0xFF if unmapped)
             * into io_data, overlay the new sectors, then program the whole
             * page. */
            uint32_t cur = page_map[lpn];
            if (cur == PAGE_UNMAPPED)
            {
                memset(io_data, 0xFF, chip_geo->page_size);
            }
            else
            {
                unsigned int bank; uint32_t page;
                ppn_to_bank_page(cur, &bank, &page);
                int rc = nand_hw_read_page(bank, page, io_data, NULL);
                if (nand_read_page_untrusted(rc))
                    memset(io_data, 0xFF, chip_geo->page_size);
            }
            memcpy(io_data + (size_t)sub * NAND_PAGE_SIZE, in,
                   (size_t)run * NAND_PAGE_SIZE);
            if (program_logical_page(lpn, io_data, FTL2_TYPE_DATA, false) != 0)
            {
                write_error_latched = true;
                return -1;
            }
        }

        sector += run;
        count  -= run;
        in     += (size_t)run * NAND_PAGE_SIZE;

        /* Opportunistic GC if we've dipped low on free blocks. Bounded: at
         * most one reclaim per write step, so a single ftl_write can't stall
         * unboundedly. */
        if (free_count <= spare_pool_target / 2 &&
            free_count > 0 /* still room to relocate into */)
        {
            gc_reclaim_one();
        }
    }

    return 0;
}

/* --- Read ----------------------------------------------------------------- */

static int read_logical_page(uint32_t lpn, uint8_t *page_out)
{
    uint32_t ppn;
    unsigned int bank;
    uint32_t page;
    int rc;

    if (lpn >= num_logical_pages)
        return -1;

    ppn = page_map[lpn];
    if (ppn == PAGE_UNMAPPED)
    {
        memset(page_out, 0xFF, chip_geo->page_size);
        return 0;
    }

    ppn_to_bank_page(ppn, &bank, &page);
    rc = nand_hw_read_page(bank, page, page_out, NULL);
    if (nand_read_page_untrusted(rc))
        return rc < 0 ? rc : -1;
    return 0;
}

int ftl_read(uint32_t sector, uint32_t count, void *buffer)
{
    uint8_t *out = (uint8_t *)buffer;

    if (!mounted)
        return -1;

    while (count > 0)
    {
        uint32_t lpn = sector / sectors_per_page;
        unsigned int sub = sector % sectors_per_page;
        unsigned int run = sectors_per_page - sub;
        if (run > count)
            run = count;

        if (run == sectors_per_page)
        {
            /* Whole page straight into the caller's buffer. */
            int rc = read_logical_page(lpn, out);
            if (rc < 0)
                return rc;
        }
        else
        {
            int rc = read_logical_page(lpn, io_data);
            if (rc < 0)
                return rc;
            memcpy(out, io_data + (size_t)sub * NAND_PAGE_SIZE,
                   (size_t)run * NAND_PAGE_SIZE);
        }

        sector += run;
        count  -= run;
        out    += (size_t)run * NAND_PAGE_SIZE;
    }
    return 0;
}

/* --- Garbage collection --------------------------------------------------- */

/* Select a GC victim: the CLOSED block with the fewest valid pages (most to
 * reclaim), tie-broken toward the lowest erase count. Never the frontier, and
 * never a fully-valid block (nothing to gain). Returns BLOCK_NONE if there is
 * no worthwhile victim. */
static uint32_t gc_pick_victim(void)
{
    uint32_t best = BLOCK_NONE;
    uint16_t best_valid = 0xFFFF;
    uint32_t best_ec = 0xFFFFFFFFu;
    uint32_t pb;

    for (pb = 0; pb < total_physical_blocks; pb++)
    {
        if (block_state[pb] != BST_CLOSED)
            continue;
        if (block_valid[pb] >= pages_per_block)
            continue; /* fully valid: reclaiming it gains nothing */

        if (block_valid[pb] < best_valid ||
            (block_valid[pb] == best_valid && block_erasectr[pb] < best_ec))
        {
            best = pb;
            best_valid = block_valid[pb];
            best_ec = block_erasectr[pb];
        }
    }
    return best;
}

/* Reclaim exactly one victim block: relocate its live pages to the frontier,
 * then erase it and return it to the free queue. Returns 0 if a block was
 * reclaimed, -1 if there was nothing to reclaim or reclaim failed.
 *
 * Crash-safety ordering is the crux: each live page is copied (with a fresh,
 * higher seq) and the map repointed BEFORE the victim is erased. If power is
 * lost after some copies, the relocated pages win by seq; a torn copy is
 * ignored and the still-un-erased original wins. The victim is erased only
 * after every live page is confirmed relocated. */
static bool gc_in_progress; /* re-entry guard, defense in depth */

static int gc_reclaim_one(void)
{
    uint32_t victim;
    unsigned int bank;
    uint32_t block, first, p;

    /* Hard guard: GC must never re-enter itself. The relocation path below
     * calls program_logical_page(from_gc=true), which is written not to call
     * back into GC -- but this guard makes the invariant impossible to break
     * accidentally in future edits, and keeps the shared gc_* scratch safe. */
    if (gc_in_progress)
        return -1;

    victim = gc_pick_victim();
    if (victim == BLOCK_NONE)
        return -1;

    /* A GC relocation may need to open a fresh frontier block, and it must be
     * able to do so WITHOUT reclaiming (which would re-enter GC / re-pick this
     * victim). Guarantee at least one free block is available for that before
     * we start draining the victim. Worst case, relocating a full victim needs
     * one new frontier block; the victim itself becomes free at the end, so
     * one reserved free block is sufficient. If none is available and the
     * current frontier has no room, we cannot safely proceed. */
    if (free_count == 0 &&
        (frontier_block == BLOCK_NONE || frontier_page >= pages_per_block))
        return -1;

    gc_in_progress = true;

    phys_block_to_bank_block(victim, &bank, &block);
    first = block * pages_per_block;

    for (p = 0; p < pages_per_block; p++)
    {
        struct ftl2_page_header hdr;
        int rc = nand_hw_read_page(bank, first + p, gc_data, gc_spare);
        if (nand_read_page_untrusted(rc))
            continue; /* unreadable page: not a live mapping we can trust */

        memcpy(&hdr, gc_spare, sizeof(hdr));
        if (hdr.magic != FTL2_MAGIC)
            continue; /* blank, bad, or foreign: nothing to relocate */
        if (hdr.lpn >= num_logical_pages)
            continue;

        /* Is this page still the live copy of its lpn? Only relocate if the
         * map still points here; a superseded page is stale by definition. */
        uint32_t this_ppn = phys_block_first_ppn(victim) + p;
        if (page_map[hdr.lpn] != this_ppn)
            continue;

        /* Relocate with a fresh seq (from_gc=true: never re-enters GC). */
        if (program_logical_page(hdr.lpn, gc_data, FTL2_TYPE_GC, true) != 0)
        {
            gc_in_progress = false;
            return -1; /* frontier program failed; leave victim intact */
        }
    }

    /* Every live page relocated (block_valid should now be 0). Safe to erase.
     * If the erase fails, the block is bad: mark it and drop it (do NOT return
     * it to the free queue). */
    if (block_valid[victim] != 0)
    {
        /* Defensive: something still claims to be live here. Do not erase --
         * leaving it CLOSED is safe (its pages are still readable). */
        nand_debug_log("ftl2_gc: victim %lu still has %u valid, not erasing",
                       (unsigned long)victim, block_valid[victim]);
        gc_in_progress = false;
        return -1;
    }

    if (erase_phys_block(victim) != 0)
    {
        mark_block_bad(victim);
        gc_in_progress = false;
        return 0; /* still counts as progress: a bad block left the CLOSED set */
    }

    free_queue_push(victim);
    gc_in_progress = false;
    return 0;
}

/* Ensure at least `want` free blocks exist, running GC as needed. Returns 0 if
 * satisfied, -1 if GC can make no further progress (device genuinely full). */
static int ensure_free_blocks(unsigned int want)
{
    unsigned int guard = total_physical_blocks + 4; /* bound the loop hard */

    while (free_count < want)
    {
        if (gc_reclaim_one() != 0)
            return -1; /* nothing left to reclaim */
        if (guard-- == 0)
            return -1;
    }
    return 0;
}

/* --- Sync ----------------------------------------------------------------- */

int ftl_sync(void)
{
    /* A completed ftl_write() has already programmed its data pages and
     * updated the map -- there is no RAM-only write buffer to flush. What
     * sync does that matters is close out the open frontier block with its
     * SUMMARY page, so that after a clean unmount EVERY block is summarised
     * and the next mount is fully fast (2 reads/block). Without this, the one
     * still-open block would force a full per-page scan of itself at the next
     * mount -- correct, but slower for that block.
     *
     * We only summarise-and-close if the frontier has data in it; an empty
     * frontier is left open (nothing to summarise), and we reopen lazily on
     * the next write. Best-effort: a failed summary program is harmless
     * (mount falls back to scanning that block). */
    if (write_error_latched)
        return -1;

    if (frontier_block != BLOCK_NONE && frontier_page > 0 && !readonly_mount)
    {
        write_summary_and_close();
        /* Reopen a fresh frontier so subsequent writes have somewhere to go,
         * if a free block is available; otherwise the next write opens one. */
        if (free_count > 0)
            open_frontier();
    }

    return 0;
}

/* --- Mount (scan-based reconstruction) ------------------------------------ */

/* Read the v2 seq stored in a page's spare header, for mount-time collision
 * resolution. Returns true and *seq_out on a valid v2 data/GC page; false if
 * the page is unreadable or not a v2 page (caller then prefers the other
 * candidate). One page read; only called on a genuine collision. */
static bool read_ppn_seq(uint32_t ppn, uint32_t *seq_out)
{
    unsigned int bank; uint32_t page;
    struct ftl2_page_header hdr;
    /* Uses the mount-only io_* scratch (scan_and_mount is the sole caller
     * path); safe because mount is single-threaded like the rest of the FTL. */
    ppn_to_bank_page(ppn, &bank, &page);
    int rc = nand_hw_read_page(bank, page, io_data, io_spare);
    if (nand_read_page_untrusted(rc))
        return false;
    memcpy(&hdr, io_spare, sizeof(hdr));
    if (hdr.magic != FTL2_MAGIC)
        return false;
    *seq_out = hdr.seq;
    return true;
}

/* Fold one candidate (lpn at ppn, with seq) into the map being rebuilt.
 * Keeps the highest-seq copy per lpn and maintains block_valid[] for both the
 * winning and displaced blocks, mirroring the runtime write path's accounting.
 * pb is the physical block ppn lives in (passed to avoid recomputing).
 *
 * Collision resolution re-reads the incumbent page's header to compare seq,
 * rather than caching every lpn's seq in RAM. That trades a permanent
 * per-logical-page seq array (8 MiB on this chip -- half the FTL's entire
 * footprint, and needed only during mount) for one extra page read per
 * genuine collision. A collision happens only when the same lpn has more than
 * one surviving copy across blocks, i.e. it was rewritten and both the old
 * and new physical blocks are still present at mount. GC keeps that count
 * bounded, so mount stays ~per-block; the saving matters because it lets the
 * whole FTL fit the main-firmware RAM budget alongside the audio buffer. */
static void fold_candidate(uint32_t lpn, uint32_t seq, uint32_t ppn,
                           uint32_t pb)
{
    uint32_t cur = page_map[lpn];
    if (cur == PAGE_UNMAPPED)
    {
        page_map[lpn] = ppn;
        block_valid[pb]++;
        return;
    }

    /* Collision: fetch the incumbent's seq. If it can't be read or isn't a
     * valid v2 page any more, prefer this candidate. */
    uint32_t cur_seq;
    bool take;
    if (!read_ppn_seq(cur, &cur_seq))
        take = true;
    else
        take = seq_newer(seq, cur_seq);

    if (take)
    {
        uint32_t old_block = ppn_to_phys_block(cur);
        if (block_valid[old_block] > 0)
            block_valid[old_block]--;
        page_map[lpn] = ppn;
        block_valid[pb]++;
    }
    /* else: incumbent is newer; this candidate is stale, ignore it. */
}

/* Classify and index every block, rebuilding page_map by highest seq per lpn.
 * Fast path uses each block's SUMMARY page (2 reads/block); falls back to a
 * per-page scan only for a block without a usable summary. Correctness rests
 * on the self-describing pages either way; the summary is an optimisation. */
static void scan_and_mount(void)
{
    uint32_t pb;
    uint32_t p;
    uint32_t max_seq = 0;
    bool any_seq = false;
    uint32_t foreign_blocks = 0; /* blocks holding non-v2 (foreign) content */

    /* Reset RAM state. */
    for (p = 0; p < num_logical_pages; p++)
        page_map[p] = PAGE_UNMAPPED;
    for (pb = 0; pb < total_physical_blocks; pb++)
    {
        block_valid[pb] = 0;
        block_erasectr[pb] = 0;
        block_state[pb] = BST_FREE;   /* provisional; refined below */
    }
    free_count = 0;
    frontier_block = BLOCK_NONE;
    frontier_page = 0;

    /* Single pass over every page. We maintain block_valid[] incrementally as
     * the map is (re)pointed -- exactly as the runtime write path does -- so
     * no second read pass is needed to count live pages. When a candidate page
     * wins an lpn from an incumbent, the incumbent's block loses a live page
     * and the winner's block gains one. This reads each page once (plus a
     * rare re-read of an incumbent header on a genuine seq collision). Mount
     * is still a full-device scan; cutting that to a checkpoint + tail replay
     * is the v2.1 optimisation. */
    for (pb = 0; pb < total_physical_blocks; pb++)
    {
        unsigned int bank; uint32_t block, first;
        struct ftl2_page_header hdr;
        bool block_has_content = false;

        phys_block_to_bank_block(pb, &bank, &block);
        first = block * pages_per_block;

        /* Page 0 first: cheapest classification (bad / erased / in-use). */
        int rc0 = nand_hw_read_page(bank, first, io_data, io_spare);
        if (rc0 < 0)
        {
            /* Hardware timeout on page 0: treat the block as bad. */
            block_state[pb] = BST_BAD;
            continue;
        }
        if (rc0 == NAND_ECC_CLEAN)
        {
            /* Page 0 is erased -> the whole block is unwritten and free.
             * Skip it with a SINGLE read: this is what keeps a blank or
             * mostly-empty device's mount cheap. A block that has any data
             * always has its page 0 written first, so an erased page 0 means
             * an erased block. */
            block_state[pb] = BST_FREE;
            continue;
        }
        if (nand_read_page_untrusted(rc0))
        {
            /* Readable but uncorrectable: don't trust it, treat as bad. */
            block_state[pb] = BST_BAD;
            continue;
        }
        memcpy(&hdr, io_spare, sizeof(hdr));
        if (hdr.magic == FTL2_MAGIC_BAD)
        {
            block_state[pb] = BST_BAD;
            continue;
        }
        if (hdr.magic != FTL2_MAGIC)
        {
            /* Page 0 is readable and non-erased but is NOT one of our pages
             * (magic is neither FTL2_MAGIC nor the bad sentinel). This is
             * FOREIGN content -- e.g. a chip still holding the previous FTL's
             * on-flash format, or an Apple-formatted region. It is not part of
             * a v2 volume, so treat the block as free/ignored with a SINGLE
             * read rather than full-scanning all its pages hunting for v2 data
             * that isn't there.
             *
             * This is what bounds first-mount over foreign data: without it,
             * every foreign block triggered a 128-page fallback scan -- 2.1M
             * reads total, a multi-minute hang confirmed on real hardware when
             * mounting v2 over the old v1 format. The host reformats the
             * volume anyway on a fresh install; a foreign block carries no v2
             * mapping to recover. foreign_blocks is counted so we can note a
             * wholly-foreign device in the log. */
            foreign_blocks++;
            block_state[pb] = BST_FREE;
            continue;
        }

        /* FAST PATH: try the block's SUMMARY page (its last page). If it is a
         * valid summary, it lists every data page's (lpn, seq) so we can fold
         * the whole block into the map with just this one extra read -- no
         * per-page scan. This is what turns a ~2.1M-read mount into ~32k. */
        bool summarised = false;
        {
            int src = nand_hw_read_page(bank, first + summary_page_index,
                                        gc_data, gc_spare);
            if (!nand_read_page_untrusted(src))
            {
                struct ftl2_page_header shdr;
                memcpy(&shdr, gc_spare, sizeof(shdr));
                if (shdr.magic == FTL2_MAGIC && shdr.type == FTL2_TYPE_SUMMARY)
                {
                    const struct ftl2_summary_entry *ents =
                        (const struct ftl2_summary_entry *)gc_data;
                    unsigned int e;
                    for (e = 0; e < data_pages_per_block; e++)
                    {
                        uint32_t elpn = ents[e].lpn;
                        uint32_t eseq = ents[e].seq;
                        if (elpn == PAGE_UNMAPPED || elpn >= num_logical_pages)
                            continue;
                        block_has_content = true;
                        if (!any_seq || seq_newer(eseq, max_seq))
                        { max_seq = eseq; any_seq = true; }
                        fold_candidate(elpn, eseq,
                                       phys_block_first_ppn(pb) + e, pb);
                    }
                    /* The summary's own seq also counts toward the global. */
                    if (!any_seq || seq_newer(shdr.seq, max_seq))
                    { max_seq = shdr.seq; any_seq = true; }
                    summarised = true;
                }
            }
        }

        /* SLOW PATH: no usable summary (open block at crash, or torn summary).
         * Fall back to reading every data page's own header -- still fully
         * correct, just this one block pays the per-page cost. */
        if (!summarised)
        {
            for (p = 0; p < data_pages_per_block; p++)
            {
                int rc = nand_hw_read_page(bank, first + p, io_data, io_spare);
                if (nand_read_page_untrusted(rc))
                    continue;

                memcpy(&hdr, io_spare, sizeof(hdr));
                if (hdr.magic != FTL2_MAGIC)
                    continue;
                block_has_content = true;
                if (!any_seq || seq_newer(hdr.seq, max_seq))
                { max_seq = hdr.seq; any_seq = true; }
                if (hdr.lpn == PAGE_UNMAPPED || hdr.lpn >= num_logical_pages)
                    continue;
                fold_candidate(hdr.lpn, hdr.seq,
                               phys_block_first_ppn(pb) + p, pb);
            }
        }

        block_state[pb] = block_has_content ? BST_CLOSED : BST_FREE;
    }

    /* Enqueue free blocks, and reclaim any CLOSED block that ended up with no
     * live pages (pure garbage) straight into the free pool. */
    for (pb = 0; pb < total_physical_blocks; pb++)
    {
        if (block_state[pb] == BST_BAD)
            continue;
        if (block_state[pb] == BST_FREE)
        {
            free_queue_push(pb);
            continue;
        }
        if (block_valid[pb] == 0) /* CLOSED but fully stale */
        {
            if (erase_phys_block(pb) == 0)
                free_queue_push(pb);
            else
                mark_block_bad(pb);
        }
    }

    /* Establish the running sequence counter. */
    next_seq = any_seq ? (max_seq + 1) : 1;

    if (foreign_blocks > 0)
        nand_debug_log("ftl2: %lu foreign blocks (non-v2 format) treated free",
                       (unsigned long)foreign_blocks);

    /* Open a frontier so the first write has somewhere to go. If none can be
     * opened now (no free blocks), the write path will try GC on demand. */
    if (free_count > 0)
        open_frontier();
}

/* --- Public init ---------------------------------------------------------- */

int ftl_init(void)
{
    mounted = false;
    write_error_latched = false;
    clean_flag = false;
    frontier_block = BLOCK_NONE;
    frontier_page = 0;

    bank_count = nand_get_bank_count();
    if (bank_count == 0)
        return FTL_ERR_NO_BANKS;

    chip_geo = nand_get_bank_geometry(0);
    if (!chip_geo || !chip_geo->recognized)
        return FTL_ERR_UNRECOGNIZED;

    blocks_per_bank = chip_geo->blocks_per_bank;
    pages_per_block = chip_geo->pages_per_block;
    sectors_per_page = chip_geo->page_size / NAND_PAGE_SIZE;
    if (sectors_per_page == 0)
        sectors_per_page = 1;

    /* Reserve the last page of each block for the summary. Need at least 2
     * pages/block for that to leave any data capacity. */
    if (pages_per_block < 2)
        return FTL_ERR_TOO_SMALL;
    summary_page_index = pages_per_block - 1;
    data_pages_per_block = pages_per_block - 1;

    /* The summary must fit the per-data-page (lpn,seq) array in one page. */
    if ((size_t)data_pages_per_block * sizeof(struct ftl2_summary_entry)
            > chip_geo->page_size)
        return FTL_ERR_TOO_SMALL;

    /* The on-flash page_index field caps pages/block at 256; the statically
     * sized RAM map caps it at the 128 real N46 parts use. Refuse to mount a
     * chip past either bound rather than overrun the map (FTL_ERR_TOO_SMALL is
     * the "geometry outside our static limits" code). */
    if (pages_per_block > FTL_MAX_PAGES_PER_BLOCK ||
        pages_per_block > FTL_MAX_PAGES_PER_BLOCK_HW)
        return FTL_ERR_TOO_SMALL;
    if (blocks_per_bank > FTL_MAX_BLOCKS_PER_BANK)
        return FTL_ERR_TOO_SMALL;

    total_physical_blocks = bank_count * blocks_per_bank;
    if (total_physical_blocks > FTL_MAX_PHYSICAL_BLOCKS)
        total_physical_blocks = FTL_MAX_PHYSICAL_BLOCKS;
    total_physical_pages = (uint32_t)total_physical_blocks * pages_per_block;

    spare_pool_target = total_physical_blocks >> FTL_SPARE_FRACTION_SHIFT;
    if (spare_pool_target < FTL_SPARE_MIN_BLOCKS)
        spare_pool_target = FTL_SPARE_MIN_BLOCKS;

    if (total_physical_blocks <= spare_pool_target + 1)
        return FTL_ERR_TOO_SMALL;

    usable_blocks = total_physical_blocks - spare_pool_target;
    /* One page per block is the summary, so a block backs data_pages_per_block
     * logical pages, not pages_per_block. */
    num_logical_pages = (uint32_t)usable_blocks * data_pages_per_block;

    if (num_logical_pages > FTL_MAX_LOGICAL_PAGES)
        return FTL_ERR_TOO_SMALL;

    scan_and_mount();

#ifdef FTL_READONLY
    readonly_mount = true;
#else
    readonly_mount = !chip_geo->recognized;
#endif

    mounted = true;
    return 0;
}

uint32_t ftl_num_sectors(void)
{
    if (!mounted)
        return 0;
    return num_logical_pages * sectors_per_page;
}

bool ftl_readonly_mount(void)
{
    return readonly_mount;
}
