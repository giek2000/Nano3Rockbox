/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") low-level NAND (FMC) driver.
 *
 * Provenance of the hardware facts relied on:
 *
 *  - FMC controller MMIO base 0x38a00000: independently confirmed present
 *    as a literal-pool constant in the decrypted OSOS 1.1.3 firmware for
 *    this exact device, referenced by controller bring-up code that zeroes
 *    two 32-bit registers at that base early in NAND initialisation. This
 *    also matches the freemyipod/wInd3x project's independent BootROM-side
 *    analysis of the same SoC family, exercised against real hardware.
 *  - The register *offsets* (command, address, byte/word counters,
 *    status, FIFO, and the extra syndrome/ECC-correction/transfer
 *    registers used below) describe the physical layout of a real
 *    hardware peripheral, not an original creative work; we record them
 *    as hardware facts (the way a datasheet's register table would be
 *    used), cross-checked against the wInd3x project's independently
 *    obtained BootROM-side driver, which agrees with them.
 *  - This controller does not behave like a generic ONFI/JEDEC NAND
 *    controller: readiness is not reliably signalled by the RBBDONE
 *    status bit (confirmed by two failed hardware attempts against a
 *    real unit, both following the generic assumption and seeing the
 *    chip never respond), but by issuing a READ STATUS command and
 *    polling a syndrome register for the status byte's ready bit -- and
 *    a page transfer is a multi-stage, per-512-byte-chunk sequence
 *    through dedicated ECC-correction and "AUTOXFER" DMA-style
 *    registers, not a single command/address/FIFO-drain sequence. These
 *    exact register pokes are hardware facts about this specific
 *    controller (confirmed against a Samsung-chip unit; also present,
 *    for the same silicon, in an already hardware-tested implementation
 *    this project consulted once the generic-protocol assumption was
 *    shown to be wrong), kept verbatim below rather than re-derived, the
 *    same way the FMC base address and GPIO pin values already are.
 *
 * The public API shape, error handling, bank-scan/probing strategy,
 * geometry decode, and everything above this low-level layer (FTL,
 * vendor ID decode) remain original work for this project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include <string.h>
#include "config.h"
#include "system.h"
#include "storage.h"
#include "panic.h"
#include "clocking-s5l8702.h"
#include "nand-target.h"
#include "nand_vendor.h"
#include "ftl-target.h"
#include <cpucache-arm.h>
#include <stdarg.h>
#include <stdio.h>

/* --- FMC (Flash Memory Controller) register map -----------------------
 * The FMC_BASE address (0x38a00000) and the register layout below are
 * already declared for this SoC/target in firmware/export/s5l87xx.h
 * (FMCTRL0, FMCTRL1, FMCMD, FMADDR0.., FMANUM, FMDNUM, FMCSTAT, FMFIFO,
 * FMCTRL0_CE(), FMCSTAT_* flags), added by prior, already-merged Rockbox
 * S5L8702 platform work rather than by this file. Our own independent
 * analysis of the decrypted OSOS 1.1.3 firmware (see file header)
 * separately confirmed 0x38a00000 as a real, live MMIO base referenced by
 * NAND bring-up code, which corroborates that existing register map from
 * a different vantage point; we reuse the existing names here rather than
 * redefine them under a different naming scheme. */
#include "s5l87xx.h"

/* A second 32-bit register, at FMC_BASE + 0x400, is also cleared alongside
 * FMCTRL0 during the OSOS firmware's NAND bring-up (observed directly in
 * our own disassembly, see file header); it is not part of the existing
 * s5l87xx.h register map and its purpose is unconfirmed, so it is kept
 * separate here rather than folded into that shared header. */
#define FMC_UNKNOWN_INIT_REG  (*(volatile uint32_t *)(FMC_BASE + 0x400))

/* Standard NAND command bytes (public ONFI/JEDEC command set, identical
 * across essentially every parallel SLC/MLC NAND part -- unlike the
 * register-level access sequence below, the command bytes themselves are
 * a public, vendor-neutral convention, not something specific to this
 * controller). */
#define NAND_CMD_READ0      0x00
#define NAND_CMD_READSTART  0x30  /* large-page 2nd read cycle command */
#define NAND_CMD_PAGEPROG1  0x80
#define NAND_CMD_PAGEPROG2  0x10
#define NAND_CMD_ERASE1     0x60
#define NAND_CMD_ERASE2     0xd0
#define NAND_CMD_READID     0x90
#define NAND_CMD_STATUS     0x70
#define NAND_CMD_RESET      0xff

#define NAND_STATUS_FAIL    0x01
#define NAND_STATUS_READY   0x40

/* FMCTRL0 is not solely a chip-enable-select register: bits 12-14 and
 * 16-18 are bus timing fields (write-strobe pulse width and a second,
 * unconfirmed timing parameter), and bit 0 is a required enable bit,
 * confirmed hardware requirement -- the chip does not respond at all if
 * only the chip-enable bits are set and the rest of the register is left
 * at its post-reset value of 0. */
#define NAND_TIMING_UNK1    4  /* 3-bit field at bit 16 */
#define NAND_TIMING_TWP     3  /* 3-bit field at bit 12, write-strobe width */
#define FMCTRL0_ENABLE      1  /* bit 0: required for the controller to run */

/* This controller reports NAND readiness through a READ STATUS command
 * and a syndrome register holding the chip's status byte, not through
 * FMCSTAT's RBBDONE bit (confirmed unreliable for this purpose on real
 * hardware). FMADDR2 = 1 selects that readback mode regardless of bank;
 * the specific FMCTRL1 mode bytes below are what makes the controller
 * actually latch the status byte into FMSYND0. */
#define FMC_STATUS_SELECT_ONE   1
#define FMC_MODE_READ_STATUS    0xca
#define FMC_MODE_IDLE           0x20
#define FMC_MODE_IDLE_AFTER_RDY 0xe0

/* Per-chunk (512-byte quarter of a 2KiB page) transfer mode bytes for
 * FMCTRL1: stage A loads 16 bytes of spare/ECC setup, stage B loads the
 * 512-byte chunk itself (after which FMCSTAT_UNK27 flags an ECC error
 * needing correction), and stage C has the controller DMA the chunk out
 * to a host buffer via the AUTOXFER mechanism (FMDATAW0/1 give the
 * destination). */
#define FMC_XFER_STAGE_A_TRIGGER  0x32
#define FMC_XFER_STAGE_B_TRIGGER  0x22
#define FMC_XFER_STAGE_C_TRIGGER  0x1a0
#define FMC_XFER_STAGE_C_TRIGGER_WRITE_A  0x34
#define FMC_XFER_STAGE_C_TRIGGER_WRITE_B  0x2e4
/* Priming trigger for chunk 0's data load, issued once before the write
 * loop starts (distinct from FMC_XFER_STAGE_C_TRIGGER_WRITE_B, which the
 * loop uses for chunks 1-3: the two differ by bit 2). */
#define FMC_XFER_WRITE_PRIME_TRIGGER      0x2e0
#define FMC_SPARE_DECODE_SETUP    0x5140  /* FMUNK78: page-read spare decode */
#define FMC_SPARE_ENCODE_SETUP    0x3210  /* FMUNK78: page-write spare encode */

/* The chunked AUTOXFER transfer mechanism moves exactly 512 bytes of main
 * data per chunk, four chunks per 2048-byte unit. */
#define NAND_CHUNK_SIZE     512

/* NAND_SPARE_META_BYTES (this controller's real per-page metadata limit,
 * FMSYND5/6/7) is declared in nand-target.h, not here, so ftl-nano3g.c
 * can size its on-flash header against the same constant rather than a
 * second, possibly-drifting hardcoded 12. Originally this file's own
 * private constant, before ftl-target.h's struct ftl_page_header was
 * redesigned to actually fit within it (see that header's comment for
 * the resolved 20-byte-vs-12-byte gap this note used to describe as
 * still open). */

/* Multi-unit (>2048-byte page, e.g. this Samsung MLC chip's 4096-byte
 * page) read sequence. This is a genuinely different, more heavily
 * pipelined register sequence from the single-unit chunked transfer
 * above -- not a loop over it -- confirmed against the reference
 * driver's nand_read_loaded_page()/nand_read_pages(), specialised here to
 * one page at a time (no multi-page pipelining, which this driver's
 * single-page API has no way to request anyway). Read-only: this driver
 * intentionally has no multi-unit *write* path (see nand_hw_write_page's
 * geometry check) -- an MLC chip in particular is never mounted
 * read-write regardless (nand_vendor_decode() only ever sets
 * ->recognized for SLC parts), so this path exists purely to let a
 * -DNAND_CHECK build (or direct probing) read back real page data/ECC
 * status for an unsupported chip without risking a write to it. */
#define NAND_UNIT_SIZE  2048u
/* Priming/first-chunk mode bytes, and the per-chunk pipeline's mode
 * bytes for chunks 1-3 of a unit (FMADDR6 = chunk_index << 4 there,
 * distinct from the single-unit path's half/upper bit encoding above --
 * this pipeline addresses chunks within a unit directly instead). */
#define FMC_MU_READ_RESUME        0x100e0
#define FMC_MU_STAGE_A_TRIGGER    0x32
#define FMC_MU_STAGE_B_TRIGGER    0xe2
#define FMC_MU_STREAM_TRIGGER     0x100
#define FMC_MU_UNIT_END_ADDR      0x201
#define FMC_MU_LAST_UNIT_TRIGGER  0x1e0
#define FMC_MU_META_DECODE_SETUP  0x3210

/* Conservative polling budgets. These are our own choices (not extracted
 * from Apple's firmware), sized generously against public datasheet
 * timing figures for 2007-08 era SLC NAND (tR ~25us typ/~100us max,
 * tPROG ~200-700us typ, tBERS ~2-3.5ms typ) with wide safety margin for
 * a bit-banged polling loop rather than an interrupt-driven one. */
#define FMC_POLL_SPINS      200000u
/* tRST (reset recovery time): the ready-wait alone cannot tell when the
 * chip has actually left its reset state, so this is waited out
 * unconditionally after issuing RESET, matching typical SLC datasheet
 * worst-case reset timing with margin. */
#define NAND_RESET_RECOVERY_US  5000u
/* Bound on how long a READ STATUS readback is allowed to take before we
 * give up and report a timeout; generous against the ~300us per-page
 * figure a full read/write takes on this controller. */
#define NAND_STATUS_TIMEOUT_US  200000u

static struct nand_geometry banks_geometry[NAND_MAX_BANKS];
static unsigned int detected_bank_count;

/* --- On-device debug log ring (see nand-target.h's declaration
 * comment for why this exists: diagnosing a real USB mass-storage
 * enumeration failure that has no other visibility). A plain ring, not
 * a queue with drop detection -- this is a bounded diagnostic aid, not
 * a general logging facility, so overwriting the oldest entry once full
 * is an acceptable, deliberate simplification. */
static char debug_log_lines[NAND_DEBUG_LOG_LINES][NAND_DEBUG_LOG_LINE_LEN];
static unsigned int debug_log_next;   /* index the next write goes to */
static unsigned int debug_log_count;  /* how many valid entries so far,
                                          capped at NAND_DEBUG_LOG_LINES */

void nand_debug_log(const char *fmt, ...)
{
    va_list ap;
    unsigned int slot = debug_log_next;

    va_start(ap, fmt);
    vsnprintf(debug_log_lines[slot], NAND_DEBUG_LOG_LINE_LEN, fmt, ap);
    va_end(ap);

    debug_log_next = (debug_log_next + 1) % NAND_DEBUG_LOG_LINES;
    if (debug_log_count < NAND_DEBUG_LOG_LINES)
        debug_log_count++;
}

unsigned int nand_debug_log_count(void)
{
    return debug_log_count;
}

void nand_debug_log_get(unsigned int i, char *out, unsigned int out_len)
{
    unsigned int oldest_slot, slot, len;

    if (i >= debug_log_count || out_len == 0)
        return;

    /* debug_log_next is where the NEXT write would land, i.e. one past
     * the newest entry (or, once the ring has wrapped, one past the
     * oldest -- which is the same slot). The oldest retained entry is
     * therefore at debug_log_next when the ring is full, or at index 0
     * when it hasn't wrapped yet. */
    oldest_slot = (debug_log_count < NAND_DEBUG_LOG_LINES) ? 0 : debug_log_next;
    slot = (oldest_slot + i) % NAND_DEBUG_LOG_LINES;

    /* Plain copy + manual NUL termination rather than pulling in
     * string-extra.h's strlcpy() just for this one call. */
    len = strlen(debug_log_lines[slot]);
    if (len >= out_len)
        len = out_len - 1;
    memcpy(out, debug_log_lines[slot], len);
    out[len] = '\0';
}

/* --- Low-level FMC command primitives -------------------------------- */

static bool fmc_wait_flag(uint32_t flag, uint32_t spins)
{
    while (spins--)
    {
        if (FMCSTAT & flag)
        {
            FMCSTAT = flag; /* write-1-to-clear */
            return true;
        }
    }
    return false;
}

/* As fmc_wait_flag(), but leaves the flag set; some flags (TRANSDONE
 * immediately after a READ ID transfer) must stay set for a following
 * step to observe. */
static bool fmc_wait_flag_noclear(uint32_t flag, uint32_t spins)
{
    while (spins--)
        if (FMCSTAT & flag)
            return true;
    return false;
}

/* Spin on an arbitrary register until a bit is set, then clear it there
 * (write-1-to-clear) rather than in FMCSTAT; used for the dedicated ECC
 * transfer-completion register FMTRANSSTAT. */
static bool fmc_wait_reg_flag(volatile uint32_t *reg, uint32_t flag,
                              uint32_t spins)
{
    while (spins--)
    {
        if (*reg & flag)
        {
            *reg = flag;
            return true;
        }
    }
    return false;
}

static void fmc_clear_status(void)
{
    FMCSTAT = 0xffffffffu;
}

static bool fmc_send_command(uint8_t cmd)
{
    FMCMD = cmd;
    return fmc_wait_flag(FMCSTAT_CMDDONE, FMC_POLL_SPINS);
}

/* Sends a single address byte (used for READ ID's one-cycle address). */
static bool fmc_send_address_byte(uint8_t addr)
{
    FMANUM = 0;
    FMADDR0 = addr;
    FMCTRL1 = FMCTRL1_DOTRANSADDR;
    return fmc_wait_flag(FMCSTAT_ADDRDONE, FMC_POLL_SPINS);
}

/* Sends a full page row address (5 address cycles: 2 column + 3 row,
 * packed the way this controller's sequencer expects rather than one
 * cycle per FMADDRn register). */
static bool fmc_send_address_page(uint32_t page)
{
    FMANUM = 4;
    FMADDR0 = page << 16;
    FMADDR1 = (page >> 16) & 0xFF;
    FMCTRL1 = FMCTRL1_DOTRANSADDR;
    return fmc_wait_flag(FMCSTAT_ADDRDONE, FMC_POLL_SPINS);
}

/* Sends a block row address for ERASE (row address only, no column). */
static bool fmc_send_address_block_row(uint32_t row)
{
    FMANUM = 2;
    FMADDR0 = row;
    FMCTRL1 = FMCTRL1_DOTRANSADDR;
    return fmc_wait_flag(FMCSTAT_ADDRDONE, FMC_POLL_SPINS);
}

/* Issues READ STATUS and polls FMSYND0 for the chip's real ready bit --
 * the confirmed-working readiness mechanism for this controller, in
 * place of relying on FMCSTAT_RBBDONE. Leaves the chip in status output
 * mode (a caller wanting to resume data output must re-issue a READ
 * command). Returns the status byte, or -1 on a timeout. */
static int fmc_read_status_ready(void)
{
    unsigned long deadline = USEC_TIMER + NAND_STATUS_TIMEOUT_US;
    uint32_t status;

    FMADDR2 = FMC_STATUS_SELECT_ONE;
    if (!fmc_send_command(NAND_CMD_STATUS))
        return -1;

    do
    {
        unsigned long spins;

        FMCTRL1 = FMC_MODE_READ_STATUS;
        status = 0;
        for (spins = 10000; spins; spins--)
        {
            status = FMSYND0;
            if (status & NAND_STATUS_READY)
                break;
        }
    } while (!(status & NAND_STATUS_READY)
             && TIME_BEFORE(USEC_TIMER, deadline));

    if (!(status & NAND_STATUS_READY))
        return -1;

    FMCSTAT = FMCSTAT_STATUSREADY;
    FMCTRL1 = FMC_MODE_IDLE;
    return status & 0xff;
}

/* Waits for the chip to finish an operation already in progress (reset,
 * program, erase), via fmc_read_status_ready(). Returns the status byte,
 * so a program/erase caller can check NAND_STATUS_FAIL, or -1 on a
 * timeout. */
static int fmc_wait_ready_status(void)
{
    int status = fmc_read_status_ready();

    FMADDR2 = 0;
    FMCTRL1 = FMC_MODE_IDLE_AFTER_RDY;
    return status;
}

/* As fmc_wait_ready_status(), for callers (reset, read) that only care
 * whether the chip went ready, not its status byte's content. */
static bool fmc_wait_ready(void)
{
    return fmc_wait_ready_status() >= 0;
}

static void fmc_select_bank(unsigned int bank)
{
    /* Chip-enable select (existing s5l87xx.h defines FMCTRL0_CE(bank) as
     * bit (bank+1)) packed together with the bus timing fields and
     * enable bit documented at their definitions above: FMCTRL0 must
     * carry all of these on every write, not just the chip-enable bits,
     * or the controller does not drive the bus at all. */
    FMCTRL0 = (NAND_TIMING_UNK1 << 16) | (NAND_TIMING_TWP << 12)
            | FMCTRL0_UNK1 | FMCTRL0_ENABLE | FMCTRL0_CE(bank);
}

/* Routes GPIO ports 8-10 to the FMC controller's NAND data/control lines.
 * Confirmed hardware requirement: without this, the FMC issues commands
 * on its internal bus but nothing reaches the physical NAND chip, since
 * the pins are left in their reset-time (non-NAND) function -- verified
 * both against the wInd3x project's independent BootROM-side NAND
 * bring-up (which calls the equivalent BootROM routine at 0x20001830
 * before touching any FMC register) and, on real hardware, by wInd3x's
 * "nand identify" command succeeding where this driver's own reset,
 * lacking this step, saw no response at all. PCON(9)'s masked bits leave
 * that port's other pins (not part of the NAND bus) untouched. */
static void fmc_config_gpio(void)
{
    PCON(10) = (PCON(10) & 0xFFFF0000) | 0x00002222;
    PCON(9)  = (PCON(9)  & ~0x000F000F) | 0x00020002;
    PCON(8)  = 0x22222222;
}

/* Per-chunk (512-byte quarter of a 2KiB page) transfer bookkeeping,
 * accumulated across all four chunks of a page the way the reference
 * sequence does, so the caller can classify the whole page's read result
 * once all four are in. */
struct fmc_chunk_ecc_state
{
    bool flagged;       /* FMCSTAT_UNK27 was set on at least one chunk */
    uint32_t unk810;     /* FMUNK810 after each correction, ORed together */
    uint32_t transstat;  /* FMTRANSSTAT after each correction, ORed together */
};

/* Moves one 512-byte chunk of the page the controller currently has
 * loaded out to dst, via the three-stage AUTOXFER sequence this
 * controller uses instead of a plain FIFO drain: stage A pulls in 16
 * bytes of per-chunk ECC/spare setup, stage B pulls in the 512-byte
 * chunk itself (after which FMCSTAT_UNK27 flags an ECC error that stage
 * B's data needs correcting), and stage C has the controller DMA the
 * (corrected) chunk out to dst behind the D-cache's back. */
static bool fmc_transfer_chunk_read(unsigned int chunk, void *dst,
                                    struct fmc_chunk_ecc_state *ecc)
{
    uint32_t half  = chunk & 1;
    uint32_t upper = (chunk >> 1) & 1;
    uint32_t sub   = half + 4;

    /* Stage A: 16 bytes of ECC/spare setup for this chunk. */
    FMADDR6 = upper ? 0x10 : 0;
    FMADDR2 = 1u << sub;
    FMDNUM = 0xf;
    FMCTRL1 = FMC_XFER_STAGE_A_TRIGGER;
    if (!fmc_wait_flag(FMCSTAT_TRANSDONE, FMC_POLL_SPINS))
        return false;

    /* Stage B: the 512-byte chunk itself. */
    FMDNUM = 0x1ff;
    FMADDR2 = 1u << half;
    FMADDR6 = 0;
    FMCTRL1 = FMC_XFER_STAGE_B_TRIGGER;
    if (!fmc_wait_flag(FMCSTAT_TRANSDONE, FMC_POLL_SPINS))
        return false;
    if (FMCSTAT & FMCSTAT_UNK27)
    {
        /* Kick the controller's ECC correction for this chunk and wait
         * for it; the register writes below are the confirmed sequence
         * for that (FMTRANSSTAT/FMTRANS1/FMTRANS0). */
        FMTRANSSTAT = 0x7f;
        FMTRANS1 = 0x01000180;
        FMTRANS0 = (1u << (sub + 8)) | (upper << 16) | (1u << (half + 8)) | 1;
        if (!fmc_wait_reg_flag(&FMTRANSSTAT, 1u << 2, FMC_POLL_SPINS))
            return false;
        ecc->flagged = true;
        ecc->unk810 |= FMUNK810;
        ecc->transstat |= FMTRANSSTAT;
    }

    /* Stage C: the controller writes dst behind the cache's back. */
    commit_discard_dcache_range(dst, NAND_CHUNK_SIZE);
    FMDATAW0 = (uint32_t)(intptr_t)dst;
    FMDATAW1 = 7;
    FMCTRL0 = (FMCTRL0 & ~FMCTRL0_ENABLEDMA) | FMCTRL0_AUTOXFER | 1;
    FMADDR2 = 1u << (half + 8);
    FMCTRL1 = FMC_XFER_STAGE_C_TRIGGER;
    if (!fmc_wait_flag(FMCSTAT_UNK20, FMC_POLL_SPINS))
        return false;
    discard_dcache_range(dst, NAND_CHUNK_SIZE);
    return true;
}

/* Has the controller decode the 12 bytes of page metadata it accumulated
 * across the four chunk reads above (FMSYND5..7), the way the reference
 * page-read sequence does after its chunk loop. This is the *only*
 * metadata this controller's chunked-transfer mechanism surfaces to
 * software for a page -- see the NAND_SPARE_META_BYTES comment at its
 * definition for why that matters to callers. */
static bool fmc_decode_spare_read(uint32_t meta[3])
{
    unsigned long spins = FMC_POLL_SPINS;

    FMUNK78 = FMC_SPARE_DECODE_SETUP;
    FMUNK7C = 2;
    while (FMUNK7C & 2)
        if (!--spins)
            return false;
    meta[0] = FMSYND5;
    meta[1] = FMSYND6;
    meta[2] = FMSYND7;
    return true;
}

/* Loads the 12 bytes of metadata for the chunk about to be programmed
 * and starts the controller's ECC encode on it, the write-side
 * counterpart of fmc_decode_spare_read(). */
static bool fmc_load_spare_write(const uint32_t meta[3])
{
    unsigned long spins = FMC_POLL_SPINS;

    FMSYND5 = meta[0];
    FMSYND6 = meta[1];
    FMSYND7 = meta[2];
    FMUNK78 = FMC_SPARE_ENCODE_SETUP;
    FMUNK7C = 1;
    while (FMUNK7C & 1)
        if (!--spins)
            return false;
    return fmc_wait_flag(FMCSTAT_UNK20, FMC_POLL_SPINS);
}

/* Starts the ECC engine's encode of one chunk about to be written;
 * "cmd" packs the chunk index into the same bit layout FMTRANS0 always
 * uses for this operation (confirmed sequence, see file header). */
static void fmc_encode_chunk(uint32_t cmd)
{
    FMTRANSSTAT = 0x1ff;
    FMTRANS1 = 0x01000180;
    FMTRANS0 = cmd;
}

/* Adds one chunk's ECC correction result into the accumulator bitmap the
 * confirmed multi-unit read sequence uses: bit 30 marks an uncorrectable
 * chunk, and low bit (n-1) marks an n-bit correction, mirroring the
 * reference sequence's nand_read_ecc_result() exactly. */
static void fmc_mu_accumulate_ecc(uint32_t *result)
{
    uint32_t ecc = FMUNK810;

    if (ecc & 1)
        *result |= 1u << 30;
    else
    {
        uint32_t count = (ecc >> 16) & 0xf;
        if (count)
            *result |= 1u << (count - 1);
    }
}

/* Reads one already-selected-and-addressed page of `units` 2048-byte
 * units (units = page_size / NAND_UNIT_SIZE) into dst, via the confirmed
 * multi-unit pipelined sequence. This is the single-page (no next-page
 * pipelining) specialisation of the reference driver's
 * nand_read_loaded_page(): every branch that only exists to prime the
 * *following* page's transfer while this one finishes (the "next"
 * parameter there) is dropped, since this driver's read_page() API has
 * no way to request that pipelining anyway. Caller must have already
 * issued READ/address/READ2/ready-wait/READ(resume); this only does the
 * chunked payload transfer, ECC correction accumulation, and metadata
 * decode. */
static bool fmc_read_multiunit_page(void *dst, unsigned int units,
                                    uint32_t meta[3], uint32_t *result)
{
    /* bufsel[]/correct[] are the confirmed per-chunk mode/correction
     * words for chunks 1-3 of a unit (chunk 0 is primed before this
     * loop starts); correct[3] (the unit's last chunk) is reused both
     * between units and once more after the final unit, exactly as the
     * reference sequence does. */
    static const uint32_t bufsel[3]  = { 0x102, 0x201, 0x102 };
    static const uint32_t correct[4] = { 0x1101, 0x11201, 0x21101, 0x31201 };
    unsigned int unit, chunk;
    unsigned long spins;
    uint32_t stat;

    /* Resume data output and prime chunk 0's 16-byte ECC/spare setup and
     * 512-byte data transfer before the per-unit loop starts. */
    FMCTRL1 = FMC_MU_READ_RESUME;
    if (!fmc_send_command(NAND_CMD_READ0))
        return false;

    FMDNUM = 0xf;
    FMADDR2 = 0x10;
    FMADDR6 = 0;
    FMCTRL1 = FMC_MU_STAGE_A_TRIGGER;
    if (!fmc_wait_flag(FMCSTAT_TRANSDONE, FMC_POLL_SPINS))
        return false;
    FMDNUM = 0x1ff;
    FMADDR2 = 1;
    FMCTRL1 = FMC_MU_STAGE_B_TRIGGER;
    if (!fmc_wait_flag(FMCSTAT_TRANSDONE, FMC_POLL_SPINS))
        return false;

    FMCTRL0 |= FMCTRL0_AUTOXFER;
    FMDATAW0 = (uint32_t)(intptr_t)dst;

    for (unit = 0; unit < units; unit++)
    {
        if (unit)
        {
            /* The previous unit's last chunk (16-byte setup, correction),
             * while priming this unit's destination address. */
            FMDNUM = 0xf;
            FMADDR2 = 0x10;
            FMADDR6 = 0;
            FMCTRL1 = FMC_MU_STAGE_A_TRIGGER;
            if (!fmc_wait_flag(FMCSTAT_TRANSDONE, FMC_POLL_SPINS))
                return false;
            FMADDR2 = FMC_MU_UNIT_END_ADDR;
            FMDNUM = 0x1ff;
            FMCTRL1 = FMC_MU_STAGE_B_TRIGGER;
            FMTRANSSTAT = 0x1ff;
            FMTRANS1 = 0x180;
            FMTRANS0 = correct[3];
            if (!fmc_wait_reg_flag(&FMTRANSSTAT, 1, FMC_POLL_SPINS))
                return false;
            fmc_mu_accumulate_ecc(result);
            FMCTRL1 = FMC_MU_STREAM_TRIGGER;
            if (!fmc_wait_flag(FMCSTAT_UNK20, FMC_POLL_SPINS))
                return false;
            FMDATAW0 = (uint32_t)(intptr_t)dst + unit * NAND_UNIT_SIZE;
            if (!fmc_wait_flag(FMCSTAT_TRANSDONE, FMC_POLL_SPINS))
                return false;
        }

        for (chunk = 0; chunk < 3; chunk++)
        {
            FMDNUM = 0xf;
            FMADDR2 = 0x10;
            FMADDR6 = (chunk + 1) << 4;
            FMCTRL1 = FMC_MU_STAGE_A_TRIGGER;
            if (!fmc_wait_flag(FMCSTAT_TRANSDONE, FMC_POLL_SPINS))
                return false;
            FMADDR2 = bufsel[chunk];
            FMDNUM = 0x1ff;
            FMCTRL1 = FMC_MU_STAGE_B_TRIGGER;

            FMTRANSSTAT = 0x1ff;
            FMTRANS1 = 0x180;
            FMTRANS0 = correct[chunk];
            if (!fmc_wait_reg_flag(&FMTRANSSTAT, 1, FMC_POLL_SPINS))
                return false;
            fmc_mu_accumulate_ecc(result);
            FMCTRL1 = FMC_MU_STREAM_TRIGGER;
            if (!fmc_wait_flag(FMCSTAT_UNK20, FMC_POLL_SPINS)
                || !fmc_wait_flag(FMCSTAT_TRANSDONE, FMC_POLL_SPINS))
                return false;
        }
    }

    /* The final unit's last chunk: same correction kick as between
     * units, but the "erased page" check (bit 29) instead uses whether
     * FMCSTAT_UNK27 is still set here, per the reference sequence. */
    stat = FMCSTAT & FMCSTAT_UNK27;
    FMCSTAT = FMCSTAT_UNK27;
    if (!stat)
        *result |= 1u << 29;
    FMTRANSSTAT = 0x1ff;
    FMTRANS1 = 0x180;
    FMTRANS0 = correct[3];

    if (!fmc_wait_reg_flag(&FMTRANSSTAT, 1, FMC_POLL_SPINS))
        return false;
    fmc_mu_accumulate_ecc(result);

    /* No pipelined next page: stream the last chunk out now. */
    FMDATAW1 = 7;
    FMADDR2 = 0x200;
    FMDNUM = 0x1ff;
    FMCTRL1 = FMC_MU_LAST_UNIT_TRIGGER;
    if (!fmc_wait_flag(FMCSTAT_UNK20, FMC_POLL_SPINS))
        return false;

    FMUNK78 = FMC_MU_META_DECODE_SETUP;
    FMUNK7C = 2;
    spins = FMC_POLL_SPINS;
    while (FMUNK7C & 2)
        if (!--spins)
            return false;
    if (meta)
    {
        meta[0] = FMSYND5;
        meta[1] = FMSYND6;
        meta[2] = FMSYND7;
    }
    return true;
}

/* --- Public driver API ------------------------------------------------ */

int nand_hw_init(void)
{
    clockgate_enable(CLOCKGATE_NAND, true);
    /* The FMC's ECC/ancillary block is on a separate clock gate; without
     * it the controller does not reliably respond either. Same hardware
     * requirement as the two clock gate enables in nand_gpio_config()'s
     * caller in the original, hardware-tested driver this project's own
     * work replaced -- confirmed still required here. */
    clockgate_enable(CLOCKGATE_NANDECC, true);

    /* Confirmed hardware behaviour: bring-up clears both FMCTRL0 and the
     * second register at FMC_BASE+0x400 before any command is issued. */
    FMCTRL0 = 0;
    FMC_UNKNOWN_INIT_REG = 0;

    /* Route the FMC's pins to their NAND function; see fmc_config_gpio()
     * for why this is required before any bank will respond. */
    fmc_config_gpio();

    memset(banks_geometry, 0, sizeof(banks_geometry));
    detected_bank_count = 0;

    return 0;
}

int nand_hw_reset(unsigned int bank)
{
    if (bank >= NAND_MAX_BANKS)
        return NAND_HWERR_NO_CHIP;

    fmc_select_bank(bank);

    if (!fmc_send_command(NAND_CMD_RESET))
        return NAND_HWERR_TIMEOUT;

    /* tRST: the ready-wait alone can't tell when the chip has actually
     * left its reset state (see file header), so it's waited out
     * unconditionally before polling readiness. */
    udelay(NAND_RESET_RECOVERY_US);

    if (!fmc_wait_ready())
        return NAND_HWERR_TIMEOUT;

    return 0;
}

/* Follows the confirmed READ ID sequence (wInd3x's BootROM-side
 * NANDIdentify(), corroborated by the already hardware-tested reference
 * driver's nand_identify()): a one-byte address cycle, then a controller
 * transfer of 7 extra bytes via FMADDR2/FMCTRL1/FMDNUM rather than a
 * plain FIFO drain, reading the first two 32-bit FIFO words out as the
 * up-to-8 ID bytes. */
int nand_hw_read_id(unsigned int bank, uint8_t *id_out, unsigned int id_len)
{
    uint32_t word0, word1;

    if (bank >= NAND_MAX_BANKS)
        return NAND_HWERR_NO_CHIP;
    if (id_len == 0 || id_len > 8)
        return NAND_HWERR_NO_CHIP;

    fmc_select_bank(bank);

    if (!fmc_send_command(NAND_CMD_READID))
        return NAND_HWERR_TIMEOUT;
    if (!fmc_send_address_byte(0x00))
        return NAND_HWERR_TIMEOUT;

    FMDNUM = 7;
    FMADDR2 = 1;
    FMCSTAT = FMCSTAT_TRANSDONE;
    FMCTRL1 = FMCTRL1_DOREADDATA | FMCTRL1_CLEARWFIFO | FMCTRL1_CLEARRFIFO
            | (1u << 8);
    if (!fmc_wait_flag_noclear(FMCSTAT_TRANSDONE, FMC_POLL_SPINS))
        return NAND_HWERR_TIMEOUT;

    FMADDR2 = 0x100;
    FMCTRL1 = FMCTRL1_CLEARWFIFO | (1u << 8) | (1u << 9);
    word0 = FMFIFO;
    word1 = FMFIFO;
    fmc_clear_status();

    memcpy(id_out, &word0, id_len >= 4 ? 4 : id_len);
    if (id_len > 4)
        memcpy(id_out + 4, &word1, id_len - 4);
    return 0;
}

/* Reads one 2048-byte page (this driver's smallest supported unit; a
 * chip with a larger page_size is refused below rather than driven
 * incorrectly -- see NAND_HWERR_UNSUPPORTED_GEOMETRY) via the confirmed
 * chunked AUTOXFER sequence: 00h, a packed 5-cycle page address, 30h,
 * wait tR (READ STATUS ready-wait, not RBBDONE), then four
 * fmc_transfer_chunk_read() calls, one per 512-byte chunk of the page. */
/* Multi-unit (page_size > 2048, e.g. this Samsung MLC chip's 4096-byte
 * page) read. Read-only counterpart to the single-unit path above -- see
 * fmc_read_multiunit_page()'s comment for why there is no corresponding
 * write path. bank/page-address/geometry/recognized checks are the
 * caller's (nand_hw_read_page()) responsibility; this only does the
 * READ/address/READ2/ready-wait bring-up and the pipelined transfer
 * itself. */
static int fmc_hw_read_page_multiunit(unsigned int bank, uint32_t page,
                                      const struct nand_geometry *geo,
                                      void *data_out, void *spare_out)
{
    /* static, not a stack local: this driver's other state
     * (banks_geometry[]/detected_bank_count) is already static/
     * non-reentrant by design, and this scratch buffer alone (up to
     * NAND_MAX_PAGE_SIZE = 4096 bytes) is large enough that, stacked on
     * top of the similar-sized locals in this call's actual callers
     * (nand_hw_read_page(), and probe_bank_capacity_blocks() further up
     * for a capacity-probe read), it overflowed the bootloader's 8KB
     * stack (firmware/target/arm/s5l8702's app.lds/boot.lds:
     * _stackbegin.._stackend spans 0x2000) -- confirmed on real
     * hardware: two consecutive
     * multi-unit read attempts faulted the board (one reset back to
     * DFU, one left it unresponsive to a DFU re-probe until retried)
     * before this was found and fixed. */
    static uint32_t meta[3];
    static uint8_t scratch[NAND_MAX_PAGE_SIZE] NAND_DMA_BUF_ATTR;
    uint32_t result = 0;
    void *dst = data_out ? data_out : scratch;

    /* Required preamble for this pipelined sequence specifically
     * (confirmed present in the reference driver's nand_read_pages(),
     * which every multi-unit read goes through there too): resets
     * FMCTRL1 and clears extended FMCSTAT bits before selecting the
     * bank, the same preamble nand_hw_write_page()/nand_hw_erase_block()
     * already carry. Missing this was a real bug in an earlier version
     * of this function -- confirmed on hardware to leave stale
     * AUTOXFER/DMA state from a prior operation in a way that faulted
     * the board (device dropped back into DFU) rather than just failing
     * the read cleanly. */
    FMCTRL1 = 0x0ff3f8e0;
    FMCSTAT = 0x0ff00ffe;
    fmc_select_bank(bank);

    if (!fmc_send_command(NAND_CMD_READ0))
        return NAND_HWERR_TIMEOUT;
    if (!fmc_send_address_page(page))
        return NAND_HWERR_TIMEOUT;
    if (!fmc_send_command(NAND_CMD_READSTART))
        return NAND_HWERR_TIMEOUT;
    /* The confirmed multi-unit sequence's own ready-wait is a plain READ
     * STATUS (fmc_read_status_ready()), not fmc_wait_ready() (used by the
     * single-unit path and reset): fmc_read_multiunit_page() immediately
     * resumes data output and re-primes FMADDR2/FMCTRL1 itself, so it
     * doesn't need -- and the reference sequence doesn't do -- the extra
     * FMADDR2=0/idle cleanup fmc_wait_ready() adds on top. */
    if (fmc_read_status_ready() < 0)
        return NAND_HWERR_TIMEOUT;

    /* The controller writes dst behind the D-cache's back for the whole
     * transfer, not per-chunk, in this pipelined sequence. */
    commit_discard_dcache_range(dst, geo->page_size);
    if (!fmc_read_multiunit_page(dst, geo->page_size / NAND_UNIT_SIZE,
                                 meta, &result))
    {
        discard_dcache_range(dst, geo->page_size);
        fmc_clear_status();
        return NAND_HWERR_TIMEOUT;
    }
    discard_dcache_range(dst, geo->page_size);
    fmc_clear_status();

    if (spare_out)
    {
        memset(spare_out, 0, geo->spare_size);
        memcpy(spare_out, meta,
               geo->spare_size < NAND_SPARE_META_BYTES
                   ? geo->spare_size : NAND_SPARE_META_BYTES);
    }

    /* Same classification the confirmed reference sequence's
     * nand_read_pages() derives from this same result bitmap. */
    if (result & (1u << 30))
        return NAND_ECC_FAILED;
    if (result & 0xff)
        return NAND_ECC_CORRECTED;
    return NAND_ECC_OK;
}

int nand_hw_read_page(unsigned int bank, uint32_t page,
                      void *data_out, void *spare_out)
{
    const struct nand_geometry *geo = &banks_geometry[bank];
    struct fmc_chunk_ecc_state ecc = { false, 0, 0 };
    uint32_t meta[3];
    /* static: see fmc_hw_read_page_multiunit()'s comment on why a
     * 2048-byte stack local here, stacked on top of the multi-unit
     * path's own scratch buffer once this function calls into it, blew
     * the bootloader's stack on real hardware. */
    static uint8_t scratch[NAND_CHUNK_SIZE * 4] NAND_DMA_BUF_ATTR;
    uint8_t *chunk_dst;
    unsigned int chunk;

    /* Reading is safe from any chip whose geometry was at least
     * plausibly decoded (page_size != 0), whether or not it's
     * "recognized" -- recognized only gates *writes* (see
     * nand_hw_write_page()/nand_hw_erase_block(), which correctly still
     * require it, and ftl-nano3g.c's readonly_mount). Requiring
     * ->recognized here too was a real bug: it silently turned every
     * read of an unrecognised chip (e.g. this driver's own capacity
     * probe, run diagnostically against an MLC chip by
     * nand_check_probe_bank_capacity()) into an immediate
     * NAND_HWERR_NO_CHIP without ever touching the hardware, which was
     * mistaken for a real capacity-probe stopping point/hardware fault
     * until traced with nand_check_last_probe_trace(). */
    if (bank >= NAND_MAX_BANKS || geo->page_size == 0)
        return NAND_HWERR_NO_CHIP;
    if (geo->page_size > NAND_CHUNK_SIZE * 4)
        return fmc_hw_read_page_multiunit(bank, page, geo, data_out,
                                          spare_out);
    if (geo->page_size != NAND_CHUNK_SIZE * 4)
        return NAND_HWERR_UNSUPPORTED_GEOMETRY;

    fmc_select_bank(bank);

    if (!fmc_send_command(NAND_CMD_READ0))
        return NAND_HWERR_TIMEOUT;
    if (!fmc_send_address_page(page))
        return NAND_HWERR_TIMEOUT;
    if (!fmc_send_command(NAND_CMD_READSTART))
        return NAND_HWERR_TIMEOUT;
    if (!fmc_wait_ready())
        return NAND_HWERR_TIMEOUT;
    if (!fmc_send_command(NAND_CMD_READ0)) /* back to data output */
        return NAND_HWERR_TIMEOUT;

    for (chunk = 0; chunk < 4; chunk++)
    {
        chunk_dst = data_out ? (uint8_t *)data_out + chunk * NAND_CHUNK_SIZE
                             : scratch;
        FMCSTAT = FMCSTAT_UNK27;
        if (!fmc_transfer_chunk_read(chunk, chunk_dst, &ecc))
            return NAND_HWERR_TIMEOUT;
    }

    if (!fmc_decode_spare_read(meta))
        return NAND_HWERR_TIMEOUT;
    if (spare_out)
    {
        /* Only NAND_SPARE_META_BYTES of real metadata exist on this path
         * (see that macro's definition); zero-pad the rest of the
         * caller's buffer so it isn't left with stale/uninitialised
         * bytes beyond that. */
        memset(spare_out, 0, geo->spare_size);
        memcpy(spare_out, meta,
               geo->spare_size < NAND_SPARE_META_BYTES
                   ? geo->spare_size : NAND_SPARE_META_BYTES);
    }

    fmc_clear_status();

    /* Same classification the confirmed-working reference sequence's page
     * read reports. */
    if (!ecc.flagged)
        return NAND_ECC_OK;
    if (ecc.unk810 & 1)
        return NAND_ECC_FAILED;
    return NAND_ECC_CORRECTED;
}

/* Programs one page of any size the AUTOXFER mechanism supports (single-
 * or multi-unit -- unlike the read side, the reference sequence's write
 * path (nand_write_pages()) uses one and the same register sequence for
 * both, generalising by the number of 512-byte chunks the page holds
 * rather than needing a second, differently-structured pipeline). This
 * is the single-page (n=1, no two-plane) case of that sequence: it
 * primes chunk 0's data and metadata before issuing PROGRAM, then for
 * every chunk but the last loads the next chunk's 16-byte ECC/spare
 * setup (jumping FMDATAW0 to the next 2KiB unit's buffer every 4th
 * chunk, for a multi-unit page) and streams the previous one's 512 bytes
 * out while encoding it, and for the page's last chunk streams it out
 * with AUTOXFER already turned back off, finishing with PROGRAM2 and a
 * status readback. The specific FMCTRL1 mode bytes and encode/bufsel
 * tables are the confirmed sequence (see file header); this function
 * keeps them but drops every multi-page/two-plane pipelining path that
 * sequence also supports, none of which a single-page call reaches. */
int nand_hw_write_page(unsigned int bank, uint32_t page,
                       const void *data_in, const void *spare_in)
{
    static const uint32_t encode[4] = { 0x01102, 0x11202, 0x21102, 0x31202 };
    static const uint32_t bufsel[4] = { 0x102, 0x201, 0x102, 0x201 };
    const struct nand_geometry *geo = &banks_geometry[bank];
    uint32_t meta[3] = { 0, 0, 0 };
    unsigned int chunk, chunks;
    int status;

    if (bank >= NAND_MAX_BANKS || !geo->recognized)
        return NAND_HWERR_NO_CHIP;
    if (geo->page_size == 0 || geo->page_size % NAND_CHUNK_SIZE != 0
        || geo->page_size > NAND_MAX_PAGE_SIZE)
        return NAND_HWERR_UNSUPPORTED_GEOMETRY;
    chunks = geo->page_size / NAND_CHUNK_SIZE;

    if (spare_in)
        memcpy(meta, spare_in,
               geo->spare_size < NAND_SPARE_META_BYTES
                   ? geo->spare_size : NAND_SPARE_META_BYTES);

    /* Reset FMCTRL1 and clear the extended status bits before starting: a
     * required preamble before a write or erase specifically (confirmed
     * present before every program/erase sequence in the reference
     * driver, and absent from its plain single-page read), presumably
     * clearing AUTOXFER/DMA-related state a previous operation left set. */
    FMCTRL1 = 0x0ff3f8e0;
    FMCSTAT = 0x0ff00ffe;
    fmc_select_bank(bank);

    /* The controller reads the page data behind the D-cache's back. */
    commit_dcache_range(data_in, geo->page_size);

    /* Prime chunk 0's data and metadata before the PROGRAM command. */
    FMDATAW0 = (uint32_t)(intptr_t)data_in;
    FMCTRL0 |= FMCTRL0_AUTOXFER;
    FMDNUM = 0x1ff;
    FMADDR2 = 1;
    FMCTRL1 = FMC_XFER_WRITE_PRIME_TRIGGER;
    if (!fmc_load_spare_write(meta))
        return NAND_HWERR_TIMEOUT;
    fmc_encode_chunk(encode[0]);

    if (!fmc_send_command(NAND_CMD_PAGEPROG1))
        return NAND_HWERR_TIMEOUT;
    if (!fmc_send_address_page(page))
        return NAND_HWERR_TIMEOUT;
    if (!fmc_wait_reg_flag(&FMTRANSSTAT, 1u << 0, FMC_POLL_SPINS))
        return NAND_HWERR_TIMEOUT;

    for (chunk = 0; chunk < chunks; chunk++)
    {
        bool pageend = (chunk + 1 == chunks);   /* single page: == last */

        if (pageend)
            FMCTRL0 &= ~FMCTRL0_AUTOXFER;

        /* 16 bytes of ECC/spare setup for the next chunk. FMADDR7 cycles
         * 0/1/2/3 within each 2KiB unit (chunk % 4), not a running index
         * -- confirmed from the reference sequence, which uses the same
         * 4-entry addressing regardless of how many units the page has. */
        FMDNUM = 0xf;
        FMADDR2 = 0x1000;
        FMADDR7 = (chunk % 4) << 4;
        FMCTRL1 = FMC_XFER_STAGE_C_TRIGGER_WRITE_A;
        /* Every 4th chunk (the last of a 2KiB unit) that isn't the
         * page's last chunk: jump the destination to the next unit's
         * buffer, so a multi-unit page's later units get written from
         * the right offset. Single-unit pages (chunks == 4) never take
         * this branch, matching this function's pre-multi-unit
         * behaviour exactly. */
        if (!pageend && chunk % 4 == 3)
            FMDATAW0 = (uint32_t)(intptr_t)data_in
                     + (chunk + 1) * NAND_CHUNK_SIZE;
        if (!fmc_wait_flag(FMCSTAT_TRANSDONE, FMC_POLL_SPINS))
            return NAND_HWERR_TIMEOUT;

        if (pageend)
        {
            /* The page's last chunk: stream it out, no further chunk to
             * load behind it. */
            FMDATAW1 = 7;
            FMADDR2 = 0x200;
            FMDNUM = 0x1ff;
            FMCTRL1 = 0xe4;
            if (!fmc_wait_flag(FMCSTAT_TRANSDONE, FMC_POLL_SPINS))
                return NAND_HWERR_TIMEOUT;
            break;
        }

        /* Stream this chunk's 512 bytes out while the ECC engine encodes
         * the next one. */
        FMADDR2 = bufsel[chunk % 4];
        FMDNUM = 0x1ff;
        FMCTRL1 = FMC_XFER_STAGE_C_TRIGGER_WRITE_B;
        if (!fmc_wait_flag(FMCSTAT_UNK20, FMC_POLL_SPINS))
            return NAND_HWERR_TIMEOUT;
        fmc_encode_chunk(encode[(chunk + 1) % 4]);
        if (!fmc_wait_reg_flag(&FMTRANSSTAT, 1u << 0, FMC_POLL_SPINS))
            return NAND_HWERR_TIMEOUT;
        if (!fmc_wait_flag(FMCSTAT_TRANSDONE, FMC_POLL_SPINS))
            return NAND_HWERR_TIMEOUT;
    }

    if (!fmc_send_command(NAND_CMD_PAGEPROG2))
        return NAND_HWERR_TIMEOUT;

    /* Confirmed finalisation for a single-bank run: a plain READ STATUS
     * (not the reset/read ready-wait's FMADDR2=0/FMCTRL1=0xe0 cleanup,
     * which the reference reserves for nand_wait_ready(), not this). */
    status = fmc_read_status_ready();
    fmc_clear_status();
    if (status < 0)
        return NAND_HWERR_TIMEOUT;
    if (status & NAND_STATUS_FAIL)
        return NAND_HWERR_PROGRAM_FAILED;

    return 0;
}

int nand_hw_erase_block(unsigned int bank, uint32_t block)
{
    const struct nand_geometry *geo = &banks_geometry[bank];
    uint32_t row;
    int status;

    if (bank >= NAND_MAX_BANKS || !geo->recognized)
        return NAND_HWERR_NO_CHIP;

    /* Same required preamble as nand_hw_write_page(); see its comment. */
    FMCTRL1 = 0x0ff3f8e0;
    FMCSTAT = 0x0ff00ffe;
    fmc_select_bank(bank);

    row = block * geo->pages_per_block;

    if (!fmc_send_command(NAND_CMD_ERASE1))
        return NAND_HWERR_TIMEOUT;
    if (!fmc_send_address_block_row(row))
        return NAND_HWERR_TIMEOUT;
    if (!fmc_send_command(NAND_CMD_ERASE2))
        return NAND_HWERR_TIMEOUT;

    /* Confirmed finalisation: a plain READ STATUS (fmc_read_status_ready()),
     * not the reset/read ready-wait's extra FMADDR2=0/FMCTRL1=idle cleanup
     * (that cleanup is specific to fmc_wait_ready(), used by reset/read). */
    status = fmc_read_status_ready();
    fmc_clear_status();
    if (status < 0)
        return NAND_HWERR_TIMEOUT;
    if (status & NAND_STATUS_FAIL)
        return NAND_HWERR_ERASE_FAILED;

    return 0;
}

/* --- Bank scanning / capacity probing --------------------------------
 * blocks_per_bank cannot be derived from the base 4-byte READ ID alone
 * (see nand_vendor.c). We probe for it directly: read the first page of
 * successively higher block numbers (in a binary-search-friendly doubling
 * pattern) and stop at the first block whose read fails outright (rather
 * than merely returning an uncorrectable/erased result, which just means
 * "unwritten", not "doesn't exist"). This is an original, hardware-driven
 * approach rather than a hardcoded per-part density table. */
#ifdef NAND_CHECK
/* Diagnostic-only trace of the capacity probe's very first stopping
 * read, so a -DNAND_CHECK build can report *why* it stopped (a genuine
 * controller timeout/no-answer, vs. this driver having got that
 * confused with something else) rather than just the resulting block
 * count. Not used by, or required for, normal (non-NAND_CHECK) probing.
 * struct nand_probe_trace itself is declared in nand-target.h. */
static struct nand_probe_trace last_probe_trace;

const struct nand_probe_trace *nand_check_last_probe_trace(void)
{
    return &last_probe_trace;
}
#endif

static uint32_t probe_bank_capacity_blocks(unsigned int bank,
                                           const struct nand_geometry *geo)
{
    /* static: see fmc_hw_read_page_multiunit()'s comment. This function
     * calls nand_hw_read_page(), which for a multi-unit chip calls
     * fmc_hw_read_page_multiunit() in turn -- three call frames, each
     * with a same-sized scratch buffer as a stack local, is what
     * overflowed the bootloader's 8KB stack on real hardware. */
    static uint8_t scratch[NAND_MAX_PAGE_SIZE] NAND_DMA_BUF_ATTR;
    uint32_t lo = 1, hi = 1;

#ifdef NAND_CHECK
    last_probe_trace.stopped = false;
#endif

    /* Grow hi until a read at its first page fails outright. Capped so a
     * wedged/misbehaving chip can't spin forever.
     *
     * This method has a real, confirmed-on-hardware limitation: raw NAND
     * chips only decode as many row-address bits as their actual page
     * count needs, and typically just ignore extra high-order bits in
     * an out-of-range address rather than erroring -- so a read past the
     * chip's real end can alias back onto an in-range page and return a
     * perfectly normal success, not NAND_HWERR_TIMEOUT/NO_CHIP. Raising
     * this cap cannot fix that: it was raised from 16384 to 1048576
     * against this Samsung MLC chip on real hardware, and every single
     * read still "succeeded" (probestop 0 both times), because the
     * requested addresses were aliasing rather than genuinely reaching
     * new storage. The cap here exists only to bound worst-case runtime
     * against a chip too large or too aliasing-prone for this method to
     * ever measure -- it should not be read as "this method can find
     * any real chip's boundary given enough tries". Whether this
     * particular probe result should be trusted for a given chip is the
     * caller's job (see nand-check-nano3g.c's "probestop 0" handling and
     * NANO3G_ORIGINAL_NAND_FTL.md's notes on this Samsung part's real,
     * ID-table-sourced 4096-block/bank capacity vs. the unreliable,
     * much larger number this probe reports for it). */
    while (hi < 16384)
    {
        uint32_t probe_page = hi * geo->pages_per_block;
        int rc = nand_hw_read_page(bank, probe_page, scratch, NULL);
        if (rc == NAND_HWERR_TIMEOUT || rc == NAND_HWERR_NO_CHIP)
        {
#ifdef NAND_CHECK
            last_probe_trace.stopped = true;
            last_probe_trace.stop_block = hi;
            last_probe_trace.stop_page = probe_page;
            last_probe_trace.stop_rc = rc;
#endif
            break;
        }
        lo = hi;
        hi *= 2;
    }

    /* Binary search the boundary between lo (known good) and hi (known
     * bad or capped). */
    while (hi - lo > 1)
    {
        uint32_t mid = lo + (hi - lo) / 2;
        int rc = nand_hw_read_page(bank, mid * geo->pages_per_block,
                                   scratch, NULL);
        if (rc == NAND_HWERR_TIMEOUT || rc == NAND_HWERR_NO_CHIP)
            hi = mid;
        else
            lo = mid;
    }

    return lo + 1; /* lo was the last confirmed-good block index */
}

#ifdef NAND_CHECK
unsigned int nand_check_probe_bank_capacity(unsigned int bank)
{
    struct nand_geometry *geo;

    if (bank >= NAND_MAX_BANKS)
        return 0;
    geo = &banks_geometry[bank];
    if (geo->page_size == 0)
        return 0; /* never got a plausible geometry decode at all */

    geo->blocks_per_bank = probe_bank_capacity_blocks(bank, geo);
    return geo->blocks_per_bank;
}

/* Decodes and records geometry for a bank nand_scan_banks() never
 * reached (it stops at the first unrecognised bank -- see its own
 * comment), so a diagnostic probe of further chip-enables (e.g.
 * nand-check-nano3g.c's bank 1-3 ID scan for a multi-die package) has
 * somewhere to put what it found besides just the raw ID bytes. Purely
 * diagnostic: does not touch detected_bank_count, so
 * nand_get_bank_count()/nand_get_bank_geometry() (what ftl_init() and
 * every other real caller use) are completely unaffected -- this bank
 * still isn't "usable" as far as the rest of the driver is concerned,
 * it just now has real geometry available to nand_check_write_test_sweep()
 * and any other diagnostic that looks at banks_geometry[] directly. */
void nand_check_decode_bank_geometry(unsigned int bank, const uint8_t *id,
                                     unsigned int id_len)
{
    if (bank >= NAND_MAX_BANKS)
        return;
    nand_vendor_decode(id, id_len, &banks_geometry[bank]);
}
#endif

#ifdef NAND_CHECK
/* One-shot write/erase validation for a chip nand_scan_banks() left
 * unrecognised -- see nand-target.h's declaration for the full picture.
 * bank's geometry must already be decoded (->page_size != 0); no
 * ->recognized check happens here on purpose, since checking whether a
 * chip CAN be safely marked recognized is exactly what this function is
 * for. This does a REAL ERASE + REAL PROGRAM against the given block. */
static struct nand_write_test_result last_write_test;

const struct nand_write_test_result *
nand_check_write_test(unsigned int bank, uint32_t block)
{
    /* static: see fmc_hw_read_page_multiunit()'s stack-overflow comment
     * -- same reasoning applies to these page-sized buffers. */
    static uint8_t pattern[NAND_MAX_PAGE_SIZE] NAND_DMA_BUF_ATTR;
    static uint8_t readback[NAND_MAX_PAGE_SIZE] NAND_DMA_BUF_ATTR;
    static uint8_t meta_out[NAND_MAX_SPARE_SIZE] NAND_DMA_BUF_ATTR;
    static uint8_t meta_in[NAND_MAX_SPARE_SIZE] NAND_DMA_BUF_ATTR;
    struct nand_geometry *geo;
    uint32_t page;
    unsigned int i;

    memset(&last_write_test, 0, sizeof(last_write_test));
    last_write_test.erase_rc = NAND_HWERR_NO_CHIP;
    last_write_test.write_rc = NAND_HWERR_NO_CHIP;
    last_write_test.read_rc = NAND_HWERR_NO_CHIP;

    if (bank >= NAND_MAX_BANKS)
        return &last_write_test;
    geo = &banks_geometry[bank];
    if (geo->page_size == 0)
        return &last_write_test;

    page = block * geo->pages_per_block;

    /* A simple, position-dependent pattern: not all-0xFF/all-0x00 (which
     * an addressing bug -- reading a different, blank or stale page --
     * could pass by accident), and byte-position-dependent enough that a
     * shifted/misaligned transfer would also be caught. */
    for (i = 0; i < geo->page_size; i++)
        pattern[i] = (uint8_t)(i * 37 + 11);
    memset(meta_in, 0, sizeof(meta_in));
    for (i = 0; i < NAND_SPARE_META_BYTES && i < geo->spare_size; i++)
        meta_in[i] = (uint8_t)(0xA5 ^ i);

    /* nand_hw_write_page()/nand_hw_erase_block() both correctly refuse
     * any bank whose ->recognized is false -- that gate is what keeps
     * ftl_init() from ever trusting an unvalidated chip, and it must
     * stay that way for every other caller. This function's entire
     * purpose is to find out whether a chip CAN be trusted, so it
     * deliberately, narrowly bypasses that one gate for the duration of
     * this call only, on this bank's local geometry entry, then puts it
     * back exactly as it was -- nothing else in the driver observes
     * ->recognized as "true" at any point this hasn't already returned. */
    bool was_recognized = geo->recognized;
    geo->recognized = true;

    last_write_test.erase_rc = nand_hw_erase_block(bank, block);
    if (last_write_test.erase_rc != 0)
    {
        geo->recognized = was_recognized;
        return &last_write_test;
    }

    last_write_test.write_rc = nand_hw_write_page(bank, page, pattern, meta_in);
    if (last_write_test.write_rc != 0)
    {
        geo->recognized = was_recognized;
        return &last_write_test;
    }

    memset(readback, 0, sizeof(readback));
    memset(meta_out, 0, sizeof(meta_out));
    last_write_test.read_rc = nand_hw_read_page(bank, page, readback, meta_out);
    geo->recognized = was_recognized;
    if (last_write_test.read_rc < 0)
        return &last_write_test;

    last_write_test.data_match =
        (memcmp(pattern, readback, geo->page_size) == 0);
    last_write_test.meta_match =
        (memcmp(meta_in, meta_out,
                NAND_SPARE_META_BYTES < geo->spare_size
                    ? NAND_SPARE_META_BYTES : geo->spare_size) == 0);

    return &last_write_test;
}

/* Broader (still bounded, still not exhaustive) confidence than a single
 * nand_check_write_test() call: one bank 0 result passing doesn't say
 * much about the other three dies in this 4-die package, or about
 * whether bank 0 got lucky at one specific block. Spreads across every
 * bank this chip actually has (from the diagnostic bank 1-3 probe in
 * nand-check-nano3g.c, which must have already run and set these dies'
 * geometry for this to reach them -- banks whose ->page_size is still 0
 * are skipped, not tested against garbage) and four blocks per bank,
 * chosen to spread across the chip's real (ID-table-sourced, not
 * probed -- see NANO3G_ORIGINAL_NAND_FTL.md) ~4096-block-per-die
 * capacity while avoiding block 0 (see nand_check_write_test()'s own
 * comment on why) and the earlier single-block test's block 8 (so this
 * sweep adds new coverage rather than re-testing the same spot). Stops
 * at the first failure rather than continuing to erase/write further
 * blocks once something has already gone wrong. */
static struct nand_write_sweep_result last_sweep;

const struct nand_write_sweep_result *nand_check_write_test_sweep(void)
{
    static const uint32_t test_blocks[] = { 16, 1024, 2048, 4000 };
    unsigned int bank;

    memset(&last_sweep, 0, sizeof(last_sweep));

    for (bank = 0; bank < NAND_MAX_BANKS; bank++)
    {
        unsigned int bi;

        if (banks_geometry[bank].page_size == 0)
            continue; /* this bank's geometry was never decoded at all */

        for (bi = 0; bi < ARRAYLEN(test_blocks); bi++)
        {
            const struct nand_write_test_result *r =
                nand_check_write_test(bank, test_blocks[bi]);
            bool ok = (r->erase_rc == 0 && r->write_rc == 0
                     && r->read_rc >= 0 && r->data_match && r->meta_match);

            last_sweep.attempted++;
            if (ok)
            {
                last_sweep.passed++;
                continue;
            }

            last_sweep.fail_bank = bank;
            last_sweep.fail_block = test_blocks[bi];
            last_sweep.fail_result = *r;
            return &last_sweep;
        }
    }

    return &last_sweep;
}
#endif

unsigned int nand_scan_banks(void)
{
    unsigned int bank;
    uint8_t first_id[8];
    bool have_reference = false;

    detected_bank_count = 0;

    for (bank = 0; bank < NAND_MAX_BANKS; bank++)
    {
        uint8_t id[8];

        if (nand_hw_reset(bank) != 0)
            break; /* no chip enable answering -> stop scanning */
        if (nand_hw_read_id(bank, id, sizeof(id)) != 0)
            break;

        if (!have_reference)
        {
            memcpy(first_id, id, sizeof(id));
            have_reference = true;
        }
        else if (memcmp(first_id, id, 4) != 0)
        {
            /* A later chip-enable answering with a different ID than
             * bank 0 means either an empty bank echoing bus noise, or a
             * genuinely mixed-part board. Either way we stop trusting
             * additional banks past this point rather than guess. */
            break;
        }

        nand_vendor_decode(id, sizeof(id), &banks_geometry[bank]);
        if (!banks_geometry[bank].recognized)
            break;

        /* nand_vendor_decode() already fills in a known-good
         * blocks_per_bank from the validated-chip table for a matched
         * part (see its own comment); only fall back to the runtime
         * probe -- confirmed unreliable for at least one real chip,
         * see NANO3G_ORIGINAL_NAND_FTL.md -- for a chip recognised by
         * the generic public-convention decode alone, which has no
         * table entry to supply a real number. Overwriting a
         * table-supplied capacity with the probe's result here would
         * silently discard the whole reason the table exists. */
        if (banks_geometry[bank].blocks_per_bank == 0)
            banks_geometry[bank].blocks_per_bank =
                probe_bank_capacity_blocks(bank, &banks_geometry[bank]);

        detected_bank_count = bank + 1;
    }

    return detected_bank_count;
}

unsigned int nand_get_bank_count(void)
{
    return detected_bank_count;
}

const struct nand_geometry *nand_get_bank_geometry(unsigned int bank)
{
    if (bank >= detected_bank_count)
        return NULL;
    return &banks_geometry[bank];
}

/* --- storage.h glue --------------------------------------------------
 * These bridge Rockbox's generic storage API to our FTL layer, mirroring
 * the pattern already used by the s5l8700 Nano 2G NAND glue
 * (firmware/target/arm/s5l8700/ata-nand-s5l8700.c) for API-shape
 * consistency with the rest of the codebase. The implementation calls
 * are all into our own nand_hw_ and ftl_ functions. */

static long last_disk_activity_tick;

/* Scans, identifies and (outside a NAND_CHECK build) mounts the FTL.
 * Split out from nand_init() so the NAND_CHECK build (see below) can
 * still get at this result to build its diagnostic report, without
 * nand_init() itself needing to know anything about that build mode. */
static int nand_init_chip(void)
{
    if (nand_hw_init() != 0)
        return NAND_ERR_NO_CHIP;

    if (nand_scan_banks() == 0)
    {
        /* Tell "nothing answered" apart from "something answered but we
         * don't recognise it", so the bootloader can report which case
         * it is. detected_bank_count stays 0 in both cases, but
         * banks_geometry[0] still holds bank 0's decoded (unrecognised)
         * ID if a chip did answer. */
        if (banks_geometry[0].maker_id != 0 || banks_geometry[0].device_id != 0)
            return NAND_ERR_UNSUPPORTED;
        return NAND_ERR_NO_CHIP;
    }

#ifndef NAND_CHECK
    {
        int rc = ftl_init();
        if (rc != 0)
            return NAND_ERR_FTL_BASE + rc;
    }
#endif
    return 0;
}

int nand_init(void)
{
    int rc = nand_init_chip();

#ifdef NAND_CHECK
    /* The check image reports whatever nand_init_chip() found (and, if a
     * chip was identified, serves it raw and read-only from
     * nand-check-nano3g.c) rather than refusing to come up over an
     * unrecognised or unmountable chip. */
    nand_check_init(rc);
    return 0;
#else
    return rc;
#endif
}

#ifdef NAND_CHECK
/* nand-check-nano3g.c needs a bank's decoded geometry even when it wasn't
 * one nand_scan_banks() counted as usable (unrecognised, or past the
 * point scanning stopped): it still wants to report what was found. */
const struct nand_geometry *nand_check_bank_geometry(unsigned int bank)
{
    if (bank >= NAND_MAX_BANKS)
        return NULL;
    return &banks_geometry[bank];
}
#endif

uint32_t nand_get_id(void)
{
    return ((uint32_t)banks_geometry[0].maker_id) |
           ((uint32_t)banks_geometry[0].device_id << 8);
}

void nand_close(void)
{
    ftl_sync();
}

#ifndef NAND_CHECK
/* A NAND_CHECK build serves the raw NAND read-only instead, from
 * nand-check-nano3g.c, which defines these entry points in this file's
 * place. */

/* The FMC DMAs directly into/out of the caller's buffer and only handles
 * word-aligned addresses: with a buffer 1-3 bytes off it ignores the low
 * address bits, so the data lands (or is fetched) shifted by that offset
 * and the transfer also runs past the end of the buffer. See this target's
 * config (STORAGE_NEEDS_BOUNCE_BUFFER) for the measured behaviour.
 *
 * That flag only makes the FAT layer bounce its own transfers, which is
 * not sufficient: firmware/common/disk.c reads the boot sector straight
 * into its disk-cache buffer, and on a real boot that pointer arrived
 * unaligned -- the read "succeeded" (rc=0) but every byte was displaced by
 * one, so the 0xAA55 signature check failed and the main firmware reported
 * "No partition found" on a filesystem its own bootloader had just mounted.
 * Silently corrupting a transfer (and writing past the caller's buffer) is
 * not something callers can be expected to guard against individually, so
 * bounce here instead, at the storage API boundary, where it covers every
 * caller. Aligned buffers -- the overwhelmingly common case -- still go
 * straight through with no copy. */
#define NAND_BUF_ALIGN_MASK  3u

static inline bool nand_buf_is_aligned(const void *buf)
{
    return ((uintptr_t)buf & NAND_BUF_ALIGN_MASK) == 0;
}

/* One sector: unaligned transfers are rare (single-sector metadata reads
 * from disk.c), so favour a small static buffer over throughput. static,
 * not a stack local, for the reason documented in ftl-nano3g.c's
 * mark_bad_best_effort(). */
static uint8_t nand_bounce_sector[NAND_PAGE_SIZE]
    __attribute__((aligned(32)));

int nand_read_sectors(IF_MD(int drive,) sector_t start, int count, void *buf)
{
    IF_MD((void)drive);
    int rc;
    last_disk_activity_tick = current_tick;

    if (nand_buf_is_aligned(buf))
    {
        rc = ftl_read((uint32_t)start, (uint32_t)count, buf);
    }
    else
    {
        uint8_t *out = (uint8_t *)buf;
        rc = 0;
        for (int i = 0; i < count; i++)
        {
            rc = ftl_read((uint32_t)start + i, 1, nand_bounce_sector);
            if (rc != 0)
                break;
            memcpy(out + (size_t)i * NAND_PAGE_SIZE, nand_bounce_sector,
                   NAND_PAGE_SIZE);
        }
    }

    return rc;
}

int nand_write_sectors(IF_MD(int drive,) sector_t start, int count,
                       const void *buf)
{
    IF_MD((void)drive);
    int rc;
    if (ftl_readonly_mount())
        return -1;
    last_disk_activity_tick = current_tick;

    if (nand_buf_is_aligned(buf))
    {
        rc = ftl_write((uint32_t)start, (uint32_t)count, buf);
    }
    else
    {
        const uint8_t *in = (const uint8_t *)buf;
        rc = 0;
        for (int i = 0; i < count; i++)
        {
            memcpy(nand_bounce_sector, in + (size_t)i * NAND_PAGE_SIZE,
                   NAND_PAGE_SIZE);
            rc = ftl_write((uint32_t)start + i, 1, nand_bounce_sector);
            if (rc != 0)
                break;
        }
    }

    return rc;
}
#endif /* !NAND_CHECK */

#ifdef HAVE_STORAGE_FLUSH
int nand_flush(void)
{
    return ftl_sync();
}
#endif

void nand_enable(bool on)
{
    (void)on;
}

void nand_spindown(int seconds)
{
    (void)seconds;
}

void nand_sleepnow(void)
{
    /* No separate low-power NAND state is implemented; the FMC clock
     * gate is left enabled for simplicity. A future improvement could
     * gate CLOCKGATE_NAND off here when idle. */
}

bool nand_disk_is_active(void)
{
    return false;
}

int nand_soft_reset(void)
{
    return 0;
}

void nand_spin(void)
{
    last_disk_activity_tick = current_tick;
}

int nand_spinup_time(void)
{
    return 0;
}

long nand_last_disk_activity(void)
{
    return last_disk_activity_tick;
}

#ifndef NAND_CHECK

#ifdef STORAGE_GET_INFO
void nand_get_info(IF_MD(int drive,) struct storage_info *info)
{
    IF_MD((void)drive);
    const struct nand_geometry *geo = nand_get_bank_geometry(0);

    info->sector_size = NAND_PAGE_SIZE;
    info->num_sectors = ftl_num_sectors();
    info->vendor  = "Apple";
    info->product = geo ? (char *)geo->maker_name : "Unknown";
    info->revision = "";

    /* Not logged: called on essentially every SCSI command (INQUIRY,
     * TEST_UNIT_READY, READ_CAPACITY, ...) and was drowning out the
     * once-per-session events (SET_CONFIG, bus reset, notify dispatch)
     * that matter for diagnosing the enumeration stall, given the ring's
     * bounded size. */
}
#endif

#ifdef HAVE_STORAGE_READONLY
bool nand_readonly(IF_MD_NONVOID(int drive))
{
    IF_MD((void)drive);
    bool ro = ftl_readonly_mount();
    /* Logged only on change: this is polled, and repeated identical lines
     * would crowd out the once-per-session entries in the debug ring. */
    static int last_logged = -1;
    if (last_logged != (int)ro)
    {
        nand_debug_log("readonly=%d", ro);
        last_logged = (int)ro;
    }
    return ro;
}
#endif

int nand_event(long id, intptr_t data)
{
    /* Not logged: firmware/storage.c's storage_thread() posts
     * Q_STORAGE_TICK here every HZ/2, which filled the entire debug ring
     * in about a minute and evicted the once-per-session entries it
     * exists to capture. */
    return storage_event_default_handler(id, data, last_disk_activity_tick,
                                         STORAGE_NAND);
}

#endif /* !NAND_CHECK */

#ifdef CONFIG_STORAGE_MULTI
int nand_num_drives(int first_drive)
{
    (void)first_drive;
    return 1;
}
#endif
