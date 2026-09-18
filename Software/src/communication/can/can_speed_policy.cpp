#include "can_speed_policy.h"

#include <iterator>

#include "../../devboard/utils/events.h"

namespace {

/* The last conflict, kept for the event message.
 *
 * set_event() logs the message string immediately, so this is written BEFORE
 * set_event() is called - the same ordering alloc_pins() documents for
 * allocator_name.
 */
struct SpeedConflict {
  CAN_Interface interface;
  const char* first_name;
  CAN_Speed first_speed;
  const char* second_name;
  CAN_Speed second_speed;
};

SpeedConflict conflict = {NO_CAN_INTERFACE, "", CAN_Speed::CAN_SPEED_500KBPS, "", CAN_Speed::CAN_SPEED_500KBPS};

void raise_conflict(CAN_Interface interface, const char* first_name, CAN_Speed first_speed, const char* second_name,
                    CAN_Speed second_speed) {
  conflict = {interface, first_name, first_speed, second_name, second_speed};
  set_event(EVENT_CAN_SPEED_CONFLICT, (int16_t)interface);
}

// The registrations for one key, checked against each other. Returns false and
// raises the event on the first disagreement; `out` is untouched then.
bool agreed_speed(const CanReceiverRegistry& registry, CAN_Interface interface, CAN_Speed& out) {
  auto range = registry.equal_range(interface);
  if (range.first == range.second) {
    return false;  // Nothing registered - not configured, which is not a conflict.
  }

  const CanReceiverRegistration& first = range.first->second;
  for (auto it = std::next(range.first); it != range.second; ++it) {
    if (it->second.speed != first.speed) {
      raise_conflict(interface, first.name, first.speed, it->second.name, it->second.speed);
      return false;
    }
  }

  out = first.speed;
  return true;
}

}  // namespace

bool resolve_interface_speed(const CanReceiverRegistry& registry, CAN_Interface interface, CAN_Speed& out) {
  return agreed_speed(registry, interface, out);
}

bool resolve_shared_interface_speed(const CanReceiverRegistry& registry, CAN_Interface primary, CAN_Interface secondary,
                                    CAN_Speed& out) {
  CAN_Speed primary_speed;
  CAN_Speed secondary_speed;
  const bool has_primary = registry.find(primary) != registry.end();
  const bool has_secondary = registry.find(secondary) != registry.end();

  // Each key has to be coherent on its own before the two are compared, and a
  // key that is not must not be papered over by the other one agreeing.
  const bool primary_ok = has_primary && agreed_speed(registry, primary, primary_speed);
  const bool secondary_ok = has_secondary && agreed_speed(registry, secondary, secondary_speed);

  if (has_primary && !primary_ok) {
    return false;
  }
  if (has_secondary && !secondary_ok) {
    return false;
  }

  if (primary_ok && secondary_ok && primary_speed != secondary_speed) {
    raise_conflict(primary, registry.find(primary)->second.name, primary_speed, registry.find(secondary)->second.name,
                   secondary_speed);
    return false;
  }

  if (primary_ok) {
    out = primary_speed;
    return true;
  }
  if (secondary_ok) {
    out = secondary_speed;
    return true;
  }
  return false;
}

bool can_speed_change_allowed(const CanReceiverRegistry& registry, CAN_Interface interface, CAN_Speed speed,
                              const CanReceiver* requester, const char* requester_name) {
  auto range = registry.equal_range(interface);

  for (auto it = range.first; it != range.second; ++it) {
    // The requester's own registration says the speed it asked for at boot, not
    // the one it is asking for now, so it is never its own peer. Identity is the
    // receiver pointer: two packs of the same battery type share a name.
    if (it->second.receiver == requester) {
      continue;
    }
    if (it->second.speed != speed) {
      raise_conflict(interface, it->second.name, it->second.speed, requester_name, speed);
      return false;
    }
  }

  return true;
}

CAN_Interface can_speed_conflict_interface() {
  return conflict.interface;
}

const char* can_speed_conflict_first_name() {
  return conflict.first_name;
}

CAN_Speed can_speed_conflict_first_speed() {
  return conflict.first_speed;
}

const char* can_speed_conflict_second_name() {
  return conflict.second_name;
}

CAN_Speed can_speed_conflict_second_speed() {
  return conflict.second_speed;
}
