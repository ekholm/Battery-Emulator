#include <gtest/gtest.h>

#include <cstring>
#include <new>

#include "../Software/src/lib/adafruit-Adafruit_NeoPixel/Adafruit_NeoPixel.h"
#include "Arduino.h"

/* The LilyGo T-2CAN FD board that would not boot.
 *
 * `Adafruit_NeoPixel::setPin()` reads `pin` before it writes it, to release a
 * pad it was previously driving. Upstream guards that read with a `begun` flag
 * initialised to false; commit ce7547f7 ("Optimize Neopixel library for
 * maximum performance") dropped the flag and left the bare `if (pin >= 0)`,
 * while `pin` had no initial value and the constructor's init list did not
 * give it one. A heap-allocated LED therefore started life with whatever bytes
 * were in the block, and handed them to pinMode().
 *
 * Usually harmless - an out-of-range number just logs "Invalid IO 232". On the
 * bench board the leftover byte was 27, which on an ESP32-S3 is
 * MSPI_IOMUX_PIN_NUM_HD, the SPI flash hold line. Reconfiguring it as an input
 * cut the flash bus in the middle of setup(), so the next instruction fetch
 * faulted, the fault handler needed flash it no longer had, and the chip sat in
 * a double exception until the interrupt watchdog reset it - 18 silent
 * TG1WDT_SYS_RST resets with no log line and no coredump.
 *
 * These tests pin both halves: construction must not touch a pad it was never
 * given, and a LATER setPin() must still release the previous one, so the fix
 * cannot be "delete the read".
 */

namespace {

constexpr uint8_t kRequestedPin = 4;
constexpr uint8_t kSecondPin = 5;
// 27 == MSPI_IOMUX_PIN_NUM_HD on the ESP32-S3: the exact value recovered from
// the failing board's heap. Poisoning with this byte reproduces the device
// condition deterministically instead of hoping the allocator hands back dirt.
constexpr unsigned char kPoison = 27;

}  // namespace

TEST(NeoPixelPinInit, ConstructionNeverConfiguresAPadItWasNotGiven) {
  alignas(Adafruit_NeoPixel) unsigned char storage[sizeof(Adafruit_NeoPixel)];
  memset(storage, kPoison, sizeof(storage));

  pinmode_calls().clear();
  Adafruit_NeoPixel* pixels = new (storage) Adafruit_NeoPixel(kRequestedPin, 1);

  EXPECT_FALSE(pinmode_calls().empty()) << "construction should still configure the pin it was given";
  for (const auto& call : pinmode_calls()) {
    EXPECT_EQ(call.pin, kRequestedPin) << "construction configured pad " << (int)call.pin
                                       << ", which it was never given - an uninitialised member leaked into pinMode()";
  }

  pixels->~Adafruit_NeoPixel();
}

TEST(NeoPixelPinInit, SetPinStillReleasesThePreviousPad) {
  // The read of `pin` exists for a reason: moving the output must stop driving
  // the old pad. Deleting the read would silence the test above and break this
  // one, which is the point of having both.
  Adafruit_NeoPixel pixels(kRequestedPin, 1);

  pinmode_calls().clear();
  pixels.setPin(kSecondPin);

  bool released_old = false;
  bool drove_new = false;
  for (const auto& call : pinmode_calls()) {
    if (call.pin == kRequestedPin && call.mode == INPUT) {
      released_old = true;
    }
    if (call.pin == kSecondPin && call.mode == OUTPUT) {
      drove_new = true;
    }
  }
  EXPECT_TRUE(released_old) << "moving the output left the previous pad still driven";
  EXPECT_TRUE(drove_new) << "moving the output never configured the new pad";
}
