#pragma once

#include "../../../../../Software/src/devboard/utils/types.h"

// Emulated stand-in: the real header pulls in the whole async web server, which
// is not part of the host binary. comm_can.cpp only needs stream_can_frame().
void stream_can_frame(const CAN_frame& frame, CAN_Interface interface, frameDirection msgDir);
