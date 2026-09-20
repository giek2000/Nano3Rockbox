# Apple-compatible FTL (FTL_APPLE_COMPAT): read-path status

Read-only progress on the optional second pipeline that reads Apple's own
on-flash Whimory format. The default, proven pipeline (`ftl-nano3g-v2.c`) is
untouched; everything here is behind `-DFTL_APPLE_COMPAT` and makes no NAND
writes.

## What is validated on hardware

Tested read-only against **two different stock Nano 3G units** (the second a
different unit from the one the format was reverse-engineered on):

- **Chip identity + physical geometry** come from the existing driver
  (`nand_get_bank_geometry`, `nand_get_bank_count`).
- **Apple geometry derivation** (`apple_derive_geometry`): planes, userblocks,
  vflspares, nsuperblocks, sbpages, usersb. On the observed 4 KiB units this is
  planes 1, userblocks 3872, vflspares 201, nsuperblocks 3895, sbpages 512,
  usersb 3872 — internally consistent and matching the historical chip model.
- **VFL context mount** (`apple_vfl_open` / `apple_vfl_open_bank`): scans
  physical blocks 1..199 on every bank, follows the context ring at struct
  offset `0x694`, selects the newest generation, and validates each context
  with the **strict** additive+XOR checksum at `0x7f8`/`0x7fc`. Confirmed: a
  valid VFL context is found on **all four banks** (ring blocks 1..4), with
  `field8 == 0` and spare type `0x80`. This is the finding that unblocked the
  whole effort (see the strict all-bank locator run).
- **Block-layout auto-detection** (`apple_detect_layout`): picks
  SINGLE/ADJACENT/HALVES/BOTH/SPLIT13 empirically by which layout makes the FTL
  control superblocks read as control pages, so no chip-ID table is needed.
  Single-plane parts short-circuit to SINGLE.

## The remaining blocker: uncommitted volumes

`apple_ftl_open` loads the FTL context + block map for a **clean** volume, and
returns `2` ("needs restore") otherwise. Both stock units hit the `needs
restore` path:

- `apple_ftlc ... clean 0` — the newest FTL control block's last written page
  is **not** a `0x43` context page.
- `apple_ftlu` shows one control superblock read many pages ending in a
  non-context type; the others' first pages are remapped spares.

This is the **normal state Apple's OS leaves behind**: it writes data pages
without committing a fresh FTL context, and relies on its own mount-time
*restore* to rebuild the block map, open logs and free pool from the medium.
The host harness confirms this is expected: `test_format blank:single4k`
performs "500 writes, no sync; remount without sync" and the reference FTL
mounts it — **via the write-capable restore path** (it erases the reclaimed
blocks and commits). A strictly read-only mount cannot do that rebuild, so it
correctly refuses rather than expose a stale/partial map.

### Consequence

A read-only Apple mount can only mount a **cleanly committed** Apple volume
(one whose newest control block ends in a `0x43` context). A stock unit that
was last touched by Apple's OS is typically uncommitted and will report `needs
restore`.

## Faithfulness of the port

`apple_unit_block`, `apple_vfl_spare_block`, `apple_vfl_phys_block`,
`apple_vpage_phys`, and the VFL/FTL context structs are byte-for-byte ports of
the historical Nano 3G Whimory implementation (commit `3c13884`,
`ftl-nano3g.c`), whose geometry/addressing passes the host `ftltest` harness
across every layout (`test_format blank:{2k4,2k8g,4k2,halves,both,single4k,
split13}` all PASS). The historical file depends on the old rich
`struct nand_geometry`; the current tree's driver exposes only physical
geometry, which is why the Apple pipeline recomputes the Apple geometry in
`apple_derive_geometry` instead of reading it from the driver.

## Diagnostic caveat

The `apple_ca` LCD diagnostic block in `nand-check-nano3g.c` produced an
**inconsistent** physical block for a control superblock (reported block 4091
where the directly-computed `apple_rmp` reported 3898). Treat `apple_ca` as
unreliable; trust `apple_rmp` (direct `apple_vfl_phys_block` result) and
`apple_ftlu`. The `apple_ca` block should be removed or rewritten before it is
used to draw further conclusions.

## Options to read an uncommitted stock volume

1. **Commit with Apple first (non-invasive, recommended for validation).**
   Boot the unit normally into Apple's OS and let it settle / eject cleanly, or
   have it do a clean shutdown, so the FTL commits a fresh context. Then a
   read-only mount should find a clean context. This is the simplest way to get
   an end-to-end read validation (MBR `55 aa`, real sectors) without any write
   code.

2. **Port the read side of `ftl_restore()` as a RAM-only rebuild (read-only).**
   Apple's restore rebuilds the map/logs/free-pool from the medium. The
   *reconstruction* is pure reads; only the subsequent erase+commit writes.
   A read-only variant can rebuild those tables in RAM and resolve reads
   through them, skipping the write-back. This is the correct next milestone
   for reading arbitrary stock volumes read-only. It is more code (log
   resolution: a page in a log is newer than the map's copy) but no NAND
   writes.

3. **Proceed to the write path (Phase 4).** The full historical restore
   (rebuild + erase + commit) is what actually mounts an uncommitted volume the
   way Apple does. This is the eventual goal (full read+write) but must stay
   gated and hardware-validated carefully; not a read-only step.

## Recommendation

Do option 1 next to get a clean end-to-end **read** validation cheaply (proves
`ftl_read` returns real Apple filesystem data). Then implement option 2 (the
RAM-only restore-read) so the read pipeline handles the common uncommitted
state without writing. Defer the write path until the read path is proven on a
committed volume and a rebuilt (uncommitted) volume.

## Final hardware read result

The compact read-only probe on a stock unit reported:

```text
apple_mnt 0 1
apple_sec 3964928
apple_scan 0 47 1 141
```

This proves the VFL+FTL mount and RAM-only restore succeeded, and that the
logical read path found a valid `0x55 0xaa` boot signature at logical sector
141. The optional first-16-byte `apple_sigh` capture was not present in the
subsequent photos, so the exact BPB/header contents remain unverified; the
signature-level read validation is nevertheless complete. No NAND writes were
used.

## Correction: sector-141 signature is not a filesystem proof

A later probe captured the first 16 bytes of the sector reported by the
`55 aa` scan:

```text
apple_scan ... sig 1@141
apple_sigh 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

Therefore the `55 aa` pair at sector 141 is not sufficient evidence of a FAT
boot sector: the sector header is all zero. It may be a coincidental signature
in a sparse/system area or another structure with a trailer signature. The
correct status is: **VFL mount, RAM-only restore, geometry and logical read
machinery are validated; recognizable filesystem byte mapping is not yet
proven**. The read probe must next locate and decode a genuine partition/FAT
structure, rather than treating any isolated `55 aa` pair as proof.

## Host-side raw acquisition path

`utils/ipodnano3g/nandcheck/nandcheck.py` was corrected so `collect` validates
`banks`, `blocks`, `ppb`, and `pagesize` instead of requiring the obsolete
`row` report field. The script passes `python3 -m py_compile`.

When the NAND-check image enumerates as a disk, run from an elevated PowerShell
with automount/format prompts cancelled:

```powershell
python C:\KIRO\Nano3Rockbox\rockbox-master\utils\ipodnano3g\nandcheck\nandcheck.py collect "\\.\PhysicalDriveN" --out C:\KIRO\TEMP\nandcheck
```

Replace `N` with the physical drive carrying the `nano3g-nandcheck` sector-0
magic. The collector's first pass records every page's 12-byte spare metadata
and read result; it then saves control-page main data in `pages.bin` without
collecting user data. This is the next authoritative target for identifying
Apple partition/control structures and validating the logical-sector mapping.
