#include <vector>

#include "../../Software/src/communication/Transmitter.h"
#include "../../Software/src/communication/can/comm_can.h"

// Records every frame the firmware managed to hand to a CAN chip, so unit tests
// can assert what it actually put on the wire (UDS requests, ISO-TP flow control
// / consecutive frames, heartbeat frames, ...).
//
// comm_can.cpp itself is compiled into this binary; the recording happens one
// level down, in the emulated chips (emul/can_drivers.cpp), so a frame appears
// here only if the real transmit path let it through.
std::vector<CAN_frame> g_emul_transmitted_frames;

void clear_transmitted_frames() {
  g_emul_transmitted_frames.clear();
}

const std::vector<CAN_frame>& get_transmitted_frames() {
  return g_emul_transmitted_frames;
}

// Lives in the webserver, which is not part of the host binary.
char const* getCANInterfaceName(CAN_Interface) {
  return "Foobar";
}

// Defined in Software.cpp, which is not part of the host binary.
void register_transmitter(Transmitter* transmitter) {}

// Declared by the emulated webserver_can_streaming.h; the real one serialises to
// an async web-server response.
void stream_can_frame(const CAN_frame& frame, CAN_Interface interface, frameDirection msgDir) {}
