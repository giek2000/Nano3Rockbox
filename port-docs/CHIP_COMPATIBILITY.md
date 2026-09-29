# Nano 3G NAND compatibility

[← back to README](../README.md)

Compatibility is determined by the NAND identity reported by **Check my iPod
(safe)**, not by the case colour, the A1236 family number, or capacity alone.
Apple shipped several different NAND packages in otherwise identical Nano 3G
units.

The published MB249 files are in the
[v2.4.5 fast-boot pre-release](https://github.com/Kryst-0/Nano3Rockbox/releases/tag/v2.4.5-mb249-fastboot).
They apply only to the exact guarded MB249 row below and only as an update to
an existing Nano3Rockbox-formatted device.

## Status meanings

- **Validated:** erase/program/read-back and a multi-bank sweep passed on real
  hardware. The normal Rockbox FTL is allowed to write.
- **Validated with an exact-ID/topology guard:** writable only when the complete
  ID and chip-enable count match the tested package.
- **Reported:** observed in a real unit, but not yet hardware write-validated.
- **Needed:** present in Apple's supported-part table but no complete validation
  archive has been collected. Reported and needed parts remain read-only.

The IDs in the table are written as the historical packed value used by the
project. The safe checker also prints the raw READ ID bytes and the number of
responding chip enables (`banks`). Those extra fields matter where shown.

## Writable, hardware-validated configurations

| Packed ID | Maker | Capacity | Chip enables | Page | Match rule | Example model |
|---|---|---:|---:|---:|---|---|
| `A514D3AD` | Hynix | 4 GB | 4 | 2 KiB | maker/device/ext ID | MA978 |
| `A555D5AD` | Hynix | 8 GB | 4 | 2 KiB | maker/device/ext ID | MB261, MB251 |
| `B614D5EC` | Micronas | 4 GB | 2 | 4 KiB | same validated die; runtime CE count sizes the disk | MB245 |
| `B614D5EC` | Micronas | 8 GB | 4 | 4 KiB | same validated die; runtime CE count sizes the disk | MB253 |
| `BA94D598` | Toshiba | 8 GB | 4 | 4 KiB | maker/device/ext ID | MB263 |
| `A5D5D589` | Intel | 4 GB | **2 only** | 2 KiB | maker/device/ext ID **and two CEs** | MA978 |
| `A585D598` | Toshiba | 8 GB | **4 only** | 2 KiB | exact raw ID `98 D5 85 A5 EA 12 02 00` **and four CEs** | MB249 |

`0xEC` is Micronas/ITT Intermetall's JEDEC manufacturer code. Older versions
of this project called it Samsung; Samsung's actual JEDEC code is `0xCE`.

The Intel 8 GB four-CE package is not covered by the validated Intel 4 GB row.
The MB249 rule is intentionally stricter than the ordinary three-byte lookup:
a similar Toshiba `D5/A5` part does not inherit write support.

## Known but not write-enabled

| Packed ID | Maker | Capacity | Chip enables | Page | Status |
|---|---|---:|---:|---:|---|
| `B614D5AD` | Hynix | 8 GB | 4 | 4 KiB | needed |
| `2555D5EC` | Micronas | 8 GB | 4 | 2 KiB | needed |
| `A585D598` | Toshiba | 4 GB | 2 | 2 KiB | needed; not covered by MB249 validation |
| `BA94D598` | Toshiba | 4 GB | 2 | 4 KiB | needed |
| `A5D5D589` | Intel | 8 GB | 4 | 2 KiB | reported; not covered by the two-CE Intel row |
| `3E94D589` | Intel | 4 GB | 2 | 4 KiB | reported |
| `3ED5D789` | Intel | 8 GB | 2 | 4 KiB | needed |
| `A5D5D52C` | Micron | 4 GB | 2 | 2 KiB | needed |
| `A5D5D52C` | Micron | 8 GB | 4 | 2 KiB | needed |
| `3E94D52C` | Micron | 4 GB | 2 | 4 KiB | needed |
| `3ED5D72C` | Micron | 8 GB | 2 | 4 KiB | needed |

## How to check safely

1. Back up anything you care about.
2. In the Windows installer choose **Check my iPod (safe)** before Install.
3. Keep the cable connected and enter DFU by holding **MENU + SELECT** until
   the display goes black.
4. Wait for the checker to show `id`, `rawid`, `banks`, `validated` and
   `verdict`.
5. Continue with installation only when the verdict explicitly says that the
   detected identity and topology are validated. Do not infer support from a
   similar ID or the same capacity.

The normal checker runs from RAM and does not write to NAND. Do not use a build
compiled with `NAND_CHECK_ALLOW_WRITE_TEST`; that separate developer image is
destructive and exists only for controlled validation of a new chip.

Detailed per-unit evidence is recorded in
[NANO3G_TEST_UNITS.md](../NANO3G_TEST_UNITS.md). The collection procedure for
new parts is in [the NAND checker README](../utils/ipodnano3g/nandcheck/README.md).
