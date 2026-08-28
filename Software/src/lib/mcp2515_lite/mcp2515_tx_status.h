#pragma once

#include <stdint.h>

/* The transmit half of what the MCP2515's READ STATUS command answers.
 *
 * The driver used to shadow "which transmit buffers are busy" in a mask it
 * maintained from the TXnIF interrupt flags. That shadow can only ever be as
 * good as the flags it saw - and (d) stops the chip raising the transmit ones
 * at all, because the pin is level triggered now and TXnIF does not clear
 * itself. Rather than keep a copy that nothing refreshes, ask the chip: READ
 * STATUS (0xA0) returns all three TXBnCTRL.TXREQ bits in one 2-byte
 * transaction, which is the same question the mask was trying to answer.
 *
 * Header-only and dependency-free, so the decode is testable on the host.
 */

/* The READ STATUS byte is RX0IF, RX1IF, TXB0 TXREQ, TX0IF, TXB1 TXREQ, TX1IF,
 * TXB2 TXREQ, TX2IF - so buffer n's TXREQ sits at bit 2 + 2n.
 */
#define MCP2515_STATUS_TXREQ0_BIT 2
#define MCP2515_STATUS_TXREQ_STRIDE 2

// Transmit buffers, and the free mask that means all of them.
#define MCP2515_TX_BUFFERS 3
#define MCP2515_TX_ALL_FREE 0x07

/* Which transmit buffers are free, as a bitmask with TXB0 in the low bit.
 *
 * A buffer is free exactly when its TXREQ is clear: the chip clears TXREQ once
 * the frame has been sent or aborted, which is the same moment the buffer can
 * be loaded again. The transmit interrupt flags in the same byte say something
 * different - that a completion has not been acknowledged yet - and are
 * deliberately not read here.
 */
static inline uint8_t mcp2515_tx_free_mask(uint8_t status) {
  uint8_t free_mask = 0;
  for (uint8_t buffer = 0; buffer < MCP2515_TX_BUFFERS; buffer++) {
    const uint8_t txreq = 1 << (MCP2515_STATUS_TXREQ0_BIT + MCP2515_STATUS_TXREQ_STRIDE * buffer);
    if ((status & txreq) == 0) {
      free_mask |= 1 << buffer;
    }
  }
  return free_mask;
}
