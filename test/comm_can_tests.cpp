#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include <SPI.h>  // VSPI, the bus the emulated board puts the MCP2515 on

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
  emul_can_init_on_full_board();

  // begin_count counts the ATTEMPT - the emulated chips increment it before
  // they consult their failure setting - so the zeros below say nothing about
  // whether the one interface this board has actually came up. The bool this
  // case used to read did; is_running() is where that question moved.
  EXPECT_TRUE(emul_can::is_running(Chip::Native));
  EXPECT_EQ(emul_can::begin_count(Chip::Native), 1);
  EXPECT_EQ(emul_can::begin_count(Chip::Mcp2515), 0) << "a board with no add-on must not have its add-on initialised";
  EXPECT_EQ(emul_can::begin_count(Chip::Mcp2518fd), 0);
  EXPECT_EQ(emul_can::begin_count(Chip::Mcp2518fd2), 0);
}

/* comm_can_reset_for_test() names every file static by hand, so a line left out
 * of it is only as visible as some other test's dependence on that particular
 * static. Measured, one line at a time, each in a clean build directory:
 * dropping the driver pointers or the native flag fails several cases, but
 * dropping `settingsespcan = nullptr` fails nothing - the reset is trusted
 * rather than checked.
 *
 * So the hook is pinned from outside: what it claims is that the CAN layer is
 * back to power-on, and this asserts that directly. A new file static still has
 * to be added to the hook by hand - only gathering the statics into one struct
 * and assigning a fresh instance would remove that step - but a forgotten line
 * is now a named failure rather than a silent order dependence.
 */
TEST_F(CommCanTest, ResettingTheCanLayerLeavesNothingBehind) {
  // Start from a known registry rather than from whatever the previous test
  // left: the whole point is to observe what the reset removes, so what went in
  // has to be this test's own.
  emul_can_tear_down_all_interfaces();
  emul_can::reset();

  RecordingReceiver native_receiver;
  register_can_receiver(&native_receiver, CAN_NATIVE);
  emul_can_init_on_full_board();
  ASSERT_TRUE(emul_can::is_running(Chip::Native));

  comm_can_reset_for_test();
  emul_can::reset();

  // Ask for ONE interface that is not the one registered above. A registry the
  // reset failed to empty still holds the native receiver, and init_CAN() would
  // bring the native controller up alongside the add-on.
  RecordingReceiver addon_receiver;
  register_can_receiver(&addon_receiver, CAN_ADDON_MCP2515);
  emul_can_init_on_full_board();

  EXPECT_EQ(emul_can::begin_count(Chip::Mcp2515), 1) << "the interface this test did ask for";
  EXPECT_EQ(emul_can::begin_count(Chip::Native), 0) << "a receiver survived the reset and asked for native CAN";
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
  emul_can_init_on_full_board();

  ASSERT_NE(get_event_pointer(EVENT_CANMCP2515_INIT_FAILURE), nullptr);
  EXPECT_TRUE(get_event_pointer(EVENT_CANMCP2515_INIT_FAILURE)->occurences > 0)
      << "an add-on that fails to start must say so, not just be absent";
  EXPECT_FALSE(emul_can::is_running(Chip::Mcp2515));
}

/* This case was written against the OLD behaviour, and rewritten here because
 * the fix arrived - which is the point of having had it. Its first version
 * pinned init_CAN() abandoning on the first failure, so which interfaces
 * survived a bad chip was decided by the order they happen to be initialised in,
 * and order is not a safety property; its comment said the fix would change this
 * test. One chip's failure now stops at that chip, and the assertion below is
 * the same one with its verdict inverted: the FD add-on is still initialised
 * after the MCP2515, and now comes up.
 *
 * It is also why reading the source could never be the right pin: a scan can see
 * the `return false` go away, but not what its absence buys.
 */
TEST_F(CommCanTest, AFailedChipStopsOnlyItself) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  emul_can::set_begin_error(Chip::Mcp2515, 1);
  reset_all_events();

  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_NATIVE);
  register_can_receiver(&receiver, CAN_ADDON_MCP2515);
  register_can_receiver(&receiver, CANFD_ADDON_MCP2518);
  emul_can_init_on_full_board();

  EXPECT_TRUE(emul_can::is_running(Chip::Native)) << "native is initialised before the add-on and survives";
  EXPECT_FALSE(emul_can::is_running(Chip::Mcp2515)) << "the chip that failed is the one that is down";
  EXPECT_TRUE(emul_can::is_running(Chip::Mcp2518fd))
      << "the FD add-on is initialised after the MCP2515 and must not be skipped for it";
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

/* "Reported" means reported as ABSENT. The send-fail flags were the right
 * assertion when a null driver pointer fell through the same check as a chip
 * that refused the frame; the lane split the two, because "buffer full or no one
 * on the bus to ACK" sends whoever is debugging to the wiring for a chip that is
 * not there. The property this case guards - a missing add-on is diagnosed, not
 * dereferenced - is unchanged; only the flag carrying it moved.
 */
TEST_F(CommCanTest, AnAddonThatIsNotFittedIsReportedRatherThanDereferenced) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_NATIVE);
  emul_can_init_on_full_board();
  ASSERT_TRUE(emul_can::is_running(Chip::Native));
  const CAN_frame frame = make_frame(0x123, 8);

  transmit_can_frame_to_interface(&frame, CAN_ADDON_MCP2515);
  transmit_can_frame_to_interface(&frame, CANFD_ADDON_MCP2518);
  transmit_can_frame_to_interface(&frame, CANFD_ADDON_MCP2518_2);

  EXPECT_TRUE(emul_can::sent_frames().empty());
  EXPECT_TRUE(datalayer.system.info.can_2515_not_initialized);
  EXPECT_TRUE(datalayer.system.info.can_2518_not_initialized);
  EXPECT_TRUE(datalayer.system.info.can_2518_2_not_initialized);
  EXPECT_FALSE(datalayer.system.info.can_2515_send_fail) << "an absent chip is not a full buffer";
  EXPECT_FALSE(datalayer.system.info.can_2518_send_fail);
  EXPECT_FALSE(datalayer.system.info.can_2518_2_send_fail);
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
  emul_can_init_on_full_board();
  ASSERT_TRUE(emul_can::is_running(Chip::Native));
  ASSERT_TRUE(emul_can::is_running(Chip::Mcp2515));
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
  emul_can_init_on_full_board();
  ASSERT_TRUE(emul_can::is_running(Chip::Mcp2518fd));
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
  emul_can_init_on_full_board();
  ASSERT_FALSE(emul_can::is_running(Chip::Native));
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

/* stop_can() gates the native end() on there being a receiver, and nothing
 * exercised the gate: every test starts with all four interfaces up, so
 * deleting the gate entirely passed the whole suite. end() writes TWAI
 * registers, which is why it must not run for an interface that was never set
 * up. This asserts the gate from the side that stays true when the gate is
 * later tightened from "registered" to "initialized".
 */
TEST_F(CommCanTest, StoppingCanLeavesAnInterfaceWithNoReceiverAlone) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();

  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_ADDON_MCP2515);
  emul_can_init_on_full_board();
  ASSERT_TRUE(emul_can::is_running(Chip::Mcp2515));
  const int native_ends_before = emul_can::end_count(Chip::Native);

  stop_can();

  EXPECT_EQ(emul_can::end_count(Chip::Native), native_ends_before)
      << "the native controller was ended although nothing ever registered on it";
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

/* The mirror of the case below, and the one that pins the reset hook's
 * `settingsespcan = nullptr`.
 *
 * change_can_speed() gates the native branch on that pointer, not on
 * native_can_initialized, so a board with no native interface must be refused
 * through it. Dropping that line from comm_can_reset_for_test() failed nothing
 * when the hook was measured line by line (see the note above
 * ResettingTheCanLayerLeavesNothingBehind); this is the observation that was
 * missing.
 */
TEST_F(CommCanTest, ChangingTheNativeSpeedOnABoardWithoutOneFails) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_ADDON_MCP2515);
  emul_can_init_on_full_board();
  ASSERT_TRUE(emul_can::is_running(Chip::Mcp2515));
  ASSERT_FALSE(emul_can::is_running(Chip::Native)) << "the losing side has to be genuinely absent";

  EXPECT_FALSE(change_can_speed(CAN_NATIVE, CAN_Speed::CAN_SPEED_250KBPS))
      << "no native interface was brought up, so there is nothing to re-speed";
  EXPECT_TRUE(change_can_speed(CAN_ADDON_MCP2515, CAN_Speed::CAN_SPEED_250KBPS))
      << "the interface that IS fitted still accepts the request";
}

// ---------------------------------------------------------------------------
// the MCP2515 speed-change verdict
// ---------------------------------------------------------------------------

/* changeSpeed() is asynchronous on the real chip: the driver task enacts it and
 * verifies it afterwards, so the verdict cannot go back the way the request
 * came and receive_frame_can_addon() picks it up instead. These pin that
 * hand-off by calling it, rather than by reading the source for the poll.
 *
 * The failure is armed with set_speed_change_fails(), NOT by failing begin():
 * a chip that never started has a null pointer and its own init event, and
 * receive_can() would not even reach the poll. What is under test here is a chip
 * that came up and is now at an unknown bitrate.
 */
TEST_F(CommCanTest, ASpeedChangeThatDidNotTakeIsReportedOnTheReceivePath) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  reset_all_events();
  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_ADDON_MCP2515);
  emul_can_init_on_full_board();
  ASSERT_TRUE(emul_can::is_running(Chip::Mcp2515));
  ASSERT_NE(get_event_pointer(EVENT_CANMCP2515_INIT_FAILURE), nullptr);
  ASSERT_EQ(get_event_pointer(EVENT_CANMCP2515_INIT_FAILURE)->occurences, 0)
      << "the chip started, so nothing has raised this yet";

  emul_can::set_speed_change_fails(Chip::Mcp2515, true);
  ASSERT_TRUE(change_can_speed(CAN_ADDON_MCP2515, CAN_Speed::CAN_SPEED_250KBPS))
      << "the request is accepted; whether it took is a later question";

  receive_can();

  ASSERT_NE(get_event_pointer(EVENT_CANMCP2515_INIT_FAILURE), nullptr);
  EXPECT_TRUE(get_event_pointer(EVENT_CANMCP2515_INIT_FAILURE)->occurences > 0)
      << "an interface left at an unknown bitrate is not usable, and must say so";
}

/* The RECOVERY half of the verdict, which nothing drove until now.
 *
 * Taking the interface out of service on a failed change is only safe because a
 * later change that TAKES puts it back - poll_can_addon_speed_change() reads
 * speedChangeSucceeded() for exactly that, and the poll deliberately runs
 * outside the usability gate so the answer can still arrive. Both halves are
 * one edit apart from a one-way door: an interface that goes out of service on
 * a bad change and never returns.
 *
 * The two cases above assert the EVENT, which the failing half raises and the
 * recovering half does not, so neither of them can see that door. The oracle
 * here is whether frames get out - the state the gate actually decides - and
 * the losing side is live before the winning one is asserted: the middle block
 * proves the interface really is down before the last block proves it comes
 * back, so a fixture that never took it out of service cannot pass this.
 */
TEST_F(CommCanTest, AnInterfaceTakenOutOfServiceByABadSpeedChangeComesBack) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  reset_all_events();
  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_ADDON_MCP2515);
  emul_can_init_on_full_board();
  ASSERT_TRUE(emul_can::is_running(Chip::Mcp2515));

  CAN_frame frame = make_frame(0x123, 8);
  transmit_can_frame_to_interface(&frame, CAN_ADDON_MCP2515);
  ASSERT_EQ(emul_can::sent_frames().size(), 1u) << "the interface is up before anything changes its speed";

  // Out of service: the change is accepted, the chip reports it did not take.
  emul_can::set_speed_change_fails(Chip::Mcp2515, true);
  ASSERT_TRUE(change_can_speed(CAN_ADDON_MCP2515, CAN_Speed::CAN_SPEED_250KBPS));
  receive_can();

  datalayer.system.info.can_2515_send_fail = false;
  transmit_can_frame_to_interface(&frame, CAN_ADDON_MCP2515);
  ASSERT_EQ(emul_can::sent_frames().size(), 1u)
      << "a chip at an unknown bitrate must not be transmitted to - nothing new should have gone out";
  ASSERT_TRUE(datalayer.system.info.can_2515_send_fail) << "and the frame that went nowhere has to say so";

  // Back in service: a later change that takes.
  emul_can::set_speed_change_fails(Chip::Mcp2515, false);
  ASSERT_TRUE(change_can_speed(CAN_ADDON_MCP2515, CAN_Speed::CAN_SPEED_500KBPS));
  receive_can();

  datalayer.system.info.can_2515_send_fail = false;
  transmit_can_frame_to_interface(&frame, CAN_ADDON_MCP2515);
  EXPECT_EQ(emul_can::sent_frames().size(), 2u)
      << "a change that took puts the interface back; without that the failure gate is a one-way door";
  EXPECT_FALSE(datalayer.system.info.can_2515_send_fail);
}

/* The replay check answers the same question the transmit path does.
 *
 * can_interface_ready() is consulted before a CAN replay is allowed to start,
 * and what it exists to refuse is an interface whose frames will reach no wire
 * while every page shows the replay running. A 2515 at an unknown bitrate is
 * that interface: the driver object is alive, so a pointer test says yes, and
 * the transmit path then drops every frame. The native case never had the gap -
 * its flag is cleared by a failed change as well as by a failed init.
 *
 * Both sides are live in this fixture: the interface answers yes before the bad
 * change, so a walkover cannot pass it, and yes again after the good one.
 */
TEST_F(CommCanTest, AnAddonAtAnUnknownBitrateIsNotOfferedToTheReplay) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  reset_all_events();
  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_ADDON_MCP2515);
  emul_can_init_on_full_board();
  ASSERT_TRUE(emul_can::is_running(Chip::Mcp2515));
  ASSERT_TRUE(can_interface_ready(CAN_ADDON_MCP2515)) << "the chip started, so a replay may use it";

  emul_can::set_speed_change_fails(Chip::Mcp2515, true);
  ASSERT_TRUE(change_can_speed(CAN_ADDON_MCP2515, CAN_Speed::CAN_SPEED_250KBPS));
  receive_can();

  EXPECT_FALSE(can_interface_ready(CAN_ADDON_MCP2515))
      << "a replay onto a chip at an unknown bitrate reaches no wire, which is what this check refuses";

  emul_can::set_speed_change_fails(Chip::Mcp2515, false);
  ASSERT_TRUE(change_can_speed(CAN_ADDON_MCP2515, CAN_Speed::CAN_SPEED_500KBPS));
  receive_can();

  EXPECT_TRUE(can_interface_ready(CAN_ADDON_MCP2515)) << "a change that took puts the interface back on offer";
}

TEST_F(CommCanTest, ASpeedChangeThatTookIsReportedAsNothing) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  reset_all_events();
  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_ADDON_MCP2515);
  emul_can_init_on_full_board();
  ASSERT_TRUE(emul_can::is_running(Chip::Mcp2515));

  ASSERT_TRUE(change_can_speed(CAN_ADDON_MCP2515, CAN_Speed::CAN_SPEED_250KBPS));

  receive_can();

  ASSERT_NE(get_event_pointer(EVENT_CANMCP2515_INIT_FAILURE), nullptr);
  EXPECT_EQ(get_event_pointer(EVENT_CANMCP2515_INIT_FAILURE)->occurences, 0)
      << "a speed change that worked must not look like an init failure";
}

/* A second FD add-on on its OWN SPI bus is not taken down with the first one.
 *
 * plan_canfd_init() already draws this line (CanFdInitPlanTest.
 * ASecondChipOnItsOwnBusIsNotToldTheFirstBusIsMissing), and the second chip's
 * own pin gate is written for it - `fd_bus_ok || !shares_first_fd_bus`. Between
 * the two sat a loop that marked all three FD interfaces unavailable whenever
 * the FIRST bus failed, which erased the second chip's registration before that
 * gate could run: the re-read below it turned the iterator into end(), the
 * gate short-circuited true, and the chip was reported missing without ever
 * having been attempted.
 *
 * Neither branch could see it. The offer that added interface_unavailable() has
 * no separate-bus gate, and the branch that added the separate-bus gate erases
 * nothing; the two arrive from different arms and only the merge has both. The
 * T-2CAN in its FD fitment is wired this way - hw_lilygo2can.h puts the second
 * MCP2518 on the controller the MCP2515 would have used.
 *
 * A pin CONFLICT is how the first bus is made to fail, and it has to be: a bus
 * whose pins are merely absent is declined by plan_canfd_init() one level up,
 * which leaves fd_bus_ok true and reaches none of this.
 */
TEST_F(CommCanTest, ASecondFdChipOnItsOwnBusSurvivesTheFirstFdBusFailing) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  emul_can::set_second_fd_on_its_own_bus(true);
  emul_can::set_first_fd_bus_pin_conflict(true);

  RecordingReceiver receiver;
  // The 2515 is registered so that its pads are allocated first; the first FD
  // bus then asks for one of them and alloc_pins() refuses it.
  register_can_receiver(&receiver, CAN_ADDON_MCP2515);
  register_can_receiver(&receiver, CANFD_ADDON_MCP2518);
  register_can_receiver(&receiver, CANFD_ADDON_MCP2518_2);
  emul_can_init_on_full_board();

  ASSERT_TRUE(emul_can::is_running(Chip::Mcp2515)) << "the conflict is on the FD bus, not on the 2515";
  ASSERT_FALSE(emul_can::is_running(Chip::Mcp2518fd))
      << "the first FD chip has no bus to talk over, so it must not have started";

  EXPECT_TRUE(emul_can::is_running(Chip::Mcp2518fd2))
      << "the second FD add-on has a bus of its own; a failure of the first one decides nothing for it";

  // ...and it is still a registered interface, not merely a live chip: the
  // erase is what a driver on this channel would have lost.
  EXPECT_EQ(get_event_pointer(EVENT_INTERFACE_MISSING)->occurences, 1)
      << "one interface was lost (the first FD chip); the second was reported missing too";

  CAN_frame frame = make_frame(0x321, 8);
  emul_can::queue_received(Chip::Mcp2518fd2, frame);
  receive_can();
  ASSERT_EQ(receiver.received.size(), 1u) << "the driver registered on the second FD add-on no longer receives";
  EXPECT_EQ(receiver.received[0].ID, 0x321u);
}

/* The same failure with both FD chips on ONE bus: there the second one really
 * does go with the first, and this is the case that must keep working.
 *
 * It is a characterization case and no mutation bites it alone - measured, not
 * assumed. Dropping the second chip from the loop entirely, sharing or not,
 * leaves this green, because the chip block below re-raises it: its pin gate
 * reads `fd_bus_ok || !shares_first_fd_bus`, which is false on a shared bus, so
 * the chip is nulled and marked unavailable a few lines later instead. The two
 * spellings differ only in WHERE the interface is written off, and nothing
 * observable separates them. What this case is here for is the other direction:
 * the fix above must not become "the second chip is never taken down", and this
 * is the board on which that would be wrong.
 */
TEST_F(CommCanTest, ASecondFdChipSharingTheFailedBusIsMarkedUnavailableWithIt) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  emul_can::set_first_fd_bus_pin_conflict(true);

  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_ADDON_MCP2515);
  register_can_receiver(&receiver, CANFD_ADDON_MCP2518);
  register_can_receiver(&receiver, CANFD_ADDON_MCP2518_2);
  emul_can_init_on_full_board();

  ASSERT_TRUE(emul_can::is_running(Chip::Mcp2515));
  EXPECT_FALSE(emul_can::is_running(Chip::Mcp2518fd));
  EXPECT_FALSE(emul_can::is_running(Chip::Mcp2518fd2)) << "both FD chips are on the bus that never came up";
  EXPECT_EQ(get_event_pointer(EVENT_INTERFACE_MISSING)->occurences, 2)
      << "both FD interfaces are reported missing, and only those two";
}

// ---------------------------------------------------------------------------
// mcp2515_bus_is_exclusive()
// ---------------------------------------------------------------------------

/* Whether the interrupt drain is ASKED FOR is a decision comm_can.cpp owns, and
 * both arms are live on the host: the emulated board puts the add-ons on
 * separate SPI controllers by default, and set_fd_bus_shared_with_2515() puts
 * them on one, which is how the boards that carry both are wired.
 *
 * What the driver then DOES with the request is not observable here - there is
 * no interrupt to install, so the emulated isrDrainActive() is always false -
 * but the request itself is the half that lives in this file.
 */
TEST_F(CommCanTest, TheDrainIsAskedForWhenTheAddonOwnsItsBus) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_ADDON_MCP2515);
  register_can_receiver(&receiver, CANFD_ADDON_MCP2518);
  emul_can_init_on_full_board();

  ASSERT_TRUE(emul_can::is_running(Chip::Mcp2515));
  ASSERT_TRUE(emul_can::is_running(Chip::Mcp2518fd)) << "the FD chip is present, it is just on another bus";
  EXPECT_EQ(emul_can::isr_drain_requests(Chip::Mcp2515), 1);
  EXPECT_EQ(emul_can::isr_drain_bus(Chip::Mcp2515), VSPI) << "the drain is asked for on the chip's own bus";
}

TEST_F(CommCanTest, TheDrainIsDeclinedWhenAnFdAddonSharesTheBus) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  emul_can::set_fd_bus_shared_with_2515(true);
  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_ADDON_MCP2515);
  register_can_receiver(&receiver, CANFD_ADDON_MCP2518);
  emul_can_init_on_full_board();

  ASSERT_TRUE(emul_can::is_running(Chip::Mcp2515));
  EXPECT_EQ(emul_can::isr_drain_requests(Chip::Mcp2515), 0)
      << "a second device on the controller makes the drain unsafe, so it is not requested";
}

TEST_F(CommCanTest, AnFdChipNobodyRegisteredDoesNotCostTheDrain) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  emul_can::set_fd_bus_shared_with_2515(true);
  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_ADDON_MCP2515);
  emul_can_init_on_full_board();

  ASSERT_TRUE(emul_can::is_running(Chip::Mcp2515));
  EXPECT_EQ(emul_can::isr_drain_requests(Chip::Mcp2515), 1)
      << "the bus is shared on paper only - nothing else was brought up on it";
}

TEST_F(CommCanTest, ChangingTheSpeedOfAnInterfaceWithNoDriverFails) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_NATIVE);
  emul_can_init_on_full_board();
  ASSERT_TRUE(emul_can::is_running(Chip::Native));

  EXPECT_FALSE(change_can_speed(CAN_ADDON_MCP2515, CAN_Speed::CAN_SPEED_250KBPS));
  EXPECT_FALSE(change_can_speed(CANFD_ADDON_MCP2518, CAN_Speed::CAN_SPEED_250KBPS));
}

// ---------------------------------------------------------------------------
// format_can_frame()
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
