#include <gtest/gtest.h>

#include <fstream>
#include <string>

/* The TWAI interrupt must stay alive through flash windows.
 *
 * The pin is two halves and both live in the vendored library's source: the
 * interrupt SOURCE is allocated with ESP_INTR_FLAG_IRAM (without it,
 * esp_intr_noniram_disable() masks the source for the whole of every NVS
 * commit and OTA chunk, and the two hardware receive slots overflow
 * silently), and everything the ISR reaches carries IRAM_ATTR (with the flag
 * but without residency, a masked interrupt becomes a cache-off fetch - a
 * crash instead of a loss). Where the compiler actually EMITS a header
 * function only the linked image can show - that is
 * scripts/acan_esp32_isr_iram_audit.py's job, run per image with its
 * positive control. This file pins the source half so neither line can be
 * reverted without a test noticing, the same shape the MCP2515 drain uses.
 */
namespace {

std::string source(const char* relative) {
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  const std::string path = dir + "/../" + relative;
  std::ifstream src(path);
  EXPECT_TRUE(src.is_open()) << "this test reads " << path;
  return std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(NativeCanIram, TheTwaiSourceIsAllocatedWithTheFlagThatSurvivesAFlashWrite) {
  const std::string src = source("Software/src/lib/pierremolinaro-acan-esp32/ACAN_ESP32.cpp");
  EXPECT_NE(src.find("esp_intr_alloc (twaiInterruptSource, ESP_INTR_FLAG_IRAM, isr, this"), std::string::npos)
      << "the TWAI source is allocated without ESP_INTR_FLAG_IRAM - the interrupt is then masked for "
         "exactly the windows this pinning exists to cover";
}

TEST(NativeCanIram, EverythingTheIsrReachesCarriesIramAttrAtTheSource) {
  const std::string driver = source("Software/src/lib/pierremolinaro-acan-esp32/ACAN_ESP32.cpp");
  const std::string buffer = source("Software/src/lib/pierremolinaro-acan-esp32/ACAN_ESP32_Buffer16.h");

  // The ISR and the five functions it reaches in the driver. The overrun
  // handler joined the chain with the RX data-overrun recovery, and the receive
  // read then started returning whether it took a real frame.
  for (const char* fn :
       {"void IRAM_ATTR ACAN_ESP32::isr (void * inUserArgument)", "void IRAM_ATTR ACAN_ESP32::handleRXInterrupt (void)",
        "void IRAM_ATTR ACAN_ESP32::handleTXInterrupt (void)",
        "void IRAM_ATTR ACAN_ESP32::handleOverrunInterrupt (void)",
        "bool IRAM_ATTR ACAN_ESP32::getReceivedMessage (CANMessage & outFrame)",
        "void IRAM_ATTR ACAN_ESP32::internalSendMessage (const CANMessage & inFrame)"}) {
    EXPECT_NE(driver.find(fn), std::string::npos) << fn << " lost its IRAM_ATTR";
  }

  // The in-header ring the handlers call. -Os has been observed emitting an in-header
  // function OUT-OF-LINE into .flash.text from identical source; the attribute
  // on the in-class definition is what makes such a copy land in IRAM.
  EXPECT_NE(buffer.find("bool IRAM_ATTR append (const CANMessage & inMessage)"), std::string::npos)
      << "Buffer16::append lost its IRAM_ATTR - an out-of-line copy would land in .flash.text";
  EXPECT_NE(buffer.find("bool IRAM_ATTR remove (CANMessage & outMessage)"), std::string::npos)
      << "Buffer16::remove lost its IRAM_ATTR - an out-of-line copy would land in .flash.text";
}
