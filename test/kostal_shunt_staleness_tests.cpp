#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "Arduino.h"  // set_millis64() for driving the RS485 state machine's clock

#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/inverter/KOSTAL-RS485.h"
#include "../Software/src/shunt/Shunt.h"

/* A BMW S-BOX that goes quiet must stop deciding what the Kostal inverter is told.
 *
 * `datalayer.shunt.available` is cleared 1000 ms after the S-BOX's last frame, but nothing read it,
 * so the cyclic frame kept carrying the last measured current for as long as the outage lasted.
 *
 * These drive the real request/response path rather than calling update_values() and peeking at the
 * buffer: what matters is the bytes that leave the port, and the two current fields sit at fixed
 * offsets inside a frame that is CRC'd and null-stuffed after update_values() has filled it.
 */

namespace {

// The protocol's own framing, restated here so the test speaks it rather than borrowing the
// implementation it is checking.
uint8_t kostal_crc(const uint8_t* frame, int len) {
  unsigned int sum = 0;
  for (int i = 1; i < len; i++) {
    sum += frame[i];
  }
  return (uint8_t)(-sum & 0xff);
}

void null_stuff(uint8_t* frame, int len) {
  int last_null_byte = 0;
  for (int i = 0; i < len; i++) {
    if (frame[i] == 0x00) {
      frame[last_null_byte] = (uint8_t)(i - last_null_byte);
      last_null_byte = i;
    }
  }
}

// A 10-byte request of the shape receive() expects: a leading zero pointer, the 'b' class byte,
// the frame header, the two-byte function code, then the CRC and the terminating zero.
std::vector<uint8_t> make_request(uint16_t code) {
  std::vector<uint8_t> f(10, 0x00);
  f[1] = 'b';
  f[2] = 0xFF;
  f[3] = 0x02;
  f[4] = 0xFF;
  f[5] = 0x29;
  f[6] = (uint8_t)(code & 0xff);
  f[7] = (uint8_t)(code >> 8);
  f[8] = kostal_crc(f.data(), 8);
  f[9] = 0x00;
  null_stuff(f.data(), 10);
  return f;
}

float frame_float_at(const std::vector<uint8_t>& frame, size_t offset) {
  float value = 0.0f;
  std::memcpy(&value, frame.data() + offset, sizeof(value));
  return value;
}

// Undo the null stuffing the firmware applies just before sending, so the payload can be read at
// the offsets update_values() wrote to.
std::vector<uint8_t> unstuff(const std::vector<uint8_t>& frame) {
  std::vector<uint8_t> out = frame;
  size_t at = out.empty() ? 0 : out[0];
  size_t last = 0;
  while (at < out.size() && at != last) {
    const size_t next = at + out[at];
    out[at] = 0x00;
    last = at;
    at = next;
  }
  return out;
}

class KostalShuntTest : public testing::Test {
 protected:
  void SetUp() override {
    Serial2.reset();
    // Not 0: receive() latches contactorMillis while it is still zero, so at time zero it relatches
    // every call and the 2 s RX gate below never opens.
    set_millis64(1000);
    user_selected_shunt_type = ShuntType::BmwSbox;

    // A plausible pack, so the fields the frame carries beside the current are not all zero.
    datalayer.battery.info.max_design_voltage_dV = 4000;
    datalayer.battery.info.min_design_voltage_dV = 3000;
    datalayer.battery.status.voltage_dV = 3500;
    datalayer.battery.status.reported_soc = 5000;

    inverter_ = new KostalInverterProtocol();

    // RX is refused until 2 s after the battery first allows contactor closing.
    datalayer.system.status.battery_allows_contactor_closing = true;
    inverter_->receive();
    set_millis64(4000);
    inverter_->receive();

    // Arms register_content_ok, then the battery-info request sets info_sent - both are
    // preconditions for the cyclic-data request being answered at all.
    inverter_->update_values();
    play(make_request(0x84a));
    ASSERT_FALSE(Serial2.sent().empty()) << "the battery-info request was not answered";
    Serial2.clear_sent();
  }

  void TearDown() override {
    delete inverter_;
    inverter_ = nullptr;
    user_selected_shunt_type = ShuntType::None;
  }

  // receive() consumes one byte per call.
  void play(const std::vector<uint8_t>& frame) {
    Serial2.feed_rx(frame.data(), frame.size());
    for (size_t i = 0; i < frame.size(); i++) {
      inverter_->receive();
    }
  }

  // The cyclic frame the inverter would see, null-stuffing undone.
  std::vector<uint8_t> poll_cyclic_frame() {
    Serial2.clear_sent();
    play(make_request(0x44a));
    const std::vector<uint8_t> sent = Serial2.sent();
    EXPECT_EQ(sent.size(), 64u) << "the cyclic-data request must be answered with a 64-byte frame";
    return unstuff(sent);
  }

  static constexpr size_t kInstantCurrentOffset = 18;
  static constexpr size_t kAverageCurrentOffset = 22;

  KostalInverterProtocol* inverter_ = nullptr;
};

}  // namespace

TEST_F(KostalShuntTest, AShuntThatIsSeenDecidesTheCurrentTheInverterIsTold) {
  datalayer.shunt.available = true;
  datalayer.shunt.measured_amperage_mA = 12300;       // 12.3 A
  datalayer.shunt.measured_avg1S_amperage_mA = 9800;  // 9.8 A
  datalayer.battery.status.reported_current_dA = -500;

  const std::vector<uint8_t> frame = poll_cyclic_frame();

  EXPECT_FLOAT_EQ(frame_float_at(frame, kInstantCurrentOffset), 12.3f);
  EXPECT_FLOAT_EQ(frame_float_at(frame, kAverageCurrentOffset), 9.8f);
}

TEST_F(KostalShuntTest, AShuntThatHasGoneQuietStopsBeingQuotedToTheInverter) {
  datalayer.shunt.available = false;
  datalayer.shunt.measured_amperage_mA = 12300;  // the last reading before it went silent
  datalayer.shunt.measured_avg1S_amperage_mA = 9800;
  datalayer.battery.status.reported_current_dA = -500;  // -50.0 A, and moving

  const std::vector<uint8_t> frame = poll_cyclic_frame();

  EXPECT_FLOAT_EQ(frame_float_at(frame, kInstantCurrentOffset), -50.0f)
      << "a silent shunt must not keep deciding the current; the battery's own reading takes over";
  EXPECT_FLOAT_EQ(frame_float_at(frame, kAverageCurrentOffset), -50.0f)
      << "the average field is quoted from the same dead shunt and needs the same fallback";
}

TEST_F(KostalShuntTest, TheFallbackTracksTheBatteryRatherThanFreezing) {
  datalayer.shunt.available = false;
  datalayer.shunt.measured_amperage_mA = 12300;
  datalayer.battery.status.reported_current_dA = -500;
  EXPECT_FLOAT_EQ(frame_float_at(poll_cyclic_frame(), kInstantCurrentOffset), -50.0f);

  datalayer.battery.status.reported_current_dA = 250;

  EXPECT_FLOAT_EQ(frame_float_at(poll_cyclic_frame(), kInstantCurrentOffset), 25.0f)
      << "the point of the fallback is that it is live - a frozen substitute would be the same bug";
}

TEST_F(KostalShuntTest, TheShuntTakesOverAgainAsSoonAsItIsSeen) {
  datalayer.shunt.available = false;
  datalayer.shunt.measured_amperage_mA = 12300;
  datalayer.battery.status.reported_current_dA = -500;
  ASSERT_FLOAT_EQ(frame_float_at(poll_cyclic_frame(), kInstantCurrentOffset), -50.0f);

  datalayer.shunt.available = true;

  EXPECT_FLOAT_EQ(frame_float_at(poll_cyclic_frame(), kInstantCurrentOffset), 12.3f)
      << "recovery must need nothing but the shunt being seen again";
}

TEST_F(KostalShuntTest, WithoutAnSboxTheShuntFlagDecidesNothing) {
  // ShuntType::Inverter sets shunt.available true and never clears it, so a board that is not on an
  // S-BOX must not have its current picked by that flag either way.
  user_selected_shunt_type = ShuntType::None;
  datalayer.shunt.available = false;
  datalayer.shunt.measured_amperage_mA = 12300;
  datalayer.battery.status.reported_current_dA = -500;

  const std::vector<uint8_t> frame = poll_cyclic_frame();

  EXPECT_FLOAT_EQ(frame_float_at(frame, kInstantCurrentOffset), -50.0f);
  EXPECT_FLOAT_EQ(frame_float_at(frame, kAverageCurrentOffset), -50.0f);
}
