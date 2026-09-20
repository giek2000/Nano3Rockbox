# Apple-compatible FTL for the Nano 3G — port plan (Phase 0)

[← back to README](../README.md)

**Goal:** a second, optional NAND pipeline that reads and (eventually) writes
Apple's own on-flash format (Whimory VFL + FTL), so a Nano 3G can share the
NAND with Apple's OS — true dual-boot — and so geometry comes from Apple's
stored context instead of per-chip validation.

**Non-goal for now:** replacing anything. This is gated behind a new build flag
`FTL_APPLE_COMPAT`; the proven page-level `FTL_NANO3G_V2` pipeline stays the
default and is not touched. `FTL_APPLE_COMPAT` is developed and validated in
safe stages (read-only first).

This document is the study output: what the in-tree Nano 2G Apple FTL does,
what changes for the 3G, what is already decoded, and what is still unknown.

## Sources this builds on

- `firmware/target/arm/s5l8700/ipodnano2g/ftl-nano2g.c` — a working, GPL,
  in-tree reimplementation of Apple's Whimory VFL+FTL for the **Nano 2G**. This
  is the structural reference: the on-flash structs and magic values match
  Apple's OFW because it mounts real Apple 2G devices.
- `firmware/target/arm/s5l8700/ipodnano2g/nand-nano2g.c`,
  `.../nand-target.h` — the 2G low-level NAND API and device-ID geometry table.
- `utils/ipodnano3g/decode/APPLE-FTL-READ.md`,
  `.../APPLE-FTL-WRITE.md` — this project's own decode of the **3G** Apple
  firmware (OSOS 1.1.3, "Whimory 2.1"), function-by-function, evidence-tagged.
  These confirm the 3G FTL is the same lineage as the 2G, and call out the 3G
  differences (4 banks, multi-unit pages, the FMSS sequencer program).

The strategy is: port the 2G's `ftl-nano2g.c` structure, cross-checking every
behaviour against the 3G decode docs and adapting for 3G geometry and the
existing 3G low-level driver (`nand-nano3g.c`).

## The three layers (unchanged in concept from 2G)

1. **Low-level NAND** (`nand-nano3g.c`, already exists): raw per-(bank,page)
   read/write/erase + ECC. On the 3G this is the confirmed FMC sequence we
   already use for our v2 FTL.
2. **VFL (Virtual Flash Layer):** per-bank bad-block remap; maps a virtual page
   across banks to a physical (bank, block, page); owns the per-bank VFL
   context. This is where **geometry and bad-block data live on flash** — the
   piece that makes chip identification automatic.
3. **FTL (Whimory):** logical sector → virtual block/page via a block map plus
   scattered-page "log" blocks; wear-levelling, GC/merge, context commit.

## On-flash structures (from the 2G, to be confirmed byte-identical on 3G)

These are `__attribute__((packed))` and read directly off flash. Sizes are the
`memcpy` lengths the 2G mount uses.

- **Device-info / BBT discovery.** Magic string **`"DEVICEINFOSIGN\0"`** (16
  bytes) in a page in the **last 10% of the chip**, only in the last 8 pages of
  a block. Marker **`"BBT"`** at offset 0x18 of that page. A low-level BBT of
  `0x410` bytes (1 bit/block, 1=good) is located from indices in the devinfo
  page. (2G: `ftl_find_devinfo`, `ftl_load_bbt`. The exact index math is flagged
  in the 2G source itself as reverse-engineered and not fully understood — a
  known risk area, see Unknowns.)
- **`ftl_vfl_cxt_type`** — per bank, `0x800` bytes read at mount. Key fields:
  `usn`, `ftlctrlblocks[3]` (where to find the FTL context), `updatecount`,
  `activecxtblock`, `nextcxtpage`, `spareused`, `firstspare`, `sparecount`,
  `remaptable[0x334]` (spare-block remap), `bbt[0x11A]` (1 bit per 8 blocks),
  `vflcxtblocks[4]` (VFL ctx ring), `scheduledstart`, `checksum1` (additive),
  `checksum2` (XOR; Whimory has a verify bug that accepts a mismatch — must be
  reproduced for compatibility). Identified on flash by spare `type == 0x80`.
- **`ftl_cxt_type`** — the FTL context, `0x28C` bytes. Key fields: `usn`
  (decremented per revision), `nextblockusn` (per-write-batch, the freshness
  counter), `freecount`, `nextfreeidx`, `swapcounter`, `blockpool[0x14]` (free
  hyperblock ring), `ftl_map_pages[8]` (where the block map is stored),
  `ftl_erasectr_pages[8]`, `ftlctrlblocks[3]`, `ftlctrlpage`, `clean_flag`
  (clean/dirty mount marker). Identified by spare `type == 0x43`.
- **Spare/OOB layout** — two overlaid layouts in the page's spare area:
  - *user* (types 0x40 data / 0x41 last-page-of-block): `lpn` (u32@0), `usn`
    (u32@4), `type` (@9), `eccmark` (@0xA; 0xFF normal, 0x55 = prior read
    error), then ECC.
  - *meta* (types 0x43 FTL ctx / 0x44 block-map / 0x46 erase-ctr / 0x47
    "mounted"/unclean mark / 0x80 VFL ctx): `usn` (@0), `idx` (@4), `type` (@9),
    `eccmark` (@0xA), then ECC.

## What changes for the 3G (the actual porting work)

| Aspect | Nano 2G (reference) | Nano 3G (target) | Source of truth |
|---|---|---|---|
| Page (data) size | 2048 B, hardwired (`0x800` everywhere, `<<11` shifts) | 2 KB **or** 4 KB depending on chip; 4 KB parts transfer as 2× 2 KB "units" | our `nand-nano3g.c`; APPLE-FTL-READ.md ("2KiB units per page") |
| Spare/OOB | 64 B (`0x40`), Apple 12-byte header + ECC | 3G uses a **12-byte** spare metadata header (`NAND_SPARE_META_BYTES=12`); ECC handled by the FMC/sequencer | our driver; APPLE-FTL docs (12-byte spare) |
| Banks | probed 1..4, page N → bank N%banks striping | 4 banks, same cross-bank striping; the FMSS program overlaps loads across chip-enables | APPLE-FTL-READ.md (`SEQ+0xd04` bank count 4) |
| ECC | `ecc_decode()` in software over spare bytes | FMC hardware ECC / the sequencer's per-chunk ECC result bitmap | APPLE-FTL-READ.md (result classifier, bit 30 = uncorrectable) |
| pages/block | 64 or 128 (device table) | 128 on the validated parts | our `nand_vendor.c` |
| Low-level API | `nand_read_page(bank,page,data,spare,doecc,checkempty)` etc. | our `nand_hw_read_page(bank,page,data,spare)` + friends — **different signature**; a thin adapter layer is needed | `nand-nano3g.c` |
| Multi-page read | `nand_read_page_fast` (all banks) | our driver has no all-banks fast read yet; port reads per-bank first, optimise later | `nand-nano3g.c` |

The single largest structural difference is **page/spare size is a runtime
value on the 3G**, not a compile-time `0x800`/`0x40`. Every `<<11`, every
`0x800` buffer, every `0x40` spare stride in the 2G code must become the
runtime `page_size` / spare size. The 2G code is written as if these are
constants; the port must parameterise them (the geometry comes from the VFL
device-info, see below).

## The key payoff: geometry from Apple's context

Our v2 pipeline needs a hand-validated `blocks_per_bank` per chip because the
runtime probe aliases (see `NAND_CHIP_IDENTIFICATION.md`). Apple's VFL context
and device-info page encode the real geometry and bad-block map on the chip
itself. Once VFL mount works, an Apple-formatted device tells us its own
capacity — no per-chip write-test needed for that path. That alone is worth the
read-only milestone.

## Build seam

`firmware/SOURCES` already selects the FTL cleanly:

```
#ifdef FTL_NANO3G_V2
target/arm/s5l8702/ipodnano3g/ftl-nano3g-v2.c
#else
target/arm/s5l8702/ipodnano3g/ftl-nano3g.c
#endif
```

Add a third branch, checked first, for the new pipeline:

```
#ifdef FTL_APPLE_COMPAT
target/arm/s5l8702/ipodnano3g/ftl-apple-nano3g.c   # new
#elif defined(FTL_NANO3G_V2)
target/arm/s5l8702/ipodnano3g/ftl-nano3g-v2.c
#else
target/arm/s5l8702/ipodnano3g/ftl-nano3g.c
#endif
```

The new file must expose the same public interface the rest of Rockbox and the
bootloader already call (matching `firmware/export/ftl-target.h`): `ftl_init`,
`ftl_read`, `ftl_write`, `ftl_sync`, and the capacity accessor. Read-only
milestones can stub `ftl_write`/`ftl_sync` exactly as the 2G read-only build
does (`ftl-nano2g.c` lines 49/56).

## Phased delivery (safe order)

- **Phase 1 — VFL read + context mount (read-only).** Port device-info/BBT
  discovery, VFL context load, `vBlock→pBlock` remap, and the VPN→(bank,page)
  math. Deliver `blocks_per_bank` and bad-block map from Apple's own data.
  *Risk: none (read-only).* First hardware goal: mount a stock unit and print
  its reconstructed geometry.
- **Phase 2 — FTL read (read-only).** Port `_FTLRead`: block map + log-entry
  resolution, spare-LPN validation, refresh classification (report only). Read
  Apple's logical sectors → mount its FAT/HFS filesystem read-only.
- **Phase 3 — hardware-validate read-only.** On a stock, untouched Nano 3G,
  confirm geometry and that the Apple filesystem reads correctly. Read-only, so
  a decode error cannot corrupt the device.
- **Phase 4 — write path.** Port log allocation, the merge variants, wear-
  levelling, and context store (`_StoreFTLCxt`) + `_FTLRestore` mount. Build a
  **host mock harness** (like `utils/ipodnano3g/ftltest2`) and pass
  crash/consistency tests **before any hardware write**.
- **Phase 5 — hardware-validate read+write + Apple interop.** On a disposable
  test unit: write via our Apple-compat FTL, read back via Apple's OS / iTunes.

## Known unknowns / risk register

1. **Devinfo index math.** The 2G `ftl_load_bbt` derivation of the BBT page
   location from the devinfo page is flagged in the 2G source itself as not
   fully understood. Must be confirmed against a real 3G devinfo page (Phase 1,
   read-only) before trusting it.
2. **VFL context is 2G-shaped.** `ftl_vfl_cxt_type` sizes (`remaptable[0x334]`,
   `bbt[0x11A]`, `0x800` total) are 2G values. The 3G may differ; confirm by
   dumping a real 3G VFL context page and checking the checksum validates.
3. **Page/spare parameterisation.** The 2G code hardwires 2 KB/64 B. A correct
   port must thread runtime geometry everywhere; a missed constant is a
   silent-corruption risk on write (Phase 4), harmless on read.
4. **ECC ownership.** 2G does software ECC over spare bytes; the 3G FMC/
   sequencer does ECC in hardware with a per-chunk result bitmap
   (APPLE-FTL-READ.md). The port uses the 3G driver's ECC, not the 2G's
   `ecc_decode`.
5. **Whimory checksum bug.** The VFL checksum verify must reproduce Apple's
   accept-on-mismatch quirk, or valid Apple contexts will be rejected.
6. **Write is irreversible if wrong.** Any Phase 4/5 defect can destroy an
   Apple device's data. Hence: mock harness first, disposable unit only, v2 stays
   default.

## Progress (Phases 1-2 implemented, pending hardware validation)

`ftl-apple-nano3g.c` exists behind `FTL_APPLE_COMPAT` and builds cleanly
(SOURCES seam added; v2 remains the default and untouched). Implemented,
read-only:

- **VFL mount:** device-info/BBT discovery, VFL context load + the Whimory
  checksum quirk, `vBlock->pBlock` remap, VPN->(bank,page) translation.
- **FTL mount:** locate + load the FTL context, load the logical-block map,
  unclean-shutdown bail.
- **FTL read:** block-map resolution + per-physical-page read with the
  4 KiB -> two-2 KiB-sector sub-page split.
- **Diagnostic:** `apple_ftl_vfl_mount_info()` + an `applecheck` image
  (`configure_apple_check.sh` / `build_apple_check.sh`,
  `-DNAND_CHECK -DFTL_APPLE_COMPAT`) reporting reconstructed geometry, VFL/FTL
  mount status, and a raw hex dump of the device-info page head.

### Unresolved before a full-volume read can be trusted

- **`userblocks` / `syshyperblocks` are provisional.** On the 2G these come
  from a hardcoded device table; we don't have that for the 3G, and the value
  is NOT `blocks - constant` (syshyperblocks is itself per-device; 0x17 is only
  an extra reserve on top). The real numbers live in Apple's device-info page.
  The diagnostic now dumps that page's head (`apple_devinfo ...`) so the first
  hardware pass can locate them. Until decoded, capacity and the VPN offset
  (`ppb * syshyperblocks`) are guesses -- the mount validates the VFL/FTL
  context DECODE and map reads, not a user-facing filesystem.

### Immediate next step

Run the `applecheck` image (read-only) from DFU against a STOCK (unmodified
Apple) Nano 3G. Expected on success: `apple_mount mounted`, plausible geometry,
a non-zero `apple_vflusn`, and the `apple_devinfo` hex to decode userblocks/
syshyperblocks. If the VFL context checksum validates, the struct offsets, the
spare byte order, and the BBT index math are all confirmed at once.
