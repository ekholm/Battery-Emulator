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
 * what forced active_hostname() to build a fresh String from bytes that already existed. */
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
