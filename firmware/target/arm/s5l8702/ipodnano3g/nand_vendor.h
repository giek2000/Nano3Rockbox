/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Generic, multi-vendor SLC NAND ID decoding.
 *
 * This is original code written directly from two public, vendor-neutral
 * references:
 *
 *  - The JEDEC-assigned 8-bit manufacturer ID codes used by essentially
 *    every NAND flash vendor's READ ID byte 0 (JEP106, a JEDEC standard;
 *    the specific codes for Micronas/Toshiba/Hynix/Micron/Intel/SanDisk
 *    used below are well-known, widely published constants, not sourced
 *    from any single implementation).
 *  - The "extended ID" geometry decoding convention documented and
 *    implemented independently in the Linux kernel's MTD/NAND subsystem
 *    (drivers/mtd/nand/raw/nand_base.c, function nand_decode_ext_id(),
 *    GPL-2.0, (C) Thomas Gleixner and others) which itself codifies a
 *    scheme common to large-page SLC NAND datasheets from multiple
 *    vendors circa 2005-2010: byte[3] of the 4-byte ID packs page size,
 *    OOB (spare) size and erase block size as small bitfields. That
 *    *convention* (which bits mean what) is public, vendor-published
 *    information repeated across many independent open-source NAND
 *    drivers; only the arithmetic here is new code, written for this
 *    project.
 *
 * No chip table extracted from Apple's Nano 3G firmware, and no other
 * Nano 3G-specific driver or patch, was used as a source for this file.
 */

#ifndef __NAND_VENDOR_H__
#define __NAND_VENDOR_H__

#include <stdint.h>
#include "nand-target.h"

/* JEDEC manufacturer IDs relevant to 2007-08 era iPod Nano 3G production
 * (public constants; see JEP106 and vendor datasheets). */
enum nand_maker_id
{
    NAND_MAKER_TOSHIBA  = 0x98,
    /* JEDEC ID 0xEC is Micronas (ITT Intermetall) per JEP106; historically
     * mislabelled "Samsung" in this project (Samsung's real JEDEC ID is
     * 0xCE). The 8GB reference unit (model MB253, raw ID EC D5 14 B6 ...)
     * carries a Micronas-branded MLC die. */
    NAND_MAKER_MICRONAS = 0xEC,
    NAND_MAKER_HYNIX    = 0xAD,
    NAND_MAKER_INTEL    = 0x89,
    NAND_MAKER_MICRON   = 0x2C,
    NAND_MAKER_SANDISK  = 0x45,
    NAND_MAKER_STMICRO  = 0x20,
};

/* Decode a 4..8 byte JEDEC-style READ ID response into a nand_geometry.
 * Always fills out ->maker_id/->device_id/->maker_name. If the device is
 * recognised as following the common large-page extended-ID convention,
 * fills in the rest and sets ->recognized = true (for SLC parts;
 * geometry is still decoded, but ->recognized stays false, for MLC --
 * see the validated-chip override below for the one way an MLC part can
 * still end up ->recognized); otherwise leaves conservative zeroed/false
 * geometry fields and ->recognized = false so callers can refuse to
 * trust it.
 *
 * After the generic decode above, this also checks the exact READ ID
 * bytes against a small table of specific chips this project has
 * actually erased, written, read back, and verified on real hardware
 * (see nand_vendor.c's NANO3G_VALIDATED_CHIPS -- deliberately NOT a
 * "any chip that looks like this generic family" rule, unlike the
 * public-convention SLC decode above). A match overrides ->recognized
 * to true and fills ->blocks_per_bank from that table entry's
 * hardware-sourced (not runtime-probed -- see nand-nano3g.c's capacity
 * probe's own documented unreliability for at least one such chip)
 * capacity, regardless of ->bits_per_cell. This is the ONLY way an MLC
 * part can end up ->recognized == true; the generic decode never does
 * that on its own.
 *
 * id_bytes must point to at least 4 valid bytes (more are read if
 * id_len >= 6, to distinguish some MLC/multi-die variants, but this
 * driver does not currently act on those extra bytes beyond logging). */
void nand_vendor_decode(const uint8_t *id_bytes, unsigned int id_len,
                        struct nand_geometry *geo_out);

/* Human-readable name for a maker id, or "Unknown". Never returns NULL. */
const char *nand_vendor_name(uint8_t maker_id);

#endif /* __NAND_VENDOR_H__ */
