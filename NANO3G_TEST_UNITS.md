# iPod Nano 3G test units used for NAND validation

This file records the physical iPod Nano 3G (N46 / S5L8702) units this project
has used to hardware-validate NAND chips for the page-level FTL (v2). Each row
in the validated-chip table in
`firmware/target/arm/s5l8702/ipodnano3g/nand_vendor.c`
(`nano3g_validated_chips[]`) corresponds to a chip that was erased, written,
read back and verified on one of the units below. This is the provenance
behind trusting an MLC part for writes; see that file's comments and
`NANO3G_ORIGINAL_NAND_FTL.md` for the reasoning.

## A note on serial numbers

The Nano 3G's real per-unit serial number is only readable from Apple's OS or
from the on-flash SysCfg; a unit sitting in DFU reports a *generic* S5L8702
DFU USB serial (typically `87020000000001`) that is the same across many units
and is **not** a unique identifier. Where a real serial was captured from
Apple's OS before reformatting, it is recorded; otherwise the entry is marked
"not captured (unit reformatted to Rockbox)". The genuinely unit-distinguishing
and validation-relevant identity is the NAND chip ID, model number, and
hardware/software version, which are recorded for every unit.

All chip identities below were read on real hardware via the `-DNAND_CHECK`
bootloader build (`build-nano3g-check`), which reports the raw READ ID bytes,
decoded geometry, and per-bank IDs, and (with `-DNAND_CHECK_ALLOW_WRITE_TEST`)
runs a bounded erase/program/verify test plus a multi-bank sweep.

## Still to capture (units can be reconnected on request)

The chip/geometry side is complete for all three units. The following
per-unit details were not captured while the units were available and would
make the record fully unit-distinguishing; they can be filled in later:

- **Real Apple serial number** for each unit (only readable from Apple's OS,
  or from the on-flash SysCfg via `mks5lboot`/a SysCfg read; not from DFU,
  which shows the generic `87020000000001`).
- **Case-printed model + capacity** for unit #1 (units #2/#3 are MB261/MB263).
- **iPod generation confirmation** (all believed 8 GB Nano 3G / N46) and any
  region/engraving notes.

If a unit is reconnected in Apple's OS (before/without reformatting), capture
its serial from Settings > About, or via iTunes, and add it to that unit's
"Real Apple serial" row below.

## Units

### Unit #1 -- Samsung 8GB

| Field | Value |
|-------|-------|
| Capacity / model class | 8 GB Nano 3G |
| DFU USB serial | `87020000000001` (generic S5L8702 DFU serial, not unique) |
| Real Apple serial | not captured (unit reformatted to Rockbox) |
| NAND maker | Samsung (JEDEC `0xEC`) |
| NAND device ID | `0xD5` |
| NAND ext-ID byte | `0xB6` |
| Decoded geometry | page 4096 B, spare 128 B, 128 pages/block, x8, MLC (2 bits/cell) |
| Blocks per bank / banks | 4096 blocks/bank, 4 banks (die = 2 GiB, 4-die package) |
| Validated-chip row | `{ NAND_MAKER_SAMSUNG, 0xD5, 0xB6, 4096, "Samsung 4x2GiB MLC (8GB unit)" }` |

Write test: erase/program/read-back verified byte-for-byte across all four
banks and multiple blocks per bank. This was the first unit brought up on the
page-level FTL and is the reference Samsung unit.

### Unit #2 -- Hynix 8GB (model MB261)

| Field | Value |
|-------|-------|
| Capacity / model | 8 GB Nano 3G, model MB261 |
| DFU USB serial | `87020000000001` (generic S5L8702 DFU serial, not unique) |
| Real Apple serial | not captured (unit reformatted to Rockbox) |
| hwvr / swvr (from check report) | hwvr `00140014`, swvr `1.0` |
| NAND maker | Hynix (JEDEC `0xAD`) |
| NAND device ID | `0xD5` |
| NAND ext-ID byte | `0xA5` |
| Raw READ ID | `AD D5 55 A5 68 AD D5 55` (packed ext-id `A555D5AD`) |
| Decoded geometry | page 2048 B, spare 64 B, 128 pages/block, x8, MLC (2 bits/cell) |
| Blocks per bank / banks | 8192 blocks/bank, 4 banks (die = 2 GiB, 4-die package) |
| Validated-chip row | `{ NAND_MAKER_HYNIX, 0xD5, 0xA5, 8192, "Hynix 4-die MLC (8GB unit)" }` |

Write test (`-DNAND_CHECK_ALLOW_WRITE_TEST`): `wtest erase 0 write 0 read 2`
(read 2 = a benign correctable-ECC result, expected on MLC), `wtest data 1
meta 1` (data and spare byte-for-byte match), `wsweep 16/16 passed` (all four
banks x blocks {16, 1024, 2048, 4000}).

Notes specific to this unit:
- The runtime capacity probe reports 16384 blocks with `blocks_unreliable 1`;
  the real die is 8192 blocks/bank, which is why the validated-chip table
  supplies 8192 explicitly rather than trusting the probe. The packed ext-id
  `A555D5AD` matches the reference Nano 3G FTL's own chip table entry for this
  part, corroborating the 8192 figure.
- Because its physical page is 2 KB (half the Samsung part's 4 KB) for the same
  2 GiB die, it has twice the block/page count, which is why the FTL's static
  bound `FTL_MAX_BLOCKS_PER_BANK` had to be raised to 8192 to mount it (see
  `ftl-nano3g-v2.c`); a 4096 bound rejected it with storage error -103
  (`FTL_ERR_TOO_SMALL`).
- The LCD has cosmetic vertical dead lines (pre-existing hardware wear); this
  is not related to the firmware and does not affect operation.

### Unit #3 -- Toshiba 8GB (model MB263)

| Field | Value |
|-------|-------|
| Capacity / model | 8 GB Nano 3G, model MB263 |
| DFU USB serial | `87020000000001` (generic S5L8702 DFU serial, not unique) |
| Real Apple serial | not captured (unit reformatted to Rockbox) |
| hwvr / swvr (from check report) | hwvr `00140010`, swvr `1.0` |
| NAND maker | Toshiba (JEDEC `0x98`) |
| NAND device ID | `0xD5` |
| NAND ext-ID byte | `0xBA` |
| Raw READ ID | `98 D5 94 BA F4 13 41 00` (packed ext-id `BA94D598`) |
| Decoded geometry | page 4096 B, spare 64 B, 128 pages/block, x8, MLC (2 bits/cell) |
| Blocks per bank / banks | 4096 blocks/bank, 4 banks (die = 2 GiB, 4-die package) |
| Validated-chip row | `{ NAND_MAKER_TOSHIBA, 0xD5, 0xBA, 4096, "Toshiba 4-die MLC (8GB unit)" }` |

Write test (`-DNAND_CHECK_ALLOW_WRITE_TEST`): `wtest erase 0 write 0 read 1`
(read 1 = a benign correctable-ECC result, expected on MLC), `wtest data 1
meta 1` (data and spare byte-for-byte match), `wsweep 16/16 passed` (all four
banks x blocks {16, 1024, 2048, 4000}).

Notes specific to this unit:
- Same 4 KB-page geometry as the Samsung part (unit #1), so `blocks_per_bank`
  is 4096 and no FTL static-bound change was needed (unlike the 2 KB-page
  Hynix part). As with the others, the runtime probe reports 16384 blocks with
  `blocks_unreliable 1`; the validated-chip table supplies the real 4096.
- Before installing our firmware, this unit arrived running some other Rockbox
  bootloader that exposed its disk as vendor "Rockbox" / product "Nano 3G NAND"
  (not "Apple <maker>"). Our firmware reports it as "Apple Toshiba".


### Unit #4 -- Hynix 4GB (model MA978)

| Field | Value |
|-------|-------|
| Capacity / model | 4 GB Nano 3G, model MA978 |
| DFU USB serial | `87020000000001` (generic S5L8702 DFU serial, not unique) |
| Real Apple serial | not captured (unit reformatted to Rockbox) |
| hwvr / swvr (from check report) | hwvr not captured, swvr `1.0` |
| NAND maker | Hynix (JEDEC `0xAD`) |
| NAND device ID | `0xD3` |
| NAND ext-ID byte | `0xA5` |
| Raw READ ID | `AD D3 14 A5 64 AD D3 14` (packed ext-id `A514D3AD`) |
| Decoded geometry | page 2048 B, spare 64 B, 128 pages/block, x8, MLC (2 bits/cell) |
| Blocks per bank / banks | 4096 blocks/bank, 4 banks (die = 1 GiB, 4-die package) |
| Validated-chip row | `{ NAND_MAKER_HYNIX, 0xD3, 0xA5, 4096, "Hynix 4x1GiB MLC (4GB unit)" }` |

Write test (`-DNAND_CHECK_ALLOW_WRITE_TEST`): `wtest erase 0 write 0 read 1`
(read 1 = a benign correctable-ECC result, expected on MLC), `wtest data 1
meta 1` (data and spare byte-for-byte match), `wsweep 16/16 passed` (all four
banks x blocks {16, 1024, 2048, 4000}).

Notes specific to this unit:
- This is the smaller 4 GB counterpart to unit #2's Hynix D5 8 GB part. Both
  use 2 KB pages and ext-ID `0xA5`, but the exact-table lookup must also match
  the device byte: D3 here vs D5 for the 8 GB chip.
- The runtime capacity probe reports 16384 blocks with `blocks_unreliable 1`.
  For this 4 GB package, four 1 GiB dies with 2 KB x 128-page erase blocks
  yield 4096 blocks/bank; the validated-chip table supplies that real value.
