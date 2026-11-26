// Binaural audio, cockpit space (docs/COCKPIT_SPACE.md 3, 4.4, 8.2): what the simulation side reads for the room, and
// the SpaceState block it publishes once per SND_process frame for the render side.
//
// Simulation thread (PanWrap, ed mutex held): no allocation, locks, logging; bounded work per voice. Every read of
// ed_sound here is gated by the optional signature rows (audio_hooks.h Resolved::spaceGain / spaceOrient): a DCS update
// that moves them turns these reads off, never binaural.
//
//  - inside / outside: ctx+0xe1 (use cockpit gain) of every non-2D context: 1 in the cockpit, 0 in external views.
//    Known only once a context reported 1 in this session (before that the Lua bridge's camera check decides).
//  - through the canopy: an owned voice whose context uses cockpit gain and whose host allows it (sound+0x1a8); DCS's
//    damping sound+0x1b0 (the sdef's constant cockpit_gain) is logged.
//  - the cockpit context: the context whose 3D sounds are nearer than 1.5 m most of the time (vote, about 2 s).
//  - the aircraft frame C_dcs: sound+0xe8 (a source's orientation in the listener frame) of the cockpit context's 3D
//    sounds; the first of a frame gives C, the others its spread. Spread over 1 deg, or a jump over 30 deg between frames
//    while the head pose did not turn that much: unreliable (the last good one is kept, counted).
//  - the listener-parameter probe: the world context's listener gain, pitch and low-pass (ctx+0x58, +0x5c, +0x60).
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "audio_voice.h"

namespace audspace {

using audv::Rd;

namespace off {
constexpr uint32_t kCtxListenerGain = 0x58;   // float
constexpr uint32_t kCtxListenerPitch = 0x5c;  // float
constexpr uint32_t kCtxListenerLp = 0x60;     // float: the voices' low-pass cap
constexpr uint32_t kSndAllowCockpitGain = 0x1a8;  // u8 (SND_host_allow_cockpit_gain)
constexpr uint32_t kSndCockpitGain = 0x1b0;   // float: the sdef's cockpit_gain, when DCS applied it
constexpr uint32_t kSndListenerOrient = 0xe8; // double4 (x, y, z, w): the source's orientation in the listener frame
}  // namespace off

constexpr double kNearVoteM = 1.5;        // a cockpit context's sounds are nearer than this most of the time
constexpr double kVoteTauS = 2.0;
constexpr float kVoteShare = 0.5f;
constexpr double kMaxSpreadRad = 1.0 * 3.14159265358979 / 180.0;
constexpr double kMaxJumpRad = 30.0 * 3.14159265358979 / 180.0;
constexpr double kFrameMaxAgeS = 1.0;     // no cockpit sound for this long: no frame (unless the pose carries it)

namespace flags {
constexpr uint32_t kInside = 1;          // the listener is in the cockpit (valid with kInsideKnown)
constexpr uint32_t kFrameValid = 2;      // c holds a good C_dcs (frameQpc its time)
constexpr uint32_t kFrameFromSound = 4;  // this frame's C came from a sound (else the last good one is carried)
constexpr uint32_t kInsideKnown = 8;     // a context reported use-cockpit-gain in this session
constexpr uint32_t kPoseValid = 16;      // qUsed / poseBase hold the head pose DCS used for C
constexpr uint32_t kReads = 32;          // the space's reads of ed_sound are on (signatures and level)
}  // namespace flags

struct SpaceData {
  uint32_t frame;       // SND_process frame
  uint32_t flags;       // flags::
  float c[4];           // C_dcs: aircraft -> listener frame (x, y, z, w)
  float qUsed[4];       // head pose DCS used for it (as MetaData)
  uint64_t poseBase;
  int64_t frameQpc;     // when c was taken (QPC)
  uint8_t cockpitCtx = 8;  // context id voted as cockpit (8 unknown)
  uint8_t worldCtx = 8;    // the context the listener probe read (8 none)
  float spreadDeg;      // the last frame's spread of C between the cockpit sounds
  uint32_t unreliable;  // frames whose C was refused, in the session
  float lgGain, lgPitch, lgLowpass;  // the listener probe
};

struct SpaceSlot {
  std::atomic<uint32_t> seq{0};
  SpaceData d{};
};
inline void SpaceWrite(SpaceSlot& s, const SpaceData& d) {
  const uint32_t q = s.seq.load(std::memory_order_relaxed);
  s.seq.store(q + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  memcpy(&s.d, &d, sizeof(d));
  std::atomic_thread_fence(std::memory_order_release);
  s.seq.store(q + 2, std::memory_order_release);
}
inline bool SpaceRead(const SpaceSlot& s, SpaceData* out) {
  for (int i = 0; i < 8; ++i) {
    const uint32_t a = s.seq.load(std::memory_order_acquire);
    if (a & 1u) continue;
    SpaceData d;
    memcpy(&d, const_cast<const SpaceData*>(&s.d), sizeof(d));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (s.seq.load(std::memory_order_relaxed) != a) continue;
    *out = d;
    return true;
  }
  return false;
}

// ---- quaternions (x, y, z, w), double ------------------------------------------------------------------------------
inline void QNorm(double q[4]) {
  const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (!(n > 1e-12) || !std::isfinite(n)) {
    q[0] = q[1] = q[2] = 0, q[3] = 1;
    return;
  }
  for (int k = 0; k < 4; ++k) q[k] /= n;
}
inline double QAngle(const double a[4], const double b[4]) {
  const double d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
  return 2.0 * std::acos(d > 1.0 ? 1.0 : d);
}
// v' = q v q* (head_pose_shm.h Rotate, ed_sound's sub_1c60)
inline void QRotate(const float q[4], const float v[3], float out[3]) {
  const double x = q[0], y = q[1], z = q[2], w = q[3];
  const double c[3] = {y * v[2] - z * v[1], z * v[0] - x * v[2], x * v[1] - y * v[0]};
  const double cc[3] = {y * c[2] - z * c[1], z * c[0] - x * c[2], x * c[1] - y * c[0]};
  for (int k = 0; k < 3; ++k) out[k] = static_cast<float>(v[k] + 2 * (w * c[k] + cc[k]));
}
// The head against the aircraft from C (aircraft -> head): yaw (left +), pitch (up +), roll, degrees, of the aircraft's
// forward axis seen from the head (for audio_space.csv; the sign follows the listener frame x fwd, y up, z right).
inline void YawPitchRoll(const float c[4], float* yaw, float* pitch, float* roll) {
  const float fwd[3] = {1, 0, 0}, up[3] = {0, 1, 0};
  float f[3], u[3];
  QRotate(c, fwd, f);
  QRotate(c, up, u);
  *yaw = static_cast<float>(std::atan2(-f[2], f[0]) * 57.29577951);
  *pitch = static_cast<float>(std::atan2(f[1], std::sqrt(f[0] * f[0] + f[2] * f[2])) * 57.29577951);
  *roll = static_cast<float>(std::atan2(u[2], u[1]) * 57.29577951);
}

// ---- the simulation side's state -----------------------------------------------------------------------------------
struct SimSpace {
  // set by the glue each frame (sim thread)
  bool gainReads = false;    // ctx+0xe1, sound+0x1a8 / +0x1b0 (Resolved::spaceGain, cockpit space on)
  bool orientReads = false;  // sound+0xe8 (Resolved::spaceOrient, cockpit space on)
  // the frame being accumulated
  uint32_t nearN[audv::off::kMaxContexts] = {}, total[audv::off::kMaxContexts] = {};
  bool haveC = false;
  double c[4] = {0, 0, 0, 1};
  double spread = 0;
  bool accPose = false;
  float accQ[4] = {0, 0, 0, 1};
  uint64_t accBase = 0;
  // across frames
  float share[audv::off::kMaxContexts] = {};
  int64_t seen[audv::off::kMaxContexts] = {};
  uint8_t cockpitCtx = 8;
  bool haveGood = false;
  double good[4] = {0, 0, 0, 1};
  int64_t goodQpc = 0;
  bool goodPose = false;
  float goodQ[4] = {0, 0, 0, 1};
  uint64_t goodBase = 0;
  bool havePrevHead = false;
  float prevHead[4] = {0, 0, 0, 1};
  uint32_t unreliable = 0;
  bool insideSeen = false;   // a context reported use-cockpit-gain in this session
  int64_t lastPublish = 0;
  float lastSpreadDeg = 0;

  void Reset() { *this = SimSpace{}; }
};

// One 3D sound seen by PanVoice (any voice of a non-2D context, owned or not): the cockpit vote and the frame.
inline void Accumulate(SimSpace& sp, uint32_t ctxId, const void* sound, double dist) {
  if (ctxId >= static_cast<uint32_t>(audv::off::kMaxContexts) || !sound) return;
  ++sp.total[ctxId];
  if (dist < kNearVoteM) ++sp.nearN[ctxId];
  if (!sp.orientReads || ctxId != sp.cockpitCtx) return;
  double q[4];
  memcpy(q, static_cast<const uint8_t*>(sound) + off::kSndListenerOrient, sizeof(q));
  if (!std::isfinite(q[0]) || !std::isfinite(q[1]) || !std::isfinite(q[2]) || !std::isfinite(q[3])) return;
  const double n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
  if (!(n2 > 0.81 && n2 < 1.21)) return;  // not a unit quaternion: not an orientation
  QNorm(q);
  if (!sp.haveC) {
    memcpy(sp.c, q, sizeof(q));
    sp.haveC = true;
    sp.spread = 0;
  } else {
    const double a = QAngle(sp.c, q);
    if (a > sp.spread) sp.spread = a;
  }
}

// Through-canopy class of an owned voice (meta flag kThroughCanopy): its context uses cockpit gain and its host allows
// it. *gain: DCS's damping (sound+0x1b0) when so, else 1.
inline bool ThroughCanopy(const SimSpace& sp, const void* ctx, const void* sound, float* gain) {
  *gain = 1.0f;
  if (!sp.gainReads || !ctx || !sound) return false;
  if (!Rd<uint8_t>(ctx, audv::off::kCtxCockpitGain) || !Rd<uint8_t>(sound, off::kSndAllowCockpitGain)) return false;
  const float g = Rd<float>(sound, off::kSndCockpitGain);
  *gain = std::isfinite(g) ? g : 1.0f;
  return true;
}

// Start of a new SND_process frame: the finished frame's results into `out` (the caller writes the slot), and the
// accumulators reset for this frame (whose head pose is q / poseValid).
inline void BeginFrame(SimSpace& sp, const uint8_t* ctxArray, int ctxCount, uint32_t frame, int64_t now, int64_t freq,
                       bool poseValid, const float q[4], uint64_t poseBase, SpaceData* out) {
  SpaceData d{};
  d.frame = frame;
  d.cockpitCtx = 8;
  d.worldCtx = 8;
  // The cockpit vote (exponential, kVoteTauS).
  const double dt = sp.lastPublish && freq ? static_cast<double>(now - sp.lastPublish) / static_cast<double>(freq) : 0.0;
  const double k = dt > 0 ? 1.0 - std::exp(-(dt < 1.0 ? dt : 1.0) / kVoteTauS) : 0.0;
  int best = -1;
  for (int i = 0; i < audv::off::kMaxContexts; ++i) {
    if (sp.total[i]) {
      const float s = static_cast<float>(sp.nearN[i]) / static_cast<float>(sp.total[i]);
      sp.share[i] += static_cast<float>(k) * (s - sp.share[i]);
      if (!sp.seen[i]) sp.share[i] = s;  // a context's first frame decides at once
      sp.seen[i] = now;
    } else if (sp.seen[i] && freq && now - sp.seen[i] > static_cast<int64_t>(kVoteTauS * static_cast<double>(freq))) {
      sp.share[i] = 0, sp.seen[i] = 0;  // gone quiet
    }
    if (sp.seen[i] && sp.share[i] > kVoteShare && (best < 0 || sp.share[i] > sp.share[best])) best = i;
  }
  sp.cockpitCtx = best < 0 ? 8 : static_cast<uint8_t>(best);
  d.cockpitCtx = sp.cockpitCtx;
  // The frame: the finished frame's C, checked.
  float spreadDeg = 0;
  if (sp.haveC) {
    spreadDeg = static_cast<float>(sp.spread * 57.29577951);
    bool ok = sp.spread <= kMaxSpreadRad;
    if (ok && sp.haveGood) {
      const double jump = QAngle(sp.c, sp.good);
      double headTurn = 0;
      if (sp.accPose && sp.goodPose) {
        const double a[4] = {sp.accQ[0], sp.accQ[1], sp.accQ[2], sp.accQ[3]};
        const double b[4] = {sp.goodQ[0], sp.goodQ[1], sp.goodQ[2], sp.goodQ[3]};
        headTurn = QAngle(a, b);
      }
      if (jump > kMaxJumpRad && headTurn < 0.5 * jump) ok = false;
    }
    if (ok) {
      memcpy(sp.good, sp.c, sizeof(sp.good));
      sp.haveGood = true;
      sp.goodQpc = now;
      sp.goodPose = sp.accPose;
      memcpy(sp.goodQ, sp.accQ, sizeof(sp.goodQ));
      sp.goodBase = sp.accBase;
      d.flags |= flags::kFrameFromSound;
    } else {
      ++sp.unreliable;
    }
    sp.lastSpreadDeg = spreadDeg;
  }
  if (sp.haveGood) {
    d.flags |= flags::kFrameValid;
    for (int i = 0; i < 4; ++i) d.c[i] = static_cast<float>(sp.good[i]);
    d.frameQpc = sp.goodQpc;
    if (sp.goodPose) {
      d.flags |= flags::kPoseValid;
      memcpy(d.qUsed, sp.goodQ, sizeof(d.qUsed));
      d.poseBase = sp.goodBase;
    }
  }
  d.spreadDeg = sp.lastSpreadDeg;
  d.unreliable = sp.unreliable;
  // Inside / outside and the listener probe (ctx+0xe1, +0x58..+0x60).
  if (sp.gainReads && ctxArray) {
    d.flags |= flags::kReads;
    bool inside = false;
    int world = -1, first3d = -1;
    for (int i = 0; i < ctxCount && i < audv::off::kMaxContexts; ++i) {
      const uint8_t* c = ctxArray + audv::off::kCtxStride * i;
      if (Rd<uint8_t>(c, audv::off::kCtxIs2D)) continue;
      if (first3d < 0) first3d = i;
      if (Rd<uint8_t>(c, audv::off::kCtxCockpitGain)) {
        inside = true;
        if (world < 0) world = i;
      }
    }
    if (inside) sp.insideSeen = true;
    if (sp.insideSeen) d.flags |= flags::kInsideKnown | (inside ? flags::kInside : 0u);
    if (world < 0) world = first3d;
    if (world >= 0) {
      const uint8_t* c = ctxArray + audv::off::kCtxStride * world;
      d.worldCtx = static_cast<uint8_t>(world);
      d.lgGain = Rd<float>(c, off::kCtxListenerGain);
      d.lgPitch = Rd<float>(c, off::kCtxListenerPitch);
      d.lgLowpass = Rd<float>(c, off::kCtxListenerLp);
    }
  } else if (sp.orientReads) {
    d.flags |= flags::kReads;
  }
  *out = d;
  // This frame's accumulators.
  memset(sp.nearN, 0, sizeof(sp.nearN));
  memset(sp.total, 0, sizeof(sp.total));
  sp.haveC = false;
  sp.spread = 0;
  sp.accPose = poseValid;
  memcpy(sp.accQ, q, sizeof(sp.accQ));
  sp.accBase = poseBase;
  sp.lastPublish = now;
}

}  // namespace audspace
