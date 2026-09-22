# Nano 3G storage validation results

Results below were recorded on 2026-09-13. Hardware results used a 4 GB Nano
3G with Hynix NAND ID `0xA514D3AD`.

## Host tests

- The independent newest-page oracle agreed with `ftl_read()` for all
  1,982,464 logical sectors in the device image.
- Read/write, wear-level, bad-block injection, allocation eligibility,
  restore and mock-NAND suites completed without failures.
- Power was cut at 300 operation boundaries in each of 48 deterministic
  campaigns. Half modeled a torn page program or half-completed erase. All
  acknowledged writes survived each remount and no page was programmed twice
  or out of order.
- The 600-file write replay retained 66,388 copy-merge reads and 34,816
  delete-merge reads. Batched reads reduced these to 528 and 280 controller
  calls respectively, averaging 125.7 and 124.3 pages per call.
- Driver and emulator register traces were identical in all write scenarios:
  one page, single-plane rows, repeated-bank pages, and one through three
  two-plane rows.
- Driver and emulator register traces were identical for one read, four-bank
  reads, repeated-bank reads and a mixed duplicate-bank split.

## Hardware tests

- A complete NAND dump produced 184 VFL context pages that all passed the
  format's checksums.
- Rockbox mounted and restored the uncommitted state left by Apple disk mode.
- A 77,067,368-byte, 600-file tree written through Rockbox read back with all
  SHA-256 hashes intact through both Rockbox and Apple disk mode.
- The same tree read at 7.550 MB/s through Rockbox USB and 9.745 MB/s through
  Apple disk mode. Rockbox wrote and synced it at 0.731 MB/s; Apple wrote and
  synced it at 1.026 MB/s.
- A setting survived reset and restore, and Apple firmware subsequently read
  the volume successfully.

Bad-block remapping and forced program/erase failure recovery have extensive
host fault-injection coverage, including validation of the persisted remap
after remount. They have not been deliberately triggered on hardware. The
implemented layout and recovery order follow the decoded Apple FTL/VFL
process documented in `decode/APPLE-FTL-WRITE.md`; that correspondence is
evidence for the implementation, not a substitute for destructive hardware
fault testing.

## Later per-chip hardware validations

Each entry below is a distinct NAND part hardware-validated on its own unit
(erase/program/read-back verified byte-for-byte, plus a multi-bank sweep) and
promoted in `nand_vendor.c`'s `nano3g_validated_chips[]`. Per-unit provenance
is in `NANO3G_TEST_UNITS.md`.

- **Hynix `A555D5AD` (8GB, 4 CE, 2KiB), unit #2 (MB261).** `wsweep 16/16
  passed`, data and spare byte-for-byte.
- **Toshiba `BA94D598` (8GB->validated as 4GB-class 4KiB row), unit #3
  (MB263).** `wsweep 16/16 passed`.
- **Hynix `A514D3AD` (4GB, 4 CE, 2KiB), unit #4 (MA978).** `wsweep 16/16
  passed`.
- **Intel `A5D5D589` (4GB, 2 CE, 2KiB; Intel JS29F32G08FAMB2), unit #5
  (MA978), validated 2026-09-22.** Bounded write test on both chip enables:
  `wtest erase 0 write 0 read 2`, `wtest data 1 meta 1`, `wsweep 8/8 passed`,
  `wisolate 3/3 map 0,1,-1,-1`, `wstatus passed` (8/8 not 16/16 because this is
  a 2-CE part; both CEs read back their own salted pattern, confirming
  independent dies). Full install validated end to end: formatted FAT32,
  copied the `.rockbox` tree, patched NOR, rebooted into Rockbox, mounts
  read-write, plays audio, and the volume survives a reboot. This is the
  first non-4-die (2-chip-enable) package validated on the port.
- **Micronas `B614D5EC` (4GB, 2 CE, 4KiB), unit #7 (MB245), validated
  2026-09-22.** Bounded write test on both chip enables: `wtest erase 0 write 0
  read 1`, `wtest data 1 meta 1`, `wsweep 8/8 passed`, `wisolate 3/3 map
  0,1,-1,-1`, `wstatus passed` (8/8 because this is a 2-CE part; both CEs read
  back their own salted pattern, confirming independent dies). This is the 2-CE
  4 GB counterpart of the 4-die 8 GB Micronas reference part (unit #1, same die
  and ext-ID `B614D5EC`); it mounts writable via that Micronas validated row,
  which uses `expected_banks = 0` so both CE counts are accepted.
