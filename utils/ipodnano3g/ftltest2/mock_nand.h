/*
 * Simulated NAND backing store for testing ftl-nano3g.c natively.
 *
 * This is original test infrastructure, written independently of the
 * cherry-picked reference project's utils/ipodnano3g/ftltest tooling (a
 * separate, unrelated implementation exists there from that project; this
 * one is written from scratch against our own nand-target.h API, which has
 * a different shape).
 */
#ifndef MOCK_NAND_H
#define MOCK_NAND_H

#include <stdint.h>
#include <stdbool.h>
#include "nand-target.h"

/* Configure the simulated geometry. Must be called before mock_nand_reset()
 * so the driver-facing nand_hw_* calls have something to identify. */
void mock_nand_configure(unsigned int banks, unsigned int blocks_per_bank,
                         unsigned int pages_per_block, unsigned int page_size,
                         unsigned int spare_size, uint8_t maker_id,
                         uint8_t device_id, uint8_t ext_id_byte);

/* Reset all simulated state to freshly erased (all 0xFF), clear all fault
 * injection and power-loss state, zero all wear/fault counters. */
void mock_nand_reset_state(void);

/* --- Fault injection ------------------------------------------------- */

/* After this many total page-program calls (across all banks/blocks) since
 * the last mock_nand_reset_state() or mock_nand_clear_power_loss(), the
 * mock stops actually writing data (but still returns success for that
 * call, simulating a program that "landed" only up to some point) and all
 * subsequent hardware calls behave as if power was cut: reads/writes/
 * erases past this point return NAND_HWERR_TIMEOUT, as they would on a
 * device that's mid-reboot. Pass 0 to disable (default). */
void mock_nand_inject_power_loss_after(uint32_t program_calls);

/* Whether the injected power-loss point (if any) has been reached. */
bool mock_nand_power_loss_triggered(void);

/* Clear a triggered power-loss condition and resume normal operation,
 * simulating the device being power-cycled back on. Does NOT erase or
 * otherwise change flash content -- only clears the "device is off"
 * fault state, so a subsequent ftl_init() call sees the flash exactly as
 * it was at the moment power was lost. */
void mock_nand_clear_power_loss(void);

/* Force nand_hw_erase_block()/nand_hw_write_page() to report failure
 * (as a real chip's status register would) for this many-specific
 * (bank, physical block) pair, permanently, to exercise bad-block
 * handling. Set count to 0 to clear. */
void mock_nand_inject_permanent_fault(unsigned int bank, uint32_t block);

/* Force the VERY NEXT nand_hw_read_page() call (any bank/page) to report
 * NAND_ECC_FAILED (a non-negative enum value -- a real, hardware-flagged
 * UNCORRECTABLE result, distinct from mock_nand_inject_permanent_fault()'s
 * negative NAND_HWERR_* "the controller/chip didn't respond at all").
 * Single-shot: clears itself after firing once, so a test doesn't need
 * to predict which exact physical (bank, page) address the FTL's
 * allocator/mapping will actually read next. The page's stored data/
 * spare are still copied out as normal -- exactly what a real chip
 * does: the bytes come back, they're just not trustworthy. */
void mock_nand_inject_ecc_failure_on_next_read(void);

/* Total erase and program operations actually performed since the last
 * reset, for wear-leveling distribution checks in tests. */
uint32_t mock_nand_erase_count(unsigned int bank, uint32_t block);

/* Total page-program calls issued to the driver since the last
 * mock_nand_reset_state() (or mock_nand_inject_power_loss_after(), which
 * also zeroes it). Used to measure write amplification directly: a
 * page-level FTL should program ~one page per logical page written, a
 * block-rewrite FTL programs a whole block's worth per write. */
uint32_t mock_nand_program_count(void);

/* Total nand_hw_read_page() calls since the last reset -- used to quantify
 * mount-scan cost at real geometry. */
uint32_t mock_nand_read_count(void);

/* Direct peek at a page's stored data/spare, bypassing the driver API --
 * used by tests to assert on-flash content without going through
 * nand_hw_read_page() (which is also what's under test). */
void mock_nand_peek_page(unsigned int bank, uint32_t page,
                         uint8_t *data_out, uint8_t *spare_out);

#endif /* MOCK_NAND_H */
