// The test-only BinauralCore stand-in's controls (audio_fake_core.cpp) and the real-time allocation counter.
#pragma once

#include <atomic>
#include <cstdint>

#include "binaural_core.h"

namespace audiofake {
extern thread_local bool t_rtThread;          // set on the tests' simulation / render threads: allocations counted
extern std::atomic<uint64_t> g_rtAllocs;
extern std::atomic<bool> g_refuse;            // Render refuses every call (the module must fall back)
extern std::atomic<uint32_t> g_spinUs;        // Render busy-waits this long (a slow core)
extern std::atomic<uint64_t> g_renders, g_resets, g_setHrtf, g_setRadius, g_setNear, g_begin, g_end;
extern std::atomic<int> g_liveSets;
extern float g_lastAdded[2 * binaural::kMaxFramesPerCall];  // what the last Render call added
extern uint32_t g_lastFrames;
extern float g_lastMono[binaural::kMaxFramesPerCall];       // its mono input
extern float g_lastRadius;
}  // namespace audiofake

namespace binaural {
void FakePan(const float p[3], float* gl, float* gr);
}
