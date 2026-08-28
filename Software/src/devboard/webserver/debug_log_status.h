#ifndef DEBUG_LOG_STATUS_H
#define DEBUG_LOG_STATUS_H

#include <string>

/* Explains an EMPTY debug-log buffer. "Web logging is off", "the CAN logger
   owns the buffer", and "logging is on but nothing was printed" are different
   claims - the debug writers run only while
   datalayer.system.info.web_logging_active is true AND can_logging_active is
   false (the two loggers share one RAM buffer, logging.cpp), so an empty
   readout with web logging off says nothing about the firmware being quiet.
   Rendering them identically is the same defect the CAN-log readout had,
   arriving on the debug path. Shared by the /log page and /export_log's RAM
   path so the two readouts cannot drift apart.

   The off-state wins over the CAN-ownership note: with web logging off the
   buffer would not fill either way, and WEBENABLED is only read at boot, so
   the reader is told about the reboot. */
std::string debug_log_empty_explanation(bool web_logging_active, bool can_logging_active);

#endif  // DEBUG_LOG_STATUS_H
