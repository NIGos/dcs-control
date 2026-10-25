// R139: measure-only VRS probe ([Suite] VrsProbe = N seconds per block, default 0 = nothing installed, nothing
// changes). Design and verdict: docs/research/R138_vrs_study.md (sections 4 and 6); the build: R139_vrs_probe_build.md.
//
// What it measures. NVIDIA's D3D11 variable pixel rate shading (NvAPI, nvapi_vrs_min.h) coarsens pixel shading in
// chosen DCS passes of chosen HMD views, for 5 s blocks in an ABBA schedule against the stock rendering, with GPU pass
// timing and pipeline statistics (gpu_pass_timing.h's session, its timestamps and PS invocation counts). The PS counts
// prove the state was active: a full-screen pass at 2x2 should fall to about 0.25x (whole view) or about 0.43x (the
// 10 degree ring); an unchanged count means DCS's state changes reset ours. LOSSY inside the ON blocks only.
//
// Arms (R138 6): A0 off; A1 main G-buffer, whole focus; A2 every pre-DLSS raster pass of the focus views, whole view;
// A3 A2 with a ring (full rate inside [Suite] VrsProbeRingDeg of the gaze, coarse outside); A4 the shadow cascades
// c0 / c1 (shared by all views, whole map); A5 SSAO only, whole focus; A6 (R140) SSAO + the lighting group in the ring
// only (the full-screen low-frequency passes outside the fovea); A7 (bit 7 of VrsProbeArms; bit 6 until R140) the
// custom arm of VrsProbePasses / VrsProbeInnerDeg. Rate, views, pass mask and ring radius are keys (ReadKeys below).
//
// R140 (the first live run, R139 2026-10-25; docs/research/R140_vrs_probe_fix.md):
//  - G-buffer passes: fixation_depth.h's in-pass OM / viewport hooks hold those slots there, so the probe's own hooks
//    (d3d11's entries only) were refused and it never saw a G-buffer bind: no size, no learnt depth, A1 never applied.
//    Now fixation_depth.h's hooks report every call through ptiming::g_omObserver / g_vpObserver (ObsOm / ObsVp).
//  - DCS's default frame buffer (bound between passes and again at each pass's end: the periphery's DLSS output size,
//    1076x1060 live) is recognised (FbTrack: the target bound at most pass starts) and never sizes a surface.
//  - Surface content: cached per surface (view x target size), uploaded only when the gaze-centre tile, the target
//    size, the projection or the radius changes, at most once per view per frame (UploadBudget; stale surfaces of a
//    view take turns). R139 re-uploaded on every apply (309532 uploads, 9 ms render thread per frame in A3).
//  - Ring geometry from the surface's own target extent (not the current viewport, which could be another target's);
//    whole-view surfaces report their real coverage (R139 logged "0 of 0"); the surface lines are per surface used in
//    the block, with the surface's own target size (R139 mixed the last upload's tiles with the last bind's size).
// Schedule: per repeat, per arm a quartet OFF ON ON OFF (even repeats) or ON OFF OFF ON (odd repeats), each block
// after VrsProbeSettleMs of the new state; a linear drift cancels in every quartet (PairedEffect). Kill gate
// (R138 6): A2 + A4 saving < 0.5 ms GPU -> close VRS for good (logged as "vrs probe gate: CLOSE"); A3 or A4 >= 0.3 ms
// -> offer it as a lossy opt-in once the image check passes.
// Image check ([Suite] VrsProbeImage = frames per dump, default 0): after the timing, VrsProbeImageWaitSec for the
// harness to pause the sim, then per image arm OFF / ON / OFF / ON dumps of the HMD views through cockpit ipd's
// [Perception] CockpitTrueIpdFrameDump machinery (its views key picks them; tools\onoff_check.py-style analysis).
//
// Hooks, render thread only, all installed at the phase start and taken out at its end:
//  - pass_timing.h's g_vrsPre (first at a pass start) and g_vrsBoundary (last at a start, first at an end): no other
//    feature's boundary work ever runs under the probe's state. Passes: their name field (+0x10, plod::KindRaw's),
//    classified by ClassOfName.
//  - View identity: the TerrainContext (b11, 2176 B, its first upload inside a GBufferPass) classified as R53 /
//    focus_twin_math.h do (ftwin::ClassifyTc: sx < 1.5 periphery, the sign of cx its eye; a focus view's eye is the
//    periphery of this frame whose shared rows it has). Uploads are seen through pass_timing.h's g_uploadObserver,
//    which every UpdateSubresource hook of ours calls (gpu_pass_timing.h's while it measures, fixation_depth.h's in its
//    arms, ours: a one-shot arm over d3d11's own entry only). The G-buffer's depth texture (bound inside the pass) is
//    then learnt for that view: a later pass binding it is identified without the upload (and the surfaces are sized
//    from it). Never by ordinals (F1 MFD views shift them, R53). MFD sensor views: "other", never armed.
//  - In the passes of a block's arm: OMSetRenderTargets(AndUnorderedAccessViews) and RSSetViewports, adopted only
//    over d3d11's own entries (never over another hook: our forwarders always reach d3d11 directly, so no chain of
//    hooks can loop); after each forwarded call the state is re-applied with the surface of the bound target's size.
//    Also re-applied at every nested pass boundary and after every ExecuteCommandList our recorders or the split
//    filter issue (split_filter.h's g_afterEclProbe). Cleared (rates disabled, surface unbound) at the pass end, so
//    our colour / glare / DoF passes, DLSS, QVFR and OFXR never inherit it. The enlight gap (outside passes, between
//    TransparentDecalPass and TransparentPass): applied lazily at DCS's first OM call there, cleared by g_vrsPre.
//  - The D3D meter holds the table (d3ds::g_vtblCount): no in-pass arm (counted); state applied at the pass begin only.
// Region of a view: from its main G-buffer pass (the one whose TerrainContext was seen, or the learnt depth texture
// at the learnt ordinal) to its DLSSPass (exclusive). Never: the cockpit G-buffer (before the main one), overlay / GUI /
// cockpit / tone map / histogram / upscale / HMD copy / water passes (ClassOfName).
// Surfaces: R8_UINT, ceil(W/16) x ceil(H/16) of the bound target, one per (slot, size); slots 0-3 HMD views, 4-5
// cascades c0 / c1. Index 0 = 1x1, index 1 = the arm's rate. Ring maps from the view's TerrainContext projection and
// the gaze (Perception GazeOut, read-only; the viewport centre without it), re-uploaded only when they change.
// Created on the suite thread (render thread with a single-threaded device), only after the R53 start gate
// (VrsProbeStartDelay s of steady frames: NvAPI object creation during a mission load hung DCS once).
//
// Fail safe: no nvapi64.dll, a missing entry point, a failed NvAPI_Initialize, a caps query that fails for both struct
// versions or a GPU without VRS -> one log line, the probe stays off. Every render-thread entry runs under SEH; a fault
// latches the probe off (state cleared, hooks forward only). The kill switch, a suite abort or frames stopping end the
// phase. Cost with the key 0: nothing (no hook, no callback; the config tick reads the keys).
//
// Included once from main.cpp (features.inc), after focus_twin.h: uses ptiming, plod, gpt, sfilt, d3ds, shadowinst,
// fixdepth (InD3d11, GazeOut, GazeName, ckipd's frame dump), ftwin (focus_twin_math.h), Log, g_benchAbort, g_engineOff.
#pragma once

#include <intrin.h>  // _ReturnAddress

#include "nvapi_vrs_min.h"
#include "focus_twin_math.h"

namespace vrsp {

// ---------------------------------------------------------------------------
// Keys (pure)
// ---------------------------------------------------------------------------
// [Suite] VrsProbePasses bits (also the arms' pass sets).
enum PassBit : uint32_t {
  kPGbMain = 1,       // the view's main G-buffer pass
  kPSsao = 2,         // SSAO
  kPLight = 4,        // lighting group: LightsPre, GBufferLighting, SSLR, TerrainShadow, DepthResolver, shadow blur, SSS
  kPTransp = 8,       // TransparentDecalPass, TransparentPass
  kPEnlight = 16,     // the enlight gap between TransparentDecalPass and TransparentPass (outside passes)
  kPCascade = 32,     // shadow cascades c0 / c1
  kPAllRaster = 64,   // every pre-DLSS raster pass of the region (decal G-buffer, flat shadows, hot air, ...)
};
constexpr uint32_t kPreDlss = kPGbMain | kPSsao | kPLight | kPTransp | kPEnlight | kPAllRaster;
constexpr int kArmCount = 8;  // A0 .. A7
constexpr int kCustomArm = 7;  // R140: the custom arm (was A6 in R139)
constexpr uint32_t kArmMask = 0xFE;  // VrsProbeArms / VrsProbeImageArms bits 1..7
constexpr uint32_t kDefArms = 0x7E;  // A1 .. A6
constexpr uint32_t kDefImageArms = 0x58;  // A3, A4, A6

struct Keys {
  int blockSec = 0;            // [Suite] VrsProbe: seconds per block (0 = off)
  int rate = nvvrs::k2x2;      // VrsProbeRate: 1x2, 2x1, 2x2, 2x4, 4x2, 4x4
  uint32_t views = 0xC;        // VrsProbeViews: bit v = HMD view v (0 / 1 periphery L / R, 2 / 3 focus L / R)
  uint32_t passes = 0;         // VrsProbePasses: the custom arm A7's passes (PassBit)
  double innerDeg = 0;         // VrsProbeInnerDeg: A7's ring radius (0 = whole view)
  double ringDeg = 10;         // VrsProbeRingDeg: the ring radius of A3 and A6
  uint32_t arms = kDefArms;    // VrsProbeArms: bit a = arm Aa (1..7) in the schedule
  int repeats = 4;             // VrsProbeRepeats: quartets per arm
  int startDelay = 10;         // VrsProbeStartDelay: seconds of steady frames before anything is created
  int settleMs = 1500;         // VrsProbeSettleMs: before each block, after its state change
  int imageFrames = 0;         // VrsProbeImage: frames per image dump (0 = no image check)
  uint32_t imageArms = kDefImageArms;  // VrsProbeImageArms: bit a = arm Aa dumped OFF / ON / OFF / ON
  int imageWaitSec = 20;       // VrsProbeImageWaitSec: before the dumps (the harness pauses the sim)
};

// "2x2" (case and spaces ignored) -> the NvAPI rate; -1 for anything else. Only the coarse rates.
inline int ParseRate(const wchar_t* s) {
  if (!s) return -1;
  wchar_t t[16] = {};
  int n = 0;
  for (const wchar_t* p = s; *p && n < 15; ++p)
    if (*p != L' ' && *p != L'\t') t[n++] = (*p == L'X') ? L'x' : *p;
  struct R {
    const wchar_t* s;
    int r;
  };
  static const R kR[] = {{L"1x2", nvvrs::k1x2}, {L"2x1", nvvrs::k2x1}, {L"2x2", nvvrs::k2x2},
                         {L"2x4", nvvrs::k2x4}, {L"4x2", nvvrs::k4x2}, {L"4x4", nvvrs::k4x4}};
  for (const R& r : kR)
    if (!wcscmp(t, r.s)) return r.r;
  return -1;
}
inline const char* RateName(int r) {
  switch (r) {
    case nvvrs::k1x2: return "1x2";
    case nvvrs::k2x1: return "2x1";
    case nvvrs::k2x2: return "2x2";
    case nvvrs::k2x4: return "2x4";
    case nvvrs::k4x2: return "4x2";
    case nvvrs::k4x4: return "4x4";
    case nvvrs::kX1: return "1x1";
  }
  return "?";
}
// Shading pixels per PS invocation of a rate (2x2 -> 4).
inline int RateArea(int r) {
  switch (r) {
    case nvvrs::k1x2:
    case nvvrs::k2x1: return 2;
    case nvvrs::k2x2: return 4;
    case nvvrs::k2x4:
    case nvvrs::k4x2: return 8;
    case nvvrs::k4x4: return 16;
  }
  return 1;
}

// "0xC", "12", "" -> value (base 0); def when empty or not a number. *bad: something unparsable was there.
inline uint32_t ParseMask(const wchar_t* s, uint32_t def, bool* bad) {
  if (!s || !*s) return def;
  wchar_t* end = nullptr;
  const unsigned long v = wcstoul(s, &end, 0);
  if (end == s || (end && *end && *end != L' ' && *end != L'\t' && *end != L';')) {
    if (bad) *bad = true;
    return def;
  }
  return static_cast<uint32_t>(v);
}
inline double ParseDeg(const wchar_t* s, double def, bool* bad) {
  if (!s || !*s) return def;
  wchar_t* end = nullptr;
  const double v = wcstod(s, &end);
  if (end == s || !std::isfinite(v)) {
    if (bad) *bad = true;
    return def;
  }
  return v;
}

// Every [Suite] VrsProbe* key, clamped; *notes gets one phrase per value that was replaced (empty: all fine).
inline Keys ReadKeys(const wchar_t* ini, std::string* notes) {
  Keys k;
  auto str = [&](const wchar_t* key, wchar_t* out, DWORD cap) { GetPrivateProfileStringW(L"Suite", key, L"", out, cap, ini); };
  auto note = [&](const char* what) {
    if (notes) *notes += (notes->empty() ? "" : "; ") + std::string(what);
  };
  wchar_t v[64];
  bool bad = false;
  k.blockSec = (std::max)(0, (std::min)(60, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"VrsProbe", 0, ini))));
  str(L"VrsProbeRate", v, 64);
  if (v[0]) {
    const int r = ParseRate(v);
    if (r < 0)
      note("VrsProbeRate is not 1x2 / 2x1 / 2x2 / 2x4 / 4x2 / 4x4: 2x2");
    else
      k.rate = r;
  }
  str(L"VrsProbeViews", v, 64);
  k.views = ParseMask(v, 0xC, &bad) & 0xF;
  if (bad) note("VrsProbeViews unreadable: 0xC"), bad = false;
  if (!k.views) note("VrsProbeViews selects no HMD view: 0xC"), k.views = 0xC;
  str(L"VrsProbePasses", v, 64);
  k.passes = ParseMask(v, 0, &bad) & 0x7F;
  if (bad) note("VrsProbePasses unreadable: 0"), bad = false;
  str(L"VrsProbeInnerDeg", v, 64);
  k.innerDeg = ParseDeg(v, 0, &bad);
  if (bad || k.innerDeg < 0 || k.innerDeg > 60) note("VrsProbeInnerDeg outside 0..60: 0"), k.innerDeg = 0, bad = false;
  str(L"VrsProbeRingDeg", v, 64);
  k.ringDeg = ParseDeg(v, 10, &bad);
  if (bad || k.ringDeg <= 0 || k.ringDeg > 60) note("VrsProbeRingDeg outside (0, 60]: 10"), k.ringDeg = 10, bad = false;
  str(L"VrsProbeArms", v, 64);
  k.arms = ParseMask(v, kDefArms, &bad) & kArmMask;
  if (bad) note("VrsProbeArms unreadable: 0x7E"), bad = false;
  if ((k.arms & 0x80) && !k.passes) note("VrsProbeArms bit 7 (custom arm) without VrsProbePasses: dropped"), k.arms &= ~0x80u;
  if (k.passes && !(k.arms & 0x80))
    note("VrsProbePasses is the custom arm A7's (VrsProbeArms bit 7, 0x80; bit 6 is A6 since R140): not scheduled");
  if (!k.arms) note("VrsProbeArms selects no arm: 0x7E"), k.arms = kDefArms;
  k.repeats = (std::max)(1, (std::min)(16, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"VrsProbeRepeats", 4, ini))));
  k.startDelay = (std::max)(0, (std::min)(300, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"VrsProbeStartDelay", 10, ini))));
  k.settleMs = (std::max)(200, (std::min)(10000, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"VrsProbeSettleMs", 1500, ini))));
  k.imageFrames = (std::max)(0, (std::min)(30, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"VrsProbeImage", 0, ini))));
  str(L"VrsProbeImageArms", v, 64);
  k.imageArms = ParseMask(v, kDefImageArms, &bad) & kArmMask;
  if (bad) note("VrsProbeImageArms unreadable: 0x58");
  k.imageWaitSec = (std::max)(0, (std::min)(120, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"VrsProbeImageWaitSec", 20, ini))));
  return k;
}

// ---------------------------------------------------------------------------
// Arms and schedule (pure)
// ---------------------------------------------------------------------------
struct ArmDef {
  uint32_t passes = 0;
  double innerDeg = 0;  // 0 = whole view
};
inline ArmDef ArmOf(int a, const Keys& k) {
  switch (a) {
    case 1: return {kPGbMain, 0};
    case 2: return {kPreDlss, 0};
    case 3: return {kPreDlss, k.ringDeg};
    case 4: return {kPCascade, 0};
    case 5: return {kPSsao, 0};
    case 6: return {kPSsao | kPLight, k.ringDeg};  // R140
    case kCustomArm: return {k.passes, k.innerDeg};
  }
  return {};
}
inline const char* ArmName(int a) {
  switch (a) {
    case 0: return "off";
    case 1: return "main G-buffer, whole focus";
    case 2: return "all pre-DLSS raster, whole focus";
    case 3: return "all pre-DLSS raster, ring";
    case 4: return "cascades c0/c1";
    case 5: return "SSAO only, whole focus";
    case 6: return "SSAO + lighting, ring";
    case kCustomArm: return "custom (VrsProbePasses / VrsProbeInnerDeg)";
  }
  return "?";
}
struct Block {
  int arm = 0;      // the tested arm of the quartet
  bool on = false;  // false: the A0 baseline block of the quartet
  int rep = 0;
};
// Per repeat, per selected arm (bits 1..7): OFF ON ON OFF (even repeat) or ON OFF OFF ON (odd repeat).
inline std::vector<Block> Schedule(uint32_t arms, int reps) {
  std::vector<Block> s;
  for (int r = 0; r < reps; ++r)
    for (int a = 1; a < kArmCount; ++a) {
      if (!(arms >> a & 1)) continue;
      const bool first = (r & 1) != 0;  // the outer blocks' state
      s.push_back({a, first, r});
      s.push_back({a, !first, r});
      s.push_back({a, !first, r});
      s.push_back({a, first, r});
    }
  return s;
}
// Paired effect of one arm on a per-block metric: per quartet, mean(ON) - mean(OFF) over its usable blocks; the
// mean over the quartets and its standard error (0 with one quartet). n = quartets with both states usable.
struct Effect {
  int n = 0;
  double mean = 0, se = 0;
};
inline Effect PairedEffect(const std::vector<Block>& s, const std::vector<double>& v, const std::vector<bool>& ok,
                           int arm) {
  std::vector<double> d;
  for (size_t i = 0; i < s.size(); i += 4) {  // Schedule's quartets start at multiples of 4
    if (s[i].arm != arm) continue;
    double on = 0, off = 0;
    int nOn = 0, nOff = 0;
    for (size_t j = i; j < i + 4 && j < s.size(); ++j) {
      if (j >= v.size() || j >= ok.size() || !ok[j]) continue;
      if (s[j].on)
        on += v[j], ++nOn;
      else
        off += v[j], ++nOff;
    }
    if (nOn && nOff) d.push_back(on / nOn - off / nOff);
  }
  Effect e;
  e.n = static_cast<int>(d.size());
  if (!e.n) return e;
  for (double x : d) e.mean += x;
  e.mean /= e.n;
  if (e.n > 1) {
    double ss = 0;
    for (double x : d) ss += (x - e.mean) * (x - e.mean);
    e.se = std::sqrt(ss / (e.n - 1) / e.n);
  }
  return e;
}
// R138 6's gates on the frame GPU time effects (negative = a saving).
struct Gate {
  bool decided = false;  // A2 and A4 both measured
  double saveA2 = 0, saveA4 = 0, saveA3 = 0, saveA6 = 0;
  bool close = false;    // A2 + A4 < 0.5 ms: close VRS for good
  bool offer = false;    // A3, A4 or A6 >= 0.3 ms (lossy opt-in after the image check)
};
inline Gate Decide(const Effect& a2, const Effect& a3, const Effect& a4, const Effect& a6 = Effect{}) {
  Gate g;
  g.saveA2 = -a2.mean, g.saveA3 = -a3.mean, g.saveA4 = -a4.mean, g.saveA6 = -a6.mean;
  g.decided = a2.n > 0 && a4.n > 0;
  if (g.decided) g.close = g.saveA2 + g.saveA4 < 0.5;
  g.offer = !g.close && ((a3.n > 0 && g.saveA3 >= 0.3) || (a4.n > 0 && g.saveA4 >= 0.3) ||
                         (a6.n > 0 && g.saveA6 >= 0.3));
  return g;
}

// ---------------------------------------------------------------------------
// Pass classes (pure)
// ---------------------------------------------------------------------------
enum Cls : uint8_t {
  kCOther = 0,  // a pre-DLSS raster pass of no named group (AllRaster only)
  kCLights,     // LightsBuffersPass: a view starts (its culling chain; compute)
  kCCascade,    // CascadeShadowPassN
  kCFrame,      // HeightMapPass (frame level)
  kCHmd,        // CopyToHMDPass: the view ends
  kCGBuffer,    // GBufferPass (cockpit, main or decal)
  kCSsao,
  kCLight,
  kCTransp,
  kCDlss,       // the region ends (DLSSPass)
  kCPost,       // the region ends too (tone map, overlays: post-DLSS)
  kCExcl,       // never armed (GUI, cockpit, water, upscale, HMD-side passes)
  kCCount
};
inline const char* ClsName(int c) {
  static const char* const k[kCCount] = {"raster", "lights", "casc", "frame", "hmd", "gb", "ssao", "light", "transp",
                                         "dlss", "post", "excl"};
  return c >= 0 && c < kCCount ? k[c] : "?";
}
inline Cls ClassOfName(const char* n) {
  if (!n || !*n) return kCOther;
  auto has = [n](const char* w) { return strstr(n, w) != nullptr; };
  if (!strcmp(n, "LightsBuffersPass")) return kCLights;
  if (!strncmp(n, "CascadeShadowPass", 17)) return kCCascade;
  if (!strcmp(n, "HeightMapPass")) return kCFrame;
  if (!strcmp(n, "CopyToHMDPass")) return kCHmd;
  if (has("DLSS")) return kCDlss;
  if (has("ToneMapping") || has("Overlay") || has("Histogram") || has("Adaptation")) return kCPost;
  if (has("GUI") || has("Cockpit") || has("Upscale") || has("HMD") || has("Water") || has("MapRaster")) return kCExcl;
  if (!strcmp(n, "GBufferPass")) return kCGBuffer;
  if (has("SSAO")) return kCSsao;
  if (has("Lighting") || has("LightsPre") || has("SSLR") || has("TerrainShadow") || has("DepthResolver") ||
      has("ShadowBlur") || has("SSS"))
    return kCLight;
  if (has("Transparent")) return kCTransp;
  return kCOther;
}
// CascadeShadowPassN -> N (0..7), else -1.
inline int CascadeOfName(const char* n) {
  if (!n || strncmp(n, "CascadeShadowPass", 17) != 0) return -1;
  const char c = n[17];
  return c >= '0' && c <= '7' && n[18] == 0 ? c - '0' : -1;
}
inline bool RasterCls(int c) {
  return c == kCOther || c == kCGBuffer || c == kCSsao || c == kCLight || c == kCTransp;
}
// Does an arm's pass set take a region pass of class c (gbMain: the view's main G-buffer pass)?
inline bool Wants(uint32_t passes, int c, bool gbMain) {
  if (!RasterCls(c)) return false;
  if (passes & kPAllRaster) return true;
  switch (c) {
    case kCGBuffer: return gbMain && (passes & kPGbMain) != 0;
    case kCSsao: return (passes & kPSsao) != 0;
    case kCLight: return (passes & kPLight) != 0;
    case kCTransp: return (passes & kPTransp) != 0;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Ring tile map (pure)
// ---------------------------------------------------------------------------
// One view's mapping: viewport (target pixels), the projection's x / y rows (ndc = s * tan + c; y up), the ring
// centre in target pixels and the inner radius (<= 0: the whole view coarse).
struct RingIn {
  int tw = 0, th = 0;  // surface tiles (ceil(target / 16))
  double vx = 0, vy = 0, vw = 0, vh = 0;
  double sx = 1, sy = 1, cx = 0, cy = 0;
  double gpx = 0, gpy = 0;
  double innerDeg = 0;
};
struct RingStats {
  int tilesIn = 0, coarseIn = 0;  // tiles whose centre is inside the viewport, and how many of them are coarse
};
// The gaze direction (tangents in the view's frame, y up) as a target pixel.
inline void GazePixel(double sx, double sy, double cx, double cy, double vx, double vy, double vw, double vh, double gx,
                      double gy, double* px, double* py) {
  *px = vx + (sx * gx + cx + 1.0) * 0.5 * vw;
  *py = vy + (1.0 - (sy * gy + cy)) * 0.5 * vh;
}
inline void PixelDir(const RingIn& r, double px, double py, double d[3]) {
  const double nx = (px - r.vx) / r.vw * 2.0 - 1.0, ny = 1.0 - (py - r.vy) / r.vh * 2.0;
  d[0] = (nx - r.cx) / r.sx;
  d[1] = (ny - r.cy) / r.sy;
  d[2] = 1.0;
}
inline double CosBetween(const double a[3], const double b[3]) {
  const double ab = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const double aa = a[0] * a[0] + a[1] * a[1] + a[2] * a[2], bb = b[0] * b[0] + b[1] * b[1] + b[2] * b[2];
  return ab / std::sqrt(aa * bb);
}
// out[tw * th]: 1 = coarse (the arm's rate), 0 = full rate. A tile is coarse only when every point of it that can be
// nearest to the ring centre (the centre clamped into the tile, its corners) lies at or beyond the inner radius, so
// the full-rate disc is never smaller than asked. Tiles outside the viewport are 0. Returns the stats.
inline RingStats BuildTiles(const RingIn& r, uint8_t* out) {
  RingStats st;
  if (r.tw <= 0 || r.th <= 0 || !out) return st;
  const bool whole = !(r.innerDeg > 0) || !(r.vw > 0) || !(r.vh > 0) || !(r.sx > 0) || !(r.sy > 0);
  const double cosIn = std::cos(r.innerDeg * 3.14159265358979323846 / 180.0);
  double g[3];
  PixelDir(r, r.gpx, r.gpy, g);
  const double x1v = r.vx + r.vw, y1v = r.vy + r.vh;
  for (int ty = 0; ty < r.th; ++ty)
    for (int tx = 0; tx < r.tw; ++tx) {
      const double x0 = tx * 16.0, y0 = ty * 16.0, x1 = x0 + 16.0, y1 = y0 + 16.0;
      const double mx = x0 + 8.0, my = y0 + 8.0;
      const bool centreIn = mx >= r.vx && mx < x1v && my >= r.vy && my < y1v;
      const bool overlaps = x1 > r.vx && x0 < x1v && y1 > r.vy && y0 < y1v;
      uint8_t v = 0;
      if (whole) {
        v = 1;
      } else if (overlaps) {
        const double cx0 = (std::max)(x0, r.vx), cy0 = (std::max)(y0, r.vy);
        const double cx1 = (std::min)(x1, x1v), cy1 = (std::min)(y1, y1v);
        const double pts[5][2] = {{(std::min)((std::max)(r.gpx, cx0), cx1), (std::min)((std::max)(r.gpy, cy0), cy1)},
                                  {cx0, cy0}, {cx1, cy0}, {cx0, cy1}, {cx1, cy1}};
        v = 1;
        for (const auto& p : pts) {
          double d[3];
          PixelDir(r, p[0], p[1], d);
          if (CosBetween(d, g) > cosIn) {
            v = 0;
            break;
          }
        }
      }
      out[ty * r.tw + tx] = v;
      if (centreIn) {
        ++st.tilesIn;
        st.coarseIn += v;
      }
    }
  return st;
}

// ---------------------------------------------------------------------------
// R140: default frame buffer, surface content key, upload budget (pure)
// ---------------------------------------------------------------------------
// DCS's default frame buffer: bound between passes (so at nearly every pass start) and again by each pass's
// popFrameBuffer at its end; never drawn into (dcs-depth-targets). Live R139 it was the periphery's DLSS output size
// (1076x1060) and sized the focus views' and cascades' surfaces whenever it was the last bind. Learnt as the target
// bound at most pass starts (a vote that a few other starts do not overturn).
struct FbTrack {
  const void* rt = nullptr;  // RTV 0 (identity only)
  const void* ds = nullptr;  // DSV (identity only)
  int votes = 0;
  bool known = false;        // 8 votes reached; kept until the candidate is replaced
  void Note(const void* r, const void* d) {
    if (r == rt && d == ds) {
      if (votes < 64) ++votes;
      if (votes >= 8) known = true;
    } else if (--votes <= 0) {
      rt = r, ds = d, votes = 1, known = false;
    }
  }
  bool Known() const { return known && (rt || ds); }
  // A bind of it: its RTV 0, or (no RTV bound) its depth.
  bool Is(const void* r, const void* d) const {
    if (!Known()) return false;
    if (r) return r == rt;
    return d && d == ds;
  }
};
// What a surface's content depends on: whole view or ring; for a ring the target size, the gaze-centre tile, the
// projection (coarsely: the focus view follows the gaze) and the radius. Not the viewport (R139 keyed it on the
// current viewport, which alternates between targets: an upload at almost every apply).
inline uint64_t ContentKey(bool ring, UINT tw, UINT th, UINT w, UINT h, double gpx, double gpy, double sx, double sy,
                           double cx, double cy, double innerDeg) {
  auto q = [](uint64_t k, double v) {
    return ftwin::Mix64(k, static_cast<uint64_t>(static_cast<int64_t>(std::llround(v))));
  };
  uint64_t key = ftwin::Mix64(0x9e3779b97f4a7c15ull, ring ? 2 : 1);
  key = ftwin::Mix64(key, tw * 65536ull + th);
  if (!ring) return key;
  key = ftwin::Mix64(key, w * 65536ull + h);
  key = q(q(key, std::floor(gpx / 16.0)), std::floor(gpy / 16.0));
  key = q(q(q(q(key, sx * 256.0), sy * 256.0), cx * 64.0), cy * 64.0);
  return q(key, innerDeg * 100.0);
}
// At most one upload per view per frame. A surface whose content changed waits when its view already uploaded this
// frame (it keeps its old content). Among the surfaces of a view still waiting (they wanted an upload in this frame or
// the last one), the one uploaded longest ago goes first: a surface yields to an older waiting one, so the stale
// surfaces of a view take turns instead of the first-applied one winning every frame.
struct UploadBudget {
  static constexpr int kN = 32;  // surface indices (kMaxSurfaces below)
  int64_t frame = -1;            // the frame of the view's last upload
  int64_t lastUp[kN], lastWant[kN];
  uint32_t members = 0;          // surfaces of this view seen
  UploadBudget() {
    for (int i = 0; i < kN; ++i) lastUp[i] = lastWant[i] = -1;
  }
  void DropWaiting() {
    for (int i = 0; i < kN; ++i) lastWant[i] = lastUp[i];
  }
};
// True: upload surface i now (frame: the render thread's frame count).
inline bool TakeBudget(UploadBudget& b, int i, uint64_t frame) {
  i &= UploadBudget::kN - 1;
  const int64_t f = static_cast<int64_t>(frame);
  b.members |= 1u << i;
  bool wait = b.frame == f;
  for (int j = 0; j < UploadBudget::kN && !wait; ++j)
    if (j != i && (b.members >> j & 1) && b.lastWant[j] > b.lastUp[j] && b.lastWant[j] + 1 >= f &&
        b.lastUp[j] < b.lastUp[i])
      wait = true;  // an older surface of this view is waiting
  if (wait) {
    b.lastWant[i] = f;
    return false;
  }
  b.frame = f, b.lastUp[i] = b.lastWant[i] = f;
  return true;
}
// Surface i's content is current: it is not waiting.
inline void BudgetUpToDate(UploadBudget& b, int i) {
  i &= UploadBudget::kN - 1;
  b.lastWant[i] = b.lastUp[i];
}

// ---------------------------------------------------------------------------
// NvAPI binding (testable with a fake nvapi_QueryInterface)
// ---------------------------------------------------------------------------
struct Api {
  nvvrs::SetRatesFn setRates = nullptr;
  nvvrs::CreateViewFn createView = nullptr;
  nvvrs::SetViewFn setView = nullptr;
  uint32_t capsBits = 0;
  int capsVer = 0;  // 2 or 1: the caps struct version the driver took
  bool ok = false;
};
// 0 ok; else the reason (why: the text of the one log line).
enum BindFail : int { kBfOk = 0, kBfNoQuery, kBfInit, kBfEntry, kBfCaps, kBfNoVrs, kBfNoDevice };
inline int Bind(nvvrs::QueryFn q, ID3D11Device* dev, Api* a, char* why, size_t cap) {
  *a = Api{};
  if (!q) return snprintf(why, cap, "nvapi64.dll or its nvapi_QueryInterface not found"), kBfNoQuery;
  if (!dev) return snprintf(why, cap, "DCS's device not known"), kBfNoDevice;
  const auto init = reinterpret_cast<nvvrs::InitFn>(q(nvvrs::kId_Initialize));
  const int is = init ? init() : nvvrs::kNoImplementation;
  if (is != nvvrs::kOk) return snprintf(why, cap, "NvAPI_Initialize failed (status %d)", is), kBfInit;
  const auto caps = reinterpret_cast<nvvrs::GetCapsFn>(q(nvvrs::kId_D3D1x_GetGraphicsCapabilities));
  a->setRates = reinterpret_cast<nvvrs::SetRatesFn>(q(nvvrs::kId_D3D11_RSSetViewportsPixelShadingRates));
  a->createView = reinterpret_cast<nvvrs::CreateViewFn>(q(nvvrs::kId_D3D11_CreateShadingRateResourceView));
  a->setView = reinterpret_cast<nvvrs::SetViewFn>(q(nvvrs::kId_D3D11_RSSetShadingRateResourceView));
  if (!caps || !a->setRates || !a->createView || !a->setView)
    return snprintf(why, cap, "an NvAPI VRS entry point is missing (caps %d, rates %d, create %d, bind %d)", caps != nullptr,
                    a->setRates != nullptr, a->createView != nullptr, a->setView != nullptr),
           kBfEntry;
  nvvrs::GraphicsCapsV2 c2{};
  int st = caps(dev, nvvrs::kCapsVer2, &c2);
  if (st == nvvrs::kOk) {
    a->capsBits = c2.bits, a->capsVer = 2;
  } else {
    nvvrs::GraphicsCapsV1 c1{};
    const int st1 = caps(dev, nvvrs::kCapsVer1, &c1);
    if (st1 != nvvrs::kOk)
      return snprintf(why, cap, "NvAPI_D3D1x_GetGraphicsCapabilities failed (status %d with caps v2, %d with v1)", st, st1),
             kBfCaps;
    a->capsBits = c1.bits, a->capsVer = 1;
  }
  if (!(a->capsBits & nvvrs::kCapVrsBit))
    return snprintf(why, cap, "the GPU / driver reports no variable pixel rate shading (caps v%d bits 0x%x)", a->capsVer,
                    a->capsBits),
           kBfNoVrs;
  a->ok = true;
  snprintf(why, cap, "NvAPI VRS available (caps v%d bits 0x%x)", a->capsVer, a->capsBits);
  return kBfOk;
}
// nvapi64.dll from System32 (the display driver's), pinned: its entry points stay valid for the process.
inline nvvrs::QueryFn SystemQuery() {
  HMODULE nv = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!nv) return nullptr;
  HMODULE pinned = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"nvapi64.dll", &pinned);
  return reinterpret_cast<nvvrs::QueryFn>(GetProcAddress(nv, "nvapi_QueryInterface"));
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
std::mutex g_keyMutex;
Keys g_keys;                       // the config tick's copy (ReadConfig)
std::atomic<int> g_keyBlock{0};    // g_keys.blockSec, for the cheap checks

// Start gate (R53): frames flowing for VrsProbeStartDelay seconds.
std::atomic<bool> g_steady{false};
inline int SteadyStep(int run, uint64_t df, uint64_t dtMs) {
  if (dtMs == 0) return run;
  return df * 1000 >= 15 * dtMs ? run + 1 : 0;
}

Api g_api;
int g_bindState = 0;  // 0 not tried, 1 ok, -1 failed (logged once; not retried in the session)
nvvrs::QueryFn g_queryOverride = nullptr;  // tests: a fake nvapi_QueryInterface
ID3D11Device* g_dev = nullptr;              // AddRef'd while a phase runs
ID3D11DeviceContext* g_ctx = nullptr;
bool g_createOnRender = false;              // single-threaded device: surfaces created on the render thread

// The block's arm (suite thread writes, render thread reads at pass boundaries).
std::atomic<uint32_t> g_cfgPasses{0};
std::atomic<int> g_cfgInnerCenti{0};
std::atomic<int> g_cfgRate{nvvrs::k2x2};
std::atomic<uint32_t> g_cfgViews{0xC};
std::atomic<uint32_t> g_cfgGen{0};  // R140: bumped by every SetArm (the render thread drops its waiting uploads)
std::atomic<bool> g_on{false};      // callbacks installed and tracking
std::atomic<bool> g_fault{false};
std::atomic<int> g_busy{0};         // render-thread entries in flight
std::atomic<DWORD> g_rt{0};         // the render thread (ptiming::g_topTid at the start)

struct Counters {
  std::atomic<uint64_t> applies{0}, reOm{0}, reVp{0}, reEcl{0}, reNested{0}, clears{0}, setFail{0}, noSurface{0},
      noSize{0}, noProj{0}, tcSeen{0}, tcFocusByOrder{0}, tcOther{0}, dsvHits{0}, gbNoView{0}, armForeign{0},
      armMeter{0}, hookLost{0}, gapApplies{0}, uploads{0}, tagFlips{0}, created{0}, createFail{0}, frames{0},
      ticks{0}, obsOm{0}, obsVp{0}, fbBinds{0}, upDeferred{0};
};
Counters g_c;

// Surfaces (slot 0-3 HMD views, 4-5 cascades c0 / c1), one per (slot, tile size).
constexpr int kSlots = 6;
constexpr int kMaxSurfaces = 24;
struct Surface {
  int slot = -1;
  UINT tw = 0, th = 0;
  UINT w = 0, h = 0;  // R140: the target size it was made for (the ring's extent)
  ID3D11Texture2D* tex = nullptr;
  nvvrs::ShadingRateView* view = nullptr;
  uint64_t key = 0;  // the content uploaded (ContentKey; 0 = all full rate, as created)
};
Surface g_surf[kMaxSurfaces];
std::atomic<int> g_nSurf{0};  // published count (release by the creator, acquire by the render thread)
SRWLOCK g_reqLock = SRWLOCK_INIT;
struct Req {
  int slot;
  UINT tw, th, w, h;
};
Req g_req[8];
int g_nReq = 0;

// Per surface (render thread writes, the suite thread reads at block ends): its content's stats and use.
struct SurfInfo {
  std::atomic<int> tilesIn{0}, coarseIn{0}, cxPx{0}, cyPx{0}, src{0};  // src: 0 whole view, 1 viewport centre, 2 gaze
  std::atomic<uint64_t> applies{0}, uploads{0};
};
SurfInfo g_si[kMaxSurfaces];
uint64_t g_siLogged[kMaxSurfaces] = {};  // suite thread: applies at the last surface log

// Render-thread state.
struct Proj {
  bool ok = false;
  double sx = 0, sy = 0, cx = 0, cy = 0, w = 0, h = 0;
};
struct DsvEntry {
  ID3D11Resource* tex = nullptr;  // identity only
  UINT w = 0, h = 0;
  int view = -1, mainOrd = 0;
};
struct Rt {
  int view = -1;        // the current HMD view: -1 unknown, -2 not an HMD view (MFD), 0..3
  int region = 0;       // 0 before the view's main G-buffer, 1 open, 2 over (DLSS / post)
  int gbOrd = 0;        // GBufferPass ordinal in the view
  int gbDepth = -1;     // depth of the open GBufferPass (-1 none)
  void* gbPass = nullptr;
  bool gbMain = false, tcSeen = false;
  ID3D11DepthStencilView* beginDsv = nullptr;  // identity only: the default frame buffer's
  ID3D11Resource* dsvTex = nullptr;            // the pass's own depth texture (identity)
  UINT dsvW = 0, dsvH = 0;
  bool bound = false;   // the owned pass bound a target (rtW / rtH valid)
  UINT rtW = 0, rtH = 0;
  D3D11_VIEWPORT vp{};
  UINT vpN = 0;
  bool vpSet = false;
  bool want = false;    // the owned pass takes the state
  void* ownPass = nullptr;
  int ownDepth = -1;
  int slot = -1;        // its surface slot
  int applied = -1;     // surface index bound (-1 none)
  bool dirty = false;   // something was set since the last clear
  bool gap = false;     // the enlight gap is open
  int gapSlot = -1;
  bool hooks = false, usArmed = false;
  bool hk[5] = {};      // per hook kind: written by this arm
  uint8_t periph[2][ftwin::kTcBytes] = {};
  bool periphOk[2] = {};
  int focusN = 0;
  bool viewsSeen = false;
  bool inOwn = false;   // our own NvAPI / upload calls (re-entry guard)
  Proj proj[4];
  UINT slotW[kSlots] = {}, slotH[kSlots] = {};  // learnt target size per slot (main G-buffer depth)
  D3D11_VIEWPORT slotVp[kSlots] = {};
  DsvEntry dsv[16];
  int nDsv = 0;
  fixdepth::GazeOut gaze{};
  bool gazeOk = false;
  FbTrack fb;                      // R140: DCS's default frame buffer
  UploadBudget budget[kSlots];     // R140: one upload per view per frame
  uint32_t gen = 0;                // R140: g_cfgGen last seen
  bool frameOpen = false;          // R140: a frame start counted; closed by a view (its upload, its HMD copy)
};
Rt g_s;
std::vector<uint8_t> g_tiles;  // render thread: map scratch

// Pass info cache (render thread).
struct PassInfo {
  uint8_t cls = kCOther;
  int8_t casc = -1;
  bool decal = false;  // TransparentDecalPass (the enlight gap follows)
};
std::unordered_map<void*, PassInfo> g_info;
// Report tags per pass object: cls | (view + 1) << 4 | main << 7 | (casc + 1) << 8 | shared << 15.
std::mutex g_tagMutex;
std::unordered_map<void*, uint16_t> g_tag;

// Gaze (Perception GazeOut, read-only).
HANDLE g_gazeMap = nullptr;
const void* g_gazeView = nullptr;

// Context table hooks.
using Ctx = ID3D11DeviceContext;
using OmFn = void(STDMETHODCALLTYPE*)(Ctx*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using OmUavFn = void(STDMETHODCALLTYPE*)(Ctx*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT,
                                         UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using VpFn = void(STDMETHODCALLTYPE*)(Ctx*, UINT, const D3D11_VIEWPORT*);
using UsFn = void(STDMETHODCALLTYPE*)(Ctx*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using Us1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*,
                                       UINT, UINT, UINT);
enum HookK { kHOm, kHOmUav, kHVp, kHUs, kHUs1, kHCount };
const char* const kHookSlotNames[kHCount] = {"OMSetRenderTargets", "OMSetRenderTargetsAndUnorderedAccessViews",
                                             "RSSetViewports", "UpdateSubresource", "UpdateSubresource1"};
int g_hSlot[kHCount] = {-1, -1, -1, -1, -1};
// d3d11's own entries our hooks forward to: written only when adopting an entry inside d3d11.dll, never cleared
// (a hook another forwarder adopted keeps a valid target).
void* volatile g_hOrig[kHCount] = {};
void** g_hTable = nullptr;
void* HookFn(int k);

inline bool OnRender() { return GetCurrentThreadId() == g_rt.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// Render thread: surfaces and the NvAPI state
// ---------------------------------------------------------------------------
int FindSurface(int slot, UINT tw, UINT th) {
  const int n = g_nSurf.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i)
    if (g_surf[i].slot == slot && g_surf[i].tw == tw && g_surf[i].th == th && g_surf[i].view) return i;
  return -1;
}
bool CreateSurface(int slot, UINT tw, UINT th, UINT w, UINT h);  // below (the creating thread)
void Request(int slot, UINT tw, UINT th, UINT w, UINT h) {
  if (g_createOnRender) {
    CreateSurface(slot, tw, th, w, h);
    return;
  }
  AcquireSRWLockExclusive(&g_reqLock);
  bool have = false;
  for (int i = 0; i < g_nReq; ++i) have = have || (g_req[i].slot == slot && g_req[i].tw == tw && g_req[i].th == th);
  if (!have && g_nReq < 8) g_req[g_nReq++] = {slot, tw, th, w, h};
  ReleaseSRWLockExclusive(&g_reqLock);
}

// Target size of a resource view's resource (0 x 0 when not a 2D texture).
void ViewSize(ID3D11View* v, UINT* w, UINT* h, ID3D11Resource** resOut = nullptr) {
  *w = *h = 0;
  if (resOut) *resOut = nullptr;
  if (!v) return;
  ID3D11Resource* r = nullptr;
  v->GetResource(&r);
  if (!r) return;
  D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  r->GetType(&dim);
  if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
    D3D11_TEXTURE2D_DESC d{};
    static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
    *w = d.Width, *h = d.Height;
  }
  if (resOut) *resOut = r;
  r->Release();  // identity only: DCS keeps it bound
}

// The content wanted for surface i (of slot) in the current state; uploaded only when it changed, at most once per
// view per frame (a surface that must wait keeps its previous content: R140). False: a ring without the view's
// projection. The ring spans the surface's own target (DCS's per-view targets are drawn whole: viewport = target).
bool EnsureContent(int i, int slot, double innerDeg) {
  Surface& s = g_surf[i];
  const bool ring = slot < 4 && innerDeg > 0;
  RingIn in;
  in.tw = static_cast<int>(s.tw), in.th = static_cast<int>(s.th);
  in.vw = s.w ? s.w : s.tw * 16.0, in.vh = s.h ? s.h : s.th * 16.0;
  in.gpx = in.vw * 0.5, in.gpy = in.vh * 0.5;
  int src = 0;
  if (ring) {
    const Proj& p = g_s.proj[slot];
    if (!p.ok) return false;
    in.sx = p.sx, in.sy = p.sy, in.cx = p.cx, in.cy = p.cy, in.innerDeg = innerDeg;
    src = 1;
    if (g_s.gazeOk) {
      const fixdepth::GazeOut& g = g_s.gaze;
      const int gi = g.viewCount >= 4 ? slot : (slot & 1);
      const double gx = g.gazeTan[gi][0], gy = g.gazeTan[gi][1];
      if (std::isfinite(gx) && std::isfinite(gy) && std::fabs(gx) < 4 && std::fabs(gy) < 4) {
        GazePixel(in.sx, in.sy, in.cx, in.cy, 0, 0, in.vw, in.vh, gx, gy, &in.gpx, &in.gpy);
        src = 2;
      }
    }
  }
  const uint64_t key = ContentKey(ring, s.tw, s.th, s.w, s.h, in.gpx, in.gpy, in.sx, in.sy, in.cx, in.cy, innerDeg);
  UploadBudget& b = g_s.budget[slot];
  if (key == s.key) {
    BudgetUpToDate(b, i);
    return true;
  }
  if (!TakeBudget(b, i, g_c.frames.load(std::memory_order_relaxed))) {
    g_c.upDeferred.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  g_tiles.resize(static_cast<size_t>(s.tw) * s.th);
  const RingStats st = BuildTiles(in, g_tiles.data());
  g_s.inOwn = true;
  g_ctx->UpdateSubresource(s.tex, 0, nullptr, g_tiles.data(), s.tw, 0);
  g_s.inOwn = false;
  g_c.uploads.fetch_add(1, std::memory_order_relaxed);
  s.key = key;
  SurfInfo& si = g_si[i];
  si.tilesIn = st.tilesIn, si.coarseIn = st.coarseIn, si.cxPx = static_cast<int>(in.gpx);
  si.cyPx = static_cast<int>(in.gpy), si.src = src;
  si.uploads.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void SetRates(bool enable, UINT n) {
  nvvrs::ViewportRateDesc vp[nvvrs::kMaxViewports];
  const int rate = g_cfgRate.load(std::memory_order_relaxed);
  for (auto& v : vp) {
    v.enableVariablePixelShadingRate = enable;
    for (auto& t : v.shadingRateTable) t = nvvrs::kX1;
    v.shadingRateTable[1] = static_cast<nvvrs::PixelRate>(rate);
  }
  nvvrs::ViewportsRateDesc d{nvvrs::kViewportsRateVer, enable ? (std::max)(1u, (std::min)(n, 16u)) : 0u, vp};
  if (g_api.setRates(g_ctx, &d) != nvvrs::kOk) g_c.setFail.fetch_add(1, std::memory_order_relaxed);
}

// Binds the state for the owned pass (or the gap) with the surface of the bound target's size.
bool ApplyNow() {
  const int slot = g_s.want ? g_s.slot : (g_s.gap ? g_s.gapSlot : -1);
  if (slot < 0 || slot >= kSlots) return false;
  UINT w = g_s.bound ? g_s.rtW : g_s.slotW[slot], h = g_s.bound ? g_s.rtH : g_s.slotH[slot];
  if (!w || !h) {
    g_c.noSize.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const UINT tw = (w + 15) / 16, th = (h + 15) / 16;
  int i = FindSurface(slot, tw, th);
  if (i < 0) {
    Request(slot, tw, th, w, h);
    i = FindSurface(slot, tw, th);  // created now (single-threaded device)
    if (i < 0) {
      g_c.noSurface.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  }
  LARGE_INTEGER a, b;
  QueryPerformanceCounter(&a);
  const double inner = g_cfgInnerCenti.load(std::memory_order_relaxed) / 100.0;
  if (!EnsureContent(i, slot, inner)) {
    QueryPerformanceCounter(&b);
    g_c.ticks.fetch_add(static_cast<uint64_t>(b.QuadPart - a.QuadPart), std::memory_order_relaxed);
    g_c.noProj.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  g_si[i].applies.fetch_add(1, std::memory_order_relaxed);
  g_s.inOwn = true;
  if (g_api.setView(g_ctx, g_surf[i].view) != nvvrs::kOk) g_c.setFail.fetch_add(1, std::memory_order_relaxed);
  SetRates(true, g_s.vpSet ? g_s.vpN : 1);
  g_s.inOwn = false;
  QueryPerformanceCounter(&b);
  g_c.ticks.fetch_add(static_cast<uint64_t>(b.QuadPart - a.QuadPart), std::memory_order_relaxed);
  g_c.applies.fetch_add(1, std::memory_order_relaxed);
  g_s.applied = i;
  g_s.dirty = true;
  return true;
}
void ClearNow() {
  if (!g_s.dirty) return;
  LARGE_INTEGER a, b;
  QueryPerformanceCounter(&a);
  g_s.inOwn = true;
  SetRates(false, 0);
  if (g_api.setView(g_ctx, nullptr) != nvvrs::kOk) g_c.setFail.fetch_add(1, std::memory_order_relaxed);
  g_s.inOwn = false;
  QueryPerformanceCounter(&b);
  g_c.ticks.fetch_add(static_cast<uint64_t>(b.QuadPart - a.QuadPart), std::memory_order_relaxed);
  g_c.clears.fetch_add(1, std::memory_order_relaxed);
  g_s.applied = -1;
  g_s.dirty = false;
}

// ---------------------------------------------------------------------------
// Render thread: in-pass hooks (adopted over d3d11's own entries only)
// ---------------------------------------------------------------------------
bool SlotsKnown() {
  for (int k = 0; k < kHCount; ++k)
    if (g_hSlot[k] < 0) return false;
  return true;
}
// Our hook into slot k unless it is already ours; only over an entry inside d3d11.dll. True when ours now.
// The upload slots are refused silently: another hook of ours there (gpu pass timing, fixation depth) reports the
// uploads through g_uploadObserver anyway.
bool ArmOne(void** vt, int k, bool count) {
  void* cur = vt[g_hSlot[k]];
  if (cur != HookFn(k)) {
    if (!fixdepth::InD3d11(cur)) {
      if (count) g_c.armForeign.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    g_hOrig[k] = cur;
    d3ds::WriteTablePointer(&vt[g_hSlot[k]], HookFn(k));
  }
  g_s.hk[k] = true;  // ours now (a hook left in the table by an earlier arm, adopted by another one, is ours too)
  return true;
}
void DisarmOne(void** vt, int k) {
  if (!vt || !g_s.hk[k]) return;
  g_s.hk[k] = false;
  if (vt[g_hSlot[k]] == HookFn(k))
    d3ds::WriteTablePointer(&vt[g_hSlot[k]], g_hOrig[k]);
  else
    g_c.hookLost.fetch_add(1, std::memory_order_relaxed);  // d3d11 rewrote it, or another hook sits over ours
}
void ArmHooks() {
  if (g_s.hooks) return;
  if (d3ds::g_vtblCount.load(std::memory_order_relaxed) > 0) {  // the D3D meter re-arms these slots itself
    g_c.armMeter.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  void** vt = *reinterpret_cast<void***>(g_ctx);
  g_hTable = vt;
  ArmOne(vt, kHOm, true), ArmOne(vt, kHOmUav, true), ArmOne(vt, kHVp, true);
  g_s.hooks = true;
}
void DisarmHooks() {
  if (!g_s.hooks) return;
  DisarmOne(g_hTable, kHOm), DisarmOne(g_hTable, kHOmUav), DisarmOne(g_hTable, kHVp);
  g_s.hooks = false;
}
// The one-shot TerrainContext observer of a G-buffer pass.
void ArmUs() {
  if (g_s.usArmed || d3ds::g_vtblCount.load(std::memory_order_relaxed) > 0) return;
  void** vt = *reinterpret_cast<void***>(g_ctx);
  g_hTable = vt;
  ArmOne(vt, kHUs, false), ArmOne(vt, kHUs1, false);
  g_s.usArmed = true;
}
void DisarmUs() {
  if (!g_s.usArmed) return;
  DisarmOne(g_hTable, kHUs), DisarmOne(g_hTable, kHUs1);
  g_s.usArmed = false;
}
// Hooks needed now: the owned pass, the gap, or a G-buffer pass learning its target.
void SyncHooks() {
  if (g_on.load(std::memory_order_relaxed) && (g_s.want || g_s.gap || g_s.gbDepth >= 0))
    ArmHooks();
  else
    DisarmHooks();
}

// The view is known inside a G-buffer pass (main: the view's main pass).
void OnIdentified(int v, bool main) {
  g_s.view = v;
  if (v < 0) return;
  g_s.viewsSeen = true;
  if (main) g_s.region = 1, g_s.gbMain = true;
  if (!g_on.load(std::memory_order_relaxed) || g_s.want || g_s.gbDepth < 0) return;
  const uint32_t passes = g_cfgPasses.load(std::memory_order_relaxed);
  if (!(g_cfgViews.load(std::memory_order_relaxed) >> v & 1) || !Wants(passes, kCGBuffer, main)) return;
  g_s.want = true, g_s.slot = v, g_s.ownPass = g_s.gbPass, g_s.ownDepth = g_s.gbDepth;
  SyncHooks();
  ApplyNow();
}

UINT g_loggedW[kSlots] = {}, g_loggedH[kSlots] = {};  // the sizes logged in this phase (reset by Start)
void LogSlotSize(int slot, UINT w, UINT h, const D3D11_VIEWPORT& vp) {
  UINT* const lw = g_loggedW;
  UINT* const lh = g_loggedH;
  if (slot < 0 || slot >= kSlots || (lw[slot] == w && lh[slot] == h)) return;
  lw[slot] = w, lh[slot] = h;
  if (slot < 4) {
    const Proj& p = g_s.proj[slot];
    const double r2d = 180.0 / 3.14159265358979323846;
    Log("vrs probe: HMD view %d (%s) depth target %ux%u, viewport %.0f,%.0f %.0fx%.0f (%ux%u tiles); TerrainContext "
        "render %.0fx%.0f, half-FOV %.1f x %.1f deg, sx %.4f sy %.4f cx %.4f cy %.4f",
        slot, slot < 2 ? (slot ? "periphery R" : "periphery L") : (slot == 2 ? "focus L" : "focus R"), w, h,
        vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, (w + 15) / 16, (h + 15) / 16, p.w, p.h,
        p.ok ? std::atan(1.0 / p.sx) * r2d : 0.0, p.ok ? std::atan(1.0 / p.sy) * r2d : 0.0, p.sx, p.sy, p.cx, p.cy);
  } else {
    Log("vrs probe: cascade c%d depth target %ux%u (%ux%u tiles)", slot - 4, w, h, (w + 15) / 16, (h + 15) / 16);
  }
}

// A depth view bound inside the open G-buffer pass (DCS's own, not the default frame buffer's).
void NoteGbDsv(ID3D11DepthStencilView* d) {
  ID3D11Resource* tex = nullptr;
  UINT w = 0, h = 0;
  ViewSize(d, &w, &h, &tex);
  if (!tex || !w) return;
  g_s.dsvTex = tex, g_s.dsvW = w, g_s.dsvH = h;
  if (g_s.tcSeen) return;
  for (int i = 0; i < g_s.nDsv; ++i) {
    const DsvEntry& e = g_s.dsv[i];
    if (e.tex == tex && e.w == w && e.h == h) {
      g_c.dsvHits.fetch_add(1, std::memory_order_relaxed);
      if (e.view >= 0 && g_s.region == 0 && g_s.gbOrd == e.mainOrd)
        OnIdentified(e.view, true);  // the learnt main pass of that view (confirmed or corrected by its upload)
      else if (e.view >= 0 && g_s.region == 1)
        OnIdentified(e.view, false);
      return;
    }
  }
}
void LearnDsv(int v) {
  if (!g_s.dsvTex) return;
  for (int i = 0; i < g_s.nDsv; ++i)
    if (g_s.dsv[i].tex == g_s.dsvTex && g_s.dsv[i].w == g_s.dsvW && g_s.dsv[i].h == g_s.dsvH) {
      g_s.dsv[i].view = v, g_s.dsv[i].mainOrd = g_s.gbOrd;
      return;
    }
  DsvEntry& e = g_s.dsv[g_s.nDsv < 16 ? g_s.nDsv++ : (g_c.tcSeen.load() % 16)];
  e = {g_s.dsvTex, g_s.dsvW, g_s.dsvH, v, g_s.gbOrd};
}

// The TerrainContext upload of the open G-buffer pass.
void OnTc(const uint8_t* tc) {
  g_s.tcSeen = true;
  g_c.tcSeen.fetch_add(1, std::memory_order_relaxed);
  const ftwin::ViewId id = ftwin::ClassifyTc(tc);
  int v = -2;
  if (id.kind == ftwin::kPeriph) {
    v = id.eye;
    memcpy(g_s.periph[id.eye], tc, ftwin::kTcBytes);
    g_s.periphOk[id.eye] = true;
  } else if (id.kind == ftwin::kFocus) {
    int e = -1;
    for (int k = 0; k < 2 && e < 0; ++k)
      if (g_s.periphOk[k] && ftwin::SharedRowsEqual(g_s.periph[k], tc)) e = k;
    if (e < 0) {
      e = g_s.focusN > 0 ? 1 : 0;  // no periphery of this frame seen: the render order (L then R)
      g_c.tcFocusByOrder.fetch_add(1, std::memory_order_relaxed);
    }
    ++g_s.focusN;
    v = 2 + e;
  } else {
    g_c.tcOther.fetch_add(1, std::memory_order_relaxed);
  }
  if (v >= 0) {
    Proj& p = g_s.proj[v];
    p.ok = true, p.sx = id.sx, p.sy = id.sy, p.cx = id.cx, p.cy = id.cy, p.w = id.w, p.h = id.h;
  }
  LearnDsv(v);
  if (g_s.view != v && g_s.want && g_s.ownPass == g_s.gbPass) {
    ClearNow();  // the depth map named another view: this pass starts again under its upload's identity
    g_s.want = false, g_s.ownPass = nullptr, g_s.ownDepth = -1, g_s.slot = -1;
  }
  if (g_s.view != v || !g_s.gbMain) OnIdentified(v, true);
  DisarmUs();
}
bool IsTcBuffer(ID3D11Resource* r) {
  D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  r->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
  D3D11_BUFFER_DESC d{};
  static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
  return d.ByteWidth == static_cast<UINT>(ftwin::kTcBytes) && (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER);
}
void ObserveBody(ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src) {
  if (g_s.gbDepth < 0 || g_s.tcSeen || g_s.inOwn || sub || !r || !src) return;
  if (box && (box->left != 0 || box->right < static_cast<UINT>(ftwin::kTcBytes))) return;
  if (!IsTcBuffer(r)) return;
  OnTc(static_cast<const uint8_t*>(src));
}
DWORD NoteFault(const char*) {
  g_fault = true;
  return EXCEPTION_EXECUTE_HANDLER;
}
// pass_timing.h's g_uploadObserver (any thread; DCS's immediate context on the render thread only).
void Observe(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src) {
  if (c != g_ctx || !g_on.load(std::memory_order_relaxed) || g_fault.load(std::memory_order_relaxed) || !OnRender())
    return;
  g_busy.fetch_add(1);
  __try {
    ObserveBody(r, sub, box, src);
  } __except (NoteFault("upload")) {
  }
  g_busy.fetch_sub(1);
}

// After a forwarded OM call: the G-buffer's depth (identification), the bound size, the state again.
void AfterOmBody(UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d, bool ours) {
  if (n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) return;
  if (!g_s.want && !g_s.gap && g_s.gbDepth < 0) return;
  // R140: DCS's default frame buffer (a pass's popFrameBuffer): nothing is drawn into it; it neither sizes a surface
  // nor names a view's depth, and the state stays as the pass's own binds left it.
  if (g_s.fb.Is(n && r ? r[0] : nullptr, d)) {
    g_c.fbBinds.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // The bound target's size first (a G-buffer pass identified at this very bind applies with it).
  UINT w = 0, h = 0;
  ViewSize(n && r && r[0] ? static_cast<ID3D11View*>(r[0]) : static_cast<ID3D11View*>(d), &w, &h);
  if (w && h) g_s.bound = true, g_s.rtW = w, g_s.rtH = h;
  const bool wanted = g_s.want;
  if (g_s.gbDepth >= 0 && d && d != g_s.beginDsv) NoteGbDsv(d);
  if (g_s.want && !wanted) return;  // identified here: OnIdentified applied already
  if (!g_s.want && !g_s.gap) return;
  if (g_s.gap && !g_s.want && ours) return;  // our own work between passes: never under the state
  if (g_s.gap && !g_s.want && g_s.applied < 0) g_c.gapApplies.fetch_add(1, std::memory_order_relaxed);
  if (ApplyNow()) g_c.reOm.fetch_add(1, std::memory_order_relaxed);
}
void AfterOm(Ctx* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d, void* ret) {
  if (c != g_ctx || !g_on.load(std::memory_order_relaxed) || g_fault.load(std::memory_order_relaxed) || g_s.inOwn ||
      !OnRender())
    return;
  g_busy.fetch_add(1);
  __try {
    AfterOmBody(n, r, d, d3ds::InPayload(ret));
  } __except (NoteFault("om")) {
  }
  g_busy.fetch_sub(1);
}
void STDMETHODCALLTYPE H_Om(Ctx* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d) {
  void* ret = _ReturnAddress();
  reinterpret_cast<OmFn>(g_hOrig[kHOm])(c, n, r, d);
  AfterOm(c, n, r, d, ret);
}
void STDMETHODCALLTYPE H_OmUav(Ctx* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d, UINT us,
                               UINT un, ID3D11UnorderedAccessView* const* u, const UINT* ic) {
  void* ret = _ReturnAddress();
  reinterpret_cast<OmUavFn>(g_hOrig[kHOmUav])(c, n, r, d, us, un, u, ic);
  AfterOm(c, n, r, d, ret);
}
// R140: ptiming::g_omObserver (fixation_depth.h's in-pass OM hooks, after forwarding). Only while our own hook of that
// kind is not armed: with it armed it is in the call chain (another hook of ours adopts whatever is in the slot and
// forwards to it) and reports the call itself.
void ObsOm(Ctx* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d, bool uav, void* ret) {
  if (c != g_ctx || !OnRender() || g_s.hk[uav ? kHOmUav : kHOm]) return;
  g_c.obsOm.fetch_add(1, std::memory_order_relaxed);
  AfterOm(c, n, r, d, ret);
}
void AfterVpBody(UINT n, const D3D11_VIEWPORT* v) {
  if (n && v) g_s.vp = v[0], g_s.vpN = n, g_s.vpSet = true;
  if (g_s.gbDepth >= 0 && g_s.gbMain && g_s.view >= 0 && g_s.view < 4 && n && v) g_s.slotVp[g_s.view] = v[0];
  if ((g_s.want || g_s.applied >= 0) && ApplyNow()) g_c.reVp.fetch_add(1, std::memory_order_relaxed);
}
void AfterVp(Ctx* c, UINT n, const D3D11_VIEWPORT* v) {
  if (c != g_ctx || !g_on.load(std::memory_order_relaxed) || g_fault.load(std::memory_order_relaxed) || g_s.inOwn ||
      !OnRender())
    return;
  g_busy.fetch_add(1);
  __try {
    AfterVpBody(n, v);
  } __except (NoteFault("vp")) {
  }
  g_busy.fetch_sub(1);
}
void STDMETHODCALLTYPE H_Vp(Ctx* c, UINT n, const D3D11_VIEWPORT* v) {
  reinterpret_cast<VpFn>(g_hOrig[kHVp])(c, n, v);
  AfterVp(c, n, v);
}
// R140: ptiming::g_vpObserver (fixation_depth.h's in-pass viewport hook), as ObsOm.
void ObsVp(Ctx* c, UINT n, const D3D11_VIEWPORT* v) {
  if (c != g_ctx || !OnRender() || g_s.hk[kHVp]) return;
  g_c.obsVp.fetch_add(1, std::memory_order_relaxed);
  AfterVp(c, n, v);
}
void STDMETHODCALLTYPE H_Us(Ctx* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, UINT rp,
                            UINT dp) {
  Observe(c, r, sub, box, src);
  reinterpret_cast<UsFn>(g_hOrig[kHUs])(c, r, sub, box, src, rp, dp);
}
void STDMETHODCALLTYPE H_Us1(ID3D11DeviceContext1* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                             const void* src, UINT rp, UINT dp, UINT fl) {
  Observe(static_cast<Ctx*>(c), r, sub, box, src);
  reinterpret_cast<Us1Fn>(g_hOrig[kHUs1])(c, r, sub, box, src, rp, dp, fl);
}
void* HookFn(int k) {
  switch (k) {
    case kHOm: return reinterpret_cast<void*>(&H_Om);
    case kHOmUav: return reinterpret_cast<void*>(&H_OmUav);
    case kHVp: return reinterpret_cast<void*>(&H_Vp);
    case kHUs: return reinterpret_cast<void*>(&H_Us);
    case kHUs1: return reinterpret_cast<void*>(&H_Us1);
  }
  return nullptr;
}
// split_filter.h's g_afterEclProbe: a command list ran on the immediate context (our recorders, DCS through the split
// filter's hook). Its state may be gone: bind it again.
void AfterEcl(void* ctx) {
  if (ctx != static_cast<void*>(g_ctx) || !g_on.load(std::memory_order_relaxed) || g_fault.load() || g_s.inOwn ||
      !OnRender())
    return;
  g_busy.fetch_add(1);
  __try {
    if ((g_s.want || g_s.applied >= 0) && ApplyNow()) g_c.reEcl.fetch_add(1, std::memory_order_relaxed);
  } __except (NoteFault("ecl")) {
  }
  g_busy.fetch_sub(1);
}

// ---------------------------------------------------------------------------
// Render thread: pass boundaries
// ---------------------------------------------------------------------------
PassInfo InfoOf(void* pass) {
  auto it = g_info.find(pass);
  if (it != g_info.end()) return it->second;
  char n[48];
  fixdepth::PassName(pass, n, sizeof(n));
  PassInfo pi;
  pi.cls = ClassOfName(n);
  pi.casc = static_cast<int8_t>(CascadeOfName(n));
  pi.decal = strstr(n, "TransparentDecal") != nullptr;
  if (g_info.size() < 8192) g_info.emplace(pass, pi);
  return pi;
}
void Tag(void* pass, uint16_t t) {
  auto it = g_tag.find(pass);  // the render thread is the only writer: an unlocked find is safe
  if (it != g_tag.end() && ((it->second & 0x7fff) == t || (it->second & 0x8000))) return;  // same, or shared already
  std::lock_guard<std::mutex> lock(g_tagMutex);
  if (it == g_tag.end()) {
    if (g_tag.size() < 8192) g_tag.emplace(pass, t);
  } else {
    it->second = static_cast<uint16_t>(t | 0x8000);  // the same object under two tags: reported as shared
    g_c.tagFlips.fetch_add(1, std::memory_order_relaxed);
  }
}
void ReadGaze() {
  g_s.gazeOk = false;
  const void* v = g_gazeView;
  if (!v) return;
  const volatile uint64_t* seq = static_cast<const volatile uint64_t*>(v);
  for (int i = 0; i < 3; ++i) {
    const uint64_t a = *seq;
    if (a & 1) continue;
    memcpy(&g_s.gaze, v, sizeof(g_s.gaze));
    if (*seq == a) {
      g_s.gazeOk = (g_s.gaze.flags & fixdepth::kGazeValid) != 0 && g_s.gaze.viewCount >= 2;
      return;
    }
  }
}
void NewFrame() {
  g_s.frameOpen = true;
  g_s.periphOk[0] = g_s.periphOk[1] = false;
  g_s.focusN = 0;
  g_s.viewsSeen = false;
  g_s.view = -1, g_s.region = 0;
  g_c.frames.fetch_add(1, std::memory_order_relaxed);
  ReadGaze();
}
void EndOwned() {
  ClearNow();
  g_s.want = false, g_s.ownPass = nullptr, g_s.ownDepth = -1, g_s.slot = -1;
  g_s.bound = false, g_s.vpSet = false;
}
void DecideTop(void* pass, const PassInfo& pi) {
  if (!g_on.load(std::memory_order_relaxed)) return;
  const uint32_t passes = g_cfgPasses.load(std::memory_order_relaxed);
  int slot = -1;
  if (pi.cls == kCCascade) {
    if ((passes & kPCascade) && (pi.casc == 0 || pi.casc == 1)) slot = 4 + pi.casc;
  } else if (g_s.view >= 0 && g_s.region == 1 && (g_cfgViews.load(std::memory_order_relaxed) >> g_s.view & 1)) {
    if (Wants(passes, pi.cls, false)) slot = g_s.view;  // a G-buffer pass here is not the main one (decal)
  }
  if (slot < 0) return;
  g_s.want = true, g_s.slot = slot, g_s.ownPass = pass, g_s.ownDepth = 0;
  g_s.bound = false, g_s.vpSet = false;
  SyncHooks();
  ApplyNow();  // with the slot's learnt size; again at the pass's own binds
}
void BoundaryBody(void* pass, bool begin, int depth) {
  const PassInfo pi = InfoOf(pass);
  if (begin) {
    if (depth == 0) {
      // R140: what is bound at a pass start (between passes: DCS's default frame buffer, by vote).
      ID3D11RenderTargetView* fr = nullptr;
      ID3D11DepthStencilView* fd = nullptr;
      g_ctx->OMGetRenderTargets(1, &fr, &fd);
      g_s.fb.Note(fr, fd);
      if (fr) fr->Release();  // identity only
      if (fd) fd->Release();
      const uint32_t gen = g_cfgGen.load(std::memory_order_relaxed);
      if (gen != g_s.gen) {  // a new arm: the waiting uploads of the old one are moot
        g_s.gen = gen;
        for (UploadBudget& b : g_s.budget) b.DropWaiting();
      }
      // A new frame: its first cascade or its height map, whichever comes first after the last frame's views (R140:
      // both counted before, two frames per frame, which halved R139's per-frame figures and aged waiting uploads).
      if ((pi.cls == kCFrame || pi.cls == kCCascade) && !g_s.frameOpen) NewFrame();
      if (pi.cls == kCLights) g_s.view = -1, g_s.region = 0, g_s.gbOrd = 0, g_s.gbMain = false, g_s.frameOpen = false;
      if ((pi.cls == kCDlss || pi.cls == kCPost) && g_s.region == 1) g_s.region = 2;
      if (!g_s.want) g_s.bound = false, g_s.vpSet = false;  // binds are this pass's own from here
    }
    if (pi.cls == kCGBuffer && g_s.gbDepth < 0) {
      g_s.gbDepth = depth, g_s.gbPass = pass, ++g_s.gbOrd;
      g_s.tcSeen = false, g_s.gbMain = false, g_s.dsvTex = nullptr;
      if (!g_s.want) g_s.bound = false, g_s.vpSet = false;
      ID3D11DepthStencilView* d = nullptr;
      g_ctx->OMGetRenderTargets(0, nullptr, &d);
      g_s.beginDsv = d;
      if (d) d->Release();  // identity only
      if (g_on.load(std::memory_order_relaxed)) {
        ArmUs();
        SyncHooks();
      }
    }
    if (depth == 0 && !g_s.want) {
      DecideTop(pass, pi);
    } else if (g_s.applied >= 0 && ApplyNow()) {
      g_c.reNested.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  // End. The G-buffer and cascade sizes are learnt first (EndOwned forgets the pass's binds).
  if (pi.cls == kCGBuffer && pass == g_s.gbPass && depth == g_s.gbDepth) {
    if (g_s.view >= 0 && g_s.view < 4 && g_s.gbMain && g_s.dsvW) {
      if (g_s.vpSet) g_s.slotVp[g_s.view] = g_s.vp;
      g_s.slotW[g_s.view] = g_s.dsvW, g_s.slotH[g_s.view] = g_s.dsvH;
      LogSlotSize(g_s.view, g_s.dsvW, g_s.dsvH, g_s.slotVp[g_s.view]);
    } else if (g_s.view == -1 && g_s.region == 0) {
      g_c.gbNoView.fetch_add(1, std::memory_order_relaxed);  // F1: the cockpit G-buffer pass too (no upload)
    }
    if (g_s.view >= 0 && g_s.gbMain && !g_s.tcSeen) LearnDsv(g_s.view);  // keeps the learnt main ordinal
    DisarmUs();
    g_s.gbDepth = -1, g_s.gbPass = nullptr;
  }
  if (pi.cls == kCCascade && depth == 0 && pi.casc >= 0 && pi.casc < 2 && g_s.bound && g_s.want &&
      pass == g_s.ownPass) {
    const int sl = 4 + pi.casc;
    if (g_s.slotW[sl] != g_s.rtW || g_s.slotH[sl] != g_s.rtH) {
      g_s.slotW[sl] = g_s.rtW, g_s.slotH[sl] = g_s.rtH;
      if (g_s.vpSet) g_s.slotVp[sl] = g_s.vp;
      LogSlotSize(sl, g_s.rtW, g_s.rtH, g_s.vp);
    }
  }
  if (g_s.want && pass == g_s.ownPass && depth == g_s.ownDepth) {
    EndOwned();
  } else if (g_s.applied >= 0 && ApplyNow()) {
    g_c.reNested.fetch_add(1, std::memory_order_relaxed);
  }
  if (depth == 0 && pi.decal && g_on.load(std::memory_order_relaxed) &&
      (g_cfgPasses.load(std::memory_order_relaxed) & kPEnlight) && g_s.view >= 0 && g_s.region == 1 &&
      (g_cfgViews.load(std::memory_order_relaxed) >> g_s.view & 1))
    g_s.gap = true, g_s.gapSlot = g_s.view;  // the enlight work outside passes until the next pass start
  if (depth == 0) {
    const int v = g_s.view >= 0 && g_s.region == 1 ? g_s.view : -1;
    const bool main = pi.cls == kCGBuffer && g_s.gbMain && g_s.region == 1;
    if (pi.cls == kCCascade)
      Tag(pass, static_cast<uint16_t>(kCCascade | (pi.casc + 1) << 8));
    else if (v >= 0 && RasterCls(pi.cls))
      Tag(pass, static_cast<uint16_t>(pi.cls | (v + 1) << 4 | (main ? 1 << 7 : 0)));
    if (pi.cls == kCHmd) g_s.view = -1, g_s.region = 0, g_s.frameOpen = false;
  }
  SyncHooks();
}
// Cascades bind their depth inside the pass and are never G-buffer passes: the OM hook sizes them (AfterOmBody). The
// cascade slot's size is learnt at the pass end from the binding seen.
void Boundary(void* pass, bool begin, int depth) {
  if (!OnRender() || g_fault.load(std::memory_order_relaxed)) return;
  g_busy.fetch_add(1);
  __try {
    BoundaryBody(pass, begin, depth);
  } __except (NoteFault("boundary")) {
  }
  g_busy.fetch_sub(1);
}
// First at every pass start: an open enlight gap closes before any other feature's boundary work; a stopped probe
// clears what it bound.
void PreBody(int depth) {
  if (depth != 0) return;
  if (g_s.gap) {
    g_s.gap = false, g_s.gapSlot = -1;
    ClearNow();
    g_s.bound = false, g_s.vpSet = false;
    SyncHooks();
  }
  if (!g_on.load(std::memory_order_relaxed) && (g_s.dirty || g_s.hooks || g_s.usArmed)) {
    g_s.want = false;
    ClearNow();
    DisarmHooks();
    DisarmUs();
  }
}
void Pre(void* /*pass*/, bool begin, int depth) {
  if (!begin || !OnRender()) return;
  g_busy.fetch_add(1);
  __try {
    PreBody(depth);
  } __except (NoteFault("pre")) {
  }
  g_busy.fetch_sub(1);
}

// ---------------------------------------------------------------------------
// Suite thread: start, surfaces, blocks, phase
// ---------------------------------------------------------------------------
bool CreateSurface(int slot, UINT tw, UINT th, UINT w, UINT h) {
  const int n = g_nSurf.load(std::memory_order_acquire);
  if (n >= kMaxSurfaces || !g_dev || FindSurface(slot, tw, th) >= 0) return false;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = tw, d.Height = th, d.MipLevels = 1, d.ArraySize = 1, d.Format = DXGI_FORMAT_R8_UINT;
  d.SampleDesc.Count = 1, d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  std::vector<uint8_t> zero(static_cast<size_t>(tw) * th, 0);
  D3D11_SUBRESOURCE_DATA init{zero.data(), tw, 0};
  ID3D11Texture2D* t = nullptr;
  if (FAILED(g_dev->CreateTexture2D(&d, &init, &t)) || !t) {
    g_c.createFail.fetch_add(1);
    return false;
  }
  nvvrs::ShadingRateViewDesc vd{};
  vd.version = nvvrs::kShadingRateViewVer, vd.Format = DXGI_FORMAT_R8_UINT, vd.ViewDimension = nvvrs::kSrrvTexture2D;
  vd.Texture2D.MipSlice = 0;
  nvvrs::ShadingRateView* v = nullptr;
  const int st = g_api.createView(g_dev, t, &vd, &v);
  if (st != nvvrs::kOk || !v) {
    t->Release();
    g_c.createFail.fetch_add(1);
    static int logged = 0;
    if (logged++ < 3) Log("vrs probe: NvAPI_D3D11_CreateShadingRateResourceView failed (status %d) for %ux%u", st, tw, th);
    return false;
  }
  Surface& s = g_surf[n];
  s.slot = slot, s.tw = tw, s.th = th, s.w = w, s.h = h, s.tex = t, s.view = v, s.key = 0;
  SurfInfo& si = g_si[n];
  si.tilesIn = 0, si.coarseIn = 0, si.cxPx = 0, si.cyPx = 0, si.src = 0, si.applies = 0, si.uploads = 0;
  g_siLogged[n] = 0;
  g_nSurf.store(n + 1, std::memory_order_release);
  g_c.created.fetch_add(1);
  return true;
}
void ServiceCreates() {
  Req r[8];
  int n = 0;
  AcquireSRWLockExclusive(&g_reqLock);
  n = g_nReq;
  for (int i = 0; i < n; ++i) r[i] = g_req[i];
  g_nReq = 0;
  ReleaseSRWLockExclusive(&g_reqLock);
  for (int i = 0; i < n; ++i) CreateSurface(r[i].slot, r[i].tw, r[i].th, r[i].w, r[i].h);
}
// Sleeps ms in 50 ms steps, creating requested surfaces; false when the phase must end.
bool Wait(int ms) {
  for (int t = 0; t < ms; t += 50) {
    ServiceCreates();
    if (g_benchAbort.load() || g_engineOff.load() || g_fault.load()) return false;
    Sleep(50);
  }
  ServiceCreates();
  return true;
}
void SetArm(int arm, const Keys& k) {
  const ArmDef a = ArmOf(arm, k);
  g_cfgRate = k.rate;
  g_cfgViews = k.views;
  g_cfgInnerCenti = static_cast<int>(std::lround(a.innerDeg * 100.0));
  g_cfgPasses = a.passes;
  g_cfgGen.fetch_add(1);
}

// Device, NvAPI, slots, gaze, callbacks. False (logged): the probe stays off.
bool Start() {
  ID3D11Device* dev = shadowinst::g_device;
  if (dev) {
    dev->AddRef();
  } else if (sfilt::Ctx* c = sfilt::g_ctx.load()) {
    c->GetDevice(&dev);
  }
  if (!dev) {
    Log("  vrs probe: DCS's device not known yet (it comes from [Model] ShadowInstancing=1 or [D3D] SplitFilter=1); "
        "probe off");
    return false;
  }
  if (g_bindState == 0) {
    char why[256] = {};
    const int r = Bind(g_queryOverride ? g_queryOverride : SystemQuery(), dev, &g_api, why, sizeof(why));
    g_bindState = r == kBfOk ? 1 : -1;
    Log(r == kBfOk ? "  vrs probe: %s" : "  vrs probe: %s; probe disabled for this session", why);
  }
  if (g_bindState != 1) {
    dev->Release();
    return false;
  }
  for (int k = 0; k < kHCount; ++k) g_hSlot[k] = DcsQv_ContextSlot(kHookSlotNames[k]);
  ptiming::Install();
  if (!SlotsKnown() || !ptiming::g_orig) {
    Log("  vrs probe: %s; probe off", ptiming::g_orig ? "context slots unknown" : "pass timing not installed");
    dev->Release();
    return false;
  }
  g_dev = dev;
  dev->GetImmediateContext(&g_ctx);
  g_createOnRender = shadowinst::g_deviceSingleThreaded;
  if (!g_gazeView) {
    g_gazeMap = OpenFileMappingW(FILE_MAP_READ, FALSE, fixdepth::GazeName().c_str());
    if (g_gazeMap) g_gazeView = MapViewOfFile(g_gazeMap, FILE_MAP_READ, 0, 0, sizeof(fixdepth::GazeOut));
  }
  g_s = Rt{};
  for (int s = 0; s < kSlots; ++s) g_loggedW[s] = g_loggedH[s] = 0;
  g_info.clear();
  {
    std::lock_guard<std::mutex> lock(g_tagMutex);
    g_tag.clear();
  }
  g_fault = false;
  g_rt = ptiming::g_topTid.load();
  SetArm(0, Keys{});
  g_on = true;
  ptiming::g_uploadObserver = &Observe;
  ptiming::g_omObserver = &ObsOm;  // R140
  ptiming::g_vpObserver = &ObsVp;
  sfilt::g_afterEclProbe = &AfterEcl;
  ptiming::g_vrsPre = &Pre;
  ptiming::g_vrsBoundary = &Boundary;
  Log("  vrs probe: started (device %s-threaded, gaze %s, render thread %lu)", g_createOnRender ? "single" : "multi",
      g_gazeView ? "GazeOut mapped" : "not available: ring centre = viewport centre",
      static_cast<unsigned long>(g_rt.load()));
  return true;
}

// Everything back: the render thread clears at its next pass start (g_on false), then the callbacks go, leftover
// hooks are restored, the surfaces released.
void StopProbe() {
  if (!g_ctx) return;
  g_cfgPasses = 0;
  g_on = false;
  for (int i = 0; i < 40 && (g_s.dirty || g_s.hooks || g_s.usArmed) && !g_fault.load(); ++i) Sleep(25);
  ptiming::BoundaryFn b = &Boundary, p = &Pre;
  ptiming::g_vrsBoundary.compare_exchange_strong(b, nullptr);
  ptiming::g_vrsPre.compare_exchange_strong(p, nullptr);
  ptiming::UploadObserverFn o = &Observe;
  ptiming::g_uploadObserver.compare_exchange_strong(o, nullptr);
  ptiming::OmObserverFn om = &ObsOm;
  ptiming::g_omObserver.compare_exchange_strong(om, nullptr);
  ptiming::VpObserverFn vo = &ObsVp;
  ptiming::g_vpObserver.compare_exchange_strong(vo, nullptr);
  void (*e)(void*) = &AfterEcl;
  sfilt::g_afterEclProbe.compare_exchange_strong(e, nullptr);
  for (int i = 0; i < 200 && g_busy.load(); ++i) Sleep(5);
  Sleep(50);
  __try {
    if (g_s.dirty) {  // no pass started since: nothing renders, the stopping thread may clear
      Log("  vrs probe: state still bound at the stop; cleared from the suite thread");
      ClearNow();
    }
    if (g_hTable)
      for (int k = 0; k < kHCount; ++k)
        if (g_hTable[g_hSlot[k]] == HookFn(k)) d3ds::WriteTablePointer(&g_hTable[g_hSlot[k]], g_hOrig[k]);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  g_s.hooks = g_s.usArmed = false;
  const int n = g_nSurf.load();
  g_nSurf = 0;
  for (int i = 0; i < n; ++i) {
    if (g_surf[i].view) g_surf[i].view->Release();
    if (g_surf[i].tex) g_surf[i].tex->Release();
    g_surf[i] = Surface{};
  }
  g_nReq = 0;
  g_ctx->Release();
  g_dev->Release();
  g_ctx = nullptr;
  g_dev = nullptr;
}

// One block: GPU pass timing with pipeline statistics, per tag GPU ms and PS invocations, frame GPU busy.
constexpr int kGroups = 13;  // gbmain, gb, ssao, light, transp, raster x {focus set, other views}; cascades c0+c1
const char* const kGroupNames[kGroups] = {"gbmain/F", "gb/F", "ssao/F", "light/F", "transp/F", "raster/F",
                                          "gbmain/P", "gb/P", "ssao/P", "light/P", "transp/P", "raster/P", "casc01"};
struct BlockOut {
  bool ok = false;
  int frames = 0, statFrames = 0;
  double busy = 0, span = 0;
  double ms[kGroups] = {}, ps[kGroups] = {};
  uint64_t applies = 0, re = 0, setFail = 0, noSurface = 0, tc = 0, rtFrames = 0, uploads = 0, deferred = 0;
  double usPerFrame = 0;
};
std::unordered_map<void*, uint16_t> g_tagSnap;  // the suite thread's copy for TagName
std::string TagName(void* p) {
  auto it = g_tagSnap.find(p);
  if (it == g_tagSnap.end()) return "-";
  const uint16_t t = it->second;
  char b[48];
  const int cls = t & 15, view = (t >> 4 & 7) - 1, casc = (t >> 8 & 7) - 1;
  if (cls == kCCascade)
    snprintf(b, sizeof(b), "casc%d%s", casc, t & 0x8000 ? "*" : "");
  else
    snprintf(b, sizeof(b), "%s%s@v%d%s", ClsName(cls), t & 0x80 ? "main" : "", view, t & 0x8000 ? "*" : "");
  return b;
}
// Group of a tag name (-1: none). focusSet = the probe's views mask.
int GroupOf(const std::string& n, uint32_t focusSet) {
  if (n.compare(0, 5, "casc0") == 0 || n.compare(0, 5, "casc1") == 0) return 12;
  const size_t at = n.find("@v");
  if (at == std::string::npos || at + 2 >= n.size()) return -1;
  const int v = n[at + 2] - '0';
  if (v < 0 || v > 3) return -1;
  const std::string c = n.substr(0, at);
  int g = c == "gbmain" ? 0 : c == "gb" ? 1 : c == "ssao" ? 2 : c == "light" ? 3 : c == "transp" ? 4 : c == "raster" ? 5 : -1;
  if (g < 0) return -1;
  return (focusSet >> v & 1) ? g : g + 6;
}
BlockOut MeasureBlock(int ms, std::atomic<uint64_t>& frames, uint32_t focusSet) {
  BlockOut o;
  const uint64_t a0 = g_c.applies.load(), r0 = g_c.reOm.load() + g_c.reVp.load() + g_c.reEcl.load() + g_c.reNested.load(),
                 f0 = g_c.setFail.load(), s0 = g_c.noSurface.load(), t0 = g_c.ticks.load(), tc0 = g_c.tcSeen.load(),
                 fr0 = g_c.frames.load(), u0 = g_c.uploads.load(), dfr0 = g_c.upDeferred.load();
  gpt::g_statsWanted = true;
  const bool began = gpt::Begin(frames, false);
  const bool waited = began && Wait(ms);
  gpt::End();
  gpt::g_statsWanted = false;
  std::vector<gpt::Done> done = gpt::TakeDone();
  if (!began || !waited) return o;
  {
    std::lock_guard<std::mutex> lock(g_tagMutex);
    g_tagSnap = g_tag;
  }
  std::vector<double> busy, span;
  for (const gpt::Done& d : done) {
    double s = 0, b = 0, x = 0;
    if (gpt::FrameGpu(d, &s, &b, &x)) busy.push_back(b), span.push_back(s);
  }
  const gpt::Result r = gpt::Analyze(done, &TagName);
  o.frames = r.frames, o.statFrames = r.statFrames;
  o.busy = gpt::Mean(busy), o.span = gpt::Mean(span);
  for (const auto& kv : r.kinds) {
    const int g = GroupOf(kv.first, focusSet);
    if (g >= 0) o.ms[g] += gpt::Mean(kv.second.perFrame);
  }
  if (r.statFrames)
    for (const auto& kv : r.pipe) {
      const int g = GroupOf(kv.first, focusSet);
      if (g >= 0) o.ps[g] += static_cast<double>(kv.second.sum.ps) / r.statFrames / 1e6;
    }
  o.applies = g_c.applies.load() - a0;
  o.re = g_c.reOm.load() + g_c.reVp.load() + g_c.reEcl.load() + g_c.reNested.load() - r0;
  o.setFail = g_c.setFail.load() - f0, o.noSurface = g_c.noSurface.load() - s0, o.tc = g_c.tcSeen.load() - tc0;
  o.rtFrames = g_c.frames.load() - fr0;
  o.uploads = g_c.uploads.load() - u0, o.deferred = g_c.upDeferred.load() - dfr0;
  o.usPerFrame = (g_c.ticks.load() - t0) * g_qpcToUs / static_cast<double>((std::max<uint64_t>)(1, o.rtFrames));
  o.ok = o.frames > 0 && !busy.empty();
  return o;
}
void LogBlock(int i, int n, const Block& b, const BlockOut& o) {
  if (!o.ok) {
    Log("  vrs probe block %d/%d A%d %s rep %d: not measured", i + 1, n, b.on ? b.arm : 0, b.on ? "ON " : "OFF",
        b.rep);
    return;
  }
  std::string g;
  char buf[64];
  for (int k = 0; k < kGroups; ++k) {
    if (o.ms[k] < 0.0005 && o.ps[k] < 0.0005) continue;
    snprintf(buf, sizeof(buf), " %s %.3f/%.2fM", kGroupNames[k], o.ms[k], o.ps[k]);
    g += buf;
  }
  Log("  vrs probe block %d/%d A%d %s rep %d: %d frames (%d with stats), GPU busy %.3f ms span %.3f; applies %llu, "
      "re-applies %llu, NvAPI failures %llu, no surface %llu, TerrainContexts %llu, map uploads %llu (waited %llu), "
      "our render-thread %.1f us/frame;%s",
      i + 1, n, b.on ? b.arm : 0, b.on ? "ON " : "OFF", b.rep, o.frames, o.statFrames, o.busy, o.span,
      static_cast<unsigned long long>(o.applies), static_cast<unsigned long long>(o.re),
      static_cast<unsigned long long>(o.setFail), static_cast<unsigned long long>(o.noSurface),
      static_cast<unsigned long long>(o.tc), static_cast<unsigned long long>(o.uploads),
      static_cast<unsigned long long>(o.deferred), o.usPerFrame, g.c_str());
}
inline const char* SlotName(int s) {
  static const char* const k[kSlots] = {"HMD view 0 (periphery L)", "HMD view 1 (periphery R)", "HMD view 2 (focus L)",
                                        "HMD view 3 (focus R)", "cascade c0", "cascade c1"};
  return s >= 0 && s < kSlots ? k[s] : "?";
}
// R140: every surface applied since the last log (the arm's own), with its own target size and content.
void LogSurfaces() {
  const int n = g_nSurf.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i) {
    const Surface& s = g_surf[i];
    const SurfInfo& si = g_si[i];
    const uint64_t ap = si.applies.load();
    if (ap == g_siLogged[i]) continue;
    const uint64_t used = ap - g_siLogged[i];
    g_siLogged[i] = ap;
    const int in = si.tilesIn.load(), co = si.coarseIn.load(), src = si.src.load();
    if (!si.uploads.load()) {
      Log("  vrs probe surface: %s target %ux%u, %ux%u tiles, content not uploaded yet (all full rate); %llu applies",
          SlotName(s.slot), s.w, s.h, s.tw, s.th, static_cast<unsigned long long>(used));
      continue;
    }
    Log("  vrs probe surface: %s target %ux%u, %ux%u tiles, %d of %d target tiles coarse (%.1f %%), %s%s%s; %llu applies "
        "since the last line, %llu uploads in all",
        SlotName(s.slot), s.w, s.h, s.tw, s.th, co, in, in ? 100.0 * co / in : 0.0,
        src == 2 ? "ring around the gaze, centre px " : src == 1 ? "ring around the target centre (no gaze), px " : "whole view",
        src ? std::to_string(si.cxPx.load()).c_str() : "", src ? ("," + std::to_string(si.cyPx.load())).c_str() : "",
        static_cast<unsigned long long>(used), static_cast<unsigned long long>(si.uploads.load()));
  }
}
void LogCounters(const char* what) {
  auto u = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load()); };
  Log("  vrs probe counters %s: frames %llu, applies %llu, re-applied after OM %llu / viewport %llu / command list %llu "
      "/ nested boundary %llu, clears %llu, NvAPI failures %llu, no surface %llu, no size %llu, ring without projection "
      "%llu; TerrainContexts %llu (focus eye by order %llu, other views %llu), depth-map hits %llu, G-buffer passes "
      "without a view %llu; hooks refused over a foreign entry %llu, by the D3D meter %llu, lost %llu; enlight gap "
      "applies %llu; map uploads %llu (waited for the view's per-frame budget %llu); surfaces %llu (failed %llu); "
      "shared pass objects %llu; OM / viewport calls seen through another hook of ours %llu / %llu; default frame "
      "buffer binds ignored %llu",
      what, u(g_c.frames), u(g_c.applies), u(g_c.reOm), u(g_c.reVp), u(g_c.reEcl), u(g_c.reNested), u(g_c.clears),
      u(g_c.setFail), u(g_c.noSurface), u(g_c.noSize), u(g_c.noProj), u(g_c.tcSeen), u(g_c.tcFocusByOrder),
      u(g_c.tcOther), u(g_c.dsvHits), u(g_c.gbNoView), u(g_c.armForeign), u(g_c.armMeter), u(g_c.hookLost),
      u(g_c.gapApplies), u(g_c.uploads), u(g_c.upDeferred), u(g_c.created), u(g_c.createFail), u(g_c.tagFlips),
      u(g_c.obsOm), u(g_c.obsVp), u(g_c.fbBinds));
}
void Summary(const Keys& k, const std::vector<Block>& s, const std::vector<BlockOut>& out) {
  std::vector<bool> ok(out.size());
  std::vector<double> busy(out.size());
  for (size_t i = 0; i < out.size(); ++i) ok[i] = out[i].ok, busy[i] = out[i].busy;
  Effect eff[kArmCount];
  for (int a = 1; a < kArmCount; ++a) {
    if (!(k.arms >> a & 1)) continue;
    eff[a] = PairedEffect(s, busy, ok, a);
    std::string g;
    char buf[96];
    for (int q = 0; q < kGroups; ++q) {
      std::vector<double> m(out.size());
      double psOn = 0, psOff = 0;
      int nOn = 0, nOff = 0;
      for (size_t i = 0; i < out.size() && i < s.size(); ++i) {
        m[i] = out[i].ms[q];
        if (s[i].arm != a || !out[i].ok || !out[i].statFrames) continue;
        (s[i].on ? psOn : psOff) += out[i].ps[q];
        ++(s[i].on ? nOn : nOff);
      }
      const Effect e = PairedEffect(s, m, ok, a);
      const double on = nOn ? psOn / nOn : 0, off = nOff ? psOff / nOff : 0;
      if (e.n == 0 || (std::fabs(e.mean) < 0.002 && std::fabs(on - off) < 0.01)) continue;
      snprintf(buf, sizeof(buf), " %s %+.3f ms PS x%.2f;", kGroupNames[q], e.mean, off > 0 ? on / off : 0.0);
      g += buf;
    }
    const ArmDef d = ArmOf(a, k);
    Log("vrs probe arm A%d (%s; passes 0x%x, rate %s, views 0x%x%s): frame GPU busy %+.3f ms (SE %.3f, %d quartets);%s",
        a, ArmName(a), d.passes, RateName(k.rate), k.views,
        d.innerDeg > 0 ? (std::string(", ring ") + std::to_string(static_cast<int>(d.innerDeg)) + " deg").c_str() : "",
        eff[a].mean, eff[a].se, eff[a].n, g.c_str());
  }
  const Gate gt = Decide(eff[2], eff[3], eff[4], eff[6]);
  if (!gt.decided)
    Log("vrs probe gate: UNDECIDED (A2 and A4 must both be measured; A2 %d quartets, A4 %d)", eff[2].n, eff[4].n);
  else
    Log("vrs probe gate: %s (A2 saves %.3f ms + A4 %.3f ms = %.3f, kill below 0.5; A3 saves %.3f, A6 %.3f; offer at 0.3 "
        "for A3, A4 or A6 with the image check)",
        gt.close ? "CLOSE VRS" : (gt.offer ? "OFFER as a lossy opt-in (image check pending)" : "KEEP OPEN (below the offer bar)"),
        gt.saveA2, gt.saveA4, gt.saveA2 + gt.saveA4, gt.saveA3, gt.saveA6);
}
void ImagePhase(const Keys& k) {
  {
    std::lock_guard<std::mutex> lock(fixdepth::ckipd::g_dumpDirMutex);
    if (fixdepth::ckipd::g_dumpBase.empty()) {
      Log("  vrs probe image: skipped (the frame dump needs [Perception] FixationDepth=1 and CockpitTrueIpd=1)");
      return;
    }
  }
  Log("  vrs probe image: waiting %d s (pause the sim now: the dumps compare paused HoldPose frames)", k.imageWaitSec);
  if (!Wait(k.imageWaitSec * 1000)) return;
  for (int a = 1; a < kArmCount; ++a) {
    if (!(k.imageArms >> a & 1) || !(k.arms >> a & 1)) continue;
    for (int i = 0; i < 4; ++i) {
      const bool on = (i & 1) != 0;  // OFF ON OFF ON
      SetArm(on ? a : 0, k);
      if (!Wait(2500)) return;  // DLSS history settles
      Log("  vrs probe image: arm A%d %s #%d: dump of %d frames requested", a, on ? "ON" : "OFF", i / 2 + 1,
          k.imageFrames);
      fixdepth::ckipd::g_dumpReq = k.imageFrames;
      int t = 0;
      while (t < 10000 && fixdepth::ckipd::g_dumpReq.load() > 0 && Wait(50)) t += 50;
      while (t < 30000 && *static_cast<volatile bool*>(&fixdepth::ckipd::g_dumpActiveFlag) && Wait(50)) t += 50;
      if (t >= 30000 || fixdepth::ckipd::g_dumpReq.load() > 0) {
        Log("  vrs probe image: the dump did not start or end in time; image phase ends");
        fixdepth::ckipd::g_dumpReq = 0;
        SetArm(0, k);
        return;
      }
    }
  }
  SetArm(0, k);
  Log("  vrs probe image: done (pairs OFF/ON per arm in the cockpit_frames folders, in the order logged)");
}

// [Suite] VrsProbe > 0: the whole probe (suite thread).
void SuitePhase(std::atomic<uint64_t>& frames) {
  Keys k;
  {
    std::lock_guard<std::mutex> lock(g_keyMutex);
    k = g_keys;
  }
  if (k.blockSec <= 0) return;
  const std::vector<Block> sched = Schedule(k.arms, k.repeats);
  Log("-- MEASURE-ONLY VRS probe, LOSSY in its ON blocks (vrs_probe.h, R139 / R140): %zu blocks of %d s (arms 0x%x x %d "
      "quartets), rate %s, views 0x%x, ring %.1f deg, settle %d ms --",
      sched.size(), k.blockSec, k.arms, k.repeats, RateName(k.rate), k.views, k.ringDeg, k.settleMs);
  for (int t = 0; t < (k.startDelay + 60) * 10 && !g_steady.load() && !g_benchAbort.load(); ++t) Sleep(100);
  if (!g_steady.load()) {
    Log("  vrs probe: frames not steady for %d s; probe skipped", k.startDelay);
    return;
  }
  if (!Start()) return;
  Log("  vrs probe: baseline pass list (GPU pass timing with statistics, 5 s, probe off):");
  gpt::g_statsWanted = true;
  gpt::Measure(5000, frames);
  gpt::g_statsWanted = false;
  std::vector<BlockOut> out;
  bool aborted = false;
  for (size_t i = 0; i < sched.size(); ++i) {
    const Block& b = sched[i];
    SetArm(b.on ? b.arm : 0, k);
    if (!Wait(k.settleMs) || !g_steady.load()) {
      aborted = true;
      break;
    }
    out.push_back(MeasureBlock(k.blockSec * 1000, frames, k.views));
    LogBlock(static_cast<int>(i), static_cast<int>(sched.size()), b, out.back());
    if (b.on && (i + 1 == sched.size() || sched[i + 1].arm != b.arm || sched[i + 1].rep != b.rep || !sched[i + 1].on))
      LogSurfaces();
    if (g_fault.load()) {
      aborted = true;
      break;
    }
  }
  SetArm(0, k);
  if (aborted)
    Log("  vrs probe: ended early (%s)", g_fault.load() ? "a fault latched the probe off" :
                                         g_engineOff.load() ? "kill switch" : g_benchAbort.load() ? "suite aborted" :
                                         "frames stopped");
  LogCounters("after the timing");
  Summary(k, sched, out);
  if (!aborted && k.imageFrames > 0) ImagePhase(k);
  StopProbe();
  Log("  vrs probe: stopped");
}

// ---------------------------------------------------------------------------
// Config tick
// ---------------------------------------------------------------------------
// LoadConfig, every second: the keys; a changed note is logged once.
void ReadConfig(const wchar_t* ini) {
  std::string notes;
  const Keys k = ReadKeys(ini, &notes);
  static std::string lastNotes;
  if (!notes.empty() && notes != lastNotes) Log("config: [Suite] VrsProbe keys: %s", notes.c_str());
  lastNotes = notes;
  std::lock_guard<std::mutex> lock(g_keyMutex);
  g_keys = k;
  g_keyBlock = k.blockSec;
}
// The worker, every second while the key is set: the start gate (steady frames, R53).
void NoteFrames(uint64_t frames) {
  static uint64_t lastF = 0;
  static ULONGLONG lastT = 0;
  static int run = 0;
  const ULONGLONG now = GetTickCount64();
  if (lastT) run = SteadyStep(run, frames - lastF, now - lastT);
  lastF = frames, lastT = now;
  int need = 10;
  {
    std::lock_guard<std::mutex> lock(g_keyMutex);
    need = g_keys.startDelay;
  }
  g_steady = need <= 0 ? run > 0 || frames > 0 : run >= need;
}
// Payload stop.
void Shutdown() {
  if (g_ctx) StopProbe();
  if (g_gazeView) UnmapViewOfFile(g_gazeView), g_gazeView = nullptr;
  if (g_gazeMap) CloseHandle(g_gazeMap), g_gazeMap = nullptr;
}

}  // namespace vrsp
