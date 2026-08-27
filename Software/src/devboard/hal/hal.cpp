#include "hal.h"

#include <Arduino.h>

Esp32Hal* esp32hal = nullptr;

void init_hal() {
#if defined(HW_LILYGO)
#include "hw_lilygo.h"
  esp32hal = new LilyGoHal();
#elif defined(HW_LILYGO2CAN)
#include "hw_lilygo2can.h"
  esp32hal = new LilyGo2CANHal();
#elif defined(HW_STARK)
#include "hw_stark.h"
  esp32hal = new StarkHal();
#elif defined(HW_3LB)
#include "hw_3LB.h"
  esp32hal = new ThreeLBHal();
#elif defined(HW_BECOM)
#include "hw_becom.h"
  esp32hal = new BEComHal();
#elif defined(HW_WAVESHARE)
#include "hw_waveshare.h"
  esp32hal = new WaveshareS3Rs485CanHal();
#elif defined(HW_DEVKIT)
#include "hw_devkit.h"
  esp32hal = new DevKitHal();
#elif defined(HW_DFROBOT_EDGE101)
#include "hw_dfrobot_edge101.h"
  esp32hal = new DFRobotEdge101Hal();
#else
#error "No HW defined."
#endif
}

bool Esp32Hal::system_booted_up() {
  return milliseconds(millis()) > BOOTUP_TIME();
}

/* See the contract on claim_spi_bus() in hal.h. The comparison is on the three
 * routed pins and not on the chip select, because CS is exactly the thing that
 * is allowed to differ: several devices sharing one bus with their own selects
 * is how SPI is meant to be used, and it is only the sck/miso/mosi triple that
 * the controller can hold one of. */
void Esp32Hal::claim_spi_bus(const char* name, uint8_t bus, gpio_num_t sck, gpio_num_t miso, gpio_num_t mosi) {
  if (sck == GPIO_NUM_NC && miso == GPIO_NUM_NC && mosi == GPIO_NUM_NC) {
    return;  // Says nothing about routing, so there is nothing to conflict over.
  }

  auto it = claimed_spi_buses.find(bus);
  if (it == claimed_spi_buses.end()) {
    claimed_spi_buses[bus] = {name, sck, miso, mosi};
    return;
  }

  const SpiBusClaim& held = it->second;
  if (held.sck == sck && held.miso == miso && held.mosi == mosi) {
    return;  // Same wiring, so the second device is just another chip select.
  }

  // First claim stays recorded: it names the device that is about to lose the
  // bus, which is the useful half of the message.
  spi_conflict_claimant = name;
  spi_conflict_holder = held.name.c_str();
  set_event(EVENT_SPI_BUS_CONFLICT, bus);  // also printing a log entry
}
