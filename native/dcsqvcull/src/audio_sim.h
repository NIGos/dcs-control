// Binaural audio, simulation side: what PanWrap does after DCS's own panner ran (docs/BINAURAL_AUDIO_HOOKS.md 5.1).
// Pure with respect to ed_sound: it reads the voice / sound / renderer / context memory it is given (the real ones in
// DCS, fakes in the tests) and writes only the voice's pending gains and our tables. Sim thread, ed mutex held: no
// allocation, no locks, no logging.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "audio_render.h"
#include "audio_space.h"
#include "audio_voice.h"

namespace audsim {

using namespace audv;

struct Cfg {
  bool on = false;          // ownership wanted: module on, kill switch off, not off for the session, core bound
  int limit = 48;           // MaxObjects, after the budget back-off
  uint32_t excludeMask = 0; // [Audio] ExcludeContexts
};

// What PanWrap reads from ed_sound once per frame (the glue fills it from the resolved globals).
struct Env {
  const void* boundRenderer = nullptr;
  const uint8_t* ctxArray = nullptr;
  int ctxCount = 0;
  int32_t engineState = 3;
  int64_t now = 0, freq = 1;
  bool poseValid = false;
  bool posValid = false;    // t is a tracked position (the pose entry had one)
  float q[4] = {0, 0, 0, 1}, t[3] = {0, 0, 0};
  uint64_t poseBase = 0;
};

struct Sim {
  Owner own;
  MetaTable* meta = nullptr;
  audr::Counters* cnt = nullptr;
  audr::EvRing* ev = nullptr;
  audspace::SpaceSlot* spaceOut = nullptr;  // cockpit space: published once per frame (null: not published)
  audspace::SimSpace space;
  MetaData last[kSlots] = {};
  uint32_t prevFrame = 0;
  bool havePrev = false;
  // pan-law self-check window (10 s)
  int64_t winStart = 0;
  uint64_t winSolves = 0, winMismatch = 0;
  bool panLawTripped = false;
  uint64_t forcedFrees = 0;
  // ranking scratch (no allocation)
  Rec* cand[Owner::kRecCap];
  Rec* ownedSeen[Owner::kRecCap];
  // The renderers the world's 3D sounds play on, counted per panner call (sim thread adds, the control thread reads and
  // clears): with a second DCS output (options.lua sound.hp_output) the module binds the one these sounds use, never
  // the headphones / radio output. Entries are claimed lock-free; a lost count between a read and a clear is harmless.
  struct RendCount {
    std::atomic<const void*> rend{nullptr};
    std::atomic<uint32_t> n{0};
  };
  static constexpr int kRendCounts = 4;
  RendCount world[kRendCounts];
  void CountWorld(const void* rnd) {
    if (!rnd) return;
    for (RendCount& c : world)
      if (c.rend.load(std::memory_order_relaxed) == rnd) {
        c.n.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    for (RendCount& c : world) {
      const void* expect = nullptr;
      if (c.rend.compare_exchange_strong(expect, rnd, std::memory_order_relaxed) || expect == rnd) {
        c.n.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }

  void Reset() {
    own.Reset();
    space.Reset();
    memset(last, 0, sizeof(last));
    havePrev = false;
    winStart = 0;
    winSolves = winMismatch = 0;
    panLawTripped = false;
    for (RendCount& c : world) {
      c.rend.store(nullptr, std::memory_order_relaxed);
      c.n.store(0, std::memory_order_relaxed);
    }
    if (meta)
      for (int s = 0; s < kSlots; ++s) {
        meta->voice[s].store(nullptr, std::memory_order_release);
        meta->ackGen[s].store(0, std::memory_order_relaxed);
      }
  }
};

inline void FreeSlot(Sim& s, int sl) {
  s.meta->voice[sl].store(nullptr, std::memory_order_release);
  s.own.slotState[sl] = kSlotFree;
  s.own.slotVoice[sl] = nullptr;
}

inline bool Own(Sim& s, Rec& r, const Env& e, bool initial) {
  const int sl = s.own.FreeSlot();
  if (sl < 0) {
    ++s.own.slotsExhausted;
    return false;
  }
  const uint32_t g = s.own.NewGen();
  s.own.slotState[sl] = kSlotOwned;
  s.own.slotGen[sl] = g;
  s.own.slotVoice[sl] = r.voice;
  r.owned = true;
  r.slot = static_cast<int16_t>(sl);
  r.ownedSince = e.now;
  r.initialOwned = initial;
  ++s.own.owned;
  MetaData d{};
  d.voice = r.voice;
  d.gen = g;
  d.flags = meta_flags::kOwned | (initial ? meta_flags::kInitial : 0u);
  d.frame = s.own.frame;
  d.ctx = r.ctx;
  s.last[sl] = d;
  MetaWrite(s.meta->slot[sl], d);
  s.meta->ackGen[sl].store(0, std::memory_order_relaxed);
  s.meta->voice[sl].store(r.voice, std::memory_order_release);
  return true;
}

// The owned set's make-up for the "audio: running" line (stereo waves). Which voices are kept wide is the render
// side's decision (distance and its own diffuse detector): Counters::ownedWideNow.
inline void StoreOwnedCounts(Sim& s) {
  uint32_t st = 0;
  const Rec* t = s.own.rec[s.own.cur];
  for (uint32_t i = 0; i < Owner::kRecCap; ++i)
    if (t[i].voice && t[i].owned && t[i].chans == 2) ++st;
  s.cnt->ownedNow.store(static_cast<uint32_t>(s.own.owned), std::memory_order_relaxed);
  s.cnt->ownedStereoNow.store(st, std::memory_order_relaxed);
}

// The sim stops tagging the voice; its slot waits for the render side's acknowledgement.
inline void Release(Sim& s, Rec& r, int64_t now) {
  if (!r.owned) return;
  r.owned = false;
  --s.own.owned;
  const int sl = r.slot;
  if (sl < 0) return;
  s.own.slotState[sl] = kSlotReleasing;
  s.own.slotReleasedAt[sl] = now;
  MetaData d = s.last[sl];
  d.flags &= ~meta_flags::kOwned;
  s.last[sl] = d;
  MetaWrite(s.meta->slot[sl], d);
}

inline void ReleaseAll(Sim& s, int64_t now) {
  Rec* t = s.own.rec[s.own.cur];
  for (uint32_t i = 0; i < Owner::kRecCap; ++i)
    if (t[i].voice && t[i].owned) Release(s, t[i], now);
}

// Start of a new SND_process frame (the first PanWrap that sees a new frame counter).
//   epochReset: the render binding changed: every slot is dropped at once (the old render state is gone).
inline void BeginFrame(Sim& s, const Cfg& cfg, const Env& e, uint32_t frame, bool epochReset) {
  Owner& o = s.own;
  if (s.spaceOut) {  // cockpit space: the finished frame's inside flag, cockpit vote and aircraft frame
    audspace::SpaceData sd;
    audspace::BeginFrame(s.space, e.ctxArray, e.ctxCount, frame, e.now, e.freq, e.poseValid, e.q, e.poseBase, &sd);
    audspace::SpaceWrite(*s.spaceOut, sd);
  }
  if (o.haveFrame) {
    s.prevFrame = o.frame;
    s.havePrev = true;
  }
  o.frame = frame;
  o.haveFrame = true;
  s.cnt->simFrames.fetch_add(1, std::memory_order_relaxed);
  // Pan-law window.
  if (!s.winStart) s.winStart = e.now;
  if (e.now - s.winStart >= 10 * e.freq) {
    if (s.winSolves >= 50 && s.winMismatch * 100 > s.winSolves && !s.panLawTripped) {
      s.panLawTripped = true;
      s.ev->Push({audr::kEvPanLawOff, static_cast<uint16_t>(std::min<uint64_t>(s.winMismatch, 65535)),
                  static_cast<uint32_t>(std::min<uint64_t>(s.winSolves, 0xffffffffu)), 0});
    }
    s.winStart = e.now;
    s.winSolves = s.winMismatch = 0;
  }
  if (epochReset) {
    Rec* t = o.rec[o.cur];
    for (uint32_t i = 0; i < Owner::kRecCap; ++i)
      if (t[i].voice) {
        t[i].owned = false;
        t[i].slot = -1;
      }
    o.owned = 0;
    for (int sl = 0; sl < kSlots; ++sl)
      if (o.slotState[sl] != kSlotFree) FreeSlot(s, sl);
  }
  // Slots the render side is done with (released and faded back, or the voice ended).
  for (int sl = 0; sl < kSlots; ++sl) {
    if (o.slotState[sl] == kSlotFree) continue;
    const bool acked = s.meta->ackGen[sl].load(std::memory_order_acquire) == o.slotGen[sl];
    const bool stale = o.slotState[sl] == kSlotReleasing && e.now - o.slotReleasedAt[sl] > e.freq;
    if (!acked && !stale) continue;
    if (stale && !acked) ++s.forcedFrees;
    Rec* r = o.Find(o.slotVoice[sl]);
    if (r && r->slot == sl) {
      if (r->owned) {
        r->owned = false;
        --o.owned;
      }
      r->slot = -1;
    }
    FreeSlot(s, sl);
  }
  // Rebuild the record table: drop records unseen for kKeepFrames frames that hold no slot.
  {
    Rec* from = o.rec[o.cur];
    Rec* to = o.rec[o.cur ^ 1];
    memset(to, 0, sizeof(Rec) * Owner::kRecCap);
    uint32_t used = 0;
    for (uint32_t i = 0; i < Owner::kRecCap; ++i) {
      const Rec& r = from[i];
      if (!r.voice) continue;
      const bool seen = frame - r.lastFrame < Owner::kKeepFrames;
      if (!seen && r.slot < 0) continue;
      if (!seen && r.owned && r.slot >= 0) {
        // Owned but no longer panned (its sound is gone): the voice keeps its last tag until it ends; its slot is
        // acknowledged then. Never seen again for 2 s: dropped (counted).
        if (e.now - r.ownedSince > 2 * e.freq && frame - r.lastFrame > 4 * Owner::kKeepFrames) {
          o.slotState[r.slot] = kSlotReleasing;
          o.slotReleasedAt[r.slot] = e.now - 2 * e.freq;  // freed at the next frame
          --o.owned;
          continue;
        }
      }
      for (uint32_t j = Owner::Hash(r.voice), n = 0; n < Owner::kRecCap; ++n, j = (j + 1) & (Owner::kRecCap - 1))
        if (!to[j].voice) {
          to[j] = r;
          ++used;
          break;
        }
    }
    o.cur ^= 1;
    o.used = used;
  }
  // Decide the owned set.
  Rec* t = o.rec[o.cur];
  if (!cfg.on) {
    ReleaseAll(s, e.now);
    StoreOwnedCounts(s);
    return;
  }
  const uint32_t prev = s.prevFrame;
  int nc = 0, no = 0;
  for (uint32_t i = 0; i < Owner::kRecCap; ++i) {
    Rec& r = t[i];
    if (!r.voice || !s.havePrev || r.lastFrame != prev) continue;
    if (r.owned) {
      if (!r.qualifies) Release(s, r, e.now);
      else s.ownedSeen[no++] = &r;
    } else if (r.qualifies && r.slot < 0) {
      s.cand[nc++] = &r;
    }
  }
  const int limit = std::max(0, std::min(cfg.limit, kSlots));
  std::sort(s.ownedSeen, s.ownedSeen + no, [](const Rec* a, const Rec* b) { return a->loud < b->loud; });
  std::sort(s.cand, s.cand + nc, [](const Rec* a, const Rec* b) { return a->loud > b->loud; });
  int oi = 0;
  while (o.owned > limit && oi < no) Release(s, *s.ownedSeen[oi++], e.now);  // the limit fell: quietest first
  int ci = 0;
  while (ci < nc && o.owned < limit && o.FreeSlot() >= 0) Own(s, *s.cand[ci++], e, false);
  // Swaps: a waiting voice 3 dB louder than the quietest owned one held at least 250 ms.
  const int64_t hold = static_cast<int64_t>(kHoldSeconds * static_cast<double>(e.freq));
  while (ci < nc && oi < no) {
    Rec& q = *s.ownedSeen[oi];
    if (!q.owned || e.now - q.ownedSince < hold) {
      ++oi;
      continue;
    }
    Rec& c = *s.cand[ci];
    if (!(c.loud >= q.loud * kReplaceRatio) || o.FreeSlot() < 0) break;
    Release(s, q, e.now);
    Own(s, c, e, false);
    ++o.swaps;
    ++ci;
    ++oi;
  }
  StoreOwnedCounts(s);
}

// The context id of a sound (8: not one of the context array's entries).
inline uint32_t CtxId(const Env& e, const void* ctx) {
  const uint8_t* p = static_cast<const uint8_t*>(ctx);
  if (!e.ctxArray || p < e.ctxArray) return 8;
  const uint64_t d = static_cast<uint64_t>(p - e.ctxArray);
  if (d % off::kCtxStride) return 8;
  const uint64_t i = d / off::kCtxStride;
  return i < static_cast<uint64_t>(std::min(e.ctxCount, off::kMaxContexts)) ? static_cast<uint32_t>(i) : 8;
}

// After DCS's own panner ran for `voice`. initial: the call from voice creation (P3b).
inline void PanVoice(Sim& s, const Cfg& cfg, const Env& e, void* voice, bool initial) {
  Owner& o = s.own;
  s.cnt->panCalls.fetch_add(1, std::memory_order_relaxed);
  void* sound = Rd<void*>(voice, off::kVSound);
  Rec* r = o.Find(voice);
  if (r && (initial || r->sound != sound)) {
    // A pooled voice now playing another sound: whatever it was is over.
    if (r->slot >= 0) {
      if (r->owned) --o.owned;
      FreeSlot(s, r->slot);
    }
    void* v = r->voice;
    memset(r, 0, sizeof(Rec));
    r->voice = v;
    r->slot = -1;
    r->k = static_cast<float>(kInitialK);
  }
  if (!r) r = o.Insert(voice);
  if (!r) return;
  r->sound = sound;
  r->lastFrame = o.frame;
  // Which output the world's 3D sounds use (counted whether binaural is on or not: the binding is chosen by it).
  if (sound && Rd<uint64_t>(sound, off::kSndIs3D) != 0 && Rd<uint8_t>(voice, off::kVCtx2d) == 0)
    s.CountWorld(Rd<void*>(voice, off::kVRenderer));
  // Qualification (hook spec 5.1). A mono wave or a stereo one (wave channels at voice+0xec4; ed_sound's panner gives
  // both the same gains, its stereo kernels apply a to the left channel and b to the right).
  const uint32_t chans = Rd<uint32_t>(voice, off::kVChannels);
  const bool is3d = sound && Rd<uint64_t>(sound, off::kSndIs3D) != 0 && Rd<uint8_t>(voice, off::kVCtx2d) == 0;
  // Cockpit space: every 3D sound of a 3D context votes for the cockpit context and carries the aircraft frame.
  if (is3d && (s.space.gainReads || s.space.orientReads)) {
    const void* sctx = Rd<void*>(sound, off::kSndCtx);
    const uint32_t cid = CtxId(e, sctx);
    if (cid < 8 && Rd<uint8_t>(sctx, off::kCtxIs2D) == 0)
      audspace::Accumulate(s.space, cid, sound, Rd<double>(sound, off::kSndDist));
  }
  bool q = cfg.on && e.boundRenderer && is3d && Rd<void*>(voice, off::kVRenderer) == e.boundRenderer &&
           e.engineState != off::kStateOffline && (chans == 1 || chans == 2);
  // Why not (the "audio: idle" line): counted only, the decision above is unchanged. A 2D sound (or a voice of a 2D
  // context) is counted as "not 3D" whatever output it plays on: the radio and the second output's sounds are 2D.
  int refuse = -1;
  if (!q)
    refuse = !cfg.on                                                          ? audr::kRefuseOff
             : !is3d                                                          ? audr::kRefuseNot3D
             : !e.boundRenderer || Rd<void*>(voice, off::kVRenderer) != e.boundRenderer ? audr::kRefuseOutput
             : e.engineState == off::kStateOffline                            ? audr::kRefuseOffline
             : chans == 0                                                     ? audr::kRefuseNoWave
                                                                              : audr::kRefuseChannels;
  uint32_t ctxId = 8;
  if (q) {
    const void* ctx = Rd<void*>(sound, off::kSndCtx);
    ctxId = CtxId(e, ctx);
    q = ctxId < 8 && !(cfg.excludeMask & (1u << ctxId)) && Rd<uint8_t>(ctx, off::kCtxIs2D) == 0;
    if (!q) refuse = audr::kRefuseContext;
  }
  const double dist = q ? Rd<double>(sound, off::kSndDist) : 0.0;
  if (q) {
    q = dist >= kMinDistanceM && std::isfinite(dist);
    if (!q) refuse = audr::kRefuseDistance;
  }
  const void* rend = e.boundRenderer;
  if (q) {
    q = Rd<uint32_t>(rend, off::kRChannels) == 2;
    if (!q) refuse = audr::kRefuseStereoOut;
  }
  float g[4] = {};
  if (q) {
    memcpy(g, static_cast<const uint8_t*>(voice) + off::kVPendGain, sizeof(g));
    q = g[1] == 0.0f && g[2] == 0.0f;  // DCS's stereo format: ch0 = (gL, 0), ch1 = (0, gR)
    if (!q) refuse = audr::kRefuseGains;
  }
  float m = 0;
  if (q) {
    double u[3], sp[2][3];
    memcpy(u, static_cast<const uint8_t*>(sound) + off::kSndToListener, sizeof(u));
    for (int ch = 0; ch < 2; ++ch)
      memcpy(sp[ch], static_cast<const uint8_t*>(rend) + off::kRSpk + off::kRSpkStride * ch + off::kSpkDir, sizeof(sp[ch]));
    const double cL = -(u[0] * sp[0][0] + u[1] * sp[0][1] + u[2] * sp[0][2]);
    const double cR = -(u[0] * sp[1][0] + u[1] * sp[1][1] + u[2] * sp[1][2]);
    double rv[3];
    memcpy(rv, static_cast<const uint8_t*>(sound) + off::kSndRelVel, sizeof(rv));
    const float base = SpeedFactor(Rd<float>(sound, off::kSndSpeed), rv) * Rd<float>(sound, off::kSndGain);
    const MonoSolve ms = SolveMono(g[0], g[3], cL, cR, base, r->k);
    s.cnt->panSolves.fetch_add(1, std::memory_order_relaxed);
    ++s.winSolves;
    if (ms.median) s.cnt->panMedian.fetch_add(1, std::memory_order_relaxed);
    if (!ms.ok) {
      s.cnt->panMismatch.fetch_add(1, std::memory_order_relaxed);
      ++s.winMismatch;
      q = false;
      refuse = audr::kRefusePanMismatch;
    } else {
      m = ms.m;
      if (!ms.median && ms.wa > 1e-12 * std::fmax(std::fabs(g[0]), 1e-30)) r->k = static_cast<float>(ms.wb / ms.wa);
    }
  }
  r->qualifies = q;
  r->ctx = static_cast<uint8_t>(ctxId);
  if (q) {
    r->loud = m;
    r->chans = static_cast<uint8_t>(chans);
    r->wide = chans == 2 && Rd<uint8_t>(sound, off::kSndAutoWidth) != 0;
    // Near with hysteresis (audr::kNearInM / kNearOutM), kept per voice; its first frame decides at kNearM.
    const bool wasNear = r->nearState == 2, known = r->nearState != 0;
    const bool isNear = known ? dist < (wasNear ? audr::kNearOutM : audr::kNearInM) : dist < audr::kNearM;
    r->nearState = isNear ? 2 : 1;
  }
  if (!q) {
    if (refuse >= 0) s.cnt->refused[refuse].fetch_add(1, std::memory_order_relaxed);
    if (r->owned) Release(s, *r, e.now);
    ClearStereoMark(voice);
    return;
  }
  if (!r->owned && r->slot < 0 && initial && o.owned < std::min(cfg.limit, kSlots)) Own(s, *r, e, true);
  if (!r->owned) {
    // Not taken: a voice is taken only when it starts (or by the ranking, BeginFrame), and only under the limit.
    s.cnt->refused[initial ? audr::kRefuseLimit : audr::kRefuseNotCreation].fetch_add(1, std::memory_order_relaxed);
    ClearStereoMark(voice);
    return;
  }
  // The tag and the voice's metadata: ch0 = (m, 0), ch1 = (0, m), ch2 = mark for mono and stereo waves alike, so the
  // render side gets m L' and m R' (m s on both for a mono wave; DCS's own width, filters, doppler and fades kept), and a
  // stream switching its channel count in the middle of a render needs nothing.
  const float mt = m > kTagFloor ? m : kTagFloor;
  const bool stereo = chans == 2;
  float tag[6];
  MakeTag(mt, tag);
  memcpy(static_cast<uint8_t*>(voice) + off::kVPendGain, tag, off::kTagBytes);
  float cockpitGain = 1.0f;
  const bool through = audspace::ThroughCanopy(s.space, Rd<void*>(sound, off::kSndCtx), sound, &cockpitGain);
  MetaData d = s.last[r->slot];
  d.voice = voice;
  d.gen = o.slotGen[r->slot];
  d.flags = meta_flags::kOwned | (r->initialOwned ? meta_flags::kInitial : 0u) |
            (e.poseValid ? meta_flags::kPoseValid : 0u) | (e.poseValid && e.posValid ? meta_flags::kPosValid : 0u) |
            (r->nearState == 2 ? meta_flags::kNear : 0u) | (stereo ? meta_flags::kStereo : 0u) |
            (stereo && r->wide ? meta_flags::kWide : 0u) | (through ? meta_flags::kThroughCanopy : 0u) |
            (ctxId == s.space.cockpitCtx ? meta_flags::kCockpitCtx : 0u);
  d.cockpitGain = cockpitGain;
  d.frame = o.frame;
  d.ctx = ctxId;
  double p[3];
  memcpy(p, static_cast<const uint8_t*>(sound) + off::kSndRelPos, sizeof(p));
  for (int k = 0; k < 3; ++k) d.pos[k] = static_cast<float>(p[k]);
  d.dist = static_cast<float>(dist);
  d.mTag = mt;
  d.gL = g[0];
  d.gR = g[3];
  memcpy(d.qUsed, e.q, sizeof(d.qUsed));
  memcpy(d.tUsed, e.t, sizeof(d.tUsed));
  d.poseBase = e.poseBase;
  s.last[r->slot] = d;
  MetaWrite(s.meta->slot[r->slot], d);
  s.cnt->ownedVoiceFrames.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace audsim
