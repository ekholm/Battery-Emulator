#ifndef _CAN_SPEED_POLICY_H_
#define _CAN_SPEED_POLICY_H_

#include <map>

#include "comm_can.h"

class CanReceiver;

/* A driver requests bus properties, the interface OWNS them.
 *
 * Every driver that talks CAN registers itself for an interface and says what
 * bitrate it needs. Two drivers can share one interface - a charger and a
 * battery on the native port is an ordinary setup - and then there is only one
 * bus, so there is only one bitrate. Whoever registered first used to win it
 * silently, which leaves the other driver deaf with nothing logged.
 *
 * This unit is the decision, kept apart from comm_can.cpp so it can be tested
 * on the host: comm_can.cpp reaches for the ESP32 CAN drivers and cannot be
 * linked into the unit tests.
 */

// One driver's request for an interface. `name` is a static-lifetime string, as
// the alloc_pins() component names are: it is kept so a conflict can name the
// parties rather than reporting a number.
struct CanReceiverRegistration {
  CanReceiver* receiver;
  CAN_Speed speed;
  const char* name;
};

typedef std::multimap<CAN_Interface, CanReceiverRegistration> CanReceiverRegistry;

/* The speed `interface` should be brought up at, or false if it must not be.
 *
 * False means one of two things, and the caller treats them the same way - the
 * interface is not started - while the user is told them apart by the event:
 * nothing registered for this interface (as today, silent), or two
 * registrations disagree, which raises EVENT_CAN_SPEED_CONFLICT and records
 * both parties for its message. `out` is written only when this returns true.
 */
bool resolve_interface_speed(const CanReceiverRegistry& registry, CAN_Interface interface, CAN_Speed& out);

/* The same question for one controller that answers to two interface keys.
 *
 * The MCP2518FD block serves both CANFD_NATIVE and CANFD_ADDON_MCP2518, so
 * registrations under the two keys land on the same wire and have to agree with
 * each other as well as among themselves. Returns false when either key's own
 * registrations disagree, when the two keys disagree with one another, or when
 * neither key has a registration.
 */
bool resolve_shared_interface_speed(const CanReceiverRegistry& registry, CAN_Interface primary, CAN_Interface secondary,
                                    CAN_Speed& out);

/* Whether a RUNTIME speed change on `interface` may go ahead.
 *
 * The same conflict, later: a driver that switches the bus it shares (BMW PHEV
 * moves the native bus between 100 and 500 kbit/s) deafens whoever else is on
 * it. A sole registrant owns its interface and is unaffected.
 *
 * `requester` is the driver asking, and its own registration is not counted
 * against it - that registration holds the speed it asked for at BOOT. Pass
 * nullptr for a caller that is not a registered receiver (the bus-off
 * recovery path). `requester_name` names it in the event and is a
 * static-lifetime string.
 */
bool can_speed_change_allowed(const CanReceiverRegistry& registry, CAN_Interface interface, CAN_Speed speed,
                              const CanReceiver* requester, const char* requester_name);

// The parties of the last conflict, for the event message - the shape
// EVENT_GPIO_CONFLICT already uses through failed_allocator()/conflicting_allocator().
CAN_Interface can_speed_conflict_interface();
const char* can_speed_conflict_first_name();
CAN_Speed can_speed_conflict_first_speed();
const char* can_speed_conflict_second_name();
CAN_Speed can_speed_conflict_second_speed();

#endif
