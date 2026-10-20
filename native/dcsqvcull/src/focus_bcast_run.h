// R53 (R38 rank 2): FastGS single-pass broadcast of the periphery's instancer draws (trees, buildings) into the same
// eye's focus view. [D3D] FocusBroadcast = 2 (default 0 = nothing installed; 1 = focus_bcast.h's readiness check).
// Docs: docs/research/R53_fastgs_broadcast.md (design, exactness, live results).
//
// Per frame and eye (HMD views render periph L, periph R, focus L, focus R; one LightsBuffersPass + G-buffer run each):
//  - Periphery main G-buffer pass of eye e (view e), from a learned start position (below) to the pass end, a BLOCK:
//    DCS's colour targets and depth-stencil are copied into slice 0 of our focus-sized 2-slice arrays (colours with
//    CopySubresourceRegion, depth with an SV_Depth pass, stencil with 8 bit-plane passes), slice 1 is cleared, and
//    our arrays are bound in place of DCS's targets with two viewports: DCS's own (0) and the broadcast viewport (1),
//    which maps periphery NDC onto the focus target (fbc::BroadcastViewport of the focus-of-periphery affine). Every
//    covered draw (an instancer VS whose output signature is known, below) runs with an NVAPI FastGS: viewport mask
//    0b11 and RT index offset by viewport index, so each primitive reaches slice 0 (the periphery, as stock) and slice
//    1 (the focus). Every other draw of the block runs as DCS issued it, into slice 0 (viewport 0 only). At the pass
//    end slice 0 goes back to DCS's targets (colours copied, depth by an SV_Depth pass, stencil by bit planes): the
//    periphery is DCS's own rendering into a copy of its targets, bit-identical to stock.
//  - Focus main G-buffer pass of the same eye (view 2 + e): at the first draw whose key (kind, VS, arguments buffer,
//    offset) was broadcast in the periphery block, slice 1 is MERGED into DCS's focus targets: one full-screen pass
//    with DCS's own depth-stencil state and stencil reference of that draw (depth GREATER_EQUAL as the instancer draws,
//    SV_Depth = the broadcast depth, pixels the broadcast did not cover discarded), DCS's colour write masks, and the
//    motion vectors rewritten into the focus's own (MV = 0.5 dims (prevNDC - curNDC), y down; exact per pixel from the
//    current and previous focus-of-periphery affines, fbc::FixMv). Every broadcast draw of the focus is then not
//    issued; every other focus draw (terrain, grass, models, cockpit, uncovered instancer draws) is DCS's.
//  - Fallback to stock per draw and per eye-frame, each counted: unknown signature, another GS / HS / DS bound, a
//    topology other than a triangle list, a blend state with blending on, a depth-stencil state or stencil reference
//    that differs from the block's, the focus projection not the predicted one, the TerrainContext upload not seen,
//    DCS rebinding its targets inside a block (the block is ended by a depth-tested merge, NEAR for that frame, and the
//    start position learned past that point), a command list executed inside a block (the block is closed exactly
//    before it), other hooks of this payload in the draw slots, PeriphInstFilter on.
//
// Focus projection. The instancer VS reads only the TerrainContext view-projection rows x / y (R38 3.1); the focus
// projection is an exact affine of the periphery's (same eye, rows z / w equal). The focus TerrainContext is uploaded
// only inside the focus pass, after the periphery's targets were consumed, so the focus affine is PREDICTED at the
// periphery pass: the previous frame's focus projection plus this frame's DLSS jitter step (the same pixel jitter in
// both views of an eye: dC_f = dC_p * W_p / W_f, R53 4). The prediction is verified in the focus pass against DCS's
// own upload (CPU copy, one-shot UpdateSubresource observer) before anything is merged or skipped; a miss renders the
// focus stock (counted). A moving gaze changes the focus centre: those frames fall back.
//
// Signatures. DCS creates its instancer VS before the payload loads and keeps no bytecode (R45). Each VS object seen
// in a periphery instancer draw is identified by its output (vs_sig_probe.h's RunTest / prefix rule, R49) against the
// instancer VS blobs of DCS's own shader cache (Saved Games\DCS\metashaders2\*.meta2, read-only), a few tests per frame
// on the render thread; the FastGS is compiled and created on a worker thread.
//
// Start position. DCS may rebind its targets or execute a command list (the G-buffer recorder) early in the pass; a
// block must start after the last such point. Learning frames (no broadcast) bind a SENTINEL depth view (our own view
// of DCS's depth texture, the same subresource: identical rendering) after each hooked draw; a hooked draw that finds
// DCS's own view again saw a rebind since the previous one. The block starts after the last rebind / command list seen.
//
// Exactness: FLOAT, not bit-exact (R38R2 bench: 0.12 % of focus G-buffer pixels from the affine mapping and viewport
// snapping to 1/256 px, 0.35 % from the 0.0086-level mip-bias step the PS keeps from the periphery). Periphery: BIT.
// [Suite] FocusBcastVerify=N: N seconds of same-frame compares of the merged focus against DCS's own focus draws of the
// same keys rendered stock into a copy (G-buffer pixels, depth, coverage, MV).
// [Suite] FocusBcastTiming=1: GPU pass timing OFF/ON/OFF/ON/OFF (5 s each). [Suite] FocusBcastProbe=N: N seconds of
// measure-only census (draw states, TerrainContext uploads, collect matrices, CB dumps, identification) to CSV.
//
// Included once from main.cpp after vs_sig_probe.h (uses pifilt, vsig, fbc, fixdepth::InD3d11, d3ds, plod, ptiming).
#pragma once

namespace fbcr {

using pifilt::Rel;

// ---------------------------------------------------------------------------
// Keys and counters
// ---------------------------------------------------------------------------
std::atomic<int> g_mode{0};          // [D3D] FocusBroadcast (2 = broadcast)
std::atomic<int> g_kinds{1};         // [D3D] FocusBroadcastKinds: bit 0 trees (SpeedTree), bit 1 other instancer VS
std::atomic<int> g_testsPerFrame{6}; // identification tests per frame (render thread)
std::atomic<int> g_relearnFrames{900};
std::atomic<int> g_suiteProbe{0}, g_suiteTiming{0}, g_suiteVerify{0};
std::atomic<int> g_force{-1};        // suite phases: 0 / 1 override the key, -1 = the key
std::atomic<bool> g_engineOff{false};
std::atomic<bool> g_probe{false};    // census rows recorded
std::atomic<bool> g_verify{false};   // same-frame compares
std::atomic<bool> g_stopping{false};
std::wstring g_arcKey;               // [D3D] FocusBroadcastArchive (empty: <Saved Games>\DCS\metashaders2)
std::wstring g_saved;                // the loader's folder (Saved Games\DCS\Scripts\DcsQvCull\)
std::wstring g_outDir;               // CSV dumps (probe)

// Start gate (R53 session 6 defect: with FocusBroadcast=2 preset at launch, DCS hung in the NVIDIA driver two frames
// into the mission, right after the NVAPI readiness check, the install and the archive load ran on the config thread
// while DCS was still loading; nothing of ours was on the render thread). Nothing is created (NVAPI, FastGS, shader
// candidates) and nothing is installed until frames have flowed for [D3D] FocusBroadcastStartDelay seconds (default
// 10, 0 = no gate); the worker's device-object creation pauses whenever frames stop (loading, mission restart).
std::atomic<int> g_startDelay{10};    // [D3D] FocusBroadcastStartDelay (s of steady frames before anything starts)
std::atomic<bool> g_steady{false};    // frames flowing for g_startDelay s now (config tick)
std::atomic<bool> g_started{false};   // g_steady was reached once (latched)
std::atomic<uint64_t> g_heldTicks{0}, g_pausedLoops{0};
// Pure: the run of steady ticks after one more tick (df frames in dtMs): at least 15 frames per second keeps it going.
inline int SteadyStep(int run, uint64_t df, uint64_t dtMs) {
  if (dtMs == 0) return run;
  return df * 1000 >= 15 * dtMs ? run + 1 : 0;
}
// Config tick (about once a second, also while a suite runs): the quad-frame counter.
void NoteFrames(uint64_t frames) {
  static uint64_t lastF = 0;
  static ULONGLONG lastT = 0;
  static int run = 0;
  const ULONGLONG now = GetTickCount64();
  if (lastT) run = SteadyStep(run, frames - lastF, now - lastT);
  lastF = frames, lastT = now;
  const int need = g_startDelay.load();
  const bool st = need <= 0 || run >= need;
  if (st != g_steady.load()) {
    if (g_mode.load() >= 2 && (g_started.load() || st))
      Log("focus broadcast: %s", st ? (g_started.load() ? "frames flowing again: identification resumes"
                                                        : "frames flowing: starting (NVAPI, archive, install)")
                                    : "frames stopped (loading?): identification paused");
    g_steady = st;
  }
  if (st) g_started = true;
}
bool ReadyGate() { return g_mode.load() < 2 || g_started.load(); }  // fbc::Readiness (the R38 check) waits too

struct Counters {
  std::atomic<uint64_t> frames{0}, blocks{0}, blocksClosed{0}, blockAborts{0}, eclCloses{0}, merges{0}, skipped{0},
      covered{0}, uncovNoSig{0}, uncovState{0}, uncovGs{0}, uncovTopo{0}, uncovBlend{0}, uncovKind{0}, predMiss{0},
      predOk{0}, noPred{0}, noTc{0}, tcForeign{0}, tcMissed{0}, viewBad{0}, resFail{0}, fmtBad{0}, learnFrames{0},
      rebinds{0}, focusNoMerge{0}, focusKeyMiss{0}, idTests{0}, notArmed{0}, sfOff{0}, pifOn{0}, partial{0},
      vpReset{0}, faults{0}, mergeSkipNoDraw{0}, stateDiff{0}, popCloses{0}, tailDirty{0}, tailClean{0}, noTail{0},
      noPrevT{0}, tcLost{0};
};
Counters g_c;

// ---------------------------------------------------------------------------
// Pure helpers (tested)
// ---------------------------------------------------------------------------
// The projection part we need from a TerrainContext copy (rows as float4: 16-19 P, 20-23 VP, 38 camera, 44 flags /
// render size).
struct Tc {
  float r[48][4] = {};
  bool ok = false;
};
// P rows x / y of a view: (sx, 0, cx, 0), (0, sy, cy, 0); w row (0, 0, 1, 0).
inline bool ProjShape(const Tc& t) {
  const float* x = t.r[16];
  const float* y = t.r[17];
  const float* w = t.r[19];
  return t.ok && x[1] == 0 && x[3] == 0 && y[0] == 0 && y[3] == 0 && w[0] == 0 && w[1] == 0 && w[2] == 1 && w[3] == 0 &&
         x[0] > 0 && y[1] > 0;
}
// The focus-of-periphery affine from the two projections: x_f = ax x_p + bx w (NDC: f = a p + b).
inline bool AffineOf(const Tc& p, const Tc& f, fbc::Affine& a) {
  if (!ProjShape(p) || !ProjShape(f)) return false;
  a.ax = static_cast<double>(f.r[16][0]) / p.r[16][0];
  a.bx = static_cast<double>(f.r[16][2]) - a.ax * p.r[16][2];
  a.ay = static_cast<double>(f.r[17][1]) / p.r[17][1];
  a.by = static_cast<double>(f.r[17][2]) - a.ay * p.r[17][2];
  a.residual = 0;
  return a.ax > 0 && a.ay > 0;
}
// The same eye: z / w rows of P and VP equal, VP rows x / y of the focus = the affine of the periphery's (relative
// tolerance), camera position equal. Returns the largest deviation (relative to the row scale) in *dev.
inline bool SameEye(const Tc& p, const Tc& f, const fbc::Affine& a, double tol, double* dev) {
  double worst = 0;
  auto nrm = [](const float* v) {
    return std::max(1e-12, std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2] + double(v[3]) * v[3]));
  };
  for (int row : {18, 19, 22, 23}) {
    const double s = nrm(p.r[row]);
    for (int k = 0; k < 4; ++k) worst = std::max(worst, std::fabs(double(p.r[row][k]) - f.r[row][k]) / s);
  }
  const double sw = nrm(p.r[23]);
  for (int k = 0; k < 4; ++k) {
    const double ex = a.ax * p.r[20][k] + a.bx * p.r[23][k];
    const double ey = a.ay * p.r[21][k] + a.by * p.r[23][k];
    worst = std::max(worst, std::fabs(ex - f.r[20][k]) / std::max(sw, nrm(f.r[20])));
    worst = std::max(worst, std::fabs(ey - f.r[21][k]) / std::max(sw, nrm(f.r[21])));
  }
  for (int k = 0; k < 3; ++k) worst = std::max(worst, std::fabs(double(p.r[38][k]) - f.r[38][k]) / (1.0 + std::fabs(p.r[38][k])));
  if (dev) *dev = worst;
  return worst <= tol;
}
// Prediction of this frame's focus P rows x / y (sx, cx, sy, cy) from the previous frame's periphery and focus and this
// frame's periphery: the scales stay, the centre moves by the periphery's jitter step in pixels (the same pixel jitter
// in both views of an eye). False when the previous frame is not usable.
struct Pred {
  double sx = 0, cx = 0, sy = 0, cy = 0;
};
inline bool PredictFocus(const Tc& pPrev, const Tc& fPrev, const Tc& pNow, double wp, double hp, double wf, double hf,
                         Pred& out) {
  if (!ProjShape(pPrev) || !ProjShape(fPrev) || !ProjShape(pNow) || wf <= 0 || hf <= 0) return false;
  if (pPrev.r[16][0] != pNow.r[16][0] || pPrev.r[17][1] != pNow.r[17][1]) return false;  // periphery FOV changed
  out.sx = fPrev.r[16][0];
  out.sy = fPrev.r[17][1];
  out.cx = static_cast<double>(fPrev.r[16][2]) + (static_cast<double>(pNow.r[16][2]) - pPrev.r[16][2]) * wp / wf;
  out.cy = static_cast<double>(fPrev.r[17][2]) + (static_cast<double>(pNow.r[17][2]) - pPrev.r[17][2]) * hp / hf;
  return true;
}
// The prediction against DCS's own focus rows (absolute NDC tolerance; 2e-6 NDC = 0.001 focus pixels).
inline bool PredMatches(const Pred& p, const Tc& f, double tol, double* dev) {
  if (!ProjShape(f)) return false;
  const double d = std::max(std::max(std::fabs(p.sx - f.r[16][0]), std::fabs(p.sy - f.r[17][1])),
                            std::max(std::fabs(p.cx - f.r[16][2]), std::fabs(p.cy - f.r[17][2])));
  if (dev) *dev = d;
  return d <= tol;
}
// The affine from the periphery rows and a predicted focus.
inline fbc::Affine AffineFromPred(const Tc& p, const Pred& f) {
  fbc::Affine a;
  a.ax = f.sx / p.r[16][0];
  a.bx = f.cx - a.ax * p.r[16][2];
  a.ay = f.sy / p.r[17][1];
  a.by = f.cy - a.ay * p.r[17][2];
  return a;
}

// Learned block start (hooked-draw ordinals of the periphery main pass): the block starts after the last rebind /
// command list seen in learning frames or in aborted blocks.
struct StartLearn {
  int start = -1;     // first ordinal allowed (-1: not learned)
  int seenMax = -1;   // learning frame in progress: the largest ordinal at which a rebind / list was seen
  void BeginLearn() { seenMax = 0; }
  void Saw(int ord) { seenMax = std::max(seenMax, ord); }
  void EndLearn(int drawsSeen) {
    if (drawsSeen > 0) start = std::max(0, seenMax);
    seenMax = -1;
  }
  void Abort(int ord) { start = std::max(start, ord + 1); }
};

// DXGI format families: the typeless format of a texture format and the integer view that copies its bits exactly.
struct Fam {
  DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN, raw = DXGI_FORMAT_UNKNOWN;
  int comps = 0;
  bool isFloat = false;  // raw view is a float format (exact for float families)
};
inline Fam FamilyOf(DXGI_FORMAT f) {
  Fam r;
  auto set = [&](DXGI_FORMAT t, DXGI_FORMAT raw, int n, bool fl) {
    r.typeless = t, r.raw = raw, r.comps = n, r.isFloat = fl;
  };
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT:
      set(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UINT, 4, false); break;
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
      set(DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UINT, 2, false); break;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
      set(DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UINT, 1, false); break;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
      set(DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UINT, 4, false); break;
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_SINT:
      set(DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UINT, 2, false); break;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT:
      set(DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UINT, 1, false); break;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT:
      set(DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UINT, 4, false); break;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
      set(DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_UINT, 1, false); break;
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
      set(DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_UINT, 2, false); break;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
      set(DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_UINT, 4, false); break;
    case DXGI_FORMAT_R11G11B10_FLOAT:
      set(DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT, 3, true); break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      set(DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, 4, true); break;
    default: break;
  }
  return r;
}
// Integer formats (bits read and written as uint) and the normalised-integer scale of UNORM formats (0: neither).
inline bool IsIntFmt(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R10G10B10A2_UINT: case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32B32A32_UINT:
      return true;
    default:
      return false;
  }
}
inline double UnormScale(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return 255.0;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      return 1023.0;
    case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16_UNORM:
      return 65535.0;
    default:
      return 0.0;
  }
}
inline bool IsTypeless(DXGI_FORMAT f) { return f != DXGI_FORMAT_UNKNOWN && FamilyOf(f).typeless == f && f != DXGI_FORMAT_R11G11B10_FLOAT; }

// Shaders: one source; entry points compiled once (vs_5_0 / ps_5_0 / cs_5_0).
const char kHlsl[] = R"(
cbuffer P : register(b13) {
  float4 Cur;   // ax, bx, ay, by: focus NDC = a * periphery NDC + b (this frame)
  float4 Prev;  // the same for the matrices the motion vectors use (previous frame)
  float4 Dims;  // periphery w, h; focus w, h
  float4 Misc;  // x: depth clear value
  uint4 Bits;   // x: stencil bit
};
struct V { float4 pos : SV_Position; };
V VsFull(uint id : SV_VertexID) {
  V o;
  float2 t = float2((id << 1) & 2, id & 2);
  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
Texture2DArray<float> Dep : register(t0);
Texture2DArray<uint2> Sten : register(t1);
Buffer<float4> Tm : register(t1);  // the merge's: gPrevFrameTransform rows (registers 32-35) of periphery (0-3), focus (4-7)
float4x4 Inv4(float4x4 m) {
  float a00 = m[0][0], a01 = m[0][1], a02 = m[0][2], a03 = m[0][3], a10 = m[1][0], a11 = m[1][1], a12 = m[1][2],
        a13 = m[1][3], a20 = m[2][0], a21 = m[2][1], a22 = m[2][2], a23 = m[2][3], a30 = m[3][0], a31 = m[3][1],
        a32 = m[3][2], a33 = m[3][3];
  float b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10, b02 = a00 * a13 - a03 * a10,
        b03 = a01 * a12 - a02 * a11, b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12,
        b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30, b08 = a20 * a33 - a23 * a30,
        b09 = a21 * a32 - a22 * a31, b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
  float det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
  float4x4 r = float4x4(
      a11 * b11 - a12 * b10 + a13 * b09, a02 * b10 - a01 * b11 - a03 * b09, a31 * b05 - a32 * b04 + a33 * b03,
      a22 * b04 - a21 * b05 - a23 * b03, a12 * b08 - a10 * b11 - a13 * b07, a00 * b11 - a02 * b08 + a03 * b07,
      a32 * b02 - a30 * b05 - a33 * b01, a20 * b05 - a22 * b02 + a23 * b01, a10 * b10 - a11 * b08 + a13 * b06,
      a01 * b08 - a00 * b10 - a03 * b06, a30 * b04 - a31 * b02 + a33 * b00, a21 * b02 - a20 * b04 - a23 * b00,
      a11 * b07 - a10 * b09 - a12 * b06, a00 * b09 - a01 * b07 + a02 * b06, a31 * b01 - a30 * b03 - a32 * b00,
      a20 * b03 - a21 * b01 + a22 * b00);
  return r / det;
}
void PsDepth(float4 pos : SV_Position, out float d : SV_Depth) { d = Dep.Load(int4(int2(pos.xy), 0, 0)); }
void PsStencil(float4 pos : SV_Position) {
  if ((Sten.Load(int4(int2(pos.xy), 0, 0)).y & Bits.x) == 0) discard;
}
)";

// The merge pixel shader for one target layout: t0 = the broadcast depth (slice view), t2.. = the broadcast colours in
// their raw formats (integer views copy bits exactly; float views of float formats too); SV_Target k = the same bits,
// except the motion-vector target `mv` (a 16-bit float target), which is rewritten into the focus's own motion vector
// (fbc::FixMv; DCS's calcMotionVector stores 0.5 dims (prevNDC - curNDC) with y negated). Pixels the broadcast did not
// cover (depth = the clear value) are discarded. `mv` < 0: no motion-vector target (the periphery abort merge).
inline std::string MergeHlsl(int n, const DXGI_FORMAT* raw, int mv) {
  std::string decl, out, body;
  char line[1024];
  for (int k = 0; k < n; ++k) {
    const Fam f = FamilyOf(raw[k]);
    const bool fl = !IsIntFmt(raw[k]);  // integer views copy bits; every other view reads / writes the same float value
    const int c = f.comps ? f.comps : 4;
    const std::string t = std::string(fl ? "float" : "uint") + (c > 1 ? std::to_string(c) : "");
    snprintf(line, sizeof(line), "Texture2DArray<%s> R%d : register(t%d);\n", t.c_str(), k, k + 2);
    decl += line;
    snprintf(line, sizeof(line), "  %s o%d : SV_Target%d;\n", t.c_str(), k, k);
    out += line;
    if (k == mv && c >= 2) {
      snprintf(line, sizeof(line),
               "  { %s m = R%d.Load(q);\n"
               "    float2 nf = float2(pos.x / Dims.z * 2 - 1, 1 - pos.y / Dims.w * 2);\n"
               "    float2 cp = (nf - Cur.yw) / Cur.xz;\n"
               "    float2 pp = cp + float2(2 * m.x / Dims.x, -2 * m.y / Dims.y);\n"
               "    float4x4 Tp = float4x4(Tm[0], Tm[1], Tm[2], Tm[3]), Tf = float4x4(Tm[4], Tm[5], Tm[6], Tm[7]);\n"
               "    float4x4 A = float4x4(Cur.x, 0, 0, Cur.y, 0, Cur.z, 0, Cur.w, 0, 0, 1, 0, 0, 0, 0, 1);\n"
               "    float4 qf = mul(mul(Tf, mul(A, Inv4(Tp))), float4(pp, d, 1));\n"
               "    float2 pf = qf.xy / qf.w;\n"
               "    m.x = 0.5 * Dims.z * (pf.x - nf.x);\n"
               "    m.y = -0.5 * Dims.w * (pf.y - nf.y);\n"
               "    o.o%d = m; }\n",
               t.c_str(), k, k);
    } else {
      snprintf(line, sizeof(line), "  o.o%d = R%d.Load(q);\n", k, k);
    }
    body += line;
  }
  return decl + "struct MO {\n" + out + "  float dep : SV_Depth;\n};\nMO PsMerge(float4 pos : SV_Position) {\n" +
         "  int4 q = int4(int2(pos.xy), 0, 0);\n  float d = Dep.Load(q);\n  if (d == Misc.x) discard;\n  MO o;\n" +
         "  o.dep = d;\n" + body + "  return o;\n}\n";
}

// ---------------------------------------------------------------------------
// Archive: the instancer VS blobs of DCS's own shader cache (read-only)
// ---------------------------------------------------------------------------
struct ArcBlob {
  std::vector<uint8_t> code;
  vsig::Blob b;
  int cat = 2;  // 1 tree (SpeedTree: wind / billboard resources), 2 other
};
std::vector<ArcBlob> g_arc;
std::atomic<int> g_arcState{0};  // 0 not loaded, 2 loaded, 3 nothing found
std::atomic<size_t> g_arcFiles{0};

inline const uint8_t* MemFind(const uint8_t* a, size_t n, const char* s) {
  const size_t m = strlen(s);
  if (m == 0 || n < m) return nullptr;
  for (size_t i = 0; i + m <= n; ++i) {
    const void* p = memchr(a + i, s[0], n - m + 1 - i);
    if (!p) return nullptr;
    i = static_cast<const uint8_t*>(p) - a;
    if (memcmp(a + i, s, m) == 0) return a + i;
  }
  return nullptr;
}
inline int CatOf(const uint8_t* b, size_t n) {
  return (MemFind(b, n, "instancer2_airflowBuffer") || MemFind(b, n, "material__bilboardData")) ? 1 : 2;
}
// Every instancer VS (a DXBC vertex shader whose resource names hold instancer2_inputMainBuffer) in one file's bytes.
inline void ScanBlobs(const std::vector<uint8_t>& f, std::set<std::string>& seen, std::vector<ArcBlob>& out) {
  size_t i = 0;
  while (i + 32 <= f.size()) {
    const uint8_t* p = MemFind(f.data() + i, f.size() - i, "DXBC");
    if (!p) break;
    const size_t at = p - f.data();
    i = at + 4;
    const uint32_t sz = vsig::DxbcSize(p, f.size() - at);
    if (!sz || sz > f.size() - at) continue;
    if (!MemFind(p, sz, "instancer2_inputMainBuffer")) continue;
    ArcBlob a;
    if (!vsig::Analyze(p, sz, a.b)) continue;
    i = at + sz;
    if (!seen.insert(vsig::SumHex(a.b.sum)).second) continue;
    a.code.assign(p, p + sz);
    a.cat = CatOf(p, sz);
    out.push_back(std::move(a));
  }
}
inline std::wstring ArchiveDir() {
  if (!g_arcKey.empty()) return g_arcKey;
  return g_saved + L"..\\..\\metashaders2";
}
// *.meta2 (DCS's cache) and *.dxbc (dev extracts) of one folder; files above 256 MB are skipped.
inline std::vector<ArcBlob> LoadArchiveFrom(const std::wstring& dir, size_t* files) {
  std::vector<ArcBlob> out;
  std::set<std::string> seen;
  size_t nf = 0;
  for (const wchar_t* pat : {L"\\*.meta2", L"\\*.dxbc"}) {
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW((dir + pat).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.nFileSizeHigh || fd.nFileSizeLow > (256u << 20)) continue;
      const HANDLE f = CreateFileW((dir + L"\\" + fd.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
      if (f == INVALID_HANDLE_VALUE) continue;
      std::vector<uint8_t> b(fd.nFileSizeLow);
      DWORD got = 0;
      const bool ok = ReadFile(f, b.data(), fd.nFileSizeLow, &got, nullptr) && got == fd.nFileSizeLow;
      CloseHandle(f);
      if (!ok) continue;
      ++nf;
      ScanBlobs(b, seen, out);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  if (files) *files = nf;
  return out;
}
void LoadArchive() {
  if (g_arcState.load() != 0) return;
  size_t nf = 0;
  g_arc = LoadArchiveFrom(ArchiveDir(), &nf);
  g_arcFiles = nf;
  size_t trees = 0;
  for (auto& a : g_arc) trees += a.cat == 1;
  g_arcState = g_arc.empty() ? 3 : 2;
  Log("focus broadcast: archive %ls: %zu files, %zu distinct instancer VS (%zu SpeedTree)", ArchiveDir().c_str(), nf,
      g_arc.size(), trees);
}

// ---------------------------------------------------------------------------
// VS objects and their identification (render thread runs the tests, a worker prepares / tallies / builds GS)
// ---------------------------------------------------------------------------
enum ObjState : int { kNew = 0, kTesting = 2, kTallied = 3, kReady = 4, kNoSig = 5, kGsFail = 6, kNoCand = 7 };
struct Obj {
  ID3D11VertexShader* vs = nullptr;  // AddRef'd (key)
  int mainSlot = -1;
  std::atomic<int> st{kNew};
  std::vector<vsig::IdTest> tests;   // render thread while kTesting
  std::vector<int> testCat;
  bool filtered = false;
  ID3D11GeometryShader* gs = nullptr;
  int cat = 0, elems = 0, gsStatus = 0, matches = 0, prefixes = 0;
  bool ambiguous = false;
  std::string sum;
  uint64_t draws[2] = {}, bcast = 0;
};
std::mutex g_objMu;
std::unordered_map<ID3D11VertexShader*, Obj*> g_objs;
std::atomic<int> g_testsThisFrame{0};

// The t-slot holding the identified mainBuffer (pifilt) among the bound VS SRVs, -1 when absent.
inline int MainSlotNow(ID3D11DeviceContext* c) {
  ID3D11ShaderResourceView* v[16] = {};
  c->VSGetShaderResources(0, 16, v);
  int slot = -1;
  for (int i = 0; i < 16; ++i)
    if (v[i] && slot < 0 && pifilt::g_d.mainB && pifilt::BufOf(v[i]) == pifilt::g_d.mainB) slot = i;
  for (auto& p : v) Rel(p);
  return slot;
}
inline Obj* FindObj(ID3D11VertexShader* vs) {
  std::lock_guard<std::mutex> lock(g_objMu);
  auto it = g_objs.find(vs);
  return it == g_objs.end() ? nullptr : it->second;
}
// Render thread: the object of a periphery instancer draw (created on first sight with its mainBuffer slot).
inline Obj* ObjOf(ID3D11DeviceContext* c, ID3D11VertexShader* vs) {
  if (Obj* o = FindObj(vs)) return o;
  Obj* o = new Obj;
  o->vs = vs;
  vs->AddRef();
  o->mainSlot = MainSlotNow(c);
  std::lock_guard<std::mutex> lock(g_objMu);
  if (g_objs.size() >= 256) {  // a sanity bound (live: 15 objects)
    o->vs->Release();
    delete o;
    return nullptr;
  }
  g_objs[vs] = o;
  return o;
}
// Worker: the device objects of one candidate test (RunTest would create them on the render thread).
inline bool Precreate(ID3D11Device* d, vsig::IdTest& t) {
  if (!t.vs && FAILED(d->CreateVertexShader(t.code.data(), t.code.size(), nullptr, &t.vs))) return false;
  if (!t.gs && FAILED(d->CreateGeometryShaderWithStreamOutput(t.gsCode.data(), t.gsCode.size(), t.decl.data(),
                                                              static_cast<UINT>(t.decl.size()), &t.stride, 1,
                                                              D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &t.gs)))
    return false;
  const UINT bytes = t.stride * vsig::kIdVerts;
  auto mk = [&](ID3D11Buffer** b, bool staging) {
    if (*b) return true;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    bd.BindFlags = staging ? 0 : D3D11_BIND_STREAM_OUTPUT;
    bd.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
    return SUCCEEDED(d->CreateBuffer(&bd, nullptr, b));
  };
  return mk(&t.soA, false) && mk(&t.soB, false) && mk(&t.stA, true) && mk(&t.stB, true);
}
// Worker: candidates for a new object (the archive's VS declaring a 64-byte structured resource at its mainBuffer
// slot, with a FastGS-able signature), prepared and their device objects created.
void PrepareObj(Obj& o) {
  if (o.mainSlot < 0 || g_arcState.load() != 2) {
    o.st = kNoCand;
    return;
  }
  std::set<std::string> taken;
  for (const ArcBlob& a : g_arc) {
    if (!vsig::DeclaresMain(a.b, o.mainSlot) || !a.b.gsOk || !taken.insert(vsig::SumHex(a.b.sum)).second) continue;
    vsig::IdTest t;
    if (!vsig::PrepareTest(a.code, t)) continue;
    if (!Precreate(pifilt::g_dev, t)) {
      t.Release();
      continue;
    }
    o.tests.push_back(std::move(t));
    o.testCat.push_back(a.cat);
  }
  o.st = o.tests.empty() ? kNoCand : kTesting;
}
// Worker: the outcome (R49's prefix rule), the FastGS (viewport mask 0b11, RT index offset by viewport index).
void FinishObj(Obj& o) {
  vsig::Info in;
  vsig::TallyTests(o.tests, in);
  o.matches = in.idMatches;
  o.prefixes = in.idPrefix;
  o.ambiguous = in.idAmbig;
  const vsig::IdTest* best = nullptr;
  int bestCat = 2;
  for (size_t i = 0; i < o.tests.size(); ++i) {
    const vsig::IdTest& t = o.tests[i];
    if (t.state == 2 && vsig::SumHex(t.b.sum) == in.matchSum && static_cast<int>(t.sig.size()) == in.idLongest) {
      best = &t;
      bestCat = o.testCat[i];
      break;
    }
  }
  if (!in.idMatch || in.idAmbig || !best) {
    o.st = kNoSig;
  } else {
    std::vector<fbc::SigElem> sig;
    fbc::ParseOutputSignature(best->code.data(), best->code.size(), sig);
    o.sum = in.matchSum;
    o.elems = static_cast<int>(sig.size());
    o.cat = bestCat;
    int st = 0;
    o.gs = fbc::CreateFastGs(pifilt::g_dev, sig, 3, true, &st);
    o.gsStatus = st;
    o.st = o.gs ? kReady : kGsFail;
  }
  for (vsig::IdTest& t : o.tests) t.Release();
  o.tests.clear();
  o.testCat.clear();
}
// Render thread, before a periphery instancer draw of an object under test: at most g_testsPerFrame per frame.
void TestStep(ID3D11DeviceContext* c, Obj& o) {
  if (o.st.load(std::memory_order_acquire) != kTesting) return;
  if (!o.filtered) {
    o.filtered = true;
    for (vsig::IdTest& t : o.tests)
      if (t.state == 0 && !vsig::StridesMatch(c, t.b)) t.state = 5;
  }
  if (g_testsThisFrame.load(std::memory_order_relaxed) >= g_testsPerFrame.load(std::memory_order_relaxed)) return;
  for (vsig::IdTest& t : o.tests)
    if (t.state == 0) {
      if (!vsig::RunTest(c, pifilt::g_dev, o.vs, t)) t.state = 4;
      g_testsThisFrame.fetch_add(1, std::memory_order_relaxed);
      g_c.idTests.fetch_add(1, std::memory_order_relaxed);
      return;
    }
}
// Render thread, at a frame start: tests read back without waiting; an object whose tests all ended is tallied.
void PollTests(ID3D11DeviceContext* c) {
  std::vector<Obj*> v;
  {
    std::lock_guard<std::mutex> lock(g_objMu);
    for (auto& kv : g_objs)
      if (kv.second->st.load() == kTesting) v.push_back(kv.second);
  }
  for (Obj* o : v) {
    bool open = false;
    for (vsig::IdTest& t : o->tests) open = open || vsig::PollTest(c, t) < 2;
    if (!open) o->st.store(kTallied, std::memory_order_release);
  }
}

std::thread g_worker;
std::atomic<bool> g_workerRun{false};
void WorkerLoop() {
  while (g_workerRun.load()) {
    if ((g_mode.load() >= 2 || g_force.load() == 1 || g_probe.load()) && !g_steady.load() && g_force.load() < 0) {
      g_pausedLoops.fetch_add(1, std::memory_order_relaxed);
    } else if (g_mode.load() >= 2 || g_force.load() == 1 || g_probe.load()) {
      LoadArchive();
      std::vector<Obj*> v;
      {
        std::lock_guard<std::mutex> lock(g_objMu);
        for (auto& kv : g_objs) v.push_back(kv.second);
      }
      for (Obj* o : v) {
        if (!g_workerRun.load()) break;
        const int st = o->st.load(std::memory_order_acquire);
        if (st == kNew) PrepareObj(*o);
        else if (st == kTallied) FinishObj(*o);
        if (o->st.load() == kReady || o->st.load() == kNoSig || o->st.load() == kGsFail || o->st.load() == kNoCand) {
          if (st != o->st.load())
            Log("focus broadcast: VS %p (mainBuffer t%d): %s%s, %d matches (%d shorter prefix signatures), %d "
                "elements, %s, FastGS status %d, checksum %s",
                static_cast<void*>(o->vs), o->mainSlot,
                o->st.load() == kReady ? "signature known" : o->st.load() == kNoCand ? "no candidate" : "no signature",
                o->ambiguous ? " (AMBIGUOUS)" : "", o->matches, o->prefixes, o->elems,
                o->cat == 1 ? "SpeedTree" : "other", o->gsStatus, o->sum.empty() ? "-" : o->sum.c_str());
        }
      }
    }
    for (int i = 0; i < 10 && g_workerRun.load(); ++i) Sleep(20);
  }
}

// ---------------------------------------------------------------------------
// Device objects, saved state, full-screen passes (render thread, DCS's immediate context)
// ---------------------------------------------------------------------------
struct Dev {
  ID3D11DeviceContext1* c1 = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader *psDepth = nullptr, *psSten = nullptr;
  ID3D11DepthStencilState* dssDepth = nullptr;
  ID3D11DepthStencilState* dssBit[8] = {};
  ID3D11DepthStencilState* dssMerge[2] = {};  // the abort merge: GREATER_EQUAL / LESS_EQUAL, no stencil
  ID3D11RasterizerState* rs = nullptr;
  ID3D11BlendState* bsNone = nullptr;
  ID3D11Buffer* cb = nullptr;
  std::map<std::string, ID3D11PixelShader*> merge;
  std::map<uint32_t, ID3D11BlendState*> bsMask;
  int fail = 0;
  bool ok = false;
};
Dev g_dev;
std::string g_devErr;

inline ID3DBlob* Compile(const std::string& src, const char* entry, const char* target) {
  if (!chainfast::LoadCompiler()) {
    g_devErr = "d3dcompiler_47.dll not available";
    return nullptr;
  }
  ID3DBlob *code = nullptr, *err = nullptr;
  const HRESULT hr = chainfast::g_compile(src.data(), src.size(), "fbcr", nullptr, nullptr, entry, target,
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
  if (FAILED(hr)) g_devErr = err ? static_cast<const char*>(err->GetBufferPointer()) : "compile failed";
  if (err) err->Release();
  return SUCCEEDED(hr) ? code : nullptr;
}
void ReleaseDev() {
  Dev& d = g_dev;
  Rel(d.c1);
  Rel(d.vs);
  Rel(d.psDepth);
  Rel(d.psSten);
  Rel(d.dssDepth);
  for (auto& s : d.dssBit) Rel(s);
  for (auto& s : d.dssMerge) Rel(s);
  Rel(d.rs);
  Rel(d.bsNone);
  Rel(d.cb);
  for (auto& kv : d.merge)
    if (kv.second) kv.second->Release();
  d.merge.clear();
  for (auto& kv : d.bsMask)
    if (kv.second) kv.second->Release();
  d.bsMask.clear();
  d.ok = false;
}
bool EnsureDev() {
  Dev& d = g_dev;
  if (d.ok) return true;
  if (d.fail >= 3 || !pifilt::g_dev || !pifilt::g_ctx) return false;
  ID3D11Device* dev = pifilt::g_dev;
  bool ok = true;
  ID3DBlob* b = Compile(kHlsl, "VsFull", "vs_5_0");
  ok = b && SUCCEEDED(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &d.vs));
  if (b) b->Release();
  b = ok ? Compile(kHlsl, "PsDepth", "ps_5_0") : nullptr;
  ok = ok && b && SUCCEEDED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &d.psDepth));
  if (b) b->Release();
  b = ok ? Compile(kHlsl, "PsStencil", "ps_5_0") : nullptr;
  ok = ok && b && SUCCEEDED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &d.psSten));
  if (b) b->Release();
  if (ok) {
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ok = SUCCEEDED(dev->CreateDepthStencilState(&ds, &d.dssDepth));
    for (int k = 0; ok && k < 2; ++k) {
      ds.DepthFunc = k == 0 ? D3D11_COMPARISON_GREATER_EQUAL : D3D11_COMPARISON_LESS_EQUAL;
      ok = SUCCEEDED(dev->CreateDepthStencilState(&ds, &d.dssMerge[k]));
    }
    D3D11_DEPTH_STENCIL_DESC st{};
    st.DepthEnable = FALSE;
    st.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    st.DepthFunc = D3D11_COMPARISON_ALWAYS;
    st.StencilEnable = TRUE;
    st.StencilReadMask = 0;
    st.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
    st.BackFace = st.FrontFace;
    for (int k = 0; ok && k < 8; ++k) {
      st.StencilWriteMask = static_cast<UINT8>(1u << k);
      ok = SUCCEEDED(dev->CreateDepthStencilState(&st, &d.dssBit[k]));
    }
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rd, &d.rs));
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = 0;
    ok = ok && SUCCEEDED(dev->CreateBlendState(&bd, &d.bsNone));
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = 80;
    cd.Usage = D3D11_USAGE_DYNAMIC;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&cd, nullptr, &d.cb));
  }
  if (ok) pifilt::g_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&d.c1));
  if (!ok) {
    ReleaseDev();
    ++d.fail;
    Log("focus broadcast: our shaders / states could not be created (%d of 3): %s", d.fail, g_devErr.c_str());
    return false;
  }
  d.ok = true;
  return true;
}
// The merge PS for a raw-format layout (cached).
ID3D11PixelShader* MergePs(int n, const DXGI_FORMAT* raw, int mv) {
  std::string key = std::to_string(n) + ":" + std::to_string(mv);
  for (int k = 0; k < n; ++k) key += ":" + std::to_string(static_cast<int>(raw[k]));
  auto it = g_dev.merge.find(key);
  if (it != g_dev.merge.end()) return it->second;
  ID3D11PixelShader* ps = nullptr;
  ID3DBlob* b = Compile(std::string(kHlsl) + MergeHlsl(n, raw, mv), "PsMerge", "ps_5_0");
  if (b) pifilt::g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps);
  if (b) b->Release();
  if (!ps) Log("focus broadcast: merge shader for layout %s failed: %s", key.c_str(), g_devErr.c_str());
  g_dev.merge[key] = ps;
  return ps;
}
// Blending off, DCS's write mask per target (packed 4 bits each).
ID3D11BlendState* MaskBs(uint32_t masks) {
  auto it = g_dev.bsMask.find(masks);
  if (it != g_dev.bsMask.end()) return it->second;
  D3D11_BLEND_DESC bd{};
  bd.IndependentBlendEnable = TRUE;
  for (int k = 0; k < 8; ++k) {
    D3D11_RENDER_TARGET_BLEND_DESC& r = bd.RenderTarget[k];
    r.BlendEnable = FALSE;
    r.SrcBlend = r.SrcBlendAlpha = D3D11_BLEND_ONE;
    r.DestBlend = r.DestBlendAlpha = D3D11_BLEND_ZERO;
    r.BlendOp = r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    r.RenderTargetWriteMask = static_cast<UINT8>((masks >> (4 * k)) & 0xf);
  }
  ID3D11BlendState* bs = nullptr;
  pifilt::g_dev->CreateBlendState(&bd, &bs);
  g_dev.bsMask[masks] = bs;
  return bs;
}

// Everything our full-screen passes touch, saved before and put back after (DCS's state caches must find what they
// set; constant buffer b13 with its *1 range).
struct Gfx {
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11GeometryShader* gs = nullptr;
  ID3D11HullShader* hs = nullptr;
  ID3D11DomainShader* ds = nullptr;
  ID3D11InputLayout* il = nullptr;
  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  ID3D11RasterizerState* rs = nullptr;
  D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
  UINT nvp = 0;
  D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
  UINT nsc = 0;
  ID3D11RenderTargetView* rtv[8] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11BlendState* bs = nullptr;
  float bf[4] = {};
  UINT sm = 0;
  ID3D11DepthStencilState* dss = nullptr;
  UINT ref = 0;
  ID3D11ShaderResourceView* srv[10] = {};
  ID3D11Buffer* cb = nullptr;
  UINT cbFirst = 0, cbNum = 0;
  void Save(ID3D11DeviceContext* c) {
    c->VSGetShader(&vs, nullptr, nullptr);
    c->PSGetShader(&ps, nullptr, nullptr);
    c->GSGetShader(&gs, nullptr, nullptr);
    c->HSGetShader(&hs, nullptr, nullptr);
    c->DSGetShader(&ds, nullptr, nullptr);
    c->IAGetInputLayout(&il);
    c->IAGetPrimitiveTopology(&topo);
    c->RSGetState(&rs);
    nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->RSGetViewports(&nvp, vp);
    nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->RSGetScissorRects(&nsc, sc);
    c->OMGetRenderTargets(8, rtv, &dsv);
    c->OMGetBlendState(&bs, bf, &sm);
    c->OMGetDepthStencilState(&dss, &ref);
    c->PSGetShaderResources(0, 10, srv);
    if (g_dev.c1)
      g_dev.c1->PSGetConstantBuffers1(13, 1, &cb, &cbFirst, &cbNum);
    else
      c->PSGetConstantBuffers(13, 1, &cb);
  }
  void Restore(ID3D11DeviceContext* c) {
    UINT n = 0;
    for (UINT i = 0; i < 8; ++i)
      if (rtv[i]) n = i + 1;
    c->OMSetRenderTargets(n, rtv, dsv);
    c->OMSetBlendState(bs, bf, sm);
    c->OMSetDepthStencilState(dss, ref);
    c->PSSetShaderResources(0, 10, srv);
    const bool offs = cb && !(cbFirst == 0 && (cbNum == 0 || cbNum == 4096));
    if (offs && g_dev.c1)
      g_dev.c1->PSSetConstantBuffers1(13, 1, &cb, &cbFirst, &cbNum);
    else
      c->PSSetConstantBuffers(13, 1, &cb);
    c->RSSetState(rs);
    c->RSSetViewports(nvp, vp);
    c->RSSetScissorRects(nsc, sc);
    c->IASetPrimitiveTopology(topo);
    c->IASetInputLayout(il);
    c->DSSetShader(ds, nullptr, 0);
    c->HSSetShader(hs, nullptr, 0);
    c->GSSetShader(gs, nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);
    c->VSSetShader(vs, nullptr, 0);
    Rel(vs), Rel(ps), Rel(gs), Rel(hs), Rel(ds), Rel(il), Rel(rs), Rel(dsv), Rel(bs), Rel(dss), Rel(cb);
    for (auto& p : rtv) Rel(p);
    for (auto& p : srv) Rel(p);
  }
};
struct Params {
  float cur[4], prev[4], dims[4], misc[4];
  uint32_t bits[4];
};
inline void PutCb(ID3D11DeviceContext* c, const Params& p) {
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(c->Map(g_dev.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    memcpy(m.pData, &p, sizeof(p));
    c->Unmap(g_dev.cb, 0);
  }
}
// Full-screen triangle with our VS and raster state (the caller set the rest).
inline void FullScreen(ID3D11DeviceContext* c) {
  c->IASetInputLayout(nullptr);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c->VSSetShader(g_dev.vs, nullptr, 0);
  c->HSSetShader(nullptr, nullptr, 0);
  c->DSSetShader(nullptr, nullptr, 0);
  c->GSSetShader(nullptr, nullptr, 0);
  c->RSSetState(g_dev.rs);
  c->Draw(3, 0);
}
// SV_Depth of every pixel of `vp` from `src` (a one-slice array view) into `dsv` (depth test ALWAYS, no stencil).
inline void DepthPass(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv, ID3D11ShaderResourceView* src,
                      const D3D11_VIEWPORT& vp) {
  ID3D11ShaderResourceView* none[2] = {};
  c->PSSetShaderResources(0, 2, none);
  c->OMSetRenderTargets(0, nullptr, dsv);
  c->OMSetDepthStencilState(g_dev.dssDepth, 0);
  const float zero[4] = {};
  c->OMSetBlendState(g_dev.bsNone, zero, 0xffffffffu);
  c->RSSetViewports(1, &vp);
  c->PSSetShader(g_dev.psDepth, nullptr, 0);
  c->PSSetShaderResources(0, 1, &src);
  FullScreen(c);
}
// The stencil of `dsv` = the stencil read from `src` (G8 of a one-slice array view): cleared, then 8 bit planes.
inline void StencilPasses(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv, ID3D11ShaderResourceView* src,
                          const D3D11_VIEWPORT& vp) {
  ID3D11ShaderResourceView* none[2] = {};
  c->PSSetShaderResources(0, 2, none);
  c->OMSetRenderTargets(0, nullptr, dsv);
  c->ClearDepthStencilView(dsv, D3D11_CLEAR_STENCIL, 0, 0);
  const float zero[4] = {};
  c->OMSetBlendState(g_dev.bsNone, zero, 0xffffffffu);
  c->RSSetViewports(1, &vp);
  c->PSSetShader(g_dev.psSten, nullptr, 0);
  c->PSSetShaderResources(1, 1, &src);
  c->PSSetConstantBuffers(13, 1, &g_dev.cb);
  for (int k = 0; k < 8; ++k) {
    Params p{};
    p.bits[0] = 1u << k;
    PutCb(c, p);
    c->OMSetDepthStencilState(g_dev.dssBit[k], 0xff);
    FullScreen(c);
  }
}

// ---------------------------------------------------------------------------
// Our 2-slice targets per eye, and our views of DCS's textures
// ---------------------------------------------------------------------------
struct Arr {
  UINT w = 0, h = 0, n = 0;
  DXGI_FORMAT texFmt[8] = {}, rtvFmt[8] = {}, rawFmt[8] = {};
  ID3D11Texture2D* tex[8] = {};
  ID3D11RenderTargetView* rtv2[8] = {};
  ID3D11ShaderResourceView *srv0[8] = {}, *srv1[8] = {};
  ID3D11Texture2D* dtex = nullptr;
  DXGI_FORMAT dTexFmt = DXGI_FORMAT_UNKNOWN, dDsvFmt = DXGI_FORMAT_UNKNOWN;
  ID3D11DepthStencilView *dsv2 = nullptr, *dsv0 = nullptr, *dsv1 = nullptr;
  ID3D11ShaderResourceView *dsrv0 = nullptr, *dsrv1 = nullptr, *ssrv0 = nullptr;
  ID3D11Buffer* tm = nullptr;             // 8 float4: gPrevFrameTransform registers of the periphery (0-3) and focus (4-7)
  ID3D11ShaderResourceView* tmSrv = nullptr;
  bool stencil = false;
  int mv = -1;
  void Release() {
    for (int k = 0; k < 8; ++k) Rel(tex[k]), Rel(rtv2[k]), Rel(srv0[k]), Rel(srv1[k]);
    Rel(dtex), Rel(dsv2), Rel(dsv0), Rel(dsv1), Rel(dsrv0), Rel(dsrv1), Rel(ssrv0), Rel(tm), Rel(tmSrv);
    *this = Arr{};
  }
};
Arr g_arr[2];

inline DXGI_FORMAT DepthSrvFmt(DXGI_FORMAT tex) {
  switch (tex) {
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
inline DXGI_FORMAT DepthDsvFmt(DXGI_FORMAT tex) {
  switch (tex) {
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_D32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
// A view's texture, mip / slice (the view's first) and the texture's description.
struct Sub {
  ID3D11Texture2D* tex = nullptr;  // not AddRef'd (the view keeps it)
  UINT mip = 0, slice = 0, sub = 0;
  D3D11_TEXTURE2D_DESC d{};
  bool ok = false;
};
inline Sub SubOf(ID3D11View* v, UINT mip, UINT slice) {
  Sub s;
  if (!v) return s;
  ID3D11Resource* r = nullptr;
  v->GetResource(&r);
  if (!r) return s;
  r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&s.tex));
  r->Release();
  if (!s.tex) return s;
  s.tex->Release();
  s.tex->GetDesc(&s.d);
  s.mip = mip, s.slice = slice;
  s.sub = D3D11CalcSubresource(mip, slice, s.d.MipLevels);
  s.ok = s.d.SampleDesc.Count == 1;
  return s;
}
inline Sub SubOfRtv(ID3D11RenderTargetView* v, DXGI_FORMAT* fmt) {
  D3D11_RENDER_TARGET_VIEW_DESC d{};
  v->GetDesc(&d);
  if (fmt) *fmt = d.Format;
  if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) return SubOf(v, d.Texture2D.MipSlice, 0);
  if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY && d.Texture2DArray.ArraySize == 1)
    return SubOf(v, d.Texture2DArray.MipSlice, d.Texture2DArray.FirstArraySlice);
  return Sub{};
}
inline Sub SubOfDsv(ID3D11DepthStencilView* v, DXGI_FORMAT* fmt) {
  D3D11_DEPTH_STENCIL_VIEW_DESC d{};
  v->GetDesc(&d);
  if (fmt) *fmt = d.Format;
  if (d.Flags) return Sub{};  // read-only DSVs are not expected in a G-buffer pass
  if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D) return SubOf(v, d.Texture2D.MipSlice, 0);
  if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY && d.Texture2DArray.ArraySize == 1)
    return SubOf(v, d.Texture2DArray.MipSlice, d.Texture2DArray.FirstArraySlice);
  return Sub{};
}
// One-slice array SRV / RTV / DSV of (tex, mip, slice).
inline ID3D11ShaderResourceView* SliceSrv(ID3D11Texture2D* t, DXGI_FORMAT f, UINT mip, UINT slice) {
  D3D11_SHADER_RESOURCE_VIEW_DESC d{};
  d.Format = f;
  d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
  d.Texture2DArray.MostDetailedMip = mip;
  d.Texture2DArray.MipLevels = 1;
  d.Texture2DArray.FirstArraySlice = slice;
  d.Texture2DArray.ArraySize = 1;
  ID3D11ShaderResourceView* v = nullptr;
  pifilt::g_dev->CreateShaderResourceView(t, &d, &v);
  return v;
}
inline ID3D11RenderTargetView* SliceRtv(ID3D11Texture2D* t, DXGI_FORMAT f, UINT mip, UINT slice, UINT count) {
  D3D11_RENDER_TARGET_VIEW_DESC d{};
  d.Format = f;
  d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
  d.Texture2DArray.MipSlice = mip;
  d.Texture2DArray.FirstArraySlice = slice;
  d.Texture2DArray.ArraySize = count;
  ID3D11RenderTargetView* v = nullptr;
  pifilt::g_dev->CreateRenderTargetView(t, &d, &v);
  return v;
}
inline ID3D11DepthStencilView* SliceDsv(ID3D11Texture2D* t, DXGI_FORMAT f, UINT mip, UINT slice, UINT count) {
  D3D11_DEPTH_STENCIL_VIEW_DESC d{};
  d.Format = f;
  d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
  d.Texture2DArray.MipSlice = mip;
  d.Texture2DArray.FirstArraySlice = slice;
  d.Texture2DArray.ArraySize = count;
  ID3D11DepthStencilView* v = nullptr;
  pifilt::g_dev->CreateDepthStencilView(t, &d, &v);
  return v;
}
// The raw view format of a target: the family's integer format when the texture is typeless (bits copied exactly;
// the motion-vector target keeps its float format), else the texture's own format.
inline DXGI_FORMAT RawFmt(DXGI_FORMAT texFmt, DXGI_FORMAT rtvFmt, bool mv) {
  if (mv) return rtvFmt;
  if (!IsTypeless(texFmt)) return texFmt;
  const Fam f = FamilyOf(texFmt);
  return f.raw != DXGI_FORMAT_UNKNOWN ? f.raw : rtvFmt;
}

// Views of DCS's textures we create (keyed by DCS's view; the resource pointer guards against a recycled address).
struct DcsViews {
  ID3D11Resource* res = nullptr;  // AddRef'd
  ID3D11ShaderResourceView *dsrv = nullptr, *ssrv = nullptr;  // depth / stencil of a DSV's subresource
  ID3D11DepthStencilView* sentinel = nullptr;                 // the same subresource, our own view object
  ID3D11RenderTargetView* raw = nullptr;                      // an RTV's subresource in the raw format
  ID3D11ShaderResourceView* rawSrv = nullptr;
  DXGI_FORMAT rawFmt = DXGI_FORMAT_UNKNOWN;
  void Release() { Rel(res), Rel(dsrv), Rel(ssrv), Rel(sentinel), Rel(raw), Rel(rawSrv); }
};
std::unordered_map<void*, DcsViews> g_dcsViews;
inline void ClearDcsViews() {
  for (auto& kv : g_dcsViews) kv.second.Release();
  g_dcsViews.clear();
}
inline DcsViews* ViewsOf(ID3D11View* v) {
  ID3D11Resource* r = nullptr;
  v->GetResource(&r);
  if (!r) return nullptr;
  r->Release();
  auto it = g_dcsViews.find(v);
  if (it != g_dcsViews.end()) {
    if (it->second.res == r) return &it->second;
    it->second.Release();
    g_dcsViews.erase(it);
  }
  if (g_dcsViews.size() >= 64) ClearDcsViews();
  DcsViews& n = g_dcsViews[v];
  n.res = r;
  r->AddRef();
  return &n;
}
inline DcsViews* DsvViews(ID3D11DepthStencilView* dsv) {
  DcsViews* w = ViewsOf(dsv);
  if (!w || w->sentinel) return w;
  DXGI_FORMAT f;
  const Sub s = SubOfDsv(dsv, &f);
  if (!s.ok) return w;
  if (s.d.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
    w->dsrv = SliceSrv(s.tex, DepthSrvFmt(s.d.Format), s.mip, s.slice);
    if (s.d.Format == DXGI_FORMAT_R32G8X24_TYPELESS)
      w->ssrv = SliceSrv(s.tex, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT, s.mip, s.slice);
  }
  w->sentinel = SliceDsv(s.tex, f, s.mip, s.slice, 1);
  return w;
}
inline DcsViews* RtvViews(ID3D11RenderTargetView* rtv, DXGI_FORMAT raw) {
  DcsViews* w = ViewsOf(rtv);
  if (!w) return w;
  if (w->raw && w->rawFmt == raw) return w;
  Rel(w->raw), Rel(w->rawSrv);
  DXGI_FORMAT f;
  const Sub s = SubOfRtv(rtv, &f);
  if (!s.ok) return w;
  w->rawFmt = raw;
  w->raw = SliceRtv(s.tex, raw, s.mip, s.slice, 1);
  if (s.d.BindFlags & D3D11_BIND_SHADER_RESOURCE) w->rawSrv = SliceSrv(s.tex, raw, s.mip, s.slice);
  return w;
}
// Our arrays for an eye: focus size w x h, the layout of DCS's periphery targets (formats per target, depth format).
// Recreated when the layout changes. False: unsupported layout (counted by the caller).
bool EnsureArr(int e, UINT w, UINT h, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
  Arr& a = g_arr[e];
  DXGI_FORMAT texF[8] = {}, rtvF[8] = {};
  for (UINT k = 0; k < n; ++k) {
    const Sub s = SubOfRtv(rtv[k], &rtvF[k]);
    if (!s.ok) return false;
    texF[k] = s.d.Format;
  }
  DXGI_FORMAT dsvF;
  const Sub ds = SubOfDsv(dsv, &dsvF);
  if (!ds.ok || DepthDsvFmt(ds.d.Format) != dsvF || DepthSrvFmt(ds.d.Format) == DXGI_FORMAT_UNKNOWN) return false;
  bool same = a.tex[0] && a.w == w && a.h == h && a.n == n && a.dTexFmt == ds.d.Format;
  for (UINT k = 0; same && k < n; ++k) same = a.texFmt[k] == texF[k] && a.rtvFmt[k] == rtvF[k];
  if (same) return true;
  a.Release();
  int mv = -1, mvCount = 0;
  for (UINT k = 0; k < n; ++k)
    if (rtvF[k] == DXGI_FORMAT_R16G16_FLOAT || rtvF[k] == DXGI_FORMAT_R16G16B16A16_FLOAT) mv = static_cast<int>(k), ++mvCount;
  if (mvCount != 1) return false;
  ID3D11Device* dev = pifilt::g_dev;
  bool ok = true;
  for (UINT k = 0; ok && k < n; ++k) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w, td.Height = h, td.MipLevels = 1, td.ArraySize = 2, td.Format = texF[k];
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &a.tex[k]));
    a.texFmt[k] = texF[k], a.rtvFmt[k] = rtvF[k];
    a.rawFmt[k] = RawFmt(texF[k], rtvF[k], static_cast<int>(k) == mv);
    if (ok) {
      a.rtv2[k] = SliceRtv(a.tex[k], rtvF[k], 0, 0, 2);
      a.srv0[k] = SliceSrv(a.tex[k], a.rawFmt[k], 0, 0);
      a.srv1[k] = SliceSrv(a.tex[k], a.rawFmt[k], 0, 1);
      ok = a.rtv2[k] && a.srv0[k] && a.srv1[k];
    }
  }
  if (ok) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w, td.Height = h, td.MipLevels = 1, td.ArraySize = 2, td.Format = ds.d.Format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &a.dtex));
    a.dTexFmt = ds.d.Format, a.dDsvFmt = dsvF;
    a.stencil = ds.d.Format == DXGI_FORMAT_R32G8X24_TYPELESS;
    if (ok) {
      a.dsv2 = SliceDsv(a.dtex, dsvF, 0, 0, 2);
      a.dsv0 = SliceDsv(a.dtex, dsvF, 0, 0, 1);
      a.dsv1 = SliceDsv(a.dtex, dsvF, 0, 1, 1);
      a.dsrv0 = SliceSrv(a.dtex, DepthSrvFmt(ds.d.Format), 0, 0);
      a.dsrv1 = SliceSrv(a.dtex, DepthSrvFmt(ds.d.Format), 0, 1);
      if (a.stencil) a.ssrv0 = SliceSrv(a.dtex, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT, 0, 0);
      ok = a.dsv2 && a.dsv0 && a.dsv1 && a.dsrv0 && a.dsrv1 && (!a.stencil || a.ssrv0);
    }
  }
  if (ok) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 128, bd.Usage = D3D11_USAGE_DEFAULT, bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &a.tm));
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT, sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER, sd.Buffer.NumElements = 8;
    ok = ok && SUCCEEDED(dev->CreateShaderResourceView(a.tm, &sd, &a.tmSrv));
  }
  if (!ok) {
    a.Release();
    g_c.resFail.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  a.w = w, a.h = h, a.n = n, a.mv = mv;
  std::string fm;
  for (UINT k = 0; k < n; ++k)
    fm += (k ? " " : "") + std::to_string(static_cast<int>(a.texFmt[k])) + "/" + std::to_string(static_cast<int>(a.rtvFmt[k])) +
          "/" + std::to_string(static_cast<int>(a.rawFmt[k]));
  Log("focus broadcast: eye %d targets %ux%u x2 slices: %u colour targets (motion vectors %d; texture/RTV/raw DXGI formats "
      "%s), depth format %d%s",
      e, w, h, n, mv, fm.c_str(), static_cast<int>(a.dTexFmt), a.stencil ? " with stencil" : "");
  return true;
}

// ---------------------------------------------------------------------------
// TerrainContext upload observer (one-shot per G-buffer pass; never over another hook)
// ---------------------------------------------------------------------------
using UsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT,
                                      UINT);
using Us1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*,
                                       UINT, UINT, UINT);
int g_usSlot = -1, g_us1Slot = -1;
void** g_usVt = nullptr;
void* volatile g_usOrig = nullptr;
void* volatile g_us1Orig = nullptr;
Tc g_cap;                  // the last TerrainContext upload seen (rows 0..47)
uint64_t g_capSerial = 0;  // the G-buffer pass serial it was seen in
uint64_t g_passSerial = 0;
bool g_usArmed = false;
std::atomic<uint64_t> g_capPartial{0};

void STDMETHODCALLTYPE H_Us(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src,
                            UINT rp, UINT dp);
void STDMETHODCALLTYPE H_Us1(ID3D11DeviceContext1* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                             const void* src, UINT rp, UINT dp, UINT fl);
inline void DisarmUs() {
  if (!g_usVt) return;
  if (g_usVt[g_usSlot] == reinterpret_cast<void*>(&H_Us)) d3ds::WriteTablePointer(&g_usVt[g_usSlot], g_usOrig);
  if (g_usVt[g_us1Slot] == reinterpret_cast<void*>(&H_Us1)) d3ds::WriteTablePointer(&g_usVt[g_us1Slot], g_us1Orig);
  g_usArmed = false;
}
inline void Capture(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src) {
  if (c != pifilt::g_ctx || !src || sub || !r || r != static_cast<ID3D11Resource*>(pifilt::g_d.tc)) return;
  if (box && (box->left != 0 || box->right < sizeof(g_cap.r))) {
    g_capPartial.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  __try {
    memcpy(g_cap.r, src, sizeof(g_cap.r));
    g_cap.ok = true;
    g_capSerial = g_passSerial;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_cap.ok = false;
  }
  DisarmUs();  // one upload per pass is all we need (the hook's caller holds the saved original)
}
void STDMETHODCALLTYPE H_Us(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src,
                            UINT rp, UINT dp) {
  const UsFn orig = reinterpret_cast<UsFn>(g_usOrig);
  if (GetCurrentThreadId() == ptiming::g_topTid.load(std::memory_order_relaxed)) Capture(c, r, sub, box, src);
  orig(c, r, sub, box, src, rp, dp);
}
void STDMETHODCALLTYPE H_Us1(ID3D11DeviceContext1* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                             const void* src, UINT rp, UINT dp, UINT fl) {
  const Us1Fn orig = reinterpret_cast<Us1Fn>(g_us1Orig);
  if (GetCurrentThreadId() == ptiming::g_topTid.load(std::memory_order_relaxed))
    Capture(static_cast<ID3D11DeviceContext*>(c), r, sub, box, src);
  orig(c, r, sub, box, src, rp, dp, fl);
}
// Fixation depth's upload hook holds the slot (CockpitTrueIpd, far depth): it reports DCS's uploads here.
void ObserveFd(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src) {
  if (GetCurrentThreadId() == ptiming::g_topTid.load(std::memory_order_relaxed)) Capture(c, r, sub, box, src);
}
// At a G-buffer pass begin: both upload entries, only over d3d11's own (R48: another hook of ours re-arms by adopting
// whatever it finds; fixation depth's upload arm then simply wins and this pass is not captured, counted).
inline void ArmUs() {
  if (g_usSlot < 0) {
    g_usSlot = DcsQv_ContextSlot("UpdateSubresource");
    g_us1Slot = DcsQv_ContextSlot("UpdateSubresource1");
  }
  if (g_usSlot < 0 || g_us1Slot < 0 || !pifilt::g_ctx || !pifilt::g_d.tc) return;
  void** vt = *reinterpret_cast<void***>(pifilt::g_ctx);
  void* a = vt[g_usSlot];
  void* b = vt[g_us1Slot];
  if (a == reinterpret_cast<void*>(&H_Us) || b == reinterpret_cast<void*>(&H_Us1)) {
    g_usArmed = true;
    return;
  }
  if (!fixdepth::InD3d11(a) || !fixdepth::InD3d11(b)) {
    g_c.tcForeign.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_usVt = vt;
  g_usOrig = a;
  g_us1Orig = b;
  d3ds::WriteTablePointer(&vt[g_usSlot], reinterpret_cast<void*>(&H_Us));
  d3ds::WriteTablePointer(&vt[g_us1Slot], reinterpret_cast<void*>(&H_Us1));
  g_usArmed = true;
}

// ---------------------------------------------------------------------------
// Per-eye render-thread state
// ---------------------------------------------------------------------------
struct Key {
  void* vs;
  void* args;
  UINT off;
  int kind;
  bool operator==(const Key& o) const { return vs == o.vs && args == o.args && off == o.off && kind == o.kind; }
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    uint64_t h = reinterpret_cast<uint64_t>(k.vs) * 0x9E3779B97F4A7C15ull;
    h ^= reinterpret_cast<uint64_t>(k.args) + 0x7f4a7c15ull + (h << 6) + (h >> 2);
    h ^= (static_cast<uint64_t>(k.off) << 2 | static_cast<uint64_t>(k.kind)) + (h << 6) + (h >> 2);
    return static_cast<size_t>(h);
  }
};
using KeySet = std::unordered_set<Key, KeyHash>;
struct EyeRt {
  Tc pCur, fCur, pPrev, fPrev;  // TerrainContext of the main passes, this frame and the previous
  fbc::Affine affCur, affPrev;   // focus-of-periphery affine (DCS's own matrices) this / previous frame
  bool haveAffCur = false, haveAffPrev = false;
  // periphery
  int gb = -1, mainGb = -1;
  int hooked[4] = {}, hookedPrev[4] = {};
  int ord = 0;
  bool capTaken = false;
  bool learn = false, learnArmed = false, learnedOnce = false;
  ID3D11DepthStencilView* learnDsv = nullptr;  // DCS's (weak)
  StartLearn sl;
  bool block = false, gsSet = false, bcast = false, refused = false, slice1Cleared = false;
  Pred pred;
  fbc::Affine aff;
  double wp = 0, hp = 0, wf = 0, hf = 0;
  D3D11_VIEWPORT vp[2] = {};
  bool scissor = false;
  D3D11_RECT sc[2] = {};
  D3D11_RECT dcsSc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
  UINT ndcsSc = 0;
  ID3D11RenderTargetView* dcsRtv[8] = {};
  UINT nRtv = 0;
  ID3D11DepthStencilView* dcsDsv = nullptr;
  ID3D11DepthStencilState* dss = nullptr;
  UINT sref = 0;
  ID3D11BlendState* bs = nullptr;
  uint32_t masks = 0;
  float depthClear = 0;
  bool dssGreater = true;
  KeySet keys, fKeys, fKeysPrev;
  // focus
  int fgb = -1;
  uint64_t pFrame = ~0ull;  // the frame / view index / render size of the periphery classified for this eye
  int pView = -1;
  float pW = 0, pH = 0;
  bool fCapTaken = false, merged = false, mergeFailed = false;
  // DCS rebinds its default frame buffer at a G-buffer pass end (pushFrameBuffer / pop): the block's targets are
  // then no longer bound at our pass-end boundary. Exact when nothing was drawn after the last hooked draw: learning
  // frames measure that interval with a pipeline-statistics query (0 unknown, 1 nothing drawn, 2 draws seen).
  int tailState = 0;
  ID3D11Query* tailQ = nullptr;
  bool tailActive = false, tailPending = false;
  void ReleaseBlockRefs() {  // DCS's targets (the block's states stay for the focus check until the frame ends)
    for (auto& p : dcsRtv) Rel(p);
    Rel(dcsDsv);
    nRtv = 0;
  }
  void ReleaseFrameRefs() {
    ReleaseBlockRefs();
    Rel(dss), Rel(bs);
  }
  void ReleaseAll() {
    ReleaseFrameRefs();
    Rel(tailQ);
  }
};
EyeRt g_eye[2];
bool g_viewsSeen = false;
uint64_t g_frame = 0;
std::atomic<bool> g_on{false};  // broadcast wanted (render thread reads)
std::atomic<bool> g_installed{false};

// ---------------------------------------------------------------------------
// Probe (census rows; render thread appends, the suite thread writes the files)
// ---------------------------------------------------------------------------
std::mutex g_probeMu;
std::vector<std::string> g_rowsDraw, g_rowsTc, g_rowsCi, g_rowsCb;
int g_probeDrawFrames = 0;  // frames of draw rows still to record
uint64_t g_probeFrameTag = 0;
struct CbDump {
  ID3D11Buffer* st = nullptr;
  int view = 0, stage = 0, slot = 0;
  UINT first = 0, num = 0, bytes = 0;
  uint64_t frame = 0;
};
std::vector<CbDump> g_cbPending;
bool g_cbWanted[4] = {};
inline std::string F4(const float* v) {
  char b[96];
  snprintf(b, sizeof(b), "%.9g,%.9g,%.9g,%.9g", v[0], v[1], v[2], v[3]);
  return b;
}
void ProbeDraw(ID3D11DeviceContext* c, int view, int gb, int ord, int kind, ID3D11VertexShader* vs, ID3D11Buffer* args,
               UINT off, const Obj* o) {
  ID3D11GeometryShader* gs = nullptr;
  ID3D11HullShader* hs = nullptr;
  ID3D11DomainShader* ds = nullptr;
  c->GSGetShader(&gs, nullptr, nullptr);
  c->HSGetShader(&hs, nullptr, nullptr);
  c->DSGetShader(&ds, nullptr, nullptr);
  D3D11_PRIMITIVE_TOPOLOGY topo{};
  c->IAGetPrimitiveTopology(&topo);
  ID3D11DepthStencilState* dss = nullptr;
  UINT ref = 0;
  c->OMGetDepthStencilState(&dss, &ref);
  D3D11_DEPTH_STENCIL_DESC dd{};
  if (dss) dss->GetDesc(&dd);
  ID3D11BlendState* bs = nullptr;
  float bf[4];
  UINT sm = 0;
  c->OMGetBlendState(&bs, bf, &sm);
  D3D11_BLEND_DESC bd{};
  if (bs) bs->GetDesc(&bd);
  ID3D11RasterizerState* rs = nullptr;
  c->RSGetState(&rs);
  D3D11_RASTERIZER_DESC rd{};
  if (rs) rs->GetDesc(&rd);
  D3D11_VIEWPORT vp[2] = {};
  UINT nvp = 2;
  c->RSGetViewports(&nvp, vp);
  ID3D11RenderTargetView* rtv[8] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  c->OMGetRenderTargets(8, rtv, &dsv);
  int nrtv = 0;
  for (int i = 0; i < 8; ++i)
    if (rtv[i]) nrtv = i + 1;
  uint32_t masks = 0;
  for (int k = 0; k < 8; ++k)
    masks |= (bd.RenderTarget[bd.IndependentBlendEnable ? k : 0].RenderTargetWriteMask & 0xfu) << (4 * k);
  char b[640];
  snprintf(b, sizeof(b),
           "%llu,%d,%d,%d,%d,%p,%p,%u,%d,%d,%d,%d,%d,%d,%p,%u,%d,%d,%d,%d,%u,%u,%d,%d,%d,%p,%d,%d,%d,0x%08x,%d,%d,%u,%.3f,%.3f,"
           "%.3f,%.3f,%d,%p,%p,%p",
           static_cast<unsigned long long>(g_frame), view, gb, ord, kind, static_cast<void*>(vs), static_cast<void*>(args),
           off, o ? o->st.load() : -1, o ? o->cat : 0, gs ? 1 : 0, hs ? 1 : 0, ds ? 1 : 0, static_cast<int>(topo),
           static_cast<void*>(dss), ref, dd.DepthEnable, static_cast<int>(dd.DepthWriteMask), static_cast<int>(dd.DepthFunc),
           dd.StencilEnable, dd.StencilReadMask, dd.StencilWriteMask, static_cast<int>(dd.FrontFace.StencilFunc),
           static_cast<int>(dd.FrontFace.StencilPassOp), static_cast<int>(dd.FrontFace.StencilDepthFailOp),
           static_cast<void*>(bs), bd.AlphaToCoverageEnable, bd.IndependentBlendEnable, bd.RenderTarget[0].BlendEnable, masks,
           static_cast<int>(rd.CullMode), rd.ScissorEnable, nvp, vp[0].TopLeftX, vp[0].TopLeftY, vp[0].Width, vp[0].Height,
           nrtv, static_cast<void*>(rtv[0]), static_cast<void*>(rtv[nrtv > 0 ? nrtv - 1 : 0]), static_cast<void*>(dsv));
  Rel(gs), Rel(hs), Rel(ds), Rel(dss), Rel(bs), Rel(rs), Rel(dsv);
  for (auto& p : rtv) Rel(p);
  std::lock_guard<std::mutex> lock(g_probeMu);
  g_rowsDraw.push_back(b);
}
// Copies of every constant buffer bound to the VS and PS (slots 0-13, with their *1 range) for one draw.
void ProbeCbs(ID3D11DeviceContext* c, int view) {
  ID3D11DeviceContext1* c1 = g_dev.c1;
  if (!c1) pifilt::g_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&g_dev.c1)), c1 = g_dev.c1;
  for (int stage = 0; stage < 2; ++stage) {
    ID3D11Buffer* b[14] = {};
    UINT first[14] = {}, num[14] = {};
    if (c1) {
      if (stage == 0) c1->VSGetConstantBuffers1(0, 14, b, first, num);
      else c1->PSGetConstantBuffers1(0, 14, b, first, num);
    } else if (stage == 0) {
      c->VSGetConstantBuffers(0, 14, b);
    } else {
      c->PSGetConstantBuffers(0, 14, b);
    }
    for (int s = 0; s < 14; ++s) {
      if (!b[s]) continue;
      D3D11_BUFFER_DESC bd{};
      b[s]->GetDesc(&bd);
      const bool offs = !(first[s] == 0 && (num[s] == 0 || num[s] == 4096));
      const UINT lo = offs ? first[s] * 16 : 0;
      const UINT hi = std::min(bd.ByteWidth, offs ? (first[s] + num[s]) * 16 : bd.ByteWidth);
      if (hi <= lo || hi - lo > 65536) {
        b[s]->Release();
        continue;
      }
      CbDump d;
      D3D11_BUFFER_DESC sd{};
      sd.ByteWidth = hi - lo;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (SUCCEEDED(pifilt::g_dev->CreateBuffer(&sd, nullptr, &d.st))) {
        const D3D11_BOX box{lo, 0, 0, hi, 1, 1};
        c->CopySubresourceRegion(d.st, 0, 0, 0, 0, b[s], 0, &box);
        d.view = view, d.stage = stage, d.slot = s, d.first = first[s], d.num = num[s], d.bytes = hi - lo;
        d.frame = g_frame;
        g_cbPending.push_back(d);
      }
      b[s]->Release();
    }
  }
}
void PollCbs(ID3D11DeviceContext* c) {
  for (size_t i = 0; i < g_cbPending.size();) {
    CbDump& d = g_cbPending[i];
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = c->Map(d.st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
      ++i;
      continue;
    }
    if (SUCCEEDED(hr)) {
      std::string row = std::to_string(d.frame) + "," + std::to_string(d.view) + "," + (d.stage ? "PS" : "VS") + "," +
                        std::to_string(d.slot) + "," + std::to_string(d.first) + "," + std::to_string(d.bytes) + ",";
      const uint8_t* p = static_cast<const uint8_t*>(m.pData);
      static const char hx[] = "0123456789abcdef";
      for (UINT k = 0; k < d.bytes; ++k) row += hx[p[k] >> 4], row += hx[p[k] & 15];
      c->Unmap(d.st, 0);
      std::lock_guard<std::mutex> lock(g_probeMu);
      g_rowsCb.push_back(row);
    }
    d.st->Release();
    g_cbPending.erase(g_cbPending.begin() + i);
  }
}
void ProbeTc(int view, int gb, const Tc& t) {
  std::string row = std::to_string(g_frame) + "," + std::to_string(view) + "," + std::to_string(gb);
  for (int r = 16; r <= 23; ++r) row += "," + F4(t.r[r]);
  row += "," + F4(t.r[38]) + "," + F4(t.r[44]);
  std::lock_guard<std::mutex> lock(g_probeMu);
  if (g_rowsTc.size() < 20000) g_rowsTc.push_back(row);
}

// Collect-time view-projections of the quad views (main.cpp's culling hook; any thread).
struct CiView {
  int index = -1, role = 0, partner = -1;
  float vp[16] = {};
};
void OnCollect(const uint8_t* infos, const int* index, const int* role, const int* partner, size_t n) {
  if (!g_probe.load(std::memory_order_relaxed) || !infos) return;
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  std::string row = std::to_string(q.QuadPart) + "," + std::to_string(g_frame) + "," + std::to_string(n);
  for (size_t i = 0; i < n && i < 8; ++i) {
    float vp[16];
    memcpy(vp, infos + static_cast<size_t>(index[i]) * kCollectionInfoStride + kCiViewProj, sizeof(vp));
    row += "," + std::to_string(index[i]) + "," + std::to_string(role[i]) + "," + std::to_string(partner[i]);
    for (float f : vp) {
      char b[24];
      snprintf(b, sizeof(b), ",%.9g", f);
      row += b;
    }
  }
  std::lock_guard<std::mutex> lock(g_probeMu);
  if (g_rowsCi.size() < 20000) g_rowsCi.push_back(row);
}

// ---------------------------------------------------------------------------
// Block (periphery) and merge (focus)
// ---------------------------------------------------------------------------
inline D3D11_VIEWPORT FullVp(double w, double h, const D3D11_VIEWPORT& like) {
  return D3D11_VIEWPORT{0, 0, static_cast<float>(w), static_cast<float>(h), like.MinDepth, like.MaxDepth};
}
std::atomic<bool> g_statsOn{false};  // [Suite] FocusBcastTiming=2 (pass statistics, own GPU timestamps)
// [Suite] FocusBcastTiming=2 also times our own work (GPU timestamps): copy-in, copy-back, merge (per frame, both eyes).
enum TsKind { kTsIn = 0, kTsBack = 1, kTsMerge = 2, kTsKinds = 3 };
struct TsFrame {
  ID3D11Query* dj = nullptr;
  ID3D11Query* q[16] = {};
  int kind[8] = {};
  int n = 0;
  bool open = false, busy = false;
};
TsFrame g_ts[4];
int g_tsCur = -1;
double g_tsSum[kTsKinds] = {};
uint64_t g_tsFrames = 0;
std::mutex g_tsMu;
inline void TsFrameBegin(ID3D11DeviceContext* c) {
  g_tsCur = -1;
  if (!g_statsOn.load(std::memory_order_relaxed)) return;
  for (int i = 0; i < 4 && g_tsCur < 0; ++i)
    if (!g_ts[i].busy && !g_ts[i].open) g_tsCur = i;
  if (g_tsCur < 0) return;
  TsFrame& f = g_ts[g_tsCur];
  if (!f.dj) {
    D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    pifilt::g_dev->CreateQuery(&d, &f.dj);
    d.Query = D3D11_QUERY_TIMESTAMP;
    for (auto& q : f.q) pifilt::g_dev->CreateQuery(&d, &q);
  }
  if (!f.dj) {
    g_tsCur = -1;
    return;
  }
  c->Begin(f.dj);
  f.n = 0;
  f.open = true;
}
struct TsScope {
  ID3D11DeviceContext* c;
  int slot = -1;
  TsScope(ID3D11DeviceContext* cc, int kind) : c(cc) {
    if (g_tsCur < 0) return;
    TsFrame& f = g_ts[g_tsCur];
    if (f.n >= 8 || !f.q[2 * f.n]) return;
    slot = f.n++;
    f.kind[slot] = kind;
    c->End(f.q[2 * slot]);
  }
  ~TsScope() {
    if (slot >= 0 && g_tsCur >= 0) c->End(g_ts[g_tsCur].q[2 * slot + 1]);
  }
};
inline void TsFrameEnd(ID3D11DeviceContext* c) {
  if (g_tsCur < 0) return;
  TsFrame& f = g_ts[g_tsCur];
  c->End(f.dj);
  f.open = false;
  f.busy = true;
  g_tsCur = -1;
}
inline void TsPoll(ID3D11DeviceContext* c) {
  for (TsFrame& f : g_ts) {
    if (!f.busy) continue;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    if (c->GetData(f.dj, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
    bool all = true;
    double add[kTsKinds] = {};
    for (int s = 0; s < f.n && all; ++s) {
      UINT64 a = 0, b = 0;
      all = c->GetData(f.q[2 * s], &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            c->GetData(f.q[2 * s + 1], &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
      if (all && dj.Frequency && !dj.Disjoint && b >= a) add[f.kind[s]] += double(b - a) * 1000.0 / dj.Frequency;
    }
    if (!all) continue;
    f.busy = false;
    if (dj.Disjoint) continue;
    std::lock_guard<std::mutex> lock(g_tsMu);
    for (int k = 0; k < kTsKinds; ++k) g_tsSum[k] += add[k];
    ++g_tsFrames;
  }
}
void TsLog(const char* tag) {
  std::lock_guard<std::mutex> lock(g_tsMu);
  const double n = g_tsFrames ? double(g_tsFrames) : 1.0;
  Log("  focus broadcast own GPU work (%s, %llu frames, both eyes): copy-in %.3f ms, copy-back %.3f ms, merge %.3f ms per frame",
      tag, static_cast<unsigned long long>(g_tsFrames), g_tsSum[0] / n, g_tsSum[1] / n, g_tsSum[2] / n);
  g_tsSum[0] = g_tsSum[1] = g_tsSum[2] = 0;
  g_tsFrames = 0;
}

// gPrevFrameTransform (cPerView registers 32-35, PS b7 with its *1 range) of the current draw into rows `at`..at+3 of
// the eye's Tm buffer. False when no cPerView of DCS's size is bound.
bool CopyPrevTransform(ID3D11DeviceContext* c, Arr& A, UINT at) {
  ID3D11Buffer* b = nullptr;
  UINT first = 0, num = 0;
  if (g_dev.c1)
    g_dev.c1->PSGetConstantBuffers1(7, 1, &b, &first, &num);
  else
    c->PSGetConstantBuffers(7, 1, &b);
  if (!b) return false;
  const bool offs = !(first == 0 && (num == 0 || num == 4096));
  const UINT base = offs ? first * 16 : 0;
  D3D11_BUFFER_DESC bd{};
  b->GetDesc(&bd);
  const bool ok = A.tm && bd.ByteWidth >= base + 36 * 16 && (!offs || num * 16 >= 36 * 16);
  if (ok) {
    const D3D11_BOX box{base + 32 * 16, 0, 0, base + 36 * 16, 1, 1};
    c->CopySubresourceRegion(A.tm, 0, at * 16, 0, 0, b, 0, &box);
  }
  b->Release();
  return ok;
}
// DCS's periphery targets into slice 0, slice 1 cleared (once per frame).
void CopyIn(ID3D11DeviceContext* c, EyeRt& E, Arr& A, DcsViews* dv) {
  TsScope ts(c, kTsIn);
  Gfx s;
  s.Save(c);
  const D3D11_BOX box{0, 0, 0, static_cast<UINT>(E.wp), static_cast<UINT>(E.hp), 1};
  c->OMSetRenderTargets(0, nullptr, nullptr);
  for (UINT k = 0; k < A.n; ++k) {
    const Sub sk = SubOfRtv(E.dcsRtv[k], nullptr);
    c->CopySubresourceRegion(A.tex[k], 0, 0, 0, 0, sk.tex, sk.sub, &box);
  }
  const D3D11_VIEWPORT pv = FullVp(E.wp, E.hp, D3D11_VIEWPORT{0, 0, 0, 0, 0, 1});
  DepthPass(c, A.dsv0, dv->dsrv, pv);
  if (A.stencil) StencilPasses(c, A.dsv0, dv->ssrv, pv);
  if (!E.slice1Cleared) {
    c->ClearDepthStencilView(A.dsv1, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, E.depthClear, 0);
    E.slice1Cleared = true;
  }
  s.Restore(c);
}
// Slice 0 back into DCS's periphery targets; DCS's targets, viewport and scissors bound again.
void CopyBack(ID3D11DeviceContext* c, EyeRt& E, Arr& A, bool oursBound) {
  TsScope ts(c, kTsBack);
  DcsViews* dv = DsvViews(E.dcsDsv);
  Gfx s;
  s.Save(c);
  const D3D11_BOX box{0, 0, 0, static_cast<UINT>(E.wp), static_cast<UINT>(E.hp), 1};
  c->OMSetRenderTargets(0, nullptr, nullptr);
  for (UINT k = 0; k < A.n; ++k) {
    const Sub sk = SubOfRtv(E.dcsRtv[k], nullptr);
    c->CopySubresourceRegion(sk.tex, sk.sub, 0, 0, 0, A.tex[k], 0, &box);
  }
  const D3D11_VIEWPORT pv = FullVp(E.wp, E.hp, D3D11_VIEWPORT{0, 0, 0, 0, 0, 1});
  DepthPass(c, E.dcsDsv, A.dsrv0, pv);
  if (A.stencil && dv) StencilPasses(c, E.dcsDsv, A.ssrv0, pv);
  s.Restore(c);
  if (!oursBound) return;  // DCS bound its own frame buffer since (a pass end): its state stays
  c->OMSetRenderTargets(E.nRtv, E.dcsRtv, E.dcsDsv);
  c->RSSetViewports(1, &E.vp[0]);
  if (E.scissor) c->RSSetScissorRects(E.ndcsSc, E.dcsSc);
}
// A full-screen merge of (srcDepth, srcColour[]) into (rtv[] raw views, dsv) under the currently bound depth-stencil
// state (focus: DCS's own) or `dss` (abort: ours), DCS's write masks, motion vectors fixed up when mv >= 0.
void MergePass(ID3D11DeviceContext* c, EyeRt& E, Arr& A, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv,
               ID3D11ShaderResourceView* srcDepth, ID3D11ShaderResourceView* const* srcCol, int mv,
               ID3D11DepthStencilState* dss, const D3D11_VIEWPORT& vp, bool* ok) {
  *ok = false;
  ID3D11PixelShader* ps = MergePs(static_cast<int>(A.n), A.rawFmt, mv);
  ID3D11BlendState* bs = MaskBs(E.masks);
  if (!ps || !bs) return;
  TsScope ts(c, kTsMerge);
  Gfx s;
  s.Save(c);
  c->OMSetRenderTargets(A.n, rtv, dsv);
  if (dss) c->OMSetDepthStencilState(dss, 0);
  const float zero[4] = {};
  c->OMSetBlendState(bs, zero, 0xffffffffu);
  c->RSSetViewports(1, &vp);
  Params p{};
  p.cur[0] = static_cast<float>(E.aff.ax), p.cur[1] = static_cast<float>(E.aff.bx);
  p.cur[2] = static_cast<float>(E.aff.ay), p.cur[3] = static_cast<float>(E.aff.by);
  const fbc::Affine& pa = E.affPrev;
  p.prev[0] = static_cast<float>(pa.ax), p.prev[1] = static_cast<float>(pa.bx);
  p.prev[2] = static_cast<float>(pa.ay), p.prev[3] = static_cast<float>(pa.by);
  p.dims[0] = static_cast<float>(E.wp), p.dims[1] = static_cast<float>(E.hp);
  p.dims[2] = static_cast<float>(E.wf), p.dims[3] = static_cast<float>(E.hf);
  p.misc[0] = E.depthClear;
  PutCb(c, p);
  c->PSSetConstantBuffers(13, 1, &g_dev.cb);
  ID3D11ShaderResourceView* srv[10] = {};
  srv[0] = srcDepth;
  srv[1] = mv >= 0 ? A.tmSrv : nullptr;
  for (UINT k = 0; k < A.n; ++k) srv[2 + k] = srcCol[k];
  c->PSSetShaderResources(0, 10, srv);
  c->PSSetShader(ps, nullptr, 0);
  FullScreen(c);
  s.Restore(c);
  *ok = true;
}
// DCS rebound its targets inside the block: slice 0 merged into DCS's targets (depth-tested, NEAR for this frame),
// the start position learned past this draw. Slice 1 keeps what was broadcast so far.
void Abort(ID3D11DeviceContext* c, int e, int ord) {
  EyeRt& E = g_eye[e];
  Arr& A = g_arr[e];
  ID3D11RenderTargetView* raw[8] = {};
  bool ok = true;
  for (UINT k = 0; k < A.n && ok; ++k) {
    DcsViews* w = RtvViews(E.dcsRtv[k], A.rawFmt[k]);
    raw[k] = w ? w->raw : nullptr;
    ok = raw[k] != nullptr;
  }
  if (ok) {
    MergePass(c, E, A, raw, E.dcsDsv, A.dsrv0, A.srv0, -1, g_dev.dssMerge[E.dssGreater ? 0 : 1], FullVp(E.wp, E.hp, E.vp[0]),
              &ok);
  }
  // Our two viewports out (DCS's own, if it set none since).
  D3D11_VIEWPORT v[2] = {};
  UINT n = 2;
  c->RSGetViewports(&n, v);
  if (n == 2 && memcmp(&v[1], &E.vp[1], sizeof(v[1])) == 0) c->RSSetViewports(1, &v[0]);
  E.block = false;
  E.sl.Abort(ord);
  E.ReleaseBlockRefs();
  g_c.blockAborts.fetch_add(1, std::memory_order_relaxed);
  if (!ok) g_c.faults.fetch_add(1, std::memory_order_relaxed);
}
void Close(ID3D11DeviceContext* c, int e, bool passEnd = false) {
  EyeRt& E = g_eye[e];
  if (!E.block) return;
  Arr& A = g_arr[e];
  if (E.gsSet) {
    c->GSSetShader(nullptr, nullptr, 0);
    E.gsSet = false;
  }
  ID3D11DepthStencilView* d = nullptr;
  c->OMGetRenderTargets(0, nullptr, &d);
  const bool ours = d == A.dsv2;
  Rel(d);
  if (!ours && !(passEnd && E.tailState == 1)) {
    Abort(c, e, E.ord);
    return;
  }
  if (!ours) g_c.popCloses.fetch_add(1, std::memory_order_relaxed);
  CopyBack(c, E, A, ours);
  E.block = false;
  E.ReleaseBlockRefs();
  g_c.blocksClosed.fetch_add(1, std::memory_order_relaxed);
}
// At the first coverable draw of the periphery main pass (from the learned start on).
bool OpenBlock(ID3D11DeviceContext* c, int e, ID3D11DepthStencilState* dss, UINT ref, ID3D11BlendState* bs) {
  EyeRt& E = g_eye[e];
  if (!EnsureDev()) return false;
  if (!E.capTaken || !ProjShape(E.pCur)) {
    g_c.noTc.fetch_add(1, std::memory_order_relaxed);
    if (!E.capTaken) {  // still armed: the upload did not pass our entry (a d3d11 table toggle) or none came
      void** vt = *reinterpret_cast<void***>(c);
      (g_usArmed && g_usVt == vt && vt[g_usSlot] == reinterpret_cast<void*>(&H_Us) ? g_c.tcMissed : g_c.tcLost)
          .fetch_add(1, std::memory_order_relaxed);
    }
    return false;
  }
  E.wp = E.pCur.r[44][2], E.hp = E.pCur.r[44][3];
  E.wf = E.fPrev.r[44][2], E.hf = E.fPrev.r[44][3];
  if (!PredictFocus(E.pPrev, E.fPrev, E.pCur, E.wp, E.hp, E.wf, E.hf, E.pred) || !(E.wf > E.wp) || !(E.hf > E.hp)) {
    g_c.noPred.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  E.aff = AffineFromPred(E.pCur, E.pred);
  // DCS's targets: the periphery size, one sample, the layout our arrays support.
  ID3D11RenderTargetView* rtv[8] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  c->OMGetRenderTargets(8, rtv, &dsv);
  UINT n = 0;
  while (n < 8 && rtv[n]) ++n;
  bool ok = dsv && n > 0;
  for (UINT k = n; k < 8; ++k) ok = ok && !rtv[k];
  if (ok) {
    const Sub s0 = SubOfRtv(rtv[0], nullptr);
    ok = s0.ok && (s0.d.Width >> s0.mip) == static_cast<UINT>(E.wp) && (s0.d.Height >> s0.mip) == static_cast<UINT>(E.hp);
    if (!ok) g_c.viewBad.fetch_add(1, std::memory_order_relaxed);
  }
  DcsViews* dv = nullptr;
  if (ok && !EnsureArr(e, static_cast<UINT>(E.wf), static_cast<UINT>(E.hf), n, rtv, dsv)) {
    ok = false;
    g_c.fmtBad.fetch_add(1, std::memory_order_relaxed);
  }
  if (ok) {
    dv = DsvViews(dsv);
    ok = dv && dv->dsrv && (!g_arr[e].stencil || dv->ssrv);
    if (!ok) g_c.fmtBad.fetch_add(1, std::memory_order_relaxed);
  }
  // Depth-stencil and blend state of the covered draws (one per block).
  D3D11_DEPTH_STENCIL_DESC dd{};
  if (dss) {
    dss->GetDesc(&dd);
  } else {
    dd.DepthEnable = TRUE, dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL, dd.DepthFunc = D3D11_COMPARISON_LESS;
  }
  if (ok && (!dd.DepthEnable || dd.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ALL)) {
    ok = false;
    g_c.uncovState.fetch_add(1, std::memory_order_relaxed);
  }
  E.dssGreater = dd.DepthFunc == D3D11_COMPARISON_GREATER || dd.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL;
  E.depthClear = E.dssGreater ? 0.0f : 1.0f;
  uint32_t masks = 0;
  if (bs) {
    D3D11_BLEND_DESC bd{};
    bs->GetDesc(&bd);
    for (UINT k = 0; k < n; ++k) {
      const D3D11_RENDER_TARGET_BLEND_DESC& r = bd.RenderTarget[bd.IndependentBlendEnable ? k : 0];
      if (r.BlendEnable) ok = false;
      masks |= (r.RenderTargetWriteMask & 0xfu) << (4 * k);
    }
    if (!ok) g_c.uncovBlend.fetch_add(1, std::memory_order_relaxed);
  } else {
    for (UINT k = 0; k < n; ++k) masks |= 0xfu << (4 * k);
  }
  // Viewports and scissors.
  D3D11_VIEWPORT v[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
  UINT nv = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  c->RSGetViewports(&nv, v);
  if (ok && (nv != 1 || v[0].TopLeftX != 0 || v[0].TopLeftY != 0 || v[0].Width != static_cast<float>(E.wp) ||
             v[0].Height != static_cast<float>(E.hp))) {
    ok = false;
    g_c.viewBad.fetch_add(1, std::memory_order_relaxed);
  }
  if (ok) {
    E.vp[0] = v[0];
    ok = fbc::BroadcastViewport(E.aff, 0, 0, static_cast<float>(E.wf), static_cast<float>(E.hf), E.vp[1]);
    E.vp[1].MinDepth = v[0].MinDepth, E.vp[1].MaxDepth = v[0].MaxDepth;
    if (!ok) g_c.viewBad.fetch_add(1, std::memory_order_relaxed);
  }
  ID3D11RasterizerState* rs = nullptr;
  c->RSGetState(&rs);
  D3D11_RASTERIZER_DESC rd{};
  if (rs) rs->GetDesc(&rd);
  Rel(rs);
  E.scissor = rd.ScissorEnable != 0;
  if (ok && E.scissor) {
    E.ndcsSc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->RSGetScissorRects(&E.ndcsSc, E.dcsSc);
    E.sc[0] = E.ndcsSc ? E.dcsSc[0] : D3D11_RECT{0, 0, static_cast<LONG>(E.wp), static_cast<LONG>(E.hp)};
    E.sc[1] = D3D11_RECT{0, 0, static_cast<LONG>(E.wf), static_cast<LONG>(E.hf)};
  }
  if (!ok) {
    Rel(dsv);
    for (auto& p : rtv) Rel(p);
    return false;
  }
  // Keep DCS's targets and the block's states; slice 0 = DCS's targets, slice 1 cleared; ours bound.
  for (UINT k = 0; k < 8; ++k) E.dcsRtv[k] = rtv[k];
  E.nRtv = n;
  E.dcsDsv = dsv;
  if (E.bcast && (dss != E.dss || ref != E.sref || bs != E.bs)) {  // a reopened block: the frame's first block's states
    g_c.uncovState.fetch_add(1, std::memory_order_relaxed);
    E.ReleaseBlockRefs();
    return false;
  }
  if (!E.bcast) {
    Rel(E.dss), Rel(E.bs);
    E.dss = dss;
    if (dss) dss->AddRef();
    E.sref = ref;
    E.bs = bs;
    if (bs) bs->AddRef();
  }
  E.masks = masks;
  Arr& A = g_arr[e];
  if (!E.bcast && !CopyPrevTransform(c, A, 0)) {  // the periphery's gPrevFrameTransform (motion-vector fixup)
    g_c.noPrevT.fetch_add(1, std::memory_order_relaxed);
    E.ReleaseBlockRefs();
    return false;
  }
  CopyIn(c, E, A, dv);
  c->OMSetRenderTargets(A.n, A.rtv2, A.dsv2);
  c->RSSetViewports(2, E.vp);
  if (E.scissor) c->RSSetScissorRects(2, E.sc);
  E.block = true;
  E.bcast = true;
  g_c.blocks.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// Which view a G-buffer pass belongs to, from its own TerrainContext (not from the LightsBuffersPass ordinal: in F1
// the MFD sensor views render first on alternate frames and shift every ordinal). Periphery: projection x scale below
// 1.5, the eye by the sign of its centre (left eye positive), the same render size as that eye's periphery so far; a
// second periphery candidate of an eye in one frame (another view index) refuses the eye-frame. Focus: the eye whose
// periphery of this frame it matches (same camera, z / w rows, VP rows x / y an affine of the periphery's). Anything
// else (sensor views, mirrors) is left alone.
struct PassRt {
  int gb = -1, view = -1, role = -1, e = -1;  // role: 0 periphery, 1 focus, 2 other
  bool tried = false;
};
PassRt g_pr;
void ClassifyPass() {
  g_pr.tried = true;
  g_pr.role = 2;
  if (!(g_cap.ok && g_capSerial == g_passSerial) || !ProjShape(g_cap)) {
    g_c.noTc.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const Tc& t = g_cap;
  const float sx = t.r[16][0], cx = t.r[16][2], w = t.r[44][2], h = t.r[44][3];
  if (sx < 1.5f) {
    const int e = cx >= 0 ? 0 : 1;
    EyeRt& E = g_eye[e];
    if (E.pW > 0 && (w != E.pW || h != E.pH)) {
      g_c.viewBad.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (E.pFrame == g_frame && E.pView != g_pr.view) {  // two periphery candidates for one eye: refuse the eye-frame
      E.refused = true;
      E.mergeFailed = true;
      g_c.viewBad.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    E.pFrame = g_frame, E.pView = g_pr.view, E.pW = w, E.pH = h;
    g_pr.role = 0, g_pr.e = e;
    E.gb = g_pr.gb;
    E.ord = 0;
    E.learnArmed = false;
    E.pCur = t;
    E.capTaken = true;
    if (g_probe.load(std::memory_order_relaxed)) ProbeTc(e, E.gb, t);
    return;
  }
  for (int e = 0; e < 2; ++e) {
    EyeRt& E = g_eye[e];
    fbc::Affine a;
    double dev = 0;
    if (!E.pCur.ok || !AffineOf(E.pCur, t, a) || !SameEye(E.pCur, t, a, 2e-5, &dev)) continue;
    g_pr.role = 1, g_pr.e = e;
    E.fgb = g_pr.gb;
    E.fCur = t;
    E.fCapTaken = true;
    E.haveAffCur = true;
    E.affCur = a;
    if (g_probe.load(std::memory_order_relaxed)) ProbeTc(2 + e, E.fgb, t);
    return;
  }
}
// Periphery: before a hooked instancer draw (view e). Returns false always (DCS's draw is issued, into our block when
// open, with our FastGS when covered).
bool PeriphPre(ID3D11DeviceContext* c, int kind, ID3D11Buffer* args, UINT off, int e) {
  EyeRt& E = g_eye[e];
  const int ord = E.ord++;
  if (E.gb >= 0 && E.gb < 4) ++E.hooked[E.gb];
  ID3D11VertexShader* vs = nullptr;
  c->VSGetShader(&vs, nullptr, nullptr);
  if (!vs) return false;
  Obj* o = ObjOf(c, vs);
  vs->Release();  // the object holds its own reference
  if (o) {
    ++o->draws[kind & 1];
    TestStep(c, *o);
  }
  if (g_probeDrawFrames > 0) ProbeDraw(c, e, E.gb, ord, kind, o ? o->vs : nullptr, args, off, o);
  if (g_probe.load(std::memory_order_relaxed) && g_cbWanted[e] && o && o->cat == 1 && E.gb == E.mainGb) {
    g_cbWanted[e] = false;
    ProbeCbs(c, e);
  }
  const bool mainPass = E.gb == E.mainGb && E.gb >= 0;
  if (!mainPass) return false;
  if (E.learn) {  // learning frame: did DCS rebind since our sentinel went in after the previous hooked draw?
    ID3D11DepthStencilView* d = nullptr;
    c->OMGetRenderTargets(0, nullptr, &d);
    if (E.learnArmed) {
      DcsViews* w = E.learnDsv ? DsvViews(E.learnDsv) : nullptr;
      if (!w || d != w->sentinel) {
        E.sl.Saw(ord);
        g_c.rebinds.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (d && (!E.learnDsv || (d != E.learnDsv && !(DsvViews(E.learnDsv) && d == DsvViews(E.learnDsv)->sentinel))))
      E.learnDsv = d;
    Rel(d);
    E.learnArmed = false;
    return false;
  }
  if (!g_on.load(std::memory_order_relaxed) || E.refused) return false;
  if (E.block) {
    ID3D11DepthStencilView* d = nullptr;
    c->OMGetRenderTargets(0, nullptr, &d);
    const bool ours = d == g_arr[e].dsv2;
    Rel(d);
    if (!ours) {
      Abort(c, e, ord);
      return false;
    }
    D3D11_VIEWPORT v[2] = {};
    UINT nv = 2;
    c->RSGetViewports(&nv, v);
    if (nv != 2 || memcmp(v, E.vp, sizeof(v)) != 0) {
      if (nv >= 1) E.vp[0] = v[0];
      c->RSSetViewports(2, E.vp);
      if (E.scissor) c->RSSetScissorRects(2, E.sc);
      g_c.vpReset.fetch_add(1, std::memory_order_relaxed);
    }
  } else if (!E.learnedOnce || E.sl.start < 0 || ord < E.sl.start) {
    return false;
  } else if (E.tailState != 1) {
    (E.tailState == 2 ? g_c.tailDirty : g_c.noTail).fetch_add(1, std::memory_order_relaxed);
    E.refused = true;
    return false;
  }
  // Coverable: a known signature, a wanted family, a key the focus drew last frame, no other GS / HS / DS, triangles,
  // the block's depth-stencil / blend state.
  if (!o || o->st.load(std::memory_order_acquire) != kReady || !o->gs) {
    if (E.block) g_c.uncovNoSig.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!((o->cat == 1 ? 1 : 2) & g_kinds.load(std::memory_order_relaxed))) {
    if (E.block) g_c.uncovKind.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const Key key{o->vs, args, off, kind};
  if (!E.fKeysPrev.count(key)) return false;
  ID3D11GeometryShader* gs = nullptr;
  ID3D11HullShader* hs = nullptr;
  ID3D11DomainShader* ds = nullptr;
  c->GSGetShader(&gs, nullptr, nullptr);
  c->HSGetShader(&hs, nullptr, nullptr);
  c->DSGetShader(&ds, nullptr, nullptr);
  const bool other = gs || hs || ds;
  Rel(gs), Rel(hs), Rel(ds);
  if (other) {
    g_c.uncovGs.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  D3D11_PRIMITIVE_TOPOLOGY topo{};
  c->IAGetPrimitiveTopology(&topo);
  if (topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
    g_c.uncovTopo.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  ID3D11DepthStencilState* dss = nullptr;
  UINT ref = 0;
  c->OMGetDepthStencilState(&dss, &ref);
  ID3D11BlendState* bs = nullptr;
  float bf[4];
  UINT sm = 0;
  c->OMGetBlendState(&bs, bf, &sm);
  bool go = true;
  if (bs && !E.block) {  // blending on a written target: not mergeable; this draw stays DCS's (no refusal)
    D3D11_BLEND_DESC bd{};
    bs->GetDesc(&bd);
    for (int k = 0; k < 8 && go; ++k) go = !bd.RenderTarget[bd.IndependentBlendEnable ? k : 0].BlendEnable;
    if (!go) {
      g_c.uncovBlend.fetch_add(1, std::memory_order_relaxed);
      Rel(dss), Rel(bs);
      return false;
    }
  }
  if (!E.block) {
    go = OpenBlock(c, e, dss, ref, bs);
    if (!go) E.refused = true;  // nothing more this frame for this eye
  } else {
    go = dss == E.dss && ref == E.sref && bs == E.bs;
    if (!go) g_c.uncovState.fetch_add(1, std::memory_order_relaxed);
  }
  Rel(dss), Rel(bs);
  if (!go) return false;
  c->GSSetShader(o->gs, nullptr, 0);
  E.gsSet = true;
  E.keys.insert(key);
  ++o->bcast;
  g_c.covered.fetch_add(1, std::memory_order_relaxed);
  return false;
}
void PeriphPost(ID3D11DeviceContext* c, int e) {
  EyeRt& E = g_eye[e];
  if (E.gsSet) {
    c->GSSetShader(nullptr, nullptr, 0);
    E.gsSet = false;
  }
  if (E.learn && E.gb == E.mainGb) {  // learning: the interval after this hooked draw (the last one's reaches the pass end)
    if (!E.tailQ && !E.tailPending) {
      D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS, 0};
      pifilt::g_dev->CreateQuery(&qd, &E.tailQ);
    }
    if (E.tailQ && !E.tailPending) {
      if (E.tailActive) c->End(E.tailQ);
      c->Begin(E.tailQ);
      E.tailActive = true;
    }
  }
  if (E.learn && E.gb == E.mainGb && E.learnDsv) {  // learning: our sentinel view of DCS's depth in place of DCS's
    DcsViews* w = DsvViews(E.learnDsv);
    if (!w || !w->sentinel) return;
    ID3D11RenderTargetView* rtv[8] = {};
    ID3D11DepthStencilView* d = nullptr;
    c->OMGetRenderTargets(8, rtv, &d);
    UINT n = 0;
    for (UINT k = 0; k < 8; ++k)
      if (rtv[k]) n = k + 1;
    if (d == E.learnDsv) c->OMSetRenderTargets(n, rtv, w->sentinel);
    E.learnArmed = d == E.learnDsv || d == w->sentinel;
    Rel(d);
    for (auto& p : rtv) Rel(p);
  }
}
// ---------------------------------------------------------------------------
// [Suite] FocusBcastVerify: same-frame compare of the merged focus against DCS's own focus draws of the broadcast keys
// (every 4th frame, both eyes). At the merge point DCS's focus targets are copied to S; after the merge to M; every
// skipped key draw is then issued by DCS's own state into S (stock). At the focus pass end a compute pass compares S
// with M: covered pixels, G-buffer pixels that differ, per target max |diff| and squared error (PSNR), depth (count,
// relative > 1e-4, max relative), coverage, stencil, motion vectors (max |diff| where the same surface won).
// ---------------------------------------------------------------------------
constexpr int kVerStats = 64;
struct VerRes {
  UINT n = 0, w = 0, h = 0;
  DXGI_FORMAT texFmt[8] = {}, rtvFmt[8] = {}, rawFmt[8] = {}, dTexFmt = DXGI_FORMAT_UNKNOWN, dsvFmt = DXGI_FORMAT_UNKNOWN;
  ID3D11Texture2D *S[8] = {}, *M[8] = {}, *Sd = nullptr, *Md = nullptr;
  ID3D11RenderTargetView* Srtv[8] = {};
  ID3D11DepthStencilView* Sdsv = nullptr;
  ID3D11ShaderResourceView *Ssrv[8] = {}, *Msrv[8] = {}, *Sdsrv = nullptr, *Mdsrv = nullptr, *Sssrv = nullptr,
                           *Mssrv = nullptr;
  chainfast::TBuf stats;
  ID3D11Buffer* cb = nullptr;
  ID3D11Buffer* staging[4] = {};
  bool busy[4] = {};
  ID3D11ComputeShader* cs = nullptr;
  int mv = -1;
  void Release() {
    for (int k = 0; k < 8; ++k) Rel(S[k]), Rel(M[k]), Rel(Srtv[k]), Rel(Ssrv[k]), Rel(Msrv[k]);
    Rel(Sd), Rel(Md), Rel(Sdsv), Rel(Sdsrv), Rel(Mdsrv), Rel(Sssrv), Rel(Mssrv), Rel(cb), Rel(cs);
    stats.Release();
    for (auto& s : staging) Rel(s);
    *this = VerRes{};
  }
};
VerRes g_ver[2];
struct VerTotals {
  uint64_t frames = 0, covered = 0, depDiff = 0, depRel = 0, cover = 0, sten = 0, anyDiff = 0, mvBad = 0;
  uint64_t rtDiff[8] = {}, rtSq[8] = {};
  uint32_t rtMax[8] = {};
  double depMaxRel = 0, mvMax = 0;
  uint64_t lost = 0, drawn = 0;
};
VerTotals g_vt;
std::mutex g_vtMu;
bool g_verFrame[2] = {}, g_verRec[2] = {};

std::string CmpHlsl(UINT n, const DXGI_FORMAT* raw, int mv) {
  std::string decl, body;
  char line[1024];
  for (UINT k = 0; k < n; ++k) {
    const Fam f = FamilyOf(raw[k]);
    const int c = f.comps ? f.comps : 4;
    const bool fl = !IsIntFmt(raw[k]);
    const double un = UnormScale(raw[k]);
    const std::string t = std::string(fl ? "float" : "uint") + (c > 1 ? std::to_string(c) : "");
    snprintf(line, sizeof(line), "Texture2DArray<%s> S%u : register(t%u);\nTexture2DArray<%s> M%u : register(t%u);\n",
             t.c_str(), k, 4 + 2 * k, t.c_str(), k, 5 + 2 * k);
    decl += line;
    if (static_cast<int>(k) == mv) {
      snprintf(line, sizeof(line),
               "  { float2 va = S%u.Load(q).xy, vb = M%u.Load(q).xy;\n"
               "    float dd = max(abs(va.x - vb.x), abs(va.y - vb.y));\n"
               "    if (dd > 0.01) InterlockedAdd(st[%u], 1u);\n"
               "    if (a == b && ca) { InterlockedMax(st[40], asuint(dd)); if (dd > 0.05) InterlockedAdd(st[41], 1u); } }\n",
               k, k, 8 + k);
    } else if (fl && un <= 0) {
      snprintf(line, sizeof(line),
               "  { float%d va = S%u.Load(q), vb = M%u.Load(q);\n"
               "    if (any(va != vb)) { InterlockedAdd(st[%u], 1u); anyd = true; } }\n",
               c, k, k, 8 + k);
    } else {
      // integer bits, or a UNORM value in its integer steps (round(v * scale): exact for stored values)
      char ld[200];
      if (fl)
        snprintf(ld, sizeof(ld), "uint%d va = uint%d(round(S%u.Load(q) * %.1f)), vb = uint%d(round(M%u.Load(q) * %.1f));",
                 c, c, k, un, c, k, un);
      else
        snprintf(ld, sizeof(ld), "uint%d va = S%u.Load(q), vb = M%u.Load(q);", c, k, k);
      snprintf(line, sizeof(line),
               "  { %s\n"
               "    uint%d d = max(va, vb) - min(va, vb);\n"
               "    if (any(d != 0)) { InterlockedAdd(st[%u], 1u); anyd = true; }\n"
               "    uint mx = 0, sq = 0;\n"
               "    for (int i = 0; i < %d; ++i) { mx = max(mx, d[i]); sq += d[i] * d[i]; }\n"
               "    InterlockedMax(st[%u], mx); Add64(%u, sq); }\n",
               ld, c, 8 + k, c, 16 + k, 24 + 2 * k);
    }
    body += line;
  }
  return std::string(R"(
cbuffer V : register(b0) { float clearDepth; uint W; uint H; uint pad; };
Texture2DArray<float> SD : register(t0);
Texture2DArray<float> MD : register(t1);
Texture2DArray<uint2> SS : register(t2);
Texture2DArray<uint2> MS : register(t3);
RWBuffer<uint> st : register(u0);
void Add64(uint at, uint v) {
  if (v == 0) return;
  uint o;
  InterlockedAdd(st[at], v, o);
  if (o > 0xffffffffu - v) InterlockedAdd(st[at + 1], 1u);
}
)") + decl + R"(
[numthreads(8, 8, 1)]
void CsCmp(uint3 id : SV_DispatchThreadID) {
  if (id.x >= W || id.y >= H) return;
  int4 q = int4(int2(id.xy), 0, 0);
  float a = SD.Load(q), b = MD.Load(q);
  bool ca = a != clearDepth, cb = b != clearDepth;
  if (!ca && !cb) return;
  InterlockedAdd(st[0], 1u);
  if (ca != cb) InterlockedAdd(st[3], 1u);
  if (a != b) {
    InterlockedAdd(st[1], 1u);
    float rel = abs(a - b) / max(abs(a), abs(b));
    if (rel > 1e-4) InterlockedAdd(st[2], 1u);
    InterlockedMax(st[4], asuint(rel));
  }
  if (SS.Load(q).y != MS.Load(q).y) InterlockedAdd(st[43], 1u);
  bool anyd = false;
)" + body + "  if (anyd) InterlockedAdd(st[42], 1u);\n}\n";
}
// The verify copies for eye e at DCS's focus layout (created once).
bool EnsureVer(int e, const Arr& A, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
  VerRes& V = g_ver[e];
  if (V.cs && V.n == A.n && V.w == A.w && V.h == A.h) return true;
  V.Release();
  ID3D11Device* d = pifilt::g_dev;
  bool ok = true;
  for (UINT k = 0; ok && k < A.n; ++k) {
    DXGI_FORMAT rf;
    const Sub s = SubOfRtv(rtv[k], &rf);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = A.w, td.Height = A.h, td.MipLevels = 1, td.ArraySize = 1, td.Format = s.d.Format;
    td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ok = s.ok && SUCCEEDED(d->CreateTexture2D(&td, nullptr, &V.S[k])) && SUCCEEDED(d->CreateTexture2D(&td, nullptr, &V.M[k]));
    if (ok) {
      V.texFmt[k] = s.d.Format, V.rtvFmt[k] = rf, V.rawFmt[k] = A.rawFmt[k];
      V.Srtv[k] = SliceRtv(V.S[k], rf, 0, 0, 1);
      V.Ssrv[k] = SliceSrv(V.S[k], A.rawFmt[k], 0, 0);
      V.Msrv[k] = SliceSrv(V.M[k], A.rawFmt[k], 0, 0);
      ok = V.Srtv[k] && V.Ssrv[k] && V.Msrv[k];
    }
  }
  if (ok) {
    DXGI_FORMAT df;
    const Sub s = SubOfDsv(dsv, &df);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = A.w, td.Height = A.h, td.MipLevels = 1, td.ArraySize = 1, td.Format = s.d.Format;
    td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ok = s.ok && SUCCEEDED(d->CreateTexture2D(&td, nullptr, &V.Sd)) && SUCCEEDED(d->CreateTexture2D(&td, nullptr, &V.Md));
    if (ok) {
      V.dTexFmt = s.d.Format, V.dsvFmt = df;
      V.Sdsv = SliceDsv(V.Sd, df, 0, 0, 1);
      V.Sdsrv = SliceSrv(V.Sd, DepthSrvFmt(s.d.Format), 0, 0);
      V.Mdsrv = SliceSrv(V.Md, DepthSrvFmt(s.d.Format), 0, 0);
      const bool st = s.d.Format == DXGI_FORMAT_R32G8X24_TYPELESS;
      V.Sssrv = st ? SliceSrv(V.Sd, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT, 0, 0) : nullptr;
      V.Mssrv = st ? SliceSrv(V.Md, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT, 0, 0) : nullptr;
      ok = V.Sdsv && V.Sdsrv && V.Mdsrv;
    }
  }
  ok = ok && chainfast::MakeBuf(d, kVerStats, 0, nullptr, false, true, V.stats);
  if (ok) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16, bd.Usage = D3D11_USAGE_DYNAMIC, bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = SUCCEEDED(d->CreateBuffer(&bd, nullptr, &V.cb));
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = kVerStats * 4, sd.Usage = D3D11_USAGE_STAGING, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; ok && i < 4; ++i) ok = SUCCEEDED(d->CreateBuffer(&sd, nullptr, &V.staging[i]));
  }
  if (ok) {
    ID3DBlob* b = Compile(CmpHlsl(A.n, A.rawFmt, A.mv), "CsCmp", "cs_5_0");
    ok = b && SUCCEEDED(d->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &V.cs));
    if (b) b->Release();
    if (!ok) Log("focus broadcast verify: compare shader failed: %s", g_devErr.c_str());
  }
  if (!ok) {
    V.Release();
    return false;
  }
  V.n = A.n, V.w = A.w, V.h = A.h, V.mv = A.mv;
  return true;
}
// DCS's focus targets (as bound) into S (before the merge) or M (after it).
void VerCopy(ID3D11DeviceContext* c, int e, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv, bool toM) {
  VerRes& V = g_ver[e];
  for (UINT k = 0; k < V.n; ++k) {
    const Sub s = SubOfRtv(rtv[k], nullptr);
    c->CopySubresourceRegion(toM ? V.M[k] : V.S[k], 0, 0, 0, 0, s.tex, s.sub, nullptr);
  }
  const Sub s = SubOfDsv(dsv, nullptr);
  c->CopySubresourceRegion(toM ? V.Md : V.Sd, 0, 0, 0, 0, s.tex, s.sub, nullptr);
}
// A skipped key draw, issued stock into S (DCS's whole state, S's targets).
void VerDraw(ID3D11DeviceContext* c, int e, int kind, ID3D11Buffer* args, UINT off) {
  VerRes& V = g_ver[e];
  ID3D11RenderTargetView* rtv[8] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  c->OMGetRenderTargets(8, rtv, &dsv);
  c->OMSetRenderTargets(V.n, V.Srtv, V.Sdsv);
  // through the table: inside pifilt's hook (t_inside) its entry forwards straight to d3d11's
  if (kind == 0)
    c->DrawIndexedInstancedIndirect(args, off);
  else
    c->DrawInstancedIndirect(args, off);
  UINT n = 0;
  for (UINT k = 0; k < 8; ++k)
    if (rtv[k]) n = k + 1;
  c->OMSetRenderTargets(n, rtv, dsv);
  Rel(dsv);
  for (auto& p : rtv) Rel(p);
  ++g_vt.drawn;
}
// The focus pass end of a recorded eye: the compare dispatch, its stats copied to a free staging buffer.
void VerEnd(ID3D11DeviceContext* c, int e) {
  VerRes& V = g_ver[e];
  g_verRec[e] = false;
  int slot = -1;
  for (int i = 0; i < 4 && slot < 0; ++i)
    if (!V.busy[i]) slot = i;
  if (slot < 0 || !V.cs) {
    ++g_vt.lost;
    return;
  }
  pifilt::CsState cs;
  cs.Save(c);
  ID3D11ShaderResourceView* hi[12] = {};  // CS t8..t19 too (CsState keeps t0..t7)
  c->CSGetShaderResources(8, 12, hi);
  const UINT zero[4] = {};
  c->ClearUnorderedAccessViewUint(V.stats.uav, zero);
  struct {
    float clear;
    uint32_t w, h, pad;
  } p{g_eye[e].depthClear, V.w, V.h, 0};
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(c->Map(V.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) memcpy(m.pData, &p, sizeof(p)), c->Unmap(V.cb, 0);
  ID3D11ShaderResourceView* srv[20] = {V.Sdsrv, V.Mdsrv, V.Sssrv, V.Mssrv};
  for (UINT k = 0; k < V.n; ++k) srv[4 + 2 * k] = V.Ssrv[k], srv[5 + 2 * k] = V.Msrv[k];
  c->CSSetShader(V.cs, nullptr, 0);
  c->CSSetConstantBuffers(0, 1, &V.cb);
  c->CSSetShaderResources(0, 20, srv);
  c->CSSetUnorderedAccessViews(0, 1, &V.stats.uav, nullptr);
  c->Dispatch((V.w + 7) / 8, (V.h + 7) / 8, 1);
  ID3D11ShaderResourceView* none[20] = {};
  c->CSSetShaderResources(0, 20, none);
  c->CSSetShaderResources(8, 12, hi);
  for (auto& h : hi) Rel(h);
  cs.Restore(c);
  c->CopyResource(V.staging[slot], V.stats.b);
  V.busy[slot] = true;
}
void VerPoll(ID3D11DeviceContext* c) {
  for (int e = 0; e < 2; ++e) {
    VerRes& V = g_ver[e];
    for (int i = 0; i < 4; ++i) {
      if (!V.busy[i]) continue;
      D3D11_MAPPED_SUBRESOURCE m{};
      const HRESULT hr = c->Map(V.staging[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
      if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
      V.busy[i] = false;
      if (FAILED(hr)) continue;
      uint32_t s[kVerStats];
      memcpy(s, m.pData, sizeof(s));
      c->Unmap(V.staging[i], 0);
      std::lock_guard<std::mutex> lock(g_vtMu);
      VerTotals& t = g_vt;
      ++t.frames;
      t.covered += s[0], t.depDiff += s[1], t.depRel += s[2], t.cover += s[3], t.sten += s[43], t.anyDiff += s[42];
      t.mvBad += s[41];
      float f;
      memcpy(&f, &s[4], 4);
      t.depMaxRel = std::max(t.depMaxRel, double(f));
      memcpy(&f, &s[40], 4);
      t.mvMax = std::max(t.mvMax, double(f));
      for (UINT k = 0; k < V.n && k < 8; ++k) {
        t.rtDiff[k] += s[8 + k];
        t.rtMax[k] = std::max(t.rtMax[k], s[16 + k]);
        t.rtSq[k] += (uint64_t(s[25 + 2 * k]) << 32) | s[24 + 2 * k];
      }
    }
  }
}
// The verify verdict over everything read so far (suite thread).
bool LogVerify() {
  std::lock_guard<std::mutex> lock(g_vtMu);
  const VerTotals& t = g_vt;
  const double cov = t.covered ? double(t.covered) : 1.0;
  Log("  focus broadcast verify: %llu compared frames (lost %llu), %llu key draws redrawn stock; covered %.0f px per "
      "frame; G-buffer px differing %.4f %%; depth differing %.3f %% (relative > 1e-4: %.4f %%, max relative %.3g); "
      "coverage %.4f %%; stencil %.4f %%; motion vectors max |diff| %.4f px, > 0.05 px %.4f %%",
      static_cast<unsigned long long>(t.frames), static_cast<unsigned long long>(t.lost),
      static_cast<unsigned long long>(t.drawn), t.frames ? cov / t.frames : 0.0, 100.0 * t.anyDiff / cov,
      100.0 * t.depDiff / cov, 100.0 * t.depRel / cov, t.depMaxRel, 100.0 * t.cover / cov, 100.0 * t.sten / cov,
      t.mvMax, 100.0 * t.mvBad / cov);
  bool ok = t.frames > 0 && t.covered > 0;
  const VerRes& V = g_ver[0];
  double worstPsnr = 1e9;
  for (UINT k = 0; k < V.n && k < 8; ++k) {
    if (static_cast<int>(k) == V.mv) continue;
    const Fam f = FamilyOf(V.rawFmt[k]);
    const int c = f.comps ? f.comps : 4;
    const DXGI_FORMAT rf = V.rawFmt[k];
    const double maxv = UnormScale(rf) > 0 ? UnormScale(rf)
                        : (rf == DXGI_FORMAT_R8G8_UINT || rf == DXGI_FORMAT_R8G8B8A8_UINT || rf == DXGI_FORMAT_R8_UINT) ? 255.0
                        : rf == DXGI_FORMAT_R10G10B10A2_UINT                                                       ? 1023.0
                        : (rf == DXGI_FORMAT_R16_UINT || rf == DXGI_FORMAT_R16G16_UINT ||
                           rf == DXGI_FORMAT_R16G16B16A16_UINT)
                            ? 65535.0
                            : 0.0;  // 32-bit channels (bit patterns): no PSNR, the pixel count and max |diff| only
    const double mse = double(t.rtSq[k]) / (cov * c);
    const double psnr = maxv <= 0 ? 999.0 : mse > 0 ? 10 * std::log10(maxv * maxv / mse) : 999.0;
    if (maxv > 0) worstPsnr = std::min(worstPsnr, psnr);
    Log("  focus broadcast verify: target %u: px differing %.4f %%, max |diff| %u, PSNR %.1f dB", k,
        100.0 * t.rtDiff[k] / cov, t.rtMax[k], psnr);
  }
  ok = ok && 100.0 * t.anyDiff / cov < 1.0 && worstPsnr >= 40.0 && 100.0 * t.depRel / cov < 0.2 &&
       100.0 * t.cover / cov < 0.2 && 100.0 * t.mvBad / cov < 0.1;
  Log("  focus broadcast verify: verdict %s (gates: G-buffer px < 1 %%, PSNR >= 40 dB per target, depth relative > "
      "1e-4 < 0.2 %%, coverage < 0.2 %%, MV > 0.05 px < 0.1 %%)",
      !t.frames ? "NO DATA" : ok ? "PASS" : "FAIL");
  return ok;
}

// Focus: before a hooked instancer draw (view 2 + e). True = not issued (already broadcast and merged).
bool FocusPre(ID3D11DeviceContext* c, int kind, ID3D11Buffer* args, UINT off, int e) {
  EyeRt& E = g_eye[e];
  ID3D11VertexShader* vs = nullptr;
  c->VSGetShader(&vs, nullptr, nullptr);
  if (!vs) return false;
  const Key key{vs, args, off, kind};
  vs->Release();
  E.fKeys.insert(key);
  if (g_probeDrawFrames > 0) ProbeDraw(c, 2 + e, E.fgb, -1, kind, vs, args, off, FindObj(vs));
  if (g_probe.load(std::memory_order_relaxed) && g_cbWanted[2 + e]) {
    Obj* o = FindObj(vs);
    if (o && o->cat == 1) {
      g_cbWanted[2 + e] = false;
      ProbeCbs(c, 2 + e);
    }
  }
  if (!E.bcast || E.mergeFailed || E.block) return false;
  if (!E.keys.count(key)) return false;
  if (!E.merged) {
    Arr& A = g_arr[e];
    double dev = 0;
    if (!E.fCapTaken) {
      g_c.noTc.fetch_add(1, std::memory_order_relaxed);
      E.mergeFailed = true;
      return false;
    }
    if (!PredMatches(E.pred, E.fCur, 2e-6, &dev) || !SameEye(E.pCur, E.fCur, E.aff, 2e-5, &dev)) {
      g_c.predMiss.fetch_add(1, std::memory_order_relaxed);
      E.mergeFailed = true;
      return false;
    }
    g_c.predOk.fetch_add(1, std::memory_order_relaxed);
    // DCS's focus targets: the layout of our arrays, the block's states.
    ID3D11RenderTargetView* rtv[8] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(8, rtv, &dsv);
    bool ok = dsv != nullptr;
    for (UINT k = 0; k < 8 && ok; ++k) {
      if (k >= A.n) {
        ok = !rtv[k];
        continue;
      }
      DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
      const Sub s = rtv[k] ? SubOfRtv(rtv[k], &f) : Sub{};
      ok = s.ok && s.d.Format == A.texFmt[k] && f == A.rtvFmt[k] && (s.d.Width >> s.mip) == A.w &&
           (s.d.Height >> s.mip) == A.h;
    }
    if (ok) {
      DXGI_FORMAT f;
      const Sub s = SubOfDsv(dsv, &f);
      ok = s.ok && s.d.Format == A.dTexFmt && (s.d.Width >> s.mip) == A.w && (s.d.Height >> s.mip) == A.h;
    }
    if (!ok) g_c.fmtBad.fetch_add(1, std::memory_order_relaxed);
    ID3D11DepthStencilState* dss = nullptr;
    UINT ref = 0;
    c->OMGetDepthStencilState(&dss, &ref);
    ID3D11BlendState* bs = nullptr;
    float bf[4];
    UINT sm = 0;
    c->OMGetBlendState(&bs, bf, &sm);
    if (ok && (dss != E.dss || ref != E.sref || bs != E.bs)) {
      ok = false;
      g_c.stateDiff.fetch_add(1, std::memory_order_relaxed);
    }
    Rel(dss), Rel(bs);
    ID3D11RenderTargetView* raw[8] = {};
    for (UINT k = 0; k < A.n && ok; ++k) {
      DcsViews* w = RtvViews(rtv[k], A.rawFmt[k]);
      raw[k] = w ? w->raw : nullptr;
      ok = raw[k] != nullptr;
    }
    D3D11_VIEWPORT v[2] = {};
    UINT nv = 2;
    c->RSGetViewports(&nv, v);
    if (ok && !CopyPrevTransform(c, A, 4)) {
      ok = false;
      g_c.noPrevT.fetch_add(1, std::memory_order_relaxed);
    }
    const bool ver = ok && g_verFrame[e] && EnsureVer(e, A, rtv, dsv);
    if (ver) VerCopy(c, e, rtv, dsv, false);
    if (ok) {
      MergePass(c, E, A, raw, dsv, A.dsrv1, A.srv1, A.mv, nullptr, FullVp(E.wf, E.hf, nv ? v[0] : D3D11_VIEWPORT{0, 0, 0, 0, 0, 1}),
                &ok);
      if (!ok) g_c.faults.fetch_add(1, std::memory_order_relaxed);
    }
    if (ok && ver) {
      VerCopy(c, e, rtv, dsv, true);
      g_verRec[e] = true;
    }
    Rel(dsv);
    for (auto& p : rtv) Rel(p);
    if (!ok) {
      E.mergeFailed = true;
      g_c.focusNoMerge.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    E.merged = true;
    g_c.merges.fetch_add(1, std::memory_order_relaxed);
  }
  if (g_verRec[e]) VerDraw(c, e, kind, args, off);
  g_c.skipped.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// [Suite] FocusBcastTiming=2: pipeline statistics of each view's main G-buffer pass (our own queries; d3d11's ops are
// not hooked, so the broadcast stays armed), per timing run.
struct VStat {
  ID3D11Query* q[4] = {};
  bool busy[4] = {};
  int cur = -1;
  uint64_t n = 0, ia = 0, vsi = 0, cprim = 0, psi = 0;
};
VStat g_vstat[4];
std::mutex g_vstatMu;
void StatBegin(ID3D11DeviceContext* c, int v) {
  VStat& s = g_vstat[v];
  s.cur = -1;
  for (int i = 0; i < 4 && s.cur < 0; ++i)
    if (!s.busy[i]) s.cur = i;
  if (s.cur < 0) return;
  if (!s.q[s.cur]) {
    D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS, 0};
    if (FAILED(pifilt::g_dev->CreateQuery(&qd, &s.q[s.cur]))) {
      s.cur = -1;
      return;
    }
  }
  c->Begin(s.q[s.cur]);
}
void StatEnd(ID3D11DeviceContext* c, int v) {
  VStat& s = g_vstat[v];
  if (s.cur < 0) return;
  c->End(s.q[s.cur]);
  s.busy[s.cur] = true;
  s.cur = -1;
}
void StatPoll(ID3D11DeviceContext* c) {
  for (VStat& s : g_vstat)
    for (int i = 0; i < 4; ++i) {
      if (!s.busy[i]) continue;
      D3D11_QUERY_DATA_PIPELINE_STATISTICS ps{};
      if (c->GetData(s.q[i], &ps, sizeof(ps), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
      s.busy[i] = false;
      std::lock_guard<std::mutex> lock(g_vstatMu);
      ++s.n, s.ia += ps.IAPrimitives, s.vsi += ps.VSInvocations, s.cprim += ps.CPrimitives, s.psi += ps.PSInvocations;
    }
}
void StatLog(const char* tag) {
  std::lock_guard<std::mutex> lock(g_vstatMu);
  for (int v = 0; v < 4; ++v) {
    VStat& s = g_vstat[v];
    const double n = s.n ? double(s.n) : 1.0;
    Log("  focus broadcast stats (%s) view %d main G-buffer: %llu passes; per pass IA primitives %.3f M, VS invocations "
        "%.3f M, rasterised primitives %.3f M, PS invocations %.3f M",
        tag, v, static_cast<unsigned long long>(s.n), s.ia / n / 1e6, s.vsi / n / 1e6, s.cprim / n / 1e6, s.psi / n / 1e6);
    s.n = s.ia = s.vsi = s.cprim = s.psi = 0;
  }
}

// pifilt's callbacks (render thread, inside its draw hooks; SEH outside).
bool Pre(ID3D11DeviceContext* c, int kind, ID3D11Buffer* args, UINT off, int view) {
  (void)view;
  if (g_stopping.load(std::memory_order_relaxed) || g_pr.gb < 0) return false;
  if (!g_pr.tried) ClassifyPass();
  if (g_pr.role == 0) return PeriphPre(c, kind, args, off, g_pr.e);
  if (g_pr.role == 1) return FocusPre(c, kind, args, off, g_pr.e);
  return false;
}
void Post(ID3D11DeviceContext* c, int kind, ID3D11Buffer* args, UINT off, int view) {
  (void)kind, (void)args, (void)off, (void)view;
  if (g_pr.role == 0) PeriphPost(c, g_pr.e);
}
void FrameStart(ID3D11DeviceContext* c) {
  ++g_frame;
  g_c.frames.fetch_add(1, std::memory_order_relaxed);
  g_testsThisFrame = 0;
  PollTests(c);
  if (!g_cbPending.empty()) PollCbs(c);
  VerPoll(c);
  StatPoll(c);
  TsPoll(c);
  TsFrameEnd(c);
  TsFrameBegin(c);
  for (int e = 0; e < 2; ++e) g_verFrame[e] = g_verify.load(std::memory_order_relaxed) && (g_frame % 4) == 0, g_verRec[e] = false;
  if (g_probeDrawFrames > 0) --g_probeDrawFrames;
  const bool on = g_on.load(std::memory_order_relaxed);
  for (int e = 0; e < 2; ++e) {
    EyeRt& E = g_eye[e];
    if (E.block) Close(c, e);  // not expected (a pass end closes it)
    E.ReleaseFrameRefs();
    if (E.tailPending && E.tailQ) {
      D3D11_QUERY_DATA_PIPELINE_STATISTICS ps{};
      if (c->GetData(E.tailQ, &ps, sizeof(ps), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
        E.tailPending = false;
        const int st = (ps.IAPrimitives || ps.VSInvocations || ps.CInvocations) ? 2 : 1;
        if (st != E.tailState)
          Log("focus broadcast: eye %d: after the main G-buffer pass's last instancer draw %s (IA primitives %llu)", e,
              st == 1 ? "nothing is drawn: a frame-buffer change at the pass end is DCS's pop" : "DCS draws more: no blocks",
              static_cast<unsigned long long>(ps.IAPrimitives));
        E.tailState = st;
        (st == 1 ? g_c.tailClean : g_c.tailDirty).fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (E.pCur.ok) E.pPrev = E.pCur;
    if (E.fCur.ok) E.fPrev = E.fCur;
    E.pCur.ok = E.fCur.ok = false;
    E.capTaken = E.fCapTaken = false;
    E.affPrev = E.affCur, E.haveAffPrev = E.haveAffCur;
    E.haveAffCur = false;
    E.fKeysPrev.swap(E.fKeys);
    E.fKeys.clear();
    E.keys.clear();
    if (E.bcast && !E.merged && !E.mergeFailed) g_c.partial.fetch_add(1, std::memory_order_relaxed);
    E.bcast = E.merged = E.mergeFailed = E.refused = E.slice1Cleared = false;
    int best = -1, bestN = 0;
    for (int g = 0; g < 4; ++g) {
      if (E.hooked[g] > bestN) best = g, bestN = E.hooked[g];
      E.hookedPrev[g] = E.hooked[g], E.hooked[g] = 0;
    }
    if (best >= 0) E.mainGb = best;
    const int every = std::max(30, g_relearnFrames.load(std::memory_order_relaxed));
    E.learn = (on || g_probe.load(std::memory_order_relaxed)) && E.mainGb >= 0 &&
              (!E.learnedOnce || static_cast<int>(g_frame % every) == e * 7);
    if (E.learn) E.sl.BeginLearn();
    E.learnArmed = false;
  }
}
// Pass boundaries (pifilt's, render thread).
void OnBoundary(void* pass, bool begin, int kind, int view, int gbInRun) {
  (void)pass;
  ID3D11DeviceContext* c = pifilt::g_ctx;
  if (!c) return;
  if (g_stopping.load(std::memory_order_relaxed)) {
    for (int e = 0; e < 2; ++e)
      if (g_eye[e].block) Close(c, e);
    DisarmUs();
    return;
  }
  if (kind == plod::kHeight || kind == plod::kCascade) {
    if (begin && g_viewsSeen) {
      g_viewsSeen = false;
      FrameStart(c);
    }
    return;
  }
  if (kind == plod::kLights) {
    if (begin) g_viewsSeen = true;
    return;
  }
  if (kind != plod::kGBuffer || view < 0) return;
  if (begin) {
    ++g_passSerial;
    ArmUs();
    g_pr = PassRt{gbInRun - 1, view, -1, -1, false};
    if (g_statsOn.load(std::memory_order_relaxed) && view < 4 && gbInRun - 1 == g_eye[view & 1].mainGb) StatBegin(c, view);
  } else {
    DisarmUs();
    if (view < 4) StatEnd(c, view);
    const PassRt pr = g_pr;
    g_pr = PassRt{};
    if (pr.role != 0 && pr.role != 1) return;
    EyeRt& E = g_eye[pr.e];
    if (pr.role == 0) {
      if (E.block) Close(c, pr.e, true);
      if (E.learn && E.gb == E.mainGb) {
        // DCS's own depth back if our sentinel is still bound; the learned start.
        if (E.learnDsv) {
          DcsViews* w = DsvViews(E.learnDsv);
          ID3D11RenderTargetView* rtv[8] = {};
          ID3D11DepthStencilView* d = nullptr;
          c->OMGetRenderTargets(8, rtv, &d);
          UINT n = 0;
          for (UINT k = 0; k < 8; ++k)
            if (rtv[k]) n = k + 1;
          if (w && d == w->sentinel) c->OMSetRenderTargets(n, rtv, E.learnDsv);
          Rel(d);
          for (auto& p : rtv) Rel(p);
        }
        if (E.tailActive) {
          c->End(E.tailQ);
          E.tailActive = false;
          E.tailPending = true;
        }
        E.sl.EndLearn(E.ord);
        if (E.ord > 0) {
          E.learnedOnce = true;
          g_c.learnFrames.fetch_add(1, std::memory_order_relaxed);
        }
        E.learn = false;
        E.learnArmed = false;
      }
      E.gb = -1;
    } else {
      if (g_verRec[pr.e]) VerEnd(c, pr.e);
      E.fgb = -1;
    }
  }
}
// Before an ExecuteCommandList on DCS's context (split filter): an open block is closed first (exactly), and the start
// position moves past this point.
void BeforeEcl(void* ctx) {
  if (ctx != static_cast<void*>(pifilt::g_ctx) ||
      GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed))
    return;
  for (int e = 0; e < 2; ++e) {
    EyeRt& E = g_eye[e];
    if (E.learn && E.gb == E.mainGb && E.gb >= 0) E.sl.Saw(E.ord);
    if (E.block) {
      __try {
        Close(static_cast<ID3D11DeviceContext*>(ctx), e);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_c.faults.fetch_add(1, std::memory_order_relaxed);
      }
      E.sl.Abort(E.ord - 1);
      g_c.eclCloses.fetch_add(1, std::memory_order_relaxed);
    }
  }
}
void BoundarySeh(void* pass, bool begin, int kind, int view, int gbInRun) {
  __try {
    OnBoundary(pass, begin, kind, view, gbInRun);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_c.faults.fetch_add(1, std::memory_order_relaxed);
    g_on = false;
  }
}

// ---------------------------------------------------------------------------
// Control (config thread / suite thread)
// ---------------------------------------------------------------------------
void ReadConfig(const wchar_t* ini) {
  const int m = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"D3D", L"FocusBroadcast", 0, ini)), 0, 2);
  if (m != g_mode.load()) Log("focus broadcast: [D3D] FocusBroadcast=%d (2 = broadcast the periphery's instancer draws into the focus)", m);
  g_mode = m;
  g_kinds = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"D3D", L"FocusBroadcastKinds", 1, ini)), 0, 3);
  g_testsPerFrame = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"D3D", L"FocusBroadcastTestsPerFrame", 6, ini)), 1, 64);
  g_relearnFrames = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"D3D", L"FocusBroadcastRelearnFrames", 900, ini)), 30, 100000);
  g_startDelay = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"D3D", L"FocusBroadcastStartDelay", 10, ini)), 0, 600);
  fbc::g_readyGate = &ReadyGate;
  wchar_t b[MAX_PATH] = {};
  GetPrivateProfileStringW(L"D3D", L"FocusBroadcastArchive", L"", b, MAX_PATH, ini);
  g_arcKey = b;
  g_suiteProbe = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Suite", L"FocusBcastProbe", 0, ini)), 0, 600);
  g_suiteTiming = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Suite", L"FocusBcastTiming", 0, ini)), 0, 2);
  g_suiteVerify = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Suite", L"FocusBcastVerify", 0, ini)), 0, 600);
}
void SetDirs(const std::wstring& saved, const std::wstring& out) {
  g_saved = saved;
  g_outDir = out;
}
void LogCounters(const char* tag) {
  const Counters& c = g_c;
  auto u = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load()); };
  Log("  focus broadcast (%s): on %d, frames %llu, blocks %llu (closed %llu, aborted %llu, closed before a command list "
      "%llu), merges %llu, focus draws not issued %llu, periphery draws broadcast %llu; prediction ok %llu / miss %llu / "
      "none %llu; TerrainContext not seen %llu (other hook in the upload slot %llu, partial uploads %llu); learning frames "
      "%llu (rebinds seen %llu), start L %d R %d; TerrainContext misses: observer still in place %llu, entry gone %llu; pass-end closes after DCS's frame-buffer pop %llu, tail clean %llu / draws %llu / unknown %llu",
      tag, g_on.load() ? 1 : 0, u(c.frames), u(c.blocks), u(c.blocksClosed), u(c.blockAborts), u(c.eclCloses),
      u(c.merges), u(c.skipped), u(c.covered), u(c.predOk), u(c.predMiss), u(c.noPred), u(c.noTc), u(c.tcForeign),
      static_cast<unsigned long long>(g_capPartial.load()), u(c.learnFrames), u(c.rebinds), g_eye[0].sl.start,
      g_eye[1].sl.start, u(c.tcMissed), u(c.tcLost), u(c.popCloses), u(c.tailClean), u(c.tailDirty), u(c.noTail));
  Log("  focus broadcast (%s): draws left stock in a block: no signature %llu, family off %llu, other GS/HS/DS %llu, "
      "topology %llu, blend %llu, state %llu; refusals: view/size %llu, format %llu, targets %llu, focus state differs "
      "%llu, focus not merged %llu; viewport re-sets %llu; identification tests %llu; pifilt re-arms refused %llu; "
      "faults %llu (callbacks %llu); no gPrevFrameTransform %llu; start gate: ticks held %llu, worker loops paused %llu",
      tag, u(c.uncovNoSig), u(c.uncovKind), u(c.uncovGs), u(c.uncovTopo), u(c.uncovBlend), u(c.uncovState),
      u(c.viewBad), u(c.fmtBad), u(c.resFail), u(c.stateDiff), u(c.focusNoMerge), u(c.vpReset), u(c.idTests),
      static_cast<unsigned long long>(pifilt::g_rearmRefused.load()), u(c.faults),
      static_cast<unsigned long long>(pifilt::g_fbcFaults.load()), u(c.noPrevT), u(g_heldTicks), u(g_pausedLoops));
}
void LogObjects() {
  std::vector<Obj*> v;
  {
    std::lock_guard<std::mutex> lock(g_objMu);
    for (auto& kv : g_objs) v.push_back(kv.second);
  }
  std::sort(v.begin(), v.end(), [](Obj* a, Obj* b) { return a->draws[0] + a->draws[1] > b->draws[0] + b->draws[1]; });
  for (Obj* o : v)
    Log("    VS %p: draws %llu indexed / %llu instanced, broadcast %llu; mainBuffer t%d; state %d (%s), %s, %d elements, "
        "matches %d (prefix %d)%s, FastGS %d, checksum %s",
        static_cast<void*>(o->vs), static_cast<unsigned long long>(o->draws[0]), static_cast<unsigned long long>(o->draws[1]),
        static_cast<unsigned long long>(o->bcast), o->mainSlot, o->st.load(),
        o->st.load() == kReady ? "ready" : o->st.load() == kTesting ? "testing" : o->st.load() == kNoCand ? "no candidate"
                                                                                : o->st.load() == kNoSig ? "no signature" : "other",
        o->cat == 1 ? "SpeedTree" : "other", o->elems, o->matches, o->prefixes, o->ambiguous ? " AMBIGUOUS" : "",
        o->gsStatus, o->sum.empty() ? "-" : o->sum.c_str());
}
void Install() {
  if (g_installed.load()) return;
  pifilt::g_fbcPre = &Pre;
  pifilt::g_fbcPost = &Post;
  pifilt::g_fbcBoundary = &BoundarySeh;
  sfilt::g_beforeEcl = &BeforeEcl;
  fixdepth::g_uploadObserver = &ObserveFd;
  g_workerRun = true;
  g_worker = std::thread(WorkerLoop);
  g_installed = true;
  pifilt::g_fbcOn = true;
  Log("focus broadcast: installed (pifilt draw arms in all four views, TerrainContext observer, command-list guard)");
}
// Every second from the config loop.
void Apply(bool engineOff) {
  g_engineOff = engineOff;
  const int f = g_force.load();
  const bool want = (f == 1 || (f < 0 && g_mode.load() >= 2)) && !engineOff;
  const bool plumb = want || g_probe.load();
  if (!plumb) {
    g_on = false;
    if (g_installed.load()) pifilt::g_fbcOn = false;  // no arms (an open block still closes at its pass end)
    return;
  }
  if (!g_installed.load() && !g_started.load() && f < 0) {  // the start gate (frames flowing; suites bypass it)
    g_heldTicks.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!pifilt::EnsureDevice()) return;
  if (!g_installed.load()) Install();
  pifilt::g_fbcOn = true;
  pifilt::Apply(engineOff);  // pifilt's boundary callback (the config tick skips it while a suite / bench runs)
  // Refusals: rank 1's filter changes the periphery lists (a zero-coverage instance may cover focus pixels); the split
  // filter's command-list hook must be live (it closes a block before a command list).
  const bool pif = pifilt::g_mask.load() != 0;
  const bool sf = sfilt::g_live.load() != nullptr;
  static bool loggedPif = false, loggedSf = false;
  if (want && pif && !loggedPif) Log("focus broadcast: off while [D3D] PeriphInstFilter is on"), loggedPif = true;
  if (want && !sf && !loggedSf) Log("focus broadcast: off: the split filter is not active (its command-list hook closes blocks)"), loggedSf = true;
  if (pif) g_c.pifOn.fetch_add(1);
  if (!sf) g_c.sfOff.fetch_add(1);
  g_on = want && !pif && sf;
  static ULONGLONG last = 0;
  const ULONGLONG now = GetTickCount64();
  if (g_on.load() && f < 0 && now - last >= 60000) {
    if (last) LogCounters("60 s");
    last = now;
  }
}
void WriteRows(const wchar_t* name, const char* header, std::vector<std::string>& rows) {
  if (g_outDir.empty()) return;
  FILE* f = nullptr;
  _wfopen_s(&f, (g_outDir + name).c_str(), L"w");
  if (!f) return;
  fprintf(f, "%s\n", header);
  for (auto& r : rows) fprintf(f, "%s\n", r.c_str());
  fclose(f);
  rows.clear();
}
bool WaitMerges(int ms) {
  const uint64_t m0 = g_c.merges.load();
  for (int t = 0; t < ms && !g_benchAbort.load() && !g_stop.load(); t += 100) {
    if (g_c.merges.load() >= m0 + 20) return true;
    Sleep(100);
  }
  return false;
}
void SuitePhases(std::atomic<uint64_t>& frameCounter) {
  const int probe = g_suiteProbe.load(), tim = g_suiteTiming.load(), ver = g_suiteVerify.load();
  if (!probe && !tim && !ver) return;
  if (probe > 0) {
    Log("-- R53 focus broadcast: census, measure only (focus_bcast_run.h, %d s) --", probe);
    g_force = 0;
    g_probe = true;
    Apply(g_engineOff.load());
    Sleep(3000);
    g_probeDrawFrames = 3;
    for (bool& w : g_cbWanted) w = true;
    Sleep(static_cast<DWORD>(probe) * 1000);
    g_probe = false;
    Sleep(500);
    {
      std::lock_guard<std::mutex> lock(g_probeMu);
      WriteRows(L"fbc_draws.csv",
                "frame,view,gb,ord,kind,vs,args,off,objstate,cat,gs,hs,ds,topo,dss,sref,depth,dwrite,dfunc,stencil,"
                "sread,swrite,sfunc,spass,sdfail,bs,a2c,indep,blend0,masks,cull,scissor,nvp,vpx,vpy,vpw,vph,nrtv,rtv0,"
                "rtvlast,dsv",
                g_rowsDraw);
      WriteRows(L"fbc_tc.csv", "frame,view,gb,P16(4),P17(4),P18(4),P19(4),VP20(4),VP21(4),VP22(4),VP23(4),cam(4),r44(4)",
                g_rowsTc);
      WriteRows(L"fbc_ci.csv", "qpc,frame,n,[index,role,partner,vp16]*", g_rowsCi);
      WriteRows(L"fbc_cb.csv", "frame,view,stage,slot,first16,bytes,hex", g_rowsCb);
    }
    LogObjects();
    LogCounters("census");
  }
  if (tim) {
    Log("-- R53 focus broadcast: GPU timing OFF/ON/OFF/ON/OFF (5 x 5 s) --");
    g_force = 1;
    Apply(g_engineOff.load());
    if (!WaitMerges(60000)) {
      Log("  focus broadcast timing: no merges within 60 s; phase skipped");
      LogCounters("timing");
    } else {
      for (int i = 0; i < 5 && !g_benchAbort.load(); ++i) {
        const bool on = (i & 1) != 0;
        g_force = on ? 1 : 0;
        Apply(g_engineOff.load());
        Sleep(1500);
        Log("  focus broadcast timing run %d: %s", i + 1, on ? "ON" : "OFF (stock)");
        if (tim == 2) g_statsOn = true;
        gpt::Measure(5000, frameCounter, true);
        g_statsOn = false;
        Sleep(200);
        if (tim == 2) StatLog(on ? "ON" : "OFF"), TsLog(on ? "ON" : "OFF");
        LogCounters(on ? "timing ON" : "timing OFF");
      }
    }
  }
  if (ver > 0) {
    Log("-- R53 focus broadcast: same-frame verify against stock (every 4th frame, %d s) --", ver);
    g_force = 1;
    Apply(g_engineOff.load());
    if (!WaitMerges(60000)) {
      Log("  focus broadcast verify: no merges within 60 s; phase skipped");
    } else {
      {
        std::lock_guard<std::mutex> lock(g_vtMu);
        g_vt = VerTotals{};
      }
      g_verify = true;
      Sleep(static_cast<DWORD>(ver) * 1000);
      g_verify = false;
      Sleep(1000);  // the last compares read back
      LogVerify();
    }
    LogCounters("verify");
  }
  g_force = -1;
  Apply(g_engineOff.load());
}
void Shutdown() {
  g_on = false;
  g_probe = false;
  if (g_installed.load()) {
    g_stopping = true;
    for (int i = 0; i < 50 && (g_eye[0].block || g_eye[1].block); ++i) Sleep(10);
    pifilt::g_fbcOn = false;
    pifilt::g_fbcBoundary = nullptr;
    pifilt::g_fbcPre = nullptr;
    pifilt::g_fbcPost = nullptr;
    void (*me)(void*) = &BeforeEcl;
    sfilt::g_beforeEcl.compare_exchange_strong(me, nullptr);
    if (fixdepth::g_uploadObserver == &ObserveFd) fixdepth::g_uploadObserver = nullptr;
    Sleep(50);
    DisarmUs();
    g_workerRun = false;
    if (g_worker.joinable()) g_worker.join();
    g_installed = false;
    g_stopping = false;
  }
  {
    std::lock_guard<std::mutex> lock(g_objMu);
    for (auto& kv : g_objs) {
      Obj* o = kv.second;
      for (vsig::IdTest& t : o->tests) t.Release();
      Rel(o->gs);
      if (o->vs) o->vs->Release();
      delete o;
    }
    g_objs.clear();
  }
  for (auto& d : g_cbPending) Rel(d.st);
  g_cbPending.clear();
  for (int e = 0; e < 2; ++e) {
    g_eye[e].ReleaseAll();
    g_eye[e] = EyeRt{};
    g_arr[e].Release();
  }
  ClearDcsViews();
  for (auto& v : g_ver) v.Release();
  for (auto& s : g_vstat) {
    for (auto& q : s.q) Rel(q);
    s = VStat{};
  }
  for (auto& f : g_ts) {
    Rel(f.dj);
    for (auto& q : f.q) Rel(q);
    f = TsFrame{};
  }
  g_tsCur = -1;
  ReleaseDev();
  g_arc.clear();
  g_arcState = 0;
}

}  // namespace fbcr
