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

// Makes the chip's next speed change fail, WITHOUT failing its begin().
//
// The real MCP2515 driver enacts a speed change from its task and then reads the
// chip back: the bitrate can be unreachable from the fitted oscillator
// (applySpeedConfig) or the chip can refuse the mode (enterMode), and either one
// leaves a chip that started perfectly well sitting at an unknown bitrate. That
// is the case speedChangeFailed() exists for, so it has to be reachable here
// without also making the chip fail to start - a chip that never started has its
// own event and a null pointer, and cannot stand in for this one.
void set_speed_change_fails(Chip chip, bool fails);

// Wires the FD add-ons onto the MCP2515's SPI bus, the way the boards that
// carry both on one controller are wired. mcp2515_bus_is_exclusive() declines
// the interrupt drain for exactly this, so both arms of that decision are
// reachable. Set it BEFORE emul_can_init_on_full_board(); reset() clears it.
void set_fd_bus_shared_with_2515(bool shared);

// Puts the SECOND FD add-on on a controller of its own, with its own SCK/SDO/SDI
// routed. That is how the T-2CAN carries two FD channels (hw_lilygo2can.h), and
// it is the wiring in which a failure of the first FD bus decides nothing for
// the second chip. Set it BEFORE emul_can_init_on_full_board(); reset() clears it.
void set_second_fd_on_its_own_bus(bool own_bus);

// Makes the first FD bus's SCK a pad the MCP2515 already owns, so alloc_pins()
// refuses it. This is the only route to a FAILED shared FD bus: a bus whose pins
// are merely absent is declined one level earlier, by plan_canfd_init(), which
// leaves fd_bus_ok true and reaches none of the code this exists to drive.
// Set it BEFORE emul_can_init_on_full_board(); reset() clears it.
void set_first_fd_bus_pin_conflict(bool conflict);

// How many times the firmware asked this chip for the interrupt drain, and the
// SPI bus it named. The request is all the host can observe - there is no
// interrupt to install, so isrDrainActive() stays false - but WHETHER it is made
// is the decision mcp2515_bus_is_exclusive() owns.
int isr_drain_requests(Chip chip);
uint8_t isr_drain_bus(Chip chip);

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
// The receive ring depth the chip's driver was last begun with (0 if never).
uint16_t rx_ring_depth(Chip chip);
int end_count(Chip chip);
bool is_running(Chip chip);
bool is_paused(Chip chip);

}  // namespace emul_can

// Runs the real init_CAN() against a HAL that defines every CAN pin. Use this
// rather than calling init_CAN() directly: no real board carries all four
// interfaces, and on a board HAL the add-ons either collide over the SPI pins or
// are not defined at all, so init_CAN() gives up early and a test can pass for a
// reason that has nothing to do with what it asserts.
//
// There is nothing to return. init_CAN() is per-interface: one chip failing to
// start says nothing about the others, so a single verdict for the whole board
// would have to lie about one of them. What came up is read back per chip with
// emul_can::is_running(), which is the same question asked where it has an
// answer.
void emul_can_init_on_full_board();

// Drops every interface: no receivers registered, no chip initialized. A test
// that wants to watch an interface come up (or fail to) starts here, registers
// the receivers it wants, and calls emul_can_init_on_full_board().
void emul_can_tear_down_all_interfaces();

// Tear-down, a receiver on each of the four interfaces, then
// emul_can_init_on_full_board(). This is the state each test starts in, so that
// a driver test can transmit without arranging an interface first - the same
// unconditional capture the hand-written emulation used to provide.
void emul_can_bring_up_all_interfaces();
