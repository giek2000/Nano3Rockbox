# Why this port validates NAND chips one at a time

[← back to README](../README.md) · see also [How storage works (the FTL)](FTL_DESIGN.md)

A recurring question: Apple's firmware runs on every Nano 3G regardless of which
flash chip is inside, so why does this port have to identify and hardware-test
each chip individually before it will write to it? Is that a limitation of our
implementation?

Short answer: Apple does not have a magic "blanket" NAND driver. Apple and this
port need the *same* information about a chip. The difference is entirely in how
that information is obtained — Apple captured it at manufacture time; we have to
recover it safely from the outside.

## What any FTL must know about the chip

Before writing, an FTL needs the chip's real geometry:

- page size, spare (OOB) size, pages per erase block — and, critically,
- **blocks per bank** (the total capacity of each die).

Get blocks-per-bank wrong and the FTL addresses past the end of the die, which
corrupts data. Neither Apple nor this port can avoid needing this number.

## How Apple avoids "probing"

Apple builds the whole device, so it controls both ends of the problem:

1. **It already knows which chip is fitted** — the part is in Apple's bill of
   materials for that production run. There is no discovery problem; Apple
   chose the part.
2. **The geometry is written into the device at the factory.** The NAND carries
   Apple's own FTL/VFL metadata, and per-device configuration lives in the
   SysCfg area on NOR. Apple's firmware reads a stored, factory-validated
   descriptor rather than working capacity out at boot.
3. **Apple can carry a complete chip table.** Its disk firmware maps a chip's
   READ-ID directly to a known-good geometry row, because the set of parts it
   ever shipped in that model is closed and known to Apple.

So Apple's "it just works" is really *a closed, known set of parts plus geometry
persisted at manufacture*. The hard information was captured before the unit
ever left the factory.

## Why this port cannot simply do the same

We are on the outside of that process:

- We do **not** have Apple's chip table, and total die capacity **cannot** be
  derived from the 4-byte READ ID alone. The extended-ID byte encodes
  page/spare/erase-block *sizes* (a public convention, which is why the generic
  decode in `nand_vendor.c` gets those fields right), but it does **not** encode
  total capacity. That needs a 5th density byte, an ONFI parameter page, or a
  vendor part-number lookup — none reliably available/documented for these
  2007-era MLC parts.
- Apple's stored geometry is in Apple's on-flash format, which this port
  deliberately does not parse (reading it wrong risks silent corruption — the
  whole reason we use our own FTL; see [FTL_DESIGN.md](FTL_DESIGN.md)).

That leaves two ways to get capacity: probe it, or look it up in a table we
build ourselves.

## Why the runtime probe is not trustworthy here

`nand-nano3g.c`'s `probe_bank_capacity_blocks()` grows the block index until a
read fails, then binary-searches the boundary. On these chips that does **not**
find the real end of the die:

> Raw NAND chips only decode as many row-address bits as their real page count
> needs, and typically ignore extra high-order bits in an out-of-range address
> rather than erroring — so a read past the chip's real end can *alias* back
> onto an in-range page and return a perfectly normal success.

This was confirmed on hardware: the probe cap was raised from 16384 to 1048576
blocks and **every** read still "succeeded" (`probestop 0`), because the
addresses were aliasing rather than reaching new storage. Consequently every
unit tested reports `blocks 16384 blocks_unreliable 1` in the check tool — the
probe literally cannot locate the die boundary. This is controller/chip
behaviour, not a bug in our code.

Because the probe is unsafe, we do not trust it for writes.

## What we do instead: a hardware-validated chip table

`nano3g_validated_chips[]` in `nand_vendor.c` supplies the **real**
blocks-per-bank for each part we have physically tested. A chip earns a row only
after an on-hardware erase / program / read-back that matches byte-for-byte
(data *and* spare) across all four banks, via the `-DNAND_CHECK_ALLOW_WRITE_TEST`
image. An unrecognised chip stays fully **readable** but mounts **read-only**,
so an untested part is never at risk. In effect we are rebuilding Apple's
per-part table from the outside, one physically-proven unit at a time. The
validated units and their exact IDs are in
[NANO3G_TEST_UNITS.md](../NANO3G_TEST_UNITS.md).

## Observed device-ID → capacity pattern (informative, not yet relied upon)

Across the validated parts, the READ-ID **device byte** tracks per-die density
in the usual vendor convention:

| Device ID | Density/die | Unit example | Page | Blocks/bank |
|-----------|-------------|--------------|------|-------------|
| `0xD5`    | 16 Gbit (2 GiB) | Micronas/Hynix/Toshiba 8 GB (4 dies); Intel/Micronas 4 GB (2 dies) | 4096 B or 2048 B | 4096 (4 KB page) / 8192 (2 KB page) |
| `0xD3`    | 8 Gbit (1 GiB)  | Hynix 4 GB (4 dies) | 2048 B | 4096 |

The same 2 GiB/die `0xD5` part appears in both 4-die (8 GB) and 2-chip-enable
(4 GB) packages: the Intel 4 GB (`A5D5D589`) and the Micronas 4 GB (`B614D5EC`,
model MB245) are 2-CE versions of the same die used in the 4-die 8 GB units.
Capacity therefore does not follow from the device ID alone; it depends on how
many chip enables are populated, which the driver determines at runtime from
the present-bank scan. (`EC` is JEDEC's Micronas code, historically mislabelled
"Samsung" in this project; Samsung's real JEDEC ID is `0xCE`.)

Blocks-per-bank then follows from density ÷ (page_size × pages_per_block). This
is consistent with every part validated so far, but it is currently treated as
corroboration for the hand-verified table values, **not** as an automatic
source of truth — a plausible-looking capacity that is wrong still corrupts
data, so it must be confirmed on hardware before a chip is trusted.

## Could this become more "blanket" like Apple's?

Three options, roughly by increasing safety/effort payoff:

1. **Read the 5th ID byte / ONFI parameter page** where present. If reliable
   across these parts, the generic decode could compute blocks-per-bank
   directly, demoting the validated table to a fallback/override and removing
   the need to hand-test each new chip. This is the only option that would
   *safely* eliminate per-chip validation — but it needs the raw data captured
   from real units first (a candidate addition to the `-DNAND_CHECK` dump).
2. **Formalise the device-ID density mapping above** into the decode. Cheaper,
   but it is a convention, not a guarantee; it should only ever be a hint
   backing the validated value, never override a hardware result.
3. **Keep growing the validated table** (the current path). Each unit is tested
   once and then works for everyone with that chip; over time the table
   approaches Apple's coverage for the parts that actually exist in the wild.
