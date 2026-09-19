# How storage works on this port (the FTL)

[← back to README](../README.md)

The iPod Nano 3G has no disk. Storage is raw NAND flash wired directly to the
S5L8702's Flash Memory Controller (FMC). Before Rockbox can keep a filesystem
on it, something has to turn that raw flash into a reliable block device that
tolerates power loss and spreads wear — a *flash translation layer* (FTL).

This port uses its **own** FTL, deliberately not a reimplementation of Apple's
(Whimory), whose on-flash format is proprietary and undocumented. Guessing at
Apple's format risks silently corrupting data. The cost of using our own format
is that the device becomes single-purpose: to go back to Apple's OS the NAND
must be erased first (see
[Going back to Apple's OS](GOING_BACK_TO_APPLE.md)).

## Two FTL generations

**v1 — block-level (the original bring-up FTL).** Each logical block mapped to
one physical erase block; any write rewrote the whole 512 KB block into a fresh
one. Correct and crash-safe, but very slow: copying an album could take tens of
minutes because every small write re-read and re-wrote up to 128 untouched
pages. Kept in the tree (`ftl-nano3g.c`) as the reference implementation.

**v2 — page-level log-structured (what ships now).** Writes append to an open
"frontier" block a page at a time, so a write costs about one page program
instead of a whole-block rewrite. Measured on hardware: an album copy runs at
roughly **2.5 MB/s** versus v1's ~7.5 KB/s, and mounting takes about a tenth of
a second. Built with the `FTL_NANO3G_V2` flag (`ftl-nano3g-v2.c`).

Key properties of v2:

- **Self-describing pages.** Every page's spare area carries a magic value, a
  type, its logical page number and a monotonic sequence number, so the newest
  copy of any logical page always wins.
- **Per-block summary page.** The last page of each block records the block's
  contents, so mounting reads roughly one page per block (about 32k reads on an
  8 GB chip) instead of scanning all ~2 million pages — the difference between
  a fraction of a second and a multi-minute hang.
- **Crash safety.** A completed write is already durable; there is no deferred
  commit to lose. Power loss mid-write leaves the previous contents intact.
- **Garbage collection + wear levelling.** Stale pages are reclaimed by
  relocating live pages and erasing the emptied block; allocation prefers
  least-erased blocks, and blocks that fail erase/program are retired.

## Which chips are supported

Writes are gated on an explicit **validated-chip table**
(`nano3g_validated_chips[]` in `nand_vendor.c`). A chip only gets a row there
after it has been erased, written, read back and verified byte-for-byte on real
hardware. An unrecognised chip is still fully **readable**, but mounts
**read-only** rather than risk writing to a part nobody has tested.

The units validated so far, and the exact chip IDs, are listed in
[Devices used](../NANO3G_TEST_UNITS.md). For *why* each chip has to be validated
individually (and how Apple sidesteps that), see
[NAND chip identification](NAND_CHIP_IDENTIFICATION.md).

## Tested in software too

A host-side test harness (`utils/ipodnano3g/ftltest2/`) exercises the FTL
without hardware — write amplification, mount cost, crash-during-write,
crash-during-GC, foreign-format handling, and the validated-chip table — for
both the v1 and v2 FTLs. Run it with `make check` (v1) or `make FTL=v2 && make
check` in that directory.
