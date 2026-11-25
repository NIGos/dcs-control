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
#include <new>

#include "audio_fake_core.h"
#include "binaural_core.h"

namespace audiofake {
std::atomic<bool> g_refuse{false};
std::atomic<uint32_t> g_spinUs{0};
std::atomic<uint64_t> g_renders{0}, g_resets{0}, g_setHrtf{0}, g_setRadius{0}, g_setNear{0}, g_begin{0}, g_end{0};
std::atomic<int> g_liveSets{0};
float g_lastAdded[2 * binaural::kMaxFramesPerCall];
uint32_t g_lastFrames = 0;
float g_lastMono[binaural::kMaxFramesPerCall];
float g_lastRadius = 0;
float g_lastEarRadius = 0;
float g_lastInitRadius = 0;
std::atomic<uint32_t> g_lastFlags{0};
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
