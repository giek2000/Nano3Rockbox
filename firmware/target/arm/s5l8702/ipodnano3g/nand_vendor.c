/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Generic, multi-vendor SLC NAND ID decoding -- implementation.
 *
 * See nand_vendor.h for provenance notes. This is original code.
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

#include "nand_vendor.h"
#include <string.h>

const char *nand_vendor_name(uint8_t maker_id)
{
    switch (maker_id)
    {
        case NAND_MAKER_TOSHIBA:  return "Toshiba";
        case NAND_MAKER_MICRONAS: return "Micronas";
        case NAND_MAKER_HYNIX:    return "Hynix";
        case NAND_MAKER_INTEL:    return "Intel";
        case NAND_MAKER_MICRON:   return "Micron";
        case NAND_MAKER_SANDISK:  return "SanDisk";
        case NAND_MAKER_STMICRO:  return "ST Micro";
        default:                  return "Unknown";
    }
}

/* Bits per cell from the 3rd ID byte's cell-type field. This 2-bit field
 * position (bits [3:2] of byte 2) is the same across the vendors' public
 * datasheets that use the large-page extended-ID scheme. 0 -> 1 bit/cell
 * (SLC), 1 -> 2 bits/cell (MLC), etc. */
static unsigned int decode_bits_per_cell(uint8_t byte2)
{
    unsigned int field = (byte2 >> 2) & 0x3;
    return field + 1;
}

/* Large-page extended-ID geometry decode, byte[3] of the READ ID response.
 * This bit layout (page size in bits[1:0], spare-per-512B in bit[2],
 * eraseblock size in bits[5:4], bus width in bit[6]) is the common
 * convention documented independently by multiple large-page SLC NAND
 * vendors' datasheets circa 2005-2010 and is reproduced by numerous
 * independent open-source NAND drivers (e.g. Linux's nand_decode_ext_id());
 * it is public, vendor-neutral information about how these parts encode
 * their own identity, not proprietary chip-table content belonging to any
 * single implementation.
 *
 * Some vendors' higher-capacity single/multi-plane parts add extra
 * qualifying bits beyond this base scheme (e.g. Samsung's plane-count bit,
 * Hynix's extra OOB-size encoding on some 8Gb+ parts). Since the specific
 * chips soldered into any given Nano 3G unit are not knowable in advance,
 * this decoder intentionally sticks to the common base scheme and lets
 * the FTL layer's runtime validation (probing actual page/OOB reads
 * against the decoded geometry before trusting it for writes) catch any
 * part where the extended decode doesn't match reality, rather than
 * hard-coding per-vendor quirk tables that we have no way to verify
 * exhaustively. */
static void decode_large_page_ext_id(uint8_t ext_byte,
                                     struct nand_geometry *geo)
{
    unsigned int v = ext_byte;

    geo->page_size = 1024u << (v & 0x3);

    v >>= 2;
    /* spare (OOB) bytes per 512 bytes of page data: 8 or 16 */
    unsigned int spare_per_512 = 8u << (v & 0x1);
    geo->spare_size = spare_per_512 * (geo->page_size / 512u);

    v >>= 2;
    /* erase block size is a multiple of 64KiB */
    unsigned int block_bytes = (64u * 1024u) << (v & 0x3);
    geo->pages_per_block = block_bytes / geo->page_size;

    v >>= 2;
    geo->bus_width_16 = (v & 0x1) != 0;
}

/* Legacy (small-page, <=512B) parts predate the extended-ID scheme; their
 * geometry is fixed by the device ID byte alone per vendor datasheets.
 * The iPod Nano 3G (2007, 4/8GB, large-page chips only) is not expected to
 * contain any of these, but we still decode enough to report something
 * sane rather than silently guessing, so a check tool run against
 * unexpected hardware gets a diagnosable answer instead of garbage. */
static bool decode_legacy_id(uint8_t device_id, struct nand_geometry *geo)
{
    switch (device_id)
    {
        /* 8/16/32/64 MiB legacy parts, 512B page - listed for completeness,
         * not expected on this device. Sizes per common legacy datasheets. */
        case 0x6B: case 0xE3: case 0xE5: /* 4MiB class */
        case 0xD6: case 0xE6:            /* 8MiB class */
        case 0x33: case 0x73: case 0x43: case 0x53: /* 16MiB class */
        case 0x35: case 0x75: case 0x45: case 0x55: /* 32MiB class */
        case 0x36: case 0x76: case 0x46: case 0x56: /* 64MiB class */
            geo->page_size = 512;
            geo->spare_size = 16;
            geo->pages_per_block = 32;
            return true;
        default:
            return false;
    }
}

/* Chips this project has actually erased, written, read back, and
 * verified byte-for-byte correct on real hardware -- distinct from, and
 * a narrower claim than, the generic public-convention SLC decode
 * above. Unlike that generic decode (which trusts any part following
 * the common extended-ID bit layout, SLC only), a row here is a
 * specific-part override: it overrides ->recognized to true (the ONLY
 * path by which an MLC part can end up recognized) and supplies a
 * hardware-known blocks_per_bank, because the runtime capacity probe
 * (nand-nano3g.c:probe_bank_capacity_blocks()) is confirmed unreliable
 * for at least this chip (out-of-range reads alias back onto in-range
 * pages instead of erroring -- see NANO3G_ORIGINAL_NAND_FTL.md).
 *
 * Adding a row here is a claim this specific project has tested that
 * exact part, the same standard the reference driver's own
 * (unrelated, not copied from) validated chip table applies -- not a
 * claim that "any similar-looking part is probably fine too". A part
 * not in this table that also isn't a recognized-by-convention SLC
 * part stays ->recognized == false and read-only, however plausible
 * its generic geometry decode looks.
 *
 * blocks_per_bank for the Micronas row below (4096) comes from the
 * public device-ID numbering (device ID 0xD5 = 2GiB/16Gigabit,
 * the same convention independently reproduced in, among others, the
 * Linux kernel's nand_ids.c) divided by this chip's own measured
 * pages_per_block (128) * page_size (4096) = 512KiB/block -- not from
 * the unreliable runtime probe. */
struct nand_validated_chip
{
    uint8_t maker_id;
    uint8_t device_id;
    uint8_t ext_id_byte;      /* READ ID byte 3, as decoded by
                                  decode_large_page_ext_id() above */
    unsigned int blocks_per_bank;
    unsigned int expected_banks; /* chip-enable count this row was hardware-
                                    validated in; 0 = no constraint. Guards
                                    against a same-ext-ID part in a different,
                                    untested topology matching this row -- see
                                    struct nand_geometry.expected_banks. Makers
                                    whose 4GB/8GB variants share an ext-id and
                                    the same per-die geometry (only the die
                                    count differs) use 0; Intel A5D5D589 uses 2
                                    because its 8GB variant is a different,
                                    untested 4-CE topology. */
    const char *note;         /* for logging/diagnostics only */
};

static const struct nand_validated_chip nano3g_validated_chips[] =
{
    /* Micronas (JEDEC 0xEC; historically mislabelled "Samsung" -- Samsung's
     * real JEDEC ID is 0xCE), 2GiB/die MLC, device ID 0xD5, ext ID byte 0xB6
     * (raw READ ID: EC D5 14 B6 74 EC D5 14; page_size=4096, spare_size=128,
     * pages_per_block=128, x8) -- erased, written, read back and verified on
     * real hardware. This one row covers both populated-die counts of the same
     * die (expected_banks=0, so any present-CE count is accepted):
     *   - 4-die 4-CE 8GB unit (model MB253): all 4 banks / multiple blocks per
     *     bank (see NANO3G_ORIGINAL_NAND_FTL.md's "First real write/erase test"
     *     and its broader sweep).
     *   - 2-die 2-CE 4GB unit (model MB245): both present CEs, wsweep 8/8,
     *     wisolate 3/3, wstatus passed (collector v5.4.4, 2026-09-22).
     * Both are per-part validations on identical silicon, not a claim that
     * Micronas MLC in general is trusted. */
    { NAND_MAKER_MICRONAS, 0xD5, 0xB6, 4096, 0, "Micronas 2GiB/die MLC (4GB 2-CE / 8GB 4-CE)" },

    /* Hynix, 4-die MLC package (4GB Nano 3G unit, model MA978), device ID
     * 0xD3, ext ID byte 0xA5 (raw READ ID: AD D3 14 A5 64 AD D3 14;
     * the generic large-page decode gives page_size=2048, spare_size=64,
     * pages_per_block=128, x8 -- but leaves ->recognized false because it
     * is MLC, and the runtime capacity probe is unreliable for it,
     * reporting 16384 blocks with blocks_unreliable=1).
     *
     * Erased, written, read back and verified byte-for-byte (data AND
     * spare metadata) across ALL FOUR banks and multiple blocks per bank
     * on real hardware this session, via the -DNAND_CHECK write test +
     * sweep: "wtest erase 0 write 0 read 1 / data 1 meta 1", "wsweep
     * 16/16 passed" (read 1 = a benign correctable-ECC result, expected
     * on MLC). This row is that per-part validation, not a claim that
     * Hynix MLC in general is trusted. blocks_per_bank is 4096: this is
     * a 2048-byte-page part in a 4GB unit, i.e. four 1GiB dies; 1GiB /
     * (2048-byte page * 128 pages/block) = 256KiB/block -> 4096
     * blocks/bank, not the unreliable probe's 16384. */
    { NAND_MAKER_HYNIX, 0xD3, 0xA5, 4096, 0, "Hynix 4x1GiB MLC (4GB unit)" },

    /* Hynix, 4-die MLC package (8GB Nano 3G unit, model MB261), device ID
     * 0xD5, ext ID byte 0xA5 (raw READ ID: AD D5 55 A5 ...; the generic
     * large-page decode gives page_size=2048, spare_size=64,
     * pages_per_block=128, x8 -- but leaves ->recognized false because it
     * is MLC, and the runtime capacity probe is unreliable for it,
     * reporting 16384 blocks with blocks_unreliable=1). This exact ext-id
     * (A555D5AD) also appears in the reference Nano 3G FTL's own chip
     * table as an 8192-block/die part, corroborating the capacity below.
     *
     * Erased, written, read back and verified byte-for-byte (data AND
     * spare metadata) across ALL FOUR banks and multiple blocks per bank
     * on real hardware this session, via the -DNAND_CHECK write test +
     * sweep: "wtest erase 0 write 0 read 2 / data 1 meta 1", "wsweep
     * 16/16 passed" (read 2 = a benign correctable-ECC result, expected
     * on MLC). This row is that per-part validation, not a claim that
     * Hynix MLC in general is trusted. blocks_per_bank is the die's total
     * (8192), from which ftl_init() takes its own spare pool. */
    { NAND_MAKER_HYNIX, 0xD5, 0xA5, 8192, 0, "Hynix 4-die MLC (8GB unit)" },

    /* Toshiba, 4-die MLC package (8GB Nano 3G unit, model MB263), device ID
     * 0xD5, ext ID byte 0xBA (raw READ ID: 98 D5 94 BA ...; the generic
     * large-page decode gives page_size=4096, spare_size=64,
     * pages_per_block=128, x8 -- same geometry as the Micronas part, but
     * left ->recognized false by the generic decode because it is MLC, and
     * the runtime capacity probe is unreliable for it, reporting 16384
     * blocks with blocks_unreliable=1).
     *
     * Erased, written, read back and verified byte-for-byte (data AND
     * spare metadata) across ALL FOUR banks and multiple blocks per bank
     * on real hardware this session, via the -DNAND_CHECK write test +
     * sweep: "wtest erase 0 write 0 read 1 / data 1 meta 1", "wsweep
     * 16/16 passed" (read 1 = a benign correctable-ECC result, expected
     * on MLC). This row is that per-part validation, not a claim that
     * Toshiba MLC in general is trusted. blocks_per_bank is 4096: this is
     * a 4096-byte-page part on a 2 GiB die (device ID 0xD5 = 16Gbit), so
     * 2GiB / (4096-byte page * 128 pages/block) = 512KiB/block -> 4096
     * blocks/bank, the same figure and reasoning as the Micronas row above,
     * not the unreliable probe's 16384. */
    { NAND_MAKER_TOSHIBA, 0xD5, 0xBA, 4096, 0, "Toshiba 4-die MLC (8GB unit)" },

    /* Intel, 2-die MLC package (4GB Nano 3G unit, model MA978), device ID
     * 0xD5, ext ID byte 0xA5 (raw READ ID: 89 D5 D5 A5 68 ...; part
     * Intel JS29F32G08FAMB2. The generic large-page decode gives
     * page_size=2048, spare_size=64, pages_per_block=128, x8 -- but
     * leaves ->recognized false because it is MLC, and the runtime
     * capacity probe is unreliable for it, reporting 16384 blocks with
     * blocks_unreliable=1). Current upstream Rockbox's chip table and the
     * exact-part controller database both give this part 8192 blocks per
     * chip enable.
     *
     * Erased, written, read back and verified byte-for-byte (data AND
     * spare metadata) across both present chip enables and multiple
     * blocks per CE on real hardware, via the -DNAND_CHECK write test +
     * sweep: "wtest erase 0 write 0 read 2 / data 1 meta 1", "wsweep
     * 8/8 passed", "wisolate 3/3", "wstatus passed" (read 2 = a benign
     * correctable-ECC result, expected on MLC; 8/8 rather than 16/16
     * because this is a 2-CE part, not 4). This row is that per-part
     * validation, not a claim that Intel MLC in general is trusted, and
     * the bank-count match keeps it off the 4-CE 8GB Intel variant that
     * shares these three ID bytes. blocks_per_bank is the die's total
     * (8192): 2048-byte page * 128 pages/block * 8192 = 2 GiB/die, x2
     * CE = 4 GiB. */
    { NAND_MAKER_INTEL, 0xD5, 0xA5, 8192, 2, "Intel 2-die MLC (4GB unit)" },
};

/* Diagnostic-only geometry hints for exact parts that have been identified
 * but have not yet passed the project's erase/program/read-back validation.
 * These entries deliberately do NOT set ->recognized and therefore cannot
 * make the normal FTL/flasher writable. They only provide a conservative
 * capacity hint to NAND_CHECK diagnostics; the hint remains marked
 * unvalidated in the report until hardware testing establishes it.
 *
 * The Intel part below is from model MA978, with stable ID bytes
 * 89 D5 D5 A5 68 00 00 00 on two chip enables. Current upstream
 * Rockbox, the exact-part controller database, and the 4 GiB package
 * arithmetic all agree on 8192 blocks per chip enable: 2048 bytes/page *
 * 128 pages/block * 8192 blocks = 2 GiB/CE, and two CEs = 4 GiB.
 * This remains a read-only geometry hint; it does not validate writes,
 * Apple VFL/remap handling, or the exact part's bad-block policy. */
struct nand_diagnostic_chip
{
    uint8_t id[8];
    unsigned int blocks_per_bank;
    const char *note;
};

static const struct nand_diagnostic_chip nano3g_diagnostic_chips[] =
{
    /* Empty: the Intel JS29F32G08FAMB2 (89 D5 D5 A5 68, MA978 4GB) that
     * previously lived here as a read-only hint has been promoted to the
     * validated table above after passing the on-hardware write test and
     * sweep. New unverified exact parts can be listed here as read-only
     * diagnostic hints until they pass the same test. */
    { { 0, 0, 0, 0, 0, 0, 0, 0 }, 0, NULL },
};

static const struct nand_diagnostic_chip *
find_diagnostic_chip(const uint8_t *id_bytes, unsigned int id_len)
{
    unsigned int i;
    unsigned int count = sizeof(nano3g_diagnostic_chips)
                       / sizeof(nano3g_diagnostic_chips[0]);

    if (id_len < 8)
        return NULL;
    /* Reject an absent/floating bank (all 0x00, all 0xFF, or a zero maker
     * byte) so it can never match a zero sentinel slot below. Mirrors
     * nand-check-nano3g.c's nand_id_present(), inlined because that helper
     * is static and this file also builds into the host FTL test. */
    if (id_bytes[0] == 0x00 || id_bytes[0] == 0xFF)
        return NULL;

    for (i = 0; i < count; i++)
    {
        const struct nand_diagnostic_chip *c =
            &nano3g_diagnostic_chips[i];
        if (c->blocks_per_bank == 0)
            continue; /* sentinel / empty slot, never a real match */
        if (memcmp(c->id, id_bytes, sizeof(c->id)) == 0)
            return c;
    }
    return NULL;
}

/* Returns the matching table row, or NULL if none of the entries above
 * match this exact maker/device/ext-id triple. */
static const struct nand_validated_chip *
find_validated_chip(uint8_t maker_id, uint8_t device_id, uint8_t ext_id_byte)
{
    /* sizeof/sizeof, not system.h's ARRAYLEN, since this file is also
     * compiled into utils/ipodnano3g/ftltest2's native host test binary
     * (see that Makefile), which does not have Rockbox's system.h on
     * its include path. */
    unsigned int i;
    unsigned int count = sizeof(nano3g_validated_chips)
                        / sizeof(nano3g_validated_chips[0]);
    for (i = 0; i < count; i++)
    {
        const struct nand_validated_chip *c = &nano3g_validated_chips[i];
        if (c->maker_id == maker_id && c->device_id == device_id &&
            c->ext_id_byte == ext_id_byte)
            return c;
    }
    return NULL;
}

void nand_vendor_decode(const uint8_t *id_bytes, unsigned int id_len,
                        struct nand_geometry *geo_out)
{
    memset(geo_out, 0, sizeof(*geo_out));
    if (id_len < 2)
        return;

    geo_out->maker_id   = id_bytes[0];
    geo_out->device_id  = id_bytes[1];
    geo_out->maker_name = nand_vendor_name(geo_out->maker_id);

    if (id_len >= 3)
        geo_out->bits_per_cell = decode_bits_per_cell(id_bytes[2]);
    else
        geo_out->bits_per_cell = 1; /* assume SLC if we can't tell */

    /* Only SLC parts are supported for mounting: the FTL's wear-leveling
     * assumptions are written for single-bit-per-cell behaviour, and
     * nothing above the low-level driver has an ECC decode/correction
     * path robust enough to trust MLC's higher raw bit error rate with
     * real data (see NANO3G_MLC_NAND_ANALYSIS.md). An MLC part's
     * large-page geometry byte still follows the same public extended-ID
     * convention as an SLC part's, though, so it's still decoded here --
     * for diagnostics (a check-tool report showing accurate page/spare
     * sizes for a chip nobody's validated yet) -- while `recognized`
     * stays gated to SLC only, which is what keeps the FTL's write path
     * (ftl-nano3g.c's readonly_mount) locked for any MLC part. */
    if (id_len >= 4)
    {
        decode_large_page_ext_id(id_bytes[3], geo_out);
        /* Sanity bounds: reject anything outside what a 2007-08-era SLC
         * or MLC part could plausibly report, rather than trusting an
         * all-0x00 or all-0xFF READ ID response (no chip present / bus
         * floating). */
        if (geo_out->page_size >= 2048 && geo_out->page_size <= NAND_MAX_PAGE_SIZE &&
            geo_out->spare_size > 0 && geo_out->spare_size <= NAND_MAX_SPARE_SIZE &&
            geo_out->pages_per_block >= 32 && geo_out->pages_per_block <= 512)
        {
            geo_out->recognized = (geo_out->bits_per_cell == 1);
        }
    }
    else if (geo_out->bits_per_cell == 1 &&
             decode_legacy_id(geo_out->device_id, geo_out))
    {
        geo_out->recognized = true;
    }

    /* blocks_per_bank is intentionally left at 0 for a chip that isn't
     * matched below: total chip capacity cannot be derived from the base
     * 4-byte ID alone under the generic scheme above (it requires either
     * a 5th density byte, an ONFI parameter page, or a vendor part-number
     * lookup). The caller determines it by probing -- reading
     * progressively higher block numbers until reads stop returning
     * plausible data -- rather than trusting a hardcoded capacity table,
     * for any chip not in the validated table below. See
     * nand-nano3g.c:probe_bank_capacity_blocks() -- and that function's
     * own comment on why this probing method is confirmed unreliable for
     * at least one real chip, which is exactly why the validated table
     * below supplies a real capacity instead of leaving that chip to the
     * probe. */
    if (id_len >= 4)
    {
        const struct nand_validated_chip *v =
            find_validated_chip(geo_out->maker_id, geo_out->device_id,
                                id_bytes[3]);
        if (v)
        {
            /* Overrides the generic decode above, including for an MLC
             * part the generic decode alone would have left
             * ->recognized == false for -- see this function's own doc
             * comment in nand_vendor.h for why that's the deliberate
             * design, not a bypass of the MLC caution elsewhere in this
             * project: a table row here is a claim that THIS SPECIFIC
             * chip was actually tested, not that "MLC in general is now
             * trusted". */
            geo_out->recognized = true;
            geo_out->blocks_per_bank = v->blocks_per_bank;
            /* Carried out to nand_scan_banks(), which knows the real bank
             * count and withdraws ->recognized if this row was validated
             * only in a package with a specific number of chip enables. */
            geo_out->expected_banks = v->expected_banks;
        }
        else
        {
            const struct nand_diagnostic_chip *d =
                find_diagnostic_chip(id_bytes, id_len);
            if (d)
            {
                /* This is deliberately not a validated-chip match. It
                 * supplies only the read-only diagnostic geometry hint;
                 * recognized remains false, so nand_scan_banks() and the
                 * normal FTL/flasher cannot use this part for writes. */
                geo_out->blocks_per_bank = d->blocks_per_bank;
                geo_out->diagnostic_capacity_hint = true;
            }
        }
    }
}
