#include <gtest/gtest.h>

#include "../../Software/src/battery/CHARGEBYTE-CCS.h"
#include "../../Software/src/battery/KIA-E-GMP-BATTERY.h"
#include "../../Software/src/battery/KIA-HYUNDAI-HYBRID-BATTERY.h"
#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/utils/events.h"

#include "Arduino.h"

// wq310: safety events that existed in the code but could not fire at runtime.
// Each test failed against the defect and pins the repaired path.

namespace {

CAN_frame frame_with(uint32_t id, std::initializer_list<uint8_t> bytes) {
  CAN_frame frame = {};
  frame.DLC = 8;
  frame.ID = id;
  uint8_t i = 0;
  for (uint8_t b : bytes) {
    if (i >= 8) {
      break;
    }
    frame.data.u8[i++] = b;
  }
  return frame;
}

}  // namespace

// The interlock decode read `(bool)(u8[1] & 0x02) >> 1`: the cast bound before
// the shift, so the whole expression was 0 on every frame and the HVIL event
// was unreachable via 0x5AE.
TEST(KiaHyundaiHybridSafetyTest, AMissingInterlockOn5AERaisesHvilFailure) {
  KiaHyundaiHybridBattery battery;

  battery.handle_incoming_can_frame(frame_with(0x5AE, {0x00, 0x02}));
  battery.update_values();

  EXPECT_EQ(get_event_pointer(EVENT_HVIL_FAILURE)->state, EVENT_STATE_ACTIVE);
}

TEST(KiaHyundaiHybridSafetyTest, APresentInterlockClearsHvilFailure) {
  KiaHyundaiHybridBattery battery;

  battery.handle_incoming_can_frame(frame_with(0x5AE, {0x00, 0x02}));
  battery.update_values();
  ASSERT_EQ(get_event_pointer(EVENT_HVIL_FAILURE)->state, EVENT_STATE_ACTIVE);

  battery.handle_incoming_can_frame(frame_with(0x5AE, {0x00, 0x00}));
  battery.update_values();

  EXPECT_NE(get_event_pointer(EVENT_HVIL_FAILURE)->state, EVENT_STATE_ACTIVE);
}

// The status ladder put inCharge before the error flags, so an error raised
// while charging reported BMS_ACTIVE - BMS_FAULT was unreachable in exactly
// the state where a fault matters most.
TEST(ChargebyteCcsSafetyTest, AnErrorWhileChargingReportsFaultNotActive) {
  ChargebyteCCSBattery battery;

  // 0x100: state nibble 6 = inCharge, error nibble (u8[2] >> 4) >= 3.
  battery.handle_incoming_can_frame(frame_with(0x100, {0x00, 0x06, 0x30}));
  battery.update_values();

  EXPECT_EQ(datalayer.battery.status.real_bms_status, BMS_FAULT);
}

TEST(ChargebyteCcsSafetyTest, ChargingWithoutErrorStillReportsActive) {
  ChargebyteCCSBattery battery;

  battery.handle_incoming_can_frame(frame_with(0x100, {0x00, 0x06, 0x00}));
  battery.update_values();

  EXPECT_EQ(datalayer.battery.status.real_bms_status, BMS_ACTIVE);
}

// No E-GMP RX path ever assigned the water-leakage member (unlike the KIA-64
// sibling's 0x5D5 decode), so the ingress check compared a constant and the
// page rendered that constant as if it were a reading. The member is gone;
// this pins that the misleading line stays gone until a real decode exists.
TEST(KiaEGmpSafetyTest, TheStatusPageNoLongerRendersTheUndecodedWaterSensor) {
  KiaEGmpBattery battery;

  std::string content = battery.get_status_renderer().get_status_html().str();

  EXPECT_EQ(content.find("Waterleakage"), std::string::npos);
  EXPECT_NE(content.find("12V voltage"), std::string::npos);  // the page itself still renders
}
