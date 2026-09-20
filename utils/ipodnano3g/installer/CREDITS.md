# Installer credits

The Nano 3G installer is part of the Nano3Rockbox project and is released
under the same GPLv2-or-later licensing as the surrounding Rockbox source.

Credits:

- **Rockbox contributors** — Rockbox core, S5L8702 platform support, storage
  interfaces, FAT support, and the `.rockbox` runtime tree.
- **Nano3Rockbox contributors** — Nano 3G bring-up, NAND support, FTL work,
  DFU workflow, safety checks, installer UI, and hardware validation.
- **freemyipod / wInd3x contributors** — the DFU exploit/workflow that makes it
  possible to run unsigned Nano 3G bootloader images.
- **Andrew Rice** — reverse-engineering and documentation of the iPod Nano 3G
  platform that helped inform this port.
- **mks5lboot contributors** — DFU and Nano 3G NOR image tooling.
- **PyInstaller contributors** — single-file Windows executable packaging.
- **Zadig contributors** — optional WinUSB driver installation utility bundled
  by the payload-preparation step when available.

The installer does not redistribute Apple firmware. It embeds only the
Rockbox/Nano3Rockbox payloads and the tools listed by the build scripts.
