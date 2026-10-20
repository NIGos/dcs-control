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
//   bit 38      R62: PerceptionOnly ([General] PerceptionOnly=1, Engine
//               Optimizations off in the app, the module loaded for Depth
//               boost): the word is then kRunning | kPerceptionOnly plus
//               bits 39-42 (the cockpit mask export and its companions, as
//               below) and nothing else, so a panel that does not know this bit shows
//               "Engine Opt.: OFF" (true) instead of "starting" for the whole
//               flight; one that knows it leaves the Engine Opt line out
//   bit 39      R65: cockpit mask export ([Perception] CockpitMaskExport=1):
//               a mask frame was committed to Local\DcsVrCockpitMask_v1 in
//               the last second (dcsvr_cockpit_mask.h); also set next to
//               bit 38 in PerceptionOnly (a panel testing bits keeps working)
//   bit 40      R65: that last commit carried DCS's xrEndFrame displayTime
//               (pairable); clear with bit 39 set = committed late (the
//               xrEndFrame hook is not in: another Visualizer.dll build)
//   bit 41      R69: cockpit depth companion ([Perception]
//               CockpitDepthExport=1): a depth slot was committed to
//               Local\DcsVrCockpitDepth_v1 in the last second, with bit 39
//               (dcsvr_cockpit_depth.h; its reader heartbeat fresh)
//   bit 42      R70: motion-vector companion ([Perception]
//               CockpitMotionExport=1): a motion slot was committed to
//               Local\DcsVrMotion_v1 in the last second, with bit 39
//               (dcsvr_motion.h; its reader heartbeat fresh)
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
constexpr uint64_t kPerceptionOnly = 1ull << 38;  // R62
constexpr uint64_t kCockpitMask = 1ull << 39;        // R65
constexpr uint64_t kCockpitMaskPaired = 1ull << 40;  // R65
constexpr uint64_t kCockpitDepth = 1ull << 41;       // R69
constexpr uint64_t kCockpitMotion = 1ull << 42;      // R70
constexpr uint64_t kCockpitSymbology = 1ull << 43;   // R95: a symbology slot committed in the last second

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
inline bool PerceptionOnly(uint64_t w) { return (w & kPerceptionOnly) != 0; }
inline bool CockpitMask(uint64_t w) { return (w & kCockpitMask) != 0; }              // R65
inline bool CockpitMaskPaired(uint64_t w) { return (w & kCockpitMaskPaired) != 0; }  // R65
inline bool CockpitDepth(uint64_t w) { return (w & kCockpitDepth) != 0; }            // R69
inline bool CockpitMotion(uint64_t w) { return (w & kCockpitMotion) != 0; }          // R70
inline bool CockpitSymbology(uint64_t w) { return (w & kCockpitSymbology) != 0; }    // R95

// ---- Dual camera status (DcsQvCull_DualCameraStatus; layout also in loader_api.h) ----
// R31 option 3, [Perception] CockpitTrueIpd. A second loader-owned word, handed
// to the payload through DcsQvPayload_SetDualCameraStatusWord, published by the
// worker about once per second through its own Sink (0: not configured, no
// payload, payload swap, after stop).
//   bit 0       CockpitTrueIpd configured (1 or 2)
//   bit 1       ON: the cockpit was patched (true eye distance) in the last
//               rendered frame or in a frame of the last second (R62: was
//               the one frame sampled each second only, so a gated frame
//               flickered "no dual camera"); mode 1, not paused, not faulted
//   bit 2       paused: switched off by its in-flight key (Alt+Shift+F7)
//   bit 3       waiting for Depth boost: no BoostOut, BoostOut stale or dry,
//               or the boost scale is 1.0 (not applied)
//   bit 4       observe mode (CockpitTrueIpd=2)
//   bit 5       R62: no cockpit drawn: not ON, and neither the last frame
//               nor any frame of the last second drew a cockpit pass
//               (external view F2 / F10 map / weapon or replay camera).
//               Nothing to patch, not a failure: a panel treats it like
//               bit 3 (no "no dual camera" warning)
//   bits 6-7    0
//   bits 8-15   current boost scale x100 rounded, clamped to 255 (140 =
//               x1.40); 0 when no fresh BoostOut is read
//   bits 16-31  frames patched in the last second, clamped to 65535
//   bits 32-63  0
constexpr uint64_t kDcConfigured = 1ull << 0;
constexpr uint64_t kDcOn = 1ull << 1;
constexpr uint64_t kDcPaused = 1ull << 2;
constexpr uint64_t kDcWaiting = 1ull << 3;
constexpr uint64_t kDcObserve = 1ull << 4;
constexpr uint64_t kDcNoCockpit = 1ull << 5;  // R62
constexpr int kDcScaleShift = 8;    // 8 bits
constexpr int kDcFramesShift = 16;  // 16 bits

inline uint64_t PackDualCamera(bool configured, bool on, bool paused, bool waiting, bool observe, double scale,
                               uint32_t framesPatchedPerSecond, bool noCockpit = false) {
  if (!configured) return 0;
  uint64_t s = 0;
  if (scale > 0 && scale < 1e6) {  // false for NaN
    const double x = scale * 100.0 + 0.5;
    s = x >= 255.0 ? 255u : static_cast<uint64_t>(x);
  }
  const uint64_t fr = framesPatchedPerSecond > 65535u ? 65535u : framesPatchedPerSecond;
  return kDcConfigured | (on ? kDcOn : 0) | (paused ? kDcPaused : 0) | (waiting ? kDcWaiting : 0) |
         (observe ? kDcObserve : 0) | (noCockpit && !on ? kDcNoCockpit : 0) | (s << kDcScaleShift) |
         (fr << kDcFramesShift);
}
// A count over dtMs as a per-second rate (0 when dtMs is 0), clamped to 65535.
inline uint32_t RatePerSecond(uint64_t delta, uint64_t dtMs) {
  if (!dtMs) return 0;
  const uint64_t r = (delta * 1000 + dtMs / 2) / dtMs;
  return r > 65535u ? 65535u : static_cast<uint32_t>(r);
}
inline bool DcConfigured(uint64_t w) { return (w & kDcConfigured) != 0; }
inline bool DcOn(uint64_t w) { return (w & kDcOn) != 0; }
inline bool DcPaused(uint64_t w) { return (w & kDcPaused) != 0; }
inline bool DcWaiting(uint64_t w) { return (w & kDcWaiting) != 0; }
inline bool DcObserve(uint64_t w) { return (w & kDcObserve) != 0; }
inline bool DcNoCockpit(uint64_t w) { return (w & kDcNoCockpit) != 0; }
inline uint32_t DcScaleX100(uint64_t w) { return static_cast<uint32_t>(w >> kDcScaleShift) & 0xff; }
inline uint32_t DcFramesPerSecond(uint64_t w) { return static_cast<uint32_t>(w >> kDcFramesShift) & 0xffff; }

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
