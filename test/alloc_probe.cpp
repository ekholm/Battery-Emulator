#include "alloc_probe.h"

#include <cstdlib>
#include <new>

static size_t g_allocations = 0;
static bool g_armed = false;

void alloc_probe_reset() {
  g_allocations = 0;
  g_armed = true;
}

size_t alloc_probe_count() {
  g_armed = false;
  return g_allocations;
}

#ifndef __SANITIZE_ADDRESS__
void* operator new(size_t size) {
  if (g_armed) {
    g_allocations++;
  }
  void* p = std::malloc(size != 0 ? size : 1);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}

void* operator new[](size_t size) {
  return ::operator new(size);
}

void operator delete(void* p) noexcept {
  std::free(p);
}

void operator delete[](void* p) noexcept {
  std::free(p);
}

void operator delete(void* p, size_t) noexcept {
  std::free(p);
}

void operator delete[](void* p, size_t) noexcept {
  std::free(p);
}
#endif  // __SANITIZE_ADDRESS__
