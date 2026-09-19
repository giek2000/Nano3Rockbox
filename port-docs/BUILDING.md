# Building from source

[← back to README](../README.md)

Most people do not need this — the [Windows app](../README.md#install-rockbox)
ships everything prebuilt. This page is for developers who want to build the
firmware, the tools, or the installer themselves.

## Toolchain

Requires an `arm-elf-eabi` toolchain, which Rockbox's own script installs:

```sh
tools/rockboxdev.sh
```

## Firmware

Builds happen in an out-of-tree build directory. The shipping firmware uses the
page-level FTL (`FTL_NANO3G_V2`).

```sh
# Bootloader (loads rockbox.ipod from the NAND, written to NOR)
../tools/configure --target=ipodnano3g --type=b \
    --extra-defines="FTL_NANO3G_V2" && make

# Main firmware
../tools/configure --target=ipodnano3g --type=n \
    --extra-defines="FTL_NANO3G_V2" && make

# NAND check image (read-only): identifies the chip and reports whether it is
# supported, before anything is written. This is what the app's
# "Check my iPod (safe)" button runs.
../tools/configure --target=ipodnano3g --type=b \
    --extra-defines="NAND_CHECK FTL_NANO3G_V2" && make
```

## Host-side FTL tests (no hardware needed)

```sh
cd utils/ipodnano3g/ftltest2
make check            # v1 FTL
make clean && make FTL=v2 && make check   # v2 FTL (what ships)
```

## Tools

- `utils/mks5lboot/` — the DFU + NOR install tool (Windows build, static
  libusb).
- `utils/ipodnano3g/installer/` — the one-click Windows installer. It bundles
  the bootloader images, the full `.rockbox` tree, `mks5lboot`, the eraser and
  check images, and Zadig into a single self-contained EXE.

## Validating a new NAND chip

Support for a chip is gated on hardware validation — see
[the FTL design notes](FTL_DESIGN.md#which-chips-are-supported) and
[Devices used](../NANO3G_TEST_UNITS.md) for the process and the chips validated
so far.
