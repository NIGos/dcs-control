// R51 VR realism effects ([VrFx], default off; docs\research\R51_gaze_dof_gvision.md). The pure parts: parameters and
// ini, physiology (pupil, defocus, G tolerance), the per-frame state machines (gaze-contingent focus with saccade
// release; G-induced vision loss), the per-view constant buffer, the HLSL, and a CPU reference of every GPU step.
// The D3D11 part rides the R42 colour pass (color_pass.h): the effects run inside the same tone-map pixel shader swap,
// one extra 8 x 8 compute dispatch per view for depth of field, no new hook. R57 cut GazeDof's GPU cost (tile
// early-out, one fovea-guard test per pixel, multiply-add taps, one-atomic tile maximum; docs\research\R57) with the
// same taps, weights and result.
//
//  - GazeDof (effect 5): defocus blur of content whose dioptric distance differs from the fixation distance. The
//    fixation distance is FixationDepth's published cone median (fixation_depth.h, DepthIn). Circle of confusion =
//    pupil (mm, Moon & Spencer from an estimated scene luminance) x |dioptre difference| beyond the eye's depth of
//    focus, in arcmin, converted per view to pixels from DCS's own projection: the same angular blur in all four
//    views (no focus / periphery seam, both eyes from one fixation distance). Released at once on saccade onset, on a
//    new fixation plane, on low confidence and on missing gaze; it returns only after a settle time, with a fade in.
//  - GVision (effect 7): G-induced vision loss shaped as in pilots: colour and light lost from the periphery first
//    (grey-out, tunnel), then the centre (blackout); red-out for negative G. Centred on the gaze (retinal), per eye.
//    The G load comes from the Lua hook (Export LoGetAccelerationUnits, through the loader); a first-order retinal
//    reserve model turns it into a vision-loss level.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "color_grade.h"

namespace vrfx {

// ---------------------------------------------------------------------------
// Parameters ([VrFx]; the ini is re-read every second, nothing needs a restart)
// ---------------------------------------------------------------------------
struct Params {
  // Effect 5
  int dof = 0;                 // GazeDof: 0 off, 1 on
  double dofStrength = 0.5;    // GazeDofStrength 0..1: share of the physiological circle of confusion
  double dofMaxArcmin = 6.0;   // GazeDofMaxArcmin 1..20: blur diameter cap
  int dofSaccadeRelease = 1;   // GazeDofSaccadeRelease 0/1: sharp at saccade onset until the eye settles
  double dofSettleMs = 120;    // GazeDofSettleMs 0..500: sharp time after a saccade / new fixation plane
  double dofFadeInMs = 250;    // GazeDofFadeInMs 50..1000: blur fade-in after the settle time
  double dofToleranceD = 0.3;  // GazeDofToleranceD 0.1..1: depth of focus (no blur within +-this many dioptres)
  double dofGuardDeg = 2.0;    // GazeDofFoveaGuardDeg 0..5: never blurred within this angle of the gaze
  double dofPupilMm = 0;       // GazeDofPupilMm 0 = estimated from the scene, else 2..8 mm fixed
  // Effect 7
  int gv = 0;                  // GVision: 0 off, 1 on
  double gvStrength = 1.0;     // GVisionStrength 0..1 (1: full blackout possible)
  int gvUseGaze = 1;           // GVisionUseGaze 0/1: tunnel centred on the gaze (1) or straight ahead (0)
  int gvRedout = 1;            // GVisionRedout 0/1: red-out under negative G
  double gvTolerance = 7.5;    // GVisionTolerance 4..10: sustained +G of full blackout (g-suit + straining ~7.5)
  int gvMode = 0;              // GVisionMode 0 auto (from DCS's G-EFFECT option), 1 complement DCS's, 2 full
};

inline Params Sanitize(Params p) {
  auto c = [](double v, double lo, double hi, double d) { return std::isfinite(v) ? std::clamp(v, lo, hi) : d; };
  p.dof = p.dof ? 1 : 0;
  p.dofStrength = c(p.dofStrength, 0, 1, 0.5);
  p.dofMaxArcmin = c(p.dofMaxArcmin, 1, 20, 6);
  p.dofSaccadeRelease = p.dofSaccadeRelease ? 1 : 0;
  p.dofSettleMs = c(p.dofSettleMs, 0, 500, 120);
  p.dofFadeInMs = c(p.dofFadeInMs, 50, 1000, 250);
  p.dofToleranceD = c(p.dofToleranceD, 0.1, 1, 0.3);
  p.dofGuardDeg = c(p.dofGuardDeg, 0, 5, 2);
  p.dofPupilMm = p.dofPupilMm <= 0 || !std::isfinite(p.dofPupilMm) ? 0 : std::clamp(p.dofPupilMm, 2.0, 8.0);
  p.gv = p.gv ? 1 : 0;
  p.gvStrength = c(p.gvStrength, 0, 1, 1);
  p.gvUseGaze = p.gvUseGaze ? 1 : 0;
  p.gvRedout = p.gvRedout ? 1 : 0;
  p.gvTolerance = c(p.gvTolerance, 4, 10, 7.5);
  p.gvMode = p.gvMode >= 0 && p.gvMode <= 2 ? p.gvMode : 0;
  return p;
}
inline bool Same(const Params& a, const Params& b) {  // field by field (the struct has padding)
  return a.dof == b.dof && a.dofStrength == b.dofStrength && a.dofMaxArcmin == b.dofMaxArcmin &&
         a.dofSaccadeRelease == b.dofSaccadeRelease && a.dofSettleMs == b.dofSettleMs && a.dofFadeInMs == b.dofFadeInMs &&
         a.dofToleranceD == b.dofToleranceD && a.dofGuardDeg == b.dofGuardDeg && a.dofPupilMm == b.dofPupilMm &&
         a.gv == b.gv && a.gvStrength == b.gvStrength && a.gvUseGaze == b.gvUseGaze && a.gvRedout == b.gvRedout &&
         a.gvTolerance == b.gvTolerance && a.gvMode == b.gvMode;
}

inline Params ReadIni(const wchar_t* ini) {
  Params p;
  auto i = [&](const wchar_t* k, int d) { return static_cast<int>(GetPrivateProfileIntW(L"VrFx", k, d, ini)); };
  auto f = [&](const wchar_t* k, double d) { return cgrade::IniD(ini, L"VrFx", k, d); };
  p.dof = i(L"GazeDof", 0);
  p.dofStrength = f(L"GazeDofStrength", p.dofStrength);
  p.dofMaxArcmin = f(L"GazeDofMaxArcmin", p.dofMaxArcmin);
  p.dofSaccadeRelease = i(L"GazeDofSaccadeRelease", 1);
  p.dofSettleMs = f(L"GazeDofSettleMs", p.dofSettleMs);
  p.dofFadeInMs = f(L"GazeDofFadeInMs", p.dofFadeInMs);
  p.dofToleranceD = f(L"GazeDofToleranceD", p.dofToleranceD);
  p.dofGuardDeg = f(L"GazeDofFoveaGuardDeg", p.dofGuardDeg);
  p.dofPupilMm = f(L"GazeDofPupilMm", 0);
  p.gv = i(L"GVision", 0);
  p.gvStrength = f(L"GVisionStrength", p.gvStrength);
  p.gvUseGaze = i(L"GVisionUseGaze", 1);
  p.gvRedout = i(L"GVisionRedout", 1);
  p.gvTolerance = f(L"GVisionTolerance", p.gvTolerance);
  p.gvMode = i(L"GVisionMode", 0);
  return Sanitize(p);
}

// DCS's G-EFFECT option (Saved Games\DCS\Config\options.lua, difficulty.geffect; read only): 0 none, 1 reduced,
// 2 realistic, -1 unknown. A mission can force its own difficulty; the log line says which value was used.
inline int ParseGEffect(const std::string& text) {
  const size_t k = text.find("[\"geffect\"]");
  if (k == std::string::npos) return -1;
  const size_t q1 = text.find('"', text.find('=', k) + 1);
  if (q1 == std::string::npos) return -1;
  const size_t q2 = text.find('"', q1 + 1);
  if (q2 == std::string::npos) return -1;
  const std::string v = text.substr(q1 + 1, q2 - q1 - 1);
  return v == "none" ? 0 : v == "reduced" ? 1 : v == "realistic" ? 2 : -1;
}
// The effective GVision mode: 1 complement (DCS draws its own G effect: we add only the gaze-centred tunnel shape and
// the colour loss, DCS darkens), 2 full (ours does everything; DCS's option is none).
inline int EffectiveGvMode(int keyMode, int dcsGEffect) {
  if (keyMode == 1 || keyMode == 2) return keyMode;
  return dcsGEffect == 0 ? 2 : 1;
}

// ---------------------------------------------------------------------------
// Physiology
// ---------------------------------------------------------------------------
constexpr double kPi = 3.14159265358979323846;
constexpr double kMmDioptreToArcmin = 3.4377467707849;  // 1 mm pupil x 1 D = 1 mrad of blur = 3.438 arcmin

// Pupil diameter (mm) for an adapting luminance in cd/m2 (Moon & Spencer 1944, as in Watson & Yellott 2012):
// 2.25 mm at 3000 cd/m2 (sunlit scene), 3.1 mm at 50, 6.5 mm at 0.03 (moonlit).
inline double PupilMm(double cdm2) {
  const double l = std::max(cdm2, 1e-6);
  return 4.9 - 3.0 * std::tanh(0.4 * std::log10(l));
}
// Daylight weight from the sun's height (cgrade::NightWeightOf's complement: 1 at y >= 0.05, 0 at y <= -0.10).
inline double DayWeight(bool sunValid, double sunY) { return sunValid ? 1.0 - cgrade::NightWeightOf(sunY) : 1.0; }
// The scene luminance the pupil adapts to, in cd/m2. DCS's HDR is not absolute (day / night differ only about 5x in
// its adapted value, R41), so the absolute level comes from the sun (log-blend 0.03 cd/m2 night .. 3000 day) and
// DCS's adapted luminance (t1[1].x, read back) only moves it around that level (cockpit interior vs sky, x0.1..x10,
// relative to 0.04, R41's day value). adapted <= 0: unknown, the sun level alone.
inline double SceneCdm2(bool sunValid, double sunY, double adapted) {
  const double w = DayWeight(sunValid, sunY);
  double l = std::exp(std::log(0.03) + (std::log(3000.0) - std::log(0.03)) * w);
  if (adapted > 0 && std::isfinite(adapted)) l *= std::clamp(adapted / 0.04, 0.1, 10.0);
  return l;
}
// Blur-circle diameter (arcmin) of content at dioptres d with the eye focused at f (thin-lens eye, small angles).
inline double CocArcmin(double d, double f, double pupilMm, double toleranceD, double amount, double maxArcmin) {
  const double dd = std::max(0.0, std::fabs(d - f) - toleranceD);
  return std::min(maxArcmin, amount * pupilMm * dd * kMmDioptreToArcmin);
}

// ---------------------------------------------------------------------------
// Gaze-contingent focus (render thread, once per frame; the GPU only applies its result)
// ---------------------------------------------------------------------------
struct DofIn {
  double dt = 0;           // s since the last frame
  bool gazeValid = false;  // GazeOut valid and fresh
  bool saccade = false;    // GazeOut bit1 (the Perception layer's saccade flag)
  double gazeX = 0, gazeY = 0;  // left-focus gaze tangents (for the velocity check)
  bool depthValid = false; // DepthIn valid and new enough
  double medianM = 0;      // DepthIn medianM (cone around the gaze; sky = 20 km)
  double conf = 0;         // share of the cone in the dominant distance band (0..1)
};
struct DofState {
  double focusD = 0;       // fixation distance in dioptres (0 = infinity)
  double gain = 0;         // 0 = no blur, 1 = full
  double holdS = 0;        // remaining settle time (s): the gain stays 0
  bool haveFocus = false;
  bool havePrevGaze = false;
  double prevGx = 0, prevGy = 0;
  // diagnostics (since the last log line)
  uint64_t saccades = 0, newPlanes = 0, lowConf = 0, noGaze = 0, frames = 0, framesBlur = 0;
};
constexpr double kSaccadeDegPerS = 120.0;  // velocity backup when the layer's flag is missing
constexpr double kConfMin = 0.5;
constexpr double kFadeOutS = 0.1;          // gain fade-out on low confidence / missing gaze

// The dominant-band share of DepthIn's 16-bin log histogram (two adjacent bins).
inline double HistConf(const uint16_t h[16]) {
  double tot = 0, best = 0;
  for (int i = 0; i < 16; ++i) tot += h[i];
  if (tot <= 0) return 0;
  for (int i = 0; i < 16; ++i) best = std::max(best, h[i] + (i + 1 < 16 ? h[i + 1] : 0.0));
  return best / tot;
}
inline double GazeSpeedDps(double x0, double y0, double x1, double y1, double dt) {
  if (dt <= 1e-4) return 0;
  const double c = (1 + x0 * x1 + y0 * y1) / std::sqrt((1 + x0 * x0 + y0 * y0) * (1 + x1 * x1 + y1 * y1));
  return std::acos(std::clamp(c, -1.0, 1.0)) * (180.0 / kPi) / dt;
}
// One frame. Rules (R51 section 3): the blur may lag the eye, but the fixated content is never blurred: every doubt
// sets the gain to 0 at once (saccade, new plane) or fades it out within 0.1 s (low confidence, no gaze); it comes
// back only after SettleMs, over FadeInMs.
inline void StepDof(DofState& s, const DofIn& in, const Params& p) {
  const double dt = std::clamp(in.dt, 0.0, 0.25);
  s.frames++;
  if (!in.gazeValid) {
    s.havePrevGaze = false;
    s.gain = std::max(0.0, s.gain - dt / kFadeOutS);
    s.noGaze++;
    return;
  }
  bool sac = in.saccade;
  if (s.havePrevGaze && GazeSpeedDps(s.prevGx, s.prevGy, in.gazeX, in.gazeY, dt) > kSaccadeDegPerS) sac = true;
  s.prevGx = in.gazeX, s.prevGy = in.gazeY, s.havePrevGaze = true;
  if (sac && p.dofSaccadeRelease) {
    if (s.gain > 0 || s.holdS <= 0) s.saccades++;
    s.gain = 0;
    s.holdS = p.dofSettleMs / 1000.0;
    return;
  }
  if (!in.depthValid) {
    s.gain = std::max(0.0, s.gain - dt / kFadeOutS);
    return;
  }
  const double target = 1.0 / std::max(in.medianM, 0.05);
  if (!s.haveFocus || std::fabs(target - s.focusD) > p.dofToleranceD) {
    if (s.haveFocus) s.newPlanes++;
    s.focusD = target;  // a new fixation plane: re-target and release at once
    s.haveFocus = true;
    s.gain = 0;
    s.holdS = p.dofSettleMs / 1000.0;
    return;
  }
  s.focusD += (target - s.focusD) * std::min(1.0, dt / 0.15);  // drift within the plane
  if (in.conf < kConfMin) {
    s.lowConf++;
    s.gain = std::max(0.0, s.gain - dt / kFadeOutS);
    s.holdS = std::max(s.holdS, p.dofSettleMs / 1000.0);
    return;
  }
  if (s.holdS > 0) {
    s.holdS -= dt;
    if (s.holdS > 0) return;
  }
  s.gain = std::min(1.0, s.gain + dt / (p.dofFadeInMs / 1000.0));
  if (s.gain > 0) s.framesBlur++;
}

// ---------------------------------------------------------------------------
// G-induced vision loss (render thread, once per frame)
// ---------------------------------------------------------------------------
// Loss level L: 0 none, ~0.3 peripheral light loss (grey-out), 1 blackout, > 1.15 G-LOC territory (DCS decides
// consciousness; we only stay black). Target from the sustained +Gz above the grey-out onset (tolerance - 1.2 G; the
// published PLL-to-blackout spread is 0.6-1.2 G), approached with the retinal oxygen reserve's ~3 s time constant
// (vision symptoms under gradual onset lag G by 3-5 s; recovery within seconds once G is off). Red-out: -Gz beyond
// 1.5 G (congestion), reached at about -3.5 G.
struct GState {
  double loss = 0, red = 0;
  double gAge = 1e9;       // s since the last G sample
};
constexpr double kGOnsetTau = 3.0, kGRecoverTau = 2.0, kRedTau = 1.5, kGStaleS = 1.0;
inline double GLossTarget(double gz, double tolerance) {
  const double onset = tolerance - 1.2;
  return std::clamp((gz - onset) / 1.2, 0.0, 1.3);
}
inline double RedTarget(double gz) { return std::clamp((-gz - 1.5) / 2.0, 0.0, 1.0); }
inline void StepG(GState& s, bool haveG, double gz, double dt, const Params& p) {
  // R58: a non-finite G or state (std::clamp passes NaN through) latched loss / red at NaN for the session: black views.
  if (!std::isfinite(gz)) haveG = false, gz = 1;
  if (!std::isfinite(s.loss)) s.loss = 0;
  if (!std::isfinite(s.red)) s.red = 0;
  if (!std::isfinite(dt)) dt = 0;
  dt = std::clamp(dt, 0.0, 0.25);
  s.gAge = haveG ? 0 : s.gAge + dt;
  const bool fresh = s.gAge < kGStaleS;
  const double tl = fresh ? GLossTarget(gz, p.gvTolerance) : 0.0;
  const double tr = fresh && p.gvRedout ? RedTarget(gz) : 0.0;
  const double tau = tl > s.loss ? kGOnsetTau : kGRecoverTau;
  s.loss += (tl - s.loss) * (1 - std::exp(-dt / tau));
  s.red += (tr - s.red) * (1 - std::exp(-dt / kRedTau));
  if (s.loss < 1e-4 && tl == 0) s.loss = 0;
  if (s.red < 1e-4 && tr == 0) s.red = 0;
}
inline double SmoothStep(double a, double b, double x) {
  const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
  return t * t * (3 - 2 * t);
}
// The visual field for a loss level (all angles from the gaze, degrees).
struct Shape {
  float tunnel = 90, edge = 4, outside = 1, global = 1;  // light: tunnel radius, edge half-width, brightness
  float desatC = 0, colorEdge = 90, desatP = 0, red = 0;  // colour: central and peripheral loss, red-out
};
inline bool Neutral(const Shape& s) {
  return s.outside >= 1 && s.global >= 1 && s.desatC <= 0 && s.desatP <= 0 && s.red <= 0;
}
inline Shape ShapeOf(double loss, double red, double strength, int mode) {
  Shape v;
  const double L = std::clamp(loss, 0.0, 1.3), s = std::clamp(strength, 0.0, 1.0);
  const double tunnel = 70.0 * (1 - SmoothStep(0.15, 1.0, L));
  v.tunnel = static_cast<float>(tunnel);
  v.edge = static_cast<float>(4.0 + 0.35 * tunnel);
  double outside = 1 - 0.92 * SmoothStep(0.10, 0.75, L);
  double global = 1 - SmoothStep(0.80, 1.05, L);
  if (mode == 1) {  // complement: DCS darkens the whole view; ours only shapes it (periphery first) and drains colour
    outside = std::max(outside, 0.35);
    global = 1;
  }
  v.outside = static_cast<float>(1 + (outside - 1) * s);
  v.global = static_cast<float>(1 + (global - 1) * s);
  v.desatC = static_cast<float>(s * SmoothStep(0.30, 0.85, L));
  v.colorEdge = static_cast<float>(std::max(0.0, 0.6 * tunnel - 2.0));
  v.desatP = static_cast<float>(s * SmoothStep(0.05, 0.35, L));
  const double R = std::clamp(red, 0.0, 1.0);
  if (mode == 2 && R > 0) {  // red-out (complement: DCS has its own red)
    v.red = static_cast<float>(s * 0.6 * SmoothStep(0.0, 1.0, R));
    v.global *= static_cast<float>(1 - 0.5 * s * SmoothStep(0.6, 1.0, R));
  }
  return v;
}

// ---------------------------------------------------------------------------
// The per-view constant buffer (HLSL cbQvFx, 8 float4) and the state layout
// ---------------------------------------------------------------------------
struct FxCb {
  float dofOn, gvOn, dofAmt, focusD;
  float tolD, pupilMm, maxArcmin, guardDeg;
  float gazeX, gazeY, viewW, viewH;
  float dvX, dvY, dvW, dvH;          // the view's depth viewport in its depth texture (texels)
  float halfW, halfH, tilesX, tilesY;
  float tunnel, edge, outside, global;
  float desatC, colorEdge, desatP, red;
  float projOff, depthOk, gvX, gvY;    // gvX/Y: the tunnel centre (gaze, or 0,0 straight ahead)
};
static_assert(sizeof(FxCb) == 128, "FxCb layout");
// In the colour pass's state buffer (color_pass.h): DCS's adapted luminance copied at view 0 (for the pupil), and per
// HMD view the 64 bytes of DCS's cPerView gProj (byte 256 of b7), copied on the GPU when the view's G-buffer run closes.
constexpr unsigned kStFxLum = 48;
// Our tone-map PS slots (R50 uses b12, t17-t21, s15; R42 b13, t16): fxCb at b11, prep / depth / tiles at t22-t24.
constexpr unsigned kPsFxCbSlot = 11, kPsFxSrvSlot = 22;
constexpr unsigned kStFxProj = 512;  // after R50's glare stamp / sizes (64..87) and gProj copies (256..511)
constexpr unsigned kStFxBytes = kStFxProj + 4 * 64;
constexpr unsigned kTile = 16;  // full-resolution pixels per tile (prep groups of 8 x 8 half-resolution texels)

// ---------------------------------------------------------------------------
// HLSL (shared by the prep compute shader and the tone-map pixel shader; FX_CB / FX_DEPTH / FX_STATE_DECL set the
// registers)
// ---------------------------------------------------------------------------
inline const char kCommonHlsl[] = R"(
cbuffer cbQvFx : register(FX_CB) {
  float fxDofOn, fxGvOn, fxDofAmt, fxFocusD;
  float fxTolD, fxPupilMm, fxMaxArcmin, fxGuardDeg;
  float fxGazeX, fxGazeY, fxViewW, fxViewH;
  float fxDvX, fxDvY, fxDvW, fxDvH;
  float fxHalfW, fxHalfH, fxTilesX, fxTilesY;
  float fxTunnel, fxEdge, fxOutside, fxGlobal;
  float fxDesatC, fxColorEdge, fxDesatP, fxRed;
  float fxProjOff, fxDepthOk, fxGvX, fxGvY;
};
Texture2D<float> fxDepth : register(FX_DEPTH);
struct FxProj { float p00, p11, ox, oy, p22, p23, p32, p33; };
// DCS's gProj as its shaders read it (default column-major packing: P[r][c] at float c * 4 + r).
FxProj FxLoadProj() {
  uint o = (uint)fxProjOff;
  uint4 a = FX_STATE.Load4(o);        // P00 P10 P20 P30
  uint2 b = FX_STATE.Load2(o + 20);   // P11 P21
  uint2 c = FX_STATE.Load2(o + 40);   // P22 P32
  uint2 d = FX_STATE.Load2(o + 56);   // P23 P33
  FxProj p;
  p.p00 = asfloat(a.x);
  p.p11 = asfloat(b.x);
  p.p22 = asfloat(c.x);
  p.p23 = asfloat(d.x);
  p.p32 = asfloat(c.y);
  p.p33 = asfloat(d.y);
  p.ox = asfloat(a.z) / p.p23;
  p.oy = asfloat(b.y) / p.p23;
  return p;
}
// View tangents (x right, y up) of a point of the view image (uv 0..1, y down).
float2 FxTan(FxProj p, float2 uv) {
  float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
  return float2((ndc.x - p.ox) / p.p00, (ndc.y - p.oy) / p.p11);
}
// Dioptres (1 / range in metres) of a depth value; 0 for the sky / far plane.
float FxDiopter(FxProj p, float d, float2 t) {
  float den = d * p.p23 - p.p22;
  if (abs(den) < 1e-30) return 0;
  float z = (p.p32 - d * p.p33) / den;
  float w = z * p.p23 + p.p33;
  if (!(w > 0)) return 0;
  return 1 / (w * sqrt(1 + dot(t, t)));
}
float FxDepthAt(float2 uv) {
  return fxDepth.Load(int3(int(fxDvX + uv.x * fxDvW), int(fxDvY + uv.y * fxDvH), 0));
}
float FxEccDeg(float2 t, float2 g) {
  float c = (1 + dot(t, g)) * rsqrt((1 + dot(t, t)) * (1 + dot(g, g)));
  return degrees(acos(clamp(c, -1, 1)));
}
// Blur-circle radius in this view's pixels for content at dioptres D seen along tangent t, before the fovea guard
// (the guard multiplies it by smoothstep(G, 2 G, eccentricity from the gaze), G = fxGuardDeg; CPU: CocRadiusPx).
float FxCocRadiusPxNoGuard(FxProj p, float D, float2 t) {
  float dd = max(0, abs(D - fxFocusD) - fxTolD);
  float am = min(fxMaxArcmin, fxDofAmt * fxPupilMm * dd * 3.4377468);
  return 0.5 * am * (0.5 * fxViewW * p.p00 * (1 + t.x * t.x) * 2.9088821e-4);
}
// R57: the fovea guard's factor shared by every direction within reachT (tangent units) of tangent t: 1 when that
// whole disc lies beyond 2 x the guard angle, 0 when it lies within the guard angle (the exact smoothstep values
// there), -1 when it touches the band in between (evaluate per direction). The angle between two view directions
// never exceeds the distance of their tangents (the radial projection onto the unit sphere does not lengthen the
// segment), so the disc's angular reach is at most degrees(reachT). The test compares cosines (no acos); 0.01 deg
// covers the rounding of both sides.
float FxGuardConst(float2 t, float reachT) {
  if (fxGuardDeg <= 0) return 1;
  float2 gz = float2(fxGazeX, fxGazeY);
  float c = (1 + dot(t, gz)) * rsqrt((1 + dot(t, t)) * (1 + dot(gz, gz)));
  float m = degrees(reachT) + 0.01;
  float hi = 2 * fxGuardDeg + m, lo = fxGuardDeg - m;
  if (hi < 180 && c < cos(radians(hi))) return 1;
  if (lo > 0 && c > cos(radians(lo))) return 0;
  return -1;
}
// The radius with a guard factor from FxGuardConst (-1: the exact per-direction smoothstep).
float FxCocRadiusPxG(FxProj p, float D, float2 t, float g) {
  float r = FxCocRadiusPxNoGuard(p, D, t);
  [branch] if (g < 0) r *= smoothstep(fxGuardDeg, 2 * fxGuardDeg, FxEccDeg(t, float2(fxGazeX, fxGazeY)));
  else r *= g;
  return r;
}
)";

// Prep (one per view, before the tone-map draw): half-resolution colour + nearest dioptres of each 2 x 2, and the
// largest blur radius of each 16 x 16 tile (a near object's blur spills over in-focus background).
inline const char kPrepHlsl[] = R"(
Texture2D<float4> hdr : register(t0);
RWTexture2D<float4> prepOut : register(u0);
RWTexture2D<float> tileOut : register(u1);
groupshared uint gsMax;
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
  if (gi == 0) gsMax = 0;
  FxProj p = FxLoadProj();
  uint W = (uint)fxViewW, H = (uint)fxViewH;
  float rmax = 0;
  if (id.x < (uint)fxHalfW && id.y < (uint)fxHalfH) {
    // R57: one guard test for the 2 x 2 (its pixels lie within 1 px of its centre)
    float g = FxGuardConst(FxTan(p, float2(id.xy * 2 + 1) / float2(W, H)), max(2 / (fxViewW * p.p00), 2 / (fxViewH * p.p11)));
    float3 c = 0;
    float dn = 0;
    [unroll] for (uint j = 0; j < 2; ++j)
    [unroll] for (uint i = 0; i < 2; ++i) {
      uint2 q = min(id.xy * 2 + uint2(i, j), uint2(W - 1, H - 1));
      float2 uv = (float2(q) + 0.5) / float2(W, H);
      // R77: a NaN / Inf / negative texel counts as black (as the glare chain's kernel 0 and the meter): min() alone
      // returned 65504 for a NaN (IEEE minNum), which the gather spread as a white disc over the defocused pixels.
      float3 h = hdr.Load(int3(q, 0)).rgb;
      h = (h.r >= 0 && h.g >= 0 && h.b >= 0 && h.r < 1e30 && h.g < 1e30 && h.b < 1e30) ? min(h, 65504.0) : 0;
      c += h;
      float2 t = FxTan(p, uv);
      float D = FxDiopter(p, FxDepthAt(uv), t);
      dn = max(dn, D);
      rmax = max(rmax, FxCocRadiusPxG(p, D, t, g));
    }
    prepOut[id.xy] = float4(c * 0.25, dn);
  }
  // R57: the tile maximum with one atomic per thread (radii are >= 0: their bit patterns order as the values)
  GroupMemoryBarrierWithGroupSync();
  InterlockedMax(gsMax, asuint(rmax));
  GroupMemoryBarrierWithGroupSync();
  if (gi == 0) tileOut[gid.xy] = asfloat(gsMax);
}
)";

// The pixel-shader part (inserted into the colour pass's tone-map shader before main; qvState is its t16).
inline const char kPsHlsl[] = R"(
Texture2D<float4> fxPrep : register(t22);
Texture2D<float> fxTiles : register(t24);
// Depth of field: gather from the half-resolution prep on a 12-tap Vogel disc as wide as the largest blur nearby; a
// sample counts where its own blur circle covers this pixel, and a farther sample never covers a nearer pixel by
// more than this pixel's own blur (no background halo over in-focus foreground).
// R57 (cost; same taps, weights and result as R51 up to float rounding, see vrfx_test.h):
//  - the 3 x 3 tile maximum comes first; a pixel's own radius is part of its tile's maximum, so a pixel with no blur
//    within reach returns before reading its depth;
//  - the fovea guard is evaluated once per pixel for the whole disc (FxGuardConst); a disc inside the guard returns
//    at once (every weight 0), only a disc touching the guard's band evaluates it per tap (a rolled loop);
//  - a tap's position in half-resolution texels, its view tangent and its blur circle are multiply-adds on per-pixel
//    constants instead of FxTan's / the radius' divisions.
float FxTapWeight(float D, float tx, float rr, float Dc, float rc, float kAm, float sc, float gf) {
  float am = min(fxMaxArcmin, kAm * max(0, abs(D - fxFocusD) - fxTolD));
  float r = am * (sc * (1 + tx * tx)) * gf;
  if (D < Dc) r = min(r, rc);
  return r > 0 ? saturate(r - rr + 0.5) / max(r * r, 1) : 0;
}
float3 FxDof(uint2 px, float3 scene) {
  if (fxDofOn <= 0 || fxDepthOk <= 0) return scene;
  int2 tile = int2(px / 16);
  float rs = 0;
  [unroll] for (int ty = -1; ty <= 1; ++ty)
  [unroll] for (int tx = -1; tx <= 1; ++tx) {
    int2 k = clamp(tile + int2(tx, ty), int2(0, 0), int2(fxTilesX - 1, fxTilesY - 1));
    rs = max(rs, fxTiles.Load(int3(k, 0)));
  }
  [branch] if (rs < 0.5) return scene;
  FxProj p = FxLoadProj();
  float2 vs = float2(fxViewW, fxViewH);
  float2 uv = (float2(px) + 0.5) / vs;
  float2 t = FxTan(p, uv);
  float Dc = FxDiopter(p, FxDepthAt(uv), t);
  // the guard test on a disc at least as wide as the final one (this pixel's radius before its guard factor); then
  // this pixel's own radius with its guard factor (1 or the exact smoothstep), as R51's FxCocRadiusPx
  float rc = FxCocRadiusPxNoGuard(p, Dc, t);
  float g = FxGuardConst(t, min(max(rs, rc), 16.0) * max(2 / (fxViewW * p.p00), 2 / (fxViewH * p.p11)));
  [branch] if (g == 0) return scene;  // the whole disc within the guard: every weight is 0
  [branch] if (g < 0) rc *= smoothstep(fxGuardDeg, 2 * fxGuardDeg, FxEccDeg(t, float2(fxGazeX, fxGazeY)));
  rs = min(max(rs, rc), 16.0);
  float wc = 1 / max(rc * rc, 1);
  float3 acc = scene * wc;
  float ws = wc;
  // per-pixel constants: the pixel centre in half-resolution texels, the last texel, the view tangents per half texel
  // and at 0, the arcmin per dioptre and the radius (px) per arcmin at tx = 0
  float2 hc = (float2(px) + 0.5) * 0.5;
  float2 hmax = float2(fxHalfW - 1, fxHalfH - 1);
  float ka = 4 / (fxViewW * p.p00), kb = (-1 - p.ox) / p.p00, kd = 4 / (fxViewH * p.p11), kc = (1 - p.oy) / p.p11;
  float kAm = fxDofAmt * fxPupilMm * 3.4377468;
  float sc = 0.25 * fxViewW * p.p00 * 2.9088821e-4;
  [branch] if (g > 0) {
    [unroll] for (uint i = 0; i < 12; ++i) {
      float a = i * 2.39996323, f = sqrt((i + 0.5) / 12.0);
      float2 qh = hc + (0.5 * rs * f) * float2(cos(a), sin(a));
      float4 s = fxPrep.Load(int3(int2(clamp(qh, 0, hmax)), 0));
      float w = FxTapWeight(s.w, qh.x * ka + kb, rs * f, Dc, rc, kAm, sc, 1);
      acc += s.rgb * w;
      ws += w;
    }
  } else {
    [loop] for (uint i = 0; i < 12; ++i) {
      float a = i * 2.39996323, f = sqrt((i + 0.5) / 12.0);
      float2 qh = hc + (0.5 * rs * f) * float2(cos(a), sin(a));
      float4 s = fxPrep.Load(int3(int2(clamp(qh, 0, hmax)), 0));
      float2 tq = float2(qh.x * ka + kb, kc - qh.y * kd);  // FxTan at the tap
      float gf = smoothstep(fxGuardDeg, 2 * fxGuardDeg, FxEccDeg(tq, float2(fxGazeX, fxGazeY)));
      float w = FxTapWeight(s.w, qh.x * ka + kb, rs * f, Dc, rc, kAm, sc, gf);
      acc += s.rgb * w;
      ws += w;
    }
  }
  return acc / ws;
}
// G-induced vision loss on the exposed linear colour (before the tone curve): colour drained, then light, from the
// periphery of the gaze inwards; red-out tint.
float3 FxGVision(uint2 px, float3 lin) {
  if (fxGvOn <= 0) return lin;
  FxProj p = FxLoadProj();
  float2 t = FxTan(p, (float2(px) + 0.5) / float2(fxViewW, fxViewH));
  float e = FxEccDeg(t, float2(fxGvX, fxGvY));
  float dim = smoothstep(fxTunnel - fxEdge, fxTunnel + fxEdge, e);
  float bright = fxGlobal * lerp(1, fxOutside, dim);
  float de = saturate(fxDesatC + fxDesatP * smoothstep(fxColorEdge - fxEdge, fxColorEdge + fxEdge, e));
  float l = dot(lin, float3(0.2126, 0.7152, 0.0722));
  lin = lerp(lin, float3(l, l, l), de);
  lin = lerp(lin, l * float3(1.9, 0.35, 0.25), fxRed);
  return lin * bright;
}
)";

// The full sources: the prep CS, and the pixel-shader block for the colour pass's marker.
inline std::string PrepSource() {
  return std::string("#define FX_CB b0\n#define FX_DEPTH t1\n#define FX_STATE fxState\n"
                     "ByteAddressBuffer fxState : register(t2);\n") +
         kCommonHlsl + kPrepHlsl;
}
inline std::string PsBlock() {
  return std::string("#define FX_CB b11\n#define FX_DEPTH t23\n#define FX_STATE qvState\n") + kCommonHlsl + kPsHlsl;
}

// ---------------------------------------------------------------------------
// CPU reference (float, mirrors the HLSL; tests and previews)
// ---------------------------------------------------------------------------
struct CProj {
  float p00 = 1, p11 = 1, ox = 0, oy = 0, p22 = 0, p23 = 1, p32 = 0.02f, p33 = 0;
};
// From the 16 floats as stored (column-major: P[r][c] at c * 4 + r).
inline CProj ProjFromStored(const float m[16]) {
  CProj p;
  p.p00 = m[0], p.p11 = m[5], p.p22 = m[10], p.p23 = m[14], p.p32 = m[11], p.p33 = m[15];
  p.ox = m[2] / p.p23, p.oy = m[6] / p.p23;
  return p;
}
// A reverse-Z infinite projection as DCS's (near n; tangents l < r, d < u, signed), stored column-major. Row-vector
// convention: clip = [x y z 1] P, w = z (P23 = 1), ndc.x = P00 tx + P20, depth = n / z (P22 = 0, P32 = n).
inline void StoreReverseInfinite(float l, float r, float u, float d, float n, float out[16]) {
  for (int i = 0; i < 16; ++i) out[i] = 0;
  auto set = [&](int row, int col, float v) { out[col * 4 + row] = v; };
  set(0, 0, 2 / (r - l)), set(1, 1, 2 / (u - d));
  set(2, 0, -(r + l) / (r - l)), set(2, 1, -(u + d) / (u - d));
  set(2, 3, 1), set(3, 2, n);
}
struct F2 {
  float x, y;
};
inline F2 Tan(const CProj& p, float u, float v) {
  const float nx = u * 2 - 1, ny = 1 - v * 2;
  return {(nx - p.ox) / p.p00, (ny - p.oy) / p.p11};
}
inline float Diopter(const CProj& p, float d, F2 t) {
  const float den = d * p.p23 - p.p22;
  if (std::fabs(den) < 1e-30f) return 0;
  const float z = (p.p32 - d * p.p33) / den;
  const float w = z * p.p23 + p.p33;
  if (!(w > 0)) return 0;
  return 1 / (w * std::sqrt(1 + t.x * t.x + t.y * t.y));
}
inline float EccDeg(F2 t, F2 g) {
  const float c = (1 + t.x * g.x + t.y * g.y) / std::sqrt((1 + t.x * t.x + t.y * t.y) * (1 + g.x * g.x + g.y * g.y));
  return std::acos(std::clamp(c, -1.0f, 1.0f)) * (180.0f / 3.14159265f);
}
inline float SmoothF(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}
inline float CocRadiusPx(const FxCb& c, const CProj& p, float D, F2 t) {
  const float dd = std::max(0.0f, std::fabs(D - c.focusD) - c.tolD);
  const float am = std::min(c.maxArcmin, c.dofAmt * c.pupilMm * dd * 3.4377468f);
  float r = 0.5f * am * (0.5f * c.viewW * p.p00 * (1 + t.x * t.x) * 2.9088821e-4f);
  if (c.guardDeg > 0) r *= SmoothF(c.guardDeg, 2 * c.guardDeg, EccDeg(t, {c.gazeX, c.gazeY}));
  return r;
}
// A view as the GPU sees it: the HDR image (W x H RGB), and its depth (here one value per image pixel: dv = image).
struct CView {
  unsigned w = 0, h = 0;
  std::vector<float> rgb;    // W * H * 3
  std::vector<float> depth;  // W * H device depth (dvX/Y 0, dvW/H = W/H)
};
struct CPrep {
  unsigned hw = 0, hh = 0, tx = 0, ty = 0;
  std::vector<float> px;     // hw * hh * 4
  std::vector<float> tiles;  // tx * ty
};
inline float DepthAt(const CView& v, const FxCb& c, float u, float vv) {
  const int x = static_cast<int>(c.dvX + u * c.dvW), y = static_cast<int>(c.dvY + vv * c.dvH);
  return v.depth[std::min<unsigned>(y, v.h - 1) * v.w + std::min<unsigned>(x, v.w - 1)];
}
inline CPrep Prep(const CView& v, const FxCb& c, const CProj& p) {
  CPrep o;
  o.hw = static_cast<unsigned>(c.halfW), o.hh = static_cast<unsigned>(c.halfH);
  o.tx = static_cast<unsigned>(c.tilesX), o.ty = static_cast<unsigned>(c.tilesY);
  o.px.assign(o.hw * o.hh * 4, 0);
  o.tiles.assign(o.tx * o.ty, 0);
  for (unsigned y = 0; y < o.ty * 8; ++y)
    for (unsigned x = 0; x < o.tx * 8; ++x) {
      float rmax = 0;
      if (x < o.hw && y < o.hh) {
        float col[3] = {0, 0, 0}, dn = 0;
        for (unsigned j = 0; j < 2; ++j)
          for (unsigned i = 0; i < 2; ++i) {
            const unsigned qx = std::min(x * 2 + i, v.w - 1), qy = std::min(y * 2 + j, v.h - 1);
            const float u = (qx + 0.5f) / v.w, vv = (qy + 0.5f) / v.h;
            const float* h = &v.rgb[(qy * v.w + qx) * 3];
            const bool ok = h[0] >= 0 && h[1] >= 0 && h[2] >= 0 && h[0] < 1e30f && h[1] < 1e30f && h[2] < 1e30f;  // R77
            for (int k = 0; k < 3; ++k) col[k] += ok ? std::min(h[k], 65504.0f) : 0.0f;
            const F2 t = Tan(p, u, vv);
            const float D = Diopter(p, DepthAt(v, c, u, vv), t);
            dn = std::max(dn, D);
            rmax = std::max(rmax, CocRadiusPx(c, p, D, t));
          }
        float* d = &o.px[(y * o.hw + x) * 4];
        d[0] = col[0] * 0.25f, d[1] = col[1] * 0.25f, d[2] = col[2] * 0.25f, d[3] = dn;
      }
      float& tm = o.tiles[(y / 8) * o.tx + x / 8];
      tm = std::max(tm, rmax);
    }
  return o;
}
inline void Dof(const CView& v, const CPrep& pr, const FxCb& c, const CProj& p, unsigned x, unsigned y, float io[3]) {
  if (c.dofOn <= 0 || c.depthOk <= 0) return;
  const float u = (x + 0.5f) / c.viewW, vv = (y + 0.5f) / c.viewH;
  const F2 t = Tan(p, u, vv);
  const float Dc = Diopter(p, DepthAt(v, c, u, vv), t);
  const float rc = CocRadiusPx(c, p, Dc, t);
  const int tx0 = static_cast<int>(x / 16), ty0 = static_cast<int>(y / 16);
  float rs = rc;
  for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
      const int kx = std::clamp(tx0 + dx, 0, static_cast<int>(c.tilesX) - 1);
      const int ky = std::clamp(ty0 + dy, 0, static_cast<int>(c.tilesY) - 1);
      rs = std::max(rs, pr.tiles[ky * pr.tx + kx]);
    }
  rs = std::min(rs, 16.0f);
  if (rs < 0.5f) return;
  const float wc = 1 / std::max(rc * rc, 1.0f);
  float acc[3] = {io[0] * wc, io[1] * wc, io[2] * wc}, ws = wc;
  for (unsigned i = 0; i < 12; ++i) {
    const float a = i * 2.39996323f;
    const float rr = rs * std::sqrt((i + 0.5f) / 12.0f);
    const float qx = x + 0.5f + rr * std::cos(a), qy = y + 0.5f + rr * std::sin(a);
    const int hx = std::clamp(static_cast<int>(std::floor(qx * 0.5f)), 0, static_cast<int>(c.halfW) - 1);
    const int hy = std::clamp(static_cast<int>(std::floor(qy * 0.5f)), 0, static_cast<int>(c.halfH) - 1);
    const float* s = &pr.px[(hy * pr.hw + hx) * 4];
    float r = CocRadiusPx(c, p, s[3], Tan(p, qx / c.viewW, qy / c.viewH));
    if (s[3] < Dc) r = std::min(r, rc);
    const float w = r > 0 ? std::clamp(r - rr + 0.5f, 0.0f, 1.0f) / std::max(r * r, 1.0f) : 0.0f;
    for (int k = 0; k < 3; ++k) acc[k] += s[k] * w;
    ws += w;
  }
  for (int k = 0; k < 3; ++k) io[k] = acc[k] / ws;
}
inline void GVision(const FxCb& c, const CProj& p, unsigned x, unsigned y, float lin[3]) {
  if (c.gvOn <= 0) return;
  const F2 t = Tan(p, (x + 0.5f) / c.viewW, (y + 0.5f) / c.viewH);
  const float e = EccDeg(t, {c.gvX, c.gvY});
  const float dim = SmoothF(c.tunnel - c.edge, c.tunnel + c.edge, e);
  const float bright = c.global * (1 + (c.outside - 1) * dim);
  const float de = std::clamp(c.desatC + c.desatP * SmoothF(c.colorEdge - c.edge, c.colorEdge + c.edge, e), 0.0f, 1.0f);
  const float l = lin[0] * 0.2126f + lin[1] * 0.7152f + lin[2] * 0.0722f;
  const float red[3] = {1.9f, 0.35f, 0.25f};
  for (int k = 0; k < 3; ++k) {
    float v = lin[k] + (l - lin[k]) * de;
    v = v + (l * red[k] - v) * c.red;
    lin[k] = v * bright;
  }
}

// The stock-emulation colour constants (R42: DCS's own tone map exactly) for effects without a colour mode.
inline cgrade::ColorCb StockCb(uint32_t frame) {
  cgrade::ColorCb c{};
  c.whiteStart = 0.6f, c.keepSat = 0.3f, c.saturation = 1, c.desatAmt = 1;
  c.warmR = c.warmG = c.warmB = 1;
  c.frame = static_cast<float>(frame % 4096u);
  return c;
}

// ---------------------------------------------------------------------------
// Shared state (config thread writes the parameters; Lua / loader writes G; render thread runs the steps)
// ---------------------------------------------------------------------------
inline std::atomic<bool> g_wantDof{false}, g_wantGv{false};
inline std::atomic<uint32_t> g_paramVer{0};
inline Params g_params;            // under g_paramLock
inline std::atomic_flag g_paramLock = ATOMIC_FLAG_INIT;
inline std::atomic<int> g_dcsGEffect{-1};
inline std::atomic<float> g_gx{0}, g_gy{1}, g_gz{0};
inline std::atomic<uint64_t> g_gSeq{0};
inline std::atomic<double> g_gTime{0};
// R58: the Lua hook's camera is away from the ownship (F2 / F6 / F10, an external replay camera): the G state still
// steps (it is right when the view returns), the pass draws no field. The hook passes a negative model time for it.
inline std::atomic<bool> g_gExternal{false};
// R58: bumped at a mission load / simulation stop / local slot change: the render thread drops the effects' state
// (a blackout or a focus of the previous aircraft or mission).
inline std::atomic<uint32_t> g_fxResetGen{0};
inline void NoteMissionEvent() { g_fxResetGen.fetch_add(1, std::memory_order_acq_rel); }
inline std::atomic<float> g_adapted{0};  // DCS's adapted luminance (readback)
inline bool Wanted() { return g_wantDof.load(std::memory_order_relaxed) || g_wantGv.load(std::memory_order_relaxed); }

// From the loader's Lua callback (any thread): body-axis acceleration in G (y = normal load, 1 in level flight).
// R58: modelTime < 0: the camera is not the cockpit's (the sample still counts). A non-finite gy is dropped (not new).
inline void NoteG(float gx, float gy, float gz, double modelTime) {
  g_gExternal.store(modelTime < 0, std::memory_order_relaxed);
  if (!std::isfinite(gy)) return;
  g_gx = gx, g_gy = gy, g_gz = gz, g_gTime = modelTime;
  g_gSeq++;
}

}  // namespace vrfx
