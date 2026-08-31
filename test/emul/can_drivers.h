#pragma once

#include <stdint.h>

#include <vector>

#include "../../Software/src/devboard/utils/types.h"

// Control and observation for the emulated CAN chips.
//
// comm_can.cpp is compiled into the host binary (see test/CMakeLists.txt); what
// is faked is the layer below it - the three vendored drivers. Everything the
// firmware does to a CAN chip therefore lands here, where a test can make it
// fail, hand it a frame to deliver, or count what it was asked to do.
namespace emul_can {

// A fake chip is identified by the driver object the firmware built, not by
// CAN_Interface: one MCP2518FD class backs both FD add-ons, and CANFD_NATIVE is
// not a chip of its own but a second name for the first of them.
enum class Chip { Native, Mcp2515, Mcp2518fd, Mcp2518fd2 };

// Puts every fake chip back to "present, healthy, nothing queued" and empties
// the transmit capture. Called before each test by the emul listener.
void reset();

// Non-zero makes the next begin() fail with this code. The MCP2515 driver has
// no error code - any non-zero value makes its begin() return false.
void set_begin_error(Chip chip, uint32_t error_code);

// Makes the chip refuse frames handed to it, the way a full hardware transmit
// buffer does.
void set_send_fails(Chip chip, bool fails);

// Raises the driver's error indication, read back by receive_can(). For the
// native controller this is the TWAI status register's error bit; the register
// layout stays inside the emulation, where the driver it belongs to is.
void set_bus_error(Chip chip, bool has_error);

// Puts the native controller bus-off, the state it has to be reinitialised out of.
void set_native_bus_off(bool bus_off);

// Queues a frame for the chip to deliver on the next receive_can().
void queue_received(Chip chip, const CAN_frame& frame);

// Every frame the firmware successfully handed to a chip, in order, with the
// chip that took it.
struct SentFrame {
  Chip chip;
  CAN_frame frame;
};
const std::vector<SentFrame>& sent_frames();

// Lifecycle counters, for the stop_can()/restart_can() paths.
int begin_count(Chip chip);
int end_count(Chip chip);
bool is_running(Chip chip);
bool is_paused(Chip chip);

}  // namespace emul_can

// Runs the real init_CAN() against a HAL that defines every CAN pin, and returns
// what it returned. Use this rather than calling init_CAN() directly: no real
// board carries all four interfaces, and on a board HAL the add-ons either
// collide over the SPI pins or are not defined at all, so init_CAN() gives up
// early and a test can pass for a reason that has nothing to do with what it
// asserts.
bool emul_can_init_on_full_board();

// Drops every interface: no receivers registered, no chip initialized. A test
// that wants to watch an interface come up (or fail to) starts here, registers
// the receivers it wants, and calls emul_can_init_on_full_board().
void emul_can_tear_down_all_interfaces();

// Tear-down, a receiver on each of the four interfaces, then
// emul_can_init_on_full_board(). This is the state each test starts in, so that
// a driver test can transmit without arranging an interface first - the same
// unconditional capture the hand-written emulation used to provide.
void emul_can_bring_up_all_interfaces();
