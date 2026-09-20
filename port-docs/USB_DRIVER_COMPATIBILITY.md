# Windows USB driver compatibility

## Problem

The Nano 3G's retail Apple firmware identifies its USB device as `05AC:1262`. Earlier Rockbox Nano 3G builds reused that same USB vendor/product ID.

On Windows PCs with iTunes / Apple Mobile Device Support installed, the Apple `AppleIPod` kernel driver (`oem33.inf`) can bind to the composite parent solely because it matches `USB\VID_05AC&PID_1262`. The temporary Rockbox bootloader still exposes a standards-compliant Bulk-Only Transport / SCSI mass-storage interface, but its Microsoft `USBSTOR` child then fails to start.

This looked like an installer stall at **Sending temporary bootloader**:

1. DFU transfer succeeds.
2. The iPod intentionally disconnects from DFU and enters Rockbox USB mode.
3. Windows sees a USB mass-storage interface but creates no usable disk, or reports Device Manager Code 10.
4. The installer waits for a disk which cannot appear.

The uninstall / restore path is unaffected because it uses DFU only and never requires temporary USB storage.

## Confirmed evidence

A diagnostic run on the affected PC recorded:

- parent instance `USB\VID_05AC&PID_1262...` bound to `AppleIPod` via `oem33.inf`;
- a `USBSTOR` mass-storage child with **ProblemCode 10** and NTSTATUS `0xC0000001`;
- stack containing `\Driver\AppleIPod`;
- Apple Mobile Device Service already stopped, proving that stopping the service cannot remove the installed kernel-driver binding.

The same temporary image works on PCs where the generic Windows storage stack owns the device.

## Resolution

Nano3Rockbox uses the Rockbox-specific Nano 3G transport identity **`05AC:127F`** for the temporary DFU-loaded image, persistent bootloader, NAND checker, and Rockbox firmware. Apple VID `05AC` remains because the hardware is an Apple device, but the product ID no longer matches the retail Apple driver rule.

Stock Apple firmware is not modified and continues to enumerate as its original `05AC:1262` identity.

On the formerly failing PC, the corrected image was hardware-validated as:

- parent `USB\VID_05AC&PID_127F...`;
- active `Rockbox Nano 3G NAND` disk device;
- normal Microsoft stack `{partmgr, disk, USBSTOR}`;
- `disk.inf` / `GenDisk`, `ProblemCode 0`.

Both a Hynix 8 GB MB261 (`A555D5AD`) and a Hynix 4 GB MA978 (`A514D3AD`) completed the safe check and full install through this path. Their copied `rockbox.ipod` files passed SHA-256 verification before the NOR bootloader was installed.

## Support diagnostics

The installer writes a timestamped `Nano3RockboxInstaller-log-*.txt` file to the Desktop. The log records the DFU commands, complete command output, disk/PnP snapshots, active device driver bindings, and USB/Kernel-PnP events around the temporary-storage handoff. This is the right file to attach when investigating a new Windows compatibility issue.
