#ifndef INVERTER_TEST_UTILS_H
#define INVERTER_TEST_UTILS_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../Software/src/devboard/utils/types.h"

// Frame recorder provided by test/emul/can.cpp
void clear_transmitted_frames();
const std::vector<CAN_frame>& get_transmitted_frames();

inline const CAN_frame* find_frame_with_id(uint32_t id) {
  for (const auto& f : get_transmitted_frames()) {
    if (f.ID == id) {
      return &f;
    }
  }
  return nullptr;
}

// Last transmitted frame with the given ID (drivers often refresh the same
// frame across intervals; asserts usually want the newest one).
inline const CAN_frame* find_last_frame_with_id(uint32_t id) {
  const CAN_frame* found = nullptr;
  for (const auto& f : get_transmitted_frames()) {
    if (f.ID == id) {
      found = &f;
    }
  }
  return found;
}

inline size_t count_frames_with_id(uint32_t id) {
  size_t n = 0;
  for (const auto& f : get_transmitted_frames()) {
    if (f.ID == id) {
      n++;
    }
  }
  return n;
}

inline uint16_t u16_be(uint8_t hi, uint8_t lo) {
  return static_cast<uint16_t>((hi << 8) | lo);
}

inline uint16_t u16_le(uint8_t lo, uint8_t hi) {
  return static_cast<uint16_t>((hi << 8) | lo);
}

#endif  // INVERTER_TEST_UTILS_H
