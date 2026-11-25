// The allocation counter of the binaural audio tests (DcsQvCullTests and DcsQvCullAudioE2ETests): operator new / delete
// counted on threads that set audiofake::t_rtThread (the tests' simulation and render threads).
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>

#include "audio_fake_core.h"

namespace audiofake {
thread_local bool t_rtThread = false;
std::atomic<uint64_t> g_rtAllocs{0};
}  // namespace audiofake

void* operator new(size_t n) {
  if (audiofake::t_rtThread) audiofake::g_rtAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](size_t n) {
  if (audiofake::t_rtThread) audiofake::g_rtAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new(size_t n, const std::nothrow_t&) noexcept {
  if (audiofake::t_rtThread) audiofake::g_rtAllocs.fetch_add(1, std::memory_order_relaxed);
  return malloc(n ? n : 1);
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept {
  if (audiofake::t_rtThread) audiofake::g_rtAllocs.fetch_add(1, std::memory_order_relaxed);
  return malloc(n ? n : 1);
}
void operator delete(void* p) noexcept {
  if (p && audiofake::t_rtThread) audiofake::g_rtAllocs.fetch_add(1, std::memory_order_relaxed);
  free(p);
}
void operator delete[](void* p) noexcept {
  if (p && audiofake::t_rtThread) audiofake::g_rtAllocs.fetch_add(1, std::memory_order_relaxed);
  free(p);
}
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete[](p); }

