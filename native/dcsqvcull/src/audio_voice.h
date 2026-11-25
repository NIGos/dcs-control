// Binaural audio, simulation side: ed_sound's structures, the mono gain rule, voice ownership and the VoiceMeta table
// (docs/BINAURAL_AUDIO_HOOKS.md sections 3-5.1, docs/BINAURAL_AUDIO.md).
//
// Runs inside PanWrap, i.e. on DCS's simulation thread inside SND_process with ed_sound's own mutex held: no
// allocation, no locks of ours, no logging (events go to the ring in audio_render.h), bounded work per voice. The
// per-frame ranking is O(records) once per SND_process.
//
// Ownership (all must hold): module on, renderer bound (the WASAPI renderer the render side validated), offline state
// off, a 3D sound (+0x18) in a context that is not 2D and not excluded, a mono or stereo wave, DCS's stereo gain format
// (ch0 = (gL, 0), ch1 = (0, gR)), distance >= 5 cm, and a rank within the top MaxObjects by loudness (hysteresis: an
// owned voice is kept at least 250 ms; a waiting voice replaces the quietest owned one only when it is 3 dB louder).
// A voice created this frame (P3b) is owned at once when a slot is free, so its onset is already binaural.
//
// For an owned voice the pending gains are replaced by the tag ch0 = (m, 0), ch1 = (0, m), ch2 = mark (m = DCS's own
// gain for that voice with the source straight ahead, solved from DCS's two gains; at least kTagFloor so a tag is never
// all zero): m L' and m R' apart for a stereo wave, m s on both channels for a mono one (IsStereoTag below; the mark in
// channel 2 tells it from DCS's frontal gains). The voice's metadata (listener-frame position, distance, m, DCS's own gains for
// the fallback re-pan, the head pose DCS used) goes to its VoiceMeta slot (one seqlock per slot, single writer).
//
// Slots are released through the render side: the sim stops tagging, the render side fades the voice back to DCS's
// panning (or sees it end) and acknowledges the slot's generation; only then the sim reuses the slot.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace audv {

// ---- ed_sound 2.9.31 layout (docs/BINAURAL_AUDIO_HOOKS.md section 4) --------------------------------------------------
namespace off {
// Sound instance
constexpr uint32_t kSndCtx = 0x000;      // Context*
constexpr uint32_t kSndRelVel = 0x108;   // double3, velocity relative to the listener
constexpr uint32_t kSndSpeed = 0x178;    // float, speed factor at rest (1.0 = none)
constexpr uint32_t kSndGain = 0x1c0;     // float, gain after distance attenuation and cones
constexpr uint32_t kSndIs3D = 0x018;     // non-zero = 3D
constexpr uint32_t kSndRelPos = 0x0d0;   // double3, listener frame (x forward, y up, z right), metres
constexpr uint32_t kSndDist = 0x1c8;     // double
constexpr uint32_t kSndToListener = 0x1d8;  // double3 unit vector source -> listener
constexpr uint32_t kSndAutoWidth = 0x1a6;   // u8, the sdef's auto_width (panner 0x1c5ee: a stereo wave's width
                                            // narrows from 1 at inner_radius to 0 at inner_radius + 1200 m)
constexpr uint32_t kSndVoice = 0x1f0;    // Voice*
// Render voice
constexpr uint32_t kVNext = 0x000;
constexpr uint32_t kVRenderer = 0x008;
constexpr uint32_t kVSound = 0x010;
constexpr uint32_t kVPendGain = 0x048;   // float[2] per output channel (stride 8)
constexpr uint32_t kVSnapGain = 0x0f8;   // float[2] per output channel: the targets the last render ramped to
constexpr uint32_t kVCtx2d = 0xec2;      // u8
constexpr uint32_t kVChannels = 0xec4;   // u32 wave channels (1 = mono, 2 = stereo; set from the wave at creation,
                                         // switched by a stream's chunk, 0x1614d)
constexpr uint32_t kTagBytes = 24;       // the gains of channels 0, 1 and 2 (the stereo tag's mark is in channel 2,
                                         // a slot DCS neither writes nor reads on a stereo output)
constexpr uint32_t kVoiceSize = 0x1f10;
// Renderer
constexpr uint32_t kRRate = 0x0000;      // double
constexpr uint32_t kRVoices = 0x0020;    // Voice* list head
constexpr uint32_t kRChannels = 0x2274;  // u32
constexpr uint32_t kRSpk = 0x2278;       // speaker records, stride 0x30
constexpr uint32_t kRSpkStride = 0x30;
constexpr uint32_t kSpkDir = 0x00;       // double3
constexpr uint32_t kSpkW3dA = 0x20;      // float: 3D weight of the wave's left channel
constexpr uint32_t kSpkW3dB = 0x24;      // float: of its right channel
constexpr uint32_t kSpkLfe = 0x28;       // u8
// SND_Wasapi: the renderer is at +8; ED's "render time exceeded allowed slice" counter at +0x2274 (lock inc, 0x201c1)
constexpr int32_t kROverrun = 0x2274 - 8;  // relative to the renderer
// Context (array stride 0x388)
constexpr uint32_t kCtxStride = 0x388;
constexpr uint32_t kCtxQuat = 0x20;      // double4 (x, y, z, w) listener -> world
constexpr uint32_t kCtxTime = 0x68;      // double
constexpr uint32_t kCtxIs2D = 0xe0;      // u8
constexpr uint32_t kCtxCockpitGain = 0xe1;  // u8
constexpr uint32_t kCtxOutput = 0xe4;    // i32
constexpr uint32_t kCtxCtThreshold = 0xf8;  // float
constexpr uint32_t kCtxCtExponent = 0xfc;   // float
constexpr int kMaxContexts = 8;
constexpr int32_t kStateOffline = 2;     // engine state (0x3ad70): 2 = offline renderer (AVIMaker)
}  // namespace off

template <typename T>
inline T Rd(const void* base, uint32_t o) {
  T v;
  memcpy(&v, static_cast<const uint8_t*>(base) + o, sizeof(T));
  return v;
}
template <typename T>
inline void Wr(void* base, uint32_t o, T v) {
  memcpy(static_cast<uint8_t*>(base) + o, &v, sizeof(T));
}

constexpr int kSlots = 64;            // = binaural::kMaxSources
constexpr float kTagFloor = 1e-20f;   // a tag is never all zero (a normal float, about -400 dB)
constexpr double kMinDistanceM = 0.05;
constexpr double kMedianThreshold = 0.05;  // |cL - cR| below: the median-plane fallback
constexpr double kInitialK = 0.5;          // WB / WA without crosstalk (0.2 / 0.4)
constexpr double kHoldSeconds = 0.250;     // an owned voice is kept at least this long
constexpr float kReplaceRatio = 1.4125375f;  // 3 dB

// ---- the mono gain rule (hook spec section 5.1) ----------------------------------------------------------------------
// DCS's gains follow g_ch = WA (1 + c_ch) + WB with c_ch = u . spk[ch] (u = the unit direction listener -> source).
// m = WA (1 + 0.5) + WB: DCS's own gain for this voice with the source straight ahead (c = 0.5 on the +-60 deg pair).
struct MonoSolve {
  bool ok = false;      // false: the pan law does not hold for these gains (counted as a mismatch)
  bool median = false;  // the source is near the median plane (|cL - cR| < 0.05): solved through `base`
  float m = 0;
  double wa = 0, wb = 0;
};

// ed_sound's speed factor (panner 0x1c208..0x1c2ae, single precision as there): s = sound+0x178, v = |relVel|
// (sound+0x108): 1 when s == 1 or v <= 0; s when v >= 200 m/s; else s t + (1 - t) with t = v / 200.
inline float SpeedFactor(float s, const double relVel[3]) {
  if (s == 1.0f) return 1.0f;
  const double v = std::sqrt(relVel[0] * relVel[0] + relVel[1] * relVel[1] + relVel[2] * relVel[2]);
  if (!(200.0 > v)) return s;
  const float vf = static_cast<float>(v);
  if (!(vf > 0.0f)) return 1.0f;
  if (vf >= 200.0f) return s;
  const float t = vf / 200.0f;
  return s * t + (1.0f - t);
}

// The pan law as three equations: g_L = WA (1 + c_L) + WB, g_R = WA (1 + c_R) + WB, and WA + WB / 2 = base / 2
// (WA = (0.4 - 0.35 x) base, WB = (0.2 + 0.7 x) base for any crosstalk x in [0, 1]); base = speed factor x the sound's
// gain, read from the sound. Two of them solve WA and WB (the channel pair, or near the median plane a channel and
// base), the third checks them, and so do WA >= 0.05 base and WB >= 0.2 base (x in [0, 1]). Without a usable base
// (<= 0) the median plane falls back to kPrev = WB / WA of an earlier exact solve. m = 1.5 WA + WB: DCS's own gain for
// this voice with the source straight ahead (c = 0.5 on the +-60 deg pair).
inline MonoSolve SolveMono(double gL, double gR, double cL, double cR, double base, double kPrev) {
  MonoSolve s;
  if (!std::isfinite(gL) || !std::isfinite(gR) || !std::isfinite(cL) || !std::isfinite(cR)) return s;
  const bool haveBase = std::isfinite(base) && base > 0;
  const double scale = std::fmax(std::fmax(std::fabs(gL), std::fabs(gR)), haveBase ? base : 1e-30);
  const double tol = 1e-4 * scale + 1e-30;
  const double d = cL - cR;
  double tolR = tol;
  if (std::fabs(d) >= kMedianThreshold) {
    s.wa = (gL - gR) / d;
    s.wb = gL - s.wa * (1.0 + cL);
    if (haveBase && std::fabs(s.wa + 0.5 * s.wb - 0.5 * base) > tol) return s;
  } else {
    s.median = true;
    if (haveBase) {
      // The channel whose speaker is further from the source conditions best (1 - c >= 0.5 on this pair).
      const bool useL = cL <= cR;
      const double g = useL ? gL : gR, c = useL ? cL : cR;
      s.wa = (base - g) / (1.0 - c);
      s.wb = base - 2.0 * s.wa;
    } else {
      const double k = std::isfinite(kPrev) && kPrev >= 0 ? kPrev : kInitialK;
      const double den = 1.0 + cL + k;
      if (!(den > 1e-9)) return s;
      s.wa = gL / den;
      s.wb = k * s.wa;
      tolR += 2.0 * std::fabs(d) * scale;  // an older k: off by up to |d| times the change of k
    }
  }
  if (s.wa < -tol || s.wb < -tol) return s;
  if (haveBase && (s.wa < 0.05 * base - tol || s.wb < 0.2 * base - tol)) return s;
  const double gl = s.wa * (1.0 + cL) + s.wb, gr = s.wa * (1.0 + cR) + s.wb;
  if (std::fabs(gl - gL) > tolR || std::fabs(gr - gR) > tolR) return s;
  const double m = 1.5 * s.wa + s.wb;
  if (!std::isfinite(m)) return s;
  s.m = static_cast<float>(m > 0 ? m : 0);
  s.ok = true;
  return s;
}

// The pan law of ed_sound's panner for a 3D voice on the stereo pair (hook spec section 2.1), for the tests.
inline void PanLaw(double base, double x, double cL, double cR, float* gL, float* gR) {
  const double a = 0.4 - 0.35 * x, b = 0.2 + 0.7 * x;
  *gL = static_cast<float>((a * (1 + cL) + b) * base);
  *gR = static_cast<float>((a * (1 + cR) + b) * base);
}

// ---- the tag (every owned voice, mono or stereo wave) -----------------------------------------------------------------
// ed_sound renders a stereo wave with the same two gains per output channel as a mono one, a for the wave's left
// channel and b for its right (stereo kernels 0x183e0 / 0x14de0: out[ch] += L' a[ch] + R' b[ch]), after its own width
// stage L' = M + w S, R' = M - w S (M = (L + R) / 2, S = (L - R) / 2; w = 1, or the auto_width distance rule, panner
// 0x1c5ee); its mono kernels play s a[ch] + s b[ch] (0x16aa0: 0x17024..0x17166). The tag ch0 = (m, 0), ch1 = (0, m)
// therefore gives the render side m L' and m R' apart for a stereo wave and m s on both channels for a mono one, also
// when a stream switches its channel count in the middle of a render (0x169ac re-reads it per kernel run). DCS's own
// gains for a frontal source are the same bytes, so the tag also carries a mark in channel 2: ch2 = (m kMarkScale,
// kStereoMark). DCS never writes or reads channel 2 on a stereo output (the panner, the ramp set-up and the kernels all
// stop at the renderer's channel count, renderer+0x2274) and never writes a negative gain. Both mark values are tiny
// normal floats: should a renderer turn multichannel in place before DCS pans again, channel 2 plays the voice about
// 240 dB down, not at full scale.
constexpr float kStereoMark = -0x1p-100f;  // a normal float (FTZ / DAZ leave it alone)
constexpr float kMarkScale = 0x1p-40f;     // ch2.a = m * 2^-40: exact, and normal for every m >= kTagFloor
inline void MakeTag(float m, float tag[6]) {
  tag[0] = m, tag[1] = 0.0f, tag[2] = 0.0f, tag[3] = m, tag[4] = m * kMarkScale, tag[5] = kStereoMark;
}
// Snapshot gains of channels 0..2 (ch0.a, ch0.b, ch1.a, ch1.b, ch2.a, ch2.b): is it the tag?
inline bool IsStereoTag(const float g[6]) {
  return g[1] == 0.0f && g[2] == 0.0f && g[0] > 0.0f && g[3] == g[0] && g[4] == g[0] * kMarkScale && g[5] == kStereoMark;
}

// The mark of a voice on a stereo output is cleared whenever the voice is not tagged (a stale mark next to DCS's own
// frontal gains would read as a tag). Channel 2 of a stereo output is ours alone, so the write is safe.
inline void ClearStereoMark(void* voice) {
  uint8_t* v = static_cast<uint8_t*>(voice);
  const void* rend = Rd<void*>(v, off::kVRenderer);
  if (!rend || Rd<uint32_t>(rend, off::kRChannels) != 2) return;
  if (Rd<float>(v, off::kVPendGain + 20) != kStereoMark) return;
  const float zero[2] = {0.0f, 0.0f};
  memcpy(v + off::kVPendGain + 16, zero, sizeof(zero));
}

// ---- VoiceMeta: one slot per owned voice, written by the sim thread, read by the render thread -----------------------
namespace meta_flags {
constexpr uint32_t kOwned = 1;      // render binaurally (else: fade back to DCS's panning, then acknowledge)
constexpr uint32_t kInitial = 2;    // owned from the voice's creation (P3b): no fade-in, the onset is binaural
constexpr uint32_t kPoseValid = 4;  // qUsed / tUsed hold the head pose DCS used for this frame (pose ring entry)
constexpr uint32_t kNear = 8;       // near the head (under 1.4 m, kept until past 1.6 m; a first frame at 1.5 m): kept
                                    // wide (DCS's side around the binaural mid), the late-translation gain applies
constexpr uint32_t kPosValid = 16;  // tUsed is a tracked position (the pose entry had one; else it is zero)
constexpr uint32_t kStereo = 32;    // a stereo wave (counted; every owned voice carries the same tag)
constexpr uint32_t kWide = 64;      // a stereo wave whose sdef sets auto_width: informational (the sound log). ED's
                                    // flag only narrows a sound with distance; point or wide is decided by kNear and
                                    // the render side's diffuse detector (audio_render.h)
}  // namespace meta_flags

struct MetaData {
  void* voice;
  uint32_t gen;           // changes whenever the slot gets another voice (or the voice another sound)
  uint32_t flags;         // meta_flags
  uint32_t frame;         // SND_process frame counter when written
  uint32_t ctx;           // context id 0..7 (8: unknown)
  float pos[3];           // listener frame (x forward, y up, z right), metres
  float dist;             // metres
  float mTag;             // the tag value written (bit-exact, to pair a snapshot with its metadata)
  float gL, gR;           // DCS's own gains of that frame (the fallback re-pan uses gL / mTag, gR / mTag)
  float qUsed[4];         // head pose (OpenXR, base space) DCS used: x, y, z, w
  float tUsed[3];
  uint64_t poseBase;      // its base space handle
};

struct MetaSlot {
  std::atomic<uint32_t> seq{0};  // odd while written
  MetaData d{};
};

struct MetaTable {
  MetaSlot slot[kSlots];
  std::atomic<void*> voice[kSlots];        // the render side finds a voice's slot here (null: free)
  std::atomic<uint32_t> ackGen[kSlots];    // the render side: the generation it is done with
  std::atomic<uint64_t> torn{0};           // reads that found the slot being written (the render kept its last copy)
};

inline void MetaWrite(MetaSlot& s, const MetaData& d) {
  const uint32_t q = s.seq.load(std::memory_order_relaxed);
  s.seq.store(q + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  memcpy(&s.d, &d, sizeof(d));
  std::atomic_thread_fence(std::memory_order_release);
  s.seq.store(q + 2, std::memory_order_release);
}

// False when the slot was being written (the caller keeps its previous copy).
inline bool MetaRead(const MetaSlot& s, MetaData* out) {
  const uint32_t a = s.seq.load(std::memory_order_acquire);
  if (a & 1u) return false;
  MetaData d;
  memcpy(&d, const_cast<const MetaData*>(&s.d), sizeof(d));
  std::atomic_thread_fence(std::memory_order_acquire);
  if (s.seq.load(std::memory_order_relaxed) != a) return false;
  *out = d;
  return true;
}

// ---- the sim side's ownership state ----------------------------------------------------------------------------------
struct Rec {
  void* voice;
  void* sound;
  uint32_t gen;
  uint32_t lastFrame;
  float loud;          // m of the last frame (the rank key)
  float k;             // WB / WA of the last exact solve (median-plane fallback)
  int16_t slot;        // -1: none
  bool owned;
  bool qualifies;      // in the last frame
  bool initialOwned;   // owned from its creation
  int64_t ownedSince;  // QPC
  uint8_t ctx;
  uint8_t chans;       // wave channels in the last frame (1 or 2)
  bool wide;           // stereo with auto_width (published as meta_flags::kWide, informational)
  uint8_t nearState;   // 0 not decided yet, 1 far, 2 near (meta_flags::kNear, with hysteresis)
};

enum SlotState : uint8_t { kSlotFree = 0, kSlotOwned = 1, kSlotReleasing = 2 };

struct Owner {
  static constexpr uint32_t kRecCap = 1024;  // power of two
  static constexpr uint32_t kKeepFrames = 8;  // a record unseen this many frames is dropped
  Rec rec[2][kRecCap];
  int cur = 0;
  uint32_t used = 0;
  uint32_t frame = 0;          // the SND_process frame counter of the current frame
  bool haveFrame = false;
  uint32_t genNext = 1;
  int owned = 0;               // records owned now
  // Slots (the sim side's view)
  SlotState slotState[kSlots] = {};
  uint32_t slotGen[kSlots] = {};
  void* slotVoice[kSlots] = {};
  int64_t slotReleasedAt[kSlots] = {};
  uint64_t tableFull = 0, slotsExhausted = 0, swaps = 0;

  void Reset() {
    memset(rec, 0, sizeof(rec));
    cur = 0;
    used = 0;
    haveFrame = false;
    owned = 0;
    memset(slotState, 0, sizeof(slotState));
    memset(slotGen, 0, sizeof(slotGen));
    memset(slotVoice, 0, sizeof(slotVoice));
    memset(slotReleasedAt, 0, sizeof(slotReleasedAt));
  }

  static uint32_t Hash(const void* p) {
    uint64_t v = reinterpret_cast<uintptr_t>(p) >> 4;
    v ^= v >> 29;
    v *= 0x9E3779B97F4A7C15ull;
    return static_cast<uint32_t>(v >> 40) & (kRecCap - 1);
  }
  Rec* Find(const void* voice) {
    Rec* t = rec[cur];
    for (uint32_t i = Hash(voice), n = 0; n < kRecCap; ++n, i = (i + 1) & (kRecCap - 1)) {
      if (t[i].voice == voice) return &t[i];
      if (!t[i].voice) return nullptr;
    }
    return nullptr;
  }
  Rec* Insert(void* voice) {
    if (used >= kRecCap * 3 / 4) {
      ++tableFull;
      return nullptr;
    }
    Rec* t = rec[cur];
    for (uint32_t i = Hash(voice), n = 0; n < kRecCap; ++n, i = (i + 1) & (kRecCap - 1)) {
      if (!t[i].voice) {
        memset(&t[i], 0, sizeof(Rec));
        t[i].voice = voice;
        t[i].slot = -1;
        t[i].k = static_cast<float>(kInitialK);
        ++used;
        return &t[i];
      }
    }
    ++tableFull;
    return nullptr;
  }
  uint32_t NewGen() {
    const uint32_t g = genNext++;
    if (!genNext) genNext = 1;
    return g ? g : NewGen();
  }

  int FreeSlot() const {
    for (int s = 0; s < kSlots; ++s)
      if (slotState[s] == kSlotFree) return s;
    return -1;
  }
};

}  // namespace audv
