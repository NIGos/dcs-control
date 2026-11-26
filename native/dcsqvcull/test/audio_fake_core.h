// The test-only BinauralCore stand-in's controls (audio_fake_core.cpp) and the real-time allocation counter.
#pragma once

#include <atomic>
#include <cstdint>

#include "binaural_core.h"
#include "binaural_room.h"

namespace audiofake {
extern thread_local bool t_rtThread;          // set on the tests' simulation / render threads: allocations counted
extern std::atomic<uint64_t> g_rtAllocs;
extern std::atomic<bool> g_refuse;            // Render refuses every call (the module must fall back)
extern std::atomic<float> g_delayL, g_delayR;  // what SourceDelays reports (samples; the stand-in delays nothing)
extern std::atomic<uint32_t> g_spinUs;        // Render busy-waits this long (a slow core)
extern std::atomic<uint64_t> g_renders, g_resets, g_setHrtf, g_setRadius, g_setNear, g_begin, g_end;
extern std::atomic<int> g_liveSets;
extern float g_lastAdded[2 * binaural::kMaxFramesPerCall];  // what the last Render call added
extern uint32_t g_lastFrames;
extern float g_lastMono[binaural::kMaxFramesPerCall];       // its mono input
extern float g_lastRadius;
extern float g_lastEarRadius;  // the last Config.earRadiusM or SetEarRadius
extern float g_lastInitRadius;                 // Config::headRadiusM of the last Init
extern std::atomic<uint32_t> g_lastFlags;      // SourceUpdate::flags of the last Render call that was not refused
extern float g_lastGain;                       // SourceUpdate::gain of that call
}  // namespace audiofake

// The recording stand-in for the cockpit space's Room (binaural_room.h): every call counted, the last frame and each
// slot's last source kept. With g_roomOutput it adds kFakeRoomGain x the weighted sends (both ears) in Render and its
// RoomDirectGain is 1 / sqrt(1 + kFakeRoomEnergy x wet); without (the default) it adds nothing and its direct gain is 1,
// so every scenario written before the room keeps its exact expectations.
namespace audiofake {
constexpr float kFakeRoomGain = 0.25f;
constexpr float kFakeRoomEnergy = 0.25f;
extern std::atomic<bool> g_roomOutput;
extern std::atomic<uint64_t> g_roomInits, g_roomBegins, g_roomAdds, g_roomAddsWeighted, g_roomResets, g_roomRenders,
    g_roomEnds, g_roomSetParams, g_roomSetHrtf, g_roomRejected;
extern std::atomic<int> g_roomLive;              // Room objects alive
struct RoomSlotView {
  uint64_t adds;
  float pos[3];
  float send;
  uint32_t flags;
  float w0, w1;
};
extern RoomSlotView g_roomSlot[64];
extern binaural::RoomFrame g_roomLastFrame;
extern binaural::RoomParams g_roomLastParams;
extern float g_roomLastAdded[2 * binaural::kMaxFramesPerCall];  // what the last Render added
extern uint32_t g_roomLastFrames;
extern std::atomic<uint32_t> g_roomAddSpinUs;  // AddSource busy-waits this long (a slow room, per voice)
extern float g_roomLastEarlyMix, g_roomLastDirectCanopy;  // the last RoomDirectGainMix's early mix and canopy (-1: none)
void ResetRoom();                                // counters and views back to zero (not the Room objects)
}  // namespace audiofake

namespace binaural {
void FakePan(const float p[3], float* gl, float* gr);
}
