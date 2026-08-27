#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/devboard/utils/events.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../../Software/src/inverter/PYLON-CAN.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the Pylontech HV CAN inverter driver (PYLON-CAN).
//
// TX side: data is assembled in update_values() and dispatched only when the
// inverter polls via 0x4200.  No periodic TX exists — transmit_can() is a
// no-op.  RX side: only 0x4200 refreshes aliveness; data[0]==0x00 triggers
// system-data burst, data[0]==0x02 triggers setup-info burst.

namespace {

class PylonCanInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // DataLayerResetListener has already wiped datalayer/events and deleted the
    // previous inverter instance.
    user_selected_pylon_send = 0;
    user_selected_pylon_30koffset = false;
    user_selected_pylon_invert_byteorder = false;
    user_selected_inverter_pylon_type = 0;
    user_selected_inverter_cells = 0;
    user_selected_inverter_modules = 0;
    user_selected_inverter_cells_per_module = 0;
    user_selected_inverter_voltage_level = 0;
    user_selected_inverter_ah_capacity = 0;
    user_selected_inverter_protocol = InverterProtocolType::Pylon;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    pylon = static_cast<PylonInverter*>(inverter);
    clear_transmitted_frames();
  }

  // Trigger the system-data burst (421X..429X).
  void request_system_data() {
    CAN_frame req = {.FD = false, .ext_ID = true, .DLC = 8, .ID = 0x4200, .data = {0x00, 0, 0, 0, 0, 0, 0, 0}};
    pylon->map_can_frame_to_variable(req);
  }

  // Trigger the setup-info burst (731X..734X).
  void request_setup_info() {
    CAN_frame req = {.FD = false, .ext_ID = true, .DLC = 8, .ID = 0x4200, .data = {0x02, 0, 0, 0, 0, 0, 0, 0}};
    pylon->map_can_frame_to_variable(req);
  }

  PylonInverter* pylon = nullptr;
};

}  // namespace

// ---- Periodic TX gate -------------------------------------------------------

TEST_F(PylonCanInverterTest, TransmitCanIsNoOp) {
  // transmit_can() must not emit any frame — all TX is RX-triggered.
  pylon->update_values();
  pylon->transmit_can(INTERVAL_60_S + 1);
  EXPECT_TRUE(get_transmitted_frames().empty()) << "PYLON-CAN must not transmit periodically; all TX is poll-driven";
}

// ---- RX aliveness -----------------------------------------------------------

TEST_F(PylonCanInverterTest, Poll0x4200RefreshesAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = true, .DLC = 8, .ID = 0x4200, .data = {0x00, 0, 0, 0, 0, 0, 0, 0}};
  pylon->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE);
}

TEST_F(PylonCanInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = true, .DLC = 8, .ID = 0x4210, .data = {0, 0, 0, 0, 0, 0, 0, 0}};
  pylon->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0);
}

// ---- RX dispatch: system data vs setup info ---------------------------------

TEST_F(PylonCanInverterTest, Poll0x4200Data00SendsSystemDataFrames) {
  pylon->update_values();
  request_system_data();
  // Nine system data frames: 421X..429X (all extended IDs)
  EXPECT_NE(find_frame_with_id(0x4210), nullptr);
  EXPECT_NE(find_frame_with_id(0x4220), nullptr);
  EXPECT_NE(find_frame_with_id(0x4230), nullptr);
  EXPECT_NE(find_frame_with_id(0x4240), nullptr);
  EXPECT_NE(find_frame_with_id(0x4250), nullptr);
  EXPECT_NE(find_frame_with_id(0x4260), nullptr);
  EXPECT_NE(find_frame_with_id(0x4270), nullptr);
  EXPECT_NE(find_frame_with_id(0x4280), nullptr);
  EXPECT_NE(find_frame_with_id(0x4290), nullptr);
  // No setup-info frames
  EXPECT_EQ(find_frame_with_id(0x7310), nullptr);
}

TEST_F(PylonCanInverterTest, Poll0x4200Data02SendsSetupInfoFrames) {
  request_setup_info();
  EXPECT_NE(find_frame_with_id(0x7310), nullptr);
  EXPECT_NE(find_frame_with_id(0x7320), nullptr);
  EXPECT_NE(find_frame_with_id(0x7330), nullptr);
  EXPECT_NE(find_frame_with_id(0x7340), nullptr);
  // No system data frames
  EXPECT_EQ(find_frame_with_id(0x4210), nullptr);
}

// ---- Payload: 421X (voltage, current, temp, SOC, SOH) ----------------------

TEST_F(PylonCanInverterTest, Frame421XEncodesVoltageCurrentTempSocSoh) {
  datalayer.battery.status.voltage_dV = 3700;
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-810);  // charging: -81.0 A
  datalayer.battery.status.temperature_max_dC = 250;                          // 25.0 °C → stored as 250 + 1000 = 1250
  datalayer.battery.status.reported_soc = 7550;                               // 75.50 % → integer: 75
  datalayer.battery.status.soh_pptt = 9900;                                   // 99.00 % → integer: 99

  pylon->update_values();
  request_system_data();

  const CAN_frame* f = find_frame_with_id(0x4210);
  ASSERT_NE(f, nullptr);
  // Voltage 3700 dV big-endian: 0x0E, 0x74
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 3700u);
  // Current -810 (int16) big-endian: 0xFC, 0xD6
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[2], f->data.u8[3])), -810);
  // BMS temp = 250 + 1000 = 1250 big-endian: 0x04, 0xE2
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 1250u);
  // SOC = 7550 / 100 = 75 in byte 6
  EXPECT_EQ(f->data.u8[6], 75u);
  // SOH = 9900 / 100 = 99 in byte 7
  EXPECT_EQ(f->data.u8[7], 99u);
}

// ---- Payload: 422X (charge/discharge voltages and currents) -----------------

TEST_F(PylonCanInverterTest, Frame422XEncodesDesignVoltageLimitsWithOffset) {
  datalayer.battery.info.max_design_voltage_dV = 4000;
  datalayer.battery.info.min_design_voltage_dV = 3000;
  datalayer.battery.status.max_charge_current_dA = 250;
  datalayer.battery.status.max_discharge_current_dA = 300;
  datalayer.battery.settings.user_set_voltage_limits_active = false;

  pylon->update_values();
  request_system_data();

  const CAN_frame* f = find_frame_with_id(0x4220);
  ASSERT_NE(f, nullptr);
  // Charge cutoff = max_design - 20 = 3980
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 3980u);
  // Discharge cutoff = min_design + 20 = 3020
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 3020u);
  // Max charge current
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 250u);
  // Max discharge current
  EXPECT_EQ(u16_be(f->data.u8[6], f->data.u8[7]), 300u);
}

TEST_F(PylonCanInverterTest, Frame422XHonoursUserVoltageLimits) {
  datalayer.battery.settings.user_set_voltage_limits_active = true;
  datalayer.battery.settings.max_user_set_charge_voltage_dV = 3900;
  datalayer.battery.settings.max_user_set_discharge_voltage_dV = 3100;
  datalayer.battery.info.max_design_voltage_dV = 4000;
  datalayer.battery.info.min_design_voltage_dV = 3000;

  pylon->update_values();
  request_system_data();

  const CAN_frame* f = find_frame_with_id(0x4220);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 3900u);
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 3100u);
}

// ---- Payload: 423X (cell voltages) -----------------------------------------

TEST_F(PylonCanInverterTest, Frame423XEncodesCellMinMaxVoltage) {
  datalayer.battery.status.cell_max_voltage_mV = 4100;
  datalayer.battery.status.cell_min_voltage_mV = 3700;

  pylon->update_values();
  request_system_data();

  const CAN_frame* f = find_frame_with_id(0x4230);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 4100u);
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 3700u);
}

// ---- Payload: 424X (per-cell temperatures with +1000 offset) ----------------

TEST_F(PylonCanInverterTest, Frame424XEncodesCellTemperaturesWithOffset) {
  datalayer.battery.status.temperature_max_dC = 300;  // 300 + 1000 = 1300 = 0x0514
  datalayer.battery.status.temperature_min_dC = 100;  // 100 + 1000 = 1100 = 0x044C

  pylon->update_values();
  request_system_data();

  const CAN_frame* f = find_frame_with_id(0x4240);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 1300u);
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 1100u);
}

// ---- Payload: 425X (status byte: sleep/charge/discharge/idle) ---------------

TEST_F(PylonCanInverterTest, StatusByteReflectsCurrentDirection) {
  pylon->update_values();  // current == 0 → Idle
  request_system_data();
  const CAN_frame* f = find_frame_with_id(0x4250);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x03u);  // Idle

  clear_transmitted_frames();
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-100);  // charging
  pylon->update_values();
  request_system_data();
  f = find_frame_with_id(0x4250);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x01u);  // Charge

  clear_transmitted_frames();
  datalayer.battery.status.reported_current_dA = 100;  // discharging
  pylon->update_values();
  request_system_data();
  f = find_frame_with_id(0x4250);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x02u);  // Discharge

  clear_transmitted_frames();
  datalayer.system.status.system_status = FAULT;
  pylon->update_values();
  request_system_data();
  f = find_frame_with_id(0x4250);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x00u);  // Sleep on FAULT
}

// ---- Payload: 428X (charge/discharge permission flags) ----------------------

TEST_F(PylonCanInverterTest, Frame428XForbidsChargeWhenCurrentIsZero) {
  datalayer.battery.status.max_charge_current_dA = 0;
  datalayer.battery.status.max_discharge_current_dA = 100;

  pylon->update_values();
  request_system_data();

  const CAN_frame* f = find_frame_with_id(0x4280);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0xAAu);  // charge forbidden
  EXPECT_EQ(f->data.u8[1], 0x00u);  // discharge allowed
}

TEST_F(PylonCanInverterTest, Frame428XForbidsBothOnFault) {
  datalayer.battery.status.max_charge_current_dA = 100;
  datalayer.battery.status.max_discharge_current_dA = 100;
  datalayer.system.status.system_status = FAULT;

  pylon->update_values();
  request_system_data();

  const CAN_frame* f = find_frame_with_id(0x4280);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0xAAu);  // charge forbidden (FAULT overrides)
  EXPECT_EQ(f->data.u8[1], 0xAAu);  // discharge forbidden
}

// ---- User option: 30k offset applied to current and limits ------------------

TEST_F(PylonCanInverterTest, Pylon30kOffsetShiftsCurrentAndLimits) {
  user_selected_pylon_30koffset = true;
  datalayer.battery.status.reported_current_dA = 100;       // 100 → 100 + 30000 = 30100 = 0x75D4
  datalayer.battery.status.max_charge_current_dA = 250;     // 250 + 30000 = 30250 = 0x762A
  datalayer.battery.status.max_discharge_current_dA = 200;  // 200 + 30000 = 30200 = 0x75F8

  pylon->update_values();
  request_system_data();

  const CAN_frame* f421 = find_frame_with_id(0x4210);
  ASSERT_NE(f421, nullptr);
  EXPECT_EQ(u16_be(f421->data.u8[2], f421->data.u8[3]), 30100u);  // current + 30k

  const CAN_frame* f422 = find_frame_with_id(0x4220);
  ASSERT_NE(f422, nullptr);
  EXPECT_EQ(u16_be(f422->data.u8[4], f422->data.u8[5]), 30250u);  // charge limit + 30k
  EXPECT_EQ(u16_be(f422->data.u8[6], f422->data.u8[7]), 30200u);  // discharge limit + 30k

  user_selected_pylon_30koffset = false;
}

// ---- User option: invert byte order -----------------------------------------

TEST_F(PylonCanInverterTest, PylonInvertByteorderSwapsWordBytes) {
  user_selected_pylon_invert_byteorder = true;
  datalayer.battery.status.voltage_dV = 3700;  // BE: 0x0E, 0x74 → swapped: 0x74, 0x0E

  pylon->update_values();
  request_system_data();

  const CAN_frame* f = find_frame_with_id(0x4210);
  ASSERT_NE(f, nullptr);
  // After swap the LE interpretation equals the original BE value
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 3700u);
  // While the BE interpretation is different
  EXPECT_NE(u16_be(f->data.u8[0], f->data.u8[1]), 3700u);

  user_selected_pylon_invert_byteorder = false;
}

// ---- User option: pylon_send = 1 shifts all frame IDs by 1 -----------------

TEST_F(PylonCanInverterTest, PylonSend1UsesOddFrameIds) {
  // Rebuild with pylon_send=1 so setup() applies the ID shift.
  user_selected_pylon_send = 1;
  delete inverter;
  inverter = nullptr;
  setup_inverter();
  pylon = static_cast<PylonInverter*>(inverter);
  clear_transmitted_frames();

  pylon->update_values();
  // System data requested via the same poll mechanism
  CAN_frame req = {.FD = false, .ext_ID = true, .DLC = 8, .ID = 0x4200, .data = {0x00, 0, 0, 0, 0, 0, 0, 0}};
  pylon->map_can_frame_to_variable(req);

  EXPECT_NE(find_frame_with_id(0x4211), nullptr);
  EXPECT_NE(find_frame_with_id(0x4221), nullptr);
  EXPECT_EQ(find_frame_with_id(0x4210), nullptr);

  CAN_frame setup_req = {.FD = false, .ext_ID = true, .DLC = 8, .ID = 0x4200, .data = {0x02, 0, 0, 0, 0, 0, 0, 0}};
  pylon->map_can_frame_to_variable(setup_req);
  EXPECT_NE(find_frame_with_id(0x7311), nullptr);
  EXPECT_EQ(find_frame_with_id(0x7310), nullptr);

  // Reset for subsequent tests
  user_selected_pylon_send = 0;
}

// ---- User option: pylon_type controls manufacturer name in 733X -------------

TEST_F(PylonCanInverterTest, PylonType0SetsPylontechManufacturerName) {
  // Default is type 0 = "PYLONTEC H"
  request_setup_info();
  const CAN_frame* f = find_frame_with_id(0x7330);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 'P');
  EXPECT_EQ(f->data.u8[1], 'Y');
  EXPECT_EQ(f->data.u8[4], 'N');
}

TEST_F(PylonCanInverterTest, PylonType2SetsDeyeManufacturerName) {
  user_selected_inverter_pylon_type = 2;
  delete inverter;
  inverter = nullptr;
  setup_inverter();
  pylon = static_cast<PylonInverter*>(inverter);
  clear_transmitted_frames();

  request_setup_info();
  const CAN_frame* f = find_frame_with_id(0x7330);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 'D');
  EXPECT_EQ(f->data.u8[1], 'e');
  EXPECT_EQ(f->data.u8[2], 'y');
  EXPECT_EQ(f->data.u8[3], 'e');

  user_selected_inverter_pylon_type = 0;
}

// ---- User option: user_selected_inverter_cells overrides cell count ---------

TEST_F(PylonCanInverterTest, UserSelectedCellsOverridesDefaultInSetup) {
  user_selected_inverter_cells = 200;
  delete inverter;
  inverter = nullptr;
  setup_inverter();
  pylon = static_cast<PylonInverter*>(inverter);
  clear_transmitted_frames();

  request_setup_info();
  const CAN_frame* f = find_frame_with_id(0x7320);
  ASSERT_NE(f, nullptr);
  // Cell count LE in bytes 0-1
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 200u);

  user_selected_inverter_cells = 0;
}
