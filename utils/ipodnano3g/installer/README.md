# Standalone Rockbox installer for the iPod Nano 3G

This directory contains the source and reproducible build files for the
self-contained Windows installer for the iPod Nano 3G (N46 / S5L8702). The
installer embeds the Rockbox bootloader images, `.rockbox` tree, `mks5lboot`,
and optional Zadig support; the built EXE has no runtime Python or libusb DLL
dependency.

The application source is `nano3g_installer.py`. Its `APP_VERSION` is the
single source of truth for the generated EXE version.

See:

- [`BUILD.md`](BUILD.md) for prerequisites, payload staging, and exact build
  commands.
- [`CREDITS.md`](CREDITS.md) for concise project and tool credits.

## Why the NOR write is last

The installer runs in five stages, and the order is the point:

1. **Expose the NAND as a disk Rockbox's FTL can use.** If a bootloader is
   already installed, the iPod presents that disk by itself and this stage is a
   no-op — no DFU, no driver, nothing is written. Otherwise the bootloader is
   run temporarily from DFU: volatile, nothing is written to the device, but it
   exposes the raw NAND to Windows as a mass-storage disk through Rockbox's own
   FTL. A *stock* device's disk does not count here — Apple's firmware presents
   the NAND in Apple's FTL format, which Rockbox cannot read.
2. **Format** the exposed disk FAT32.
3. **Copy** the `.rockbox` tree and verify `rockbox.ipod` by SHA256.
4. **Flush** the volume so FAT metadata reaches NAND before power is cut.
5. **Patch NOR** with the persistent dual bootloader.

If anything fails in stages 1–4, NOR was never touched and the device is
exactly as it was. Because the files are already in place when the patch lands,
the device boots straight into Rockbox rather than showing `Can't load
rockbox.ipod: File not found`.

Stage 1 skipping DFU also makes the installer useful as a repair tool: tick
**Do not write NOR** on a device that already has the bootloader and it just
re-lays the `.rockbox` tree over USB.

## Safety warnings

Installing **erases the iPod completely, including Apple's firmware**.
Rockbox's FTL format is incompatible with Apple's, and Apple's OS lives on the
NAND (NOR is only 1 MB and holds just Apple's 128 KB bootloader). Returning to
Apple's OS requires the documented erase-and-iTunes restore process.

**Never boot with the HOLD switch on afterwards.** On a Rockbox-formatted
device Apple's firmware writes to the NAND while trying to repair storage it
cannot read, which wipes the Rockbox install. Treat this as a single-boot
device.

The installer restricts target disks to the exact Rockbox FTL inquiry names
`Apple Samsung`, `Apple Hynix`, and `Apple Toshiba`, requires USB transport,
and enforces a 1–20 GB safety range. It never accepts Apple's `Apple iPod`
disk name.

## What the installer handles

- Releases Apple Mobile Device Service and leftover `wInd3x` processes that
  can claim the DFU interface.
- Verifies stock DFU state 2 before sending an image.
- Formats the Rockbox pseudo-partition directly instead of relying on a Windows
  free-extent operation that fails on this device.
- Verifies `rockbox.ipod` by SHA256 after copying.
- Flushes and dismounts the volume before the persistent NOR step.
- Reports the intentionally slow NAND copy progress rather than appearing
  hung.

## Files

| file | purpose |
|---|---|
| `nano3g_installer.py` | tkinter application and install/uninstall logic |
| `icon.ico` | application icon |
| `Nano3RockboxInstaller.spec` | PyInstaller single-file definition |
| `prepare_payload.ps1` | stages the embedded payload |
| `build.bat` | runs payload preparation and PyInstaller |
| `BUILD.md` | reproducible build instructions and prerequisites |
| `CREDITS.md` | concise credits |

Generated `payload`, `build`, `dist`, and Python cache contents are deliberately
not source and should not be committed. The payload-preparation script rebuilds
them from the Rockbox tree and local tool/staging paths.
