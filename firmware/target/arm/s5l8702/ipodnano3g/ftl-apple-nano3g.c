/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") Apple-compatible FTL/VFL -- SECOND, OPTIONAL pipeline.
 *
 * Built only when FTL_APPLE_COMPAT is defined. The default and proven
 * pipeline is the project's own page-level log-structured FTL
 * (ftl-nano3g-v2.c); this file does not replace it and is not built unless
 * the flag is set. See port-docs/APPLE_FTL_PORT_PLAN.md.
 *
 * GOAL: read (and eventually write) Apple's own on-flash format ("Whimory"
 * VFL + FTL) so the Nano 3G can share the NAND with Apple's OS, and so
 * geometry comes from Apple's stored context rather than per-chip
 * validation.
 *
 * STATUS: Phase 1 -- read-only VFL mount scaffolding. The write path is a
 * stub (returns an error) until the read path is proven on hardware.
 *
 * PROVENANCE: this is a port of the in-tree, GPL Nano 2G Apple FTL
 * (firmware/target/arm/s5l8700/ipodnano2g/ftl-nano2g.c), which is itself a
 * reimplementation of Apple's Whimory whose on-flash structs match the OFW.
 * The 2G reference is cross-checked here against this project's own decode
 * of the 3G Apple firmware in utils/ipodnano3g/decode/APPLE-FTL-READ.md and
 * APPLE-FTL-WRITE.md. Differences from the 2G that this port must handle
 * (page size runtime 2KB/4KB, 12-byte HW-ECC spare metadata via FMSYND5..7,
 * 4 banks, the s5l8702 FMC low-level API) are documented in the port plan.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 ****************************************************************************/

#include "config.h"
#include "system.h"
#include "string.h"
#include "nand-target.h"
#include "ftl-target.h"

/* --- Spare/OOB metadata (Apple layout, within the 12 bytes this
 * controller surfaces) ------------------------------------------------
 *
 * CRITICAL FEASIBILITY POINT (see port plan "ECC ownership"): our low-level
 * driver (nand-nano3g.c) surfaces exactly NAND_SPARE_META_BYTES (12) bytes
 * of per-page metadata, read from FMSYND5/6/7 through the controller's
 * hardware-ECC path. APPLE-FTL-READ.md states Apple's own FMSS read program
 * (0x22009030) "decodes and stores FMSYND5..7 as the 12-byte spare" -- the
 * SAME three registers. So Apple's 12-byte spare header should arrive in the
 * spare_out buffer of nand_hw_read_page() already, without porting Apple's
 * sequencer.
 *
 * The exact byte ORDER within those 12 bytes (does Apple's type byte land at
 * offset 9 as the 2G layout has it?) is the one thing only hardware can
 * confirm -- Phase 3 reads a real Apple page and checks the type byte lands
 * where this struct expects it. Until then this mirrors the 2G's user/meta
 * layout, truncated to 12 bytes (the ECC fields at 2G offset 0xC.. do not
 * exist here -- the 3G controller owns ECC, we never see parity bytes). */

#define APPLE_SPARE_BYTES  NAND_SPARE_META_BYTES  /* 12 */

/* Page "type" byte values, from the 2G reference and APPLE-FTL docs. */
#define APPLE_TYPE_DATA        0x40  /* user data page                     */
#define APPLE_TYPE_DATA_LAST   0x41  /* last data page of a hyperblock      */
#define APPLE_TYPE_FTLCXT      0x43  /* FTL context page                    */
#define APPLE_TYPE_MAP         0x44  /* block-map page                      */
#define APPLE_TYPE_ERASECTR    0x46  /* erase-counter page                  */
#define APPLE_TYPE_MOUNTED     0x47  /* "FTL mounted"/unclean-shutdown mark */
#define APPLE_TYPE_VFLCXT      0x80  /* VFL context page                    */
#define APPLE_TYPE_ERASED      0xFF  /* erased/blank page                   */

/* User-data spare header (types 0x40/0x41). Only the first 12 bytes exist
 * on the 3G; the 2G's ECC fields beyond offset 0xB are the controller's
 * job here and are never seen by software. */
struct apple_spare_user
{
    uint32_t lpn;       /* offset 0: logical page (sector) number */
    uint32_t usn;       /* offset 4: update seq (freshness)       */
    uint8_t  field_8;   /* offset 8 */
    uint8_t  type;      /* offset 9: APPLE_TYPE_DATA / _DATA_LAST  */
    uint8_t  eccmark;   /* offset 0xA: 0xFF ok, 0x55 prior err     */
    uint8_t  field_B;   /* offset 0xB */
} __attribute__((packed));

/* Meta spare header (types 0x43/0x44/0x46/0x47/0x80). */
struct apple_spare_meta
{
    uint32_t usn;       /* offset 0: ftl_cxt.usn or vfl updatecount */
    uint16_t idx;       /* offset 4: index of the map/erasectr page  */
    uint8_t  field_6;
    uint8_t  field_7;
    uint8_t  field_8;
    uint8_t  type;      /* offset 9: APPLE_TYPE_*                     */
    uint8_t  eccmark;   /* offset 0xA */
    uint8_t  field_B;   /* offset 0xB */
} __attribute__((packed));

_Static_assert(sizeof(struct apple_spare_user) == 12, "user spare header is 12 bytes");
_Static_assert(sizeof(struct apple_spare_meta) == 12, "meta spare header is 12 bytes");

/* --- Device-info / VFL context on-flash structures -------------------
 *
 * Ported field-for-field from the 2G reference. Sizes marked (2G) are the
 * 2G's values and are flagged in the port plan as "confirm on 3G": the 3G
 * VFL context may differ. Phase 1 reads a real 3G VFL context page and
 * checks its checksum validates against these offsets before trusting them.
 */

/* Device-info page signature. The 2G uses "DEVICEINFOSIGN"; hardware scan of
 * a stock 3G (Micronas [JEDEC 0xEC] MB253) showed the 3G devinfo page begins
 * "DEVICEINFOBBT" (bytes 44 45 56 49 43 45 49 4E 46 4F 42 42 54). We match on
 * the common "DEVICEINFO" prefix so both suffixes are accepted, then handle
 * the 3G's BBT layout below. Confirmed on hardware 2026-09-19. */
#define APPLE_DEVICEINFO_PREFIX     "DEVICEINFO"     /* 10 bytes, no NUL */
#define APPLE_DEVICEINFO_PREFIX_LEN 10
/* Device-info is intentionally not used by the live Apple mount. VFL
 * contexts are located directly in physical blocks 1..199 and validated by
 * strict type/field/checksum rules. */

/* Per-bank VFL context (0x800 bytes). This layout is the dump-derived
 * Nano 3G one (remap at 0x2c, ring at 0x694, checksums at 0x7f8) -- CONFIRMED
 * on stock hardware 2026-09-20: the strict all-bank locator found a valid
 * context (strict additive+XOR checksum AND spare 0x80/field8=0) at block 1,
 * page 0 on every bank, with the ring at 0x694 decoding to blocks 1..4. This
 * replaces the earlier 2G-derived layout, whose ring/remap offsets were wrong
 * for the 3G. Structure and field names follow the historical Nano 3G FTL
 * (commit 3c13884) and its FTL_CHECK offset assertions. */
struct apple_vfl_cxt
{
    uint32_t usn;                 /* cross-bank update sequence number */
    uint16_t ftlctrlblocks[3];    /* superblocks holding FTL control pages */
    uint16_t field_a;
    uint32_t updatecount;         /* decrementing, also in the spare */
    uint16_t activecxtblock;
    uint16_t nextcxtpage;
    uint16_t badcount;            /* slots marked 0xffff (bad spares) */
    uint16_t field_16[3];
    uint16_t usedcount[4];        /* slots taken in each unit's table */
    uint16_t field_24[4];
    uint16_t remap[820];          /* per-plane tables, vflspares entries each */
    uint16_t vflcxtblocks[4];     /* physical blocks of the context ring */
    uint16_t pendingcount;        /* blocks awaiting replacement */
    uint16_t pending[20];         /* physical blocks that failed a read/write */
    uint8_t  field_6c6[0x12e];
    uint32_t version;
    uint32_t checksum1;           /* additive */
    uint32_t checksum2;           /* XOR */
} __attribute__((packed));

_Static_assert(sizeof(struct apple_vfl_cxt) == 0x800, "vfl cxt is 0x800");
_Static_assert(offsetof(struct apple_vfl_cxt, remap) == 0x2c, "remap at 0x2c");
_Static_assert(offsetof(struct apple_vfl_cxt, vflcxtblocks) == 0x694,
               "ring at 0x694");
_Static_assert(offsetof(struct apple_vfl_cxt, checksum1) == 0x7f8,
               "checksum at 0x7f8");

/* FTL context (0x800 bytes), historical Nano 3G layout (commit 3c13884).
 * Only the fields the read path needs are named; the rest is padding kept so
 * the offsets match. FTL_CXT offsets are pinned by _Static_assert below. */
#define APPLE_FTL_POOL_SIZE   20
#define APPLE_FTL_CXT_LOGS    18
#define APPLE_FTL_PAGELIST    36
#define APPLE_FTL_MAX_LOGS    17

struct apple_ftl_cxt_log
{
    uint32_t usn;
    uint16_t sb;
    uint16_t lblock;
    uint32_t offsetsptr;
    uint16_t pagesused;
    uint16_t pagescurrent;
    uint32_t issequential;
};

struct apple_ftl_cxt
{
    uint32_t usn;                          /* decrements once per ctrl page */
    uint32_t maxusn;
    uint16_t freecount;
    uint16_t nextfreeidx;
    uint16_t swapcounter;
    uint16_t blockpool[APPLE_FTL_POOL_SIZE];
    uint8_t  field_36[2];
    uint32_t mappages[18];                 /* vpages holding the block map */
    uint32_t ecpages[APPLE_FTL_PAGELIST];
    uint32_t logpages[APPLE_FTL_MAX_LOGS];
    uint8_t  field_154[0x44];
    uint32_t mapptr;
    uint32_t ecptr;
    uint32_t logoffsptr;
    struct apple_ftl_cxt_log logs[APPLE_FTL_CXT_LOGS];
    uint8_t  field_30c[6];
    uint16_t ctrlblocks[3];
    uint32_t ctrlpage;
    uint32_t cleanflag;
    uint32_t rcpages[APPLE_FTL_PAGELIST];
    uint32_t rcptr;
    uint8_t  field_3b4[0x1c];
    uint32_t statspage;
    uint32_t statsflag;
    uint8_t  field_3d8[0x428];
} __attribute__((packed));

_Static_assert(sizeof(struct apple_ftl_cxt) == 0x800, "ftl cxt is 0x800");
_Static_assert(offsetof(struct apple_ftl_cxt, mappages) == 0x038,
               "mappages at 0x038");
_Static_assert(offsetof(struct apple_ftl_cxt, logs) == 0x1a4, "logs at 0x1a4");
_Static_assert(offsetof(struct apple_ftl_cxt, ctrlblocks) == 0x312,
               "ctrlblocks at 0x312");
_Static_assert(offsetof(struct apple_ftl_cxt, cleanflag) == 0x31c,
               "cleanflag at 0x31c");

/* --- Runtime geometry (filled at mount from the low-level driver) -----
 *
 * The current shared NAND driver (nand-nano3g.c) exposes only physical
 * geometry (page_size, pages_per_block, blocks_per_bank) plus the chip's
 * maker/device id. Apple's FTL additionally needs the chip "mode" and the
 * plane/layout/userblocks/vflspares that follow from it. We derive those
 * here, in the Apple pipeline, from the chip identity -- WITHOUT modifying
 * the shared driver or the proven v2 pipeline. The values match the
 * historical Nano 3G port's chip table and its ftltest presets. */

/* Block-placement layouts (subset of the historical NAND_LAYOUT_*). */
enum apple_layout
{
    APPLE_LAYOUT_SINGLE = 1,   /* mode 1: one physical block per VFL block */
    APPLE_LAYOUT_ADJACENT,     /* modes 3,8: 2*b + plane */
    APPLE_LAYOUT_HALVES,       /* modes 2,9,12: b + plane*blocks/2 */
    APPLE_LAYOUT_BOTH,         /* mode 4 */
    APPLE_LAYOUT_SPLIT13       /* mode 13 (Toshiba 8320) */
};

struct apple_geometry
{
    unsigned int banks;
    unsigned int blocks;         /* per bank */
    unsigned int pages_per_block;
    unsigned int page_size;
    unsigned int planes;         /* physical blocks per bank in a VFL block */
    unsigned int userblocks;     /* per bank, as Whimory counts them */
    unsigned int vflspares;      /* VFL reserved blocks per plane */
    enum apple_layout layout;
    bool valid;
};

static struct apple_geometry a_geo;

static const struct nand_geometry *chip_geo;
static unsigned int a_bank_count;      /* chip-enables that answered */
static unsigned int a_pages_per_block;
static unsigned int a_page_size;       /* bytes of data per physical page */
static unsigned int a_blocks_per_bank;
static unsigned int a_sectors_per_page;/* page_size / NAND_PAGE_SIZE */

/* Superblock geometry (Apple's units), derived in apple_setup_geometry(). */
static unsigned int a_nsuperblocks;    /* superblocks in the virtual area */
static unsigned int a_sbpages;         /* pages per superblock */
static unsigned int a_usersb;          /* logical superblocks (user) */

static bool a_mounted;

/* Additional reserve beyond the user blocks: 3 FTL control blocks and a
 * pool of 20 (historical port's derivation from the 4GB unit's 89 spares). */
#define APPLE_EXTRA_RESERVE  23

/* Derive Apple geometry (planes/layout/userblocks/vflspares) from the chip
 * identity and physical geometry. Returns false if this chip's Apple layout
 * is not known. Currently covers the validated single-plane 4 KiB Micronas/
 * Toshiba parts and the 2 KiB Hynix parts; others return false rather than
 * mount with a wrong map. Values match the historical chip table and the
 * ftltest presets in utils/ipodnano3g/ftltest/ftl_hooks.c. */
static bool apple_derive_geometry(void)
{
    const struct nand_geometry *g = nand_get_bank_geometry(0);
    unsigned int nonuser;

    memset(&a_geo, 0, sizeof(a_geo));
    if (!g || g->page_size == 0 || g->pages_per_block == 0
        || g->blocks_per_bank == 0)
        return false;

    a_geo.banks           = nand_get_bank_count();
    a_geo.blocks          = g->blocks_per_bank;
    a_geo.pages_per_block = g->pages_per_block;
    a_geo.page_size       = g->page_size;

    /* planes: 4 KiB parts are single-plane (mode 1); 2 KiB parts are
     * two-plane. userblocks follows from capacity. The exact BLOCK LAYOUT
     * (ADJACENT vs HALVES vs BOTH) is NOT knowable from page size alone --
     * an 8 GB 2 KiB unit can be mode 8 (adjacent) or mode 9 (halves). So we
     * leave a_geo.layout unset here and DETECT it empirically after the VFL
     * mount (apple_detect_layout), by checking which layout makes the FTL
     * control superblocks actually read as control pages. */
    if (g->page_size == 4096)
    {
        a_geo.planes     = 1;
        a_geo.userblocks = 3872;
    }
    else if (g->page_size == 2048)
    {
        a_geo.planes     = 2;
        a_geo.userblocks = (g->blocks_per_bank >= 8192) ? 7744 : 3872;
    }
    else
        return false;

    /* vflspares per plane: the blocks a bank has beyond its user blocks,
     * split across planes, less the extra reserve. Matches nand-nano3g.c's
     * historical derivation (89 measured on the 4 GB unit). */
    if (a_geo.userblocks >= a_geo.blocks)
        return false;
    nonuser = a_geo.blocks - a_geo.userblocks;
    if (a_geo.planes == 0 || nonuser / a_geo.planes <= APPLE_EXTRA_RESERVE)
        return false;
    a_geo.vflspares = nonuser / a_geo.planes - APPLE_EXTRA_RESERVE;

    if (a_geo.planes * a_geo.vflspares > 820)
        return false;

    /* Provisional layout so single-plane parts work with no detection, and
     * so apple_detect_layout has a starting point. Single-plane has only
     * one placement; multi-plane is refined by detection. */
    a_geo.layout = (a_geo.planes == 1) ? APPLE_LAYOUT_SINGLE
                                       : APPLE_LAYOUT_ADJACENT;
    a_geo.valid = true;
    return true;
}

/* --- Low-level read adapter ------------------------------------------
 *
 * The 2G reference calls nand_read_page(bank,page,data,spare,doecc,
 * checkempty) and inspects a return bitmask. Our 3G driver's
 * nand_hw_read_page(bank,page,data,spare) returns a nand_ecc_result (>=0)
 * or negative NAND_HWERR_*. This adapter presents the 2G-style "is this a
 * usable read" test the rest of the port wants. */

/* True if the read is trustworthy enough to use its bytes (clean, ok, or
 * corrected -- but not an uncorrectable ECC failure or a hardware error). */
static bool apple_read_ok(int rc)
{
    return rc == NAND_ECC_CLEAN || rc == NAND_ECC_OK
        || rc == NAND_ECC_CORRECTED;
}

/* --- Static working buffers ------------------------------------------
 * One page's data + one page's spare metadata. Page buffer is sized to the
 * max supported page (4 KiB) and 32-byte aligned for the FMC DMA (see
 * NAND_DMA_BUF_ATTR). Kept static (not on-stack) for the same
 * stack-overflow reason nand-nano3g.c documents for its own scratch. */
static uint8_t  a_pagebuf[NAND_MAX_PAGE_SIZE] NAND_DMA_BUF_ATTR;
static uint8_t  a_sparebuf[NAND_MAX_SPARE_SIZE] NAND_DMA_BUF_ATTR;

/* One VFL context per bank, loaded from flash at mount. */
static struct apple_vfl_cxt a_vfl[NAND_MAX_BANKS];
static uint32_t a_vfl_usn_max;   /* highest VFL usn seen (newest context) */

/* FTL context + block map, loaded from flash at mount (Phase 2). */
static struct apple_ftl_cxt a_ftl;

/* Snapshot of the start of bank 0's device-info page, kept for diagnostics.
 * The device-info page is where Apple stores the real user-block /
 * system-block counts we still need to decode (see apple_ftl_open). Dumping
 * its leading bytes lets the read-only hardware pass locate those fields. */
#define APPLE_DEVINFO_DUMP  128
static uint8_t a_devinfo_head[APPLE_DEVINFO_DUMP];
static bool    a_devinfo_captured;
/* Logical superblock -> physical superblock map, read from the FTL context's
 * mappages (clean) or rebuilt by the read-only restore (uncommitted). Sized
 * for the largest supported chip (historical FTL_MAX_USERSB). */
#define APPLE_MAX_USERSB    4096
#define APPLE_MAX_SB        4096
#define APPLE_MAX_SBPAGES   1024
#define APPLE_MAX_LOGS      17
#define APPLE_FTL_NO_PAGE   0xffff
static uint16_t a_map[APPLE_MAX_USERSB];

/* The FTL control superblocks, from the newest VFL context. */
static uint16_t a_ftl_ctrl[3];
static uint32_t a_ftl_ctrl_idx;   /* which of the 3 is newest (restore) */

/* --- Open-log tracking (read-only restore + log-aware resolve) --------
 * On an uncommitted Apple volume, pages written since the last commit live in
 * "log" superblocks. A page present in a log is newer than the block map's
 * copy. The restore rebuilds these logs in RAM (no writes); apple_ftl_resolve
 * then prefers a log's copy over the map. Ported from the historical 3G FTL
 * (commit 3c13884): struct ftl_log, ftl_restore_*, ftl_resolve. */
struct apple_log
{
    uint32_t usn;
    uint16_t sb;                          /* the scattered superblock */
    uint16_t lblock;                      /* the logical block it stands for */
    uint16_t pagesused;
    uint16_t pagescurrent;
    uint32_t issequential;
    uint16_t offsets[APPLE_MAX_SBPAGES];  /* index in lblock -> v in sb */
};

/* Restore working state. Static (not on-stack): each struct apple_log is ~2KB
 * and there are up to 18 of them. Live only during the mount. */
#define APPLE_RS_LOG    0x40
#define APPLE_RS_DATA   0x41
#define APPLE_RS_CTRL   0x42
#define APPLE_RS_FREE   0x48
#define APPLE_RS_SLOTS  (APPLE_MAX_LOGS + 1)
#define APPLE_POOL_SIZE 20

struct apple_rs { uint16_t lblock; uint8_t state; uint32_t usn; };
static struct apple_rs a_rs[APPLE_MAX_SB];
static struct apple_log a_logs[APPLE_MAX_LOGS];
static struct apple_log a_rs_slots[APPLE_RS_SLOTS];
static unsigned int a_nlogs;
static uint16_t a_freepool[APPLE_POOL_SIZE];
static uint16_t a_freecount;

/* Diagnostics from the most recent apple_ftl_open() FTL-control-block scan,
 * so a read-only probe can distinguish "unclean volume" (valid scan, no
 * trailing context) from "couldn't read the control blocks" (a bug). */
static uint32_t a_dbg_ctrl_used[3];   /* pages read before erased/error */
static int      a_dbg_ctrl_last[3];   /* last page's spare type, or -1 */
static uint32_t a_dbg_newblk;         /* chosen newest block index, or 3 */
static int      a_dbg_ftl_clean;      /* 1 clean, 0 needs restore */

/* --- VFL block placement + remap (ported from historical 3G FTL) ------
 *
 * Apple's VFL groups each bank's physical blocks into `planes` units. Virtual
 * block b of a unit maps to a physical block placed by the chip mode
 * (apple_unit_block), unless that bank's VFL context remaps it onto a
 * reserved spare. */

/* Physical block of virtual block vblock of a unit, per the chip's mode. */
static uint32_t apple_unit_block(uint32_t unit, uint32_t vblock)
{
    uint32_t half = a_geo.blocks / 2;

    switch (a_geo.layout)
    {
    case APPLE_LAYOUT_SINGLE:
        return vblock;
    case APPLE_LAYOUT_HALVES:
        return vblock + unit * half;
    case APPLE_LAYOUT_BOTH:
        return 2 * vblock + (unit & 1) + ((unit & 2) ? half : 0);
    case APPLE_LAYOUT_SPLIT13:
        if (vblock < 4096)
            return vblock + (vblock >= 2048 ? 2048 : 0) + unit * 2048;
        if (vblock < 4128)
            return 8192 + unit * 32 + (vblock - 4096);
        return 8256 + unit * 32 + (vblock - 4128);
    default: /* APPLE_LAYOUT_ADJACENT */
        return 2 * vblock + unit;
    }
}

/* Physical block of remap slot k's reserved block. */
static uint32_t apple_vfl_spare_block(uint32_t k)
{
    return apple_unit_block(k / a_geo.vflspares,
                            a_nsuperblocks + k % a_geo.vflspares);
}

/* Where virtual block vblock of a unit really is: its own physical block, or
 * the reserved block a slot of any unit remapped it to. A full unit borrows
 * another's, so Apple's lookup scans every plane's table. */
static uint32_t apple_vfl_phys_block(unsigned int bank, uint32_t unit,
                                     uint32_t vblock)
{
    const uint16_t *remap = a_vfl[bank].remap;
    uint32_t block = apple_unit_block(unit, vblock);
    uint32_t k, n = a_geo.planes * a_geo.vflspares;

    for (k = 0; k < n; k++)
        if (remap[k] == block)
            return apple_vfl_spare_block(k);
    return block;
}

/* Spare fields as raw words (the driver returns 3 x uint32 = 12 bytes). */
#define APPLE_META_USN(sp)   (((const uint32_t *)(sp))[0])
#define APPLE_META_W2(sp)    (((const uint32_t *)(sp))[2])
#define APPLE_META_TYPE(sp)  ((uint8_t)((APPLE_META_W2(sp) >> 8) & 0xff))
#define APPLE_META_F8(sp)    ((uint8_t)(APPLE_META_W2(sp) & 0xff))

/* --- VFL context page read (historical 3G logic) --------------------
 * A VFL context commit is 8 copies. Read the first good copy at or after
 * `page` of `block` on `bank` into a_pagebuf/a_sparebuf. "Good" = readable,
 * spare type 0x80 with field_8 == 0, AND strict context checksum valid.
 * Returns 0 if found. */
static int apple_vfl_read_cxt(unsigned int bank, uint32_t block,
                              uint32_t page)
{
    uint32_t i;
    for (i = page; i < page + 8 && i < a_pages_per_block; i++)
    {
        int rc = nand_hw_read_page(bank, block * a_pages_per_block + i,
                                   a_pagebuf, a_sparebuf);
        if (!apple_read_ok(rc))
            continue;
        if (APPLE_META_TYPE(a_sparebuf) != APPLE_TYPE_VFLCXT)
            continue;
        if (APPLE_META_F8(a_sparebuf) != 0)
            continue;
        {
            const uint32_t *w = (const uint32_t *)a_pagebuf;
            uint32_t c1 = 0xAABBCCDD, c2 = 0xAABBCCDD, j;
            for (j = 0; j < offsetof(struct apple_vfl_cxt, checksum1) / 4; j++)
            {
                c1 += w[j];
                c2 ^= w[j];
            }
            if (c1 == ((const struct apple_vfl_cxt *)a_pagebuf)->checksum1
                && c2 == ((const struct apple_vfl_cxt *)a_pagebuf)->checksum2)
                return 0;
        }
    }
    return 1;
}

/* Newest VFL context (highest usn) across banks -- yields the FTL control
 * block list. */
static struct apple_vfl_cxt *apple_vfl_newest(void)
{
    struct apple_vfl_cxt *c = NULL;
    uint32_t i, maxusn = 0;
    for (i = 0; i < a_bank_count; i++)
        if (c == NULL || a_vfl[i].usn > maxusn)
        {
            c = &a_vfl[i];
            maxusn = a_vfl[i].usn;
        }
    return c;
}

/* Check a bank's VFL context is laid out as the derived geometry says: the
 * ring and control blocks are in range, each plane's remap table has its
 * used/free/bad slots in the expected order, and unused remap entries are
 * zero. A wrong geometry (planes/vflspares) moves those boundaries, so this
 * refuses a context that does not fit rather than mount a wrong map.
 * Ported from the historical 3G ftl_vfl_check_cxt. */
#define APPLE_VFL_FREE  0xfff0
#define APPLE_VFL_BAD   0xffff

static int apple_vfl_check_cxt(const struct apple_vfl_cxt *cxt)
{
    uint32_t unit, i, v;
    bool freeseen;

    for (i = 0; i < 4; i++)
        if (cxt->vflcxtblocks[i] >= a_blocks_per_bank)
            return -1;
    for (i = 0; i < 3; i++)
        if (cxt->ftlctrlblocks[i] >= a_nsuperblocks)
            return -1;
    for (unit = 0; unit < a_geo.planes; unit++)
    {
        const uint16_t *table = &cxt->remap[unit * a_geo.vflspares];
        if (cxt->usedcount[unit] > a_geo.vflspares)
            return -1;
        freeseen = false;
        for (i = 0; i < a_geo.vflspares; i++)
        {
            v = table[i];
            if (v == APPLE_VFL_FREE)
                freeseen = true;
            else if (v != APPLE_VFL_BAD
                     && (freeseen || v >= a_blocks_per_bank))
                return -1;
        }
    }
    for (i = a_geo.planes * a_geo.vflspares;
         i < sizeof(cxt->remap) / sizeof(cxt->remap[0]); i++)
        if (cxt->remap[i])
            return -1;
    return 0;
}

/* --- VFL mount (historical 3G ftl_vfl_open_bank, read-only) ----------
 * Per bank: scan physical blocks 1..199 for any valid VFL context page; that
 * context lists the ring of context blocks; pick the ring member whose page-0
 * spare update count (meta[0]) is the lowest positive value; replay its ring
 * in steps of 8 pages to the newest committed generation; load that context.
 * CONFIRMED on hardware: contexts live at block 1..4, page 0, on all banks. */
#define APPLE_VFL_SCANBLOCKS 200

static int apple_vfl_open_bank(unsigned int bank)
{
    uint16_t ring[4];
    uint32_t i, block, last, best = 4, bestusn = 0xffffffff;

    for (block = 1; block < APPLE_VFL_SCANBLOCKS
                    && block < a_blocks_per_bank; block++)
        if (!apple_vfl_read_cxt(bank, block, 0))
            break;
    if (block >= APPLE_VFL_SCANBLOCKS || block >= a_blocks_per_bank)
        return 1;

    memcpy(ring, ((const struct apple_vfl_cxt *)a_pagebuf)->vflcxtblocks,
           sizeof(ring));

    for (i = 0; i < 4; i++)
    {
        if (ring[i] >= a_blocks_per_bank
            || apple_vfl_read_cxt(bank, ring[i], 0))
            continue;
        if (APPLE_META_USN(a_sparebuf)
            && APPLE_META_USN(a_sparebuf) <= bestusn)
        {
            bestusn = APPLE_META_USN(a_sparebuf);
            best = i;
        }
    }
    if (best == 4)
        return 1;

    block = ring[best];
    last = 0;
    for (i = 8; i < a_pages_per_block; i += 8)
    {
        if (apple_vfl_read_cxt(bank, block, i))
            break;
        last = i;
    }
    if (apple_vfl_read_cxt(bank, block, last))
        return 1;
    memcpy(&a_vfl[bank], a_pagebuf, sizeof(struct apple_vfl_cxt));
    return 0;
}

/* Mount every bank's VFL context. Returns 0 only if all banks succeed. */
static int apple_vfl_open(void)
{
    unsigned int i;

    a_vfl_usn_max = 0;
    for (i = 0; i < a_bank_count; i++)
    {
        if (apple_vfl_open_bank(i) != 0)
            return 1;
        if (apple_vfl_check_cxt(&a_vfl[i]) != 0)
            return 1;
        if (a_vfl_usn_max < a_vfl[i].usn)
            a_vfl_usn_max = a_vfl[i].usn;
    }
    return 0;
}

/* --- Superblock addressing (historical 3G) --------------------------
 * A superblock spans all banks and planes. Virtual page v of superblock sb
 * is on bank (v % banks), plane ((v / banks) % planes), physical page
 * (v / (banks*planes)) of that unit's (possibly remapped) physical block. */
static void apple_vpage_phys(uint32_t sb, uint32_t v,
                             uint32_t *bank, uint32_t *page)
{
    uint32_t plane = (v / a_geo.banks) % a_geo.planes;

    *bank = v % a_geo.banks;
    *page = apple_vfl_phys_block(*bank, plane, sb) * a_geo.pages_per_block
          + v / (a_geo.banks * a_geo.planes);
}

/* Read virtual page v of superblock sb into a_pagebuf/a_sparebuf. Returns
 * the driver's ecc-result (>=0) or negative on hardware error. */
static int apple_read_vpage(uint32_t sb, uint32_t v, void *data_out,
                            void *spare_out)
{
    uint32_t bank, page;
    int rc;

    apple_vpage_phys(sb, v, &bank, &page);
    rc = nand_hw_read_page(bank, page, data_out, spare_out);
    if (rc < 0 && rc != NAND_HWERR_NO_CHIP)
    {
        nand_hw_reset(bank);
        rc = nand_hw_read_page(bank, page, data_out, spare_out);
    }
    return rc;
}

/* A logical/virtual page number is represented directly as sb/v where
 * needed; no separate helper is required. */

/* --- Block-layout detection (read-only) ------------------------------
 * page_size alone cannot tell ADJACENT (mode 8) from HALVES (mode 9) on an
 * 8 GB 2 KiB unit, and the wrong layout maps every superblock to the wrong
 * physical block. The VFL mount is layout-independent (it scans physical
 * blocks), so after it we know the FTL control superblocks. Try each
 * candidate layout and keep the one under which page 0 of a control
 * superblock reads as an FTL control page (spare type 0x43..0x4f). This
 * needs no chip-ID table and generalises to any stock unit. Single-plane
 * parts have only one placement and skip detection. */
static bool apple_ctrlblock_reads_as_control(uint16_t sb)
{
    uint32_t bank, page;
    int rc;

    if (sb >= a_nsuperblocks)
        return false;
    apple_vpage_phys(sb, 0, &bank, &page);
    rc = nand_hw_read_page(bank, page, a_pagebuf, a_sparebuf);
    if (!apple_read_ok(rc))
        return false;
    return (uint8_t)(APPLE_META_TYPE(a_sparebuf) - APPLE_TYPE_FTLCXT)
           <= (0x4f - 0x43);
}

static bool apple_detect_layout(void)
{
    static const enum apple_layout candidates[] = {
        APPLE_LAYOUT_ADJACENT, APPLE_LAYOUT_HALVES,
        APPLE_LAYOUT_BOTH, APPLE_LAYOUT_SPLIT13
    };
    const struct apple_vfl_cxt *cxt = apple_vfl_newest();
    unsigned int c, i;

    if (a_geo.planes == 1)
    {
        a_geo.layout = APPLE_LAYOUT_SINGLE;
        return true;
    }
    if (!cxt)
        return false;

    for (c = 0; c < sizeof(candidates) / sizeof(candidates[0]); c++)
    {
        unsigned int hits = 0;
        a_geo.layout = candidates[c];
        for (i = 0; i < 3; i++)
            if (apple_ctrlblock_reads_as_control(cxt->ftlctrlblocks[i]))
                hits++;
        if (hits >= 2)   /* majority of the 3 control blocks agree */
            return true;
    }
    return false;
}

/* Read page v of superblock sb; return its spare type (0x40..) or -1 on a
 * failed/uncorrectable read. Mirrors the historical ftl_read_vpage, which the
 * restore phases use. Leaves the page in a_pagebuf and spare in a_sparebuf,
 * so meta words are read via APPLE_META_* right after. */
static int apple_read_vpage_type(uint32_t sb, uint32_t v)
{
    int rc = apple_read_vpage(sb, v, a_pagebuf, a_sparebuf);
    if (!apple_read_ok(rc))
        return -1;
    return APPLE_META_TYPE(a_sparebuf);
}

/* --- Read-only restore (rebuild map+logs+pool from the medium) --------
 * Ported from the historical 3G ftl_restore()/ftl_resolve() (commit 3c13884).
 * Every phase is READS ONLY: it reconstructs the in-RAM block map, open logs
 * and free pool. It never erases or programs (the historical write-capable
 * restore's erase+commit steps are deliberately omitted here). */

/* A log whose every readable page sits at its own index can stand as data. */
static bool apple_log_in_place(uint32_t sb)
{
    uint32_t v;
    for (v = 0; v < a_sbpages; v++)
    {
        int type = apple_read_vpage_type(sb, v);
        if (type >= 0 && type != APPLE_TYPE_ERASED
            && APPLE_META_USN(a_sparebuf) % a_sbpages != v)
            return false;
    }
    return true;
}

/* Scan a log superblock: last copy of each index wins. */
static void apple_scan_log(struct apple_log *log)
{
    uint32_t v, idx;
    memset(log->offsets, 0xff, a_sbpages * sizeof(log->offsets[0]));
    log->pagescurrent = 0;
    for (v = 0; v < a_sbpages; v++)
    {
        int type = apple_read_vpage_type(log->sb, v);
        if (type < 0 || type == APPLE_TYPE_ERASED)
            continue;
        idx = APPLE_META_USN(a_sparebuf) % a_sbpages;
        if (log->offsets[idx] == APPLE_FTL_NO_PAGE)
            log->pagescurrent++;
        log->offsets[idx] = v;
    }
}

/* phase A: the FTL control blocks; newest by page-0 usn. */
static int apple_restore_ctrl_blocks(void)
{
    uint32_t i, page0usn = 0xffffffff;
    for (i = 0; i < 3; i++)
    {
        if (a_ftl_ctrl[i] >= a_nsuperblocks)
            return -30;
        a_rs[a_ftl_ctrl[i]].state = APPLE_RS_CTRL;
        if (apple_read_vpage_type(a_ftl_ctrl[i], 0) >= 0
            && APPLE_META_USN(a_sparebuf) < page0usn)
        {
            page0usn = APPLE_META_USN(a_sparebuf);
            a_ftl_ctrl_idx = i;
        }
    }
    return 0;
}

/* phase B: classify every other superblock; closed blocks (last page 0x41)
 * go into the map, highest usn winning. Returns the highest data usn. */
static uint32_t apple_restore_classify(void)
{
    uint32_t sb, lblock, maxusn = 0;
    memset(a_map, 0xff, sizeof(a_map));
    for (sb = 0; sb < a_nsuperblocks; sb++)
    {
        struct apple_rs *r = &a_rs[sb];
        int type;
        if (r->state == APPLE_RS_CTRL)
            continue;
        r->state = APPLE_RS_FREE;
        type = apple_read_vpage_type(sb, a_sbpages - 1);
        if (type < 0 || type == APPLE_TYPE_ERASED)
        {
            type = apple_read_vpage_type(sb, 0);
            if (type != APPLE_TYPE_DATA)
                continue;
            r->state = APPLE_RS_LOG;
        }
        else if (type == APPLE_TYPE_DATA_LAST)
            r->state = APPLE_RS_DATA;
        else if (type == APPLE_TYPE_DATA)
            r->state = APPLE_RS_LOG;
        else
            continue;
        lblock = APPLE_META_USN(a_sparebuf) / a_sbpages;
        if (lblock >= a_usersb)
        {
            r->state = APPLE_RS_FREE;
            continue;
        }
        r->lblock = lblock;
        r->usn = ((const uint32_t *)a_sparebuf)[1];   /* meta[1] */
        if (r->usn > maxusn)
            maxusn = r->usn;
        if (r->state != APPLE_RS_DATA)
            continue;
        if (a_map[lblock] != 0xffff)
        {
            if (a_rs[a_map[lblock]].usn >= r->usn)
            {
                r->state = APPLE_RS_FREE;
                continue;
            }
            a_rs[a_map[lblock]].state = APPLE_RS_FREE;
        }
        a_map[lblock] = sb;
    }
    return maxusn;
}

/* phase C: place logs, one per logical block. */
static int apple_restore_place_logs(uint32_t maxusn)
{
    static struct apple_log cand;
    uint32_t sb, i, lblock;

    for (i = 0; i < APPLE_RS_SLOTS; i++)
        a_rs_slots[i].lblock = 0xffff;
    for (sb = 0; sb < a_nsuperblocks; sb++)
    {
        struct apple_rs *r = &a_rs[sb];
        uint32_t datausn = 0;
        if (r->state != APPLE_RS_LOG)
            continue;
        lblock = r->lblock;
        if (a_map[lblock] == 0xffff)
        {
            if (apple_log_in_place(sb))
            {
                a_map[lblock] = sb;
                r->state = APPLE_RS_DATA;
                continue;
            }
        }
        else
            datausn = a_rs[a_map[lblock]].usn;
        if (r->usn < datausn)
        {
            r->state = APPLE_RS_FREE;
            continue;
        }
        for (i = 0; i < APPLE_RS_SLOTS; i++)
        {
            struct apple_log *s = &a_rs_slots[i];
            if (s->lblock == 0xffff)
            {
                s->lblock = lblock;
                s->sb = sb;
                s->usn = r->usn;
                break;
            }
            if (s->lblock != lblock)
                continue;
            if (r->usn != maxusn && s->usn != maxusn)
            {
                if (s->usn >= r->usn)
                    r->state = APPLE_RS_FREE;
                else
                {
                    a_rs[s->sb].state = APPLE_RS_FREE;
                    s->sb = sb;
                    s->usn = r->usn;
                }
            }
            else
            {
                const struct apple_log *newer, *older;
                uint32_t idx, loserusn;
                cand.sb = sb;
                cand.usn = r->usn;
                apple_scan_log(&cand);
                apple_scan_log(s);
                newer = r->usn == maxusn ? &cand : s;
                older = newer == &cand ? s : &cand;
                for (idx = 0; idx < a_sbpages; idx++)
                    if (older->offsets[idx] != APPLE_FTL_NO_PAGE
                        && newer->offsets[idx] == APPLE_FTL_NO_PAGE)
                        break;
                loserusn = idx == a_sbpages ? older->usn : newer->usn;
                if (loserusn == r->usn)
                    r->state = APPLE_RS_FREE;
                else
                {
                    a_rs[s->sb].state = APPLE_RS_FREE;
                    s->sb = sb;
                    s->usn = r->usn;
                }
            }
            break;
        }
        if (i == APPLE_RS_SLOTS)
            return -31;
    }
    if (a_rs_slots[APPLE_MAX_LOGS].lblock != 0xffff)
    {
        for (i = 0; i <= APPLE_MAX_LOGS; i++)
            if (a_rs_slots[i].usn == maxusn)
                break;
        if (i > APPLE_MAX_LOGS)
            return -32;
        a_rs[a_rs_slots[i].sb].state = APPLE_RS_FREE;
        if (i < APPLE_MAX_LOGS)
            a_rs_slots[i] = a_rs_slots[APPLE_MAX_LOGS];
        a_rs_slots[APPLE_MAX_LOGS].lblock = 0xffff;
    }
    return 0;
}

/* phase D: a logical block with no data block gets the next free one. In a
 * READ-ONLY mount we do not erase these "holes"; they simply read as 0xFF. */
static int apple_restore_fill_holes(void)
{
    uint32_t lblock, f = 0;
    for (lblock = 0; lblock < a_usersb; lblock++)
    {
        if (a_map[lblock] != 0xffff)
            continue;
        while (f < a_nsuperblocks && a_rs[f].state != APPLE_RS_FREE)
            f++;
        if (f == a_nsuperblocks)
            return -33;
        a_rs[f].state = APPLE_RS_DATA;
        a_rs[f].lblock = lblock;
        a_map[lblock] = f;
    }
    return 0;
}

/* phase E: the free pool is everything left, after one slot per log. */
static int apple_restore_build_pool(void)
{
    uint32_t sb, i, n = 0;
    a_nlogs = 0;
    for (i = 0; i < APPLE_MAX_LOGS; i++)
        if (a_rs_slots[i].lblock != 0xffff)
            a_logs[a_nlogs++] = a_rs_slots[i];
    for (i = 0; i < APPLE_POOL_SIZE; i++)
        a_freepool[i] = 0xffff;
    for (sb = 0; sb < a_nsuperblocks && a_nlogs + n < APPLE_POOL_SIZE; sb++)
        if (a_rs[sb].state == APPLE_RS_FREE)
            a_freepool[n++] = sb;
    a_freecount = n;
    if (a_nlogs + n != APPLE_POOL_SIZE)
        return -34;
    return 0;
}

/* phase I: what each log holds. */
static void apple_restore_scan_logs(void)
{
    uint32_t i;
    for (i = 0; i < a_nlogs; i++)
    {
        apple_scan_log(&a_logs[i]);
        a_logs[i].pagesused = a_sbpages;
        a_logs[i].issequential = 0;
    }
}

static int apple_ftl_restore(void)
{
    uint32_t maxusn;
    int ret;

    memset(a_rs, 0, sizeof(a_rs));
    a_nlogs = 0;

    ret = apple_restore_ctrl_blocks();          /* phase A */
    if (ret)
        return ret;
    maxusn = apple_restore_classify();          /* phase B */
    ret = apple_restore_place_logs(maxusn);     /* phase C */
    if (ret)
        return ret;
    ret = apple_restore_fill_holes();           /* phase D (RAM only) */
    if (ret)
        return ret;
    ret = apple_restore_build_pool();           /* phase E */
    if (ret)
        return ret;
    apple_restore_scan_logs();                  /* phase I */
    return 0;
}

/* Resolve a logical page to (superblock, page): the newest log that holds it,
 * else the block map. Ported from ftl_resolve. */
static int apple_ftl_resolve(uint32_t lpn, uint32_t *sb, uint32_t *v)
{
    uint32_t lblock = lpn / a_sbpages, idx = lpn % a_sbpages, i;
    uint32_t bestusn = 0;
    bool found = false;

    if (lblock >= a_usersb)
        return -1;
    *sb = a_map[lblock];
    *v = idx;
    for (i = 0; i < a_nlogs; i++)
    {
        const struct apple_log *log = &a_logs[i];
        if (log->lblock == lblock && log->offsets[idx] != APPLE_FTL_NO_PAGE
            && (!found || log->usn > bestusn))
        {
            *sb = log->sb;
            *v = log->offsets[idx];
            bestusn = log->usn;
            found = true;
        }
    }
    return 0;
}

/* --- FTL layer mount (ported from 2G ftl_open, read-only) ------------
 * Locate the newest FTL context via the VFL's ftlctrlblocks, load it, then
 * load the logical-block map. Detects an unclean shutdown the 2G way (a
 * non-0x43 page newer than the context). Returns 0 on success. */
static int apple_ftl_open(void)
{
    const struct apple_vfl_cxt *newest = apple_vfl_newest();
    uint32_t i, v, ctrl = 3, cxtv = 0;
    uint32_t minusn = 0xffffffff, page0usn = 0xffffffff, newblk = 3;
    uint32_t used[3] = { 0, 0, 0 };
    int type, lasttype[3] = { -1, -1, -1 };
    bool clean;
    uint32_t npages;

    if (!newest)
        return 1;
    memcpy(a_ftl_ctrl, newest->ftlctrlblocks, sizeof(a_ftl_ctrl));

    /* Find the FTL control block whose page-0 has the lowest usn, and the
     * newest 0x43 context page overall. Historical ftl_load_cxt. */
    for (i = 0; i < 3; i++)
    {
        if (a_ftl_ctrl[i] >= a_nsuperblocks)
            continue;
        for (v = 0; v < a_sbpages; v++)
        {
            int rc = apple_read_vpage(a_ftl_ctrl[i], v, a_pagebuf, a_sparebuf);
            if (!apple_read_ok(rc))
            {
                type = -1;
            }
            else
            {
                type = APPLE_META_TYPE(a_sparebuf);
            }
            if (type == APPLE_TYPE_ERASED || type < 0)
                break;
            if (v == 0
                && (uint8_t)(type - APPLE_TYPE_FTLCXT) <= (0x4f - 0x43)
                && APPLE_META_USN(a_sparebuf) < page0usn)
            {
                page0usn = APPLE_META_USN(a_sparebuf);
                newblk = i;
            }
            if (type == APPLE_TYPE_FTLCXT
                && APPLE_META_USN(a_sparebuf) <= minusn)
            {
                minusn = APPLE_META_USN(a_sparebuf);
                ctrl = i;
                cxtv = v;
            }
            lasttype[i] = type;
        }
        used[i] = v;
    }

    /* Record the scan for the read-only diagnostic. */
    for (i = 0; i < 3; i++)
    {
        a_dbg_ctrl_used[i] = used[i];
        a_dbg_ctrl_last[i] = lasttype[i];
    }
    a_dbg_newblk = newblk;

    /* Clean means the newest control block ends in a context page. An
     * uncommitted volume (the usual state after Apple's OS) is rebuilt from
     * the medium by the READ-ONLY restore below -- reconstruction only, no
     * NAND writes (unlike the historical write-capable restore, which also
     * erases the reclaimed blocks and commits). */
    clean = newblk < 3 && lasttype[newblk] == APPLE_TYPE_FTLCXT;
    a_dbg_ftl_clean = clean ? 1 : 0;
    if (!clean)
    {
        /* Keep the newest context page found (for its fields) if any, then
         * rebuild map+logs+pool in RAM. */
        if (ctrl != 3
            && apple_read_vpage(a_ftl_ctrl[ctrl], cxtv, a_pagebuf, a_sparebuf)
               >= 0
            && APPLE_META_TYPE(a_sparebuf) == APPLE_TYPE_FTLCXT)
            memcpy(&a_ftl, a_pagebuf, sizeof(a_ftl));
        return apple_ftl_restore();
    }
    ctrl = newblk;
    cxtv = used[newblk] - 1;

    if (ctrl == 3
        || apple_read_vpage(a_ftl_ctrl[ctrl], cxtv, a_pagebuf, a_sparebuf) < 0
        || APPLE_META_TYPE(a_sparebuf) != APPLE_TYPE_FTLCXT)
        return 1;
    memcpy(&a_ftl, a_pagebuf, sizeof(a_ftl));

    /* Load the block map from the context's mappages list: usersb entries of
     * 2 bytes each, one map page (SPARE_FTL_MAP) per pagesize bytes. Each map
     * page's spare index (meta[1] low 16) must equal its position. */
    npages = (a_usersb * 2 + a_page_size - 1) / a_page_size;
    if (npages > sizeof(a_ftl.mappages) / sizeof(a_ftl.mappages[0]))
        return 1;
    for (i = 0; i < npages; i++)
    {
        uint32_t vpn = a_ftl.mappages[i];
        uint32_t toread;
        if (vpn / a_sbpages >= a_nsuperblocks)
            return 1;
        if (apple_read_vpage(vpn / a_sbpages, vpn % a_sbpages,
                             a_pagebuf, a_sparebuf) < 0)
            return 1;
        if (APPLE_META_TYPE(a_sparebuf) != APPLE_TYPE_MAP)
            return 1;
        if ((((const uint32_t *)a_sparebuf)[1] & 0xffff) != i)
            return 1;
        toread = a_usersb * 2 - i * a_page_size;
        if (toread > a_page_size)
            toread = a_page_size;
        memcpy((uint8_t *)a_map + i * a_page_size, a_pagebuf, toread);
    }

    /* Validate every map entry is a real superblock. */
    for (i = 0; i < a_usersb; i++)
        if (a_map[i] >= a_nsuperblocks)
            return 1;

    return 0;
}

/* --- FTL read (historical 3G resolve, read-only) --------------------
 * Resolve the logical page through apple_ftl_resolve: the newest open log
 * that holds it, else the block map. This works for both a clean volume
 * (no logs) and an uncommitted one rebuilt by apple_ftl_restore. `sector`
 * counts NAND_PAGE_SIZE (2048-byte) logical sectors, a_sectors_per_page per
 * physical page. */
static int apple_ftl_read_sector(uint32_t sector, void *out)
{
    uint32_t lpn    = sector / a_sectors_per_page;   /* logical page */
    uint32_t subsec = sector % a_sectors_per_page;   /* half within a 4K page */
    uint32_t sb, v;
    int rc;

    if (apple_ftl_resolve(lpn, &sb, &v) != 0)
    {
        memset(out, 0xFF, NAND_PAGE_SIZE);
        return 0;
    }

    rc = apple_read_vpage(sb, v, a_pagebuf, a_sparebuf);
    if (rc == NAND_ECC_CLEAN)
    {
        memset(out, 0xFF, NAND_PAGE_SIZE);
        return 0;
    }
    if (!apple_read_ok(rc))
    {
        memset(out, 0xFF, NAND_PAGE_SIZE);
        return -1;
    }
    memcpy(out, (const uint8_t *)a_pagebuf + subsec * NAND_PAGE_SIZE,
           NAND_PAGE_SIZE);
    return 0;
}

/* --- Public interface (ftl-target.h) ---------------------------------
 *
 * Phase 1 implements the VFL mount + geometry recovery; Phase 2 adds the
 * FTL context load + read path. The write path fails closed and the mount
 * stays read-only, so a diagnostic image can exercise the whole read stack
 * against a stock unit without risking anything. */

int ftl_init(void)
{
    a_mounted = false;

    a_bank_count = nand_get_bank_count();
    if (a_bank_count == 0)
        return FTL_ERR_NO_BANKS;

    chip_geo = nand_get_bank_geometry(0);
    if (!chip_geo)
        return FTL_ERR_UNRECOGNIZED;

    a_page_size        = chip_geo->page_size;
    a_pages_per_block  = chip_geo->pages_per_block;
    a_blocks_per_bank  = chip_geo->blocks_per_bank;
    a_sectors_per_page = a_page_size / NAND_PAGE_SIZE;
    if (a_sectors_per_page == 0)
        a_sectors_per_page = 1;
    if (a_page_size == 0 || a_pages_per_block == 0 || a_blocks_per_bank == 0)
        return FTL_ERR_UNRECOGNIZED;

    /* Derive Apple's plane/layout/userblocks/vflspares from the chip. */
    if (!apple_derive_geometry())
        return FTL_ERR_UNRECOGNIZED;

    /* Superblock geometry (Apple's units), from the derived values. A
     * superblock spans every bank and plane; there are `planes` physical
     * blocks per bank in one VFL block. Historical ftl_setup_geometry. */
    a_nsuperblocks = a_geo.blocks / a_geo.planes - a_geo.vflspares;
    a_sbpages      = a_geo.pages_per_block * a_geo.planes * a_geo.banks;
    a_usersb       = a_geo.userblocks / a_geo.planes;
    if (a_nsuperblocks == 0 || a_usersb == 0 || a_usersb > a_nsuperblocks
        || a_usersb > APPLE_MAX_USERSB)
        return FTL_ERR_TOO_SMALL;

    /* Phase 1: mount every bank's VFL context (strict checksum, ring-follow)
     * and validate its layout against the derived geometry. Read-only. */
    if (apple_vfl_open() != 0)
        return FTL_ERR_UNRECOGNIZED;

    /* Phase 1b: detect the block layout (ADJACENT/HALVES/...) empirically,
     * since page size alone cannot distinguish them on 2-plane parts. */
    if (!apple_detect_layout())
        return FTL_ERR_UNRECOGNIZED;

    /* Phase 2: load the FTL context + block map (clean volumes only; an
     * unclean volume returns 2 and we stay unmounted rather than rebuild,
     * since the read-only pipeline does not write). Read-only. */
    if (apple_ftl_open() != 0)
        return FTL_ERR_UNRECOGNIZED;

    a_mounted = true;
    return 0;
}

/* --- Read-only diagnostic ---------------------------------------------
 * Exposes what the VFL mount recovered, for a check-style image to print
 * and photograph. Safe: read-only, no side effects. Returns 0 if the VFL
 * mounted, non-zero otherwise; fills *out with the reconstructed values. */
struct apple_vfl_mount_info
{
    unsigned int banks;
    unsigned int page_size;
    unsigned int pages_per_block;
    unsigned int blocks_per_bank;
    uint32_t     vfl_usn_max;
    uint16_t     ftlctrlblocks[3];  /* where the FTL context lives */
    uint16_t     spareused[NAND_MAX_BANKS];
    uint16_t     firstspare[NAND_MAX_BANKS];
    uint16_t     sparecount[NAND_MAX_BANKS];
    unsigned int user_blocks;       /* PROVISIONAL, see apple_ftl_open */
    uint8_t      devinfo_head[APPLE_DEVINFO_DUMP]; /* head of devinfo page */
    bool         devinfo_captured;
    bool         ftl_mounted;       /* FTL context + map loaded too */
};

/* --- Raw scan diagnostic (read-only) --------------------------------
 * The first hardware run reported apple_devinfo_captured 0 -- the
 * DEVICEINFOSIGN scan found nothing. Before assuming why, dump ground truth:
 * scan bank 0's search region and report, by read result, how many pages
 * were blank / OK / corrected / ECC-failed / hw-error, and the first 16
 * bytes of the first non-blank page found. This answers empirically whether
 * we can read Apple's data at all and, if so, what the pages contain. */
struct apple_rawscan_info
{
    unsigned int scanned;
    unsigned int blank;      /* NAND_ECC_CLEAN (all 0xFF) */
    unsigned int okpages;    /* NAND_ECC_OK */
    unsigned int corrected;  /* NAND_ECC_CORRECTED */
    unsigned int eccfailed;  /* NAND_ECC_FAILED */
    unsigned int hwerr;      /* negative rc */
    uint32_t     first_nonblank_page;
    int          first_nonblank_rc;
    uint8_t      first_nonblank_head[16];
    int          found_first;
};

void apple_ftl_rawscan(struct apple_rawscan_info *out)
{
    uint32_t blocks, lowest, block, page, pagenum;
    unsigned int i;

    memset(out, 0, sizeof(*out));

    /* Ensure geometry is set even if a prior ftl_init isn't in effect. */
    a_bank_count = nand_get_bank_count();
    chip_geo = a_bank_count ? nand_get_bank_geometry(0) : NULL;
    if (!chip_geo)
        return;
    a_page_size       = chip_geo->page_size;
    a_pages_per_block = chip_geo->pages_per_block;
    a_blocks_per_bank = chip_geo->blocks_per_bank;
    if (a_page_size == 0 || a_pages_per_block == 0 || a_blocks_per_bank == 0)
        return;

    blocks = a_blocks_per_bank;
    lowest = blocks - (blocks / 10);

    /* Scan the same region apple_find_devinfo does (bank 0, last 10%, last 8
     * pages/block), reading every page regardless of ECC result. */
    for (block = blocks - 1; block >= lowest && block < blocks; block--)
    {
        for (page = a_pages_per_block - 8; page < a_pages_per_block; page++)
        {
            int rc;
            pagenum = block * a_pages_per_block + page;
            rc = nand_hw_read_page(0, pagenum, a_pagebuf, a_sparebuf);
            out->scanned++;
            if (rc < 0)
                out->hwerr++;
            else if (rc == NAND_ECC_CLEAN)
                out->blank++;
            else if (rc == NAND_ECC_OK)
                out->okpages++;
            else if (rc == NAND_ECC_CORRECTED)
                out->corrected++;
            else if (rc == NAND_ECC_FAILED)
                out->eccfailed++;

            /* Capture the first page that isn't a clean/blank read, whatever
             * its ECC verdict -- that's where real data (or the magic) would
             * be. We read the DATA bytes regardless of ECC verdict. */
            if (!out->found_first && rc != NAND_ECC_CLEAN)
            {
                out->found_first = 1;
                out->first_nonblank_page = pagenum;
                out->first_nonblank_rc = rc;
                for (i = 0; i < 16; i++)
                    out->first_nonblank_head[i] = a_pagebuf[i];
            }
        }
    }
}

/* --- System-block page dump (read-only) -----------------------------
 * The devinfo lists system blocks 4095..4092, but their page-0 spare type
 * is A5/FF, not the 2G's 0x80 -- so the VFL context is at a different page
 * or uses a different type. Dump, for a given block, each of the first N
 * pages: the spare type byte + the first 16 data bytes, to locate the VFL
 * context and read its structure. Read-only. */
struct apple_blockdump_info
{
    uint32_t block;
    unsigned int npages;         /* pages dumped (<= 16) */
    uint8_t  type[16];           /* spare type byte per page */
    int      rc[16];             /* read rc per page */
    uint8_t  head[16][16];       /* first 16 data bytes per page */
};

void apple_ftl_blockdump(uint32_t block, struct apple_blockdump_info *out)
{
    unsigned int p;

    memset(out, 0, sizeof(*out));
    out->block = block;

    a_bank_count = nand_get_bank_count();
    chip_geo = a_bank_count ? nand_get_bank_geometry(0) : NULL;
    if (!chip_geo)
        return;
    a_page_size       = chip_geo->page_size;
    a_pages_per_block = chip_geo->pages_per_block;
    a_blocks_per_bank = chip_geo->blocks_per_bank;
    if (a_pages_per_block == 0 || block >= a_blocks_per_bank)
        return;

    out->npages = a_pages_per_block < 8 ? a_pages_per_block : 8;
    for (p = 0; p < out->npages; p++)
    {
        int rc = nand_hw_read_page(0, block * a_pages_per_block + p,
                                   a_pagebuf, a_sparebuf);
        unsigned int b;
        out->rc[p] = rc;
        if (apple_read_ok(rc))
        {
            out->type[p] = ((const struct apple_spare_meta *)a_sparebuf)->type;
            for (b = 0; b < 16; b++)
                out->head[p][b] = a_pagebuf[b];
        }
    }
}

/* --- System-block generation probe (read-only) ---------------------
 * Apple/Whimory context records are conventionally written in generations
 * of eight pages. Block 4091's first generation (pages 0..7) is a repeated
 * NANDCRI record; sample the first page of every generation (0,8,..,120)
 * to find a later record without touching NAND. Only two leading data bytes
 * are retained: the diagnostic report is one sector, and a binary VFL page
 * should diverge from the NANDCRI (4e 41...) signature immediately. */
struct apple_blockgroup_info
{
    uint32_t block;
    unsigned int ngroups;        /* sampled eight-page generations (<= 16) */
    uint8_t  type[16];
    uint8_t  head[16][2];
    int      rc[16];
};

void apple_ftl_blockgroup_probe(uint32_t block,
                                 struct apple_blockgroup_info *out)
{
    unsigned int g;

    memset(out, 0, sizeof(*out));
    out->block = block;

    a_bank_count = nand_get_bank_count();
    chip_geo = a_bank_count ? nand_get_bank_geometry(0) : NULL;
    if (!chip_geo)
        return;
    a_page_size       = chip_geo->page_size;
    a_pages_per_block = chip_geo->pages_per_block;
    a_blocks_per_bank = chip_geo->blocks_per_bank;
    if (a_pages_per_block == 0 || block >= a_blocks_per_bank)
        return;

    out->ngroups = (a_pages_per_block + 7) / 8;
    if (out->ngroups > 16)
        out->ngroups = 16;
    for (g = 0; g < out->ngroups; g++)
    {
        int rc = nand_hw_read_page(0, block * a_pages_per_block + g * 8,
                                   a_pagebuf, a_sparebuf);
        out->rc[g] = rc;
        if (apple_read_ok(rc))
        {
            out->type[g] = ((const struct apple_spare_meta *)a_sparebuf)->type;
            out->head[g][0] = a_pagebuf[0];
            out->head[g][1] = a_pagebuf[1];
        }
    }
}

/* --- Strict all-bank VFL context locator (read-only) -----------------
 * The dump-derived Nano 3G format uses a 0x800-byte VFL context, strict
 * additive+XOR checksums at 0x7f8/0x7fc, and eight redundant pages at the
 * start of a context block. The historical Nano 3G implementation searches
 * physical blocks 1..199 independently on every bank.
 *
 * A 4 KiB physical page may contain either of two 2 KiB Apple units, while
 * the current low-level API exposes one 12-byte metadata result for the
 * physical read. Therefore validate both 0x800-byte halves independently
 * and count the spare 00/80 marker separately. A checksum-valid half is
 * reported even if that marker does not agree. This function only calls
 * nand_hw_read_page(); it cannot program or erase NAND. */
#define APPLE_LOCATOR_BLOCK_LIMIT 200
#define APPLE_VFL_CXT_BYTES       0x800
#define APPLE_VFL_CKSUM_OFFSET    0x7f8
#define APPLE_VFL_RING_OFFSET     0x694

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

static uint16_t apple_get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t apple_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool apple_vfl_strict_checksum(const uint8_t *cxt)
{
    uint32_t sum = 0xAABBCCDD;
    uint32_t x = 0xAABBCCDD;
    unsigned int off;

    for (off = 0; off < APPLE_VFL_CKSUM_OFFSET; off += 4)
    {
        uint32_t w = apple_get_le32(cxt + off);
        sum += w;
        x ^= w;
    }
    return sum == apple_get_le32(cxt + APPLE_VFL_CKSUM_OFFSET)
        && x == apple_get_le32(cxt + APPLE_VFL_CKSUM_OFFSET + 4);
}

void apple_ftl_vfl_locator(struct apple_vfl_locator_info *out)
{
    unsigned int bank;

    memset(out, 0, sizeof(*out));
    out->banks = nand_get_bank_count();
    if (out->banks > NAND_MAX_BANKS)
        out->banks = NAND_MAX_BANKS;

    chip_geo = out->banks ? nand_get_bank_geometry(0) : NULL;
    if (!chip_geo || chip_geo->pages_per_block == 0)
        return;
    out->page_size = chip_geo->page_size;

    for (bank = 0; bank < out->banks; bank++)
    {
        const struct nand_geometry *geo = nand_get_bank_geometry(bank);
        struct apple_vfl_locator_bank *r = &out->bank[bank];
        uint32_t block;

        if (!geo || geo->page_size < APPLE_VFL_CXT_BYTES
            || geo->page_size > NAND_MAX_PAGE_SIZE
            || geo->pages_per_block < 8)
            continue;

        for (block = 1;
             block < APPLE_LOCATOR_BLOCK_LIMIT
                 && block < geo->blocks_per_bank;
             block++)
        {
            unsigned int page;
            for (page = 0; page < 8; page++)
            {
                const struct apple_spare_meta *m;
                unsigned int halves, half;
                int rc = nand_hw_read_page(bank,
                           block * geo->pages_per_block + page,
                           a_pagebuf, a_sparebuf);

                r->reads++;
                if (!apple_read_ok(rc))
                    continue;
                r->readable++;
                m = (const struct apple_spare_meta *)a_sparebuf;
                if (m->field_8 == 0 && m->type == APPLE_TYPE_VFLCXT)
                    r->marker_hits++;

                halves = geo->page_size / APPLE_VFL_CXT_BYTES;
                if (halves > 2)
                    halves = 2;
                for (half = 0; half < halves; half++)
                {
                    const uint8_t *cxt = a_pagebuf
                                       + half * APPLE_VFL_CXT_BYTES;
                    if (!apple_vfl_strict_checksum(cxt))
                        continue;
                    r->strict_hits++;
                    if (m->field_8 == 0 && m->type == APPLE_TYPE_VFLCXT)
                        r->both_hits++;
                    if (!r->found)
                    {
                        unsigned int i;
                        r->found = 1;
                        r->first_block = block;
                        r->first_page = page;
                        r->first_half = half;
                        r->first_type = m->type;
                        r->first_field8 = m->field_8;
                        r->first_spare_usn = m->usn;
                        r->first_payload_usn = apple_get_le32(cxt);
                        for (i = 0; i < 4; i++)
                            r->ring[i] = apple_get_le16(cxt
                                + APPLE_VFL_RING_OFFSET + i * 2);
                    }
                }
            }
        }
    }
}

/* --- System-region type map (read-only) -----------------------------
 * Block 4095 is all devinfo (type A5); no VFL context (2G type 0x80) was
 * found there. Map the page-0 spare type of a range of high blocks to find
 * where the VFL context (and other Whimory metadata) actually lives on the
 * 3G. Reports one type byte per block for [start, start+count). Also flags,
 * per block, whether ANY of its first 8 pages has a non-A5/non-FF type (a
 * candidate metadata block). Read-only. */
struct apple_typemap_info
{
    uint32_t start;
    unsigned int count;          /* blocks mapped (<= 32) */
    uint8_t  page0_type[32];
    uint8_t  interesting[32];    /* 1 if a non-FF/non-A5 type seen in pages 0..7 */
    uint8_t  interesting_type[32];
    uint8_t  interesting_page[32];
};

void apple_ftl_typemap(uint32_t start, struct apple_typemap_info *out)
{
    unsigned int i, p;

    memset(out, 0, sizeof(*out));
    out->start = start;

    a_bank_count = nand_get_bank_count();
    chip_geo = a_bank_count ? nand_get_bank_geometry(0) : NULL;
    if (!chip_geo)
        return;
    a_page_size       = chip_geo->page_size;
    a_pages_per_block = chip_geo->pages_per_block;
    a_blocks_per_bank = chip_geo->blocks_per_bank;
    if (a_pages_per_block == 0)
        return;

    out->count = 32;
    for (i = 0; i < 32; i++)
    {
        uint32_t blk = start + i;
        if (blk >= a_blocks_per_bank)
        {
            out->count = i;
            break;
        }
        for (p = 0; p < 8 && p < a_pages_per_block; p++)
        {
            int rc = nand_hw_read_page(0, blk * a_pages_per_block + p,
                                       a_pagebuf, a_sparebuf);
            uint8_t t;
            if (!apple_read_ok(rc))
                continue;
            t = ((const struct apple_spare_meta *)a_sparebuf)->type;
            if (p == 0)
                out->page0_type[i] = t;
            if (t != 0xFF && t != 0xA5 && !out->interesting[i])
            {
                out->interesting[i] = 1;
                out->interesting_type[i] = t;
                out->interesting_page[i] = p;
            }
        }
    }
}

/* Read-only read-path probe: runs the full mount, reports whether it
 * mounted, the sector count, and the first 16 bytes of chosen logical
 * sectors (sector 0 is the MBR on a stock unit, ending in 55 AA). Lets a
 * diagnostic validate that the VFL+FTL read path returns real filesystem
 * data, not just that the mount structures decoded. Read-only. */
struct apple_read_probe_info
{
    int          mount_rc;
    bool         mounted;
    uint32_t     num_sectors;
    uint32_t     usersb;
    uint32_t     nsuperblocks;
    uint32_t     sbpages;
    uint16_t     map0;             /* a_map[0], the first logical block */
    int          sector0_rc;
    uint8_t      sector0_head[16]; /* first 16 bytes of logical sector 0 */
    uint8_t      sector0_tail[4];  /* bytes 508..511 (MBR signature area) */
    uint16_t     ftlctrl[3];       /* FTL control superblocks */
    uint32_t     ctrl_used[3];     /* pages scanned per control block */
    int          ctrl_last[3];     /* last page's spare type per control block */
    uint32_t     newblk;           /* chosen newest control block, or 3 */
    int          ftl_clean;        /* 1 clean, 0 needs restore */
    uint32_t     ctrl_bank[3];     /* page-0 physical bank per control block */
    uint32_t     ctrl_page[3];     /* page-0 physical page per control block */
    int          ctrl_rc[3];       /* raw read rc at that page */
    uint8_t      ctrl_type[3];     /* spare type there (0xFF if unreadable) */
    uint8_t      layout;           /* detected block layout */
    uint8_t      planes;
    uint32_t     dbg_nsuperblocks;
    uint32_t     dbg_vflspares;
    uint32_t     dbg_unit_block;   /* apple_unit_block(0, ftlctrl[1]) */
    uint32_t     dbg_phys_block;   /* apple_vfl_phys_block(0,0,ftlctrl[1]) */
    uint16_t     dbg_remap[6];     /* first remap entries of bank 0 */
    uint16_t     dbg_usedcount[4]; /* per-plane used remap slots, bank 0 */
    uint16_t     dbg_ring0;        /* bank 0 vflcxtblocks[0] */
    uint32_t     scan_firstnz;     /* first non-blank/non-FF logical sector */
    uint8_t      scan_nzhead[16];  /* its first 16 bytes */
    uint8_t      scan_sigfound;    /* a 55 AA boot signature seen while sampling */
    uint32_t     scan_sigsector;   /* which sector had it */
    uint8_t      scan_sighead[16]; /* first 16 bytes of that sector */
    uint32_t     scan_nzcount;     /* how many sampled sectors were non-blank */
    uint32_t     scan_nzoff;       /* offset of first non-zero byte in nzhead */
    /* Dense low-sector scan: first 256 logical sectors, looking for Apple
     * partition-map ("ER"/0x4552, "PM"/0x504D) or FAT (55 AA) signatures. */
    uint32_t     low_firstnz;      /* first non-blank sector in 0..255 */
    uint8_t      low_head[16];     /* its first 16 bytes */
    uint32_t     apm_sector;       /* sector whose first 2 bytes are ER/PM */
    uint16_t     apm_sig;          /* those 2 bytes, or 0 */
};

void apple_ftl_read_probe(struct apple_read_probe_info *out)
{
    static uint8_t sbuf[NAND_PAGE_SIZE] NAND_DMA_BUF_ATTR;
    int rc;
    unsigned int i;

    memset(out, 0, sizeof(*out));
    out->mount_rc = ftl_init();
    out->mounted  = (out->mount_rc == 0) && a_mounted;
    out->num_sectors = ftl_num_sectors();
    out->usersb = a_usersb;
    out->nsuperblocks = a_nsuperblocks;
    out->sbpages = a_sbpages;
    out->map0 = a_mounted ? a_map[0] : 0xFFFF;
    for (i = 0; i < 3; i++)
    {
        out->ftlctrl[i]   = a_ftl_ctrl[i];
        out->ctrl_used[i] = a_dbg_ctrl_used[i];
        out->ctrl_last[i] = a_dbg_ctrl_last[i];
    }
    out->newblk    = a_dbg_newblk;
    out->ftl_clean = a_dbg_ftl_clean;
    out->layout    = (uint8_t)a_geo.layout;
    out->planes    = (uint8_t)a_geo.planes;

    /* Remap-table + addressing dump for control superblock ftlctrl[1]
     * (sb 3 on the observed units), to see why phys_block lands in the
     * reserved/DeviceInfo region. */
    out->dbg_nsuperblocks = a_nsuperblocks;
    out->dbg_vflspares    = a_geo.vflspares;
    if (a_geo.valid && a_bank_count > 0)
    {
        uint32_t sb = a_ftl_ctrl[1];
        out->dbg_unit_block = apple_unit_block(0, sb);
        out->dbg_phys_block = apple_vfl_phys_block(0, 0, sb);
        for (i = 0; i < 6; i++)
            out->dbg_remap[i] = a_vfl[0].remap[i];
        for (i = 0; i < 4; i++)
            out->dbg_usedcount[i] = a_vfl[0].usedcount[i];
        out->dbg_ring0 = a_vfl[0].vflcxtblocks[0];
    }

    /* Address dump: for each FTL control superblock, compute where page 0
     * lands (bank, physical page) and the raw read result there. Shows if an
     * unreadable control block is an addressing fault vs a genuine bad read. */
    for (i = 0; i < 3; i++)
    {
        uint32_t bank = 0, page = 0;
        int rrc = -999;
        if (a_mounted || a_geo.valid)
        {
            apple_vpage_phys(a_ftl_ctrl[i], 0, &bank, &page);
            rrc = nand_hw_read_page(bank, page, sbuf, a_sparebuf);
        }
        out->ctrl_bank[i] = bank;
        out->ctrl_page[i] = page;
        out->ctrl_rc[i]   = rrc;
        out->ctrl_type[i] = apple_read_ok(rrc) ? APPLE_META_TYPE(a_sparebuf)
                                               : 0xFF;
    }
    if (!out->mounted)
    {
        out->sector0_rc = -1;
        return;
    }
    rc = apple_ftl_read_sector(0, sbuf);
    out->sector0_rc = rc;
    if (rc == 0)
    {
        memcpy(out->sector0_head, sbuf, 16);
        memcpy(out->sector0_tail, sbuf + NAND_PAGE_SIZE - 4, 4);
    }

    /* Locate real filesystem content across the whole volume: sample sectors
     * on a stride, report the first non-blank/non-FF sector + its head, count
     * how many samples were non-blank, and whether any sampled sector ends in
     * the 55 AA boot signature. A wide stride finds the partition table / FAT
     * boot sector wherever it sits in the FTL's logical space. */
    {
        uint32_t s, total = ftl_num_sectors();
        uint32_t stride = total / 4096;   /* ~4096 samples across the volume */
        if (stride == 0)
            stride = 1;
        out->scan_firstnz = 0xFFFFFFFF;
        out->scan_nzcount = 0;
        for (s = 0; s < total; s += stride)
        {
            unsigned int j;
            bool nonzero = false, notff = false;
            if (apple_ftl_read_sector(s, sbuf) != 0)
                continue;
            if (sbuf[NAND_PAGE_SIZE - 2] == 0x55
                && sbuf[NAND_PAGE_SIZE - 1] == 0xAA && !out->scan_sigfound)
            {
                out->scan_sigfound = 1;
                out->scan_sigsector = s;
                memcpy(out->scan_sighead, sbuf, 16);
            }
            for (j = 0; j < NAND_PAGE_SIZE; j++)
            {
                if (sbuf[j] != 0x00) nonzero = true;
                if (sbuf[j] != 0xFF) notff = true;
            }
            (void)notff;
            if (nonzero && notff)
            {
                out->scan_nzcount++;
                if (out->scan_firstnz == 0xFFFFFFFF)
                {
                    unsigned int k = 0;
                    out->scan_firstnz = s;
                    /* Capture 16 bytes starting at the first non-zero byte,
                     * so a sector whose leading bytes are zero still shows
                     * its real content. */
                    while (k < NAND_PAGE_SIZE && sbuf[k] == 0x00)
                        k++;
                    out->scan_nzoff = k;
                    for (j = 0; j < 16 && k + j < NAND_PAGE_SIZE; j++)
                        out->scan_nzhead[j] = sbuf[k + j];
                }
            }
        }
    }

    /* Dense scan of the first 256 logical sectors: find the first non-blank
     * one and its head, and look for Apple Partition Map ("ER" 0x4552 in the
     * driver descriptor, "PM" 0x504D in a map entry) or FAT (55 AA) sigs.
     * These live at the very start of an iPod's disk in host-sector space. */
    {
        uint32_t s, total = ftl_num_sectors();
        out->low_firstnz = 0xFFFFFFFF;
        for (s = 0; s < 256 && s < total; s++)
        {
            unsigned int j;
            bool nonzero = false;
            uint16_t be0;
            if (apple_ftl_read_sector(s, sbuf) != 0)
                continue;
            be0 = ((uint16_t)sbuf[0] << 8) | sbuf[1];   /* big-endian first 2 */
            if ((be0 == 0x4552 || be0 == 0x504D) && !out->apm_sig)
            {
                out->apm_sig = be0;
                out->apm_sector = s;
            }
            if (sbuf[NAND_PAGE_SIZE - 2] == 0x55
                && sbuf[NAND_PAGE_SIZE - 1] == 0xAA && !out->scan_sigfound)
            {
                out->scan_sigfound = 1;
                out->scan_sigsector = s;
                memcpy(out->scan_sighead, sbuf, 16);
            }
            for (j = 0; j < NAND_PAGE_SIZE; j++)
                if (sbuf[j] != 0x00) { nonzero = true; break; }
            if (nonzero && out->low_firstnz == 0xFFFFFFFF)
            {
                out->low_firstnz = s;
                for (j = 0; j < 16; j++)
                    out->low_head[j] = sbuf[j];
            }
        }
    }
}

int apple_ftl_vfl_mount_info(struct apple_vfl_mount_info *out)
{
    unsigned int i;
    int rc = ftl_init();   /* runs the full read-only mount (VFL + FTL) */

    memset(out, 0, sizeof(*out));
    out->banks           = a_bank_count;
    out->page_size       = a_page_size;
    out->pages_per_block  = a_pages_per_block;
    out->blocks_per_bank  = a_blocks_per_bank;
    out->vfl_usn_max      = a_vfl_usn_max;
    out->user_blocks      = a_usersb;      /* user superblocks */
    out->devinfo_captured = a_devinfo_captured;
    out->ftl_mounted      = a_mounted;
    memcpy(out->devinfo_head, a_devinfo_head, sizeof(out->devinfo_head));
    for (i = 0; i < a_bank_count && i < NAND_MAX_BANKS; i++)
    {
        /* Report the per-bank VFL ring's first two blocks + used-count[0],
         * which are meaningful under the corrected layout. */
        out->spareused[i]   = a_vfl[i].usedcount[0];
        out->firstspare[i]  = a_vfl[i].vflcxtblocks[0];
        out->sparecount[i]  = a_vfl[i].vflcxtblocks[1];
    }
    if (a_bank_count > 0)
        memcpy(out->ftlctrlblocks, a_ftl_ctrl, sizeof(out->ftlctrlblocks));

    /* rc == 0 means the full read-only mount (VFL + FTL context + map)
     * succeeded. Any negative means the mount failed at some stage; the
     * fields filled above still show how far it got. */
    return (rc == 0) ? 0 : 1;
}

uint32_t ftl_num_sectors(void)
{
    if (!a_mounted)
        return 0;
    /* Logical sectors = user superblocks * pages/superblock * host sectors
     * per physical page. */
    return (uint32_t)a_usersb * a_sbpages * a_sectors_per_page;
}

int ftl_read(uint32_t sector, uint32_t count, void *buffer)
{
    uint8_t *dst = (uint8_t *)buffer;
    uint32_t total_sectors;
    int error = 0;
    uint32_t i;

    if (!a_mounted)
        return -1;

    total_sectors = (uint32_t)a_usersb * a_sbpages * a_sectors_per_page;
    if (sector + count > total_sectors || count == 0)
        return (count == 0) ? 0 : -2;

    for (i = 0; i < count; i++)
    {
        int rc = apple_ftl_read_sector(sector + i,
                                       dst + (size_t)i * NAND_PAGE_SIZE);
        if (rc != 0)
            error = -4;   /* keep going; caller sees the volume-level error */
    }
    return error;
}

int ftl_write(uint32_t sector, uint32_t count, const void *buffer)
{
    (void)sector; (void)count; (void)buffer;
    /* Read-only until Phase 4, and even then gated behind heavy safeguards. */
    return -1;
}

int ftl_sync(void)
{
    /* Nothing to flush in a read-only build. */
    return 0;
}

bool ftl_readonly_mount(void)
{
    /* Always read-only for now (Phases 1-3). */
    return true;
}
