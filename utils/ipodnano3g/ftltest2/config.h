/*
 * Minimal stand-in for Rockbox's firmware/export/config.h, just enough for
 * ftl-nano3g.c and nand_vendor.c to compile natively on the host for
 * testing. This is NOT used in the real firmware/bootloader build (which
 * uses the real config.h via the normal include path); it exists solely
 * so our FTL and NAND-ID-decoding logic can be exercised, unmodified,
 * against a simulated NAND on a development machine before it ever runs
 * against real hardware.
 */
#ifndef STUB_CONFIG_H
#define STUB_CONFIG_H
/* BOOTLOADER intentionally left undefined: the test harness exercises the
 * read/write-capable path (ftl-target.h's FTL_READONLY is only set when
 * BOOTLOADER is defined). */
#endif
