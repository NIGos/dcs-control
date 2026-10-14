// Cockpit at the true eye distance while Depth boost renders the world at the
// boosted one ([Perception] CockpitTrueIpd, default 0; design: docs/research
// R31_dual_camera_boost.md, option 3). Not a performance optimization: outside
// the kill switch, not one of the status word's features.
//
// Approach A of the Perception contract (dcsvr_perception_shm.h, BoostOut, a
// verbatim copy of the layer's perception_shm.h): the pupil shift layer keeps
// returning boosted eye poses from xrLocateViews, each eye moved along the
// eye axis by h = (renderedIpdM - baseIpdM) / 2 in every view of that eye. DCS
// renders the whole frame with them. This module moves only the cockpit-class
// passes of HMD views 0-3 back to the true eye by rewriting the bytes of their
// cPerView upload (register b7, 0x970 bytes, uploaded with UpdateSubresource
// inside the pass by ContextBase::updateBuffers); every other pass, the world,
// shadows, MFD views, HUD and overlays keep DCS's bytes. Every failure forwards
// DCS's own bytes, i.e. today's frame.
//
// Which passes (R31 3.2):
//  - GBufferPass executions with shading model 4 (SM_GBUFFER_PBR_COCKPIT),
//    classified in gbpass::Hook (gb_batch.h, slot 19) before DCS's binder
//    uploads cPerView inside the same execution: gbpass::ItemVector ->
//    gbrec::ExecKeyGuarded -> CollKey.sm (tag 600 only counted as a cross
//    check). The HMD view is fixation_depth.h's CopyToHMDPass ordinal.
//  - TransparentCockpitPass / TransparentCockpitDecalPass (any name containing
//    "TransparentCockpit"), only when that view's sm 4 G-buffer pass was
//    patched this frame ([Perception] CockpitTrueIpdForward, default 1). Their
//    upload entries are armed alone (ArmUsOnly in fixation_depth.h).
//  - Nothing else (CockpitGlassEffects only after the live L0 trace shows it
//    uploads its own b7; the trace below reports that).
//
// The patch (R31 3.4), in double, written back as float, matrices stored
// transposed (logical M[r][c] at f[c * 4 + r]), row vectors:
//   gView        T(-Dw) * V           gViewInv      VI * T(Dw)
//   gViewProj    T(-Dw) * VP          gViewProjInv  VPI * T(Dw)
//   gPrevFrameViewProj  T(-Dp) * PVP (DCS's previous matrix, no stored state)
//   gPrevFrameTransform += outer(VPI[:, 3], (Dw - Dp, 0) * PVP)
//   gCameraPos   xyz += Dw
// gProj (DLSS jitter), gProjInv and every other byte are DCS's. A cell whose
// value does not change keeps its exact bits.
//   Dw = sigma * h * k * u: sigma +1 for views 0 and 2 (left eye, moved back to
//   its right), -1 for views 1 and 3; h from the frame latch; k DCS units per
//   metre (1 unless the learned estimate is more than 1 % off); u the eye axis,
//   learned as a head-frame constant from views 0/1 (unit(C1 - C0) in view 0's
//   camera frame, 120 frames), the view's own right axis until then.
//   Dp = the Dw applied to the same view in the previous frame (0 if none). A
//   frame whose Dw is 0 with Dp != 0 patches only the two gPrev fields (tail).
// Self-checks per upload (failure: not patched, counted; > 1 % of the patch
// attempts over 300 frames latches the option inert): |gViewInv row 0| in
// [0.99, 1.01], gViewProj == gView * gProj within 1e-4 relative, finite
// matrices; |gCameraPos - gViewInv row 3| < 1 m is logged only.
// Restore: at the end of each pass that forwarded a patched upload, DCS's
// original bytes are uploaded again into the same buffer and box (one extra
// upload, ordered by D3D11 after the pass's draws), so later binder-less
// passes and copies see DCS's content. Every later upload of such a buffer
// in the pass that is forwarded as DCS's is merged into the restore entry, so
// the restore never writes older bytes over newer ones. A struct whose
// buffer holds several learned structs (offset binding) is the one the box
// covers alone, else the one b7 binds now; else it is forwarded (ambiguous).
//
// The observe flag and h are latched once per frame (a mode change applies
// from the next frame, so Dp always describes what was really applied).
// Frame pairing (R31 3.5): BoostOut (Local\DcsVrDepthBoost_v1 plus the
// DCSVR_PERCEPTION_SHM_SUFFIX suffix) opened read-only only when the option is
// on, retried once per second, seqlock read (3 tries) at each top-level pass
// boundary, new records into a 16-entry ring. One latch per frame at view 0's
// cockpit upload, used by all views and passes: (1) all candidates (records
// newer than the previous latch, the newest and the 3 a lag can pick) agree
// within 0.05 mm: the newest; (2) the FOV fingerprint: view 0's FOV from this upload's gProj
// against a GazeOut ring, decisive only (pupil shift on); (3) the learned lag
// L (0..2 records behind the newest). Verification at view 1's upload: the
// measured |C1 - C0| / k against each lag's record plus a learned bias; a lag
// better by 0.3 mm for 3 frames of a changing scale is adopted; > 3 mm at every
// lag for 10 frames latches pairFault (h = 0) until 60 frames agree again.
// Fallbacks (h = 0, the tail frame still runs): no block, stale (no new
// record for 30 frames, or none since the previous latch for more latches
// than the learned lag covers: dry), enabled/applied clear, scale within 1e-3 of 1,
// h < 0.05 mm, h > 40 mm, pairFault, k outside [0.95, 1.05], axis sign wrong,
// self-check rate. Whole frame unpatched: foreign table hooks at the latch
// (gpu_pass_timing / gpu_dedup / D3D meter; configured at load: refused),
// view 0's cockpit pass not handled, a second sm 4 pass in a view or more than
// four; a frame that patched some views and failed a later one forces the
// next frame off; so does a forward cockpit pass of a moved view that was not
// armed, lost its arm or forwarded a cPerView upload unpatched (also counted
// with the self-check rate). SEH anywhere (or a fixation depth fault): off for
// this load; a pass of ours open at the fault is still closed (restore, arm).
// [Perception] CockpitTrueIpd=2 (observe): everything above runs and is
// logged, nothing is modified. CockpitTrueIpdLog=N: a per-pass trace for N
// frames (view, pass, sm, tag, uploads of a known cPerView, Dw, restores) also
// of every pass whose name contains "Cockpit". CockpitTrueIpdTestShiftMm:
// shift the cockpit passes by that many mm per eye without the layer
// (ignored while BoostOut reports a real boost).
// Phase 2 (CockpitTrueIpdLighting=1, default 0, R31 3.7): during a patched
// HMD view's GBufferLightingPass (in-pass arm: OM / viewport / upload and the
// draw entries) a non-indexed full-screen Draw(3|4, 0) whose PS b7 is a known
// cPerView buffer, which reads the cockpit pass's depth texture at a PS SRV,
// and whose RTV matches that texture's size, is drawn twice through a
// read-only DSV on it: stencil NOT_EQUAL 0x28 (mask 0x38) with DCS's b7, then EQUAL with a
// private copy of the lighting pass's bytes patched with this frame's Dw.
// DSV, depth-stencil state and ref, VS/PS b7 are put back exactly. MSAA
// depth, a bound state that already uses stencil or writes depth, compute and
// other draws: drawn once as today (counted).
// Included from fixation_depth.h inside namespace fixdepth (after State, the
// shared-memory globals and far_depth.h).
#pragma once

namespace ckipd {

namespace psh = ::dcsvr_perception;

// ---------------------------------------------------------------------------
// Configuration (config thread writes; render thread reads)
// ---------------------------------------------------------------------------

std::atomic<int> g_cfgMode{0};  // 0 off, 1 on, 2 observe
std::atomic<bool> g_cfgLighting{false}, g_cfgForward{true}, g_cfgForeign{false};
std::atomic<double> g_cfgShiftMm{0.0};
std::atomic<int> g_logLeft{0};
std::atomic<bool> g_on{false};     // started: the render-thread paths run
std::atomic<bool> g_fault{false};  // SEH: off for this load
std::atomic<int> g_busy{0};
// In-flight A/B ([Perception] CockpitTrueIpdToggle): true leaves the cockpit at
// the layer's boosted eye (today's image) until pressed again.
std::atomic<bool> g_userOff{false};
// Seams and diagnostics (the off-path tests read them).
std::atomic<uint64_t> g_gbInstallCalls{0}, g_boostOpens{0}, g_starts{0};
std::wstring g_boostNameOverride;  // tests: their own mapping name

inline bool Observe() { return g_cfgMode.load(std::memory_order_relaxed) == 2; }
inline bool Wanted() { return g_cfgMode.load() != 0 && !g_cfgForeign.load(); }
// The render-thread paths patch, split or classify only while this is true:
// started, no fault of ours and none of fixation_depth.h's (whose boundary
// callback then stops; a pass of ours that is still open is closed by its
// fault path, OnBoundary). g_on stays set after a fault: only Stop clears it,
// after draining g_busy, so a pass open at the fault is still closed (its
// restore and its arm) on the render thread.
inline bool Active() {
  return g_on.load(std::memory_order_relaxed) && !g_fault.load(std::memory_order_relaxed) &&
         !::fixdepth::g_fault.load(std::memory_order_relaxed);
}
// The cPerView upload entries are armed in G-buffer passes only while this is true.
inline bool WantUploads() { return Active(); }

// ---------------------------------------------------------------------------
// cPerView layout and the patch (pure)
// ---------------------------------------------------------------------------

constexpr uint32_t kCbBytes = 0x970;
constexpr uint32_t kOffView = 0x080, kOffViewInv = 0x0C0, kOffProj = 0x100, kOffProjInv = 0x140,
                   kOffViewProj = 0x180, kOffViewProjInv = 0x1C0, kOffPrevXf = 0x200, kOffPrevVP = 0x240,
                   kOffCamPos = 0x620;
constexpr uint32_t kMaxSrc = 0x4000;  // largest upload we copy (DCS's buffer is 0x970)

struct M4 {
  double a[4][4];
};
inline M4 LoadM(const uint8_t* s, uint32_t off) {
  float f[16];
  memcpy(f, s + off, sizeof(f));
  M4 m;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) m.a[r][c] = f[c * 4 + r];
  return m;
}
inline void StoreM(uint8_t* s, uint32_t off, const M4& m) {
  float f[16];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) f[c * 4 + r] = static_cast<float>(m.a[r][c]);
  memcpy(s + off, f, sizeof(f));
}
inline M4 MulM(const M4& x, const M4& y) {
  M4 m;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      double v = 0;
      for (int k = 0; k < 4; ++k) v += x.a[r][k] * y.a[k][c];
      m.a[r][c] = v;
    }
  return m;
}
inline bool NonZero(const double v[3]) { return v[0] != 0 || v[1] != 0 || v[2] != 0; }
// a += t, leaving a's exact bits (and the sign of a zero) when t is 0.
inline void AddTo(double& a, double t) {
  if (t != 0) a += t;
}

// Moves the camera of one cPerView struct (s: its first byte) by dw and the
// previous frame's camera by dp, world units (R31 3.4). Pure.
enum : uint32_t {
  kFView = 1, kFViewInv = 2, kFViewProj = 4, kFViewProjInv = 8, kFPrevXf = 16, kFPrevVP = 32, kFCamPos = 64
};
uint32_t PatchStruct(uint8_t* s, const double dw[3], const double dp[3]) {
  uint32_t changed = 0;
  const double e[3] = {dw[0] - dp[0], dw[1] - dp[1], dw[2] - dp[2]};
  const M4 vpi = LoadM(s, kOffViewProjInv);  // DCS's
  const M4 pvp = LoadM(s, kOffPrevVP);       // DCS's
  if (NonZero(dw)) {
    auto rowSub = [&](uint32_t off) {  // T(-dw) * M: row 3 -= sum_j dw_j * row j
      M4 m = LoadM(s, off);
      for (int c = 0; c < 4; ++c) AddTo(m.a[3][c], -(dw[0] * m.a[0][c] + dw[1] * m.a[1][c] + dw[2] * m.a[2][c]));
      StoreM(s, off, m);
    };
    auto colAdd = [&](uint32_t off) {  // M * T(dw): [r][c < 3] += M[r][3] * dw_c
      M4 m = LoadM(s, off);
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) AddTo(m.a[r][c], m.a[r][3] * dw[c]);
      StoreM(s, off, m);
    };
    rowSub(kOffView);
    colAdd(kOffViewInv);
    rowSub(kOffViewProj);
    colAdd(kOffViewProjInv);
    float cp[3];
    memcpy(cp, s + kOffCamPos, sizeof(cp));
    for (int i = 0; i < 3; ++i)
      if (dw[i] != 0) cp[i] = static_cast<float>(static_cast<double>(cp[i]) + dw[i]);
    memcpy(s + kOffCamPos, cp, sizeof(cp));
    changed |= kFView | kFViewInv | kFViewProj | kFViewProjInv | kFCamPos;
  }
  if (NonZero(e)) {  // VPI' * PVP' = VPI * T(dw - dp) * PVP = VPI * PVP + VPI[:, 3] (x) (e, 0) * PVP
    double row[4];
    for (int c = 0; c < 4; ++c) row[c] = e[0] * pvp.a[0][c] + e[1] * pvp.a[1][c] + e[2] * pvp.a[2][c];
    M4 x = LoadM(s, kOffPrevXf);
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) AddTo(x.a[r][c], vpi.a[r][3] * row[c]);
    StoreM(s, kOffPrevXf, x);
    changed |= kFPrevXf;
  }
  if (NonZero(dp)) {
    M4 q = pvp;
    for (int c = 0; c < 4; ++c) AddTo(q.a[3][c], -(dp[0] * pvp.a[0][c] + dp[1] * pvp.a[1][c] + dp[2] * pvp.a[2][c]));
    StoreM(s, kOffPrevVP, q);
    changed |= kFPrevVP;
  }
  return changed;
}

// Self-checks of DCS's struct (bits: failure reasons). kChkCam alone is not a failure.
enum : uint32_t { kChkAxis = 1, kChkVp = 2, kChkFinite = 4, kChkCam = 8 };
uint32_t SelfCheck(const uint8_t* s) {
  uint32_t bad = 0;
  const uint32_t offs[] = {kOffView, kOffViewInv, kOffProj, kOffViewProj, kOffViewProjInv, kOffPrevXf, kOffPrevVP};
  for (uint32_t o : offs) {
    const M4 m = LoadM(s, o);
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        if (!std::isfinite(m.a[r][c])) bad |= kChkFinite;
  }
  if (bad) return bad;
  const M4 vi = LoadM(s, kOffViewInv);
  const double ax = std::sqrt(vi.a[0][0] * vi.a[0][0] + vi.a[0][1] * vi.a[0][1] + vi.a[0][2] * vi.a[0][2]);
  if (!(ax >= 0.99 && ax <= 1.01)) bad |= kChkAxis;
  const M4 vp = LoadM(s, kOffViewProj);
  const M4 ref = MulM(LoadM(s, kOffView), LoadM(s, kOffProj));
  double err = 0, scale = 1;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      err = std::max(err, std::fabs(vp.a[r][c] - ref.a[r][c]));
      scale = std::max(scale, std::fabs(vp.a[r][c]));
    }
  if (!(err <= 1e-4 * scale)) bad |= kChkVp;
  float cp[3];
  memcpy(cp, s + kOffCamPos, sizeof(cp));
  const double dx = cp[0] - vi.a[3][0], dy = cp[1] - vi.a[3][1], dz = cp[2] - vi.a[3][2];
  if (!(dx * dx + dy * dy + dz * dz < 1.0)) bad |= kChkCam;
  return bad;
}

// The camera of a struct: position (gViewInv row 3) and axes (rows 0..2).
void Camera(const uint8_t* s, double C[3], double R[3][3]) {
  const M4 vi = LoadM(s, kOffViewInv);
  for (int i = 0; i < 3; ++i) {
    C[i] = vi.a[3][i];
    for (int j = 0; j < 3; ++j) R[i][j] = vi.a[i][j];
  }
}

// DCS's FOV tangents (left, right, up, down) from the struct's gProj; false
// when it is not a perspective projection.
bool StructFov(const uint8_t* s, double fov[4]) {
  const M4 p = LoadM(s, kOffProj);
  float m[16];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) m[r * 4 + c] = static_cast<float>(p.a[r][c]);
  const Proj pr = ParseProj(m);
  if (!pr.ok) return false;
  DcsFov(pr, fov);
  return true;
}

// ---------------------------------------------------------------------------
// Eye axis (pure): unit(C1 - C0) as a constant in view 0's camera frame
// ---------------------------------------------------------------------------

struct Axis {
  static constexpr int kLearn = 120;
  double uh[3] = {};
  int n = 0;
  bool Learned() const { return n >= kLearn; }
  void Add(const double R0[3][3], const double d[3]) {
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-4)) return;
    const double a = 1.0 / std::min(n + 1, kLearn);
    for (int i = 0; i < 3; ++i) {
      const double x = (d[0] * R0[i][0] + d[1] * R0[i][1] + d[2] * R0[i][2]) / len;
      uh[i] = uh[i] * (1 - a) + x * a;
    }
    ++n;
  }
  // The axis in world space for a frame whose view 0 has axes R0.
  bool World(const double R0[3][3], double out[3]) const {
    double l = 0;
    for (int j = 0; j < 3; ++j) {
      out[j] = uh[0] * R0[0][j] + uh[1] * R0[1][j] + uh[2] * R0[2][j];
      l += out[j] * out[j];
    }
    l = std::sqrt(l);
    if (!(l > 1e-6)) return false;
    for (int j = 0; j < 3; ++j) out[j] /= l;
    return true;
  }
  // Degrees between the learned axis and the camera's right axis.
  double AngleToRightDeg() const {
    const double l = std::sqrt(uh[0] * uh[0] + uh[1] * uh[1] + uh[2] * uh[2]);
    return l > 0 ? std::acos(std::clamp(uh[0] / l, -1.0, 1.0)) * 57.29577951308232 : 0.0;
  }
};

// ---------------------------------------------------------------------------
// Pairing the layer's records with the frame DCS renders (pure)
// ---------------------------------------------------------------------------

struct Rec {
  int64_t displayTime = 0;
  uint32_t flags = 0, frame = 0;
  float scale = 1, base = 0, rendered = 0;
};
inline Rec FromBoost(const psh::BoostOut& b) {
  Rec r;
  r.displayTime = b.displayTime;
  r.flags = b.flags;
  r.frame = b.frame;
  r.scale = b.scale;
  r.base = b.baseIpdM;
  r.rendered = b.renderedIpdM;
  return r;
}
// Per-eye shift (metres) the record's views carry; 0 when nothing was applied.
inline double RecH(const Rec& r) {
  if (!(r.flags & psh::boost_flags::enabled) || !(r.flags & psh::boost_flags::applied)) return 0;
  if (!(std::fabs(r.scale - 1.0f) >= 1e-3f)) return 0;
  const double h = (static_cast<double>(r.rendered) - static_cast<double>(r.base)) / 2;
  return std::isfinite(h) ? h : 0.0;
}
// The eye distance DCS renders that record's views with (metres).
inline double RecIpd(const Rec& r) { return RecH(r) != 0 ? r.rendered : r.base; }

struct GazeRec {
  int64_t displayTime = 0;
  float fov[4] = {};
};
struct GazeRing {
  static constexpr int kN = 16;
  GazeRec r[kN];
  int n = 0, head = 0;
  const GazeRec& At(int back) const { return r[(head - 1 - back + 2 * kN) % kN]; }
  void Push(const GazeRec& g) {
    if (n && At(0).displayTime == g.displayTime) return;
    r[head] = g;
    head = (head + 1) % kN;
    if (n < kN) ++n;
  }
};

enum Path : int { kPathNone = 0, kPathSame = 1, kPathFov = 2, kPathLag = 3, kPathTest = 4 };
const char* const kPathNames[5] = {"none", "same", "fov", "lag", "test"};

struct Matcher {
  static constexpr int kRing = 16, kLags = 3, kStaleFrames = 30;
  Rec ring[kRing];
  int count = 0, head = 0;
  uint64_t lastPushDcs = 0;
  uint64_t pushes = 0, latchedPushes = 0;  // records pushed; the count at the previous latch
  int dry = 0;                              // consecutive latches without a new record
  uint64_t drys = 0;                        // diagnostics
  bool latchedAny = false;
  uint32_t latchedFrame = 0;
  int lag = 0;
  int better[kLags] = {};
  bool fault = false;
  int bad = 0, good = 0;
  double bias = 0, kEst = 1;
  int biasN = 0, kN = 0;
  // diagnostics
  uint64_t paths[5] = {}, switches = 0, faults = 0, verifies = 0;
  double residSum = 0, residMax = 0;
  uint64_t residN = 0;

  const Rec& At(int back) const { return ring[(head - 1 - back + 2 * kRing) % kRing]; }
  // A new record (render thread); false when it is the newest one already.
  bool Push(const Rec& r, uint64_t dcsFrame) {
    if (count && At(0).frame == r.frame && At(0).displayTime == r.displayTime) return false;
    ring[head] = r;
    head = (head + 1) % kRing;
    if (count < kRing) ++count;
    lastPushDcs = dcsFrame;
    ++pushes;
    return true;
  }
  bool Stale(uint64_t dcsFrame) const { return !count || dcsFrame - lastPushDcs >= static_cast<uint64_t>(kStaleFrames); }
  // k: DCS units per metre (1 unless the learned estimate is more than 1 % off).
  double K() const { return kN >= 30 && std::fabs(kEst - 1) > 0.01 ? kEst : 1.0; }
  bool KBad() const { return kN >= 30 && std::fabs(kEst - 1) > 0.05; }

  struct LatchOut {
    double h = 0;
    int path = kPathNone;
    bool stale = true, any = false;
    bool dry = false;  // stale because no new record came for more latches than the lag covers
    Rec rec;
    Rec lagRec[kLags];
    bool lagOk[kLags] = {};
  };

  // The FOV fingerprint: the boost record (back index) whose displayTime is
  // the one of the gaze record matching view 0's FOV decisively; -1 if none.
  int FovPick(const GazeRing& g, const double fov[4]) const {
    int best = -1;
    double bestD = 1e9, second = 1e9;
    for (int i = 0; i < g.n; ++i) {
      double d = 0;
      for (int k = 0; k < 4; ++k) d = std::max(d, std::fabs(static_cast<double>(g.At(i).fov[k]) - fov[k]));
      if (d < bestD) {
        second = bestD;
        bestD = d;
        best = i;
      } else if (d < second) {
        second = d;
      }
    }
    if (best < 0 || bestD > 0.004 || second - bestD < 0.002) return -1;
    const int64_t dt = g.At(best).displayTime;
    for (int b = 0; b < count; ++b)
      if (At(b).displayTime == dt) return b;
    return -1;
  }

  // Once per DCS frame, at view 0's cockpit upload.
  LatchOut Latch(uint64_t dcsFrame, const GazeRing* gaze, const double* fov) {
    LatchOut o;
    o.stale = Stale(dcsFrame);
    if (!count) return o;
    for (int L = 0; L < kLags; ++L) {
      o.lagOk[L] = L < count;
      if (o.lagOk[L]) o.lagRec[L] = At(L);
    }
    if (o.stale) return o;
    // The layer writes one record per xrLocateViews while Depth boost runs, so
    // each DCS frame brings one. A latch without a new record for more
    // frames than the learned lag covers renders poses the ring does not
    // describe (Depth boost switched off: the last record stays in the block,
    // DCS already gets unboosted poses): h 0, and no verification on it.
    const bool fresh = pushes != latchedPushes;
    latchedPushes = pushes;
    dry = fresh ? 0 : dry + 1;
    if (dry > lag) {
      o.stale = o.dry = true;
      bad = 0;  // a pairing fault needs consecutive verified frames
      ++drys;
      return o;
    }
    o.any = true;
    // Candidates: records newer than the previous latch, plus the newest and
    // every record a learnable lag could pick (0..kLags-1 behind the newest).
    int nc = 0;
    for (int b = 0; b < count; ++b) {
      if (latchedAny && static_cast<int32_t>(At(b).frame - latchedFrame) <= 0) break;
      ++nc;
    }
    nc = std::max(nc, std::min(count, kLags));
    bool same = true;
    const double ipd0 = RecIpd(At(0));
    const bool h0 = RecH(At(0)) != 0;
    for (int b = 1; b < nc && same; ++b)
      if (std::fabs(RecIpd(At(b)) - ipd0) > 0.05e-3 || (RecH(At(b)) != 0) != h0) same = false;
    int pick = 0;
    if (same) {
      o.path = kPathSame;
    } else {
      const int f = gaze && fov ? FovPick(*gaze, fov) : -1;
      if (f >= 0) {
        pick = f;
        o.path = kPathFov;
      } else {
        pick = std::min(lag, count - 1);
        o.path = kPathLag;
      }
    }
    latchedAny = true;
    latchedFrame = At(0).frame;
    o.rec = At(pick);
    o.h = fault ? 0.0 : RecH(o.rec);
    paths[o.path]++;
    return o;
  }

  // Once per DCS frame at view 1: the measured eye separation (DCS units).
  void Verify(double sep, const LatchOut& lo) {
    if (lo.stale || !lo.lagOk[0] || !(sep > 0)) return;  // a stale or dry latch describes no record
    ++verifies;
    bool allPlain = true;
    for (int L = 0; L < kLags; ++L)
      if (lo.lagOk[L] && (RecH(lo.lagRec[L]) != 0 || !(lo.lagRec[L].base > 0) ||
                          std::fabs(lo.lagRec[L].base - lo.lagRec[0].base) > 1e-6f))
        allPlain = false;
    if (allPlain) {  // nothing applied around this frame: k and the bias
      const double a = 1.0 / std::min(kN + 1, 64);
      kEst = kEst * (1 - a) + sep / lo.lagRec[0].base * a;
      ++kN;
      const double b = 1.0 / std::min(biasN + 1, 64);
      bias = bias * (1 - b) + (sep / K() - lo.lagRec[0].base) * b;
      ++biasN;
    }
    const double meas = sep / K();
    double res[kLags];
    int best = -1;
    double lo_ = 1e9, hi = -1e9;
    for (int L = 0; L < kLags; ++L) {
      res[L] = 1e9;
      if (!lo.lagOk[L]) continue;
      const double ipd = RecIpd(lo.lagRec[L]);
      res[L] = std::fabs(meas - (ipd + bias));
      lo_ = std::min(lo_, ipd);
      hi = std::max(hi, ipd);
      if (best < 0 || res[L] < res[best]) best = L;
    }
    if (best < 0) return;
    const int cur = std::min(lag, kLags - 1);
    const double rc = lo.lagOk[cur] ? res[cur] : res[best];
    residSum += rc * 1e3;
    residMax = std::max(residMax, rc * 1e3);
    ++residN;
    const bool changing = hi - lo_ > 0.3e-3;
    if (changing) {
      if (best != cur && res[best] + 0.3e-3 < rc) {
        for (int L = 0; L < kLags; ++L)
          if (L != best) better[L] = 0;
        if (++better[best] >= 3) {
          lag = best;
          ++switches;
          for (int& x : better) x = 0;
        }
      } else {
        for (int& x : better) x = 0;
      }
    }
    if (!fault) {
      bad = res[best] > 3e-3 ? bad + 1 : 0;
      if (bad >= 10) {
        fault = true;
        good = 0;
        ++faults;
      }
    } else {
      good = rc < 0.5e-3 ? good + 1 : 0;
      if (good >= 60) {
        fault = false;
        bad = 0;
      }
    }
  }
};

// ---------------------------------------------------------------------------
// Shared memory: BoostOut (read-only) and the gaze ring (fixation_depth.h's
// GazeOut mapping, when it is open)
// ---------------------------------------------------------------------------

HANDLE g_boostMap = nullptr;
std::atomic<void*> g_boostView{nullptr};
ULONGLONG g_lastBoostTry = 0;
bool g_boostMissingLogged = false;

std::wstring BoostName() {
  if (!g_boostNameOverride.empty()) return g_boostNameOverride;
  std::wstring n = psh::boost_block_name;
  char suf[64] = {};
  const DWORD k = GetEnvironmentVariableA(psh::name_suffix_variable, suf, sizeof(suf));
  if (k > 0 && k < sizeof(suf))
    for (DWORD i = 0; i < k; ++i) n += static_cast<wchar_t>(static_cast<unsigned char>(suf[i]));
  return n;
}

// Config thread: once per second while missing.
void OpenBoost(bool force = false) {
  if (g_boostView.load()) return;
  const ULONGLONG now = GetTickCount64();
  if (!force && g_lastBoostTry && now - g_lastBoostTry < 1000) return;
  g_lastBoostTry = now;
  const std::wstring name = BoostName();
  g_boostOpens++;
  HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
  if (!h) {
    if (!g_boostMissingLogged) Log("cockpit ipd: %ls not found (no Depth boost layer yet); retried once per second", name.c_str());
    g_boostMissingLogged = true;
    return;
  }
  void* v = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(psh::BoostOut));
  if (!v) {
    CloseHandle(h);
    return;
  }
  g_boostMap = h;
  g_boostView = v;
  Log("cockpit ipd: %ls opened (Depth boost layer)", name.c_str());
}

void CloseBoost() {
  if (void* v = g_boostView.exchange(nullptr)) UnmapViewOfFile(v);
  if (g_boostMap) CloseHandle(g_boostMap);
  g_boostMap = nullptr;
  g_lastBoostTry = 0;
  g_boostMissingLogged = false;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct Stats {
  std::atomic<uint64_t> frames{0}, framesPatched{0}, offStale{0}, offScale1{0}, offForeign{0}, offFault{0},
      offNext{0}, offView0{0}, offMulti{0}, offInert{0}, tooBig{0}, gbExecs{0}, ckExecs{0}, ckNotHmd{0},
      tagOther{0}, idFail{0}, gbPatched{0}, fwdPatched{0}, fwdPasses{0}, fwdNoArm{0}, uploadsSeen{0},
      patched{0}, observed{0}, restored{0}, partialBox{0}, subNz{0}, bigSrc{0}, unknownBuf{0}, selfFail{0},
      camFar{0}, failNoUpload{0}, tail{0}, stencilOk{0}, stencilMismatch{0}, stencilWorld{0}, prevOk{0},
      prevBad{0}, boostReads{0}, boostTorn{0}, gazeReads{0}, nested{0}, learned{0}, badBound{0},
      lightSplit{0}, lightSkip{0}, lightArms{0}, lightNoCopy{0}, offDry{0}, fwdFail{0}, ambiguous{0},
      restFull{0};
};
Stats g_st;
std::mutex g_traceMutex;
std::vector<std::string> g_trace;  // render thread fills (under g_traceMutex), Tick logs
double g_lastH = 0, g_lastScale = 1;
int g_lastPath = 0;

void TraceLine(const char* fmt, ...) {
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  std::lock_guard<std::mutex> lock(g_traceMutex);
  if (g_trace.size() < 6000) g_trace.emplace_back(line);
}

// ---------------------------------------------------------------------------
// Render-thread state
// ---------------------------------------------------------------------------

enum Cls : int { kClsNone = 0, kClsGb = 1, kClsFwd = 2, kClsWatch = 3, kClsLight = 4 };

struct Known {
  ID3D11Resource* res = nullptr;  // identity only
  UINT base = 0, size = 0;
  uint64_t lastUse = 0;
};
Known g_known[4];
uint64_t g_useClock = 0;

struct ViewFrame {
  int gbCk = 0;
  bool haveOrig = false, dwReady = false, patched = false, failed = false, done = false, selfFailed = false;
  double C[3] = {}, R[3][3] = {}, dw[3] = {}, dp[3] = {};
  ID3D11Resource* depthTex = nullptr;  // phase 2: the cockpit pass's depth texture (identity)
};
struct Frame {
  bool latched = false, off = false, verified = false, anyPatched = false, anyFailed = false;
  bool observe = false;  // CockpitTrueIpd=2 at the latch: the whole frame observes (a mode change applies next frame)
  double h = 0;
  int path = kPathNone;
  Matcher::LatchOut lo;
  ViewFrame v[4];
  int ckTotal = 0;
};
Frame g_f;
double g_prevDw[4][3] = {};
bool g_offNext = false;
uint64_t g_frame = 0;
Matcher g_match;
Axis g_axis;
GazeRing g_gaze;
uint64_t g_lastBoostSeq = 0, g_lastGazeSeq = 0;
bool g_inertSelf = false, g_inertAxis = false;
uint64_t g_winAttempts = 0, g_winFails = 0, g_winFrames = 0;
// DCS's previous gViewProj of each view's cockpit pass (R6 gate statistics).
M4 g_lastVP[4];
uint64_t g_lastVPFrame[4] = {};
bool g_loggedView[4] = {};
bool g_loggedAxis = false, g_loggedK = false, g_loggedLag = false, g_loggedTag = false;
int g_loggedLagValue = 0;

// The current pass (render thread).
int t_cls = kClsNone;
int t_view = -1;
void* t_pass = nullptr;  // the forward / watched / lighting pass whose end closes the class
uint16_t t_sm = 0;
uint32_t t_tag = 0;
int t_uploads = 0, t_patchedN = 0;
int t_gbDepth = 0;  // gbpass executions open (1 normally)
bool t_gbForeign = false;  // the open execution began inside another class's pass
bool t_fwdArmed = false;   // the forward cockpit pass's upload entries were armed at its begin

// DCS's latest bytes of a byte range [lo, lo + len) of a buffer that took a
// patched upload in the current pass, uploaded again at the pass end.
struct Restore {
  ID3D11Resource* res = nullptr;
  UINT sub = 0;
  bool hasBox = false, us1 = false;
  D3D11_BOX box{};
  UINT lo = 0, len = 0;
  uint8_t bytes[kMaxSrc];
};
Restore g_rest[4];
int g_restN = 0;
alignas(16) uint8_t g_scratch[kMaxSrc];

// Phase 2: the last bytes of each known struct (buffer and offset) uploaded in the lighting pass.
struct LightCopy {
  ID3D11Resource* res = nullptr;
  UINT base = 0, len = 0;
  bool valid = false;
  uint8_t bytes[kMaxSrc];
};
LightCopy g_lightCopy[2];

Known* FindKnown(ID3D11Resource* r, UINT base) {
  for (Known& k : g_known)
    if (k.res == r && r && k.base == base) return &k;
  return nullptr;
}
bool IsKnown(ID3D11Resource* r) {
  for (const Known& k : g_known)
    if (k.res == r && r) return true;
  return false;
}
UINT KnownSize(ID3D11Resource* r) {
  for (const Known& k : g_known)
    if (k.res == r && r) return k.size;
  return 0;
}

// The struct offset b7 binds `r` at right now (VS, else PS); false when b7
// does not bind `r`.
bool BoundBase(ID3D11Resource* r, UINT* base) {
  State& s = g_s;
  if (!s.ctx || !s.ctx1) return false;
  for (int stage = 0; stage < 2; ++stage) {
    ID3D11Buffer* cb = nullptr;
    UINT first = 0, num = 0;
    if (stage == 0)
      s.ctx1->VSGetConstantBuffers1(7, 1, &cb, &first, &num);
    else
      s.ctx1->PSGetConstantBuffers1(7, 1, &cb, &first, &num);
    if (!cb) continue;
    const bool same = static_cast<ID3D11Resource*>(cb) == r;
    cb->Release();
    if (!same) continue;
    *base = first == 0 && (num == 4096 || num == 0) ? 0 : first * 16;
    return true;
  }
  return false;
}

// The known struct an upload of [lo, hi) of `r` carries: the one learned
// struct of `r` the range covers, or, when several do (per-view structs at
// offsets of one buffer), the one b7 binds now; nullptr when that does not
// decide (forwarded as DCS's, counted).
Known* PickKnown(ID3D11Resource* r, UINT lo, UINT hi) {
  Known* k = nullptr;
  int n = 0;
  for (Known& c : g_known)
    if (c.res == r && r && c.base >= lo && c.base + kCbBytes <= hi) {
      k = &c;
      ++n;
    }
  if (n <= 1) return k;
  UINT b = 0;
  if (!BoundBase(r, &b)) return nullptr;
  for (Known& c : g_known)
    if (c.res == r && c.base == b && c.base >= lo && c.base + kCbBytes <= hi) return &c;
  return nullptr;
}

// Keeps the pass-end restore exact: DCS's bytes of every upload of a buffer
// that has a restore entry are merged into it, so the restore re-uploads
// DCS's latest content, never older bytes over newer ones. `bytes` are the
// upload's source, the content of [lo, hi). patched: this upload is forwarded
// patched and needs its own range restored (false: no free entry; the caller
// then forwards DCS's bytes). An entry the upload covers entirely is dropped
// (the buffer holds DCS's bytes there now) or replaced (patched).
bool NoteRestore(ID3D11Resource* r, UINT lo, UINT hi, const D3D11_BOX* box, const uint8_t* bytes, bool us1,
                 bool patched) {
  bool covered = false;
  for (int i = 0; i < g_restN;) {
    Restore& e = g_rest[i];
    if (e.res != r) {
      ++i;
      continue;
    }
    const UINT eHi = e.lo + e.len;
    if (lo <= e.lo && hi >= eHi) {  // covers the entry: drop it
      g_rest[i] = g_rest[g_restN - 1];
      --g_restN;
      continue;
    }
    const UINT a = std::max(lo, e.lo), b = std::min(hi, eHi);
    if (a < b) memcpy(e.bytes + (a - e.lo), bytes + (a - lo), b - a);
    if (e.lo <= lo && eHi >= hi) covered = true;  // the entry already restores this range (merged above)
    ++i;
  }
  if (!patched || covered) return true;
  if (g_restN >= 4 || hi - lo > kMaxSrc) return false;
  Restore& e = g_rest[g_restN++];
  e.res = r;
  e.sub = 0;
  e.hasBox = box != nullptr;
  if (box) e.box = *box;
  e.us1 = us1;
  e.lo = lo;
  e.len = hi - lo;
  memcpy(e.bytes, bytes, e.len);
  return true;
}

// An upload forwarded with DCS's own bytes: merged into the restore entries.
void NoteForwarded(ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src) {
  if (!g_restN || !src || sub != 0) return;
  bool any = false;
  for (int i = 0; i < g_restN; ++i) any = any || g_rest[i].res == r;
  if (!any) return;
  UINT lo = 0, hi = 0;
  if (box) {
    lo = box->left;
    hi = box->right;
  } else {
    hi = KnownSize(r);
    if (!hi) {  // no longer known (evicted): its size is unknown; its entries go (never older bytes over newer)
      for (int i = 0; i < g_restN;)
        if (g_rest[i].res == r)
          g_rest[i] = g_rest[--g_restN];
        else
          ++i;
      return;
    }
  }
  if (hi > lo) NoteRestore(r, lo, hi, box, static_cast<const uint8_t*>(src), false, false);
}

// Learns the cPerView bound at VS b7 (else PS b7) with its *SetConstantBuffers1 offset.
void LearnBound() {
  State& s = g_s;
  if (!s.ctx) return;
  ID3D11Buffer* cb = nullptr;
  UINT first = 0, num = 0;
  if (s.ctx1) {
    s.ctx1->VSGetConstantBuffers1(7, 1, &cb, &first, &num);
    if (!cb) s.ctx1->PSGetConstantBuffers1(7, 1, &cb, &first, &num);
  } else {
    s.ctx->VSGetConstantBuffers(7, 1, &cb);
    if (!cb) s.ctx->PSGetConstantBuffers(7, 1, &cb);
    num = 4096;
  }
  if (!cb) return;
  D3D11_BUFFER_DESC d{};
  cb->GetDesc(&d);
  ID3D11Resource* res = cb;
  cb->Release();  // identity only: DCS keeps it bound
  const bool offsets = !(first == 0 && (num == 4096 || num == 0));
  const UINT base = offsets ? first * 16 : 0;
  if (!(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || base + kCbBytes > d.ByteWidth ||
      (offsets && num * 16 < kCbBytes)) {
    g_st.badBound++;
    return;
  }
  ++g_useClock;
  for (Known& k : g_known)
    if (k.res == res && k.base == base) {
      k.lastUse = g_useClock;
      k.size = d.ByteWidth;
      return;
    }
  Known* slot = &g_known[0];
  for (Known& k : g_known)
    if (!k.res || k.lastUse < slot->lastUse) slot = &k;
  *slot = Known{res, base, d.ByteWidth, g_useClock};
  if (g_st.learned++ < 4)
    Log("cockpit ipd: cPerView buffer learned (%u bytes, struct at byte %u)", d.ByteWidth, base);
}

void ResetRender() {
  g_f = Frame{};
  memset(g_prevDw, 0, sizeof(g_prevDw));
  g_offNext = false;
  g_frame = 0;
  g_match = Matcher{};
  g_axis = Axis{};
  g_gaze = GazeRing{};
  g_lastBoostSeq = g_lastGazeSeq = 0;
  g_inertSelf = g_inertAxis = false;
  g_winAttempts = g_winFails = g_winFrames = 0;
  for (Known& k : g_known) k = Known{};
  g_useClock = 0;
  memset(g_lastVPFrame, 0, sizeof(g_lastVPFrame));
  for (bool& b : g_loggedView) b = false;
  g_loggedAxis = g_loggedK = g_loggedLag = g_loggedTag = false;
  g_loggedLagValue = 0;
  t_cls = kClsNone;
  t_view = -1;
  t_pass = nullptr;
  t_gbDepth = 0;
  t_gbForeign = false;
  t_fwdArmed = false;
  g_restN = 0;
  for (LightCopy& l : g_lightCopy) l.valid = false, l.res = nullptr, l.base = 0;
}

// Each top-level pass boundary: new BoostOut and GazeOut records.
void Poll() {
  if (void* v = g_boostView.load(std::memory_order_relaxed)) {
    const uint64_t seq = *static_cast<const volatile uint64_t*>(v);
    if (seq != g_lastBoostSeq) {
      psh::BoostOut b{};
      if (psh::seqlock_read(static_cast<const psh::BoostOut*>(v), b, 3)) {
        g_lastBoostSeq = b.seq;
        g_st.boostReads++;
        g_match.Push(FromBoost(b), g_frame);
      } else if (seq != 0) {
        g_st.boostTorn++;
      }
    }
  }
  if (void* v = g_gazeView.load(std::memory_order_relaxed)) {
    const uint64_t seq = *static_cast<const volatile uint64_t*>(v);
    if (seq != g_lastGazeSeq && !(seq & 1)) {
      GazeOut g{};
      if (SeqRead(v, g, 3)) {
        g_lastGazeSeq = g.seq;
        GazeRec r;
        r.displayTime = g.displayTime;
        for (int k = 0; k < 4; ++k) r.fov[k] = g.fovTan[0][k];
        g_gaze.Push(r);
        g_st.gazeReads++;
      }
    }
  }
}

bool InertNow(const char** why) {
  if (g_match.fault) return *why = "pairFault", true;
  if (g_match.KBad()) return *why = "k out of range", true;
  if (g_inertSelf) return *why = "self-check rate", true;
  if (g_inertAxis) return *why = "eye axis sign", true;
  return false;
}

// The frame's one latch (view 0's first cockpit upload, DCS's original bytes).
void LatchFrame(const uint8_t* s0) {
  Frame& f = g_f;
  f.latched = true;
  f.observe = Observe();
  if (ForeignTableHooks()) {
    f.off = true;
    g_st.offForeign++;
    g_offNext = false;
    return;
  }
  if (g_offNext) {
    f.off = true;
    g_st.offNext++;
    g_offNext = false;
    return;
  }
  double fov[4];
  const bool fovOk = StructFov(s0, fov);
  f.lo = g_match.Latch(g_frame, g_gaze.n ? &g_gaze : nullptr, fovOk ? fov : nullptr);
  f.path = f.lo.path;
  double h = f.lo.stale ? 0.0 : f.lo.h;
  const bool real = !f.lo.stale && RecH(f.lo.rec) != 0;
  if (f.lo.stale) (f.lo.dry ? g_st.offDry : g_st.offStale)++;
  else if (!real) g_st.offScale1++;
  const char* why = nullptr;
  if (h != 0 && InertNow(&why)) {
    h = 0;
    g_st.offInert++;
  } else if (real && g_match.fault) {
    g_st.offFault++;
  }
  if (std::fabs(h) > 40e-3) {
    if (g_st.tooBig++ == 0) Log("cockpit ipd: per-eye shift %.1f mm > 40 mm ignored", h * 1e3);
    h = 0;
  }
  if (std::fabs(h) < 0.05e-3) h = 0;
  if (g_userOff.load(std::memory_order_relaxed)) h = 0;
  const double shift = g_cfgShiftMm.load(std::memory_order_relaxed);
  if (!real && shift != 0) {
    h = shift * 1e-3;
    f.path = kPathTest;
  }
  f.h = h;
  g_lastH = h;
  g_lastPath = f.path;
  g_lastScale = real ? f.lo.rec.scale : 1.0;
}

// View t's Dw / Dp for this frame (at its first cockpit upload).
void ComputeDw(int t) {
  ViewFrame& vf = g_f.v[t];
  if (vf.dwReady) return;
  vf.dwReady = true;
  for (int i = 0; i < 3; ++i) vf.dp[i] = g_prevDw[t][i], vf.dw[i] = 0;
  if (g_f.off) {
    vf.dp[0] = vf.dp[1] = vf.dp[2] = 0;
    return;
  }
  if (g_f.h == 0) return;
  double u[3] = {vf.R[0][0], vf.R[0][1], vf.R[0][2]};
  if (!(g_axis.Learned() && g_f.v[0].haveOrig && g_axis.World(g_f.v[0].R, u))) {
    const double l = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (!(l > 0.5)) return;
    for (double& x : u) x /= l;
  }
  const double sg = (t & 1) ? -1.0 : 1.0;
  const double m = sg * g_f.h * g_match.K();
  for (int i = 0; i < 3; ++i) vf.dw[i] = m * u[i];
}

// View 1's first original upload: the measured eye separation.
void VerifyFrame() {
  Frame& f = g_f;
  f.verified = true;
  const ViewFrame& a = f.v[0];
  const ViewFrame& b = f.v[1];
  const double d[3] = {b.C[0] - a.C[0], b.C[1] - a.C[1], b.C[2] - a.C[2]};
  const double sep = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  g_axis.Add(a.R, d);
  double u[3] = {a.R[0][0], a.R[0][1], a.R[0][2]};
  if (g_axis.Learned()) {
    g_axis.World(a.R, u);
    if (!g_loggedAxis) {
      g_loggedAxis = true;
      Log("cockpit ipd: eye axis learned, %.2f deg from the camera's right axis", g_axis.AngleToRightDeg());
    }
  }
  if (sep > 1e-4 && d[0] * u[0] + d[1] * u[1] + d[2] * u[2] <= 0 && !g_inertAxis) {
    g_inertAxis = true;
    Log("cockpit ipd: the right eye is not on the +axis side of the left one; inert");
  }
  if (!f.off && !f.lo.stale && f.lo.lagOk[0]) {
    const int lagBefore = g_match.lag;
    const bool faultBefore = g_match.fault;
    g_match.Verify(sep, f.lo);
    if (g_match.lag != lagBefore) Log("cockpit ipd: frame pairing lag %d -> %d records", lagBefore, g_match.lag);
    if (g_match.fault != faultBefore)
      Log(g_match.fault ? "cockpit ipd: pairing fault (eye distance off by > 3 mm at every lag for 10 frames; DCS "
                          "overrides the eye distance?); shift 0 until 60 frames agree"
                        : "cockpit ipd: pairing agrees again");
    if (!g_loggedK && g_match.kN >= 30) {
      g_loggedK = true;
      Log("cockpit ipd: k = %.4f DCS units per metre%s", g_match.kEst,
          g_match.KBad() ? " (outside 0.95..1.05: inert)" : std::fabs(g_match.kEst - 1) > 0.01 ? " (warning: used)" : "");
    }
  }
}

// R6 gate statistics: DCS's gPrevFrameViewProj against its previous gViewProj.
void NotePrev(int t, const uint8_t* s) {
  const M4 pvp = LoadM(s, kOffPrevVP), vp = LoadM(s, kOffViewProj);
  if (g_lastVPFrame[t] && g_lastVPFrame[t] == g_frame) {  // stored as (frame + 1) by the previous frame
    double err = 0, sc = 1;
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) {
        err = std::max(err, std::fabs(pvp.a[r][c] - g_lastVP[t].a[r][c]));
        sc = std::max(sc, std::fabs(pvp.a[r][c]));
      }
    (err <= 1e-5 * sc ? g_st.prevOk : g_st.prevBad)++;
  }
  g_lastVP[t] = vp;
  g_lastVPFrame[t] = g_frame + 1;
}

// UsHook / Us1Hook (render thread, DCS's context): the pointer to forward.
const void* OnUploadImpl(ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, bool us1) {
  if (!src || t_cls == kClsNone) return src;
  if (!IsKnown(r)) {
    if (t_cls == kClsGb || t_cls == kClsFwd) g_st.unknownBuf++;
    return src;
  }
  g_st.uploadsSeen++;
  ++t_uploads;
  if (t_cls == kClsWatch) return src;
  if (sub != 0) {
    g_st.subNz++;
    return src;
  }
  if (box && (box->top != 0 || box->bottom != 1 || box->front != 0 || box->back != 1 || box->right <= box->left)) {
    g_st.partialBox++;
    return src;
  }
  Known* k = PickKnown(r, box ? box->left : 0, box ? box->right : 0xffffffffu);
  if (!k) {
    // No learned struct of this buffer inside the box, or several and b7 does not say which one.
    (box ? g_st.partialBox : g_st.ambiguous)++;
    return src;
  }
  const uint8_t* base = static_cast<const uint8_t*>(src);
  UINT len = k->size, at = k->base;
  if (box) {
    len = box->right - box->left;
    at = k->base - box->left;
  }
  if (len > kMaxSrc || at + kCbBytes > len) {
    g_st.bigSrc++;
    return src;
  }
  const uint8_t* s = base + at;
  if (t_cls == kClsLight) {  // phase 2: keep the lighting pass's bytes of this struct (buffer and offset)
    LightCopy* lc = nullptr;
    for (LightCopy& l : g_lightCopy)
      if (l.res == r && l.base == k->base) lc = &l;
    if (!lc)
      for (LightCopy& l : g_lightCopy)
        if (!l.valid) {
          lc = &l;
          break;
        }
    if (!lc) lc = &g_lightCopy[1];
    lc->res = r;
    lc->base = k->base;
    lc->len = kCbBytes;
    memcpy(lc->bytes, s, kCbBytes);
    lc->valid = true;
    return src;
  }
  const int t = t_view;
  if (t < 0 || t > 3) return src;
  Frame& f = g_f;
  ViewFrame& vf = f.v[t];
  if (!vf.haveOrig) {
    vf.haveOrig = true;
    Camera(s, vf.C, vf.R);
    if (t_cls == kClsGb) NotePrev(t, s);
  }
  if (t == 0 && !f.latched) LatchFrame(s);
  if (t == 1 && !f.verified && f.v[0].haveOrig) VerifyFrame();
  if (!f.latched || f.off) return src;
  ComputeDw(t);
  if (!NonZero(vf.dw) && !NonZero(vf.dp)) return src;
  const uint32_t chk = SelfCheck(s);
  if (chk & kChkCam) g_st.camFar++;
  ++g_winAttempts;
  if (chk & ~kChkCam) {
    ++g_winFails;
    g_st.selfFail++;
    vf.selfFailed = true;
    vf.failed = true;
    return src;
  }
  // DCS's bytes for the restore at the pass end; no free entry: not patched
  // (a patched buffer that cannot be restored would leak into later passes).
  if (!f.observe && !NoteRestore(r, box ? box->left : 0, box ? box->right : len, box, base, us1, true)) {
    ++g_winFails;
    g_st.restFull++;
    vf.failed = true;
    return src;
  }
  vf.patched = true;
  f.anyPatched = true;
  ++t_patchedN;
  if (!NonZero(vf.dw)) g_st.tail++;
  if (f.observe) {
    g_st.observed++;
    return src;
  }
  memcpy(g_scratch, base, len);
  PatchStruct(g_scratch + at, vf.dw, vf.dp);
  g_st.patched++;
  (t_cls == kClsGb ? g_st.gbPatched : g_st.fwdPatched)++;
  return g_scratch;
}

// The pointer to forward; every upload forwarded with DCS's bytes is merged
// into the pass's restore entries (the restore never writes older bytes).
const void* OnUpload(ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, bool us1) {
  const void* out = OnUploadImpl(r, sub, box, src, us1);
  if (out == src) NoteForwarded(r, sub, box, src);
  return out;
}

// UsHook / Us1Hook. Counted in g_busy before g_on is read, so Stop (which
// clears g_on, then drains g_busy) never releases or resets state under it.
// After a fault (Active() false, g_on still set) only the restore bookkeeping
// runs: the pass end, closed on the render thread, restores DCS's latest bytes.
const void* OnUploadSeh(ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, bool us1) {
  g_busy.fetch_add(1);
  const void* out = src;
  if (g_on.load()) {
    __try {
      if (Active())
        out = OnUpload(r, sub, box, src, us1);
      else
        NoteForwarded(r, sub, box, src);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_fault = true;
      out = src;
    }
  }
  g_busy.fetch_sub(1);
  return out;
}

// DCS's bytes back into every buffer that took a patched upload in this pass
// (through the context: the upload hooks forward it unchanged).
void RestoreAll() {
  State& s = g_s;
  const int n = g_restN;
  g_restN = 0;
  if (!s.ctx) return;
  for (int i = 0; i < n; ++i) {
    Restore& e = g_rest[i];
    if (e.us1 && s.ctx1)
      s.ctx1->UpdateSubresource1(e.res, e.sub, e.hasBox ? &e.box : nullptr, e.bytes, 0, 0, 0);
    else
      s.ctx->UpdateSubresource(e.res, e.sub, e.hasBox ? &e.box : nullptr, e.bytes, 0, 0);
    g_st.restored++;
  }
}

// ---------------------------------------------------------------------------
// gbpass observer: the G-buffer executions (render thread)
// ---------------------------------------------------------------------------

void GbBegin(void* pass, void* ctx) {
  if (t_gbDepth++ > 0) {  // nested (not expected): the inner execution is left alone
    g_st.nested++;
    return;
  }
  if (t_cls != kClsNone) {  // inside another class's pass (not expected): left alone
    t_gbForeign = true;
    g_st.nested++;
    return;
  }
  t_gbForeign = false;
  t_cls = kClsNone;
  t_view = -1;
  t_uploads = t_patchedN = 0;
  t_sm = 0xffff;
  t_tag = 0;
  if (!WantUploads()) return;
  g_st.gbExecs++;
  void** vec = gbpass::ItemVector(pass, ctx);
  gbrec::CollKey key;
  void* rg = nullptr;
  if (!vec || !gbrec::ExecKeyGuarded(vec, ctx, &key, &rg)) {
    g_st.idFail++;
    return;
  }
  t_sm = key.sm;
  t_tag = key.tag;
  if (key.sm != gbrec::kSmCockpit) return;
  g_st.ckExecs++;
  if (key.tag != 600) {
    g_st.tagOther++;
    if (!g_loggedTag) Log("cockpit ipd: cockpit G-buffer execution with viewport tag %u (600 expected); counted", key.tag);
    g_loggedTag = true;
  }
  const State& s = g_s;
  const int t = s.hmdOrd;
  if (s.lastHmdCount < 2 || t > 3 || t >= s.lastHmdCount) {
    g_st.ckNotHmd++;
    return;
  }
  Frame& f = g_f;
  if (f.off) return;
  ViewFrame& vf = f.v[t];
  ++f.ckTotal;
  if (++vf.gbCk > 1 || f.ckTotal > 4) {  // a second cockpit pass in this view: the rest of the frame as DCS's
    f.off = true;
    g_st.offMulti++;
    if (f.anyPatched) g_offNext = true;
    return;
  }
  if (t > 0 && !f.v[0].done) {  // view 0 was not handled: the whole frame as DCS's
    f.off = true;
    g_st.offView0++;
    return;
  }
  t_cls = kClsGb;
  t_view = t;
  if (!g_loggedView[t]) {
    g_loggedView[t] = true;
    Log("cockpit ipd: first cockpit G-buffer execution of HMD view %d (tag %u)", t, key.tag);
  }
}

void GbEnd() {
  if (t_gbDepth > 0 && --t_gbDepth > 0) return;  // a nested execution's end
  if (t_gbForeign) {
    t_gbForeign = false;
    return;
  }
  int cls = t_cls;
  const int t = t_view;
  if (cls == kClsNone && t_sm == 0xffff && !g_restN) {
    if (WantUploads()) LearnBound();
    return;
  }
  t_cls = kClsNone;  // before the restore: it goes through the upload hook unchanged
  t_view = -1;
  if (WantUploads()) LearnBound();
  int restores = g_restN;
  if (!WantUploads()) cls = kClsNone;  // after a fault: restore only (below), no frame bookkeeping
  if (cls == kClsGb && t >= 0) {
    Frame& f = g_f;
    ViewFrame& vf = f.v[t];
    const bool needed =
        !f.off && (vf.dwReady ? (NonZero(vf.dw) || NonZero(vf.dp)) : (f.h != 0 || NonZero(g_prevDw[t])));
    // Not patched, or some cPerView upload of this execution forwarded as
    // DCS's (partial box, ambiguous offset, self-check): its draws mix eyes.
    if (needed && (!vf.patched || t_patchedN < t_uploads)) {
      if (!vf.selfFailed && !vf.patched) g_st.failNoUpload++;
      vf.failed = true;
    }
    if (vf.failed) {
      f.anyFailed = true;
      if (t == 0)
        f.off = true;
      else if (f.anyPatched)
        g_offNext = true;
    } else {
      vf.done = true;
    }
    // Stencil cross-check (counted only: the state bound at the end need not be the last draw's).
    State& s = g_s;
    if (s.ctx) {
      ID3D11DepthStencilState* dss = nullptr;
      UINT ref = 0;
      s.ctx->OMGetDepthStencilState(&dss, &ref);
      D3D11_DEPTH_STENCIL_DESC d{};
      if (dss) {
        dss->GetDesc(&d);
        dss->Release();
      }
      (dss && d.StencilEnable && ref == 40 ? g_st.stencilOk : g_st.stencilMismatch)++;
    }
    vf.depthTex = PassDepthTexNow();
  } else if (t_sm != 0xffff && g_s.ctx) {
    ID3D11DepthStencilState* dss = nullptr;
    UINT ref = 0;
    g_s.ctx->OMGetDepthStencilState(&dss, &ref);
    if (dss) {
      D3D11_DEPTH_STENCIL_DESC d{};
      dss->GetDesc(&d);
      dss->Release();
      if (d.StencilEnable && ref == 40) g_st.stencilWorld++;
    }
  }
  RestoreAll();
  if (g_logLeft.load(std::memory_order_relaxed) > 0) {
    const ViewFrame* vf = t >= 0 ? &g_f.v[t] : nullptr;
    TraceLine("frame %llu view %d GBufferPass sm %u tag %u: %s, cPerView uploads %d, patched %d, Dw %.2f,%.2f,%.2f "
              "mm Dp %.2f,%.2f,%.2f mm, restores %d",
              static_cast<unsigned long long>(g_frame), t >= 0 ? t : g_s.hmdOrd, t_sm, t_tag,
              cls == kClsGb ? "cockpit" : "not patched", t_uploads, t_patchedN, vf ? vf->dw[0] * 1e3 : 0.0,
              vf ? vf->dw[1] * 1e3 : 0.0, vf ? vf->dw[2] * 1e3 : 0.0, vf ? vf->dp[0] * 1e3 : 0.0,
              vf ? vf->dp[1] * 1e3 : 0.0, vf ? vf->dp[2] * 1e3 : 0.0, restores);
  }
  t_sm = 0xffff;
}

void Observer(void* pass, void* ctx, bool begin) {
  if (GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed)) return;
  g_busy.fetch_add(1);
  __try {
    if (begin)
      GbBegin(pass, ctx);
    else
      GbEnd();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_fault = true;
    t_cls = kClsNone;
    t_gbDepth = 0;
    g_restN = 0;
  }
  g_busy.fetch_sub(1);
}

// ---------------------------------------------------------------------------
// Other passes (fixation_depth.h's boundary callback, render thread)
// ---------------------------------------------------------------------------

// A pass begins: true when its upload entries should be armed (forward
// cockpit pass of a patched view, or a watched "Cockpit" pass while tracing).
bool PassBegin(void* pass) {
  if (!WantUploads() || t_cls != kClsNone) return false;
  char n[48];
  PassName(pass, n, sizeof(n));
  if (!strstr(n, "Cockpit")) return false;
  const int t = g_s.hmdOrd;
  const bool tc = strstr(n, "TransparentCockpit") != nullptr;
  t_uploads = t_patchedN = 0;
  t_fwdArmed = false;
  if (tc && g_cfgForward.load(std::memory_order_relaxed) && !g_f.off && t >= 0 && t <= 3 && g_f.v[t].patched &&
      !g_f.v[t].failed) {
    t_cls = kClsFwd;
    t_view = t;
    t_pass = pass;
    g_st.fwdPasses++;
    return true;
  }
  if (g_logLeft.load(std::memory_order_relaxed) > 0) {
    t_cls = kClsWatch;
    t_view = t;
    t_pass = pass;
    return true;
  }
  return false;
}
void PassArmed(bool ok) {
  t_fwdArmed = ok;
  if (!ok && t_cls == kClsFwd) g_st.fwdNoArm++;
}
bool PassOwns(void* pass) { return t_pass && t_pass == pass && (t_cls == kClsFwd || t_cls == kClsWatch); }
// A pass of ours (forward, watched, lighting) is open: its end must still be
// handled after a fault.
bool PassOpen() { return t_pass && (t_cls == kClsFwd || t_cls == kClsWatch || t_cls == kClsLight); }
// armIntact: the upload entries were still ours at the end (fixation_depth.h
// checks). d3d11 also switches them after the binder's upload (seen on WARP),
// so a lost arm counts only when no cPerView upload was seen: then the
// switch may have come before the binder and its upload went past us.
void PassEnd(void* pass, bool armIntact = true) {
  const int cls = t_cls, t = t_view;
  const bool armed = t_fwdArmed && (armIntact || t_uploads > 0);
  t_cls = kClsNone;
  t_view = -1;
  t_pass = nullptr;
  t_fwdArmed = false;
  const bool active = Active();
  if (active) LearnBound();
  const int restores = g_restN;
  RestoreAll();
  // A forward cockpit pass of a view whose opaque cockpit was moved must have
  // moved every cPerView upload: not armed, the arm lost to a table switch
  // with no upload seen, or an upload forwarded as DCS's (partial box, ambiguous offset,
  // self-check, no restore entry) drew the transparent cockpit at the boosted
  // eye over the opaque one at the true eye. The pass is drawn; the next frame
  // goes unpatched and the failure counts towards the self-check rate (> 1 %:
  // inert), so a persistent cause does not alternate frames.
  bool failed = false;
  if (active && cls == kClsFwd && t >= 0 && t <= 3) {
    Frame& f = g_f;
    const ViewFrame& vf = f.v[t];
    const bool needed = !f.off && vf.dwReady && (NonZero(vf.dw) || NonZero(vf.dp));
    if (needed && (!armed || t_patchedN < t_uploads)) {
      failed = true;
      g_st.fwdFail++;
      f.anyFailed = true;
      if (f.anyPatched) g_offNext = true;
      ++g_winAttempts;
      ++g_winFails;
    }
  }
  if (g_logLeft.load(std::memory_order_relaxed) > 0) {
    char n[48];
    PassName(pass, n, sizeof(n));
    TraceLine("frame %llu view %d %s: %s, cPerView uploads %d, patched %d, restores %d%s",
              static_cast<unsigned long long>(g_frame), t, n, cls == kClsFwd ? "forward cockpit" : "watched",
              t_uploads, t_patchedN, restores, failed ? (armed ? ", FAILED (upload not patched)" : ", FAILED (not armed)") : "");
  }
}

// At fixation_depth.h's frame end.
void EndFrame() {
  Frame& f = g_f;
  const bool obs = f.observe;
  // The camera each view's cockpit was really rendered with (also in a frame
  // turned off after some views were patched); 0 when not patched.
  for (int t = 0; t < 4; ++t)
    for (int i = 0; i < 3; ++i) g_prevDw[t][i] = !obs && f.v[t].patched ? f.v[t].dw[i] : 0.0;
  if (f.anyPatched && f.anyFailed) g_offNext = true;
  g_st.frames++;
  if (f.anyPatched) g_st.framesPatched++;
  if (++g_winFrames >= 300) {
    if (g_winAttempts >= 30 && g_winFails * 100 > g_winAttempts && !g_inertSelf) {
      g_inertSelf = true;
      Log("cockpit ipd: self-checks failed on %llu of %llu patch attempts in 300 frames; inert until reload",
          static_cast<unsigned long long>(g_winFails), static_cast<unsigned long long>(g_winAttempts));
    }
    g_winFrames = g_winAttempts = g_winFails = 0;
  }
  if (g_logLeft.load(std::memory_order_relaxed) > 0) {
    TraceLine("frame %llu end: h %.3f mm (%s), scale %.3f, %s%s", static_cast<unsigned long long>(g_frame),
              f.h * 1e3, kPathNames[f.path], f.lo.any ? f.lo.rec.scale : 1.0f, f.off ? "off" : "on",
              f.anyPatched ? ", patched" : "");
    g_logLeft--;
  }
  g_f = Frame{};
  ++g_frame;
}

// ---------------------------------------------------------------------------
// Phase 2: stencil-split lighting (render thread, inside GBufferLightingPass)
// ---------------------------------------------------------------------------

struct LightRes {
  ID3D11Resource* tex = nullptr;  // identity (held by dsv)
  ID3D11DepthStencilView* dsv = nullptr;  // read-only depth and stencil
  ID3D11DepthStencilState* srcState = nullptr;  // identity of the state it was derived from (no reference)
  bool srcNull = false;
  ID3D11DepthStencilState* ne = nullptr;
  ID3D11DepthStencilState* eq = nullptr;
  ID3D11Buffer* cb = nullptr;  // private b7 (same size as DCS's)
  UINT cbSize = 0;
};
LightRes g_light[4];

void ReleaseLight() {
  for (LightRes& l : g_light) {
    if (l.dsv) l.dsv->Release();
    if (l.ne) l.ne->Release();
    if (l.eq) l.eq->Release();
    if (l.cb) l.cb->Release();
    l = LightRes{};
  }
}

// GBufferLightingPass begins: true when the draw entries (and uploads) should be armed.
bool LightBegin(void* pass) {
  if (!WantUploads() || !g_cfgLighting.load(std::memory_order_relaxed) || g_f.observe || t_cls != kClsNone)
    return false;
  const int t = g_s.hmdOrd;
  if (t < 0 || t > 3 || g_f.off || !g_f.v[t].patched || g_f.v[t].failed || !NonZero(g_f.v[t].dw) ||
      !g_f.v[t].depthTex)
    return false;
  t_cls = kClsLight;
  t_view = t;
  t_pass = pass;
  for (LightCopy& l : g_lightCopy) l.valid = false;
  g_st.lightArms++;
  return true;
}
bool LightOwns(void* pass) { return t_cls == kClsLight && t_pass == pass; }
void LightEnd() {
  t_cls = kClsNone;
  t_view = -1;
  t_pass = nullptr;
}

DXGI_FORMAT DsvFormatOf(DXGI_FORMAT tex) {
  switch (tex) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
      return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default:
      return DXGI_FORMAT_UNKNOWN;  // no stencil plane
  }
}

// The split of one draw. drawFn issues DCS's draw on the context. True when
// the draw was issued here (twice); false: the caller issues it once as DCS's.
template <class DrawFn>
bool SplitDraw(ID3D11DeviceContext* c, UINT count, UINT start, DrawFn drawFn) {
  State& s = g_s;
  const int t = t_view;
  // Callers hold g_busy (Stop releases g_light only after draining it).
  if (!Active() || t_cls != kClsLight || t < 0 || t > 3 || c != s.ctx || !s.ctx1 || !s.dev) return false;
  if (!(count == 3 || count == 4) || start != 0) return false;
  ViewFrame& vf = g_f.v[t];
  // PS b7 a known cPerView, its lighting-pass bytes captured.
  ID3D11Buffer* b7 = nullptr;
  UINT first = 0, num = 0;
  s.ctx1->PSGetConstantBuffers1(7, 1, &b7, &first, &num);
  if (!b7) return false;
  ID3D11Resource* b7r = b7;
  // The struct the lighting shader reads: b7's own offset (*SetConstantBuffers1).
  const bool psOffB = !(first == 0 && (num == 4096 || num == 0));
  const UINT psBase = psOffB ? first * 16 : 0;
  Known* k = (!psOffB || num * 16 >= kCbBytes) ? FindKnown(b7r, psBase) : nullptr;
  const LightCopy* lc = nullptr;
  for (const LightCopy& l : g_lightCopy)
    if (l.valid && l.res == b7r && l.base == psBase) lc = &l;
  if (!k || !lc) {
    b7->Release();
    g_st.lightNoCopy++;
    return false;
  }
  // The depth texture: single-sample, with a stencil plane; RTV 0 of its size.
  ID3D11Texture2D* dt = nullptr;
  D3D11_TEXTURE2D_DESC td{};
  if (FAILED(vf.depthTex->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&dt))) || !dt) {
    b7->Release();
    g_st.lightSkip++;
    return false;
  }
  dt->GetDesc(&td);
  dt->Release();
  ID3D11RenderTargetView* rtv[8] = {};
  ID3D11DepthStencilView* dsv0 = nullptr;
  c->OMGetRenderTargets(8, rtv, &dsv0);
  UINT nrtv = 0;
  for (UINT i = 0; i < 8; ++i)
    if (rtv[i]) nrtv = i + 1;
  bool ok = td.SampleDesc.Count == 1 && DsvFormatOf(td.Format) != DXGI_FORMAT_UNKNOWN && rtv[0] && td.ArraySize == 1;
  if (ok) {
    ID3D11Resource* rr = nullptr;
    rtv[0]->GetResource(&rr);
    ID3D11Texture2D* rt = nullptr;
    if (rr && SUCCEEDED(rr->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&rt))) && rt) {
      D3D11_TEXTURE2D_DESC rd{};
      rt->GetDesc(&rd);
      rt->Release();
      ok = rd.Width == td.Width && rd.Height == td.Height && rd.SampleDesc.Count == 1;
    } else {
      ok = false;
    }
    if (rr) rr->Release();
  }
  // A PS SRV views the view's depth texture (the lighting reads the G-buffer depth).
  if (ok) {
    ID3D11ShaderResourceView* srv[16] = {};
    c->PSGetShaderResources(0, 16, srv);
    bool reads = false;
    for (ID3D11ShaderResourceView* v : srv) {
      if (!v) continue;
      ID3D11Resource* r = nullptr;
      v->GetResource(&r);
      if (r == vf.depthTex) reads = true;
      if (r) r->Release();
      v->Release();
    }
    ok = reads;
  }
  // The bound depth-stencil state: no stencil use, no depth write; a depth
  // test only against this very depth texture.
  ID3D11DepthStencilState* dss0 = nullptr;
  UINT ref0 = 0;
  c->OMGetDepthStencilState(&dss0, &ref0);
  D3D11_DEPTH_STENCIL_DESC dd{};
  if (dss0)
    dss0->GetDesc(&dd);
  else
    dd = CD3D11_DEPTH_STENCIL_DESC(D3D11_DEFAULT);
  if (ok && (dd.StencilEnable || (dd.DepthEnable && dd.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO))) ok = false;
  if (ok && dd.DepthEnable) {
    ID3D11Resource* dr = nullptr;
    if (dsv0) dsv0->GetResource(&dr);
    ok = dr == vf.depthTex;
    if (dr) dr->Release();
  }
  LightRes& L = g_light[t];
  if (ok && (L.tex != vf.depthTex || !L.dsv)) {
    if (L.dsv) L.dsv->Release();
    L.dsv = nullptr;
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
    vd.Format = DsvFormatOf(td.Format);
    vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    vd.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
    ok = SUCCEEDED(s.dev->CreateDepthStencilView(vf.depthTex, &vd, &L.dsv)) && L.dsv;
    L.tex = ok ? vf.depthTex : nullptr;
  }
  if (ok && (!L.ne || L.srcState != dss0 || L.srcNull != (dss0 == nullptr))) {
    if (L.ne) L.ne->Release();
    if (L.eq) L.eq->Release();
    L.ne = L.eq = nullptr;
    D3D11_DEPTH_STENCIL_DESC d = dd;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.StencilEnable = TRUE;
    d.StencilReadMask = 0x38;
    d.StencilWriteMask = 0;
    d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_NOT_EQUAL};
    d.BackFace = d.FrontFace;
    ok = SUCCEEDED(s.dev->CreateDepthStencilState(&d, &L.ne));
    d.FrontFace.StencilFunc = d.BackFace.StencilFunc = D3D11_COMPARISON_EQUAL;
    ok = ok && SUCCEEDED(s.dev->CreateDepthStencilState(&d, &L.eq));
    L.srcState = dss0;
    L.srcNull = dss0 == nullptr;
  }
  D3D11_BUFFER_DESC bd{};
  b7->GetDesc(&bd);
  if (ok && (!L.cb || L.cbSize != bd.ByteWidth)) {
    if (L.cb) L.cb->Release();
    L.cb = nullptr;
    D3D11_BUFFER_DESC nd{};
    nd.ByteWidth = bd.ByteWidth;
    nd.Usage = D3D11_USAGE_DEFAULT;
    nd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ok = SUCCEEDED(s.dev->CreateBuffer(&nd, nullptr, &L.cb)) && L.cb;
    L.cbSize = ok ? bd.ByteWidth : 0;
  }
  if (ok && (k->base + kCbBytes > L.cbSize || L.cbSize > kMaxSrc)) ok = false;
  if (ok) {
    // The private b7: the lighting pass's bytes, patched with this frame's Dw / Dp.
    memset(g_scratch, 0, L.cbSize);
    memcpy(g_scratch + k->base, lc->bytes, kCbBytes);
    PatchStruct(g_scratch + k->base, vf.dw, vf.dp);
    const int cls = t_cls;
    t_cls = kClsNone;  // our own upload passes the hook unchanged
    c->UpdateSubresource(L.cb, 0, nullptr, g_scratch, 0, 0);
    t_cls = cls;
    ID3D11Buffer* vs7 = nullptr;
    UINT vsFirst = 0, vsNum = 0;
    s.ctx1->VSGetConstantBuffers1(7, 1, &vs7, &vsFirst, &vsNum);
    const bool psOff = !(first == 0 && (num == 4096 || num == 0));
    const bool vsOff = !(vsFirst == 0 && (vsNum == 4096 || vsNum == 0));
    // Draw 1: world pixels, DCS's b7.
    c->OMSetRenderTargetsAndUnorderedAccessViews(nrtv, rtv, L.dsv, nrtv, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr,
                                                nullptr);
    c->OMSetDepthStencilState(L.ne, 0x28);
    drawFn();
    // Draw 2: cockpit pixels, the private b7 at the same offset.
    if (psOff)
      s.ctx1->PSSetConstantBuffers1(7, 1, &L.cb, &first, &num);
    else
      c->PSSetConstantBuffers(7, 1, &L.cb);
    if (vs7 == b7) {
      if (vsOff)
        s.ctx1->VSSetConstantBuffers1(7, 1, &L.cb, &vsFirst, &vsNum);
      else
        c->VSSetConstantBuffers(7, 1, &L.cb);
    }
    c->OMSetDepthStencilState(L.eq, 0x28);
    drawFn();
    // Everything back exactly.
    if (psOff)
      s.ctx1->PSSetConstantBuffers1(7, 1, &b7, &first, &num);
    else
      c->PSSetConstantBuffers(7, 1, &b7);
    if (vs7 == b7) {
      if (vsOff)
        s.ctx1->VSSetConstantBuffers1(7, 1, &vs7, &vsFirst, &vsNum);
      else
        c->VSSetConstantBuffers(7, 1, &vs7);
    }
    c->OMSetRenderTargetsAndUnorderedAccessViews(nrtv, rtv, dsv0, nrtv, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr,
                                                nullptr);
    c->OMSetDepthStencilState(dss0, ref0);
    if (vs7) vs7->Release();
    g_st.lightSplit++;
  } else {
    g_st.lightSkip++;
  }
  for (ID3D11RenderTargetView* r : rtv)
    if (r) r->Release();
  if (dsv0) dsv0->Release();
  if (dss0) dss0->Release();
  b7->Release();
  return ok;
}

// ---------------------------------------------------------------------------
// Control (config thread)
// ---------------------------------------------------------------------------

bool g_refusedLogged = false, g_mismatchLogged = false, g_faultLoggedCk = false;
ULONGLONG g_lastStats = 0;

// From main.cpp each second (before fixdepth::Tick).
void Configure(int mode, bool lighting, bool forward, int logFrames, double shiftMm, bool foreignConfigured) {
  g_cfgMode = mode == 1 || mode == 2 ? mode : 0;
  g_cfgLighting = lighting;
  g_cfgForward = forward;
  g_cfgShiftMm = std::clamp(shiftMm, -40.0, 40.0);
  g_cfgForeign = foreignConfigured;
  if (mode && foreignConfigured && !g_refusedLogged) {
    g_refusedLogged = true;
    Log("cockpit ipd: refused: GPU pass timing, GPU dedup or the D3D meter is configured (their context-table hooks "
        "would chain with ours)");
  }
  if (!mode || !foreignConfigured) g_refusedLogged = false;
  static int lastLog = 0;
  if (logFrames != lastLog) {
    lastLog = logFrames;
    g_logLeft = std::clamp(logFrames, 0, 600);
  }
}

void ObserverOut() {
  gbpass::ObsFn me = &Observer;
  gbpass::g_obs.compare_exchange_strong(me, nullptr);
  for (int i = 0; i < 200 && g_busy.load(); ++i) Sleep(5);
}

// Starts the render-thread paths (fixation_depth.h's Install, or Tick retries).
bool Start() {
  if (g_on.load()) return true;
  if (!Wanted() || g_fault.load()) return false;
  if (gbpass::g_state == 0) {
    g_gbInstallCalls++;
    gbpass::Install();
  }
  if (gbpass::g_state < 0) {
    if (!g_mismatchLogged) Log("cockpit ipd: build mismatch (G-buffer pass execute not hooked); off");
    g_mismatchLogged = true;
    return false;
  }
  if (gbpass::g_state == 0) return false;  // GraphicsCore not loaded yet: retried
  ResetRender();
  OpenBoost(true);
  gbpass::g_obs = &Observer;
  g_on = true;
  g_starts++;
  Log("cockpit ipd: on (%s; forward cockpit passes %s, lighting split %s)",
      Observe() ? "observe: nothing modified" : "cockpit passes at the true eye distance",
      g_cfgForward.load() ? "on" : "off", g_cfgLighting.load() ? "on" : "off");
  return true;
}

// fixation_depth.h's Uninstall, after its boundary callback is drained.
// Clearing g_on, then draining g_busy (the observer, the upload and the
// split-draw hooks count themselves in it before they read g_on), makes the
// render thread leave every object below alone before it is released or reset.
void Stop() {
  const bool was = g_on.exchange(false);
  ObserverOut();
  CloseBoost();
  ReleaseLight();
  t_cls = kClsNone;
  g_restN = 0;
  g_faultLoggedCk = false;
  if (was) Log("cockpit ipd: off");
}

void LogStats() {
  auto u = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load()); };
  const Matcher& m = g_match;
  Log("cockpit ipd: frames %llu patched %llu off(stale %llu scale1 %llu foreign %llu fault %llu inert %llu next %llu "
      "view0 %llu multi %llu big %llu) scale %.3f h %.2fmm k %.4f lag %d match same/fov/lag/test %llu/%llu/%llu/%llu "
      "resid %.2f/%.2fmm bias %.2fmm",
      u(g_st.frames), u(g_st.framesPatched), u(g_st.offStale), u(g_st.offScale1), u(g_st.offForeign),
      u(g_st.offFault), u(g_st.offInert), u(g_st.offNext), u(g_st.offView0), u(g_st.offMulti), u(g_st.tooBig),
      g_lastScale, g_lastH * 1e3, m.kEst, m.lag, static_cast<unsigned long long>(m.paths[kPathSame]),
      static_cast<unsigned long long>(m.paths[kPathFov]), static_cast<unsigned long long>(m.paths[kPathLag]),
      static_cast<unsigned long long>(m.paths[kPathTest]), m.residN ? m.residSum / m.residN : 0.0, m.residMax,
      m.bias * 1e3);
  Log("cockpit ipd: G-buffer execs %llu (cockpit %llu, not HMD %llu, other tag %llu, no identity %llu) passes "
      "gb/fwd %llu/%llu (forward passes %llu, not armed %llu) uploads %llu patched %llu observed %llu tail %llu "
      "restored %llu; skipped: partialBox %llu sub %llu big %llu unknown %llu selfchk %llu (camera far %llu) no "
      "upload %llu ambiguous %llu restFull %llu fwdFail %llu; stencil ok %llu mismatch %llu world40 %llu; prev VP ok "
      "%llu bad %llu; boost reads %llu torn %llu dry %llu, gaze %llu; lightSplit %llu lightSkip %llu (no copy %llu) "
      "arms %llu",
      u(g_st.gbExecs), u(g_st.ckExecs), u(g_st.ckNotHmd), u(g_st.tagOther), u(g_st.idFail), u(g_st.gbPatched),
      u(g_st.fwdPatched), u(g_st.fwdPasses), u(g_st.fwdNoArm), u(g_st.uploadsSeen), u(g_st.patched),
      u(g_st.observed), u(g_st.tail), u(g_st.restored), u(g_st.partialBox), u(g_st.subNz), u(g_st.bigSrc),
      u(g_st.unknownBuf), u(g_st.selfFail), u(g_st.camFar), u(g_st.failNoUpload), u(g_st.ambiguous),
      u(g_st.restFull), u(g_st.fwdFail), u(g_st.stencilOk), u(g_st.stencilMismatch), u(g_st.stencilWorld),
      u(g_st.prevOk), u(g_st.prevBad), u(g_st.boostReads), u(g_st.boostTorn), u(g_st.offDry), u(g_st.gazeReads),
      u(g_st.lightSplit), u(g_st.lightSkip), u(g_st.lightNoCopy),
      u(g_st.lightArms));
}

// fixation_depth.h's Tick, each second while the option is wanted.
void TickCfg() {
  if (!g_on.load()) Start();
  // A fault (ours, or fixation_depth.h's) makes Active() false: nothing is
  // patched, split or classified any more. g_on stays set (and the gbpass
  // observer in) so the render thread still closes a pass that was open at the
  // fault (restore, disarm); Stop clears it at the uninstall.
  if ((g_fault.load() || ::fixdepth::g_fault.load()) && g_on.load() && !g_faultLoggedCk) {
    g_faultLoggedCk = true;
    Log(g_fault.load() ? "cockpit ipd: fault latched; off until the payload reloads"
                       : "cockpit ipd: off (fixation depth fault latched) until the payload reloads");
  }
  if (g_on.load() && Active()) OpenBoost();
  std::vector<std::string> tl;
  {
    std::lock_guard<std::mutex> lock(g_traceMutex);
    tl.swap(g_trace);
  }
  for (const std::string& l : tl) Log("cockpit ipd trace: %s", l.c_str());
  const ULONGLONG now = GetTickCount64();
  if (g_on.load() && now - g_lastStats >= 10000) {
    if (g_lastStats) LogStats();
    g_lastStats = now;
  }
}

}  // namespace ckipd
