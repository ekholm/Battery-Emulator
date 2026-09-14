#include <gtest/gtest.h>

#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/utils/events.h"
#include "../Software/src/inverter/INVERTERS.h"
#include "../Software/src/inverter/SOLAX-CAN.h"

// The SolaX contactor-close permission must not outlive the inverter's request
// to open. The CONTACTOR_CLOSED case sets inverter_allows_contactor_closing
// near its top and only then tests for the open payload; resetting STATE alone
// there would leave the permission true until the NEXT received frame drives
// the BATTERY_ANNOUNCE case - and indefinitely, if the inverter goes quiet
// after asking to disconnect. These tests pin the revocation to the same
// frame that carries the open request.

namespace {

// 0x1871 payloads, byte0=0x02 (inverter status), byte4 = contactor request.
static const uint64_t PAYLOAD_ANNOUNCE = __builtin_bswap64(0x0200010000000000);
static const uint64_t PAYLOAD_CLOSE = __builtin_bswap64(0x0200010001000000);
static const uint64_t PAYLOAD_OPEN = PAYLOAD_ANNOUNCE;

class SolaxContactorPermissionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // DataLayerResetListener has already reset datalayer/events and deleted
    // the previous inverter instance.
    user_selected_inverter_protocol = InverterProtocolType::Solax;
    user_selected_inverter_contactor_mode = inverter_contactor_mode_enum::NoWorkaround;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    solax = static_cast<SolaxInverter*>(inverter);
  }

  void TearDown() override { user_selected_inverter_contactor_mode = inverter_contactor_mode_enum::NoWorkaround; }

  void rx1871(uint64_t payload) {
    CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x1871, .data = {}};
    f.data.u64 = payload;
    solax->map_can_frame_to_variable(f);
  }

  // BATTERY_ANNOUNCE -> WAITING_FOR_CONTACTOR -> CONTACTOR_CLOSED, then one
  // more frame so the CONTACTOR_CLOSED case has run and granted permission.
  // The hold frames must carry byte4=1: a byte4=0 frame IS the open payload,
  // and with same-frame revocation it would end the closed state right here.
  void walk_to_contactor_closed() {
    rx1871(PAYLOAD_CLOSE);
    rx1871(PAYLOAD_CLOSE);
    rx1871(PAYLOAD_CLOSE);
    ASSERT_TRUE(datalayer.system.status.inverter_allows_contactor_closing);
  }

  SolaxInverter* solax = nullptr;
};

}  // namespace

TEST_F(SolaxContactorPermissionTest, OpenRequestRevokesPermissionInTheSameFrame) {
  walk_to_contactor_closed();

  // The frame that carries the open request must leave the permission revoked -
  // no further frame may be needed. If the inverter goes quiet after asking,
  // this is the value the contactor logic keeps acting on.
  rx1871(PAYLOAD_OPEN);
  EXPECT_FALSE(datalayer.system.status.inverter_allows_contactor_closing)
      << "closing permission survived the inverter's open request";
  EXPECT_EQ(get_event_pointer(EVENT_INVERTER_OPEN_CONTACTOR)->occurences, 1);
}

TEST_F(SolaxContactorPermissionTest, PermissionStaysRevokedWhileAnnounceRuns) {
  walk_to_contactor_closed();
  rx1871(PAYLOAD_OPEN);

  // Subsequent frames drive BATTERY_ANNOUNCE, which must agree.
  rx1871(PAYLOAD_ANNOUNCE);
  EXPECT_FALSE(datalayer.system.status.inverter_allows_contactor_closing);
}

TEST_F(SolaxContactorPermissionTest, LockAfterFirstCloseIgnoresOpenRequest) {
  // In LockAfterFirstClose mode the open request is deliberately ignored, and
  // the revocation must not fire either - the permission stays granted.
  delete inverter;
  inverter = nullptr;
  user_selected_inverter_contactor_mode = inverter_contactor_mode_enum::LockAfterFirstClose;
  setup_inverter();
  solax = static_cast<SolaxInverter*>(inverter);

  walk_to_contactor_closed();
  rx1871(PAYLOAD_OPEN);
  EXPECT_TRUE(datalayer.system.status.inverter_allows_contactor_closing)
      << "LockAfterFirstClose must keep the permission despite the open payload";
  EXPECT_EQ(get_event_pointer(EVENT_INVERTER_OPEN_CONTACTOR)->occurences, 0);
}
