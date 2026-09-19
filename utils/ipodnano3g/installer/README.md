# Standalone Rockbox installer for the iPod Nano 3G

Builds a single self-contained Windows EXE that installs Rockbox on an iPod
Nano 3G (N46 / S5L8702). No Python, no libusb DLL, no separate tools: the
bootloader images, the `.rockbox` tree, `mks5lboot` and Zadig are all embedded.

```
build.bat            ->  dist\Nano3RockboxInstaller_v1.1.0.exe   (~26 MB)
```

## Why the NOR write is last

The installer runs in five stages, and the order is the point:

1. **Expose the NAND as a disk Rockbox's FTL can use.** If a bootloader is
   already installed, the iPod presents that disk by itself and this stage is a
   no-op — no DFU, no driver, nothing. Otherwise the bootloader is run
   temporarily from DFU: volatile, nothing is written to the device, but it
   exposes the raw NAND to Windows as a mass-storage disk through Rockbox's own
   FTL. A *stock* device's disk does not count here — Apple's firmware presents
   the NAND in Apple's FTL format, which Rockbox cannot read, so files copied
   to it would be invisible.
2. **Format** the exposed disk FAT32.
3. **Copy** the `.rockbox` tree and verify `rockbox.ipod` by SHA256.
4. **Flush** the volume so FAT metadata reaches NAND before power is cut.
5. **Patch NOR** with the persistent dual bootloader.

If anything fails in stages 1–4, NOR was never touched and the device is
exactly as it was. And because the files are already in place when the patch
lands, the device boots straight into a working Rockbox rather than coming up
to `Can't load rockbox.ipod: File not found`.

Stage 1 skipping DFU also makes the installer usable as a repair tool: tick
**Do not write NOR** on a device that already has the bootloader and it just
re-lays the `.rockbox` tree over USB.

## What it handles for you

Each of these cost real debugging time during bring-up:

- **Apple Mobile Device Service claims the DFU device**, making libusb report
  `bad access` (`LIBUSB_ERROR_ACCESS`). Stopped before use, restarted after.
- **A leftover `wInd3x` process holds the DFU interface exclusively**, with the
  same symptom. Terminated before use.
- **`wInd3x` and `mks5lboot` cannot share a DFU session.** wInd3x's "Haxed DFU"
  replaces the stock implementation, after which `mks5lboot` sees DFU state 9
  (`dfuERROR`) instead of state 2 (`dfuIDLE`), and `--dfureset` does *not*
  clear it. The installer verifies state 2 before writing NOR and tells the
  user to power-cycle if it sees anything else.
- **`New-Partition` fails here** with "Not enough available capacity": Windows
  exposes this removable device as one pseudo-partition at offset 0 spanning
  the whole disk ("superfloppy"), so there is no free extent. The volume is
  formatted directly instead, which also matches what Rockbox's `disk_mount()`
  looks for first (`fat_mount(drive, 0)`).
- **The copy is slow by design** — every write rewrites a full 512 KB erase
  block — so progress is reported and the UI says so, rather than looking hung.

Every destructive step re-checks the target disk by name, bus type *and* size,
so a disk number changing between stages cannot cause the wrong device to be
formatted.

## Warnings it surfaces

Installing **erases the iPod completely, including Apple's firmware**.
Rockbox's FTL format is incompatible with Apple's, and Apple's OS lives on the
NAND (NOR is only 1 MB and holds just Apple's 128 KB bootloader). Going back to
Apple's OS is possible, but iTunes cannot do it directly on a Rockbox-formatted
device — the NAND has to be erased back to blank first (Apple's firmware
refuses a chip carrying a foreign FTL's metadata but accepts blank silicon).
See "Going back to Apple's OS" in the top-level `README.md`.

**Never boot with the HOLD switch on afterwards.** The dual bootloader hands
off to Apple's bootloader as designed, but on a Rockbox-formatted device
Apple's firmware writes to the NAND trying to repair storage it cannot read,
which wipes the Rockbox install. Confirmed on hardware. Treat this as a
single-boot device.

## Building

Needs Python (via the `py` launcher — note the MSYS2 python on `PATH` has no
PyInstaller) and PyInstaller, which `build.bat` installs if absent.

`prepare_payload.ps1` gathers the payload from:

| component | source |
|---|---|
| `mks5lboot.exe` | `utils/mks5lboot/` (build with MinGW + static libusb) |
| `bootloader.bin`, `bootloader-ipodnano3g.ipod` | `build-nano3g-boot/` |
| `.rockbox` tree | staged via `tools/buildzip.pl --install=<dir>` |
| `zadig.exe` | bundled from a sibling project, for driver setup |

`mks5lboot.exe` must be linked against **static** libusb so no DLL is needed.
Verify with `objdump -p mks5lboot.exe`: it should import only `KERNEL32`,
`msvcrt`, `SETUPAPI` and `USER32`.

The spec sets `uac_admin=True`, so Windows prompts for elevation at launch
rather than letting the install fail partway through at the format step.

## Files

| file | purpose |
|---|---|
| `nano3g_installer.py` | the application (tkinter UI + install logic) |
| `icon.ico` | application icon, embedded in the EXE |
| `Nano3RockboxInstaller.spec` | PyInstaller spec, single-file + elevation |
| `prepare_payload.ps1` | gathers the embedded payload |
| `build.bat` | payload + PyInstaller build |

A plain PowerShell equivalent, without the GUI or bundling, is in
[`../install-nano3g.ps1`](../install-nano3g.ps1).
