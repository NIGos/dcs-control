// R42 colour modes, the D3D11 part ([Color] Mode, default 0 = nothing armed, nothing changed;
// docs\research\R42_color_modes.md). The pure parts (parameters, ini, constant buffer, CPU reference) are in
// color_grade.h.
//
// What it does, per HMD view (0 periphery L, 1 periphery R, 2 focus L, 3 focus R): at the first draw of the view's
// ToneMappingPass (fixation_depth.h arms the draw entries for that pass and calls ToneDrawPre / ToneDrawPost around
// DCS's draw) the bound state is checked (R8G8B8A8_UNORM target of the size the view's CopyToHMDPass reads, PS t0 an
// R16G16B16A16_FLOAT texture of that size, t1 the 8-byte-stride luminance buffer, b0 cbTonemapParams, no 3D LUT).
// Then DCS's pixel shader is swapped for ours for that one draw: same draw, same bindings, same target, so the cost
// is a few more ALU per pixel; the HUD / MFD symbology (OverlayLDRPass, GUIPass) is drawn after it and is not
// touched. DCS's pixel shader, PS b13 and PS t16 go back right after the draw.
// Small extras on the GPU (only when the mode needs them):
//  - view 1: 16 bytes of b0 (exposure key .. EC) copied into our state buffer: the EC all views use (EqualizeEc);
//  - view 0: the adaptation step (1 thread) on DCS's adapted luminance (t1[1].x) with asymmetric time constants and
//    the focus / periphery log-average ratio (MeterFocus); views 0 and 2: a 16 x 16-thread log-average meter.
// One state for the whole frame: the four views get the same exposure (no seam, no eye difference).
// Timing ([Color] Timing=1, also with Mode 0 = stock): GPU timestamps around each view's tone-map draw (and ours).
// Render-thread functions run inside DCS's draw hook (fixation_depth.h); the config thread compiles and creates.
#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "color_grade.h"
#include "vrfx.h"  // R51 [VrFx] GazeDof / GVision: run inside this pass (psFx, one prep dispatch per view)
// R57: with [Color] Mode 0 the effects run on psFxStock, the same source compiled with QV_STOCK=1: vrfx::StockCb's
// values become compile-time constants, so R42 / R50's colour, glare and night-vision code (off in that constant
// buffer) is not in the shader. Same output; a smaller shader is cheaper for every pixel (R57: about -0.025 ms over the
// four views). Used only while the frame's constant buffer is the stock one (Rt::cbStock).

namespace cpass {

using LogFn = void (*)(const char*, ...);
inline LogFn g_log = nullptr;

// Our slots in DCS's tone-map pixel shader stage (restored after the draw) and the state layout (bytes).
constexpr UINT kPsCbSlot = 13, kPsStateSlot = 16;
constexpr UINT kStAdapted = 0, kStValid = 4, kStDcs = 8, kStTarget = 12, kStPeri = 16, kStFocus = 20, kStEcRow = 32;
constexpr UINT kRsBytes = 32;  // R81: the adaptation reset log reads state bytes 0..31 (adapted .. focus)
constexpr UINT kStateBytes = vrfx::kStFxBytes;  // R51: 512 -> 768 (DCS adapted luminance at 48, per-view gProj at 512)
// R50: per-eye glare stamp (+64), periphery size (+72), and each view's gProj copy (+256 + v * 64).
constexpr UINT kStStamp = 64, kStPeriSize = 72, kStProj = 256;
constexpr UINT kPsViewSlot = 12, kPsAuxSlot = 17, kPsAuxN = 5, kPsSamplerSlot = 15;
static_assert(kStProj + 4 * 64 <= vrfx::kStFxProj && kStStamp >= vrfx::kStFxLum + 4, "R50 / R51 state layout");
static_assert(vrfx::kPsFxCbSlot != kPsViewSlot && vrfx::kPsFxCbSlot != kPsCbSlot &&
                  vrfx::kPsFxSrvSlot >= kPsAuxSlot + kPsAuxN,
              "R50 / R51 PS slots");
constexpr UINT kGlareAlloc = 640;  // level 0 allocation (periphery up to 4 x 640 px)

inline const char kPsHlsl[] = R"(
struct VSO {
  noperspective float4 pos : SV_POSITION0;
  noperspective float4 projPos : TEXCOORD0;
  noperspective float3 worldPos : TEXCOORD1;
};
Texture2D<float4> ComposedMap : register(t0);
StructuredBuffer<float2> avgLuminance : register(t1);
Texture2D<float4> bloomTexture : register(t2);
ByteAddressBuffer qvState : register(t16);
cbuffer cbTonemapParams : register(b0) {
  float dcExposureKey, dcSigmaKey, dcTau, dcExposureCorrection;
  float bloomThreshold, bloomLerpFactor, whiteBalanceFactor, vignetteFactor;
  float4 dcViewport;
  uint2 dcDims; float tmExp; float tmPower;
  float sceneLuminanceMin, sceneLuminanceMax, focusWidth, focusSigma;
  float inputLuminanceMin, inputLuminanceMax, inputLuminanceRange, hwFactor;
  float percentMin, percentMax, operatorGamma, cubeAverageLumAmount;
  float slope, toe, shoulder, blackClip;
  float whiteClip, LUTLogLuminanceMin, LUTLogLuminanceMax, cockpitExposureClamp;
  float2 inputLuminanceScaleOffset; float outputGammaInv; float tpDummy01;
};
cbuffer cAmbientMap : register(b8) { float4 AmbientMap[9]; };
cbuffer cbQvColor : register(b13) {
  float qSrgb, qOwnCurve, qHueKeep, qChromaNorm;
  float qSlope, qToe, qShoulder, qWhiteClip;
  float qWhiteStart, qKeepSat, qSaturation, qDesatAmt;
  float3 qWarm; float qFloorLum;
  float qEvOffset, qDither, qFrame, qUseState;
  float qDt, qTauBright, qTauDark, qMeterFocus;
  float qEqualizeEc, qReset, qHueShadow, qPad1;
  float qNvW, qNvTintR, qNvTintG, qNvTintB;
  float qNvBlur, qNvConeLo, qNvConeHi, qGlareOn;
  float qGlareStrength, qGlareT, qGlareCap, qGlareStreak;
  float qSig0, qSig1, qSig2, qStreakLen;
  float qW0, qW1, qW2, qQuad;
  float qDebug, qAuxOn, qPad2, qPad3;
};
#if QV_STOCK
// R57 psFxStock: vrfx::StockCb's values (the fields it leaves 0 and its 1s) as constants. qFrame stays the buffer's
// (only the aux / dither >= 1 paths read it, both off here); qWhiteStart / qKeepSat are only read by the hue-keep
// path, off here.
#define qSrgb 0.0
#define qOwnCurve 0.0
#define qHueKeep 0.0
#define qHueShadow 0.0
#define qSaturation 1.0
#define qDesatAmt 1.0
#define qWarm float3(1.0, 1.0, 1.0)
#define qFloorLum 0.0
#define qEvOffset 0.0
#define qDither 0.0
#define qUseState 0.0
#define qEqualizeEc 0.0
#define qNvW 0.0
#define qGlareOn 0.0
#define qDebug 0.0
#define qAuxOn 0.0
#endif
// R50: this draw's HMD view (0..3; 4 = unknown: no glare / low-pass) and the eye's 1/4-resolution levels.
cbuffer cbQvView : register(b12) { uint qView; uint3 qViewPad; };
Texture2D<float4> qvGlare0 : register(t17);
Texture2D<float4> qvGlare1 : register(t18);
Texture2D<float4> qvGlare2 : register(t19);
Texture2D<float4> qvStreak : register(t20);
Texture2D<float4> qvLow : register(t21);
SamplerState qvLinear : register(s15);
static const float3 LUMW = float3(0.2125, 0.7154, 0.0721);
static const float3 REC = float3(0.2126, 0.7152, 0.0722);
float PRaw(uint v, uint i) { return asfloat(qvState.Load(256 + v * 64 + i * 4)); }
// DCS's gProj (cPerView +256) as copied at the view's tone-map draw; default column-major packing: P[r][c] at raw
// [c * 4 + r]. q = (P00, P11, ox, oy). A perspective projection without shear, or false.
bool ProjOk(uint v, out float4 q) {
  float p00 = PRaw(v, 0), p11 = PRaw(v, 5), p23 = PRaw(v, 14), p33 = PRaw(v, 15);
  float p03 = PRaw(v, 12), p13 = PRaw(v, 13), p10 = PRaw(v, 1), p01 = PRaw(v, 4);
  q = float4(p00, p11, PRaw(v, 2) / p23, PRaw(v, 6) / p23);
  return abs(p23) > 1e-6 && abs(p33) <= 1e-4 * abs(p23) && p00 > 0.1 && p00 < 50 && abs(p11) > 0.1 && abs(p11) < 50 &&
         abs(p03) <= 1e-4 && abs(p13) <= 1e-4 && abs(p10) <= 0.02 * p00 && abs(p01) <= 0.02 * abs(p11) &&
         abs(q.z) < 4 && abs(q.w) < 4;
}
float SmoothStepQ(float a, float b, float x) {
  float t = saturate((x - a) / (b - a));
  return t * t * (3 - 2 * t);
}
float ScotopicOf(float3 c) {
  float X = dot(c, float3(0.4124, 0.3576, 0.1805)), Y = dot(c, float3(0.2126, 0.7152, 0.0722));
  float Z = dot(c, float3(0.0193, 0.1192, 0.9505));
  if (!(X > 1e-30) || !(Y > 0)) return 0;
  return max(0, Y * (1.33 * (1 + (Y + Z) / X) - 1.68)) * (1.0 / 2.5730);
}
static const uint ditherArray[8][8] = {
  { 0, 32, 8, 40, 2, 34, 10, 42}, {48, 16, 56, 24, 50, 18, 58, 26}, {12, 44, 4, 36, 14, 46, 6, 38},
  {60, 28, 52, 20, 62, 30, 54, 22}, { 3, 35, 11, 43, 1, 33, 9, 41}, {51, 19, 59, 27, 49, 17, 57, 25},
  {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21}};
float Seg(float c0, float c1, float ca, float s, float x) {
  float t = 1 + c1 - c0;
  return 2 * t / (1 + exp((2 * s / t) * (x - ca))) - c1;
}
float Filmic(float x, float sl, float to, float sh, float bc, float wc) {
  float ta = (1 - to - 0.18) / sl - 0.733;
  float sa = (sh - 0.18) / sl - 0.733;
  if (x < ta) return Seg(to, bc, ta, -sl, x);
  if (x < sa) return sl * (x + 0.733) + 0.18;
  return 1 - Seg(sh, wc, sa, sl, x);
}
float SrgbOetf(float l) {
  l = saturate(l);
  return l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
}
float Max3(float3 v) { return max(v.x, max(v.y, v.z)); }
float Min3(float3 v) { return min(v.x, min(v.y, v.z)); }
// @VRFX_BLOCK@
// R95: the tone map of one HDR colour at pixel px (tw x th: the view's size). main evaluates it once; the symbology
// variant (QV_SYM) a second time on the HDR without the symbology draws, with every other input the same.
float3 QvTone(uint2 px, float3 scene, float3 bloom, uint tw, uint th) {
#if VRFX
  scene = FxDof(px, scene);
#endif
  float adapted = avgLuminance[1].x;
  if (qUseState > 0 && qvState.Load(4) == 1) adapted = asfloat(qvState.Load(0));
  float ec = dcExposureCorrection;
  if (qEqualizeEc > 0 && asfloat(qvState.Load(32)) > 0) ec = asfloat(qvState.Load(44));
  float floorL = qFloorLum > 0 ? qFloorLum : sceneLuminanceMin;
  float lum = clamp(adapted, floorL, sceneLuminanceMax);
  float exposure = exp2(log2(0.18 / lum) + ec + qEvOffset);
  float3 lin = lerp(scene, bloom, bloomLerpFactor) * exposure;
  if (whiteBalanceFactor > 0) lin /= lerp(1, AmbientMap[8].rgb, whiteBalanceFactor);
  lin *= qWarm;
  // R50: the eye's glare and low-pass (computed at its periphery view, this frame), sampled at this pixel's
  // direction: a focus view maps its pixel through its own and the periphery's projection, so both views read the
  // same angular field (no seam); a periphery view only uses it while its eye's focus projection is valid too.
  float3 glare = 0, low = lin;
  if (qAuxOn > 0 && qView < 4) {
    uint e = qView & 1;
    if (asfloat(qvState.Load(64 + e * 4)) == qFrame) {
      float2 uv = (float2(px) + 0.5) / float2(max(tw, 1), max(th, 1));
      float4 qs, qp;
      bool ok = true;
      if (qView >= 2) {
        ok = ProjOk(qView, qs) && ProjOk(e, qp);
        float tx = (uv.x * 2 - 1 - qs.z) / qs.x, ty = (1 - uv.y * 2 - qs.w) / qs.y;
        uv = float2((tx * qp.x + qp.z) * 0.5 + 0.5, 0.5 - (ty * qp.y + qp.w) * 0.5);
      } else if (qQuad > 0) {
        ok = ProjOk(e + 2, qs);
      }
      if (ok) {
        float2 sc = float2(asfloat(qvState.Load(72 + e * 8)), asfloat(qvState.Load(76 + e * 8))) / 2560.0;
        float2 auv = clamp(uv * sc, 0.5 / 640.0, sc - 0.5 / 160.0);
        if (qGlareOn > 0) {
          float3 h = qW0 * qvGlare0.SampleLevel(qvLinear, auv, 0).rgb + qW1 * qvGlare1.SampleLevel(qvLinear, auv, 0).rgb +
                     qW2 * qvGlare2.SampleLevel(qvLinear, auv, 0).rgb;
          glare = qGlareStrength * (h + (qvStreak.SampleLevel(qvLinear, auv, 0).rgb - h) * qGlareStreak);
        }
        if (qNvBlur > 0) low = qvLow.SampleLevel(qvLinear, auv, 0).rgb * qWarm;
      }
    }
  }
  float3 pre = lin;  // R50: the light reaching the eye (the rods' input)
  float l = dot(lin, LUMW);
  float wd = sqrt(min(1, l / sceneLuminanceMin * 0.4));
  if (qDesatAmt != 1) wd = 1 + (wd - 1) * qDesatAmt;
  lin = l + (lin - l) * wd;
  if (qSaturation != 1) {
    float l2 = dot(lin, REC);
    float mn = Min3(lin);
    float s = qSaturation;
    if (s > 1 && l2 > mn && l2 > 0) s = min(s, l2 / (l2 - mn));
    lin = max(0, l2 + (lin - l2) * s);
  }
  if (qNvW > 0) {  // R50 night vision: the rod image over the cone image (DCS's dark desaturation kept)
    float Yn = dot(pre, REC);
    float evn = log2(max(Yn, 1e-30) / 0.18);
    float g = 1 - SmoothStepQ(qNvConeLo, qNvConeHi, evn);
    float mxn = Max3(pre), mnn = Min3(pre);
    float sn = mxn > 1e-30 ? (mxn - mnn) / mxn : 0;
    float wn = qNvW * g * (1 - 0.75 * SmoothStepQ(0.6, 0.95, sn) * SmoothStepQ(-3.5, -1.5, evn));
    float vs = ScotopicOf(pre);
    if (qNvBlur > 0) vs += (ScotopicOf(low) - vs) * qNvBlur;
    lin += (vs * float3(qNvTintR, qNvTintG, qNvTintB) - lin) * wn;
  }
  if (qGlareOn > 0) lin += glare * qWarm;
  if (qDebug > 0) lin = glare;
#if VRFX
  lin = FxGVision(px, lin);
#endif
  bool own = qOwnCurve > 0;
  float sl = own ? qSlope : slope, to = own ? qToe : toe, sh = own ? qShoulder : shoulder, wc = own ? qWhiteClip : whiteClip;
  float3 tm;
  tm.r = Filmic(log10(max(lin.r, 1e-30)), sl, to, sh, blackClip, wc);
  tm.g = Filmic(log10(max(lin.g, 1e-30)), sl, to, sh, blackClip, wc);
  tm.b = Filmic(log10(max(lin.b, 1e-30)), sl, to, sh, blackClip, wc);
  if (qHueKeep > 0 || qHueShadow > 0) {
    float l3 = dot(lin, REC);
    float n = l3 + (Max3(lin) - l3) * qChromaNorm;
    float tn = Filmic(log10(max(n, 1e-30)), sl, to, sh, blackClip, wc);
    float3 hp = n > 1e-30 ? lin * (tn / n) : float3(0, 0, 0);
    float tt = saturate((tn - qWhiteStart) / max(1 - qWhiteStart, 1e-4));
    float w = tt * tt * (3 - 2 * tt);
    float mx = Max3(hp), mn = Min3(hp);
    float sIn = mx > 1e-12 ? (mx - mn) / mx : 0;
    w *= 1 - sIn * sIn;
    float hl = dot(hp, REC);
    hp += (hl - hp) * w;
    hl = min(dot(hp, REC), 1);
    mx = Max3(hp);
    if (mx > 0.9) {
      float target = 0.9 + 0.1 * tanh(min((mx - 0.9) / 0.1, 10));
      float k = mx - hl > 1e-6 ? saturate((target - hl) / (mx - hl)) : 1;
      float3 a = hl + (hp - hl) * k;
      float3 b = hp * (target / mx);
      hp = a + (b - a) * qKeepSat;
    }
    float t2 = saturate((tn - 0.05) / 0.30);
    float hk = qHueShadow + (qHueKeep - qHueShadow) * (t2 * t2 * (3 - 2 * t2));
    tm += (hp - tm) * hk;
  }
  float3 g = pow(abs(clamp(tm, 0, 2)), outputGammaInv);
  uint dm = (uint)(qDither + 0.5);
  if (dm != 1) {
    float lm = dot(g, 0.333333);
    float k = saturate(lm * (1 / 0.285));
    float f = lerp(0.9 + 0.2 * ((ditherArray[px.x % 8][px.y % 8] + 0.5) / 64.0), 1, k);
    g *= f;
  }
  float3 v = g;
  if (qSrgb > 0) {
    float3 ld = pow(max(g, 0), 1 / outputGammaInv);
    v = float3(SrgbOetf(ld.r), SrgbOetf(ld.g), SrgbOetf(ld.b));
  }
  if (dm >= 1) {
    float o = 5.588238 * fmod(qFrame, 64);
    float f = 0.06711056 * (px.x + o) + 0.00583715 * (px.y + o);
    float a = 52.9829189 * frac(f);
    float d = (frac(a) - 0.5) * (0.98 / 255);
    if (v.r > 0 && v.r < 1) v.r += d;
    if (v.g > 0 && v.g < 1) v.g += d;
    if (v.b > 0 && v.b < 1) v.b += d;
  }
  return v;
}
#if QV_SYM
// R95 (symbology.h): the view's window (DCS's target pixels x, y, w, h), its origin in the emission atlas, the alpha
// switch; the accumulated HDR change of the COLLIMATOR / HELMET draws (window texels, fp32); the emission atlas (u1).
// Where the symbology changed the HDR the shader leaves dec(C) (the second evaluation as the target would store it)
// and the coverage alpha there; symbology.h's finalize dispatch right after the draw makes it E = dec(F) - dec(C) from
// the bytes DCS's target holds. Elsewhere in the window it writes E = 0.
cbuffer cbQvSym : register(b10) { int4 qsWin; int4 qsAtl; };
Texture2D<float4> qsAcc : register(t25);
RWTexture2D<float4> qsOut : register(u1);
#if QV_SYM_TEST
RWTexture2D<float4> qsTestC : register(u2);  // tests: C's bytes / 255 (the second evaluation, as the target stores it)
#endif
float QsDec(float c) { return c <= 0.04045 ? c * (1.0 / 12.92) : pow((c + 0.055) * (1.0 / 1.055), 2.4); }
// The bytes the R8G8B8A8_UNORM target stores for v, decoded as Quad Views decodes the swapchain (sRGB).
float3 QsLin8(float3 v) {
  float3 q = floor(saturate(v) * 255.0 + 0.5) * (1.0 / 255.0);
  return float3(QsDec(q.r), QsDec(q.g), QsDec(q.b));
}
void QvSymOut(uint2 px, float3 scene, float3 bloom, uint tw, uint th, float3 v) {
  int2 w = int2(px) - qsWin.xy;
  [branch] if (any(w < 0) || any(w >= qsWin.zw)) return;
  float4 d = qsAcc.Load(int3(w, 0));
  float4 e = float4(0, 0, 0, 0);
  [branch] if (any(d != 0)) {
    float3 vp = QvTone(px, scene - d.rgb, bloom, tw, th);
    e.rgb = QsLin8(vp);  // dec(C); E = dec(F) - dec(C) after the draw (symbology.h FinalizeE: F as stored)
    if (qsAtl.z != 0) {  // the coverage: the target's alpha before the symbology ~0, the added alpha in [0, 1] (R94)
      float pa = ComposedMap.Load(int3(px, 0)).a - d.a;
      e.a = (abs(pa) <= 1.0 / 512.0 && d.a >= -1.0 / 512.0 && d.a <= 1.0 + 1.0 / 512.0) ? saturate(d.a) : 0;
    }
#if QV_SYM_TEST
    qsTestC[qsAtl.xy + w] = float4(floor(saturate(vp) * 255.0 + 0.5) * (1.0 / 255.0), 1);
  } else {
    qsTestC[qsAtl.xy + w] = float4(floor(saturate(v) * 255.0 + 0.5) * (1.0 / 255.0), 0);
#endif
  }
  qsOut[qsAtl.xy + w] = e;
}
#endif
float4 main(VSO i) : SV_Target0 {
  uint2 px = uint2(i.pos.xy);
  float3 scene = ComposedMap.Load(int3(px, 0)).rgb;
  uint tw, th, bw, bh;
  ComposedMap.GetDimensions(tw, th);
  bloomTexture.GetDimensions(bw, bh);
  float3 bloom = 0;
  if (bw > 0 && bh > 0)
    bloom = bloomTexture.Load(int3(min(px * uint2(bw, bh) / max(uint2(tw, th), uint2(1, 1)), uint2(bw - 1, bh - 1)), 0)).rgb;
  float3 v = QvTone(px, scene, bloom, tw, th);
#if QV_SYM
  QvSymOut(px, scene, bloom, tw, th, v);
#endif
  return float4(v, 1);
}
)";

// Log-average luminance of an HDR view on a 32 x 32 grid (CENTER: Gaussian centre weight, the focus views follow the
// gaze); one group of 16 x 16 threads, the result stored at byte SLOT of the state.
inline const char kMeterHlsl[] = R"(
Texture2D<float4> hdr : register(t0);
RWByteAddressBuffer st : register(u0);
groupshared float2 acc[256];
[numthreads(16, 16, 1)]
void main(uint3 gt : SV_GroupThreadID, uint gi : SV_GroupIndex) {
  uint w, h;
  hdr.GetDimensions(w, h);
  float2 a = 0;
  [unroll] for (uint j = 0; j < 2; ++j)
  [unroll] for (uint k = 0; k < 2; ++k) {
    float2 uv = (float2(gt.xy * 2 + uint2(k, j)) + 0.5) / 32.0;
    uint2 p = min(uint2(uv * float2(w, h)), uint2(max(w, 1) - 1, max(h, 1) - 1));
    float l = dot(hdr.Load(int3(p, 0)).rgb, float3(0.2126, 0.7152, 0.0722));
    l = (l > 1e-5 && l < 1e30) ? l : 1e-5;
    float wt = 1;
#if CENTER
    float2 r = uv * 2 - 1;
    wt = exp(-dot(r, r) * (1.0 / (2 * 0.35 * 0.35)));
#endif
    a += float2(log(l) * wt, wt);
  }
  acc[gi] = a;
  GroupMemoryBarrierWithGroupSync();
  [unroll] for (uint s = 128; s > 0; s >>= 1) {
    if (gi < s) acc[gi] += acc[gi + s];
    GroupMemoryBarrierWithGroupSync();
  }
  if (gi == 0) st.Store(SLOT, asuint(exp(acc[0].x / max(acc[0].y, 1e-6))));
}
)";

// The adaptation step (once per frame, view 0): eye-like time constants on top of DCS's adapted luminance.
inline const char kAdaptHlsl[] = R"(
StructuredBuffer<float2> avgLuminance : register(t1);
RWByteAddressBuffer st : register(u0);
cbuffer cbQvColor : register(b0) {
  float qSrgb, qOwnCurve, qHueKeep, qChromaNorm;
  float qSlope, qToe, qShoulder, qWhiteClip;
  float qWhiteStart, qKeepSat, qSaturation, qDesatAmt;
  float3 qWarm; float qFloorLum;
  float qEvOffset, qDither, qFrame, qUseState;
  float qDt, qTauBright, qTauDark, qMeterFocus;
  float qEqualizeEc, qReset, qHueShadow, qPad1;
  float qNvW, qNvTintR, qNvTintG, qNvTintB;
  float qNvBlur, qNvConeLo, qNvConeHi, qGlareOn;
  float qGlareStrength, qGlareT, qGlareCap, qGlareStreak;
  float qSig0, qSig1, qSig2, qStreakLen;
  float qW0, qW1, qW2, qQuad;
};
[numthreads(1, 1, 1)]
void main() {
  // R81: a reset frame. The state is invalid (no blend from it), and so is the focus meter: view 2 writes it after
  // this dispatch, so what it holds now is from a frame before the reset (before Mode 0, a gap, a mission event).
  // R77 snapped onto a target built with it (x (old focus / new periphery)^MeterFocus: 0.42 ln off at a x4 pitch
  // change) and blended from there with the eye-like time constants: the R80 live ramps of 2-3.5 s.
  if (qReset != 0) {
    st.Store(4, 0u);
    st.Store(20, 0u);
  }
  float dcs = avgLuminance[1].x;
  // R77: no usable DCS luminance: nothing (a reset stays pending in the state: valid 0).
  if (!(dcs > 1e-6 && dcs < 1e6)) return;
  float target = dcs;
  float peri = asfloat(st.Load(16)), foc = asfloat(st.Load(20));
  if (qMeterFocus > 0 && peri > 0 && foc > 0) target *= clamp(pow(foc / peri, qMeterFocus), 0.25, 4.0);
  float prev = asfloat(st.Load(0));
  uint valid = st.Load(4);
  float o = target;
  if (valid == 1 && prev > 0) {
    float tau = target > prev ? qTauBright : qTauDark;
    if (tau > 0) o = exp(lerp(log(prev), log(target), 1 - exp(-qDt / tau)));
  }
  // R81: with focus views and MeterFocus on, the state becomes valid only with a focus meter of this period: until
  // then the pixel shader uses DCS's adapted luminance and each frame starts at its target (no blend), so the frame
  // after the reset (view 2's meter has run) snaps onto the full target. The valid word counts the frames waited
  // (2..8; the pixel shader uses the state only at 1): without a focus meter for 8 frames (no view 2 tone map) the
  // adaptation goes on with the periphery-only target.
  // R86: only while not valid yet (after a reset): once valid it stays valid without a focus meter. Before, valid 1
  // with no meter went back to 2, so without view 2 the state was used 1 frame in 8 (the PS alternated with DCS's).
  uint nextValid = 1u;
  if (valid != 1 && qMeterFocus > 0 && qQuad > 0 && !(foc > 0)) {
    uint waited = valid >= 2 ? valid - 1 : 0;
    nextValid = waited + 1 >= 8 ? 1u : waited + 2;
  }
  st.Store(0, asuint(o));
  st.Store(4, nextValid);
  st.Store(8, asuint(dcs));
  st.Store(12, asuint(target));
}
)";

// R50: the glare / low-pass chain, run per eye at its periphery view (one source, KERNEL 0..3):
//  0 down:   1/4 resolution, 4 x 4 boxes of the exposed HDR: u1 = the above-threshold (glare) light, u2 = all of it
//            (the night-vision low-pass); thread 0 writes the eye's stamp (this frame, or -1 when the periphery
//            projection is not usable) and the periphery size into the state;
//  1 half:   [1 3 3 1] / 8 tent downsample into the next level (1/8, 1/16): a source's energy is split between
//            the coarse texels by its position (centroid kept), so a coarse level does not move it;
//  2 blur:   separable Gaussian, sigma in tangent units (qSig0..2) times the level's pixels per tangent (from the
//            periphery's own projection), so the halo has the same angular size in every view and in both eyes;
//  3 streak: 6 spikes (3 lines at 15 / 75 / 135 deg) from the blurred 1/8 level (so a spike's width is angular,
//            sigma 0.35 x radius, not a texel), exponential fall-off over qStreakLen (tangent units),
//            energy-normalised.
// Allocation: level 0 is 640 x 640 (a periphery up to 2560 px), level 1 320, level 2 160; level l texel i covers
// periphery pixels [i, i + 1) x 4 x 2^l, so a periphery uv maps to uv x W / 2560 in every level.
inline const char kGlareHlsl[] = R"(
cbuffer cbQvColor : register(b0) {
  float qSrgb, qOwnCurve, qHueKeep, qChromaNorm;
  float qSlope, qToe, qShoulder, qWhiteClip;
  float qWhiteStart, qKeepSat, qSaturation, qDesatAmt;
  float3 qWarm; float qFloorLum;
  float qEvOffset, qDither, qFrame, qUseState;
  float qDt, qTauBright, qTauDark, qMeterFocus;
  float qEqualizeEc, qReset, qHueShadow, qPad1;
  float qNvW, qNvTintR, qNvTintG, qNvTintB;
  float qNvBlur, qNvConeLo, qNvConeHi, qGlareOn;
  float qGlareStrength, qGlareT, qGlareCap, qGlareStreak;
  float qSig0, qSig1, qSig2, qStreakLen;
  float qW0, qW1, qW2, qQuad;
  float qDebug, qAuxOn, qPad2, qPad3;
};
cbuffer cbTonemapParams : register(b1) {
  float dcExposureKey, dcSigmaKey, dcTau, dcExposureCorrection;
  float bloomThreshold, bloomLerpFactor, whiteBalanceFactor, vignetteFactor;
  float4 dcViewport;
  uint2 dcDims; float tmExp; float tmPower;
  float sceneLuminanceMin, sceneLuminanceMax, focusWidth, focusSigma;
};
cbuffer cbQvDispatch : register(b2) { uint qEye, qLevel, qDir, qDispPad; };
Texture2D<float4> src : register(t0);
StructuredBuffer<float2> avgLuminance : register(t1);
RWByteAddressBuffer st : register(u0);
RWTexture2D<float4> dst : register(u1);
RWTexture2D<float4> dstLow : register(u2);
SamplerState lin : register(s0);
static const float3 REC = float3(0.2126, 0.7152, 0.0722);
float PRaw(uint v, uint i) { return asfloat(st.Load(256 + v * 64 + i * 4)); }
bool ProjOk(uint v, out float4 q) {
  float p00 = PRaw(v, 0), p11 = PRaw(v, 5), p23 = PRaw(v, 14), p33 = PRaw(v, 15);
  float p03 = PRaw(v, 12), p13 = PRaw(v, 13), p10 = PRaw(v, 1), p01 = PRaw(v, 4);
  q = float4(p00, p11, PRaw(v, 2) / p23, PRaw(v, 6) / p23);
  return abs(p23) > 1e-6 && abs(p33) <= 1e-4 * abs(p23) && p00 > 0.1 && p00 < 50 && abs(p11) > 0.1 && abs(p11) < 50 &&
         abs(p03) <= 1e-4 && abs(p13) <= 1e-4 && abs(p10) <= 0.02 * p00 && abs(p01) <= 0.02 * abs(p11) &&
         abs(q.z) < 4 && abs(q.w) < 4;
}
uint2 PeriSize() { return uint2(asfloat(st.Load(72 + qEye * 8)), asfloat(st.Load(76 + qEye * 8))); }
uint2 LevelSize(uint l) {
  uint2 s = (PeriSize() + 3) / 4;
  [unroll] for (uint k = 0; k < 2; ++k) if (k < l) s = (s + 1) / 2;
  return max(s, uint2(1, 1));
}
// Pixels per tangent unit of level l (x, y) from the periphery projection.
float2 PxPerTan(uint l) {
  float4 q;
  ProjOk(qEye, q);
  return float2(q.x, abs(q.y)) * float2(PeriSize()) * 0.5 / float(4u << l);
}
#if KERNEL == 0
float3 Bright(float3 c) {
  float e = dot(c, REC);
  if (!(e > 0)) return 0;
  float k = 0.5 * qGlareT;
  float s = clamp(e - qGlareT + k, 0, 2 * k);
  float f = max(s * s / (4 * k), e - qGlareT) / e;
  if (e > qGlareCap) f *= qGlareCap / e;
  return c * f;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  src.GetDimensions(w, h);
  float adapted = avgLuminance[1].x;
  if (qUseState > 0 && st.Load(4) == 1) adapted = asfloat(st.Load(0));
  float ec = dcExposureCorrection;
  if (qEqualizeEc > 0 && asfloat(st.Load(32)) > 0) ec = asfloat(st.Load(44));
  float floorL = qFloorLum > 0 ? qFloorLum : sceneLuminanceMin;
  float ex = exp2(log2(0.18 / clamp(adapted, floorL, sceneLuminanceMax)) + ec + qEvOffset);
  if (id.x == 0 && id.y == 0) {
    float4 q;
    st.Store(64 + qEye * 4, asuint(ProjOk(qEye, q) ? qFrame : -1.0));
    st.Store(72 + qEye * 8, asuint(float(w)));
    st.Store(76 + qEye * 8, asuint(float(h)));
  }
  uint2 o = uint2((w + 3) / 4, (h + 3) / 4);
  if (id.x >= o.x || id.y >= o.y) return;
  float3 b = 0, n = 0;
  [unroll] for (uint j = 0; j < 4; ++j)
  [unroll] for (uint i = 0; i < 4; ++i) {
    float3 c = src.Load(int3(min(id.xy * 4 + uint2(i, j), uint2(w - 1, h - 1)), 0)).rgb * ex;
    c = (c.r >= 0 && c.g >= 0 && c.b >= 0 && c.r < 1e30 && c.g < 1e30 && c.b < 1e30) ? c : 0;
    n += c;
    b += Bright(c);
  }
  dst[id.xy] = float4(b * (1.0 / 16), 1);
  dstLow[id.xy] = float4(n * (1.0 / 16), 1);
}
#elif KERNEL == 1
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 o = LevelSize(qLevel), s = LevelSize(qLevel - 1);
  if (id.x >= o.x || id.y >= o.y) return;
  const float wt[4] = {0.125, 0.375, 0.375, 0.125};
  float3 a = 0;
  [unroll] for (int j = 0; j < 4; ++j)
  [unroll] for (int i = 0; i < 4; ++i)
    a += src.Load(int3(clamp(int2(id.xy) * 2 + int2(i, j) - 1, int2(0, 0), int2(s) - 1), 0)).rgb * (wt[i] * wt[j]);
  dst[id.xy] = float4(a, 1);
}
#elif KERNEL == 2
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 o = LevelSize(qLevel);
  if (id.x >= o.x || id.y >= o.y) return;
  float sig = (qLevel == 0 ? qSig0 : qLevel == 1 ? qSig1 : qSig2) * PxPerTan(qLevel)[qDir];
  // The pyramid (4 x 4 box, tent downsamples) and the bilinear read-back already blur by about half a texel
  // (variance 0.25 / 0.37 / 0.41 texel^2 at levels 0 / 1 / 2): only the rest is added, so the PSF has the same
  // angular size at any resolution.
  sig = sqrt(max(sig * sig - (qLevel == 0 ? 0.25 : qLevel == 1 ? 0.37 : 0.41), 0));
  if (!(sig >= 0.3)) {
    dst[id.xy] = src.Load(int3(id.xy, 0));
    return;
  }
  int r = min((int)ceil(4 * sig), 32);
  int2 d = qDir == 0 ? int2(1, 0) : int2(0, 1);
  float3 a = 0;
  float ws = 0, k2 = -0.5 / (sig * sig);
  for (int k = -r; k <= r; ++k) {
    float wk = exp(k * k * k2);
    int2 p = clamp(int2(id.xy) + d * k, int2(0, 0), int2(o) - 1);
    a += src.Load(int3(p, 0)).rgb * wk;
    ws += wk;
  }
  dst[id.xy] = float4(a / ws, 1);
}
#else
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 o = LevelSize(1);
  if (id.x >= o.x || id.y >= o.y) return;
  float2 ppt = PxPerTan(1);
  float2 hi = (float2(o) - 0.5) / 320.0, lo = 0.5 / 320.0;
  float2 c = (float2(id.xy) + 0.5) / 320.0;
  float3 a = 0;
  float ws = 0;
  [unroll] for (uint dd = 0; dd < 3; ++dd) {
    float an = radians(15.0 + 60.0 * dd);
    float2 step = float2(cos(an), -sin(an)) * qStreakLen * ppt / (12.0 * 320.0);
    [unroll] for (uint k = 1; k <= 12; ++k) {
      float wk = exp(-4.0 * k / 12.0);
      a += (src.SampleLevel(lin, clamp(c + step * k, lo, hi), 0).rgb +
            src.SampleLevel(lin, clamp(c - step * k, lo, hi), 0).rgb) * wk;
      ws += 2 * wk;
    }
  }
  dst[id.xy] = float4(a / ws, 1);
}
#endif
)";

// ---------------------------------------------------------------------------
// Compilation (any thread; d3dcompiler_47 from System32 as fixation_depth.h)
// ---------------------------------------------------------------------------
struct Blobs {
  std::vector<uint8_t> psFx, prep;  // R51: the tone map with the VrFx block; the DoF prep CS
  std::vector<uint8_t> psFxStock;   // R57: psFx with vrfx::StockCb's values as constants (Mode 0 + effects)
  std::vector<uint8_t> ps, meterPeri, meterFocus, adapt;
  std::vector<uint8_t> glare[4];  // R50: down, half, blur, streak
  std::vector<uint8_t> psSym[3];  // R95: ps, psFx, psFxStock with QV_SYM (CompileSym; only while the export is wanted)
};
// R51: the tone-map source with the VrFx block at its marker (compiled with VRFX=1).
inline std::string PsFxSource() {
  std::string s(kPsHlsl);
  const std::string m = "// @VRFX_BLOCK@";
  const size_t k = s.find(m);
  if (k != std::string::npos) s.replace(k, m.size(), vrfx::PsBlock());
  return s;
}
inline bool CompileOne(pD3DCompile compile, const char* src, const char* target, const D3D_SHADER_MACRO* m,
                       std::vector<uint8_t>& out, std::string& err) {
  ID3DBlob *code = nullptr, *e = nullptr;
  const HRESULT hr = compile(src, strlen(src), "qv_color", m, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                             &code, &e);
  if (FAILED(hr) || !code) {
    err = e ? std::string(static_cast<const char*>(e->GetBufferPointer()), e->GetBufferSize()) : "D3DCompile failed";
    if (e) e->Release();
    if (code) code->Release();
    return false;
  }
  const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
  out.assign(p, p + code->GetBufferSize());
  code->Release();
  if (e) e->Release();
  return true;
}
inline bool CompileAll(Blobs& b, std::string& err) {
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  HMODULE m = LoadLibraryW((std::wstring(sys) + L"\\d3dcompiler_47.dll").c_str());
  const pD3DCompile compile = m ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
  if (!compile) {
    err = "d3dcompiler_47.dll / D3DCompile not available";
    return false;
  }
  const D3D_SHADER_MACRO peri[] = {{"CENTER", "0"}, {"SLOT", "16"}, {nullptr, nullptr}};
  const D3D_SHADER_MACRO foc[] = {{"CENTER", "1"}, {"SLOT", "20"}, {nullptr, nullptr}};
  const D3D_SHADER_MACRO gk[4][2] = {{{"KERNEL", "0"}, {nullptr, nullptr}}, {{"KERNEL", "1"}, {nullptr, nullptr}},
                                     {{"KERNEL", "2"}, {nullptr, nullptr}}, {{"KERNEL", "3"}, {nullptr, nullptr}}};
  const D3D_SHADER_MACRO fx[] = {{"VRFX", "1"}, {nullptr, nullptr}};
  const D3D_SHADER_MACRO fxStock[] = {{"VRFX", "1"}, {"QV_STOCK", "1"}, {nullptr, nullptr}};
  const std::string psFx = PsFxSource(), prep = vrfx::PrepSource();
  return CompileOne(compile, kPsHlsl, "ps_5_0", nullptr, b.ps, err) &&
         CompileOne(compile, kMeterHlsl, "cs_5_0", peri, b.meterPeri, err) &&
         CompileOne(compile, kMeterHlsl, "cs_5_0", foc, b.meterFocus, err) &&
         CompileOne(compile, kAdaptHlsl, "cs_5_0", nullptr, b.adapt, err) &&
         CompileOne(compile, kGlareHlsl, "cs_5_0", gk[0], b.glare[0], err) &&
         CompileOne(compile, kGlareHlsl, "cs_5_0", gk[1], b.glare[1], err) &&
         CompileOne(compile, kGlareHlsl, "cs_5_0", gk[2], b.glare[2], err) &&
         CompileOne(compile, kGlareHlsl, "cs_5_0", gk[3], b.glare[3], err) &&
         CompileOne(compile, psFx.c_str(), "ps_5_0", fx, b.psFx, err) &&
         CompileOne(compile, psFx.c_str(), "ps_5_0", fxStock, b.psFxStock, err) &&
         CompileOne(compile, prep.c_str(), "cs_5_0", nullptr, b.prep, err);
}
// R95: the symbology export's tone maps (QV_SYM: the second evaluation, the emission atlas UAV), [0] ps, [1] psFx,
// [2] psFxStock; test: QV_SYM_TEST too (C's bytes to u2: the tests' check of the identity, never in DCS).
inline bool CompileSym(Blobs& b, std::string& err, bool test = false) {
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  HMODULE m = LoadLibraryW((std::wstring(sys) + L"\\d3dcompiler_47.dll").c_str());
  const pD3DCompile compile = m ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
  if (!compile) {
    err = "d3dcompiler_47.dll / D3DCompile not available";
    return false;
  }
  const char* t = test ? "1" : "0";
  const D3D_SHADER_MACRO base[] = {{"QV_SYM", "1"}, {"QV_SYM_TEST", t}, {nullptr, nullptr}};
  const D3D_SHADER_MACRO fx[] = {{"VRFX", "1"}, {"QV_SYM", "1"}, {"QV_SYM_TEST", t}, {nullptr, nullptr}};
  const D3D_SHADER_MACRO fxStock[] = {{"VRFX", "1"}, {"QV_STOCK", "1"}, {"QV_SYM", "1"}, {"QV_SYM_TEST", t},
                                      {nullptr, nullptr}};
  const std::string psFx = PsFxSource();
  return CompileOne(compile, kPsHlsl, "ps_5_0", base, b.psSym[0], err) &&
         CompileOne(compile, psFx.c_str(), "ps_5_0", fx, b.psSym[1], err) &&
         CompileOne(compile, psFx.c_str(), "ps_5_0", fxStock, b.psSym[2], err);
}

// ---------------------------------------------------------------------------
// D3D objects (created on any thread from the device; used on the render thread)
// ---------------------------------------------------------------------------
constexpr int kQSets = 4;
struct QSet {
  ID3D11Query* disjoint = nullptr;
  ID3D11Query* ts[8] = {};
  unsigned mask = 0;  // views with both stamps issued
  bool open = false, issued = false;
};
struct Objects {
  ID3D11Device* dev = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11ComputeShader* meter[2] = {};
  ID3D11ComputeShader* adapt = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11Buffer* state = nullptr;
  ID3D11ShaderResourceView* stateSrv = nullptr;
  ID3D11UnorderedAccessView* stateUav = nullptr;
  ID3D11Buffer* stStage[2] = {};
  ID3D11Buffer* rsStage[2] = {};  // R81: the reset log's readback (state bytes 0..31 before the reset / at the snap)
  QSet q[kQSets];
  // R50: the glare / low-pass chain. Per eye: level 0 glare and low-pass, level 1 glare and streak, level 2 glare
  // (index kTx*); temporaries for the separable blur (shared by the eyes). SRV + UAV each.
  enum { kTxG0, kTxLow, kTxG1, kTxStreak, kTxG2, kTxPerEye };
  ID3D11Texture2D* tex[2][kTxPerEye] = {};
  ID3D11ShaderResourceView* texSrv[2][kTxPerEye] = {};
  ID3D11UnorderedAccessView* texUav[2][kTxPerEye] = {};
  ID3D11Texture2D* tmp[3] = {};
  ID3D11ShaderResourceView* tmpSrv[3] = {};
  ID3D11UnorderedAccessView* tmpUav[3] = {};
  ID3D11ComputeShader* glare[4] = {};
  ID3D11SamplerState* linClamp = nullptr;
  ID3D11Buffer* viewCb[5] = {};     // PS b12: uint view 0..4
  ID3D11Buffer* dispCb[2][8] = {};  // CS b2: (eye, level, dir) at [e][level * 2 + dir]; [e][6] down / streak
  static UINT LevelAlloc(int level) { return kGlareAlloc >> level; }
  // R51 VrFx: the tone map with the effects, the DoF prep, the per-view constants; the prep targets are sized on the
  // render thread for the largest view seen (EnsureFxTargets).
  ID3D11PixelShader* psFx = nullptr;
  ID3D11PixelShader* psFxStock = nullptr;  // R57: Mode 0 + effects
  ID3D11PixelShader* psSym[3] = {};  // R95: ps / psFx / psFxStock with QV_SYM (made by Tick; g_symPsReady)
  ID3D11ComputeShader* prep = nullptr;
  ID3D11Buffer* fxCb = nullptr;
  ID3D11Texture2D* prepTex = nullptr;
  ID3D11UnorderedAccessView* prepUav = nullptr;
  ID3D11ShaderResourceView* prepSrv = nullptr;
  ID3D11Texture2D* tileTex = nullptr;
  ID3D11UnorderedAccessView* tileUav = nullptr;
  ID3D11ShaderResourceView* tileSrv = nullptr;
  UINT prepW = 0, prepH = 0, tileW = 0, tileH = 0;
  bool fxTargetsFailed = false;
  bool EnsureFxTargets(UINT hw, UINT hh, UINT tw, UINT th) {
    if (prepTex && hw <= prepW && hh <= prepH && tw <= tileW && th <= tileH) return true;
    if (fxTargetsFailed || !dev) return false;
    // R58: grow only, per dimension. Sized to the requesting view alone, a periphery taller and a focus wider than
    // the other recreated all four targets twice per frame on the render thread (the PS and the prep use the view's
    // own halfW/H and tilesX/Y, so a larger target is fine).
    if (prepTex) hw = std::max(hw, prepW), hh = std::max(hh, prepH), tw = std::max(tw, tileW), th = std::max(th, tileH);
    ReleaseFxTargets();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = hw, d.Height = hh, d.MipLevels = 1, d.ArraySize = 1, d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1, d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    D3D11_TEXTURE2D_DESC t = d;
    t.Width = tw, t.Height = th, t.Format = DXGI_FORMAT_R32_FLOAT;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &prepTex)) || FAILED(dev->CreateUnorderedAccessView(prepTex, nullptr, &prepUav)) ||
        FAILED(dev->CreateShaderResourceView(prepTex, nullptr, &prepSrv)) || FAILED(dev->CreateTexture2D(&t, nullptr, &tileTex)) ||
        FAILED(dev->CreateUnorderedAccessView(tileTex, nullptr, &tileUav)) ||
        FAILED(dev->CreateShaderResourceView(tileTex, nullptr, &tileSrv))) {
      ReleaseFxTargets();
      fxTargetsFailed = true;
      return false;
    }
    prepW = hw, prepH = hh, tileW = tw, tileH = th;
    return true;
  }
  void ReleaseFxTargets() {
    auto rel = [](auto*& p) {
      if (p) p->Release();
      p = nullptr;
    };
    rel(prepSrv), rel(prepUav), rel(prepTex), rel(tileSrv), rel(tileUav), rel(tileTex);
    prepW = prepH = tileW = tileH = 0;
  }
  bool Create(ID3D11Device* d, const Blobs& b, std::string& err) {
    Release();
    d->AddRef();
    dev = d;
    auto fail = [&](const char* what) {
      err = what;
      Release();
      return false;
    };
    if (FAILED(d->CreatePixelShader(b.ps.data(), b.ps.size(), nullptr, &ps))) return fail("pixel shader");
    if (FAILED(d->CreateComputeShader(b.meterPeri.data(), b.meterPeri.size(), nullptr, &meter[0])) ||
        FAILED(d->CreateComputeShader(b.meterFocus.data(), b.meterFocus.size(), nullptr, &meter[1])) ||
        FAILED(d->CreateComputeShader(b.adapt.data(), b.adapt.size(), nullptr, &adapt)))
      return fail("compute shaders");
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = sizeof(cgrade::ColorCb);
    cd.Usage = D3D11_USAGE_DYNAMIC;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(d->CreateBuffer(&cd, nullptr, &cb))) return fail("constant buffer");
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = kStateBytes;
    sd.Usage = D3D11_USAGE_DEFAULT;
    sd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    sd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    const uint8_t zero[kStateBytes] = {};
    D3D11_SUBRESOURCE_DATA init{zero, 0, 0};
    if (FAILED(d->CreateBuffer(&sd, &init, &state))) return fail("state buffer");
    D3D11_SHADER_RESOURCE_VIEW_DESC rv{};
    rv.Format = DXGI_FORMAT_R32_TYPELESS;
    rv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    rv.BufferEx.NumElements = kStateBytes / 4;
    rv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    if (FAILED(d->CreateShaderResourceView(state, &rv, &stateSrv))) return fail("state SRV");
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = DXGI_FORMAT_R32_TYPELESS;
    uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uv.Buffer.NumElements = kStateBytes / 4;
    uv.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(d->CreateUnorderedAccessView(state, &uv, &stateUav))) return fail("state UAV");
    D3D11_BUFFER_DESC rd{};
    rd.ByteWidth = kStateBytes;
    rd.Usage = D3D11_USAGE_STAGING;
    rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& s : stStage)
      if (FAILED(d->CreateBuffer(&rd, nullptr, &s))) return fail("state readback");
    rd.ByteWidth = kRsBytes;
    for (auto& s : rsStage)
      if (FAILED(d->CreateBuffer(&rd, nullptr, &s))) return fail("reset readback");
    if (FAILED(d->CreatePixelShader(b.psFx.data(), b.psFx.size(), nullptr, &psFx)) ||
        FAILED(d->CreatePixelShader(b.psFxStock.data(), b.psFxStock.size(), nullptr, &psFxStock)) ||
        FAILED(d->CreateComputeShader(b.prep.data(), b.prep.size(), nullptr, &prep)))
      return fail("VrFx shaders");
    D3D11_BUFFER_DESC fd{};
    fd.ByteWidth = sizeof(vrfx::FxCb);
    fd.Usage = D3D11_USAGE_DYNAMIC;
    fd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    fd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(d->CreateBuffer(&fd, nullptr, &fxCb))) return fail("VrFx constant buffer");
    for (QSet& s : q) {
      D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
      if (FAILED(d->CreateQuery(&qd, &s.disjoint))) return fail("queries");
      qd.Query = D3D11_QUERY_TIMESTAMP;
      for (auto& t : s.ts)
        if (FAILED(d->CreateQuery(&qd, &t))) return fail("queries");
    }
    // R50 glare / night-vision objects (about 15 MB of VRAM; used only while Glare or NightVisionBlur is on)
    for (int k = 0; k < 4; ++k)
      if (FAILED(d->CreateComputeShader(b.glare[k].data(), b.glare[k].size(), nullptr, &glare[k])))
        return fail("glare compute shaders");
    auto mk = [&](UINT size, ID3D11Texture2D** t, ID3D11ShaderResourceView** s, ID3D11UnorderedAccessView** u) {
      D3D11_TEXTURE2D_DESC td{};
      td.Width = td.Height = size, td.MipLevels = 1, td.ArraySize = 1, td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
      // zeros: a bilinear tap with weight 0 just outside the used part must never read a NaN
      const std::vector<uint8_t> zero(static_cast<size_t>(size) * size * 8, 0);
      D3D11_SUBRESOURCE_DATA zd{zero.data(), size * 8, 0};
      return SUCCEEDED(d->CreateTexture2D(&td, &zd, t)) && SUCCEEDED(d->CreateShaderResourceView(*t, nullptr, s)) &&
             SUCCEEDED(d->CreateUnorderedAccessView(*t, nullptr, u));
    };
    const int lvl[kTxPerEye] = {0, 0, 1, 1, 2};
    for (int e = 0; e < 2; ++e)
      for (int k = 0; k < kTxPerEye; ++k)
        if (!mk(LevelAlloc(lvl[k]), &tex[e][k], &texSrv[e][k], &texUav[e][k])) return fail("glare textures");
    for (int l = 0; l < 3; ++l)
      if (!mk(LevelAlloc(l), &tmp[l], &tmpSrv[l], &tmpUav[l])) return fail("glare textures");
    D3D11_SAMPLER_DESC sm{};
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    sm.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (FAILED(d->CreateSamplerState(&sm, &linClamp))) return fail("glare sampler");
    auto icb = [&](UINT a, UINT b2, UINT c2, ID3D11Buffer** out) {
      const UINT v[4] = {a, b2, c2, 0};
      D3D11_BUFFER_DESC bd{};
      bd.ByteWidth = 16, bd.Usage = D3D11_USAGE_IMMUTABLE, bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      D3D11_SUBRESOURCE_DATA sd2{v, 0, 0};
      return SUCCEEDED(d->CreateBuffer(&bd, &sd2, out));
    };
    for (UINT v = 0; v < 5; ++v)
      if (!icb(v, 0, 0, &viewCb[v])) return fail("view constants");
    for (UINT e = 0; e < 2; ++e)
      for (UINT k = 0; k < 8; ++k)
        if (!icb(e, k < 6 ? k / 2 : 1, k < 6 ? k % 2 : 0, &dispCb[e][k])) return fail("dispatch constants");
    return true;
  }
  void Release() {
    auto rel = [](auto*& p) {
      if (p) p->Release();
      p = nullptr;
    };
    rel(ps), rel(meter[0]), rel(meter[1]), rel(adapt), rel(cb), rel(stateSrv), rel(stateUav), rel(state);
    rel(stStage[0]), rel(stStage[1]), rel(rsStage[0]), rel(rsStage[1]);
    ReleaseFxTargets();
    rel(psFx), rel(psFxStock), rel(prep), rel(fxCb);
    for (auto*& p : psSym) rel(p);  // R95
    fxTargetsFailed = false;
    for (QSet& s : q) {
      rel(s.disjoint);
      for (auto& t : s.ts) rel(t);
      s = QSet{};
    }
    for (int e = 0; e < 2; ++e)
      for (int k = 0; k < kTxPerEye; ++k) rel(texUav[e][k]), rel(texSrv[e][k]), rel(tex[e][k]);
    for (int l = 0; l < 3; ++l) rel(tmpUav[l]), rel(tmpSrv[l]), rel(tmp[l]);
    for (auto& g : glare) rel(g);
    rel(linClamp);
    for (auto& v : viewCb) rel(v);
    for (auto& r : dispCb)
      for (auto& v : r) rel(v);
    rel(dev);
  }
};

// ---------------------------------------------------------------------------
// Shared state: the config thread writes, the render thread reads
// ---------------------------------------------------------------------------
inline std::atomic<int> g_mode{0};        // effective mode (ini [Color] Mode, or the hotkey's choice)
inline std::atomic<int> g_timing{0};      // [Color] Timing
inline std::atomic<int> g_fxStockShader{1};  // R57 (dev A/B, tests): 0 = Mode 0 + effects on the full psFx
inline std::atomic<int> g_verifyReq{0};
inline std::atomic<int> g_armRepair{3};   // R47 [Color] ArmRepair (dev A/B): 1 table normalize, 2 toggle points
   // [Color] VerifyProbe: frames to verify (dev)
inline std::mutex g_paramMutex;
inline cgrade::ModeParams g_params[3];    // [1] Fix, [2] Enhanced (index 0 unused)
inline std::atomic<uint32_t> g_paramVer{0};
inline std::atomic<bool> g_sunValid{false};
inline std::atomic<float> g_sunY{0.0f};
inline std::atomic<bool> g_fault{false};  // a fault in our part: off until the payload restarts
// R77: a mission load / slot change / simulation stop (main.cpp OnMissionEvent): the eye-like adaptation state
// belongs to the previous scene; the render thread resets it at its next tone map (as vrfx::g_fxResetGen).
inline std::atomic<uint32_t> g_adaptResetGen{0};
inline void NoteMissionEvent() { g_adaptResetGen.fetch_add(1, std::memory_order_acq_rel); }
// R81: a colour mode switch seen by the config thread (Tick). In Mode 0 without VrFx and timing, fixation depth is
// off and no tone map of ours runs, so the render thread never sees Mode 0 itself: this generation tells it that
// the mode was switched (Enhanced -> Off -> Enhanced resets even within the 1 s gap).
inline std::atomic<uint32_t> g_modeGen{0};
inline std::atomic<int> g_modeFrom{-1};  // R81: the mode before the config thread's last switch (the reset log)
// R81: why the adaptation state is reset (bits; the reset log line names them).
enum ResetWhy : uint32_t { kRsStart = 1, kRsMode = 2, kRsGap = 4, kRsMission = 8, kRsParams = 16 };
// R81: the last logged reset (the log line's values; tests read them).
struct ResetLog {
  uint32_t why = 0;
  int fromMode = -1, toMode = 0;
  double gapSec = 0;
  float before = 0, after = 0, target = 0, dcs = 0, peri = 0, focus = 0;
  uint32_t validBefore = 0, validAfter = 0;
};
inline ResetLog g_resetLog;                      // render thread writes before g_resetLogged++
inline std::atomic<uint32_t> g_resetLogged{0};   // reset log lines written
// R95: the symbology export (cockpit_mask.h's symbology.h) evaluates the tone map a second time in this draw. Its
// query (render thread, ToneDrawPre) gives the view's bindings when the view has E this frame; done reports the draw.
// With it the draw is ours in every mode: [Color] Mode 0 runs the stock replica (psFxStock, R57: DCS's output).
struct SymBind {
  ID3D11Buffer* cb = nullptr;                // PS b10: the window, the atlas origin, the alpha switch
  ID3D11ShaderResourceView* acc = nullptr;   // PS t25: the accumulated HDR change of the symbology draws (window)
  ID3D11UnorderedAccessView* out = nullptr;  // OM UAV 1: the emission atlas of this frame's slot
};
constexpr UINT kPsSymCbSlot = 10, kPsSymSrvSlot = 25, kPsSymUavSlot = 1;
inline std::atomic<bool> g_symWant{false};  // [Perception] SymbologyExport (with the mask export): our pass is wanted
inline std::atomic<int> g_symPsReady{0};    // the QV_SYM shaders exist in g_obj (1; stored after they are made)
inline bool (*g_symQuery)(int v, ID3D11Resource* t0, SymBind* out) = nullptr;  // set once before any draw hook
inline void (*g_symDone)(int v, bool written) = nullptr;
inline void (*g_symDrawBegin)(int v) = nullptr;  // R97: right before the draw with the second evaluation (its GPU time)
inline int64_t g_symBindQpc = 0;  // R97: render thread: QPC ticks binding / restoring our slots this frame (symbology.h)
inline ID3D11UnorderedAccessView* g_symTestC = nullptr;  // tests (QV_SYM_TEST shaders): C's bytes, OM UAV 2
inline std::atomic<uint64_t> g_symDraws{0}, g_symNoPs{0};
inline std::atomic<ID3D11Device*> g_devSeen{nullptr};  // DCS's device as seen on the render thread (AddRef'd)
inline std::atomic<Objects*> g_obj{nullptr};            // ready objects (owned by the config thread)
inline Objects g_objStore;

inline bool Wanted() {
  return (g_mode.load(std::memory_order_relaxed) != 0 || g_timing.load(std::memory_order_relaxed) != 0 || vrfx::Wanted() ||
          g_symWant.load(std::memory_order_relaxed)) &&
         !g_fault.load(std::memory_order_relaxed);
}

// Statistics (render thread adds, config thread reads and clears)
enum Reject { kRjNoObjects, kRjTarget, kRjSize, kRjUnknownSize, kRjHdr, kRjLum, kRjCb, kRjLut, kRjCount };
inline const char* const kRejectNames[kRjCount] = {"objects not ready", "target not R8G8B8A8_UNORM 2D", "size not the view's",
                                                   "view size not learnt yet", "t0 not an RGBA16F 2D texture of that size",
                                                   "t1 not the luminance buffer", "b0 not cbTonemapParams", "3D LUT bound"};
inline std::atomic<uint64_t> g_swaps[4] = {}, g_rejects[kRjCount] = {}, g_timedFrames{0}, g_lostSets{0};
// R46: HMD tone maps left unswapped while a mode was on (per view; [4] = view unknown), any cause (reject, arm
// refused, no draw reached): each one is a stock (grey) image in that view for a frame. Since the last log line /
// since start. And swaps of unknown-view tone maps (first partial frame after fixation depth installs).
inline std::atomic<uint64_t> g_miss[5] = {}, g_missTotal{0}, g_swapUnknown{0};
// R47: why (since the last 10 s line): kMissReject the bound-state check said no (its reason is in "not swapped"),
// kMissArm our draw arm was refused (another module holds the context table), kMissBypass armed but DCS's tone-map
// draw never reached our draw hook (d3d11 put its "after flush" entries into the table after the arm and nothing
// re-armed before the draw). Not a miss: a tone map while our objects are still being created (the first switch to a
// mode after the payload loads; the image changes about a second later, no dropout) -> g_missPending.
enum MissWhy { kMissReject, kMissArm, kMissBypass, kMissWhyCount };
inline std::atomic<uint64_t> g_missWhy[kMissWhyCount] = {}, g_missPending{0};
// R83 D1: the mode in effect for the frame of the latest miss (0: Mode 0 with VrFx), for the MISSED line (cfg.mode at
// log time can already be the next mode).
inline std::atomic<int> g_missMode{0};
inline std::atomic<double> g_gpuUs{0.0}, g_gpuViewUs[4] = {};
inline std::atomic<float> g_rbAdapted{0}, g_rbDcs{0}, g_rbTarget{0}, g_rbPeri{0}, g_rbFocus{0}, g_rbEc{0};
// R50: glare / night-vision low-pass runs per eye, skips (periphery too large for the allocation), projection
// copies per view (copied from VS / PS b7, or missing: zeros written, the view gets no glare); the read-back
// projections (P00, P11, ox, oy) and eye stamps for the 10 s log; debug output (dev / tests / preview).
inline std::atomic<uint64_t> g_glareRuns[2] = {}, g_glareTooBig{0}, g_projCopied[4] = {}, g_projMissing[4] = {};
inline std::atomic<float> g_rbProj[4][4] = {}, g_rbStamp[2] = {};
inline std::atomic<int> g_glareDebug{0};
inline std::atomic<bool> g_rbFresh{false};
inline std::atomic<float> g_rbFxLum{0};  // R51: DCS's adapted luminance copied at view 0 (pupil estimate)
inline std::atomic<bool> g_rbWant{false};

// ---------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------
// R46: the view an armed tone map belongs to when its frame's view count is not trustworthy (the first, partial
// frame after fixation depth installs: hmdOrd counts CopyToHMDPass ends since a frame start it never saw). Such a
// draw is swapped all the same (no grey frame), only the per-view extras (meters, adaptation, EC copy, timing) skip.
inline constexpr int kViewUnknown = 4;
inline constexpr int kMaxSizes = 8;
inline constexpr double kAdaptGapSec = 1.0;  // R77: a longer gap between our tone maps starts the adaptation anew
struct Rt {
  UINT ldrW[4] = {}, ldrH[4] = {};  // learnt from the views' CopyToHMDPass source (per view: for the log only)
  // R46: the HMD image sizes seen at any view's CopyToHMDPass. The tone-map check accepts any of them: a view index
  // that is wrong for a frame (the size flip of the R46 log: view 0 "learnt" a focus size) can no longer reject a
  // genuine HMD tone map. A non-HMD target (mirror) has none of these sizes.
  UINT knownW[kMaxSizes] = {}, knownH[kMaxSizes] = {};
  int knownN = 0, knownNext = 0;
  int learnFrames = 32;             // HMD views left to learn (CopyToHMDPass draw arms; 8 frames of 4 views)
  ULONGLONG lastRelearn = 0;
  int lastView = 99;
  uint32_t frame = 0;
  LARGE_INTEGER lastQpc{};
  bool frameCbDone = false;
  uint32_t paramVer = ~0u;
  cgrade::ModeParams p[3];
  int lastMode = -1;
  bool resetNext = true;
  uint32_t adaptGen = 0;  // R77: g_adaptResetGen at the last reset request
  uint32_t modeGen = 0;   // R81: g_modeGen at the last reset request
  // R81: the pending reset's reasons (with resetNext), and those of this frame's constant buffer (cbReset: built
  // with qReset; the adaptation dispatch at view 0 consumes it)
  uint32_t resetWhy = kRsStart;
  int resetFrom = -1;
  double resetGap = 0;
  bool cbReset = false;
  uint32_t cbWhy = 0;
  int cbFrom = -1;
  double cbGap = 0;
  // R81 reset log: 0 idle, 1 "before" copied at the reset dispatch (the next dispatch copies "after": the snap),
  // 2 both copied, read back (never waiting) a few frames later
  int rsPhase = 0, rsAge = 0, rsTo = 0;
  uint32_t rsWhy = 0;
  int rsFrom = -1;
  double rsGap = 0;
  // the swap in progress
  bool swapped = false;
  ID3D11PixelShader* oldPs = nullptr;
  ID3D11ClassInstance* oldInst[8] = {};
  UINT oldInstN = 0;
  ID3D11Buffer* oldCb = nullptr;
  ID3D11ShaderResourceView* oldSrv = nullptr;
  // R50: PS b12, t17..t21, s15 (ours during the swap)
  ID3D11Buffer* oldViewCb = nullptr;
  ID3D11ShaderResourceView* oldAux[kPsAuxN] = {};
  ID3D11SamplerState* oldSampler = nullptr;
  bool auxOn = false, glareOn = false, streakOn = false;  // this frame's constant buffer
  bool cbStock = false;  // R57: this frame's constant buffer is vrfx::StockCb (Mode 0 + effects, no debug)
  // R95: the symbology's slots saved around the swapped draw (symView >= 0: bound for that view)
  int symView = -1;
  ID3D11Buffer* oldSymCb = nullptr;
  ID3D11ShaderResourceView* oldSymSrv = nullptr;
  ID3D11UnorderedAccessView* oldSymUav[2] = {};
  // timing
  int qCur = -1;
  uint32_t qFrame = 0;
  // state readback
  int rbSlot = 0, rbPending = -1, rbAge = 0;
  // verify probe (dev)
  int verifyLeft = 0;
};
inline Rt g_rt;

inline bool NeedLearn() { return g_rt.learnFrames > 0; }
inline bool SizeKnown(UINT w, UINT h) {
  for (int i = 0; i < g_rt.knownN; ++i)
    if (g_rt.knownW[i] == w && g_rt.knownH[i] == h) return true;
  return false;
}
inline void AddSize(UINT w, UINT h) {
  if (!w || !h || SizeKnown(w, h)) return;
  const int i = g_rt.knownN < kMaxSizes ? g_rt.knownN++ : g_rt.knownNext;
  g_rt.knownNext = (i + 1) % kMaxSizes;
  g_rt.knownW[i] = w, g_rt.knownH[i] = h;
}
inline void NoteLdr(int v, UINT w, UINT h) {
  if (v < 0 || v > 3 || !w || !h) return;
  AddSize(w, h);
  if (g_rt.ldrW[v] != w || g_rt.ldrH[v] != h) {
    g_rt.ldrW[v] = w, g_rt.ldrH[v] = h;
    if (g_log) g_log("colour: HMD view %d image %ux%u (its CopyToHMDPass source)", v, w, h);
  }
}
// At a CopyToHMDPass end (render thread): one learning view less.
inline void HmdEnd(int v) {
  if (v >= 0 && v <= 3 && g_rt.learnFrames > 0) --g_rt.learnFrames;
}
// R46 miss accounting, one HMD ToneMappingPass at a time (render thread): TonePassBegin at its begin (whether or not
// our draw arm took), ToneDrawPre marks it swapped, TonePassEnd counts a miss when a mode was on and it was not.
// R83 D1: when ToneDrawPre is reached it decides again whether the pass was due, from the state it swaps by (the
// frame's FxStep has run, vrfx::Wanted and the mode read once). TonePassBegin's decision uses the previous frame's
// effect state, so it stands only for a pass whose draw never reached the hook (arm refused / bypassed).
struct TonePass {
  bool open = false, modeOn = false, swapped = false;
  bool armed = false, reached = false;  // R47: our draw arm took; ToneDrawPre ran for this pass
  int v = -1;
  int mode = 0;  // R83: the mode the decision was made with (logged with a miss)
};
inline TonePass g_tp;
inline bool FxActive();  // R51, below
inline void TonePassBegin(int v) {
  g_tp.open = true;
  g_tp.v = v < 0 || v > kViewUnknown ? kViewUnknown : v;
  g_tp.mode = g_mode.load(std::memory_order_relaxed);
  g_tp.modeOn = (g_tp.mode != 0 || (vrfx::Wanted() && FxActive())) && !g_fault.load(std::memory_order_relaxed);
  g_tp.swapped = false;
  g_tp.armed = g_tp.reached = false;
}
// R47: whether the pass's draw arm took (ColorBoundary, right after TonePassBegin).
inline void TonePassArmed(bool armed) {
  if (g_tp.open) g_tp.armed = armed;
}
inline void TonePassEnd() {
  if (!g_tp.open) return;
  g_tp.open = false;
  if (g_tp.modeOn && !g_tp.swapped && !g_fault.load(std::memory_order_relaxed)) {
    if (!g_obj.load(std::memory_order_acquire)) {  // the first switch to a mode: objects being created, not a dropout
      g_missPending++;
      return;
    }
    g_miss[g_tp.v]++;
    g_missMode = g_tp.mode;  // R83: before the total (the config thread logs when the total changes)
    g_missTotal++;
    g_missWhy[g_tp.reached ? kMissReject : g_tp.armed ? kMissBypass : kMissArm]++;
  }
}
// The device the render thread sees; the config thread creates our objects for it.
inline void NoteDevice(ID3D11Device* dev) {
  if (!dev || g_devSeen.load(std::memory_order_relaxed) == dev) return;
  dev->AddRef();
  if (ID3D11Device* old = g_devSeen.exchange(dev)) old->Release();
}

struct Bound {
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11ShaderResourceView* srv[16] = {};
  ID3D11Buffer* b0 = nullptr;
  D3D11_TEXTURE2D_DESC rtd{}, t0d{};
  ID3D11Resource* rtRes = nullptr;
  ID3D11Resource* t0Res = nullptr;
  void Release() {
    if (rtv) rtv->Release();
    for (auto& s : srv)
      if (s) s->Release(), s = nullptr;
    if (b0) b0->Release();
    if (rtRes) rtRes->Release();
    if (t0Res) t0Res->Release();
    rtv = nullptr, b0 = nullptr, rtRes = t0Res = nullptr;
  }
};
// The tone-map draw's bound state; kRjCount when it is DCS's HMD tone map of view v.
inline int CheckBound(ID3D11DeviceContext* c, int v, Bound& b) {
  c->OMGetRenderTargets(1, &b.rtv, nullptr);
  if (!b.rtv) return kRjTarget;
  D3D11_RENDER_TARGET_VIEW_DESC rd{};
  b.rtv->GetDesc(&rd);
  if (rd.Format != DXGI_FORMAT_R8G8B8A8_UNORM || rd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || rd.Texture2D.MipSlice)
    return kRjTarget;
  b.rtv->GetResource(&b.rtRes);
  D3D11_RESOURCE_DIMENSION dim{};
  if (!b.rtRes) return kRjTarget;
  b.rtRes->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return kRjTarget;
  static_cast<ID3D11Texture2D*>(b.rtRes)->GetDesc(&b.rtd);
  if (b.rtd.SampleDesc.Count != 1) return kRjTarget;
  (void)v;
  if (!g_rt.knownN) return kRjUnknownSize;
  if (!SizeKnown(b.rtd.Width, b.rtd.Height)) return kRjSize;
  c->PSGetShaderResources(0, 16, b.srv);
  for (int i = 0; i < 16; ++i)
    if (b.srv[i]) {
      D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
      b.srv[i]->GetDesc(&sd);
      if (sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE3D) return kRjLut;
    }
  if (!b.srv[0]) return kRjHdr;
  {
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    b.srv[0]->GetDesc(&sd);
    if (sd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || sd.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        sd.Texture2D.MostDetailedMip)
      return kRjHdr;
    b.srv[0]->GetResource(&b.t0Res);
    if (!b.t0Res) return kRjHdr;
    b.t0Res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return kRjHdr;
    static_cast<ID3D11Texture2D*>(b.t0Res)->GetDesc(&b.t0d);
    if (b.t0d.Width != b.rtd.Width || b.t0d.Height != b.rtd.Height || b.t0d.SampleDesc.Count != 1) return kRjHdr;
  }
  if (!b.srv[1]) return kRjLum;
  {
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    b.srv[1]->GetDesc(&sd);
    if (sd.ViewDimension != D3D11_SRV_DIMENSION_BUFFER && sd.ViewDimension != D3D11_SRV_DIMENSION_BUFFEREX) return kRjLum;
    ID3D11Resource* r = nullptr;
    b.srv[1]->GetResource(&r);
    if (!r) return kRjLum;
    D3D11_BUFFER_DESC bd{};
    r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) static_cast<ID3D11Buffer*>(r)->GetDesc(&bd);
    r->Release();
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER || bd.StructureByteStride != 8 || bd.ByteWidth < 16) return kRjLum;
  }
  c->PSGetConstantBuffers(0, 1, &b.b0);
  if (!b.b0) return kRjCb;
  D3D11_BUFFER_DESC bd{};
  b.b0->GetDesc(&bd);
  if (bd.ByteWidth < 160) return kRjCb;
  return kRjCount;
}

// The current parameters (render thread copy, refreshed on a version change; the lock is only contended for the
// microseconds a config write takes).
inline const cgrade::ModeParams& CurParams(int mode) {
  const uint32_t v = g_paramVer.load(std::memory_order_acquire);
  if (v != g_rt.paramVer) {
    std::lock_guard<std::mutex> lock(g_paramMutex);
    for (int i = 0; i < 3; ++i) g_rt.p[i] = g_params[i];
    g_rt.paramVer = v;
    g_rt.resetNext = true;
    g_rt.resetWhy |= kRsParams;
  }
  return g_rt.p[mode == 2 ? 2 : 1];
}

inline void Timestamp(ID3D11DeviceContext* c, Objects* o, int v, bool end) {
  if (g_rt.qCur < 0 || v < 0 || v > 3) return;
  QSet& s = o->q[g_rt.qCur];
  if (!s.open) return;
  c->End(s.ts[v * 2 + (end ? 1 : 0)]);
  if (end) s.mask |= 1u << v;
}
// Collects finished sets (never waits) and opens this frame's set.
inline void TimingFrameStart(ID3D11DeviceContext* c, Objects* o) {
  for (int i = 0; i < kQSets; ++i) {
    QSet& s = o->q[i];
    if (s.open) {  // the previous frame's set: no view 3 came
      c->End(s.disjoint);
      s.open = false;
      s.issued = true;
    }
  }
  const int next = static_cast<int>(++g_rt.qFrame % kQSets);
  QSet& n = o->q[next];
  if (n.issued) {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    bool ok = c->GetData(n.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !dj.Disjoint &&
              dj.Frequency;
    double sum = 0, per[4] = {};
    for (int v = 0; ok && v < 4; ++v) {
      if (!(n.mask & (1u << v))) continue;
      UINT64 a = 0, b = 0;
      ok = c->GetData(n.ts[v * 2], &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
           c->GetData(n.ts[v * 2 + 1], &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && b >= a;
      per[v] = ok ? (b - a) * 1e6 / static_cast<double>(dj.Frequency) : 0;
      sum += per[v];
    }
    if (ok) {
      g_gpuUs.store(g_gpuUs.load() + sum);
      for (int v = 0; v < 4; ++v) g_gpuViewUs[v].store(g_gpuViewUs[v].load() + per[v]);
      g_timedFrames++;
    } else {
      g_lostSets++;
    }
    n.issued = false;
  }
  n.mask = 0;
  c->Begin(n.disjoint);
  n.open = true;
  g_rt.qCur = next;
}

// State readback for the log (render thread; never waits).
inline void ReadbackStep(ID3D11DeviceContext* c, Objects* o) {
  if (g_rt.rbPending >= 0 && ++g_rt.rbAge >= 3) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (c->Map(o->stStage[g_rt.rbPending], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) == S_OK) {
      const float* f = static_cast<const float*>(m.pData);
      g_rbAdapted = f[0], g_rbDcs = f[2], g_rbTarget = f[3], g_rbPeri = f[4], g_rbFocus = f[5], g_rbEc = f[11];
      g_rbStamp[0] = f[kStStamp / 4], g_rbStamp[1] = f[kStStamp / 4 + 1];
      g_rbFxLum = f[vrfx::kStFxLum / 4];
      for (int v = 0; v < 4; ++v) {  // logical P[r][c] at raw [c * 4 + r] (column-major cbuffer packing)
        const float* q = f + (kStProj + v * 64) / 4;
        g_rbProj[v][0] = q[0], g_rbProj[v][1] = q[5];
        g_rbProj[v][2] = q[14] != 0 ? q[2] / q[14] : 0.0f, g_rbProj[v][3] = q[14] != 0 ? q[6] / q[14] : 0.0f;
      }
      c->Unmap(o->stStage[g_rt.rbPending], 0);
      g_rbFresh = true;
      g_rt.rbPending = -1;
    } else if (g_rt.rbAge > 30) {
      g_rt.rbPending = -1;
    }
  }
  if (g_rt.rbPending < 0 && g_rbWant.exchange(false)) {
    g_rt.rbSlot ^= 1;
    c->CopyResource(o->stStage[g_rt.rbSlot], o->state);
    g_rt.rbPending = g_rt.rbSlot;
    g_rt.rbAge = 0;
  }
}

// R81: the adaptation reset log (render thread, at a new frame; never waits): the state before the reset dispatch
// and right after the next one (the snap), read back a few frames later. One line per reset, so a live check sees
// the reset itself (reason, adapted before -> after, target) instead of judging it from the mirror's brightness.
inline void ResetLogStep(ID3D11DeviceContext* c, Objects* o) {
  if (g_rt.rsPhase != 2 || ++g_rt.rsAge < 3) return;
  D3D11_MAPPED_SUBRESOURCE m0{}, m1{};
  const bool ok0 = c->Map(o->rsStage[0], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m0) == S_OK;
  const bool ok1 = ok0 && c->Map(o->rsStage[1], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m1) == S_OK;
  ResetLog& L = g_resetLog;
  L.why = g_rt.rsWhy, L.fromMode = g_rt.rsFrom, L.toMode = g_rt.rsTo, L.gapSec = g_rt.rsGap;
  if (ok1) {
    const float* b = static_cast<const float*>(m0.pData);
    const float* a = static_cast<const float*>(m1.pData);
    L.before = b[kStAdapted / 4], L.validBefore = reinterpret_cast<const uint32_t*>(b)[kStValid / 4];
    L.after = a[kStAdapted / 4], L.validAfter = reinterpret_cast<const uint32_t*>(a)[kStValid / 4];
    L.target = a[kStTarget / 4], L.dcs = a[kStDcs / 4], L.peri = a[kStPeri / 4], L.focus = a[kStFocus / 4];
  }
  if (ok0) c->Unmap(o->rsStage[0], 0);
  if (ok1) c->Unmap(o->rsStage[1], 0);
  if (!ok1 && g_rt.rsAge <= 60) return;  // not ready yet: the next frame tries again
  g_rt.rsPhase = 0;
  if (g_log) {
    char why[96];
    int n = 0;
    why[0] = 0;
    auto add = [&](const char* t) { n += snprintf(why + n, sizeof(why) - n, "%s%s", n ? " + " : "", t); };
    if (L.why & kRsStart) add("start");
    if (L.why & kRsMode) add("mode switch");
    if (L.why & kRsGap) add("gap");
    if (L.why & kRsMission) add("mission event");
    if (L.why & kRsParams) add("parameters");
    if (!ok1)
      g_log("colour: adaptation reset (%s; mode %d -> %d, gap %.2f s, frame %u): state not read back", why, L.fromMode,
            L.toMode, L.gapSec, g_rt.frame);
    else
      g_log("colour: adaptation reset (%s; mode %d -> %d, gap %.2f s, frame %u): adapted %.5f (valid %u) -> %.5f "
            "(valid %u) at the next frame, target %.5f (DCS's %.5f, log-avg periphery %.5f focus %.5f), after / target "
            "%.4f",
            why, L.fromMode, L.toMode, L.gapSec, g_rt.frame, L.before, L.validBefore, L.after, L.validAfter, L.target,
            L.dcs, L.peri, L.focus, L.target > 0 ? L.after / L.target : 0.0f);
  }
  g_resetLogged.fetch_add(1, std::memory_order_release);
}

// Saves the CS state we touch, runs `fn`, restores it.
// (R50: CS t0-t1, u0-u2, b0-b2, s0: the glare chain uses them all.)
template <class F>
inline void WithCs(ID3D11DeviceContext* c, F fn) {
  ID3D11ComputeShader* cs = nullptr;
  ID3D11ClassInstance* inst[8] = {};
  UINT ni = 8;
  c->CSGetShader(&cs, inst, &ni);
  ID3D11ShaderResourceView* srv[2] = {};
  c->CSGetShaderResources(0, 2, srv);
  ID3D11UnorderedAccessView* uav[3] = {};
  c->CSGetUnorderedAccessViews(0, 3, uav);
  ID3D11Buffer* cb[3] = {};
  c->CSGetConstantBuffers(0, 3, cb);
  ID3D11SamplerState* smp = nullptr;
  c->CSGetSamplers(0, 1, &smp);
  fn();
  c->CSSetShader(cs, ni ? inst : nullptr, ni);
  c->CSSetShaderResources(0, 2, srv);
  const UINT keep[3] = {static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1)};
  c->CSSetUnorderedAccessViews(0, 3, uav, keep);
  c->CSSetConstantBuffers(0, 3, cb);
  c->CSSetSamplers(0, 1, &smp);
  if (cs) cs->Release();
  for (UINT i = 0; i < ni && i < 8; ++i)
    if (inst[i]) inst[i]->Release();
  for (auto* s : srv)
    if (s) s->Release();
  for (auto* u : uav)
    if (u) u->Release();
  for (auto* b : cb)
    if (b) b->Release();
  if (smp) smp->Release();
}

// R50: DCS's gProj of view v as its tone-map draw sees it (cPerView, VS b7, else PS b7: context.hlsl, gProj at
// byte 256) into our state, on the GPU (no read-back, no wait). Without such a buffer: zeros (no glare there).
inline void CopyProj(ID3D11DeviceContext* c, Objects* o, int v) {
  ID3D11Buffer* cb = nullptr;
  c->VSGetConstantBuffers(7, 1, &cb);
  if (!cb) c->PSGetConstantBuffers(7, 1, &cb);
  D3D11_BUFFER_DESC d{};
  if (cb) cb->GetDesc(&d);
  if (cb && d.ByteWidth >= 320) {
    const D3D11_BOX box{256, 0, 0, 320, 1, 1};
    c->CopySubresourceRegion(o->state, 0, kStProj + v * 64, 0, 0, cb, 0, &box);
    g_projCopied[v]++;
  } else {
    static const float zero[16] = {};
    const D3D11_BOX box{kStProj + v * 64, 0, 0, kStProj + v * 64 + 64, 1, 1};
    c->UpdateSubresource(o->state, 0, &box, zero, 0, 0);
    g_projMissing[v]++;
  }
  if (cb) cb->Release();
}

// R50: the glare / low-pass chain of eye e (its periphery view, W x H), inside WithCs.
inline void RunGlare(ID3D11DeviceContext* c, Objects* o, int e, UINT W, UINT H, ID3D11ShaderResourceView* hdr,
                     ID3D11ShaderResourceView* lum, ID3D11Buffer* dcsB0) {
  const UINT w0 = (W + 3) / 4, h0 = (H + 3) / 4;
  if (w0 > kGlareAlloc || h0 > kGlareAlloc) {
    g_glareTooBig++;
    return;
  }
  const UINT w1 = (w0 + 1) / 2, h1 = (h0 + 1) / 2, w2 = (w1 + 1) / 2, h2 = (h1 + 1) / 2;
  auto g8 = [](UINT n) { return (n + 7) / 8; };
  ID3D11ShaderResourceView* noSrv[2] = {};
  ID3D11UnorderedAccessView* noUav[2] = {};
  const UINT keep[2] = {static_cast<UINT>(-1), static_cast<UINT>(-1)};
  auto bindIo = [&](ID3D11ShaderResourceView* s, ID3D11UnorderedAccessView* u, ID3D11Buffer* disp) {
    c->CSSetShaderResources(0, 1, noSrv);
    c->CSSetUnorderedAccessViews(1, 2, noUav, keep);
    c->CSSetShaderResources(0, 1, &s);
    c->CSSetUnorderedAccessViews(1, 1, &u, keep);
    c->CSSetConstantBuffers(2, 1, &disp);
  };
  using O = Objects;
  ID3D11UnorderedAccessView* st = o->stateUav;
  c->CSSetUnorderedAccessViews(0, 1, &st, keep);
  c->CSSetConstantBuffers(0, 1, &o->cb);
  c->CSSetConstantBuffers(1, 1, &dcsB0);
  // down: level 0 glare + low-pass
  c->CSSetShader(o->glare[0], nullptr, 0);
  bindIo(hdr, o->texUav[e][O::kTxG0], o->dispCb[e][6]);
  c->CSSetShaderResources(1, 1, &lum);
  c->CSSetUnorderedAccessViews(2, 1, &o->texUav[e][O::kTxLow], keep);
  c->Dispatch(g8(w0), g8(h0), 1);
  c->CSSetShaderResources(0, 2, noSrv);
  if (g_rt.glareOn) {
    c->CSSetShader(o->glare[1], nullptr, 0);  // levels 1 and 2
    bindIo(o->texSrv[e][O::kTxG0], o->texUav[e][O::kTxG1], o->dispCb[e][2]);
    c->Dispatch(g8(w1), g8(h1), 1);
    bindIo(o->texSrv[e][O::kTxG1], o->texUav[e][O::kTxG2], o->dispCb[e][4]);
    c->Dispatch(g8(w2), g8(h2), 1);
    c->CSSetShader(o->glare[2], nullptr, 0);  // separable blur of each level, through the temporaries
    const int tx[3] = {O::kTxG0, O::kTxG1, O::kTxG2};
    const UINT lw[3] = {w0, w1, w2}, lh[3] = {h0, h1, h2};
    for (int l = 0; l < 3; ++l) {
      bindIo(o->texSrv[e][tx[l]], o->tmpUav[l], o->dispCb[e][l * 2]);
      c->Dispatch(g8(lw[l]), g8(lh[l]), 1);
      bindIo(o->tmpSrv[l], o->texUav[e][tx[l]], o->dispCb[e][l * 2 + 1]);
      c->Dispatch(g8(lw[l]), g8(lh[l]), 1);
    }
    if (g_rt.streakOn) {  // from the blurred level 1
      c->CSSetShader(o->glare[3], nullptr, 0);
      c->CSSetSamplers(0, 1, &o->linClamp);
      bindIo(o->texSrv[e][O::kTxG1], o->texUav[e][O::kTxStreak], o->dispCb[e][6]);
      c->Dispatch(g8(w1), g8(h1), 1);
    }
  }
  c->CSSetShaderResources(0, 1, noSrv);
  c->CSSetUnorderedAccessViews(1, 2, noUav, keep);
  g_glareRuns[e]++;
}

// ---------------------------------------------------------------------------
// R51 VrFx (render thread): per-frame steps, per-view constants and the DoF prep, inside the tone-map draw hook
// ---------------------------------------------------------------------------
// Inputs the glue (fixation_depth.h: ColorBoundary and CloseRun, same thread) leaves before each HMD view's tone map:
// the view's depth (SRV on DCS's depth target of its G-buffer run; fixdepth's SRV cache keeps the reference) and its
// depth viewport; per frame (at view 0) the newest gaze and FixationDepth's newest published record.
struct FxViewIn {
  ID3D11ShaderResourceView* depth = nullptr;  // no reference (fixdepth's SRV cache holds it for >= 120 frames)
  ID3D11Resource* depthTex = nullptr;         // identity
  float dv[4] = {};
  bool valid = false;
};
struct FxFrameIn {
  bool gazeValid = false, saccade = false;
  float gaze[4][2] = {};
  bool depthValid = false;
  double medianM = 0, conf = 0;
};
inline FxViewIn g_fxView[4];
inline FxFrameIn g_fxIn;
inline bool g_fxProjSeen[4] = {};
struct FxRt {
  vrfx::Params p;
  uint32_t ver = ~0u;
  vrfx::DofState dof;
  vrfx::GState g;
  vrfx::Shape shape;
  bool dofOn = false, gvOn = false;
  int gvMode = 1;
  double pupil = 4.0;
  uint64_t gSeq = 0;
  uint32_t resetGen = 0;  // R58: vrfx::g_fxResetGen at the last reset
  LARGE_INTEGER last{};
  // our PS slots saved around the swapped draw
  bool bound = false;
  ID3D11ShaderResourceView* oldSrv[3] = {};
  ID3D11Buffer* oldCb = nullptr;
};
inline FxRt g_fx;
inline bool g_fxHold = false;  // tests / previews: the state machines do not step (g_fx set by hand)
// Diagnostics (the render thread adds / stores, the config thread logs every 10 s).
inline std::atomic<uint64_t> g_fxPrep{0}, g_fxNoDepth{0}, g_fxNoProj{0}, g_fxDepthBound{0}, g_fxNoTargets{0};
inline std::atomic<float> g_fxGain{0}, g_fxFocusD{0}, g_fxPupil{0}, g_fxLoss{0}, g_fxRed{0}, g_fxGz{0};
inline std::atomic<float> g_fxGainMax{0}, g_fxLossMax{0};
inline std::atomic<uint64_t> g_fxSacc{0}, g_fxPlanes{0}, g_fxLowConf{0}, g_fxNoGaze{0}, g_fxFrames{0}, g_fxBlurFrames{0};

// Once per frame, at the first view's tone map (before its draw).
inline void FxFrame(bool sunValid, float sunY) {
  FxRt& f = g_fx;
  const uint32_t ver = vrfx::g_paramVer.load(std::memory_order_acquire);
  if (ver != f.ver) {
    while (vrfx::g_paramLock.test_and_set(std::memory_order_acquire)) YieldProcessor();
    f.p = vrfx::g_params;
    vrfx::g_paramLock.clear(std::memory_order_release);
    f.ver = ver;
  }
  LARGE_INTEGER now, fq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&fq);
  double dt = f.last.QuadPart ? (now.QuadPart - f.last.QuadPart) / static_cast<double>(fq.QuadPart) : 0.0;
  f.last = now;
  f.dofOn = f.p.dof && vrfx::g_wantDof.load(std::memory_order_relaxed);
  if (g_fxHold) {
    f.gvOn = f.p.gv && vrfx::g_wantGv.load(std::memory_order_relaxed);
    return;
  }
  // R58: a mission load / stop / slot change drops the state (no frame steps it while DCS loads, so a blackout or
  // red-out of the previous aircraft carried into the next); G samples from before the event are not new.
  if (const uint32_t gen = vrfx::g_fxResetGen.load(std::memory_order_acquire); gen != f.resetGen) {
    f.resetGen = gen;
    f.g = vrfx::GState{};
    f.dof = vrfx::DofState{};
    f.gSeq = vrfx::g_gSeq.load(std::memory_order_acquire);
    dt = 0;
  }
  if (f.dofOn) {
    vrfx::DofIn in;
    in.dt = dt;
    in.gazeValid = g_fxIn.gazeValid;
    in.saccade = g_fxIn.saccade;
    in.gazeX = g_fxIn.gaze[2][0], in.gazeY = g_fxIn.gaze[2][1];
    in.depthValid = g_fxIn.depthValid;
    in.medianM = g_fxIn.medianM;
    in.conf = g_fxIn.conf;
    vrfx::StepDof(f.dof, in, f.p);
    const double target = f.p.dofPupilMm > 0 ? f.p.dofPupilMm
                                             : vrfx::PupilMm(vrfx::SceneCdm2(sunValid, sunY, g_rbFxLum.load()));
    f.pupil += (target - f.pupil) * (1 - std::exp(-std::clamp(dt, 0.0, 0.25) / 0.8));  // pupil response ~0.8 s
  } else {
    f.dof = vrfx::DofState{};
  }
  f.gvOn = f.p.gv && vrfx::g_wantGv.load(std::memory_order_relaxed);
  if (f.gvOn) {
    const uint64_t sq = vrfx::g_gSeq.load(std::memory_order_acquire);
    const bool have = sq != f.gSeq;
    f.gSeq = sq;
    vrfx::StepG(f.g, have, vrfx::g_gy.load(std::memory_order_relaxed), dt, f.p);
    f.gvMode = vrfx::EffectiveGvMode(f.p.gvMode, vrfx::g_dcsGEffect.load(std::memory_order_relaxed));
    // R58: no field while the camera is not the cockpit's (the state keeps stepping for the return).
    f.shape = vrfx::g_gExternal.load(std::memory_order_relaxed) ? vrfx::Shape{}
                                                                 : vrfx::ShapeOf(f.g.loss, f.g.red, f.p.gvStrength, f.gvMode);
  } else {
    f.g = vrfx::GState{};
    f.shape = vrfx::Shape{};
  }
  g_fxGain = static_cast<float>(f.dof.gain), g_fxFocusD = static_cast<float>(f.dof.focusD);
  g_fxPupil = static_cast<float>(f.pupil), g_fxLoss = static_cast<float>(f.g.loss), g_fxRed = static_cast<float>(f.g.red);
  g_fxGz = vrfx::g_gy.load(std::memory_order_relaxed);
  if (f.dof.gain > g_fxGainMax.load()) g_fxGainMax = static_cast<float>(f.dof.gain);
  if (f.g.loss > g_fxLossMax.load()) g_fxLossMax = static_cast<float>(f.g.loss);
  g_fxSacc = f.dof.saccades, g_fxPlanes = f.dof.newPlanes, g_fxLowConf = f.dof.lowConf, g_fxNoGaze = f.dof.noGaze;
  g_fxFrames = f.dof.frames, g_fxBlurFrames = f.dof.framesBlur;
}
// R62: once per frame instead of FxFrame while neither effect is wanted. FxFrame's own "effect off" resets only run
// while it runs, i.e. while the other effect is on: with both off it stopped, the G / DoF state of the moment they
// were switched off stayed, and switching GVision back on in the same mission drew that blackout / red-out again
// (decaying over ~6 s, without any G). The state is dropped once here; G samples from before are not new.
inline void FxIdle() {
  FxRt& f = g_fx;
  if (g_fxHold || (!f.dofOn && !f.gvOn && !f.last.QuadPart)) return;  // tests set g_fx by hand / already idle
  f.g = vrfx::GState{};
  f.dof = vrfx::DofState{};
  f.shape = vrfx::Shape{};
  f.dofOn = f.gvOn = false;
  f.gSeq = vrfx::g_gSeq.load(std::memory_order_acquire);
  f.last = LARGE_INTEGER{};  // the first frame back steps with dt 0
  g_fxGain = 0, g_fxLoss = 0, g_fxRed = 0;
}
// The frame's VrFx step: FxFrame while an effect is wanted, else FxIdle.
inline void FxStep(bool sunValid, float sunY) {
  if (vrfx::Wanted()) FxFrame(sunValid, sunY);
  else FxIdle();
}

// The pass runs our shader with the effects this frame: GazeDof enabled (always while enabled, so the image does not
// switch between DCS's and our tone map at every saccade), or a non-neutral G vision field.
inline bool FxActive() { return g_fx.dofOn || (g_fx.gvOn && !vrfx::Neutral(g_fx.shape)); }

// The per-view constants (no D3D; tests use it).
inline vrfx::FxCb FxBuild(int v, UINT w, UINT h, bool depthOk) {
  const FxRt& f = g_fx;
  vrfx::FxCb cb{};
  cb.viewW = static_cast<float>(w), cb.viewH = static_cast<float>(h);
  const UINT hw = (w + 1) / 2, hh = (h + 1) / 2;
  cb.halfW = static_cast<float>(hw), cb.halfH = static_cast<float>(hh);
  cb.tilesX = static_cast<float>((hw + 7) / 8), cb.tilesY = static_cast<float>((hh + 7) / 8);
  if (v < 0 || v > 3) return cb;
  cb.projOff = static_cast<float>(vrfx::kStFxProj + 64 * v);
  const bool proj = g_fxProjSeen[v];
  if (g_fxIn.gazeValid) cb.gazeX = g_fxIn.gaze[v][0], cb.gazeY = g_fxIn.gaze[v][1];
  if (f.dofOn && proj && depthOk && f.dof.gain > 0) {
    cb.dofOn = 1;
    cb.depthOk = 1;
    cb.dofAmt = static_cast<float>(f.p.dofStrength * f.dof.gain);
    cb.focusD = static_cast<float>(f.dof.focusD);
    cb.tolD = static_cast<float>(f.p.dofToleranceD);
    cb.pupilMm = static_cast<float>(f.pupil);
    cb.maxArcmin = static_cast<float>(f.p.dofMaxArcmin);
    cb.guardDeg = g_fxIn.gazeValid ? static_cast<float>(f.p.dofGuardDeg) : 0.0f;
    const FxViewIn& vi = g_fxView[v];
    cb.dvX = vi.dv[0], cb.dvY = vi.dv[1], cb.dvW = vi.dv[2], cb.dvH = vi.dv[3];
  }
  if (f.gvOn && proj && !vrfx::Neutral(f.shape)) {
    const vrfx::Shape& s = f.shape;
    cb.gvOn = 1;
    cb.tunnel = s.tunnel, cb.edge = s.edge, cb.outside = s.outside, cb.global = s.global;
    cb.desatC = s.desatC, cb.colorEdge = s.colorEdge, cb.desatP = s.desatP, cb.red = s.red;
    if (f.p.gvUseGaze && g_fxIn.gazeValid) cb.gvX = cb.gazeX, cb.gvY = cb.gazeY;
  }
  return cb;
}

// Before the swapped draw of HMD view v (b: the checked bound state): constants, the DoF prep dispatch (CS state
// saved and restored), our PS slots t22-t24 / b11 saved and bound (R50 owns b12, t17-t21, s15). False: no effects in this view.
inline bool FxViewPre(ID3D11DeviceContext* c, int v, Objects* o, const Bound& b) {
  FxRt& f = g_fx;
  bool depthOk = false;
  if (v >= 0 && v <= 3 && f.dofOn && f.dof.gain > 0) {
    const FxViewIn& vi = g_fxView[v];
    if (!g_fxProjSeen[v]) g_fxNoProj++;
    if (!vi.valid || !vi.depth) {
      g_fxNoDepth++;
    } else {
      ID3D11DepthStencilView* dsv = nullptr;
      c->OMGetRenderTargets(0, nullptr, &dsv);
      ID3D11Resource* dr = nullptr;
      if (dsv) dsv->GetResource(&dr), dsv->Release();
      if (dr) dr->Release();  // identity only
      if (dr && dr == vi.depthTex) g_fxDepthBound++;  // bound as DSV: d3d11 would null our SRV
      else depthOk = true;
    }
  }
  vrfx::FxCb cb = FxBuild(v, b.rtd.Width, b.rtd.Height, depthOk);
  if (cb.dofOn > 0 &&
      !o->EnsureFxTargets(static_cast<UINT>(cb.halfW), static_cast<UINT>(cb.halfH), static_cast<UINT>(cb.tilesX),
                          static_cast<UINT>(cb.tilesY))) {
    g_fxNoTargets++;
    cb.dofOn = 0;
  }
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(c->Map(o->fxCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
  memcpy(m.pData, &cb, sizeof(cb));
  c->Unmap(o->fxCb, 0);
  if (v == 0 && b.srv[1]) {  // DCS's adapted luminance (t1[1].x) for the pupil estimate (read back every 2 s)
    ID3D11Resource* lr = nullptr;
    b.srv[1]->GetResource(&lr);
    if (lr) {
      const D3D11_BOX box{8, 0, 0, 12, 1, 1};
      c->CopySubresourceRegion(o->state, 0, vrfx::kStFxLum, 0, 0, lr, 0, &box);
      lr->Release();
    }
  }
  if (cb.dofOn > 0) {
    ID3D11ComputeShader* cs = nullptr;
    ID3D11ClassInstance* inst[8] = {};
    UINT ni = 8;
    c->CSGetShader(&cs, inst, &ni);
    ID3D11ShaderResourceView* srv[3] = {};
    c->CSGetShaderResources(0, 3, srv);
    ID3D11UnorderedAccessView* uav[2] = {};
    c->CSGetUnorderedAccessViews(0, 2, uav);
    ID3D11Buffer* cb0 = nullptr;
    c->CSGetConstantBuffers(0, 1, &cb0);
    ID3D11ShaderResourceView* ns[3] = {b.srv[0], g_fxView[v].depth, o->stateSrv};
    ID3D11UnorderedAccessView* nu[2] = {o->prepUav, o->tileUav};
    const UINT keep[2] = {static_cast<UINT>(-1), static_cast<UINT>(-1)};
    c->CSSetShader(o->prep, nullptr, 0);
    c->CSSetShaderResources(0, 3, ns);
    c->CSSetUnorderedAccessViews(0, 2, nu, keep);
    c->CSSetConstantBuffers(0, 1, &o->fxCb);
    c->Dispatch(static_cast<UINT>(cb.tilesX), static_cast<UINT>(cb.tilesY), 1);
    g_fxPrep++;
    c->CSSetShader(cs, ni ? inst : nullptr, ni);
    c->CSSetShaderResources(0, 3, srv);
    c->CSSetUnorderedAccessViews(0, 2, uav, keep);
    c->CSSetConstantBuffers(0, 1, &cb0);
    if (cs) cs->Release();
    for (UINT i = 0; i < ni && i < 8; ++i)
      if (inst[i]) inst[i]->Release();
    for (auto* s : srv)
      if (s) s->Release();
    for (auto* u : uav)
      if (u) u->Release();
    if (cb0) cb0->Release();
  }
  c->PSGetShaderResources(vrfx::kPsFxSrvSlot, 3, f.oldSrv);
  c->PSGetConstantBuffers(vrfx::kPsFxCbSlot, 1, &f.oldCb);
  ID3D11ShaderResourceView* ps[3] = {cb.dofOn > 0 ? o->prepSrv : nullptr, cb.dofOn > 0 ? g_fxView[v].depth : nullptr,
                                     cb.dofOn > 0 ? o->tileSrv : nullptr};
  c->PSSetShaderResources(vrfx::kPsFxSrvSlot, 3, ps);
  c->PSSetConstantBuffers(vrfx::kPsFxCbSlot, 1, &o->fxCb);
  f.bound = true;
  return true;
}
inline void FxViewPost(ID3D11DeviceContext* c) {
  FxRt& f = g_fx;
  if (!f.bound) return;
  c->PSSetShaderResources(vrfx::kPsFxSrvSlot, 3, f.oldSrv);
  c->PSSetConstantBuffers(vrfx::kPsFxCbSlot, 1, &f.oldCb);
  for (auto*& s : f.oldSrv)
    if (s) s->Release(), s = nullptr;
  if (f.oldCb) f.oldCb->Release(), f.oldCb = nullptr;
  f.bound = false;
}
// At an HMD view's G-buffer run close (render thread, glue): DCS's cPerView gProj (byte 256 of the view's b7, at its
// first-constant offset) into our state, copied on the GPU (DCS's buffer; no CPU read, no stall).
inline void FxNoteProj(ID3D11DeviceContext* c, int v, ID3D11Buffer* perView, UINT firstConst) {
  Objects* o = g_obj.load(std::memory_order_acquire);
  if (!o || !c || !perView || v < 0 || v > 3) return;
  D3D11_BUFFER_DESC d{};
  perView->GetDesc(&d);
  const UINT off = firstConst * 16 + 256;
  if (d.ByteWidth < off + 64) return;
  const D3D11_BOX box{off, 0, 0, off + 64, 1, 1};
  c->CopySubresourceRegion(o->state, 0, vrfx::kStFxProj + 64 * v, 0, 0, perView, 0, &box);
  g_fxProjSeen[v] = true;
}

// Before DCS's tone-map draw of HMD view v (render thread, inside the draw hook). Returns 1: our shader is bound
// (ToneDrawPost restores), 2: timing only, 0: nothing.
inline int ToneDrawPre(ID3D11DeviceContext* c, int v) {
  if (g_fault.load(std::memory_order_relaxed) || v < 0 || v > kViewUnknown) return 0;
  if (g_tp.open) g_tp.reached = true;  // R47 miss cause
  Objects* o = g_obj.load(std::memory_order_acquire);
  const int mode = g_mode.load(std::memory_order_relaxed);
  const bool timing = g_timing.load(std::memory_order_relaxed) != 0;
  if (!o) {
    g_rejects[kRjNoObjects]++;
    return 0;
  }
  const bool known = v <= 3;
  const bool newFrame = known && v <= g_rt.lastView;
  if (known) g_rt.lastView = v;
  if (newFrame) {
    g_rt.frameCbDone = false;
    ++g_rt.frame;
    if (timing) TimingFrameStart(c, o);
    else g_rt.qCur = -1;
    ReadbackStep(c, o);
    ResetLogStep(c, o);  // R81
    FxStep(g_sunValid.load(std::memory_order_relaxed), g_sunY.load(std::memory_order_relaxed));  // R51, R62
  }
  // R83 D1: whether this pass is due, from the same state the swap below goes by (this frame's FxStep has run). A Mode 0
  // pass whose effects went inactive at this frame's step (VrFx turned off, the G shape neutral, vrfx::Wanted flipped
  // since the pass began) is DCS's own tone map by design, not a miss.
  const bool fxDue = vrfx::Wanted() && FxActive();
  if (g_tp.open) g_tp.modeOn = mode != 0 || fxDue, g_tp.mode = mode;
  Bound b;
  const int why = CheckBound(c, v, b);
  if (why != kRjCount) {
    g_rejects[why]++;
    b.Release();
    // A size not seen yet at any CopyToHMDPass (a resolution change): learn again at once (1 s cooldown: a target
    // that is not an HMD view's never learns its size, so it keeps being rejected without re-arming every frame).
    if ((why == kRjSize || why == kRjUnknownSize) && GetTickCount64() - g_rt.lastRelearn > 1000) {
      g_rt.lastRelearn = GetTickCount64();
      g_rt.learnFrames = 32;
    }
    return 0;
  }
  Timestamp(c, o, v, false);
  const bool fx = fxDue && o->psFx;  // R51: the effects (with DCS's own tone map in Mode 0)
  // R95: the symbology's second evaluation in this draw (only a known view with E due this frame).
  SymBind sb;
  bool symV = false;
  if (known && g_symQuery) {
    if (g_symPsReady.load(std::memory_order_acquire) != 1) {
      if (g_symQuery(v, b.t0Res, &sb)) g_symNoPs++;  // E was due, the shaders are not made yet
    } else {
      symV = g_symQuery(v, b.t0Res, &sb);
    }
  }
  if (mode == 0 && !fx && !symV) {
    // R77: Mode 0 counts as a mode: Enhanced -> Off -> Enhanced resets the adaptation (lastMode stayed 2 here, so
    // Enhanced resumed from the luminance it had when it was turned off).
    g_rt.lastMode = 0;
    b.Release();
    return timing ? 2 : 0;
  }
  static const cgrade::ModeParams kStockState = [] {  // Mode 0 + effects: no EC copy, meter or adaptation
    cgrade::ModeParams s;
    s.equalizeEc = 0;
    return s;
  }();
  const cgrade::ModeParams& p = mode ? CurParams(mode) : kStockState;
  if (mode != g_rt.lastMode) {
    if (!(g_rt.resetWhy & kRsMode)) g_rt.resetFrom = g_rt.lastMode;
    g_rt.lastMode = mode;
    g_rt.resetNext = true;
    g_rt.resetWhy |= kRsMode;
  }
  if (const uint32_t gen = g_modeGen.load(std::memory_order_acquire); gen != g_rt.modeGen) {
    g_rt.modeGen = gen;  // R81: the config thread switched the mode (also through a Mode 0 we never saw)
    if (!(g_rt.resetWhy & kRsMode)) g_rt.resetFrom = g_modeFrom.load(std::memory_order_relaxed);
    g_rt.resetNext = true;
    g_rt.resetWhy |= kRsMode;
  }
  if (const uint32_t gen = g_adaptResetGen.load(std::memory_order_acquire); gen != g_rt.adaptGen) {
    g_rt.adaptGen = gen;  // R77: a mission event since the last tone map
    g_rt.resetNext = true;
    g_rt.resetWhy |= kRsMission;
  }
  if (!g_rt.frameCbDone) {
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    const double dt = g_rt.lastQpc.QuadPart ? (now.QuadPart - g_rt.lastQpc.QuadPart) / static_cast<double>(f.QuadPart) : 0;
    g_rt.lastQpc = now;
    if (dt > kAdaptGapSec) {  // R77: no tone map of ours for a while (a mode / VrFx gap, a load)
      g_rt.resetNext = true;
      g_rt.resetWhy |= kRsGap;
      g_rt.resetGap = dt;
    }
    cgrade::ColorCb cb = mode ? cgrade::BuildCb(p, g_sunValid.load(std::memory_order_relaxed),
                                                g_sunY.load(std::memory_order_relaxed), g_rt.frame, dt, g_rt.resetNext,
                                                g_rt.ldrW[2] != 0 || g_rt.ldrW[3] != 0)
                              : vrfx::StockCb(g_rt.frame);  // R51: Mode 0 + effects = DCS's tone map
    if (g_glareDebug.load(std::memory_order_relaxed)) cb.debug = 1.0f;  // tests / preview (the ini: GlareDebug)
    g_rt.auxOn = cgrade::UsesAux(cb);
    g_rt.glareOn = cb.glareOn > 0;
    g_rt.streakOn = cb.glareOn > 0 && cb.glareStreak > 0;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(c->Map(o->cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
      memcpy(m.pData, &cb, sizeof(cb));
      c->Unmap(o->cb, 0);
      g_rt.cbStock = mode == 0 && cb.debug == 0;  // R57
    }
    // R81: this frame's constants reset the adaptation (Mode 0 + effects: the stock constants, no adaptation)
    g_rt.cbReset = mode != 0 && g_rt.resetNext;
    g_rt.cbWhy = g_rt.resetWhy, g_rt.cbFrom = g_rt.resetFrom, g_rt.cbGap = g_rt.resetGap;
    // R77: the reset is consumed by the adaptation dispatch, which runs at view 0 only: constants built at another
    // view (a switch seen mid-frame) keep it for the next frame.
    if (v == 0) {
      g_rt.resetNext = false;
      g_rt.resetWhy = 0, g_rt.resetFrom = -1, g_rt.resetGap = 0;
    }
    g_rt.frameCbDone = true;
  }
  if (v == 1 && p.equalizeEc) {
    const D3D11_BOX box{0, 0, 0, 16, 1, 1};
    c->CopySubresourceRegion(o->state, 0, kStEcRow, 0, 0, b.b0, 0, &box);
  }
  const bool meter = p.meterFocus > 0 && (v == 0 || v == 2);
  const bool adapt = cgrade::UsesState(p) && v == 0;
  // R50: each known view's projection (the focus views map their pixels into the eye's periphery field with it);
  // the eye's glare / low-pass chain at its periphery view (views 0 / 1 come before 2 / 3 in every frame).
  if (g_rt.auxOn && known) CopyProj(c, o, v);
  const bool glareRun = g_rt.auxOn && (v == 0 || v == 1);
  if (meter || adapt || glareRun) {
    WithCs(c, [&] {
      ID3D11UnorderedAccessView* u = o->stateUav;
      const UINT keep = static_cast<UINT>(-1);
      c->CSSetUnorderedAccessViews(0, 1, &u, &keep);
      if (meter) {
        c->CSSetShader(o->meter[v == 2 ? 1 : 0], nullptr, 0);
        c->CSSetShaderResources(0, 1, &b.srv[0]);
        c->Dispatch(1, 1, 1);
      }
      if (adapt) {
        const D3D11_BOX rsBox{0, 0, 0, kRsBytes, 1, 1};
        if (g_rt.cbReset) {  // R81 reset log: the state before the reset (once per logged reset; reasons merged)
          if (g_rt.rsPhase == 0) {
            c->CopySubresourceRegion(o->rsStage[0], 0, 0, 0, 0, o->state, 0, &rsBox);
            g_rt.rsPhase = 1, g_rt.rsWhy = 0, g_rt.rsFrom = g_rt.cbFrom, g_rt.rsGap = 0;
          }
          if (g_rt.rsPhase == 1) {
            g_rt.rsWhy |= g_rt.cbWhy, g_rt.rsTo = mode;
            if (g_rt.cbGap > g_rt.rsGap) g_rt.rsGap = g_rt.cbGap;
          }
        }
        ID3D11ShaderResourceView* s2[2] = {nullptr, b.srv[1]};
        c->CSSetShaderResources(0, 2, s2);
        c->CSSetConstantBuffers(0, 1, &o->cb);
        c->CSSetShader(o->adapt, nullptr, 0);
        c->Dispatch(1, 1, 1);
        if (!g_rt.cbReset && g_rt.rsPhase == 1) {  // R81: the first dispatch after the reset frame: the snap
          c->CopySubresourceRegion(o->rsStage[1], 0, 0, 0, 0, o->state, 0, &rsBox);
          g_rt.rsPhase = 2, g_rt.rsAge = 0;
        }
      }
      if (glareRun) RunGlare(c, o, v, b.rtd.Width, b.rtd.Height, b.srv[0], b.srv[1], b.b0);
    });
  }
  // The swap: DCS's pixel shader, our b13 / t16 slots saved first.
  Rt& r = g_rt;
  r.oldInstN = 8;
  c->PSGetShader(&r.oldPs, r.oldInst, &r.oldInstN);
  c->PSGetConstantBuffers(kPsCbSlot, 1, &r.oldCb);
  c->PSGetShaderResources(kPsStateSlot, 1, &r.oldSrv);
  c->PSGetConstantBuffers(kPsViewSlot, 1, &r.oldViewCb);
  c->PSGetShaderResources(kPsAuxSlot, kPsAuxN, r.oldAux);
  c->PSGetSamplers(kPsSamplerSlot, 1, &r.oldSampler);
  // R51; R95: Mode 0 with the symbology alone runs the stock replica psFxStock too (no effect on in its constants)
  const bool fxOn = (fx || (symV && mode == 0)) && FxViewPre(c, v, o, b);
  // R57: the stock-constant variant while the bound constant buffer is the stock one (g_fxStockShader 0: off)
  const bool stockPs = g_rt.cbStock && o->psFxStock && g_fxStockShader.load(std::memory_order_relaxed);
  ID3D11PixelShader* const fxPs = stockPs ? o->psFxStock : o->psFx;
  // R95: the QV_SYM variant of the shader this draw runs anyway (Mode 0 without FxViewPre: ps with the stock constants)
  ID3D11PixelShader* ps = fxOn ? fxPs : o->ps;
  if (symV) ps = o->psSym[fxOn ? (stockPs ? 2 : 1) : 0];
  c->PSSetShader(ps, nullptr, 0);
  if (symV) {  // b10, t25, OM UAV 1 (2 in tests) saved, ours bound (the target stays bound: UAVs after RTV 0)
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    Rt& rr = g_rt;
    c->PSGetConstantBuffers(kPsSymCbSlot, 1, &rr.oldSymCb);
    c->PSGetShaderResources(kPsSymSrvSlot, 1, &rr.oldSymSrv);
    const UINT nu = g_symTestC ? 2 : 1;
    c->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, kPsSymUavSlot, nu, rr.oldSymUav);
    c->PSSetConstantBuffers(kPsSymCbSlot, 1, &sb.cb);
    c->PSSetShaderResources(kPsSymSrvSlot, 1, &sb.acc);
    ID3D11UnorderedAccessView* u[2] = {sb.out, g_symTestC};
    c->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr,
                                                 kPsSymUavSlot, nu, u, nullptr);
    rr.symView = v;
    g_symDraws++;
    QueryPerformanceCounter(&q1);
    g_symBindQpc += q1.QuadPart - q0.QuadPart;
  }
  c->PSSetConstantBuffers(kPsCbSlot, 1, &o->cb);
  c->PSSetShaderResources(kPsStateSlot, 1, &o->stateSrv);
  {  // R50: this view's index and its eye's levels (t17 G0, t18 G1, t19 G2, t20 streak, t21 low-pass)
    c->PSSetConstantBuffers(kPsViewSlot, 1, &o->viewCb[known ? v : 4]);
    const int e = known ? (v & 1) : 0;
    ID3D11ShaderResourceView* aux[kPsAuxN] = {o->texSrv[e][Objects::kTxG0], o->texSrv[e][Objects::kTxG1],
                                              o->texSrv[e][Objects::kTxG2], o->texSrv[e][Objects::kTxStreak],
                                              o->texSrv[e][Objects::kTxLow]};
    c->PSSetShaderResources(kPsAuxSlot, kPsAuxN, aux);
    c->PSSetSamplers(kPsSamplerSlot, 1, &o->linClamp);
  }
  if (symV && g_symDrawBegin) g_symDrawBegin(v);  // R97: the GPU time of this draw (the symbology's cost line)
  r.swapped = true;
  if (known) g_swaps[v]++;
  else g_swapUnknown++;
  if (g_tp.open) g_tp.swapped = true;
  b.Release();
  return 1;
}

// Right after DCS's (swapped) draw: DCS's pixel shader and slots back; timing stamp.
inline void ToneDrawPost(ID3D11DeviceContext* c, int v, int act) {
  Rt& r = g_rt;
  if (r.swapped) {
    c->PSSetShader(r.oldPs, r.oldInstN ? r.oldInst : nullptr, r.oldInstN);
    c->PSSetConstantBuffers(kPsCbSlot, 1, &r.oldCb);
    c->PSSetShaderResources(kPsStateSlot, 1, &r.oldSrv);
    c->PSSetConstantBuffers(kPsViewSlot, 1, &r.oldViewCb);
    c->PSSetShaderResources(kPsAuxSlot, kPsAuxN, r.oldAux);
    c->PSSetSamplers(kPsSamplerSlot, 1, &r.oldSampler);
    if (r.oldViewCb) r.oldViewCb->Release(), r.oldViewCb = nullptr;
    for (auto*& a : r.oldAux)
      if (a) a->Release(), a = nullptr;
    if (r.oldSampler) r.oldSampler->Release(), r.oldSampler = nullptr;
    if (r.oldPs) r.oldPs->Release();
    for (UINT i = 0; i < r.oldInstN && i < 8; ++i)
      if (r.oldInst[i]) r.oldInst[i]->Release(), r.oldInst[i] = nullptr;
    if (r.oldCb) r.oldCb->Release();
    if (r.oldSrv) r.oldSrv->Release();
    r.oldPs = nullptr, r.oldCb = nullptr, r.oldSrv = nullptr, r.oldInstN = 0;
    r.swapped = false;
    FxViewPost(c);  // R51
    if (r.symView >= 0) {  // R95: the symbology's slots back, then the export learns that E was written
      LARGE_INTEGER q0, q1;
      QueryPerformanceCounter(&q0);
      const UINT nu = g_symTestC ? 2 : 1;
      c->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr,
                                                   kPsSymUavSlot, nu, r.oldSymUav, nullptr);
      c->PSSetConstantBuffers(kPsSymCbSlot, 1, &r.oldSymCb);
      c->PSSetShaderResources(kPsSymSrvSlot, 1, &r.oldSymSrv);
      for (auto*& u : r.oldSymUav)
        if (u) u->Release(), u = nullptr;
      if (r.oldSymCb) r.oldSymCb->Release(), r.oldSymCb = nullptr;
      if (r.oldSymSrv) r.oldSymSrv->Release(), r.oldSymSrv = nullptr;
      QueryPerformanceCounter(&q1);
      g_symBindQpc += q1.QuadPart - q0.QuadPart;
      if (g_symDone) g_symDone(r.symView, true);
      r.symView = -1;
    }
  }
  Objects* o = g_obj.load(std::memory_order_acquire);
  if (o && act && g_timing.load(std::memory_order_relaxed)) {
    Timestamp(c, o, v, true);
    if (v == 3 && r.qCur >= 0 && o->q[r.qCur].open) {
      c->End(o->q[r.qCur].disjoint);
      o->q[r.qCur].open = false;
      o->q[r.qCur].issued = true;
    }
  }
  if (o && v == 3 && r.rbPending < 0 && g_rbWant.load(std::memory_order_relaxed)) ReadbackStep(c, o);
}

// ---------------------------------------------------------------------------
// Config thread (once per second)
// ---------------------------------------------------------------------------
struct Cfg {
  int mode = 0, timing = 0;
  cgrade::ModeParams fix = cgrade::FixDefaults(), enh = cgrade::EnhancedDefaults();
};
inline Blobs g_blobs;
inline bool g_blobsTried = false, g_blobsOk = false;
inline ID3D11Device* g_objDev = nullptr;

inline const char* ModeName(int m) { return m == 1 ? "Fix" : m == 2 ? "Enhanced" : "off (DCS's own)"; }
inline void LogParams(int mode, const cgrade::ModeParams& p) {
  if (!g_log) return;
  g_log("colour: mode %d %s: SrgbMatch %d EqualizeEc %d NightFloor %.4f (gate %d) ExposureEV %+.2f NightEV %+.2f curve %s "
        "(%.3f/%.3f/%.3f/%.3f) HueKeep %.2f/%.2f (mid-high/shadow) ChromaNorm %.2f WhiteStart %.2f GamutKeepSat %.2f Saturation %.2f Warmth %.2f "
        "DarkDesat %.2f/%.2f (night/day) Dither %d AdaptBright %.2f s AdaptDark %.2f s MeterFocus %.2f; NightVision %d "
        "(strength %.2f blue %.2f blur %.2f, scotopic %.4f / photopic %.2f cd/m2) Glare %d (strength %.2f threshold %+.1f EV "
        "radius %.1f deg streak %.2f)",
        mode, ModeName(mode), p.srgbMatch, p.equalizeEc, p.nightFloor, p.nightGate, p.exposureEv, p.nightEv,
        cgrade::OwnCurve(p) ? "own" : "DCS's", p.slope, p.toe, p.shoulder, p.whiteClip, p.hueKeep, p.hueShadow, p.chromaNorm,
        p.whiteStart, p.gamutKeepSat, p.saturation, p.warmth, p.darkDesat, p.darkDesatDay, p.dither, p.adaptBright,
        p.adaptDark, p.meterFocus, p.nightVision, p.nvStrength, p.nvBlue, p.nvBlur, p.nvScotopic, p.nvPhotopic, p.glare,
        p.glareStrength, p.glareThresholdEv, p.glareRadiusDeg, p.glareStreak);
}

// Publishes the configuration and the sun, compiles and creates when wanted, logs every 10 s.
inline void Tick(const Cfg& cfg, bool sunValid, float sunY) {
  static int lastMode = -1, lastTiming = -1;
  static cgrade::ModeParams lastFix{}, lastEnh{};
  static bool first = true;
  const bool paramsChanged = first || !cgrade::SameMode(cfg.fix, lastFix) || !cgrade::SameMode(cfg.enh, lastEnh);
  if (paramsChanged) {
    {
      std::lock_guard<std::mutex> lock(g_paramMutex);
      g_params[1] = cfg.fix;
      g_params[2] = cfg.enh;
    }
    g_paramVer++;
    lastFix = cfg.fix, lastEnh = cfg.enh;
  }
  first = false;
  if (cfg.mode != lastMode) {  // R81 (before the mode is published)
    g_modeFrom.store(lastMode, std::memory_order_relaxed);
    g_modeGen.fetch_add(1, std::memory_order_acq_rel);
  }
  g_mode = cfg.mode;
  g_timing = cfg.timing;
  g_sunY = sunY;
  g_sunValid = sunValid;
  if (cfg.mode != lastMode || (paramsChanged && cfg.mode)) {
    if (cfg.mode) LogParams(cfg.mode, cfg.mode == 2 ? cfg.enh : cfg.fix);
    else if (lastMode > 0 && g_log) g_log("colour: mode 0 (DCS's own tone map)");
  }
  if (cfg.timing != lastTiming && g_log && (cfg.timing || lastTiming > 0))
    g_log("colour: GPU timing of the tone-map draws %s", cfg.timing ? "on" : "off");
  lastMode = cfg.mode, lastTiming = cfg.timing;
  if (!Wanted()) return;
  if (!g_blobsTried) {
    g_blobsTried = true;
    std::string err;
    g_blobsOk = CompileAll(g_blobs, err);
    if (!g_blobsOk && g_log) g_log("colour: shaders not compiled (%s); colour modes off", err.c_str());
  }
  ID3D11Device* dev = g_devSeen.load();
  if (g_blobsOk && dev && dev != g_objDev && !g_obj.load()) {
    std::string err;
    if (g_objStore.Create(dev, g_blobs, err)) {
      g_objDev = dev;
      g_obj.store(&g_objStore, std::memory_order_release);
      if (g_log) {  // R97: its VRAM (made whenever the pass is wanted: a [Color] mode, VrFx, or the symbology export)
        double mb = 0;
        const int lvl[Objects::kTxPerEye] = {0, 0, 1, 1, 2};
        for (int k = 0; k < Objects::kTxPerEye; ++k) mb += 2.0 * Objects::LevelAlloc(lvl[k]) * Objects::LevelAlloc(lvl[k]) * 8;
        for (int l = 0; l < 3; ++l) mb += 1.0 * Objects::LevelAlloc(l) * Objects::LevelAlloc(l) * 8;
        g_log("colour: pass ready (pixel shader swap at each HMD view's ToneMappingPass draw); VRAM %.1f MB (the glare / "
              "night-vision textures, made with the pass; GazeDof's targets come when it runs)",
              mb / 1e6);
      }
    } else {
      g_blobsOk = false;
      if (g_log) g_log("colour: D3D objects not created (%s); colour modes off", err.c_str());
    }
  }
  // R95: the symbology export's shader variants (the config thread; the render thread uses them after g_symPsReady).
  {
    static bool symTried = false, symOk = false;
    static Blobs symBlobs;
    Objects* so = g_obj.load(std::memory_order_acquire);
    if (g_symWant.load() && so && g_symPsReady.load() == 0 && (!symTried || symOk)) {
      if (!symTried) {
        symTried = true;
        std::string err;
        symOk = CompileSym(symBlobs, err);
        if (!symOk && g_log) g_log("colour: symbology tone-map shaders not compiled (%s); no symbology export", err.c_str());
      }
      if (symOk) {
        bool made = true;
        for (int i = 0; i < 3 && made; ++i)
          made = SUCCEEDED(so->dev->CreatePixelShader(symBlobs.psSym[i].data(), symBlobs.psSym[i].size(), nullptr,
                                                      &so->psSym[i]));
        if (made) {
          g_symPsReady.store(1, std::memory_order_release);
          if (g_log) g_log("colour: symbology tone maps ready (the second evaluation in each HMD view's tone-map draw)");
        } else {
          for (auto*& p : so->psSym)
            if (p) p->Release(), p = nullptr;
          symOk = false;
          if (g_log) g_log("colour: symbology tone-map shaders not created; no symbology export");
        }
      }
    }
  }
  static ULONGLONG lastLog = 0;
  static uint64_t missLogged = 0;
  const ULONGLONG now = GetTickCount64();
  // R46: a missed HMD tone map (a stock, grey image in that view for a frame) is logged within a second.
  {
    const uint64_t mt = g_missTotal.load();
    if (mt != missLogged && g_log)
      g_log("colour: MISSED HMD tone-map swaps while mode %d%s on: %llu since start (per view since the last 10 s line "
            "%llu/%llu/%llu/%llu, view unknown %llu; why: rejected %llu, arm refused %llu, draw bypassed the hook %llu)",
            g_missMode.load(), g_missMode.load() == 0 ? " + VrFx" : "", static_cast<unsigned long long>(mt), static_cast<unsigned long long>(g_miss[0].load()),
            static_cast<unsigned long long>(g_miss[1].load()), static_cast<unsigned long long>(g_miss[2].load()),
            static_cast<unsigned long long>(g_miss[3].load()), static_cast<unsigned long long>(g_miss[4].load()),
            static_cast<unsigned long long>(g_missWhy[kMissReject].load()),
            static_cast<unsigned long long>(g_missWhy[kMissArm].load()),
            static_cast<unsigned long long>(g_missWhy[kMissBypass].load()));
    missLogged = mt;
  }
  if (now - lastLog >= 2000) g_rbWant = true;
  if (now - lastLog < 10000) return;
  lastLog = now;
  uint64_t sw[4], ms[5];
  for (int v = 0; v < 4; ++v) sw[v] = g_swaps[v].exchange(0);
  for (int v = 0; v < 5; ++v) ms[v] = g_miss[v].exchange(0);
  const uint64_t swu = g_swapUnknown.exchange(0);
  uint64_t why[kMissWhyCount];
  for (int i = 0; i < kMissWhyCount; ++i) why[i] = g_missWhy[i].exchange(0);
  const uint64_t pend = g_missPending.exchange(0);
  char rj[512] = {};
  int n = 0;
  for (int i = 0; i < kRjCount; ++i)
    if (const uint64_t k = g_rejects[i].exchange(0))
      n += snprintf(rj + n, sizeof(rj) - n, "%s%s %llu", n ? ", " : "", kRejectNames[i], static_cast<unsigned long long>(k));
  if (g_log)
    g_log("colour: mode %d, swapped draws per view %llu/%llu/%llu/%llu (view unknown %llu) in 10 s; not swapped: %s; "
          "missed per view %llu/%llu/%llu/%llu (unknown %llu, %llu since start; rejected %llu, arm refused %llu, draw "
          "bypassed %llu; while the pass was being created %llu); sun y %s%.3f",
          cfg.mode, static_cast<unsigned long long>(sw[0]), static_cast<unsigned long long>(sw[1]),
          static_cast<unsigned long long>(sw[2]), static_cast<unsigned long long>(sw[3]),
          static_cast<unsigned long long>(swu), n ? rj : "none", static_cast<unsigned long long>(ms[0]),
          static_cast<unsigned long long>(ms[1]), static_cast<unsigned long long>(ms[2]),
          static_cast<unsigned long long>(ms[3]), static_cast<unsigned long long>(ms[4]),
          static_cast<unsigned long long>(g_missTotal.load()), static_cast<unsigned long long>(why[kMissReject]),
          static_cast<unsigned long long>(why[kMissArm]), static_cast<unsigned long long>(why[kMissBypass]),
          static_cast<unsigned long long>(pend), sunValid ? "" : "unknown ", sunValid ? sunY : 0.0f);
  if (g_rbFresh.exchange(false) && g_log && cfg.mode)
    g_log("colour: exposure state: adapted %.5f (ours) / %.5f (DCS's, t1[1]) target %.5f, log-avg periphery %.5f focus "
          "%.5f, EC used %+.3f EV",
          g_rbAdapted.load(), g_rbDcs.load(), g_rbTarget.load(), g_rbPeri.load(), g_rbFocus.load(), g_rbEc.load());
  {  // R50: glare / night-vision state (only when something used it)
    uint64_t runs[2], cp[4], cm[4];
    for (int e = 0; e < 2; ++e) runs[e] = g_glareRuns[e].exchange(0);
    for (int v = 0; v < 4; ++v) cp[v] = g_projCopied[v].exchange(0), cm[v] = g_projMissing[v].exchange(0);
    const uint64_t big = g_glareTooBig.exchange(0);
    if (g_log && cfg.mode && (runs[0] | runs[1] | cp[0] | cm[0] | big)) {
      const cgrade::ModeParams& lp = cfg.mode == 2 ? cfg.enh : cfg.fix;
      const cgrade::ColorCb lc = cgrade::BuildCb(lp, sunValid, sunY, 0, 0, false);
      g_log("colour: night vision rod share %.2f (adaptation %.4g cd/m2 from the sun), glare %s: runs per eye %llu/%llu, "
            "periphery too large %llu; projection copies per view %llu/%llu/%llu/%llu, missing %llu/%llu/%llu/%llu; "
            "read back P00/P11/ox/oy v0 %.3f/%.3f/%+.3f/%+.3f v1 %.3f/%.3f/%+.3f/%+.3f v2 %.3f/%.3f/%+.3f/%+.3f "
            "v3 %.3f/%.3f/%+.3f/%+.3f, eye stamps %.0f/%.0f (-1 = projection not usable)",
            lc.nvW, sunValid ? cgrade::AdaptationCdm2(sunY) : 0.0, lp.glare ? "on" : "off",
            static_cast<unsigned long long>(runs[0]), static_cast<unsigned long long>(runs[1]),
            static_cast<unsigned long long>(big), static_cast<unsigned long long>(cp[0]),
            static_cast<unsigned long long>(cp[1]), static_cast<unsigned long long>(cp[2]),
            static_cast<unsigned long long>(cp[3]), static_cast<unsigned long long>(cm[0]),
            static_cast<unsigned long long>(cm[1]), static_cast<unsigned long long>(cm[2]),
            static_cast<unsigned long long>(cm[3]), g_rbProj[0][0].load(), g_rbProj[0][1].load(), g_rbProj[0][2].load(),
            g_rbProj[0][3].load(), g_rbProj[1][0].load(), g_rbProj[1][1].load(), g_rbProj[1][2].load(),
            g_rbProj[1][3].load(), g_rbProj[2][0].load(), g_rbProj[2][1].load(), g_rbProj[2][2].load(),
            g_rbProj[2][3].load(), g_rbProj[3][0].load(), g_rbProj[3][1].load(), g_rbProj[3][2].load(),
            g_rbProj[3][3].load(), g_rbStamp[0].load(), g_rbStamp[1].load());
    }
  }
  if (vrfx::Wanted() && g_log)  // R51
    g_log("vrfx: GazeDof %s: gain %.2f (max %.2f in 10 s), focus %.3f D, pupil %.1f mm; frames %llu, blurred %llu; "
          "releases: saccade %llu, new plane %llu, low confidence %llu, no gaze %llu; prep dispatches %llu, views "
          "without depth %llu / projection %llu / depth still bound %llu / targets %llu. GVision %s: Gy %.2f, loss "
          "%.2f (max %.2f in 10 s), red %.2f, mode %d (DCS G-EFFECT %d)",
          vrfx::g_wantDof.load() ? "on" : "off", g_fxGain.load(), g_fxGainMax.exchange(0), g_fxFocusD.load(),
          g_fxPupil.load(), static_cast<unsigned long long>(g_fxFrames.load()),
          static_cast<unsigned long long>(g_fxBlurFrames.load()), static_cast<unsigned long long>(g_fxSacc.load()),
          static_cast<unsigned long long>(g_fxPlanes.load()), static_cast<unsigned long long>(g_fxLowConf.load()),
          static_cast<unsigned long long>(g_fxNoGaze.load()), static_cast<unsigned long long>(g_fxPrep.exchange(0)),
          static_cast<unsigned long long>(g_fxNoDepth.exchange(0)), static_cast<unsigned long long>(g_fxNoProj.exchange(0)),
          static_cast<unsigned long long>(g_fxDepthBound.exchange(0)),
          static_cast<unsigned long long>(g_fxNoTargets.exchange(0)), vrfx::g_wantGv.load() ? "on" : "off",
          g_fxGz.load(), g_fxLoss.load(), g_fxLossMax.exchange(0), g_fxRed.load(),
          vrfx::EffectiveGvMode(g_fx.p.gvMode, vrfx::g_dcsGEffect.load()), vrfx::g_dcsGEffect.load());
  if (cfg.timing) {
    const uint64_t f = g_timedFrames.exchange(0), lost = g_lostSets.exchange(0);
    const double us = g_gpuUs.exchange(0.0);
    double pv[4];
    for (int v = 0; v < 4; ++v) pv[v] = g_gpuViewUs[v].exchange(0.0);
    if (g_log && f)
      g_log("colour: GPU time of the four tone-map draws (mode %d%s): %.4f ms per frame (views %.4f/%.4f/%.4f/%.4f ms), "
            "%llu frames, %llu sets not ready",
            cfg.mode, cfg.mode ? ", our shader and dispatches included" : ", DCS's shader", us / 1000.0 / f,
            pv[0] / 1000.0 / f, pv[1] / 1000.0 / f, pv[2] / 1000.0 / f, pv[3] / 1000.0 / f,
            static_cast<unsigned long long>(f), static_cast<unsigned long long>(lost));
  }
}

// Payload stop (config thread, after the render-thread hooks are out).
inline void Shutdown() {
  g_symPsReady = 0;  // R95
  g_obj.store(nullptr);
  for (int v = 0; v < 4; ++v) g_fxProjSeen[v] = false, g_fxView[v] = FxViewIn{};  // R51
  g_objStore.Release();
  g_objDev = nullptr;
  if (ID3D11Device* d = g_devSeen.exchange(nullptr)) d->Release();
}

}  // namespace cpass
