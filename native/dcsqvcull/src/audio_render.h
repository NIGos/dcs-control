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
//
// Every owned voice carries the stereo tag (audio_voice.h IsStereoTag): the tagged scratch is m L', m R' (DCS's own
// channels after its width stage; a mono wave gives m s on both, ED's mono kernels play s (a + b)). In every block the
// two channels are first recovered from DCS's known gain ramps (RecoverChannels: fades, torn and half-written snapshots
// alike). The point source is the mid M = (L' + R') / 2, DCS's own stereo-to-mono rule; the side S = (L' - R') / 2 is
// what DCS's stereo adds on top (zero for a mono wave):
//   T = h HRTF(c' M) + (1-h) RePan(M) + (1-h) SidePan(S) + h q g SidePan(S delayed per ear by the HRTF's delay)
// where SidePan puts S back with DCS's gains (+gL left, -gR right), so h = 0 is DCS's stock stereo exactly. q = 0, a
// point source: c' = c (Loudness, 1..2) keeps the stereo's loudness. q = 1, a wide sound (a source near the head:
// kNear, under 1.4 m until past 1.6 m; or a diffuse bed by its own mid / side powers): c' = 1, DCS's side stays around
// a binaural mid, delayed per ear by that ear's HRTF delay (SideDelayed: they arrive together, no comb) and with the
// mid's level trim and late-translation gain g. The sdef's auto_width is not a reason for wide: ED's flag only narrows a
// sound with distance, and its width stays in L', R'. A change of q is crossfaded across one block (a new voice starts
// at its own q). c' also carries DCS's own level for the direction (PanLevel below: the HRTF part is as loud as DCS's
// own panning of that voice, the HRTF adds the direction's cues).
#pragma once

#include <immintrin.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

#include "audio_space.h"
#include "audio_voice.h"
#include "binaural_core.h"
#include "binaural_room.h"  // cockpit space (native/binaural, or the module's stand-in until CORE is merged)
#include "head_pose_reader.h"  // head_pose_shm.h and its checked readers (staleness, writer, speed guards)

namespace audr {

using audv::MetaData;

constexpr uint32_t kMaxFrames = binaural::kMaxFramesPerCall;  // longer device blocks pass through (counted)
constexpr float kNearM = 1.5f;                                 // a voice's first frame: near (kNear) below this
// Near (meta_flags::kNear: kept wide, the late-translation gain) with hysteresis per voice: entered under kNearInM,
// left past kNearOutM (a cockpit sound moving around 1.5 m does not toggle between point and wide).
constexpr float kNearInM = 1.4f, kNearOutM = 1.6f;
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
inline bool CoreDelays(const binaural::Renderer* r, uint32_t slot, float out[2]) { return r->SourceDelays(slot, out); }
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
inline bool CoreDelays(const binaural::Renderer*, uint32_t, float*) { return false; }
inline void CoreEnd(binaural::Renderer*) {}
inline binaural::Stats CoreStats(const binaural::Renderer*) { return binaural::Stats{}; }
inline binaural::HrtfSet* CoreLoadHrtf(const void*, size_t, uint32_t, char* e, size_t en) {
  if (en) strncpy_s(e, en, "no binaural renderer in this build", _TRUNCATE);
  return nullptr;
}
inline void CoreReleaseHrtf(binaural::HrtfSet*) {}
inline binaural::HrtfInfo CoreDescribe(const binaural::HrtfSet*) { return binaural::HrtfInfo{}; }
#endif

// ---- cockpit space (docs/COCKPIT_SPACE.md 4, 8.2): the core's Room (binaural_room.h). Builds without a Room
// implementation (the core's src/room.cpp not merged yet: the module's stand-in header) create none: no room call is
// ever made and the output is exactly today's. ----------------------------------------------------------------------
#ifndef DCSQV_HAVE_ROOM
#define DCSQV_HAVE_ROOM 0
#endif
constexpr bool kHaveRoom = DCSQV_HAVE_ROOM != 0;
#if DCSQV_HAVE_ROOM
inline binaural::Room* RoomNew() { return new (std::nothrow) binaural::Room(); }
inline void RoomDelete(binaural::Room* r) { delete r; }
inline bool RoomInit(binaural::Room* r, uint32_t rate, uint32_t n, char* e, size_t en) { return r->Init(rate, n, e, en); }
inline void RoomSetHrtf(binaural::Room* r, const binaural::HrtfSet* s) { r->SetHrtf(s); }
inline const binaural::HrtfSet* RoomTakeRetired(binaural::Room* r) { return r->TakeRetiredHrtf(); }
inline void RoomSetHeadRadius(binaural::Room* r, float m) { r->SetHeadRadius(m); }
inline void RoomSetParams(binaural::Room* r, const binaural::RoomParams& p) { r->SetParams(p); }
inline void RoomBegin(binaural::Room* r, uint32_t frames, const binaural::RoomFrame& f) { r->BeginBlock(frames, f); }
inline bool RoomAdd(binaural::Room* r, uint32_t slot, const binaural::RoomSource& s, const binaural::MonoInput& in,
                    uint32_t frames, float w0, float w1) {
  return r->AddSource(slot, s, in, frames, w0, w1);
}
inline void RoomReset(binaural::Room* r, uint32_t slot) { r->ResetSource(slot); }
inline void RoomRender(binaural::Room* r, float* out, uint32_t frames) { r->Render(out, frames); }
inline void RoomEnd(binaural::Room* r) { r->EndBlock(); }
inline bool RoomIdle(const binaural::Room* r) { return r->Idle(); }
inline float RoomDirect(const binaural::RoomParams& p, float wet, float canopy, bool early, const float pos[3], uint32_t f) {
  return binaural::RoomDirectGain(p, wet, canopy, early, pos, f);
}
inline float RoomSendGain(const binaural::RoomParams& p, const float pos[3], uint32_t f) {
  return binaural::RoomSendDistanceGain(p, pos, f);
}
#else
inline binaural::Room* RoomNew() { return nullptr; }
inline void RoomDelete(binaural::Room*) {}
inline bool RoomInit(binaural::Room*, uint32_t, uint32_t, char* e, size_t en) {
  if (en) strncpy_s(e, en, "no room renderer in this build", _TRUNCATE);
  return false;
}
inline void RoomSetHrtf(binaural::Room*, const binaural::HrtfSet*) {}
inline const binaural::HrtfSet* RoomTakeRetired(binaural::Room*) { return nullptr; }
inline void RoomSetHeadRadius(binaural::Room*, float) {}
inline void RoomSetParams(binaural::Room*, const binaural::RoomParams&) {}
inline void RoomBegin(binaural::Room*, uint32_t, const binaural::RoomFrame&) {}
inline bool RoomAdd(binaural::Room*, uint32_t, const binaural::RoomSource&, const binaural::MonoInput&, uint32_t, float, float) {
  return false;
}
inline void RoomReset(binaural::Room*, uint32_t) {}
inline void RoomRender(binaural::Room*, float*, uint32_t) {}
inline void RoomEnd(binaural::Room*) {}
inline bool RoomIdle(const binaural::Room*) { return true; }
inline float RoomDirect(const binaural::RoomParams&, float, float, bool, const float*, uint32_t) { return 1.0f; }
inline float RoomSendGain(const binaural::RoomParams&, const float*, uint32_t) { return 1.0f; }
#endif

// What the control thread publishes for the render side's room (seqlock, read once per armed block).
struct SpaceCtl {
  int level = 0;              // CockpitSpace 0..3 (0: the room fades out and goes idle; no SetParams)
  bool haveParams = false;    // params hold a preset (the room got the same through SetParams)
  binaural::RoomParams params{};
  float canopy = -1.0f;       // 0..1 from the Lua bridge (CockpitSpaceCanopy on), -1 unknown (closed)
  int luaInside = -1;         // the Lua bridge's camera check: 1 cockpit, 0 external view, -1 unknown
};
struct SpaceCtlSlot {
  std::atomic<uint32_t> seq{0};
  SpaceCtl d;
};
inline void WriteCtl(SpaceCtlSlot& s, const SpaceCtl& c) {
  const uint32_t q = s.seq.load(std::memory_order_relaxed);
  s.seq.store(q + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  s.d = c;
  std::atomic_thread_fence(std::memory_order_release);
  s.seq.store(q + 2, std::memory_order_release);
}
inline bool ReadCtl(const SpaceCtlSlot& s, SpaceCtl* out) {
  for (int i = 0; i < 8; ++i) {
    const uint32_t a = s.seq.load(std::memory_order_acquire);
    if (a & 1u) continue;
    SpaceCtl c = s.d;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (s.seq.load(std::memory_order_relaxed) != a) continue;
    *out = c;
    return true;
  }
  return false;
}
constexpr double kWetRampS = 0.3;     // inside <-> outside, level on <-> off: linear
constexpr double kCanopyTauS = 0.3;   // canopy: smoothed (it moves over seconds)

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
  kEvEarlyOff,       // cockpit space back-off step 1: early reflections off (b: over-budget blocks, per mille)
  kEvEarlyOn,        // early reflections on again (10 s under half the budget, MaxObjects back first)
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
  kRefuseOutput,         // a 3D sound, but no output bound, or the voice plays on another one (a second DCS output)
  kRefuseNot3D,          // no sound, a 2D sound, or a voice of a 2D context (on any output)
  kRefuseOffline,        // ed_sound's engine state offline
  kRefuseChannels,       // a wave of more than two channels
  kRefuseContext,        // its context is not one of the array's, is excluded ([Audio] ExcludeContexts) or 2D
  kRefuseDistance,       // nearer than the minimum distance, or not finite
  kRefuseStereoOut,      // the bound output is not stereo
  kRefuseGains,          // DCS's gains are not its stereo format
  kRefusePanMismatch,    // the pan law check failed
  kRefuseNotCreation,    // qualifies, but a voice is taken only when it starts
  kRefuseLimit,          // qualifies at its start, but the sound limit was reached
  kRefuseNoWave,         // a stream voice before its first chunk (wave channels still 0, ed_sound 0x16109)
  kRefuseCount
};
struct Counters {
  std::atomic<uint64_t> refused[kRefuseCount] = {};
  std::atomic<uint64_t> blocks{0}, armedBlocks{0}, candidateCalls{0}, hrtfCalls{0}, repanCalls{0}, fadeIns{0},
      fadeOuts{0}, steady{0}, passCandidates{0}, coreRejected{0}, orphanTagged{0}, nonFinite{0}, framesTooLong{0},
      voiceEnded{0}, unarmedCalls{0}, overruns{0}, poseBlocks{0}, tornSnaps{0}, passBlocks{0}, stereoCalls{0};
  std::atomic<uint64_t> panCalls{0}, panSolves{0}, panMismatch{0}, panMedian{0}, ownedVoiceFrames{0}, simFrames{0};
  std::atomic<uint32_t> ownedNow{0}, limitNow{binaural::kMaxSources}, activeLastBlock{0};
  std::atomic<uint32_t> ownedStereoNow{0};  // of ownedNow: stereo waves (the sim)
  std::atomic<uint32_t> ownedWideNow{0};    // tagged voices kept wide in the last armed block (under 1.4 / 1.6 m or diffuse)
  // What the render side did with each slot's voice in its last block (the sound log, control thread): relaxed, each
  // field on its own (a row may mix two blocks; gen names the voice).
  struct VoiceView {
    std::atomic<uint32_t> gen{0};
    std::atomic<uint32_t> state{0};       // kViewTagged | kViewDiffuse | kViewWide; 0: not rendered by us
    std::atomic<float> comp{1.0f};        // the loudness factor sqrt((P_M + P_S) / P_M)
    std::atomic<float> panLevel{1.0f};    // DCS's level for the direction (PanLevel)
    std::atomic<float> power{0.0f};       // smoothed P_M + P_S of m L', m R' (per frame): the point / wide energy share
    // cockpit space (audio_voices.csv): sent to the room in its last block, the send and direct gains, the
    // aircraft-frame position
    std::atomic<uint32_t> room{0};
    std::atomic<float> sendDb{0.0f}, dcompDb{0.0f};
    std::atomic<float> ax{0.0f}, ay{0.0f}, az{0.0f};
  };
  static constexpr uint32_t kViewTagged = 1, kViewDiffuse = 2, kViewWide = 4;
  VoiceView view[audv::kSlots];
  // Block time of our own DSP, microseconds x 10, the last 1024 armed blocks (single writer: the render thread).
  std::atomic<uint32_t> timeRing[1024];
  std::atomic<uint32_t> timeHead{0};
  std::atomic<uint32_t> worstTenthUs{0};
  // Cockpit space: the room's own time per armed block it ran (microseconds x 10, last 1024), the voices sent to it in
  // the last armed block, their mean direct-gain compensation (dB), the room's state at the last block's end.
  std::atomic<uint32_t> roomRing[1024];
  std::atomic<uint32_t> roomHead{0};
  std::atomic<uint32_t> roomVoicesNow{0};
  std::atomic<float> dcompMeanDb{0.0f};
  std::atomic<float> roomWet{0.0f}, roomCanopy{0.0f};
  std::atomic<uint32_t> roomFrameValid{0}, roomEarly{1};
  std::atomic<float> roomC[4] = {};  // C_now of the last room block (aircraft -> head)
  std::atomic<uint64_t> roomBlocks{0}, roomSends{0}, roomRefused{0};
  void Reset() {  // the module's install (and the tests): every counter back to its start value
    for (std::atomic<uint64_t>* c : {&blocks, &armedBlocks, &candidateCalls, &hrtfCalls, &repanCalls, &fadeIns, &fadeOuts,
                                     &steady, &passCandidates, &coreRejected, &orphanTagged, &nonFinite, &framesTooLong,
                                     &voiceEnded, &unarmedCalls, &overruns, &poseBlocks, &tornSnaps, &passBlocks, &panCalls, &panSolves,
                                     &panMismatch, &panMedian, &ownedVoiceFrames, &simFrames, &stereoCalls})
      c->store(0, std::memory_order_relaxed);
    for (std::atomic<uint64_t>& c : refused) c.store(0, std::memory_order_relaxed);
    ownedNow.store(0, std::memory_order_relaxed);
    ownedStereoNow.store(0, std::memory_order_relaxed);
    ownedWideNow.store(0, std::memory_order_relaxed);
    for (VoiceView& v : view) {
      v.gen.store(0, std::memory_order_relaxed);
      v.state.store(0, std::memory_order_relaxed);
    }
    limitNow.store(binaural::kMaxSources, std::memory_order_relaxed);
    activeLastBlock.store(0, std::memory_order_relaxed);
    timeHead.store(0, std::memory_order_relaxed);
    worstTenthUs.store(0, std::memory_order_relaxed);
    roomHead.store(0, std::memory_order_relaxed);
    roomVoicesNow.store(0, std::memory_order_relaxed);
    dcompMeanDb.store(0.0f, std::memory_order_relaxed);
    roomWet.store(0.0f, std::memory_order_relaxed);
    roomCanopy.store(0.0f, std::memory_order_relaxed);
    roomFrameValid.store(0, std::memory_order_relaxed);
    roomEarly.store(1, std::memory_order_relaxed);
    for (std::atomic<uint64_t>* c : {&roomBlocks, &roomSends, &roomRefused}) c->store(0, std::memory_order_relaxed);
    for (VoiceView& v : view) v.room.store(0, std::memory_order_relaxed);
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
// *gainOut: the late-translation gain for sources near the head (meta_flags::kNear).
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

// SidePan: a stereo voice's side with DCS's own gains: s(i) = (a[2i] - b[2i]) / 2 (b = a + 1: the two channels of the
// scratch), out_L += wgt s rL, out_R -= wgt s rR. With RePan of the mid (scale 1/2) it gives DCS's stock stereo output
// m L' gL / m, m R' gR / m back exactly (L' = M + S, R' = M - S). Returns the non-finite count.
inline uint32_t SidePan(const float* a, const float* b, uint32_t frames, float w0, float w1, float rL0, float rL1,
                        float rR0, float rR1, float* out) {
  if (!frames || (w0 == 0.0f && w1 == 0.0f)) return 0;
  const float inv = 1.0f / static_cast<float>(frames);
  uint32_t bad = 0;
  for (uint32_t i = 0; i < frames; ++i) {
    const float t = static_cast<float>(i) * inv;
    const float w = w0 + (w1 - w0) * t;
    const float x = 0.5f * (a[2 * i] - b[2 * i]);
    const float l = w * x * (rL0 + (rL1 - rL0) * t);
    const float r = w * x * (rR0 + (rR1 - rR0) * t);
    if (!std::isfinite(l) || !std::isfinite(r)) {
      ++bad;
      continue;
    }
    out[2 * i] += l;
    out[2 * i + 1] -= r;
  }
  return bad;
}

// The side of a wide voice next to its HRTF mid, delayed per ear by that ear's HRTF delay (the core's onset / ITD
// delay plus its 12 samples of look-ahead, Renderer::SourceDelays): both reach each ear together. Undelayed, DCS's
// side led the mid by 0.5-1 ms (24-48 samples at 48 kHz): comb notches from about 1 kHz wherever mid and side are
// correlated, and the precedence effect pulled the image toward DCS's own panning. Whole samples: half a sample of
// error is 10 us, a notch above 48 kHz.
//   u(k) = v(k) s(k), s = (a - b) / 2, v linear v0 -> v1 across the call (the weight goes on the input, so a fade, a
//   refusal or a delay change is a continuous signal at the output); with `rampEnd` < frames, v reaches v1 at that
//   frame and stays there (a voice's last block: its ramp to 0 ends `delay` frames early, so no tail is left in the
//   history when the slot goes back to DCS);
//   y_e(i) = (1 - t) u(i - D0_e) + t u(i - D1_e), t = i / frames (a changed delay is crossfaded across the call);
//   out_L += y_L rL(i), out_R -= y_R rR(i), the ratios linear as in SidePan.
// `hist` holds u of the kSideHist samples before this call (oldest first) and receives this call's; `u` is scratch of
// `frames` floats. *zeroRun counts the zero samples at the history's end (>= kSideHist: all zero, the call is skipped
// when v stays 0). Returns the non-finite count (those samples enter as 0).
constexpr uint32_t kSideHist = 512;  // > 4 ms (kMaxDelayMs) + 12 samples at 96 kHz
inline uint32_t SideDelayed(const float* a, const float* b, uint32_t frames, float v0, float v1, const int d0[2],
                            const int d1[2], float rL0, float rL1, float rR0, float rR1, float* u, float* hist,
                            uint32_t* zeroRun, float* out, uint32_t rampEnd = 0xffffffffu) {
  if (!frames || (v0 == 0.0f && v1 == 0.0f && *zeroRun >= kSideHist)) return 0;
  const float inv = 1.0f / static_cast<float>(frames);
  const uint32_t re = rampEnd < frames ? rampEnd : frames;
  const float invRamp = re ? 1.0f / static_cast<float>(re) : 0.0f;
  uint32_t bad = 0;
  for (uint32_t i = 0; i < frames; ++i) {
    const float v = i >= re ? v1 : v0 + (v1 - v0) * static_cast<float>(i) * invRamp;
    const float x = v * 0.5f * (a[2 * i] - b[2 * i]);
    if (!std::isfinite(x)) {
      ++bad;
      u[i] = 0.0f;
    } else {
      u[i] = x;
    }
  }
  auto at = [&](int32_t k) { return k >= 0 ? u[k] : hist[static_cast<int32_t>(kSideHist) + k]; };
  for (uint32_t i = 0; i < frames; ++i) {
    const float t = static_cast<float>(i) * inv;
    const int32_t n = static_cast<int32_t>(i);
    const float yl = d0[0] == d1[0] ? at(n - d0[0]) : (1.0f - t) * at(n - d0[0]) + t * at(n - d1[0]);
    const float yr = d0[1] == d1[1] ? at(n - d0[1]) : (1.0f - t) * at(n - d0[1]) + t * at(n - d1[1]);
    out[2 * i] += yl * (rL0 + (rL1 - rL0) * t);
    out[2 * i + 1] -= yr * (rR0 + (rR1 - rR0) * t);
  }
  if (frames >= kSideHist) {
    memcpy(hist, u + (frames - kSideHist), sizeof(float) * kSideHist);
  } else {
    memmove(hist, hist + frames, sizeof(float) * (kSideHist - frames));
    memcpy(hist + (kSideHist - frames), u, sizeof(float) * frames);
  }
  *zeroRun = v0 == 0.0f && v1 == 0.0f ? (*zeroRun > kSideHist ? kSideHist : *zeroRun) + frames : 0;
  return bad;
}
// An ear delay (samples) as the history's whole-sample delay: rounded, 0 .. kSideHist - 1.
inline int SideDelaySamples(float d) {
  if (!(d > 0.0f)) return 0;
  const float r = std::floor(d + 0.5f);
  return r >= static_cast<float>(kSideHist - 1) ? static_cast<int>(kSideHist - 1) : static_cast<int>(r);
}

// The loudness of a stereo voice's mid against its stereo: (P_M + P_S) / P_M of this block's m L', m R'. The mid alone
// keeps DCS's own stereo-to-mono rule (M = (L + R) / 2, what its stream decoder does at 0x16260 and its width stage
// gives at w = 0) and is as loud as the stereo when the channels are equal (most of DCS's stereo waves); for
// decorrelated channels it is up to 3 dB quieter, so the HRTF part is raised by sqrt of this ratio, smoothed over
// kLoudTau and limited to kMaxLoudComp (anti-phase content is not chased).
constexpr float kLoudTau = 0.05f;      // seconds
constexpr float kMaxLoudComp = 2.0f;   // +6 dB
inline void BlockMidSide(const float* lr, uint32_t frames, double* pm, double* ps) {
  double m = 0, s = 0;
  for (uint32_t i = 0; i < frames; ++i) {
    const double a = lr[2 * i], b = lr[2 * i + 1];
    m += 0.25 * (a + b) * (a + b);
    s += 0.25 * (a - b) * (a - b);
  }
  *pm = m;
  *ps = s;
}

// A stereo wave whose channels stay this decorrelated is a diffuse bed (wind, rain, cabin noise), not a point: its
// side keeps DCS's stereo around the binaural mid (wide). Entered when the loudness factor stays above kDiffuseOn for
// kDiffuseHoldS, left when it stays under kDiffuseOff as long; the change is crossfaded over one block (Rs::qW).
constexpr float kDiffuseOn = 1.3f;     // P_S / P_M above 0.69
constexpr float kDiffuseOff = 1.15f;   // P_S / P_M under 0.32
constexpr double kDiffuseHoldS = 0.2;

// DCS's own level of a voice against its frontal gain m: sqrt((gL^2 + gR^2) / (2 m^2)) from its pan ratios gL / m,
// gR / m. ed_sound's pan law gives a frontal source m on both channels and every other direction less (about -1.3 dB
// at the sides, -2.5 dB above, -6 dB behind without crosstalk), broadband. The HRTF is normalised to the front only
// and at low frequencies, where DCS's engine and cockpit sounds carry most of their power, it is about as loud from
// every direction (SADIE II KU100, 100-400 Hz: within 1.7 dB of the front all round). So the HRTF part carries DCS's
// own level for the direction and the HRTF adds the interaural and spectral cues: the kill switch keeps the level
// (flight of 2026-11-25: cockpit sounds behind and above the pilot came out 2.3-2.8 dB louder with the mid at m).
// m is DCS's gain for c = 0.5 on both channels (the frontal gain of a +-60 degree speaker pair, audio_voice.h
// SolveMono); the level is computed from DCS's actual gains, so other speaker directions are carried as they are (a
// pair at +-30 degrees plays the front at (1.87 WA + WB) / m, above 1). The cap is only a guard: with any speaker
// direction (c <= 1) a gain is at most (2 WA + WB) / (1.5 WA + WB) <= 4/3 of m. Non-finite ratios give 1.
constexpr float kPanLevelMin = 0.25f;   // -12 dB (the pan law's own floor is about -6 dB)
constexpr float kPanLevelMax = 4.0f / 3.0f;
inline float PanLevel(float rL, float rR) {
  const float p = 0.5f * (rL * rL + rR * rR);
  if (!std::isfinite(p)) return 1.0f;
  const float l = std::sqrt(p);
  return l < kPanLevelMin ? kPanLevelMin : (l > kPanLevelMax ? kPanLevelMax : l);
}

// The mid / side estimate of one voice (render thread). Non-finite input leaves it untouched; a non-finite state
// (never expected) is dropped and rebuilt from the next finite block.
struct Loudness {
  double powM = 0, powS = 0;  // smoothed per-frame powers of m L', m R''s mid and side
  bool have = false;
  float comp = 1;             // sqrt((P_M + P_S) / P_M), 1 .. kMaxLoudComp
  double hiS = 0, loS = 0;    // time above kDiffuseOn / under kDiffuseOff
  bool diffuse = false;
  // One block of m L', m R' (exact: recovered from DCS's known gain ramps).
  void Update(const float* lr, uint32_t frames, uint32_t rate) {
    if (!frames) return;
    double pm = 0, ps = 0;
    BlockMidSide(lr, frames, &pm, &ps);
    pm /= frames;
    ps /= frames;
    if (!std::isfinite(powM) || !std::isfinite(powS)) have = false;
    if (!std::isfinite(pm) || !std::isfinite(ps)) return;
    const bool first = !have;
    if (!have) {
      powM = pm, powS = ps, have = true;
    } else {
      const double k = 1.0 - std::exp(-static_cast<double>(frames) / (static_cast<double>(rate ? rate : 48000) * kLoudTau));
      powM += k * (pm - powM);
      powS += k * (ps - powS);
    }
    if (powM + powS > 1e-24) {
      const double c = powM > 0 ? std::sqrt((powM + powS) / powM) : kMaxLoudComp;
      comp = static_cast<float>(c < 1.0 ? 1.0 : (c > kMaxLoudComp ? kMaxLoudComp : c));
    }
    const double dt = static_cast<double>(frames) / (rate ? rate : 48000);
    hiS = comp > kDiffuseOn ? hiS + dt : 0.0;
    loS = comp < kDiffuseOff ? loS + dt : 0.0;
    // A voice's first block decides at once (a bed starts as a bed); later changes need kDiffuseHoldS.
    if (!diffuse && (hiS >= kDiffuseHoldS || (first && comp > kDiffuseOn))) diffuse = true;
    if (diffuse && loS >= kDiffuseHoldS) diffuse = false;
  }
};

// DCS's own channels back from a block whose gains ramped linearly (t = (i + 1) / frames) from (l0, r0) to (l1, r1)
// on its two output channels, scaled to the tag's level m0 -> m1: lr[2i] = m(t) L', lr[2i + 1] = m(t) R'. Exact for
// the stereo tag, DCS's own stereo gains and any mix of them (a fade, a torn snapshot). A zero gain gives zero.
inline void RecoverChannels(float* lr, uint32_t frames, float l0, float l1, float r0, float r1, float m0, float m1) {
  if (l0 == m0 && r0 == m0 && l1 == m1 && r1 == m1) return;  // tag to tag: already m L', m R'
  const float inv = 1.0f / static_cast<float>(frames);
  for (uint32_t i = 0; i < frames; ++i) {
    const float t = static_cast<float>(i + 1) * inv;
    const float gl = l0 + (l1 - l0) * t, gr = r0 + (r1 - r0) * t, mt = m0 + (m1 - m0) * t;
    lr[2 * i] = gl > 0.0f ? lr[2 * i] * (mt / gl) : 0.0f;
    lr[2 * i + 1] = gr > 0.0f ? lr[2 * i + 1] * (mt / gr) : 0.0f;
  }
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
  float tornS0 = 0;         // > 0: the last block's snapshot was torn (not taken as torn twice in a row)
  float snapL = 0, snapR = 0;  // the last block's snapshot gains ch0.a, ch1.b: where this block's ramps start
  bool haveSnap = false;
  Loudness loud;            // the mid / side estimate (loudness factor, diffuse)
  float qW = 0;             // the side's share at the end of the last block: 0 point source, 1 wide (DCS's side kept)
  bool haveQ = false;       // qW holds a decision (else a new voice starts at its own: no fade of its side from 0)
  // The side's two parts at the end of the last block (where this block's ramps start, whatever happened since):
  bool haveSide = false;
  float sideRp = 0;         // the re-panned share's side (DCS's, undelayed)
  float sideV = 0;          // the HRTF share's side (delayed with the HRTF, input weight of SideDelayed)
  bool haveD = false;
  int sideD[2] = {0, 0};    // the ears' whole-sample delays of the last HRTF block
  bool histClean = false;   // the slot's side history is valid (else zeroed before its next use)
  uint32_t sideZero = 0;    // SideDelayed's zeroRun
  MetaData meta{};          // the last consistent read
  MetaData prev{};          // the one before (pairs a snapshot written before the newest metadata)
  bool roomLive = false;    // cockpit space: the room's slot holds this voice's tap line (else the next send resets it)
  float roomPos[3] = {0, 0, 0};  // its last send: aircraft-frame position, send gain, room flags
  float roomSend = 0;
  uint32_t roomFlags = 0;
};

struct Binding {
  void* renderer = nullptr;
  uint32_t rate = 0;
  binaural::Renderer* core = nullptr;
  float* scratch = nullptr;  // 2 x kMaxFrames, 64-byte aligned (one allocation with `onset`)
  float* onset = nullptr;    // kMaxFrames: a source's first HRTF block from silence, faded in at the input
  float* side = nullptr;     // kMaxFrames: SideDelayed's u (same allocation)
  float* sideHist = nullptr; // kSlots x kSideHist: each slot's delayed side history (same allocation; null: undelayed)
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
  // cockpit space (render thread, except `room`: created and freed with the binding by the control thread)
  binaural::Room* room = nullptr;
  bool earlyOff = false;       // back-off step 1
  float wet = 0;               // the room's wet weight at the end of the last block
  float canopy = 0;            // the smoothed canopy at the end of the last block
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
  uint32_t wideVoices = 0;  // tagged voices kept wide (Counters::ownedWideNow)
  // cockpit space: the room runs in this block (its BeginBlock called); the block-end targets it got
  bool roomOn = false;
  bool roomFrameValid = false;
  float roomC[4] = {0, 0, 0, 1};  // C_now: aircraft -> head
  float roomWet = 0, roomCanopy = 0;
  bool roomEarly = true;
  binaural::RoomParams roomParams{};
  uint32_t roomVoices = 0;
  double dcompSumDb = 0;
  int64_t roomTicks = 0;    // QPC ticks inside the room's own calls (also in dspTicks)
};

struct MxcsrScope {  // FTZ | DAZ for our DSP, the caller's mode back afterwards
  unsigned int saved;
  MxcsrScope() : saved(_mm_getcsr()) { _mm_setcsr(saved | 0x8040u); }
  ~MxcsrScope() { _mm_setcsr(saved); }
};

// The output rule for one candidate voice. scratch: the voice's own render (2 x frames); snap: its snapshot gains
// (ch0.a, ch0.b, ch1.a, ch1.b, ch2.a, ch2.b) after that render; metaOk/meta: a consistent read of its slot (else rs.meta
// is used); ended: voice_render returned 0. Adds to blk.out.
inline void ProcessVoice(Block& blk, Counters& cnt, int slot, void* voice, float* scratch, const float snap[6],
                         bool metaOk, const MetaData& metaIn, bool ended, std::atomic<uint32_t>* ackGen) {
  Binding& b = *blk.b;
  Rs& rs = b.rs[slot];
  const uint32_t frames = blk.frames;
  float* out = blk.out;
  MetaData meta = metaIn;
  if (!metaOk) {
    if (!rs.haveMeta || rs.voice != voice) {
      // No consistent metadata yet: the stock part only (a tagged voice plays centred for this block).
      if (audv::IsStereoTag(snap)) cnt.orphanTagged.fetch_add(1, std::memory_order_relaxed);
      for (uint32_t i = 0; i < 2 * frames; ++i) out[i] += scratch[i];
      return;
    }
    meta = rs.meta;
  }
  if (rs.voice != voice || rs.gen != meta.gen) {
    if (rs.live) audr::CoreResetSource(b.core, static_cast<uint32_t>(slot));
    if (rs.roomLive) audr::RoomReset(b.room, static_cast<uint32_t>(slot));
    rs = Rs{};
    rs.voice = voice;
    rs.gen = meta.gen;
    // Owned from its creation: its first block is already tagged and has no stock past to fade from (ED starts the
    // ramps at their targets: the snapshot is where they start).
    rs.prevTagged = (meta.flags & audv::meta_flags::kInitial) != 0 && audv::IsStereoTag(snap);
    if (rs.prevTagged) {
      rs.lastTag = snap[0];
      rs.snapL = snap[0], rs.snapR = snap[3], rs.haveSnap = true;
    }
    rs.hW = blk.on && (meta.flags & audv::meta_flags::kOwned) ? 1.0f : 0.0f;
  }
  if (metaOk && (!rs.haveMeta || meta.frame != rs.meta.frame)) {
    rs.prev = rs.haveMeta ? rs.meta : meta;
    rs.meta = meta;
    rs.haveMeta = true;
  }
  const bool tagSnap = audv::IsStereoTag(snap);
  const bool prevTagged = rs.prevTagged;
  const bool owned = (meta.flags & audv::meta_flags::kOwned) != 0;
  const float hTarget = blk.on && owned && b.core ? 1.0f : 0.0f;
  const float sumNow = snap[0] + snap[1] + snap[2] + snap[3];
  const bool stereoFormat = snap[1] == 0.0f && snap[2] == 0.0f;  // ch0 = (x, 0), ch1 = (0, y): DCS's or the tag
  // A torn snapshot (see the top of this file): DCS's stereo gains (or a part of the tag) for an owned, tagged voice
  // whose metadata still says owned. Not twice in a row (then it is taken as what it looks like).
  const bool torn = !tagSnap && prevTagged && owned && !ended && rs.tornS0 == 0.0f && rs.lastTag > 0.0f && stereoFormat &&
                    snap[0] > 0.0f && snap[3] > 0.0f && std::isfinite(sumNow);
  if (torn) cnt.tornSnaps.fetch_add(1, std::memory_order_relaxed);
  const bool tagged = tagSnap || torn;
  // Where this block's ramps started: the last block's snapshot. Unknown only when a voice's first block as a candidate
  // is already tagged (it was not a candidate a block before): DCS's gains of that frame stand in.
  const bool guess = !rs.haveSnap && tagged && !prevTagged;
  const float startL = rs.haveSnap ? rs.snapL : (guess ? rs.meta.gL : snap[0]);
  const float startR = rs.haveSnap ? rs.snapR : (guess ? rs.meta.gR : snap[3]);
  rs.snapL = snap[0], rs.snapR = snap[3], rs.haveSnap = true;

  if (!prevTagged && !tagged) {
    for (uint32_t i = 0; i < 2 * frames; ++i) out[i] += scratch[i];
    cnt.passCandidates.fetch_add(1, std::memory_order_relaxed);
    rs.hW = hTarget;
    rs.tornS0 = 0.0f;
    // DCS's own output. A fade out just before left the delayed side's last samples in its history (up to the HRTF
    // delay, at the fade's last weights): they are played out here, or the fade would end in a step of about delay /
    // frames of the side. Then the history starts from zero again.
    if (rs.haveSide && rs.histClean && rs.sideZero < kSideHist && b.sideHist && b.side) {
      const int d[2] = {rs.sideD[0], rs.sideD[1]};
      SideDelayed(scratch, scratch + 1, frames, 0.0f, 0.0f, d, d, rs.rL, rs.rL, rs.rR, rs.rR, b.side,
                  b.sideHist + static_cast<size_t>(slot) * kSideHist, &rs.sideZero, out);
    }
    rs.haveSide = rs.haveD = rs.histClean = false;
    cnt.view[slot].state.store(0, std::memory_order_relaxed);
    if (!owned || ended) ackGen[slot].store(meta.gen, std::memory_order_release);
    if (ended && rs.live) {
      audr::CoreResetSource(b.core, static_cast<uint32_t>(slot));
      rs.live = false;
    }
    if (rs.roomLive) {  // DCS's own again (or ended): the room's tap line for it starts afresh at its next send
      audr::RoomReset(b.room, static_cast<uint32_t>(slot));
      rs.roomLive = false;
    }
    cnt.view[slot].room.store(0, std::memory_order_relaxed);
    return;
  }

  // The metadata that belongs to this snapshot: its tag value names it (the sim may have written a newer frame).
  const MetaData& pm = tagSnap && rs.prev.mTag == snap[0] && rs.meta.mTag != snap[0] ? rs.prev : rs.meta;
  const float mRef = pm.mTag > 0 ? pm.mTag : 1.0f;
  float rL1 = pm.gL / mRef, rR1 = pm.gR / mRef;
  const float panLevel = PanLevel(rL1, rR1);
  if (!std::isfinite(rL1) || !std::isfinite(rR1)) rL1 = rR1 = 0.5f;
  if (rs.rL == 0 && rs.rR == 0) rs.rL = rL1, rs.rR = rR1;

  // Weights. w: ownership fade (1 steady); h: HRTF share of the tagged part.
  float w0 = 1, w1 = 1, h0 = rs.hW, h1 = rs.hW;
  if (prevTagged && tagged) {
    h1 = hTarget;
    cnt.steady.fetch_add(1, std::memory_order_relaxed);
  } else {
    if (tagged) {  // fade in
      w0 = 0, w1 = 1;
      cnt.fadeIns.fetch_add(1, std::memory_order_relaxed);
    } else {       // fade out
      w0 = 1, w1 = 0;
      cnt.fadeOuts.fetch_add(1, std::memory_order_relaxed);
    }
    AddStockWeighted(scratch, frames, w0, w1, out);
  }
  // m L', m R' back from DCS's known gain ramps (the stock part above is already added): exact in every block whose two
  // snapshots are in DCS's stereo format, the tag included (fades, torn snapshots, a part of the tag).
  const float m0 = rs.lastTag > 0.0f ? rs.lastTag : mRef, m1 = tagSnap ? snap[0] : m0;
  if (stereoFormat) RecoverChannels(scratch, frames, startL, snap[0], startR, snap[3], m0, m1);
  const float* a = scratch;
  const float* bb = scratch + 1;
  const float scale = 0.5f;
  rs.loud.Update(scratch, frames, b.rate);
  // The side's share: a point source drops it, a wide sound keeps DCS's: a source near the head (kNear, under 1.4 m and
  // until past 1.6 m: a cockpit-wide stereo bed is not a point at the head) or a diffuse bed by its own signal. The
  // sdef's auto_width is not a reason: ED's flag only narrows a sound with distance (w < 1 past inner_radius, hook spec
  // 10.1), inside it the sound plays at width 1 like any stereo sound, and the width DCS gives it stays in L', R' (the
  // point source's loudness factor follows it). A change is crossfaded across this block; a voice's first decision
  // starts where it is (no fade of its side from 0).
  const bool wideNow = (pm.flags & audv::meta_flags::kNear) != 0 || rs.loud.diffuse;
  const float q1 = wideNow ? 1.0f : 0.0f, q0 = rs.haveQ ? rs.qW : q1;
  if (wideNow) ++blk.wideVoices;
  // HRTF part: weight w(i) h(i). Only one of the two ramps moves in a block, so the product stays linear. Its gain: the
  // stereo's loudness for a point source (the side is not there), 1 with the side kept; the core ramps it.
  const float hw0 = w0 * h0, hw1 = w1 * h1;
  float rpw0 = w0 * (1.0f - h0), rpw1 = w1 * (1.0f - h1);
  bool hrtfDone = false;
  bool roomSent = false;
  float gT = 1.0f;  // the late-translation gain (kNear): the mid's and the kept side's alike
  float dcomp = 1.0f;  // cockpit space: the direct gain d (section 6.2), the mid's and the kept side's alike
  if (hw0 > 0.0f || hw1 > 0.0f) {
    binaural::SourceUpdate u{};
    CorrectedPosition(pm, blk.pose, blk.late, u.position, &gT);
    u.gain = blk.trim * gT * panLevel * ((1.0f - q1) * rs.loud.comp + q1);
    // Cockpit space: the voice's aircraft-frame position (conj(C_now) applied to its corrected head-frame position),
    // its send (the HRTF gain before d, times the distance law) and d. Diffuse beds feed the tail only; voices heard
    // through the canopy get their excitation on the box.
    float posA[3] = {u.position[0], u.position[1], u.position[2]};
    uint32_t rflags = 0;
    float send = 0.0f;
    if (blk.roomOn) {
      if (blk.roomFrameValid) {
        const float ci[4] = {-blk.roomC[0], -blk.roomC[1], -blk.roomC[2], blk.roomC[3]};
        audspace::QRotate(ci, u.position, posA);
      }
      rflags = (rs.loud.diffuse ? binaural::room_flags::kLateOnly : 0u) |
               ((pm.flags & audv::meta_flags::kThroughCanopy) ? binaural::room_flags::kThroughCanopy : 0u);
      send = u.gain * audr::RoomSendGain(blk.roomParams, posA, rflags);
      dcomp = audr::RoomDirect(blk.roomParams, blk.roomWet, blk.roomCanopy, blk.roomEarly, posA, rflags);
      if (!(dcomp > 0.0f && dcomp <= 1.0f)) dcomp = 1.0f;  // never louder, never a non-finite gain
      if (!std::isfinite(send)) send = 0.0f;
      u.gain *= dcomp;
    }
    // Near field on/off is the core's own (Renderer::SetNearField fades it); no per-source exclusion here.
    u.flags = rs.live ? 0u : binaural::source_flags::kReset;
    binaural::MonoInput in{a, bb, 2, scale};
    float cw0 = hw0, cw1 = hw1;
    if (!rs.live && hw0 == 0.0f && b.onset) {
      // Faded in from silence with a fresh core source (kill switch on, a fade in, off for the session over): the
      // core's delay lines start empty, so a weight on its output would cut the later ear in d samples late at weight
      // d / frames (a step). The fade goes on the input instead: both ears rise from zero, d samples apart.
      const float inv = 1.0f / static_cast<float>(frames);
      for (uint32_t i = 0; i < frames; ++i)
        b.onset[i] = (hw0 + (hw1 - hw0) * static_cast<float>(i) * inv) * scale * (a[2 * i] + bb[2 * i]);
      in = binaural::MonoInput{b.onset, nullptr, 1, 1.0f};
      cw0 = cw1 = 1.0f;
    }
    if (!blk.coreBegun) {
      audr::CoreBegin(b.core, frames);
      blk.coreBegun = true;
    }
    if (audr::CoreRender(b.core, static_cast<uint32_t>(slot), u, in, frames, cw0, cw1, out)) {
      rs.live = true;
      hrtfDone = true;
      cnt.hrtfCalls.fetch_add(1, std::memory_order_relaxed);
      if (pm.flags & audv::meta_flags::kStereo) cnt.stereoCalls.fetch_add(1, std::memory_order_relaxed);
      ++blk.hrtfVoices;
      if (blk.roomOn) {
        // The room gets exactly the core's input and weights: it follows every fade of the direct path. A voice that
        // was not sent in the last block starts a fresh tap line.
        binaural::RoomSource src{};
        memcpy(src.posAircraft, posA, sizeof(posA));
        src.send = send;
        src.flags = rflags | (rs.roomLive ? 0u : binaural::room_flags::kReset);
        if (audr::RoomAdd(b.room, static_cast<uint32_t>(slot), src, in, frames, cw0, cw1)) {
          rs.roomLive = true;
          roomSent = true;
          memcpy(rs.roomPos, posA, sizeof(posA));
          rs.roomSend = send;
          rs.roomFlags = rflags;
          ++blk.roomVoices;
          blk.dcompSumDb += 20.0 * std::log10(static_cast<double>(dcomp));
          cnt.roomSends.fetch_add(1, std::memory_order_relaxed);
          Counters::VoiceView& vv = cnt.view[slot];
          vv.sendDb.store(send > 0.0f ? static_cast<float>(20.0 * std::log10(static_cast<double>(send))) : -999.0f,
                          std::memory_order_relaxed);
          vv.dcompDb.store(static_cast<float>(20.0 * std::log10(static_cast<double>(dcomp))), std::memory_order_relaxed);
          vv.ax.store(posA[0], std::memory_order_relaxed);
          vv.ay.store(posA[1], std::memory_order_relaxed);
          vv.az.store(posA[2], std::memory_order_relaxed);
        } else {
          rs.roomLive = false;
          cnt.roomRefused.fetch_add(1, std::memory_order_relaxed);
        }
      }
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
  if (!roomSent && rs.roomLive) {
    // Not sent in this block (its HRTF share ended, or the room was skipped): the room's tap line plays out what it
    // holds with zero input weight, once, then starts afresh at the voice's next send.
    if (blk.roomOn) {
      binaural::RoomSource src{};
      memcpy(src.posAircraft, rs.roomPos, sizeof(src.posAircraft));
      src.send = rs.roomSend;
      src.flags = rs.roomFlags;
      const binaural::MonoInput in{a, bb, 2, scale};
      audr::RoomAdd(b.room, static_cast<uint32_t>(slot), src, in, frames, 0.0f, 0.0f);
    }
    rs.roomLive = false;
  }
  if (rpw0 > 0.0f || rpw1 > 0.0f) {
    const uint32_t bad = RePan(a, bb, scale, frames, rpw0, rpw1, rs.rL, rL1, rs.rR, rR1, out);
    if (bad) cnt.nonFinite.fetch_add(bad, std::memory_order_relaxed);
    cnt.repanCalls.fetch_add(1, std::memory_order_relaxed);
  }
  // The side (zero for a mono wave), in two parts:
  //  - the re-panned share's, with DCS's own gains, undelayed (so h = 0 is DCS's stereo exactly);
  //  - the HRTF share times q (a wide sound keeps DCS's side around its binaural mid), with the same level trim and
  //    late-translation gain as the mid, delayed per ear by the HRTF's own delay (SideDelayed).
  // Each part ramps from where it ended in the last block, so a core refusal in the middle of a stream (its share moves
  // to the re-pan at once for the mid, which has no HRTF output left to fade from) crossfades the side from the delayed
  // to the undelayed part across the block instead of stepping it at the block's first sample.
  {
    const float rp1 = rpw1;
    const float rp0 = rs.haveSide ? rs.sideRp : rpw0;
    const float gS = blk.trim * gT * dcomp;  // the kept side gets the mid's direct gain (its mid / side balance kept)
    const float v1 = hrtfDone ? hw1 * q1 * gS : 0.0f;
    const float v0 = rs.haveSide ? rs.sideV : (hrtfDone ? hw0 * q0 * gS : 0.0f);
    uint32_t bad = SidePan(a, bb, frames, rp0, rp1, rs.rL, rL1, rs.rR, rR1, out);
    float* hist = b.sideHist && b.side ? b.sideHist + static_cast<size_t>(slot) * kSideHist : nullptr;
    if (hist) {
      if (!rs.histClean) {
        memset(hist, 0, sizeof(float) * kSideHist);
        rs.sideZero = kSideHist;
        rs.histClean = true;
      }
      int d1[2] = {rs.haveD ? rs.sideD[0] : 0, rs.haveD ? rs.sideD[1] : 0};
      float dd[2];
      if (hrtfDone && audr::CoreDelays(b.core, static_cast<uint32_t>(slot), dd)) {
        d1[0] = SideDelaySamples(dd[0]);
        d1[1] = SideDelaySamples(dd[1]);
      }
      const int d0[2] = {rs.haveD ? rs.sideD[0] : d1[0], rs.haveD ? rs.sideD[1] : d1[1]};
      // The voice's last block (faded back to DCS, or it ended): the slot is acknowledged now and never comes back to
      // this function, so the delayed side must be silent by the block's end: its ramp to 0 ends the longest delay
      // early (a 10 ms block: about 1 ms early; nothing is left in the history to be cut).
      uint32_t rampEnd = frames;
      if ((!tagged || ended) && v1 == 0.0f) {
        const int dmax = std::max(std::max(d0[0], d0[1]), std::max(d1[0], d1[1]));
        rampEnd = frames > static_cast<uint32_t>(dmax) ? frames - static_cast<uint32_t>(dmax) : 0u;
      }
      bad += SideDelayed(a, bb, frames, v0, v1, d0, d1, rs.rL, rL1, rs.rR, rR1, b.side, hist, &rs.sideZero, out, rampEnd);
      rs.sideD[0] = d1[0], rs.sideD[1] = d1[1];
      rs.haveD = rs.haveD || hrtfDone;
    } else {
      bad += SidePan(a, bb, frames, v0, v1, rs.rL, rL1, rs.rR, rR1, out);
    }
    if (bad) cnt.nonFinite.fetch_add(bad, std::memory_order_relaxed);
    rs.sideRp = rp1;
    rs.sideV = v1;
    rs.haveSide = true;
  }
  {
    Counters::VoiceView& vv = cnt.view[slot];
    vv.gen.store(meta.gen, std::memory_order_relaxed);
    vv.comp.store(rs.loud.comp, std::memory_order_relaxed);
    vv.panLevel.store(panLevel, std::memory_order_relaxed);
    vv.power.store(static_cast<float>(rs.loud.powM + rs.loud.powS), std::memory_order_relaxed);
    vv.state.store(Counters::kViewTagged | (rs.loud.diffuse ? Counters::kViewDiffuse : 0u) |
                       (wideNow ? Counters::kViewWide : 0u),
                   std::memory_order_relaxed);
    vv.room.store(roomSent ? 1u : 0u, std::memory_order_relaxed);
  }
  rs.qW = q1;
  rs.haveQ = true;
  rs.hW = h1;
  rs.rL = rL1;
  rs.rR = rR1;
  rs.prevTagged = tagged;
  rs.tornS0 = torn ? sumNow : 0.0f;
  if (tagSnap) rs.lastTag = snap[0];
  if (!tagged && !owned) ackGen[slot].store(meta.gen, std::memory_order_release);  // faded back: DCS's again
  if (ended) {
    if (rs.live) audr::CoreResetSource(b.core, static_cast<uint32_t>(slot));
    if (rs.roomLive) audr::RoomReset(b.room, static_cast<uint32_t>(slot));
    rs.roomLive = false;
    rs.live = false;
    rs.prevTagged = false;
    rs.tornS0 = 0.0f;
    rs.haveSide = rs.haveD = rs.histClean = false;
    cnt.view[slot].state.store(0, std::memory_order_relaxed);
    ackGen[slot].store(meta.gen, std::memory_order_release);
    cnt.voiceEnded.fetch_add(1, std::memory_order_relaxed);
  }
}

// ---- cockpit space, once per armed block (render thread) -----------------------------------------------------------
// Before the mix: the block-end targets (wet: inside x level on x binaural on, ramped linearly over kWetRampS; canopy:
// smoothed over kCanopyTauS; C_now: C_dcs with the late head correction since the pose DCS used, else C_dcs as is while
// it is younger than 1 s) and the room's BeginBlock. The room is skipped entirely (no call at all) while its wet weight
// stays 0 and it is idle: CockpitSpace=0 and every external view cost nothing and leave the output exactly as without it.
inline void RoomBlockBegin(Block& blk, const audspace::SpaceData* sd, const SpaceCtl& ctl, int64_t now, int64_t freq) {
  Binding& b = *blk.b;
  blk.roomOn = false;
  if (!kHaveRoom || !b.room || !b.rate) return;
  const bool reads = sd && (sd->flags & audspace::flags::kReads);
  const bool inside = reads && (sd->flags & audspace::flags::kInsideKnown) ? (sd->flags & audspace::flags::kInside) != 0
                      : ctl.luaInside >= 0                                  ? ctl.luaInside != 0
                                                                            : true;  // unknown: the cockpit
  const float target = ctl.level > 0 && ctl.haveParams && blk.on && inside ? 1.0f : 0.0f;
  const float step = static_cast<float>(static_cast<double>(blk.frames) / (kWetRampS * b.rate));
  float wet = b.wet + (target > b.wet ? step : -step);
  if ((target > b.wet && wet > target) || (target < b.wet && wet < target) || target == b.wet) wet = target;
  if (wet == 0.0f && b.wet == 0.0f && audr::RoomIdle(b.room)) {
    b.canopy = ctl.canopy > 0.0f ? ctl.canopy : 0.0f;  // the room starts at the canopy's position
    return;
  }
  const float cTarget = ctl.canopy > 0.0f ? (ctl.canopy > 1.0f ? 1.0f : ctl.canopy) : 0.0f;
  const double kc = 1.0 - std::exp(-static_cast<double>(blk.frames) / (kCanopyTauS * b.rate));
  float canopy = b.canopy + static_cast<float>(kc) * (cTarget - b.canopy);
  if (std::fabs(canopy - cTarget) < 1e-4f) canopy = cTarget;
  // The aircraft frame.
  bool valid = false;
  float c[4] = {0, 0, 0, 1};
  if (sd && (sd->flags & audspace::flags::kFrameValid)) {
    const double ageS = freq ? static_cast<double>(now - sd->frameQpc) / static_cast<double>(freq) : 1e9;
    namespace hp = dcsvr_headpose;
    hp::Quat cq = hp::Normalize({sd->c[0], sd->c[1], sd->c[2], sd->c[3]});
    bool late = false;
    if (blk.late && blk.pose.valid && (sd->flags & audspace::flags::kPoseValid) && sd->poseBase == blk.pose.base) {
      const hp::Quat qu = hp::Normalize({sd->qUsed[0], sd->qUsed[1], sd->qUsed[2], sd->qUsed[3]});
      const hp::LateCorrection lc = hp::ComputeLateCorrection(qu, {0, 0, 0}, blk.pose.q, {0, 0, 0});
      const double w = std::fabs(lc.rotation.w) > 1.0 ? 1.0 : std::fabs(lc.rotation.w);
      if (2.0 * std::acos(w) <= kMaxLateRad) {
        cq = hp::Normalize(hp::Mul(lc.rotation, cq));
        late = true;
      }
    }
    valid = late || ageS < audspace::kFrameMaxAgeS;
    c[0] = static_cast<float>(cq.x), c[1] = static_cast<float>(cq.y), c[2] = static_cast<float>(cq.z), c[3] = static_cast<float>(cq.w);
    if (!(std::isfinite(c[0]) && std::isfinite(c[1]) && std::isfinite(c[2]) && std::isfinite(c[3]))) valid = false;
  }
  binaural::RoomFrame f{};
  memcpy(f.aircraftToHead, valid ? c : blk.roomC, sizeof(f.aircraftToHead));
  f.frameValid = valid;
  f.wet = wet;
  f.canopyOpen = canopy;
  f.early = !b.earlyOff;
  audr::RoomBegin(b.room, blk.frames, f);
  blk.roomOn = true;
  blk.roomFrameValid = valid;
  memcpy(blk.roomC, f.aircraftToHead, sizeof(blk.roomC));
  blk.roomWet = wet;
  blk.roomCanopy = canopy;
  blk.roomEarly = f.early;
  blk.roomParams = ctl.params;
  b.wet = wet;
  b.canopy = canopy;
}

// After the mix: the room's buses and tail added to the block (inside the caller's MxcsrScope), its time recorded.
inline void RoomBlockEnd(Block& blk, Counters& cnt, int64_t roomTicks, int64_t freq) {
  Binding& b = *blk.b;
  cnt.roomVoicesNow.store(blk.roomOn ? blk.roomVoices : 0u, std::memory_order_relaxed);
  cnt.roomWet.store(b.wet, std::memory_order_relaxed);
  cnt.roomCanopy.store(b.canopy, std::memory_order_relaxed);
  if (!blk.roomOn) return;
  cnt.roomBlocks.fetch_add(1, std::memory_order_relaxed);
  cnt.roomFrameValid.store(blk.roomFrameValid ? 1u : 0u, std::memory_order_relaxed);
  cnt.roomEarly.store(blk.roomEarly ? 1u : 0u, std::memory_order_relaxed);
  for (int k = 0; k < 4; ++k) cnt.roomC[k].store(blk.roomC[k], std::memory_order_relaxed);
  if (blk.roomVoices) cnt.dcompMeanDb.store(static_cast<float>(blk.dcompSumDb / blk.roomVoices), std::memory_order_relaxed);
  if (freq) {
    const uint32_t tenth = static_cast<uint32_t>(std::fmin(static_cast<double>(roomTicks) / static_cast<double>(freq) * 1e7, 4e9));
    const uint32_t h = cnt.roomHead.load(std::memory_order_relaxed);
    cnt.roomRing[h & 1023].store(tenth, std::memory_order_relaxed);
    cnt.roomHead.store(h + 1, std::memory_order_release);
  }
}

// ---- CPU budget and back-off (binaural_core.h) -------------------------------------------------------------------
struct BudgetCfg {
  int cpuBudgetPercent = 25;
  int maxObjects = 48;
};
// Called once per armed block after the mix. Returns the new limit in use when the back-off changed (else 0).
// roomEarly: the cockpit space's early reflections ran in this block (back-off step 1 is theirs).
inline uint32_t BudgetTick(Binding& b, Counters& cnt, EvRing& ev, const BudgetCfg& cfg, uint32_t frames, int64_t ticks,
                           int64_t now, int64_t freq, bool roomEarly = false) {
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
      if (roomEarly && !b.earlyOff) {
        // Cockpit space, step 1: the early reflections go first (the late tail only), before any sound is dropped.
        b.earlyOff = true;
        ev.Push({kEvEarlyOff, 0, b.secOver * 1000 / b.secBlocks, 0});
      } else {
        const uint32_t nl = limit > 16 ? limit - 8 : 8;
        if (nl != limit) {
          ev.Push({kEvBudgetDown, static_cast<uint16_t>(nl), b.secOver * 1000 / b.secBlocks, 0});
          newCap = nl;
        }
      }
      b.calmSeconds = 0;
    } else if (b.secHalf == 0) {  // a whole second under half the budget
      b.calmSeconds += secS;
      if (b.calmSeconds >= 10.0 && cap < binaural::kMaxSources) {
        const uint32_t nl = limit + 4;
        newCap = nl >= cfgMax ? binaural::kMaxSources : nl;
        ev.Push({kEvBudgetUp, static_cast<uint16_t>(nl >= cfgMax ? cfgMax : nl), 0, 0});
        b.calmSeconds = 0;
      } else if (b.calmSeconds >= 10.0 && b.earlyOff) {  // the sounds are back: then the early reflections
        b.earlyOff = false;
        ev.Push({kEvEarlyOn, 0, 0, 0});
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
