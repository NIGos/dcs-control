// [Color] pure parts: the sRGB transfer functions, the R41 live probes (copy classification, half floats, HDR
// downsample, luminance statistics) and the R42 colour modes' parameters ([ColorFix] / [ColorEnhanced], IniD /
// ReadModeIni). The R40 opt-in grading ([Color] SrgbMatch / BlackFloor / Contrast / Saturation / OutputGamma, never
// wired, superseded by the R42 modes) was removed in R107 (research_archive\R40_color_grading).
#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ini_read.h"  // R107: qvini::Double

namespace cgrade {

inline double SrgbEotf(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
inline double SrgbOetf(double l) { return l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055; }

// ---------------------------------------------------------------------------
// R41 live probes (dev, measure only; every key defaults to 0 = nothing armed).
//   [Color] ToneProbe=N      N frames: at each HMD view's ToneMappingPass first draw, log the PS SRVs
//                            (formats, small buffers / textures as floats: the adapted luminance), the
//                            PS constant buffer b0 (cbTonemapParams) and the target; a new value starts anew.
//   [Color] ToneProbeViews=M view mask (bit v = HMD view v), default 15.
//   [Color] ToneProbeHdr=F   1 = also write each view's HDR source (first probe frame) downsampled
//                            (periphery 1/4, focus 1/8, RGB float32) to <log folder>\tone_probe\.
//   [Color] SwapProbe=N      N frames: at each HMD view's CopyToHMDPass, the bound RTV / PS SRV0 formats and a
//                            64 x 64 byte compare of the source LDR image against the swapchain image.
//   [Suite] GeoOnlyProbe=1   R38 rank 2: the focus views' main GBufferPass draws into a 16 x 16 viewport off
//                            target (lossy while on: test only).
// Written by Configure (config thread), consumed on the render thread.
inline std::atomic<int> g_toneProbeReq{0}, g_toneProbeViews{15}, g_toneProbeHdr{0}, g_swapProbeReq{0};
inline std::atomic<int> g_geoOnlyProbe{0};

// One channel byte copy check: dst against src, as copied (equal), or as encoded once more by an sRGB view
// (dst = round(255 * sRGB_OETF(src / 255)) +- 1, src not a fixed point).
struct CopyClass {
  int n = 0, equal = 0, srgb = 0, other = 0;
};
inline uint8_t SrgbEncodeByte(uint8_t v) {
  return static_cast<uint8_t>(std::clamp(std::lround(SrgbOetf(v / 255.0) * 255.0), 0l, 255l));
}
// RGBA8 pixels (alpha ignored), `px` pixels each.
inline CopyClass ClassifyCopy(const uint8_t* src, const uint8_t* dst, size_t px) {
  CopyClass c;
  for (size_t i = 0; i < px; ++i)
    for (int ch = 0; ch < 3; ++ch) {
      const int s = src[i * 4 + ch], d = dst[i * 4 + ch];
      ++c.n;
      const int e = SrgbEncodeByte(static_cast<uint8_t>(s));
      if (d == s)
        ++c.equal;
      else if (std::abs(d - e) <= 1)
        ++c.srgb;
      else
        ++c.other;
    }
  return c;
}
// "A" = bytes copied unchanged (into an sRGB swapchain: decoded as sRGB, the near-black lift),
// "B" = encoded again by an sRGB RTV, "?" otherwise. Needs >= 90 % agreement over pixels that tell them apart.
inline char CopyVerdict(const CopyClass& c) {
  if (c.n == 0) return '?';
  if (c.equal * 10 >= c.n * 9) return 'A';
  if ((c.srgb + c.equal) * 10 >= c.n * 9 && c.srgb * 2 > c.n - c.equal - c.srgb) return 'B';
  return '?';
}

inline float HalfToFloat(uint16_t h) {
  const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1fu, m = h & 0x3ffu;
  float f;
  if (e == 0)
    f = std::ldexp(static_cast<float>(m), -24);
  else if (e == 31)
    f = m ? std::nanf("") : INFINITY;
  else
    f = std::ldexp(static_cast<float>(m | 0x400u), static_cast<int>(e) - 25);
  return s ? -f : f;
}

// Box downsample (factor f) of an RGBA16F image (rows of `pitch` bytes) into RGB float32, w/f x h/f.
// Non-finite or negative values count as 0 (they would poison the box).
inline void DownsampleHalf(const uint8_t* rows, size_t pitch, unsigned w, unsigned h, unsigned f,
                           std::vector<float>& out, unsigned* ow, unsigned* oh) {
  *ow = f ? w / f : 0;
  *oh = f ? h / f : 0;
  out.assign(static_cast<size_t>(*ow) * *oh * 3, 0.0f);
  if (!*ow || !*oh) return;
  const float inv = 1.0f / static_cast<float>(f * f);
  for (unsigned y = 0; y < *oh; ++y)
    for (unsigned x = 0; x < *ow; ++x) {
      float acc[3] = {0, 0, 0};
      for (unsigned j = 0; j < f; ++j) {
        const uint16_t* r = reinterpret_cast<const uint16_t*>(rows + (static_cast<size_t>(y) * f + j) * pitch);
        for (unsigned i = 0; i < f; ++i)
          for (int c = 0; c < 3; ++c) {
            const float v = HalfToFloat(r[(static_cast<size_t>(x) * f + i) * 4 + c]);
            if (std::isfinite(v) && v > 0) acc[c] += v;
          }
      }
      float* o = &out[(static_cast<size_t>(y) * *ow + x) * 3];
      for (int c = 0; c < 3; ++c) o[c] = acc[c] * inv;
    }
}

// Rec.709 luminance statistics of RGB float32 data: arithmetic mean, log-average (geometric, floor 1e-6),
// percentiles 1 / 50 / 99 and the max.
struct LumStats {
  double mean = 0, logAvg = 0, p1 = 0, p50 = 0, p99 = 0, max = 0;
  size_t n = 0;
};
inline LumStats LumStatsOf(const std::vector<float>& rgb) {
  LumStats s;
  std::vector<float> l;
  l.reserve(rgb.size() / 3);
  double sum = 0, lsum = 0;
  for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
    const double y = 0.2126 * rgb[i] + 0.7152 * rgb[i + 1] + 0.0722 * rgb[i + 2];
    l.push_back(static_cast<float>(y));
    sum += y;
    lsum += std::log(std::max(y, 1e-6));
  }
  s.n = l.size();
  if (!s.n) return s;
  s.mean = sum / s.n;
  s.logAvg = std::exp(lsum / s.n);
  std::sort(l.begin(), l.end());
  auto pct = [&](double p) { return static_cast<double>(l[std::min(s.n - 1, static_cast<size_t>(p * (s.n - 1) + 0.5))]); };
  s.p1 = pct(0.01);
  s.p50 = pct(0.5);
  s.p99 = pct(0.99);
  s.max = l.back();
  return s;
}

// ---------------------------------------------------------------------------
// R42 colour modes (docs\research\R42_color_modes.md). [Color] Mode = 0 off (default), 1 Fix, 2 Enhanced.
// The pass (color_pass.h) swaps the pixel shader of each HMD view's ToneMappingPass draw for ours, which reads
// DCS's own bindings (t0 HDR scene, t1 luminance buffer, t2 bloom, b0 cbTonemapParams, b8 AmbientMap) plus our
// constant buffer and adaptation state. One per-pixel function, one exposure state for all four views: seam-safe.
// This part is pure: the per-mode parameters, their ini sections, the constant-buffer packing and a CPU reference
// of the shader (tests, offline previews). Facts it relies on (R41 [M], R42 [V]):
//  - DCS encodes pow(tm, 1/2.2); the bytes reach an sRGB swapchain unchanged and are decoded with the piecewise
//    sRGB EOTF (QVFR's sRGB SRVs, the runtime): SrgbMatch encodes with the piecewise sRGB OETF instead.
//  - View 0 (left periphery) is tone mapped with an exposure correction 0.08 EV below views 1-3 (b0 +0x0c, all
//    R41 scenes; the R41 LDR means match only with that split): EqualizeEc uses view 1's value for all four.
//  - whiteBalanceFactor is 0 live (b0 +0x18); the (1, 0.85, 0.6, 1) rows at +0xa0 are bloomTint/Intensity.
// ---------------------------------------------------------------------------

struct ModeParams {
  int srgbMatch = 1;         // 1: piecewise sRGB OETF (what the decoder inverts); 0: DCS's pow(1/2.2)
  int equalizeEc = 1;        // 1: one exposure correction (view 1's) for all four views
  double nightFloor = 0.0;   // adapted-luminance floor at night (0 = DCS's 0.01); < 0.01 brightens, > darkens
  int nightGate = 1;         // 1: floor scaled by the night weight from the sun (0 at y >= 0.05, 1 at y <= -0.10)
  double exposureEv = 0.0;   // added EV
  double nightEv = 0.0;      // added EV at night (scaled by the night weight; gated like the floor)
  double slope = 0, toe = 0, shoulder = 0, whiteClip = 0;  // own filmic curve; all 0 = DCS's (b0)
  double hueKeep = 0.0;      // 0 = DCS's per-channel curve, 1 = hue-preserving (blend between), mid-tones and up
  double hueShadow = 0.0;    // the same in the shadows (the per-channel toe keeps dark colours rich)
  double chromaNorm = 0.0;   // hue-preserving norm: 0 = luminance, 1 = max RGB
  double whiteStart = 0.6;   // tone-mapped level where low-chroma highlights start to whiten
  double gamutKeepSat = 0.3; // out of gamut: 0 = desaturate (luminance kept), 1 = darken (saturation kept)
  double saturation = 1.0;   // scene-linear, luminance kept
  double warmth = 0.0;       // golden-hour / dusk warm shift (sun-gated), 0..1
  double darkDesat = 1.0;    // DCS's dark-pixel desaturation at night (1 = DCS's)
  double darkDesatDay = 1.0; // the same in daylight
  int dither = 2;            // 0 = DCS's ordered dither only, 1 = fine (+-0.5 code, temporal) only, 2 = both
  double adaptBright = 0.0;  // eye-like adaptation on top of DCS's: time constant toward brighter, s (0 = off)
  double adaptDark = 0.0;    // ... toward darker, s
  double meterFocus = 0.0;   // weight of the focus view's log-average against the periphery's (0 = DCS's meter)
  // R50 (Enhanced only; [ColorFix] never reads these): physiological night vision (mesopic / scotopic, Purkinje).
  int nightVision = 0;       // 1: rod / cone blend by the adaptation level (sun) and each pixel's exposed luminance
  double nvStrength = 0.8;   // 0..1 share of the rod image at full night
  double nvBlue = 0.4;       // 0..1 blue shift of the rod image (1 = CIE xy 0.25 / 0.25, Thompson et al. 2002)
  double nvBlur = 0.25;      // 0..1 loss of fine detail in the rod image (blend toward a ~0.5 deg low-pass)
  double nvScotopic = 0.005; // cd/m2: adaptation luminance at and below which vision is fully scotopic (CIE 191)
  double nvPhotopic = 5.0;   // cd/m2: at and above which it is fully photopic
  // R50 realistic glare (Enhanced only): veiling halo + optional streak from genuinely bright sources.
  int glare = 0;             // 1: on
  double glareStrength = 0.35;    // 0..1 share of the above-threshold light scattered into the glare
  double glareThresholdEv = 3.5;  // 0..8 EV above the adapted mid-grey where a source starts to glare
  double glareRadiusDeg = 5.0;    // 1..20: radius (deg) of the wide halo (the PSF's widest Gaussian, sigma)
  double glareStreak = 0.3;       // 0..1 share of the glare drawn as a 6-spike radial streak (0 = halo only)
  int glareDebug = 0;             // dev only (not a GUI key): 1 = the image is the glare alone (live seam checks)
  // R111 ([Color] PanelGamut, read for both modes): compensation of a wide-gamut panel shown sRGB content untagged.
  // 0 = off; 1 = linear sRGB -> DCI-P3 (D65) matrix on the display-linear tone, before the encode (the panel taken
  // as P3); 2 = half strength (a panel part-way to P3). See PanelGamutStrength / kSrgbToP3.
  int panelGamut = 0;
};

inline ModeParams FixDefaults() { return ModeParams{}; }
inline ModeParams EnhancedDefaults() {
  ModeParams p;
  p.slope = 0.90, p.toe = 0.56, p.shoulder = 0.36, p.whiteClip = 0.06;
  p.hueKeep = 0.8, p.hueShadow = 0.3, p.chromaNorm = 0.0, p.whiteStart = 0.6, p.gamutKeepSat = 0.3;
  p.saturation = 1.15, p.warmth = 0.35, p.darkDesat = 1.0, p.darkDesatDay = 0.3, p.dither = 1;
  p.adaptBright = 0.4, p.adaptDark = 1.5, p.meterFocus = 0.3;
  p.nightVision = 1;  // R50: on in Enhanced (0 by day: sun-gated; replaces DCS's dark desaturation at night)
  p.glare = 1;        // R50: on in Enhanced since R55 (live: A0 projection source and A3 cost pass, docs/research/R55_effects_live.md)
  return p;
}
inline ModeParams ModeDefaults(int mode) { return mode == 2 ? EnhancedDefaults() : FixDefaults(); }

inline double ClampD(double v, double lo, double hi, double dflt) { return std::isfinite(v) ? std::clamp(v, lo, hi) : dflt; }
inline ModeParams SanitizeMode(ModeParams p) {
  p.srgbMatch = p.srgbMatch ? 1 : 0;
  p.equalizeEc = p.equalizeEc ? 1 : 0;
  p.nightFloor = p.nightFloor <= 0 || !std::isfinite(p.nightFloor) ? 0.0 : std::clamp(p.nightFloor, 0.004, 0.05);
  p.nightGate = p.nightGate ? 1 : 0;
  p.exposureEv = ClampD(p.exposureEv, -2.0, 2.0, 0.0);
  p.nightEv = ClampD(p.nightEv, -2.0, 2.0, 0.0);
  const bool own = p.slope > 0 || p.toe > 0 || p.shoulder > 0 || p.whiteClip > 0;
  if (own) {  // any curve key set: all four (unset ones take DCS's stock values)
    p.slope = p.slope > 0 ? ClampD(p.slope, 0.5, 1.5, 0.865) : 0.865;
    p.toe = p.toe > 0 ? ClampD(p.toe, 0.3, 0.95, 0.617) : 0.617;
    p.shoulder = p.shoulder > 0 ? ClampD(p.shoulder, 0.05, 0.9, 0.322) : 0.322;
    p.whiteClip = p.whiteClip > 0 ? ClampD(p.whiteClip, 0.0, 0.3, 0.04) : 0.04;
  } else {
    p.slope = p.toe = p.shoulder = p.whiteClip = 0;
  }
  p.hueKeep = ClampD(p.hueKeep, 0, 1, 0);
  p.hueShadow = ClampD(p.hueShadow, 0, 1, 0);
  p.chromaNorm = ClampD(p.chromaNorm, 0, 1, 0);
  p.whiteStart = ClampD(p.whiteStart, 0.3, 0.99, 0.6);
  p.gamutKeepSat = ClampD(p.gamutKeepSat, 0, 1, 0.3);
  p.saturation = ClampD(p.saturation, 0.5, 1.5, 1.0);
  p.warmth = ClampD(p.warmth, 0, 1, 0);
  p.darkDesat = ClampD(p.darkDesat, 0, 1, 1);
  p.darkDesatDay = ClampD(p.darkDesatDay, 0, 1, 1);
  p.dither = p.dither < 0 || p.dither > 2 ? 2 : p.dither;
  p.adaptBright = ClampD(p.adaptBright, 0, 10, 0);
  p.adaptDark = ClampD(p.adaptDark, 0, 30, 0);
  p.meterFocus = ClampD(p.meterFocus, 0, 1, 0);
  p.nightVision = p.nightVision ? 1 : 0;
  p.nvStrength = ClampD(p.nvStrength, 0, 1, 0.8);
  p.nvBlue = ClampD(p.nvBlue, 0, 1, 0.4);
  p.nvBlur = ClampD(p.nvBlur, 0, 1, 0.25);
  p.nvScotopic = ClampD(p.nvScotopic, 0.0005, 0.5, 0.005);
  p.nvPhotopic = ClampD(p.nvPhotopic, 0.05, 100, 5.0);
  if (p.nvPhotopic < p.nvScotopic * 4) p.nvPhotopic = p.nvScotopic * 4;  // at least 0.6 decades of transition
  p.glare = p.glare ? 1 : 0;
  p.glareStrength = ClampD(p.glareStrength, 0, 1, 0.35);
  p.glareThresholdEv = ClampD(p.glareThresholdEv, 0, 8, 3.5);
  p.glareRadiusDeg = ClampD(p.glareRadiusDeg, 1, 20, 5.0);
  p.glareStreak = ClampD(p.glareStreak, 0, 1, 0.3);
  p.glareDebug = p.glareDebug == 1 ? 1 : 0;
  p.panelGamut = p.panelGamut == 1 || p.panelGamut == 2 ? p.panelGamut : 0;  // R111
  return p;
}
inline bool SameMode(const ModeParams& a, const ModeParams& b) {
  return a.srgbMatch == b.srgbMatch && a.equalizeEc == b.equalizeEc && a.nightFloor == b.nightFloor &&
         a.nightGate == b.nightGate && a.exposureEv == b.exposureEv && a.nightEv == b.nightEv && a.slope == b.slope && a.toe == b.toe &&
         a.shoulder == b.shoulder && a.whiteClip == b.whiteClip && a.hueKeep == b.hueKeep && a.hueShadow == b.hueShadow && a.chromaNorm == b.chromaNorm &&
         a.whiteStart == b.whiteStart && a.gamutKeepSat == b.gamutKeepSat && a.saturation == b.saturation &&
         a.warmth == b.warmth && a.darkDesat == b.darkDesat && a.darkDesatDay == b.darkDesatDay && a.dither == b.dither &&
         a.adaptBright == b.adaptBright && a.adaptDark == b.adaptDark && a.meterFocus == b.meterFocus &&
         a.nightVision == b.nightVision && a.nvStrength == b.nvStrength && a.nvBlue == b.nvBlue && a.nvBlur == b.nvBlur &&
         a.nvScotopic == b.nvScotopic && a.nvPhotopic == b.nvPhotopic && a.glare == b.glare &&
         a.glareStrength == b.glareStrength && a.glareThresholdEv == b.glareThresholdEv &&
         a.glareRadiusDeg == b.glareRadiusDeg && a.glareStreak == b.glareStreak && a.glareDebug == b.glareDebug &&
         a.panelGamut == b.panelGamut;
}
inline bool OwnCurve(const ModeParams& p) { return p.slope > 0; }
inline bool UsesState(const ModeParams& p) { return p.adaptBright > 0 || p.adaptDark > 0 || p.meterFocus > 0; }

// One mode's section ([ColorFix] / [ColorEnhanced]); a missing key keeps the mode's default.
// R107: the shared reader (ini_read.h, the same rule as main.cpp's ReadDouble).
inline double IniD(const wchar_t* ini, const wchar_t* sec, const wchar_t* key, double dflt) {
  return qvini::Double(sec, key, dflt, ini);
}
// R115 G4: an int key (IniD, then qvini::ToInt: nan / inf / out of int range -> the default, no undefined cast).
inline int IniI(const wchar_t* ini, const wchar_t* sec, const wchar_t* key, int dflt) {
  return qvini::ToInt(IniD(ini, sec, key, dflt), dflt);
}
inline ModeParams ReadModeIni(const wchar_t* ini, int mode) {
  ModeParams p = ModeDefaults(mode);
  const wchar_t* s = mode == 2 ? L"ColorEnhanced" : L"ColorFix";
  p.srgbMatch = IniI(ini, s, L"SrgbMatch", p.srgbMatch);
  p.equalizeEc = IniI(ini, s, L"EqualizeEc", p.equalizeEc);
  p.nightFloor = IniD(ini, s, L"NightFloor", p.nightFloor);
  p.nightGate = IniI(ini, s, L"NightGate", p.nightGate);
  p.exposureEv = IniD(ini, s, L"ExposureEV", p.exposureEv);
  p.nightEv = IniD(ini, s, L"NightEV", p.nightEv);
  p.slope = IniD(ini, s, L"Slope", p.slope);
  p.toe = IniD(ini, s, L"Toe", p.toe);
  p.shoulder = IniD(ini, s, L"Shoulder", p.shoulder);
  p.whiteClip = IniD(ini, s, L"WhiteClip", p.whiteClip);
  p.hueKeep = IniD(ini, s, L"HueKeep", p.hueKeep);
  p.hueShadow = IniD(ini, s, L"HueShadow", p.hueShadow);
  p.chromaNorm = IniD(ini, s, L"ChromaNorm", p.chromaNorm);
  p.whiteStart = IniD(ini, s, L"WhiteStart", p.whiteStart);
  p.gamutKeepSat = IniD(ini, s, L"GamutKeepSat", p.gamutKeepSat);
  p.saturation = IniD(ini, s, L"Saturation", p.saturation);
  p.warmth = IniD(ini, s, L"Warmth", p.warmth);
  p.darkDesat = IniD(ini, s, L"DarkDesat", p.darkDesat);
  p.darkDesatDay = IniD(ini, s, L"DarkDesatDay", p.darkDesatDay);
  p.dither = IniI(ini, s, L"Dither", p.dither);
  p.adaptBright = IniD(ini, s, L"AdaptBright", p.adaptBright);
  p.adaptDark = IniD(ini, s, L"AdaptDark", p.adaptDark);
  p.meterFocus = IniD(ini, s, L"MeterFocus", p.meterFocus);
  if (mode == 2) {  // R50: night vision and glare are Enhanced features
    p.nightVision = IniI(ini, s, L"NightVision", p.nightVision);
    p.nvStrength = IniD(ini, s, L"NightVisionStrength", p.nvStrength);
    p.nvBlue = IniD(ini, s, L"NightVisionBlue", p.nvBlue);
    p.nvBlur = IniD(ini, s, L"NightVisionBlur", p.nvBlur);
    p.nvScotopic = IniD(ini, s, L"NightVisionScotopic", p.nvScotopic);
    p.nvPhotopic = IniD(ini, s, L"NightVisionPhotopic", p.nvPhotopic);
    p.glare = IniI(ini, s, L"Glare", p.glare);
    p.glareStrength = IniD(ini, s, L"GlareStrength", p.glareStrength);
    p.glareThresholdEv = IniD(ini, s, L"GlareThresholdEV", p.glareThresholdEv);
    p.glareRadiusDeg = IniD(ini, s, L"GlareRadius", p.glareRadiusDeg);
    p.glareStreak = IniD(ini, s, L"GlareStreak", p.glareStreak);
    p.glareDebug = IniI(ini, s, L"GlareDebug", p.glareDebug);
  }
  // R111: one key for both modes ([Color], next to Mode): it describes the headset's panel, not a look.
  p.panelGamut = IniI(ini, L"Color", L"PanelGamut", p.panelGamut);
  return SanitizeMode(p);
}

// Sun weights from the sun direction's y (GraphicsCore context, night_tone.h). Night: 0 at y >= 0.05, 1 at
// y <= -0.10 (night_tone.h's gate). Warm (golden hour / dusk): 1 for y in [0, 0.10], to 0 at y = 0.30 (17 deg) and
// at y = -0.10.
inline double NightWeightOf(double y) { return y >= 0.05 ? 0.0 : y <= -0.10 ? 1.0 : (0.05 - y) / 0.15; }
inline double WarmWeightOf(double y) {
  if (y <= -0.10 || y >= 0.30) return 0.0;
  if (y < 0.0) return (y + 0.10) / 0.10;
  if (y <= 0.10) return 1.0;
  return (0.30 - y) / 0.20;
}

// The pass's constant buffer (HLSL cbQvColor, 7 float4).
struct ColorCb {
  float srgb, ownCurve, hueKeep, chromaNorm;
  float slope, toe, shoulder, whiteClip;
  float whiteStart, keepSat, saturation, desatAmt;
  float warmR, warmG, warmB, floorLum;
  float evOffset, ditherMode, frame, useState;
  float dt, tauBright, tauDark, meterFocus;
  float equalizeEc, reset, hueShadow, pad1;
  // R50 night vision and glare
  float nvW, nvTintR, nvTintG, nvTintB;           // rod share at full night x strength (0 = off); rod tint
  float nvBlur, nvConeLo, nvConeHi, glareOn;      // blur share; per-pixel cone range (EV vs mid-grey); glare 0/1
  float glareStrength, glareT, glareCap, glareStreak;  // exposed threshold / cap (linear); streak share
  float sig0, sig1, sig2, streakLen;              // PSF sigmas and streak length (tangent units)
  float w0, w1, w2, quad;                         // PSF weights; 1 = four HMD views (focus views exist)
  float debug, auxOn, pad2, panelGamut;           // debug 1 = output the glare only; auxOn = the 1/4 pass runs;
                                                  // R111 panelGamut: sRGB -> P3 strength (0 off, 0.5, 1)
};
static_assert(sizeof(ColorCb) == 208, "ColorCb layout");

// ---------------------------------------------------------------------------
// R50 night vision: the adaptation level. DCS's own adapted luminance cannot tell night from day (it sits at the
// 0.01 floor at night and only 1.5 decades below the day value, R41 F6), so the eye's state comes from the sun's
// elevation: horizontal illuminance (sun + sky, standard twilight tables, log10 lux) plus a fixed night sky / moon
// share (0.02 lux: DCS's moon phase is not available to us), seen on an average 20 % reflecting landscape
// (L = 0.2 E / pi). Each pixel's own level then comes from its exposed luminance relative to DCS's exposure state.
// ---------------------------------------------------------------------------
inline double SunIlluminanceLog10(double elevDeg) {
  static const double k[][2] = {{-90, -3.3}, {-18, -3.0}, {-12, -2.1}, {-9, -0.8}, {-6, 0.53}, {-3, 1.6}, {0, 2.6},
                                {3, 3.3},    {5, 3.6},    {10, 4.0},   {20, 4.4},  {40, 4.8},  {90, 5.0}};
  const int n = static_cast<int>(sizeof(k) / sizeof(k[0]));
  if (elevDeg <= k[0][0]) return k[0][1];
  for (int i = 1; i < n; ++i)
    if (elevDeg <= k[i][0]) return k[i - 1][1] + (k[i][1] - k[i - 1][1]) * (elevDeg - k[i - 1][0]) / (k[i][0] - k[i - 1][0]);
  return k[n - 1][1];
}
constexpr double kNightSkyLux = 0.02;
inline double AdaptationCdm2(double sunY) {
  const double e = std::asin(std::clamp(sunY, -1.0, 1.0)) * 57.29577951308232;
  return 0.2 / 3.14159265358979 * (std::pow(10.0, SunIlluminanceLog10(e)) + kNightSkyLux);
}
inline double SmoothStep(double a, double b, double x) {
  const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
  return t * t * (3 - 2 * t);
}
// Scotopic share of vision for the adaptation level (0 photopic .. 1 scotopic), log-luminance smoothstep.
inline double ScotopicShare(const ModeParams& p, double sunY) {
  return 1.0 - SmoothStep(std::log10(p.nvScotopic), std::log10(p.nvPhotopic), std::log10(AdaptationCdm2(sunY)));
}
// Rod tint: CIE xy (0.25, 0.25) in linear Rec.709 at luminance 1, blended with neutral by `blue`.
inline void RodTint(double blue, float out[3]) {
  const double t[3] = {0.70617, 0.98993, 1.96572};
  for (int c = 0; c < 3; ++c) out[c] = static_cast<float>(1.0 + (t[c] - 1.0) * blue);
}
// Glare PSF: three Gaussians (sigma = 0.12 / 0.35 / 1.0 x radius, weights 0.55 / 0.30 / 0.15): a core, a halo and
// a wide veil, a ~1/theta^2-like fall-off as Stiles-Holladay's veiling glare, in angular (tangent) units.
constexpr double kPsfF[3] = {0.12, 0.35, 1.0}, kPsfW[3] = {0.55, 0.30, 0.15};
constexpr double kGlareCapEv = 8.0;  // per-pixel exposed level cap (fireflies, the sun's core): 2^8 x mid-grey

constexpr double kRec709[3] = {0.2126, 0.7152, 0.0722};
// R111: linear sRGB / Rec.709 -> linear DCI-P3 (D65 white, Display P3 primaries): inv(NPM_P3) x NPM_709, from the
// primaries' xy (709: 0.64/0.33, 0.30/0.60, 0.15/0.06; P3: 0.680/0.320, 0.265/0.690, 0.150/0.060; D65 0.3127/0.3290).
// Every coefficient >= 0 and every row sums to 1: an RGB in [0,1] stays in [0,1] (each output channel is a weighted
// mean of the inputs), greys (r = g = b) are unchanged, and no gamut mapping is needed. The panel's own primaries are
// not published (R109: Sony ECX344A class, "up to 96 % DCI-P3" coverage), hence the half-strength option.
constexpr float kSrgbToP3[3][3] = {{0.8224621f, 0.1775380f, 0.0000000f},
                                   {0.0331942f, 0.9668058f, 0.0000000f},
                                   {0.0170826f, 0.0723974f, 0.9105199f}};
inline float PanelGamutStrength(int panelGamut) { return panelGamut == 1 ? 1.0f : panelGamut == 2 ? 0.5f : 0.0f; }
// tm (display-linear, 0..2 before the clamp of the encode) -> tm + s (M tm - tm). Applied after the tone curve and
// the hue-keep / gamut stage, before the 2.2 / sRGB encode and the dithers.
inline void ApplyPanelGamut(float tm[3], float s) {
  if (!(s > 0)) return;
  float o[3];
  for (int r = 0; r < 3; ++r) o[r] = kSrgbToP3[r][0] * tm[0] + kSrgbToP3[r][1] * tm[1] + kSrgbToP3[r][2] * tm[2];
  for (int i = 0; i < 3; ++i) tm[i] += (o[i] - tm[i]) * s;
}
// Luminance-neutral warm gain for w in 0..1 (+R, -B: about -800 K around 6500 K at w = 1).
inline void WarmGain(double w, float out[3]) {
  const double g[3] = {1 + 0.10 * w, 1.0, 1 - 0.16 * w};
  const double y = g[0] * kRec709[0] + g[1] * kRec709[1] + g[2] * kRec709[2];
  for (int c = 0; c < 3; ++c) out[c] = static_cast<float>(g[c] / y);
}
// sunValid false: night weight 0 (daylight values), warm weight 0.
inline ColorCb BuildCb(const ModeParams& p, bool sunValid, double sunY, uint32_t frame, double dt, bool reset,
                       bool quad = true) {
  ColorCb c{};
  const double nw = sunValid ? NightWeightOf(sunY) : 0.0, ww = sunValid ? WarmWeightOf(sunY) : 0.0;
  c.srgb = static_cast<float>(p.srgbMatch);
  c.ownCurve = OwnCurve(p) ? 1.0f : 0.0f;
  c.hueKeep = static_cast<float>(p.hueKeep);
  c.chromaNorm = static_cast<float>(p.chromaNorm);
  c.slope = static_cast<float>(p.slope), c.toe = static_cast<float>(p.toe);
  c.shoulder = static_cast<float>(p.shoulder), c.whiteClip = static_cast<float>(p.whiteClip);
  c.whiteStart = static_cast<float>(p.whiteStart), c.keepSat = static_cast<float>(p.gamutKeepSat);
  c.saturation = static_cast<float>(p.saturation);
  c.desatAmt = static_cast<float>(p.darkDesatDay + (p.darkDesat - p.darkDesatDay) * nw);
  float g[3];
  WarmGain(p.warmth * ww, g);
  c.warmR = g[0], c.warmG = g[1], c.warmB = g[2];
  c.floorLum = p.nightFloor > 0
                   ? static_cast<float>(std::exp(std::log(0.01) + (std::log(p.nightFloor) - std::log(0.01)) *
                                                                      (p.nightGate ? nw : 1.0)))
                   : 0.0f;
  c.evOffset = static_cast<float>(p.exposureEv + p.nightEv * (p.nightGate ? nw : (sunValid ? 1.0 : 0.0)));
  c.ditherMode = static_cast<float>(p.dither);
  c.frame = static_cast<float>(frame % 4096u);
  c.useState = UsesState(p) ? 1.0f : 0.0f;
  c.dt = static_cast<float>(std::clamp(dt, 0.0, 0.25));
  c.tauBright = static_cast<float>(p.adaptBright);
  c.tauDark = static_cast<float>(p.adaptDark);
  c.meterFocus = static_cast<float>(p.meterFocus);
  c.equalizeEc = static_cast<float>(p.equalizeEc);
  c.reset = reset ? 1.0f : 0.0f;
  c.hueShadow = static_cast<float>(p.hueShadow);
  // R50 night vision: rod share (sun-gated; none with the sun unknown). It extends DCS's dark desaturation (kept
  // as it is, so the darks are never more colourful than stock): the rod image is blended in after it.
  const double nv = p.nightVision && sunValid ? p.nvStrength * ScotopicShare(p, sunY) : 0.0;
  c.nvW = static_cast<float>(nv);
  float tint[3];
  RodTint(p.nvBlue, tint);
  c.nvTintR = tint[0], c.nvTintG = tint[1], c.nvTintB = tint[2];
  c.nvBlur = static_cast<float>(nv > 0 ? p.nvBlur : 0.0);
  c.nvConeLo = -1.0f, c.nvConeHi = 1.5f;
  // R50 glare
  c.glareOn = p.glare ? 1.0f : 0.0f;
  c.glareStrength = static_cast<float>(p.glareStrength);
  c.glareT = static_cast<float>(0.18 * std::exp2(p.glareThresholdEv));
  c.glareCap = static_cast<float>(0.18 * std::exp2(kGlareCapEv));
  c.glareStreak = static_cast<float>(p.glareStreak);
  const double rad = 3.14159265358979 / 180.0;
  c.sig0 = static_cast<float>(std::tan(kPsfF[0] * p.glareRadiusDeg * rad));
  c.sig1 = static_cast<float>(std::tan(kPsfF[1] * p.glareRadiusDeg * rad));
  c.sig2 = static_cast<float>(std::tan(kPsfF[2] * p.glareRadiusDeg * rad));
  c.streakLen = static_cast<float>(std::tan(std::min(2.0 * p.glareRadiusDeg, 40.0) * rad));
  c.w0 = static_cast<float>(kPsfW[0]), c.w1 = static_cast<float>(kPsfW[1]), c.w2 = static_cast<float>(kPsfW[2]);
  c.quad = quad ? 1.0f : 0.0f;
  c.auxOn = (p.glare || c.nvBlur > 0) ? 1.0f : 0.0f;
  c.debug = p.glare && p.glareDebug ? 1.0f : 0.0f;
  c.panelGamut = PanelGamutStrength(p.panelGamut);  // R111
  return c;
}
inline bool UsesAux(const ColorCb& c) { return c.auxOn > 0; }

// The fields of DCS's cbTonemapParams (tonemapCommon.hlsl) the shader reads, at their byte offsets.
struct DcsTone {
  float exposureKey = 1.3f, sigmaKey = 0.5f, tau = 4.0f, ec = 0.0f;          // +0x00
  float bloomThreshold = 0.4f, bloomLerp = 0.035f, whiteBalance = 0.0f, vignette = 0.0f;  // +0x10
  float lumMin = 0.01f, lumMax = 0.6f;                                        // +0x40
  float slope = 0.865f, toe = 0.617f, shoulder = 0.322f, blackClip = 0.0f;  // +0x70
  float whiteClip = 0.04f;                                                    // +0x80
  float outputGammaInv = 1.0f / 2.2f;                                         // +0x98
};
// A 256-byte cbTonemapParams image with these fields (tests, previews); the rest as R41 read it live.
inline void PackDcsTone(const DcsTone& t, float out[64]) {
  for (int i = 0; i < 64; ++i) out[i] = 0;
  out[0] = t.exposureKey, out[1] = t.sigmaKey, out[2] = t.tau, out[3] = t.ec;
  out[4] = t.bloomThreshold, out[5] = t.bloomLerp, out[6] = t.whiteBalance, out[7] = t.vignette;
  out[14] = 1.4f, out[15] = 1.1f;  // tmExp, tmPower
  out[16] = t.lumMin, out[17] = t.lumMax, out[18] = 1.0f, out[19] = 0.4f;
  out[20] = -6.644f, out[21] = 1.0f, out[22] = 7.644f;
  out[24] = 0.3f, out[25] = 0.85f, out[26] = 1.0f, out[27] = 0.25f;
  out[28] = t.slope, out[29] = t.toe, out[30] = t.shoulder, out[31] = t.blackClip;
  out[32] = t.whiteClip, out[33] = -2.5f, out[34] = 1.5f;
  out[36] = 0.1308f, out[37] = 0.8692f, out[38] = t.outputGammaInv;
  for (int r = 0; r < 6; ++r) out[40 + r * 4] = 1.0f, out[41 + r * 4] = 0.8499f, out[42 + r * 4] = 0.6f, out[43 + r * 4] = 1.0f;
}

// --- CPU reference of the pixel shader (float, as the GPU) ---
inline float FilmicCurveSeg(float c0, float c1, float ca, float s, float x) {
  const float t = 1 + c1 - c0;
  return 2 * t / (1 + std::exp((2 * s / t) * (x - ca))) - c1;
}
// DCS's TonemapFilmic (toneMap.hlsl) on log10 of the input.
inline float Filmic(float logL, float slope, float toe, float shoulder, float black, float white) {
  const float ta = (1 - toe - 0.18f) / slope - 0.733f;
  const float sa = (shoulder - 0.18f) / slope - 0.733f;
  if (logL < ta) return FilmicCurveSeg(toe, black, ta, -slope, logL);
  if (logL < sa) return slope * (logL + 0.733f) + 0.18f;
  return 1 - FilmicCurveSeg(shoulder, white, sa, slope, logL);
}
inline float SrgbOetfF(float l) {
  l = std::clamp(l, 0.0f, 1.0f);
  return l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}
inline float Bayer8(unsigned x, unsigned y) {  // dithering.hlsl: ditherArray[x % 8][y % 8]
  static const uint8_t k[8][8] = {{0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26},
                                  {12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
                                  {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25},
                                  {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21}};
  return (k[x % 8][y % 8] + 0.5f) / 64.0f;
}
// Interleaved gradient noise, frame-offset (the shader's fine dither), 0..1.
inline float Ign(unsigned x, unsigned y, float frame) {
  const float o = 5.588238f * std::fmod(frame, 64.0f);
  const float f = 0.06711056f * (x + o) + 0.00583715f * (y + o);
  const float a = 52.9829189f * (f - std::floor(f));
  return a - std::floor(a);
}

struct ShadeIn {
  float scene[3] = {0, 0, 0};
  float bloom[3] = {0, 0, 0};
  float adapted = 0.18f;     // the adapted luminance the pixel uses (state or DCS's t1[1].x)
  float ec = 0.0f;           // the exposure correction it uses (view 1's or its own b0)
  unsigned x = 0, y = 0;     // pixel
  float ambientWhite[3] = {1, 1, 1};
  // R50: the composited glare (exposed, before the warm gain) and the 1/4-resolution low-pass (exposed) at this
  // pixel, as the pass samples them; hasLow false = no low-pass this frame (the pixel itself is used).
  float glare[3] = {0, 0, 0};
  float low[3] = {0, 0, 0};
  bool hasLow = false;
};

// R50 night vision, per pixel. `pre`: the exposed linear Rec.709 colour after the warm gain (the light reaching
// the eye); `low`: the low-pass in the same units; `lin`: the cone image (after DCS's dark desaturation and the
// saturation step), blended in place toward the rod image.
// Scotopic luminance after Larson, Rushmeier & Piatko 1997 from CIE XYZ, normalised so that a neutral keeps its
// luminance (V / Y of Rec.709 white = 2.5730); the rod image is that, tinted; the blend weight is the rod share
// (nvW) x the pixel's own darkness (full rods up to 1 EV below mid-grey, full cones from +1.5 EV: lights, lit
// instruments and HUD keep their colour) x a guard for strongly saturated emissives from 3.5 EV below mid-grey up (a
// dimmed HUD, LEDs, taxi lights; not dark surfaces, which go to the rods whatever their colour).
inline float ScotopicOf(const float c[3]) {
  const float X = 0.4124f * c[0] + 0.3576f * c[1] + 0.1805f * c[2];
  const float Y = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
  const float Z = 0.0193f * c[0] + 0.1192f * c[1] + 0.9505f * c[2];
  if (!(X > 1e-30f) || !(Y > 0)) return 0.0f;
  return std::max(0.0f, Y * (1.33f * (1 + (Y + Z) / X) - 1.68f)) * (1.0f / 2.5730f);
}
inline float SmoothStepF(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}
inline void NightVision(const ColorCb& c, const float pre[3], const float low[3], float lin[3]) {
  const float Y = 0.2126f * pre[0] + 0.7152f * pre[1] + 0.0722f * pre[2];
  const float ev = std::log2(std::max(Y, 1e-30f) / 0.18f);
  const float g = 1 - SmoothStepF(c.nvConeLo, c.nvConeHi, ev);
  const float mx = std::max(pre[0], std::max(pre[1], pre[2])), mn = std::min(pre[0], std::min(pre[1], pre[2]));
  const float s = mx > 1e-30f ? (mx - mn) / mx : 0.0f;
  const float w = c.nvW * g * (1 - 0.75f * SmoothStepF(0.6f, 0.95f, s) * SmoothStepF(-3.5f, -1.5f, ev));
  float v = ScotopicOf(pre);
  if (c.nvBlur > 0) v += (ScotopicOf(low) - v) * c.nvBlur;
  const float tint[3] = {c.nvTintR, c.nvTintG, c.nvTintB};
  for (int i = 0; i < 3; ++i) lin[i] += (v * tint[i] - lin[i]) * w;
}
// mode 0 (stock emulation): pass c = nullptr. Returns the value written to the UNORM target (0..1, unclamped).
// mid (R51 VrFx tests): called on the exposed linear colour right before the tone curve (where the pass's VRFX
// variant applies G vision).
inline void Shade(const DcsTone& t, const ColorCb* c, const ShadeIn& in, float out[3],
                  void (*mid)(float lin[3], const void* ctx) = nullptr, const void* midCtx = nullptr) {
  const float lumW[3] = {0.2125f, 0.7154f, 0.0721f};
  const float rec[3] = {0.2126f, 0.7152f, 0.0722f};
  const float floorL = c && c->floorLum > 0 ? c->floorLum : t.lumMin;
  const float lum = std::clamp(in.adapted, floorL, t.lumMax);
  const float ev = in.ec + (c ? c->evOffset : 0.0f);
  const float exposure = std::exp2(std::log2(0.18f / lum) + ev);
  float lin[3];
  for (int i = 0; i < 3; ++i) {
    lin[i] = (in.scene[i] + (in.bloom[i] - in.scene[i]) * t.bloomLerp) * exposure;
    if (t.whiteBalance > 0) lin[i] /= 1 + (in.ambientWhite[i] - 1) * t.whiteBalance;
  }
  if (c) lin[0] *= c->warmR, lin[1] *= c->warmG, lin[2] *= c->warmB;
  const float pre[3] = {lin[0], lin[1], lin[2]};  // R50: the light reaching the eye (rods, glare order)
  {  // DCS's desturateColorHack, scaled by desatAmt
    const float l = lin[0] * lumW[0] + lin[1] * lumW[1] + lin[2] * lumW[2];
    float wd = std::sqrt(std::min(1.0f, l / t.lumMin * 0.4f));
    if (c && c->desatAmt != 1.0f) wd = 1 + (wd - 1) * c->desatAmt;
    for (int i = 0; i < 3; ++i) lin[i] = l + (lin[i] - l) * wd;
  }
  if (c && c->saturation != 1.0f) {
    const float l = lin[0] * rec[0] + lin[1] * rec[1] + lin[2] * rec[2];
    const float mn = std::min(lin[0], std::min(lin[1], lin[2]));
    float s = c->saturation;
    if (s > 1 && l > mn && l > 0) s = std::min(s, l / (l - mn));
    for (int i = 0; i < 3; ++i) lin[i] = std::max(0.0f, l + (lin[i] - l) * s);
  }
  if (c && c->nvW > 0) {  // R50 night vision: the rod image over the cone image
    float low[3];
    const float wg[3] = {c->warmR, c->warmG, c->warmB};
    for (int i = 0; i < 3; ++i) low[i] = in.hasLow ? in.low[i] * wg[i] : pre[i];
    NightVision(*c, pre, low, lin);
  }
  if (c && c->glareOn > 0) {  // R50 glare (exposed, with the warm gain as the scene)
    lin[0] += in.glare[0] * c->warmR, lin[1] += in.glare[1] * c->warmG, lin[2] += in.glare[2] * c->warmB;
  }
  if (c && c->debug > 0) {  // R50 debug: the glare only, tone-mapped as the scene
    lin[0] = in.glare[0], lin[1] = in.glare[1], lin[2] = in.glare[2];
  }
  if (mid) mid(lin, midCtx);
  const bool own = c && c->ownCurve > 0;
  const float sl = own ? c->slope : t.slope, to = own ? c->toe : t.toe, sh = own ? c->shoulder : t.shoulder;
  const float wc = own ? c->whiteClip : t.whiteClip;
  float tm[3];
  for (int i = 0; i < 3; ++i) tm[i] = Filmic(std::log10(std::max(lin[i], 1e-30f)), sl, to, sh, t.blackClip, wc);
  if (c && (c->hueKeep > 0 || c->hueShadow > 0)) {
    const float l = lin[0] * rec[0] + lin[1] * rec[1] + lin[2] * rec[2];
    const float mx0 = std::max(lin[0], std::max(lin[1], lin[2]));
    const float n = l + (mx0 - l) * c->chromaNorm;
    const float tn = Filmic(std::log10(std::max(n, 1e-30f)), sl, to, sh, t.blackClip, wc);
    float hp[3];
    for (int i = 0; i < 3; ++i) hp[i] = n > 1e-30f ? lin[i] * (tn / n) : 0.0f;
    // path to white for low-chroma highlights
    float tt = std::clamp((tn - c->whiteStart) / std::max(1 - c->whiteStart, 1e-4f), 0.0f, 1.0f);
    float w = tt * tt * (3 - 2 * tt);
    float mx = std::max(hp[0], std::max(hp[1], hp[2])), mn = std::min(hp[0], std::min(hp[1], hp[2]));
    const float sIn = mx > 1e-12f ? (mx - mn) / mx : 0.0f;
    w *= 1 - sIn * sIn;
    float hl = hp[0] * rec[0] + hp[1] * rec[1] + hp[2] * rec[2];
    for (int i = 0; i < 3; ++i) hp[i] += (hl - hp[i]) * w;
    // gamut: soft from 0.9, between desaturating (luminance kept) and darkening (saturation kept)
    hl = std::min(hp[0] * rec[0] + hp[1] * rec[1] + hp[2] * rec[2], 1.0f);
    mx = std::max(hp[0], std::max(hp[1], hp[2]));
    if (mx > 0.9f) {
      const float target = 0.9f + 0.1f * std::tanh(std::min((mx - 0.9f) / 0.1f, 10.0f));
      const float k = mx - hl > 1e-6f ? std::clamp((target - hl) / (mx - hl), 0.0f, 1.0f) : 1.0f;
      for (int i = 0; i < 3; ++i) {
        const float a = hl + (hp[i] - hl) * k, b = hp[i] * (target / mx);
        hp[i] = a + (b - a) * c->keepSat;
      }
    }
    // hue keep by tone: hueShadow below tn 0.05 (about -1.7 EV), hueKeep from tn 0.35 (+0.6 EV) up
    const float t2 = std::clamp((tn - 0.05f) / 0.30f, 0.0f, 1.0f);
    const float hk = c->hueShadow + (c->hueKeep - c->hueShadow) * (t2 * t2 * (3 - 2 * t2));
    for (int i = 0; i < 3; ++i) tm[i] += (hp[i] - tm[i]) * hk;
  }
  if (c) ApplyPanelGamut(tm, c->panelGamut);  // R111 [Color] PanelGamut (0: nothing)
  float g[3];
  for (int i = 0; i < 3; ++i) g[i] = std::pow(std::fabs(std::clamp(tm[i], 0.0f, 2.0f)), t.outputGammaInv);
  const int dm = c ? static_cast<int>(c->ditherMode + 0.5f) : 0;
  if (dm != 1) {  // DCS's applyDitheringOnLowLuminance(pixel, color, 1 / 0.285)
    const float l = (g[0] + g[1] + g[2]) * 0.333333f;
    const float k = std::clamp(l * (1 / 0.285f), 0.0f, 1.0f);
    const float f = (0.9f + 0.2f * Bayer8(in.x, in.y)) * (1 - k) + k;
    for (int i = 0; i < 3; ++i) g[i] *= f;
  }
  for (int i = 0; i < 3; ++i) {
    float v = g[i];
    if (c && c->srgb > 0) v = SrgbOetfF(std::pow(std::max(v, 0.0f), 1.0f / t.outputGammaInv));
    if (c && dm >= 1 && v > 0 && v < 1) v += (Ign(in.x, in.y, c->frame) - 0.5f) * (0.98f / 255.0f);
    out[i] = v;
  }
}
inline uint8_t ToUnorm8(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

// ---------------------------------------------------------------------------
// R50 CPU reference of the glare / low-pass chain (color_pass.h kGlareHlsl, the PS composite): tests, offline
// checks. Images are RGB float, row-major; the levels as the GPU holds them (used part of the allocation).
// ---------------------------------------------------------------------------
constexpr int kGlareAllocRef = 640;
// DCS's gProj (a perspective projection, row-vector convention) as its cbuffer holds it: P[r][c] at raw[c * 4 + r].
inline void PackProj(float p00, float p11, float ox, float oy, float raw[16]) {
  float P[4][4] = {{p00, 0, 0, 0}, {0, p11, 0, 0}, {ox, oy, 0, 1}, {0, 0, 0.1f, 0}};  // reverse-Z, infinite far
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) raw[c * 4 + r] = P[r][c];
}
struct ProjQ {
  float p00 = 0, p11 = 0, ox = 0, oy = 0;
  bool ok = false;
};
inline ProjQ ProjFromRaw(const float* r) {
  ProjQ q;
  const float p23 = r[14], p33 = r[15];
  q.p00 = r[0], q.p11 = r[5], q.ox = r[2] / p23, q.oy = r[6] / p23;
  q.ok = std::fabs(p23) > 1e-6f && std::fabs(p33) <= 1e-4f * std::fabs(p23) && q.p00 > 0.1f && q.p00 < 50 &&
         std::fabs(q.p11) > 0.1f && std::fabs(q.p11) < 50 && std::fabs(r[12]) <= 1e-4f && std::fabs(r[13]) <= 1e-4f &&
         std::fabs(r[1]) <= 0.02f * q.p00 && std::fabs(r[4]) <= 0.02f * std::fabs(q.p11) && std::fabs(q.ox) < 4 &&
         std::fabs(q.oy) < 4;
  return q;
}
// A focus pixel's uv (0..1 over its image) to the periphery's uv: through the view tangents.
inline void FocusToPeriUv(const ProjQ& f, const ProjQ& p, float u, float v, float* pu, float* pv) {
  const float tx = (u * 2 - 1 - f.ox) / f.p00, ty = (1 - v * 2 - f.oy) / f.p11;
  *pu = (tx * p.p00 + p.ox) * 0.5f + 0.5f;
  *pv = 0.5f - (ty * p.p11 + p.oy) * 0.5f;
}
struct GlareLevels {
  int W = 0, H = 0;           // periphery size
  int w[3] = {}, h[3] = {};   // used level sizes
  std::vector<float> g[3], low, streak;
};
inline void BrightRef(const ColorCb& c, const float in[3], float out[3]) {
  const float e = 0.2126f * in[0] + 0.7152f * in[1] + 0.0722f * in[2];
  if (!(e > 0)) {
    out[0] = out[1] = out[2] = 0;
    return;
  }
  const float k = 0.5f * c.glareT;
  const float s = std::clamp(e - c.glareT + k, 0.0f, 2 * k);
  float f = std::max(s * s / (4 * k), e - c.glareT) / e;
  if (e > c.glareCap) f *= c.glareCap / e;
  for (int i = 0; i < 3; ++i) out[i] = in[i] * f;
}
// Bilinear sample (clamp sampler) of a level allocated `alloc` x `alloc` whose used part is w x h (the rest 0).
inline void SampleRef(const std::vector<float>& img, int w, int h, int alloc, float u, float v, float out[3]) {
  const float x = u * alloc - 0.5f, y = v * alloc - 0.5f;
  const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
  const float fx = x - x0, fy = y - y0;
  out[0] = out[1] = out[2] = 0;
  for (int j = 0; j < 2; ++j)
    for (int i = 0; i < 2; ++i) {
      const int xi = std::clamp(x0 + i, 0, alloc - 1), yi = std::clamp(y0 + j, 0, alloc - 1);
      const float wt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
      if (xi >= w || yi >= h || wt == 0) continue;
      for (int k = 0; k < 3; ++k) out[k] += img[(static_cast<size_t>(yi) * w + xi) * 3 + k] * wt;
    }
}
// The chain for one eye: hdr (W x H, scene-linear), its exposure, the periphery projection's P00 / P11.
inline void GlareChainRef(const float* hdr, int W, int H, float exposure, const ColorCb& c, float p00, float p11,
                          GlareLevels& L) {
  L.W = W, L.H = H;
  L.w[0] = (W + 3) / 4, L.h[0] = (H + 3) / 4;
  for (int l = 1; l < 3; ++l) L.w[l] = (L.w[l - 1] + 1) / 2, L.h[l] = (L.h[l - 1] + 1) / 2;
  for (int l = 0; l < 3; ++l) L.g[l].assign(static_cast<size_t>(L.w[l]) * L.h[l] * 3, 0.0f);
  L.low.assign(L.g[0].size(), 0.0f);
  L.streak.assign(L.g[1].size(), 0.0f);
  for (int y = 0; y < L.h[0]; ++y)
    for (int x = 0; x < L.w[0]; ++x) {
      float b[3] = {0, 0, 0}, n[3] = {0, 0, 0};
      for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
          const int sx = std::min(x * 4 + i, W - 1), sy = std::min(y * 4 + j, H - 1);
          float px[3], br[3];
          bool fin = true;
          for (int k = 0; k < 3; ++k) {
            px[k] = hdr[(static_cast<size_t>(sy) * W + sx) * 3 + k] * exposure;
            fin = fin && px[k] >= 0 && px[k] < 1e30f;
          }
          if (!fin) px[0] = px[1] = px[2] = 0;
          BrightRef(c, px, br);
          for (int k = 0; k < 3; ++k) n[k] += px[k], b[k] += br[k];
        }
      for (int k = 0; k < 3; ++k) {
        L.g[0][(static_cast<size_t>(y) * L.w[0] + x) * 3 + k] = b[k] * (1.0f / 16);
        L.low[(static_cast<size_t>(y) * L.w[0] + x) * 3 + k] = n[k] * (1.0f / 16);
      }
    }
  for (int l = 1; l < 3; ++l)
    for (int y = 0; y < L.h[l]; ++y)
      for (int x = 0; x < L.w[l]; ++x)
        for (int k = 0; k < 3; ++k) {  // [1 3 3 1] / 8 tent
          static const float wt[4] = {0.125f, 0.375f, 0.375f, 0.125f};
          float a = 0;
          for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
              a += L.g[l - 1][(static_cast<size_t>(std::clamp(y * 2 + j - 1, 0, L.h[l - 1] - 1)) * L.w[l - 1] +
                               std::clamp(x * 2 + i - 1, 0, L.w[l - 1] - 1)) * 3 + k] * (wt[i] * wt[j]);
          L.g[l][(static_cast<size_t>(y) * L.w[l] + x) * 3 + k] = a;
        }
  auto ppt = [&](int l, int d) { return (d == 0 ? p00 * W : std::fabs(p11) * H) * 0.5f / static_cast<float>(4 << l); };
  const float sigT[3] = {c.sig0, c.sig1, c.sig2};
  for (int l = 0; l < 3; ++l)
    for (int d = 0; d < 2; ++d) {
      float sig = sigT[l] * ppt(l, d);
      sig = std::sqrt(std::max(sig * sig - (l == 0 ? 0.25f : l == 1 ? 0.37f : 0.41f), 0.0f));  // pyramid + bilinear
      if (!(sig >= 0.3f)) continue;
      const int r = std::min(static_cast<int>(std::ceil(4 * sig)), 32);
      const float k2 = -0.5f / (sig * sig);
      std::vector<float> out(L.g[l].size());
      for (int y = 0; y < L.h[l]; ++y)
        for (int x = 0; x < L.w[l]; ++x) {
          float a[3] = {0, 0, 0}, ws = 0;
          for (int k = -r; k <= r; ++k) {
            const float wk = std::exp(static_cast<float>(k * k) * k2);
            const int sx = d == 0 ? std::clamp(x + k, 0, L.w[l] - 1) : x, sy = d == 1 ? std::clamp(y + k, 0, L.h[l] - 1) : y;
            for (int i = 0; i < 3; ++i) a[i] += L.g[l][(static_cast<size_t>(sy) * L.w[l] + sx) * 3 + i] * wk;
            ws += wk;
          }
          for (int i = 0; i < 3; ++i) out[(static_cast<size_t>(y) * L.w[l] + x) * 3 + i] = a[i] / ws;
        }
      L.g[l].swap(out);
    }
  if (c.glareStreak > 0) {
    const float alloc = 320.0f;
    for (int y = 0; y < L.h[1]; ++y)
      for (int x = 0; x < L.w[1]; ++x) {
        const float cu = (x + 0.5f) / alloc, cv = (y + 0.5f) / alloc;
        const float hu = (L.w[1] - 0.5f) / alloc, hv = (L.h[1] - 0.5f) / alloc, lo = 0.5f / alloc;
        float a[3] = {0, 0, 0}, ws = 0;
        for (int dd = 0; dd < 3; ++dd) {
          const float an = (15.0f + 60.0f * dd) * 3.14159265f / 180.0f;
          const float su = std::cos(an) * c.streakLen * ppt(1, 0) / (12.0f * alloc);
          const float sv = -std::sin(an) * c.streakLen * ppt(1, 1) / (12.0f * alloc);
          for (int k = 1; k <= 12; ++k) {
            const float wk = std::exp(-4.0f * k / 12.0f);
            float s1[3], s2[3];
            SampleRef(L.g[1], L.w[1], L.h[1], 320, std::clamp(cu + su * k, lo, hu), std::clamp(cv + sv * k, lo, hv), s1);
            SampleRef(L.g[1], L.w[1], L.h[1], 320, std::clamp(cu - su * k, lo, hu), std::clamp(cv - sv * k, lo, hv), s2);
            for (int i = 0; i < 3; ++i) a[i] += (s1[i] + s2[i]) * wk;
            ws += 2 * wk;
          }
        }
        for (int i = 0; i < 3; ++i) L.streak[(static_cast<size_t>(y) * L.w[1] + x) * 3 + i] = a[i] / ws;
      }
  }
}
// The PS composite at a periphery uv: the glare (exposed, before the warm gain) and the low-pass.
inline void GlareAtRef(const GlareLevels& L, const ColorCb& c, float u, float v, float glare[3], float low[3]) {
  const float sx = L.W / 2560.0f, sy = L.H / 2560.0f;
  const float au = std::clamp(u * sx, 0.5f / 640.0f, sx - 0.5f / 160.0f);
  const float av = std::clamp(v * sy, 0.5f / 640.0f, sy - 0.5f / 160.0f);
  float g0[3], g1[3], g2[3], st[3];
  SampleRef(L.g[0], L.w[0], L.h[0], 640, au, av, g0);
  SampleRef(L.g[1], L.w[1], L.h[1], 320, au, av, g1);
  SampleRef(L.g[2], L.w[2], L.h[2], 160, au, av, g2);
  SampleRef(L.streak, L.w[1], L.h[1], 320, au, av, st);
  SampleRef(L.low, L.w[0], L.h[0], 640, au, av, low);
  for (int i = 0; i < 3; ++i) {
    const float h = c.w0 * g0[i] + c.w1 * g1[i] + c.w2 * g2[i];
    glare[i] = c.glareOn > 0 ? c.glareStrength * (h + (st[i] - h) * c.glareStreak) : 0.0f;
  }
}

}  // namespace cgrade
