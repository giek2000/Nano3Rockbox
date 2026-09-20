# -*- mode: python ; coding: utf-8 -*-
#
# Standalone, single-file Windows installer for Rockbox on the iPod Nano 3G,
# FTL v2 (page-level log-structured FTL -- fast writes).
#
# Identical embedding to the v1 spec; the difference is entirely in the
# payload/ directory, which prepare_payload_v2.ps1 fills from the v2 build
# dirs so the NOR bootloader and the NAND rockbox.ipod are both v2 (they must
# match -- a v1 bootloader cannot mount a v2 NAND).
#
#   mks5lboot.exe               DFU + NOR tool (static libusb, no DLL)
#   bootloader.bin              v2 bootloader, run temporarily from DFU
#   bootloader-ipodnano3g.ipod  v2 bootloader, written to NOR
#   rockbox_files/.rockbox      v2 firmware, codecs, plugins
#   zadig.exe                   USB driver setup fallback
#
# uac_admin=True: formatting and raw volume access need elevation.

# Derive the EXE filename from APP_VERSION in nano3g_installer.py so the two
# never drift. Bump APP_VERSION there and the output name follows automatically.
import re as _re
with open('nano3g_installer.py', 'r', encoding='utf-8') as _fh:
    _m = _re.search(r'^APP_VERSION\s*=\s*"([^"]+)"', _fh.read(), _re.M)
APP_VERSION = _m.group(1) if _m else '0.0.0'
EXE_NAME = f'Nano3RockboxInstaller_v{APP_VERSION}-ftlv2'

a = Analysis(
    ['nano3g_installer.py'],
    pathex=[],
    binaries=[],
    datas=[
        ('payload/mks5lboot.exe', '.'),
        ('payload/bootloader.bin', '.'),
        ('payload/bootloader-ipodnano3g.ipod', '.'),
        ('payload/eraser.bin', '.'),
        ('payload/check.bin', '.'),
        ('payload/zadig.exe', '.'),
        ('payload/rockbox_files', 'rockbox_files'),
        ('_ftlvariant.txt', '.'),
        # Needed at runtime as well as for EXE resources: App.iconbitmap()
        # loads this from PyInstaller's extracted resource directory.
        ('icon.ico', '.'),
    ],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=['numpy', 'pandas', 'matplotlib', 'scipy', 'PIL', 'cv2',
              'pytest', 'setuptools', 'pip'],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name=EXE_NAME,
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    uac_admin=True,
    icon='icon.ico',
)
