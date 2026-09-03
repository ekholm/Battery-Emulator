#pragma once

#include <WString.h>
#include <stdint.h>

// User-configured hostname. Loaded from NVM ("HOSTNAME"); empty when unset
extern String custom_hostname;

/* Both of these hand back a reference to storage that outlives every caller: the MAC-derived
 * default is a function-local static built once, and the custom name is a global that only
 * settings loading writes. They used to return by value, which copied 21+ bytes onto the heap
 * on every call - "battery-emulator-" plus four hex digits is well past the 13 characters an
 * Arduino String keeps inline - and syslog_send() calls active_hostname().c_str() once per
 * LINE. Callers that want their own copy still get one by assigning to a String.
 *
 * custom_hostname is a String rather than a std::string for the same reason: the mixture was
 * what forced active_hostname() to build a fresh String from bytes that already existed.
 *
 * WHAT MAKES THE REFERENCE SAFE, so it is clear what would break it: custom_hostname is
 * written in exactly one place, init_stored_settings(), which setup() calls once BEFORE it
 * creates any task - so no reader can observe a write, and there is no reassignment at
 * runtime at all. The settings page saves "HOSTNAME" to NVM but deliberately does not touch
 * this global, which is why a hostname change needs a reboot to take effect.
 *
 * So: making the hostname apply WITHOUT a reboot is the change that turns these references
 * into a hazard, because String::operator= frees the old buffer and any caller holding the
 * reference - or a c_str() taken from it - is left dangling, from another task. Every call
 * site today either copies immediately or consumes the reference within one expression; if
 * live update is ever added, they must be re-audited and this should hand back a copy (or the
 * write must be made under the same lock every reader takes). */
/* The name a MAC maps to, as a pure function of the six bytes. Split out from the cached
 * accessor below because that one reads the eFuse once and remembers the answer, which makes
 * the FORMAT - lowercase, two digits per byte, leading zeroes kept - impossible to check
 * through it: a second call with a different MAC returns the first answer, correctly. This is
 * what goes on the wire, so it is worth being able to test. */
String hostname_for_mac(const uint8_t mac[6]);

const String& default_hostname();

// Returns the effective hostname: the user's custom_hostname if set, otherwise
// default_hostname().
const String& active_hostname();
