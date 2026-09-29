# Nano 3G MB249 validation and fast boot

This procedure applies only to the validated MB249 identity and topology. See
the [central NAND compatibility table](CHIP_COMPATIBILITY.md) before reusing it
on another Nano 3G.

This document describes the Toshiba MB249 hardware validation and the FTL
mount-time verification change tested on an 8 GB iPod Nano 3G. It also gives a
non-destructive update procedure for an iPod that already uses Nano3Rockbox's
on-flash format.

## Hardware-tested download

The exact files used for this result are available in the
[v2.4.5 MB249 fast-boot pre-release](https://github.com/Kryst-0/Nano3Rockbox/releases/tag/v2.4.5-mb249-fastboot).
Download `Nano3Rockbox-MB249-fastboot-2026-09-28.zip` and verify its SHA-256:

```text
C0B5C82BF90D1F46CAA31D4B03A404FDB8C43BB6D7EF903BD06A6D831ABB4B9F
```

The archive is for a device already using Nano3Rockbox's on-flash format. It
is not a universal first-time installer and must not be used on a different
NAND identity or topology.

## Hardware tested

- iPod model: MB249, 8 GB
- NAND READ ID on each of four chip enables:
  `98 D5 85 A5 EA 12 02 00`
- Geometry: 2,048-byte pages, 128 pages per erase block, 8,192 blocks per
  chip enable, four chip enables
- Hardware validation: write/read-back test passed; 16/16 sweep passed;
  four-bank isolation passed

The full eight-byte ID and the four-bank topology are required. A merely
similar Toshiba D5/A5 part does not inherit write support.

## Why boot used to take minutes

The block-level FTL reconstructs its logical-to-physical map by reading page 0
of every physical block. The former conservative path then read all remaining
pages of every mapped block during mount. Once several gigabytes had been
copied, that second pass became effectively a full-device read on every boot.

A tempting shortcut is to skip the second pass. That starts quickly, but it
can select the highest-generation block even when power was lost partway
through programming it. The original crash-recovery test demonstrates the
problem: the incomplete new generation can be exposed instead of the older
complete copy.

## Verification-on-first-access design

Mount still reads page 0 of every physical block. For each logical block it
records all generation candidates and selects the newest one provisionally.
Full-page verification is deferred until that logical block is first read or
written:

1. Verify every page's ECC result and FTL header.
2. If the newest generation is complete, cache that result for the rest of
   the mount.
3. If it is structurally incomplete, try the next older generation.
4. On a transient NAND read error, return an error and retain all candidates
   so a later access can retry.
5. If no complete generation exists, return an error instead of presenting
   torn data or pretending the block is erased.

Older generations are not placed in the free pool before verification. This
prevents unrelated writes from erasing the only recoverable copy. If free
space is exhausted, candidates are reclaimed only after a complete generation
has been identified. The same validation runs before a first-access write, so
a partial update cannot merge with torn data.

The on-flash format is unchanged. Additional static RAM use is approximately
160 KiB at the maximum supported physical-block count.

## Tests

The native FTL suite is in `utils/ipodnano3g/ftltest2`. Build and run it from
MSYS2 or another environment with GCC:

```sh
cd utils/ipodnano3g/ftltest2
make clean
make check
```

The added coverage includes:

- mount cost with 100 populated logical blocks;
- verification cached after first access;
- 31 torn commit positions and one complete commit;
- unrelated writes before recovery;
- a full logical volume with multiple torn generations and free-pool pressure;
- first access being a partial write rather than a read;
- no valid generation available;
- transient uncorrectable ECC handling.

In the small simulated geometry, mounting 100 populated logical blocks takes
128 page reads (one per physical block) instead of 3,328 reads with eager full
verification. This is a read-count comparison, not a hardware timing claim.
The complete native suite passes with `-Wall -Wextra -Werror`.

Hardware observation on the MB249 unit with about 4 GB of music:

- previous eager verification: approximately 20 minutes;
- verification-on-first-access build: approximately 20 seconds.

## Building

Use the repository's normal build prerequisites from [BUILDING.md](BUILDING.md).
For an ARM cross compiler named `arm-none-eabi-*`:

```sh
mkdir build-nano3g-boot
cd build-nano3g-boot
../tools/configure --target=ipodnano3g --type=b --compiler-prefix=arm-none-eabi-
make "$PWD/sysfont.h" "$PWD/rbversion.h"
make -j4 bin

cd ..
mkdir build-nano3g-rockbox
cd build-nano3g-rockbox
../tools/configure --target=ipodnano3g --type=n --compiler-prefix=arm-none-eabi-
make "$PWD/sysfont.h" "$PWD/rbversion.h"
make -j4 bin
```

The hardware-tested build used GCC 14.2.0. The project configurator recommends
GCC 9.5.0, so reproduce and test with the toolchain you intend to distribute.

## Updating an existing Nano3Rockbox installation

This procedure is only for a device already formatted for Nano3Rockbox. It
does not erase or reformat the music volume.

1. Back up the current `.rockbox/rockbox.ipod`, `rockbox-info.txt`, and any
   irreplaceable files.
2. Copy the newly built `rockbox.ipod` and `rockbox-info.txt` into `.rockbox`.
   Stage them under temporary names first, verify their SHA-256 hashes, then
   rename them into place. Keep the old files as rollback copies.
3. Safely unmount the volume.
4. With the cable connected, hold **MENU + SELECT** for about 12 seconds to
   enter DFU (black screen).
5. Test the RAM-only bootloader first:

   ```sh
   mks5lboot --dfuscan
   mks5lboot --dfusend nano3g-lazy-verify.dfu
   ```

6. Confirm that Rockbox loads and the library is readable. Re-enter DFU and
   install the same tested bootloader permanently:

   ```sh
   mks5lboot --bl-inst bootloader-ipodnano3g.ipod
   ```

7. The installer emits an alive tone, then a dual success tone and reboots.
   A low 330 Hz failure tone means the install did not succeed; three such
   tones indicate NOR corruption and require recovery.

Do **not** use a NAND eraser or format the volume for this update.

## Critical Apple-firmware warning

Nano3Rockbox's FTL is not compatible with Apple's Whimory format. On a Nano 3G
whose NAND has been formatted by Nano3Rockbox, booting Apple's firmware with
HOLD or MENU can make Apple attempt a repair and destroy the Rockbox files.
Use MENU + SELECT for DFU. Treat this configuration as Rockbox-only unless you
perform a complete iTunes restore back to Apple's format.

## Rollback

If the updated Rockbox firmware does not start but bootloader USB mode is
available, restore the saved `rockbox.ipod` and `rockbox-info.txt`. To remove
the permanent Rockbox bootloader, follow the repository's normal uninstall and
iTunes-restore documentation; do not attempt to boot Apple's firmware against
the Rockbox-formatted NAND.

## Hardware-tested artifact hashes

The following SHA-256 values identify the exact artifacts used for the MB249
test. They are supplied for reproducibility; rebuilds from another commit or
toolchain will differ.

| Artifact | SHA-256 |
|---|---|
| `rockbox.ipod` | `BDD48EE3FF8FECEF970787325E65E73D8C26C2C337CA9083C4220B4A541A4549` |
| `bootloader-ipodnano3g.ipod` | `C17657C2C0739AEABE385121B47133CD6CFB551E80FC6C2D639985F3F7B977B0` |
| `nano3g-lazy-verify.dfu` | `E328330AC5CB912878AE4E9D0298E9A4A0428A958DBA9B06DE665C466E89AD7A` |

The diagnostic archive and any user data are intentionally not part of the
repository or release artifacts.
