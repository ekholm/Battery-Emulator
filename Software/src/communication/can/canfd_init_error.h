#ifndef _CANFD_INIT_ERROR_H_
#define _CANFD_INIT_ERROR_H_

#include <stdint.h>

/* ACAN2517FD::begin() returns a BITMASK of one-hot error codes occupying bits 0
   through 20 (the kRequestedConfigurationModeTimeOut .. kInvalidTDCO block in
   ACAN2517FD.h). The event hub stores its custom data in an int16_t, so that
   mask cannot be handed to set_event() as it stands.

   Casting it, which is what this event used to do, is worse than lossy. A
   (uint8_t) cast keeps bits 0-7, so all thirteen codes from bit 8 up arrive in
   the event log as data 0 - and 0 is also what a reader takes for "this event
   carried no detail". Among those thirteen is kRequestedModeTimeOut (1 << 16),
   the one code begin() can return with the chip's interrupt already attached,
   i.e. the one a field report most needs to name.

   So report the ONE-BASED BIT INDEX instead. It fits the int16_t, it tells all
   twenty-one codes apart, and the one-based offset keeps 0 reserved for "no
   code" rather than colliding with kRequestedConfigurationModeTimeOut (1 << 0).

   What this gives up: begin() ORs codes together, and only the lowest set bit
   is reported. That is a real loss and it is bounded - the full mask is printed
   to the serial log on the line above each call site, and the codes that can
   accumulate are the settings-validation ones, which are a build-time fault
   where any one of them names the problem. Every code begin() raises after its
   `if (errorCode == 0)` guard, kRequestedModeTimeOut included, is necessarily
   alone in the mask and so is reported exactly. */
inline int16_t canfd_init_error_index(uint32_t error_code) {
  for (int16_t bit = 0; bit < 32; bit++) {
    if (error_code & (static_cast<uint32_t>(1) << bit)) {
      return static_cast<int16_t>(bit + 1);
    }
  }
  return 0;
}

#endif  // _CANFD_INIT_ERROR_H_
