# Nano3Rockbox installer source and build

This directory contains the source for the standalone Windows installer:

- `nano3g_installer.py` — tkinter application and install/uninstall logic.
- `Nano3RockboxInstaller.spec` — PyInstaller single-file executable definition.
- `prepare_payload.ps1` — stages the bootloader, `.rockbox` tree, `mks5lboot`, and optional Zadig driver tool.
- `build.bat` — prepares the payload and builds the EXE.
- `icon.ico` — installer icon.
- `CREDITS.md` — concise project credits.

## Requirements

Build on Windows with:

- Python 3 from [python.org](https://www.python.org/downloads/windows/), including the `py` launcher.
- PyInstaller (`py -m pip install pyinstaller`). `build.bat` installs it if missing.
- PowerShell and `robocopy` (included with Windows).
- An ARM bootloader build and staged Rockbox tree.
- `mks5lboot.exe` built from `utils/mks5lboot`.

The current payload-preparation script uses these project-local paths:

```text
C:\KIRO\Nano3Rockbox\rockbox-master\build-nano3g-boot\bootloader.bin
C:\KIRO\Nano3Rockbox\rockbox-master\build-nano3g-boot\bootloader-ipodnano3g.ipod
C:\KIRO\TEMP\rbstage\.rockbox
C:\KIRO\Nano3Rockbox\rockbox-master\utils\mks5lboot\mks5lboot.exe
```

It optionally picks up `C:\KIRO\iPodUniversalDecrypt\zadig.exe` and
`icon.ico`. If Zadig is absent, the installer still builds; its driver-setup
button is simply unavailable.

## Build

From this directory, run:

```powershell
.\build.bat
```

The script:

1. Checks the Python launcher and PyInstaller.
2. Runs `prepare_payload.ps1`.
3. Embeds the staged payload through `Nano3RockboxInstaller.spec`.
4. Writes the one-file executable under `dist\`.

To run the steps manually:

```powershell
py -m pip install pyinstaller
powershell -NoProfile -ExecutionPolicy Bypass -File .\prepare_payload.ps1
py -m PyInstaller --clean --noconfirm .\Nano3RockboxInstaller.spec
```

Do not commit `payload\`, `build\`, `dist\`, PyInstaller caches, or bundled
third-party/generated binaries. The payload is rebuilt from the project and
local staging paths by `prepare_payload.ps1`.

## Safety

The installer requires administrator rights and formats the Rockbox-visible
NAND volume. Installing Rockbox destroys Apple's on-NAND format; returning to
Apple's OS requires the documented erase-and-iTunes restore process. The
installer deliberately writes the persistent NOR bootloader only after the
filesystem has been formatted, copied, verified, and flushed.
