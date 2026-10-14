// R30 field diagnostics (section 5, L1) and the GPU-bound governor (R30
// proposal 1): the parts that do not depend on main.cpp's globals, so the
// offline tests can drive them with synthetic timings.
//
//  - Frame sampler: one call per main collect call (DCS makes one
//    collectSceneObjectsRenderables call per frame with every collection of
//    the frame: calls/s == quad/s in our logs, quad or stereo). The call runs
//    on whichever pool thread picks up the culling task (live 2026-10-11:
//    not the render thread, and not always the same thread), so every
//    main-sized call counts, from any thread. It stores the frame interval in
//    a ring and the view layout of that call. No lock, no system call: one
//    QPC (taken by the hook anyway) and a few atomics per frame.
//  - Detector: one verdict per second from the frame rate, the render
//    thread's busy share (its cycle time over the wall time, read by the
//    worker) and the GPU's busy share (D3DKMT node running time, any vendor).
//  - Governor: steps the recorders down while the PC is GPU-bound and back up
//    when it is not, with a verdict window, a minimum dwell per stage and a
//    longer dwell after a bounce (step up, then down again soon after).
//  - Stage settings: which exact path each stage uses (all already exist and
//    are switched at run time by the kill switch and bench modes 28/29).
#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace perfgov {

// ---------------------------------------------------------------------------
// Frame sampler (render thread)
// ---------------------------------------------------------------------------
constexpr uint32_t kRing = 4096;        // frame intervals kept (> 10 s at 400 fps)
float g_ring[kRing];                    // ms; written by the sampled thread only
std::atomic<uint64_t> g_frames{0};      // main collect calls counted (frames)
std::atomic<DWORD> g_tid{0};            // the thread of the last main call (a culling pool thread)
std::atomic<uint32_t> g_maxCount{0};    // largest collection count this window
std::atomic<uint32_t> g_mainCount{0};   // largest of the previous window (0: none yet)
std::atomic<int> g_views{-1};           // shading-model-0 views of the last main call
std::atomic<int> g_pairs{-1};           // focus/peripheral pairs found in it
std::atomic<uint64_t> g_otherThread{0}; // main calls on a different thread than the previous one (diagnostic)
std::atomic<int64_t> g_lastQpc{0};      // QPC of the last main call
double g_qpcToMs = 0;                   // set by Init

inline void Init() {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_qpcToMs = 1000.0 / static_cast<double>(f.QuadPart);
}

// Collect hook, after the original returned. count = collections in the call,
// views/pairs = Classify's result (-1 when the call was not classified).
inline void OnCollect(uint32_t count, int views, int pairs, int64_t qpc) {
  if (count < 4) return;
  uint32_t m = g_maxCount.load(std::memory_order_relaxed);
  while (count > m && !g_maxCount.compare_exchange_weak(m, count, std::memory_order_relaxed)) {
  }
  // The frame's main call: at least half the largest call of the last window
  // (MFD or mirror views, if DCS ever collects them in their own call, are
  // far smaller and do not count as frames).
  const uint32_t main = g_mainCount.load(std::memory_order_relaxed);
  if (main && count * 2 < main) return;
  const DWORD tid = GetCurrentThreadId();
  if (g_tid.exchange(tid, std::memory_order_relaxed) != tid) g_otherThread.fetch_add(1, std::memory_order_relaxed);
  const int64_t prev = g_lastQpc.exchange(qpc, std::memory_order_relaxed);
  const float ms = prev && qpc > prev && g_qpcToMs > 0 ? static_cast<float>((qpc - prev) * g_qpcToMs) : -1.0f;
  const uint64_t n = g_frames.fetch_add(1, std::memory_order_acq_rel);
  g_ring[n % kRing] = ms;
  g_views.store(views, std::memory_order_relaxed);
  g_pairs.store(pairs, std::memory_order_relaxed);
}

// Worker: start of a new main-call window (every 10 s): the largest count of
// the closing window becomes the main-call size.
inline void RollWindow() {
  const uint32_t m = g_maxCount.exchange(0, std::memory_order_relaxed);
  if (m) g_mainCount.store(m, std::memory_order_relaxed);
}

// Worker: the intervals of frames [from, to) (at most kRing - 64 of the
// newest; negative entries = first frame after a thread change, skipped).
template <class Out>
inline uint64_t ReadIntervals(uint64_t from, Out& out) {
  const uint64_t to = g_frames.load(std::memory_order_acquire);
  if (to - from > kRing - 64) from = to - (kRing - 64);
  for (uint64_t k = from; k < to; ++k) {
    const float v = g_ring[k % kRing];
    if (v > 0) out.push_back(v);
  }
  return to;
}

// ---------------------------------------------------------------------------
// Detector
// ---------------------------------------------------------------------------
enum Verdict : int { kNone = 0, kCpu = 1, kGpu = 2, kMixed = 3, kCapped = 4 };

inline const char* VerdictName(int v) {
  switch (v) {
    case kCpu: return "CPU";
    case kGpu: return "GPU";
    case kMixed: return "mixed";
    case kCapped: return "capped";
    default: return "none";
  }
}

struct Thresholds {
  int gpuBusyPct = 92;     // GPU at or above: the GPU is full
  int renderIdlePct = 85;  // render thread at or below: it waits (for the GPU or the runtime)
  int cpuBusyPct = 92;     // render thread at or above: it is the limit
  int gpuFreePct = 80;     // GPU below: it has headroom (CPU, another thread or a frame cap limits)
  // GPU at or above: GPU-bound whatever the render thread's busy share. Live
  // (2026-10-11, 5090, airfield F2, ~67 fps): GPU 98-100% with the render
  // thread 93% busy: a thread waiting for the GPU inside the driver or the
  // runtime may spin, and spinning counts as busy. The governor's fps trial
  // reverts a step that does not pay off, so this verdict may be bold.
  int gpuFullPct = 98;
  int minFrames = 3;       // fewer frames in the second: no verdict (loading, menus)
};

// One second: frames counted, render-thread busy and GPU busy in percent
// (gpu < 0 = unknown).
struct Second {
  uint32_t frames = 0;
  double renderPct = -1;
  double gpuPct = -1;
};

inline Verdict Classify(const Second& s, const Thresholds& t) {
  if (s.frames < static_cast<uint32_t>(t.minFrames) || s.renderPct < 0) return kNone;
  const double rt = s.renderPct, gpu = s.gpuPct;
  if (gpu < 0) return rt >= t.cpuBusyPct ? kCpu : kNone;  // no GPU reading: only a full render thread is clear
  if (gpu >= t.gpuFullPct) return kGpu;
  if (gpu >= t.gpuBusyPct && rt <= t.renderIdlePct) return kGpu;
  if (rt >= t.cpuBusyPct && gpu < t.gpuBusyPct) return kCpu;
  if (gpu < t.gpuFreePct) return rt > t.renderIdlePct ? kCpu : kCapped;
  return kMixed;
}

// ---------------------------------------------------------------------------
// Frame-rate cap (field 2026-10-11: 5080 + Virtual Desktop at 90 Hz with SSW
// ran at exactly 30.0 fps = 90/3 with the GPU at 67-75%; every A/B then reads
// "+0.03%", and an fps trial cannot show anything). A cap is a steady median
// frame time at 1000 * N / Hz for a common headset rate (N = 1..4): the
// runtime releases frames on its vsync multiples. The OpenXR display refresh
// is not read (no OpenXR call is hooked in production); the common rates
// cover the headsets in the field.
// ---------------------------------------------------------------------------
struct Cap {
  double fps = 0;  // 0 = not capped
  int hz = 0, div = 0;
  bool operator==(const Cap& o) const { return hz == o.hz && div == o.div; }
};

constexpr int kCapRates[] = {90, 72, 80, 120, 144};  // the order breaks ties (30 = 90/3 before 120/4)

// Frame intervals (ms) of a window. Capped when at least minFrames, the
// interquartile range within iqrPct of the median, the median's rate within
// tolPct of Hz/N and the mean rate within 2 * tolPct (a long tail is not a
// cap). The smallest N matches first.
inline Cap DetectCap(std::vector<float> iv, double tolPct = 1.5, double iqrPct = 8.0, size_t minFrames = 20) {
  Cap c;
  if (iv.size() < minFrames) return c;
  double sum = 0;
  for (float v : iv) sum += v;
  std::sort(iv.begin(), iv.end());
  const double p25 = iv[iv.size() / 4], p50 = iv[iv.size() / 2], p75 = iv[(iv.size() * 3) / 4];
  if (p50 <= 0 || sum <= 0 || 100.0 * (p75 - p25) / p50 > iqrPct) return c;
  const double fpsMed = 1000.0 / p50, fpsMean = 1000.0 * iv.size() / sum;
  for (int n = 1; n <= 4; ++n)
    for (int hz : kCapRates) {
      const double f = static_cast<double>(hz) / n;
      const double errMed = 100.0 * (fpsMed > f ? fpsMed - f : f - fpsMed) / f;
      const double errMean = 100.0 * (fpsMean > f ? fpsMean - f : f - fpsMean) / f;
      if (errMed <= tolPct && errMean <= 2 * tolPct) {
        c.fps = f;
        c.hz = hz;
        c.div = n;
        return c;
      }
    }
  return c;
}

// The last `seconds` seconds of intervals (one Add per second): the cap of
// that rolling window. A change of cap shows within `seconds` (the window
// spanning both rates is not steady: no cap meanwhile).
struct CapTracker {
  static constexpr int kMax = 16;
  std::vector<float> sec[kMax];
  int head = 0, filled = 0, seconds = 5;
  Cap last;
  void Add(const std::vector<float>& thisSecond) {
    sec[head] = thisSecond;
    head = (head + 1) % seconds;
    if (filled < seconds) ++filled;
    std::vector<float> all;
    for (int i = 0; i < filled; ++i) all.insert(all.end(), sec[i].begin(), sec[i].end());
    last = filled >= (seconds + 1) / 2 ? DetectCap(all) : Cap();
  }
  void Clear() {
    filled = head = 0;
    last = Cap();
  }
};

// ---------------------------------------------------------------------------
// Stages
// ---------------------------------------------------------------------------
//   0  as configured
//   1  shadow recorder helper lists only on cascade 3 (Split & 0x8; R26 9.2a: -0.10 ms GPU)
//   2  no helper lists: shadow Split 0, G-buffer Helpers 0 (one list per job)
//   3  both recorders paused: DCS draws shadows and the G-buffer itself (stock)
// Every stage is a path the recorders already run (ini values, kill switch);
// the image is the same in all of them.
constexpr int kStages = 4;

struct StageSettings {
  uint32_t shadowSplit;
  uint32_t gbHelpers;
  bool pause;
  bool operator==(const StageSettings& o) const {
    return shadowSplit == o.shadowSplit && gbHelpers == o.gbHelpers && pause == o.pause;
  }
};

inline StageSettings ForStage(int stage, uint32_t cfgSplit, uint32_t cfgGbHelpers) {
  StageSettings s{cfgSplit & 0xf, cfgGbHelpers, false};
  if (stage >= 1) s.shadowSplit &= 0x8;
  if (stage >= 2) {
    s.shadowSplit = 0;
    s.gbHelpers = 0;
  }
  if (stage >= 3) s.pause = true;
  return s;
}

// What a stage changes for the recorders that can run: a stage is usable
// when its effective settings differ from the previous usable stage's (so
// every step does something). shadowOn / gbOn = that recorder is configured
// and not latched. Bit k = stage k usable; bit 0 always.
inline uint32_t UsableStages(bool shadowOn, bool gbOn, uint32_t cfgSplit, uint32_t cfgGbHelpers) {
  uint32_t mask = 1;
  if (!shadowOn && !gbOn) return mask;
  auto eff = [&](int st) {
    StageSettings s = ForStage(st, cfgSplit, cfgGbHelpers);
    if (!shadowOn) s.shadowSplit = 0;
    if (!gbOn) s.gbHelpers = 0;
    return s;
  };
  StageSettings prev = eff(0);
  for (int st = 1; st < kStages; ++st) {
    const StageSettings s = eff(st);
    if (!(s == prev)) {
      mask |= 1u << st;
      prev = s;
    }
  }
  return mask;
}

// ---------------------------------------------------------------------------
// Governor (worker, one Tick per second)
// ---------------------------------------------------------------------------
struct GovConfig {
  bool enabled = true;
  int windowSec = 8;     // verdicts considered
  int needSec = 6;       // of them GPU-bound (step down) or not GPU-bound (step up)
  int dwellSec = 10;     // minimum time in a stage before the next step
  int maxDwellSec = 320; // longest up-dwell after repeated bounces
  int bounceSec = 60;    // a step down this soon after a step up from the same stage is a bounce
  // fps proof: every step is a paired trial (A-B-A). A = the trialSec before
  // the step, B = trialSec at the new stage (after trialSettleSec), then the
  // old stage again for recheckSec (after the settle): A'. The reference for
  // B is A and A' interpolated to B's middle, so a scene that drifts linearly
  // (fps rising or falling on its own) does not decide the trial. A step
  // down is kept (the new stage set again) only when B beats the reference
  // by more than minGainPct; a step up is reverted when B is below it by
  // more than minGainPct. A reverted step blocks its target stage for
  // revertBackoffSec, doubling per revert up to maxRevertBackoffSec.
  // recheckSec 0 = plain before/after. Live (airfield, 5090): fps moves
  // +-3% from one 10-s window to the next; 20-s means and the A-B-A
  // reference bring that well under 2%. Pausing the recorders moves their
  // work back to the render thread: near the CPU limit that costs more fps
  // than the GPU time it saves, and the trial catches it.
  double minGainPct = 2.0;
  int trialSettleSec = 2;
  int trialSec = 20;
  int recheckSec = 10;
  int revertBackoffSec = 300;
  int maxRevertBackoffSec = 3600;
  int unsteadySec = 60;  // an unsteady trial's stage waits this long, doubling per unsteady trial of it (to maxRevertBackoffSec)
};

// What the last Tick did (for the log).
enum GovEvent : int {
  kEvNone = 0,
  kEvDown,          // stepped down (a trial starts when there is an fps baseline)
  kEvUp,            // stepped up (likewise)
  kEvUnusable,      // the stage stopped applying: moved at once, no trial
  kEvKept,          // a trial ended and the step stays
  kEvInconclusive,  // a trial ended without enough fps seconds: the step stays
  kEvRevertDown,    // a step down did not raise fps: back up, its stage blocked
  kEvRevertUp,      // a step up lowered fps: back down, its stage blocked
  kEvRecheck,       // B measured: back to the old stage for the A' window (temporary)
  kEvUnsteady,      // A and A' disagree (the scene changed during the trial): no verdict; a step down is
                    // undone, a step up (toward the configuration) stays; its stage waits unsteadySec
};

struct Governor {
  int stage = 0;
  int window[64] = {};
  int filled = 0, head = 0;
  double lastChange = -1e18;
  double upDwell[kStages] = {};  // per stage: dwell before stepping up from it (0 = dwellSec)
  double lastUpAt = -1e18;
  int lastUpFrom = -1;
  int bounces = 0;
  // fps history (one entry per Tick; < 0 = no frames that second).
  double fpsT[128] = {}, fpsV[128] = {};
  int fpsN = 0;
  // The running trial: phase 1 = measuring B at trialTo, 2 = measuring A' at trialFrom.
  bool trial = false;
  int trialPhase = 0;
  int trialFrom = 0, trialTo = 0;
  double trialT0 = 0, trialT1 = 0, trialBase = 0, trialAfter = 0;
  double evRecheck = 0;  // A' of the last decided trial (0 = none)
  // Reverts: per target stage, blocked until, and the next back-off.
  double blockedUntil[kStages] = {};
  double backoff[kStages] = {};
  double unsteadyBackoff[kStages] = {};  // next wait after an unsteady trial (0 = unsteadySec)
  int reverts = 0;
  // Last decision (for the log).
  int lastGpu = 0, lastFree = 0, lastN = 0;
  GovEvent lastEvent = kEvNone;
  double evFpsBefore = 0, evFpsAfter = 0, evBlockSec = 0;
  int evStage = 0;  // the stage a trial or revert was about
  int evWaitStage = 0;  // kEvUnsteady: the stage that waits

  void Clear() {
    filled = head = 0;
  }
  // Clears the window, the fps history and any trial, and restarts the dwell
  // timer (kill switch, bench, nothing to govern). Blocks stay.
  void Reset(double now) {
    Clear();
    fpsN = 0;
    trial = false;
    lastChange = now;
  }
  double UpDwell(const GovConfig& c, int st) const {
    const double d = upDwell[st];
    return d > 0 ? d : static_cast<double>(c.dwellSec);
  }
  bool Blocked(int st, double now) const { return now < blockedUntil[st]; }
  // Mean fps of the seconds with t in (t0, t1]; n = how many had frames.
  double MeanFps(double t0, double t1, int* n) const {
    double sum = 0;
    int k = 0;
    const int first = fpsN > 128 ? fpsN - 128 : 0;
    for (int i = first; i < fpsN; ++i) {
      const double t = fpsT[i % 128], v = fpsV[i % 128];
      if (t <= t0 || t > t1 || v < 0) continue;
      sum += v;
      ++k;
    }
    *n = k;
    return k ? sum / k : 0;
  }
  // A trial for the step from -> to at now, when at least half of the
  // trialSec seconds before it had frames (else the step is verdict-only).
  void StartTrial(const GovConfig& c, int from, int to, double now) {
    int n = 0;
    const double base = MeanFps(now - c.trialSec - 0.5, now, &n);
    trial = n * 2 >= c.trialSec && base > 0;
    trialPhase = 1;
    trialFrom = from;
    trialTo = to;
    trialT0 = now;
    trialBase = base;
  }
  void Block(const GovConfig& c, int st, double now) {
    double b = backoff[st] > 0 ? backoff[st] : static_cast<double>(c.revertBackoffSec);
    if (b > c.maxRevertBackoffSec) b = c.maxRevertBackoffSec;
    blockedUntil[st] = now + b;
    evBlockSec = b;
    backoff[st] = b * 2 > c.maxRevertBackoffSec ? c.maxRevertBackoffSec : b * 2;
    ++reverts;
  }
  // The end of a trial: B (trialAfter) against the reference. The stage is
  // trialFrom when `atFrom` (after the A' window), else trialTo. Returns the
  // stage change it makes (+1 down, -1 up, 0 none).
  int Decide(const GovConfig& c, double now, double ref, bool down, bool atFrom) {
    trial = false;
    trialPhase = 0;
    evFpsBefore = ref;
    evFpsAfter = trialAfter;
    evStage = trialTo;
    const double gain = ref > 0 ? 100.0 * (trialAfter / ref - 1.0) : 0.0;
    const bool revert = down ? gain <= c.minGainPct : gain < -c.minGainPct;
    const int was = stage;
    lastChange = now;
    Clear();
    unsteadyBackoff[trialTo] = unsteadyBackoff[trialFrom] = 0;  // a decisive trial: the scene was steady
    if (!revert) {
      if (down) backoff[trialTo] = 0;  // a kept step down forgets the stage's earlier reverts
      stage = trialTo;
      lastEvent = kEvKept;
    } else {
      Block(c, trialTo, now);
      stage = trialFrom;
      lastEvent = down ? kEvRevertDown : kEvRevertUp;
      if (down) lastUpFrom = -1;  // not a bounce: the stage is blocked instead
    }
    (void)atFrom;
    return stage > was ? +1 : stage < was ? -1 : 0;
  }
  // +1 = stepped down (to a later stage), -1 = stepped up, 0 = no change;
  // lastEvent says why. fps = this second's frame rate (< 0: no frames).
  // usable = UsableStages(...); a stage beyond it is left at once.
  int Tick(Verdict v, double fps, double now, const GovConfig& c, uint32_t usable) {
    lastEvent = kEvNone;
    fpsT[fpsN % 128] = now;
    fpsV[fpsN % 128] = fps;
    ++fpsN;
    const int w = c.windowSec < 1 ? 1 : c.windowSec > 64 ? 64 : c.windowSec;
    if (filled > w || head >= w) Clear();  // the window was shortened (ini)
    window[head] = v;
    head = (head + 1) % w;
    if (filled < w) ++filled;
    // Decay of a bounce penalty: a step up that held for bounceSec.
    if (lastUpFrom >= 0 && now - lastUpAt > c.bounceSec) {
      upDwell[lastUpFrom] = upDwell[lastUpFrom] > 2.0 * c.dwellSec ? upDwell[lastUpFrom] / 2 : 0;
      lastUpFrom = -1;
    }
    if (!c.enabled) return 0;
    // The current stage is no longer usable (a recorder latched, ini changed): to the nearest usable one below.
    if (stage > 0 && !(usable & (1u << stage))) {
      int to = stage - 1;
      while (to > 0 && !(usable & (1u << to))) --to;
      stage = to;
      lastChange = now;
      lastGpu = lastFree = lastN = 0;
      trial = false;
      lastEvent = kEvUnusable;
      Clear();
      return -1;
    }
    // A running trial decides first; no other step until it has.
    if (trial) {
      const bool down = trialTo > trialFrom;
      if (trialPhase == 1) {
        if (now < trialT0 + c.trialSettleSec + c.trialSec) return 0;
        int n = 0;
        trialAfter = MeanFps(trialT0 + c.trialSettleSec, now, &n);
        evFpsBefore = trialBase;
        evFpsAfter = trialAfter;
        evRecheck = 0;
        evStage = trialTo;
        if (n * 2 < c.trialSec || trialAfter <= 0) {
          trial = false;
          lastEvent = kEvInconclusive;
          return 0;
        }
        if (c.recheckSec > 0) {  // A': back to the old stage for a moment
          trialPhase = 2;
          trialT1 = now;
          stage = trialFrom;
          lastEvent = kEvRecheck;
          return down ? -1 : +1;
        }
        return Decide(c, now, trialBase, down, false);
      }
      if (now < trialT1 + c.trialSettleSec + c.recheckSec) return 0;
      int n = 0;
      const double again = MeanFps(trialT1 + c.trialSettleSec, now, &n);
      evRecheck = again;
      double ref = trialBase;
      if (n * 2 >= c.recheckSec && again > 0) {
        const double apart = 100.0 * (again > trialBase ? again / trialBase - 1.0 : trialBase / again - 1.0);
        if (apart > c.minGainPct) {
          // A and A' (the same stage) disagree by more than the gain sought: the scene changed (a loading dip,
          // a new view), it did not drift, and no reference is fair (live 2026-10-11: A 48 fps in a dip, B
          // 66.5, A' 66.1 had kept a step for +14.7% that was +0.6%). No verdict: a step down is undone (it
          // needs proof), a step up stays (back toward the configuration); that stage waits unsteadySec.
          trial = false;
          trialPhase = 0;
          evFpsBefore = trialBase;
          evFpsAfter = trialAfter;
          evStage = trialTo;
          const int was = stage;
          stage = down ? trialFrom : trialTo;
          const int waits = down ? trialTo : trialFrom;
          double b = unsteadyBackoff[waits] > 0 ? unsteadyBackoff[waits] : static_cast<double>(c.unsteadySec);
          if (b > c.maxRevertBackoffSec) b = c.maxRevertBackoffSec;
          blockedUntil[waits] = now + b;
          unsteadyBackoff[waits] = b * 2;  // a scene that keeps changing is retried ever more rarely
          evBlockSec = b;
          evWaitStage = waits;
          lastChange = now;
          Clear();
          lastEvent = kEvUnsteady;
          return stage > was ? +1 : stage < was ? -1 : 0;
        } else {
          // A at the middle of its window, B and A' likewise: ref = A..A' interpolated to B's middle.
          const double ta = trialT0 - c.trialSec / 2.0;
          const double tb = trialT0 + c.trialSettleSec + c.trialSec / 2.0;
          const double tc = trialT1 + c.trialSettleSec + c.recheckSec / 2.0;
          ref = trialBase + (again - trialBase) * (tb - ta) / (tc - ta);
        }
      }
      return Decide(c, now, ref, down, true);
    }
    int gpu = 0, free = 0;
    for (int i = 0; i < filled; ++i) {
      gpu += window[i] == kGpu;
      free += window[i] == kCpu || window[i] == kCapped;
    }
    lastGpu = gpu;
    lastFree = free;
    lastN = filled;
    const int need = c.needSec < 1 ? 1 : c.needSec > w ? w : c.needSec;
    const double since = now - lastChange;
    if (gpu >= need && since >= c.dwellSec) {
      int to = stage + 1;
      while (to < kStages && !(usable & (1u << to))) ++to;
      if (to >= kStages || Blocked(to, now)) return 0;  // a blocked stage is not skipped over
      if (lastUpFrom == to && now - lastUpAt <= c.bounceSec) {
        const double d = UpDwell(c, to) * 2;
        upDwell[to] = d > c.maxDwellSec ? c.maxDwellSec : d;
        lastUpFrom = -1;
        ++bounces;
      }
      StartTrial(c, stage, to, now);
      stage = to;
      lastChange = now;
      Clear();
      lastEvent = kEvDown;
      return +1;
    }
    if (stage > 0 && free >= need && since >= UpDwell(c, stage)) {
      int to = stage - 1;
      while (to > 0 && !(usable & (1u << to))) --to;
      if (Blocked(to, now)) return 0;
      lastUpFrom = stage;
      lastUpAt = now;
      StartTrial(c, stage, to, now);
      stage = to;
      lastChange = now;
      Clear();
      lastEvent = kEvUp;
      return -1;
    }
    return 0;
  }
  // Without fps (no trials): the verdict-only governor.
  int Tick(Verdict v, double now, const GovConfig& c, uint32_t usable) { return Tick(v, -1.0, now, c, usable); }
};

}  // namespace perfgov
