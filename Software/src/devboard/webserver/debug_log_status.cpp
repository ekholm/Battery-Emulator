#include "debug_log_status.h"

std::string debug_log_empty_explanation(bool web_logging_active, bool can_logging_active) {
  if (!web_logging_active) {
    return "Web logging is off - debug messages are only recorded while it is on. "
           "Enable 'General logging via Webserver' in the settings and reboot.";
  }
  if (can_logging_active) {
    return "The CAN logger currently owns the shared log buffer - debug messages are "
           "not recorded while it is active. Stop it from the CAN logger page to resume "
           "debug logging.";
  }
  return "Web logging is on, but nothing has been logged since it started.";
}
