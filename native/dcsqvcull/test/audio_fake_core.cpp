// Test-only stand-in for BinauralCore (native/binaural/include/binaural_core.h), linked into DcsQvCullTests: the
// module's own tests check the module (ownership, fades, fallbacks, refusals), not the DSP, and need a core whose
// output they can predict and whose refusals and slowness they control. The real core is linked into the payload and
// into DcsQvCullAudioE2ETests (audio_e2e_test.cpp). Behaviour: an equal-power left/right pan from the source's position
// (z right), the call's gain, interpolated linearly across each call from the previous call's state (kReset: no
// interpolation), added with the ownership weight w0 -> w1. Same thread rules and refusals as the contract (frames >
// kMaxFramesPerCall, slot out of range, no set). The allocation counter is in audio_rt_alloc.cpp.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>

#include "audio_fake_core.h"
#include "binaural_core.h"

namespace audiofake {
std::atomic<bool> g_refuse{false};
std::atomic<uint32_t> g_spinUs{0};
std::atomic<float> g_delayL{0.0f}, g_delayR{0.0f};
std::atomic<uint64_t> g_renders{0}, g_resets{0}, g_setHrtf{0}, g_setRadius{0}, g_setNear{0}, g_begin{0}, g_end{0};
std::atomic<int> g_liveSets{0};
float g_lastAdded[2 * binaural::kMaxFramesPerCall];
uint32_t g_lastFrames = 0;
float g_lastMono[binaural::kMaxFramesPerCall];
float g_lastRadius = 0;
float g_lastEarRadius = 0;
float g_lastInitRadius = 0;
std::atomic<uint32_t> g_lastFlags{0};
float g_lastGain = 0;
}  // namespace audiofake

namespace binaural {

struct HrtfSet {
  uint32_t rate;
  uint32_t directions;
  char name[64];
};

HrtfSet* LoadHrtf(const void* data, std::size_t bytes, std::uint32_t deviceRate, char* error, std::size_t errorCapacity) {
  if (!data || bytes < 4 || deviceRate < kMinSampleRate || deviceRate > kMaxSampleRate) {
    if (errorCapacity) strncpy_s(error, errorCapacity, "fake core: bad set", _TRUNCATE);
    return nullptr;
  }
  auto* s = new HrtfSet{};
  s->rate = deviceRate;
  s->directions = static_cast<uint32_t>(bytes / 4);
  memcpy(s->name, data, bytes < 63 ? bytes : 63);
  audiofake::g_liveSets.fetch_add(1);
  return s;
}
void ReleaseHrtf(HrtfSet* set) {
  if (!set) return;
  audiofake::g_liveSets.fetch_sub(1);
  delete set;
}
HrtfInfo DescribeHrtf(const HrtfSet* set) {
  HrtfInfo i{};
  if (set) {
    i.directions = set->directions;
    i.taps = 128;
    i.sampleRate = set->rate;
    i.datasetHeadRadiusM = 0.0875f;
    memcpy(i.name, set->name, sizeof(i.name));
  }
  return i;
}

struct Renderer::Impl {
  Config cfg{};
  std::atomic<const HrtfSet*> set{nullptr};
  std::atomic<const HrtfSet*> retired{nullptr};
  struct Src {
    bool have = false;
    float gl = 0, gr = 0, gain = 0;
  } src[kMaxSources];
  std::atomic<uint64_t> blocks{0}, renders{0}, rejected{0}, nonFinite{0};
  std::atomic<float> radius{0};
};

Renderer::Renderer() : impl_(new Impl) {}
Renderer::~Renderer() { delete impl_; }
bool Renderer::Init(const Config& c, char* error, std::size_t errorCapacity) {
  if (c.sampleRate < kMinSampleRate || c.sampleRate > kMaxSampleRate || !c.maxSources || c.maxSources > kMaxSources) {
    if (errorCapacity) strncpy_s(error, errorCapacity, "fake core: bad config", _TRUNCATE);
    return false;
  }
  impl_->cfg = c;
  impl_->radius = c.headRadiusM;
  audiofake::g_lastInitRadius = c.headRadiusM;
  audiofake::g_lastEarRadius = c.earRadiusM;
  return true;
}
void Renderer::SetHrtf(const HrtfSet* s) {
  audiofake::g_setHrtf.fetch_add(1);
  const HrtfSet* old = impl_->set.exchange(s);
  if (old && old != s) impl_->retired.store(old);
}
const HrtfSet* Renderer::TakeRetiredHrtf() { return impl_->retired.exchange(nullptr); }
void Renderer::SetHeadRadius(float m) {
  audiofake::g_setRadius.fetch_add(1);
  audiofake::g_lastRadius = m;
  impl_->radius = m;
}
void Renderer::SetEarRadius(float m) { audiofake::g_lastEarRadius = m; }
void Renderer::SetNearField(bool) { audiofake::g_setNear.fetch_add(1); }
void Renderer::BeginBlock(std::uint32_t) {
  audiofake::g_begin.fetch_add(1, std::memory_order_relaxed);
  impl_->blocks.fetch_add(1, std::memory_order_relaxed);
}
void Renderer::EndBlock() { audiofake::g_end.fetch_add(1, std::memory_order_relaxed); }

void FakePan(const float p[3], float* gl, float* gr) {
  const double r = std::sqrt(double(p[0]) * p[0] + double(p[1]) * p[1] + double(p[2]) * p[2]);
  const double s = r > 1e-9 ? p[2] / r : 0.0;  // -1 left .. +1 right
  *gl = static_cast<float>(std::sqrt((1.0 - s) * 0.5));
  *gr = static_cast<float>(std::sqrt((1.0 + s) * 0.5));
}

bool Renderer::Render(std::uint32_t slot, const SourceUpdate& u, const MonoInput& in, std::uint32_t frames, float w0,
                      float w1, float* outLR) {
  if (audiofake::g_refuse.load(std::memory_order_relaxed) || slot >= impl_->cfg.maxSources || frames == 0 ||
      frames > kMaxFramesPerCall || !impl_->set.load(std::memory_order_relaxed)) {
    impl_->rejected.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (const uint32_t us = audiofake::g_spinUs.load(std::memory_order_relaxed)) {
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    do QueryPerformanceCounter(&b);
    while ((b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart < us);
  }
  audiofake::g_lastFlags.store(u.flags, std::memory_order_relaxed);
  audiofake::g_lastGain = u.gain;
  Impl::Src& s = impl_->src[slot];
  float gl1, gr1;
  FakePan(u.position, &gl1, &gr1);
  const float g1 = u.gain;
  if (!s.have || (u.flags & source_flags::kReset)) {
    s.gl = gl1, s.gr = gr1, s.gain = g1;
    s.have = true;
  }
  for (uint32_t i = 0; i < frames; ++i) {
    const float x = in.scale * (in.a[i * in.stride] + (in.b ? in.b[i * in.stride] : 0.0f));
    if (!std::isfinite(x)) {
      s.have = false;
      impl_->nonFinite.fetch_add(1, std::memory_order_relaxed);
      return true;  // the contract: the source is reset and nothing is added for this call
    }
  }
  const float inv = 1.0f / static_cast<float>(frames);
  audiofake::g_lastFrames = frames;
  for (uint32_t i = 0; i < frames; ++i) {
    const float t = static_cast<float>(i) * inv;
    const float w = w0 + (w1 - w0) * t;
    const float gain = s.gain + (g1 - s.gain) * t;
    const float l = s.gl + (gl1 - s.gl) * t, r = s.gr + (gr1 - s.gr) * t;
    const float x = in.scale * (in.a[i * in.stride] + (in.b ? in.b[i * in.stride] : 0.0f));
    audiofake::g_lastMono[i] = x;
    const float al = w * gain * l * x, ar = w * gain * r * x;
    audiofake::g_lastAdded[2 * i] = al;
    audiofake::g_lastAdded[2 * i + 1] = ar;
    outLR[2 * i] += al;
    outLR[2 * i + 1] += ar;
  }
  s.gl = gl1, s.gr = gr1, s.gain = g1;
  impl_->renders.fetch_add(1, std::memory_order_relaxed);
  audiofake::g_renders.fetch_add(1, std::memory_order_relaxed);
  return true;
}
void Renderer::ResetSource(std::uint32_t slot) {
  if (slot < kMaxSources) impl_->src[slot].have = false;
  audiofake::g_resets.fetch_add(1, std::memory_order_relaxed);
}
// The stand-in adds no delay of its own; the tests set the delays it reports (0 by default: the side unshifted).
bool Renderer::SourceDelays(std::uint32_t slot, float out[2]) const {
  if (slot >= kMaxSources || !impl_->src[slot].have) return false;
  out[0] = audiofake::g_delayL.load(std::memory_order_relaxed);
  out[1] = audiofake::g_delayR.load(std::memory_order_relaxed);
  return true;
}
Stats Renderer::GetStats() const {
  Stats s{};
  s.blocks = impl_->blocks.load();
  s.renders = impl_->renders.load();
  s.rejected = impl_->rejected.load();
  s.nonFinite = impl_->nonFinite.load();
  return s;
}
void Renderer::ResetStats() {}
std::uint32_t Renderer::SampleRate() const { return impl_->cfg.sampleRate; }

}  // namespace binaural

// ---- the cockpit space's Room (binaural_room.h): a recording fake -----------------------------------------------------
// Same thread rules as the contract: Init / SetHrtf / SetParams on the control thread (may allocate), the rest on the
// render thread (no allocation: the accumulation buffer is allocated by Init). AddSource refuses a slot out of range,
// frames over kMaxFramesPerCall and a call outside BeginBlock / EndBlock; a non-finite input sample adds nothing for that
// call (counted as rejected), as the contract's non-finite rule.
namespace audiofake {
std::atomic<bool> g_roomOutput{false};
std::atomic<uint64_t> g_roomInits{0}, g_roomBegins{0}, g_roomAdds{0}, g_roomAddsWeighted{0}, g_roomResets{0},
    g_roomRenders{0}, g_roomEnds{0}, g_roomSetParams{0}, g_roomSetHrtf{0}, g_roomRejected{0};
std::atomic<int> g_roomLive{0};
RoomSlotView g_roomSlot[64];
binaural::RoomFrame g_roomLastFrame{};
binaural::RoomParams g_roomLastParams{};
float g_roomLastAdded[2 * binaural::kMaxFramesPerCall];
uint32_t g_roomLastFrames = 0;
void ResetRoom() {
  for (std::atomic<uint64_t>* c : {&g_roomInits, &g_roomBegins, &g_roomAdds, &g_roomAddsWeighted, &g_roomResets,
                                   &g_roomRenders, &g_roomEnds, &g_roomSetParams, &g_roomSetHrtf, &g_roomRejected})
    c->store(0);
  memset(g_roomSlot, 0, sizeof(g_roomSlot));
  g_roomLastFrame = binaural::RoomFrame{};
  g_roomLastParams = binaural::RoomParams{};
  g_roomLastFrames = 0;
  g_roomOutput = false;
}
}  // namespace audiofake

namespace binaural {

struct Room::Impl {
  uint32_t rate = 0, maxSources = 0;
  float* acc = nullptr;  // kMaxFramesPerCall: the weighted sends of this block
  bool inBlock = false;
  uint32_t frames = 0;
  float wet = 0;
  uint32_t quietBlocks = 1000;  // blocks since the last one with a wet weight above 0 (the fake's tail)
  std::atomic<const HrtfSet*> set{nullptr}, retired{nullptr};
};

Room::Room() : impl_(new Impl) { audiofake::g_roomLive.fetch_add(1); }
Room::~Room() {
  delete[] impl_->acc;
  delete impl_;
  audiofake::g_roomLive.fetch_sub(1);
}
bool Room::Init(std::uint32_t sampleRate, std::uint32_t maxSources, char* error, std::size_t errorCapacity) {
  if (sampleRate < kMinSampleRate || sampleRate > kMaxSampleRate || !maxSources || maxSources > kMaxSources) {
    if (errorCapacity) strncpy_s(error, errorCapacity, "fake room: bad config", _TRUNCATE);
    return false;
  }
  impl_->rate = sampleRate;
  impl_->maxSources = maxSources;
  impl_->acc = new float[kMaxFramesPerCall]();
  audiofake::g_roomInits.fetch_add(1);
  return true;
}
void Room::SetHrtf(const HrtfSet* s) {
  audiofake::g_roomSetHrtf.fetch_add(1);
  const HrtfSet* old = impl_->set.exchange(s);
  if (old && old != s) impl_->retired.store(old);
}
const HrtfSet* Room::TakeRetiredHrtf() { return impl_->retired.exchange(nullptr); }
void Room::SetHeadRadius(float) {}
void Room::SetParams(const RoomParams& p) {
  audiofake::g_roomSetParams.fetch_add(1);
  audiofake::g_roomLastParams = p;
}
void Room::BeginBlock(std::uint32_t frames, const RoomFrame& f) {
  audiofake::g_roomBegins.fetch_add(1, std::memory_order_relaxed);
  audiofake::g_roomLastFrame = f;
  impl_->inBlock = frames > 0 && frames <= kMaxFramesPerCall;
  impl_->frames = frames;
  impl_->wet = f.wet;
  if (impl_->inBlock) memset(impl_->acc, 0, sizeof(float) * frames);
}
bool Room::AddSource(std::uint32_t slot, const RoomSource& s, const MonoInput& in, std::uint32_t frames, float w0, float w1) {
  if (!impl_->inBlock || slot >= impl_->maxSources || frames != impl_->frames || !impl_->acc) {
    audiofake::g_roomRejected.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  for (uint32_t i = 0; i < frames; ++i) {
    const float x = in.scale * (in.a[i * in.stride] + (in.b ? in.b[i * in.stride] : 0.0f));
    if (!std::isfinite(x)) {
      audiofake::g_roomRejected.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
  audiofake::g_roomAdds.fetch_add(1, std::memory_order_relaxed);
  audiofake::RoomSlotView& v = audiofake::g_roomSlot[slot];
  ++v.adds;
  memcpy(v.pos, s.posAircraft, sizeof(v.pos));
  v.send = s.send;
  v.flags = s.flags;
  v.w0 = w0, v.w1 = w1;
  if (w0 != 0.0f || w1 != 0.0f) audiofake::g_roomAddsWeighted.fetch_add(1, std::memory_order_relaxed);
  const float inv = 1.0f / static_cast<float>(frames);
  for (uint32_t i = 0; i < frames; ++i) {
    const float w = w0 + (w1 - w0) * static_cast<float>(i) * inv;
    impl_->acc[i] += w * s.send * in.scale * (in.a[i * in.stride] + (in.b ? in.b[i * in.stride] : 0.0f));
  }
  return true;
}
void Room::ResetSource(std::uint32_t) { audiofake::g_roomResets.fetch_add(1, std::memory_order_relaxed); }
void Room::Render(float* outLR, std::uint32_t frames) {
  audiofake::g_roomRenders.fetch_add(1, std::memory_order_relaxed);
  if (!impl_->inBlock || frames != impl_->frames) return;
  const bool on = audiofake::g_roomOutput.load(std::memory_order_relaxed);
  audiofake::g_roomLastFrames = frames;
  for (uint32_t i = 0; i < frames; ++i) {
    const float y = on ? audiofake::kFakeRoomGain * impl_->wet * impl_->acc[i] : 0.0f;
    audiofake::g_roomLastAdded[2 * i] = audiofake::g_roomLastAdded[2 * i + 1] = y;
    outLR[2 * i] += y;
    outLR[2 * i + 1] += y;
  }
}
void Room::EndBlock() {
  audiofake::g_roomEnds.fetch_add(1, std::memory_order_relaxed);
  impl_->inBlock = false;
  if (impl_->wet > 0.0f) impl_->quietBlocks = 0;
  else if (impl_->quietBlocks < 1000) ++impl_->quietBlocks;
}
// Idle: wet 0 for 3 blocks (the fake's "tail").
bool Room::Idle() const { return impl_->wet == 0.0f && impl_->quietBlocks >= 3; }
RoomStats Room::GetStats() const { return RoomStats{}; }
void Room::ResetStats() {}

float RoomDirectGain(const RoomParams&, float wet, float, bool, const float*, std::uint32_t) {
  if (!audiofake::g_roomOutput.load(std::memory_order_relaxed)) return 1.0f;
  return 1.0f / std::sqrt(1.0f + audiofake::kFakeRoomEnergy * wet);
}
float RoomSendDistanceGain(const RoomParams&, const float p[3], std::uint32_t flags) {
  if (flags & room_flags::kThroughCanopy) return 1.0f;
  const float r = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) / kRoomRefDistanceM;
  return r < 0.5f ? 0.5f : (r > 2.0f ? 2.0f : r);
}

}  // namespace binaural
