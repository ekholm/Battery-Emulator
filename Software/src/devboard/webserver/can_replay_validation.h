#ifndef CAN_REPLAY_VALIDATION_H
#define CAN_REPLAY_VALIDATION_H

#include <stddef.h>

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

/* Validates the DLC a replay log line DECLARES, before any of its data bytes
   are copied.

   The parser read `[n]` straight out of the uploaded line into CAN_frame's
   uint8_t DLC and then copied that many space-separated tokens into a 64-byte
   array. uint8_t admits 0..255, so a line declaring 200 bytes and supplying
   200 tokens wrote 136 bytes past the end of a FILE-SCOPE frame - into .bss,
   from content that arrives entirely through the log upload.

   Two failures are separated deliberately. A value that does not fit the frame
   is a bad line and the line is refused; TRUNCATING it would replay a frame the
   file did not describe, which is worse than skipping one. And the check is on
   the value as PARSED, not after it is narrowed - `[300]` narrows to 44 in a
   uint8_t, so a check after the assignment would accept a line the file never
   meant and silently change its length.

   `capacity` is sizeof(CAN_frame::data.u8), passed in so this stays free of
   the firmware's types and the bound cannot drift from the array it protects.

   Returns "" when the DLC is usable; otherwise why the line was refused. */
std::string can_replay_dlc_rejection(long declared, size_t capacity);

/* Parses the space-separated hex bytes of a replay line's data field into `out`
   (which must hold at least `declared` bytes) and returns how many were parsed.

   It exists to take the strtok scan OUT of the webserver, where nothing on the
   host could see it. The bug it retires: the parser called
   `strtok((char*)dataBytes.c_str(), " ")`, and a line that ends at or just past
   the `]` gives a default-constructed String whose c_str()/begin() is NULL.
   strtok(NULL, ...) does not START a scan - it CONTINUES the previous one, whose
   buffer was the last line's data field, freed when it went out of scope. So a
   NULL or empty field would feed strtol() freed heap and put it on the wire.
   Here a NULL/empty field parses zero bytes and strtok is never handed NULL.

   `data_field` is mutated in place (strtok writes NULs over the delimiters), so
   it must be the line's own writable buffer - String::begin(), not a const cast
   of c_str(). The caller compares the return against the declared DLC: fewer
   bytes than declared means the line promised more than it supplied, and it is
   skipped rather than truncated - the same refuse-don't-truncate rule as the
   DLC bound, and the reason a valid zero-length ([0]) frame still transmits
   while a [8]-with-no-data line does not. */
size_t can_replay_parse_data(char* data_field, size_t declared, unsigned char* out);

#endif  // CAN_REPLAY_VALIDATION_H
