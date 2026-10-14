// Status word exported by the loader as DcsQvCull_Status() (loader.cpp) for
// the DCS Control app's in-headset diagnostic panel, which polls it about
// once per second from the render thread.
//
//   bit 0       payload running (its hooks are installed and its worker runs)
//   bit 1       optimizations active: running and not turned off by the
//               in-flight kill switch ([Hotkeys] Toggle)
//   bit 2       at least one production optimization latched off this session
//   bits 8-15   production optimizations currently active (0-255)
//   bits 16-23  production optimizations latched off this session (0-255)
//   bits 24-31  production optimizations switched on in the ini (0-255),
//               regardless of the kill switch; 0 from payloads older than
//               this field
//   R30 (0 from payloads older than these fields):
//   bit 3       warming up: shader variants still compiling (shadow
//               instancing, batching, the texture skip and the recorders
//               are only partly active until it clears)
//   bits 32-33  GPU-bound governor stage: 0 as configured, 1 shadow
//               recorder helper lists on cascade 3 only, 2 no helper lists,
//               3 both recorders paused (DCS draws them stock)
//   bits 34-36  load verdict over the last 8 s: 0 none (no frames or no
//               reading), 1 CPU-bound, 2 GPU-bound, 3 mixed, 4 capped
//               (neither the render thread nor the GPU full)
//   bit 37      governor enabled ([Governor] Enabled)
//   0           not running / not ready (no payload, payload swap, after stop)
//
// Production optimizations counted: 18 (ComputeStatus in main.cpp; the 18th
// is the terrain prefetch fix, [Prefetch] Mode=skip, prefetch_fix.h: active
// while installed in skip mode with patched pointers or imports, latched
// when the kernel functions or the install folder cannot be resolved).
//
// The loader owns the 64-bit word and hands its address to the payload
// (DcsQvPayload_SetStatusWord) before DcsQvPayload_Start. The payload's
// worker recomputes the word about once per second and publishes it through
// a Sink; DcsQvPayload_Stop detaches the Sink (storing 0) so a stopped
// payload never writes again. The reader only does an acquire load.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace qvstatus {

constexpr uint64_t kRunning = 1ull << 0;
constexpr uint64_t kOptimizing = 1ull << 1;
constexpr uint64_t kLatched = 1ull << 2;
constexpr int kActiveShift = 8;
constexpr int kLatchedShift = 16;
constexpr int kConfiguredShift = 24;
constexpr uint32_t kCountMax = 255;
constexpr uint64_t kWarming = 1ull << 3;
constexpr int kStageShift = 32;     // 2 bits
constexpr int kVerdictShift = 34;   // 3 bits
constexpr uint64_t kGovernorOn = 1ull << 37;

// One production optimization: active = doing its work right now (a
// recorder counts only when it drew passes from command lists since the last
// update, so an enabled but idle recorder is not active); latched = off for
// the rest of the session (install failed: build mismatch, verify mismatch,
// contained exception), never counted as active; configured = switched on in
// the ini, whatever the kill switch or the feature's state.
struct Feature {
  bool active;
  bool latched;
  bool configured = false;
};

inline uint64_t Pack(bool running, bool optimizing, uint32_t active, uint32_t latched, uint32_t configured = 0) {
  if (!running) return 0;
  if (active > kCountMax) active = kCountMax;
  if (latched > kCountMax) latched = kCountMax;
  if (configured > kCountMax) configured = kCountMax;
  return kRunning | (optimizing ? kOptimizing : 0) | (latched ? kLatched : 0) |
         (static_cast<uint64_t>(active) << kActiveShift) | (static_cast<uint64_t>(latched) << kLatchedShift) |
         (static_cast<uint64_t>(configured) << kConfiguredShift);
}

inline uint64_t Compose(const Feature* f, size_t n, bool running, bool engineOff) {
  uint32_t active = 0, latched = 0, configured = 0;
  for (size_t i = 0; i < n; ++i) {
    if (f[i].configured) ++configured;
    if (f[i].latched)
      ++latched;
    else if (f[i].active)
      ++active;
  }
  return Pack(running, running && !engineOff, active, latched, configured);
}

// The R30 fields on top of a composed word (nothing when not running).
inline uint64_t WithExtras(uint64_t w, bool warming, uint32_t stage, uint32_t verdict, bool governorOn) {
  if (!w) return 0;
  if (stage > 3) stage = 3;
  if (verdict > 7) verdict = 0;
  return w | (warming ? kWarming : 0) | (static_cast<uint64_t>(stage) << kStageShift) |
         (static_cast<uint64_t>(verdict) << kVerdictShift) | (governorOn ? kGovernorOn : 0);
}

inline bool Running(uint64_t w) { return (w & kRunning) != 0; }
inline bool Optimizing(uint64_t w) { return (w & kOptimizing) != 0; }
inline bool AnyLatched(uint64_t w) { return (w & kLatched) != 0; }
inline uint32_t ActiveCount(uint64_t w) { return static_cast<uint32_t>(w >> kActiveShift) & 0xff; }
inline uint32_t LatchedCount(uint64_t w) { return static_cast<uint32_t>(w >> kLatchedShift) & 0xff; }
inline uint32_t ConfiguredCount(uint64_t w) { return static_cast<uint32_t>(w >> kConfiguredShift) & 0xff; }
inline bool Warming(uint64_t w) { return (w & kWarming) != 0; }
inline uint32_t GovernorStage(uint64_t w) { return static_cast<uint32_t>(w >> kStageShift) & 3; }
inline uint32_t Verdict(uint64_t w) { return static_cast<uint32_t>(w >> kVerdictShift) & 7; }
inline bool GovernorOn(uint64_t w) { return (w & kGovernorOn) != 0; }

// Payload side: the word it may write to. Publish and Detach are serialized,
// so once Detach returns no further write reaches the (loader-owned) word.
// Detach waits at most 2 s for the lock: a publisher that faulted inside
// Publish (its thread ended by qvexit::Run, the lock never released) must not
// hang the exit quiesce or a payload stop. The pointer is cleared regardless
// (that publisher no longer runs).
class Sink {
 public:
  void Attach(std::atomic<uint64_t>* word) {
    std::lock_guard<std::timed_mutex> lock(m_);
    out_.store(word, std::memory_order_release);
  }
  void Publish(uint64_t w) {
    std::lock_guard<std::timed_mutex> lock(m_);
    if (std::atomic<uint64_t>* o = out_.load(std::memory_order_acquire)) o->store(w, std::memory_order_release);
  }
  void Detach() {
    std::unique_lock<std::timed_mutex> lock(m_, std::defer_lock);
    const bool locked = lock.try_lock_for(std::chrono::milliseconds(2000));
    std::atomic<uint64_t>* o = out_.exchange(nullptr, std::memory_order_acq_rel);
    if (o && locked) o->store(0, std::memory_order_release);
  }

 private:
  std::timed_mutex m_;
  std::atomic<std::atomic<uint64_t>*> out_{nullptr};
};

}  // namespace qvstatus
