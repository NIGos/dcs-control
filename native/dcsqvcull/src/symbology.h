// R95: the HUD symbology emission export ([Perception] SymbologyExport=0 default, SymbologyAlpha=1; contract
// dcsvr_symbology.h, Local\DcsVrSymbology_v1; design docs/research/R89_hud_layer_fg.md §6, R94_hud_trace.md; the
// writer: docs/research/R95_symbology_export.md). A companion of the cockpit mask (the same seq, slot index, fence,
// generation, held set; its block and heartbeat in symbology_block.h).
//
// Per HMD view v (0..3), on DCS's render thread:
//  1. OverlayHDRPass begin (PassBegin): the view's capacity WINDOW is chosen on the CPU, before anything is drawn:
//     the collimated symbology's learnt direction rectangle in the tracking (cockpit) space (azimuth / elevation of
//     the HUD's field), projected with this frame's located pose and FOV (cockpit_time.h's xrLocateViews ring: the
//     record with the smallest displayTime after the last committed one), united with the helmet symbology's learnt
//     view-pixel extent; a margin of 10 % + 4 px (+ what symbols at the window edge added); larger than the capacity
//     (periphery 512, focus 1024) -> overflow (present 0). Nothing learnt yet, symbols at the window edge last time,
//     or every 64 frames: a SCAN of the whole view (8 x 8-tile signatures) learns where the symbology is.
//  2. The cockpit record replay (ovl_cpu.h's render point, record byte +0xc9 = DCS's indicator type, before DCS draws
//     the record): at most 3 contiguous type runs per view (R94). A COLLIMATOR (1) or HELMET (2) run's start copies the
//     window of DCS's HDR target (RunStart: `pre`); its end (the next type, the replay end) adds what the run changed
//     to the fp32 accumulator (RunEnd, one dispatch): acc = sum over the 1 / 2 runs of (HDR after - HDR before), and
//     writes the class atlas (1 / 2 where the run changed the texel, else 0), the per-type bounding boxes, the changed
//     texels at the window's inner edges and the coverage-alpha check into the statistics buffer. COMMON (0) runs
//     (MFD / DED quads) are never accumulated: they stay in both tone-map evaluations and cancel exactly.
//  3. ToneMappingPass's draw (color_pass.h, the draw we own: [Color] Mode 1 / 2, or the stock replica psFxStock in
//     Mode 0): the same pixel shader evaluates the tone map twice per window texel with acc != 0: on the HDR as DCS
//     tone-maps it (post, what it returns: DCS's bytes F) and on post - acc (C); it writes E = dec(q8(T(post))) -
//     dec(q8(T(post - acc))) (sRGB-decoded linear light, the swapchain's bytes) and the coverage alpha into the
//     emission atlas (a PS UAV). Glare, night-vision low-pass and GazeDof's gather read the same auxiliaries in both
//     evaluations (they are computed from post), so dec(F) - E == dec(C) holds to the byte for every mode.
//  4. The xrEndFrame commit (Commit, before the mask header): the slot (seq = maskSeq), the views' windows and flags,
//     the per-eye focus layout from the submitted (or the pupil shift layer's original) FOVs, Quad Views' settings.
//  5. The frame end (FrameEnd): the statistics copied to a staging ring and read back >= 2 frames later (never waiting)
//     to learn the rectangle / extent, the alpha validity per view and the margin.
// Cost (R95 doc §6): copies and dispatches over the windows only; the second evaluation only where acc != 0.
// R97 (docs/research/R97_symbology_fixes.md, after R96's live run): the field projected with view-frustum clipping and
// learnt from the GPU's tracking-space angles of the type 1 groups; the helmet's extent decays (6 x 32 frames); the
// HUD-only fallback (present, overflow clear; the helmet stays in the image) when the union overflows; alpha_valid only
// from complete still read-backs, on after 256 valid ones, latched off for the generation at the first invalid one
// (opaque cores required); timestamps on every 8th frame, constants written on change, the window / learning cached,
// the finalize gated by the class atlas, atomics test-and-max; the cost breakdown line; [Dev] SymbologyPublishAlways.
// R101 (docs/research/R101_symbology_window_decay.md, after R100 �5.2: the F-16's focus views overflowed from the first
// roll weave on, for the rest of the session): the collimated field ages (each side the extreme of its recent
// observations, aged only by frames that could see it); a recent-field fallback with a margin clamped to the capacity
// when even the long field's HUD alone overflows; faster scans in the fallback and in overflow.
// Included from cockpit_mask.h inside namespace fixdepth::cmask (after cockpit_overlay.h: covl::RunOv).
#pragma once

namespace sym {

constexpr int kViews = 4;
constexpr uint32_t kTile = 8;          // scan tile (output px)
constexpr uint32_t kScanTiles = 512;   // the scan signature texture: 512 x 512 tiles (views up to 4096 px)
constexpr int kStatU = 32;             // statistics words per view
constexpr int kRing = 4;               // statistics / timing frames in flight
constexpr int kTsOps = 24;             // timestamped GPU operations per frame
constexpr uint64_t kScanEvery = 64;    // frames between a view's periodic scans
constexpr uint64_t kScanRetry = 4;     // frames between scans while nothing is learnt
constexpr int kMaxExtra = 64;          // px: the most symbols at the window edge add to the margin
// R97: the helmet's view-pixel extent decays: the union of its observations in the last kHelBuckets buckets of
// kHelBucket frames (6 x 32 = 192 frames, ~4 s at 45 fps; at least two of the view's 64-frame scans). R95 kept a union
// until 900 frames unseen, which grew under head turns (the JHMCS's world-referenced cues) and never shrank.
constexpr uint64_t kHelBucket = 32;
constexpr int kHelBuckets = 6;
// R97: the HUD-only fallback (the helmet left out of E and the class plane, so the window needs only the collimated
// field) starts at the first frame the union overflows and the HUD alone fits; it ends after kHelmetBack frames in a
// row in which the union fits.
constexpr int kHelmetBack = 45;
// R101: the collimated field ages. R97 kept it as a union for the session. The HUD's frame is fixed in the cockpit, but
// what it draws is not: the horizon line and the pitch ladder rotate against the cockpit with the bank, so a banked
// frame lights parts of the HUD a level frame leaves dark, and a weave (left and right bank in turn) adds both sides:
// the union grew past the focus capacity and stayed there (R100: v3 818 -> 1014 px within 0.25 s at 25-31 deg of bank,
// overflow from the next peak to the end of the session, wings level or not). The HUD's footprint does not roll in the
// view (the head and the HUD are both fixed in the tracking space); its content does, so no roll-invariant form helps:
// a disc around the rotating symbols is larger than any box of them. Each side of the field (azimuth min / max,
// elevation min / max) is instead the extreme of its observations in a ring of buckets:
//  - LONG (the window's field, as R97's: it grows at once): kColBuckets buckets of kColBucket frames (~8.5 s: longer
//    than R100's 8 s weave period, so a symbol that comes back once a cycle stays in the window);
//  - RECENT (used only when even the long field's HUD alone overflows): kRecBuckets of kRecBucket frames (~0.7 s).
// A frame moves a side's ring to a new bucket only when it could see that side: the side's visible part (its outline
// samples inside the view) inside the rectangle the frame observed (the window for the accumulation, the view for a
// scan), and at least one sample visible. A HUD out of view, behind the window's edge or outside a view's frustum keeps
// its field (no shrinking from what was not looked at: R97's yaw replay keeps 0 misses).
constexpr uint64_t kColBucket = 32;
constexpr int kColBuckets = 12;
constexpr uint64_t kRecBucket = 8;
constexpr int kRecBuckets = 4;
// R101: the recent-field fallback starts at the first frame the long field overflows even with the HUD alone while the
// recent field fits (the helmet left out, as R97's HUD-only); it ends after kRecentBack frames in a row in which the long
// field's HUD fits. Its window's margin shrinks to fit the capacity, down to kMinMargin px; the rest of the capacity
// covers as much of the long field as fits. A view in the fallback or in overflow scans every kScanFallback frames
// (the long field ages from the scans: its sides are outside the fallback's window).
constexpr int kRecentBack = 45;
constexpr uint64_t kScanFallback = 8;
constexpr int kMinMargin = 4;
// Statistics words of a view (all InterlockedMax / Add on zero-cleared words; a box's minimum is stored as ~x).
// R97: kStAlphaCore counts the type 1 / 2 texels whose added alpha is >= 0.75 (an opaque core: a coverage alpha has
// them); kStAng1 / kStScanAng1: the tracking-space azimuth / elevation box (EncAng) of the type 1 run's changed texels /
// of the changed tiles of a type 1 scan (the collimated field is learnt from them, not from the pixel box).
enum : int {
  kStBox1 = 0, kStBox2 = 4, kStEdge = 8, kStChanged = 9, kStAlphaBad = 10, kStAlphaCore = 11, kStScan1 = 12,
  kStScan2 = 16, kStAng1 = 20, kStScanAng1 = 24
};
// The pixel shader's slots (color_pass.h binds them around the tone-map draw).
constexpr UINT kPsCbSlot = 10, kPsSrvSlot = 25, kPsUavSlot = 1;

std::atomic<uint32_t> g_capPeri{csy::default_capacity_periphery}, g_capFocus{csy::default_capacity_focus};  // tests
std::atomic<uint32_t> g_resetGen{0};  // mission events (main.cpp): the learnt fields belong to the previous aircraft
inline void NoteMissionEvent() { g_resetGen.fetch_add(1, std::memory_order_acq_rel); }

// ---------------------------------------------------------------------------
// Pure helpers (offline tested)
// ---------------------------------------------------------------------------

// The atlas packing (contract: the periphery windows stacked in one column, the focus windows beside it).
struct Pack {
  uint32_t x[4] = {}, y[4] = {}, cap[4] = {}, W = 0, H = 0;
};
inline Pack MakePack(uint32_t cp, uint32_t cf) {
  Pack p;
  p.x[0] = 0, p.y[0] = 0, p.x[1] = 0, p.y[1] = cp, p.x[2] = cp, p.y[2] = 0, p.x[3] = cp + cf, p.y[3] = 0;
  p.cap[0] = p.cap[1] = cp, p.cap[2] = p.cap[3] = cf;
  p.W = cp + 2 * cf;
  p.H = std::max(2 * cp, cf);
  return p;
}

// A box in view pixels, exclusive ends.
struct Box {
  bool ok = false;
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
inline Box Union(const Box& a, const Box& b) {
  if (!a.ok) return b;
  if (!b.ok) return a;
  return {true, std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}
inline Box Clip(const Box& a, int w, int h) {
  if (!a.ok) return a;
  Box c{true, std::max(a.x0, 0), std::max(a.y0, 0), std::min(a.x1, w), std::min(a.y1, h)};
  if (c.x1 <= c.x0 || c.y1 <= c.y0) return Box{};
  return c;
}
// The statistics' encoding: words[0] = max(~x0), [1] = max(~y0), [2] = max(x1), [3] = max(y1); 0 = empty.
inline Box DecodeBox(const uint32_t* w) {
  if (!w[2] || !w[3]) return Box{};
  return {true, static_cast<int>(0xffffffffu - w[0]), static_cast<int>(0xffffffffu - w[1]), static_cast<int>(w[2]),
          static_cast<int>(w[3])};
}
// R97: an angle (radians, |a| < 4) as the statistics store it: (a + 4) * 2^29 (never 0), the minimum as ~x.
constexpr double kAngScale = 536870912.0;
inline uint32_t EncAng(double a) { return static_cast<uint32_t>((a + 4.0) * kAngScale); }
inline double DecAng(uint32_t u) { return u / kAngScale - 4.0; }
// A tracking-space azimuth / elevation box (radians, as AzEl): words max(~az0), max(~el0), max(az1), max(el1).
struct AngBox {
  bool ok = false;
  double az0 = 0, el0 = 0, az1 = 0, el1 = 0;
};
inline AngBox DecodeAng(const uint32_t* w) {
  if (!w[2] || !w[3]) return AngBox{};
  AngBox b;
  b.az0 = DecAng(0xffffffffu - w[0]), b.el0 = DecAng(0xffffffffu - w[1]), b.az1 = DecAng(w[2]), b.el1 = DecAng(w[3]);
  b.ok = b.az1 >= b.az0 && b.el1 >= b.el0 && b.az0 > -3.2 && b.az1 < 3.2 && b.el0 > -1.6 && b.el1 < 1.6;
  return b;
}

// A view's pose: XR orientation (x, y, z, w: view -> tracking space) and its FOV tangents (left, right, up, down).
struct Pose {
  bool ok = false;
  float q[4] = {0, 0, 0, 1};
  float tan[4] = {};
};
inline Pose PoseOf(const float q[4], const float fovAngles[4]) {
  Pose p;
  memcpy(p.q, q, sizeof(p.q));
  for (int k = 0; k < 4; ++k) p.tan[k] = static_cast<float>(std::tan(static_cast<double>(fovAngles[k])));
  p.ok = p.tan[1] > p.tan[0] && p.tan[2] > p.tan[3] && std::isfinite(p.tan[0]) && std::isfinite(p.tan[1]) &&
         std::isfinite(p.tan[2]) && std::isfinite(p.tan[3]);
  const double n = std::sqrt(double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2] + double(q[3]) * q[3]);
  if (!(n > 0.5 && n < 1.5)) p.ok = false;
  return p;
}
// v rotated by q (inverse: by its conjugate).
inline void QRot(const float qf[4], const double v[3], double o[3], bool inverse) {
  double q[4] = {qf[0], qf[1], qf[2], qf[3]};
  const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  for (double& c : q) c /= n > 0 ? n : 1;
  if (inverse) q[0] = -q[0], q[1] = -q[1], q[2] = -q[2];
  const double t[3] = {2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0])};
  o[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
  o[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
  o[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}
// The tracking-space direction of view pixel (px, py) (pixel units: 0..outW, 0..outH; v down).
inline bool PixelDir(const Pose& p, double outW, double outH, double px, double py, double d[3]) {
  if (!p.ok || !(outW > 0) || !(outH > 0)) return false;
  const double tx = p.tan[0] + px / outW * (double(p.tan[1]) - p.tan[0]);
  const double ty = p.tan[2] - py / outH * (double(p.tan[2]) - p.tan[3]);
  const double n = std::sqrt(tx * tx + ty * ty + 1);
  const double c[3] = {tx / n, ty / n, -1 / n};
  QRot(p.q, c, d, false);
  return true;
}
// The view pixel of a tracking-space direction; false behind the view plane.
inline bool DirPixel(const Pose& p, double outW, double outH, const double d[3], double& px, double& py) {
  if (!p.ok) return false;
  double c[3];
  QRot(p.q, d, c, true);
  if (!(c[2] < -1e-4)) return false;
  const double tx = c[0] / -c[2], ty = c[1] / -c[2];
  px = (tx - p.tan[0]) / (double(p.tan[1]) - p.tan[0]) * outW;
  py = (p.tan[2] - ty) / (double(p.tan[2]) - p.tan[3]) * outH;
  return std::isfinite(px) && std::isfinite(py);
}
inline void AzEl(const double d[3], double& az, double& el) {
  az = std::atan2(d[0], -d[2]);
  el = std::atan2(d[1], std::sqrt(d[0] * d[0] + d[2] * d[2]));
}
inline void DirOf(double az, double el, double d[3]) {
  d[0] = std::cos(el) * std::sin(az);
  d[1] = std::sin(el);
  d[2] = -std::cos(el) * std::cos(az);
}

// The collimated symbology's field: an azimuth / elevation rectangle in the tracking space (radians), the union of
// the observed boxes (each converted with its own frame's pose).
struct Cone {
  bool ok = false;
  double az0 = 0, az1 = 0, el0 = 0, el1 = 0;
  uint64_t seen = 0;
};
constexpr int kEdgeSamples = 6;  // points per box edge (the gnomonic edges are curves in azimuth / elevation)
// Adds box b (view pixels) seen with pose p; false when it cannot be converted (behind, wrapping past +-170 deg).
inline bool LearnCone(Cone& c, const Pose& p, double outW, double outH, const Box& b) {
  if (!b.ok || !p.ok) return false;
  double a0 = 1e9, a1 = -1e9, e0 = 1e9, e1 = -1e9;
  for (int side = 0; side < 4; ++side)
    for (int i = 0; i <= kEdgeSamples; ++i) {
      const double t = double(i) / kEdgeSamples;
      const double x = side == 0 ? b.x0 + t * (b.x1 - b.x0) : side == 1 ? b.x1 : side == 2 ? b.x1 - t * (b.x1 - b.x0) : b.x0;
      const double y = side == 0 ? b.y0 : side == 1 ? b.y0 + t * (b.y1 - b.y0) : side == 2 ? b.y1 : b.y1 - t * (b.y1 - b.y0);
      double d[3], az, el;
      if (!PixelDir(p, outW, outH, x, y, d)) return false;
      AzEl(d, az, el);
      if (std::fabs(az) > 2.97) return false;  // the HUD is ahead: a field across +-170 deg is not one
      a0 = std::min(a0, az), a1 = std::max(a1, az), e0 = std::min(e0, el), e1 = std::max(e1, el);
    }
  if (!c.ok) {
    c.ok = true;
    c.az0 = a0, c.az1 = a1, c.el0 = e0, c.el1 = e1;
  } else {
    c.az0 = std::min(c.az0, a0), c.az1 = std::max(c.az1, a1), c.el0 = std::min(c.el0, e0), c.el1 = std::max(c.el1, e1);
  }
  return true;
}
// R97: adds a tracking-space azimuth / elevation box (the GPU's statistics of the type 1 texels: each 8 x 8 group's
// changed-texel box, its corners as directions through the frame's view rotation). The pixel box of R95 (LearnCone)
// loses the field's shape: near a wide view's edge the projected HUD is a curved quadrilateral, its pixel box converted
// back covers ~50 % more elevation, and the union grew with every head turn (R96 §2.3: periphery windows 154 -> 217
// after the yaw). The groups' angles are exact to a group's curvature (< 0.1 px), at any head orientation.
inline bool LearnConeAng(Cone& c, const AngBox& b) {
  if (!b.ok || std::fabs(b.az0) > 2.97 || std::fabs(b.az1) > 2.97) return false;  // the HUD is ahead (as LearnCone)
  if (!c.ok) {
    c.ok = true;
    c.az0 = b.az0, c.az1 = b.az1, c.el0 = b.el0, c.el1 = b.el1;
  } else {
    c.az0 = std::min(c.az0, b.az0), c.az1 = std::max(c.az1, b.az1), c.el0 = std::min(c.el0, b.el0), c.el1 = std::max(c.el1, b.el1);
  }
  return true;
}
// R101: a box in view pixels seen with pose p, as a tracking-space angle box (R95's pixel-box path, LearnCone's
// conversion); not ok when it cannot be converted.
inline AngBox BoxAng(const Pose& p, double outW, double outH, const Box& b) {
  AngBox o;
  Cone c;
  if (!LearnCone(c, p, outW, outH, b)) return o;
  o.ok = true, o.az0 = c.az0, o.az1 = c.az1, o.el0 = c.el0, o.el1 = c.el1;
  return o;
}
// R101: an aging field. Side s (0 az0, 1 az1, 2 el0, 3 el1) keeps one value per bucket in a ring; bit s of `obs` says
// the observing frame could see side s (see kColBucket). An observation in a later bucket that can see the side starts
// the next slot (the oldest is dropped); any other one is merged into the current slot (min / max). So a side shrinks
// only after n buckets of frames that saw it, and grows at once.
constexpr int kAgeMax = kColBuckets > kRecBuckets ? kColBuckets : kRecBuckets;
struct AgedCone {
  double v[4][kAgeMax] = {};
  uint64_t k[4][kAgeMax] = {};  // the bucket key (frame / bucket + 1) of each slot, 0 empty
  int head[4] = {};
};
inline bool AgeObserve(AgedCone& a, const AngBox& b, uint64_t frame, uint64_t bucket, int n, uint32_t obs) {
  if (!b.ok || std::fabs(b.az0) > 2.97 || std::fabs(b.az1) > 2.97) return false;  // the HUD is ahead (as LearnCone)
  const uint64_t key = frame / bucket + 1;
  const double val[4] = {b.az0, b.az1, b.el0, b.el1};
  for (int s = 0; s < 4; ++s) {
    int& h = a.head[s];
    if (!a.k[s][h] || (((obs >> s) & 1u) && key > a.k[s][h])) {
      if (a.k[s][h]) h = (h + 1) % n;
      a.k[s][h] = key, a.v[s][h] = val[s];
    } else {
      a.v[s][h] = (s & 1) ? std::max(a.v[s][h], val[s]) : std::min(a.v[s][h], val[s]);
    }
  }
  return true;
}
inline Cone AgedField(const AgedCone& a, int n) {
  Cone c;
  double e[4] = {1e9, -1e9, 1e9, -1e9};
  bool any[4] = {};
  for (int s = 0; s < 4; ++s)
    for (int i = 0; i < n; ++i)
      if (a.k[s][i]) any[s] = true, e[s] = (s & 1) ? std::max(e[s], a.v[s][i]) : std::min(e[s], a.v[s][i]);
  if (!any[0] || !any[1] || !any[2] || !any[3] || e[1] < e[0] || e[3] < e[2]) return c;
  c.ok = true, c.az0 = e[0], c.az1 = e[1], c.el0 = e[2], c.el1 = e[3];
  return c;
}
// The tracking-space azimuth / elevation of a view pixel position (the GPU's formula, in double).
inline bool PixelTrackAng(const Pose& p, double outW, double outH, double px, double py, double& az, double& el) {
  double d[3];
  if (!PixelDir(p, outW, outH, px, py, d)) return false;
  AzEl(d, az, el);
  return true;
}
// The view's rotation (view -> tracking) as three rows, for the GPU.
inline void RotRows(const float qf[4], float r[12]) {
  for (int k = 0; k < 3; ++k) {
    const double e[3] = {k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0};
    double c[3];
    QRot(qf, e, c, false);  // column k
    for (int i = 0; i < 3; ++i) r[i * 4 + k] = static_cast<float>(c[i]);
  }
  r[3] = r[7] = r[11] = 0;
}
// The rectangle's pixel box in a view with pose p, clipped to the view, or not ok: no part of it inside the view.
// R97: the rectangle's outline (as directions in the view's space) is clipped against the view frustum (the near plane
// and the four FOV planes) before it is projected, so the box is that of the visible part. R95 took the box of the
// whole outline and clipped the box: a HUD leaving the view through its side kept the vertical extent of its far
// (stretched) part (R96: periphery windows 211 -> 347 px tall at the edge), and a HUD partly behind the view plane
// gave the whole view, an overflow while nothing of it was visible (R96: 3 runs of 12-28 frames per turn and view).
constexpr int kOutline = 4 * (kEdgeSamples + 1);
// The rectangle's outline as tracking-space directions (R97: cached per cone by DecideWindow: the trigonometry once).
inline void ConeOutline(const Cone& c, double d[kOutline][3]) {
  int n = 0;
  for (int side = 0; side < 4; ++side)
    for (int i = 0; i <= kEdgeSamples; ++i) {
      const double t = double(i) / kEdgeSamples;
      const double az = side == 0 ? c.az0 + t * (c.az1 - c.az0) : side == 1 ? c.az1 : side == 2 ? c.az1 - t * (c.az1 - c.az0) : c.az0;
      const double el = side == 0 ? c.el0 : side == 1 ? c.el0 + t * (c.el1 - c.el0) : side == 2 ? c.el1 : c.el1 - t * (c.el1 - c.el0);
      DirOf(az, el, d[n++]);
    }
}
// R101: the view-pixel extent of each side's visible outline samples (in front, inside the view): side s as AgedCone's
// (0 az0, 1 az1, 2 el0, 3 el1); any[s] false: none of its samples visible.
struct SideVis {
  bool any[4] = {};
  double x0[4] = {}, y0[4] = {}, x1[4] = {}, y1[4] = {};
};
// The sides the rectangle (view pixels) shows: bit s set when side s has a visible sample and all of them are inside it.
inline uint32_t SideMask(const SideVis& v, double rx0, double ry0, double rx1, double ry1) {
  uint32_t m = 0;
  for (int s = 0; s < 4; ++s)
    if (v.any[s] && v.x0[s] >= rx0 - 1 && v.y0[s] >= ry0 - 1 && v.x1[s] <= rx1 + 1 && v.y1[s] <= ry1 + 1) m |= 1u << s;
  return m;
}
// The outline's pixel box in a view with pose p, clipped to the view (see ProjectCone); R101: vis (optional) gets the
// sides' visible extents (ConeOutline's sides 0..3 are el0, az1, el1, az0).
inline Box ProjectDirs(const double dirs[kOutline][3], const Pose& p, uint32_t outW, uint32_t outH,
                       SideVis* vis = nullptr) {
  if (vis) *vis = SideVis{};
  if (!p.ok || !outW || !outH) return Box{};
  constexpr int kCap = kOutline + 8;  // each plane adds at most one vertex to a convex polygon (+ slack)
  double a[kCap][3], b[kCap][3];
  int n = 0;
  for (int i = 0; i < kOutline; ++i) QRot(p.q, dirs[i], a[n++], true);
  if (vis) {
    static constexpr int kSideOf[4] = {2, 1, 3, 0};
    const double sx = outW / (double(p.tan[1]) - p.tan[0]), sy = outH / (double(p.tan[2]) - p.tan[3]);
    for (int i = 0; i < kOutline; ++i) {
      const double z = -a[i][2];
      if (!(z > 1e-4)) continue;
      const double px = (a[i][0] / z - p.tan[0]) * sx, py = (p.tan[2] - a[i][1] / z) * sy;
      if (!(px >= -0.5 && py >= -0.5 && px <= outW + 0.5 && py <= outH + 0.5)) continue;
      const int s = kSideOf[i / (kEdgeSamples + 1)];
      if (!vis->any[s]) {
        vis->any[s] = true, vis->x0[s] = vis->x1[s] = px, vis->y0[s] = vis->y1[s] = py;
      } else {
        vis->x0[s] = std::min(vis->x0[s], px), vis->x1[s] = std::max(vis->x1[s], px);
        vis->y0[s] = std::min(vis->y0[s], py), vis->y1[s] = std::max(vis->y1[s], py);
      }
    }
  }
  // Planes n . v >= k: near (-z >= 1e-4), left (x >= tanL * -z), right, up (y <= tanU * -z), down.
  const double L = p.tan[0], R = p.tan[1], U = p.tan[2], D = p.tan[3];
  const double pl[5][4] = {{0, 0, -1, 1e-4}, {1, 0, L, 0}, {-1, 0, -R, 0}, {0, -1, -U, 0}, {0, 1, D, 0}};
  double (*src)[3] = a;
  double (*dst)[3] = b;
  for (const auto& P : pl) {
    int m = 0;
    for (int i = 0; i < n && m < kCap - 1; ++i) {
      const double* s = src[i];
      const double* e = src[(i + 1) % n];
      const double ds = P[0] * s[0] + P[1] * s[1] + P[2] * s[2] - P[3];
      const double de = P[0] * e[0] + P[1] * e[1] + P[2] * e[2] - P[3];
      if (ds >= 0) memcpy(dst[m++], s, sizeof(double) * 3);
      if ((ds >= 0) != (de >= 0) && m < kCap) {
        const double t = ds / (ds - de);
        for (int k = 0; k < 3; ++k) dst[m][k] = s[k] + t * (e[k] - s[k]);
        ++m;
      }
    }
    n = m;
    std::swap(src, dst);
    if (!n) return Box{};
  }
  double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
  const double sx = outW / (R - L), sy = outH / (U - D);
  for (int i = 0; i < n; ++i) {
    const double z = -src[i][2];
    if (!(z > 0)) continue;
    const double px = (src[i][0] / z - L) * sx, py = (U - src[i][1] / z) * sy;
    x0 = std::min(x0, px), y0 = std::min(y0, py), x1 = std::max(x1, px), y1 = std::max(y1, py);
  }
  if (!(x1 >= x0) || !(y1 >= y0)) return Box{};
  const Box o{true, static_cast<int>(std::floor(std::max(x0, -1e7))), static_cast<int>(std::floor(std::max(y0, -1e7))),
              static_cast<int>(std::ceil(std::min(x1, 1e7))), static_cast<int>(std::ceil(std::min(y1, 1e7)))};
  return Clip(o, static_cast<int>(outW), static_cast<int>(outH));
}
inline Box ProjectCone(const Cone& c, const Pose& p, uint32_t outW, uint32_t outH) {
  if (!c.ok || !p.ok || !outW || !outH) return Box{};
  double d[kOutline][3];
  ConeOutline(c, d);
  return ProjectDirs(d, p, outW, outH);
}

// The frame's window: the union of the collimated box and the helmet extent (each clipped to the view), a margin of
// 10 % + 4 px (+ extra) around it, clipped to the view; larger than the capacity -> overflow.
struct WinOut {
  bool any = false, overflow = false;
  uint32_t x = 0, y = 0, w = 0, h = 0;
};
inline WinOut ChooseWindow(const Box& col, const Box& hel, uint32_t outW, uint32_t outH, uint32_t capW, uint32_t capH,
                           int extra) {
  WinOut o;
  const int W = static_cast<int>(outW), H = static_cast<int>(outH);
  const Box u = Union(Clip(col, W, H), Clip(hel, W, H));
  if (!u.ok) return o;
  const int mx = static_cast<int>(std::ceil(0.05 * (u.x1 - u.x0))) + 4 + extra;
  const int my = static_cast<int>(std::ceil(0.05 * (u.y1 - u.y0))) + 4 + extra;
  const Box g = Clip(Box{true, u.x0 - mx, u.y0 - my, u.x1 + mx, u.y1 + my}, W, H);
  if (!g.ok) return o;
  o.any = true;
  o.x = static_cast<uint32_t>(g.x0), o.y = static_cast<uint32_t>(g.y0);
  o.w = static_cast<uint32_t>(g.x1 - g.x0), o.h = static_cast<uint32_t>(g.y1 - g.y0);
  if (o.w > capW || o.h > capH) o.overflow = true;
  return o;
}

// Stability (Dev's 0049: a pair of real frames is used only when both carry symbology for the same views): the window's
// size grows at once and shrinks only after kShrinkFrames frames in a row below it (centred on the frame's own window,
// inside the view); an overflow starts at once and ends only after kOverflowExit frames in a row that fit.
constexpr int kShrinkFrames = 30, kOverflowExit = 10;
struct Hold {
  uint32_t w = 0, h = 0;
  int shrinkW = 0, shrinkH = 0;
  bool over = false;
  int fit = 0;
};
inline WinOut Stabilize(Hold& s, const WinOut& c, uint32_t outW, uint32_t outH, uint32_t cap) {
  if (!c.any && !c.overflow) {  // nothing in view (or nothing learnt): the hold goes
    s = Hold{};
    return c;
  }
  if (c.overflow) {
    s.over = true;
    s.fit = 0;
    return c;
  }
  if (s.over) {
    if (++s.fit < kOverflowExit) {
      WinOut o;
      o.overflow = true;
      return o;
    }
    s.over = false;
    s.fit = 0;
  }
  auto dim = [](uint32_t cand, uint32_t& held, int& run) {
    if (cand >= held) {
      held = cand;
      run = 0;
    } else if (++run >= kShrinkFrames) {
      held = cand;
      run = 0;
    }
    return held;
  };
  WinOut o = c;
  o.w = std::max(c.w, std::min(dim(c.w, s.w, s.shrinkW), std::min(cap, outW)));
  o.h = std::max(c.h, std::min(dim(c.h, s.h, s.shrinkH), std::min(cap, outH)));
  const uint32_t gx = (o.w - c.w) / 2, gy = (o.h - c.h) / 2;
  o.x = c.x >= gx ? c.x - gx : 0;
  o.y = c.y >= gy ? c.y - gy : 0;
  if (o.x + o.w > outW) o.x = outW - o.w;
  if (o.y + o.h > outH) o.y = outH - o.h;
  return o;
}
// The coverage alpha's validity for the header (emission_flags::alpha_valid, which Dev's 0049 reads). R97 (R96 §3.2:
// DCS's HDR alpha before the overlay is not 0 by day anywhere under the HUD and on 45-49 % of the F/A-18's at night; the
// added alpha scales with the symbols' brightness, 0.17-0.39 at the night cores, up to 5.7 by day; the R95 vote
// flickered under yaw on views that saw part of the HUD): a vote counts only from a read-back of a complete view set
// (every view of the frame carried a window with >= 16 symbol texels) whose windows did not move since the last
// read-back (no yaw). verdict: 1 such a read-back with every view valid (failing texels <= 1 %, opaque cores >= 1 %);
// -1 such a read-back with a view invalid; 0 anything else (incomplete, moving, no symbols). The bit comes on after
// g_alphaOnRun valid votes in a row (default 256 read-backs, ~6 s of a still head with the HUD in all four views); any
// non-vote restarts that run; an invalid vote clears it and LATCHES it off for the rest of the generation (the set's;
// AlphaReset at a new generation). So it flips at most twice per generation, never on a moving head.
std::atomic<int> g_alphaOnRun{256};  // tests shorten it
struct AlphaHold {
  bool on = false, latched = false;
  int run = 0;
  uint32_t flips = 0;  // statistics: changes of `on`
};
inline void AlphaVote(AlphaHold& a, int verdict) {
  if (verdict < 0) {
    if (a.on) ++a.flips;
    a.on = false;
    a.latched = true;
    a.run = 0;
    return;
  }
  if (verdict == 0 || a.latched || a.on) {
    a.run = 0;
    return;
  }
  if (++a.run >= std::max(1, g_alphaOnRun.load(std::memory_order_relaxed))) {
    a.on = true;
    a.run = 0;
    ++a.flips;
  }
}
inline void AlphaReset(AlphaHold& a) {
  const uint32_t f = a.flips;
  a = AlphaHold{};
  a.flips = f;
}
// The per-view verdict of a read-back's statistics (changed, failing, core texel counts): 1 valid, -1 invalid, 0 too
// few symbol texels to tell.
inline int AlphaViewVerdict(uint32_t changed, uint32_t bad, uint32_t core) {
  if (changed < 16) return 0;
  const bool ok = uint64_t(bad) * 100 <= changed && uint64_t(core) * 100 >= changed && core >= 4;
  return ok ? 1 : -1;
}

// What a view learnt (kept across frames; pure, offline tested and replayed): the collimated field, the helmet's decaying
// view-pixel extent, the margin's extra, the scans, the window's stability and the HUD-only fallback.
// R101: a cone's projection with the frame's pose, cached (a still head repeats it): the outline (rebuilt when the cone
// changes), the box and the sides' visible extents.
struct ProjCache {
  Cone of;
  double dirs[kOutline][3] = {};
  Pose pose;
  Box box;
  SideVis vis;
  uint32_t W = 0, H = 0;
  bool ok = false;
};
struct Field {
  Cone col;              // the collimated field (R101: the long aging field's current extent)
  Cone rec;              // R101: the recent aging field's current extent (the fallback's)
  AgedCone colAge, recAge;
  bool recent = false;   // R101: the window comes from the recent field (the long one overflowed, even the HUD alone)
  int recFit = 0;        // frames in a row the long field's HUD fitted while recent
  // R101: the sides the frame's window / view showed (SideMask), long and recent, for the read-back's aging
  uint8_t obsWinL = 0, obsViewL = 0, obsWinR = 0, obsViewR = 0;
  ProjCache pcR;         // R101: the recent field's projection cache
  // R101: per axis (azimuth, elevation), the long field's side the fallback's spare capacity goes to first: -1 the low
  // side, 1 the high one, 0 both evenly; set when the recent field stops covering one side (the content just left it:
  // in a weave or a turn reversal the other side comes next). cov*: the recent field covered the side last frame.
  int lean[2] = {};
  bool covLo[2] = {}, covHi[2] = {};
  SideVis visL;          // R101: the long field's sides this frame
  Box helB[kHelBuckets];
  uint64_t helK[kHelBuckets] = {};  // the bucket index (frame / kHelBucket) + 1 of each box (0: empty)
  int extra = 0;
  uint64_t lastEdge = 0, lastScan = 0;
  bool scanReq = true;
  int alpha = 0;        // 1 the coverage alpha checked valid in the last read-back, -1 invalid, 0 not known yet
  Hold hold;            // the window's stability
  bool hudOnly = false; // R97: the helmet left out (its union with the HUD overflowed)
  int helFit = 0;       // frames in a row the union fitted while hudOnly
  // R97 render-thread caches (a still head repeats the frame's inputs): the cone's outline, the last projection, the
  // last angle observation per kind (accumulation / scan).
  Cone dirsOf;
  double dirs[kOutline][3] = {};
  Pose lastPose;
  Box lastCol;
  uint32_t lastW = 0, lastH = 0;
  bool lastOk = false;
  uint32_t lastAng[2][4] = {};
};
inline bool SameCone(const Cone& a, const Cone& b) {
  return a.ok == b.ok && a.az0 == b.az0 && a.az1 == b.az1 && a.el0 == b.el0 && a.el1 == b.el1;
}
inline bool SamePose(const Pose& a, const Pose& b) {
  return a.ok == b.ok && !memcmp(a.q, b.q, sizeof(a.q)) && !memcmp(a.tan, b.tan, sizeof(a.tan));
}
// R97: the helmet's extent: the union of its boxes seen in the last kHelBuckets buckets.
inline Box HelmetExtent(const Field& L, uint64_t frame) {
  const uint64_t cur = frame / kHelBucket + 1;
  Box u;
  for (int i = 0; i < kHelBuckets; ++i)
    if (L.helK[i] && L.helK[i] + kHelBuckets > cur) u = Union(u, L.helB[i]);
  return u;
}
inline void NoteHelmet(Field& L, const Box& b, uint64_t frame) {
  if (!b.ok) return;
  const uint64_t k = frame / kHelBucket + 1;
  const int i = static_cast<int>(k % kHelBuckets);
  if (L.helK[i] != k) L.helK[i] = k, L.helB[i] = Box{};
  L.helB[i] = Union(L.helB[i], b);
}
inline void ForgetHelmet(Field& L) {
  for (int i = 0; i < kHelBuckets; ++i) L.helK[i] = 0, L.helB[i] = Box{};
}
// R97: the frame's window of a view from what it learnt: the collimated field projected with the frame's pose (clipped
// to the view), united with the helmet's extent unless the union overflows and the HUD alone fits (then hudOnly: the
// helmet is not accumulated this frame; it comes back after kHelmetBack frames of a fitting union). Overflow (present
// 0) only when even the HUD alone does not fit (or the helmet alone, without a HUD in view). Then the stability.
// R101: the recent fallback's window: the recent field's box with a margin of 10 % + 4 px (without the edge hits' extra:
// it would take the capacity the lean aims) shrunk to fit the capacity (down to kMinMargin px a side), then widened
// toward the long field's box (with its full margin, extra included) as far as the
// capacity allows: first toward the side `lean` names (per pixel axis: -1 low, 1 high), else evenly; clipped to the
// view. Overflow when even kMinMargin does not fit.
inline WinOut FitWindow(const Box& rec, const Box& lng, uint32_t outW, uint32_t outH, uint32_t cap, int extra,
                        int leanX = 0, int leanY = 0) {
  WinOut o;
  const int W = static_cast<int>(outW), H = static_cast<int>(outH), C = static_cast<int>(cap);
  const Box u = Clip(rec, W, H), l = Clip(lng, W, H);
  if (!u.ok) return o;
  auto axis = [&](int a0, int a1, int l0, int l1, bool lok, int lim, int lean, int& g0, int& g1) {
    const int len = a1 - a0;
    int m = static_cast<int>(std::ceil(0.05 * len)) + 4;  // (the edge hits' extra: on the long field's side below)
    if (len + 2 * m > C) m = (C - len) / 2;
    if (m < kMinMargin) return false;
    g0 = a0 - m, g1 = a1 + m;
    if (lok) {
      const int lm = static_cast<int>(std::ceil(0.05 * (l1 - l0))) + 4 + extra;
      const int want0 = std::max(0, g0 - (l0 - lm)), want1 = std::max(0, (l1 + lm) - g1);
      int slack = C - (g1 - g0);
      int e0 = 0, e1 = 0;
      if (lean < 0) {
        e0 = std::min(want0, slack), e1 = std::min(want1, slack - e0);
      } else if (lean > 0) {
        e1 = std::min(want1, slack), e0 = std::min(want0, slack - e1);
      } else {
        e0 = std::min(want0, slack / 2), e1 = std::min(want1, slack / 2);
        slack -= e0 + e1;
        const int f0 = std::min(want0 - e0, slack);
        e0 += f0, slack -= f0;
        e1 += std::min(want1 - e1, slack);
      }
      g0 -= e0, g1 += e1;
    }
    g0 = std::max(g0, 0), g1 = std::min(g1, lim);
    return g1 > g0;
  };
  int x0, x1, y0, y1;
  if (!axis(u.x0, u.x1, l.x0, l.x1, l.ok, W, leanX, x0, x1) || !axis(u.y0, u.y1, l.y0, l.y1, l.ok, H, leanY, y0, y1)) {
    o.overflow = true;
    return o;
  }
  o.any = true;
  o.x = static_cast<uint32_t>(x0), o.y = static_cast<uint32_t>(y0);
  o.w = static_cast<uint32_t>(x1 - x0), o.h = static_cast<uint32_t>(y1 - y0);
  if (o.w > cap || o.h > cap) o.overflow = true;
  return o;
}
// R101: the fallback's lean (Field::lean) from the recent field against the long one (0.5 deg: a side covered).
inline void UpdateLean(Field& L) {
  if (!L.col.ok || !L.rec.ok) return;
  constexpr double kCov = 0.5 * 3.14159265358979 / 180;
  const double g[2][2] = {{L.rec.az0 - L.col.az0, L.col.az1 - L.rec.az1}, {L.rec.el0 - L.col.el0, L.col.el1 - L.rec.el1}};
  for (int a = 0; a < 2; ++a) {
    const bool lo = g[a][0] < kCov, hi = g[a][1] < kCov;
    if (L.covHi[a] && !hi) L.lean[a] = -1;  // the content just left the high side: the low side next
    if (L.covLo[a] && !lo) L.lean[a] = 1;
    L.covLo[a] = lo, L.covHi[a] = hi;
  }
}
// R101: a cone's projection through its cache.
inline Box ProjectCached(ProjCache& c, const Cone& cone, const Pose& pose, uint32_t W, uint32_t H, SideVis& vis) {
  if (!cone.ok || !pose.ok) {
    vis = SideVis{};
    return Box{};
  }
  if (!SameCone(c.of, cone)) {
    ConeOutline(cone, c.dirs);
    c.of = cone;
    c.ok = false;
  }
  if (!(c.ok && c.W == W && c.H == H && SamePose(c.pose, pose))) {
    c.box = ProjectDirs(c.dirs, pose, W, H, &c.vis);
    c.pose = pose, c.W = W, c.H = H, c.ok = true;
  }
  vis = c.vis;
  return c.box;
}
// R101: the scan schedule (PassBegin; the replays use it too): a requested scan (symbols at the window's edge), every
// kScanRetry frames while nothing is learnt, every kScanEvery frames (staggered by view) once learnt, and every
// kScanFallback frames in the recent fallback or in overflow (the long field ages from them).
inline bool ScanDue(const Field& L, uint64_t frame, int v, bool learnt) {
  return L.scanReq || (!learnt && frame - L.lastScan >= kScanRetry) ||
         (learnt && (frame + static_cast<uint64_t>(v) * 16) % kScanEvery == 0) ||
         (learnt && (L.recent || L.hold.over) && (frame + static_cast<uint64_t>(v) * 2) % kScanFallback == 0);
}
inline WinOut DecideWindow(Field& L, const Pose& pose, uint32_t W, uint32_t H, uint32_t cap, uint64_t frame) {
  L.obsWinL = L.obsViewL = L.obsWinR = L.obsViewR = 0;
  Box col;
  if (L.col.ok && pose.ok) {  // (R97's cache; R101: with the sides' visible extents)
    if (!SameCone(L.dirsOf, L.col)) {
      ConeOutline(L.col, L.dirs);
      L.dirsOf = L.col;
      L.lastOk = false;
    }
    if (L.lastOk && L.lastW == W && L.lastH == H && SamePose(L.lastPose, pose)) {
      col = L.lastCol;
    } else {
      col = ProjectDirs(L.dirs, pose, W, H, &L.visL);
      L.lastPose = pose, L.lastCol = col, L.lastW = W, L.lastH = H, L.lastOk = true;
    }
  } else {
    L.visL = SideVis{};
  }
  const Box hel = HelmetExtent(L, frame);
  if (!col.ok && !hel.ok) {
    L.hold = Hold{};
    L.hudOnly = false, L.helFit = 0;
    L.recent = false, L.recFit = 0;
    if (pose.ok) L.obsViewL = L.obsViewR = static_cast<uint8_t>(SideMask(L.visL, 0, 0, W, H));
    return WinOut{};
  }
  const WinOut hudL = ChooseWindow(col, Box{}, W, H, cap, cap, L.extra);
  UpdateLean(L);
  // (pixel axes: azimuth grows with x, elevation against y)
  const int leanX = L.lean[0], leanY = -L.lean[1];
  if (L.recent && !hudL.overflow && ++L.recFit >= kRecentBack) {  // R101: the long field fits again: back to it
    L.recent = false, L.recFit = 0;
    L.hudOnly = true, L.helFit = 0;  // (the helmet comes back after kHelmetBack fitting frames, as R97's)
  } else if (L.recent && hudL.overflow) {
    L.recFit = 0;
  }
  WinOut c;
  SideVis visR;
  bool recProj = false;
  if (!L.recent) {
    const WinOut both = ChooseWindow(col, hel, W, H, cap, cap, L.extra);
    c = both;
    if (L.hudOnly) {
      if (!both.overflow && ++L.helFit >= kHelmetBack) {
        L.hudOnly = false, L.helFit = 0;
      } else {
        if (both.overflow) L.helFit = 0;
        c = hudL;
      }
    } else if (both.overflow && col.ok && hel.ok) {
      if (!hudL.overflow) {
        L.hudOnly = true, L.helFit = 0;
        c = hudL;
      }
    }
    if (L.hudOnly && !col.ok) c = WinOut{};  // the HUD out of view while the helmet is left out: no window
    if (c.overflow && col.ok && L.rec.ok) {  // R101: even the HUD alone overflows: the recent field, if it fits
      const Box rcol = ProjectCached(L.pcR, L.rec, pose, W, H, visR);
      recProj = true;
      const WinOut r = FitWindow(rcol, col, W, H, cap, L.extra, leanX, leanY);
      if (r.any && !r.overflow) {
        L.recent = true, L.recFit = 0;
        L.hudOnly = true, L.helFit = 0;
        c = r;
      }
    }
  } else {
    const Box rcol = ProjectCached(L.pcR, L.rec, pose, W, H, visR);
    recProj = true;
    const WinOut r = FitWindow(rcol, col, W, H, cap, L.extra, leanX, leanY);
    c = r.any || r.overflow ? r : hudL;
    if (!col.ok && !rcol.ok) c = WinOut{};
  }
  const WinOut o = Stabilize(L.hold, c, W, H, cap);
  if (pose.ok) {  // R101: the sides this frame shows (the read-back ages them)
    const uint32_t vl = SideMask(L.visL, 0, 0, W, H);
    const uint32_t wl = o.any && !o.overflow ? SideMask(L.visL, o.x, o.y, o.x + o.w, o.y + o.h) : 0u;
    L.obsViewL = static_cast<uint8_t>(vl), L.obsWinL = static_cast<uint8_t>(wl);
    if (recProj) {
      L.obsViewR = static_cast<uint8_t>(SideMask(visR, 0, 0, W, H));
      L.obsWinR = static_cast<uint8_t>(o.any && !o.overflow ? SideMask(visR, o.x, o.y, o.x + o.w, o.y + o.h) : 0u);
    } else {  // (the recent field lies inside the long one: what shows the long one's sides shows its own)
      L.obsViewR = L.obsViewL, L.obsWinR = L.obsWinL;
    }
  }
  return o;
}
// What a read-back frame's view was (the render thread's ring entry).
struct StatMeta {
  bool win = false, scan = false, hudOnly = false;
  bool angOk = false;            // the GPU had the view's FOV (the angles in the statistics are meaningful)
  float q[4] = {0, 0, 0, 1};     // the orientation the frame was rendered with (the submitted pose; else the located)
  Pose pose;                     // the same pose with its FOV (the pixel-box fallback)
  uint32_t W = 0, H = 0;
  uint64_t frame = 0;
  uint32_t wx = 0, wy = 0, ww = 0, wh = 0;  // the window
  // R101: the sides the frame's window / view showed (Field::obs*, set by DecideWindow), long and recent
  uint8_t obsWinL = 0, obsViewL = 0, obsWinR = 0, obsViewR = 0;
};
inline void NoteObs(StatMeta& m, const Field& L) {
  m.obsWinL = L.obsWinL, m.obsViewL = L.obsViewL, m.obsWinR = L.obsWinR, m.obsViewR = L.obsViewR;
}
struct LearnCount {
  uint64_t col = 0, hel = 0, edges = 0;
};
// Learning from a read-back view (pure). Returns the view's alpha verdict (AlphaViewVerdict, 0 without a window).
inline int LearnStats(Field& L, const StatMeta& m, const uint32_t* s, LearnCount& n) {
  // R101: every observation goes into both aging fields (the accumulation's with the window's sides, the scan's with the
  // view's); a still head's repeated observation is no longer skipped: it is what ages a side that stopped growing.
  // n.col counts the observations that changed the long field.
  bool obs = false;
  for (int i = 0; i < 2; ++i) {
    const AngBox b = m.angOk ? DecodeAng(s + (i ? kStScanAng1 : kStAng1))
                             : BoxAng(m.pose, m.W, m.H, DecodeBox(s + (i ? kStScan1 : kStBox1)));  // R95's pixel box
    if (!b.ok) continue;
    AgeObserve(L.colAge, b, m.frame, kColBucket, kColBuckets, i ? m.obsViewL : m.obsWinL);
    AgeObserve(L.recAge, b, m.frame, kRecBucket, kRecBuckets, i ? m.obsViewR : m.obsWinR);
    obs = true;
  }
  if (obs) {
    const Cone c = AgedField(L.colAge, kColBuckets), r = AgedField(L.recAge, kRecBuckets);
    if (c.ok) {
      if (!SameCone(c, L.col)) ++n.col;
      const uint64_t seen = L.col.seen;
      L.col = c;
      L.col.seen = m.frame > seen ? m.frame : seen;
    }
    if (r.ok) L.rec = r;
  }
  for (int k : {kStBox2, kStScan2}) {
    const Box b = DecodeBox(s + k);
    if (b.ok) NoteHelmet(L, b, m.frame), ++n.hel;
  }
  if (m.win && s[kStEdge]) {
    L.scanReq = true;
    L.extra = std::min(L.extra + 8, kMaxExtra);
    L.lastEdge = m.frame;
    ++n.edges;
  } else if (L.extra > 0 && m.frame - L.lastEdge > 600) {
    --L.extra;
  }
  if (!m.win) return 0;
  const int a = AlphaViewVerdict(s[kStChanged], s[kStAlphaBad], s[kStAlphaCore]);
  if (a) L.alpha = a;
  return a;
}
// R97: the read-back's vote for alpha_valid (see AlphaVote) from its views' verdicts: seen[v] the view rendered that
// frame; same[v] its window equals the previous read-back's.
inline int AlphaFrameVote(const bool seen[4], const bool win[4], const int verdict[4], const bool same[4]) {
  bool complete = true, still = true, bad = false, any = false;
  for (int v = 0; v < 4; ++v) {
    if (!seen[v]) continue;
    any = true;
    if (!win[v] || verdict[v] == 0) complete = false;
    if (!same[v]) still = false;
    if (verdict[v] < 0) bad = true;
  }
  if (!any || !complete || !still) return 0;
  return bad ? -1 : 1;
}

// The located pose of the frame being rendered: among DCS's xrLocateViews calls made before now, the smallest
// displayTime after the last committed frame's (DCS locates each frame once, before rendering it; R74 §4), the latest
// call of it; -1 none (the caller falls back to the last committed pose).
inline int PickPredict(const ctime::Rec* r, int n, int64_t lastDt, int64_t nowQpc) {
  int best = -1;
  for (int i = 0; i < n; ++i) {
    if (!r[i].q0 || r[i].q0 > nowQpc || !r[i].vn || r[i].dt <= lastDt) continue;
    if (best < 0 || r[i].dt < r[best].dt || (r[i].dt == r[best].dt && r[i].q0 > r[best].q0)) best = i;
  }
  return best;
}

// Quad Views' settings from its configuration text (layer.cpp ParseConfigurationStatement): "//" or "#" comments,
// [app:x] / [exe:x] sections active when x is in the application / executable name, any other [section] (a runtime or
// system name, unknown here) inactive; name=value, a value that does not parse is skipped. The values carry over
// between files (QV parses its files in order into the same settings).
struct QvVals {
  float sharpen = 0.7f, periSharpen = 0.0f, smooth = 0.2f, shape = 0.0f;
  int farDepth = 0, temporal = 0, taper = 0, debugKeys = 0;
};
inline bool ParseF(const std::string& v, float& out) {
  const char* s = v.c_str();
  char* e = nullptr;
  const float f = strtof(s, &e);
  if (e == s || !std::isfinite(f)) return false;
  out = f;
  return true;
}
inline bool ParseI(const std::string& v, int& out) {
  const char* s = v.c_str();
  char* e = nullptr;
  const long i = strtol(s, &e, 10);
  if (e == s) return false;
  out = static_cast<int>(i);
  return true;
}
inline void QvParseText(const std::string& text, const std::string& appName, const std::string& exeName, QvVals& io) {
  bool active = true;
  size_t at = 0;
  while (at <= text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();  // (std::getline in text mode drops the CR)
    if (line.empty()) continue;
    if ((line[0] == '/' && line.size() > 1 && line[1] == '/') || line[0] == '#') continue;
    if (line[0] == '[' && line.back() == ']') {
      if (line.size() >= 6 && line.compare(1, 4, "app:") == 0)
        active = appName.find(line.substr(5, line.size() - 6)) != std::string::npos;
      else if (line.size() >= 6 && line.compare(1, 4, "exe:") == 0)
        active = exeName.find(line.substr(5, line.size() - 6)) != std::string::npos;
      else
        active = false;  // a runtime / system section: not knowable in DCS's process (R95 doc §5.3)
      continue;
    }
    if (!active) continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string name = line.substr(0, eq), value = line.substr(eq + 1);
    float f = 0;
    int i = 0;
    if (name == "sharpen_focus_view" && ParseF(value, f)) io.sharpen = std::clamp(f, 0.0f, 1.0f);
    else if (name == "dcsvr_periphery_sharpen" && ParseF(value, f)) io.periSharpen = std::clamp(f, 0.0f, 1.0f);
    else if (name == "smoothen_focus_view_edges" && ParseF(value, f)) io.smooth = std::clamp(f, 0.0f, 0.5f);
    else if (name == "focus_view_shape" && ParseF(value, f)) io.shape = std::clamp(f, 0.0f, 16.0f);
    else if (name == "dcsvr_far_depth" && ParseI(value, i)) io.farDepth = i;
    else if (name == "dcsvr_temporal" && ParseI(value, i)) io.temporal = i;
    else if (name == "dcsvr_sharpen_taper" && ParseI(value, i)) io.taper = i;
    else if (name == "debug_keys" && ParseI(value, i)) io.debugKeys = i;
  }
}
inline csy::QvSettings QvSettingsOf(const QvVals& v, bool profileFile) {
  csy::QvSettings s{};
  s.sharpenFocusView = v.sharpen, s.peripherySharpen = v.periSharpen, s.smoothenFocusViewEdges = v.smooth;
  s.focusViewShape = v.shape;
  s.flags = (v.farDepth ? csy::qv_flags::far_depth_warp : 0u) | (v.temporal ? csy::qv_flags::periphery_temporal : 0u) |
            (v.taper ? csy::qv_flags::sharpen_taper : 0u) | (v.debugKeys ? csy::qv_flags::debug_keys : 0u) |
            (profileFile ? csy::qv_flags::profile_file : 0u);
  return s;
}

// The pupil shift layer's original FOVs (angles) of this frame: the entry with displayTime whose shiftedFov equals
// DCS's submitted FOV for every submitted view (within 1e-5 rad). False: none (dcs_submitted).
inline bool PupilOriginal(const dcsvr_pupil_fov::Block& b, int64_t displayTime, const float sub[4][4], uint32_t nViews,
                          float orig[4][4]) {
  const dcsvr_pupil_fov::Entry* e = dcsvr_pupil_fov::find(b, displayTime);
  if (!e || nViews == 0 || nViews > 4 || e->viewCount < nViews) return false;
  for (uint32_t v = 0; v < nViews; ++v)
    for (int k = 0; k < 4; ++k)
      if (!(std::fabs(e->views[v].shiftedFov[k] - sub[v][k]) <= 1e-5f) || !std::isfinite(e->views[v].originalFov[k]))
        return false;
  for (uint32_t v = 0; v < nViews; ++v) memcpy(orig[v], e->views[v].originalFov, sizeof(orig[v]));
  return true;
}
// The focus layout from four views' FOV tangents (contract: focus_rect per eye).
inline bool MakeLayout(const float tan[4][4], csy::FocusLayout& L) {
  L = csy::FocusLayout{};
  bool ok = true;
  for (int e = 0; e < 2; ++e) {
    memcpy(L.eye[e].eyeFovTan, tan[e], sizeof(L.eye[e].eyeFovTan));
    memcpy(L.eye[e].focusFovTan, tan[2 + e], sizeof(L.eye[e].focusFovTan));
    ok = csy::focus_rect(tan[e], tan[2 + e], L.eye[e].rectNorm) && ok;
  }
  if (ok) L.flags = csy::layout_flags::layout_valid | csy::layout_flags::from_submit;
  return ok;
}

// ---------------------------------------------------------------------------
// Compute shaders: the accumulation (MODE 0: TYPE 1 / 2, INIT 1 for the view's first run) and the scan (MODE 1: the
// tile signatures at a run's start, MODE 2: changed tiles at its end -> the type's box)
// ---------------------------------------------------------------------------
const char kCsHlsl[] = R"(
cbuffer P : register(b0) {
  uint4 win;  // the window in DCS's HDR target: x, y, w, h
  uint4 atl;  // the class atlas origin of the window x, y; the view; bits 0-3: the window's left / top / right /
              // bottom edge lies inside the view
  uint4 box;  // the view's box in the target: x, y, w, h
  uint4 scn;  // tiles x, y
  float4 fov; // R97: the view's FOV tangents (left, right, up, down) and its rotation (view -> tracking, rows) for
  float4 rot[3]; //      the tracking-space angles of the type 1 texels' groups
};
Texture2D<float4> hdr : register(t0);
#if MODE == 0
Texture2D<float4> pre : register(t1);
RWTexture2D<float4> acc : register(u0);
RWBuffer<uint> st : register(u1);
RWTexture2D<uint> cls : register(u2);
#elif MODE == 3
Texture2D<float4> accT : register(t1);
RWTexture2D<float4> em : register(u0);
RWTexture2D<uint> clsU : register(u1);  // R97: the slot's class atlas (1 byte read per texel; acc only where != 0)
#else
RWTexture2D<uint> sig : register(u0);
RWBuffer<uint> st : register(u1);
#endif
groupshared uint gs[8];
void Bound(uint x0, uint y0, uint x1, uint y1) {
  InterlockedMax(gs[0], 0xffffffffu - x0);
  InterlockedMax(gs[1], 0xffffffffu - y0);
  InterlockedMax(gs[2], x1);
  InterlockedMax(gs[3], y1);
}
// R97: the view-relative azimuth / elevation box of the view-pixel rectangle [x0, x1] x [y0, y1] (its corners: the
// azimuth is monotonic in x, the elevation's extremes are at the corners), (a + 2) * 2^30 into st[ob .. ob + 3] (the
// minimum as ~x). Once per group, from the group's changed-texel box (or a changed scan tile).
#if MODE != 3
// R97: an atomic max only when the (possibly stale, never larger) current value is below: one global atomic per word
// and group was R95's main accumulation cost on a large window (contention on the 4 box words).
void MaxTo(uint i, uint v) {
  if (v > st[i]) InterlockedMax(st[i], v);
}
uint EncA(float a) { return (uint)((a + 4.0) * 536870912.0); }
float2 TrackAng(float x, float y) {  // the tracking-space azimuth / elevation of view pixel position (x, y)
  float3 c = float3(fov.x + x / float(box.z) * (fov.y - fov.x), fov.z - y / float(box.w) * (fov.z - fov.w), -1);
  float3 d = float3(dot(rot[0].xyz, c), dot(rot[1].xyz, c), dot(rot[2].xyz, c));
  return float2(atan2(d.x, -d.z), atan2(d.y, sqrt(d.x * d.x + d.z * d.z)));
}
void AngOut(uint ob, float x0, float y0, float x1, float y1) {
  float2 a = TrackAng(x0, y0), b = TrackAng(x1, y0), c = TrackAng(x0, y1), d = TrackAng(x1, y1);
  float2 lo = min(min(a, b), min(c, d)), hi = max(max(a, b), max(c, d));
  MaxTo(ob + 0, 0xffffffffu - EncA(lo.x));
  MaxTo(ob + 1, 0xffffffffu - EncA(lo.y));
  MaxTo(ob + 2, EncA(hi.x));
  MaxTo(ob + 3, EncA(hi.y));
}
#endif
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
  if (gi < 8) gs[gi] = 0;
  GroupMemoryBarrierWithGroupSync();
  uint base = atl.z * 32u;
#if MODE == 0
  if (id.x < win.z && id.y < win.w) {
    float4 h = hdr.Load(int3(win.xy + id.xy, 0));
    float4 a = pre.Load(int3(id.xy, 0));
    bool ch = any(asuint(h) != asuint(a));
    float4 d = ch ? h - a : float4(0, 0, 0, 0);
#if INIT
    acc[id.xy] = d;
    cls[atl.xy + id.xy] = ch ? TYPE : 0u;
#else
    if (ch) {
      acc[id.xy] = acc[id.xy] + d;
      cls[atl.xy + id.xy] = TYPE;
    }
#endif
    if (ch) {
      uint vx = win.x - box.x + id.x, vy = win.y - box.y + id.y;
      Bound(vx, vy, vx + 1, vy + 1);
      InterlockedAdd(gs[5], 1u);
      bool edge = ((atl.w & 1u) && id.x < 2) || ((atl.w & 2u) && id.y < 2) || ((atl.w & 4u) && id.x + 2 >= win.z) ||
                  ((atl.w & 8u) && id.y + 2 >= win.w);
      if (edge) InterlockedAdd(gs[4], 1u);
      // the coverage alpha (R94): the target's alpha before the run ~0 and the run's added alpha in [0, 1]
      bool aok = abs(a.a) <= 1.0 / 512.0 && d.a >= -1.0 / 512.0 && d.a <= 1.0 + 1.0 / 512.0;
      if (!aok) InterlockedAdd(gs[6], 1u);
      else if (d.a >= 0.75) InterlockedAdd(gs[7], 1u);  // R97: an opaque core (a coverage alpha has them)
    }
  }
  GroupMemoryBarrierWithGroupSync();
  uint ob = base + (TYPE == 2 ? 4u : 0u);
  if (gi == 0 && gs[2]) {
    MaxTo(ob + 0, gs[0]);
    MaxTo(ob + 1, gs[1]);
    MaxTo(ob + 2, gs[2]);
    MaxTo(ob + 3, gs[3]);
  }
  if (gi == 1 && gs[4]) InterlockedAdd(st[base + 8], gs[4]);
  if (gi == 2 && gs[5]) InterlockedAdd(st[base + 9], gs[5]);
  if (gi == 3 && gs[6]) InterlockedAdd(st[base + 10], gs[6]);
  if (gi == 4 && gs[7]) InterlockedAdd(st[base + 11], gs[7]);
#if TYPE == 1
  // R97: the group's changed-texel box (view pixels, gs[0..3]) as view-relative angles, once per group
  if (gi == 5 && gs[2]) {
    float vx0 = float(0xffffffffu - gs[0]), vy0 = float(0xffffffffu - gs[1]);
    AngOut(base + 20u, vx0, vy0, float(gs[2]), float(gs[3]));
  }
#endif
#elif MODE == 3
  // E's first term from the bytes DCS's target holds (t0: its UNORM view): E = dec(F) - dec(C), the pixel shader left
  // dec(C) in the atlas (a GPU's float -> UNORM8 conversion need not be the shader's round-half-up, R95 doc).
  if (id.x < win.z && id.y < win.w && clsU[atl.xy + id.xy] != 0u) {
    float4 d = accT.Load(int3(id.xy, 0));
    if (any(d != 0)) {
      float3 b = round(saturate(hdr.Load(int3(win.xy + id.xy, 0)).rgb) * 255.0) / 255.0;
      float3 f = float3(b.r <= 0.04045 ? b.r / 12.92 : pow((b.r + 0.055) / 1.055, 2.4),
                        b.g <= 0.04045 ? b.g / 12.92 : pow((b.g + 0.055) / 1.055, 2.4),
                        b.b <= 0.04045 ? b.b / 12.92 : pow((b.b + 0.055) / 1.055, 2.4));
      float4 e = em[atl.xy + id.xy];
      em[atl.xy + id.xy] = float4(f - e.rgb, e.a);
    }
  }
#else
  if (id.x < scn.x && id.y < scn.y) {
    uint x0 = id.x * 8u, y0 = id.y * 8u;
    uint x1 = min(x0 + 8u, box.z), y1 = min(y0 + 8u, box.w);
    uint hs = 2166136261u;
    for (uint y = y0; y < y1; ++y)
      for (uint x = x0; x < x1; ++x) {
        uint4 u = asuint(hdr.Load(int3(box.x + x, box.y + y, 0)));
        hs = (hs ^ u.x) * 16777619u;
        hs = (hs ^ u.y) * 16777619u;
        hs = (hs ^ u.z) * 16777619u;
        hs = (hs ^ u.w) * 16777619u;
      }
#if MODE == 1
    sig[id.xy] = hs;
#else
    if (sig[id.xy] != hs) Bound(x0, y0, x1, y1);
#endif
  }
#if MODE == 2
  GroupMemoryBarrierWithGroupSync();
  uint ob = base + (TYPE == 2 ? 16u : 12u);
  if (gi == 0 && gs[2]) {
    MaxTo(ob + 0, gs[0]);
    MaxTo(ob + 1, gs[1]);
    MaxTo(ob + 2, gs[2]);
    MaxTo(ob + 3, gs[3]);
  }
#if TYPE == 1
  if (gi == 1 && gs[2]) {  // R97: the changed tiles' angles
    float vx0 = float(0xffffffffu - gs[0]), vy0 = float(0xffffffffu - gs[1]);
    AngOut(base + 24u, vx0, vy0, float(gs[2]), float(gs[3]));
  }
#endif
#endif
#endif
}
)";
// cs[0..3]: accumulation type 1 first run, type 1, type 2 first run, type 2; cs[4] scan start; cs[5 / 6] scan end
// of a type 1 / 2 run; cs[7] E from DCS's bytes after the tone-map draw (t0 = its target).
constexpr int kCsN = 8;
std::vector<uint8_t> g_blob[kCsN];

bool CompileCs(pD3DCompile compile, std::string& err) {
  const char* mode[kCsN] = {"0", "0", "0", "0", "1", "2", "2", "3"};
  const char* type[kCsN] = {"1", "1", "2", "2", "1", "1", "2", "1"};
  const char* init[kCsN] = {"1", "0", "1", "0", "0", "0", "0", "0"};
  for (int i = 0; i < kCsN; ++i) {
    const D3D_SHADER_MACRO defs[] = {{"MODE", mode[i]}, {"TYPE", type[i]}, {"INIT", init[i]}, {nullptr, nullptr}};
    ID3DBlob* code = nullptr;
    ID3DBlob* e = nullptr;
    const HRESULT hr = compile(kCsHlsl, sizeof(kCsHlsl) - 1, "symbology", defs, nullptr, "main", "cs_5_0",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &e);
    if (FAILED(hr) || !code) {
      err = e ? std::string(static_cast<const char*>(e->GetBufferPointer()), e->GetBufferSize())
              : std::string("D3DCompile failed (symbology)");
      if (e) e->Release();
      if (code) code->Release();
      return false;
    }
    const uint8_t* b = static_cast<const uint8_t*>(code->GetBufferPointer());
    g_blob[i].assign(b, b + code->GetBufferSize());
    code->Release();
    if (e) e->Release();
  }
  return true;
}

// ---------------------------------------------------------------------------
// Base objects (made with cockpit_mask.h's EnsureBase: the config thread at the install, or the first capture)
// ---------------------------------------------------------------------------
constexpr UINT kCbBytes = 128;  // the compute constants P (8 x 16 bytes; R97: + the FOV and the rotation)
struct Base {
  ID3D11ComputeShader* cs[kCsN] = {};
  ID3D11Buffer* cb[kViews] = {};    // the views' compute constants (P above)
  ID3D11Buffer* psCb[kViews] = {};  // the views' pixel-shader constants (color_pass.h's cbQvSym)
  uint32_t cbLast[kViews][32] = {}, psLast[kViews][16] = {};  // R97: what each holds (written only when it changes)
  bool cbValid[kViews] = {}, psValid[kViews] = {};
  ID3D11Buffer* stat = nullptr;     // kViews * kStatU words
  ID3D11UnorderedAccessView* statUav = nullptr;
  ID3D11Buffer* stage[kRing] = {};
  ID3D11Query* dj[kRing] = {};
  ID3D11Query* ts[kRing][kTsOps][2] = {};
  bool ok = false;
};
Base g_b;

void ReleaseQueriesB() {
  for (int r = 0; r < kRing; ++r) {
    RelP(g_b.dj[r]);
    for (auto& t : g_b.ts[r]) RelP(t[0]), RelP(t[1]);
  }
}
void MakeQueriesB(ID3D11Device* dev) {
  D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, qt{D3D11_QUERY_TIMESTAMP, 0};
  for (int r = 0; r < kRing; ++r) {
    dev->CreateQuery(&qd, &g_b.dj[r]);
    for (auto& t : g_b.ts[r]) dev->CreateQuery(&qt, &t[0]), dev->CreateQuery(&qt, &t[1]);
  }
}
void ReleaseBase() {
  for (auto*& c : g_b.cs) RelP(c);
  for (auto*& b : g_b.cb) RelP(b);
  for (auto*& b : g_b.psCb) RelP(b);
  RelP(g_b.statUav);
  RelP(g_b.stat);
  for (auto*& s : g_b.stage) RelP(s);
  ReleaseQueriesB();
  g_b = Base{};
}
bool MakeBase(ID3D11Device* dev) {
  ReleaseBase();
  for (int i = 0; i < kCsN; ++i)
    if (g_blob[i].empty() || FAILED(dev->CreateComputeShader(g_blob[i].data(), g_blob[i].size(), nullptr, &g_b.cs[i])))
      return false;
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = kCbBytes;  // R97: 128 (the FOV tangents and the rotation added)
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  D3D11_BUFFER_DESC pd = bd;
  pd.ByteWidth = 64;
  for (int v = 0; v < kViews; ++v)
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_b.cb[v])) || FAILED(dev->CreateBuffer(&pd, nullptr, &g_b.psCb[v])))
      return false;
  D3D11_BUFFER_DESC sd{};
  sd.ByteWidth = kViews * kStatU * 4;
  sd.Usage = D3D11_USAGE_DEFAULT;
  sd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
  ud.Format = DXGI_FORMAT_R32_UINT;
  ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  ud.Buffer.NumElements = kViews * kStatU;
  if (FAILED(dev->CreateBuffer(&sd, nullptr, &g_b.stat)) || FAILED(dev->CreateUnorderedAccessView(g_b.stat, &ud, &g_b.statUav)))
    return false;
  D3D11_BUFFER_DESC rd = sd;
  rd.Usage = D3D11_USAGE_STAGING;
  rd.BindFlags = 0;
  rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (auto*& s : g_b.stage)
    if (FAILED(dev->CreateBuffer(&rd, nullptr, &s))) return false;
  MakeQueriesB(dev);
  g_b.ok = true;
  return true;
}

// ---------------------------------------------------------------------------
// Render-thread state
// ---------------------------------------------------------------------------

// Per view, kept across frames: what is learnt (Field: pure, offline replayed) and DCS's HDR target of its overlay pass.
struct Learn : Field {
  ID3D11Resource* tex = nullptr;  // DCS's HDR target (reference)
  ID3D11ShaderResourceView* srv = nullptr;
  UINT sub = 0;
  uint32_t bx = 0, by = 0, bw = 0, bh = 0;
  bool tgtOk = false;
  ID3D11Resource* ldr = nullptr;  // the tone map's target (reference) and its UNORM view (FinalizeE reads it)
  ID3D11ShaderResourceView* ldrSrv = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;  // R99: the RTV the target was learnt through (reference: its pointer is unique)
};
Learn g_l[kViews];
AlphaHold g_alphaHold;  // render thread: emission_flags::alpha_valid as published
uint32_t g_alphaGen = 0;  // the generation g_alphaHold belongs to (R97: its latch lasts the generation)
csy::FocusLayout g_lastFocus{};  // the last frame's focus layout (a frame without poses publishes it again)
// Per view, this frame.
struct FrameView {
  bool seen = false;       // its OverlayHDRPass began this frame (the record is this frame's)
  bool armed = false;      // its replay's records are reported (window, scan or the target learnt)
  bool win = false, overflow = false, scan = false, learnOnly = false;
  bool hudOnly = false;    // R97: the helmet's runs are not accumulated (the union overflowed, the HUD alone fits)
  uint32_t wx = 0, wy = 0, ww = 0, wh = 0;  // the window (view pixels)
  bool tgtChecked = false, tgtBad = false;
  int cur = 0;             // the open run's type (0: none / COMMON)
  bool accInit = false;    // a type 1 / 2 run was accumulated
  int runs = 0;
  bool untouched = true, overlayEnded = false, preDlss = false;
  bool written = false;    // the tone map evaluated E
  Pose pose;               // the pose the window was chosen with
  uint8_t obsWinL = 0, obsViewL = 0, obsWinR = 0, obsViewR = 0;  // R101: the sides it showed (DecideWindow's)
  int toneOp = -1;         // R97: the timestamp pair of the tone-map draw with the second evaluation
};
FrameView g_fv[kViews];
uint64_t g_frameN = 0;     // frames begun with the symbology (render thread)
int64_t g_lastDt = 0;      // the last committed paired displayTime
Pose g_lastPose[kViews];   // the last committed submitted pose per view
uint32_t g_lastResetGen = 0;
bool g_posed = false;      // this frame's commit read DCS's submitted poses (g_lastPose is this frame's)
int g_watch = -1;          // the view whose OverlayHDRPass ended and whose ToneMappingPass has not begun
std::atomic<bool> g_fault{false};
// R97: the cost breakdown. GPU operations by kind (timestamps on every kTimeEvery-th frame only: R96's render thread
// paid two query Ends per operation and their GetData every frame); render-thread time by stage.
enum GpuCat : int { kGCopy, kGAcc, kGScan, kGFinal, kGTone, kGCats };
enum CpuCat : int { kCWin, kCRec, kCTone, kCCommit, kCEnd, kCCats };
constexpr uint64_t kTimeEvery = 8;
bool g_timed = false;       // this frame is timed (render thread)
// Statistics / timing ring (render thread).
struct RingMeta {
  bool pending = false, timed = false;
  uint64_t frame = 0;
  int ops = 0;
  uint8_t cat[kTsOps] = {};
  bool armed[kViews] = {}, seen[kViews] = {};
  StatMeta sm[kViews];
};
RingMeta g_ring[kRing];
int g_ringCur = 0;          // this frame's ring entry
bool g_ringOpen = false;    // its disjoint query began
int g_tsOps = 0;            // timestamped operations this frame
uint32_t g_voteWin[kViews][4] = {};  // R97: the windows of the last read-back (the alpha vote's "still")
bool g_voteWinOk = false;

// Statistics (any thread).
std::atomic<uint64_t> g_frames{0}, g_commits{0}, g_viewsPresent{0}, g_viewsWin{0}, g_overflows{0}, g_scans{0},
    g_runs{0}, g_noTarget{0}, g_tgtLearns{0}, g_preSame{0}, g_touched{0}, g_preDlss{0}, g_noTone{0}, g_edges{0},
    g_learnCol{0}, g_learnHel{0}, g_pupilFrames{0}, g_noRenderPoint{0}, g_predicted{0}, g_predictFallback{0},
    g_alphaValid{0}, g_alphaInvalid{0}, g_readbacks{0}, g_resets{0}, g_ringFull{0}, g_focusRepeats{0},
    g_hudOnlyViews{0}, g_alphaVotes[3] = {}, g_cbWrites{0}, g_recentViews{0};
std::atomic<ULONGLONG> g_lastCommitMs{0};
std::mutex g_statMu;
// under g_statMu: GPU us per timed frame by kind, render-thread us per frame by stage
double g_gpuCat[kGCats] = {}, g_gpuUsMax = 0, g_cpuCat[kCCats] = {}, g_cpuUsMax = 0;
uint64_t g_gpuN = 0, g_cpuN = 0;
double g_frameCpu[kCCats] = {};  // render thread: this frame's QPC sums
bool g_frameWork = false;        // render thread: this frame did symbology work (counted in the averages)

// Config-thread outputs read at the commit.
std::mutex g_qvMu;
csy::QvSettings g_qv{};           // under g_qvMu
std::atomic<uint32_t> g_qvGen{0};  // changes (0: never read)
std::atomic<bool> g_qvOk{false};
std::atomic<const dcsvr_pupil_fov::Block*> g_pupil{nullptr};  // the pupil shift layer's block (read-only view)
HANDLE g_pupilMap = nullptr;
std::wstring g_pupilName;

inline double QpcToUs() {
  static const double k = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart ? 1e6 / double(f.QuadPart) : 0.0;
  }();
  return k;
}
struct CpuScope {
  LARGE_INTEGER t0;
  int cat;
  explicit CpuScope(int c) : cat(c) { QueryPerformanceCounter(&t0); }
  ~CpuScope() {
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    g_frameCpu[cat] += (t1.QuadPart - t0.QuadPart) * QpcToUs();
    g_frameWork = true;
  }
};

std::atomic<bool> g_testRenderPoint{false};  // tests: the records are reported by the test (ckipd not installed)
inline bool RenderPoint() {
  return g_testRenderPoint.load(std::memory_order_relaxed) ||
         (ckipd::ocpu::Installed() && ckipd::ocpu::g_slotRender != nullptr);
}
// The frame writes the companion (cockpit_mask.h BeginFrame sets g_f.sym) and its objects exist.
inline bool Ready() {
  return g_f.begun && !g_f.skipped && g_f.slot >= 0 && g_f.sym && g_r.haveSym && g_b.ok &&
         !g_fault.load(std::memory_order_relaxed) && g_s.ctx;
}

// The release of DCS's targets (the uninstall's HoldRes / ReleaseAll, a device change).
void ReleaseTargets() {
  for (Learn& l : g_l) {
    RelP(l.rtv);  // R99
    RelP(l.srv);
    RelP(l.tex);
    RelP(l.ldrSrv);
    RelP(l.ldr);
    l.tgtOk = false;
  }
  ckipd::ocpu::g_symViews = 0;
}
// Everything learnt dropped (a mission event, the install). R97: the alpha flag's latch stays (it lasts the
// generation); its qualifying run restarts.
void ResetLearn() {
  for (Learn& l : g_l) static_cast<Field&>(l) = Field{};
  g_alphaHold.run = 0;
  g_voteWinOk = false;
  g_resets++;
}

// ---- GPU timing (one disjoint query per timed frame, a timestamp pair per operation) ----
void TsBegin(int& op, int cat) {
  op = -1;
  if (!g_timed) return;
  RingMeta& m = g_ring[g_ringCur];
  if (m.pending || !g_b.dj[g_ringCur] || g_tsOps >= kTsOps) return;
  if (!g_ringOpen) {
    g_s.ctx->Begin(g_b.dj[g_ringCur]);
    g_ringOpen = true;
  }
  op = g_tsOps++;
  m.cat[op] = static_cast<uint8_t>(cat);
  g_s.ctx->End(g_b.ts[g_ringCur][op][0]);
}
void TsEnd(int op) {
  if (op >= 0) g_s.ctx->End(g_b.ts[g_ringCur][op][1]);
}

// ---------------------------------------------------------------------------
// Render thread: the frame
// ---------------------------------------------------------------------------

// cockpit_mask.h BeginFrame (the frame's first capture): this frame's view records start empty.
void FrameBegin() {
  for (FrameView& f : g_fv) f = FrameView{};
  g_watch = -1;
  g_tsOps = 0;
  for (double& c : g_frameCpu) c = 0;
  g_frameWork = false;
  ckipd::ocpu::g_symViews = 0;
  if (g_f.sym) ++g_frameN;
  g_timed = g_frameN % kTimeEvery == 0;
}

// The pose the window is chosen with: the located views of the frame being rendered, else the last committed one.
// (The ring is read once per frame, at its first view: DCS locates a frame's four views in one call.)
uint64_t g_predFrame = ~0ull;
ctime::Rec g_predRec{};
bool g_predOk = false;
Pose PredictPose(int v) {
  if (g_predFrame != g_frameN) {
    g_predFrame = g_frameN;
    g_predOk = false;
    if (ctime::g_hooked.load(std::memory_order_relaxed)) {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      AcquireSRWLockShared(&ctime::g_lock);
      const int i = PickPredict(ctime::g_ring, ctime::kRing, g_lastDt, now.QuadPart);
      if (i >= 0) g_predRec = ctime::g_ring[i], g_predOk = true;
      ReleaseSRWLockShared(&ctime::g_lock);
    }
  }
  if (g_predOk && static_cast<uint32_t>(v) < g_predRec.vn) {
    g_predicted++;
    return PoseOf(g_predRec.vq[v], g_predRec.vfov[v]);
  }
  g_predictFallback++;
  return g_lastPose[v];
}

// The view's constants (R97: only written when they change: a still head keeps them).
void WriteConstants(int v, const FrameView& fv, const Learn& L, const Pack& pk) {
  const uint32_t tx = std::min((L.bw + kTile - 1) / kTile, kScanTiles), ty = std::min((L.bh + kTile - 1) / kTile, kScanTiles);
  const uint32_t edge = (fv.wx > 0 ? 1u : 0u) | (fv.wy > 0 ? 2u : 0u) | (fv.wx + fv.ww < L.bw ? 4u : 0u) |
                        (fv.wy + fv.wh < L.bh ? 8u : 0u);
  uint32_t p[32] = {L.bx + fv.wx, L.by + fv.wy, fv.ww, fv.wh, pk.x[v], pk.y[v], static_cast<uint32_t>(v), edge,
                    L.bx, L.by, L.bw, L.bh, tx, ty, 0, 0};
  const float tan[4] = {fv.pose.ok ? fv.pose.tan[0] : 0.0f, fv.pose.ok ? fv.pose.tan[1] : 0.0f,
                        fv.pose.ok ? fv.pose.tan[2] : 0.0f, fv.pose.ok ? fv.pose.tan[3] : 0.0f};
  memcpy(p + 16, tan, sizeof(tan));
  float rot[12] = {};
  if (fv.pose.ok) RotRows(fv.pose.q, rot);
  memcpy(p + 20, rot, sizeof(rot));
  if (!g_b.cbValid[v] || memcmp(p, g_b.cbLast[v], sizeof(p)) != 0) {
    g_s.ctx->UpdateSubresource(g_b.cb[v], 0, nullptr, p, 0, 0);
    memcpy(g_b.cbLast[v], p, sizeof(p));
    g_b.cbValid[v] = true;
    g_cbWrites++;
  }
  if (fv.win) {
    int32_t full[16] = {static_cast<int32_t>(L.bx + fv.wx), static_cast<int32_t>(L.by + fv.wy),
                        static_cast<int32_t>(fv.ww), static_cast<int32_t>(fv.wh), static_cast<int32_t>(pk.x[v]),
                        static_cast<int32_t>(pk.y[v]), g_symAlpha.load(std::memory_order_relaxed) ? 1 : 0, 0};
    if (!g_b.psValid[v] || memcmp(full, g_b.psLast[v], sizeof(full)) != 0) {
      g_s.ctx->UpdateSubresource(g_b.psCb[v], 0, nullptr, full, 0, 0);
      memcpy(g_b.psLast[v], full, sizeof(full));
      g_b.psValid[v] = true;
      g_cbWrites++;
    }
  }
}

// OverlayHDRPass begin of HMD view v (fixation_depth.h's boundary callback, first in the boundary).
void PassBegin(int v) {
  if (v < 0 || v >= kViews || !Ready()) return;
  if (!RenderPoint()) {
    g_noRenderPoint++;
    return;
  }
  CpuScope cpu(kCWin);
  FrameView& fv = g_fv[v];
  if (fv.seen) return;  // (a second OverlayHDRPass of the view in one frame: the first one's record stands)
  fv = FrameView{};
  fv.seen = true;
  if (const uint32_t rg = g_resetGen.load(std::memory_order_acquire); rg != g_lastResetGen) {
    g_lastResetGen = rg;
    ResetLearn();
  }
  Learn& L = g_l[v];
  const Pack pk = MakePack(g_r.sCap[0], g_r.sCap[1]);
  if (!L.tgtOk) {  // the target is learnt at the replay's first record; windows from the next frame on
    fv.armed = fv.learnOnly = true;
    ckipd::ocpu::g_symViews |= 1u << v;
    return;
  }
  fv.pose = PredictPose(v);
  const bool learnt = L.col.ok || HelmetExtent(L, g_frameN).ok;
  fv.scan = ScanDue(L, g_frameN, v, learnt);  // R101: + every kScanFallback frames in the fallback / overflow
  if (fv.scan) {
    L.scanReq = false;
    L.lastScan = g_frameN;
    g_scans++;
  }
  const WinOut w = DecideWindow(L, fv.pose, L.bw, L.bh, pk.cap[v], g_frameN);
  if (w.overflow) {
    fv.overflow = true;
    g_overflows++;
  } else if (w.any) {
    fv.win = true;
    fv.wx = w.x, fv.wy = w.y, fv.ww = w.w, fv.wh = w.h;
    fv.hudOnly = L.hudOnly;
    g_viewsWin++;
    if (fv.hudOnly) g_hudOnlyViews++;
    if (L.recent) g_recentViews++;
  }
  fv.obsWinL = L.obsWinL, fv.obsViewL = L.obsViewL, fv.obsWinR = L.obsWinR, fv.obsViewR = L.obsViewR;  // R101
  fv.armed = fv.win || fv.scan;
  if (!fv.armed) return;
  WriteConstants(v, fv, L, pk);
  ckipd::ocpu::g_symViews |= 1u << v;
}

// The first record of the view's replay: DCS's HDR target as bound (RTV 0) and the viewport. A new target or box is
// learnt; this frame then does nothing more for the view when the box's size changed (the window was chosen for the
// old one).
void CheckTarget(int v) {
  FrameView& fv = g_fv[v];
  Learn& L = g_l[v];
  fv.tgtChecked = true;
  ID3D11DeviceContext* c = g_s.ctx;
  ID3D11RenderTargetView* rtv = nullptr;
  c->OMGetRenderTargets(1, &rtv, nullptr);
  D3D11_VIEWPORT vp{};
  UINT nvp = 1;
  c->RSGetViewports(&nvp, &vp);
  const uint32_t bx = static_cast<uint32_t>(std::max(0.0f, vp.TopLeftX)), by = static_cast<uint32_t>(std::max(0.0f, vp.TopLeftY));
  const uint32_t bw = static_cast<uint32_t>(vp.Width), bh = static_cast<uint32_t>(vp.Height);
  // R99: the RTV the target was learnt through, the same box: nothing to look up (a view's resource and format are
  // fixed at its creation, and the reference held keeps its pointer from being reused).
  if (rtv && rtv == L.rtv && L.tgtOk && nvp && bx == L.bx && by == L.by && bw == L.bw && bh == L.bh) {
    rtv->Release();
    return;
  }
  ID3D11Resource* res = nullptr;
  D3D11_RENDER_TARGET_VIEW_DESC rd{};
  if (rtv) {
    rtv->GetResource(&res);
    rtv->GetDesc(&rd);
  }
  bool ok = res && nvp && bw && bh && rd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
            rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && rd.Texture2D.MipSlice == 0;
  if (ok && res == L.tex && L.tgtOk && bx == L.bx && by == L.by && bw == L.bw && bh == L.bh) {
    res->Release();
    RelP(L.rtv);  // R99: another RTV of the same target: remembered for the next frame's check
    L.rtv = rtv;
    return;
  }
  D3D11_TEXTURE2D_DESC td{};
  if (ok) {
    ID3D11Texture2D* t2 = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t2))) && t2) {
      t2->GetDesc(&td);
      t2->Release();
      ok = td.SampleDesc.Count == 1 && (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) && td.ArraySize == 1 &&
           bx + bw <= td.Width && by + bh <= td.Height && bw <= kScanTiles * kTile && bh <= kScanTiles * kTile;
    } else {
      ok = false;
    }
  }
  const bool sameSize = L.tgtOk && bw == L.bw && bh == L.bh && bx == L.bx && by == L.by;
  RelP(L.srv);
  RelP(L.tex);
  RelP(L.rtv);
  L.tgtOk = false;
  if (ok) {
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MostDetailedMip = 0;
    sd.Texture2D.MipLevels = 1;
    if (SUCCEEDED(g_r.dev->CreateShaderResourceView(res, &sd, &L.srv)) && L.srv) {
      L.tex = res;
      res = nullptr;  // the reference moves into L.tex
      L.sub = 0;
      if (!sameSize) ForgetHelmet(L);  // view pixels of another size
      L.bx = bx, L.by = by, L.bw = bw, L.bh = bh;
      L.tgtOk = true;
      L.rtv = rtv;  // R99: the reference moves into L.rtv
      rtv = nullptr;
      g_b.cbValid[v] = g_b.psValid[v] = false;
      g_tgtLearns++;
    }
  }
  if (res) res->Release();
  if (rtv) rtv->Release();
  if (!L.tgtOk) g_noTarget++;
  // A new target of the same box keeps this frame's window; anything else waits for the next frame.
  if (!(L.tgtOk && sameSize) || fv.learnOnly) fv.tgtBad = true;
}

// R99: one of our dispatches on DCS's context during its overlay replay or right after its tone-map draw, leaner than
// covl::RunOv (the profile, 5090 HW pipeline: RunOv 1.09 us of the records stage's 1.8 us per transition):
//  - DCS's target `hdr` is bound (the replay's RTV 0, CheckTarget; the tone map's RTV 0, LdrSrv), so the OM targets are
//    taken off and put back unconditionally: no GetResource / Release over the eight RTV slots to find the conflict
//    (with nothing bound the put-back is the same state, so this is exact either way);
//  - only the slots our shaders use are saved and set: b0 (b1 is not ours to clear), t0-t1, u0-u2 in one call each;
//  - no separate nulling of t0-t1 after the dispatch: the put-back of DCS's t0-t1 comes before the OM targets.
// Every binding touched is put back exactly, as RunOv (constant-buffer offsets through ID3D11DeviceContext1 included).
void RunSym(ID3D11ComputeShader* cs, ID3D11Buffer* cb, ID3D11ShaderResourceView* t0, ID3D11ShaderResourceView* t1,
            ID3D11UnorderedAccessView* u0, ID3D11UnorderedAccessView* u1, ID3D11UnorderedAccessView* u2, UINT gx, UINT gy,
            bool unbindOm) {
  ID3D11DeviceContext* c = g_s.ctx;
  ID3D11DeviceContext1* c1 = g_s.ctx1;
  ID3D11ComputeShader* sCs = nullptr;
  ID3D11ClassInstance* sInst[256];
  UINT nInst = 256;
  c->CSGetShader(&sCs, sInst, &nInst);
  if (nInst > 256) nInst = 256;
  ID3D11Buffer* sCb = nullptr;
  UINT cbFirst = 0, cbNum = 0;
  bool cbOff = false;
  if (c1) {
    c1->CSGetConstantBuffers1(0, 1, &sCb, &cbFirst, &cbNum);
    cbOff = sCb && !(cbFirst == 0 && (cbNum == 4096 || cbNum == 0));
  } else {
    c->CSGetConstantBuffers(0, 1, &sCb);
  }
  ID3D11ShaderResourceView* sSrv[2] = {};
  c->CSGetShaderResources(0, 2, sSrv);
  ID3D11UnorderedAccessView* sUav[3] = {};
  c->CSGetUnorderedAccessViews(0, 3, sUav);
  ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  if (unbindOm) {
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    c->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
  }
  const UINT keep[3] = {static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1)};
  c->CSSetShader(cs, nullptr, 0);
  c->CSSetConstantBuffers(0, 1, &cb);
  ID3D11UnorderedAccessView* us[3] = {u0, u1, u2};
  c->CSSetUnorderedAccessViews(0, 3, us, keep);  // before the SRVs (a target of DCS's left at a CS UAV is displaced)
  ID3D11ShaderResourceView* ts[2] = {t0, t1};
  c->CSSetShaderResources(0, 2, ts);
  c->Dispatch(gx, gy, 1);
  c->CSSetUnorderedAccessViews(0, 3, sUav, keep);
  c->CSSetShaderResources(0, 2, sSrv);
  if (cbOff && c1)
    c1->CSSetConstantBuffers1(0, 1, &sCb, &cbFirst, &cbNum);
  else
    c->CSSetConstantBuffers(0, 1, &sCb);
  c->CSSetShader(sCs, nInst ? sInst : nullptr, nInst);
  if (unbindOm) {
    UINT n = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
      if (rtv[i]) n = i + 1;
    c->OMSetRenderTargetsAndUnorderedAccessViews(n, rtv, dsv, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
    for (auto* r : rtv)
      if (r) r->Release();
    if (dsv) dsv->Release();
  }
  if (sCs) sCs->Release();
  for (UINT i = 0; i < nInst; ++i)
    if (sInst[i]) sInst[i]->Release();
  if (sCb) sCb->Release();
  for (auto* v : sSrv)
    if (v) v->Release();
  for (auto* u : sUav)
    if (u) u->Release();
}

// R97: a helmet (type 2) run is accumulated only outside the HUD-only fallback; its scan runs either way (the helmet's
// extent is learnt from the scans while it is left out).
inline bool AccRun(const FrameView& fv, int t) { return fv.win && (t == 1 || !fv.hudOnly); }
void RunStart(int v, int t) {
  FrameView& fv = g_fv[v];
  const Learn& L = g_l[v];
  ID3D11DeviceContext* c = g_s.ctx;
  int op;
  if (AccRun(fv, t)) {
    TsBegin(op, kGCopy);
    const D3D11_BOX b{L.bx + fv.wx, L.by + fv.wy, 0, L.bx + fv.wx + fv.ww, L.by + fv.wy + fv.wh, 1};
    c->CopySubresourceRegion(g_r.sPre, 0, 0, 0, 0, L.tex, L.sub, &b);
    TsEnd(op);
  }
  if (fv.scan) {
    TsBegin(op, kGScan);
    const uint32_t tx = std::min((L.bw + kTile - 1) / kTile, kScanTiles), ty = std::min((L.bh + kTile - 1) / kTile, kScanTiles);
    RunSym(g_b.cs[4], g_b.cb[v], L.srv, nullptr, g_r.sSigUav, g_b.statUav, nullptr, (tx + 7) / 8, (ty + 7) / 8, true);
    TsEnd(op);
  }
}
void RunEnd(int v, int t) {
  FrameView& fv = g_fv[v];
  const Learn& L = g_l[v];
  int op;
  if (AccRun(fv, t)) {
    TsBegin(op, kGAcc);
    RunSym(g_b.cs[(t == 2 ? 2 : 0) + (fv.accInit ? 1 : 0)], g_b.cb[v], L.srv, g_r.sPreSrv, g_r.sAccUav, g_b.statUav,
           g_r.scuav[g_f.slot], (fv.ww + 7) / 8, (fv.wh + 7) / 8, true);
    TsEnd(op);
    fv.accInit = true;
  }
  if (fv.scan) {
    TsBegin(op, kGScan);
    const uint32_t tx = std::min((L.bw + kTile - 1) / kTile, kScanTiles), ty = std::min((L.bh + kTile - 1) / kTile, kScanTiles);
    RunSym(g_b.cs[t == 2 ? 6 : 5], g_b.cb[v], L.srv, nullptr, g_r.sSigUav, g_b.statUav, nullptr, (tx + 7) / 8,
           (ty + 7) / 8, true);
    TsEnd(op);
  }
  ++fv.runs;
  g_runs++;
}

// ovl_cpu.h's render point: a record of view v's replay, before DCS draws it (c9: its indicator type, -1 unreadable).
void Record(int v, int c9) {
  if (v < 0 || v >= kViews) return;
  FrameView& fv = g_fv[v];
  const int t = (c9 == 1 || c9 == 2) ? c9 : 0;
  if (!fv.armed || (fv.tgtChecked && (fv.tgtBad || t == fv.cur))) return;  // most records: the same run (no QPC)
  if (!Ready()) return;
  CpuScope cpu(kCRec);
  if (!fv.tgtChecked) CheckTarget(v);
  if (fv.tgtBad) return;
  if (t == fv.cur) return;
  if (fv.cur) RunEnd(v, fv.cur);
  if (t) RunStart(v, t);
  fv.cur = t;
}
// The replay's end (ovl_cpu.h), and the pass end as a backstop: an open type 1 / 2 run ends.
void ReplayEnd(int v) {
  if (v < 0 || v >= kViews) return;
  FrameView& fv = g_fv[v];
  if (!fv.cur) return;
  if (fv.armed && !fv.tgtBad && Ready()) {
    CpuScope cpu(kCRec);
    RunEnd(v, fv.cur);
  }
  fv.cur = 0;
}

// fixation_depth.h's boundary callback, every kOther pass (begin and end) with the HMD ordinal at the time.
void Boundary(void* pass, bool begin, int v) {
  if (v < 0 || v >= kViews) return;
  char n[48];
  PassName(pass, n, sizeof(n));
  if (begin) {
    if (strcmp(n, "OverlayHDRPass") == 0) {
      PassBegin(v);
      return;
    }
    if (strcmp(n, "OverlayHDRPassDLSS") == 0) {  // HUD_MFD_after_DLSS = false: the HUD went through DLSS
      g_fv[v].preDlss = true;
      return;
    }
    if (g_watch == v) {
      if (strcmp(n, "ToneMappingPass") == 0) {
        g_watch = -1;
      } else if (!strstr(n, "Histogram") && !strstr(n, "Luminance") && !strstr(n, "Adaptation") && !strstr(n, "Exposure")) {
        g_fv[v].untouched = false;  // a pass that may write the HDR between the overlay and the tone map (NVG?)
      }
    }
    return;
  }
  if (strcmp(n, "OverlayHDRPass") == 0 && g_fv[v].seen && !g_fv[v].overlayEnded) {
    ReplayEnd(v);
    g_fv[v].overlayEnded = true;
    ckipd::ocpu::g_symViews &= ~(1u << v);
    g_watch = v;
  }
}
// A GBufferPass began with an overlay pass of ours still open (its end never came).
void Abort() {
  for (int v = 0; v < kViews; ++v)
    if (g_fv[v].cur) g_fv[v].cur = 0;
  ckipd::ocpu::g_symViews = 0;
  g_watch = -1;
}

// color_pass.h, before view v's tone-map draw: its bindings when the view has E to evaluate this frame. t0: the
// draw's HDR source (DCS's tone map reads the target the overlay pass drew into: R94 pre_same).
bool ToneQuery(int v, ID3D11Resource* t0, cpass::SymBind* out) {
  if (v < 0 || v >= kViews || !Ready()) return false;
  FrameView& fv = g_fv[v];
  if (!fv.win || !fv.accInit || fv.tgtBad) return false;
  if (!fv.untouched) {
    g_touched++;
    return false;
  }
  if (fv.preDlss) {
    g_preDlss++;
    return false;
  }
  if (!t0 || t0 != g_l[v].tex) {
    g_preSame++;
    return false;
  }
  out->cb = g_b.psCb[v];
  out->acc = g_r.sAccSrv;
  out->out = g_r.suav[g_f.slot];
  return true;
}
// R97: color_pass.h, right before the draw with the second evaluation (its slots bound): the draw's GPU time.
void ToneDrawBegin(int v) {
  if (v < 0 || v >= kViews || !g_timed || !Ready() || !g_fv[v].win) return;
  TsBegin(g_fv[v].toneOp, kGTone);
}
// E = dec(F) - dec(C) over the window (cb: the view's compute constants; ldr: DCS's target, UNORM view). R97: only at
// the texels the class atlas marks (a type 1 / 2 run changed them; acc != 0 implies it): the 16-byte accumulator is
// read at ~8 % of the window instead of all of it.
void FinalizeE(ID3D11Buffer* cb, ID3D11ShaderResourceView* ldr, ID3D11Resource* ldrRes, ID3D11ShaderResourceView* acc,
               ID3D11UnorderedAccessView* atlas, ID3D11UnorderedAccessView* cls, uint32_t w, uint32_t h) {
  RunSym(g_b.cs[7], cb, ldr, acc, atlas, cls, nullptr, (w + 7) / 8, (h + 7) / 8, ldrRes != nullptr);  // R99
}
std::atomic<uint64_t> g_finalized{0}, g_noLdr{0};
// The target the tone map wrote (RTV 0, still bound right after the draw) as a UNORM shader resource.
ID3D11ShaderResourceView* LdrSrv(int v, ID3D11Resource** resOut) {
  Learn& L = g_l[v];
  ID3D11RenderTargetView* rtv = nullptr;
  g_s.ctx->OMGetRenderTargets(1, &rtv, nullptr);
  if (!rtv) return nullptr;
  ID3D11Resource* res = nullptr;
  rtv->GetResource(&res);
  rtv->Release();
  if (!res) return nullptr;
  if (res != L.ldr) {
    RelP(L.ldrSrv);
    RelP(L.ldr);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(g_r.dev->CreateShaderResourceView(res, &sd, &L.ldrSrv)) || !L.ldrSrv) {
      res->Release();
      return nullptr;
    }
    L.ldr = res;
    res->AddRef();
  }
  *resOut = res;
  res->Release();  // (L.ldr holds it)
  return L.ldrSrv;
}
// color_pass.h, right after the swapped draw and its slots back: E finalized from DCS's bytes (else no E this frame).
void ToneDone(int v, bool written) {
  if (v < 0 || v >= kViews) return;
  FrameView& fv = g_fv[v];
  TsEnd(fv.toneOp);
  fv.toneOp = -1;
  fv.written = false;
  if (!written || !fv.win || !Ready()) return;
  CpuScope cpu(kCTone);
  ID3D11Resource* ldrRes = nullptr;
  ID3D11ShaderResourceView* ldr = LdrSrv(v, &ldrRes);
  if (!ldr) {
    g_noLdr++;
    return;
  }
  int op;
  TsBegin(op, kGFinal);
  FinalizeE(g_b.cb[v], ldr, ldrRes, g_r.sAccSrv, g_r.suav[g_f.slot], g_r.scuav[g_f.slot], fv.ww, fv.wh);
  TsEnd(op);
  fv.written = true;
  g_finalized++;
}

// ---- Learning from a read-back frame ----
void ProcessStats(const RingMeta& m, const uint32_t* w) {
  g_readbacks++;
  bool seen[kViews] = {}, win[kViews] = {}, same[kViews] = {};
  int verdict[kViews] = {};
  for (int v = 0; v < kViews; ++v) {
    seen[v] = m.seen[v];
    const StatMeta& sm = m.sm[v];
    win[v] = m.armed[v] && sm.win;
    same[v] = g_voteWinOk && g_voteWin[v][0] == sm.wx && g_voteWin[v][1] == sm.wy && g_voteWin[v][2] == sm.ww &&
              g_voteWin[v][3] == sm.wh;
    g_voteWin[v][0] = sm.wx, g_voteWin[v][1] = sm.wy, g_voteWin[v][2] = sm.ww, g_voteWin[v][3] = sm.wh;
    if (!m.armed[v]) continue;
    Learn& L = g_l[v];
    if (sm.W != L.bw || sm.H != L.bh) {  // another box since: the pixels mean something else
      win[v] = false;
      continue;
    }
    LearnCount n;
    verdict[v] = LearnStats(L, sm, w + v * kStatU, n);
    g_learnCol += n.col, g_learnHel += n.hel, g_edges += n.edges;
  }
  g_voteWinOk = true;
  const int vote = AlphaFrameVote(seen, win, verdict, same);
  g_alphaVotes[vote + 1]++;
  AlphaVote(g_alphaHold, vote);
}

// The frame end (cockpit_mask.h Commit, every frame, committed or not): the statistics to the ring, older ones read.
void FrameEnd(bool committed) {
  LARGE_INTEGER t0;
  QueryPerformanceCounter(&t0);
  ID3D11DeviceContext* c = g_s.ctx;
  ckipd::ocpu::g_symViews = 0;
  g_watch = -1;
  bool any = false;
  for (const FrameView& f : g_fv) any = any || f.armed;
  if (c && g_b.ok && any) {
    RingMeta& m = g_ring[g_ringCur];
    if (!m.pending) {
      const uint8_t* cat = m.cat;  // (written by TsBegin this frame)
      uint8_t keep[kTsOps];
      memcpy(keep, cat, sizeof(keep));
      m = RingMeta{};
      memcpy(m.cat, keep, sizeof(keep));
      m.pending = true;
      m.timed = g_ringOpen;
      m.ops = g_tsOps;
      m.frame = g_frameN;
      for (int v = 0; v < kViews; ++v) {
        const FrameView& f = g_fv[v];
        StatMeta& s = m.sm[v];
        m.armed[v] = f.armed && !f.tgtBad && !f.learnOnly;
        m.seen[v] = f.seen;
        s.win = f.win, s.scan = f.scan, s.hudOnly = f.hudOnly;
        s.angOk = f.pose.ok;
        s.pose = f.pose;
        memcpy(s.q, f.pose.q, sizeof(s.q));
        s.W = g_l[v].bw, s.H = g_l[v].bh;
        s.frame = g_frameN;
        s.wx = f.win ? f.wx : 0, s.wy = f.win ? f.wy : 0, s.ww = f.win ? f.ww : 0, s.wh = f.win ? f.wh : 0;
        s.obsWinL = f.obsWinL, s.obsViewL = f.obsViewL, s.obsWinR = f.obsWinR, s.obsViewR = f.obsViewR;  // R101
        // The orientation the frame was rendered with: the submitted one (the commit's) when it was read
        if (committed && g_posed && g_lastPose[v].ok && f.seen) {
          memcpy(s.q, g_lastPose[v].q, sizeof(s.q));
          if (!s.angOk) s.pose = g_lastPose[v];
          else memcpy(s.pose.q, g_lastPose[v].q, sizeof(s.q));
        }
      }
      c->CopyResource(g_b.stage[g_ringCur], g_b.stat);
      if (g_ringOpen) c->End(g_b.dj[g_ringCur]);
      g_ringCur = (g_ringCur + 1) % kRing;
    } else {
      g_ringFull++;
      if (g_ringOpen) c->End(g_b.dj[g_ringCur]);  // (closed; its stamps are lost)
    }
    const UINT zero[4] = {};
    c->ClearUnorderedAccessViewUint(g_b.statUav, zero);
  }
  g_ringOpen = false;
  g_tsOps = 0;
  g_posed = false;
  // Older frames, oldest first, never waiting: their statistics (learning) and timing. R97: the first one not ready
  // ends the poll (the later ones were submitted after it).
  if (c && g_b.ok)
    for (int k = 0; k < kRing; ++k) {
      const int i = (g_ringCur + k) % kRing;  // the oldest entry first; the newest (this frame's) is not polled
      RingMeta& m = g_ring[i];
      if (!m.pending || i == (g_ringCur + kRing - 1) % kRing) continue;
      D3D11_MAPPED_SUBRESOURCE mp{};
      if (c->Map(g_b.stage[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mp) != S_OK) break;
      uint32_t w[kViews * kStatU];
      memcpy(w, mp.pData, sizeof(w));
      c->Unmap(g_b.stage[i], 0);
      double us[kGCats] = {};
      bool timed = false;
      if (m.timed) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (c->GetData(g_b.dj[i], &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !dj.Disjoint && dj.Frequency) {
          timed = true;
          for (int k2 = 0; k2 < m.ops && timed; ++k2) {
            UINT64 a = 0, b = 0;
            if (c->GetData(g_b.ts[i][k2][0], &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                c->GetData(g_b.ts[i][k2][1], &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
              timed = false;
            else
              us[m.cat[k2] < kGCats ? m.cat[k2] : 0] += (b >= a ? b - a : 0) * 1e6 / static_cast<double>(dj.Frequency);
          }
        }
      }
      if (timed) {
        std::lock_guard<std::mutex> lock(g_statMu);
        double tot = 0;
        for (int k2 = 0; k2 < kGCats; ++k2) g_gpuCat[k2] += us[k2], tot += us[k2];
        g_gpuUsMax = std::max(g_gpuUsMax, tot);
        ++g_gpuN;
      }
      ProcessStats(m, w);
      m.pending = false;
    }
  LARGE_INTEGER t1;
  QueryPerformanceCounter(&t1);
  // The colour pass's binding of our slots around the draw (color_pass.h, this frame) belongs to the tone stage.
  const int64_t bind = cpass::g_symBindQpc;
  cpass::g_symBindQpc = 0;
  if (any || g_frameWork) {
    g_frameCpu[kCEnd] += (t1.QuadPart - t0.QuadPart) * QpcToUs();
    g_frameCpu[kCTone] += bind * QpcToUs();
    std::lock_guard<std::mutex> lock(g_statMu);
    double tot = 0;
    for (int k = 0; k < kCCats; ++k) g_cpuCat[k] += g_frameCpu[k], tot += g_frameCpu[k];
    g_cpuUsMax = std::max(g_cpuUsMax, tot);
    ++g_cpuN;
  }
  for (double& x : g_frameCpu) x = 0;
  g_frameWork = false;
  for (FrameView& f : g_fv) f = FrameView{};
}

// A consistent copy of the pupil shift layer's block (its seqlock); false: none, or being written.
bool PupilCopy(dcsvr_pupil_fov::Block& out) {
  const dcsvr_pupil_fov::Block* b = g_pupil.load(std::memory_order_acquire);
  if (!b) return false;
  auto& seq = *reinterpret_cast<const std::atomic<uint64_t>*>(&b->seq);
  for (int i = 0; i < 3; ++i) {
    const uint64_t s0 = seq.load(std::memory_order_acquire);
    if (s0 == 0 || (s0 & 1)) continue;
    memcpy(&out, b, sizeof(out));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq.load(std::memory_order_relaxed) == s0) return true;
  }
  return false;
}

// The commit (cockpit_mask.h Commit, before the depth and mask headers): the slot of this frame. True when written.
bool Commit(const Frame& f, uint64_t seq, int64_t displayTime, bool paired, const XrFrameInfo* xr) {
  if (!f.sym || !g_sview.load()) return false;
  CpuScope cpu(kCCommit);
  csy::EmissionSlot& s = g_shdr.slot[f.slot];
  s = csy::EmissionSlot{};
  const Pack pk = MakePack(g_r.sCap[0], g_r.sCap[1]);
  // The FOVs: DCS's submitted (the image's); for composition the pupil shift layer's originals when it has them.
  const bool poses = paired && xr && xr->poses && xr->viewCount > 0;
  const uint32_t nv = poses ? std::min<uint32_t>(xr->viewCount, 4) : 0;
  float pubFov[4][4] = {};
  bool pupil = false;
  if (nv) {
    for (uint32_t v = 0; v < nv; ++v) memcpy(pubFov[v], xr->fov[v], sizeof(pubFov[v]));
    static dcsvr_pupil_fov::Block pb;  // (4.7 KB: not on the stack of DCS's render thread)
    float orig[4][4];
    if (displayTime && PupilCopy(pb) && PupilOriginal(pb, displayTime, xr->fov, nv, orig)) {
      memcpy(pubFov, orig, sizeof(float) * 4 * nv);
      pupil = true;
      g_pupilFrames++;
    }
    for (uint32_t v = 0; v < nv; ++v) g_lastPose[v] = PoseOf(xr->q[v], xr->fov[v]);  // the image's pose and FOV
    g_posed = true;
  }
  float pubTan[4][4] = {};
  for (uint32_t v = 0; v < nv; ++v)
    for (int k = 0; k < 4; ++k) pubTan[v][k] = static_cast<float>(std::tan(static_cast<double>(pubFov[v][k])));
  bool partial = false;
  for (int v = 0; v < kViews; ++v) {
    const FrameView& fv = g_fv[v];
    if (!fv.seen) continue;
    const Learn& L = g_l[v];
    csy::EmissionView& m = s.view[v];
    m.flags = csy::view_flags::valid | (fv.untouched ? csy::view_flags::untouched_hdr : 0u) |
              (fv.preDlss ? csy::view_flags::pre_dlss_hud : 0u) | (fv.overflow ? csy::view_flags::overflow : 0u);
    m.x = pk.x[v], m.y = pk.y[v], m.capW = m.capH = pk.cap[v];
    m.outW = L.tgtOk ? L.bw : 0, m.outH = L.tgtOk ? L.bh : 0;
    if (fv.written && fv.win) {
      m.flags |= csy::view_flags::present | csy::view_flags::own_tonemap;
      m.winX = fv.wx, m.winY = fv.wy, m.winW = fv.ww, m.winH = fv.wh;
      g_viewsPresent++;
      (L.alpha > 0 ? g_alphaValid : g_alphaInvalid)++;
    } else if (fv.win && fv.accInit) {
      partial = true;  // E was due and the tone map did not evaluate it
      g_noTone++;
    }
    if (static_cast<uint32_t>(v) < nv) memcpy(m.fovTan, pubTan[v], sizeof(m.fovTan));
    s.viewMask |= 1u << v;
  }
  // The focus layout always (Dev's 0049 skips a slot without one): this frame's, else the last one again.
  if (nv == 4 && MakeLayout(pubTan, s.focus)) {
    s.focus.fovSource = pupil ? csy::fov_source_pupil_original : csy::fov_source_dcs_submitted;
    g_lastFocus = s.focus;
  } else if (g_lastFocus.flags & csy::layout_flags::layout_valid) {
    s.focus = g_lastFocus;
    g_focusRepeats++;
  }
  s.seq = seq;
  s.engineFrame = f.engineFrame;
  s.displayTime = paired ? displayTime : 0;
  s.flags = csy::slot_flags::valid | (paired && displayTime ? csy::slot_flags::display_time : 0u) |
            (paired ? 0u : csy::slot_flags::late_commit) | (partial ? csy::slot_flags::partial : 0u);
  g_shdr.newestSeq = seq;
  g_shdr.newestSlot = static_cast<uint32_t>(f.slot);
  g_shdr.fovSource = pupil ? csy::fov_source_pupil_original : csy::fov_source_dcs_submitted;
  // The coverage alpha's validity (header; R97: complete still read-backs only, latched off for the generation).
  if (g_shdr.generation != g_alphaGen) {
    g_alphaGen = g_shdr.generation;
    AlphaReset(g_alphaHold);
  }
  if (g_symAlpha.load(std::memory_order_relaxed) && g_alphaHold.on)
    g_shdr.emissionFlags |= csy::emission_flags::alpha_valid;
  else
    g_shdr.emissionFlags &= ~csy::emission_flags::alpha_valid;
  if (g_qvOk.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(g_qvMu);
    g_shdr.qv = g_qv;
    g_shdr.flags |= csy::header_flags::qv_settings;
  } else {  // no file of Quad Views' read: the reader applies its defaults (0.2 / 0)
    g_shdr.qv = csy::QvSettings{};
    g_shdr.flags &= ~csy::header_flags::qv_settings;
  }
  PublishSymHeader();
  if (paired && displayTime) g_lastDt = displayTime;
  g_commits++;
  g_lastCommitMs = GetTickCount64();
  return true;
}

// ---------------------------------------------------------------------------
// Config thread (cockpit_mask.h Tick, once per second): Quad Views' settings and the pupil shift layer's block
// ---------------------------------------------------------------------------
struct FileStamp {
  std::wstring path;
  FILETIME t{};
  uint64_t size = 0;
  bool exists = false;
};
FileStamp g_qvFiles[2];
ULONGLONG g_qvLastCheck = 0;

inline FileStamp StampOf(const std::wstring& p) {
  FileStamp s;
  s.path = p;
  WIN32_FILE_ATTRIBUTE_DATA a{};
  if (!p.empty() && GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) &&
      !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
    s.exists = true;
    s.t = a.ftLastWriteTime;
    s.size = (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
  }
  return s;
}
inline bool SameStamp(const FileStamp& a, const FileStamp& b) {
  return a.path == b.path && a.exists == b.exists && a.size == b.size && a.t.dwLowDateTime == b.t.dwLowDateTime &&
         a.t.dwHighDateTime == b.t.dwHighDateTime;
}
inline std::string ReadSmallFile(const std::wstring& p) {
  std::string s;
  FILE* f = nullptr;
  if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || !f) return s;
  char b[4096];
  size_t n;
  while ((n = fread(b, 1, sizeof(b), f)) > 0 && s.size() < (1u << 20)) s.append(b, n);
  fclose(f);
  return s;
}
// Quad Views' two configuration files, in its order: the layer's own folder, then %DCSVR_QUAD_SETTINGS% (or
// %LocalAppData%\Quad-Views-Foveated\settings.cfg without it). profile: the environment variable named the second.
inline void QvPaths(std::wstring out[2], bool* profile) {
  out[0].clear(), out[1].clear();
  *profile = false;
  if (HMODULE m = GetModuleHandleW(L"XR_APILAYER_MBUCCHIA_quad_views_foveated.dll")) {
    wchar_t p[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(m, p, MAX_PATH);
    if (n && n < MAX_PATH) {
      std::wstring d(p);
      const size_t sl = d.find_last_of(L"\\/");
      if (sl != std::wstring::npos) out[0] = d.substr(0, sl + 1) + L"settings.cfg";
    }
  }
  wchar_t e[32768];
  const DWORD n = GetEnvironmentVariableW(L"DCSVR_QUAD_SETTINGS", e, 32768);
  if (n > 0 && n < 32768) {
    out[1] = e;
    *profile = true;
  } else {
    const DWORD k = GetEnvironmentVariableW(L"LOCALAPPDATA", e, 32768);
    if (k > 0 && k < 32768) out[1] = std::wstring(e) + L"\\Quad-Views-Foveated\\settings.cfg";
  }
}
inline std::string ExeName() {
  wchar_t p[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
  std::wstring w(p, n);
  const size_t sl = w.find_last_of(L"\\/");
  if (sl != std::wstring::npos) w = w.substr(sl + 1);
  std::string s;
  for (wchar_t c : w) s += static_cast<char>(c < 128 ? c : '?');
  return s;
}
// force: re-read whatever the files' stamps say (the install).
void TickQv(bool force) {
  const ULONGLONG now = GetTickCount64();
  if (!force && now - g_qvLastCheck < 1000) return;
  g_qvLastCheck = now;
  std::wstring p[2];
  bool profile = false;
  QvPaths(p, &profile);
  FileStamp st[2] = {StampOf(p[0]), StampOf(p[1])};
  if (!force && SameStamp(st[0], g_qvFiles[0]) && SameStamp(st[1], g_qvFiles[1])) return;
  g_qvFiles[0] = st[0], g_qvFiles[1] = st[1];
  QvVals v;
  const std::string exe = ExeName();
  std::string app = exe;  // DCS's XrApplicationInfo name is not visible here: [app:x] matches the executable's name
  const size_t dot = app.rfind('.');
  if (dot != std::string::npos) app = app.substr(0, dot);
  for (int i = 0; i < 2; ++i)
    if (st[i].exists) QvParseText(ReadSmallFile(p[i]), app, exe, v);
  const csy::QvSettings s = QvSettingsOf(v, profile && st[1].exists);
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(g_qvMu);
    const uint32_t gen = g_qvGen.load();
    csy::QvSettings ns = s;
    ns.settingsGen = gen;
    changed = memcmp(&ns, &g_qv, sizeof(ns)) != 0;
    if (changed) {
      ns.settingsGen = gen + 1;
      g_qv = ns;
      g_qvGen = gen + 1;
    }
  }
  g_qvOk = st[0].exists || st[1].exists;
  if (changed)
    Log("symbology: Quad Views settings: sharpen_focus_view %.2f, dcsvr_periphery_sharpen %.2f, "
        "smoothen_focus_view_edges %.2f, focus_view_shape %.1f, flags 0x%x (files: layer %s, %s %s)",
        s.sharpenFocusView, s.peripherySharpen, s.smoothenFocusViewEdges, s.focusViewShape, s.flags,
        st[0].exists ? "read" : "absent", profile ? "profile" : "LocalAppData", st[1].exists ? "read" : "absent");
}
void TickPupil() {
  if (g_pupil.load()) return;
  static ULONGLONG last = 0;
  const ULONGLONG now = GetTickCount64();
  if (now - last < 1000) return;
  last = now;
  if (g_pupilName.empty()) g_pupilName = std::wstring(dcsvr_pupil_fov::block_name) + perc_sfx::SuffixW();
  HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, g_pupilName.c_str());
  if (!m) return;
  void* v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(dcsvr_pupil_fov::Block));
  if (!v) {
    CloseHandle(m);
    return;
  }
  g_pupilMap = m;
  g_pupil.store(static_cast<const dcsvr_pupil_fov::Block*>(v), std::memory_order_release);
  Log("symbology: %ls mapped (the pupil shift layer's original FOVs for the focus layout)", g_pupilName.c_str());
}
// R97: the record render point's state, logged once it is known (R96 §2.1: the install line said "NOT hooked" because
// ovl_cpu.h's install ran after it in the same millisecond). Hooked: logged at the first config tick that sees it;
// not hooked: logged once after kRpGraceMs (the dual camera's install has had its chance), and again on any change.
constexpr ULONGLONG kRpGraceMs = 10000;
std::atomic<int> g_rpLogged{-1};  // -1 nothing logged since the install, 0 / 1 the state logged
ULONGLONG g_rpSince = 0;
void NoteInstall() {
  g_rpLogged = -1;
  g_rpSince = GetTickCount64();
}
void TickRenderPoint() {
  const int now = RenderPoint() ? 1 : 0;
  const int last = g_rpLogged.load();
  if (now == last) return;
  if (!now && last < 0 && GetTickCount64() - g_rpSince < kRpGraceMs) return;
  g_rpLogged = now;
  if (now)
    Log("symbology: record render point hooked (ovl_cpu.h, the cockpit dual camera's cpu overlay path): E per view");
  else
    Log("symbology: record render point NOT hooked (needs CockpitTrueIpd with the cpu overlay path)%s: no E",
        last == 1 ? " any more" : "");
}

// R97: the export's VRAM in MB (decimal), for its creation line.
struct Vram {
  double e = 0, cls = 0, shared = 0, pre = 0, acc = 0, sig = 0, priv = 0, stats = 0, total = 0;
  uint32_t snap = 0;
};
inline Vram VramOf(uint32_t atlasW, uint32_t atlasH, uint32_t capP, uint32_t capF) {
  Vram v;
  const double MB = 1e6;
  v.e = double(atlasW) * atlasH * 8 / MB;
  v.cls = double(atlasW) * atlasH / MB;
  v.shared = csy::slot_count * (v.e + v.cls);
  v.snap = std::max(capP, capF);
  v.pre = double(v.snap) * v.snap * 8 / MB;
  v.acc = double(v.snap) * v.snap * 16 / MB;
  v.sig = double(kScanTiles) * kScanTiles * 4 / MB;
  v.priv = v.pre + v.acc + v.sig;
  v.stats = double(kViews) * kStatU * 4 * (1 + kRing) / MB + double(kViews) * (kCbBytes + 64) / MB;
  v.total = v.shared + v.priv + v.stats;
  return v;
}

// The uninstall (config thread, after the render thread left the module): the pupil block unmapped.
void ClosePupil() {
  if (const dcsvr_pupil_fov::Block* b = g_pupil.exchange(nullptr)) UnmapViewOfFile(b);
  if (g_pupilMap) CloseHandle(g_pupilMap);
  g_pupilMap = nullptr;
  g_pupilName.clear();
}

// extra (R97): the cost breakdown and the alpha votes (a second log line: the first stays R96's format).
std::string StatsText(std::string* extra = nullptr) {
  auto u = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load()); };
  double g[kGCats] = {}, c[kCCats] = {}, gMax, cMax;
  unsigned long long gN, cN;
  {
    std::lock_guard<std::mutex> lock(g_statMu);
    for (int k = 0; k < kGCats; ++k) g[k] = g_gpuN ? g_gpuCat[k] / double(g_gpuN) : 0.0, g_gpuCat[k] = 0;
    for (int k = 0; k < kCCats; ++k) c[k] = g_cpuN ? g_cpuCat[k] / double(g_cpuN) : 0.0, g_cpuCat[k] = 0;
    gMax = g_gpuUsMax, cMax = g_cpuUsMax;
    gN = g_gpuN, cN = g_cpuN;
    g_gpuUsMax = g_cpuUsMax = 0;
    g_gpuN = g_cpuN = 0;
  }
  const double gOps = g[kGCopy] + g[kGAcc] + g[kGScan] + g[kGFinal];
  double cTot = 0;
  for (double x : c) cTot += x;
  char b[1100], x[900];
  sprintf_s(b,
            "symbology: %llu commits, views present %llu (window %llu, overflow %llu), alpha valid %llu / not %llu; "
            "type runs %llu, scans %llu, learnt rectangle %llu / helmet %llu, edge hits %llu, read-backs %llu; not "
            "evaluated: no tone map %llu, HDR touched before the tone map %llu, another tone-map source %llu, pre-DLSS "
            "HUD %llu; no render point %llu, no target %llu (learnt %llu); pose located %llu / last commit %llu; "
            "pupil FOVs %llu; focus layout repeated %llu; coverage alpha %s; resets %llu; ring full %llu; cost GPU %.1f "
            "us/frame avg / %.1f max (%llu timed; copies and dispatches; the tone-map draws with the second evaluation "
            "below), render thread %.1f us/frame avg / %.1f max (%llu frames)",
            u(g_commits), u(g_viewsPresent), u(g_viewsWin), u(g_overflows), u(g_alphaValid), u(g_alphaInvalid), u(g_runs),
            u(g_scans), u(g_learnCol), u(g_learnHel), u(g_edges), u(g_readbacks), u(g_noTone), u(g_touched),
            u(g_preSame), u(g_preDlss), u(g_noRenderPoint), u(g_noTarget), u(g_tgtLearns), u(g_predicted),
            u(g_predictFallback), u(g_pupilFrames), u(g_focusRepeats), g_alphaHold.on ? "valid" : "not valid",
            u(g_resets), u(g_ringFull), gOps, gMax, gN, cTot, cMax, cN);
  sprintf_s(x,
            "symbology cost (R97): GPU by kind (timed frames) copies %.1f, accumulation %.1f, scans %.1f, finalize %.1f, "
            "tone-map draws with the second evaluation %.1f (whole draws; they replace DCS's own draw), export + draws "
            "%.1f us/frame; render thread by stage windows %.1f, records %.1f, tone %.1f, commit %.1f, frame end %.1f "
            "us/frame; constant writes %llu; HUD-only view-frames %llu; coverage alpha votes valid / neutral / invalid "
            "%llu / %llu / %llu, flips %u%s; recent-field view-frames %llu (R101)",
            g[kGCopy], g[kGAcc], g[kGScan], g[kGFinal], g[kGTone], gOps + g[kGTone], c[kCWin], c[kCRec], c[kCTone],
            c[kCCommit], c[kCEnd], u(g_cbWrites), u(g_hudOnlyViews), u(g_alphaVotes[2]), u(g_alphaVotes[1]),
            u(g_alphaVotes[0]), g_alphaHold.flips, g_alphaHold.latched ? ", latched off for this generation" : "",
            u(g_recentViews));
  if (extra) *extra = x;
  return b;
}

}  // namespace sym
