// Binaural audio, render side: the per-voice output rule inside ed_sound's mix (docs/BINAURAL_AUDIO_HOOKS.md 5.2,
// 5.3), the fallback re-pan, the binding of one WASAPI renderer to a BinauralCore, the CPU budget, and the event ring
// the real-time threads report through (the module's control thread turns events into log lines).
//
// Runs on DCS's audio render thread (MMCSS): no allocation, locks, logging, file I/O or waits. FTZ/DAZ only around our
// own DSP (restored before ED's next kernel runs, so DCS's voices render exactly as stock).
//
// Output of a candidate voice (one with a VoiceMeta slot; its own render went into a zeroed scratch first):
//
//   previous block | this block | added to `out`
//   ---------------+------------+--------------------------------------------------------------------------------
//   not tagged     | not tagged | scratch                                        (stock within 1 ulp, see below)
//   not tagged     | tagged     | (1-w) scratch + w T(1/2 (L+R)),  w 0 -> 1     (fade in)
//   tagged         | tagged     | T(L)                                           (L == R == mono * m)
//   tagged         | not tagged | (1-w) scratch + w T(1/2 (L+R)),  w 1 -> 0     (fade out)
//
// T(x) = h HRTF(x) + (1-h) RePan(x): h is 1 for an owned voice while binaural is on, 0 otherwise (kill switch, module
// off, the voice released, a core refusal), ramped across one block. RePan(x) puts the mono signal back with DCS's
// own gains of that frame: out_L += x gL/m, out_R += x gR/m, which is exactly DCS's stock output for that voice (the
// "null" property the tests check to -60 dB). So the kill switch, a fallback and every ownership change reach DCS's
// own panning within one block, with no step.
// A voice owned from its creation (meta_flags::kInitial) starts tagged with no fade: its onset is binaural.
//
// "scratch" is the voice's own render into a zeroed buffer, added to `out` here. ED's float kernels add into `out`
// exactly as we do (bit-identical), but its SIMD mono 3D kernel (0x12dd0) adds in double: out = float(double(out) +
// v), where we add float(v) after the fact. So a candidate voice's stock part is within 1 ulp of stock, not always
// bit-identical. Voices without a VoiceMeta slot are never touched (DCS's own call, bit-identical).
//
// A torn snapshot: DCS's panner stores its own gains and the sim overwrites them with the tag right after, without the
// render thread's lock; a voice render in between snapshots DCS's stereo gains for one block (and ramps back to the
// tag in the next). For an owned voice whose metadata is unchanged that is not a release: the block stays tagged, the
// mono signal is recovered from the known ramp (sum of the gains from tag to stock and back) and the voice stays
// binaural (counted: Counters::tornSnaps). A real release that happens to look torn then fades out from that block.
#pragma once

#include <immintrin.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

#include "audio_voice.h"
#include "binaural_core.h"
#include "head_pose_reader.h"  // head_pose_shm.h and its checked readers (staleness, writer, speed guards)

namespace audr {

using audv::MetaData;

constexpr uint32_t kMaxFrames = binaural::kMaxFramesPerCall;  // longer device blocks pass through (counted)
constexpr float kNearM = 1.5f;                                 // late-translation gain below this distance
constexpr float kTransMin = 0.70794578f, kTransMax = 1.41253754f;  // -3 / +3 dB
constexpr double kMaxLateRad = 1.0471975511965976;             // a late correction beyond 60 deg is not trusted
constexpr double kMaxLateTransM = 0.3;                         // a late head translation is clamped to 30 cm

// ---- core calls (BinauralCore, binaural_core.h). Builds without the core library keep the module off. ----------
#ifndef DCSQV_HAVE_BINAURAL_CORE
#define DCSQV_HAVE_BINAURAL_CORE 0
#endif
constexpr bool kHaveCore = DCSQV_HAVE_BINAURAL_CORE != 0;
#if DCSQV_HAVE_BINAURAL_CORE
inline binaural::Renderer* CoreNew() { return new (std::nothrow) binaural::Renderer(); }
inline void CoreDelete(binaural::Renderer* r) { delete r; }
inline bool CoreInit(binaural::Renderer* r, const binaural::Config& c, char* e, size_t n) { return r->Init(c, e, n); }
inline void CoreSetHrtf(binaural::Renderer* r, const binaural::HrtfSet* s) { r->SetHrtf(s); }
inline const binaural::HrtfSet* CoreTakeRetired(binaural::Renderer* r) { return r->TakeRetiredHrtf(); }
inline void CoreSetHeadRadius(binaural::Renderer* r, float m) { r->SetHeadRadius(m); }
inline void CoreSetEarRadius(binaural::Renderer* r, float m) { r->SetEarRadius(m); }
inline void CoreSetNearField(binaural::Renderer* r, bool on) { r->SetNearField(on); }
inline void CoreBegin(binaural::Renderer* r, uint32_t frames) { r->BeginBlock(frames); }
inline bool CoreRender(binaural::Renderer* r, uint32_t slot, const binaural::SourceUpdate& u, const binaural::MonoInput& in,
                       uint32_t frames, float w0, float w1, float* out) {
  return r->Render(slot, u, in, frames, w0, w1, out);
}
inline void CoreResetSource(binaural::Renderer* r, uint32_t slot) { r->ResetSource(slot); }
inline void CoreEnd(binaural::Renderer* r) { r->EndBlock(); }
inline binaural::Stats CoreStats(const binaural::Renderer* r) { return r->GetStats(); }
inline binaural::HrtfSet* CoreLoadHrtf(const void* d, size_t n, uint32_t rate, char* e, size_t en) {
  return binaural::LoadHrtf(d, n, rate, e, en);
}
inline void CoreReleaseHrtf(binaural::HrtfSet* s) { binaural::ReleaseHrtf(s); }
inline binaural::HrtfInfo CoreDescribe(const binaural::HrtfSet* s) { return binaural::DescribeHrtf(s); }
#else
inline binaural::Renderer* CoreNew() { return nullptr; }
inline void CoreDelete(binaural::Renderer*) {}
inline bool CoreInit(binaural::Renderer*, const binaural::Config&, char* e, size_t n) {
  if (n) strncpy_s(e, n, "no binaural renderer in this build", _TRUNCATE);
  return false;
}
inline void CoreSetHrtf(binaural::Renderer*, const binaural::HrtfSet*) {}
inline const binaural::HrtfSet* CoreTakeRetired(binaural::Renderer*) { return nullptr; }
inline void CoreSetHeadRadius(binaural::Renderer*, float) {}
inline void CoreSetEarRadius(binaural::Renderer*, float) {}
inline void CoreSetNearField(binaural::Renderer*, bool) {}
inline void CoreBegin(binaural::Renderer*, uint32_t) {}
inline bool CoreRender(binaural::Renderer*, uint32_t, const binaural::SourceUpdate&, const binaural::MonoInput&, uint32_t,
                       float, float, float*) {
  return false;
}
inline void CoreResetSource(binaural::Renderer*, uint32_t) {}
inline void CoreEnd(binaural::Renderer*) {}
inline binaural::Stats CoreStats(const binaural::Renderer*) { return binaural::Stats{}; }
inline binaural::HrtfSet* CoreLoadHrtf(const void*, size_t, uint32_t, char* e, size_t en) {
  if (en) strncpy_s(e, en, "no binaural renderer in this build", _TRUNCATE);
  return nullptr;
}
inline void CoreReleaseHrtf(binaural::HrtfSet*) {}
inline binaural::HrtfInfo CoreDescribe(const binaural::HrtfSet*) { return binaural::HrtfInfo{}; }
#endif

// Status word bits 53-55 (status_word.h), published by the audio module's control thread.
std::atomic<uint64_t> g_status{0};

// ---- events from the real-time threads (bounded MPMC ring; the control thread drains it) ---------------------------
enum EvCode : uint16_t {
  kEvNone = 0,
  kEvBudgetDown,     // a: new MaxObjects limit, b: over-budget blocks of the last second (per mille)
  kEvBudgetUp,       // a: new limit
  kEvOverrunsOff,    // a: ED overruns within 10 s
  kEvPanLawOff,      // a: mismatches, b: solves (10 s window)
  kEvNewRenderer,    // c: renderer address (the control thread validates it)
  kEvFramesTooLong,  // b: frames
};
struct Ev {
  uint16_t code;
  uint16_t a;
  uint32_t b;
  uint64_t c;
};
struct EvRing {
  static constexpr uint32_t kN = 256;  // power of two
  struct Cell {
    std::atomic<uint32_t> seq;
    Ev ev;
  };
  Cell cell[kN];
  std::atomic<uint32_t> enq{0}, deq{0};
  std::atomic<uint64_t> dropped{0};
  EvRing() {
    for (uint32_t i = 0; i < kN; ++i) cell[i].seq.store(i, std::memory_order_relaxed);
  }
  bool Push(const Ev& e) {
    uint32_t pos = enq.load(std::memory_order_relaxed);
    for (;;) {
      Cell& c = cell[pos & (kN - 1)];
      const uint32_t seq = c.seq.load(std::memory_order_acquire);
      const int32_t dif = static_cast<int32_t>(seq - pos);
      if (dif == 0) {
        if (enq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          c.ev = e;
          c.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (dif < 0) {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
      } else {
        pos = enq.load(std::memory_order_relaxed);
      }
    }
  }
  bool Pop(Ev* e) {  // one consumer (the control thread)
    const uint32_t pos = deq.load(std::memory_order_relaxed);
    Cell& c = cell[pos & (kN - 1)];
    if (c.seq.load(std::memory_order_acquire) != pos + 1) return false;
    *e = c.ev;
    c.seq.store(pos + kN, std::memory_order_release);
    deq.store(pos + 1, std::memory_order_relaxed);
    return true;
  }
};

// ---- counters (render and sim write, the control thread reads) -------------------------------------------------------
// Why the simulation did not give a 3D sound to the binaural renderer (PanVoice, one count per panner call; the "audio:
// idle" line logs them). The first reason that applies is counted.
enum Refuse : int {
  kRefuseOff = 0,        // binaural not on for this frame (settings, kill switch, an off reason, a pass or handover)
  kRefuseOutput,         // no output bound, or the voice plays on another one (a second DCS output)
  kRefuseNot3D,          // no sound, a 2D sound, or a voice of a 2D context
  kRefuseOffline,        // ed_sound's engine state offline
  kRefuseNotMono,        // a voice with more than one channel
  kRefuseContext,        // its context is not one of the array's, is excluded ([Audio] ExcludeContexts) or 2D
  kRefuseDistance,       // nearer than the minimum distance, or not finite
  kRefuseStereoOut,      // the bound output is not stereo
  kRefuseGains,          // DCS's gains are not its stereo format
  kRefusePanMismatch,    // the pan law check failed
  kRefuseNotCreation,    // qualifies, but a voice is taken only when it starts
  kRefuseLimit,          // qualifies at its start, but the sound limit was reached
  kRefuseCount
};
struct Counters {
  std::atomic<uint64_t> refused[kRefuseCount] = {};
  std::atomic<uint64_t> blocks{0}, armedBlocks{0}, candidateCalls{0}, hrtfCalls{0}, repanCalls{0}, fadeIns{0},
      fadeOuts{0}, steady{0}, passCandidates{0}, coreRejected{0}, orphanTagged{0}, nonFinite{0}, framesTooLong{0},
      voiceEnded{0}, unarmedCalls{0}, overruns{0}, poseBlocks{0}, tornSnaps{0}, passBlocks{0};
  std::atomic<uint64_t> panCalls{0}, panSolves{0}, panMismatch{0}, panMedian{0}, ownedVoiceFrames{0}, simFrames{0};
  std::atomic<uint32_t> ownedNow{0}, limitNow{binaural::kMaxSources}, activeLastBlock{0};
  // Block time of our own DSP, microseconds x 10, the last 1024 armed blocks (single writer: the render thread).
  std::atomic<uint32_t> timeRing[1024];
  std::atomic<uint32_t> timeHead{0};
  std::atomic<uint32_t> worstTenthUs{0};
  void Reset() {  // the module's install (and the tests): every counter back to its start value
    for (std::atomic<uint64_t>* c : {&blocks, &armedBlocks, &candidateCalls, &hrtfCalls, &repanCalls, &fadeIns, &fadeOuts,
                                     &steady, &passCandidates, &coreRejected, &orphanTagged, &nonFinite, &framesTooLong,
                                     &voiceEnded, &unarmedCalls, &overruns, &poseBlocks, &tornSnaps, &passBlocks, &panCalls, &panSolves,
                                     &panMismatch, &panMedian, &ownedVoiceFrames, &simFrames})
      c->store(0, std::memory_order_relaxed);
    for (std::atomic<uint64_t>& c : refused) c.store(0, std::memory_order_relaxed);
    ownedNow.store(0, std::memory_order_relaxed);
    limitNow.store(binaural::kMaxSources, std::memory_order_relaxed);
    activeLastBlock.store(0, std::memory_order_relaxed);
    timeHead.store(0, std::memory_order_relaxed);
    worstTenthUs.store(0, std::memory_order_relaxed);
  }
};

// ---- the late correction for one block (render thread) ---------------------------------------------------------------
struct PoseNow {
  bool valid = false;
  bool posValid = false;  // t is a tracked position (else it is zero and no translation is applied)
  dcsvr_headpose::Quat q{0, 0, 0, 1};
  dcsvr_headpose::Vec3 t{0, 0, 0};
  uint64_t base = 0;
};

// A meta's listener-frame position with the head motion since DCS's frame applied (when both poses are valid, in one
// base space, and the turn is plausible). The translation only when both poses have a position (a missing one is
// stored as zero: the whole head offset from the origin would move every sound), clamped to kMaxLateTransM.
// *gainOut: the late-translation gain for sources under 1.5 m.
inline void CorrectedPosition(const MetaData& m, const PoseNow& now, bool late, float pos[3], float* gainOut) {
  pos[0] = m.pos[0];
  pos[1] = m.pos[1];
  pos[2] = m.pos[2];
  *gainOut = 1.0f;
  if (!late || !now.valid || !(m.flags & audv::meta_flags::kPoseValid) || m.poseBase != now.base) return;
  namespace hp = dcsvr_headpose;
  const hp::Quat qu = hp::Normalize({m.qUsed[0], m.qUsed[1], m.qUsed[2], m.qUsed[3]});
  const bool positions = now.posValid && (m.flags & audv::meta_flags::kPosValid);
  const hp::Vec3 zero{0, 0, 0};
  hp::LateCorrection lc = hp::ComputeLateCorrection(qu, positions ? hp::Vec3{m.tUsed[0], m.tUsed[1], m.tUsed[2]} : zero,
                                                    now.q, positions ? now.t : zero);
  const double tl = std::sqrt(lc.translation.x * lc.translation.x + lc.translation.y * lc.translation.y +
                              lc.translation.z * lc.translation.z);
  if (!std::isfinite(tl)) return;
  if (tl > kMaxLateTransM) {
    const double k = kMaxLateTransM / tl;
    lc.translation = {lc.translation.x * k, lc.translation.y * k, lc.translation.z * k};
  }
  const double w = std::fabs(lc.rotation.w) > 1.0 ? 1.0 : std::fabs(lc.rotation.w);
  if (2.0 * std::acos(w) > kMaxLateRad) return;
  const hp::Vec3 p = hp::Apply(lc, {m.pos[0], m.pos[1], m.pos[2]});
  if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return;
  pos[0] = static_cast<float>(p.x);
  pos[1] = static_cast<float>(p.y);
  pos[2] = static_cast<float>(p.z);
  if (m.flags & audv::meta_flags::kNear) {
    const double rNow = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    if (rNow > 1e-3 && m.dist > 0) {
      float g = static_cast<float>(m.dist / rNow);
      *gainOut = g < kTransMin ? kTransMin : (g > kTransMax ? kTransMax : g);
    }
  }
}

// ---- RePan: the mono signal back with DCS's gains (fallback, kill switch, release) -----------------------------------
// out[2i] += wgt(i) x(i) rL(i), out[2i+1] += wgt(i) x(i) rR(i); wgt and the ratios linear across the call from their
// start (i = 0) to their end value (i = frames). x(i) = scale (a[2i] + b[2i]) (b may be null). Non-finite samples
// are skipped (counted). Returns the non-finite count.
inline uint32_t RePan(const float* a, const float* b, float scale, uint32_t frames, float w0, float w1, float rL0,
                      float rL1, float rR0, float rR1, float* out) {
  if (!frames || (w0 == 0.0f && w1 == 0.0f)) return 0;
  const float inv = 1.0f / static_cast<float>(frames);
  uint32_t bad = 0;
  for (uint32_t i = 0; i < frames; ++i) {
    const float t = static_cast<float>(i) * inv;
    const float w = w0 + (w1 - w0) * t;
    const float x = scale * (a[2 * i] + (b ? b[2 * i] : 0.0f));
    const float l = w * x * (rL0 + (rL1 - rL0) * t);
    const float r = w * x * (rR0 + (rR1 - rR0) * t);
    if (!std::isfinite(l) || !std::isfinite(r)) {
      ++bad;
      continue;
    }
    out[2 * i] += l;
    out[2 * i + 1] += r;
  }
  return bad;
}

// out += (1 - w(i)) scratch (both channels), w linear w0 -> w1 across the call.
inline void AddStockWeighted(const float* scratch, uint32_t frames, float w0, float w1, float* out) {
  const float inv = 1.0f / static_cast<float>(frames);
  for (uint32_t i = 0; i < frames; ++i) {
    const float k = 1.0f - (w0 + (w1 - w0) * static_cast<float>(i) * inv);
    out[2 * i] += k * scratch[2 * i];
    out[2 * i + 1] += k * scratch[2 * i + 1];
  }
}

// ---- per-slot render state and the binding -------------------------------------------------------------------------
struct Rs {
  void* voice = nullptr;
  uint32_t gen = 0;
  bool prevTagged = false;
  bool live = false;        // the core slot holds this voice's state (else the next HRTF call resets it)
  bool haveMeta = false;
  float hW = 0;             // HRTF weight at the end of the last block
  float rL = 0, rR = 0;     // re-pan ratios at the end of the last block
  float lastTag = 0;        // the tag value of the last tagged snapshot (the ramp's start in the next block)
  float tornS0 = 0;         // > 0: the last block's snapshot was torn; this block's gain ramp starts from that sum
  MetaData meta{};          // the last consistent read
  MetaData prev{};          // the one before (pairs a snapshot written before the newest metadata)
};

struct Binding {
  void* renderer = nullptr;
  uint32_t rate = 0;
  binaural::Renderer* core = nullptr;
  float* scratch = nullptr;  // 2 x kMaxFrames, 64-byte aligned
  uint32_t* overrun = nullptr;  // ED's overrun counter of this WASAPI renderer
  uint32_t lastOverrun = 0;
  bool haveOverrun = false;
  Rs rs[audv::kSlots];
  std::atomic<int64_t> lastUseQpc{0};
  std::atomic<bool> retired{false};
  // budget (render thread only)
  uint32_t secBlocks = 0, secOver = 0, secHalf = 0;
  int64_t secStart = 0;
  double calmSeconds = 0;
  int64_t overrunTimes[3] = {};
  int overrunHead = 0;
  double prevShare = 0;        // our share of the previous block (dspTicks / block duration)
  uint32_t overrunGen = 0;     // the module's re-arm generation these overrun times belong to
};

// What one armed device block carries (render thread, set by MixWrap, read by RenderWrap).
struct Block {
  Binding* b = nullptr;
  float* out = nullptr;
  uint32_t frames = 0;
  bool on = false;          // binaural wanted now (module on, kill switch off, not off for the session)
  bool late = true;
  float trim = 1.0f;
  PoseNow pose;
  bool coreBegun = false;
  int64_t dspTicks = 0;     // QPC ticks spent in our DSP this block
  uint32_t hrtfVoices = 0;
};

struct MxcsrScope {  // FTZ | DAZ for our DSP, the caller's mode back afterwards
  unsigned int saved;
  MxcsrScope() : saved(_mm_getcsr()) { _mm_setcsr(saved | 0x8040u); }
  ~MxcsrScope() { _mm_setcsr(saved); }
};

// The output rule for one candidate voice. scratch: the voice's own render (2 x frames); snap: its snapshot gains
// (ch0.a, ch0.b, ch1.a, ch1.b) after that render; metaOk/meta: a consistent read of its slot (else rs.meta is used);
// ended: voice_render returned 0. Adds to blk.out.
inline void ProcessVoice(Block& blk, Counters& cnt, int slot, void* voice, float* scratch, const float snap[4],
                         bool metaOk, const MetaData& metaIn, bool ended, std::atomic<uint32_t>* ackGen) {
  Binding& b = *blk.b;
  Rs& rs = b.rs[slot];
  const uint32_t frames = blk.frames;
  float* out = blk.out;
  MetaData meta = metaIn;
  if (!metaOk) {
    if (!rs.haveMeta || rs.voice != voice) {
      // No consistent metadata yet: the stock part only (a tagged voice plays centred for this block).
      if (audv::IsTag(snap)) cnt.orphanTagged.fetch_add(1, std::memory_order_relaxed);
      for (uint32_t i = 0; i < 2 * frames; ++i) out[i] += scratch[i];
      return;
    }
    meta = rs.meta;
  }
  if (rs.voice != voice || rs.gen != meta.gen) {
    if (rs.live) audr::CoreResetSource(b.core, static_cast<uint32_t>(slot));
    rs = Rs{};
    rs.voice = voice;
    rs.gen = meta.gen;
    // Owned from its creation: its first block is already tagged and has no stock past to fade from.
    rs.prevTagged = (meta.flags & audv::meta_flags::kInitial) != 0 && audv::IsTag(snap);
    if (rs.prevTagged) rs.lastTag = snap[0];
    rs.hW = blk.on && (meta.flags & audv::meta_flags::kOwned) ? 1.0f : 0.0f;
  }
  if (metaOk && (!rs.haveMeta || meta.frame != rs.meta.frame)) {
    rs.prev = rs.haveMeta ? rs.meta : meta;
    rs.meta = meta;
    rs.haveMeta = true;
  }
  const bool tagSnap = audv::IsTag(snap);
  const bool prevTagged = rs.prevTagged;
  const bool owned = (meta.flags & audv::meta_flags::kOwned) != 0;
  const float hTarget = blk.on && owned && b.core ? 1.0f : 0.0f;
  const float sumNow = snap[0] + snap[1] + snap[2] + snap[3];
  // A torn snapshot (see the top of this file): DCS's stereo gains for an owned, tagged voice whose metadata still says
  // owned. Not twice in a row (then it is taken as what it looks like).
  const bool torn = !tagSnap && prevTagged && owned && !ended && rs.tornS0 == 0.0f && rs.lastTag > 0.0f &&
                    snap[1] == 0.0f && snap[2] == 0.0f && snap[0] > 0.0f && snap[3] > 0.0f && std::isfinite(sumNow);
  if (torn) cnt.tornSnaps.fetch_add(1, std::memory_order_relaxed);
  const bool tagged = tagSnap || torn;

  if (!prevTagged && !tagged) {
    for (uint32_t i = 0; i < 2 * frames; ++i) out[i] += scratch[i];
    cnt.passCandidates.fetch_add(1, std::memory_order_relaxed);
    rs.hW = hTarget;
    rs.tornS0 = 0.0f;
    if (!owned || ended) ackGen[slot].store(meta.gen, std::memory_order_release);
    if (ended && rs.live) {
      audr::CoreResetSource(b.core, static_cast<uint32_t>(slot));
      rs.live = false;
    }
    return;
  }

  // The metadata that belongs to this snapshot: its tag value names it (the sim may have written a newer frame).
  const MetaData& pm = tagSnap && rs.prev.mTag == snap[0] && rs.meta.mTag != snap[0] ? rs.prev : rs.meta;
  const float mRef = pm.mTag > 0 ? pm.mTag : 1.0f;
  float rL1 = pm.gL / mRef, rR1 = pm.gR / mRef;
  if (!std::isfinite(rL1) || !std::isfinite(rR1)) rL1 = rR1 = 0.5f;
  if (rs.rL == 0 && rs.rR == 0) rs.rL = rL1, rs.rR = rR1;

  // Weights. w: ownership fade (1 steady); h: HRTF share of the tagged part.
  float w0 = 1, w1 = 1, h0 = rs.hW, h1 = rs.hW;
  const float* a = scratch;
  const float* bb = nullptr;
  float scale = 1.0f;
  if (prevTagged && tagged) {
    h1 = hTarget;
    cnt.steady.fetch_add(1, std::memory_order_relaxed);
  } else {
    a = scratch;
    bb = scratch + 1;
    scale = 0.5f;
    if (tagged) {  // fade in
      w0 = 0, w1 = 1;
      cnt.fadeIns.fetch_add(1, std::memory_order_relaxed);
    } else {       // fade out
      w0 = 1, w1 = 0;
      cnt.fadeOuts.fetch_add(1, std::memory_order_relaxed);
    }
    AddStockWeighted(scratch, frames, w0, w1, out);
  }
  // A block whose gain ramp does not run tag to tag (this snapshot torn, or the last one): the mono signal at the tag's
  // level from both channels and the known ramp of their gain sum, mono(i) = (L + R) M(t) / S(t), into the scratch's
  // left channel (the stock part above is already added). DCS ramps each gain linearly, t = (i + 1) / frames.
  if (torn || rs.tornS0 > 0.0f) {
    const float s0 = torn ? 2.0f * rs.lastTag : rs.tornS0;
    const float s1 = tagSnap ? 2.0f * snap[0] : sumNow;
    const float m0 = rs.lastTag, m1 = tagSnap ? snap[0] : rs.lastTag;
    const float inv = 1.0f / static_cast<float>(frames);
    for (uint32_t i = 0; i < frames; ++i) {
      const float t = static_cast<float>(i + 1) * inv;
      const float s = s0 + (s1 - s0) * t;
      scratch[2 * i] = s > 0.0f ? (scratch[2 * i] + scratch[2 * i + 1]) * ((m0 + (m1 - m0) * t) / s) : 0.0f;
    }
    a = scratch;
    bb = nullptr;
    scale = 1.0f;
  }
  // HRTF part: weight w(i) h(i). Only one of the two ramps moves in a block, so the product stays linear.
  const float hw0 = w0 * h0, hw1 = w1 * h1;
  float rpw0 = w0 * (1.0f - h0), rpw1 = w1 * (1.0f - h1);
  if (hw0 > 0.0f || hw1 > 0.0f) {
    binaural::SourceUpdate u{};
    float gT = 1.0f;
    CorrectedPosition(pm, blk.pose, blk.late, u.position, &gT);
    u.gain = blk.trim * gT;
    // Near field on/off is the core's own (Renderer::SetNearField fades it); no per-source exclusion here.
    u.flags = rs.live ? 0u : binaural::source_flags::kReset;
    const binaural::MonoInput in{a, bb, 2, scale};
    if (!blk.coreBegun) {
      audr::CoreBegin(b.core, frames);
      blk.coreBegun = true;
    }
    if (audr::CoreRender(b.core, static_cast<uint32_t>(slot), u, in, frames, hw0, hw1, out)) {
      rs.live = true;
      cnt.hrtfCalls.fetch_add(1, std::memory_order_relaxed);
      ++blk.hrtfVoices;
    } else {
      // Refused (or no core): that share is re-panned with DCS's gains instead.
      cnt.coreRejected.fetch_add(1, std::memory_order_relaxed);
      rpw0 += hw0;
      rpw1 += hw1;
      h1 = 0;
      rs.live = false;
    }
  } else if (rs.live) {
    // The core skipped this block (no HRTF share): its slot's history is stale now; the next call starts afresh.
    rs.live = false;
  }
  if (rpw0 > 0.0f || rpw1 > 0.0f) {
    const uint32_t bad = RePan(a, bb, scale, frames, rpw0, rpw1, rs.rL, rL1, rs.rR, rR1, out);
    if (bad) cnt.nonFinite.fetch_add(bad, std::memory_order_relaxed);
    cnt.repanCalls.fetch_add(1, std::memory_order_relaxed);
  }
  rs.hW = h1;
  rs.rL = rL1;
  rs.rR = rR1;
  rs.prevTagged = tagged;
  rs.tornS0 = torn ? sumNow : 0.0f;
  if (tagSnap) rs.lastTag = snap[0];
  if (!tagged && !owned) ackGen[slot].store(meta.gen, std::memory_order_release);  // faded back: DCS's again
  if (ended) {
    if (rs.live) audr::CoreResetSource(b.core, static_cast<uint32_t>(slot));
    rs.live = false;
    rs.prevTagged = false;
    rs.tornS0 = 0.0f;
    ackGen[slot].store(meta.gen, std::memory_order_release);
    cnt.voiceEnded.fetch_add(1, std::memory_order_relaxed);
  }
}

// ---- CPU budget and back-off (binaural_core.h) -------------------------------------------------------------------
struct BudgetCfg {
  int cpuBudgetPercent = 25;
  int maxObjects = 48;
};
// Called once per armed block after the mix. Returns the new limit in use when the back-off changed (else 0).
inline uint32_t BudgetTick(Binding& b, Counters& cnt, EvRing& ev, const BudgetCfg& cfg, uint32_t frames, int64_t ticks,
                           int64_t now, int64_t freq) {
  if (!b.rate || !freq) return 0;
  const double blockS = static_cast<double>(frames) / b.rate;
  const double usedS = static_cast<double>(ticks) / static_cast<double>(freq);
  const double budgetS = blockS * cfg.cpuBudgetPercent / 100.0;
  {
    const uint32_t tenth = static_cast<uint32_t>(std::fmin(usedS * 1e7, 4e9));
    const uint32_t h = cnt.timeHead.load(std::memory_order_relaxed);
    cnt.timeRing[h & 1023].store(tenth, std::memory_order_relaxed);
    cnt.timeHead.store(h + 1, std::memory_order_release);
    if (tenth > cnt.worstTenthUs.load(std::memory_order_relaxed)) cnt.worstTenthUs.store(tenth, std::memory_order_relaxed);
  }
  if (!b.secStart) b.secStart = now;
  ++b.secBlocks;
  if (usedS > budgetS) ++b.secOver;
  if (usedS > budgetS * 0.5) ++b.secHalf;
  // limitNow is the back-off cap (kMaxSources: none); the limit in use is min(cap, MaxObjects), so a Live change of
  // MaxObjects applies at once and a back-off is measured from the limit actually in use.
  const uint32_t cfgMax = static_cast<uint32_t>(cfg.maxObjects < 1 ? 1 : (cfg.maxObjects > 64 ? 64 : cfg.maxObjects));
  const uint32_t cap = cnt.limitNow.load(std::memory_order_relaxed);
  uint32_t newCap = cap;
  if (now - b.secStart >= freq) {
    const uint32_t limit = cap < cfgMax ? cap : cfgMax;
    const double secS = static_cast<double>(now - b.secStart) / freq;
    if (b.secBlocks && b.secOver * 10 > b.secBlocks) {  // more than 10 % of the blocks of the last second
      const uint32_t nl = limit > 16 ? limit - 8 : 8;
      if (nl != limit) {
        ev.Push({kEvBudgetDown, static_cast<uint16_t>(nl), b.secOver * 1000 / b.secBlocks, 0});
        newCap = nl;
      }
      b.calmSeconds = 0;
    } else if (b.secHalf == 0) {  // a whole second under half the budget
      b.calmSeconds += secS;
      if (b.calmSeconds >= 10.0 && cap < binaural::kMaxSources) {
        const uint32_t nl = limit + 4;
        newCap = nl >= cfgMax ? binaural::kMaxSources : nl;
        ev.Push({kEvBudgetUp, static_cast<uint16_t>(nl >= cfgMax ? cfgMax : nl), 0, 0});
        b.calmSeconds = 0;
      }
    } else {
      b.calmSeconds = 0;
    }
    b.secStart = now;
    b.secBlocks = b.secOver = b.secHalf = 0;
  }
  if (newCap == cap) return 0;
  cnt.limitNow.store(newCap, std::memory_order_release);
  return newCap < cfgMax ? newCap : cfgMax;
}

// ED's own overrun counter: true when the third overrun within 10 s happened while binaural rendered something.
inline bool OverrunTick(Binding& b, bool active, int64_t now, int64_t freq) {
  if (!b.overrun) return false;
  uint32_t v;
  __try {
    v = *reinterpret_cast<volatile uint32_t*>(b.overrun);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    b.overrun = nullptr;
    return false;
  }
  if (!b.haveOverrun) {
    b.haveOverrun = true;
    b.lastOverrun = v;
    return false;
  }
  const uint32_t d = v - b.lastOverrun;
  b.lastOverrun = v;
  if (!d || !active) return false;
  for (uint32_t k = 0; k < d && k < 3; ++k) {
    b.overrunTimes[b.overrunHead] = now;
    b.overrunHead = (b.overrunHead + 1) % 3;
  }
  int recent = 0;
  for (int64_t t : b.overrunTimes)
    if (t && now - t <= 10 * freq) ++recent;
  return recent >= 3;
}

}  // namespace audr
