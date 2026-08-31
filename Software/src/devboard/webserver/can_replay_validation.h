#ifndef CAN_REPLAY_VALIDATION_H
#define CAN_REPLAY_VALIDATION_H

#include <string>

/* Validates a requested CAN-replay interface number BEFORE it is stored or
   used. Two traps this closes:

   - /setCANInterface stored ANY integer unvalidated, and the numbering is a
     trap across dialects: 5 means "CAN (MCP2515 add-on)" in the
     settings/BATTCOMM dialect but NO_CAN_INTERFACE in the CAN_Interface enum
     the replay uses (MCP2515 is 2 here). A replay at a nonexistent interface
     is indistinguishable, on every page, from one that is transmitting.
   - An in-range interface whose driver never initialized on this board sends
     frames into a null driver: nothing reaches any wire and the only symptom
     is an eventual buffer-full count.

   `ready` reports whether the given interface's driver actually initialized
   on this build/board (comm_can's can_interface_ready). Injected as a pointer
   so the decision logic is host-testable without the driver stack.

   Returns "" when the interface is usable; otherwise the rejection text for
   the HTTP response, which names the valid numbering so the dialect trap is
   visible to the person who typed the wrong one. */
std::string can_replay_interface_rejection(int requested, bool (*ready)(int));

#endif  // CAN_REPLAY_VALIDATION_H
