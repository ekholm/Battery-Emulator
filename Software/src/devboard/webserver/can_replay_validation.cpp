#include "can_replay_validation.h"

#include "../utils/types.h"

std::string can_replay_interface_rejection(int requested, bool (*ready)(int)) {
  if (requested < CAN_NATIVE || requested >= NO_CAN_INTERFACE) {
    return "Invalid CAN interface " + std::to_string(requested) +
           ". Valid: 0=CAN Native, 1=CANFD Native, 2=CAN Addon MCP2515, 3=CANFD Addon MCP2518, "
           "4=second CANFD Addon. (Note: this is NOT the settings/BATTCOMM numbering, where 5 is "
           "the MCP2515 add-on - here 5 means no interface at all.)";
  }
  if (!ready(requested)) {
    return "CAN interface " + std::to_string(requested) +
           " is not initialized on this board - a replay at it would reach no wire. Pick an "
           "interface that is actually running (check the startup log), or fix its wiring/config "
           "and reboot.";
  }
  return "";
}

std::string can_replay_dlc_rejection(long declared, size_t capacity) {
  if (declared < 0) {
    return "negative frame length";
  }
  if ((unsigned long)declared > (unsigned long)capacity) {
    return "frame length " + std::to_string(declared) + " exceeds the " + std::to_string(capacity) +
           " byte maximum a CAN frame can carry";
  }
  return "";
}
