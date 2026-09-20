# Nano3Rockbox installer source and build

This directory contains the source for the standalone **FTL v2** Windows installer:

- `nano3g_installer.py` — tkinter application, install/uninstall workflow, safe NAND check, and diagnostic logging.
- `Nano3RockboxInstaller.spec` — PyInstaller one-file executable definition. The output name is derived from `APP_VERSION` in the Python source.
- `prepare_payload.ps1` — stages internally consistent v2 bootloaders, checker, eraser, `.rockbox` tree, `mks5lboot`, and optional Zadig.
- `build.bat` — refreshes the v2 payload and builds the EXE.
- `_ftlvariant.txt` — embeds the visible FTL v2 label.
- `icon.ico` — installer icon.
- `CREDITS.md` — concise project credits.

## Requirements

Build on Windows with:

- Python 3 from [python.org](https://www.python.org/downloads/windows/), including the `py` launcher.
- PyInstaller (`py -m pip install pyinstaller`). `build.bat` installs it if missing.
- PowerShell and `robocopy` (included with Windows).
- Fresh v2 bootloader, checker, eraser, and main-firmware builds.
- A staged v2 `.rockbox` tree.
- `mks5lboot.exe` built from `utils/mks5lboot`.

The payload script intentionally uses these project-local locations:

```text
C:\KIRO\Nano3Rockbox\rockbox-master\build-nano3g-boot-v2\bootloader.bin
C:\KIRO\Nano3Rockbox\rockbox-master\build-nano3g-boot-v2\bootloader-ipodnano3g.ipod
C:\KIRO\Nano3Rockbox\rockbox-master\build-nano3g-erase\bootloader.bin
C:\KIRO\Nano3Rockbox\rockbox-master\build-nano3g-check\bootloader.bin
C:\KIRO\TEMP\rbstage-v2\.rockbox
C:\KIRO\Nano3Rockbox\rockbox-master\utils\mks5lboot\mks5lboot.exe
```

It optionally picks up `C:\KIRO\iPodUniversalDecrypt\zadig.exe`. If Zadig is absent, the installer still builds; its driver-setup button is unavailable.

## Build

From this directory, run:

```powershell
.\build.bat
```

The script:

1. Checks the Python launcher and PyInstaller.
2. Runs `prepare_payload.ps1` to stage a consistent FTL v2 payload.
3. Embeds the payload through `Nano3RockboxInstaller.spec`.
4. Writes `dist\Nano3RockboxInstaller_v<APP_VERSION>-ftlv2.exe`.

To run the steps manually:

```powershell
py -m pip install pyinstaller
powershell -NoProfile -ExecutionPolicy Bypass -File .\prepare_payload.ps1
py -m PyInstaller --clean --noconfirm .\Nano3RockboxInstaller.spec
```

## USB driver compatibility

The Rockbox Nano 3G images enumerate as `05AC:127F`, rather than Apple retail `05AC:1262`. This prevents iTunes' `AppleIPod` kernel driver from claiming the temporary Rockbox mass-storage image and leaving Windows' `USBSTOR` child unable to start. See [USB driver compatibility](../../../port-docs/USB_DRIVER_COMPATIBILITY.md) for the confirmed diagnosis and validation.

## Safety

The installer requires administrator rights and formats the Rockbox-visible NAND volume. Installing Rockbox destroys Apple's on-NAND format; returning to Apple's OS requires the documented erase-and-iTunes restore process. The installer writes the persistent NOR bootloader only after the filesystem has been formatted, copied, SHA-256 verified, and flushed.

Generated `payload`, `build`, `dist`, PyInstaller caches, and bundled third-party/generated binaries are deliberately excluded from source control. The release EXE is deliberately tracked under `releases/` as a distribution artifact.
