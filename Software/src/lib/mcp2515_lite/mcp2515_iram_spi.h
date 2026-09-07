#pragma once

#include <stdint.h>

#include <esp_attr.h>

/* A freestanding, register-level SPI service for the MCP2515's interrupt drain
 * so it stays usable while the flash cache is off.
 *
 * The ESP-IDF SPI master driver is not callable from an interrupt, and the
 * Arduino SPI class reaches into flash-resident code. This does one full-duplex
 * transfer of up to 64 bytes by writing the SPI peripheral's own registers and
 * busy-waiting on the USR bit, with software chip select driven through the GPIO
 * output registers - all of it register access the compiler emits inline, so the
 * whole transfer survives a flash-cache-off window.
 *
 * 64 bytes is not a limitation here: the longest transfer the drain issues is
 * the 14 bytes of one READ RX BUFFER, which the peripheral's internal FIFO holds
 * outright, so no DMA and no descriptors are involved.
 *
 * The service does NOT configure the bus. It snapshots the configuration the
 * Arduino SPI class left behind at bind() time and restores those registers
 * before every transfer, which is what lets it share a peripheral with the
 * driver task without depending on who used it last.
 */
class Mcp2515IramSpi {
 public:
  /* Capture the bus configuration and the chip select. Must be called from task
   * context, with the bus already configured by an Arduino transaction using the
   * settings the drain will run at. Returns false if this target has no
   * register-level implementation, in which case the caller must not use the
   * ISR drain.
   */
  bool bind(uint8_t spi_bus, int8_t cs_pin);

  bool bound() const { return _bound; }

  /* One full-duplex transfer of `length` bytes with chip select asserted around
   * it. Either buffer may be null; a null transmit buffer clocks out 0xFF.
   *
   * Returns false when the transfer did not complete - a rejected argument, or
   * the peripheral never dropping USR. The caller MUST discard rx_data then
   * the receive registers still hold whatever was there, and a drain
   * that decodes them publishes a CAN frame that was never on the wire.
   *
   * The definition carries IRAM_ATTR, not this declaration: the attribute names
   * a section per occurrence, so stating it twice makes the two disagree.
   */
  bool transfer(const uint8_t* tx_data, uint8_t* rx_data, uint8_t length);

  // Transfers that gave up waiting for the peripheral. Non-zero means the bus
  // is not behaving; those transfers return false and their frames are dropped.
  uint32_t timeouts() const { return _timeouts; }

 private:
  bool _bound = false;
  uint8_t _bus = 0;

  // Snapshot of the peripheral configuration registers, restored per transfer.
  uint32_t _clock = 0;
  uint32_t _ctrl = 0;
  uint32_t _user = 0;
  uint32_t _user1 = 0;
  uint32_t _user2 = 0;
  uint32_t _pin = 0;

  // Chip select, as the GPIO set/clear registers and the bit to write to them.
  uint32_t _cs_set_reg = 0;
  uint32_t _cs_clear_reg = 0;
  uint32_t _cs_mask = 0;

  volatile uint32_t _timeouts = 0;
};
