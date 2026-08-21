#include <gtest/gtest.h>

#include "../Software/src/battery/BATTERIES.h"
#include "../Software/src/battery/CHADEMO-SHUNTS.h"
#include "../Software/src/battery/CanBattery.h"
#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/datalayer/datalayer_extended.h"
#include "../Software/src/devboard/hal/hal.h"
#include "../Software/src/devboard/utils/events.h"

/* The CHAdeMO restoration pins: the two silent regressions that made the
 * driver unable to hold a session (chademo-regression-analysis.md, B and D).
 * Each test names the failure it guards against; if one starts failing after
 * a refactor, the answer is to restore the behavior, not adjust the test.
 */

namespace {

CAN_frame isa_frame(uint32_t id, long value) {
  CAN_frame f{};
  f.ID = id;
  f.DLC = 8;
  f.data.u8[2] = (value >> 24) & 0xFF;
  f.data.u8[3] = (value >> 16) & 0xFF;
  f.data.u8[4] = (value >> 8) & 0xFF;
  f.data.u8[5] = value & 0xFF;
  return f;
}

class ChademoRestorationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    user_selected_battery_type = BatteryType::Chademo;
    user_selected_shunt_type = ShuntType::None;  // anything but CustomClamp uses the ISA path
    setup_battery();
    ASSERT_NE(battery, nullptr);
    chademo = static_cast<CanBattery*>(battery);
  }

  CanBattery* chademo = nullptr;
};

// Regression B (2fe64690): the ISA shunt's frames were routed behind the
// CHADEMO ID filter and never decoded, so voltage and current sat at zero -
// which made the POWERFLOW stop condition permanently true and ended the
// A.7.2.9 current-drop wait instantly. The shunt frames must reach the
// decoder even though their IDs are outside the CHADEMO range.
TEST_F(ChademoRestorationTest, IsaShuntFramesReachTheDecoder) {
  chademo->handle_incoming_can_frame(isa_frame(0x522, 370123));  // millivolts
  EXPECT_FLOAT_EQ(get_measured_voltage(), 370.123f) << "the ISA voltage frame never reached ISA_handleFrame";

  chademo->handle_incoming_can_frame(isa_frame(0x521, 12500));  // milliamps
  EXPECT_FLOAT_EQ(get_measured_current(), 12.5f) << "the ISA current frame never reached ISA_handleFrame";
}

// The shunt talks CAN too, but it is not the vehicle: its frames must not
// count as vehicle liveness, or a plugged-out session with a chatty shunt
// would never notice the vehicle going silent.
TEST_F(ChademoRestorationTest, ShuntFramesDoNotCountAsVehicleLiveness) {
  datalayer.battery.status.CAN_battery_still_alive = 0;
  chademo->handle_incoming_can_frame(isa_frame(0x522, 370000));
  EXPECT_EQ(datalayer.battery.status.CAN_battery_still_alive, 0);
}

// The routing range must match ISA_handleFrame's own guard exactly
// (0x510..0x528, CHADEMO-SHUNTS.cpp). An off-by-one in the router either
// starves the decoder of an endpoint frame or, worse, feeds a neighbouring
// ID into it as vehicle traffic. Both endpoints decode; both neighbours are
// ignored by the shunt path and do not disturb the measurement.
TEST_F(ChademoRestorationTest, IsaRoutingRangeMatchesTheDecoderExactly) {
  // In-range endpoints reach the decoder: 0x522 is voltage (proven above);
  // 0x521 is current in mA, 0x528 is energy - use current for the low check.
  chademo->handle_incoming_can_frame(isa_frame(0x521, 12500));  // 12.5 A
  EXPECT_FLOAT_EQ(get_measured_current(), 12.5f) << "0x521 (low neighbourhood) must reach ISA_handleFrame";

  chademo->handle_incoming_can_frame(isa_frame(0x522, 370000));
  EXPECT_FLOAT_EQ(get_measured_voltage(), 370.0f);

  // One past each end: not shunt frames. They must not change measurements,
  // and (not being vehicle IDs either) must not feed liveness.
  datalayer.battery.status.CAN_battery_still_alive = 0;
  chademo->handle_incoming_can_frame(isa_frame(0x50F, 999999));
  chademo->handle_incoming_can_frame(isa_frame(0x529, 999999));
  EXPECT_FLOAT_EQ(get_measured_voltage(), 370.0f) << "out-of-range IDs disturbed the shunt measurement";
  EXPECT_FLOAT_EQ(get_measured_current(), 12.5f);
  EXPECT_EQ(datalayer.battery.status.CAN_battery_still_alive, 0)
      << "a non-vehicle, non-shunt ID must not count as vehicle liveness";
}

// Regression D: update_values() used to write the liveness counter
// unconditionally ("Always write the CAN as alive!"), which defeated both the
// battery-missing alarm and the battery_detected contactor guard - the system
// considered a vehicle present within a second of boot, always. The contract:
// quiet while IDLE (an unplugged EVSE is the normal state), but once a
// session is underway only real vehicle frames may feed the counter.
TEST_F(ChademoRestorationTest, LivenessComesFromVehicleFramesOnceOffIdle) {
  // Idle and unplugged: the tick keeps the watchdog quiet.
  datalayer.battery.status.CAN_battery_still_alive = 0;
  battery->update_values();
  EXPECT_EQ(datalayer.battery.status.CAN_battery_still_alive, CAN_STILL_ALIVE)
      << "an unplugged idle EVSE must not raise the battery-missing alarm";

  // Leave IDLE through the public user-stop path.
  datalayer_extended.chademo.UserRequestStop = true;
  battery->update_values();

  datalayer.battery.status.CAN_battery_still_alive = 0;
  battery->update_values();
  EXPECT_EQ(datalayer.battery.status.CAN_battery_still_alive, 0)
      << "off IDLE, the timer tick must not fake vehicle liveness";

  // A real vehicle frame is what liveness means.
  CAN_frame vehicle{};
  vehicle.ID = 0x100;
  vehicle.DLC = 8;
  chademo->handle_incoming_can_frame(vehicle);
  EXPECT_EQ(datalayer.battery.status.CAN_battery_still_alive, CAN_STILL_ALIVE)
      << "a vehicle frame must refresh the liveness counter";
}

}  // namespace
