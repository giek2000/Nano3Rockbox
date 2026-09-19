# Going back to Apple's OS

[← back to README](../README.md)

Installing Rockbox reformats the NAND into this port's own format, which is
incompatible with Apple's. Apple's OS lives on the NAND (the NOR chip is only
1 MB and holds just Apple's small bootloader), so installing Rockbox removes
Apple's OS.

You can get back to Apple's OS, but not by a normal iTunes restore alone.
Apple's firmware cannot initialise a chip that still carries Rockbox's FTL
data — it *can* initialise a blank chip (that is how they leave the factory).
So the NAND has to be erased back to blank first, then iTunes can restore.

## The easy way: the installer's Uninstall button

The Windows app has an **Uninstall (restore Apple)** button that does this for
you:

1. It restores Apple's bootloader to NOR.
2. It erases the NAND back to blank (all four banks).
3. The iPod reboots into recovery mode.
4. Open **iTunes** and restore the iPod — it will offer to reinstall Apple's
   software.

The app walks you through the two short DFU steps in its log window and waits
for the iPod's own on-screen prompts, so there is nothing to time by hand.

## What's happening underneath

The erase step is a small bootloader built with `NANO3G_ERASE_ALL`, run
volatile from DFU (nothing is written except the erase itself). It counts down
a few seconds on screen, erases every block of every bank, and reports how many
failed — a small nonzero count is normal, those are the chip's genuine factory
bad blocks.

Once the chip is blank, Apple's bootloader sees blank silicon and drops into
its recovery state, which is what iTunes talks to.

This has been confirmed end to end on hardware.
