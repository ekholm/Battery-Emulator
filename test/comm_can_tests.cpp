#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "emul/can_drivers.h"

#include "../Software/src/communication/can/CanReceiver.h"
#include "../Software/src/communication/can/comm_can.h"
#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/safety/safety.h"
#include "../Software/src/devboard/utils/events.h"

/* comm_can.cpp, called rather than read.
 *
 * Four CAN fixes in a row had to be pinned by tests that opened this file and
 * matched its TEXT, because it was not part of the host binary: the emulation
 * supplied transmit_can_frame_to_interface() and friends, so the real ones could
 * not be reached. Tests that assert on source break on any behaviour-preserving
 * refactor while the guard they protect is intact, and they cannot see a guard
 * that is present but wrong.
 *
 * The file is in the binary now (test/CMakeLists.txt), with emulated chips under
 * the three vendored drivers (emul/can_drivers.cpp). These are the tests that
 * idiom was standing in for.
 */

using emul_can::Chip;

namespace {

// Collects the frames handed to it, so a test can see what came off an interface.
class RecordingReceiver : public CanReceiver {
 public:
  void receive_can_frame(CAN_frame* rx_frame) override { received.push_back(*rx_frame); }
  std::vector<CAN_frame> received;
};

CAN_frame make_frame(uint32_t id, uint8_t dlc, bool ext = false, bool fd = false) {
  CAN_frame frame = {};
  frame.ID = id;
  frame.DLC = dlc;
  frame.ext_ID = ext;
  frame.FD = fd;
  for (uint8_t i = 0; i < dlc && i < sizeof(frame.data.u8); i++) {
    frame.data.u8[i] = i;
  }
  return frame;
}

class CommCanTest : public testing::Test {
 protected:
  // Each test starts with all four interfaces up (test/tests.cpp listener). A
  // test that wants a different board tears them down and registers its own.
  void SetUp() override { allowed_to_send_CAN = true; }
};

}  // namespace

// ---------------------------------------------------------------------------
// init_CAN()
// ---------------------------------------------------------------------------

TEST_F(CommCanTest, EveryRequestedInterfaceIsBroughtUp) {
  EXPECT_TRUE(emul_can::is_running(Chip::Native));
  EXPECT_TRUE(emul_can::is_running(Chip::Mcp2515));
  EXPECT_TRUE(emul_can::is_running(Chip::Mcp2518fd));
  EXPECT_TRUE(emul_can::is_running(Chip::Mcp2518fd2));
  EXPECT_EQ(emul_can::begin_count(Chip::Native), 1);
  EXPECT_EQ(emul_can::begin_count(Chip::Mcp2515), 1);
  EXPECT_EQ(emul_can::begin_count(Chip::Mcp2518fd), 1);
  EXPECT_EQ(emul_can::begin_count(Chip::Mcp2518fd2), 1);
}

TEST_F(CommCanTest, AnInterfaceNobodyAskedForIsNeverTouched) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();

  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_NATIVE);
  EXPECT_TRUE(init_CAN());

  EXPECT_EQ(emul_can::begin_count(Chip::Native), 1);
  EXPECT_EQ(emul_can::begin_count(Chip::Mcp2515), 0) << "a board with no add-on must not have its add-on initialised";
  EXPECT_EQ(emul_can::begin_count(Chip::Mcp2518fd), 0);
  EXPECT_EQ(emul_can::begin_count(Chip::Mcp2518fd2), 0);
}

TEST_F(CommCanTest, AChipThatFailsToStartRaisesItsOwnEvent) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  emul_can::set_begin_error(Chip::Mcp2515, 1);
  reset_all_events();

  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_ADDON_MCP2515);
  EXPECT_FALSE(init_CAN());

  ASSERT_NE(get_event_pointer(EVENT_CANMCP2515_INIT_FAILURE), nullptr);
  EXPECT_TRUE(get_event_pointer(EVENT_CANMCP2515_INIT_FAILURE)->occurences > 0)
      << "an add-on that fails to start must say so, not just be absent";
  EXPECT_FALSE(emul_can::is_running(Chip::Mcp2515));
}

/* Pinned as the code behaves TODAY, not as it should: init_CAN() abandons on the
 * first failure, so which interfaces survive a bad chip is decided by the order
 * they happen to be initialised in, and order is not a safety property. The fix
 * for that - one chip's failure stopping at that chip - changes this test, which
 * is the point of having it, and the reason reading the source could never be
 * the right pin: it can see the `return false`, but not what the return costs.
 */
TEST_F(CommCanTest, AFailedChipCurrentlyStopsEveryInterfaceAfterIt) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  emul_can::set_begin_error(Chip::Mcp2515, 1);
  reset_all_events();

  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_NATIVE);
  register_can_receiver(&receiver, CAN_ADDON_MCP2515);
  register_can_receiver(&receiver, CANFD_ADDON_MCP2518);
  EXPECT_FALSE(init_CAN());

  EXPECT_TRUE(emul_can::is_running(Chip::Native)) << "native is initialised before the add-on and survives";
  EXPECT_EQ(emul_can::begin_count(Chip::Mcp2518fd), 0)
      << "the FD add-on is initialised after the MCP2515 and is skipped entirely";
}

// ---------------------------------------------------------------------------
// transmit_can_frame_to_interface()
// ---------------------------------------------------------------------------

TEST_F(CommCanTest, EachInterfaceIsHandedItsOwnFrames) {
  const CAN_frame frame = make_frame(0x123, 8);

  transmit_can_frame_to_interface(&frame, CAN_NATIVE);
  transmit_can_frame_to_interface(&frame, CAN_ADDON_MCP2515);
  transmit_can_frame_to_interface(&frame, CANFD_ADDON_MCP2518);
  transmit_can_frame_to_interface(&frame, CANFD_ADDON_MCP2518_2);

  ASSERT_EQ(emul_can::sent_frames().size(), 4u);
  EXPECT_EQ(emul_can::sent_frames()[0].chip, Chip::Native);
  EXPECT_EQ(emul_can::sent_frames()[1].chip, Chip::Mcp2515);
  EXPECT_EQ(emul_can::sent_frames()[2].chip, Chip::Mcp2518fd);
  EXPECT_EQ(emul_can::sent_frames()[3].chip, Chip::Mcp2518fd2);
  for (const auto& sent : emul_can::sent_frames()) {
    EXPECT_EQ(sent.frame.ID, 0x123u);
    EXPECT_EQ(sent.frame.DLC, 8);
  }
}

TEST_F(CommCanTest, TheNativeFdInterfaceIsTheFirstFdAddonUnderAnotherName) {
  const CAN_frame frame = make_frame(0x321, 8);

  transmit_can_frame_to_interface(&frame, CANFD_NATIVE);

  ASSERT_EQ(emul_can::sent_frames().size(), 1u);
  EXPECT_EQ(emul_can::sent_frames()[0].chip, Chip::Mcp2518fd);
}

TEST_F(CommCanTest, NothingIsSentWhileSendingIsForbidden) {
  allowed_to_send_CAN = false;
  const CAN_frame frame = make_frame(0x123, 8);

  transmit_can_frame_to_interface(&frame, CAN_NATIVE);

  EXPECT_TRUE(emul_can::sent_frames().empty());
}

TEST_F(CommCanTest, AnFdLengthFrameIsRefusedByTheClassicInterfaces) {
  const CAN_frame frame = make_frame(0x123, 16, false, true);

  transmit_can_frame_to_interface(&frame, CAN_NATIVE);
  EXPECT_TRUE(emul_can::sent_frames().empty()) << "a 16-byte frame does not fit a classic CAN controller";
  EXPECT_TRUE(datalayer.system.info.can_native_send_fail);

  transmit_can_frame_to_interface(&frame, CAN_ADDON_MCP2515);
  EXPECT_TRUE(emul_can::sent_frames().empty());
  EXPECT_TRUE(datalayer.system.info.can_2515_send_fail);
}

TEST_F(CommCanTest, AnFdLengthFrameIsAcceptedByTheFdInterfaces) {
  const CAN_frame frame = make_frame(0x123, 64, false, true);

  transmit_can_frame_to_interface(&frame, CANFD_ADDON_MCP2518);

  ASSERT_EQ(emul_can::sent_frames().size(), 1u);
  EXPECT_EQ(emul_can::sent_frames()[0].frame.DLC, 64);
  EXPECT_TRUE(emul_can::sent_frames()[0].frame.FD);
  EXPECT_FALSE(datalayer.system.info.can_2518_send_fail);
}

TEST_F(CommCanTest, AChipThatRefusesAFrameIsReportedPerInterface) {
  emul_can::set_send_fails(Chip::Native, true);
  emul_can::set_send_fails(Chip::Mcp2515, true);
  emul_can::set_send_fails(Chip::Mcp2518fd, true);
  emul_can::set_send_fails(Chip::Mcp2518fd2, true);
  const CAN_frame frame = make_frame(0x123, 8);

  transmit_can_frame_to_interface(&frame, CAN_NATIVE);
  transmit_can_frame_to_interface(&frame, CAN_ADDON_MCP2515);
  transmit_can_frame_to_interface(&frame, CANFD_ADDON_MCP2518);
  transmit_can_frame_to_interface(&frame, CANFD_ADDON_MCP2518_2);

  EXPECT_TRUE(datalayer.system.info.can_native_send_fail);
  EXPECT_TRUE(datalayer.system.info.can_2515_send_fail);
  EXPECT_TRUE(datalayer.system.info.can_2518_send_fail);
  EXPECT_TRUE(datalayer.system.info.can_2518_2_send_fail);
}

TEST_F(CommCanTest, AnAddonThatIsNotFittedIsReportedRatherThanDereferenced) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_NATIVE);
  ASSERT_TRUE(init_CAN());
  const CAN_frame frame = make_frame(0x123, 8);

  transmit_can_frame_to_interface(&frame, CAN_ADDON_MCP2515);
  transmit_can_frame_to_interface(&frame, CANFD_ADDON_MCP2518);
  transmit_can_frame_to_interface(&frame, CANFD_ADDON_MCP2518_2);

  EXPECT_TRUE(emul_can::sent_frames().empty());
  EXPECT_TRUE(datalayer.system.info.can_2515_send_fail);
  EXPECT_TRUE(datalayer.system.info.can_2518_send_fail);
  EXPECT_TRUE(datalayer.system.info.can_2518_2_send_fail);
}

TEST_F(CommCanTest, AnInterfaceValueThatNamesNoChipSendsNothing) {
  const CAN_frame frame = make_frame(0x123, 8);

  transmit_can_frame_to_interface(&frame, static_cast<CAN_Interface>(99));

  EXPECT_TRUE(emul_can::sent_frames().empty());
  EXPECT_FALSE(datalayer.system.info.can_native_send_fail);
  EXPECT_FALSE(datalayer.system.info.can_2515_send_fail);
  EXPECT_FALSE(datalayer.system.info.can_2518_send_fail);
  EXPECT_FALSE(datalayer.system.info.can_2518_2_send_fail);
}

// ---------------------------------------------------------------------------
// receive_can()
// ---------------------------------------------------------------------------

TEST_F(CommCanTest, AReceivedFrameReachesOnlyTheReceiversOnThatInterface) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  RecordingReceiver on_native;
  RecordingReceiver on_addon;
  register_can_receiver(&on_native, CAN_NATIVE);
  register_can_receiver(&on_addon, CAN_ADDON_MCP2515);
  ASSERT_TRUE(init_CAN());
  emul_can::queue_received(Chip::Native, make_frame(0x111, 3));

  receive_can();

  ASSERT_EQ(on_native.received.size(), 1u);
  EXPECT_EQ(on_native.received[0].ID, 0x111u);
  EXPECT_EQ(on_native.received[0].DLC, 3);
  EXPECT_TRUE(on_addon.received.empty());
}

TEST_F(CommCanTest, TheFirstFdAddonDeliversToBothOfItsInterfaceNames) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  RecordingReceiver on_addon;
  RecordingReceiver on_native_fd;
  register_can_receiver(&on_addon, CANFD_ADDON_MCP2518);
  register_can_receiver(&on_native_fd, CANFD_NATIVE);
  ASSERT_TRUE(init_CAN());
  emul_can::queue_received(Chip::Mcp2518fd, make_frame(0x222, 12, false, true));

  receive_can();

  ASSERT_EQ(on_addon.received.size(), 1u);
  ASSERT_EQ(on_native_fd.received.size(), 1u);
  EXPECT_EQ(on_addon.received[0].ID, 0x222u);
  EXPECT_EQ(on_native_fd.received[0].ID, 0x222u);
}

TEST_F(CommCanTest, AnUninitializedNativeControllerIsNotPolled) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  emul_can::set_begin_error(Chip::Native, 0x40);
  RecordingReceiver on_native;
  register_can_receiver(&on_native, CAN_NATIVE);
  ASSERT_FALSE(init_CAN());
  emul_can::queue_received(Chip::Native, make_frame(0x111, 3));
  emul_can::set_bus_error(Chip::Native, true);

  receive_can();

  EXPECT_TRUE(on_native.received.empty());
  EXPECT_FALSE(datalayer.system.info.can_native_bus_error)
      << "a controller that never started has no bus to report an error on";
}

TEST_F(CommCanTest, BusErrorsAreLatchedPerInterface) {
  emul_can::set_bus_error(Chip::Native, true);
  emul_can::set_bus_error(Chip::Mcp2515, true);
  emul_can::set_bus_error(Chip::Mcp2518fd, true);
  emul_can::set_bus_error(Chip::Mcp2518fd2, true);

  receive_can();

  EXPECT_TRUE(datalayer.system.info.can_native_bus_error);
  EXPECT_TRUE(datalayer.system.info.can_2515_bus_error);
  EXPECT_TRUE(datalayer.system.info.can_2518_bus_error);
  EXPECT_TRUE(datalayer.system.info.can_2518_2_bus_error);
}

TEST_F(CommCanTest, ABusOffNativeControllerIsRestarted) {
  const int before = emul_can::begin_count(Chip::Native);
  emul_can::set_native_bus_off(true);

  receive_can();

  EXPECT_EQ(emul_can::begin_count(Chip::Native), before + 1)
      << "bus-off is recovered by reinitialising the controller at the same speed";
  EXPECT_TRUE(datalayer.system.info.can_native_bus_error);
}

// ---------------------------------------------------------------------------
// stop_can() / restart_can()
// ---------------------------------------------------------------------------

TEST_F(CommCanTest, StoppingCanTakesEveryInterfaceOutOfService) {
  stop_can();

  EXPECT_EQ(emul_can::end_count(Chip::Native), 1);
  EXPECT_TRUE(emul_can::is_paused(Chip::Mcp2515));
  EXPECT_EQ(emul_can::end_count(Chip::Mcp2518fd), 1);
  EXPECT_EQ(emul_can::end_count(Chip::Mcp2518fd2), 1);
}

TEST_F(CommCanTest, RestartingCanBringsEveryInterfaceBack) {
  stop_can();

  restart_can();

  EXPECT_EQ(emul_can::begin_count(Chip::Native), 2);
  EXPECT_FALSE(emul_can::is_paused(Chip::Mcp2515));
  EXPECT_EQ(emul_can::begin_count(Chip::Mcp2518fd), 2);
  EXPECT_EQ(emul_can::begin_count(Chip::Mcp2518fd2), 2);
}

// ---------------------------------------------------------------------------
// change_can_speed()
// ---------------------------------------------------------------------------

TEST_F(CommCanTest, ChangingTheNativeSpeedReinitializesTheController) {
  const int before = emul_can::begin_count(Chip::Native);

  EXPECT_TRUE(change_can_speed(CAN_NATIVE, CAN_Speed::CAN_SPEED_250KBPS));

  EXPECT_EQ(emul_can::begin_count(Chip::Native), before + 1);
}

TEST_F(CommCanTest, ChangingTheSpeedOfAnInterfaceWithNoDriverFails) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_NATIVE);
  ASSERT_TRUE(init_CAN());

  EXPECT_FALSE(change_can_speed(CAN_ADDON_MCP2515, CAN_Speed::CAN_SPEED_250KBPS));
  EXPECT_FALSE(change_can_speed(CANFD_ADDON_MCP2518, CAN_Speed::CAN_SPEED_250KBPS));
}

// ---------------------------------------------------------------------------
// format_can_frame() / dump_can_frame()
// ---------------------------------------------------------------------------

TEST_F(CommCanTest, AStandardFrameIsFormattedWithThreeHexDigitsOfIdentifier) {
  char buffer[128] = {};
  const CAN_frame frame = make_frame(0x123, 3);

  const size_t written = format_can_frame(buffer, sizeof(buffer), frame, CAN_NATIVE, MSG_RX);

  EXPECT_EQ(written, strlen(buffer));
  EXPECT_NE(std::string(buffer).find(" rx0 123 [3] 00 01 02\n"), std::string::npos) << buffer;
}

TEST_F(CommCanTest, AnExtendedFrameIsFormattedWithEightHexDigitsOfIdentifier) {
  char buffer[128] = {};
  const CAN_frame frame = make_frame(0x18DAF110, 1, true);

  format_can_frame(buffer, sizeof(buffer), frame, CAN_NATIVE, MSG_RX);

  EXPECT_NE(std::string(buffer).find(" rx0 18daf110 [1] 00\n"), std::string::npos) << buffer;
}

TEST_F(CommCanTest, TheDirectionAndInterfaceAreEncodedInTheChannelNumber) {
  char rx[128] = {};
  char tx[128] = {};
  const CAN_frame frame = make_frame(0x1, 0);

  format_can_frame(rx, sizeof(rx), frame, CAN_ADDON_MCP2515, MSG_RX);
  format_can_frame(tx, sizeof(tx), frame, CAN_ADDON_MCP2515, MSG_TX);

  // The channel number is the interface ordinal doubled, plus one for a
  // transmit - so RX and TX of one interface are two channels a log reader can
  // separate. CAN_ADDON_MCP2515 is 2, hence 4 and 5.
  EXPECT_EQ(static_cast<int>(CAN_ADDON_MCP2515), 2);
  EXPECT_NE(std::string(rx).find(" rx4 "), std::string::npos) << rx;
  EXPECT_NE(std::string(tx).find(" tx5 "), std::string::npos) << tx;
}

TEST_F(CommCanTest, AnFdFrameIsMarkedByCaseSoALogReaderCanTellThemApart) {
  char buffer[256] = {};
  const CAN_frame frame = make_frame(0x1, 12, false, true);

  format_can_frame(buffer, sizeof(buffer), frame, CANFD_ADDON_MCP2518, MSG_RX);

  EXPECT_EQ(static_cast<int>(CANFD_ADDON_MCP2518), 3);
  EXPECT_NE(std::string(buffer).find(" RX6 "), std::string::npos) << buffer;
  EXPECT_NE(std::string(buffer).find("[12]"), std::string::npos) << buffer;
}

TEST_F(CommCanTest, ABufferTooSmallForTheLineIsLeftEmptyRatherThanOverrun) {
  char buffer[16];
  memset(buffer, 'x', sizeof(buffer));
  const CAN_frame frame = make_frame(0x123, 8);

  const size_t written = format_can_frame(buffer, sizeof(buffer), frame, CAN_NATIVE, MSG_RX);

  EXPECT_EQ(written, 0u);
  EXPECT_EQ(buffer[0], '\0');
}

TEST_F(CommCanTest, TheLogWrapsToTheStartWhenTheTailIsTooShort) {
  CAN_frame frame = make_frame(0x123, 8);
  datalayer.system.info.logged_can_messages_offset = sizeof(datalayer.system.info.logged_can_messages) - 8;

  dump_can_frame(frame, CAN_NATIVE, MSG_RX);

  EXPECT_LT(datalayer.system.info.logged_can_messages_offset, sizeof(datalayer.system.info.logged_can_messages) - 8)
      << "the line does not fit at the tail, so it is written from the start again";
  EXPECT_NE(std::string(datalayer.system.info.logged_can_messages).find(" 123 [8] "), std::string::npos);
}

TEST_F(CommCanTest, TheCutoffFilterKeepsLowIdentifiersOutOfTheLog) {
  datalayer.system.info.can_logging_active = true;
  user_selected_CAN_ID_cutoff_filter = 0x200;
  const CAN_frame below = make_frame(0x100, 1);
  const CAN_frame above = make_frame(0x300, 1);

  transmit_can_frame_to_interface(&below, CAN_NATIVE);
  transmit_can_frame_to_interface(&above, CAN_NATIVE);

  const std::string log(datalayer.system.info.logged_can_messages);
  EXPECT_EQ(log.find(" 100 "), std::string::npos) << log;
  EXPECT_NE(log.find(" 300 "), std::string::npos) << log;
}
