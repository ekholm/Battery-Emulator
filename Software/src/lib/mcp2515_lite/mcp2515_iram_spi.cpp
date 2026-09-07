#include "mcp2515_iram_spi.h"

#include <soc/gpio_reg.h>
#include <soc/soc.h>
#include <soc/spi_reg.h>

// The register layout below is the ESP32's (LX6). The S3 and the risc-v parts
// name and place these fields differently, and no board in the tree puts an
// MCP2515 on one, so bind() refuses rather than pretending.
#if CONFIG_IDF_TARGET_ESP32

// The peripheral clocks 64 bytes at 10 MHz in ~51 us; at 240 MHz that is a few
// thousand passes of the wait loop. This ceiling is two orders above it, so it
// can only be reached by a bus that has stopped responding.
static const uint32_t SPI_WAIT_SPINS_MAX = 200000;

// Only pins 0..33 can drive an output, and 32/33 live in the second bank.
static const uint8_t GPIO_BANK1_FIRST_PIN = 32;
static const uint8_t GPIO_LAST_OUTPUT_PIN = 33;

bool Mcp2515IramSpi::bind(uint8_t spi_bus, int8_t cs_pin) {
  _bound = false;

  // REG_SPI_BASE() covers SPI0..SPI3; the two general purpose buses are the
  // only ones a device may be attached to.
  if (spi_bus != 2 && spi_bus != 3) {
    return false;
  }
  if (cs_pin < 0 || cs_pin > GPIO_LAST_OUTPUT_PIN) {
    return false;
  }

  _bus = spi_bus;
  _clock = REG_READ(SPI_CLOCK_REG(_bus));
  _ctrl = REG_READ(SPI_CTRL_REG(_bus));
  _user = REG_READ(SPI_USER_REG(_bus));
  _user1 = REG_READ(SPI_USER1_REG(_bus));
  _user2 = REG_READ(SPI_USER2_REG(_bus));
  _pin = REG_READ(SPI_PIN_REG(_bus));

  if (cs_pin >= GPIO_BANK1_FIRST_PIN) {
    _cs_set_reg = GPIO_OUT1_W1TS_REG;
    _cs_clear_reg = GPIO_OUT1_W1TC_REG;
    _cs_mask = 1u << (cs_pin - GPIO_BANK1_FIRST_PIN);
  } else {
    _cs_set_reg = GPIO_OUT_W1TS_REG;
    _cs_clear_reg = GPIO_OUT_W1TC_REG;
    _cs_mask = 1u << cs_pin;
  }

  _bound = true;
  return true;
}

bool IRAM_ATTR Mcp2515IramSpi::transfer(const uint8_t* tx_data, uint8_t* rx_data, uint8_t length) {
  if (!_bound || length == 0 || length > 64) {
    return false;
  }

  const uint8_t bus = _bus;

  // Restore what this service was bound with. Skipping this would make the
  // transfer depend on whichever transaction touched the bus last.
  REG_WRITE(SPI_CLOCK_REG(bus), _clock);
  REG_WRITE(SPI_CTRL_REG(bus), _ctrl);
  REG_WRITE(SPI_USER_REG(bus), _user);
  REG_WRITE(SPI_USER1_REG(bus), _user1);
  REG_WRITE(SPI_USER2_REG(bus), _user2);
  REG_WRITE(SPI_PIN_REG(bus), _pin);

  const uint32_t bit_length = (uint32_t)length * 8u - 1u;
  REG_WRITE(SPI_MOSI_DLEN_REG(bus), bit_length);
  REG_WRITE(SPI_MISO_DLEN_REG(bus), bit_length);

  const uint8_t words = (length + 3) / 4;
  for (uint8_t word = 0; word < words; word++) {
    uint32_t value = 0;
    for (uint8_t byte = 0; byte < 4; byte++) {
      const uint8_t index = word * 4 + byte;
      const uint8_t out = (tx_data != nullptr && index < length) ? tx_data[index] : 0xFF;
      value |= (uint32_t)out << (8 * byte);
    }
    REG_WRITE(SPI_W0_REG(bus) + word * 4, value);
  }

  REG_WRITE(_cs_clear_reg, _cs_mask);
  REG_WRITE(SPI_CMD_REG(bus), SPI_USR);
  uint32_t spins = 0;
  bool completed = true;
  while (REG_READ(SPI_CMD_REG(bus)) & SPI_USR) {
    if (++spins > SPI_WAIT_SPINS_MAX) {
      _timeouts = _timeouts + 1;
      completed = false;
      break;
    }
  }
  REG_WRITE(_cs_set_reg, _cs_mask);

  /* A transfer that never finished has receive registers holding whatever was
   * there before, and reading them out would hand the caller bytes the chip
   * never sent. For the drain that is not a lost frame but an INVENTED
   * one: an arbitrary identifier and payload published into the ring and given
   * to a battery or inverter driver as traffic. So say so and copy nothing.
   */
  if (!completed || rx_data == nullptr) {
    return completed;
  }
  for (uint8_t word = 0; word < words; word++) {
    const uint32_t value = REG_READ(SPI_W0_REG(bus) + word * 4);
    for (uint8_t byte = 0; byte < 4; byte++) {
      const uint8_t index = word * 4 + byte;
      if (index < length) {
        rx_data[index] = (uint8_t)(value >> (8 * byte));
      }
    }
  }
  return true;
}

#else  // CONFIG_IDF_TARGET_ESP32

bool Mcp2515IramSpi::bind(uint8_t spi_bus, int8_t cs_pin) {
  (void)spi_bus;
  (void)cs_pin;
  _bound = false;
  return false;
}

bool IRAM_ATTR Mcp2515IramSpi::transfer(const uint8_t* tx_data, uint8_t* rx_data, uint8_t length) {
  (void)tx_data;
  (void)rx_data;
  (void)length;
  return false;
}

#endif  // CONFIG_IDF_TARGET_ESP32
