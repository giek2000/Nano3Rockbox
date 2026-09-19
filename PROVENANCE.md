# Provenance and commit history

This repository was published from a **shallow clone** of upstream Rockbox.
A shallow clone is missing objects that its own boundary commits reference, so
the original branch could not be pushed intact — GitHub rejected it with
`did not receive expected object 85adf518…`. Rather than omit the work, the
published commit is a **snapshot of the complete source tree** with no parent
commits.

That means the per-commit history and its authorship is not preserved in git
here, so it is recorded below instead. Nothing about the code itself was
changed or removed.

Upstream Rockbox, with full history, is at <https://github.com/Rockbox/rockbox>.

## The iPod Nano 3G port series

These are the commits that made up the `nano3g-port` branch, oldest last:

| commit | date | author | subject |
|---|---|---|---|
| `ba69b8c` | 2026-09-19 | this work | mks5lboot: document validated Nano 3G NAND chips and the dual-boot caveat |
| `ab180dc` | 2026-09-18 | this work | ipodnano3g: Samsung MLC NAND support, original FTL, and boot fixes |
| `faee0d7` | 2026-09-17 | Andrew Rice | utils: update the Nano 3G NAND check tool |
| `4d85212` | 2026-09-17 | Andrew Rice | utils: iPod Nano 3G NAND and FTL tooling |
| `1a3b8c9` | 2026-09-17 | Andrew Rice | ipodnano3g: NAND check image for validating other chips |
| `14e55d6` | 2026-09-17 | Andrew Rice | rbutil: add the iPod Nano 3G |
| `4877632` | 2026-09-17 | Andrew Rice | manual: add the iPod Nano 3G |
| `53f710f` | 2026-09-17 | Andrew Rice | ipodnano3g: power, RTC, backlight, battery and audio |
| `3c13884` | 2026-09-17 | Andrew Rice | ipodnano3g: NAND driver, FTL and storage |
| `aed1945` | 2026-09-13 | Andrew Rice | mks5lboot: support the iPod Nano 3G |
| `66bc072` | 2026-09-12 | Andrew Rice | ipodnano3g: preserve PMU register 0x10 bit 2, which the NAND needs |

Everything below `3c13884` in that list, plus all the S5L8702 platform support
and the Rockbox core, is other people's work. See the
[Provenance section of the README](README.md#provenance) for the detailed
breakdown of which parts of the NAND/FTL code are original to this work and
which were carried over — in particular, the FMC controller register
sequences in `nand-nano3g.c` were retained from `3c13884` as documented
hardware facts, while `ftl-nano3g.c` and `nand_vendor.c` are independent.

## Verifying the snapshot

The tree published here is byte-identical to the working tree it was taken
from; the commit was created with `git commit-tree` against the existing tree
object rather than by re-adding files, so no content was normalised or lost in
the process.
