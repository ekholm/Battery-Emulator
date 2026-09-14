#pragma once

// Emulated ESP-IDF capability heap, for host tests that compile a vendored
// header which asks for its placement (ACAN_ESP32_Buffer16). It allocates from
// the host heap and records what it was asked for, so a test can hold the
// caller to internal DRAM and make the next allocation fail.

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#define MALLOC_CAP_EXEC (1 << 0)
#define MALLOC_CAP_32BIT (1 << 1)
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DMA (1 << 3)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DEFAULT (1 << 12)

namespace emul_heap {
inline uint32_t last_caps = 0;
inline size_t last_size = 0;
inline int allocations = 0;
inline int frees = 0;
inline bool fail_next = false;

inline void reset() {
  last_caps = 0;
  last_size = 0;
  allocations = 0;
  frees = 0;
  fail_next = false;
}
}  // namespace emul_heap

inline void* heap_caps_malloc(size_t size, uint32_t caps) {
  emul_heap::last_caps = caps;
  emul_heap::last_size = size;
  if (emul_heap::fail_next) {
    emul_heap::fail_next = false;
    return nullptr;
  }
  emul_heap::allocations++;
  return std::malloc(size);
}

inline void heap_caps_free(void* ptr) {
  if (ptr != nullptr) {
    emul_heap::frees++;
  }
  std::free(ptr);
}
