// STAND-IN for native/binaural/include/binaural_room.h (CORE, branch feat/cspace-core): the interface of
// docs/COCKPIT_SPACE.md 8.1, verbatim. The audio module builds against it only while the core's header does not exist
// (CMakeLists.txt adds this folder after native/binaural/include, and only then). With this stand-in the payload has no
// Room implementation (DCSQV_HAVE_ROOM=0: no Room is ever created, the output is today's); the module's tests link a
// recording fake (test/audio_fake_core.cpp). Once CORE is merged its header and src/room.cpp are used and this folder goes.
#pragma once

#include <cstddef>
#include <cstdint>

#include "binaural_core.h"
#include "cockpit_space.h"  // Surface, RoomParams, Family, presets (pure)

namespace binaural {

using cockpit_space::RoomParams;

constexpr std::uint32_t kRoomBuses = 14;         // 6 axes + 8 cube corners, aircraft frame
constexpr std::uint32_t kRoomTaps = 8;           // 6 first-order walls + 2 canopy scatter (up-left, up-right)
constexpr std::uint32_t kRoomFdnLines = 8;
constexpr float kRoomMaxTapDelayMs = 12.0f;      // per-voice delay line: 12 ms at 96 kHz (next power of two)
constexpr float kRoomBusDistanceM = 2.0f;        // buses are far-field sources (kNoNearField)
constexpr float kRoomParamsCrossfadeMs = 100.0f; // SetParams: old set -> new set
constexpr float kRoomRefDistanceM = 0.6f;

namespace room_flags {
constexpr std::uint32_t kReset = 1u;          // a new voice in this slot (tap lines and filter states cleared)
constexpr std::uint32_t kThroughCanopy = 2u;  // outside voice heard through the canopy: excitation on the box, throughDb
constexpr std::uint32_t kLateOnly = 4u;       // diffuse bed: no early taps, late send -3 dB
}  // namespace room_flags

// Per block, set by the audio module at BeginBlock (audio thread). Every value is the target at the block's END;
// the room ramps from the previous block's values across this block.
struct RoomFrame {
  float aircraftToHead[4];  // unit quaternion (x, y, z, w): aircraft frame -> head frame (x fwd, y up, z right)
  bool frameValid;          // false: early reflections fade out (late only), the rotation is ignored
  float wet;                // 0..1: inside x (module on, kill switch off); strength is in RoomParams
  float canopyOpen;         // 0..1 (0 when unknown or CockpitSpaceCanopy=0)
  bool early;               // false: early reflections fade out (CPU back-off step 1)
};

// Per owned voice per block (audio thread), the target at the block's end.
struct RoomSource {
  float posAircraft[3];  // aircraft frame, head at origin, metres (module: conj(C) applied to the corrected pos)
  float send;            // linear: the voice's HRTF gain (u.gain without RoomDirectGain) x the distance law
  std::uint32_t flags;   // room_flags
};

struct RoomStats {
  std::uint64_t blocks, sourcesAdded, earlyOffBlocks, frameInvalidBlocks, nonFinite, rejected, idleBlocks;
  std::uint64_t lastBlockNs, worstBlockNs;
  float lastWet, lastCanopy;
};

class Room {
 public:
  Room();
  ~Room();
  Room(const Room&) = delete;
  Room& operator=(const Room&) = delete;

  // ---- control thread -----------------------------------------------------------------------------------------------
  bool Init(std::uint32_t sampleRate, std::uint32_t maxSources, char* error, std::size_t errorCapacity);
  void SetHrtf(const HrtfSet* set);
  const HrtfSet* TakeRetiredHrtf();
  void SetHeadRadius(float metres);
  void SetParams(const RoomParams& p);

  // ---- audio thread -------------------------------------------------------------------------------------------------
  void BeginBlock(std::uint32_t frames, const RoomFrame& f);
  bool AddSource(std::uint32_t slot, const RoomSource& s, const MonoInput& in, std::uint32_t frames, float w0, float w1);
  void ResetSource(std::uint32_t slot);
  void Render(float* outLR, std::uint32_t frames);
  void EndBlock();
  bool Idle() const;

  // ---- any thread ---------------------------------------------------------------------------------------------------
  RoomStats GetStats() const;
  void ResetStats();

 private:
  struct Impl;
  Impl* impl_;
};

float RoomDirectGain(const RoomParams& p, float wet, float canopyOpen, bool early, const float posAircraft[3],
                     std::uint32_t flags);
float RoomSendDistanceGain(const RoomParams& p, const float posAircraft[3], std::uint32_t flags);

}  // namespace binaural
