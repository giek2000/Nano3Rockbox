/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") flash translation layer.
 *
 * This is an original design, not a reimplementation of Apple's Whimory
 * FTL/VFL. Apple's on-flash format is proprietary and undocumented; rather
 * than guess at it (with the risk of silent data loss or excess wear if
 * the guess were wrong), this FTL defines its own on-flash layout and
 * algorithm:
 *
 *   - Block-level indirection: each logical block (one NAND erase block's
 *     worth of logical sectors) maps to exactly one physical erase block
 *     at a time. Writes to any sector within a logical block cause the
 *     whole block to be re-written into a freshly erased physical block
 *     (read-modify-write at block granularity), after which the old
 *     physical block is erased and returned to the free pool.
 *   - Every page carries a small header in its spare/OOB area: a magic
 *     number, the logical block it belongs to, a monotonically increasing
 *     per-logical-block generation counter, and its page index within the
 *     block. Mounting scans every block's headers and, for each logical
 *     block, keeps the physical block with the highest generation whose
 *     headers are fully self-consistent (all pages present, matching
 *     magic/logical-block/generation). This makes the layout self-
 *     describing: no separate superblock commit protocol is needed, and
 *     a power loss during a block rewrite is safe by construction, since
 *     the old physical block for that logical block is never erased until
 *     the new one has been completely and verifiably written.
 *   - A small pool of erase blocks (roughly 3% of total, minimum 4) is
 *     held back from the logical address space as free/spare capacity to
 *     absorb wear-leveling churn and future bad blocks discovered at
 *     runtime.
 *   - Wear-leveling is a simple least-erased-first free block picker;
 *     blocks that fail an erase or program are marked bad (via a sentinel
 *     header pattern) and permanently excluded.
 *
 * This trades some write amplification (whole-block rewrite on any write)
 * for a design that is simple enough to reason about and to test
 * exhaustively against a simulated NAND before ever touching real
 * hardware -- prioritising correctness and auditability over matching
 * Apple's factory write patterns.
 */

#ifndef __FTL_TARGET_H__
#define __FTL_TARGET_H__

#include <stdint.h>
#include <stdbool.h>

#if defined(BOOTLOADER) && !defined(HAVE_BOOTLOADER_USB_MODE)
/* A bootloader that only loads and starts firmware (or, in the NAND check
 * image, serves raw NAND read-only over USB) never needs write support.
 *
 * A bootloader built WITH HAVE_BOOTLOADER_USB_MODE does, though: it exposes
 * this storage to a USB host as a mass-storage device, and that host has to
 * be able to partition it, put a filesystem on it and copy files to it --
 * on a freshly-ported device that has never been formatted, the bootloader
 * is in fact the only thing that can offer that, since the main firmware
 * lives on the very filesystem that doesn't exist yet.
 *
 * Keeping FTL_READONLY defined here was a real, confirmed-on-hardware bug:
 * every host write came back as a SCSI medium error (surfacing on Windows
 * as a CRC error, and from its storage service as an opaque failure) while
 * reads worked perfectly, because nand_write_sectors() refused the write
 * before the FTL ever saw it. Note this is independent of the
 * chip-validation gate below, which ftl-nano3g.c applies separately --
 * an unvalidated chip is still mounted read-only either way. */
#define FTL_READONLY
#endif

/* Scan all banks' blocks and rebuild the logical block map in RAM. Must be
 * called after nand_scan_banks() has found at least one usable bank.
 * Returns 0 on success, or a negative value (see below) on failure. */
int ftl_init(void);

enum ftl_init_error
{
    FTL_ERR_NO_BANKS       = -1,  /* nand_scan_banks() found nothing */
    FTL_ERR_UNRECOGNIZED   = -2,  /* bank 0's chip wasn't confidently ID'd */
    FTL_ERR_TOO_SMALL      = -3,  /* fewer good blocks than our minimum */
    FTL_ERR_ALLOC          = -4,  /* couldn't allocate the RAM block map */
};

/* Read count logical sectors (NAND_PAGE_SIZE bytes each) starting at
 * sector into buffer. Unmapped logical blocks read back as all-0xFF
 * (as if erased), matching NAND's natural unwritten state. Returns 0, or
 * a negative value on an unrecoverable read (uncorrectable/hardware
 * failure on every candidate copy -- should not happen absent hardware
 * fault, since only fully-verified blocks are ever selected as valid). */
int ftl_read(uint32_t sector, uint32_t count, void *buffer);

/* Write count logical sectors starting at sector from buffer. Returns 0,
 * or a negative value if the FTL is mounted read-only or ran out of free
 * blocks. Data is safely committed to flash before this returns (each
 * logical block rewrite is a single, individually crash-safe operation);
 * there is no separate "unsynced" state to flush, but ftl_sync() is still
 * provided for API symmetry with other Rockbox storage backends and to
 * allow a future write-coalescing optimisation without changing callers. */
int ftl_write(uint32_t sector, uint32_t count, const void *buffer);

/* No-op beyond returning any latent error from the last write, since
 * ftl_write() commits synchronously. Returns 0 normally. */
int ftl_sync(void);

/* Total logical sectors exposed, 0 if ftl_init() has not succeeded. */
uint32_t ftl_num_sectors(void);

/* True if writes are refused: either a read-only build (see FTL_READONLY
 * above -- a plain bootloader, but NOT one serving USB mass storage), or
 * the mounted chip wasn't one we're confident enough in to risk writing. */
bool ftl_readonly_mount(void);

#endif /* __FTL_TARGET_H__ */
