/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") NAND chip driver interface.
 *
 * Design notes (original work; not derived from any third-party Nano 3G
 * patch or driver):
 *
 * The N46 is a S5L8702 flash player. It has no CE-ATA/HDD interface;
 * instead the SoC's "FMC" (Flash Memory Controller) peripheral drives an
 * array of raw SLC/MLC NAND chip enables. Two independent lines of
 * evidence were used to confirm the FMC MMIO base address used below:
 *
 *   1. Static analysis of the decrypted OSOS 1.1.3 firmware for this
 *      device: the constant 0x38a00000 occurs as a 32-bit literal-pool
 *      value referenced by initialisation code that zeroes two 32-bit
 *      registers (offsets +0x000 and +0x400) early in NAND bring-up,
 *      consistent with a controller/status register pair being cleared
 *      before first use.
 *   2. The freemyipod/wInd3x BootROM DFU exploit tooling (an independent,
 *      third-party project) separately documents 0x38a00000 as the "first
 *      FMC controller" base for this same device family, exercised live
 *      against real hardware over USB DFU. That is corroborating evidence
 *      from a different vantage point (BootROM code, not OSOS), not the
 *      source of this driver's implementation.
 *
 * Only the MMIO base address above and the general "byte-wide command /
 * ready-busy polled" shape of the interface are treated as verified
 * hardware facts. The chip identification and geometry logic in this
 * driver is our own, written from public NAND standards (JEDEC
 * manufacturer ID assignments and the industry-common "extended ID"
 * decoding scheme documented in the Linux kernel's MTD/NAND subsystem,
 * itself derived from NAND vendors' public datasheets) rather than from
 * any device-specific chip table extracted from Apple's firmware or from
 * any other project's Nano 3G work.
 */

#ifndef __NAND_TARGET_H__
#define __NAND_TARGET_H__

#include <stdint.h>
#include <stdbool.h>

/* Alignment required of every page/spare buffer passed to
 * nand_hw_read_page()/nand_hw_write_page().
 *
 * The FMC DMAs straight into/out of the caller's buffer and ignores the low
 * bits of the address, so a buffer that is 1-3 bytes off a word boundary
 * transfers *shifted by that offset* while still reporting success. A plain
 * `static uint8_t[]` carries no such guarantee. This was a real,
 * confirmed-on-hardware bug: the linker placed the FTL's page buffer on an
 * odd address in the main-firmware build but an aligned one in the
 * bootloader build, so the bootloader mounted the filesystem and loaded the
 * firmware from it correctly while that same firmware read every sector
 * displaced by one byte -- the FAT boot signature 0xAA55 came back as
 * 0x00AA and it declared "No partition found" on intact media.
 *
 * 32 bytes (the ARM926 cache line) also makes the driver's
 * commit_dcache_range()/DISCARD_DCACHE_RANGE() calls safe, since those act
 * on whole cache lines and would otherwise touch neighbouring data. */
#define NAND_DMA_BUF_ATTR  __attribute__((aligned(32)))

/* Smallest supported page size. 4KiB-page chips are handled as two
 * NAND_PAGE_SIZE sectors per physical page at the storage layer, matching
 * how Rockbox's other flash targets present large-page NAND. */
#define NAND_PAGE_SIZE      2048
#define NAND_MAX_PAGE_SIZE  4096
#define NAND_MAX_SPARE_SIZE 256

/* This controller's chunked/pipelined transfer mechanism (nand-nano3g.c)
 * surfaces only this many bytes of real per-page metadata to software
 * (FMSYND5/6/7), regardless of a chip's actual spare_size -- the
 * controller consumes/generates the rest of the spare area internally
 * as ECC parity, which software never sees as raw bytes through this
 * path. This is a confirmed hardware fact (see nand-nano3g.c's file
 * header), not a design choice: any code (spare_in/spare_out callers of
 * nand_hw_read_page()/nand_hw_write_page(), and any on-flash metadata
 * format built on top, e.g. ftl-target.h's struct ftl_page_header) must
 * fit within this many bytes to survive round-tripping through this
 * driver. */
#define NAND_SPARE_META_BYTES  12

/* The FMC has chip-enable (bank) select lines; four are wired on the units
 * we can identify from public teardown photography of the 4GB/8GB N46. */
#define NAND_MAX_BANKS      4

/* Outcomes of a single-page read, ordered by severity so callers can treat
 * the value as "worse than" comparisons where useful. */
enum nand_ecc_result
{
    NAND_ECC_CLEAN     = 0,  /* page reads all-0xFF: erased/unwritten */
    NAND_ECC_OK        = 1,  /* page had no correctable bit errors */
    NAND_ECC_CORRECTED = 2,  /* page had correctable bit errors, fixed */
    NAND_ECC_FAILED    = 3,  /* uncorrectable: data is not trustworthy */
};

/* Chip family classification used to select the geometry-decode strategy.
 * Legacy small-page parts (<=512B page) predate the "extended ID" scheme;
 * everything shipped in a 2007-era Nano 3G is expected to hit LARGE_PAGE. */
enum nand_id_scheme
{
    NAND_ID_SCHEME_UNKNOWN    = 0,
    NAND_ID_SCHEME_LEGACY     = 1,  /* byte[1] alone selects a fixed geometry */
    NAND_ID_SCHEME_LARGE_PAGE = 2,  /* byte[3] "extended ID" decode (JEDEC-
                                       style, see nand_vendor.c) */
};

struct nand_geometry
{
    uint8_t  maker_id;          /* READ ID byte 0 */
    uint8_t  device_id;         /* READ ID byte 1 */
    const char *maker_name;     /* for logging only; may be "Unknown" */
    unsigned int page_size;     /* bytes of data per page (2048 or 4096) */
    unsigned int spare_size;    /* out-of-band bytes per page */
    unsigned int pages_per_block;
    unsigned int blocks_per_bank;/* total, including any reserved for VFL/BBT */
    unsigned int bits_per_cell; /* 1 = SLC, >1 = MLC (unsupported for now) */
    bool         bus_width_16;  /* NAND data bus width; N46 wiring is x8 */
    bool         recognized;    /* false: decoded speculatively, treat with
                                    extra caution (still readable, but the
                                    FTL layer will refuse to mount r/w) */
};

/* --- Low level bank/chip operations --------------------------------- */

/* One-time controller + bank bring-up. Returns 0 on success. */
int nand_hw_init(void);

/* Send a RESET (0xFF) to the given bank and wait for the chip to report
 * ready. Returns 0, or a negative NAND_HWERR_* on timeout. */
int nand_hw_reset(unsigned int bank);

/* Issue READ ID (0x90) at address 0x00 on the given bank. Fills up to
 * id_len bytes (id_len should be 4 or 8; chips vary in how many bytes of
 * their ID are meaningful, see nand_vendor.c). Returns 0 on success. */
int nand_hw_read_id(unsigned int bank, uint8_t *id_out, unsigned int id_len);

/* Read one page's main data (and, if spare_out is non-NULL, its full
 * out-of-band area) into caller buffers sized for the caller's already
 * negotiated geometry. Returns a nand_ecc_result on success (>= 0), or a
 * negative NAND_HWERR_* on a hardware timeout. */
int nand_hw_read_page(unsigned int bank, uint32_t page,
                      void *data_out, void *spare_out);

/* Program one page. data_in must be page_size bytes; spare_in, if
 * non-NULL, is spare_size bytes. Returns 0, NAND_HWERR_PROGRAM_FAILED if
 * the chip's status register reports failure, or another NAND_HWERR_*. */
int nand_hw_write_page(unsigned int bank, uint32_t page,
                       const void *data_in, const void *spare_in);

/* Erase one block. Returns 0, NAND_HWERR_ERASE_FAILED, or another
 * NAND_HWERR_*. */
int nand_hw_erase_block(unsigned int bank, uint32_t block);

enum nand_hw_error
{
    NAND_HWERR_TIMEOUT        = -1,  /* controller/chip never went ready */
    NAND_HWERR_ERASE_FAILED   = -2,  /* chip status reports erase failure */
    NAND_HWERR_PROGRAM_FAILED = -3,  /* chip status reports program failure */
    NAND_HWERR_NO_CHIP        = -4,  /* bank has no chip enable asserted */
    /* This pass of the driver only ported the controller's single
     * 2048-byte-unit chunked transfer sequence (see nand-nano3g.c); a
     * chip whose decoded page_size isn't 2048 hits this rather than
     * being driven incorrectly. Multi-unit (4KiB-page) support is a
     * known, documented follow-up, not an oversight. */
    NAND_HWERR_UNSUPPORTED_GEOMETRY = -5,
};

/* --- Bank discovery --------------------------------------------------- */

/* Reset and identify every chip-enable in turn, stopping at the first bank
 * that doesn't answer or whose ID doesn't match bank 0. Probes each
 * recognised bank's block count directly (see nand-nano3g.c) rather than
 * trusting a hardcoded density table. Returns the number of usable banks
 * (0 if nothing was found or the first bank's chip wasn't recognised). */
unsigned int nand_scan_banks(void);

/* Number of banks nand_scan_banks() found usable. 0 before scanning. */
unsigned int nand_get_bank_count(void);

/* Decoded geometry for a scanned bank, or NULL if out of range. */
const struct nand_geometry *nand_get_bank_geometry(unsigned int bank);

/* nand_init() return codes the bootloader checks for, to decide whether to
 * fall back to Apple's firmware rather than mount an unrecognised or
 * absent chip. */
#define NAND_ERR_NO_CHIP        (-1)  /* no chip enable answered */
#define NAND_ERR_UNSUPPORTED    (-2)  /* chip answered, not recognised */

/* nand_init() folds ftl_init()'s (negative, see ftl-target.h's
 * enum ftl_init_error) return code into its own return value as
 * NAND_ERR_FTL_BASE + ftl_rc, keeping it clear of NAND_ERR_NO_CHIP and
 * NAND_ERR_UNSUPPORTED. A caller that gets an nand_init() return value
 * more negative than NAND_ERR_UNSUPPORTED can recover the original
 * ftl_init() code as (rc - NAND_ERR_FTL_BASE). */
#define NAND_ERR_FTL_BASE      (-100)

/* Bank 0's raw maker/device ID bytes packed as (maker | device << 8), for
 * diagnostic display when nand_init() reports NAND_ERR_UNSUPPORTED. 0 if
 * nand_scan_banks() has not run or nothing answered. */
uint32_t nand_get_id(void);

#ifdef NAND_CHECK
/* Decoded geometry for any bank slot, even one nand_scan_banks() didn't
 * count as usable (unrecognised chip, or past where scanning stopped),
 * so the check image (nand-check-nano3g.c) can report what was found
 * without nand-nano3g.c needing to expose its internal state array. NULL
 * if bank is out of range. */
const struct nand_geometry *nand_check_bank_geometry(unsigned int bank);

/* Read-only block-count probe for a bank whose chip answered READ ID and
 * got a plausible (page_size != 0) geometry decode, but wasn't counted
 * "usable" by nand_scan_banks() -- e.g. an identified-but-unrecognised
 * (MLC, or otherwise unvalidated) chip, for which the normal scan never
 * ran its capacity probe. This only issues nand_hw_read_page() calls (no
 * writes/erases), so it's safe to run against any chip regardless of
 * validation status; it exists so a -DNAND_CHECK build can still dump
 * real page/spare data for such a chip rather than reporting "row none".
 * Fills in and returns nand_check_bank_geometry(bank)->blocks_per_bank;
 * 0 if that bank's geometry was never decoded (page_size == 0). */
unsigned int nand_check_probe_bank_capacity(unsigned int bank);
#endif

/* A small, always-compiled-in (not NAND_CHECK-only) ring of short debug
 * lines, appended to by this driver's real (non-NAND_CHECK) storage glue
 * functions (nand_get_info()/nand_read_sectors()/nand_write_sectors()/
 * nand_event() in nand-nano3g.c) every time the USB mass-storage stack
 * calls into them. Exists to diagnose a real hardware finding: the real
 * USB mass-storage class driver fails to start against this device
 * (Windows: CM_PROB_FAILED_START) even though a simpler, custom USB
 * serving path (the NAND_CHECK build's own nand-check-nano3g.c) works,
 * and the failure gives no other visibility into which storage call --
 * if any -- ran, or what it returned, before the USB session ended.
 * A bootloader build can print this ring, one page at a time (see
 * nand_debug_log_render_page()), advanced by a button press, so the
 * user can photograph each page for review -- no host-side USB serving
 * is needed to read it, which matters here since the failure IS in USB
 * serving. */
#define NAND_DEBUG_LOG_LINES  128
#define NAND_DEBUG_LOG_LINE_LEN 40
void nand_debug_log(const char *fmt, ...);
/* Number of lines actually recorded so far (may be less than
 * NAND_DEBUG_LOG_LINES if the ring hasn't wrapped yet). */
unsigned int nand_debug_log_count(void);
/* Copies up to NAND_DEBUG_LOG_LINE_LEN-1 chars + NUL of the line at
 * logical index i (0 = oldest still retained) into out. Does nothing if
 * i >= nand_debug_log_count(). */
void nand_debug_log_get(unsigned int i, char *out, unsigned int out_len);
#ifdef NAND_CHECK

/* Decodes geometry for a bank nand_scan_banks() never reached (it stops
 * at the first unrecognised bank), purely for diagnostics -- see
 * nand-nano3g.c's implementation for exactly what this does and does
 * not affect. */
void nand_check_decode_bank_geometry(unsigned int bank, const uint8_t *id,
                                     unsigned int id_len);

/* One-shot, explicit-invocation-only erase/write/read/verify self-test
 * against a single page, for validating a chip nand_scan_banks() left
 * unrecognised (this is the only way to reach nand_hw_write_page()/
 * nand_hw_erase_block() for such a chip at all -- both still gate on
 * ->recognized, and this function is the one place that deliberately,
 * temporarily bypasses that gate for the duration of the test, then
 * restores it). Never called automatically by nand_init() or
 * nand_check_init(); a NAND_CHECK build's own code must call it
 * explicitly, which it currently does not do by default. This performs
 * a REAL ERASE (destroys the target block's prior contents) and a REAL
 * PROGRAM -- it is not read-only, unlike every other NAND_CHECK
 * diagnostic in this driver. */
struct nand_write_test_result
{
    int  erase_rc;    /* nand_hw_erase_block()'s return */
    int  write_rc;     /* nand_hw_write_page()'s return */
    int  read_rc;       /* nand_hw_read_page()'s return (an ecc result on
                           success, i.e. >= 0; a negative NAND_HWERR_* on
                           failure) */
    bool data_match;   /* written pattern read back byte-for-byte equal */
    bool meta_match;    /* the 12 real spare-metadata bytes round-tripped */
};
const struct nand_write_test_result *
nand_check_write_test(unsigned int bank, uint32_t block);

/* Runs nand_check_write_test() across a small, bounded set of (bank,
 * block) pairs rather than just one, for broader (still not exhaustive)
 * confidence than a single passing block gives. Same real-erase/
 * real-program caveats as nand_check_write_test() apply, multiplied by
 * however many pairs are tested. See nand-nano3g.c's implementation for
 * the actual pairs tried. */
struct nand_write_sweep_result
{
    unsigned int attempted;   /* how many (bank, block) pairs were tried */
    unsigned int passed;      /* erase==0 && write==0 && read>=0 &&
                                  data_match && meta_match, for all of
                                  the above */
    /* First failing pair's details, valid only if passed < attempted */
    unsigned int fail_bank;
    uint32_t     fail_block;
    struct nand_write_test_result fail_result;
};
const struct nand_write_sweep_result *nand_check_write_test_sweep(void);

/* Diagnostic trace of the last capacity probe's doubling-phase stopping
 * point (see nand-nano3g.c:probe_bank_capacity_blocks()): which block/
 * page a read failed at, and with what nand_hw_error/nand_ecc_result
 * code, so a report can distinguish "genuinely ran out of chip" /
 * "hit a hardware timeout partway through" from a plain bad block
 * (which doesn't stop the probe at all -- only NAND_HWERR_TIMEOUT/
 * NAND_HWERR_NO_CHIP do). Always available in a NAND_CHECK build,
 * whether or not the doubling phase actually stopped early (->stopped
 * is false if it ran to the 16384-block cap instead). */
struct nand_probe_trace
{
    bool     stopped;
    uint32_t stop_block;
    uint32_t stop_page;
    int      stop_rc;
};
const struct nand_probe_trace *nand_check_last_probe_trace(void);

/* Provided by nand-check-nano3g.c, called from nand_init() (nand-nano3g.c)
 * once nand_init_chip() has run, so the check image's report reflects
 * whatever was found even when that isn't a chip we'd normally mount. */
void nand_check_init(int rc);
const char *nand_check_report(void);
void nand_check_note(const char *line);
#endif

#endif /* __NAND_TARGET_H__ */
