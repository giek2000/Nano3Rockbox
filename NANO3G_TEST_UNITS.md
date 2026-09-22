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

The chip/geometry side is complete for all units. The following
per-unit details were not captured while the units were available and would
make the record fully unit-distinguishing; they can be filled in later:

- **Real Apple serial number** for each unit (only readable from Apple's OS,
  or from the on-flash SysCfg via `mks5lboot`/a SysCfg read; not from DFU,
  which shows the generic `87020000000001`).
- **iPod generation confirmation** (units #1-#3 and #6 are 8 GB, units #4/#5/#7
  are 4 GB Nano 3G / N46) and any region/engraving notes.

Case-printed model + capacity is now recorded for every unit: unit #1 is MB253,
units #2/#3 are MB261/MB263, units #4/#5 are MA978, unit #6 is MB251, unit #7
is MB245.

If a unit is reconnected in Apple's OS (before/without reformatting), capture
its serial from Settings > About, or via iTunes, and add it to that unit's
"Real Apple serial" row below.

## Units

### Unit #1 -- Micronas 8GB (model MB253)

| Field | Value |
|-------|-------|
| Capacity / model | 8 GB Nano 3G, model MB253 |
| DFU USB serial | `87020000000001` (generic S5L8702 DFU serial, not unique) |
| Real Apple serial | not captured (unit reformatted to Rockbox) |
| hwvr / swvr (from check report) | hwvr `00140010`, swvr `1.0` |
| NAND maker | Micronas (JEDEC `0xEC`) -- see note below on the "Samsung" label |
| NAND device ID | `0xD5` |
| NAND ext-ID byte | `0xB6` |
| Raw READ ID | `EC D5 14 B6 74 EC D5 14` (packed ext-id `B614D5EC`) |
| Decoded geometry | page 4096 B, spare 128 B, 128 pages/block, x8, MLC (2 bits/cell) |
| Blocks per bank / banks | 4096 blocks/bank, 4 banks (die = 2 GiB, 4-die package) |
| Validated-chip row | `{ NAND_MAKER_MICRONAS, 0xD5, 0xB6, 4096, 0, "Micronas 4x2GiB MLC (8GB unit)" }` |

Write test: erase/program/read-back verified byte-for-byte across all four
banks and multiple blocks per bank. This was the first unit brought up on the
page-level FTL and is the reference unit for this part.

Note on the maker label: JEDEC manufacturer ID `0xEC` is **Micronas
(ITT Intermetall)**, not Samsung -- Samsung's JEDEC ID is `0xCE`. This is
corroborated by the standard JEDEC bank-0 table (OpenOCD `jep106.inc`, entry
`0x6c` + parity = `0xEC` = Micronas) and by upstream Rockbox's `nandcheck`
notes, which label the `B614D5EC` part as Micronas. As of this change the
codebase reflects that: the enum is `NAND_MAKER_MICRONAS`, `nand_vendor_name()`
returns "Micronas", the validated-chip row/note say "Micronas", the collector's
`MAKERS` map says "Micronas", and the installer's disk allow-list uses
"Apple Micronas". A unit running the rebuilt firmware therefore reports its disk
as **"Apple Micronas"** (previously "Apple Samsung").

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
| Validated-chip row | `{ NAND_MAKER_HYNIX, 0xD5, 0xA5, 8192, 0, "Hynix 4-die MLC (8GB unit)" }` |

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
- Because its physical page is 2 KB (half the Micronas part's 4 KB) for the same
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
| Validated-chip row | `{ NAND_MAKER_TOSHIBA, 0xD5, 0xBA, 4096, 0, "Toshiba 4-die MLC (8GB unit)" }` |

Write test (`-DNAND_CHECK_ALLOW_WRITE_TEST`): `wtest erase 0 write 0 read 1`
(read 1 = a benign correctable-ECC result, expected on MLC), `wtest data 1
meta 1` (data and spare byte-for-byte match), `wsweep 16/16 passed` (all four
banks x blocks {16, 1024, 2048, 4000}).

Notes specific to this unit:
- Same 4 KB-page geometry as the Micronas part (unit #1), so `blocks_per_bank`
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
| Validated-chip row | `{ NAND_MAKER_HYNIX, 0xD3, 0xA5, 4096, 0, "Hynix 4x1GiB MLC (4GB unit)" }` |

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


### Unit #5 -- Intel 4GB (model MA978)

| Field | Value |
|-------|-------|
| Capacity / model | 4 GB Nano 3G, model MA978 |
| DFU USB serial | `87020000000001` (generic S5L8702 DFU serial, not unique) |
| Real Apple serial | not captured (unit reformatted to Rockbox) |
| hwvr / swvr (from check report) | hwvr `00140010`, swvr `1.0` |
| NAND maker | Intel (JEDEC `0x89`) |
| NAND device ID | `0xD5` |
| NAND ext-ID byte | `0xA5` |
| Raw READ ID | `89 D5 D5 A5 68 00 00 00` (packed ext-id `A5D5D589`) |
| Manufacturer part | Intel JS29F32G08FAMB2 (72 nm MLC-2K, 4-bit/512B ECC per device DB) |
| Decoded geometry | page 2048 B, spare 64 B, 128 pages/block, x8, MLC (2 bits/cell) |
| Blocks per bank / banks | 8192 blocks/bank, 2 chip enables (die = 2 GiB, 2-CE package) |
| Validated-chip row | `{ NAND_MAKER_INTEL, 0xD5, 0xA5, 8192, 2, "Intel 2-die MLC (4GB unit)" }` |

Write test (`-DNAND_CHECK_ALLOW_WRITE_TEST`, collector v5.4.2, two independent
runs -- reports `...-195904` and `...-134240`): `wtest erase 0 write 0 read 2`
(read 2 = a benign correctable-ECC result, expected on MLC), `wtest data 1
meta 1` (data and spare byte-for-byte match), `wsweep 8/8 passed`,
`wisolate 3/3 map 0,1,-1,-1`, `wstatus passed`. The sweep is 8/8 rather than
16/16 because this is a 2-CE part (4 blocks x 2 chip enables), not the 4-bank
package of units #1-#4; both present chip enables read back their own unique
salted pattern, confirming they are independent dies.

Full read/write install validated on hardware (test installer v2.4.3): the
device formatted FAT32, copied the complete `.rockbox` tree, patched NOR, and
rebooted into Rockbox. It mounts read-write, plays music, and the volume
survives a reboot. This is the on-hardware validation behind promoting the
row -- the same bar units #1-#4 met.

Notes specific to this unit:
- Unlike units #1-#4 (all 4-die, 4-bank packages), this Intel part is a
  **2-chip-enable** package: 2 KB page x 128 pages/block x 8192 blocks =
  2 GiB per CE, x2 CE = 4 GiB. CEs 2 and 3 are absent and read all-zero, which
  is expected and correctly reported as not-present.
- The runtime capacity probe reports 16384 blocks with `blocks_unreliable 1`,
  and its out-of-range read aliases repeat at 16384; that is **not** a capacity
  measurement (out-of-range NAND row behaviour is undefined). Upstream Rockbox's
  chip table, the exact-part controller database, and the 4 GiB package
  arithmetic all give 8192 blocks/CE, which the validated-chip table supplies.
- The exact-ID diagnostic hint that previously carried this part read-only
  (matching all 8 READ ID bytes `89 D5 D5 A5 68 00 00 00`) has been removed;
  the part is now in `nano3g_validated_chips[]`. The 3-byte validated lookup
  (maker `0x89`, device `0xD5`, ext `0xA5`) plus the bank count keeps it off
  the 4-CE 8 GB Intel `A5D5D589` variant, which is a different topology and has
  not been validated.
- The Rockbox FTL reports this unit's disk as "Apple Intel"; the installer's
  disk allow-list (`DISK_NAMES`) includes that string as of installer v2.4.3.


### Unit #6 -- Hynix 8GB (model MB251)

| Field | Value |
|-------|-------|
| Capacity / model | 8 GB Nano 3G, model MB251 |
| DFU USB serial | `87020000000001` (generic S5L8702 DFU serial, not unique) |
| Real Apple serial | not captured (unit reformatted to Rockbox) |
| hwvr / swvr (from check report) | hwvr `00140010`, swvr `1.0` |
| NAND maker | Hynix (JEDEC `0xAD`) |
| NAND device ID | `0xD5` |
| NAND ext-ID byte | `0xA5` |
| Raw READ ID | `AD D5 55 A5 68 AD D5 55` (packed ext-id `A555D5AD`) |
| Decoded geometry | page 2048 B, spare 64 B, 128 pages/block, x8, MLC (2 bits/cell) |
| Blocks per bank / banks | 8192 blocks/bank, 4 banks (die = 2 GiB, 4-die package) |
| Validated-chip row | `{ NAND_MAKER_HYNIX, 0xD5, 0xA5, 8192, 0, "Hynix 4-die MLC (8GB unit)" }` |

This is a **second physical unit of the same validated Hynix D5 8 GB part as
unit #2** -- identical chip identity (`AD D5 55 A5 68`, packed ext-id
`A555D5AD`), geometry, and validated-chip row. It differs only in case-printed
model number (MB251 vs unit #2's MB261) and hardware version (hwvr `00140010`
vs `00140014`). A read-only quick scan recognized and mounted it (`recognized
1`, `verdict OK, mounted`), corroborating the existing Hynix D5 8 GB row on a
distinct unit; no separate write test or new table row was needed.

### Unit #7 -- Micronas 4GB (model MB245)

| Field | Value |
|-------|-------|
| Capacity / model | 4 GB Nano 3G, model MB245 |
| DFU USB serial | `87020000000001` (generic S5L8702 DFU serial, not unique) |
| Real Apple serial | not captured (unit reformatted to Rockbox) |
| hwvr / swvr (from check report) | hwvr `00140010`, swvr `1.0` |
| NAND maker | Micronas (JEDEC `0xEC`; see Unit #1's note on the historical "Samsung" label) |
| NAND device ID | `0xD5` |
| NAND ext-ID byte | `0xB6` |
| Raw READ ID | `EC D5 14 B6 74 EC D5 14` (packed ext-id `B614D5EC`) |
| Decoded geometry | page 4096 B, spare 128 B, 128 pages/block, x8, MLC (2 bits/cell) |
| Blocks per bank / banks | 4096 blocks/bank, **2 chip enables** (die = 2 GiB, 2-CE package) |
| Matches validated row | `{ NAND_MAKER_MICRONAS, 0xD5, 0xB6, 4096, 0, "Micronas 4x2GiB MLC (8GB unit)" }` |

This is the **2-chip-enable, 4 GB counterpart of the validated Micronas part in
unit #1** (which is the 4-CE, 8 GB MB253). Both carry the same die: identical
maker/device/ext-ID (`EC D5 14 B6`), identical per-die geometry (4 KB page,
128 pages/block, 4096 blocks/bank = 2 GiB/die). The only difference is the chip-
enable count -- 2 CEs here (2 x 2 GiB = 4 GiB) vs 4 CEs in unit #1 (4 x 2 GiB =
8 GiB). CEs 2 and 3 are absent and read all-zero, correctly reported as
not-present (`topology present 3`, `rawid2/3 ... present 0`).

Write test (`-DNAND_CHECK_ALLOW_WRITE_TEST`, collector v5.4.4, report
`...-181514`): `wtest erase 0 write 0 read 1` (read 1 = a benign correctable-
ECC result, expected on MLC), `wtest data 1 meta 1` (data and spare byte-for-
byte match), `wsweep 8/8 passed`, `wisolate 3/3 map 0,1,-1,-1`, `wstatus
passed`. The sweep is 8/8 (4 blocks x 2 present chip enables), and both CEs
read back their own unique salted pattern, confirming they are independent
dies. This is the on-hardware erase/program/read-back validation -- the same
bar units #1-#5 met.

It matches the Micronas row on the 3-byte lookup (maker/device/ext), and
because the per-die geometry and driver behaviour are identical to unit #1, the
row deliberately uses `expected_banks = 0` (no chip-enable constraint) so both
the 2-CE and 4-CE Micronas packages mount correctly. The FTL sizes itself from
the present bank count at runtime (4 GB volume here, 8 GB on unit #1).

This is the reason the Micronas row is `expected_banks = 0` while the Intel row
is `expected_banks = 2`: for Micronas the 4 GB/8 GB variants share ext-ID *and*
per-die geometry (only the die count differs), so any present-CE count is safe
and both are now write-validated; for Intel the 8 GB variant is a different,
untested topology that must be kept off the validated row.
