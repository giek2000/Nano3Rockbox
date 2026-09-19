/*
 * Simulated NAND backing store -- implementation. Provides the same
 * nand_hw_*() function signatures declared in nand-target.h, so
 * ftl-nano3g.c (compiled unmodified) can be linked and exercised natively
 * against this instead of real hardware.
 *
 * Original test infrastructure for this project.
 */
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include "nand-target.h"
#include "nand_vendor.h"
#include "mock_nand.h"

#define MAX_BANKS   NAND_MAX_BANKS
/* Large enough for the biggest real geometry the FTL supports: the Hynix
 * 2KB-page 8GB-unit part is 8192 blocks/bank (twice the Samsung 4KB-page
 * part's 4096), so a Hynix-geometry mount/GC test can be configured. The
 * per-block page arrays are still heap-allocated on demand in
 * mock_nand_configure(), so this only grows the static pointer table. */
#define MAX_BLOCKS  8192

/* ftl-nano3g.c records diagnostics through nand_debug_log(), which on the
 * device lives in nand-nano3g.c (a file this native harness deliberately
 * does not link, since it drives real FMC registers). Provide the same
 * symbol here so the FTL still compiles and links unmodified. Output is
 * discarded by default to keep test output clean; set MOCK_NAND_DEBUG_LOG
 * in the environment to see the lines the FTL would have recorded. */
void nand_debug_log(const char *fmt, ...)
{
    static int enabled = -1;
    va_list ap;

    if (enabled < 0)
        enabled = getenv("MOCK_NAND_DEBUG_LOG") != NULL;
    if (!enabled)
        return;

    va_start(ap, fmt);
    fputs("  [ftl] ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
}

struct mock_page
{
    uint8_t data[NAND_MAX_PAGE_SIZE];
    uint8_t spare[NAND_MAX_SPARE_SIZE];
    bool programmed; /* true once written since last erase */
};

struct mock_block
{
    struct mock_page *pages;
    uint32_t erase_count;
    bool permanent_fault;
};

static unsigned int cfg_banks;
static unsigned int cfg_blocks_per_bank;
static unsigned int cfg_pages_per_block;
static unsigned int cfg_page_size;
static unsigned int cfg_spare_size;
static uint8_t cfg_id[4];

static struct mock_block blocks[MAX_BANKS][MAX_BLOCKS];
static unsigned int selected_bank;

static uint32_t program_call_count;
static uint32_t power_loss_after;
static bool power_loss_triggered;

static bool ecc_failure_pending;

/* Backing storage for the nand_get_bank_geometry()/nand_get_bank_count()
 * shim below; populated by nand_scan_banks(). */
static struct nand_geometry mock_bank_geo_table[MAX_BANKS];
static unsigned int mock_bank_geo_count;

void mock_nand_configure(unsigned int banks, unsigned int blocks_per_bank,
                         unsigned int pages_per_block, unsigned int page_size,
                         unsigned int spare_size, uint8_t maker_id,
                         uint8_t device_id, uint8_t ext_id_byte)
{
    unsigned int b, k;

    cfg_banks = banks;
    cfg_blocks_per_bank = blocks_per_bank;
    cfg_pages_per_block = pages_per_block;
    cfg_page_size = page_size;
    cfg_spare_size = spare_size;
    cfg_id[0] = maker_id;
    cfg_id[1] = device_id;
    cfg_id[2] = 0x00; /* SLC, bits_per_cell field = 0 -> 1 bit/cell */
    cfg_id[3] = ext_id_byte;

    for (b = 0; b < banks; b++)
    {
        for (k = 0; k < blocks_per_bank; k++)
        {
            free(blocks[b][k].pages);
            blocks[b][k].pages = calloc(pages_per_block, sizeof(struct mock_page));
            blocks[b][k].erase_count = 0;
            blocks[b][k].permanent_fault = false;
            for (unsigned int p = 0; p < pages_per_block; p++)
                memset(blocks[b][k].pages[p].data, 0xFF, sizeof(blocks[b][k].pages[p].data));
        }
    }
}

void mock_nand_reset_state(void)
{
    unsigned int b, k, p;
    for (b = 0; b < cfg_banks; b++)
    {
        for (k = 0; k < cfg_blocks_per_bank; k++)
        {
            blocks[b][k].erase_count = 0;
            blocks[b][k].permanent_fault = false;
            for (p = 0; p < cfg_pages_per_block; p++)
            {
                memset(blocks[b][k].pages[p].data, 0xFF, sizeof(blocks[b][k].pages[p].data));
                memset(blocks[b][k].pages[p].spare, 0xFF, sizeof(blocks[b][k].pages[p].spare));
                blocks[b][k].pages[p].programmed = false;
            }
        }
    }
    program_call_count = 0;
    power_loss_after = 0;
    power_loss_triggered = false;
    ecc_failure_pending = false;
}

void mock_nand_inject_power_loss_after(uint32_t program_calls)
{
    power_loss_after = program_calls;
    power_loss_triggered = false;
    program_call_count = 0;
}

bool mock_nand_power_loss_triggered(void)
{
    return power_loss_triggered;
}

void mock_nand_clear_power_loss(void)
{
    power_loss_triggered = false;
    power_loss_after = 0;
}

void mock_nand_inject_permanent_fault(unsigned int bank, uint32_t block)
{
    if (bank < cfg_banks && block < cfg_blocks_per_bank)
        blocks[bank][block].permanent_fault = true;
}

void mock_nand_inject_ecc_failure_on_next_read(void)
{
    ecc_failure_pending = true;
}

uint32_t mock_nand_erase_count(unsigned int bank, uint32_t block)
{
    if (bank >= cfg_banks || block >= cfg_blocks_per_bank)
        return 0;
    return blocks[bank][block].erase_count;
}

uint32_t mock_nand_program_count(void)
{
    return program_call_count;
}

extern uint32_t mock_read_call_count;
uint32_t mock_nand_read_count(void)
{
    return mock_read_call_count;
}

void mock_nand_peek_page(unsigned int bank, uint32_t page,
                         uint8_t *data_out, uint8_t *spare_out)
{
    unsigned int block = page / cfg_pages_per_block;
    unsigned int p = page % cfg_pages_per_block;
    if (bank >= cfg_banks || block >= cfg_blocks_per_bank)
        return;
    if (data_out)
        memcpy(data_out, blocks[bank][block].pages[p].data, cfg_page_size);
    if (spare_out)
        memcpy(spare_out, blocks[bank][block].pages[p].spare, cfg_spare_size);
}

/* --- nand_hw_* implementations consumed by ftl-nano3g.c --------------- */

int nand_hw_init(void)
{
    selected_bank = 0;
    return 0;
}

int nand_hw_reset(unsigned int bank)
{
    if (bank >= cfg_banks)
        return NAND_HWERR_NO_CHIP;
    if (power_loss_triggered)
        return NAND_HWERR_TIMEOUT;
    selected_bank = bank;
    return 0;
}

int nand_hw_read_id(unsigned int bank, uint8_t *id_out, unsigned int id_len)
{
    unsigned int i;
    if (bank >= cfg_banks)
        return NAND_HWERR_NO_CHIP;
    if (power_loss_triggered)
        return NAND_HWERR_TIMEOUT;
    for (i = 0; i < id_len; i++)
        id_out[i] = (i < 4) ? cfg_id[i] : 0x00;
    return 0;
}

uint32_t mock_read_call_count; /* exposed via mock_nand_read_count() */

int nand_hw_read_page(unsigned int bank, uint32_t page,
                      void *data_out, void *spare_out)
{
    unsigned int block, p;

    mock_read_call_count++;

    if (bank >= cfg_banks)
        return NAND_HWERR_NO_CHIP;
    if (power_loss_triggered)
        return NAND_HWERR_TIMEOUT;

    block = page / cfg_pages_per_block;
    p = page % cfg_pages_per_block;
    if (block >= cfg_blocks_per_bank)
        return NAND_HWERR_TIMEOUT; /* out of range -> simulate no response */

    if (data_out)
        memcpy(data_out, blocks[bank][block].pages[p].data, cfg_page_size);
    if (spare_out)
        memcpy(spare_out, blocks[bank][block].pages[p].spare, cfg_spare_size);

    if (ecc_failure_pending)
    {
        ecc_failure_pending = false;
        return NAND_ECC_FAILED;
    }

    return blocks[bank][block].pages[p].programmed ? NAND_ECC_OK : NAND_ECC_CLEAN;
}

int nand_hw_write_page(unsigned int bank, uint32_t page,
                       const void *data_in, const void *spare_in)
{
    unsigned int block, p;

    if (bank >= cfg_banks)
        return NAND_HWERR_NO_CHIP;
    if (power_loss_triggered)
        return NAND_HWERR_TIMEOUT;

    block = page / cfg_pages_per_block;
    p = page % cfg_pages_per_block;
    if (block >= cfg_blocks_per_bank)
        return NAND_HWERR_TIMEOUT;

    if (blocks[bank][block].permanent_fault)
        return NAND_HWERR_PROGRAM_FAILED;

    program_call_count++;
    if (power_loss_after != 0 && program_call_count > power_loss_after)
    {
        /* Simulate power loss occurring mid-program: the page's contents
         * are left exactly as they were before this call (a real NAND
         * program that's interrupted mid-transfer typically leaves the
         * page's bits somewhere between all-1s and the intended data --
         * we model the conservative case of "nothing landed" here, which
         * is what a driver correctly detects via a subsequent timeout/
         * failed status check). */
        power_loss_triggered = true;
        return NAND_HWERR_TIMEOUT;
    }

    memcpy(blocks[bank][block].pages[p].data, data_in, cfg_page_size);
    if (spare_in)
        memcpy(blocks[bank][block].pages[p].spare, spare_in, cfg_spare_size);
    blocks[bank][block].pages[p].programmed = true;

    return 0;
}

int nand_hw_erase_block(unsigned int bank, uint32_t block)
{
    unsigned int p;

    if (bank >= cfg_banks)
        return NAND_HWERR_NO_CHIP;
    if (power_loss_triggered)
        return NAND_HWERR_TIMEOUT;
    if (block >= cfg_blocks_per_bank)
        return NAND_HWERR_TIMEOUT;

    if (blocks[bank][block].permanent_fault)
        return NAND_HWERR_ERASE_FAILED;

    for (p = 0; p < cfg_pages_per_block; p++)
    {
        memset(blocks[bank][block].pages[p].data, 0xFF, cfg_page_size);
        memset(blocks[bank][block].pages[p].spare, 0xFF, cfg_spare_size);
        blocks[bank][block].pages[p].programmed = false;
    }
    blocks[bank][block].erase_count++;
    return 0;
}

unsigned int nand_scan_banks(void)
{
    /* The real driver's bank-scan/probing logic (nand-nano3g.c) is not
     * under test here -- only the FTL layer is. Tests call
     * mock_nand_configure() then this simplified scan, which decodes the
     * configured ID bytes through the real nand_vendor_decode() (so that
     * *is* exercised end-to-end) and reports the pre-configured block
     * count rather than re-deriving it via hardware probing (that probing
     * logic lives in nand-nano3g.c, which isn't linked into this host
     * test binary). */
    unsigned int b;
    uint8_t id[4] = { cfg_id[0], cfg_id[1], cfg_id[2], cfg_id[3] };

    for (b = 0; b < cfg_banks; b++)
    {
        nand_vendor_decode(id, sizeof(id), &mock_bank_geo_table[b]);
        mock_bank_geo_table[b].blocks_per_bank = cfg_blocks_per_bank;
    }
    mock_bank_geo_count = cfg_banks;
    return cfg_banks;
}

unsigned int nand_get_bank_count(void)
{
    return mock_bank_geo_count;
}

const struct nand_geometry *nand_get_bank_geometry(unsigned int bank)
{
    if (bank >= mock_bank_geo_count)
        return NULL;
    return &mock_bank_geo_table[bank];
}

uint32_t nand_get_id(void)
{
    return ((uint32_t)cfg_id[0]) | ((uint32_t)cfg_id[1] << 8);
}
