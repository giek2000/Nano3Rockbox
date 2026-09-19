# -*- mode: python ; coding: utf-8 -*-
#
# Standalone, single-file Windows installer for Rockbox on the iPod Nano 3G.
#
# Everything needed is embedded, so the EXE has no external dependencies:
#   mks5lboot.exe               DFU + NOR tool. libusb is linked statically
#                               (objdump shows only KERNEL32/msvcrt/SETUPAPI/
#                               USER32), so no libusb-1.0.dll is required --
#                               unlike wInd3x, which needs the DLL alongside it.
#   bootloader.bin              run temporarily from DFU to expose the NAND
#   bootloader-ipodnano3g.ipod  scrambled image written to NOR
#   rockbox_files/.rockbox      firmware, codecs, plugins, fonts, themes
#   zadig.exe                   USB driver setup if libusb cannot see the DFU
#                               device (it needs Zadig's WinUSB GUID; Apple's
#                               own driver uses one libusb cannot open)
#
# uac_admin=True: formatting and raw volume access need elevation, so Windows
# prompts on launch rather than the install failing halfway through.

a = Analysis(
    ['nano3g_installer.py'],
    pathex=[],
    binaries=[],
    datas=[
        ('payload/mks5lboot.exe', '.'),
        ('payload/bootloader.bin', '.'),
        ('payload/bootloader-ipodnano3g.ipod', '.'),
        ('payload/zadig.exe', '.'),
        ('payload/rockbox_files', 'rockbox_files'),
    ],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    # Nothing here needs the scientific stack or image libraries; excluding
    # them keeps the binary from ballooning.
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
    name='Nano3RockboxInstaller_v1.1.0',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    # The .rockbox payload is mostly already-compressed data; UPX on the
    # bundled binaries still helps a little and matches the sibling project.
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
