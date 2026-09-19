# Fixes made while bringing this port up

[← back to README](../README.md)

Five defects stood between "driver written" and "Rockbox boots". Four are in
*shared* Rockbox code and are not Nano 3G specific. Recorded here for anyone
doing similar S5L87xx work.

1. **DMA buffer alignment** (`nand-nano3g.c`, `ftl-nano3g.c`) — the actual boot
   blocker. Page/spare buffers were plain `static uint8_t[]`. The FMC DMAs
   straight into the caller's buffer and ignores the low address bits, so a
   buffer 1–3 bytes off a word boundary transfers **displaced by that offset
   while reporting success**, running past the end of the buffer. gcc happened
   to align them in the bootloader build and misalign them in the main
   firmware, so the bootloader mounted the filesystem and loaded the firmware
   perfectly while that same firmware saw the FAT signature `0xAA55` as
   `0x00AA` and declared intact media empty. Now carries `NAND_DMA_BUF_ATTR`
   (32-byte, which also makes the driver's cache maintenance safe).

2. **`FTL_READONLY` on any `BOOTLOADER` build** (`ftl-target.h`) — refused every
   host write before it reached the FTL. A bootloader serving USB mass storage
   is the only thing that can format a never-initialised device, since the main
   firmware lives on the filesystem that does not exist yet. Now scoped to
   `BOOTLOADER && !HAVE_BOOTLOADER_USB_MODE`.

3. **USB teardown mid-session** (`bootloader/ipod-s5l87xx.c`) — any
   `SYS_USB_DISCONNECTED` was treated as unplug and called `usb_close()`,
   posting `USB_QUIT` whose handler is `thread_exit()`. But
   `usb_release_exclusive_storage()` broadcasts that event during ordinary
   re-configuration, which Windows triggers with a second `SET_CONFIGURATION`.
   With `usb_thread` dead, the next bus reset latched `bus_reset_pending`
   forever and every later SETUP packet was silently dropped. Also: every
   `SYS_USB_CONNECTED` broadcast now gets its own ack, not just the first.

4. **`panicf("mount: %d")` on unplug** (`firmware/usb.c`) —
   `usb_slave_mode(false)` panicked when no partition existed, which is the
   normal state of a device being formatted for the first time, and destroyed
   the diagnostics needed to understand why. Non-fatal in bootloader builds.

5. **`SYNCHRONIZE CACHE` (0x35) unhandled** (`firmware/usbstack/usb_storage.c`)
   — fell through to `default:` and was answered `SENSE_ILLEGAL_REQUEST`. Now
   honoured via `storage_flush()`.

Also added during bring-up: an on-device debug log ring with a paged LCD viewer
(`nand_debug_log()` + the bootloader's viewer), which is how the alignment bug
was ultimately found, and bounce-buffering of unaligned caller buffers at the
storage API boundary.
