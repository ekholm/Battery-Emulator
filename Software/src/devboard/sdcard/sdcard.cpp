#include "sdcard.h"

#ifdef SDCARD

#include "freertos/ringbuf.h"

File can_log_file;
File log_file;
RingbufHandle_t can_bufferHandle;
RingbufHandle_t log_bufferHandle;

bool can_logging_paused = false;
bool can_file_open = false;
bool delete_can_file = false;

bool logging_paused = false;
bool log_file_open = false;
bool delete_log_file = false;

bool sd_card_active = false;

/* Every SD failure below used to be discarded.
 *
 * EVENT_SD_INIT_FAILED guards the MOUNT only, so anything that killed the card
 * AFTER mounting - the SPI bus collision fixed alongside this, a pulled card,
 * one, a bad connector - left the user with an empty or truncated CAN log and no
 * event anywhere. `SD.open()` hands back a File that is falsy when the open
 * failed and `write()` returns what it actually wrote; both were dropped on the
 * floor. `flush()` returns void, so it cannot report and is not checked.
 *
 * LATCHED on purpose - but what prevents log spam is NOT the latching.
 * set_event_internal() emits a log line only on the inactive->active edge, and
 * an event stays active however it was set, so a plain set_event() would also
 * log once and then only bump the occurrence count. What latched actually buys
 * is that clear_event() cannot clear it: the record of a failure whose evidence
 * is MISSING data survives until the user resets the events page, the same
 * shape as EVENT_SD_INIT_FAILED beside it.
 *
 * WARNING, not ERROR, matching EVENT_SD_INIT_FAILED beside it: update_bms_status()
 * turns any active error into system_status = FAULT, and losing an optional log
 * is not a reason to fault a running emulator.
 */
static void report_sd_write_failure() {
  set_event_latched(EVENT_SD_WRITE_FAILED, 0);
}

/* Open a log file and say whether it worked.
 *
 * The `is_open` flag was previously set to true unconditionally next to every
 * SD.open(), which is what turned ONE failed open into an unbounded run of
 * writes to an invalid File - each of them also silent.
 */
static bool open_sd_log_file(File& file, const char* path, bool& is_open) {
  file = SD.open(path, FILE_APPEND);
  is_open = (bool)file;
  if (!is_open) {
    report_sd_write_failure();
  }
  return is_open;
}

void delete_can_log() {
  can_logging_paused = true;
  delete_can_file = true;
}

void resume_can_writing() {
  can_logging_paused = false;
  open_sd_log_file(can_log_file, CAN_LOG_FILE, can_file_open);
}

void pause_can_writing() {
  can_logging_paused = true;
}

void delete_log() {
  logging_paused = true;
  if (log_file_open) {
    log_file.close();
    log_file_open = false;
  }
  SD.remove(LOG_FILE);
  logging_paused = false;
}

void resume_log_writing() {
  logging_paused = false;
  open_sd_log_file(log_file, LOG_FILE, log_file_open);
}

void pause_log_writing() {
  logging_paused = true;
}

// Number of frames lost because the ring buffer was full (SD writer stalled).
// Reported as a gap marker in the log once the buffer has room again.
static uint32_t can_frames_dropped = 0;

void add_can_frame_to_buffer(CAN_frame frame, CAN_Interface interface, frameDirection msgDir) {

  if (!sd_card_active)
    return;

  // Sized for the worst case: gap marker + header + 64 data bytes (CAN-FD) at 3 chars each
  static char messagestr_buffer[320];
  size_t size = 0;

  if (can_frames_dropped > 0) {
    // Frames were lost while the SD writer was stalled. Record the gap so the
    // log stays honest, then continue with the current frame in the same send.
    size += snprintf(messagestr_buffer + size, sizeof(messagestr_buffer) - size,
                     "[%lu CAN frames dropped, SD buffer full]\n", (unsigned long)can_frames_dropped);
  }

  // Format the frame, as long as it fits
  size_t written =
      format_can_frame(messagestr_buffer + size, sizeof(messagestr_buffer) - size, frame, interface, msgDir);
  if (written > 0) {
    size += written;
  }

  // One send per frame, zero timeout: this runs in the core task and must never
  // block. If the buffer is full the SD card is stalled anyway - waiting here
  // would not save the frame, it would only delay the 10ms tasks (EVENT_TASK_OVERRUN).
  if (xRingbufferSend(can_bufferHandle, messagestr_buffer, size, 0) != pdTRUE) {
    can_frames_dropped++;
    return;
  }
  can_frames_dropped = 0;
}

void write_can_frame_to_sdcard() {

  if (!sd_card_active)
    return;

  size_t receivedMessageSize;
  uint8_t* buffer = (uint8_t*)xRingbufferReceive(can_bufferHandle, &receivedMessageSize, pdMS_TO_TICKS(10));

  if (buffer != NULL) {

    if (can_logging_paused) {
      if (can_file_open) {
        can_log_file.close();
        can_file_open = false;
      }
      if (delete_can_file) {
        SD.remove(CAN_LOG_FILE);
        delete_can_file = false;
        can_logging_paused = false;
      }
      vRingbufferReturnItem(can_bufferHandle, (void*)buffer);
      return;
    }

    if (can_file_open == false) {
      if (!open_sd_log_file(can_log_file, CAN_LOG_FILE, can_file_open)) {
        // Stop through the existing pause machinery rather than retrying every
        // pass: the card is not going to come back on its own. Nothing offers a
        // bare "resume" control today - what actually re-opens the file
        // is exporting or deleting the log (both routes end in resume) or a
        // reboot, and the event text names those.
        can_logging_paused = true;
        vRingbufferReturnItem(can_bufferHandle, (void*)buffer);
        return;
      }
    }

    const size_t can_written = can_log_file.write(buffer, receivedMessageSize);
    can_log_file.flush();
    if (can_written != receivedMessageSize) {
      report_sd_write_failure();
      can_log_file.close();
      can_file_open = false;
      can_logging_paused = true;
    }

    vRingbufferReturnItem(can_bufferHandle, (void*)buffer);
  }
}

void add_log_to_buffer(const uint8_t* buffer, size_t size) {

  if (!sd_card_active)
    return;

  // Zero timeout: called from the logging path of any task, must never block.
  // NOTE: do not log from the failure path here. logging.println() would call
  // Logging::write() -> add_log_to_buffer() again while the buffer is still
  // full, recursing until the stack overflows.
  xRingbufferSend(log_bufferHandle, buffer, size, 0);
}

void write_log_to_sdcard() {

  if (!sd_card_active)
    return;

  size_t receivedMessageSize;
  uint8_t* buffer = (uint8_t*)xRingbufferReceive(log_bufferHandle, &receivedMessageSize, pdMS_TO_TICKS(10));

  if (buffer != NULL) {

    if (logging_paused) {
      vRingbufferReturnItem(log_bufferHandle, (void*)buffer);
      return;
    }

    if (log_file_open == false) {
      if (!open_sd_log_file(log_file, LOG_FILE, log_file_open)) {
        logging_paused = true;
        vRingbufferReturnItem(log_bufferHandle, (void*)buffer);
        return;
      }
    }

    const size_t log_written = log_file.write(buffer, receivedMessageSize);
    log_file.flush();
    if (log_written != receivedMessageSize) {
      report_sd_write_failure();
      log_file.close();
      log_file_open = false;
      logging_paused = true;
    }

    vRingbufferReturnItem(log_bufferHandle, (void*)buffer);
  }
}

void init_logging_buffers() {

  if (datalayer.system.info.CAN_SD_logging_active) {
    can_bufferHandle = xRingbufferCreate(32 * 1024, RINGBUF_TYPE_BYTEBUF);
    if (can_bufferHandle == NULL) {
      logging.println("Failed to create CAN ring buffer!");
      return;
    }
  }

  if (datalayer.system.info.SD_logging_active) {
    log_bufferHandle = xRingbufferCreate(1024, RINGBUF_TYPE_BYTEBUF);
    if (log_bufferHandle == NULL) {
      logging.println("Failed to create log ring buffer!");
      return;
    }
  }
}

void deinit_logging_buffers() {
  if ((!datalayer.system.info.CAN_SD_logging_active) && (!datalayer.system.info.SD_logging_active)) {
    if (can_bufferHandle != NULL) {
      vRingbufferDelete(can_bufferHandle);
    }
    if (log_bufferHandle != NULL) {
      vRingbufferDelete(log_bufferHandle);
    }
  }
}

bool init_sdcard() {
  auto miso_pin = esp32hal->SD_MISO_PIN();
  auto mosi_pin = esp32hal->SD_MOSI_PIN();
  auto sclk_pin = esp32hal->SD_SCLK_PIN();
  auto cs_pin = esp32hal->SD_CS_PIN();

  if (!esp32hal->alloc_pins("SD Card", miso_pin, mosi_pin, sclk_pin, cs_pin)) {
    return false;
  }

  static SPIClass sd_spi(esp32hal->SD_SPI_BUS());
  sd_spi.begin(sclk_pin, miso_pin, mosi_pin, cs_pin);

  constexpr uint32_t SD_SPI_FREQ = 20 * 1000000;  // 20 MHz
  constexpr uint8_t SD_MAX_OPEN_FILES = 5;        // library default
  constexpr bool FORMAT_IF_EMPTY = true;

  if (!SD.begin(cs_pin, sd_spi, SD_SPI_FREQ, "/root", SD_MAX_OPEN_FILES, FORMAT_IF_EMPTY)) {
    set_event_latched(EVENT_SD_INIT_FAILED, 0);  // also printing a log entry
    return false;
  }

  clear_event(EVENT_SD_INIT_FAILED);
  logging.println("SD Card initialization successful.");

  sd_card_active = true;

  log_sdcard_details();

  return true;
}

void log_sdcard_details() {

  logging.print("SD Card Type: ");
  switch (SD.cardType()) {
    case CARD_MMC:
      logging.println("MMC");
      break;
    case CARD_SD:
      logging.println("SD");
      break;
    case CARD_SDHC:
      logging.println("SDHC");
      break;
    case CARD_UNKNOWN:
      logging.println("UNKNOWN");
      break;
    case CARD_NONE:
      logging.println("No SD Card found");
      break;
  }

  if (SD.cardType() != CARD_NONE) {
    logging.print("SD Card Size: ");
    logging.print(SD.cardSize() / 1024 / 1024);
    logging.println(" MB");

    logging.print("Total space: ");
    logging.print(SD.totalBytes() / 1024 / 1024);
    logging.println(" MB");

    logging.print("Used space: ");
    logging.print(SD.usedBytes() / 1024 / 1024);
    logging.println(" MB");
  }
}

#endif  // SDCARD
