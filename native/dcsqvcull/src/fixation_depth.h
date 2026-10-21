// Fixation depth publisher for Depth boost ([Perception] FixationDepth,
// default 0; design: dlss5vrframegen docs/FIXATION_DEPTH.md, read-only here).
// Not a performance optimization: it is outside the kill switch and is not
// one of the status word's 18 features.
//
// Interface (named file mappings in the session, both #pragma pack(8),
// little-endian, seq odd while writing and even when stable):
//  - reads  Local\DcsVrPerceptionGaze_v1  (GazeOut, the pupil shift layer
//    writes it at each xrLocateViews);
//  - writes Local\DcsVrFixationDepth_v1   (DepthIn, one record per rendered
//    frame, in frame order).
//  - writes Local\DcsVrDepthGrid_v1 ([Perception] DepthGrid, independent of
//    FixationDepth; DepthGrid struct below): a 32 x 32 grid over the whole
//    left periphery view (HMD view 0), read after the water passes like the
//    cone, for the layer's saccade landing prediction. One dispatch of
//    32 x 32 groups reduces every texel of that view (476 x 468 with DLSS:
//    about 15 x 15 texels per cell): nearestM = min distance, typicalM =
//    geometric mean (log-mean) of the distances, nearShare = share under
//    1.5 m (0..255), water bit per cell. 12 KB copied to a 3-slot staging
//    ring, mapped DO_NOT_WAIT >= 2 frames later (the grid is 2 frames old
//    when published), every frame. fovTan from DCS's projection (signed).
//
// Per frame, on the render thread (pass_timing.h's g_depthBoundary):
//  1. Views. DCS renders each main view's G-buffer as a run of GBufferPass
//     executions (cockpit, main, decal; R24 1.2) on the view's own depth
//     target. That DSV and its viewport are bound only inside the pass (see
//     the in-pass capture before CaptureRun); at each GBufferPass end they
//     and the G-buffer's cPerView constant buffer (VS b7, else PS b7) are
//     taken; consecutive passes on the same DSV and viewport are one run. A
//     run closes at the next pass that is not a GBufferPass.
//     When it is sampled: DCS's water passes draw water into the same depth
//     target with depth writes on and stencil composition WATER
//     (Bazar/shaders/enlight/waterCommon.hlsl Water_DepthStencilState,
//     common/stencil.hlsl), after the G-buffer and before the lighting and
//     every transparent pass (pass trace: RiversPass, WaterSeaPass, ...,
//     GBufferLightingPass, TransparentPass, TransparentCockpitPass). So the
//     target run is sampled at the end of WaterSeaPass: opaque scene plus
//     water, no glass or particles (flag bit4). A view without WaterSeaPass
//     is sampled at its GBufferLightingPass / the next view / the frame end.
//     Which run: every HMD view ends with a CopyToHMDPass and DCS renders
//     them in order (periphery L, periphery R, focus L, focus R; MFD sensor
//     views come before all of them), so HMD view t's run is the first run
//     after the t-th CopyToHMDPass of the frame. Measured 2026-10-11 (F-4E,
//     DLSS): all four views render their G-buffer at 816x804 into one shared
//     depth target, so size cannot tell focus from periphery. t = 2 with four
//     HMD views, else 0; the FOV check below moves t if needed. Without
//     CopyToHMDPass (fallback): the largest run size of the previous frame,
//     rank 0 among runs of that size. A frame ends at the next
//     CascadeShadowPass / HeightMapPass after a run or an HMD copy.
//  2. Gaze. GazeOut is read with a seq-guarded copy; the newest record is
//     used (DCS's xrWaitFrame/xrBeginFrame are not hooked in production, so
//     the frame's own displayTime is unknown) and its displayTime is
//     published. A record whose seq has not changed for 30 frames
//     counts as no gaze. Wanted view: 2 (left focus) with four views, else 0.
//     Periphery fallback (Decide, once per frame at its first G-buffer run):
//     when the 1° cone around the gaze is not inside the left focus view's
//     FOV (QV clamps the focus at the edges, or it lags the gaze), view 0
//     (left periphery, rendered first) is sampled instead with its own gaze
//     tangents; its cone is widened so it holds >= 40 texels (ConeDegFor:
//     about 1.1° at the DLSS periphery size, wider at lower densities) and
//     the ring is 1.5 times the cone. Flags: bit2 clear, bit5 set, view 0.
//  3. Projection. DCS's cPerView (Bazar/shaders/common/context.hlsl: gLocal,
//     gLocalInv, gView, gViewInv, gProj at byte 256) is bound to our compute
//     shader as b1, declared exactly as DCS declares it (default column-major
//     packing, DCS compiles with O3 only), so gProj[r][c] means what DCS's
//     shaders mean: clip = mul(float4(view, 1), gProj). The shader writes
//     gProj into the readback, so the CPU converts with the projection of
//     that very frame and view. Reverse-Z and an infinite far plane are
//     detected from it (ParseProj); the viewport's MinDepth/MaxDepth are
//     undone first.
//  4. Window. The shader maps the gaze tangents through gProj into the
//     viewport (exact for the rendered image) and Loads a G x G grid of
//     depth texels around it (stride s), G = 2 * half + 1 <= 127, sized on
//     the CPU from the view's pixels per tangent so the grid covers the 1.5°
//     ring (about 101 x 101 texels, stride 1, at 2028 px over ±31.5°). It
//     writes into our typed UAV buffer, copied (only the used bytes) into one
//     of 3 staging buffers, mapped with DO_NOT_WAIT >= 2 frames later. The
//     depth SRV is created on DCS's typeless depth texture (R32G8X24, R24G8,
//     R32, R16), Texture2D / Array / MS / MS array. While the shader runs,
//     DCS's DSV is unbound when it is the sampled texture (else d3d11 would
//     null our SRV) and put back exactly.
//  5. State. Everything touched on DCS's immediate context is saved and
//     restored: CS shader (+ class instances), CS CB 0-1 (with their *1
//     offsets), CS SRV 0-1 (depth, stencil), CS UAV 0 (counter kept with -1), OM RTVs + DSV (only
//     when the DSV had to be unbound; OM UAVs kept). No sampler, no other
//     stage. The split filter filters only VS/PS/IA/OM-blend/depth/RS setters
//     (none touched here) and its OM rule is unaffected; the d3dstate meter
//     treats OMSetRenderTargets* as "SRVs unknown" (correct). We run between
//     passes, never inside a pass, so no recorder ExecuteCommandList is open.
//  6. Summary (a worker thread, not the render thread): per sample the
//     direction from the texel centre (gProj inverted), the angle to the gaze,
//     the range in metres (forward distance x |dir|). Cone <= 1° (wider in
//     the periphery, see 2), ring (cone, 1.5 cone]. Water in the cone (the
//     stencil plane read with the depth: composition WATER or UNDERWATER)
//     sets flag bit3; its distances are the water surface's (sampled after
//     the water passes). near < 1.5 m, far > 3 m. medianM / nearestM / farMedianM
//     over the cone (ranges clamped to 20 km; sky = 20 km), 16-bin log
//     histogram 0.2 m .. 20 km of the cone. View check: DCS's FOV tangents
//     from gProj are compared with GazeOut.fovTan of every view; a decisive
//     best match other than the wanted view makes the frame invalid, and three
//     in a row move the target (HMD view index by the difference, or the
//     other eye's run in the fallback).
//  7. Publication in frame order; a frame that cannot be sampled is queued as
//     valid = 0 in the same FIFO. latencyFrames = frames between the dispatch
//     and the readback.
// Cost: GPU timestamps around our work (dispatch + copy), CPU QPC around the
// render-thread part; both in the 30 s log line.
// Test gaze ([Perception] FixationDepthTestGaze = "x,y", tangents): when no
// GazeOut mapping exists we create one and write that fixed gaze for every
// view, with each view's fovTan from DCS's projection of that HMD view once
// sampled (a self-consistent check only). [Perception] FixationDepthVerify=1
// samples every HMD view (or run) in turn and logs per index its size, DCS
// FOV tangents, best GazeOut match, camera x and median; =2 also logs one
// trace of the top-level passes and the DSVs bound in them.
// [Perception] FixationDepthDump=1 writes the next window as a PGM and logs
// a whole-texture readback of the depth target (one stall, debug only).
// [Perception] FarDepthExport (far_depth.h, independent of FixationDepth and
// DepthGrid) reuses the run capture, the CopyToHMDPass ordinal and the
// WaterSeaPass flush to copy HMD views 2 and 3 for Quad Views' Far depth; its
// in-pass arm also redirects UpdateSubresource to read cPerView on the CPU.
// SEH contains the render-thread and worker code; a fault latches the
// feature off. Install/uninstall are hot-reload safe: callback out and
// drained, worker joined, D3D objects released, views unmapped, handles closed.
// Included once from main.cpp inside its anonymous namespace, after
// pass_timing.h and periph_lod.h (plod::KindRaw).
#pragma once
// (Inside main.cpp's anonymous namespace: no standard headers here; main.cpp
// has <algorithm>, <condition_variable>, <thread>, d3d11_1.h, d3dcompiler.h.)

#include "perception_suffix.h"  // R79: DCSVR_PERCEPTION_SHM_SUFFIX, one rule for every block

namespace fixdepth {

// ---------------------------------------------------------------------------
// Shared structs (exactly as FIXATION_DEPTH.md)
// ---------------------------------------------------------------------------

#pragma pack(push, 8)
struct GazeOut {
  uint64_t seq;           // even = stable
  int64_t displayTime;    // XrTime of the located views
  uint32_t flags;         // bit0 valid gaze, bit1 saccade, bit2 per-eye gaze
  uint32_t viewCount;     // 2 or 4
  float gazeTan[4][2];    // per view: x right, y up, in that view's frame
  float fovTan[4][4];     // per view: left, right, up, down tangents (signed, XrFovf)
  float vergenceM;        // <= 0 unknown
};
struct DepthIn {
  uint64_t seq;           // even = stable
  int64_t displayTime;    // the GazeOut.displayTime the samples were taken for
  uint32_t flags;         // bit0 valid, bit1 reverse-Z detected, bit2 focus view
  uint32_t view;          // which view was sampled (GazeOut index)
  float coneDeg;          // radius actually used
  float nearShare, farShare, edgeNearShare, edgeFarShare;
  float medianM, nearestM, farMedianM;
  uint16_t histogram[16]; // log-distance bins, 0.2 m .. 20 km
  float latencyFrames;
};
#pragma pack(pop)
static_assert(sizeof(GazeOut) == 128, "GazeOut layout");
static_assert(offsetof(GazeOut, displayTime) == 8 && offsetof(GazeOut, flags) == 16 &&
                  offsetof(GazeOut, viewCount) == 20 && offsetof(GazeOut, gazeTan) == 24 &&
                  offsetof(GazeOut, fovTan) == 56 && offsetof(GazeOut, vergenceM) == 120,
              "GazeOut offsets");
static_assert(sizeof(DepthIn) == 96, "DepthIn layout");
static_assert(offsetof(DepthIn, flags) == 16 && offsetof(DepthIn, view) == 20 && offsetof(DepthIn, coneDeg) == 24 &&
                  offsetof(DepthIn, nearShare) == 28 && offsetof(DepthIn, edgeFarShare) == 40 &&
                  offsetof(DepthIn, medianM) == 44 && offsetof(DepthIn, farMedianM) == 52 &&
                  offsetof(DepthIn, histogram) == 56 && offsetof(DepthIn, latencyFrames) == 88,
              "DepthIn offsets");

// Local\DcsVrDepthGrid_v1 (2026-10-12, the Depth boost layer's saccade
// landing prediction): a coarse 32 x 32 grid over a whole view.
constexpr int kGridN = 32;
#pragma pack(push, 8)
struct DepthGrid {
  uint64_t seq;            // even = stable
  int64_t displayTime;     // newest GazeOut displayTime used (as DepthIn)
  uint32_t flags;          // bit0 valid, bit1 reverse-Z, bit3 water present, bit4 after the water passes
  uint32_t view;           // view sampled (GazeOut index)
  uint32_t w, h;           // 32 x 32
  float fovTan[4];         // left, right, up, down of that view, from DCS's projection (signed, XrFovf)
  float nearestM[kGridN * kGridN];     // min distance in the cell (m; 20 km = sky; 0 = no sample)
  float typicalM[kGridN * kGridN];     // geometric mean (log-mean) of the cell's distances (m)
  uint8_t nearShare[kGridN * kGridN];  // share of the cell under 1.5 m, 0..255
};
#pragma pack(pop)
static_assert(sizeof(DepthGrid) == 9264, "DepthGrid layout");
static_assert(offsetof(DepthGrid, displayTime) == 8 && offsetof(DepthGrid, flags) == 16 &&
                  offsetof(DepthGrid, view) == 20 && offsetof(DepthGrid, w) == 24 && offsetof(DepthGrid, h) == 28 &&
                  offsetof(DepthGrid, fovTan) == 32 && offsetof(DepthGrid, nearestM) == 48 &&
                  offsetof(DepthGrid, typicalM) == 4144 && offsetof(DepthGrid, nearShare) == 8240,
              "DepthGrid offsets");

constexpr uint32_t kGazeValid = 1, kGazeSaccade = 2;
constexpr uint32_t kDepthValid = 1, kDepthReverseZ = 2, kDepthFocus = 4;
// Added 2026-10-12 (existing bits unchanged, struct unchanged):
// bit3: water in the cone (stencil composition WATER/UNDERWATER, written by
//       DCS's water passes); its distances are the water surface's.
// bit4: the depth was read after DCS's water passes (WaterSeaPass), so water
//       surfaces are in it; 0 = read right after the G-buffer (no water pass
//       seen in that view: water would be missing).
// bit5: periphery fallback: the gaze cone was not inside the eye's focus
//       view, so that eye's periphery view was sampled (bit2 clear, `view` =
//       0 left / 1 right) with the cone widened to its texel density.
constexpr uint32_t kDepthWater = 8, kDepthWaterPass = 16, kDepthPeriphery = 32;
// DCS's stencil composition type (Bazar/shaders/common/stencil.hlsl): bits
// 3-5 of the stencil; 2 = water, 6 = underwater.
constexpr uint32_t kStencilWater = 2, kStencilUnderwater = 6;
inline bool IsWaterStencil(uint32_t st) {
  const uint32_t t = (st >> 3) & 7;
  return t == kStencilWater || t == kStencilUnderwater;
}

// Mapping names. R79 (R77c N1): the base names (= dcsvr_perception_shm.h's)
// plus DCSVR_PERCEPTION_SHM_SUFFIX, read at each open attempt like BoostOut,
// StereoAck and the periphery filter's GazeOut (perception_suffix.h). Tests
// may set an override (non-empty: used as it is); empty = the suffixed name.
constexpr const wchar_t* kGazeBase = L"Local\\DcsVrPerceptionGaze_v1";
constexpr const wchar_t* kDepthBase = L"Local\\DcsVrFixationDepth_v1";
constexpr const wchar_t* kGridBase = L"Local\\DcsVrDepthGrid_v1";
std::wstring g_gazeName, g_depthName, g_gridName;  // test overrides ("" = none)
std::wstring GazeName() { return g_gazeName.empty() ? perc_sfx::Name(kGazeBase) : g_gazeName; }
std::wstring DepthName() { return g_depthName.empty() ? perc_sfx::Name(kDepthBase) : g_depthName; }
std::wstring GridName() { return g_gridName.empty() ? perc_sfx::Name(kGridBase) : g_gridName; }

// ---------------------------------------------------------------------------
// Seq guard
// ---------------------------------------------------------------------------

// Seq-guarded copy of a shared record: true with a consistent copy, false
// when the writer stayed busy (odd) or kept changing it for `tries` attempts.
template <class T>
bool SeqRead(const void* shared, T& out, int tries = 64) {
  const volatile uint64_t* seq = static_cast<const volatile uint64_t*>(shared);
  for (int i = 0; i < tries; ++i) {
    const uint64_t s1 = *seq;
    if (s1 & 1) {
      YieldProcessor();
      continue;
    }
    MemoryBarrier();
    const volatile uint8_t* src = static_cast<const volatile uint8_t*>(shared);
    uint8_t* dst = reinterpret_cast<uint8_t*>(&out);
    for (size_t k = 0; k < sizeof(T); ++k) dst[k] = src[k];
    MemoryBarrier();
    const uint64_t s2 = *seq;
    if (s1 == s2) {
      memcpy(&out, &s1, sizeof(s1));
      return true;
    }
  }
  return false;
}

// Seq-guarded write (single writer): seq odd, payload, seq even.
template <class T>
void SeqWrite(void* shared, const T& v) {
  volatile uint64_t* seq = static_cast<volatile uint64_t*>(shared);
  uint64_t s = *seq;
  if (s & 1) ++s;  // a writer that died mid-write
  *seq = s + 1;
  MemoryBarrier();
  volatile uint8_t* dst = static_cast<volatile uint8_t*>(shared);
  const uint8_t* src = reinterpret_cast<const uint8_t*>(&v);
  for (size_t k = sizeof(uint64_t); k < sizeof(T); ++k) dst[k] = src[k];
  MemoryBarrier();
  *seq = s + 2;
}

// ---------------------------------------------------------------------------
// Projection (gProj, row-vector convention: clip = [x y z 1] * P)
// ---------------------------------------------------------------------------

struct Proj {
  double P[4][4] = {};
  bool ok = false, reverse = false, infinite = false;
  double farLimit = 0;  // depth at infinite distance
  double ox = 0, oy = 0;  // P20/P23, P21/P23: the NDC of the view axis
};

bool Finite(double v) { return std::isfinite(v); }

double DepthAtW(const Proj& p, double w) {
  const double z = (w - p.P[3][3]) / p.P[2][3];
  return (z * p.P[2][2] + p.P[3][2]) / w;
}

// m: logical P[r][c] at m[r * 4 + c].
Proj ParseProj(const float* m) {
  Proj p;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      p.P[r][c] = m[r * 4 + c];
      if (!Finite(p.P[r][c])) return p;
    }
  const double p23 = p.P[2][3];
  if (std::fabs(p23) < 1e-6 || std::fabs(p.P[3][3]) > 1e-4 * std::fabs(p23)) return p;  // not perspective
  if (std::fabs(p.P[0][0]) < 1e-6 || std::fabs(p.P[1][1]) < 1e-6) return p;
  if (std::fabs(p.P[0][3]) > 1e-4 || std::fabs(p.P[1][3]) > 1e-4) return p;
  p.ox = p.P[2][0] / p23;
  p.oy = p.P[2][1] / p23;
  const double d1 = DepthAtW(p, 1.0), d2 = DepthAtW(p, 1000.0);
  if (!Finite(d1) || !Finite(d2) || d1 == d2) return p;
  p.reverse = d1 > d2;
  p.farLimit = p.P[2][2] / p23;
  p.infinite = p.reverse ? std::fabs(p.farLimit) < 1e-6 : std::fabs(p.farLimit - 1.0) < 1e-6;
  p.ok = true;
  return p;
}

// Forward distance (clip w, metres) of an NDC depth; +inf for the sky / far plane.
double ForwardDist(const Proj& p, double d) {
  if (p.reverse ? d <= p.farLimit + 1e-9 : d >= p.farLimit - 1e-9) return INFINITY;
  const double den = d * p.P[2][3] - p.P[2][2];
  if (std::fabs(den) < 1e-30) return INFINITY;
  const double z = (p.P[3][2] - d * p.P[3][3]) / den;
  const double w = z * p.P[2][3] + p.P[3][3];
  if (!Finite(w) || w <= 0) return INFINITY;
  return w;
}

// View tangents (x right, y up per unit forward distance) <-> NDC.
void TanToNdc(const Proj& p, double tx, double ty, double& nx, double& ny) {
  nx = tx * p.P[0][0] + ty * p.P[1][0] + p.ox;
  ny = tx * p.P[0][1] + ty * p.P[1][1] + p.oy;
}
void NdcToTan(const Proj& p, double nx, double ny, double& tx, double& ty) {
  const double a = p.P[0][0], b = p.P[1][0], c = p.P[0][1], d = p.P[1][1];
  const double det = a * d - b * c;
  const double x = nx - p.ox, y = ny - p.oy;
  tx = (d * x - b * y) / det;
  ty = (a * y - c * x) / det;
}
// The view's FOV tangents (left, right, up, down; signed like XrFovf).
void DcsFov(const Proj& p, double fov[4]) {
  fov[0] = (-1.0 - p.ox) / p.P[0][0];
  fov[1] = (1.0 - p.ox) / p.P[0][0];
  fov[2] = (1.0 - p.oy) / p.P[1][1];
  fov[3] = (-1.0 - p.oy) / p.P[1][1];
}

struct Viewport {
  double x = 0, y = 0, w = 0, h = 0, minD = 0, maxD = 1;
};
// Pixel position (not rounded) of a view tangent.
void TanToPixel(const Proj& p, const Viewport& vp, double tx, double ty, double& px, double& py) {
  double nx, ny;
  TanToNdc(p, tx, ty, nx, ny);
  px = vp.x + (nx * 0.5 + 0.5) * vp.w;
  py = vp.y + (0.5 - 0.5 * ny) * vp.h;
}
// View tangent of a texel centre.
void TexelToTan(const Proj& p, const Viewport& vp, int ix, int iy, double& tx, double& ty) {
  const double nx = ((ix + 0.5 - vp.x) / vp.w) * 2.0 - 1.0;
  const double ny = 1.0 - ((iy + 0.5 - vp.y) / vp.h) * 2.0;
  NdcToTan(p, nx, ny, tx, ty);
}

// ---------------------------------------------------------------------------
// Window plan, histogram, summary
// ---------------------------------------------------------------------------

constexpr double kConeDeg = 1.0;
constexpr double kRingDeg = 1.5;
constexpr double kNearM = 1.5, kFarM = 3.0;
constexpr double kHistMinM = 0.2, kHistMaxM = 20000.0;
constexpr int kMaxHalf = 63;  // G <= 127
constexpr int kMaxG = 2 * kMaxHalf + 1;
constexpr int kHeader = 32;   // floats before the samples in the readback
constexpr int kMinCone = 4;
constexpr double kDeg = 0.017453292519943295;

struct Plan {
  int half = 0, stride = 1, G = 1;
  double radiusPx = 0;
};

// Grid covering `radiusDeg` (+ margin) around a gaze of tangents (gx, gy)
// at pxPerTan pixels per unit tangent on each axis.
Plan PlanWindow(double pxPerTanX, double pxPerTanY, double gx, double gy, double radiusDeg = kRingDeg + 0.1,
                int maxHalf = kMaxHalf) {
  Plan pl;
  const double g = std::sqrt(gx * gx + gy * gy);
  const double a = std::atan(g);
  // tan(a + r) - tan(a) bounds the tangent extent of the circle along the
  // radial direction (the steepest); the tangential extent is smaller.
  const double dt = std::tan(std::min(a + radiusDeg * kDeg, 89.0 * kDeg)) - g;
  pl.radiusPx = std::max(pxPerTanX, pxPerTanY) * dt + 1.0;
  pl.stride = std::max(1, static_cast<int>(std::ceil(pl.radiusPx / maxHalf)));
  pl.half = std::min(maxHalf, static_cast<int>(std::ceil(pl.radiusPx / pl.stride)));
  pl.G = 2 * pl.half + 1;
  return pl;
}

// The cone radius for a view: 1° unless that holds fewer than kMinConeTexels
// texels at the view's density (periphery: about 3 px per degree), then the
// radius that holds them. The ring is 1.5 times the cone.
constexpr double kMinConeTexels = 40;
double ConeDegFor(double pxPerTan) {
  if (!(pxPerTan > 0)) return kConeDeg;
  const double rPx = std::sqrt(kMinConeTexels / 3.14159265358979);
  return std::max(kConeDeg, std::atan(rPx / pxPerTan) / kDeg);
}

// True when the whole cone (radius coneDeg) around the gaze lies inside the
// view's FOV tangents (left, right, up, down; signed).
bool ConeInside(const float fov[4], double gx, double gy, double coneDeg) {
  if (!(fov[1] > fov[0]) || !(fov[2] > fov[3])) return false;
  const double ax = std::atan(gx), ay = std::atan(gy), r = coneDeg * kDeg;
  return std::tan(ax - r) >= fov[0] && std::tan(ax + r) <= fov[1] && std::tan(ay + r) <= fov[2] &&
         std::tan(ay - r) >= fov[3];
}

int HistBin(double m) {
  if (!(m > kHistMinM)) return 0;
  if (m >= kHistMaxM) return 15;
  const int b = static_cast<int>(std::floor(16.0 * std::log(m / kHistMinM) / std::log(kHistMaxM / kHistMinM)));
  return b < 0 ? 0 : b > 15 ? 15 : b;
}

struct Window {
  float P[16] = {};      // gProj as the shader read it
  int cx = 0, cy = 0;    // centre texel
  int G = 0, half = 0, stride = 1;
  Viewport vp;
  double gx = 0, gy = 0; // gaze tangents
  const float* samples = nullptr;  // G * G raw depth, < 0 = outside
  const float* stencil = nullptr;  // G * G stencil values (optional)
};

struct Summary {
  bool valid = false, reverse = false, infinite = false;
  int cone = 0, ring = 0;
  float nearShare = 0, farShare = 0, edgeNearShare = 0, edgeFarShare = 0;
  float medianM = 0, nearestM = 0, farMedianM = 0;
  uint16_t hist[16] = {};
  double dcsFov[4] = {};
  int water = 0;  // cone samples on water
};

float Median(std::vector<float>& v) {
  if (v.empty()) return 0;
  const size_t m = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + m, v.end());
  float hi = v[m];
  if (v.size() % 2) return hi;
  const float lo = *std::max_element(v.begin(), v.begin() + m);
  return 0.5f * (lo + hi);
}

Summary Summarize(const Window& w, double coneDeg = kConeDeg, double ringDeg = kRingDeg) {
  Summary s;
  const Proj p = ParseProj(w.P);
  if (!p.ok || !w.samples || w.G <= 0 || w.vp.w <= 0 || w.vp.h <= 0) return s;
  s.reverse = p.reverse;
  s.infinite = p.infinite;
  DcsFov(p, s.dcsFov);
  const double gl = std::sqrt(w.gx * w.gx + w.gy * w.gy + 1.0);
  const double cosCone = std::cos(coneDeg * kDeg), cosRing = std::cos(ringDeg * kDeg);
  const double dscale = w.vp.maxD - w.vp.minD;
  std::vector<float> cone, farv;
  cone.reserve(static_cast<size_t>(w.G) * w.G);
  int cn = 0, cf = 0, rn = 0, rf = 0;
  float nearest = static_cast<float>(kHistMaxM);
  uint32_t hist[16] = {};
  for (int j = 0; j < w.G; ++j) {
    const int iy = w.cy + (j - w.half) * w.stride;
    for (int i = 0; i < w.G; ++i) {
      const float raw = w.samples[j * w.G + i];
      if (!(raw >= 0.0f)) continue;  // outside the viewport (or NaN)
      const int ix = w.cx + (i - w.half) * w.stride;
      double tx, ty;
      TexelToTan(p, w.vp, ix, iy, tx, ty);
      const double dl = std::sqrt(tx * tx + ty * ty + 1.0);
      const double c = (tx * w.gx + ty * w.gy + 1.0) / (dl * gl);
      if (c < cosRing) continue;
      const double d = dscale != 0 ? (raw - w.vp.minD) / dscale : raw;
      const double fwd = ForwardDist(p, d);
      double r = fwd * dl;
      if (!(r < kHistMaxM)) r = kHistMaxM;
      const float rf32 = static_cast<float>(r);
      const bool isNear = r < kNearM, isFar = r > kFarM;
      if (c >= cosCone) {
        ++s.cone;
        if (w.stencil && IsWaterStencil(static_cast<uint32_t>(w.stencil[j * w.G + i]))) ++s.water;
        cn += isNear;
        cf += isFar;
        cone.push_back(rf32);
        if (isFar) farv.push_back(rf32);
        nearest = std::min(nearest, rf32);
        ++hist[HistBin(r)];
      } else {
        ++s.ring;
        rn += isNear;
        rf += isFar;
      }
    }
  }
  if (s.cone < kMinCone) return s;
  s.nearShare = static_cast<float>(cn) / s.cone;
  s.farShare = static_cast<float>(cf) / s.cone;
  if (s.ring > 0) {
    s.edgeNearShare = static_cast<float>(rn) / s.ring;
    s.edgeFarShare = static_cast<float>(rf) / s.ring;
  }
  s.nearestM = nearest;
  s.medianM = Median(cone);
  s.farMedianM = Median(farv);
  for (int b = 0; b < 16; ++b) s.hist[b] = static_cast<uint16_t>(std::min<uint32_t>(hist[b], 65535));
  s.valid = true;
  return s;
}

// Which GazeOut view has the FOV DCS rendered with. Tangents compared as
// signed (XrFovf) and, as a second convention, with left/down given as
// magnitudes. best = -1 when nothing is within `tol`.
struct Match {
  int best = -1, convention = 0;
  double err = 1e9, second = 1e9;
  bool decisive = false;
};
Match MatchFov(const GazeOut& g, const double dcs[4], double tol = 0.03, double margin = 0.004) {
  Match m;
  const uint32_t n = std::min<uint32_t>(g.viewCount, 4);
  for (int conv = 0; conv < 2; ++conv) {
    double errs[4];
    for (uint32_t v = 0; v < 4; ++v) {
      errs[v] = 1e9;
      if (v >= n) continue;
      double f[4] = {g.fovTan[v][0], g.fovTan[v][1], g.fovTan[v][2], g.fovTan[v][3]};
      if (conv == 1) {
        f[0] = -f[0];
        f[3] = -f[3];
      }
      double e = 0;
      for (int k = 0; k < 4; ++k) e = std::max(e, std::fabs(f[k] - dcs[k]));
      errs[v] = Finite(e) ? e : 1e9;
    }
    int b = -1;
    double be = 1e9, se = 1e9;
    for (int v = 0; v < 4; ++v) {
      if (errs[v] < be) {
        se = be;
        be = errs[v];
        b = v;
      } else if (errs[v] < se) {
        se = errs[v];
      }
    }
    if (be < m.err) {
      m.err = be;
      m.second = se;
      m.best = b;
      m.convention = conv;
    }
  }
  if (m.err > tol) m.best = -1;
  m.decisive = m.best >= 0 && m.second - m.err > margin;
  return m;
}

// ---------------------------------------------------------------------------
// Compute shader
// ---------------------------------------------------------------------------

const char kShader[] = R"(
cbuffer FdParams : register(b0) {
  float2 gGaze; float2 gVpOrg; float2 gVpSize; int2 gTex;
  int gSlice; int gStride; int gHalf; int gG;
};
cbuffer cPerView : register(b1) {
  float4x4 gLocal, gLocalInv;
  float4x4 gView, gViewInv;
  float4x4 gProj, gProjInv;
};
#if FD_MS && FD_ARRAY
Texture2DMSArray<float> gDepth : register(t0);
Texture2DMSArray<uint2> gStencil : register(t1);
#elif FD_MS
Texture2DMS<float> gDepth : register(t0);
Texture2DMS<uint2> gStencil : register(t1);
#elif FD_ARRAY
Texture2DArray<float> gDepth : register(t0);
Texture2DArray<uint2> gStencil : register(t1);
#else
Texture2D<float> gDepth : register(t0);
Texture2D<uint2> gStencil : register(t1);
#endif
RWBuffer<float> gOut : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  float p23 = gProj[2][3];
  float ndcx = gGaze.x * gProj[0][0] + gGaze.y * gProj[1][0] + gProj[2][0] / p23;
  float ndcy = gGaze.x * gProj[0][1] + gGaze.y * gProj[1][1] + gProj[2][1] / p23;
  float cxf = gVpOrg.x + (ndcx * 0.5 + 0.5) * gVpSize.x;
  float cyf = gVpOrg.y + (0.5 - 0.5 * ndcy) * gVpSize.y;
  cxf = clamp(cxf, -100000.0, 100000.0);
  cyf = clamp(cyf, -100000.0, 100000.0);
  int2 c = int2(floor(cxf), floor(cyf));
  if (id.x == 0 && id.y == 0) {
    [unroll] for (int r = 0; r < 4; ++r) [unroll] for (int k = 0; k < 4; ++k) gOut[r * 4 + k] = gProj[r][k];
    gOut[16] = (float)c.x; gOut[17] = (float)c.y; gOut[18] = cxf; gOut[19] = cyf;
    gOut[20] = gViewInv[0][0]; gOut[21] = gViewInv[0][1]; gOut[22] = gViewInv[0][2];
    gOut[23] = gViewInv[3][0]; gOut[24] = gViewInv[3][1]; gOut[25] = gViewInv[3][2];
    gOut[26] = 1.0;
  }
  if ((int)id.x >= gG || (int)id.y >= gG) return;
  int2 p = c + (int2(id.xy) - gHalf) * gStride;
  int2 lo = int2(max(0, (int)floor(gVpOrg.x)), max(0, (int)floor(gVpOrg.y)));
  int2 hi = min(gTex, int2(floor(gVpOrg + gVpSize)));
  float d = -1.0;
  uint st = 0;  // no stencil view bound: reads 0
  if (all(p >= lo) && all(p < hi)) {
#if FD_MS && FD_ARRAY
    d = gDepth.Load(int3(p, gSlice), 0);
    st = gStencil.Load(int3(p, gSlice), 0).g;
#elif FD_MS
    d = gDepth.Load(p, 0);
    st = gStencil.Load(p, 0).g;
#elif FD_ARRAY
    d = gDepth.Load(int4(p, gSlice, 0));
    st = gStencil.Load(int4(p, gSlice, 0)).g;
#else
    d = gDepth.Load(int3(p, 0));
    st = gStencil.Load(int3(p, 0)).g;
#endif
  }
  gOut[32 + id.y * gG + id.x] = d;
  gOut[32 + gG * gG + id.y * gG + id.x] = (float)st;
}
)";

struct Params {
  float gaze[2], vpOrg[2], vpSize[2];
  int32_t tex[2];
  int32_t slice, stride, half, G;
};
static_assert(sizeof(Params) == 48, "FdParams layout");

// The grid reduction: one 16 x 16 group per cell; each thread walks its
// share of the cell's texels (the cell bounds split the viewport evenly),
// converts depth to distance along the ray with gProj as the CPU does for
// the cone (sky and anything at or past the far limit = 20 km), then the
// group reduces min, sum of logs, near count, sample count, water count.
// Output (raw words): nearest[1024], typical[1024], packed[1024] (bits 0-7
// near share 0..255, bit 8 water in the cell, bit 9 has samples), then gProj
// (16 floats) at word 3072.
const char kGridShader[] = R"(
cbuffer GridParams : register(b0) {
  float2 gVpOrg; float2 gVpSize; int2 gTex; float gMinD; float gMaxD;
};
cbuffer cPerView : register(b1) {
  float4x4 gLocal, gLocalInv;
  float4x4 gView, gViewInv;
  float4x4 gProj, gProjInv;
};
#if FD_MS && FD_ARRAY
Texture2DMSArray<float> gDepth : register(t0);
Texture2DMSArray<uint2> gStencil : register(t1);
#define LOADD(p) gDepth.Load(int3(p, 0), 0)
#define LOADS(p) gStencil.Load(int3(p, 0), 0).g
#elif FD_MS
Texture2DMS<float> gDepth : register(t0);
Texture2DMS<uint2> gStencil : register(t1);
#define LOADD(p) gDepth.Load(p, 0)
#define LOADS(p) gStencil.Load(p, 0).g
#elif FD_ARRAY
Texture2DArray<float> gDepth : register(t0);
Texture2DArray<uint2> gStencil : register(t1);
#define LOADD(p) gDepth.Load(int4(p, 0, 0))
#define LOADS(p) gStencil.Load(int4(p, 0, 0)).g
#else
Texture2D<float> gDepth : register(t0);
Texture2D<uint2> gStencil : register(t1);
#define LOADD(p) gDepth.Load(int3(p, 0))
#define LOADS(p) gStencil.Load(int3(p, 0)).g
#endif
RWByteAddressBuffer gOut : register(u0);
groupshared float sMin[256];
groupshared float sLog[256];
groupshared uint sNear[256];
groupshared uint sN[256];
groupshared uint sW[256];
[numthreads(16, 16, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
  float p22 = gProj[2][2], p23 = gProj[2][3], p32 = gProj[3][2], p33 = gProj[3][3];
  float ox = gProj[2][0] / p23, oy = gProj[2][1] / p23;
  float d1 = ((1.0 - p33) / p23 * p22 + p32);
  float d2 = ((1000.0 - p33) / p23 * p22 + p32) / 1000.0;
  bool rev = d1 > d2;
  float lim = p22 / p23;
  int x0 = max(0, (int)floor(gVpOrg.x + gid.x * gVpSize.x / 32.0));
  int x1 = min(gTex.x, (int)floor(gVpOrg.x + (gid.x + 1) * gVpSize.x / 32.0));
  int y0 = max(0, (int)floor(gVpOrg.y + gid.y * gVpSize.y / 32.0));
  int y1 = min(gTex.y, (int)floor(gVpOrg.y + (gid.y + 1) * gVpSize.y / 32.0));
  float mn = 1e30, sl = 0;
  uint nn = 0, n = 0, nw = 0;
  float dscale = gMaxD - gMinD;
  for (int y = y0 + (int)tid.y; y < y1; y += 16) {
    for (int x = x0 + (int)tid.x; x < x1; x += 16) {
      int2 p = int2(x, y);
      float d = LOADD(p);
      uint st = LOADS(p);
      float dn = dscale != 0 ? (d - gMinD) / dscale : d;
      float r = 20000.0;
      bool sky = rev ? dn <= lim + 1e-9 : dn >= lim - 1e-9;
      if (!sky) {
        float z = (p32 - dn * p33) / (dn * p23 - p22);
        float w = z * p23 + p33;
        if (w > 0) {
          float nx = ((x + 0.5 - gVpOrg.x) / gVpSize.x) * 2.0 - 1.0;
          float ny = 1.0 - ((y + 0.5 - gVpOrg.y) / gVpSize.y) * 2.0;
          float tx = (nx - ox) / gProj[0][0], ty = (ny - oy) / gProj[1][1];
          r = min(w * sqrt(1.0 + tx * tx + ty * ty), 20000.0);
        }
      }
      mn = min(mn, r);
      sl += log(max(r, 0.01));
      nn += r < 1.5 ? 1u : 0u;
      n += 1u;
      uint t = (st >> 3) & 7u;
      nw += (t == 2u || t == 6u) ? 1u : 0u;
    }
  }
  sMin[gi] = mn; sLog[gi] = sl; sNear[gi] = nn; sN[gi] = n; sW[gi] = nw;
  GroupMemoryBarrierWithGroupSync();
  [unroll] for (uint k = 128; k > 0; k >>= 1) {
    if (gi < k) {
      sMin[gi] = min(sMin[gi], sMin[gi + k]);
      sLog[gi] += sLog[gi + k];
      sNear[gi] += sNear[gi + k];
      sN[gi] += sN[gi + k];
      sW[gi] += sW[gi + k];
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if (gi == 0) {
    uint c = gid.y * 32 + gid.x;
    uint cnt = sN[0];
    float nearest = cnt ? sMin[0] : 0.0;
    float typical = cnt ? exp(sLog[0] / cnt) : 0.0;
    uint share = cnt ? (uint)floor(255.0 * sNear[0] / cnt + 0.5) : 0u;
    gOut.Store(c * 4, asuint(nearest));
    gOut.Store((1024 + c) * 4, asuint(typical));
    gOut.Store((2048 + c) * 4, share | (sW[0] ? 256u : 0u) | (cnt ? 512u : 0u));
    if (c == 0) {
      [unroll] for (int r = 0; r < 4; ++r) [unroll] for (int q = 0; q < 4; ++q) gOut.Store((3072 + r * 4 + q) * 4, asuint(gProj[r][q]));
    }
  }
}
)";
constexpr int kGridWords = 3 * kGridN * kGridN + 16;

struct GridParams {
  float vpOrg[2], vpSize[2];
  int32_t tex[2];
  float minD, maxD;
};
static_assert(sizeof(GridParams) == 32, "GridParams layout");

// The published grid from the shader's words (pure; tests use it too).
DepthGrid BuildGrid(const uint32_t* words, int64_t displayTime, uint32_t view, bool afterWater) {
  DepthGrid g{};
  g.displayTime = displayTime;
  g.view = view;
  g.w = g.h = kGridN;
  float P[16];
  memcpy(P, words + 3 * kGridN * kGridN, sizeof(P));
  const Proj pr = ParseProj(P);
  bool water = false, any = false;
  for (int c = 0; c < kGridN * kGridN; ++c) {
    memcpy(&g.nearestM[c], words + c, 4);
    memcpy(&g.typicalM[c], words + kGridN * kGridN + c, 4);
    const uint32_t pk = words[2 * kGridN * kGridN + c];
    g.nearShare[c] = static_cast<uint8_t>(pk & 255);
    water |= (pk & 256) != 0;
    any |= (pk & 512) != 0;
  }
  if (pr.ok) {
    double f[4];
    DcsFov(pr, f);
    for (int k = 0; k < 4; ++k) g.fovTan[k] = static_cast<float>(f[k]);
  }
  g.flags = (pr.ok && any ? kDepthValid : 0) | (pr.ok && pr.reverse ? kDepthReverseZ : 0) | (water ? kDepthWater : 0) |
            (afterWater ? kDepthWaterPass : 0);
  return g;
}

std::vector<uint8_t> g_blob[4];  // variant = ms * 2 + array
std::vector<uint8_t> g_gridBlob[4];
std::atomic<bool> g_blobsReady{false};
std::string g_compileError;

bool CompileShaders() {
  if (g_blobsReady.load()) return true;
  wchar_t sys[MAX_PATH];
  const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
  if (!n || n >= MAX_PATH) return false;
  HMODULE m = LoadLibraryW((std::wstring(sys) + L"\\d3dcompiler_47.dll").c_str());
  if (!m) {
    g_compileError = "d3dcompiler_47.dll not loadable";
    return false;
  }
  auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile"));
  if (!compile) {
    g_compileError = "D3DCompile not exported";
    return false;
  }
  for (int v = 0; v < 4; ++v) {
    const D3D_SHADER_MACRO defs[] = {{"FD_MS", (v & 2) ? "1" : "0"}, {"FD_ARRAY", (v & 1) ? "1" : "0"}, {nullptr, nullptr}};
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    const HRESULT hr = compile(kShader, sizeof(kShader) - 1, "fixation_depth", defs, nullptr, "main", "cs_5_0",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr) || !code) {
      g_compileError = err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize())
                           : std::string("D3DCompile failed");
      if (err) err->Release();
      if (code) code->Release();
      return false;
    }
    const uint8_t* b = static_cast<const uint8_t*>(code->GetBufferPointer());
    g_blob[v].assign(b, b + code->GetBufferSize());
    code->Release();
    if (err) err->Release();
    code = err = nullptr;
    const HRESULT hg = compile(kGridShader, sizeof(kGridShader) - 1, "depth_grid", defs, nullptr, "main", "cs_5_0",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hg) || !code) {
      g_compileError = err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize())
                           : std::string("D3DCompile (grid) failed");
      if (err) err->Release();
      if (code) code->Release();
      return false;
    }
    b = static_cast<const uint8_t*>(code->GetBufferPointer());
    g_gridBlob[v].assign(b, b + code->GetBufferSize());
    code->Release();
    if (err) err->Release();
  }
  g_blobsReady = true;
  return true;
}

// SRV format of the stencil plane (UNKNOWN: none).
DXGI_FORMAT StencilSrvFormat(DXGI_FORMAT tex) {
  switch (tex) {
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

// SRV format for a typeless depth texture (UNKNOWN: not sampleable).
DXGI_FORMAT SrvFormat(DXGI_FORMAT tex) {
  switch (tex) {
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

// ---------------------------------------------------------------------------
// Statistics (any thread)
// ---------------------------------------------------------------------------

std::atomic<uint64_t> g_frames{0}, g_runs{0}, g_dispatches{0}, g_published{0}, g_publishedValid{0},
    g_noGaze{0}, g_staleGaze{0}, g_noTarget{0}, g_ringFull{0}, g_noSrv{0}, g_badProj{0}, g_mismatch{0},
    g_noMatch{0}, g_matchOk{0}, g_retarget{0}, g_omUnbinds{0}, g_mapLate{0}, g_badCb{0};
std::atomic<bool> g_fault{false};
// R69: the first fault of this load, for the log (fault latched line): the SEH site (its source line), the
// exception code and the faulting address as module + offset (Tick resolves the module).
std::atomic<int> g_faultSite{0};
std::atomic<uint32_t> g_faultCode{0};
std::atomic<uintptr_t> g_faultAddr{0};
inline int FaultNote(EXCEPTION_POINTERS* ep, int site) {
  int none = 0;
  if (ep && ep->ExceptionRecord && g_faultSite.compare_exchange_strong(none, site)) {
    g_faultCode = ep->ExceptionRecord->ExceptionCode;
    g_faultAddr = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}
// Which outputs are on ([Perception] FixationDepth, DepthGrid); either alone
// or both (then they share the boundary callback, the in-pass capture and
// the frame logic).
std::atomic<bool> g_coneOn{true}, g_gridOn{false};
std::atomic<uint64_t> g_gridDispatches{0}, g_gridPublished{0}, g_gridValid{0}, g_gridLate{0}, g_gridRingFull{0};
double g_gridUsSum = 0, g_gridUsMax = 0, g_gridCpuUsSum = 0;  // under g_statMutex
uint64_t g_gridUsN = 0, g_gridCpuN = 0;
std::atomic<int> g_targetK{0};     // size mode: rank among the largest runs
std::atomic<int> g_hmdOffset{0};   // HMD mode: HMD view index - GazeOut view index (learned by the FOV check)
std::atomic<uint64_t> g_periphFrames{0}, g_afterWater{0}, g_waterFrames{0};
std::atomic<int> g_passesMin{0}, g_passesMax{0};  // GBufferPass executions in the sampled run (since the last log)
std::atomic<uint64_t> g_execFrame{0};
std::atomic<int> g_verify{0};
std::atomic<bool> g_dumpWanted{false};
std::wstring g_dumpDir;  // set before g_dumpWanted
std::mutex g_statMutex;
double g_gpuUsSum = 0, g_gpuUsMax = 0, g_cpuUsSum = 0, g_cpuUsMax = 0, g_latSum = 0;
uint64_t g_gpuN = 0, g_cpuN = 0, g_latN = 0;
DepthIn g_last{};
int g_lastConv = -1;
std::string g_lastNote;
// Verify mode: per run rank.
struct RankInfo {
  int w = 0, h = 0, best = -2, dcsSamples = 0;
  double fov[4] = {}, camX = 0;
  float median = 0, nearShare = 0;
};
RankInfo g_rank[16];
// Latest DCS FOV of the sampled run (window sizing, test gaze).
// DCS's FOV per sampled index (HMD view or run rank) from the latest readback
// (window sizing; the test gaze writer's fovTan).
std::atomic<float> g_dcsFov[8][4] = {};
std::atomic<bool> g_haveDcsFov[8] = {};

// ---------------------------------------------------------------------------
// Render-thread state
// ---------------------------------------------------------------------------

struct Slot {
  bool busy = false, gpu = false, timed = false;
  uint64_t frame = 0;
  ID3D11Buffer* staging = nullptr;
  ID3D11Query* ts0 = nullptr;
  ID3D11Query* ts1 = nullptr;
  ID3D11Query* dj = nullptr;
  // meta
  GazeOut gaze{};
  uint32_t view = 0;
  int G = 0, half = 0, stride = 1, rank = 0, w = 0, h = 0;
  Viewport vp;
  double gx = 0, gy = 0, cpuUs = 0;
  bool wantFocus = false;
  bool hmdMode = false;  // rank = HMD view index (else rank among the largest runs)
  bool afterWater = false, periphery = false;
  double coneDeg = kConeDeg, ringDeg = kRingDeg;
};

struct SrvEntry {
  ID3D11Resource* tex = nullptr;  // identity only (the SRV holds the reference)
  UINT slice = 0;
  int variant = 0;
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11ShaderResourceView* stencil = nullptr;  // may be null (no stencil plane)
  uint64_t lastFrame = 0;
};

struct Run {
  bool open = false;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11Resource* tex = nullptr;  // identity (held by dsv)
  UINT slice = 0;
  int variant = 0;
  DXGI_FORMAT texFmt = DXGI_FORMAT_UNKNOWN;
  UINT texW = 0, texH = 0;
  D3D11_VIEWPORT vp{};
  ID3D11Buffer* cb = nullptr;
  UINT cbFirst = 0, cbNum = 0;
  D3D11_COMPARISON_FUNC depthFunc = D3D11_COMPARISON_NEVER;
  int passes = 0;  // GBufferPass executions merged into this run so far
  // Far depth export (far_depth.h): DCS's texture as it is (texFmt is
  // UNKNOWN without an SRV bind flag), whether the in-pass arm held, and the
  // CPU copy of this pass's cPerView matrices (logical P[r][c] at r * 4 + c).
  DXGI_FORMAT rawFmt = DXGI_FORMAT_UNKNOWN;
  UINT mipLevels = 1;
  bool armOk = false, cpuProj = false;
  float proj[16] = {}, viewInv[16] = {};
  float origin[3] = {};    // R71: this pass's gOrigin (DCS's floating render origin) when originOk
  bool originOk = false;
};

struct State {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11DeviceContext1* ctx1 = nullptr;
  ID3D11ComputeShader* cs[4] = {};
  ID3D11Buffer* params = nullptr;
  ID3D11Buffer* out = nullptr;
  ID3D11UnorderedAccessView* uav = nullptr;
  Slot ring[3];
  int head = 0, count = 0;
  // Depth grid: shaders, parameters, the raw output buffer and its ring.
  ID3D11ComputeShader* gridCs[4] = {};
  ID3D11Buffer* gridParams = nullptr;
  ID3D11Buffer* gridOut = nullptr;
  ID3D11UnorderedAccessView* gridUav = nullptr;
  Slot gridRing[3];  // staging, queries, frame, gaze, view, afterWater
  int gridHead = 0, gridCount = 0;
  Run gridPending;
  bool hasGridPending = false;
  SrvEntry srv[8];  // R65: 8 (was 4): the cockpit mask samples up to four HMD views' targets besides the cone / grid
  Run run;
  // Frame.
  uint64_t frame = 0;
  int runsThisFrame = 0;
  bool sampled = false;
  std::vector<std::pair<int, int>> sizes, lastSizes;
  uint32_t verifyCounter = 0;
  bool loggedSetup = false;
  int hmdOrd = 0, lastHmdCount = 0;  // CopyToHMDPass ends in this / the last frame
  // The frame's decision (Decide): gaze, GazeOut view to sample, fallback.
  bool decided = false, haveGaze = false, focus = false, periphery = false;
  GazeOut gaze{};
  uint32_t want = 2;
  // The target run waits for DCS's water passes (FlushPending).
  Run pending;
  bool hasPending = false, pendingHmd = false, waterSeen = false;
  int pendingIdx = 0, pendingW = 0, pendingH = 0;
  bool content = false;              // a G-buffer run or an HMD copy since the frame began
  // Far depth export: the focus view waiting for its WaterSeaPass end.
  Run farPending;
  bool hasFarPending = false;
  int farIdx = 0;
  // Gaze freshness.
  uint64_t lastGazeSeq = 0;
  uint64_t gazeSeqFrame = 0;
};
State g_s;
// R51 VrFx (color_pass.h FxViewPre): every HMD view's last G-buffer run (its depth target and cPerView), kept until
// the view's next run; render thread only.
Run g_fxRun[4];
bool g_fxRunValid[4] = {};

#include "far_depth.h"  // [Perception] FarDepthExport: the focus views' depth for Quad Views' Far depth

// Shared memory (opened by Tick, used by the render thread and the worker).
HANDLE g_gazeMap = nullptr;
std::atomic<void*> g_gazeView{nullptr};
bool g_gazeOwned = false;  // created by our test writer
uint64_t g_testWriterSeq = 0;                  // the seq our last test write left (render thread)
std::atomic<bool> g_testWriterYielded{false};  // another writer appeared: test writes stopped
HANDLE g_depthMap = nullptr;
std::atomic<void*> g_depthView{nullptr};
HANDLE g_gridMap = nullptr;
std::atomic<void*> g_gridView{nullptr};
std::atomic<bool> g_testGaze{false};
std::atomic<float> g_testGx{0}, g_testGy{0};

// [Perception] CockpitTrueIpd (cockpit_ipd.h, R31 option 3): rides on this
// module's boundary callback, in-pass arm and upload hooks.
void PassName(void* pass, char* out, size_t cap);
bool ForeignTableHooks();
ID3D11Resource* PassDepthTexNow();
#include "sym_trace.h"  // R94: [Perception] SymbologyTrace (dev, default off): the live HUD trace (ovl_cpu.h reports into it)
#include "cockpit_ipd.h"

// ---------------------------------------------------------------------------
// Worker (summary + publication)
// ---------------------------------------------------------------------------

struct Job {
  Slot meta;  // staging/queries not used
  int kind = 0;  // 0 cone (DepthIn), 1 grid (DepthGrid)
  bool gpu = false;
  std::vector<float> data;
  double latency = 0;
  double gpuUs = -1;
};
std::mutex g_jobMutex;
std::condition_variable g_jobCv;
std::vector<Job> g_jobs;
// R60: jobs queued and jobs finished (processed, or dropped from a full queue): equal once the worker is idle (tests
// wait for that instead of a fixed time after the queue emptied).
std::atomic<uint64_t> g_jobsQueued{0}, g_jobsDone{0};
std::thread g_worker;
std::atomic<bool> g_workerStop{false};
int g_mismatchStreak = 0;  // worker only
// R91 (R90 phase 2, M4): the per-frame readback copies (Poll, PollGrid) reuse their buffers: the render thread takes
// one from this pool (empty: a new one), the worker gives it back after the job. Without it every frame allocated
// and freed one or two vectors (a cone readback ~36 KB, the grid ~16 KB) across the two threads.
std::mutex g_poolMutex;
std::vector<std::vector<float>> g_dataPool;  // under g_poolMutex; at most kDataPoolMax
constexpr size_t kDataPoolMax = 8;
std::atomic<uint64_t> g_poolReuses{0}, g_poolNew{0};
std::vector<float> TakeData() {
  {
    std::lock_guard<std::mutex> lock(g_poolMutex);
    if (!g_dataPool.empty()) {
      std::vector<float> v = std::move(g_dataPool.back());
      g_dataPool.pop_back();
      g_poolReuses++;
      return v;
    }
  }
  g_poolNew++;
  return std::vector<float>();
}
void GiveData(std::vector<float>&& v) {
  if (!v.capacity()) return;
  v.clear();
  std::lock_guard<std::mutex> lock(g_poolMutex);
  if (g_dataPool.size() < kDataPoolMax) g_dataPool.push_back(std::move(v));
}

void Publish(const DepthIn& d) {
  void* v = g_depthView.load();
  if (v) SeqWrite(v, d);
  g_published++;
  if (d.flags & kDepthValid) g_publishedValid++;
}

void WriteDump(const Job& j, const Window& w) {
  std::wstring dir = g_dumpDir;
  if (dir.empty()) return;
  wchar_t name[64];
  swprintf_s(name, L"fixdepth_%llu.pgm", static_cast<unsigned long long>(j.meta.frame));
  FILE* f = nullptr;
  if (_wfopen_s(&f, (dir + name).c_str(), L"wb") || !f) return;
  fprintf(f, "P5\n%d %d\n255\n", w.G, w.G);
  const Proj p = ParseProj(w.P);
  for (int k = 0; k < w.G * w.G; ++k) {
    const float raw = w.samples[k];
    uint8_t px = 0;
    if (raw >= 0 && p.ok) {
      const double fwd = ForwardDist(p, w.vp.maxD != w.vp.minD ? (raw - w.vp.minD) / (w.vp.maxD - w.vp.minD) : raw);
      const double m = std::min(fwd, kHistMaxM);
      px = static_cast<uint8_t>(std::clamp(255.0 * std::log(std::max(m, kHistMinM) / kHistMinM) /
                                               std::log(kHistMaxM / kHistMinM), 1.0, 255.0));
    }
    fputc(px, f);
  }
  fclose(f);
  float lo = 1e30f, hi = -1e30f;
  for (int k = 0; k < w.G * w.G; ++k)
    if (w.samples[k] >= 0) lo = std::min(lo, w.samples[k]), hi = std::max(hi, w.samples[k]);
  const float c = w.samples[w.half * w.G + w.half];
  Log("fixation depth: dump gProj rows %+.4g %+.4g %+.4g %+.4g | %+.4g %+.4g %+.4g %+.4g | %+.4g %+.4g %+.4g %+.4g | "
      "%+.4g %+.4g %+.4g %+.4g; centre texel %d,%d raw %.6g, raw range %.6g..%.6g",
      w.P[0], w.P[1], w.P[2], w.P[3], w.P[4], w.P[5], w.P[6], w.P[7], w.P[8], w.P[9], w.P[10], w.P[11], w.P[12],
      w.P[13], w.P[14], w.P[15], w.cx, w.cy, c, lo, hi);
  Log("fixation depth: window dump %ls%ls (%dx%d, stride %d, log range 0.2 m black .. 20 km white, 0 = outside)",
      dir.c_str(), name, w.G, w.G, w.stride);
}

void ProcessGrid(const Job& j) {
  DepthGrid g{};
  if (j.gpu && j.data.size() >= static_cast<size_t>(kGridWords)) {
    g = BuildGrid(reinterpret_cast<const uint32_t*>(j.data.data()), j.meta.gaze.displayTime, j.meta.view,
                  j.meta.afterWater);
  } else {
    g.displayTime = j.meta.gaze.displayTime;
    g.view = j.meta.view;
    g.w = g.h = kGridN;
  }
  if (void* v = g_gridView.load()) SeqWrite(v, g);
  g_gridPublished++;
  if (g.flags & kDepthValid) g_gridValid++;
  std::lock_guard<std::mutex> lock(g_statMutex);
  if (j.gpuUs >= 0) {
    g_gridUsSum += j.gpuUs;
    g_gridUsMax = std::max(g_gridUsMax, j.gpuUs);
    ++g_gridUsN;
  }
  g_gridCpuUsSum += j.meta.cpuUs;
  ++g_gridCpuN;
}

void ProcessJob(const Job& j) {
  if (j.kind == 1) {
    ProcessGrid(j);
    return;
  }
  DepthIn d{};
  d.displayTime = j.meta.gaze.displayTime;
  d.view = j.meta.view;
  d.coneDeg = static_cast<float>(j.meta.coneDeg);
  d.latencyFrames = static_cast<float>(j.latency);
  if (j.gpu && j.data.size() >= static_cast<size_t>(kHeader) + 2 * static_cast<size_t>(j.meta.G) * j.meta.G) {
    Window w;
    memcpy(w.P, j.data.data(), sizeof(w.P));
    w.cx = static_cast<int>(j.data[16]);
    w.cy = static_cast<int>(j.data[17]);
    w.G = j.meta.G;
    w.half = j.meta.half;
    w.stride = j.meta.stride;
    w.vp = j.meta.vp;
    w.gx = j.meta.gx;
    w.gy = j.meta.gy;
    w.samples = j.data.data() + kHeader;
    w.stencil = j.data.data() + kHeader + static_cast<size_t>(w.G) * w.G;
    const Summary s = Summarize(w, j.meta.coneDeg, j.meta.ringDeg);
    const Proj pr = ParseProj(w.P);
    if (!pr.ok) g_badProj++;
    bool ok = s.valid;
    Match m;
    if (pr.ok) {
      const int fi = j.meta.rank & 7;
      for (int k = 0; k < 4; ++k) g_dcsFov[fi][k] = static_cast<float>(s.dcsFov[k]);
      g_haveDcsFov[fi] = true;
      m = MatchFov(j.meta.gaze, s.dcsFov);
      if (m.best < 0) {
        g_noMatch++;
      } else if (m.decisive && m.best != static_cast<int>(j.meta.view)) {
        g_mismatch++;
        ok = false;
        if (!g_verify.load() && ++g_mismatchStreak >= 3) {
          g_mismatchStreak = 0;
          g_retarget++;
          if (j.meta.hmdMode) {
            // HMD view index = GazeOut view index + offset (0 when, as
            // measured, DCS renders the views in GazeOut order).
            const int off = std::clamp(j.meta.rank - m.best, -3, 3);
            g_hmdOffset = off;
            Log("fixation depth: HMD view %d matches GazeOut view %d, not %u; HMD index offset now %d", j.meta.rank,
                m.best, j.meta.view, off);
          } else {
            g_targetK = g_targetK.load() ^ 1;
            Log("fixation depth: the sampled run matches GazeOut view %d, not %u; switching to the other eye's run "
                "(rank %d)",
                m.best, j.meta.view, g_targetK.load());
          }
        }
      } else {
        g_matchOk++;
        g_mismatchStreak = 0;
      }
    }
    if (g_verify.load() && j.meta.rank >= 0 && j.meta.rank < 16) {
      std::lock_guard<std::mutex> lock(g_statMutex);
      RankInfo& r = g_rank[j.meta.rank];
      r.w = j.meta.w;
      r.h = j.meta.h;
      r.best = m.best;
      for (int k = 0; k < 4; ++k) r.fov[k] = s.dcsFov[k];
      r.median = s.medianM;
      r.nearShare = s.nearShare;
      // Camera position along its own right axis (eye separation sign).
      r.camX = static_cast<double>(j.data[20]) * j.data[23] + static_cast<double>(j.data[21]) * j.data[24] +
               static_cast<double>(j.data[22]) * j.data[25];
      ++r.dcsSamples;
    }
    if (g_dumpWanted.exchange(false)) WriteDump(j, w);
    if (s.valid && s.water > 0) g_waterFrames++;
    if (s.valid) {
      d.nearShare = s.nearShare;
      d.farShare = s.farShare;
      d.edgeNearShare = s.edgeNearShare;
      d.edgeFarShare = s.edgeFarShare;
      d.medianM = s.medianM;
      d.nearestM = s.nearestM;
      d.farMedianM = s.farMedianM;
      memcpy(d.histogram, s.hist, sizeof(d.histogram));
    }
    d.flags = (ok ? kDepthValid : 0) | (pr.ok && pr.reverse ? kDepthReverseZ : 0) |
              (j.meta.wantFocus ? kDepthFocus : 0) | (s.valid && s.water > 0 ? kDepthWater : 0) |
              (j.meta.afterWater ? kDepthWaterPass : 0) | (j.meta.periphery ? kDepthPeriphery : 0);
    std::lock_guard<std::mutex> lock(g_statMutex);
    if (j.gpuUs >= 0) {
      g_gpuUsSum += j.gpuUs;
      g_gpuUsMax = std::max(g_gpuUsMax, j.gpuUs);
      ++g_gpuN;
    }
    g_latSum += j.latency;
    ++g_latN;
    g_lastConv = m.best >= 0 ? m.convention : g_lastConv;
    char note[256];
    snprintf(note, sizeof(note),
             "HMD/run %d, cone %.2f deg: %d samples (water %d), ring %d, %s%s, %s, match view %d err %.4f (2nd %.4f)",
             j.meta.rank, j.meta.coneDeg, s.cone, s.water, s.ring,
             pr.ok ? (pr.reverse ? "reverse-Z" : "forward-Z") : "bad projection",
             pr.ok ? (pr.infinite ? " infinite far" : " finite far") : "",
             j.meta.afterWater ? "after the water passes" : "right after the G-buffer", m.best,
             m.err < 1e8 ? m.err : -1.0, m.second < 1e8 ? m.second : -1.0);
    g_lastNote = note;
  } else {
    d.flags = (j.meta.wantFocus ? kDepthFocus : 0) | (j.meta.periphery ? kDepthPeriphery : 0);
  }
  Publish(d);
  std::lock_guard<std::mutex> lock(g_statMutex);
  g_last = d;
}

void ProcessJobSeh(const Job& j) {
  __try {
    ProcessJob(j);
  } __except (FaultNote(GetExceptionInformation(), __LINE__)) {
    g_fault = true;
  }
}

void WorkerLoop() {
  for (;;) {
    Job j;
    {
      std::unique_lock<std::mutex> lock(g_jobMutex);
      g_jobCv.wait(lock, [] { return g_workerStop.load() || !g_jobs.empty(); });
      if (g_jobs.empty()) return;  // stop requested and drained
      j = std::move(g_jobs.front());
      g_jobs.erase(g_jobs.begin());
    }
    if (!g_fault.load()) ProcessJobSeh(j);
    GiveData(std::move(j.data));  // R91 (M4)
    g_jobsDone.fetch_add(1, std::memory_order_release);
  }
}

void PushJob(Job&& j) {
  {
    std::lock_guard<std::mutex> lock(g_jobMutex);
    if (g_jobs.size() >= 8) {  // the worker is stuck: keep the newest
      g_jobs.erase(g_jobs.begin());
      g_jobsDone.fetch_add(1, std::memory_order_release);
    }
    g_jobsQueued.fetch_add(1, std::memory_order_release);
    g_jobs.push_back(std::move(j));
  }
  g_jobCv.notify_one();
}

// ---------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------

namespace cmask {  // R65 cockpit_mask.h (included after RunCs)
void DropPending();
}
void ReleaseRun(Run& r) {
  if (r.dsv) r.dsv->Release();
  if (r.cb) r.cb->Release();
  r = Run{};
}

void ReleaseD3D() {
  State& s = g_s;
  for (int i = 0; i < 4; ++i) {  // R51
    if (g_fxRunValid[i]) ReleaseRun(g_fxRun[i]);
    g_fxRunValid[i] = false;
    cpass::g_fxView[i] = cpass::FxViewIn{};
  }
  // Far depth: the layer's references first (every generation pushed), then
  // our copies (before the device reference goes).
  fdexport::ReleaseAll();
  cmask::DropPending();  // R65: DCS's targets held for the mask (its own objects: cmask::Uninstall / EnsureBase)
  ReleaseRun(s.farPending);
  ReleaseRun(s.run);
  ReleaseRun(s.pending);
  ReleaseRun(s.gridPending);
  for (Slot& sl : s.gridRing) {
    if (sl.staging) sl.staging->Release();
    if (sl.ts0) sl.ts0->Release();
    if (sl.ts1) sl.ts1->Release();
    if (sl.dj) sl.dj->Release();
    sl = Slot{};
  }
  for (auto*& c : s.gridCs) {
    if (c) c->Release();
    c = nullptr;
  }
  if (s.gridUav) s.gridUav->Release();
  if (s.gridOut) s.gridOut->Release();
  if (s.gridParams) s.gridParams->Release();
  for (Slot& sl : s.ring) {
    if (sl.staging) sl.staging->Release();
    if (sl.ts0) sl.ts0->Release();
    if (sl.ts1) sl.ts1->Release();
    if (sl.dj) sl.dj->Release();
    sl = Slot{};
  }
  s.head = s.count = 0;
  for (SrvEntry& e : s.srv) {
    if (e.srv) e.srv->Release();
    if (e.stencil) e.stencil->Release();
    e = SrvEntry{};
  }
  for (auto*& c : s.cs) {
    if (c) c->Release();
    c = nullptr;
  }
  if (s.uav) s.uav->Release();
  if (s.out) s.out->Release();
  if (s.params) s.params->Release();
  if (s.ctx1) s.ctx1->Release();
  if (s.ctx) s.ctx->Release();
  if (s.dev) s.dev->Release();
  s = State{};
}

bool EnsureDevice(ID3D11Device* dev) {
  State& s = g_s;
  if (s.dev == dev) return s.out != nullptr;
  if (s.dev) {
    Log("fixation depth: device changed, resources recreated");
    ReleaseD3D();
  }
  if (!g_blobsReady.load()) return false;
  dev->AddRef();
  s.dev = dev;
  dev->GetImmediateContext(&s.ctx);
  s.ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&s.ctx1));
  for (int v = 0; v < 4; ++v)
    if (FAILED(dev->CreateComputeShader(g_blob[v].data(), g_blob[v].size(), nullptr, &s.cs[v]))) return false;
  D3D11_BUFFER_DESC pd{};
  pd.ByteWidth = 256;  // 16 constants: a whole *SetConstantBuffers1 range
  pd.Usage = D3D11_USAGE_DYNAMIC;
  pd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  pd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(dev->CreateBuffer(&pd, nullptr, &s.params))) return false;
  const UINT bytes = (kHeader + 2 * kMaxG * kMaxG) * 4;  // depth, then stencil
  D3D11_BUFFER_DESC od{};
  od.ByteWidth = bytes;
  od.Usage = D3D11_USAGE_DEFAULT;
  od.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  if (FAILED(dev->CreateBuffer(&od, nullptr, &s.out))) return false;
  D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
  ud.Format = DXGI_FORMAT_R32_FLOAT;
  ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  ud.Buffer.NumElements = kHeader + 2 * kMaxG * kMaxG;
  if (FAILED(dev->CreateUnorderedAccessView(s.out, &ud, &s.uav))) return false;
  for (Slot& sl : s.ring) {
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = bytes;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev->CreateBuffer(&sd, nullptr, &sl.staging))) return false;
    D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP, 0};
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    dev->CreateQuery(&q, &sl.ts0);
    dev->CreateQuery(&q, &sl.ts1);
    dev->CreateQuery(&qd, &sl.dj);
  }
  for (int v = 0; v < 4; ++v)
    if (FAILED(dev->CreateComputeShader(g_gridBlob[v].data(), g_gridBlob[v].size(), nullptr, &s.gridCs[v])))
      return false;
  if (FAILED(dev->CreateBuffer(&pd, nullptr, &s.gridParams))) return false;
  D3D11_BUFFER_DESC gd{};
  gd.ByteWidth = kGridWords * 4;
  gd.Usage = D3D11_USAGE_DEFAULT;
  gd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  gd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  if (FAILED(dev->CreateBuffer(&gd, nullptr, &s.gridOut))) return false;
  D3D11_UNORDERED_ACCESS_VIEW_DESC gu{};
  gu.Format = DXGI_FORMAT_R32_TYPELESS;
  gu.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  gu.Buffer.NumElements = kGridWords;
  gu.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  if (FAILED(dev->CreateUnorderedAccessView(s.gridOut, &gu, &s.gridUav))) return false;
  for (Slot& sl : s.gridRing) {
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = kGridWords * 4;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev->CreateBuffer(&sd, nullptr, &sl.staging))) return false;
    D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP, 0};
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    dev->CreateQuery(&q, &sl.ts0);
    dev->CreateQuery(&q, &sl.ts1);
    dev->CreateQuery(&qd, &sl.dj);
  }
  return true;
}

const SrvEntry* FindSrv(const Run& r) {
  State& s = g_s;
  SrvEntry* freeE = nullptr;
  SrvEntry* oldest = &s.srv[0];
  for (SrvEntry& e : s.srv) {
    if (e.srv && e.tex == r.tex && e.slice == r.slice && e.variant == r.variant) {
      e.lastFrame = s.frame;
      return &e;
    }
    if (!e.srv && !freeE) freeE = &e;
    if (e.lastFrame < oldest->lastFrame) oldest = &e;
  }
  const DXGI_FORMAT f = SrvFormat(r.texFmt);
  if (f == DXGI_FORMAT_UNKNOWN) return nullptr;
  D3D11_SHADER_RESOURCE_VIEW_DESC d{};
  d.Format = f;
  switch (r.variant) {
    case 0:
      d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      d.Texture2D.MipLevels = 1;
      break;
    case 1:
      d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
      d.Texture2DArray.MipLevels = 1;
      d.Texture2DArray.FirstArraySlice = r.slice;
      d.Texture2DArray.ArraySize = 1;
      break;
    case 2:
      d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
      break;
    default:
      d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
      d.Texture2DMSArray.FirstArraySlice = r.slice;
      d.Texture2DMSArray.ArraySize = 1;
      break;
  }
  ID3D11ShaderResourceView* srv = nullptr;
  if (FAILED(s.dev->CreateShaderResourceView(r.tex, &d, &srv)) || !srv) return nullptr;
  ID3D11ShaderResourceView* st = nullptr;
  d.Format = StencilSrvFormat(r.texFmt);
  if (d.Format != DXGI_FORMAT_UNKNOWN && FAILED(s.dev->CreateShaderResourceView(r.tex, &d, &st))) st = nullptr;
  SrvEntry* e = freeE ? freeE : oldest;
  if (e->srv) e->srv->Release();
  if (e->stencil) e->stencil->Release();
  *e = SrvEntry{r.tex, r.slice, r.variant, srv, st, s.frame};
  return e;
}

// Drops SRVs (and so DCS's old textures) unused for a while.
void EvictSrvs() {
  State& s = g_s;
  for (SrvEntry& e : s.srv)
    if (e.srv && s.frame - e.lastFrame > 120) {
      e.srv->Release();
      if (e.stencil) e.stencil->Release();
      e = SrvEntry{};
    }
}

// The G-buffer's frame buffer is bound only inside the pass. Measured
// 2026-10-11 (F-4E, DLSS, pass trace): between passes DCS keeps a default
// 816x804 frame buffer bound whose depth is never written (all zero after
// the G-buffer); each view has its own depth target, bound by DepthPass and
// the GBufferPasses (pushFrameBuffer) and replaced by the default one at
// each pass end (popFrameBuffer); the first GBufferPass of a view binds the
// default one first. So while a GBufferPass runs, DCS's immediate context's
// OMSetRenderTargets* entries are redirected (periph_lod.h's ArmDraw
// pattern: the table lives in the context object and d3d11 may rewrite it,
// then the pass is missed and counted) and the DSVs DCS binds are listed;
// the G-buffer's DSV is the last one that differs from the DSV bound before
// the pass (DCS's default frame buffer); see PickPassDsv.
using OmFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                      ID3D11DepthStencilView*);
using OmUavFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                         ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*,
                                         const UINT*);
using VpFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
bool g_lastArmOk = false;  // our entries were still in the table at the pass end
OmFn g_omOrig = nullptr;
OmUavFn g_omUavOrig = nullptr;
VpFn g_vpOrig = nullptr;
int g_omSlot = -1, g_omUavSlot = -1, g_vpSlot = -1;
// Far depth export: DCS's cPerView uploads inside the pass (far_depth.h 3).
using UsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT,
                                      UINT);
using Us1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*,
                                       UINT, UINT, UINT);
UsFn g_usOrig = nullptr;
Us1Fn g_us1Orig = nullptr;
bool g_ckRawUpload = false;  // cockpit ipd's helmet switch uploads (render thread): forwarded past every hook
int g_usSlot = -1, g_us1Slot = -1;
bool g_usArmed = false;    // the upload entries are ours in this arm
bool g_lastUsOk = false;   // ... and were still ours at the pass end (diagnostic)
void** g_armTable = nullptr;
// Cockpit ipd's forward-pass arm (ArmUsOnly, below): g_usOnlyArm when it owns
// the arm (upload entries, OM / viewport for the re-arm only); g_fwdNested
// when it runs inside another feature's in-pass arm (far depth HDR / overlay /
// trace, the verify trace) and shares its upload entries (g_fwdAddedUs: it
// added them to an arm that had none; they go again at the forward pass end).
bool g_usOnlyArm = false;
bool g_usOnlyOm = false;  // the forward-pass arm also holds the OM / viewport entries
bool g_fwdNested = false, g_fwdAddedUs = false;
bool g_ovlAddedDraws = false;  // the draw entries were added to another feature's arm for a nested overlay pass
// R32 r3: the arm held is cockpit ipd's (its own forward / overlay arm, its share of another feature's arm, its
// lighting arm). Only such an arm takes R32's Unmap toggle point and the "normal, not ours" repair: with
// CockpitTrueIpd=0 far depth's arms behave exactly as before R32 (Flush / Map / GetData, toggles repaired only
// when the table shows them).
bool g_ckLightArm = false;
inline bool CkArm() { return g_usOnlyArm || g_fwdNested || g_ckLightArm; }
void STDMETHODCALLTYPE UsHook(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                              const void* src, UINT rp, UINT dp);
void STDMETHODCALLTYPE Us1Hook(ID3D11DeviceContext1* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                               const void* src, UINT rp, UINT dp, UINT flags);
// d3d11 switches the update/copy entries of the immediate context's table to
// another variant: Flush and ExecuteCommandList toggle the draw/copy/clear
// slots, and the first copy after a Flush rewrites its own slot from inside
// d3d11 (split_filter.h, gpu_pass_timing.h, offline on this PC). That drops
// our upload hooks for the rest of the pass: "arm missed" (live 2026-10-13,
// about 2.7 % of the focus views). Like gpu_pass_timing.h, the current entry
// is taken as the original and the hook put back, after each forwarded
// upload and at each OM / viewport call of the pass (render thread).
// Table writes of the in-pass arms (hundreds per frame since the overlay passes arm their draw and toggle-point
// entries): d3ds::WriteTablePointer asks VirtualQuery for every write (a system call each, ~1 ms per frame
// live 2026-10-13); the context table lives in the context object's writable memory, so the region found
// writable is remembered and written directly.
uintptr_t g_rwBase = 0, g_rwEnd = 0;
void FdWriteSlot(void** slot, void* value) {
  const uintptr_t a = reinterpret_cast<uintptr_t>(slot);
  if (a >= g_rwBase && a + sizeof(void*) <= g_rwEnd) {
    InterlockedExchangePointer(slot, value);
    return;
  }
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(slot, &mbi, sizeof(mbi)) &&
      (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mbi.Protect & PAGE_GUARD)) {
    g_rwBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    g_rwEnd = g_rwBase + mbi.RegionSize;
    InterlockedExchangePointer(slot, value);
    return;
  }
  d3ds::WriteTablePointer(slot, value);  // not plainly writable: the protect / write / restore path
}
// Uninstall (config thread) stops every re-arm before it disarms: g_rearmOk
// false, then g_hookBusy drained, so no render-thread re-arm can write a hook
// back into the table after the disarm restored d3d11's entry.
std::atomic<bool> g_rearmOk{true};
std::atomic<int> g_hookBusy{0};
struct HookBusy {
  HookBusy() { g_hookBusy.fetch_add(1, std::memory_order_acq_rel); }
  ~HookBusy() { g_hookBusy.fetch_sub(1, std::memory_order_acq_rel); }
};
// d3d11's own entries of the upload slots in the table's normal state (after
// any call through them), learned after each upload of ours; a slot holding
// something else (neither our hook nor these) is in the after-Flush state.
void* g_usNormal = nullptr;
void* g_us1Normal = nullptr;
bool g_rearmAfterUpload = false;  // UsRearm runs right after our own forwarded upload (a switch there is benign)
// Diagnostics: the last of our hooks DCS's render thread went through (0 none, 1 upload, 2 upload1, 3 draw, 4 OM,
// 5 viewport, 10 + k toggle point k), and per kind how often a toggle was found right after the next one.
int g_lastCall = 0;
std::atomic<uint64_t> g_togglePrev[32] = {};
void UsRearm() {
  void** vt = g_armTable;
  if (!g_rearmOk.load(std::memory_order_acquire)) return;
  if (!g_usArmed || !vt || !g_s.ctx || *reinterpret_cast<void***>(g_s.ctx) != vt) return;
  void* cur = vt[g_usSlot];
  // Found switched at a re-arm point that is not our own upload (OM / viewport / draw / toggle point) inside an
  // overlay pass: uploads since the switch may have gone past the hook (counted per view).
  // (Right after a draw of ours the repair comes before any upload: not counted.)
  if (cur != reinterpret_cast<void*>(&UsHook) && !g_rearmAfterUpload && g_lastCall != 3 && ckipd::t_cls == ckipd::kClsOvl)
    ckipd::NoteOvlMidLoss();
  if (cur != reinterpret_cast<void*>(&UsHook)) {
    g_usOrig = reinterpret_cast<UsFn>(cur);
    FdWriteSlot(&vt[g_usSlot], reinterpret_cast<void*>(&UsHook));
    fdexport::g_usRearms++;
  }
  cur = vt[g_us1Slot];
  if (cur != reinterpret_cast<void*>(&Us1Hook)) {
    g_us1Orig = reinterpret_cast<Us1Fn>(cur);
    FdWriteSlot(&vt[g_us1Slot], reinterpret_cast<void*>(&Us1Hook));
    fdexport::g_usRearms++;
  }
}
// After each forwarded upload of the pass both the upload and the draw
// entries are put back (InPassRearm, below): the binder's cPerView upload is
// the last call before an effect's draw, so a draw-slot toggle before it (a
// Flush / ExecuteCommandList, or one inside that very upload) no longer lets
// the draw past the draw hooks (cockpit ipd's lighting split, live
// 2026-10-13: ~0.3 % of the patched views lit from the boosted eye).
void InPassRearm();
// Cockpit ipd's upload path: DCS's immediate context on the render thread
// only (its per-pass state is the render thread's); another thread's upload
// on the same context is forwarded untouched (counted).
inline const void* CkUpload(ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, bool us1) {
  if (!ckipd::g_on.load(std::memory_order_relaxed)) return src;
  ckipd::CostScope cost(ckipd::kCostUpload);
  if (GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed)) {
    ckipd::g_st.usOtherThread++;
    return src;
  }
  return ckipd::OnUploadSeh(r, sub, box, src, us1);
}
void STDMETHODCALLTYPE UsHook(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                              const void* src, UINT rp, UINT dp) {
  HookBusy busy;
  const bool ours = c == g_s.ctx && !g_ckRawUpload;
  if (ours) fdexport::NoteUpload(r, sub, box, src);  // DCS's original bytes, as before
  // Cockpit ipd: a patched private copy for a cockpit pass's cPerView; else DCS's pointer.
  const void* fwd = ours ? CkUpload(r, sub, box, src, false) : src;
  g_usOrig(c, r, sub, box, fwd, rp, dp);
  if (ours) {
    g_rearmAfterUpload = true;
    InPassRearm();
    g_rearmAfterUpload = false;
    g_lastCall = 1;
    g_usNormal = reinterpret_cast<void*>(g_usOrig);  // a call went through it: the table is in its normal state
    g_us1Normal = reinterpret_cast<void*>(g_us1Orig);
  }
}
void STDMETHODCALLTYPE Us1Hook(ID3D11DeviceContext1* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                               const void* src, UINT rp, UINT dp, UINT flags) {
  HookBusy busy;
  // NO_OVERWRITE / DISCARD keep the bytes' meaning: the source is the new content of the box.
  const bool ours = static_cast<ID3D11DeviceContext*>(c) == g_s.ctx && !g_ckRawUpload;
  if (ours) fdexport::NoteUpload(r, sub, box, src);
  const void* fwd = ours ? CkUpload(r, sub, box, src, true) : src;
  g_us1Orig(c, r, sub, box, fwd, rp, dp, flags);
  if (ours) {
    g_rearmAfterUpload = true;
    InPassRearm();
    g_rearmAfterUpload = false;
    g_usNormal = reinterpret_cast<void*>(g_usOrig);
    g_us1Normal = reinterpret_cast<void*>(g_us1Orig);
  }
}
// Cockpit ipd's helmet camera switch (cockpit_ipd.h): an upload into the pass's own cPerView buffer that neither
// the restore entries, far depth's matrices nor the patch may take for DCS's (whichever entry is in the table).
void ckipd::RawUpload(ID3D11Resource* r, const D3D11_BOX* box, const void* src, bool us1) {
  State& s = g_s;
  if (!s.ctx) return;
  g_ckRawUpload = true;
  if (us1 && s.ctx1)
    s.ctx1->UpdateSubresource1(r, 0, box, src, 0, 0, 0);
  else
    s.ctx->UpdateSubresource(r, 0, box, src, 0, 0);
  g_ckRawUpload = false;
}
// Other modules that re-arm their own hooks in the same context table and
// take whatever entry they find as d3d11's: gpu_pass_timing.h (copy / update /
// clear / dispatch slots, incl. UpdateSubresource) and the D3D
// meter (d3dstate.h: draws, OM). Two such re-arms on one slot adopt each
// other's hook as the original: an endless call cycle (stack overflow). While
// one of them holds the table, the upload and draw hooks stay out.
bool ForeignTableHooks() {
  return gpt::g_opsHooked.load(std::memory_order_relaxed) || d3ds::g_vtblCount.load(std::memory_order_relaxed) > 0;
}

// Far depth 5b: the draw entries, redirected while a focus view's
// OverlayLDRPass is watched for its scene composite (or the overlay trace
// window is open); re-armed like the upload entries. Each hook forwards
// exactly DCS's call; the copy (when due) follows the forwarded draw.
using DrawFnT = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIdxFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawInstFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawIdxInstFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawAutoFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using DrawIndFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
void* volatile g_dOrig[fdexport::kDkCount] = {};
int g_dSlot[fdexport::kDkCount] = {-1, -1, -1, -1, -1, -1, -1};
bool g_drawArmed = false;  // the draw entries are ours in this arm
void* DrawHookOf(int k);
int DrawPreSeh(int kind, UINT count, UINT inst);  // below; 1: copy after the draw
void DrawPostSeh(int act);
void DrawRearm() {
  void** vt = g_armTable;
  if (!g_rearmOk.load(std::memory_order_acquire)) return;
  if (!g_drawArmed || !vt || !g_s.ctx || *reinterpret_cast<void***>(g_s.ctx) != vt) return;
  for (int k = 0; k < fdexport::kDkCount; ++k) {
    void* cur = vt[g_dSlot[k]];
    if (cur == DrawHookOf(k)) continue;
    g_dOrig[k] = cur;
    FdWriteSlot(&vt[g_dSlot[k]], DrawHookOf(k));
    fdexport::g_drawRearms++;
  }
}
void OvlDrawSeh(ID3D11DeviceContext* c);  // below (cockpit ipd: an overlay pass's draw, probe)
#define FD_DRAW_BODY(KIND, FN, COUNT, INST, ...)                      \
  HookBusy busy;                                                       \
  ckipd::CostScope cost(ckipd::kCostDraw);                             \
  const bool ours = c == g_s.ctx && !g_fault.load(std::memory_order_relaxed); \
  if (c == g_s.ctx) ckipd::LightDrawSeen();                           \
  if (c == g_s.ctx && ckipd::t_cls == ckipd::kClsOvl) OvlDrawSeh(c);  \
  const int act = ours ? DrawPreSeh(KIND, COUNT, INST) : 0;            \
  reinterpret_cast<FN>(g_dOrig[KIND])(__VA_ARGS__);                    \
  if (ours) DrawPostSeh(act);
bool SplitDrawSeh(ID3D11DeviceContext* c, UINT n, UINT sv);  // below (cockpit ipd phase 2)
void STDMETHODCALLTYPE DhDraw(ID3D11DeviceContext* c, UINT n, UINT sv) {
  if (ckipd::t_cls == ckipd::kClsLight && c == g_s.ctx) {
    HookBusy busy;
    ckipd::LightDrawSeen();
    if (SplitDrawSeh(c, n, sv)) {
      DrawRearm();
      return;
    }
    // Forwarded once as DCS's (seen above: not counted again by the body).
    const bool ours = !g_fault.load(std::memory_order_relaxed);
    const int act = ours ? DrawPreSeh(fdexport::kDkDraw, n, 1) : 0;
    reinterpret_cast<DrawFnT>(g_dOrig[fdexport::kDkDraw])(c, n, sv);
    if (ours) DrawPostSeh(act);
    return;
  }
  FD_DRAW_BODY(fdexport::kDkDraw, DrawFnT, n, 1, c, n, sv)
}
void STDMETHODCALLTYPE DhDrawIndexed(ID3D11DeviceContext* c, UINT n, UINT si, INT bv) {
  FD_DRAW_BODY(fdexport::kDkIndexed, DrawIdxFn, n, 1, c, n, si, bv)
}
void STDMETHODCALLTYPE DhDrawInstanced(ID3D11DeviceContext* c, UINT n, UINT inst, UINT sv, UINT si) {
  FD_DRAW_BODY(fdexport::kDkInstanced, DrawInstFn, n, inst, c, n, inst, sv, si)
}
void STDMETHODCALLTYPE DhDrawIndexedInstanced(ID3D11DeviceContext* c, UINT n, UINT inst, UINT si, INT bv, UINT sin) {
  FD_DRAW_BODY(fdexport::kDkIndexedInstanced, DrawIdxInstFn, n, inst, c, n, inst, si, bv, sin)
}
void STDMETHODCALLTYPE DhDrawAuto(ID3D11DeviceContext* c) { FD_DRAW_BODY(fdexport::kDkAuto, DrawAutoFn, 0, 0, c) }
void STDMETHODCALLTYPE DhDrawIndexedInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
  FD_DRAW_BODY(fdexport::kDkIndexedIndirect, DrawIndFn, 0, 0, c, b, o)
}
void STDMETHODCALLTYPE DhDrawInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
  FD_DRAW_BODY(fdexport::kDkInstancedIndirect, DrawIndFn, 0, 0, c, b, o)
}
#undef FD_DRAW_BODY
void* DrawHookOf(int k) {
  static void* const hooks[fdexport::kDkCount] = {
      reinterpret_cast<void*>(&DhDraw),          reinterpret_cast<void*>(&DhDrawIndexed),
      reinterpret_cast<void*>(&DhDrawInstanced), reinterpret_cast<void*>(&DhDrawIndexedInstanced),
      reinterpret_cast<void*>(&DhDrawAuto),      reinterpret_cast<void*>(&DhDrawIndexedInstancedIndirect),
      reinterpret_cast<void*>(&DhDrawInstancedIndirect)};
  return hooks[k];
}
// Into the current arm (g_armTable); not with another module's hooks in the
// table, and not into the forward cockpit pass's own arm (it wants no draws).
void ArmDraws() {
  void** vt = g_armTable;
  if (g_drawArmed || !vt || g_usOnlyArm) return;
  if (ForeignTableHooks()) {
    fdexport::g_drawSkipForeign++;
    return;
  }
  if (g_dSlot[0] < 0)
    for (int k = 0; k < fdexport::kDkCount; ++k) g_dSlot[k] = DcsQv_ContextSlot(fdexport::kDrawNames[k]);
  for (int k = 0; k < fdexport::kDkCount; ++k)
    if (g_dSlot[k] < 0 || vt[g_dSlot[k]] == DrawHookOf(k)) return;  // unknown slot / a stale arm
  for (int k = 0; k < fdexport::kDkCount; ++k) g_dOrig[k] = vt[g_dSlot[k]];
  for (int k = 0; k < fdexport::kDkCount; ++k) FdWriteSlot(&vt[g_dSlot[k]], DrawHookOf(k));
  g_drawArmed = true;
  fdexport::g_drawArms++;
}
void DisarmDraws() {
  void** vt = g_armTable;
  if (!g_drawArmed || !vt) return;
  g_drawArmed = false;
  bool lost = false;
  for (int k = 0; k < fdexport::kDkCount; ++k) {
    if (vt[g_dSlot[k]] == DrawHookOf(k))
      FdWriteSlot(&vt[g_dSlot[k]], g_dOrig[k]);
    else
      lost = true;  // d3d11 switched it after our last re-arm: its entry stays
  }
  if (lost) fdexport::g_drawLost++;
}
void InPassRearm() {
  UsRearm();
  DrawRearm();
}

// d3d11 toggles 24 draw / copy / update / clear entries of the immediate
// context's table whenever it flushes on a call that is not one of them
// (offline, WARP and this PC's hardware driver, 2026-10-13: Flush,
// ExecuteCommandList, GetData, a Map that has to wait; the toggle writes
// "after flush" variants into the 24 slots, the first call through any of
// them writes the normal ones back; command-buffer-full submissions inside a
// draw or upload toggle nothing). Between the toggle and our next re-arm
// point every upload and draw goes past our hooks: the first one after it
// always (it is d3d11's "after flush" entry itself), the rest until an OM /
// viewport call (overlay passes: about 80 effect uploads per pass and almost
// no OM calls). Flush, Map and GetData are not among the 24: they are hooked
// with the OM / viewport entries of every in-pass arm (toggle points), and
// ExecuteCommandList is reported by the split filter's hook (or our recorders
// after their own). After one of them, when the upload entry shows the toggle:
// the table is put back to its normal state right away by one private 16-byte
// upload through d3d11's current entry (the "after flush" variant restores
// the normal 24), then our entries are re-armed on it. An arm taken on a
// table still in the after-flush state (a toggle between passes: DLSS, the
// runtime) is normalized the same way first.
using FlushFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                         D3D11_MAPPED_SUBRESOURCE*);
using GetDataFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*, void*, UINT, UINT);
// Toggle points: Flush, Map, GetData (seen toggling offline and live) in every in-pass arm. In the overlay
// passes also the state calls DCS's effects make between two draws (live 2026-10-13, probed one by one: the
// table was found toggled right after IASetVertexBuffers, PSSetShaderResources, IASetIndexBuffer,
// VSSetShaderResources, GSSetShader, i.e. d3d11 submits inside them; with only the draws and Flush / Map /
// GetData re-arming, ~3 % of the frames still drew symbols at the boosted eye; with those five, view 0's
// overlay pass still lost the arm mid-pass in ~6 % of its frames: every state call an effect makes is hooked
// now, none of them a toggled entry) and the
// draws themselves (CockpitIpdOverlayBegin). Only in the overlay arms: the G-buffer arms make thousands of
// such calls per frame.
enum TogglePoint {
  // Basic (every in-pass arm): Flush, Map, GetData, Unmap (R32: the cpu path's observer reads a mapped
  // constant buffer at its Unmap).
  kTpFlush = 0, kTpMap, kTpGetData, kTpUnmap, kTpBasic,
  kTpPSSetSRV = kTpBasic, kTpVSSetSRV, kTpIASetVB, kTpIASetIB, kTpGSSetShader, kTpBegin, kTpEnd, kTpVSSetCB,
  kTpPSSetCB, kTpPSSetShader, kTpVSSetShader, kTpPSSetSamplers, kTpVSSetSamplers, kTpIASetLayout, kTpIASetTopo,
  kTpGSSetSRV, kTpOMSetBlend, kTpOMSetDepth, kTpRSSetState,
  // R32 4.5: the remaining state calls of an effect between two draws, none of them among d3d11's 22 toggled
  // entries (probed on this PC's driver and on WARP, 2026-10-13: every draw / dispatch / copy / update / clear /
  // resolve / GenerateMips entry toggles, so CopySubresourceRegion and the clears cannot be toggle points: they are
  // toggled entries themselves, and the first call through one writes d3d11's normal entries back, which the next
  // toggle point now repairs).
  kTpVSSetCB1, kTpPSSetCB1, kTpGSSetCB1, kTpRSSetScissor, kTpGSSetCB, kTpGSSetSamplers, kTpCount
};
const char* const kTpNames[kTpCount] = {"Flush",               "Map",                "GetData",
                                        "Unmap",                "PSSetShaderResources", "VSSetShaderResources", "IASetVertexBuffers",
                                        "IASetIndexBuffer",     "GSSetShader",          "Begin",
                                        "End",                  "VSSetConstantBuffers", "PSSetConstantBuffers",
                                        "PSSetShader",          "VSSetShader",          "PSSetSamplers",
                                        "VSSetSamplers",        "IASetInputLayout",     "IASetPrimitiveTopology",
                                        "GSSetShaderResources", "OMSetBlendState",      "OMSetDepthStencilState",
                                        "RSSetState",           "VSSetConstantBuffers1", "PSSetConstantBuffers1",
                                        "GSSetConstantBuffers1", "RSSetScissorRects",   "GSSetConstantBuffers",
                                        "GSSetSamplers"};
void* g_tpOrig[kTpCount] = {};
int g_tpSlot[kTpCount] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static_assert(kTpCount == 29, "g_tpSlot's initializer");
bool g_tpExtended = false;  // the next ArmOmEntries also takes the overlay passes' state-call toggle points
bool g_tpCk = false;        // the next ArmOmEntries is cockpit ipd's: R32's Unmap toggle point too (R32 r3)
bool g_tpAddedUnmap = false;  // the Unmap point added to another feature's arm for a nested cockpit pass
bool g_tpArmed[kTpCount] = {};
std::atomic<uint64_t> g_tpHits[kTpCount] = {};  // the table found toggled right after this call
// R32 4.5: the arm found "normal, not ours" right after this call (d3d11 had toggled the table and the first call
// through a toggled entry wrote its normal entries over our hooks; UsToggled cannot see that state), repaired there.
std::atomic<uint64_t> g_tpNormalRepair[kTpCount] = {};
void* TogglePointHook(int k);  // below
ID3D11Buffer* g_tinyCb = nullptr;   // our 16-byte constant buffer for the normalizing upload
ID3D11Device* g_tinyDev = nullptr;  // its device (identity)
// The table holds d3d11's "after flush" upload entry (neither ours nor the learned normal one).
bool UsToggled(void** vt) {
  void* cur = vt[g_usSlot];
  return g_usNormal && cur != reinterpret_cast<void*>(&UsHook) && cur != g_usNormal;
}
// R32 4.5 (the hole UsToggled leaves): d3d11's toggle writes its "after flush" entries into its 22 toggled slots,
// and the first call through any of them writes the NORMAL entries back into all of them. After that one call the
// table looks normal (UsToggled false) while our upload / draw hooks are gone for the rest of the pass (probe
// r32 tog.cpp, this PC's driver and WARP). True when an entry our current arm holds is not ours any more.
bool ArmNotOurs(void** vt) {
  if (!vt || vt != g_armTable) return false;
  if (g_usArmed && (vt[g_usSlot] != reinterpret_cast<void*>(&UsHook) || vt[g_us1Slot] != reinterpret_cast<void*>(&Us1Hook)))
    return true;
  if (g_drawArmed)
    for (int k = 0; k < fdexport::kDkCount; ++k)
      if (g_dSlot[k] >= 0 && vt[g_dSlot[k]] != DrawHookOf(k)) return true;
  return false;
}
extern int g_cpView;  // R47 (colour pass, below): a ToneMappingPass armed, its tone-map draw still to come
// A toggle point found the arm "normal, not ours": put back right there (counted per call, so the live run names
// the call d3d11 submits in).
void RepairNormal(int point) {
  if (point >= 0 && point < kTpCount) g_tpNormalRepair[point]++;
  ckipd::g_st.tpNormalRepair++;
  InPassRearm();
}
// One private upload through d3d11's current UpdateSubresource entry (render thread, DCS's context).
void NormalizeTable(void** vt) {
  State& s = g_s;
  if (!s.ctx || !vt || g_usSlot < 0) return;
  {
    ID3D11Device* dev = nullptr;
    s.ctx->GetDevice(&dev);
    if (dev != g_tinyDev && g_tinyCb) {  // another device (a new DCS device, tests): its own buffer
      g_tinyCb->Release();
      g_tinyCb = nullptr;
    }
    if (!g_tinyCb && dev) {
      D3D11_BUFFER_DESC d{};
      d.ByteWidth = 16;
      d.Usage = D3D11_USAGE_DEFAULT;
      d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      dev->CreateBuffer(&d, nullptr, &g_tinyCb);
    }
    g_tinyDev = g_tinyCb ? dev : nullptr;  // identity only
    if (dev) dev->Release();
    if (!g_tinyCb) return;
  }
  static const float kZero[4] = {};
  void* cur = vt[g_usSlot];
  UsFn fn = cur == reinterpret_cast<void*>(&UsHook) ? g_usOrig : reinterpret_cast<UsFn>(cur);
  if (!fn) return;
  fn(s.ctx, g_tinyCb, 0, nullptr, kZero, 0, 0);
  ckipd::g_st.tblNormalized++;
  // A slot that did not change on a call is d3d11's normal entry (another table set: learned anew).
  if (vt[g_usSlot] == cur && cur != reinterpret_cast<void*>(&UsHook)) {
    g_usNormal = cur;
    g_us1Normal = vt[g_us1Slot];
  }
}
// After a toggle point on DCS's context while an arm of ours is held (render
// thread). sure: the call always toggles (Flush, ExecuteCommandList); else
// only a table that shows the toggle is repaired (Map / GetData usually do not).
void AfterToggle(bool sure, int point = -1) {
  void** vt = g_armTable;
  if (!vt || !g_s.ctx || *reinterpret_cast<void***>(g_s.ctx) != vt || !g_rearmOk.load(std::memory_order_acquire))
    return;
  const bool toggled = UsToggled(vt);
  if (toggled && point >= 0 && point < kTpCount) g_tpHits[point]++;
  if (toggled) g_togglePrev[g_lastCall < 32 ? g_lastCall : 31]++;
  if (point >= 0) g_lastCall = 10 + point;
  if (!toggled && !(sure && !g_usNormal)) {
    // (Reported twice for one ExecuteCommandList when the split filter's hook and a recorder both report it: the
    // second finds our hook back in the slot and does nothing more.)
    if (sure)
      InPassRearm();
    else if ((CkArm() || (g_cpView >= 0 && (cpass::g_armRepair.load(std::memory_order_relaxed) & 2))) &&
             ArmNotOurs(vt))  // R47: the colour pass's tone-map arm too
      RepairNormal(point);  // R32 4.5: a toggled entry was called since the toggle: normal entries over our hooks
    return;
  }
  ckipd::g_st.tblToggles++;
  if (ckipd::t_cls == ckipd::kClsOvl) ckipd::g_st.ovlToggles++;
  if (g_usArmed || g_drawArmed) NormalizeTable(vt);
  InPassRearm();
}
void AfterToggleSeh(bool sure, int point = -1) {
  __try {
    AfterToggle(sure, point);
  } __except (FaultNote(GetExceptionInformation(), __LINE__)) {
    g_fault = true;
  }
}
inline bool OnRenderCtx(ID3D11DeviceContext* c) {
  if (c != g_s.ctx) return false;
  if (GetCurrentThreadId() == ptiming::g_topTid.load(std::memory_order_relaxed)) return true;
  ckipd::g_st.tpOtherThread++;  // a toggle point on DCS's context from another thread (not repaired here)
  return false;
}
void STDMETHODCALLTYPE FlushHook(ID3D11DeviceContext* c) {
  HookBusy busy;
  reinterpret_cast<FlushFn>(g_tpOrig[kTpFlush])(c);
  if (OnRenderCtx(c)) AfterToggleSeh(true, kTpFlush);
}
void TpAfter(ID3D11DeviceContext* c, int k);  // below
HRESULT STDMETHODCALLTYPE MapHook(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags,
                                  D3D11_MAPPED_SUBRESOURCE* m) {
  HookBusy busy;
  const HRESULT hr = reinterpret_cast<MapFn>(g_tpOrig[kTpMap])(c, r, sub, type, flags, m);
  if (OnRenderCtx(c)) {
    if (ckipd::t_cls == ckipd::kClsOvl && SUCCEEDED(hr)) ckipd::OvlNoteMap(r, sub, type, m);  // diagnostic; cpu observer
    AfterToggleSeh(false, kTpMap);
  }
  return hr;
}
// Unmap: a toggle point like the state calls; a constant buffer an open overlay pass mapped for writing is
// observed first (the cpu path's learner: DCS's bytes as written).
void STDMETHODCALLTYPE UnmapHook(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
  HookBusy busy;
  // Only the render thread's own Unmap (R32 r3: another thread's Unmap of DCS's context, with an overlay pass
  // open on the render thread, must not consume or observe that pass's mapping).
  if (c == g_s.ctx && ckipd::t_cls == ckipd::kClsOvl &&
      GetCurrentThreadId() == ptiming::g_topTid.load(std::memory_order_relaxed))
    ckipd::OvlNoteUnmap(r, sub);
  reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT)>(g_tpOrig[kTpUnmap])(c, r, sub);
  if (c == g_s.ctx) TpAfter(c, kTpUnmap);
}
HRESULT STDMETHODCALLTYPE GetDataHook(ID3D11DeviceContext* c, ID3D11Asynchronous* a, void* d, UINT n, UINT flags) {
  HookBusy busy;
  const HRESULT hr = reinterpret_cast<GetDataFn>(g_tpOrig[kTpGetData])(c, a, d, n, flags);
  if (c == g_s.ctx) g_lastCall = 10 + kTpGetData;
  if (OnRenderCtx(c)) AfterToggleSeh(false, kTpGetData);
  return hr;
}
// After a state-call toggle point on DCS's context (R32 4.5): a table that shows the toggle is normalized and
// re-armed (AfterToggle); one whose arm is "normal, not ours" (a toggled entry was called since the toggle, which
// wrote d3d11's normal entries over our hooks: UsToggled cannot see it) is re-armed too (RepairNormal). Before
// 2026-10-13 only the first case was repaired, so after one draw or upload the rest of the pass went past us.
void TpAfter(ID3D11DeviceContext* c, int k) {
  void** vt = *reinterpret_cast<void***>(c);
  const bool tog = UsToggled(vt);
  const bool cpRep = g_cpView >= 0 && (cpass::g_armRepair.load(std::memory_order_relaxed) & 2);  // R47
  if ((tog || ((CkArm() || cpRep) && ArmNotOurs(vt))) && OnRenderCtx(c)) {
    if (tog) {
      AfterToggleSeh(false, k);
    } else {
      __try {
        RepairNormal(k);
      } __except (FaultNote(GetExceptionInformation(), __LINE__)) {
        g_fault = true;
      }
    }
  }
  g_lastCall = 10 + k;
}
#define FD_TP_HOOK(NAME, K, PARAMS, ARGS)                                              \
  void STDMETHODCALLTYPE NAME PARAMS {                                                \
    HookBusy busy;                                                                    \
    using Fn = void(STDMETHODCALLTYPE*) PARAMS;                                       \
    reinterpret_cast<Fn>(g_tpOrig[K]) ARGS;                                           \
    if (c == g_s.ctx) TpAfter(c, K);                                                  \
  }
FD_TP_HOOK(TpPSSetSRV, kTpPSSetSRV, (ID3D11DeviceContext * c, UINT s0, UINT n, ID3D11ShaderResourceView* const* v),
           (c, s0, n, v))
FD_TP_HOOK(TpVSSetSRV, kTpVSSetSRV, (ID3D11DeviceContext * c, UINT s0, UINT n, ID3D11ShaderResourceView* const* v),
           (c, s0, n, v))
FD_TP_HOOK(TpIASetVB, kTpIASetVB,
           (ID3D11DeviceContext * c, UINT s0, UINT n, ID3D11Buffer* const* b, const UINT* st, const UINT* of),
           (c, s0, n, b, st, of))
FD_TP_HOOK(TpIASetIB, kTpIASetIB, (ID3D11DeviceContext * c, ID3D11Buffer * b, DXGI_FORMAT f, UINT o), (c, b, f, o))
FD_TP_HOOK(TpGSSetShader, kTpGSSetShader,
           (ID3D11DeviceContext * c, ID3D11GeometryShader * g, ID3D11ClassInstance* const* ci, UINT n), (c, g, ci, n))
FD_TP_HOOK(TpBegin, kTpBegin, (ID3D11DeviceContext * c, ID3D11Asynchronous * a), (c, a))
FD_TP_HOOK(TpEnd, kTpEnd, (ID3D11DeviceContext * c, ID3D11Asynchronous * a), (c, a))
FD_TP_HOOK(TpVSSetCB, kTpVSSetCB, (ID3D11DeviceContext * c, UINT s0, UINT n, ID3D11Buffer* const* b), (c, s0, n, b))
FD_TP_HOOK(TpPSSetCB, kTpPSSetCB, (ID3D11DeviceContext * c, UINT s0, UINT n, ID3D11Buffer* const* b), (c, s0, n, b))
FD_TP_HOOK(TpPSSetShader, kTpPSSetShader,
           (ID3D11DeviceContext * c, ID3D11PixelShader * sh, ID3D11ClassInstance* const* ci, UINT n), (c, sh, ci, n))
FD_TP_HOOK(TpVSSetShader, kTpVSSetShader,
           (ID3D11DeviceContext * c, ID3D11VertexShader * sh, ID3D11ClassInstance* const* ci, UINT n), (c, sh, ci, n))
FD_TP_HOOK(TpPSSetSamplers, kTpPSSetSamplers,
           (ID3D11DeviceContext * c, UINT s0, UINT n, ID3D11SamplerState* const* sm), (c, s0, n, sm))
FD_TP_HOOK(TpVSSetSamplers, kTpVSSetSamplers,
           (ID3D11DeviceContext * c, UINT s0, UINT n, ID3D11SamplerState* const* sm), (c, s0, n, sm))
FD_TP_HOOK(TpIASetLayout, kTpIASetLayout, (ID3D11DeviceContext * c, ID3D11InputLayout * l), (c, l))
FD_TP_HOOK(TpIASetTopo, kTpIASetTopo, (ID3D11DeviceContext * c, D3D11_PRIMITIVE_TOPOLOGY t), (c, t))
FD_TP_HOOK(TpGSSetSRV, kTpGSSetSRV, (ID3D11DeviceContext * c, UINT s0, UINT n, ID3D11ShaderResourceView* const* v),
           (c, s0, n, v))
FD_TP_HOOK(TpOMSetBlend, kTpOMSetBlend, (ID3D11DeviceContext * c, ID3D11BlendState * b, const FLOAT f[4], UINT m),
           (c, b, f, m))
FD_TP_HOOK(TpOMSetDepth, kTpOMSetDepth, (ID3D11DeviceContext * c, ID3D11DepthStencilState * d, UINT r), (c, d, r))
FD_TP_HOOK(TpRSSetState, kTpRSSetState, (ID3D11DeviceContext * c, ID3D11RasterizerState * r), (c, r))
FD_TP_HOOK(TpVSSetCB1, kTpVSSetCB1,
           (ID3D11DeviceContext * c, UINT s0, UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* m),
           (c, s0, n, b, f, m))
FD_TP_HOOK(TpPSSetCB1, kTpPSSetCB1,
           (ID3D11DeviceContext * c, UINT s0, UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* m),
           (c, s0, n, b, f, m))
FD_TP_HOOK(TpGSSetCB1, kTpGSSetCB1,
           (ID3D11DeviceContext * c, UINT s0, UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* m),
           (c, s0, n, b, f, m))
FD_TP_HOOK(TpRSSetScissor, kTpRSSetScissor, (ID3D11DeviceContext * c, UINT n, const D3D11_RECT* r), (c, n, r))
FD_TP_HOOK(TpGSSetCB, kTpGSSetCB, (ID3D11DeviceContext * c, UINT s0, UINT n, ID3D11Buffer* const* b), (c, s0, n, b))
FD_TP_HOOK(TpGSSetSamplers, kTpGSSetSamplers,
           (ID3D11DeviceContext * c, UINT s0, UINT n, ID3D11SamplerState* const* sm), (c, s0, n, sm))
#undef FD_TP_HOOK
void* TogglePointHook(int k) {
  static void* const hooks[kTpCount] = {
      reinterpret_cast<void*>(&FlushHook),  reinterpret_cast<void*>(&MapHook),     reinterpret_cast<void*>(&GetDataHook),
      reinterpret_cast<void*>(&UnmapHook),  reinterpret_cast<void*>(&TpPSSetSRV), reinterpret_cast<void*>(&TpVSSetSRV),  reinterpret_cast<void*>(&TpIASetVB),
      reinterpret_cast<void*>(&TpIASetIB),  reinterpret_cast<void*>(&TpGSSetShader), reinterpret_cast<void*>(&TpBegin),
      reinterpret_cast<void*>(&TpEnd),      reinterpret_cast<void*>(&TpVSSetCB),   reinterpret_cast<void*>(&TpPSSetCB),
      reinterpret_cast<void*>(&TpPSSetShader), reinterpret_cast<void*>(&TpVSSetShader),
      reinterpret_cast<void*>(&TpPSSetSamplers), reinterpret_cast<void*>(&TpVSSetSamplers),
      reinterpret_cast<void*>(&TpIASetLayout), reinterpret_cast<void*>(&TpIASetTopo),
      reinterpret_cast<void*>(&TpGSSetSRV), reinterpret_cast<void*>(&TpOMSetBlend),
      reinterpret_cast<void*>(&TpOMSetDepth), reinterpret_cast<void*>(&TpRSSetState),
      reinterpret_cast<void*>(&TpVSSetCB1), reinterpret_cast<void*>(&TpPSSetCB1), reinterpret_cast<void*>(&TpGSSetCB1),
      reinterpret_cast<void*>(&TpRSSetScissor), reinterpret_cast<void*>(&TpGSSetCB),
      reinterpret_cast<void*>(&TpGSSetSamplers)};
  return hooks[k];
}
// The split filter's ExecuteCommandList hook (or a recorder after its own) on DCS's immediate context.
void AfterEclEntry(void* ctx) {
  if (!OnRenderCtx(static_cast<ID3D11DeviceContext*>(ctx))) return;
  HookBusy busy;
  AfterToggleSeh(true, -1);
}
// Far depth pre-overlay (far_depth.h 5): while an overlay-class pass runs, the
// render target bound (identity) waiting for the viewport that follows it.
bool g_overlayArm = false;
bool g_overlayLdr = false;  // the overlay pass is an OverlayLDRPass (else GUIPass)
ID3D11RenderTargetView* g_ovRtv = nullptr;  // no reference: DCS keeps it bound until the viewport
void FarPreOverlaySeh(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp);  // below
void OverlayBind(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* r) {
  if (c != g_s.ctx) return;
  g_ovRtv = n && r ? r[0] : nullptr;
}
// R74b (cockpit_overlay.h, mask v1.1): while an HMD view's OverlayHDRPass is open with the mask's overlay note, the
// pass's first render target 0 (AddRef'd; cockpit_overlay.h takes it at the pass end) and the first viewport after it.
extern bool g_trOpen;
bool g_cmOvlArm = false;
ID3D11RenderTargetView* g_cmOvlRtv = nullptr;  // reference
D3D11_VIEWPORT g_cmOvlVp{};
bool g_cmOvlWantVp = false;
void CmOvlBind(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* r) {
  if (c != g_s.ctx || g_cmOvlRtv || !n || !r || !r[0]) return;
  g_cmOvlRtv = r[0];
  g_cmOvlRtv->AddRef();
  g_cmOvlWantVp = true;
}
void CmOvlViewport(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v) {
  if (c != g_s.ctx || !g_cmOvlWantVp || !n || !v) return;
  g_cmOvlVp = v[0];
  g_cmOvlWantVp = false;
}
void OverlayViewport(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v) {
  if (c != g_s.ctx || !g_ovRtv || !n || !v) return;
  ID3D11RenderTargetView* rtv = g_ovRtv;
  g_ovRtv = nullptr;  // the first viewport after the bind only
  FarPreOverlaySeh(rtv, v[0]);
}
ID3D11DepthStencilView* g_passDsv = nullptr;  // reference
std::atomic<uint64_t> g_armMissed{0}, g_passDsvSeen{0};

ID3D11DepthStencilView* g_traceDsv[4] = {};  // trace: the pass's distinct DSVs in bind order (no references)
int g_traceDsvN = 0;
// DSVs bound during the current GBufferPass, in order, consecutive repeats
// merged (no references: DCS holds them while the pass runs).
ID3D11DepthStencilView* g_seq[8] = {};
D3D11_VIEWPORT g_seqVp[8] = {};  // the first viewport DCS set after each bind (Width 0 = none)
int g_seqN = 0;
// R70: the motion-vector render target (the single R16G16_FLOAT / R16G16B16A16_FLOAT RTV) bound with each recorded DSV,
// while the motion export wants it (g_mvWanted, set by cockpit_mask.h's ConfigureMotion); no references, as g_seq.
std::atomic<bool> g_mvWanted{false};
ID3D11RenderTargetView* g_seqMv[8] = {};
ID3D11RenderTargetView* g_passMv = nullptr;  // reference: the MV RTV of g_passDsv's entry (PickPassDsv)
ID3D11RenderTargetView* g_runMv = nullptr;   // reference: the current run's MV RTV (the latest pass that had one)
std::atomic<uint64_t> g_mvAmbiguous{0};      // a bind with more than one 16-bit float RG / RGBA target
// The MV candidate among a bind's render targets: exactly one R16G16_FLOAT or R16G16B16A16_FLOAT view.
ID3D11RenderTargetView* MvOfBind(UINT n, ID3D11RenderTargetView* const* r) {
  if (!r) return nullptr;
  ID3D11RenderTargetView* found = nullptr;
  int count = 0;
  for (UINT k = 0; k < n && k < 8; ++k) {
    if (!r[k]) continue;
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    r[k]->GetDesc(&d);
    if ((d.Format == DXGI_FORMAT_R16G16_FLOAT || d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) &&
        d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && d.Texture2D.MipSlice == 0) {
      found = r[k];
      ++count;
    }
  }
  if (count > 1) {
    g_mvAmbiguous++;
    return nullptr;
  }
  return found;
}
// R69: a GBufferPass begin was seen by this install since the last end. The entries above are DCS's views WITHOUT a
// reference, valid only while that pass runs: a pass end whose begin came before an install (a reinstall mid-pass)
// must not use them (live: an AddRef on a released DSV, 0xc0000005 in PickPassDsv after a CockpitDepthExport toggle).
bool g_gbBegun = false;
void KeepDsv(ID3D11DeviceContext* c, ID3D11DepthStencilView* d) {
  if (!d || c != g_s.ctx) return;
  if (g_traceDsvN < 4 && (!g_traceDsvN || g_traceDsv[g_traceDsvN - 1] != d)) g_traceDsv[g_traceDsvN++] = d;
  if (g_seqN && g_seq[g_seqN - 1] == d) return;
  if (g_seqN == 8) {
    memmove(g_seq, g_seq + 1, sizeof(g_seq) - sizeof(g_seq[0]));
    memmove(g_seqVp, g_seqVp + 1, sizeof(g_seqVp) - sizeof(g_seqVp[0]));
    --g_seqN;
  }
  g_seqVp[g_seqN] = D3D11_VIEWPORT{};
  g_seqMv[g_seqN] = nullptr;
  g_seq[g_seqN++] = d;
}
// R70: the MV target bound with the DSV just recorded (the latest bind of that entry that had one wins).
void KeepMv(ID3D11DeviceContext* c, ID3D11DepthStencilView* d, UINT n, ID3D11RenderTargetView* const* r) {
  if (!d || c != g_s.ctx || !g_mvWanted.load(std::memory_order_relaxed) || !g_seqN || g_seq[g_seqN - 1] != d) return;
  if (ID3D11RenderTargetView* mv = MvOfBind(n, r)) g_seqMv[g_seqN - 1] = mv;
}
// With DLSS each view renders at its own resolution (measured: periphery
// 476x468, focus 1176x1164) while the default frame buffer is 816x804, so
// the viewport must be the one set inside the pass.
// false while the OM / viewport entries are in only to re-arm the upload
// entries (the forward cockpit pass's arm, ArmUsOnly): no DSV / viewport
// capture, no pre-overlay bind tracking.
bool g_omCapture = true;
// [Perception] CockpitTrueIpdFrameDump: an HMD view's OverlayLDRPass / GUIPass is open while a dump captures it
// (DumpPassBoundary): the OM / viewport hooks report its render target binds to cockpit_ipd.h, whichever feature
// holds the arm (also an uploads-only arm, whose OM entries are re-arm points only). g_dumpArm: the arm is the
// dump's own (taken when no other feature held one; given back at the pass end).
bool g_dumpPass = false, g_dumpArm = false;
extern bool g_geoActive;  // R41 GeoOnlyProbe (defined with the probes below)
extern uint64_t g_geoVps;
void STDMETHODCALLTYPE VpHook(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v) {
  HookBusy busy;
  if (g_dumpPass && c == g_s.ctx && n && v) ckipd::DumpVpSet(v[0]);
  if (g_omCapture && c == g_s.ctx && n && v && g_seqN && g_seqVp[g_seqN - 1].Width == 0) g_seqVp[g_seqN - 1] = v[0];
  // Pre-overlay: copied before DCS's call (the copy reads the target only;
  // the viewport is not needed bound for it).
  if (g_omCapture && g_overlayArm) OverlayViewport(c, n, v);
  if (g_omCapture && g_cmOvlArm) CmOvlViewport(c, n, v);  // R74b
  if (g_geoActive && c == g_s.ctx && n && v) {  // R41 [Suite] GeoOnlyProbe: every triangle set up, no pixel written
    D3D11_VIEWPORT off[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    const UINT m = std::min<UINT>(n, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
    for (UINT i = 0; i < m; ++i) off[i] = D3D11_VIEWPORT{-64.0f, -64.0f, 16.0f, 16.0f, v[i].MinDepth, v[i].MaxDepth};
    ++g_geoVps;
    g_vpOrig(c, m, off);
  } else {
    g_vpOrig(c, n, v);
  }
  if (c == g_s.ctx) InPassRearm(), g_lastCall = 5;
}
D3D11_VIEWPORT g_passVp{};  // the viewport that goes with g_passDsv (Width 0 = unknown)
// The DSV bound when the GBufferPass began: DCS's default frame buffer
// (identity only, no reference).
ID3D11DepthStencilView* g_beginDsv = nullptr;
// At a GBufferPass end: the last DSV the pass bound other than the one bound
// before it (DCS's default frame buffer); if the pass bound none other, the
// one it bound last; if it bound none, the one bound now when it is still
// the one from before the pass (the pass drew into it). AddRef'd into
// g_passDsv with its viewport.
void PickPassDsv() {
  g_passVp = D3D11_VIEWPORT{};
  int at = -1;
  for (int i = g_seqN - 1; i >= 0; --i)
    if (g_seq[i] != g_beginDsv) {
      g_passDsv = g_seq[i];
      g_passVp = g_seqVp[i];
      at = i;
      break;
    }
  if (!g_passDsv && g_seqN) {
    g_passDsv = g_seq[g_seqN - 1];
    g_passVp = g_seqVp[g_seqN - 1];
    at = g_seqN - 1;
  }
  if (g_passMv) g_passMv->Release();  // R70
  g_passMv = (at >= 0 && g_passDsv) ? g_seqMv[at] : nullptr;
  if (g_passMv) g_passMv->AddRef();
  if (g_passDsv) {
    g_passDsv->AddRef();
  } else {
    ID3D11DepthStencilView* cur = nullptr;
    g_s.ctx->OMGetRenderTargets(0, nullptr, &cur);
    if (cur && cur == g_beginDsv && g_lastArmOk)
      g_passDsv = cur;  // keeps the reference (seen: the pass bound nothing else)
    else if (cur)
      cur->Release();
  }
  g_seqN = 0;
}
// Cockpit ipd phase 2: the depth texture the current G-buffer pass rendered
// into (as PickPassDsv: the last DSV bound other than the default frame buffer).
ID3D11Resource* PassDepthTexNow() {
  ID3D11DepthStencilView* d = nullptr;
  for (int i = g_seqN - 1; i >= 0; --i)
    if (g_seq[i] != g_beginDsv) {
      d = g_seq[i];
      break;
    }
  if (!d) return nullptr;
  ID3D11Resource* r = nullptr;
  d->GetResource(&r);
  if (r) r->Release();  // identity only
  return r;
}
void STDMETHODCALLTYPE OmHook(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* r,
                              ID3D11DepthStencilView* d) {
  HookBusy busy;
  if (g_dumpPass && c == g_s.ctx) ckipd::DumpOmBind(n && r ? r[0] : nullptr);
  if (g_omCapture) {
    KeepDsv(c, d);
    KeepMv(c, d, n, r);  // R70
    if (g_overlayArm) OverlayBind(c, n, r);
    if (g_cmOvlArm) CmOvlBind(c, n, r);  // R74b
  }
  g_omOrig(c, n, r, d);
  if (c == g_s.ctx) InPassRearm(), g_lastCall = 4;
}
void STDMETHODCALLTYPE OmUavHook(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* r,
                                 ID3D11DepthStencilView* d, UINT us, UINT un, ID3D11UnorderedAccessView* const* u,
                                 const UINT* ic) {
  HookBusy busy;
  if (g_dumpPass && c == g_s.ctx && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
    ckipd::DumpOmBind(n && r ? r[0] : nullptr);
  if (g_omCapture && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
    KeepDsv(c, d);
    KeepMv(c, d, n, r);  // R70
    if (g_overlayArm) OverlayBind(c, n, r);
    if (g_cmOvlArm) CmOvlBind(c, n, r);  // R74b
  }
  g_omUavOrig(c, n, r, d, us, un, u, ic);
  if (c == g_s.ctx) InPassRearm();
}

// overlay: an overlay-class pass (pre-overlay copy; no upload hook).
std::atomic<uint64_t> g_omSkipForeign{0};
// The OM / viewport slots (resolved once); false when unknown.
bool OmSlots() {
  if (g_omSlot < 0) {
    g_omSlot = DcsQv_ContextSlot("OMSetRenderTargets");
    g_omUavSlot = DcsQv_ContextSlot("OMSetRenderTargetsAndUnorderedAccessViews");
    g_vpSlot = DcsQv_ContextSlot("RSSetViewports");
    for (int k = 0; k < kTpCount; ++k) g_tpSlot[k] = DcsQv_ContextSlot(kTpNames[k]);
  }
  return g_omSlot >= 0 && g_omUavSlot >= 0 && g_vpSlot >= 0;
}
bool UsSlots() {
  if (g_usSlot < 0) {
    g_usSlot = DcsQv_ContextSlot("UpdateSubresource");
    g_us1Slot = DcsQv_ContextSlot("UpdateSubresource1");
  }
  return g_usSlot >= 0 && g_us1Slot >= 0;
}
// The upload entries into vt (DCS's current ones become the originals);
// false when they are ours already (a stale arm, should not happen).
bool ArmUsEntries(void** vt) {
  if (vt[g_usSlot] == reinterpret_cast<void*>(&UsHook) || vt[g_us1Slot] == reinterpret_cast<void*>(&Us1Hook))
    return false;
  // The table still in d3d11's after-Flush state (a Flush since the last call through it): put back to its normal
  // entries first, else the first draw / copy / clear of the pass would replace our hooks with them.
  if (UsToggled(vt) && GetCurrentThreadId() == ptiming::g_topTid.load(std::memory_order_relaxed)) {
    NormalizeTable(vt);
    ckipd::g_st.tblNormArm++;
  }
  g_usOrig = reinterpret_cast<UsFn>(vt[g_usSlot]);
  g_us1Orig = reinterpret_cast<Us1Fn>(vt[g_us1Slot]);
  FdWriteSlot(&vt[g_usSlot], reinterpret_cast<void*>(&UsHook));
  FdWriteSlot(&vt[g_us1Slot], reinterpret_cast<void*>(&Us1Hook));
  g_usArmed = true;
  return true;
}
// The upload entries back (ours only: an entry d3d11 switched after our last
// re-arm stays d3d11's). True when both were still ours.
bool DisarmUsEntries(void** vt) {
  bool ok = true;
  if (vt[g_usSlot] == reinterpret_cast<void*>(&UsHook))
    FdWriteSlot(&vt[g_usSlot], reinterpret_cast<void*>(g_usOrig));
  else
    ok = false;
  if (vt[g_us1Slot] == reinterpret_cast<void*>(&Us1Hook))
    FdWriteSlot(&vt[g_us1Slot], reinterpret_cast<void*>(g_us1Orig));
  else
    ok = false;
  if (!ok) fdexport::g_usLost++;  // lost after the last re-arm chance (diagnostic)
  g_usArmed = false;
  return ok;
}
bool InD3d11(const void* p);  // below (R47)
// R48: one rule for every toggle-point arm, counted when it leaves a point out (before, R49's skips were silent and
// the basic points and R32's Unmap point took any pointer). The state-call points (kTpBasic..): d3d11's own entry
// only (R47 / R49: the split filter owns several of these slots). The basic points (Flush, Map, GetData, Unmap): not
// over another hook of this payload (gpu_dedup.h's Map / Unmap hooks re-arm after every call they forward, taking
// whatever is there as d3d11's: the same mutual forwarding); a third-party hook there is chained over as before.
std::atomic<uint64_t> g_tpSkipForeign{0};
inline bool TpSlotFree(void** vt, int k) {
  if (g_tpSlot[k] < 0 || vt[g_tpSlot[k]] == TogglePointHook(k)) return false;
  const void* cur = vt[g_tpSlot[k]];
  if (InD3d11(cur) || (k < kTpBasic && !d3ds::InPayload(cur))) return true;
  g_tpSkipForeign.fetch_add(1, std::memory_order_relaxed);
  return false;
}
// The OM / viewport entries into vt; false when unknown or ours already.
bool ArmOmEntries(void** vt) {
  if (!OmSlots()) return false;
  void* om = vt[g_omSlot];
  void* omu = vt[g_omUavSlot];
  void* vp = vt[g_vpSlot];
  if (om == reinterpret_cast<void*>(&OmHook) || omu == reinterpret_cast<void*>(&OmUavHook) ||
      vp == reinterpret_cast<void*>(&VpHook))
    return false;  // a stale arm (should not happen)
  g_omOrig = reinterpret_cast<OmFn>(om);
  g_omUavOrig = reinterpret_cast<OmUavFn>(omu);
  g_vpOrig = reinterpret_cast<VpFn>(vp);
  FdWriteSlot(&vt[g_omSlot], reinterpret_cast<void*>(&OmHook));
  FdWriteSlot(&vt[g_omUavSlot], reinterpret_cast<void*>(&OmUavHook));
  FdWriteSlot(&vt[g_vpSlot], reinterpret_cast<void*>(&VpHook));
  // The toggle points (Flush, Map, GetData: not among d3d11's toggled entries): a toggle repaired right after it.
  for (int k = 0; k < kTpCount; ++k) {
    g_tpArmed[k] = false;
    if (k >= kTpBasic && !g_tpExtended) continue;
    if (k == kTpUnmap && !g_tpCk) continue;  // R32: cockpit ipd's arms only
    // R49: the state-call points (an overlay pass's own arm, cockpit ipd's d3d11 overlay path) only over d3d11's
    // own entry: never into a slot the split filter owns (mutual forwarding, see InD3d11 below). R48: TpSlotFree.
    if (!TpSlotFree(vt, k)) continue;
    g_tpOrig[k] = vt[g_tpSlot[k]];
    FdWriteSlot(&vt[g_tpSlot[k]], TogglePointHook(k));
    g_tpArmed[k] = true;
  }
  return true;
}
// The overlay passes' state-call toggle points into an arm held by another feature (a nested overlay pass);
// DisarmTpExtended takes them out again at the overlay pass end (DisarmOmEntries too, when it comes first).
// R49: cockpit ipd's nested d3d11 overlay path uses ArmTpExtendedD3d11Only too (the unguarded variant is gone).
bool g_tpAddedExt = false;
// R47: the colour pass's tone-map arm takes only the state-call points whose entry is d3d11's own. The split
// filter (split_filter.h) hooks VS/PS shader, constant buffer, sampler, layout, topology and raster entries and puts
// its hook back whenever it finds another pointer there, taking that pointer as its original: with ours in such a
// slot (ours forwarding to its hook, its hook forwarding to ours) a call recursed until the stack ran out (live
// 2026-10-16 21:24 / 21:42 / 21:45: DCS gone without a crash report as soon as the tone maps were swapped). The
// points d3d11 was seen submitting in (PSSetShaderResources, VSSetShaderResources, IASetVertexBuffers,
// IASetIndexBuffer, GSSetShader) are not among the split filter's.
bool InD3d11(const void* p) {
  static uintptr_t lo = 0, hi = 0;  // d3d11.dll's image (loaded before any context exists; never unloaded under DCS)
  if (!lo) {
    if (const HMODULE d3d = GetModuleHandleW(L"d3d11.dll")) {
      const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(d3d);
      const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(d3d) + dos->e_lfanew);
      hi = reinterpret_cast<uintptr_t>(d3d) + nt->OptionalHeader.SizeOfImage;
      lo = reinterpret_cast<uintptr_t>(d3d);
    }
  }
  const uintptr_t a = reinterpret_cast<uintptr_t>(p);
  return lo && a >= lo && a < hi;
}
void ArmTpExtendedD3d11Only(void** vt) {
  for (int k = kTpBasic; k < kTpCount; ++k) {
    if (g_tpArmed[k] || !TpSlotFree(vt, k)) continue;  // d3d11's own entry only (R48: counted)
    g_tpOrig[k] = vt[g_tpSlot[k]];
    FdWriteSlot(&vt[g_tpSlot[k]], TogglePointHook(k));
    g_tpArmed[k] = true;
    g_tpAddedExt = true;
  }
}
// R32's Unmap toggle point into another feature's arm while a cockpit pass is nested in it (the cpu path's
// observer reads a mapped effect buffer at its Unmap); out again at the nested pass's end.
void ArmTpUnmap(void** vt) {
  if (!vt || g_tpArmed[kTpUnmap] || !TpSlotFree(vt, kTpUnmap)) return;  // R48: not over a hook of ours
  g_tpOrig[kTpUnmap] = vt[g_tpSlot[kTpUnmap]];
  FdWriteSlot(&vt[g_tpSlot[kTpUnmap]], TogglePointHook(kTpUnmap));
  g_tpArmed[kTpUnmap] = true;
  g_tpAddedUnmap = true;
}
void DisarmTpUnmap(void** vt) {
  if (!g_tpAddedUnmap) return;
  g_tpAddedUnmap = false;
  if (g_tpArmed[kTpUnmap] && vt && vt[g_tpSlot[kTpUnmap]] == TogglePointHook(kTpUnmap))
    FdWriteSlot(&vt[g_tpSlot[kTpUnmap]], g_tpOrig[kTpUnmap]);
  g_tpArmed[kTpUnmap] = false;
}
void DisarmTpExtended(void** vt) {
  if (!g_tpAddedExt) return;
  g_tpAddedExt = false;
  for (int k = kTpBasic; k < kTpCount; ++k) {
    if (g_tpArmed[k] && vt && vt[g_tpSlot[k]] == TogglePointHook(k)) FdWriteSlot(&vt[g_tpSlot[k]], g_tpOrig[k]);
    g_tpArmed[k] = false;
  }
}
// True when OMSetRenderTargets was still ours (the others are put back when ours).
bool DisarmOmEntries(void** vt) {
  bool missed = false;
  if (vt[g_omSlot] == reinterpret_cast<void*>(&OmHook))
    FdWriteSlot(&vt[g_omSlot], reinterpret_cast<void*>(g_omOrig));
  else
    missed = true;
  if (vt[g_omUavSlot] == reinterpret_cast<void*>(&OmUavHook))
    FdWriteSlot(&vt[g_omUavSlot], reinterpret_cast<void*>(g_omUavOrig));
  if (vt[g_vpSlot] == reinterpret_cast<void*>(&VpHook))
    FdWriteSlot(&vt[g_vpSlot], reinterpret_cast<void*>(g_vpOrig));
  for (int k = 0; k < kTpCount; ++k) {
    if (g_tpArmed[k] && vt[g_tpSlot[k]] == TogglePointHook(k))
      FdWriteSlot(&vt[g_tpSlot[k]], g_tpOrig[k]);
    g_tpArmed[k] = false;
  }
  g_tpAddedExt = false;
  g_tpAddedUnmap = false;
  return !missed;
}

// True when this call armed (false: no context, foreign hooks, unknown slots,
// or an arm is held already, by the caller's own feature or another one).
bool ArmOm(bool overlay = false) {
  State& s = g_s;
  if (!s.ctx || g_armTable) return false;
  // The D3D meter re-arms OMSetRenderTargets* itself (ForeignTableHooks; gpu_dedup.h did too until R107):
  // no in-pass arm while it holds the table.
  if (d3ds::g_vtblCount.load(std::memory_order_relaxed) > 0) {
    g_omSkipForeign++;
    return false;
  }
  void** vt = *reinterpret_cast<void***>(s.ctx);
  if (!ArmOmEntries(vt)) return false;
  g_armTable = vt;
  g_omCapture = true;
  // Far depth / cockpit ipd: the cPerView uploads, only while something uses them.
  g_usArmed = false;
  const bool wantUs = !overlay && (fdexport::WantUploads() || ckipd::WantUploads());
  if (wantUs && ForeignTableHooks()) fdexport::g_usSkipForeign++;
  if (wantUs && !ForeignTableHooks() && UsSlots()) ArmUsEntries(vt);
  return true;
}

void DisarmUsOnly();
void DisarmOm() {
  if (g_usOnlyArm) {
    // The forward cockpit pass's own arm: closed at that pass's end
    // (CockpitIpdBoundary), never by another feature's pass end. With no
    // cockpit pass open (uninstall, an end that never came) it goes here.
    if (ckipd::PassOpen()) return;
    return DisarmUsOnly();
  }
  void** vt = g_armTable;
  if (!vt) return;
  if (g_fwdNested && g_usArmed && ckipd::PassOpen()) {
    // The feature whose arm a forward cockpit pass nested in ends first: the
    // arm passes to the forward pass (upload entries kept; OM / viewport kept
    // for the re-arm only; draws out). Its end removes it (DisarmUsOnly).
    DisarmDraws();
    g_ovlAddedDraws = false;
    g_omCapture = false;
    g_usOnlyOm = true;
    g_usOnlyArm = true;
    g_fwdNested = g_fwdAddedUs = false;
    g_ovRtv = nullptr;
    ckipd::g_st.fwdHandover++;
    return;
  }
  g_fwdNested = g_fwdAddedUs = false;
  DisarmDraws();
  const bool missed = !DisarmOmEntries(vt);
  if (missed) fdexport::g_omLost++;
  g_lastUsOk = false;
  if (g_usArmed) g_lastUsOk = DisarmUsEntries(vt);
  if (missed && !g_seqN) g_armMissed++;
  g_lastArmOk = !missed;
  g_armTable = nullptr;
  g_ovRtv = nullptr;
  g_ckLightArm = false;
}

// Cockpit ipd: a pass whose cPerView uploads are wanted without the OM /
// viewport capture (TransparentCockpitPass, watched cockpit passes). Never
// with another module's hooks in the table.
//  - No arm held: its own arm. The upload entries, and the OM / viewport
//    entries only as re-arm points (g_omCapture false: nothing captured).
//    d3d11 switches the update entries after a Flush / ExecuteCommandList
//    (UsRearm); an uploads-only arm with no other entry of ours could only
//    re-arm after an upload it saw, so a switch between the pass begin and
//    the binder's first cPerView upload sent that upload past us (live
//    2026-10-13: 17 of 20776 forward passes, logged "not armed" by a
//    classification slip in PassEnd; the counters showed the arm itself never
//    failed). DCS binds the pass's frame buffer and viewport before its draws,
//    so the OM / viewport hooks put the upload entries back before the upload.
//  - Another feature's in-pass arm held (far depth HDR / overlay arm, the
//    overlay trace, the verify trace; all ArmOm, same table): nested. Its
//    upload entries are shared (or added when it has none, and removed again
//    at the forward pass end); if that feature's pass ends first, the arm
//    passes to the forward pass (DisarmOm), so neither end cuts the other's
//    entries.
// draws: the draw entries too (own arm only; unused today).
bool ArmUsOnly(bool draws = false) {
  State& s = g_s;
  if (!s.ctx || g_usOnlyArm || g_fwdNested) return false;
  if (ForeignTableHooks()) {
    fdexport::g_usSkipForeign++;
    return false;
  }
  if (!UsSlots()) return false;
  void** vt = *reinterpret_cast<void***>(s.ctx);
  if (g_armTable) {
    if (draws || g_armTable != vt) return false;
    if (g_usArmed) {
      UsRearm();
    } else {
      if (!ArmUsEntries(vt)) return false;
      g_fwdAddedUs = true;
    }
    ArmTpUnmap(vt);  // R32: the cockpit pass's own toggle point in the shared arm (out again at its end)
    g_fwdNested = true;
    ckipd::g_st.fwdNested++;
    return true;
  }
  if (!ArmUsEntries(vt)) return false;
  g_armTable = vt;
  g_tpExtended = draws;  // an overlay pass's own arm: its state-call toggle points too
  g_tpCk = true;         // cockpit ipd's own arm: R32's Unmap point too
  g_usOnlyOm = ArmOmEntries(vt);
  g_tpExtended = false;
  g_tpCk = false;
  g_omCapture = false;
  if (draws) ArmDraws();  // before g_usOnlyArm: ArmDraws refuses the forward arm to other callers
  g_usOnlyArm = true;
  return true;
}
// The forward cockpit pass's end (and the uninstall through DisarmOm): its own
// arm out, or its share of another feature's arm.
void DisarmUsOnly() {
  if (g_fwdNested) {
    g_fwdNested = false;
    if (g_ovlAddedDraws) DisarmDraws();  // added for a nested overlay pass: the outer arm had none
    g_ovlAddedDraws = false;
    void** vt = g_armTable;
    DisarmTpExtended(vt);
    DisarmTpUnmap(vt);
    if (g_fwdAddedUs && g_usArmed && vt) DisarmUsEntries(vt);  // the outer arm had no upload entries
    g_fwdAddedUs = false;
    return;
  }
  if (!g_usOnlyArm) return;
  g_usOnlyArm = false;
  g_omCapture = true;
  void** vt = g_armTable;
  if (!vt) {
    g_usOnlyOm = false;
    return;
  }
  DisarmDraws();
  if (g_usOnlyOm) DisarmOmEntries(vt);
  g_usOnlyOm = false;
  if (g_usArmed) DisarmUsEntries(vt);
  g_armTable = nullptr;
  g_ovRtv = nullptr;
}

// Captures the G-buffer's depth target (bound inside the pass), viewport and
// cPerView at a GBufferPass end.
bool CaptureRun(Run& r) {
  State& s = g_s;
  ID3D11DeviceContext* c = s.ctx;
  ID3D11DepthStencilView* dsv = g_passDsv;  // reference moves to the run
  g_passDsv = nullptr;
  if (dsv) g_passDsvSeen++;
  if (!dsv) return false;
  r.dsv = dsv;
  D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
  dsv->GetDesc(&dd);
  ID3D11Resource* res = nullptr;
  dsv->GetResource(&res);
  r.tex = res;
  if (res) res->Release();  // dsv keeps it alive
  r.variant = -1;
  switch (dd.ViewDimension) {
    case D3D11_DSV_DIMENSION_TEXTURE2D: r.variant = 0; break;
    case D3D11_DSV_DIMENSION_TEXTURE2DARRAY: r.variant = 1; r.slice = dd.Texture2DArray.FirstArraySlice; break;
    case D3D11_DSV_DIMENSION_TEXTURE2DMS: r.variant = 2; break;
    case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY: r.variant = 3; r.slice = dd.Texture2DMSArray.FirstArraySlice; break;
    default: break;
  }
  if (r.variant == 1 && dd.Texture2DArray.MipSlice != 0) r.variant = -1;
  if (r.variant == 0 && dd.Texture2D.MipSlice != 0) r.variant = -1;
  ID3D11Texture2D* t = nullptr;
  if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
    D3D11_TEXTURE2D_DESC td{};
    t->GetDesc(&td);
    t->Release();
    r.texFmt = td.Format;
    r.rawFmt = td.Format;
    r.mipLevels = td.MipLevels ? td.MipLevels : 1;
    r.texW = td.Width;
    r.texH = td.Height;
    if (!(td.BindFlags & D3D11_BIND_SHADER_RESOURCE)) r.texFmt = DXGI_FORMAT_UNKNOWN;
  }
  // The viewport set with the G-buffer's DSV; else the whole texture.
  r.vp = g_passVp;
  if (r.vp.Width <= 0 || r.vp.Height <= 0 || r.vp.TopLeftX + r.vp.Width > r.texW + 0.5f ||
      r.vp.TopLeftY + r.vp.Height > r.texH + 0.5f)
    r.vp = D3D11_VIEWPORT{0, 0, static_cast<float>(r.texW), static_cast<float>(r.texH), 0, 1};
  ID3D11Buffer* cb = nullptr;
  UINT first = 0, num = 0;
  if (s.ctx1) {
    s.ctx1->VSGetConstantBuffers1(7, 1, &cb, &first, &num);
    if (!cb) s.ctx1->PSGetConstantBuffers1(7, 1, &cb, &first, &num);
  } else {
    c->VSGetConstantBuffers(7, 1, &cb);
    if (!cb) c->PSGetConstantBuffers(7, 1, &cb);
    num = 4096;
  }
  r.cb = cb;
  r.cbFirst = first;
  r.cbNum = num;
  // Far depth: this pass's matrices on the CPU (far_depth.h 3).
  // The OM entries must have held (the DSV and viewport are this pass's); the
  // upload entries need not: cpuProj requires this pass's upload, and every
  // upload of a view's cPerView in one pass carries the same gViewInv/gProj.
  r.armOk = g_lastArmOk;
  UINT cbBytes = 0;  // R71: a whole-buffer binding's span (gOrigin at byte 0x610 is read only inside it)
  if (cb && fdexport::WantUploads()) {
    D3D11_BUFFER_DESC bd{};
    cb->GetDesc(&bd);
    cbBytes = bd.ByteWidth;
  }
  r.cpuProj = fdexport::WantUploads() &&
              fdexport::TakeProj(cb, first, num, r.proj, r.viewInv, r.origin, &r.originOk, cbBytes);
  ID3D11DepthStencilState* dss = nullptr;
  UINT ref = 0;
  c->OMGetDepthStencilState(&dss, &ref);
  if (dss) {
    D3D11_DEPTH_STENCIL_DESC d{};
    dss->GetDesc(&d);
    r.depthFunc = d.DepthFunc;
    dss->Release();
  }
  r.open = true;
  return true;
}

bool SameTarget(const Run& a, ID3D11Resource* tex, UINT slice, const D3D11_VIEWPORT& vp) {
  return a.tex == tex && a.slice == slice && a.vp.TopLeftX == vp.TopLeftX && a.vp.TopLeftY == vp.TopLeftY &&
         a.vp.Width == vp.Width && a.vp.Height == vp.Height;
}

bool CbBigEnough(const Run& r) {
  if (!r.cb) return false;
  D3D11_BUFFER_DESC d{};
  r.cb->GetDesc(&d);
  if (!(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return false;
  const bool offsets = !(r.cbFirst == 0 && (r.cbNum == 4096 || r.cbNum == 0));
  if (!offsets) return d.ByteWidth >= 384;
  return r.cbNum >= 24 && (static_cast<uint64_t>(r.cbFirst) + 24) * 16 <= d.ByteWidth;
}

// Queues a frame that cannot be sampled (valid = 0), in order.
void QueueInvalid(const GazeOut* g, uint32_t view, bool focus) {
  State& s = g_s;
  if (s.count == 3) {
    g_ringFull++;
    return;
  }
  Slot& sl = s.ring[(s.head + s.count) % 3];
  sl.busy = true;
  sl.gpu = false;
  sl.frame = s.frame;
  sl.gaze = g ? *g : GazeOut{};
  sl.view = view;
  sl.wantFocus = focus;
  sl.periphery = s.periphery;
  sl.afterWater = false;
  sl.coneDeg = kConeDeg;
  sl.ringDeg = kRingDeg;
  ++s.count;
}

// Reads GazeOut; false = no usable gaze (counted).
bool ReadGaze(GazeOut& g) {
  State& s = g_s;
  void* v = g_gazeView.load();
  // Test writer: only while nobody else writes the block. Once a seq we did
  // not write appears (the pupil shift layer started after we created the
  // block), our writes stop for good and its records are read.
  if (g_testGaze.load() && v && g_gazeOwned && !g_testWriterYielded.load()) {
    const uint64_t cur = *static_cast<const volatile uint64_t*>(v);
    if (g_testWriterSeq != 0 && cur != g_testWriterSeq) {
      g_testWriterYielded = true;
      Log("fixation depth: another writer updates %ls (seq %llu, ours %llu): test gaze writes stopped",
          GazeName().c_str(), static_cast<unsigned long long>(cur), static_cast<unsigned long long>(g_testWriterSeq));
    }
  }
  if (g_testGaze.load() && v && g_gazeOwned && !g_testWriterYielded.load()) {
    GazeOut t{};
    t.displayTime = 0;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    t.displayTime = q.QuadPart;
    t.flags = kGazeValid;
    t.viewCount = 4;
    for (int i = 0; i < 4; ++i) {
      t.gazeTan[i][0] = g_testGx.load();
      t.gazeTan[i][1] = g_testGy.load();
    }
    const float def[4] = {-0.6f, 0.6f, 0.6f, -0.6f};
    for (int i = 0; i < 4; ++i)
      for (int k = 0; k < 4; ++k) t.fovTan[i][k] = g_haveDcsFov[i].load() ? g_dcsFov[i][k].load() : def[k];
    t.vergenceM = -1;
    SeqWrite(v, t);
    g_testWriterSeq = *static_cast<const volatile uint64_t*>(v);
  }
  if (!v) {
    g_noGaze++;
    return false;
  }
  if (!SeqRead(v, g)) {
    g_noGaze++;
    return false;
  }
  if (g.seq != s.lastGazeSeq) {
    s.lastGazeSeq = g.seq;
    s.gazeSeqFrame = s.frame;
  } else if (s.frame - s.gazeSeqFrame > 30) {
    g_staleGaze++;
    return false;
  }
  if (!(g.flags & kGazeValid) || g.viewCount < 1) {
    g_noGaze++;
    return false;
  }
  return true;
}

// Saved CS/OM state.
struct Saved {
  ID3D11ComputeShader* cs = nullptr;
  ID3D11ClassInstance* inst[256] = {};
  UINT nInst = 256;
  ID3D11Buffer* cb[2] = {};
  UINT cbFirst[2] = {}, cbNum[2] = {};
  bool cbOffsets = false;
  ID3D11ShaderResourceView* srv[2] = {};
  ID3D11UnorderedAccessView* uav = nullptr;
  bool om = false;
  ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  ID3D11DepthStencilView* dsv = nullptr;
};

void SaveState(ID3D11DeviceContext* c, ID3D11DeviceContext1* c1, Saved& sv) {
  c->CSGetShader(&sv.cs, sv.inst, &sv.nInst);
  if (c1) {
    c1->CSGetConstantBuffers1(0, 2, sv.cb, sv.cbFirst, sv.cbNum);
    for (int i = 0; i < 2; ++i)
      if (sv.cb[i] && !(sv.cbFirst[i] == 0 && (sv.cbNum[i] == 4096 || sv.cbNum[i] == 0))) sv.cbOffsets = true;
  } else {
    c->CSGetConstantBuffers(0, 2, sv.cb);
  }
  c->CSGetShaderResources(0, 2, sv.srv);
  c->CSGetUnorderedAccessViews(0, 1, &sv.uav);
}

void RestoreState(ID3D11DeviceContext* c, ID3D11DeviceContext1* c1, Saved& sv) {
  const UINT keep = static_cast<UINT>(-1);
  c->CSSetUnorderedAccessViews(0, 1, &sv.uav, &keep);
  c->CSSetShaderResources(0, 2, sv.srv);
  if (sv.cbOffsets && c1)
    c1->CSSetConstantBuffers1(0, 2, sv.cb, sv.cbFirst, sv.cbNum);
  else
    c->CSSetConstantBuffers(0, 2, sv.cb);
  c->CSSetShader(sv.cs, sv.nInst ? sv.inst : nullptr, sv.nInst);
  if (sv.om) {
    UINT n = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
      if (sv.rtv[i]) n = i + 1;
    c->OMSetRenderTargetsAndUnorderedAccessViews(n, sv.rtv, sv.dsv, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr,
                                                 nullptr);
  }
  if (sv.cs) sv.cs->Release();
  for (UINT i = 0; i < sv.nInst && i < 256; ++i)
    if (sv.inst[i]) sv.inst[i]->Release();
  for (auto* b : sv.cb)
    if (b) b->Release();
  for (auto* v : sv.srv)
    if (v) v->Release();
  if (sv.uav) sv.uav->Release();
  for (auto* r : sv.rtv)
    if (r) r->Release();
  if (sv.dsv) sv.dsv->Release();
}

// Runs one of our compute shaders on DCS's context: our parameters as b0,
// the run's cPerView as b1 (with its *1 range), `srvs` as t0-t1, `uav` as
// u0. Every binding touched is saved before and put back after (see 5).
// R69: uav1 (optional) is bound as u1 too; DCS's CS UAV 1 is saved and put back exactly.
// R70: srv2 (optional) is bound as t2 too; DCS's CS SRV 2 is saved and put back exactly.
void RunCs(const Run& r, ID3D11Buffer* params, ID3D11ComputeShader* cs, ID3D11ShaderResourceView* const* srvs,
           ID3D11UnorderedAccessView* uav, UINT groupsX, UINT groupsY, ID3D11UnorderedAccessView* uav1 = nullptr,
           ID3D11ShaderResourceView* srv2 = nullptr) {
  State& s = g_s;
  ID3D11DeviceContext* c = s.ctx;
  Saved sv;
  SaveState(c, s.ctx1, sv);
  ID3D11UnorderedAccessView* savedU1 = nullptr;
  if (uav1) c->CSGetUnorderedAccessViews(1, 1, &savedU1);
  ID3D11ShaderResourceView* savedT2 = nullptr;
  if (srv2) c->CSGetShaderResources(2, 1, &savedT2);
  // DCS's DSV on the sampled texture would make d3d11 null our SRV: unbind
  // the output targets for the dispatch (OM UAVs kept), put them back after.
  {
    ID3D11DepthStencilView* cur = nullptr;
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &cur);
    bool conflict = false;
    if (cur) {
      ID3D11Resource* res = nullptr;
      cur->GetResource(&res);
      conflict = res == r.tex;
      if (res) res->Release();
    }
    if (conflict) {
      sv.om = true;
      memcpy(sv.rtv, rtv, sizeof(rtv));
      sv.dsv = cur;
      c->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr,
                                                   nullptr);
      g_omUnbinds++;
    } else {
      for (auto* v : rtv)
        if (v) v->Release();
      if (cur) cur->Release();
    }
  }
  ID3D11Buffer* cbs[2] = {params, r.cb};
  const bool cbOffsets = !(r.cbFirst == 0 && (r.cbNum == 4096 || r.cbNum == 0));
  if (cbOffsets && s.ctx1) {
    UINT first[2] = {0, r.cbFirst}, num[2] = {16, r.cbNum};
    s.ctx1->CSSetConstantBuffers1(0, 2, cbs, first, num);
  } else {
    c->CSSetConstantBuffers(0, 2, cbs);
  }
  c->CSSetShader(cs, nullptr, 0);
  c->CSSetShaderResources(0, 2, srvs);
  if (srv2) c->CSSetShaderResources(2, 1, &srv2);
  const UINT keep = static_cast<UINT>(-1);
  if (uav1) {
    ID3D11UnorderedAccessView* both[2] = {uav, uav1};
    const UINT keep2[2] = {keep, keep};
    c->CSSetUnorderedAccessViews(0, 2, both, keep2);
  } else {
    c->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
  }
  c->Dispatch(groupsX, groupsY, 1);
  ID3D11ShaderResourceView* nullSrv[2] = {};
  c->CSSetShaderResources(0, 2, nullSrv);  // before OM restore re-binds the DSV
  if (uav1) {
    c->CSSetUnorderedAccessViews(1, 1, &savedU1, &keep);
    if (savedU1) savedU1->Release();
  }
  if (srv2) {
    c->CSSetShaderResources(2, 1, &savedT2);
    if (savedT2) savedT2->Release();
  }
  RestoreState(c, s.ctx1, sv);
}

// Verify mode: the bindings we touch, read before and after our work on
// DCS's context (pointers and *1 offsets; no references kept).
struct Bind {
  void* p[24] = {};
  UINT u[8] = {};
  bool operator==(const Bind& o) const { return !memcmp(p, o.p, sizeof(p)) && !memcmp(u, o.u, sizeof(u)); }
};
std::atomic<uint64_t> g_stateChecks{0}, g_stateDiffs{0};
Bind ReadBind(ID3D11DeviceContext* c, ID3D11DeviceContext1* c1) {
  Bind b;
  int n = 0;
  auto put = [&](IUnknown* x) {
    b.p[n++] = x;
    if (x) x->Release();
  };
  ID3D11ComputeShader* cs = nullptr;
  c->CSGetShader(&cs, nullptr, nullptr);
  put(cs);
  ID3D11ShaderResourceView* srv[2] = {};
  c->CSGetShaderResources(0, 2, srv);
  put(srv[0]), put(srv[1]);
  ID3D11UnorderedAccessView* uav[2] = {};
  c->CSGetUnorderedAccessViews(0, 2, uav);
  put(uav[0]), put(uav[1]);
  ID3D11Buffer* cb[3] = {};
  if (c1)
    c1->CSGetConstantBuffers1(0, 3, cb, b.u, b.u + 3);
  else
    c->CSGetConstantBuffers(0, 3, cb);
  put(cb[0]), put(cb[1]), put(cb[2]);
  ID3D11RenderTargetView* rtv[8] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  c->OMGetRenderTargets(8, rtv, &dsv);
  for (auto* r : rtv) put(r);
  put(dsv);
  return b;
}

// The dispatch on DCS's context: window of `run` around gaze (gx, gy).
// Returns false (nothing queued on the GPU, state untouched) on any setup
// failure.
bool Dispatch(const Run& r, Slot& sl) {
  State& s = g_s;
  ID3D11DeviceContext* c = s.ctx;
  const SrvEntry* se = FindSrv(r);
  if (!se) {
    g_noSrv++;
    return false;
  }
  ID3D11ShaderResourceView* srvs[2] = {se->srv, se->stencil};
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(c->Map(s.params, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
  Params p{};
  p.gaze[0] = static_cast<float>(sl.gx);
  p.gaze[1] = static_cast<float>(sl.gy);
  p.vpOrg[0] = r.vp.TopLeftX;
  p.vpOrg[1] = r.vp.TopLeftY;
  p.vpSize[0] = r.vp.Width;
  p.vpSize[1] = r.vp.Height;
  p.tex[0] = static_cast<int32_t>(r.texW);
  p.tex[1] = static_cast<int32_t>(r.texH);
  p.slice = 0;  // the SRV covers only the DSV's slice
  p.stride = sl.stride;
  p.half = sl.half;
  p.G = sl.G;
  memcpy(m.pData, &p, sizeof(p));
  c->Unmap(s.params, 0);

  const bool check = g_verify.load(std::memory_order_relaxed) != 0;
  Bind before;
  if (check) before = ReadBind(c, s.ctx1);
  if (sl.dj) c->Begin(sl.dj);
  if (sl.ts0) c->End(sl.ts0);
  RunCs(r, s.params, s.cs[r.variant], srvs, s.uav, static_cast<UINT>((sl.G + 7) / 8),
        static_cast<UINT>((sl.G + 7) / 8));
  D3D11_BOX box{0, 0, 0, static_cast<UINT>((kHeader + 2 * sl.G * sl.G) * 4), 1, 1};
  c->CopySubresourceRegion(sl.staging, 0, 0, 0, 0, s.out, 0, &box);
  if (sl.ts1) c->End(sl.ts1);
  if (sl.dj) c->End(sl.dj);
  sl.timed = sl.ts0 && sl.ts1 && sl.dj;
  if (check) {
    g_stateChecks++;
    if (!(ReadBind(c, s.ctx1) == before)) g_stateDiffs++;
  }
  return true;
}

#include "cockpit_mask.h"  // R65: [Perception] CockpitMaskExport (per-view cockpit mask for a frame-generation layer)

// Debug (with a dump request): the whole depth texture copied and read back
// right away (one stall), to check the shader's samples against the truth.
void DebugWholeTexture(const Run& r) {
  State& s = g_s;
  ID3D11Texture2D* t = nullptr;
  if (FAILED(r.tex->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) || !t) return;
  D3D11_TEXTURE2D_DESC d{};
  t->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  ID3D11Texture2D* st = nullptr;
  if (SUCCEEDED(s.dev->CreateTexture2D(&d, nullptr, &st)) && st) {
    s.ctx->CopyResource(st, t);
    D3D11_MAPPED_SUBRESOURCE m{};
    const UINT sub = D3D11CalcSubresource(0, r.slice, d.MipLevels);
    if (SUCCEEDED(s.ctx->Map(st, sub, D3D11_MAP_READ, 0, &m))) {
      uint64_t nonzero = 0;
      uint32_t lo = 0xffffffff, hi = 0;
      for (UINT y = 0; y < d.Height; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m.pData) + y * m.RowPitch);
        for (UINT x = 0; x < d.Width; ++x) {
          const uint32_t v = d.Format == DXGI_FORMAT_R32G8X24_TYPELESS ? row[x * 2] : row[x];
          const uint32_t depthBits = d.Format == DXGI_FORMAT_R24G8_TYPELESS ? (v & 0xffffff) : v;
          nonzero += depthBits != 0;
          lo = std::min(lo, depthBits);
          hi = std::max(hi, depthBits);
        }
      }
      s.ctx->Unmap(st, sub);
      Log("fixation depth: debug copy of the depth target %ux%u fmt %u: %llu of %u texels non-zero, depth bits "
          "0x%x..0x%x",
          d.Width, d.Height, static_cast<unsigned>(d.Format), static_cast<unsigned long long>(nonzero),
          d.Width * d.Height, lo, hi);
    }
    st->Release();
  }
  t->Release();
}

// Samples the closed run (the frame's target).
// The frame's sampling decision, taken once (at its first G-buffer run, so
// before the periphery views render): the newest gaze, the GazeOut view to
// sample (2 = left focus with four views, else 0) and the periphery
// fallback: when the 1° cone around the gaze is not inside the left focus
// view's FOV (GazeOut's fovTan when sane, else DCS's own from an earlier
// readback), the left periphery view (0) is sampled instead.
void Decide() {
  State& s = g_s;
  if (s.decided) return;
  s.decided = true;
  s.haveGaze = ReadGaze(s.gaze);
  s.focus = s.haveGaze && s.gaze.viewCount >= 4;
  s.want = s.focus ? 2u : 0u;
  s.periphery = false;
  if (!s.focus || s.lastHmdCount < 4 || g_verify.load()) return;
  const GazeOut& g = s.gaze;
  float fov[4] = {g.fovTan[2][0], g.fovTan[2][1], g.fovTan[2][2], g.fovTan[2][3]};
  if (!(fov[1] > fov[0] && fov[2] > fov[3])) {
    const int hi = std::clamp(2 + g_hmdOffset.load(), 0, 7);
    if (!g_haveDcsFov[hi].load()) return;  // nothing to test against: focus
    for (int k = 0; k < 4; ++k) fov[k] = g_dcsFov[hi][k].load();
  }
  if (!ConeInside(fov, g.gazeTan[2][0], g.gazeTan[2][1], kConeDeg)) {
    s.want = 0;
    s.focus = false;
    s.periphery = true;
    g_periphFrames++;
  }
}

void Sample(const Run& r, int rank, int w, int h, bool hmd, bool afterWater) {
  State& s = g_s;
  LARGE_INTEGER t0, t1;
  QueryPerformanceCounter(&t0);
  Decide();
  const GazeOut& g = s.gaze;
  const bool haveGaze = s.haveGaze;
  const uint32_t view = s.want;
  const bool focus = s.focus;
  if (!haveGaze) {
    QueueInvalid(nullptr, view, focus);
    return;
  }
  if (r.variant < 0 || !CbBigEnough(r)) {
    if (!CbBigEnough(r)) g_badCb++;
    QueueInvalid(&g, view, focus);
    return;
  }
  if (afterWater) g_afterWater++;
  if (s.count == 3) {
    g_ringFull++;
    return;
  }
  Slot& sl = s.ring[(s.head + s.count) % 3];
  sl.gaze = g;
  sl.view = view;
  sl.wantFocus = focus;
  sl.frame = s.frame;
  sl.rank = rank;
  sl.hmdMode = hmd;
  sl.afterWater = afterWater;
  sl.periphery = s.periphery;
  sl.w = w;
  sl.h = h;
  sl.gx = g.gazeTan[view][0];
  sl.gy = g.gazeTan[view][1];
  sl.vp = Viewport{r.vp.TopLeftX, r.vp.TopLeftY, r.vp.Width, r.vp.Height, r.vp.MinDepth, r.vp.MaxDepth};
  // Pixels per tangent: DCS's own FOV from the last readback, else GazeOut's.
  double fl = g.fovTan[view][0], fr = g.fovTan[view][1], fu = g.fovTan[view][2], fdn = g.fovTan[view][3];
  if (g_haveDcsFov[rank & 7].load()) {
    fl = g_dcsFov[rank & 7][0];
    fr = g_dcsFov[rank & 7][1];
    fu = g_dcsFov[rank & 7][2];
    fdn = g_dcsFov[rank & 7][3];
  }
  double sx = fr - fl, sy = fu - fdn;
  if (!(sx > 0.05 && sx < 50)) sx = 1.25;
  if (!(sy > 0.05 && sy < 50)) sy = 1.25;
  // Cone and ring for this view's texel density (the periphery has about a
  // third of the focus density at DLSS render size).
  sl.coneDeg = ConeDegFor(std::min(r.vp.Width / sx, r.vp.Height / sy));
  sl.ringDeg = sl.coneDeg * (kRingDeg / kConeDeg);
  const Plan pl = PlanWindow(r.vp.Width / sx, r.vp.Height / sy, sl.gx, sl.gy, sl.ringDeg + 0.1);
  sl.G = pl.G;
  sl.half = pl.half;
  sl.stride = pl.stride;
  if (g_dumpWanted.load()) DebugWholeTexture(r);
  if (!Dispatch(r, sl)) {
    QueueInvalid(&g, view, focus);
    return;
  }
  sl.busy = true;
  sl.gpu = true;
  ++s.count;
  g_dispatches++;
  QueryPerformanceCounter(&t1);
  sl.cpuUs = (t1.QuadPart - t0.QuadPart) * g_qpcToUs;
}

// Maps the oldest slots (in order) with DO_NOT_WAIT.
void Poll() {
  State& s = g_s;
  while (s.count > 0) {
    Slot& sl = s.ring[s.head];
    Job j;
    j.meta = sl;
    j.meta.staging = nullptr;
    j.meta.ts0 = j.meta.ts1 = j.meta.dj = nullptr;
    j.latency = static_cast<double>(s.frame - sl.frame);
    if (sl.gpu) {
      if (s.frame - sl.frame < 2) break;
      D3D11_MAPPED_SUBRESOURCE m{};
      const HRESULT hr = s.ctx->Map(sl.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
      if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        g_mapLate++;
        break;
      }
      if (SUCCEEDED(hr)) {
        const size_t n = static_cast<size_t>(kHeader) + 2 * static_cast<size_t>(sl.G) * sl.G;
        const float* f = static_cast<const float*>(m.pData);
        j.data = TakeData();  // R91 (M4): a buffer of an earlier frame
        j.data.assign(f, f + n);
        s.ctx->Unmap(sl.staging, 0);
        j.gpu = true;
      }
      if (sl.timed) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 a = 0, b = 0;
        if (s.ctx->GetData(sl.dj, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !dj.Disjoint &&
            dj.Frequency && s.ctx->GetData(sl.ts0, &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            s.ctx->GetData(sl.ts1, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && b >= a)
          j.gpuUs = (b - a) * 1e6 / static_cast<double>(dj.Frequency);
      }
      std::lock_guard<std::mutex> lock(g_statMutex);
      g_cpuUsSum += sl.cpuUs;
      g_cpuUsMax = std::max(g_cpuUsMax, sl.cpuUs);
      ++g_cpuN;
    }
    sl.busy = false;
    sl.gpu = false;
    s.head = (s.head + 1) % 3;
    --s.count;
    PushJob(std::move(j));
  }
}

void PassName(void* pass, char* out, size_t cap) {
  out[0] = 0;
  __try {
    const uint8_t* b = static_cast<const uint8_t*>(pass);
    const uint64_t n = *reinterpret_cast<const uint64_t*>(b + 8);
    if (n > 0 && n < 40) {
      const size_t m = static_cast<size_t>(n) < cap - 1 ? static_cast<size_t>(n) : cap - 1;
      memcpy(out, b + 0x10, m);
      out[m] = 0;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = 0;
  }
}

void FlushPending(bool afterWater, bool seaEnd = false);
void FarCapture(const Run& r, int idx, bool seaEnd);

// R51 VrFx: keeps HMD view idx's run (references) and copies its gProj into the colour pass's state on the GPU.
void FxKeepRun(const Run& r, int idx) {
  if (idx < 0 || idx > 3) return;
  if (g_fxRunValid[idx]) ReleaseRun(g_fxRun[idx]);
  g_fxRun[idx] = r;
  if (r.dsv) r.dsv->AddRef();
  if (r.cb) r.cb->AddRef();
  g_fxRunValid[idx] = true;
  cpass::FxNoteProj(g_s.ctx, idx, r.cb, r.cbFirst);
}

void CloseRun() {
  State& s = g_s;
  Run& r = s.run;
  if (!r.open) return;
  {
    const int w = static_cast<int>(r.vp.Width), h = static_cast<int>(r.vp.Height);
    int k = 0;
    for (auto& z : s.sizes)
      if (z.first == w && z.second == h) ++k;
    const int rank = s.runsThisFrame++;
    s.sizes.emplace_back(w, h);
    g_runs++;
    bool target = false;
    const bool hmd = s.lastHmdCount >= 2;
    int idx = rank;
    if (hmd) {
      // DCS renders the HMD views in order (each ends with a CopyToHMDPass;
      // MFD sensor views come before all of them): view t's G-buffer run is
      // the first run after the t-th CopyToHMDPass. With DLSS all views share
      // one depth target and render size, so the size cannot tell them apart.
      idx = s.hmdOrd;
      Decide();
      int t = std::clamp(static_cast<int>(s.want) + g_hmdOffset.load(), 0, s.lastHmdCount - 1);
      if (g_verify.load()) t = static_cast<int>(s.verifyCounter % static_cast<uint32_t>(s.lastHmdCount));
      target = s.hmdOrd == t;
    } else if (g_verify.load()) {
      const size_t n = s.lastSizes.empty() ? 1 : s.lastSizes.size();
      target = static_cast<size_t>(rank) == s.verifyCounter % n;
    } else if (!s.lastSizes.empty()) {
      std::pair<int, int> big{0, 0};
      for (auto& z : s.lastSizes)
        if (static_cast<int64_t>(z.first) * z.second > static_cast<int64_t>(big.first) * big.second) big = z;
      target = w == big.first && h == big.second && k == g_targetK.load();
    }
    // The grid: the left periphery (HMD view 0 + the learned offset; in the
    // fallback without CopyToHMDPass the frame's first run).
    if (g_gridOn.load() &&
        (hmd ? idx == std::clamp(g_hmdOffset.load(), 0, s.lastHmdCount - 1) : rank == 0)) {
      if (s.hasGridPending) ReleaseRun(s.gridPending);
      s.gridPending = r;
      if (r.dsv) r.dsv->AddRef();
      if (r.cb) r.cb->AddRef();
      s.hasGridPending = true;
      s.waterSeen = false;
    }
    // Far depth export: the focus views (HMD views 2 and 3 by the
    // CopyToHMDPass ordinal), copied at their WaterSeaPass end (FlushPending).
    if (fdexport::Active() && hmd && (idx == 2 || idx == 3)) {
      if (r.cpuProj && fdexport::g_logProjLeft.load() > 0) {
        fdexport::LogProj(s.frame, static_cast<uint32_t>(idx), r.proj, r.vp);
        fdexport::g_loggedThisFrame = true;
      }
      if (fdexport::g_mask & (1u << idx)) {
        if (s.hasFarPending) {  // not expected: the next view's passes flush it first
          s.hasFarPending = false;
          FarCapture(s.farPending, s.farIdx, false);
          ReleaseRun(s.farPending);
        }
        s.farPending = r;
        if (r.dsv) r.dsv->AddRef();
        if (r.cb) r.cb->AddRef();
        s.hasFarPending = true;
        s.farIdx = idx;
        s.waterSeen = false;
      }
    }
    // R65 cockpit mask: the configured HMD views, sampled at their WaterSeaPass end (or before their transparents).
    if (hmd && cmask::Active()) cmask::NoteRun(r, idx, s.lastHmdCount, g_runMv);  // R70: with the run's MV target
    // R51 VrFx: every HMD view's run (GazeDof's depth; both effects' projection).
    if (hmd && idx >= 0 && idx <= 3 && vrfx::Wanted()) FxKeepRun(r, idx);
    if (!g_coneOn.load()) target = false;
    if (target && !s.sampled) {
      s.sampled = true;
      if (!s.loggedSetup) {
        s.loggedSetup = true;
        Log("fixation depth: first target run: depth %ux%u format %u variant %d slice %u, viewport %.0f,%.0f "
            "%.0fx%.0f depth %.2f..%.2f, depth func %d, cPerView %s (first %u num %u), %s %d size rank %d, "
            "%d G-buffer passes, execute frame arg %llu",
            r.texW, r.texH, static_cast<unsigned>(r.texFmt), r.variant, r.slice, r.vp.TopLeftX, r.vp.TopLeftY,
            r.vp.Width, r.vp.Height, r.vp.MinDepth, r.vp.MaxDepth, static_cast<int>(r.depthFunc),
            r.cb ? "bound" : "missing", r.cbFirst, r.cbNum, hmd ? "HMD view" : "run rank", idx, k, r.passes,
            static_cast<unsigned long long>(ptiming::g_execFrame.load(std::memory_order_relaxed)));
      }
      if (!g_passesMin.load() || r.passes < g_passesMin.load()) g_passesMin = r.passes;
      if (r.passes > g_passesMax.load()) g_passesMax = r.passes;
      // Sampled after DCS's water passes (FlushPending): they draw water into
      // this depth target, with depth writes on and the WATER stencil type
      // (Bazar/shaders/enlight/waterCommon.hlsl Water_DepthStencilState).
      if (s.hasPending) ReleaseRun(s.pending);
      s.pending = r;
      r = Run{};
      s.hasPending = true;
      s.pendingIdx = idx;
      s.pendingW = w;
      s.pendingH = h;
      s.pendingHmd = hmd;
      s.waterSeen = false;
    }
  }
  ReleaseRun(r);
}

// Samples the waiting target run: at the end of WaterSeaPass (DCS's last
// water pass of the view; afterWater), or, when the view has none, at the
// next pass that could change or reuse its depth target's meaning
// (GBufferLightingPass, the next view, the frame end).
// The depth grid of the grid's target run (the left periphery: the whole
// field of view), after the water passes like the cone: one 32 x 32-group
// dispatch reading every texel of the viewport once, the 12 KB result copied
// into a 3-slot staging ring, mapped with DO_NOT_WAIT >= 2 frames later.
void SampleGrid(const Run& r, bool afterWater) {
  State& s = g_s;
  LARGE_INTEGER t0, t1;
  QueryPerformanceCounter(&t0);
  Decide();  // the newest gaze's displayTime (no gaze: 0)
  if (s.gridCount == 3) {
    g_gridRingFull++;
    return;
  }
  if (r.variant < 0 || !CbBigEnough(r) || !s.gridOut) return;
  const SrvEntry* se = FindSrv(r);
  if (!se) {
    g_noSrv++;
    return;
  }
  ID3D11DeviceContext* c = s.ctx;
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(c->Map(s.gridParams, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
  GridParams gp{};
  gp.vpOrg[0] = r.vp.TopLeftX;
  gp.vpOrg[1] = r.vp.TopLeftY;
  gp.vpSize[0] = r.vp.Width;
  gp.vpSize[1] = r.vp.Height;
  gp.tex[0] = static_cast<int32_t>(r.texW);
  gp.tex[1] = static_cast<int32_t>(r.texH);
  gp.minD = r.vp.MinDepth;
  gp.maxD = r.vp.MaxDepth;
  memcpy(m.pData, &gp, sizeof(gp));
  c->Unmap(s.gridParams, 0);
  Slot& sl = s.gridRing[(s.gridHead + s.gridCount) % 3];
  const bool check = g_verify.load(std::memory_order_relaxed) != 0;
  Bind before;
  if (check) before = ReadBind(c, s.ctx1);
  if (sl.dj) c->Begin(sl.dj);
  if (sl.ts0) c->End(sl.ts0);
  ID3D11ShaderResourceView* srvs[2] = {se->srv, se->stencil};
  RunCs(r, s.gridParams, s.gridCs[r.variant], srvs, s.gridUav, kGridN, kGridN);
  c->CopyResource(sl.staging, s.gridOut);
  if (sl.ts1) c->End(sl.ts1);
  if (sl.dj) c->End(sl.dj);
  sl.timed = sl.ts0 && sl.ts1 && sl.dj;
  if (check) {
    g_stateChecks++;
    if (!(ReadBind(c, s.ctx1) == before)) g_stateDiffs++;
  }
  sl.busy = sl.gpu = true;
  sl.frame = s.frame;
  sl.gaze = s.haveGaze ? s.gaze : GazeOut{};
  sl.view = 0;
  sl.afterWater = afterWater;
  ++s.gridCount;
  g_gridDispatches++;
  QueryPerformanceCounter(&t1);
  sl.cpuUs = (t1.QuadPart - t0.QuadPart) * g_qpcToUs;
}

void PollGrid() {
  State& s = g_s;
  while (s.gridCount > 0) {
    Slot& sl = s.gridRing[s.gridHead];
    if (s.frame - sl.frame < 2) break;
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = s.ctx->Map(sl.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
      g_gridLate++;
      break;
    }
    Job j;
    j.kind = 1;
    j.meta = sl;
    j.meta.staging = nullptr;
    j.meta.ts0 = j.meta.ts1 = j.meta.dj = nullptr;
    j.latency = static_cast<double>(s.frame - sl.frame);
    if (SUCCEEDED(hr)) {
      const float* f = static_cast<const float*>(m.pData);
      j.data = TakeData();  // R91 (M4)
      j.data.assign(f, f + kGridWords);
      s.ctx->Unmap(sl.staging, 0);
      j.gpu = true;
    }
    if (sl.timed) {
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
      UINT64 a = 0, b = 0;
      if (s.ctx->GetData(sl.dj, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !dj.Disjoint &&
          dj.Frequency && s.ctx->GetData(sl.ts0, &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
          s.ctx->GetData(sl.ts1, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && b >= a)
        j.gpuUs = (b - a) * 1e6 / static_cast<double>(dj.Frequency);
    }
    sl.busy = sl.gpu = false;
    s.gridHead = (s.gridHead + 1) % 3;
    --s.gridCount;
    PushJob(std::move(j));
  }
}

// Far depth export (far_depth.h 2-5): the waiting focus view's depth+stencil
// copied whole into our ring texture, then its descriptor pushed to Quad
// Views. seaEnd: at the view's WaterSeaPass end (else a fallback point). No
// binding is read or changed.
void FarCapture(const Run& r, int idx, bool seaEnd) {
  State& s = g_s;
  if (!fdexport::Active() || !(fdexport::g_mask & (1u << idx)) || !s.ctx || !s.dev) return;
  LARGE_INTEGER t0, t1;
  QueryPerformanceCounter(&t0);
  // Identity must be certain (FAR_DEPTH.md duty 5).
  if (!r.armOk) {
    fdexport::g_skipArm++;
    return;
  }
  if (s.lastHmdCount != 4 || g_hmdOffset.load() != 0) {
    fdexport::g_skipIdentity++;
    return;
  }
  if (r.variant == 2 || r.variant == 3) {
    fdexport::g_skipMsaa++;
    return;
  }
  if (r.variant < 0 || !fdexport::FormatOk(r.rawFmt) || !r.tex) {
    fdexport::g_skipFormat++;
    return;
  }
  const Proj p = ParseProj(r.proj);
  if (!r.cpuProj || !p.ok) {
    fdexport::g_skipNoProj++;
    return;
  }
  if (void* gv = g_gazeView.load()) {
    GazeOut g{};
    if (SeqRead(gv, g) && (g.flags & kGazeValid) && g.viewCount >= 4) {
      double f[4];
      DcsFov(p, f);
      const Match m = MatchFov(g, f);
      if (m.decisive && m.best != idx) {
        fdexport::g_skipMismatch++;
        return;
      }
    }
  }
  const int slots = std::clamp(fdexport::g_slots.load(), 1, fdexport::kMaxSlots);
  const int slot = static_cast<int>(s.frame % static_cast<uint64_t>(slots));
  ID3D11Texture2D* t = fdexport::EnsureTex(s.dev, idx, slot, r.rawFmt, r.texW, r.texH, slots);  // R91: all slots
  if (!t) {
    fdexport::g_skipFormat++;
    return;
  }
  ID3D11DeviceContext* c = s.ctx;
  const bool check = g_verify.load(std::memory_order_relaxed) != 0;
  Bind before;
  if (check) before = ReadBind(c, s.ctx1);
  // Whole subresource, no box (D3D11 forbids a source box on depth-stencil);
  // the DSV's slice into our subresource 0.
  c->CopySubresourceRegion(t, 0, 0, 0, 0, r.tex, D3D11CalcSubresource(0, r.slice, r.mipLevels), nullptr);
  fdexport::g_copies++;
  if (check) {
    fdexport::g_stateChecks++;
    if (!(ReadBind(c, s.ctx1) == before)) fdexport::g_stateDiffs++;
  }
  if (seaEnd)
    fdexport::g_afterWater++;
  else
    fdexport::g_fallback++;
  fdexport::CaptureInfo ci;
  ci.hmdView = static_cast<uint32_t>(idx);
  ci.engineFrame = s.frame;
  ci.generation = fdexport::g_gen;
  ci.format = r.rawFmt;
  ci.texture = t;
  ci.texW = r.texW;
  ci.texH = r.texH;
  ci.vp = r.vp;
  memcpy(ci.proj, r.proj, sizeof(ci.proj));
  memcpy(ci.viewInv, r.viewInv, sizeof(ci.viewInv));
  ci.viewInvValid = true;
  ci.afterWater = seaEnd;
  ci.fallback = !seaEnd;
  fdexport::PendingPush& pp = fdexport::g_pend[idx - 2];
  fdexport::ClearPend(pp);
  if (!fdexport::FillView(ci, pp.d)) {
    fdexport::g_skipNoProj++;
    return;
  }
  pp.on = true;
  // Pushed with its pre-overlay colour (FarPreOverlay, at the overlay pass
  // end) or at the view's CopyToHMDPass end; at once without the pre-overlay.
  if (!fdexport::g_preOn.load(std::memory_order_relaxed)) fdexport::DoPush(idx);
  QueryPerformanceCounter(&t1);
  const double us = (t1.QuadPart - t0.QuadPart) * g_qpcToUs;
  std::lock_guard<std::mutex> lock(fdexport::g_statMutex);
  fdexport::g_cpuUsSum += us;
  fdexport::g_cpuUsMax = std::max(fdexport::g_cpuUsMax, us);
  ++fdexport::g_cpuN;
}

// Copies the viewport box of `rtv` (resource `res`, bound by DCS) into view
// `view`'s pre-overlay texture and marks its waiting descriptor. point: 0 at
// the first viewport after the bind, 1 right after the scene composite draw,
// 2 just before the first draw into the target (not full-screen). Reads the
// target only; no binding changes.
void FarPreCopy(int view, ID3D11RenderTargetView* rtv, ID3D11Resource* res, const D3D11_VIEWPORT& vp, int point) {
  State& s = g_s;
  fdexport::PendingPush& p = fdexport::g_pend[view - 2];
  if (!p.on) return;
  ID3D11Texture2D* t = nullptr;
  if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) || !t) {
    fdexport::g_preSkipFormat++;
    return;
  }
  D3D11_TEXTURE2D_DESC td{};
  t->GetDesc(&td);
  t->Release();
  D3D11_RENDER_TARGET_VIEW_DESC rd{};
  rtv->GetDesc(&rd);
  if (td.SampleDesc.Count != 1 || rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMS ||
      rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY) {
    fdexport::g_preSkipMsaa++;
    return;
  }
  UINT mip = 0, slice = 0;
  if (rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) {
    mip = rd.Texture2D.MipSlice;
  } else if (rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY && rd.Texture2DArray.ArraySize == 1) {
    mip = rd.Texture2DArray.MipSlice;
    slice = rd.Texture2DArray.FirstArraySlice;
  } else {
    fdexport::g_preSkipFormat++;
    return;
  }
  const UINT w = std::max(1u, td.Width >> mip), h = std::max(1u, td.Height >> mip);
  D3D11_BOX box{};
  if (!fdexport::PreBox(vp, w, h, box)) {
    fdexport::g_preNoViewport++;
    return;
  }
  const UINT bw = box.right - box.left, bh = box.bottom - box.top;
  const DXGI_FORMAT dstFmt = fdexport::TypelessOf(td.Format);
  const int slots = std::clamp(fdexport::g_slots.load(), 1, fdexport::kMaxSlots);
  const int slot = static_cast<int>(s.frame % static_cast<uint64_t>(slots));
  // 5c: room for the post-overlay copy at (w, 0).
  bool post = fdexport::g_postOn.load(std::memory_order_relaxed);
  if (post && 2 * bw > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
    post = false;
    fdexport::g_postSkipWide++;
  }
  ID3D11Texture2D* pre = fdexport::EnsurePreTex(s.dev, view, slot, dstFmt, post ? 2 * bw : bw, bh, slots);  // R91
  if (!pre) {
    fdexport::g_preSkipFormat++;
    return;
  }
  ID3D11DeviceContext* c = s.ctx;
  const bool check = g_verify.load(std::memory_order_relaxed) != 0;
  Bind before;
  if (check) before = ReadBind(c, s.ctx1);
  c->CopySubresourceRegion(pre, 0, 0, 0, 0, res, D3D11CalcSubresource(mip, slice, td.MipLevels), &box);
  if (check) {
    fdexport::g_stateChecks++;
    if (!(ReadBind(c, s.ctx1) == before)) fdexport::g_stateDiffs++;
  }
  if (!(p.d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY)) fdexport::g_preCopies++;  // one per view and frame
  p.d.preOverlay = pre;
  p.d.preOverlayFormat = static_cast<uint32_t>(rd.Format != DXGI_FORMAT_UNKNOWN ? rd.Format : td.Format);
  p.d.preOverlaySize = bw | (bh << 16);
  p.d.flags |= DCSVR_DEPTH_HAS_PRE_OVERLAY;
  p.d.flags &= ~static_cast<uint32_t>(DCSVR_DEPTH_PRE_POST_OVERLAY);
  if (p.postSrc) p.postSrc->Release();
  p.postSrc = nullptr;
  if (post) {  // the same box of the same target, at the view's CopyToHMDPass begin
    p.postSrc = res;
    res->AddRef();
    p.postSub = D3D11CalcSubresource(mip, slice, td.MipLevels);
    p.postBox = box;
    p.postDst = pre;
  }
  fdexport::g_target[view - 2] = res;
  if (!fdexport::g_preLogged[view - 2]) {
    fdexport::g_preLogged[view - 2] = true;
    static const char* const kPoint[3] = {"the first viewport after the bind", "after the scene composite draw",
                                          "before the first draw into it"};
    Log("far depth: pre-overlay of HMD view %d at %s (%s): target %ux%u format %u (RTV format %u, mip %u slice %u), "
        "viewport %.2f,%.2f %.2fx%.2f -> box %u,%u %ux%u copied as format %u",
        view, g_overlayLdr ? "OverlayLDRPass" : "GUIPass", kPoint[point], td.Width, td.Height,
        static_cast<unsigned>(td.Format), static_cast<unsigned>(rd.Format), mip, slice, vp.TopLeftX, vp.TopLeftY,
        vp.Width, vp.Height, box.left, box.top, bw, bh, static_cast<unsigned>(dstFmt));
  }
}

// 5b: the focus view whose OverlayLDRPass is watched for its scene composite.
struct Cmp {
  int view = -1;                 // 2 or 3 while watched, else -1
  bool pending = false;          // target bound, viewport set: waiting for the first draw into it
  bool learning = false;         // the view's copy point is not decided yet (the viewport copy was made)
  ID3D11Resource* res = nullptr;  // the target (identity; DCS keeps it bound)
  D3D11_VIEWPORT vp{};           // the first viewport after the bind (the copied box)
  int others = 0;                // draws into other targets meanwhile
  ID3D11RenderTargetView* rtv = nullptr;  // AddRef'd from the composite draw's begin to its end
} g_cmp;

// The overlay trace window ([Perception] FarDepthOverlayTrace): a focus
// view's passes from ToneMappingPass begin to OverlayLDRPass end.
bool g_trOpen = false;
int g_trView = -1;
char g_trPass[48] = "";
int g_trDraws = 0, g_trTargetDraws = 0;

// 5d. The HDR source per focus view: the resource ToneMappingPass's first
// draw reads at PS SRV 0 (AddRef'd), its subresource, formats, size and the
// viewport of that draw.
struct HdrSrc {
  ID3D11Resource* res = nullptr;
  UINT sub = 0, texW = 0, texH = 0;
  DXGI_FORMAT texFmt = DXGI_FORMAT_UNKNOWN, viewFmt = DXGI_FORMAT_UNKNOWN;
  D3D11_VIEWPORT vp{};
};
HdrSrc g_hdr[2];
bool g_hdrLogged[2] = {};
// R91 (R90 phase 2, M3): the "HDR source of HMD view" line at most this often per view and install (a source that
// alternates between two resources logged one line per frame on the render thread); the learns stay counted.
constexpr int kHdrSrcLogMax = 8;
int g_hdrLogN[2] = {};
int g_hdrLearnView = -1;  // ToneMappingPass of this view: its first draw's PS SRV 0 is learnt
bool g_hdrArmOwn = false;  // the learning arm is ours (no overlay / trace arm under it)
bool g_dlssSeen[2] = {};   // this frame: the view's DLSSPass ended
void HdrRelease() {
  for (HdrSrc& h : g_hdr) {
    if (h.res) h.res->Release();
    h = HdrSrc{};
  }
  g_hdrLogged[0] = g_hdrLogged[1] = false;
  g_hdrLogN[0] = g_hdrLogN[1] = 0;  // R91
  g_hdrLearnView = -1;
  g_dlssSeen[0] = g_dlssSeen[1] = false;
}
void HdrLearnDraw(const D3D11_VIEWPORT& vp, UINT nvp);  // below

int g_cmpPassView = -1;          // the view watched in the running OverlayLDRPass (until its end)
bool g_cmpPassObserved = false;  // its first draw into the target was seen

// The watch of this pass ends; the draw hooks go right after the decision.
void CmpFinish() {
  if (g_cmp.rtv) g_cmp.rtv->Release();
  g_cmp = Cmp{};
  if (!g_trOpen) DisarmDraws();
}

void LogCopyPoint(int view, int mode, int kind, UINT count) {
  if (fdexport::g_modeLogged[view - 2] || mode == fdexport::kPreLearn) return;
  fdexport::g_modeLogged[view - 2] = true;
  const int seen = fdexport::g_learnSeen[view - 2], full = fdexport::g_learnFull[view - 2];
  if (mode == fdexport::kPreAfterComposite)
    Log("far depth: pre-overlay copy point of HMD view %d: after the scene composite draw (%s, %u vertices) in "
        "OverlayLDRPass (%d of %d learning passes began with a full-screen draw into the target)",
        view, fdexport::kDrawNames[kind], count, full, seen);
  else if (seen)
    Log("far depth: pre-overlay copy point of HMD view %d: the first viewport after the target bind (first draw "
        "%s, %u vertices; %d of %d learning passes began with a full-screen draw); draw hooks off",
        view, fdexport::kDrawNames[kind], count, full, seen);
  else
    Log("far depth: pre-overlay copy point of HMD view %d: the first viewport after the target bind (no draw into "
        "the target in %d OverlayLDRPasses); draw hooks off",
        view, fdexport::kLearnMaxPasses);
}

// The size of `rtv`'s mip (0 x 0 when not a 2D texture).
void RtvSize(ID3D11RenderTargetView* rtv, ID3D11Resource* res, UINT* w, UINT* h) {
  *w = *h = 0;
  ID3D11Texture2D* t = nullptr;
  if (!rtv || !res || FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) || !t)
    return;
  D3D11_TEXTURE2D_DESC td{};
  t->GetDesc(&td);
  t->Release();
  D3D11_RENDER_TARGET_VIEW_DESC rd{};
  rtv->GetDesc(&rd);
  UINT mip = 0;
  if (rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) mip = rd.Texture2D.MipSlice;
  if (rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) mip = rd.Texture2DArray.MipSlice;
  *w = std::max(1u, td.Width >> mip);
  *h = std::max(1u, td.Height >> mip);
}

void TraceDraw(int kind, UINT count, UINT inst, ID3D11RenderTargetView* rtv, ID3D11Resource* res,
               const D3D11_VIEWPORT& vp, UINT nvp) {
  ++g_trDraws;
  const bool isTarget = res && res == fdexport::g_target[g_trView - 2];
  if (isTarget) ++g_trTargetDraws;
  if (g_trDraws > 16) return;
  State& s = g_s;
  UINT w = 0, h = 0;
  RtvSize(rtv, res, &w, &h);
  D3D11_RENDER_TARGET_VIEW_DESC rdesc{};
  if (rtv) rtv->GetDesc(&rdesc);
  const HdrSrc& hs = g_hdr[g_trView - 2];
  ID3D11PixelShader* ps = nullptr;
  s.ctx->PSGetShader(&ps, nullptr, nullptr);
  if (ps) ps->Release();  // identity
  ID3D11ShaderResourceView* srv = nullptr;
  s.ctx->PSGetShaderResources(0, 1, &srv);
  ID3D11Resource* srvRes = nullptr;
  if (srv) {
    srv->GetResource(&srvRes);
    if (srvRes) srvRes->Release();
    srv->Release();
  }
  const bool full = fdexport::FullScreenDraw(kind, count, inst, vp, w, h);
  char l[400];
  snprintf(l, sizeof(l),
           "frame %llu view %d %s draw %d: %s(%u) x%u; RTV0 res %p %ux%u format %u; viewport %.1f,%.1f %.1fx%.1f "
           "(%u); PS %p; PS SRV0 res %p; full-screen %s; pre-overlay target %s; HDR source %s",
           static_cast<unsigned long long>(s.frame), g_trView, g_trPass, g_trDraws, fdexport::kDrawNames[kind], count,
           inst, static_cast<void*>(res), w, h, static_cast<unsigned>(rdesc.Format), vp.TopLeftX, vp.TopLeftY,
           vp.Width, vp.Height, nvp, static_cast<void*>(ps), static_cast<void*>(srvRes), full ? "yes" : "no",
           !fdexport::g_target[g_trView - 2] ? "unknown" : isTarget ? "YES" : "no",
           !hs.res ? "unknown" : (res && res == hs.res) ? "YES" : "no");
  fdexport::TraceLine(l);
}

// ---------------------------------------------------------------------------
// R41 live probes (dev, measure only; [Color] ToneProbe / SwapProbe, [Suite] GeoOnlyProbe; color_grade.h).
// Nothing here runs while the keys are 0. Each probe reads state at a pass's first draw (the draw entries of an
// arm, as 5d's HDR learning) and reads back through staging copies with a Map wait (dev only: a few frames).
// ---------------------------------------------------------------------------
int g_tpView = -1;            // ToneMappingPass of this view: its first draw is probed
bool g_tpArmOwn = false;
int g_tpFramesLeft = 0;       // probe frames left in the running session
bool g_tpThisFrame = false;   // this frame is probed (decided at view 0's ToneMappingPass)
int g_tpSession = 0;
bool g_tpHdrDone[4] = {};     // one HDR file per view and session
bool g_tpCbDone[4] = {};      // one full cb0 dump per view and session
std::wstring g_tpDir;
// R91 (R90 phase 2): the tone probe's HDR files and folders are written by the config thread (Tick), not the render
// thread (a file write can stall for ms on a filter driver or a reader of the folder).
struct TpFile {
  std::wstring root, dir, name;
  std::vector<float> data;
};
std::mutex g_tpFileMutex;
std::vector<TpFile> g_tpFiles;  // under g_tpFileMutex
std::wstring g_tpRoot;          // render thread: tone_probe\ of this session's g_tpDir
void WriteToneProbeFiles() {
  std::vector<TpFile> files;
  {
    std::lock_guard<std::mutex> lock(g_tpFileMutex);
    files.swap(g_tpFiles);
  }
  for (const TpFile& f : files) {
    CreateDirectoryW(f.root.c_str(), nullptr);
    CreateDirectoryW(f.dir.c_str(), nullptr);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, (f.dir + f.name).c_str(), L"wb") == 0 && fp) {
      fwrite(f.data.data(), sizeof(float), f.data.size(), fp);
      fclose(fp);
    }
  }
}
int g_spView = -1;            // CopyToHMDPass of this view: its first draw is probed
bool g_spArmOwn = false;
int g_spFramesLeft = 0;
bool g_spThisFrame = false;
struct SwapSeen {
  ID3D11Resource* dst = nullptr;  // the RTV's resource (swapchain image), AddRef'd
  ID3D11Resource* src = nullptr;  // PS SRV 0's resource (the view's LDR image), AddRef'd
  UINT dstSub = 0, srcSub = 0;
  D3D11_VIEWPORT vp{};
};
SwapSeen g_spSeen;
// R71: [Perception] CockpitFgDump: HMD view 0 / 1's CopyToHMDPass, its first draw's target (the swapchain image) and
// PS SRV 0 (its source), kept until the pass end, where cmask::FgNoteColour copies the submitted box.
int g_fgView = -1;
bool g_fgArmOwn = false;
SwapSeen g_fgSeen;
// R71 live: a view's CopyToHMDPass whose draw was not seen (draw arm refused or lost: ~1 frame in 36) takes its copy
// source instead, the view's LDR image as seen in the previous frame (the bytes DCS copies unchanged: R41 verdict A).
int g_fgWant = -1;                         // the CopyToHMDPass (view) that should be captured
ID3D11Resource* g_fgLastSrc[2] = {};       // the view's last copy source (AddRef'd while a dump runs)
UINT g_fgLastSrcSub[2] = {};
uint64_t g_fgLastSrcFrame[2] = {};
std::atomic<uint64_t> g_fgArmMiss{0}, g_fgDrawMiss{0}, g_fgFallback{0};
// GeoOnlyProbe: the focus views' main GBufferPass is open (first GBufferPass of HMD view 2 / 3).
bool g_geoActive = false;
int g_geoLastView = -1;
uint64_t g_geoPasses = 0, g_geoVps = 0;

const char* ProbeFmtName(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
    case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
    default: return "other";
  }
}

// A staging copy of a resource, mapped; Done unmaps and releases.
struct ProbeMap {
  ID3D11Resource* st = nullptr;
  D3D11_MAPPED_SUBRESOURCE m{};
  bool ok = false;
  void Done(ID3D11DeviceContext* c) {
    if (st) {
      if (ok) c->Unmap(st, 0);
      st->Release();
    }
    st = nullptr;
    ok = false;
  }
};
bool ProbeReadTex(ID3D11Texture2D* t, UINT sub, const D3D11_BOX* box, ProbeMap& pm) {
  State& s = g_s;
  D3D11_TEXTURE2D_DESC td{};
  t->GetDesc(&td);
  if (td.SampleDesc.Count != 1 || !s.dev) return false;
  const UINT mip = sub % td.MipLevels;
  D3D11_TEXTURE2D_DESC sd{};
  sd.Width = box ? box->right - box->left : std::max(1u, td.Width >> mip);
  sd.Height = box ? box->bottom - box->top : std::max(1u, td.Height >> mip);
  sd.MipLevels = sd.ArraySize = 1;
  sd.Format = td.Format;
  sd.SampleDesc.Count = 1;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* st = nullptr;
  if (FAILED(s.dev->CreateTexture2D(&sd, nullptr, &st)) || !st) return false;
  s.ctx->CopySubresourceRegion(st, 0, 0, 0, 0, t, sub, box);
  pm.st = st;
  pm.ok = SUCCEEDED(s.ctx->Map(st, 0, D3D11_MAP_READ, 0, &pm.m));
  return pm.ok;
}
bool ProbeReadBuf(ID3D11Buffer* b, ProbeMap& pm) {
  State& s = g_s;
  D3D11_BUFFER_DESC bd{};
  b->GetDesc(&bd);
  if (!bd.ByteWidth || !s.dev) return false;
  D3D11_BUFFER_DESC sd{};
  sd.ByteWidth = bd.ByteWidth;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  sd.StructureByteStride = bd.StructureByteStride;
  sd.MiscFlags = bd.MiscFlags & (D3D11_RESOURCE_MISC_BUFFER_STRUCTURED | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS);
  ID3D11Buffer* st = nullptr;
  if (FAILED(s.dev->CreateBuffer(&sd, nullptr, &st)) || !st) return false;
  s.ctx->CopyResource(st, b);
  pm.st = st;
  pm.ok = SUCCEEDED(s.ctx->Map(st, 0, D3D11_MAP_READ, 0, &pm.m));
  return pm.ok;
}
void ProbeLogFloats(const char* what, const void* p, UINT bytes, bool half) {
  char l[900];
  int n = snprintf(l, sizeof(l), "tone probe:   %s:", what);
  const UINT cnt = std::min<UINT>(half ? bytes / 2 : bytes / 4, 24);
  for (UINT i = 0; i < cnt && n < static_cast<int>(sizeof(l)) - 24; ++i) {
    const float v = half ? cgrade::HalfToFloat(static_cast<const uint16_t*>(p)[i]) : static_cast<const float*>(p)[i];
    n += snprintf(l + n, sizeof(l) - n, " %.6g", v);
  }
  Log("%s", l);
}

// ToneMappingPass's first draw of view g_tpView.
void ToneProbeDraw(const D3D11_VIEWPORT& vp) {
  State& s = g_s;
  const int v = g_tpView;
  g_tpView = -1;
  if (!s.dev) return;
  ID3D11RenderTargetView* rtv = nullptr;
  s.ctx->OMGetRenderTargets(1, &rtv, nullptr);
  D3D11_RENDER_TARGET_VIEW_DESC rd{};
  D3D11_TEXTURE2D_DESC rtd{};
  if (rtv) {
    rtv->GetDesc(&rd);
    ID3D11Resource* r = nullptr;
    rtv->GetResource(&r);
    ID3D11Texture2D* t = nullptr;
    if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
      t->GetDesc(&rtd);
      t->Release();
    }
    if (r) r->Release();
    rtv->Release();
  }
  Log("tone probe: session %d view %d ToneMappingPass first draw: target RTV %s (%d) on texture %s (%d) %ux%u, "
      "viewport %.0f,%.0f %.0fx%.0f",
      g_tpSession, v, ProbeFmtName(rd.Format), static_cast<int>(rd.Format), ProbeFmtName(rtd.Format),
      static_cast<int>(rtd.Format), rtd.Width, rtd.Height, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);
  ID3D11ShaderResourceView* srvs[8] = {};
  s.ctx->PSGetShaderResources(0, 8, srvs);
  for (int i = 0; i < 8; ++i) {
    ID3D11ShaderResourceView* sv = srvs[i];
    if (!sv) continue;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sv->GetDesc(&sd);
    ID3D11Resource* r = nullptr;
    sv->GetResource(&r);
    sv->Release();
    if (!r) continue;
    D3D11_RESOURCE_DIMENSION dim{};
    r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
      ID3D11Buffer* b = static_cast<ID3D11Buffer*>(r);
      D3D11_BUFFER_DESC bd{};
      b->GetDesc(&bd);
      Log("tone probe:  t%d buffer %u bytes, view %s (%d), stride %u", i, bd.ByteWidth, ProbeFmtName(sd.Format),
          static_cast<int>(sd.Format), bd.StructureByteStride);
      ProbeMap pm;
      if (bd.ByteWidth <= 65536 && ProbeReadBuf(b, pm)) {
        char w[32];
        snprintf(w, sizeof(w), "t%d floats", i);
        ProbeLogFloats(w, pm.m.pData, std::min<UINT>(96, bd.ByteWidth), false);
      }
      pm.Done(s.ctx);
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
      ID3D11Texture2D* t = static_cast<ID3D11Texture2D*>(r);
      D3D11_TEXTURE2D_DESC td{};
      t->GetDesc(&td);
      Log("tone probe:  t%d texture %ux%u mips %u array %u, texture %s (%d), view %s (%d)", i, td.Width, td.Height,
          td.MipLevels, td.ArraySize, ProbeFmtName(td.Format), static_cast<int>(td.Format), ProbeFmtName(sd.Format),
          static_cast<int>(sd.Format));
      const bool f32 = td.Format == DXGI_FORMAT_R32_FLOAT || td.Format == DXGI_FORMAT_R32G32_FLOAT ||
                       td.Format == DXGI_FORMAT_R32G32B32A32_FLOAT || td.Format == DXGI_FORMAT_R32_TYPELESS;
      const bool f16 = td.Format == DXGI_FORMAT_R16_FLOAT || td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                       td.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS;
      if ((f32 || f16) && td.Width * td.Height <= 64 && td.SampleDesc.Count == 1) {
        ProbeMap pm;
        if (ProbeReadTex(t, 0, nullptr, pm)) {
          char w[32];
          snprintf(w, sizeof(w), "t%d texels", i);
          UINT bpp = 4;
          if (td.Format == DXGI_FORMAT_R32G32B32A32_FLOAT) bpp = 16;
          else if (td.Format == DXGI_FORMAT_R32G32_FLOAT || td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                   td.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS) bpp = 8;
          else if (td.Format == DXGI_FORMAT_R16_FLOAT) bpp = 2;
          ProbeLogFloats(w, pm.m.pData, std::min<UINT>(td.Width * bpp, pm.m.RowPitch), f16);
        }
        pm.Done(s.ctx);
      }
      const bool hdr = td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || td.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS;
      if (hdr && td.Width >= 256 && td.SampleDesc.Count == 1 && cgrade::g_toneProbeHdr.load() && !g_tpHdrDone[v] &&
          !g_tpDir.empty()) {
        g_tpHdrDone[v] = true;
        ProbeMap pm;
        const UINT sub = sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY
                             ? D3D11CalcSubresource(0, sd.Texture2DArray.FirstArraySlice, td.MipLevels)
                             : 0;
        if (ProbeReadTex(t, sub, nullptr, pm)) {
          const unsigned f = v >= 2 ? 8u : 4u;
          std::vector<float> ds;
          unsigned ow = 0, oh = 0;
          cgrade::DownsampleHalf(static_cast<const uint8_t*>(pm.m.pData), pm.m.RowPitch, td.Width, td.Height, f, ds,
                                 &ow, &oh);
          pm.Done(s.ctx);
          const cgrade::LumStats ls = cgrade::LumStatsOf(ds);
          wchar_t fn[96];
          swprintf_s(fn, L"hdr_s%d_v%d_%ux%u_f%u_%ux%u.f32", g_tpSession, v, td.Width, td.Height, f, ow, oh);
          {  // R91: written by the config thread (fixation_depth.h Tick)
            std::lock_guard<std::mutex> lock(g_tpFileMutex);
            if (g_tpFiles.size() < 16) g_tpFiles.push_back(TpFile{g_tpRoot, g_tpDir, fn, std::move(ds)});
          }
          Log("tone probe:  t%d HDR source of view %d: %ls queued (RGB float32, 1/%u box); luminance over the "
              "texture: mean %.5g, log-avg %.5g, p1 %.5g, p50 %.5g, p99 %.5g, max %.5g",
              i, v, fn, f, ls.mean, ls.logAvg, ls.p1, ls.p50, ls.p99, ls.max);
        }
        pm.Done(s.ctx);
      }
    } else {
      Log("tone probe:  t%d resource dimension %d, view %s (%d)", i, static_cast<int>(dim), ProbeFmtName(sd.Format),
          static_cast<int>(sd.Format));
    }
    r->Release();
  }
  ID3D11Buffer* cbs[4] = {};
  s.ctx->PSGetConstantBuffers(0, 4, cbs);
  for (int i = 0; i < 4; ++i) {
    if (!cbs[i]) continue;
    D3D11_BUFFER_DESC bd{};
    cbs[i]->GetDesc(&bd);
    if (i == 0 && bd.ByteWidth <= 4096) {
      ProbeMap pm;
      if (ProbeReadBuf(cbs[i], pm)) {
        const float* f = static_cast<const float*>(pm.m.pData);
        const UINT rows = std::min<UINT>(bd.ByteWidth / 16, g_tpCbDone[v] ? 11u : 64u);
        for (UINT r = 0; r < rows; ++r)
          Log("tone probe:  b0 view %d +0x%03x: %.6g %.6g %.6g %.6g", v, r * 16, f[r * 4], f[r * 4 + 1],
              f[r * 4 + 2], f[r * 4 + 3]);
        g_tpCbDone[v] = true;
      }
      pm.Done(s.ctx);
    } else {
      Log("tone probe:  b%d %u bytes", i, bd.ByteWidth);
    }
    cbs[i]->Release();
  }
}

// CopyToHMDPass's first draw of view g_spView: the target and the source, kept until the pass end.
void SwapProbeDraw(const D3D11_VIEWPORT& vp) {
  State& s = g_s;
  const int v = g_spView;
  g_spView = -1;
  ID3D11RenderTargetView* rtv = nullptr;
  s.ctx->OMGetRenderTargets(1, &rtv, nullptr);
  ID3D11ShaderResourceView* srv = nullptr;
  s.ctx->PSGetShaderResources(0, 1, &srv);
  D3D11_RENDER_TARGET_VIEW_DESC rd{};
  D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
  D3D11_TEXTURE2D_DESC dtd{}, stdd{};
  SwapSeen& w = g_spSeen;
  if (rtv) {
    rtv->GetDesc(&rd);
    rtv->GetResource(&w.dst);
    rtv->Release();
    ID3D11Texture2D* t = nullptr;
    if (w.dst && SUCCEEDED(w.dst->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
      t->GetDesc(&dtd);
      t->Release();
      w.dstSub = rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY
                     ? D3D11CalcSubresource(rd.Texture2DArray.MipSlice, rd.Texture2DArray.FirstArraySlice, dtd.MipLevels)
                     : rd.Texture2D.MipSlice;
    }
  }
  if (srv) {
    srv->GetDesc(&sd);
    srv->GetResource(&w.src);
    srv->Release();
    ID3D11Texture2D* t = nullptr;
    if (w.src && SUCCEEDED(w.src->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
      t->GetDesc(&stdd);
      t->Release();
      w.srcSub = sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY
                     ? D3D11CalcSubresource(sd.Texture2DArray.MostDetailedMip, sd.Texture2DArray.FirstArraySlice,
                                            stdd.MipLevels)
                     : sd.Texture2D.MostDetailedMip;
    }
  }
  w.vp = vp;
  Log("swap probe: view %d CopyToHMDPass first draw: RTV %s (%d) dim %d on texture %s (%d) %ux%u array %u sub %u; "
      "PS SRV0 %s (%d) on texture %s (%d) %ux%u; viewport %.0f,%.0f %.0fx%.0f",
      v, ProbeFmtName(rd.Format), static_cast<int>(rd.Format), static_cast<int>(rd.ViewDimension),
      ProbeFmtName(dtd.Format), static_cast<int>(dtd.Format), dtd.Width, dtd.Height, dtd.ArraySize, w.dstSub,
      ProbeFmtName(sd.Format), static_cast<int>(sd.Format), ProbeFmtName(stdd.Format), static_cast<int>(stdd.Format),
      stdd.Width, stdd.Height, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);
}
// CopyToHMDPass end: a 64 x 64 box of the source against the same box of the target (1:1 copy).
void SwapProbeEnd(int v) {
  State& s = g_s;
  SwapSeen& w = g_spSeen;
  if (w.dst && w.src && s.dev) {
    ID3D11Texture2D *dt = nullptr, *st = nullptr;
    w.dst->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&dt));
    w.src->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&st));
    if (dt && st) {
      D3D11_TEXTURE2D_DESC dd{}, sd{};
      dt->GetDesc(&dd);
      st->GetDesc(&sd);
      const UINT cx = static_cast<UINT>(w.vp.TopLeftX + w.vp.Width / 2);
      const UINT cy = static_cast<UINT>(w.vp.TopLeftY + w.vp.Height / 2);
      const bool oneToOne = std::fabs(w.vp.Width - static_cast<float>(sd.Width)) < 1 &&
                            std::fabs(w.vp.Height - static_cast<float>(sd.Height)) < 1;
      const UINT sx = static_cast<UINT>(w.vp.Width / 2), sy = static_cast<UINT>(w.vp.Height / 2);
      const D3D11_BOX db{cx - 32, cy - 32, 0, cx + 32, cy + 32, 1}, sb{sx - 32, sy - 32, 0, sx + 32, sy + 32, 1};
      ProbeMap dm, sm;
      const bool fit = cx >= 32 && cy >= 32 && sx >= 32 && sy >= 32 && cx + 32 <= dd.Width && cy + 32 <= dd.Height &&
                       sx + 32 <= sd.Width && sy + 32 <= sd.Height;
      const bool ok = fit && ProbeReadTex(dt, w.dstSub, &db, dm) && ProbeReadTex(st, w.srcSub, &sb, sm);
      if (ok) {
        std::vector<uint8_t> a(64 * 64 * 4), b(64 * 64 * 4);
        for (int y = 0; y < 64; ++y) {
          memcpy(&a[y * 256], static_cast<const uint8_t*>(sm.m.pData) + y * sm.m.RowPitch, 256);
          memcpy(&b[y * 256], static_cast<const uint8_t*>(dm.m.pData) + y * dm.m.RowPitch, 256);
        }
        const cgrade::CopyClass c = cgrade::ClassifyCopy(a.data(), b.data(), 64 * 64);
        double ma = 0, mb = 0;
        int dark = 0, darkEq = 0;
        for (int i = 0; i < 64 * 64; ++i)
          for (int ch = 0; ch < 3; ++ch) {
            ma += a[i * 4 + ch];
            mb += b[i * 4 + ch];
            if (a[i * 4 + ch] < 40) {
              ++dark;
              if (a[i * 4 + ch] == b[i * 4 + ch]) ++darkEq;
            }
          }
        Log("swap probe: view %d 64x64 box (source %u,%u -> target %u,%u, 1:1 %s): channels %d, equal %d, "
            "sRGB-encoded %d, other %d; mean byte source %.2f target %.2f; source bytes < 40: %d (%d equal); "
            "first px source %u %u %u target %u %u %u; verdict %c (A = bytes copied unchanged, B = encoded again)",
            v, sx, sy, cx, cy, oneToOne ? "yes" : "no", c.n, c.equal, c.srgb, c.other, ma / (64 * 64 * 3),
            mb / (64 * 64 * 3), dark, darkEq, a[0], a[1], a[2], b[0], b[1], b[2], cgrade::CopyVerdict(c));
      } else {
        Log("swap probe: view %d: box readback not possible (target %ux%u, source %ux%u)", v, dd.Width, dd.Height,
            sd.Width, sd.Height);
      }
      dm.Done(s.ctx);
      sm.Done(s.ctx);
    }
    if (dt) dt->Release();
    if (st) st->Release();
  }
  if (w.dst) w.dst->Release();
  if (w.src) w.src->Release();
  w = SwapSeen{};
}

// R71: CopyToHMDPass's first draw of view g_fgView (CockpitFgDump): the target and the source, no log.
void FgDraw(const D3D11_VIEWPORT& vp) {
  State& s = g_s;
  g_fgView = -1;
  SwapSeen& w = g_fgSeen;
  if (w.dst || w.src) return;
  ID3D11RenderTargetView* rtv = nullptr;
  s.ctx->OMGetRenderTargets(1, &rtv, nullptr);
  ID3D11ShaderResourceView* srv = nullptr;
  s.ctx->PSGetShaderResources(0, 1, &srv);
  if (rtv) {
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rtv->GetDesc(&rd);
    rtv->GetResource(&w.dst);
    rtv->Release();
    ID3D11Texture2D* t = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    if (w.dst && SUCCEEDED(w.dst->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
      t->GetDesc(&td);
      t->Release();
      w.dstSub = rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY
                     ? D3D11CalcSubresource(rd.Texture2DArray.MipSlice, rd.Texture2DArray.FirstArraySlice, td.MipLevels)
                     : rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D ? rd.Texture2D.MipSlice : 0;
    }
  }
  if (srv) {
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    srv->GetDesc(&sd);
    srv->GetResource(&w.src);
    srv->Release();
    ID3D11Texture2D* t = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    if (w.src && SUCCEEDED(w.src->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
      t->GetDesc(&td);
      t->Release();
      w.srcSub = sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY
                     ? D3D11CalcSubresource(sd.Texture2DArray.MostDetailedMip, sd.Texture2DArray.FirstArraySlice,
                                            td.MipLevels)
                     : sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D ? sd.Texture2D.MostDetailedMip : 0;
    }
  }
  w.vp = vp;
}
// R71: CopyToHMDPass end of view v: DCS's copy is in the stream; the submitted box goes to the dump's record.
void FgEnd(int v) {
  SwapSeen& w = g_fgSeen;
  cmask::FgNoteColour(v, w.dst, w.dstSub, w.vp, w.src, w.srcSub);
  if (w.src && (v == 0 || v == 1)) {  // the fallback of a later frame whose draw is not seen
    if (g_fgLastSrc[v] != w.src) {
      if (g_fgLastSrc[v]) g_fgLastSrc[v]->Release();
      g_fgLastSrc[v] = w.src;
      w.src->AddRef();
    }
    g_fgLastSrcSub[v] = w.srcSub;
    g_fgLastSrcFrame[v] = g_s.frame;
  }
  if (w.dst) w.dst->Release();
  if (w.src) w.src->Release();
  w = SwapSeen{};
}

void ProbeDisarm(bool& own) {
  if (own && !g_trOpen && !g_overlayArm && !g_hdrArmOwn && !g_dumpPass) DisarmOm();
  own = false;
}

// From OnBoundaryInner, before its own work for the boundary (hmdOrd not yet advanced at a CopyToHMDPass end).
void ProbeBoundary(void* pass, bool begin, int k) {
  State& s = g_s;
  const int v = s.hmdOrd;
  static int geoDbg = 0;
  if (k == plod::kGBuffer && begin && cgrade::g_geoOnlyProbe.load(std::memory_order_relaxed) && geoDbg < 24) {
    ++geoDbg;
    Log("probe: GeoOnlyProbe: GBufferPass begin, HMD ordinal %d, last view %d, ctx %p", v, g_geoLastView,
        static_cast<void*>(s.ctx));
  }
  if (v < 0 || v > 3 || !s.ctx) return;
  if (k == plod::kGBuffer) {
    if (begin) {
      g_geoActive = false;
      if (cgrade::g_geoOnlyProbe.load(std::memory_order_relaxed) && (v == 2 || v == 3) && g_geoLastView != v) {
        g_geoActive = true;  // the view's first GBufferPass: the main pass (the decal pass follows)
        ++g_geoPasses;
      }
      g_geoLastView = v;
    } else {
      if (g_geoActive && g_geoPasses % 600 == 1)
        Log("probe: GeoOnlyProbe: %llu focus main G-buffer passes so far, %llu viewports set off target",
            static_cast<unsigned long long>(g_geoPasses), static_cast<unsigned long long>(g_geoVps));
      g_geoActive = false;
    }
    return;
  }
  if (k == plod::kHmd) {
    // R71: CockpitFgDump: views 0 / 1's submitted image (their CopyToHMDPass's first draw arms, its end copies).
    if (begin) {
      g_fgWant = -1;
      if ((v == 0 || v == 1) && g_fgView < 0 && !g_fgSeen.dst && !g_fgSeen.src && cmask::FgColourWanted()) {
        g_fgWant = v;
        if (!g_armTable) g_fgArmOwn = ArmOm(true);
        ArmDraws();
        if (g_drawArmed) {
          g_fgView = v;
        } else {
          g_fgArmMiss++;
          ProbeDisarm(g_fgArmOwn);
        }
      }
    } else {
      g_fgView = -1;
      if (g_fgSeen.dst || g_fgSeen.src) {
        FgEnd(v);
      } else if (g_fgWant == v && (v == 0 || v == 1)) {
        g_fgDrawMiss++;
        if (g_fgLastSrc[v] && g_s.frame - g_fgLastSrcFrame[v] <= 2) {  // the same image target as in the last frame
          cmask::FgNoteColour(v, nullptr, 0, D3D11_VIEWPORT{}, g_fgLastSrc[v], g_fgLastSrcSub[v]);
          g_fgFallback++;
        }
        static int logged = 0;
        if (logged < 8) {
          ++logged;
          Log("cockpit mask: frame-generation dump: view %d CopyToHMDPass without a seen draw (draw arm %s); colour %s",
              v, g_fgArmMiss.load() ? "refused at least once" : "held", g_fgLastSrc[v] ? "from its copy source" : "missing");
        }
      }
      g_fgWant = -1;
      ProbeDisarm(g_fgArmOwn);
      if (!cmask::FgColourWanted())
        for (int i = 0; i < 2; ++i)
          if (g_fgLastSrc[i]) {
            g_fgLastSrc[i]->Release();
            g_fgLastSrc[i] = nullptr;
          }
    }
    if (begin) {
      if (v == 0) {
        if (g_spFramesLeft <= 0) {
          const int r = cgrade::g_swapProbeReq.exchange(0);
          if (r > 0) g_spFramesLeft = std::min(r, 30);
        }
        g_spThisFrame = g_spFramesLeft > 0;
        if (g_spThisFrame) --g_spFramesLeft;
      }
      if (g_spThisFrame && g_spView < 0) {
        if (!g_armTable) g_spArmOwn = ArmOm(true);
        ArmDraws();
        if (g_drawArmed)
          g_spView = v;
        else
          ProbeDisarm(g_spArmOwn);
      }
    } else {
      g_spView = -1;
      if (g_spSeen.dst || g_spSeen.src) SwapProbeEnd(v);
      ProbeDisarm(g_spArmOwn);
      if (v == 3) g_geoLastView = -1;
    }
    return;
  }
  if (k != plod::kOther) return;
  char n[48];
  PassName(pass, n, sizeof(n));
  if (strcmp(n, "ToneMappingPass") != 0) return;
  if (!begin) {
    g_tpView = -1;
    ProbeDisarm(g_tpArmOwn);
    return;
  }
  if (v == 0) {
    if (g_tpFramesLeft <= 0) {
      const int r = cgrade::g_toneProbeReq.exchange(0);
      if (r > 0) {
        g_tpFramesLeft = std::min(r, 600);
        ++g_tpSession;
        for (int i = 0; i < 4; ++i) g_tpHdrDone[i] = g_tpCbDone[i] = false;
        std::wstring base;
        {
          std::lock_guard<std::mutex> lock(ckipd::g_dumpDirMutex);
          base = ckipd::g_dumpBase;
        }
        const size_t at = base.rfind(L"cockpit_frames\\");
        g_tpDir.clear();
        if (at != std::wstring::npos) {
          SYSTEMTIME st;
          GetLocalTime(&st);
          wchar_t sub[48];
          swprintf_s(sub, L"tone_probe\\%04u%02u%02u_%02u%02u%02u\\", st.wYear, st.wMonth, st.wDay, st.wHour,
                     st.wMinute, st.wSecond);
          g_tpRoot = base.substr(0, at) + L"tone_probe\\";
          g_tpDir = base.substr(0, at) + sub;  // R91: made by the config thread with the session's first file
        }
        Log("tone probe: session %d, %d frames, views mask 0x%x, HDR files %d (%ls)", g_tpSession, g_tpFramesLeft,
            cgrade::g_toneProbeViews.load(), cgrade::g_toneProbeHdr.load(), g_tpDir.c_str());
      }
    }
    g_tpThisFrame = g_tpFramesLeft > 0;
    if (g_tpThisFrame) --g_tpFramesLeft;
  }
  if (g_tpThisFrame && (cgrade::g_toneProbeViews.load() & (1 << v)) && g_tpView < 0) {
    if (!g_armTable) g_tpArmOwn = ArmOm(true);
    ArmDraws();
    if (g_drawArmed)
      g_tpView = v;
    else
      ProbeDisarm(g_tpArmOwn);
  }
}
bool ProbesWanted() {
  return g_tpFramesLeft > 0 || g_tpView >= 0 || g_tpArmOwn || g_spFramesLeft > 0 || g_spView >= 0 || g_spArmOwn ||
         g_spSeen.dst || g_spSeen.src || g_geoActive || g_geoLastView >= 0 ||
         g_fgView >= 0 || g_fgArmOwn || g_fgSeen.dst || g_fgSeen.src || cmask::FgColourWanted() ||  // R71
         g_fgLastSrc[0] || g_fgLastSrc[1] ||
         cgrade::g_toneProbeReq.load(std::memory_order_relaxed) > 0 ||
         cgrade::g_swapProbeReq.load(std::memory_order_relaxed) > 0 ||
         cgrade::g_geoOnlyProbe.load(std::memory_order_relaxed) != 0;
}

// ---------------------------------------------------------------------------
// R42 colour modes ([Color] Mode / Timing; color_pass.h). The draw entries are armed in each HMD view's
// ToneMappingPass (its first draw: DCS's tone map, pixel shader swapped for ours by cpass::ToneDrawPre, put back by
// cpass::ToneDrawPost right after) and, while the views' image sizes are learnt, in their CopyToHMDPass (PS SRV 0 =
// the view's LDR image). Nothing is armed while both keys are 0. A fault in our part turns the colour modes off
// (cpass::g_fault), not fixation depth.
// ---------------------------------------------------------------------------
int g_cpView = -1;         // ToneMappingPass of this HMD view is open with our draw arm: its first draw is the tone map
bool g_cpArmOwn = false;
int g_cpDrawView = -1;     // the draw in flight is that view's tone map (ToneDrawPre ran)
int g_cpAct = 0;
int g_cpLearnView = -1;    // CopyToHMDPass of this view: its first draw's PS SRV 0 is the view's image
bool g_cpLearnArmOwn = false;
bool g_cpTpAdded = false;  // R47: the state-call toggle points were added to the arm for this ToneMappingPass
uint64_t g_cpArmFail = 0;  // arms refused (another module's context hooks hold the table)

// ---------------------------------------------------------------------------
// R94: [Perception] SymbologyTrace (sym_trace.h; dev, measure only). In each HMD view's ToneMappingPass of a learn or
// traced frame the draw entries are armed (as the R41 tone probe does: its own arm when none is held, else nested);
// the first draw learns the tone map's PS SRV0 / RTV 0 and, in a traced frame, copies the source. Nothing runs while
// no session is active (one bool test per boundary).
// ---------------------------------------------------------------------------
int g_stTmView = -1;     // ToneMappingPass of this HMD view is open with the draw arm: its first draw is traced
bool g_stArmOwn = false;
void SymTraceDisarm() {
  if (g_stArmOwn && !g_trOpen && !g_overlayArm && !g_hdrArmOwn && !g_dumpPass && !g_tpArmOwn && !g_spArmOwn &&
      !g_fgArmOwn && !g_cpArmOwn && !g_cpLearnArmOwn)
    DisarmOm();
  g_stArmOwn = false;
}
void SymTraceBoundary(void* pass, bool begin, int k) {
  State& s = g_s;
  symtr::OnBoundary(pass, begin, k);
  if (k == plod::kGBuffer && begin && (g_stTmView >= 0 || g_stArmOwn)) {  // a ToneMappingPass whose end did not come
    g_stTmView = -1;
    SymTraceDisarm();
    return;
  }
  if (k != plod::kOther) return;
  const int v = s.hmdOrd;
  if (v < 0 || v > 3 || !s.ctx) return;
  char n[48];
  PassName(pass, n, sizeof(n));
  if (strcmp(n, "ToneMappingPass") != 0) return;
  if (!begin) {
    g_stTmView = -1;
    SymTraceDisarm();
    return;
  }
  if (!symtr::g_learn || g_stTmView >= 0) return;
  if (!g_armTable) g_stArmOwn = ArmOm(true);
  ArmDraws();
  if (g_drawArmed)
    g_stTmView = v;
  else
    SymTraceDisarm();
}
// The tone map's first draw of view g_stTmView (DrawPre, before the colour pass's pixel shader swap).
void SymTraceToneDraw(const D3D11_VIEWPORT& vp) {
  const int v = g_stTmView;
  g_stTmView = -1;
  symtr::ToneDraw(v, g_s.ctx, vp);
  if (symtr::g_on.load(std::memory_order_relaxed) && symtr::g_tone && g_tpView < 0) {  // the R41 tone probe's lines
    Log("symtrace: frame %llu view %d tone probe:", static_cast<unsigned long long>(symtr::g_frameNo), v);
    g_tpView = v;
    ToneProbeDraw(vp);
  }
}
// R46: s.hmdOrd counts CopyToHMDPass ends since a frame start this install saw (EndFrame). Fixation depth installs
// from the config thread at any point of a frame (it does at each colour mode 0 -> 1/2 switch): until the next frame
// start hmdOrd is stale, and the views of that partial frame were numbered wrongly (the R46 log: view 0 "learnt"
// the focus size 2028x2004, then the focus views' tone maps were rejected as "size not the view's": a grey fovea for
// a frame). Until then a tone map's view is kViewUnknown: swapped all the same, nothing learnt.
bool g_cpSynced = false;
// The colour pass's view for a boundary: 0..3, cpass::kViewUnknown (frame not synced yet), -1: not an HMD view (a
// pass begin after the 4th CopyToHMDPass of a synced frame: left alone, as before R46).
int ColorView(bool synced, int hmdOrd, bool begin) {
  if (!synced) return cpass::kViewUnknown;
  if (hmdOrd >= 0 && hmdOrd <= 3) return hmdOrd;
  return begin ? -1 : cpass::kViewUnknown;  // an end still closes what its begin opened
}

// R51 VrFx: the inputs of HMD view v's tone map (render thread, at its ToneMappingPass begin): the view's depth SRV
// and depth viewport; at view 0 the frame's gaze (GazeOut, newest) and FixationDepth's newest published record.
void FxInputs(int v) {
  if (v < 0 || v > 3) return;
  cpass::FxViewIn& vi = cpass::g_fxView[v];
  vi = cpass::FxViewIn{};
  if (g_fxRunValid[v] && g_fxRun[v].variant == 0 && g_fxRun[v].tex) {  // Texture2D depth only (the PS's fxDepth)
    if (const SrvEntry* e = FindSrv(g_fxRun[v])) {
      vi.depth = e->srv;
      vi.depthTex = g_fxRun[v].tex;
      const D3D11_VIEWPORT& p = g_fxRun[v].vp;
      vi.dv[0] = p.TopLeftX, vi.dv[1] = p.TopLeftY, vi.dv[2] = p.Width, vi.dv[3] = p.Height;
      vi.valid = vi.depth != nullptr && p.Width > 0 && p.Height > 0;
    }
  }
  if (v != 0) return;
  static uint64_t lastGazeSeq = 0, gazeSeqFrame = 0, lastDepthSeq = 0, depthSeqFrame = 0;
  const uint64_t frame = g_s.frame;
  cpass::FxFrameIn& in = cpass::g_fxIn;
  in = cpass::FxFrameIn{};
  GazeOut g{};
  if (void* gv = g_gazeView.load()) {
    if (SeqRead(gv, g)) {
      if (g.seq != lastGazeSeq) lastGazeSeq = g.seq, gazeSeqFrame = frame;
      in.gazeValid = (g.flags & kGazeValid) && frame - gazeSeqFrame <= 30;
      in.saccade = (g.flags & kGazeSaccade) != 0;
      const uint32_t n = std::min<uint32_t>(g.viewCount, 4);
      for (uint32_t i = 0; i < n; ++i) in.gaze[i][0] = g.gazeTan[i][0], in.gaze[i][1] = g.gazeTan[i][1];
      if (n == 2) in.gaze[2][0] = in.gaze[0][0], in.gaze[2][1] = in.gaze[0][1];  // two views: the left eye's
    }
  }
  DepthIn d{};
  if (void* dv = g_depthView.load()) {
    if (SeqRead(dv, d)) {
      if (d.seq != lastDepthSeq) lastDepthSeq = d.seq, depthSeqFrame = frame;
      in.depthValid = (d.flags & kDepthValid) && d.medianM > 0 && frame - depthSeqFrame <= 10;
      in.medianM = d.medianM;
      in.conf = vrfx::HistConf(d.histogram);
    }
  }
}

void ColorBoundary(void* pass, bool begin, int k) {
  State& s = g_s;
  if (!s.ctx) return;
  const int v = ColorView(g_cpSynced, s.hmdOrd, begin);
  if (v < 0) return;
  if (k == plod::kHmd) {
    if (begin) {
      if (v <= 3 && cpass::Wanted() && cpass::NeedLearn() && g_cpLearnView < 0) {
        if (!g_armTable) g_cpLearnArmOwn = ArmOm(true);
        ArmDraws();
        if (g_drawArmed)
          g_cpLearnView = v;
        else
          ProbeDisarm(g_cpLearnArmOwn), g_cpArmFail++;
      }
    } else {
      g_cpLearnView = -1;
      ProbeDisarm(g_cpLearnArmOwn);
      cpass::HmdEnd(v);
    }
    return;
  }
  if (k != plod::kOther) return;
  char n[48];
  PassName(pass, n, sizeof(n));
  if (strcmp(n, "ToneMappingPass") != 0) return;
  if (!begin) {
    g_cpView = -1;
    if (g_cpTpAdded && g_armTable) DisarmTpExtended(g_armTable);  // R47
    g_cpTpAdded = false;
    ProbeDisarm(g_cpArmOwn);
    cpass::TonePassEnd();  // R46: a miss when a mode was on and this view's tone map was not swapped
    return;
  }
  if (!cpass::Wanted()) return;
  cpass::NoteDevice(s.dev);
  if (g_cpView >= 0) return;
  if (vrfx::Wanted()) FxInputs(v);  // R51
  cpass::TonePassBegin(v);
  if (!g_armTable) g_cpArmOwn = ArmOm(true);
  // R47: this arm has no upload entries, so nothing put a table still in d3d11's "after flush" state (a Flush or a
  // submit since the last call through it, e.g. between two views) back to normal: the first upload / copy of the
  // pass (DCS's cbTonemapParams update) then wrote d3d11's normal draw entries over ours and the tone-map draw went
  // past the hook (live: 1-25 such misses per 10 s in steady Enhanced, none rejected). One private 16-byte upload
  // through the current entry normalizes it (a no-op on a normal table) and teaches UsToggled the normal entry.
  if ((cpass::g_armRepair.load(std::memory_order_relaxed) & 1) && g_armTable && !g_drawArmed && !ForeignTableHooks() &&
      UsSlots() &&
      GetCurrentThreadId() == ptiming::g_topTid.load(std::memory_order_relaxed))
    NormalizeTable(g_armTable);
  ArmDraws();
  if (g_drawArmed) {
    g_cpView = v;
    // R47: the live miss (21:09:11, one focus view, no reject): a state call between this begin and the tone-map draw
    // made d3d11 submit and write its "after flush" draw entries over ours, so DCS's draw went past the hook. The
    // overlay passes' state-call toggle points (PSSetShaderResources, PSSetShader, ...) re-arm right after such a call.
    if (!g_tpAddedExt && g_armTable && (cpass::g_armRepair.load(std::memory_order_relaxed) & 2)) {
      ArmTpExtendedD3d11Only(g_armTable);
      g_cpTpAdded = g_tpAddedExt;
    }
  } else {
    ProbeDisarm(g_cpArmOwn), g_cpArmFail++;
  }
  cpass::TonePassArmed(g_drawArmed);
}
// CopyToHMDPass's first draw of view g_cpLearnView: the size of its PS SRV 0 (the view's tone-mapped image).
void ColorLearnDraw() {
  const int v = g_cpLearnView;
  g_cpLearnView = -1;
  // R95 (Dev's open item: Quad Views' swapchain format): the view's CopyToHMDPass target (DCS's swapchain image) as
  // bound, logged at each change of its view / resource format.
  {
    ID3D11RenderTargetView* rtv = nullptr;
    g_s.ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv) {
      D3D11_RENDER_TARGET_VIEW_DESC rd{};
      rtv->GetDesc(&rd);
      ID3D11Resource* res = nullptr;
      rtv->GetResource(&res);
      D3D11_TEXTURE2D_DESC td{};
      D3D11_RESOURCE_DIMENSION dim{};
      if (res) res->GetType(&dim);
      if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
      static int lastView = -1, lastRes = -1;
      if (static_cast<int>(rd.Format) != lastView || static_cast<int>(td.Format) != lastRes) {
        lastView = static_cast<int>(rd.Format), lastRes = static_cast<int>(td.Format);
        Log("colour: HMD view %d CopyToHMDPass target: view format %d, resource format %d (%ux%u)%s", v, lastView, lastRes,
            td.Width, td.Height,
            rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || rd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                ? ": sRGB view (the swapchain decodes as sRGB)"
                : "");
      }
      if (res) res->Release();
      rtv->Release();
    }
  }
  ID3D11ShaderResourceView* srv = nullptr;
  g_s.ctx->PSGetShaderResources(0, 1, &srv);
  if (!srv) return;
  ID3D11Resource* r = nullptr;
  srv->GetResource(&r);
  srv->Release();
  if (!r) return;
  D3D11_RESOURCE_DIMENSION dim{};
  r->GetType(&dim);
  if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
    D3D11_TEXTURE2D_DESC td{};
    static_cast<ID3D11Texture2D*>(r)->GetDesc(&td);
    cpass::NoteLdr(v, td.Width, td.Height);
  }
  r->Release();
}
int ColorPreSeh(int v) {
  __try {
    return cpass::ToneDrawPre(g_s.ctx, v);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    cpass::g_fault = true;
    return 0;
  }
}
void ColorPostSeh(int v, int act) {
  __try {
    cpass::ToneDrawPost(g_s.ctx, v, act);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    cpass::g_fault = true;
  }
}

// Before a draw DCS issues while the draw entries are ours. Returns 1 when the
// pre-overlay copy is due right after it.
int DrawPre(int kind, UINT count, UINT inst) {
  State& s = g_s;
  if (!s.ctx || (!g_cmp.pending && !g_trOpen && g_hdrLearnView < 0 && g_tpView < 0 && g_spView < 0 && g_cpView < 0 &&
                 g_cpLearnView < 0 && g_fgView < 0 && g_stTmView < 0))
    return 0;
  ID3D11RenderTargetView* rtv = nullptr;
  s.ctx->OMGetRenderTargets(1, &rtv, nullptr);
  ID3D11Resource* res = nullptr;
  if (rtv) {
    rtv->GetResource(&res);
    if (res) res->Release();  // identity: the RTV holds it
  }
  D3D11_VIEWPORT vp{};
  UINT nvp = 1;
  s.ctx->RSGetViewports(&nvp, &vp);
  if (!nvp) vp = D3D11_VIEWPORT{};
  if (g_trOpen) TraceDraw(kind, count, inst, rtv, res, vp, nvp);
  if (g_hdrLearnView >= 2) HdrLearnDraw(vp, nvp);
  if (g_tpView >= 0) ToneProbeDraw(vp);
  if (g_stTmView >= 0) SymTraceToneDraw(vp);  // R94: before the colour pass's pixel shader swap below
  if (g_spView >= 0) SwapProbeDraw(vp);
  if (g_fgView >= 0) FgDraw(vp);  // R71: CockpitFgDump
  if (g_cpLearnView >= 0) ColorLearnDraw();  // R42: the view's image size (CopyToHMDPass source)
  if (const int cpv = g_cpView; cpv >= 0) {  // R42: the tone map's pixel shader swapped for this one draw
    // R47: the state-call toggle points have done their job (the draw reached the hook): out before our own state
    // and query calls (the swap, the timing queries), which must not go through them (a DCS crash at the first
    // timed frame with them armed).
    if (g_cpTpAdded && g_armTable) DisarmTpExtended(g_armTable);
    g_cpTpAdded = false;
    g_cpDrawView = cpv;  // R58: the value tested (the config thread's uninstall may clear g_cpView meanwhile)
    g_cpView = -1;
    g_cpAct = ColorPreSeh(g_cpDrawView);
  }
  int act = 0;
  if (g_cmp.pending) {
    if (!res || res != g_cmp.res) {
      if (++g_cmp.others > 64) {  // not this pass's pattern: give up for this frame
        fdexport::g_cmpGaveUp++;
        CmpFinish();
      }
    } else {
      g_cmp.pending = false;
      g_cmpPassObserved = true;
      D3D11_BOX box{};
      UINT tw = 0, th = 0;
      RtvSize(rtv, res, &tw, &th);
      const bool boxOk = fdexport::PreBox(g_cmp.vp, tw, th, box);
      const bool full = boxOk && fdexport::FullScreenDraw(kind, count, inst, vp, box.right - box.left,
                                                           box.bottom - box.top);
      const int view = g_cmp.view;
      int mode = fdexport::g_preMode[view - 2];
      if (g_cmp.learning) {
        mode = fdexport::Learn(view, full);
        LogCopyPoint(view, mode, kind, count);
      }
      if (full) {
        act = 1;  // copied after the forwarded draw
        g_cmp.rtv = rtv;
        rtv->AddRef();
      } else {
        if (!g_cmp.learning) {
          FarPreCopy(view, rtv, res, g_cmp.vp, 2);  // the target as the viewport point saw it
          fdexport::g_cmpMiss++;
        }
        CmpFinish();
      }
    }
  }
  if (rtv) rtv->Release();
  return act;
}

void DrawPost(int act) {
  if (g_cpDrawView >= 0) {  // R42: DCS's pixel shader back first
    ColorPostSeh(g_cpDrawView, g_cpAct);
    g_cpDrawView = -1;
    g_cpAct = 0;
  }
  if (act == 1 && g_cmp.rtv && g_cmp.view >= 2) {
    const int view = g_cmp.view;
    ID3D11Resource* res = nullptr;
    g_cmp.rtv->GetResource(&res);
    if (res) {
      res->Release();  // the RTV holds it
      if (g_cmp.learning) fdexport::g_learnCopies++;
      FarPreCopy(view, g_cmp.rtv, res, g_cmp.vp, 1);  // this frame's scene, before any overlay
      fdexport::g_cmpCopies++;
    }
    CmpFinish();
  }
  g_lastCall = 3;  // (before the re-arm: a toggle found here came from this draw, repaired before any upload)
  InPassRearm();  // the upload entries too: a draw is also a point where d3d11 may have toggled them
}

int DrawPreSeh(int kind, UINT count, UINT inst) {
  __try {
    return DrawPre(kind, count, inst);
  } __except (FaultNote(GetExceptionInformation(), __LINE__)) {
    g_fault = true;
    return 0;
  }
}
void DrawPostSeh(int act) {
  __try {
    DrawPost(act);
  } __except (FaultNote(GetExceptionInformation(), __LINE__)) {
    g_fault = true;
  }
}
// Cockpit ipd phase 2: true when the draw was issued (split in two) here.
// After a fault inside the split DCS's state goes back exactly (the capture
// FinishSplit restores) and the references are released; a fault after a
// draw was issued counts as drawn (never DCS's draw a second time on top).
void FinishSplitSeh(ckipd::SplitCap* cap) {
  __try {
    ckipd::FinishSplit(*cap);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ckipd::t_lightBusy = false;
  }
}
bool SplitDrawGuarded(ID3D11DeviceContext* c, UINT n, UINT sv) {
  ckipd::SplitCap cap;
  __try {
    return ckipd::SplitDraw(
        c, n, sv, [c, n, sv] { reinterpret_cast<DrawFnT>(g_dOrig[fdexport::kDkDraw])(c, n, sv); }, &cap);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ckipd::g_fault = true;
    FinishSplitSeh(&cap);
    return cap.draws > 0;
  }
}
// Counted in ckipd::g_busy before g_on is read: ckipd::Stop clears g_on and
// drains g_busy before it releases the phase-2 objects SplitDraw binds.
bool SplitDrawSeh(ID3D11DeviceContext* c, UINT n, UINT sv) {
  ckipd::g_busy.fetch_add(1);
  const bool r = ckipd::g_on.load() && ckipd::Active() && SplitDrawGuarded(c, n, sv);
  ckipd::g_busy.fetch_sub(1);
  return r;
}
// The lighting pass end (cockpit ipd phase 2): its outcome, then DCS's bytes
// back into the cPerView buffer the arm forwarded at the true eye (before the
// disarm: the restore goes through the still-armed upload entries, forwarded
// unchanged). The class is always closed and the restore always done, also
// after a fault.
void RestoreAllSeh() {
  __try {
    ckipd::RestoreAll();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ckipd::g_fault = true;
    ckipd::g_restN = 0;
  }
}
void LightEndSeh() {
  ckipd::g_busy.fetch_add(1);
  if (ckipd::g_on.load()) {
    __try {
      ckipd::LightEnd();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ckipd::g_fault = true;
    }
  }
  ckipd::LightClose();
  if (ckipd::g_on.load()) RestoreAllSeh();
  ckipd::g_busy.fetch_sub(1);
}
// Cockpit ipd: a draw of an open overlay pass (draw entries armed only for the
// draw probe, ckipd::OvlProbeDraws). The upload entries must still be ours: a
// table toggle that nothing repaired would have sent the effect uploads before
// this draw past the hooks (counted, then re-armed); the probe of the bound
// constant buffers (ckipd::OvlDrawProbe).
void OvlDrawSeh(ID3D11DeviceContext* c) {
  if (GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed)) return;
  ckipd::g_busy.fetch_add(1);
  if (ckipd::g_on.load() && ckipd::Active()) {
    __try {
      void** vt = g_armTable;
      if (g_usArmed && vt && *reinterpret_cast<void***>(c) == vt &&
          (vt[g_usSlot] != reinterpret_cast<void*>(&UsHook) || vt[g_us1Slot] != reinterpret_cast<void*>(&Us1Hook))) {
        ckipd::g_st.ovlDrawSlotLost++;
        UsRearm();
      }
      ckipd::OvlDrawProbe(c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ckipd::g_fault = true;
    }
  }
  ckipd::g_busy.fetch_sub(1);
}

// Called while an overlay-class pass runs: `rtv` was bound by DCS in that pass
// and `vp` is the first viewport set after it. When it is the colour target of
// a focus view whose depth is waiting this frame, its viewport box is copied
// now (the viewport point), or, in a watched OverlayLDRPass decided "after the
// composite", after the scene composite draw (5b).
void FarPreOverlay(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp) {
  State& s = g_s;
  if (!fdexport::Active() || !fdexport::g_preOn.load(std::memory_order_relaxed) || !rtv || !s.ctx || !s.dev) return;
  ID3D11Resource* res = nullptr;
  rtv->GetResource(&res);
  if (!res) return;
  res->Release();  // identity: DCS keeps it bound
  int view = -1;
  for (int v = 2; v <= 3 && view < 0; ++v) {
    const fdexport::PendingPush& p = fdexport::g_pend[v - 2];
    if (p.on && !p.copied && !fdexport::ViewHdr(v) &&
        (res == fdexport::g_target[v - 2] || (g_overlayLdr && s.hmdOrd == v)))
      view = v;
  }
  if (view < 0) return;
  fdexport::g_pend[view - 2].copied = true;  // the first overlay-class bind of this view's target: copied now or never
  const int mode = fdexport::g_preMode[view - 2];
  if (g_overlayLdr && g_cmp.view == view && g_drawArmed && mode != fdexport::kPreAtViewport) {
    g_cmp.pending = true;
    g_cmp.res = res;
    g_cmp.vp = vp;
    g_cmp.learning = mode == fdexport::kPreLearn;
    if (!g_cmp.learning) return;  // copied after the scene composite draw
  }
  FarPreCopy(view, rtv, res, vp, 0);
}

// 5c. At view `view`'s CopyToHMDPass begin: the post-overlay copy of the
// pre-overlay box to (w, 0) of the same texture, then the push.
void FarPostOverlay(int view) {
  State& s = g_s;
  fdexport::PendingPush& p = fdexport::g_pend[view - 2];
  if (!p.on || !p.postSrc || !p.postDst || !(p.d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY) || !s.ctx ||
      fdexport::g_stopping.load(std::memory_order_relaxed))
    return;
  ID3D11DeviceContext* c = s.ctx;
  const bool check = g_verify.load(std::memory_order_relaxed) != 0;
  Bind before;
  if (check) before = ReadBind(c, s.ctx1);
  const UINT w = p.postBox.right - p.postBox.left;
  c->CopySubresourceRegion(p.postDst, 0, w, 0, 0, p.postSrc, p.postSub, &p.postBox);
  if (check) {
    fdexport::g_stateChecks++;
    if (!(ReadBind(c, s.ctx1) == before)) fdexport::g_stateDiffs++;
  }
  p.d.flags |= DCSVR_DEPTH_PRE_POST_OVERLAY;
  fdexport::g_postCopies++;
}

// 5d. ToneMappingPass's first draw (view g_hdrLearnView): its PS SRV 0 is
// the HDR source. A change voids this frame's HDR colour of the view.
void HdrLearnDraw(const D3D11_VIEWPORT& vp, UINT nvp) {
  State& s = g_s;
  const int view = g_hdrLearnView;
  g_hdrLearnView = -1;
  ID3D11ShaderResourceView* srv = nullptr;
  s.ctx->PSGetShaderResources(0, 1, &srv);
  ID3D11Resource* res = nullptr;
  if (srv) srv->GetResource(&res);
  D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
  if (srv) srv->GetDesc(&sd);
  ID3D11Texture2D* t = nullptr;
  D3D11_TEXTURE2D_DESC td{};
  if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
    t->GetDesc(&td);
    t->Release();
  }
  UINT sub = ~0u;
  if (t && td.SampleDesc.Count == 1) {
    if (sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D)
      sub = D3D11CalcSubresource(sd.Texture2D.MostDetailedMip, 0, td.MipLevels);
    else if (sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY)
      sub = D3D11CalcSubresource(sd.Texture2DArray.MostDetailedMip, sd.Texture2DArray.FirstArraySlice, td.MipLevels);
  }
  HdrSrc& h = g_hdr[view - 2];
  const bool same = res && h.res == res && h.sub == sub && sub != ~0u;
  fdexport::PendingPush& p = fdexport::g_pend[view - 2];
  if (!same) {
    // This frame's HDR copies (if any) came from another resource: voided.
    if (fdexport::ViewHdr(view) && (p.d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY)) {
      p.d.flags &= ~static_cast<uint32_t>(DCSVR_DEPTH_HAS_PRE_OVERLAY | DCSVR_DEPTH_PRE_POST_OVERLAY);
      fdexport::g_hdrMismatch++;
    }
    if (h.res) h.res->Release();
    h = HdrSrc{};
    if (res && sub != ~0u) {
      h.res = res;
      res->AddRef();
      h.sub = sub;
      h.texFmt = td.Format;
      h.viewFmt = sd.Format != DXGI_FORMAT_UNKNOWN ? sd.Format : td.Format;
      const UINT mip = sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D ? sd.Texture2D.MostDetailedMip
                                                                         : sd.Texture2DArray.MostDetailedMip;
      h.texW = std::max(1u, td.Width >> mip);
      h.texH = std::max(1u, td.Height >> mip);
      fdexport::g_hdrLearns++;
    }
  }
  if (h.res) h.vp = nvp ? vp : D3D11_VIEWPORT{0, 0, static_cast<float>(h.texW), static_cast<float>(h.texH), 0, 1};
  if (h.res && (!g_hdrLogged[view - 2] || !same)) {
    g_hdrLogged[view - 2] = true;
    int& n = g_hdrLogN[view - 2];  // R91 (M3)
    if (n < kHdrSrcLogMax) {
      ++n;
      Log("far depth: HDR source of HMD view %d (ToneMappingPass PS SRV0): res %p %ux%u format %u (SRV format %u), "
          "subresource %u, draw viewport %.1f,%.1f %.1fx%.1f%s",
          view, static_cast<void*>(h.res), h.texW, h.texH, static_cast<unsigned>(h.texFmt),
          static_cast<unsigned>(h.viewFmt), h.sub, h.vp.TopLeftX, h.vp.TopLeftY, h.vp.Width, h.vp.Height,
          n == kHdrSrcLogMax ? "; further changes of this view are counted (HDR source learns), not logged" : "");
    }
  }
  if (g_trOpen && g_trView == view) {
    char l[260];
    snprintf(l, sizeof(l), "frame %llu view %d ToneMappingPass PS SRV0: res %p %ux%u format %u (SRV format %u)%s",
             static_cast<unsigned long long>(s.frame), view, static_cast<void*>(res), td.Width, td.Height,
             static_cast<unsigned>(td.Format), static_cast<unsigned>(sd.Format), same ? "" : " (new)");
    fdexport::TraceLine(l);
  }
  if (res) res->Release();
  if (srv) srv->Release();
  if (!g_trOpen && g_cmp.view < 2) DisarmDraws();
}

// 5d. Pre: the HDR source's box of view `view` into (0, 0) of its 2w texture
// (DLSSPass end, or the fallback pass begin).
void FarHdrPre(int view) {
  State& s = g_s;
  fdexport::PendingPush& p = fdexport::g_pend[view - 2];
  if (!fdexport::Active() || !fdexport::g_preOn.load(std::memory_order_relaxed) || !p.on || p.copied || !s.ctx ||
      !s.dev)
    return;
  const HdrSrc& h = g_hdr[view - 2];
  if (!h.res) {
    fdexport::g_hdrNoSource++;
    return;
  }
  p.copied = true;
  if (!fdexport::HdrFloatFormat(h.viewFmt)) {  // 5e: not an HDR buffer; no colour (the fallback follows)
    fdexport::g_hdrBadFormat++;
    return;
  }
  D3D11_BOX box{};
  if (!fdexport::PreBox(h.vp, h.texW, h.texH, box) &&
      !fdexport::PreBox(D3D11_VIEWPORT{0, 0, static_cast<float>(h.texW), static_cast<float>(h.texH), 0, 1}, h.texW,
                        h.texH, box)) {
    fdexport::g_preNoViewport++;
    return;
  }
  const UINT bw = box.right - box.left, bh = box.bottom - box.top;
  if (2 * bw > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
    fdexport::g_postSkipWide++;
    return;
  }
  const int slots = std::clamp(fdexport::g_slots.load(), 1, fdexport::kMaxSlots);
  const int slot = static_cast<int>(s.frame % static_cast<uint64_t>(slots));
  const DXGI_FORMAT dstFmt = fdexport::TypelessOf(h.texFmt);
  ID3D11Texture2D* pre = fdexport::EnsurePreTex(s.dev, view, slot, dstFmt, 2 * bw, bh, slots);  // R91
  if (!pre) {
    fdexport::g_preSkipFormat++;
    return;
  }
  ID3D11DeviceContext* c = s.ctx;
  const bool check = g_verify.load(std::memory_order_relaxed) != 0;
  Bind before;
  if (check) before = ReadBind(c, s.ctx1);
  c->CopySubresourceRegion(pre, 0, 0, 0, 0, h.res, h.sub, &box);
  if (check) {
    fdexport::g_stateChecks++;
    if (!(ReadBind(c, s.ctx1) == before)) fdexport::g_stateDiffs++;
  }
  fdexport::g_hdrPre++;
  fdexport::g_preCopies++;
  p.d.preOverlay = pre;
  p.d.preOverlayFormat = static_cast<uint32_t>(h.viewFmt);
  p.d.preOverlaySize = bw | (bh << 16);
  p.d.flags |= DCSVR_DEPTH_HAS_PRE_OVERLAY;
  p.d.flags &= ~static_cast<uint32_t>(DCSVR_DEPTH_PRE_POST_OVERLAY);
  if (p.postSrc) p.postSrc->Release();
  p.postSrc = h.res;
  h.res->AddRef();
  p.postSub = h.sub;
  p.postBox = box;
  p.postDst = pre;
  if (!fdexport::g_preLogged[view - 2]) {
    fdexport::g_preLogged[view - 2] = true;
    Log("far depth: HDR coverage of HMD view %d: pre at the %s, post at the ToneMappingPass begin: source %ux%u "
        "format %u (SRV format %u) -> box %u,%u %ux%u copied as format %u into %ux%u",
        view, g_dlssSeen[view - 2] ? "DLSSPass end" : "first pass after it (no DLSSPass)", h.texW, h.texH,
        static_cast<unsigned>(h.texFmt), static_cast<unsigned>(h.viewFmt), box.left, box.top, bw, bh,
        static_cast<unsigned>(dstFmt), 2 * bw, bh);
  }
}

// 5d. The HDR-interval pass boundaries of the focus view being rendered.
void HdrBoundary(void* pass, bool begin) {
  State& s = g_s;
  const int v = s.hmdOrd;
  if ((v != 2 && v != 3) || !fdexport::Active()) return;
  char n[48];
  PassName(pass, n, sizeof(n));
  const bool hdr = fdexport::ViewHdr(v);  // this view's copies follow 5d (not fallen back, 5e)
  fdexport::HdrWatch& hw = fdexport::g_hw[v - 2];
  if (!begin) {
    if (strcmp(n, "DLSSPass") == 0) {
      g_dlssSeen[v - 2] = true;
      hw.pre = true;
      if (hdr) FarHdrPre(v);
    } else if (strcmp(n, "ToneMappingPass") == 0 && (g_hdrLearnView >= 0 || g_hdrArmOwn)) {
      g_hdrLearnView = -1;
      if (g_hdrArmOwn && !g_trOpen && !g_overlayArm) DisarmOm();
      g_hdrArmOwn = false;
    }
    return;
  }
  const bool preClass = strcmp(n, "OverlayHDRPass") == 0 || strcmp(n, "HistogramPass") == 0;
  if (preClass) hw.pre = true;
  if (hdr && !g_dlssSeen[v - 2] && !fdexport::g_pend[v - 2].copied && preClass) {
    const uint64_t n0 = fdexport::g_hdrPre.load();
    FarHdrPre(v);
    if (fdexport::g_hdrPre.load() != n0) fdexport::g_hdrFallback++;
  }
  if (strcmp(n, "ToneMappingPass") != 0) return;
  if (hw.pre) hw.tone = true;
  // Post: the same box right before ToneMappingPass reads it.
  fdexport::PendingPush& p = fdexport::g_pend[v - 2];
  if (hdr && p.on && p.postSrc) {
    FarPostOverlay(v);
    if (p.d.flags & DCSVR_DEPTH_PRE_POST_OVERLAY) fdexport::g_hdrPost++;
    p.postSrc->Release();
    p.postSrc = nullptr;
  }
  // Learn / re-check the source at this pass's first draw (hdr mode, also
  // while fallen back so the source can return (5e), or the trace).
  if ((fdexport::HdrMode() && (fdexport::g_mask & (1u << v))) || g_trOpen) {
    if (!g_armTable) g_hdrArmOwn = ArmOm(true);  // (a forward cockpit pass's arm is never taken as ours)
    ArmDraws();
    if (g_drawArmed) g_hdrLearnView = v;
  }
}

// 5e. At the frame end (after fdexport::EndFrame: nothing waits to be
// pushed): each focus view that was wanted and rendered this frame had a
// usable HDR pair or not; a sustained miss switches it to the overlay-mode
// copies, a sustained pair back to hdr.
void HdrWatchFrame() {
  const bool eval = fdexport::Active() && fdexport::HdrMode() && fdexport::g_preOn.load(std::memory_order_relaxed);
  static const char* const kWhy[4] = {"no ToneMappingPass source learnt",
                                      "no DLSSPass / OverlayHDRPass / HistogramPass pre point",
                                      "no ToneMappingPass after the pre point", "source format not a float HDR format"};
  for (int v = 2; v <= 3; ++v) {
    fdexport::HdrWatch& w = fdexport::g_hw[v - 2];
    const bool counted = eval && w.rendered && (fdexport::g_mask & (1u << v));
    const bool pre = w.pre, tone = w.tone;
    w.rendered = w.pre = w.tone = false;
    if (!counted) continue;
    if (w.fb) fdexport::g_hdrOverlayFrames++;
    const HdrSrc& h = g_hdr[v - 2];
    const int why = !h.res ? 0 : !pre ? 1 : !tone ? 2 : !fdexport::HdrFloatFormat(h.viewFmt) ? 3 : -1;
    if (why >= 0) {
      fdexport::g_hdrNoPair[why]++;
      w.ok = 0;
      if (w.fb || ++w.miss < fdexport::kHdrMissFallback) continue;
      w.fb = true;
      w.miss = 0;
      fdexport::ReleasePreView(v);
      fdexport::g_preLogged[v - 2] = false;
      fdexport::g_hdrToOverlay++;
      if (!w.fbLogged) {
        w.fbLogged = true;
        Log("far depth: HMD view %d: no usable HDR pair for %d frames (%s, format %u): overlay-mode coverage copies "
            "for this view until the HDR source returns",
            v, fdexport::kHdrMissFallback, kWhy[why], static_cast<unsigned>(h.viewFmt));
      }
    } else {
      w.miss = 0;
      if (!w.fb || ++w.ok < fdexport::kHdrOkReturn) continue;
      w.fb = false;
      w.ok = 0;
      fdexport::ReleasePreView(v);
      fdexport::g_preLogged[v - 2] = false;
      fdexport::g_hdrToHdr++;
      if (!w.backLogged) {
        w.backLogged = true;
        Log("far depth: HMD view %d: HDR pair back for %d frames (source format %u): hdr coverage again", v,
            fdexport::kHdrOkReturn, static_cast<unsigned>(h.viewFmt));
      }
    }
  }
}

void FarPreOverlaySeh(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp) {
  __try {
    FarPreOverlay(rtv, vp);
  } __except (FaultNote(GetExceptionInformation(), __LINE__)) {
    g_fault = true;
  }
}

// Overlay-class passes (far_depth.h 5): armed only while a focus view's
// depth is waiting for its pre-overlay colour.
bool IsOverlayPass(void* pass, bool* ldr) {
  char n[48];
  PassName(pass, n, sizeof(n));
  *ldr = strcmp(n, "OverlayLDRPass") == 0;
  return *ldr || strcmp(n, "GUIPass") == 0;
}
bool PreOverlayWaiting() {
  if (!fdexport::Active() || !fdexport::g_preOn.load(std::memory_order_relaxed)) return false;
  for (int v = 2; v <= 3; ++v) {  // views in hdr mode (5d) take their colour elsewhere
    const fdexport::PendingPush& p = fdexport::g_pend[v - 2];
    if (p.on && !p.copied && !fdexport::ViewHdr(v)) return true;
  }
  return false;
}
void OverlayPassBoundary(void* pass, bool begin) {
  if (begin) {
    bool ldr = false;
    if (g_overlayArm || !PreOverlayWaiting() || !IsOverlayPass(pass, &ldr)) return;
    ArmOm(true);  // (already armed by the overlay trace window: kept)
    // No context yet, the slots are unknown, or a forward cockpit pass holds
    // its own arm (no capture in it; not expected around an overlay pass).
    if (!g_armTable || g_usOnlyArm) return;
    g_overlayArm = true;
    g_overlayLdr = ldr;
    g_ovRtv = nullptr;
    fdexport::g_preArms++;
    // 5b: the view's OverlayLDRPass, watched for its scene composite while
    // its copy point is learnt or decided "after the composite".
    const int v = g_s.hmdOrd;
    if (ldr && (v == 2 || v == 3) && fdexport::g_pend[v - 2].on && !fdexport::g_pend[v - 2].copied &&
        !fdexport::ViewHdr(v) && fdexport::g_preMode[v - 2] != fdexport::kPreAtViewport) {
      ArmDraws();
      if (g_drawArmed) {
        g_cmp = Cmp{};
        g_cmp.view = v;
        g_cmpPassView = v;
        g_cmpPassObserved = false;
      }
    }
    return;
  }
  if (!g_overlayArm) return;
  bool ldr = false;
  if (!IsOverlayPass(pass, &ldr)) return;  // a nested pass
  if (g_cmpPassView >= 2) {
    const int v = g_cmpPassView;
    // Decided "after the composite" and no draw into the target came: the
    // depth goes without its pre-overlay colour (counted).
    if (g_cmp.view == v && g_cmp.pending && !g_cmp.learning) fdexport::g_cmpNoDraw++;
    if (!g_cmpPassObserved && fdexport::g_preMode[v - 2] == fdexport::kPreLearn) {
      const int m = fdexport::LearnEmpty(v);
      if (m != fdexport::kPreLearn) LogCopyPoint(v, m, 0, 0);
    }
    CmpFinish();
    g_cmpPassView = -1;
  }
  if (!g_trOpen) DisarmOm();  // the trace window keeps its arm to its own end
  g_overlayArm = false;
  for (int v = 2; v <= 3; ++v)  // with a post copy due: pushed after it, at the view's CopyToHMDPass begin
    if (fdexport::g_pend[v - 2].on && fdexport::g_pend[v - 2].copied && !fdexport::g_pend[v - 2].postSrc)
      fdexport::DoPush(v);
}

// [Perception] FarDepthOverlayTrace (far_depth.h 5b): a focus view's passes
// from ToneMappingPass begin to OverlayLDRPass end, with the draw entries
// redirected (only while tracing).
bool TraceIsTarget(ID3D11Resource* r) { return r && g_trView >= 2 && r == fdexport::g_target[g_trView - 2]; }
void TracePassEnd(const char* name) {
  State& s = g_s;
  ID3D11RenderTargetView* rtv = nullptr;
  s.ctx->OMGetRenderTargets(1, &rtv, nullptr);
  bool rtvT = false;
  if (rtv) {
    ID3D11Resource* r = nullptr;
    rtv->GetResource(&r);
    rtvT = TraceIsTarget(r);
    if (r) r->Release();
    rtv->Release();
  }
  ID3D11UnorderedAccessView* uav[8] = {};
  s.ctx->CSGetUnorderedAccessViews(0, 8, uav);
  int uavT = -1;
  for (int i = 0; i < 8; ++i)
    if (uav[i]) {
      ID3D11Resource* r = nullptr;
      uav[i]->GetResource(&r);
      if (uavT < 0 && TraceIsTarget(r)) uavT = i;
      if (r) r->Release();
      uav[i]->Release();
    }
  char l[300];
  snprintf(l, sizeof(l),
           "frame %llu view %d %s end: draws %d, into the pre-overlay target %d; left bound: RTV0 %s, CS UAV %s",
           static_cast<unsigned long long>(s.frame), g_trView, name, g_trDraws, g_trTargetDraws,
           rtvT ? "= target" : "other/none", uavT >= 0 ? "= target" : "other/none");
  fdexport::TraceLine(l);
}
void TraceClose() {
  if (!g_trOpen) return;
  g_trOpen = false;
  g_trView = -1;
  if (!g_overlayArm)
    DisarmOm();
  else if (g_cmp.view < 2)
    DisarmDraws();
}
void TraceOpen(int v, const char* where) {
  State& s = g_s;
  g_trOpen = true;
  g_trView = v;
  fdexport::g_tracedThisFrame = true;
  if (!g_armTable) ArmOm(true);
  ArmDraws();
  // What the DLSS pass left bound (its output, if DCS leaves it).
  ID3D11RenderTargetView* rtv = nullptr;
  s.ctx->OMGetRenderTargets(1, &rtv, nullptr);
  ID3D11Resource* rr = nullptr;
  if (rtv) {
    rtv->GetResource(&rr);
    rtv->Release();
  }
  ID3D11UnorderedAccessView* uav = nullptr;
  s.ctx->CSGetUnorderedAccessViews(0, 1, &uav);
  ID3D11Resource* ur = nullptr;
  if (uav) {
    uav->GetResource(&ur);
    uav->Release();
  }
  ID3D11ShaderResourceView* srv = nullptr;
  s.ctx->CSGetShaderResources(0, 1, &srv);
  ID3D11Resource* sr = nullptr;
  if (srv) {
    srv->GetResource(&sr);
    srv->Release();
  }
  static const char* const kMode[3] = {"learning", "first viewport after the bind", "after the scene composite"};
  char l[420];
  snprintf(l, sizeof(l),
           "frame %llu view %d: window opens at %s; left bound: RTV0 res %p, CS UAV0 res %p, CS SRV0 res %p; HDR "
           "source %p; pre-overlay target %p; LDR copy point %s; coverage mode %s; draw hooks %s",
           static_cast<unsigned long long>(s.frame), v, where, static_cast<void*>(rr), static_cast<void*>(ur),
           static_cast<void*>(sr), static_cast<void*>(g_hdr[v - 2].res), static_cast<void*>(fdexport::g_target[v - 2]),
           kMode[fdexport::g_preMode[v - 2]],
           fdexport::ViewHdr(v) ? "hdr" : fdexport::HdrMode() ? "overlay (hdr fallback)" : "overlay",
           g_drawArmed ? "in" : "NOT in (another module holds the table)");
  if (rr) rr->Release();
  if (ur) ur->Release();
  if (sr) sr->Release();
  fdexport::TraceLine(l);
}
void TraceBoundary(void* pass, bool begin) {
  State& s = g_s;
  char n[48];
  PassName(pass, n, sizeof(n));
  const int v = s.hmdOrd;
  const bool can = !g_trOpen && fdexport::g_ovTraceLeft.load(std::memory_order_relaxed) > 0 && fdexport::Active() &&
                   s.ctx && (v == 2 || v == 3);
  if (begin) {
    if (can && (strcmp(n, "OverlayHDRPass") == 0 || strcmp(n, "HistogramPass") == 0 ||
                strcmp(n, "ToneMappingPass") == 0)) {
      char where[80];
      snprintf(where, sizeof(where), "%s begin (no DLSSPass)", n);
      TraceOpen(v, where);
    }
    if (!g_trOpen) return;
    strncpy_s(g_trPass, n, _TRUNCATE);
    g_trDraws = g_trTargetDraws = 0;
    return;
  }
  if (g_trOpen) {
    TracePassEnd(n);
    return;  // closed at the view's CopyToHMDPass begin
  }
  if (can && strcmp(n, "DLSSPass") == 0) {
    TraceOpen(v, "DLSSPass end");
    strncpy_s(g_trPass, "(after DLSSPass)", _TRUNCATE);
    g_trDraws = g_trTargetDraws = 0;
  }
}

void FlushPending(bool afterWater, bool seaEnd) {
  State& s = g_s;
  if (s.hasFarPending) {
    s.hasFarPending = false;
    FarCapture(s.farPending, s.farIdx, seaEnd);
    ReleaseRun(s.farPending);
  }
  if (s.hasGridPending) {
    s.hasGridPending = false;
    SampleGrid(s.gridPending, afterWater);
    ReleaseRun(s.gridPending);
  }
  if (!s.hasPending) return;
  s.hasPending = false;
  Sample(s.pending, s.pendingIdx, s.pendingW, s.pendingH, s.pendingHmd, afterWater);
  ReleaseRun(s.pending);
}

bool PassIs(void* pass, const char* name) {
  char n[48];
  PassName(pass, n, sizeof(n));
  return strcmp(n, name) == 0;
}

// R46: hmdOrd is stale until the next frame start after an install from the config thread (g_cpSynced false).
// R48: a re-install (Depth boost off and on again while flying) also kept the previous install's hmdOrd and
// lastHmdCount: the rest of that frame's views were numbered from the old count and cockpit ipd (its guard:
// lastHmdCount >= 2, hmdOrd <= 3) took a pass for another HMD view (another eye's shift) for a frame. Now every
// install starts from the counts of the first one (no HMD view known until a frame start).
void UnsyncViewCount() {
  g_cpSynced = false;
  g_s.hmdOrd = 0;
  g_s.lastHmdCount = 0;
}

void EndFrame() {
  State& s = g_s;
  CloseRun();
  FlushPending(s.waterSeen);
  if (!s.sampled && g_coneOn.load()) {
    g_noTarget++;
    QueueInvalid(nullptr, 2, false);
  }
  s.decided = false;
  s.periphery = false;
  s.lastSizes.swap(s.sizes);
  s.sizes.clear();
  s.runsThisFrame = 0;
  s.sampled = false;
  s.lastHmdCount = s.hmdOrd;
  s.hmdOrd = 0;
  g_cpSynced = true;  // R46: hmdOrd counts from a frame start from here on
  s.content = false;
  g_dlssSeen[0] = g_dlssSeen[1] = false;
  if (fdexport::g_on.load(std::memory_order_relaxed)) {
    fdexport::EndFrame();
    HdrWatchFrame();
  }
  if (ckipd::g_on.load(std::memory_order_relaxed)) ckipd::EndFrame();
  if (cmask::g_on.load(std::memory_order_relaxed)) cmask::OnEndFrame();  // R65: a frame xrEndFrame did not commit
  ++s.verifyCounter;
  ++s.frame;
  g_frames++;
  symtr::EndFrame();  // R94: the traced frame's text out, its copies polled, the next frame's role (cheap while idle)
  Poll();
  PollGrid();
  if ((s.frame & 63) == 0) EvictSrvs();
}

// [Perception] FixationDepthVerify=2: one trace of the top-level passes
// (name, begin/end, at a GBufferPass end the bound DSV and viewport), logged
// by Tick.
std::atomic<int> g_traceLeft{0};
std::vector<std::string> g_trace;  // render thread fills, Tick reads after g_traceDone
std::atomic<bool> g_traceDone{false};


void TraceStep(void* pass, bool begin, int depth) {
  State& s = g_s;
  char name[48];
  PassName(pass, name, sizeof(name));
  char line[300];
  const bool gb = plod::KindRaw(pass) == plod::kGBuffer;
  if (begin && depth == 0) {
    g_traceDsvN = 0;
    if (!gb) {
      if (g_passDsv) g_passDsv->Release();
      g_passDsv = nullptr;
      ArmOm();
    }
  }
  char binds[160] = "";
  if (!begin && depth == 0) {
    if (!gb) DisarmOm();
    for (int i = 0; i < g_traceDsvN; ++i) {
      ID3D11Resource* res = nullptr;
      g_traceDsv[i]->GetResource(&res);
      char one[40];
      snprintf(one, sizeof(one), " %p(tex %p)", static_cast<void*>(g_traceDsv[i]), static_cast<void*>(res));
      if (res) res->Release();
      strncat_s(binds, one, _TRUNCATE);
    }
  }
  if (!begin && gb && s.ctx) {
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    s.ctx->OMGetRenderTargets(1, &rtv, &dsv);
    D3D11_VIEWPORT vp{};
    UINT n = 1;
    s.ctx->RSGetViewports(&n, &vp);
    snprintf(line, sizeof(line), "%d end   %s dsv %p rtv %p vp %.0fx%.0f, DSVs bound in the pass:%s", depth, name,
             static_cast<void*>(dsv), static_cast<void*>(rtv), vp.Width, vp.Height, binds);
    if (dsv) dsv->Release();
    if (rtv) rtv->Release();
  } else {
    snprintf(line, sizeof(line), "%d %s %s%s%s", depth, begin ? "begin" : "end  ", name, binds[0] ? " DSVs:" : "",
             binds);
    if (!begin && depth == 0 && !gb && g_passDsv) {
      g_passDsv->Release();
      g_passDsv = nullptr;
    }
  }
  g_trace.emplace_back(line);
  if (g_traceLeft.fetch_sub(1) == 1) g_traceDone = true;
}

bool ArmUsOnly(bool draws);
void DisarmUsOnly();
// The forward cockpit pass's arm (its own, or its share of another feature's)
// still holds both upload entries of the table DCS's context uses at the pass
// end (no d3d11 switch took them after the last re-arm point).
bool UsOnlyArmIntact() {
  void** vt = g_armTable;
  if (!(g_usOnlyArm || g_fwdNested) || !g_usArmed || !vt || !g_s.ctx || *reinterpret_cast<void***>(g_s.ctx) != vt)
    return false;
  return vt[g_usSlot] == reinterpret_cast<void*>(&UsHook) && vt[g_us1Slot] == reinterpret_cast<void*>(&Us1Hook);
}
// Cockpit ipd (cockpit_ipd.h): the forward cockpit passes (ArmUsOnly: their
// own arm or nested in another feature's) and, in phase 2,
// GBufferLightingPass of a patched view (the in-pass arm with the draw
// entries: the OM / viewport calls re-arm them after a d3d11 table switch).
// The begin paths start nothing unless ckipd::Active(); the end paths run
// regardless (also after a fault), so a pass of ours is always closed:
// restore, class, arm. Order at a forward pass end: PassEnd (its restore
// uploads go through the still-armed hooks, forwarded unchanged), then the
// arm; this runs before far depth's boundary work for the same boundary
// (OnBoundaryInner), so a far depth arm ending at the same boundary finds
// the forward share already gone.
void CockpitIpdBoundary(void* pass, bool begin, int k) {
  ckipd::CostScope cost(ckipd::kCostBoundary);
  // Another pass's boundary inside an open forward cockpit pass: one more
  // re-arm point (never at the pass's own end: its intact check must see a loss).
  if ((g_usOnlyArm || g_fwdNested) && ckipd::PassOpen() && !ckipd::PassOwns(pass)) UsRearm();
  if (k != plod::kOther) return;
  // R32 (ovl_cpu.h): which overlay pass of which HMD view is open (the record replay's view); at its end the
  // backstop restore of DCS's camera bytes.
  if (ckipd::ocpu::Installed()) ckipd::ocpu::PassBoundary(pass, begin, g_s.hmdOrd);
  if (begin) {
    if (ckipd::PassBegin(pass)) {
      ckipd::PassArmed(ArmUsOnly(false));
    } else if (PassIs(pass, "GBufferLightingPass") && ckipd::LightBegin(pass)) {
      bool ok = false;
      if (!g_armTable) {
        g_tpCk = true;  // cockpit ipd's lighting arm: R32's Unmap point too
        ArmOm();
        g_tpCk = false;
        g_ckLightArm = g_armTable != nullptr;
        ok = g_armTable && g_usArmed;
        if (ok) ArmDraws();
        ok = ok && g_drawArmed;
        if (!ok) DisarmOm();
      }
      if (!ok) ckipd::LightArmFailed();
    }
  } else if (ckipd::PassOwns(pass)) {
    ckipd::PassEnd(pass, UsOnlyArmIntact());
    DisarmUsOnly();
  } else if (ckipd::LightOwns(pass)) {
    LightEndSeh();  // before the disarm: a repair draw goes through the still-armed draw entries
    DisarmOm();
  }
}
// Cockpit ipd's overlay passes (OverlayHDRPass, OverlayLDRPass, GUIPass of an
// HMD view): begun after far depth's own begin work for the same boundary, so
// far depth's overlay / trace arm (ArmOm, no upload entries) is already held
// and ours nests in it (ArmUsOnly adds the upload entries and removes them at
// our end, which comes first at the shared end boundary: CockpitIpdBoundary
// runs before far depth's end work). With no arm held, its own arm.
// The draw entries and the state-call toggle points (kTpBasic..: IASetVertexBuffers, PSSetShaderResources, ...)
// are armed in every overlay pass of a moved view (live 2026-10-13: a draw or state call that makes d3d11
// submit its command buffer leaves the table in the "after flush" state, and the next effect upload went past
// the hook: one frame of HUD / MFD symbols at the boosted eye, every few seconds). Each of these hooks puts the
// upload entries back right after the call (DrawPost -> InPassRearm, AfterToggle). The
// draw probe (OvlDrawProbe: the diagnostics of the bound constant buffers) runs for a budget of passes only.
void CockpitIpdOverlayBegin(void* pass) {
  ckipd::CostScope cost(ckipd::kCostBoundary);
  if (!ckipd::OvlBegin(pass)) return;
  const bool ovl = ckipd::t_cls == ckipd::kClsOvl;
  const bool own = !g_armTable;
  // The cpu path (R32): the arm only observes the effect uploads (the learner's samples): the upload entries and
  // the basic toggle points; no draw entries, no state-call toggle points (no image depends on them).
  const bool full = ovl && !ckipd::PathCpu();
  const bool ok = ArmUsOnly(full && own);
  ckipd::PassArmed(ok);
  if (!full) {
    if (ok && g_fwdNested && ovl) ckipd::g_st.ovlNested++;
    return;
  }
  if (ok && ovl && !own && g_fwdNested && !g_drawArmed) {  // nested in far depth's arm: its draw entries too
    ArmDraws();
    g_ovlAddedDraws = g_drawArmed;
  }
  if (ok && ovl && !own && g_fwdNested && g_armTable) ArmTpExtendedD3d11Only(g_armTable);  // its state-call points (R49: d3d11's own only)
  if (ok && ovl && g_drawArmed && ckipd::OvlProbeDraws()) ckipd::OvlProbePassArmed();
  if (ok && g_fwdNested && ovl) ckipd::g_st.ovlNested++;
}

// [Perception] CockpitTrueIpdFrameDump: an HMD view's OverlayLDRPass / GUIPass while a dump captures that view.
// Its render target binds are the view's own image (between passes DCS has its default frame buffer bound, the
// desktop mirror, the same for every view). Begun after far depth's begin work and before cockpit ipd's overlay
// begin for the same boundary: an arm already held (far depth's) is used as it is, else the dump takes its own
// (OM / viewport entries only, as far depth's overlay arm), and cockpit ipd's overlay arm nests in it as it nests
// in far depth's. Ended after cockpit ipd's and far depth's end work (their shares are gone by then).
void DumpPassBoundary(void* pass, bool begin) {
  State& s = g_s;
  if (begin) {
    if (g_dumpPass) return;  // (nested: the outer watched pass keeps it)
    char n[48];
    PassName(pass, n, sizeof(n));
    if (!ckipd::DumpPassWanted(s.hmdOrd, n)) return;
    ckipd::DumpPassOpen(s.hmdOrd);
    g_dumpPass = true;
    if (!g_armTable) g_dumpArm = ArmOm(true);
    return;
  }
  if (!g_dumpPass) return;
  char n[48];
  PassName(pass, n, sizeof(n));
  if (strcmp(n, "OverlayLDRPass") != 0 && strcmp(n, "GUIPass") != 0) return;  // a nested pass
  ckipd::DumpPassClose();
  g_dumpPass = false;
  if (g_dumpArm) {
    g_dumpArm = false;
    DisarmOm();
  }
}

void OnBoundaryInner(void* pass, bool begin) {
  State& s = g_s;
  const int k = plod::KindRaw(pass);
  // R74b: mask v1.1 overlay symbology, an HMD view's OverlayHDRPass begin: its pre signature and its arm, before any
  // other work of this boundary (no arm held yet; far depth's and cockpit ipd's overlay arms nest in ours).
  if (begin && k == plod::kOther && cmask::covl::Wanted()) cmask::covl::Boundary(pass, true, s.hmdOrd);
  // R95: the symbology export's view windows (OverlayHDRPass begin, before any arm of this boundary: its constant
  // uploads), its pass watch until the tone map, the overlay pass's end (an open run closes).
  if (k == plod::kOther && cmask::SymWanted() && cmask::Active()) cmask::sym::Boundary(pass, begin, s.hmdOrd);
  if (ProbesWanted()) ProbeBoundary(pass, begin, k);  // R41 live probes (dev, measure only)
  if (symtr::Busy() || g_stTmView >= 0 || g_stArmOwn) SymTraceBoundary(pass, begin, k);  // R94 (dev, measure only)
  if (cpass::Wanted() || g_cpView >= 0 || g_cpLearnView >= 0 || g_cpArmOwn || g_cpLearnArmOwn || cpass::g_tp.open)
    ColorBoundary(pass, begin, k);  // R42 colour modes
  if (ckipd::g_dumpActive && k == plod::kOther && s.hmdOrd >= 0 && s.hmdOrd <= 3) {  // CockpitTrueIpdFrameDump
    char pn[48];
    PassName(pass, pn, sizeof(pn));
    ckipd::DumpNotePass(s.hmdOrd, pn, begin);
  }
  if (ckipd::g_on.load(std::memory_order_relaxed)) CockpitIpdBoundary(pass, begin, k);
  // [Perception] CockpitTrueIpdFrameDump: the views' final image at their CopyToHMDPass begin.
  if (begin && k == plod::kHmd && ckipd::g_on.load(std::memory_order_relaxed) && ckipd::Active())
    ckipd::DumpAtHmd(s.hmdOrd);
  // Far depth export: Quad Views' wanted views, once per frame at its first boundary.
  if (fdexport::g_on.load(std::memory_order_relaxed)) {
    fdexport::DecideFrame();
    if (k == plod::kOther) {
      if (begin) TraceBoundary(pass, true);
      HdrBoundary(pass, begin);
      OverlayPassBoundary(pass, begin);
      if (!begin) TraceBoundary(pass, false);
    } else if (begin && g_trOpen && (k == plod::kGBuffer || k == plod::kHmd)) {
      TraceClose();  // the window's OverlayLDRPass end did not come
    }
    // 5c: after the view's last overlay pass, before CopyToHMDPass: the
    // post-overlay copy, then the push (same frame, before xrEndFrame).
    if (begin && k == plod::kHmd && (s.hmdOrd == 2 || s.hmdOrd == 3) && fdexport::g_pend[s.hmdOrd - 2].postSrc) {
      fdexport::PendingPush& p = fdexport::g_pend[s.hmdOrd - 2];
      if (fdexport::ViewHdr(s.hmdOrd)) {  // 5d: no ToneMappingPass came after the pre copy: no colour this frame
        p.d.flags &= ~static_cast<uint32_t>(DCSVR_DEPTH_HAS_PRE_OVERLAY | DCSVR_DEPTH_PRE_POST_OVERLAY);
        p.postSrc->Release();
        p.postSrc = nullptr;
        fdexport::g_hdrNoPost++;
      } else {
        FarPostOverlay(s.hmdOrd);
      }
      fdexport::DoPush(s.hmdOrd);
    }
  }
  // R74b: the OverlayHDRPass end, after cockpit ipd's and far depth's end work: post signature and the 128 texels.
  if (!begin && k == plod::kOther && (g_cmOvlArm || cmask::covl::g_passView >= 0)) cmask::covl::Boundary(pass, false, s.hmdOrd);
  // [Perception] CockpitTrueIpdFrameDump: the views' overlay pass render targets (their own images).
  if (k == plod::kOther &&
      (g_dumpPass || (begin && (ckipd::g_dumpActive || ckipd::g_dumpReq.load(std::memory_order_relaxed) > 0) &&
                      ckipd::g_on.load(std::memory_order_relaxed) && ckipd::Active())))
    DumpPassBoundary(pass, begin);
  if (begin && k == plod::kOther && ckipd::g_on.load(std::memory_order_relaxed)) CockpitIpdOverlayBegin(pass);
  if (s.hasPending || s.hasGridPending || s.hasFarPending || cmask::HasPending()) {
    // The water passes of the target view: depth with water after WaterSeaPass.
    if (!begin && (PassIs(pass, "WaterSeaPass") || PassIs(pass, "RiversPass"))) {
      s.waterSeen = true;
      if (PassIs(pass, "WaterSeaPass")) {
        cmask::Flush(0);  // R65
        FlushPending(true, true);
      }
    } else if (begin && (k == plod::kGBuffer || k == plod::kLights || k == plod::kHmd || k == plod::kCascade ||
                         k == plod::kHeight || PassIs(pass, "GBufferLightingPass"))) {
      if (cmask::HasPending()) cmask::Flush(PassIs(pass, "GBufferLightingPass") ? 1 : 3);  // R65
      FlushPending(s.waterSeen);
    } else if (begin && k == plod::kOther && cmask::HasPending()) {
      // R65: never after a transparent pass of the view (DCS's transparent cockpit materials write COCKPIT into the
      // stencil: the canopy glass would become cockpit).
      char pn[48];
      PassName(pass, pn, sizeof(pn));
      if (strstr(pn, "Transparent")) cmask::Flush(2);
    }
  }
  if (begin) {
    if (k == plod::kGBuffer) {
      cmask::covl::Abort();  // R74b: an OverlayHDRPass whose end did not come
      cmask::sym::Abort();   // R95: the same for the symbology's runs
      if (g_hdrArmOwn || g_hdrLearnView >= 0) {  // a ToneMappingPass whose end did not come
        g_hdrLearnView = -1;
        if (g_hdrArmOwn && !g_overlayArm && !g_trOpen) DisarmOm();
        g_hdrArmOwn = false;
      }
      if (g_overlayArm) {  // an overlay pass whose end did not come
        CmpFinish();
        g_cmpPassView = -1;
        g_trOpen = false;
        DisarmOm();
        g_overlayArm = false;
      }
      if (g_cpView >= 0 || g_cpLearnView >= 0 || g_cpArmOwn || g_cpLearnArmOwn) {  // R42: a pass whose end did not come
        if (g_cpTpAdded && g_armTable) DisarmTpExtended(g_armTable);  // R47
        g_cpTpAdded = false;
        g_cpView = g_cpLearnView = -1;
        if ((g_cpArmOwn || g_cpLearnArmOwn) && !g_overlayArm && !g_trOpen && !g_hdrArmOwn) DisarmOm();
        g_cpArmOwn = g_cpLearnArmOwn = false;
      }
      if (g_dumpPass || g_dumpArm) {  // a frame dump's watched pass whose end did not come
        ckipd::DumpPassClose();
        g_dumpPass = false;
        if (g_dumpArm) DisarmOm();
        g_dumpArm = false;
      }
      if (g_passDsv) g_passDsv->Release();
      g_passDsv = nullptr;
      if (g_passMv) g_passMv->Release();  // R70
      g_passMv = nullptr;
      g_seqN = 0;
      g_gbBegun = true;  // R69
      g_beginDsv = nullptr;
      if (s.ctx) {
        s.ctx->OMGetRenderTargets(0, nullptr, &g_beginDsv);
        if (g_beginDsv) g_beginDsv->Release();  // identity only: DCS keeps it bound
      }
      fdexport::BeginGBufferPass();
      ArmOm();
      return;
    }
    if (s.run.open) CloseRun();
    if ((k == plod::kCascade || k == plod::kHeight) && s.content) EndFrame();
    return;
  }
  if (k == plod::kHmd) {
    // Far depth: the view's last chance before xrEndFrame (no pre-overlay copy came).
    if (fdexport::g_on.load(std::memory_order_relaxed) && (s.hmdOrd == 2 || s.hmdOrd == 3)) {
      fdexport::DoPush(s.hmdOrd);
      fdexport::g_hw[s.hmdOrd - 2].rendered = true;  // 5e: this frame counts for the view
    }
    ++s.hmdOrd;
    s.content = true;
    return;
  }
  if (k != plod::kGBuffer) return;
  DisarmOm();
  if (s.ctx && !g_passDsv && g_gbBegun) PickPassDsv();  // R69: only for a pass whose begin this install saw
  if (!g_gbBegun) g_seqN = 0;
  g_gbBegun = false;
  if (!s.ctx) {
    // DCS's device (shadow instancing or the split filter found it); its
    // immediate context is the one the passes run on.
    ID3D11Device* dev = shadowinst::g_device;
    if (dev)
      dev->AddRef();
    else if (sfilt::Ctx* c = sfilt::g_ctx.load())
      c->GetDevice(&dev);
    else
      dev = pdev::Device();  // [General] PerceptionOnly: neither of them is installed
    if (!dev) return;
    const bool ok = EnsureDevice(dev);
    dev->Release();
    if (!ok) {
      g_fault = true;
      Log("fixation depth: D3D resources could not be created; off");
      return;
    }
  }
  Run nr;
  ID3D11RenderTargetView* mv = g_passMv;  // R70: this pass's MV target (reference), with its DSV
  g_passMv = nullptr;
  if (!CaptureRun(nr)) {
    if (mv) mv->Release();
    return;
  }
  s.content = true;
  if (s.run.open && !SameTarget(s.run, nr.tex, nr.slice, nr.vp)) CloseRun();
  nr.passes = 1;
  if (s.run.open) {
    // Same view: keep the latest pass's state (the main pass after the cockpit).
    nr.passes = s.run.passes + 1;
    ReleaseRun(s.run);
  } else if (g_runMv) {  // R70: a new run; the previous run's MV target goes
    g_runMv->Release();
    g_runMv = nullptr;
  }
  if (mv) {  // R70: the latest pass of the run that had an MV target
    if (g_runMv) g_runMv->Release();
    g_runMv = mv;
  }
  s.run = nr;
}

std::atomic<int> g_busy{0};
std::atomic<bool> g_installed{false};

// Uninstall (config thread): a cockpit ipd pass of ours still open is closed
// on the render thread at its next pass boundary (its restore and its arm),
// so no patched bytes stay in a buffer DCS does not upload again.
std::atomic<bool> g_ckCloseReq{false}, g_ckCloseDone{false};
void CkCloseOnRenderThread() {
  if (ckipd::PassOpen()) {
    if (ckipd::t_cls == ckipd::kClsLight) {
      LightEndSeh();
      DisarmOm();
    } else {
      void* pass = ckipd::t_pass;
      ckipd::PassEnd(pass, UsOnlyArmIntact());
      DisarmUsOnly();
    }
  }
  if (ckipd::g_restN) RestoreAllSeh();
}

// The boundary work (OnBoundary's __try).
void OnBoundaryBody(void* pass, bool begin, int depth) {
  // Far depth export, uninstall: the layer's references and our copies
  // are released here, on the thread Quad Views reads them on.
  if (g_ckCloseReq.load()) {
    CkCloseOnRenderThread();
    g_ckCloseReq = false;
    g_ckCloseDone = true;
  }
  if (fdexport::g_releaseReq.load()) {
    fdexport::ReleaseAll();
    if (g_s.hasFarPending) {
      g_s.hasFarPending = false;
      ReleaseRun(g_s.farPending);
    }
    fdexport::g_releaseReq = false;
    fdexport::g_releaseDone = true;
  }
  if (g_traceLeft.load(std::memory_order_relaxed) > 0 && depth <= 1) TraceStep(pass, begin, depth);
  if (depth == 0 && ckipd::Active()) ckipd::Poll();  // BoostOut / GazeOut records
  OnBoundaryInner(pass, begin);
}

void OnBoundary(void* pass, bool begin, int depth) {
  if (GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed)) return;
  // After a fault only a cockpit ipd pass that is still open is closed (its
  // restore, its class and its arm); everything else stops as before.
  const bool faulted = g_fault.load(std::memory_order_relaxed);
  if (faulted && !(ckipd::g_on.load(std::memory_order_relaxed) && ckipd::PassOpen())) return;
  g_busy.fetch_add(1);
  // A pass that loaded the callback before Uninstall took it out (its end
  // still arrives here): nothing may be created or bound any more.
  if (!g_installed.load()) {
    g_busy.fetch_sub(1);
    return;
  }
  __try {
    if (faulted)
      CockpitIpdBoundary(pass, begin, plod::KindRaw(pass));
    else
      OnBoundaryBody(pass, begin, depth);
  } __except (FaultNote(GetExceptionInformation(), __LINE__)) {
    g_fault = true;
  }
  g_busy.fetch_sub(1);
}
// ---------------------------------------------------------------------------
// Control (config thread)
// ---------------------------------------------------------------------------

bool g_faultLogged = false;
ULONGLONG g_lastLog = 0, g_lastOpen = 0;

void OpenMappings() {
  if (!g_gridMap && g_gridOn.load()) {
    g_gridMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 16384, GridName().c_str());
    if (g_gridMap) {
      void* v = MapViewOfFile(g_gridMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DepthGrid));
      if (!v) {
        CloseHandle(g_gridMap);
        g_gridMap = nullptr;
      } else {
        g_gridView = v;
      }
    }
  }
  if (!g_depthMap && g_coneOn.load()) {
    g_depthMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, DepthName().c_str());
    if (g_depthMap) {
      void* v = MapViewOfFile(g_depthMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DepthIn));
      if (!v) {
        CloseHandle(g_depthMap);
        g_depthMap = nullptr;
      } else {
        g_depthView = v;
      }
    }
  }
  if (!g_gazeMap) {
    const std::wstring gazeName = GazeName();
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, gazeName.c_str());
    bool owned = false;
    if (!h && g_testGaze.load()) {
      h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, gazeName.c_str());
      owned = h != nullptr;
    }
    if (h) {
      void* v = MapViewOfFile(h, owned ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ, 0, 0, sizeof(GazeOut));
      if (v) {
        g_gazeMap = h;
        g_gazeOwned = owned;
        g_gazeView = v;
        Log("fixation depth: %ls %s", gazeName.c_str(),
            owned ? "created by the test gaze writer (no pupil shift layer)" : "opened (pupil shift layer)");
      } else {
        CloseHandle(h);
      }
    }
  }
}

void CloseMappings() {
  if (void* v = g_gridView.exchange(nullptr)) {
    DepthGrid g{};  // a last record: not valid
    SeqWrite(v, g);
    UnmapViewOfFile(v);
  }
  if (g_gridMap) CloseHandle(g_gridMap);
  g_gridMap = nullptr;
  if (void* v = g_depthView.exchange(nullptr)) {
    DepthIn d{};  // a last record: not valid
    SeqWrite(v, d);
    UnmapViewOfFile(v);
  }
  if (g_depthMap) CloseHandle(g_depthMap);
  g_depthMap = nullptr;
  if (void* v = g_gazeView.exchange(nullptr)) UnmapViewOfFile(v);
  if (g_gazeMap) CloseHandle(g_gazeMap);
  g_gazeMap = nullptr;
  g_gazeOwned = false;
  g_testWriterSeq = 0;
  g_testWriterYielded = false;
}

bool g_ckWith = false;  // installed with the cockpit ipd option wanted
bool g_maskWith = false;  // R65: installed with the cockpit mask export wanted
bool g_depthWith = false;  // R69: installed with the cockpit depth companion wanted
bool g_motionWith = false;  // R70: installed with the motion-vector companion wanted
bool g_symWith = false;  // R95: installed with the symbology companion wanted
bool Install() {
  if (g_installed.load()) return true;
  if (!CompileShaders()) {
    static bool logged = false;
    if (!logged) Log("fixation depth: compute shader not compiled (%s)", g_compileError.c_str());
    logged = true;
    return false;
  }
  ptiming::Install();
  if (!ptiming::g_orig) return false;
  g_fault = false;
  g_seqN = 0;          // R69: nothing recorded by a previous install is used (no references held on them)
  if (g_passMv) g_passMv->Release();  // R70
  g_passMv = nullptr;
  if (g_runMv) g_runMv->Release();
  g_runMv = nullptr;
  g_traceDsvN = 0;
  g_gbBegun = false;
  g_faultSite = 0;  // R69: the next fault of this install is reported
  g_faultLogged = false;
  g_workerStop = false;
  fdexport::g_stopping = false;
  fdexport::g_releaseReq = false;
  fdexport::SeedGeneration();
  fdexport::ResetFrameState();
  g_worker = std::thread([] { qvexit::RunVoid(&WorkerLoop); });
  OpenMappings();
  // D3D objects created here when DCS's device is known (CreateComputeShader
  // can take milliseconds in the driver: not on the render thread); else the
  // render thread creates them at the first GBufferPass.
  {
    ID3D11Device* dev = shadowinst::g_device;
    if (dev)
      dev->AddRef();
    else if (sfilt::Ctx* c = sfilt::g_ctx.load())
      c->GetDevice(&dev);
    else
      dev = pdev::Device();  // [General] PerceptionOnly: neither of them is installed
    if (dev) {
      if (!EnsureDevice(dev)) ReleaseD3D();
      dev->Release();
    }
  }
  UnsyncViewCount();  // R46 / R48: the render thread may be mid-frame (before the boundary callback goes in)
  cpass::g_tp.open = false;
  ptiming::g_depthBoundary = &OnBoundary;
  g_rearmOk = true;
  sfilt::g_afterEcl = &AfterEclEntry;
  g_maskWith = cmask::Wanted();
  g_depthWith = cmask::DepthWanted();
  g_motionWith = cmask::MotionWanted();
  g_symWith = cmask::SymWanted();  // R95
  cmask::Install();  // R65: block mapped, xrEndFrame hooked (before the callback goes in: nothing to commit yet)
  g_installed = true;
  g_ckWith = ckipd::Wanted();
  if (g_ckWith) ckipd::Start();
  Log("fixation depth: on (%ls%ls%ls, reads %ls)%s", g_coneOn.load() ? DepthName().c_str() : L"",
      g_coneOn.load() && g_gridOn.load() ? L" and " : L"", g_gridOn.load() ? GridName().c_str() : L"",
      GazeName().c_str(),
      fdexport::g_on.load() ? "; far depth export on (focus views to Quad Views' DcsVr_QuadViewsDepth)" : "");
  return true;
}

// Far depth export at uninstall: the render thread releases the layer's
// references and our copies at its next pass boundary (bounded wait). When it
// does not come (DCS not rendering), ReleaseD3D does it on this thread.
void FarReleaseOnRenderThread() {
  fdexport::g_stopping = true;  // no new copy or push from now on
  if (!fdexport::g_on.load()) return;
  fdexport::g_releaseDone = false;
  fdexport::g_releaseReq = true;
  const ULONGLONG deadline = GetTickCount64() + 300;
  while (!fdexport::g_releaseDone.load() && GetTickCount64() < deadline) Sleep(1);
  fdexport::g_releaseReq = false;
  if (!fdexport::g_releaseDone.load())
    Log("far depth: the render thread did not reach a pass boundary within 300 ms; Quad Views' references are "
        "released from the uninstall");
}

// Callback out and drained, worker joined, D3D objects released, mappings
// closed. Safe to call when not installed. R85: keepExports (an export key
// switched, Tick) keeps the cockpit exports' shared set held (cockpit_mask.h
// top, 11.); false (Shutdown: the payload stop or reload) releases it.
void Uninstall(bool keepExports = true) {
  if (!g_installed.load()) return;
  cmask::UninstallHook();  // R65: xrEndFrame hook out and drained; no new capture or commit
  FarReleaseOnRenderThread();  // while the callback is still in
  if (ckipd::g_on.load()) {
    // A cockpit ipd pass still open is closed on the render thread (restore, arm), bounded like far depth's.
    g_ckCloseDone = false;
    g_ckCloseReq = true;
    const ULONGLONG deadline = GetTickCount64() + 300;
    while (!g_ckCloseDone.load() && GetTickCount64() < deadline) Sleep(1);
    g_ckCloseReq = false;
  }
  if (!g_installed.exchange(false)) return;
  sfilt::g_afterEcl = nullptr;
  ckipd::g_on = false;  // the upload hooks forward DCS's bytes from now on
  ptiming::BoundaryFn me = &OnBoundary;
  ptiming::g_depthBoundary.compare_exchange_strong(me, nullptr);
  for (int i = 0; i < 200 && g_busy.load(); ++i) Sleep(5);
  if (g_busy.load()) Log("fixation depth: the boundary callback was still running at the uninstall (1 s)");
  Sleep(50);  // a callback that loaded the pointer but had not entered yet
  ckipd::Stop();  // gbpass observer out and drained, BoostOut unmapped, phase-2 objects released
  g_ckWith = false;
  {
    std::lock_guard<std::mutex> lock(g_jobMutex);
    g_workerStop = true;
  }
  g_jobCv.notify_all();
  if (g_worker.joinable()) g_worker.join();
  {
    std::lock_guard<std::mutex> lock(g_jobMutex);
    g_jobs.clear();
  }
  // Slots never mapped: nothing outstanding on our side (queries and copies
  // complete on their own). Unmap is never pending: we unmap at once.
  if (g_cmp.rtv) g_cmp.rtv->Release();
  g_cmp = Cmp{};
  g_cmpPassView = -1;
  g_trOpen = false;
  g_hdrArmOwn = false;
  // R58: g_cpDrawView (the tone-map draw in flight) is cleared only after the hooks drained below: cleared here, a
  // draw between its ToneDrawPre and DrawPost skipped the restore (our PS and slots left bound, DCS's refs leaked).
  g_cpView = g_cpLearnView = -1;  // R42
  g_cpTpAdded = false;  // R47 (the entries go with the arm below)
  g_cpSynced = false;  // R46
  cpass::g_tp.open = false;
  g_cpArmOwn = g_cpLearnArmOwn = false;
  // No render-thread re-arm from here on: a hook running now may still re-arm, so wait for it (bounded); a hook
  // entered later only forwards. Then our entries go (draw entries too).
  g_rearmOk = false;
  for (int i = 0; i < 200 && g_hookBusy.load(); ++i) Sleep(1);
  if (g_hookBusy.load()) Log("fixation depth: a context hook was still running at the uninstall (200 ms)");
  g_cpDrawView = -1;  // R58: after the drain (see above)
  g_cpAct = 0;
  g_stTmView = -1;  // R94
  g_stArmOwn = false;
  DisarmOm();  // a pass that began before the callback went out (draw entries too)
  g_rearmOk = true;  // nothing of ours is armed now (g_armTable null): a re-arm needs a new arm first
  if (g_tinyCb) g_tinyCb->Release();
  g_tinyCb = nullptr;
  g_tinyDev = nullptr;
  g_usNormal = g_us1Normal = nullptr;
  HdrRelease();
  g_overlayArm = false;
  if (g_passDsv) g_passDsv->Release();
  g_passDsv = nullptr;
  cmask::Uninstall(keepExports);  // R65: after the callback drained: the block inactive; R85: the shared set held or released
  fdexport::DropFrameModule();  // R91: a frame's Quad Views module reference whose frame end did not come
  if (g_passMv) g_passMv->Release();  // R70: the MV targets of DCS held for the motion export
  g_passMv = nullptr;
  if (g_runMv) g_runMv->Release();
  g_runMv = nullptr;
  g_maskWith = false;
  g_depthWith = false;
  g_motionWith = false;
  g_symWith = false;  // R95
  symtr::Reset();  // R94: its copies and the targets it holds (before the device reference goes)
  ReleaseD3D();
  CloseMappings();
  Log("fixation depth: off");
}

void FarLogStats() {
  double avg = 0, mx = 0;
  {
    std::lock_guard<std::mutex> lock(fdexport::g_statMutex);
    avg = fdexport::g_cpuN ? fdexport::g_cpuUsSum / fdexport::g_cpuN : 0.0;
    mx = fdexport::g_cpuUsMax;
    fdexport::g_cpuUsSum = fdexport::g_cpuUsMax = 0;
    fdexport::g_cpuN = 0;
  }
  auto u = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load()); };
  Log("far depth: frames %llu (wanted %llu, mask 0x%x), copies %llu (after WaterSeaPass %llu, fallback %llu), pushes "
      "%llu accepted %llu, rejected null/size %llu device %llu format %llu MSAA %llu not wanted %llu thread %llu "
      "layout %llu; layer missing %llu, export missing %llu, API version %llu",
      u(fdexport::g_frames), u(fdexport::g_wantedFrames), fdexport::g_lastMask.load(), u(fdexport::g_copies), u(fdexport::g_afterWater), u(fdexport::g_fallback),
      u(fdexport::g_pushes), u(fdexport::g_accepted), u(fdexport::g_rejected[0]), u(fdexport::g_rejected[2]), u(fdexport::g_rejected[3]), u(fdexport::g_rejected[4]),
      u(fdexport::g_rejected[5]), u(fdexport::g_rejected[6]), u(fdexport::g_rejected[7]), u(fdexport::g_noLayer), u(fdexport::g_noExport), u(fdexport::g_badApi));
  Log("far depth: skipped: no CPU projection %llu, arm missed %llu, identity %llu, FOV mismatch %llu, MSAA %llu, "
      "format %llu; cPerView uploads seen %llu; textures created %llu, released %llu (layer release calls %llu), "
      "generation %u, %d slot(s); state checks %llu, differences %llu; render thread %.1f us avg / %.1f max per view",
      u(fdexport::g_skipNoProj), u(fdexport::g_skipArm), u(fdexport::g_skipIdentity), u(fdexport::g_skipMismatch), u(fdexport::g_skipMsaa), u(fdexport::g_skipFormat),
      u(fdexport::g_uploadsSeen), u(fdexport::g_texCreates), u(fdexport::g_releases), u(fdexport::g_releaseCalls), fdexport::g_gen, fdexport::g_slots.load(), u(fdexport::g_stateChecks),
      u(fdexport::g_stateDiffs), avg, mx);
  Log("far depth: in-pass hooks: upload entries switched by d3d11 and re-armed %llu, still lost at a pass end %llu, "
      "OM entries lost %llu; pre-overlay %s: overlay arms %llu, copies %llu, pushed with %llu / without %llu, skipped "
      "MSAA %llu, format %llu, no viewport %llu; dropped at the frame end %llu; post-overlay %s: copies %llu, "
      "pushed without %llu, too wide %llu",
      u(fdexport::g_usRearms), u(fdexport::g_usLost), u(fdexport::g_omLost),
      fdexport::g_preOn.load() ? "on" : "off", u(fdexport::g_preArms), u(fdexport::g_preCopies),
      u(fdexport::g_prePushed), u(fdexport::g_preMissing), u(fdexport::g_preSkipMsaa), u(fdexport::g_preSkipFormat),
      u(fdexport::g_preNoViewport), u(fdexport::g_dropped), fdexport::g_postOn.load() ? "on" : "off",
      u(fdexport::g_postCopies), u(fdexport::g_postMissing), u(fdexport::g_postSkipWide));
  Log("far depth: coverage mode %s; HDR: pre %llu (after no DLSSPass %llu), post %llu, no source yet %llu, source "
      "changed (frame voided) %llu, no ToneMappingPass after the pre %llu, sources learnt %llu, not a float format "
      "%llu; fallback to overlay: view 2 %s, view 3 %s, switches to overlay %llu, back to hdr %llu, view-frames on "
      "overlay %llu; view-frames without a pair: no source %llu, no pre point %llu, no ToneMappingPass after it %llu, "
      "format %llu",
      fdexport::HdrMode() ? "hdr" : "overlay", u(fdexport::g_hdrPre), u(fdexport::g_hdrFallback),
      u(fdexport::g_hdrPost), u(fdexport::g_hdrNoSource), u(fdexport::g_hdrMismatch), u(fdexport::g_hdrNoPost),
      u(fdexport::g_hdrLearns), u(fdexport::g_hdrBadFormat), fdexport::g_hw[0].fb ? "yes" : "no",
      fdexport::g_hw[1].fb ? "yes" : "no", u(fdexport::g_hdrToOverlay), u(fdexport::g_hdrToHdr),
      u(fdexport::g_hdrOverlayFrames), u(fdexport::g_hdrNoPair[0]), u(fdexport::g_hdrNoPair[1]),
      u(fdexport::g_hdrNoPair[2]), u(fdexport::g_hdrNoPair[3]));
  static const char* const kMode[3] = {"learning", "viewport", "after composite"};
  Log("far depth: pre-overlay copy point: view 2 %s, view 3 %s (learning passes with a draw into the target %d/%d, "
      "full-screen first %d/%d); copies after the composite %llu (while learning %llu), before a non-full-screen "
      "first draw %llu, composite expected but no draw into the target %llu, gave up %llu; draw hooks armed %llu, "
      "re-armed %llu, lost %llu, not armed (other module's table hooks) %llu; upload hook not armed (same) %llu",
      kMode[fdexport::g_preMode[0]], kMode[fdexport::g_preMode[1]], fdexport::g_learnSeen[0],
      fdexport::g_learnSeen[1], fdexport::g_learnFull[0], fdexport::g_learnFull[1], u(fdexport::g_cmpCopies),
      u(fdexport::g_learnCopies), u(fdexport::g_cmpMiss), u(fdexport::g_cmpNoDraw), u(fdexport::g_cmpGaveUp),
      u(fdexport::g_drawArms), u(fdexport::g_drawRearms), u(fdexport::g_drawLost), u(fdexport::g_drawSkipForeign),
      u(fdexport::g_usSkipForeign));
}

void LogStats() {
  if (fdexport::g_on.load()) FarLogStats();
  cmask::LogStats();  // R65 (nothing when off)
  {  // the context-table toggle points: the table found toggled right after each
    char b[600] = "";
    for (int k = 0; k < kTpCount; ++k) {
      const size_t n = strlen(b);
      snprintf(b + n, sizeof(b) - n, "%s%s %llu", k ? ", " : "", kTpNames[k], static_cast<unsigned long long>(g_tpHits[k].load()));
    }
    Log("fixation depth: context table found toggled right after: %s; toggle points left out (slot not d3d11's "
        "own entry / held by another hook of ours) %llu",
        b, static_cast<unsigned long long>(g_tpSkipForeign.load()));
    b[0] = 0;
    for (int k = 0; k < 32; ++k)
      if (const uint64_t v = g_togglePrev[k].load()) {
        const size_t n = strlen(b);
        snprintf(b + n, sizeof(b) - n, " [%d] %llu", k, static_cast<unsigned long long>(v));
      }
    Log("fixation depth: ... and the hook passed before it (1 upload, 3 draw, 4 OM, 5 viewport, 10+ point):%s", b);
  }
  if (!g_coneOn.load() && !g_gridOn.load()) return;
  // R91 (R90 phase 2): what g_statMutex guards is copied (and reset) under it, then logged without it: the render
  // thread (Poll's timing) and the worker take it every frame, and Log can stall on the file for ms.
  DepthIn d{};
  std::string note;
  int conv = -1;
  double gridGpuAvg = 0, gridGpuMax = 0, gridCpuAvg = 0, gpuAvg = 0, gpuMax = 0, cpuAvg = 0, cpuMax = 0, latAvg = 0;
  uint64_t gridGpuN = 0, gpuN = 0;
  RankInfo rank[16];
  {
    std::lock_guard<std::mutex> lock(g_statMutex);
    d = g_last;
    note = g_lastNote;
    conv = g_lastConv;
    gridGpuAvg = g_gridUsN ? g_gridUsSum / g_gridUsN : 0.0;
    gridGpuMax = g_gridUsMax;
    gridGpuN = g_gridUsN;
    gridCpuAvg = g_gridCpuN ? g_gridCpuUsSum / g_gridCpuN : 0.0;
    g_gridUsSum = g_gridUsMax = g_gridCpuUsSum = 0;
    g_gridUsN = g_gridCpuN = 0;
    gpuAvg = g_gpuN ? g_gpuUsSum / g_gpuN : 0.0;
    gpuMax = g_gpuUsMax;
    gpuN = g_gpuN;
    cpuAvg = g_cpuN ? g_cpuUsSum / g_cpuN : 0.0;
    cpuMax = g_cpuUsMax;
    latAvg = g_latN ? g_latSum / g_latN : 0.0;
    g_gpuUsSum = g_gpuUsMax = g_cpuUsSum = g_cpuUsMax = g_latSum = 0;
    g_gpuN = g_cpuN = g_latN = 0;
    for (int i = 0; i < 16; ++i) rank[i] = g_rank[i];
  }
  Log("fixation depth: frames %llu, runs %llu, dispatches %llu, published %llu (valid %llu); no gaze %llu, stale "
      "%llu, no target %llu, ring full %llu, map late %llu, no SRV %llu, bad cPerView %llu, bad projection %llu; "
      "view check ok %llu / mismatch %llu / no match %llu (convention %d), retargets %llu, OM unbinds %llu",
      static_cast<unsigned long long>(g_frames.load()), static_cast<unsigned long long>(g_runs.load()),
      static_cast<unsigned long long>(g_dispatches.load()), static_cast<unsigned long long>(g_published.load()),
      static_cast<unsigned long long>(g_publishedValid.load()), static_cast<unsigned long long>(g_noGaze.load()),
      static_cast<unsigned long long>(g_staleGaze.load()), static_cast<unsigned long long>(g_noTarget.load()),
      static_cast<unsigned long long>(g_ringFull.load()), static_cast<unsigned long long>(g_mapLate.load()),
      static_cast<unsigned long long>(g_noSrv.load()), static_cast<unsigned long long>(g_badCb.load()),
      static_cast<unsigned long long>(g_badProj.load()), static_cast<unsigned long long>(g_matchOk.load()),
      static_cast<unsigned long long>(g_mismatch.load()), static_cast<unsigned long long>(g_noMatch.load()),
      conv, static_cast<unsigned long long>(g_retarget.load()),
      static_cast<unsigned long long>(g_omUnbinds.load()));
  if (g_gridOn.load())
    Log("fixation depth: grid dispatches %llu, published %llu (valid %llu), ring full %llu, map late %llu; cost GPU %.1f "
        "us avg / %.1f max (%llu timed), render thread %.1f us avg",
        static_cast<unsigned long long>(g_gridDispatches.load()), static_cast<unsigned long long>(g_gridPublished.load()),
        static_cast<unsigned long long>(g_gridValid.load()), static_cast<unsigned long long>(g_gridRingFull.load()),
        static_cast<unsigned long long>(g_gridLate.load()), gridGpuAvg, gridGpuMax,
        static_cast<unsigned long long>(gridGpuN), gridCpuAvg);
  Log("fixation depth: sampled after the water passes %llu, water in the cone %llu, periphery fallback %llu",
      static_cast<unsigned long long>(g_afterWater.load()), static_cast<unsigned long long>(g_waterFrames.load()),
      static_cast<unsigned long long>(g_periphFrames.load()));
  Log("fixation depth: G-buffer depth targets seen in the pass %llu, passes missed (table rewritten) %llu, not "
      "armed (GPU dedup / D3D meter hooks) %llu; state checks %llu, differences %llu",
      static_cast<unsigned long long>(g_passDsvSeen.load()), static_cast<unsigned long long>(g_armMissed.load()),
      static_cast<unsigned long long>(g_omSkipForeign.load()),
      static_cast<unsigned long long>(g_stateChecks.load()), static_cast<unsigned long long>(g_stateDiffs.load()));
  Log("fixation depth: cost GPU %.1f us avg / %.1f max (%llu timed), render thread %.1f us avg / %.1f max; latency "
      "%.2f frames; sampled runs had %d..%d G-buffer passes",
      gpuAvg, gpuMax, static_cast<unsigned long long>(gpuN), cpuAvg, cpuMax, latAvg, g_passesMin.load(),
      g_passesMax.load());
  Log("fixation depth: readback buffers reused %llu, made %llu (R91)", static_cast<unsigned long long>(g_poolReuses.load()),
      static_cast<unsigned long long>(g_poolNew.load()));
  g_passesMin = 0;
  g_passesMax = 0;
  Log("fixation depth: last flags %u view %u near %.2f far %.2f edge %.2f/%.2f median %.2f m nearest %.2f m far "
      "median %.1f m; %s",
      d.flags, d.view, d.nearShare, d.farShare, d.edgeNearShare, d.edgeFarShare, d.medianM, d.nearestM,
      d.farMedianM, note.c_str());
  if (g_verify.load())
    for (int i = 0; i < 16; ++i) {
      const RankInfo& r = rank[i];
      if (!r.dcsSamples) continue;
      Log("fixation depth verify: run rank %d %dx%d fov L %+.3f R %+.3f U %+.3f D %+.3f -> GazeOut view %d, camera "
          "x %.4f, median %.2f m near %.2f (%d samples)",
          i, r.w, r.h, r.fov[0], r.fov[1], r.fov[2], r.fov[3], r.best, r.camX, r.median, r.nearShare, r.dcsSamples);
    }
}

// Once per second from the config loop.
void Tick(bool coneOn, bool gridOn, int verify, const wchar_t* testGaze, bool dump, const std::wstring& dumpDir,
          bool farOn = false, int farSlots = 2, int farLogProj = 0, bool farPreOverlay = true,
          int farOverlayTrace = 0, bool farPostOverlay = true, int farCoverageMode = 1) {
  const bool ckOn = ckipd::Wanted();  // [Perception] CockpitTrueIpd (ckipd::Configure, before this call)
  const bool maskOn = cmask::Wanted();  // R65: [Perception] CockpitMaskExport (cmask::Configure, before this call)
  const bool depthOn = cmask::DepthWanted();  // R69: [Perception] CockpitDepthExport (cmask::ConfigureDepth)
  const bool motionOn = cmask::MotionWanted();  // R70: [Perception] CockpitMotionExport (cmask::ConfigureMotion)
  const bool symOn = cmask::SymWanted();  // R95: [Perception] SymbologyExport (cmask::ConfigureSymbology)
  const bool on = coneOn || gridOn || farOn || ckOn || maskOn || cpass::Wanted();  // R42: the colour modes use the pass hooks too
  // A change of which outputs are on: reinstall (mappings and state follow).
  if (g_installed.load() &&
      (coneOn != g_coneOn.load() || gridOn != g_gridOn.load() || farOn != fdexport::g_on.load() || ckOn != g_ckWith ||
       maskOn != g_maskWith || depthOn != g_depthWith || motionOn != g_motionWith || symOn != g_symWith))
    Uninstall();
  g_coneOn = coneOn;
  g_gridOn = gridOn;
  fdexport::g_on = farOn;
  fdexport::g_slots = std::clamp(farSlots, 1, fdexport::kMaxSlots);
  fdexport::g_preOn = farPreOverlay;
  fdexport::g_postOn = farPreOverlay && farPostOverlay;
  fdexport::g_covMode = farCoverageMode == fdexport::kCovHdr ? fdexport::kCovHdr : fdexport::kCovOverlay;
  // [Perception] FarDepthLogProj: a new value (re)starts the projection log.
  static int lastLogProj = 0;
  if (farLogProj != lastLogProj) {
    lastLogProj = farLogProj;
    fdexport::g_logProjLeft = farOn ? std::max(0, farLogProj) : 0;
  }
  // [Perception] FarDepthOverlayTrace: a new value (re)starts the overlay trace.
  static int lastOvTrace = 0;
  if (farOverlayTrace != lastOvTrace) {
    lastOvTrace = farOverlayTrace;
    fdexport::g_ovTraceLeft = farOn ? std::clamp(farOverlayTrace, 0, 600) : 0;
  }
  {
    std::vector<std::string> tl;
    {
      std::lock_guard<std::mutex> lock(fdexport::g_statMutex);
      tl.swap(fdexport::g_ovTrace);
    }
    for (const std::string& l : tl) Log("far depth overlay trace: %s", l.c_str());
  }
  WriteToneProbeFiles();  // R91: [Color] ToneProbe's HDR files, off the render thread
  symtr::WriteFiles();    // R94: [Perception] SymbologyTrace's frame texts and target copies
  {
    std::vector<fdexport::ProjLog> pl;
    {
      std::lock_guard<std::mutex> lock(fdexport::g_statMutex);
      pl.swap(fdexport::g_projLog);
    }
    for (const fdexport::ProjLog& e : pl)
      Log("far depth proj: frame %llu view %u P20 %+.7f P21 %+.7f P23 %+.3f P00 %.6f P11 %.6f; viewport "
          "origin %.4f,%.4f width %.1f (P20/P23 in px %+.4f)",
          static_cast<unsigned long long>(e.frame), e.view, e.p20, e.p21, e.p23, e.p00, e.p11, e.vx, e.vy, e.vw,
          e.p23 != 0 ? 0.5 * e.vw * e.p20 / e.p23 : 0.0);
  }
  float tx = 0, ty = 0;
  const bool test = testGaze && testGaze[0] && swscanf_s(testGaze, L"%f , %f", &tx, &ty) == 2;
  g_testGx = tx;
  g_testGy = ty;
  g_testGaze = test;
  g_verify = verify;
  cmask::CloseBuried(false);  // R76: the mask export's old NT handles, closed after their grace (also while it is off)
  // R85: a held set whose device was removed is released. R86: also while installed for other features with the mask
  // export off (the cockpit mask not installed: its render-thread paths do not run).
  if (!g_installed.load() || !g_maskWith) cmask::CheckHeld();
  if (!on) {
    Uninstall();
    return;
  }
  if (!g_installed.load()) {
    Install();
    return;
  }
  if (ckOn) ckipd::TickCfg();
  if (maskOn) cmask::Tick();
  if (g_fault.load()) {
    if (!g_faultLogged) {
      // R69: where (site = the SEH line in fixation_depth.h; the address as module + offset).
      const uintptr_t at = g_faultAddr.load();
      HMODULE mod = nullptr;
      wchar_t name[MAX_PATH] = L"?";
      if (at && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(at), &mod) && mod) {
        wchar_t full[MAX_PATH] = {};
        if (GetModuleFileNameW(mod, full, MAX_PATH)) {
          const wchar_t* sl = wcsrchr(full, L'\\');
          wcsncpy_s(name, sl ? sl + 1 : full, _TRUNCATE);
        }
      }
      Log("fixation depth: fault latched (SEH at fixation_depth.h line %d, code 0x%08x, address %ls+0x%llx); "
          "publishing stopped until the payload reloads",
          g_faultSite.load(), g_faultCode.load(), name,
          static_cast<unsigned long long>(mod ? at - reinterpret_cast<uintptr_t>(mod) : at));
    }
    g_faultLogged = true;
    return;
  }
  const ULONGLONG now = GetTickCount64();
  if (!g_gazeView.load() && now - g_lastOpen >= 1000) {
    g_lastOpen = now;
    OpenMappings();
  }
  if (dump && !dumpDir.empty() && g_dumpDir.empty()) {
    g_dumpDir = dumpDir;
    g_dumpWanted = true;
  }
  if (!dump) g_dumpDir.clear();
  static bool traced = false;
  if (verify >= 2 && !traced && g_s.ctx) {
    traced = true;
    g_trace.clear();
    g_traceDone = false;
    g_traceLeft = 600;
  }
  if (g_traceDone.exchange(false)) {
    Log("fixation depth trace (depth, begin/end, pass):");
    for (const std::string& l : g_trace) Log("  %s", l.c_str());
  }
  if (now - g_lastLog >= 30000) {
    if (g_lastLog) LogStats();
    g_lastLog = now;
  }
}

void Shutdown() {
  Uninstall(false);  // R85: the stop releases the cockpit exports' shared set (R76's records: a new generation, flags 0)
  cmask::ReleaseHeld("payload stop");  // R85: a set held while the exports were off
  cmask::CloseBuried(true);  // R76: the unload: no later Tick
  cmask::StopBuilder();  // R91: the export's builder thread ends (a set it made and nobody took is released)
}

}  // namespace fixdepth
