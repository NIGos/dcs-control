// R42 [Color] Mode (Fix / Enhanced): pure parts (parameters, ini, weights, constant buffer, CPU reference against
// DCS's curve and the design goals) and the pass on WARP (pixel shader swap at a stand-in of DCS's tone-map draw:
// GPU against the CPU reference, state restored, EC equalised, adaptation, meters, rejects, timing).
#pragma once

namespace cmtest {

// R107: stand-ins for gpu_dedup.h's Map / Unmap hooks (archived): functions of this payload, never called (distinct
// bodies, so the linker cannot fold them into one address).
int g_r107MapCalls = 0, g_r107UnmapCalls = 0;
void R107PayloadMapHook() { ++g_r107MapCalls; }
void R107PayloadUnmapHook() { g_r107UnmapCalls += 2; }

inline uint16_t FloatToHalf(float f) {
  uint32_t x;
  memcpy(&x, &f, 4);
  const uint32_t s = (x >> 16) & 0x8000u;
  int e = static_cast<int>((x >> 23) & 0xff) - 127 + 15;
  uint32_t m = x & 0x7fffffu;
  if (e <= 0) return static_cast<uint16_t>(s);  // (no denormals needed here)
  if (e >= 31) return static_cast<uint16_t>(s | 0x7c00u);
  uint32_t h = s | (static_cast<uint32_t>(e) << 10) | (m >> 13);
  if ((m & 0x1fffu) > 0x1000u || ((m & 0x1fffu) == 0x1000u && (h & 1u))) ++h;
  return static_cast<uint16_t>(h);
}
inline double HueDeg(const float c[3]) {
  const double h = std::atan2(std::sqrt(3.0) * (c[1] - c[2]), 2.0 * c[0] - c[1] - c[2]) * 180.0 / 3.14159265358979;
  return h < 0 ? h + 360 : h;
}
inline double SatOf(const float c[3]) {
  const float mx = std::max(c[0], std::max(c[1], c[2])), mn = std::min(c[0], std::min(c[1], c[2]));
  return mx > 1e-9f ? (mx - mn) / mx : 0.0;
}
inline double LumOf(const float c[3]) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }

// The display-linear light a value written by the pass shows on the sRGB-decoding swapchain.
inline double Shown(float v) { return cgrade::SrgbEotf(cgrade::ToUnorm8(v) / 255.0); }

// Tone-mapped (display-linear) output of the CPU reference for a scene colour at exposure 1 (adapted 0.18, EC 0),
// before any encode: the curve stage only, by running Shade with sRGB / 2.2 encodes undone.
inline void TmOf(const cgrade::DcsTone& t, const cgrade::ColorCb* c, const float rgb[3], float out[3]) {
  cgrade::ShadeIn in;
  for (int i = 0; i < 3; ++i) in.scene[i] = rgb[i] / (1 - t.bloomLerp);
  in.adapted = 0.18f;
  in.x = 3, in.y = 5;
  cgrade::ColorCb cc{};
  if (c) cc = *c;
  cc.srgb = 0, cc.ditherMode = 1, cc.frame = 0;  // 2.2 encode, no DCS dither; fine dither is removed below
  if (!c) cc = cgrade::BuildCb(cgrade::ModeParams{}, false, 0, 0, 0, false), cc.srgb = 0, cc.ditherMode = 1;
  float v[3];
  cgrade::Shade(t, &cc, in, v);
  for (int i = 0; i < 3; ++i) {
    const float d = (v[i] > 0 && v[i] < 1) ? (cgrade::Ign(in.x, in.y, 0) - 0.5f) * (0.98f / 255.0f) : 0.0f;
    out[i] = std::pow(std::max(v[i] - d, 0.0f), 2.2f);
  }
}

inline void PureTests() {
  using namespace cgrade;
  // Defaults: a missing ini gives each mode's defaults; Mode itself defaults to 0 (nothing armed).
  {
    const ModeParams f = ReadModeIni(L"Z:\\no\\such\\dir\\none.ini", 1), e = ReadModeIni(L"Z:\\no\\such\\dir\\none.ini", 2);
    Check(SameMode(f, SanitizeMode(FixDefaults())) && SameMode(e, SanitizeMode(EnhancedDefaults())),
          "colour mode: a missing ini gives the Fix / Enhanced defaults");
    Check(f.srgbMatch == 1 && f.equalizeEc == 1 && !OwnCurve(f) && f.hueKeep == 0 && f.saturation == 1 && f.warmth == 0 &&
              f.darkDesat == 1 && f.darkDesatDay == 1 && !UsesState(f) && f.nightFloor == 0 && f.dither == 2,
          "colour mode: Fix changes only the encode and the EC split (DCS's curve, colour, adaptation kept)");
    Check(OwnCurve(e) && e.hueKeep > 0 && UsesState(e) && e.warmth > 0 && e.dither == 1,
          "colour mode: Enhanced has its own curve, hue keep, adaptation, warmth, fine dither");
    Check(cpass::g_mode.load() == 0 && cpass::g_timing.load() == 0 && !cpass::Wanted() && cpass::g_obj.load() == nullptr,
          "colour mode: off by default (Mode 0, Timing 0: nothing wanted, nothing created)");
  }
  // Ini sections and clamps.
  {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring ini = std::wstring(tmp) + L"qv_r42_color_test.ini";
    WritePrivateProfileStringW(L"ColorEnhanced", L"Saturation", L"1.3", ini.c_str());
    WritePrivateProfileStringW(L"ColorEnhanced", L"Slope", L"0.95", ini.c_str());
    WritePrivateProfileStringW(L"ColorEnhanced", L"NightFloor", L"0.5", ini.c_str());
    WritePrivateProfileStringW(L"ColorEnhanced", L"Dither", L"7", ini.c_str());
    WritePrivateProfileStringW(L"ColorFix", L"Toe", L"0.6", ini.c_str());
    WritePrivateProfileStringW(L"ColorFix", L"HueKeep", L"-1", ini.c_str());
    WritePrivateProfileStringW(L"ColorFix", L"NightFloor", L"0.015", ini.c_str());
    const ModeParams e = ReadModeIni(ini.c_str(), 2), f = ReadModeIni(ini.c_str(), 1);
    DeleteFileW(ini.c_str());
    Check(std::fabs(e.saturation - 1.3) < 1e-9 && std::fabs(e.slope - 0.95) < 1e-9 && std::fabs(e.toe - 0.56) < 1e-9 &&
              e.nightFloor == 0.05 && e.dither == 2,
          "colour mode: [ColorEnhanced] keys read, unset keys keep Enhanced's defaults, out-of-range values clamped");
    Check(OwnCurve(f) && std::fabs(f.toe - 0.6) < 1e-9 && std::fabs(f.slope - 0.865) < 1e-9 &&
              std::fabs(f.shoulder - 0.322) < 1e-9 && std::fabs(f.whiteClip - 0.04) < 1e-9 && f.hueKeep == 0 &&
              std::fabs(f.nightFloor - 0.015) < 1e-9,
          "colour mode: [ColorFix] one curve key takes DCS's stock values for the others; HueKeep clamped");
    ModeParams bad;
    bad.exposureEv = std::nan(""), bad.saturation = 9, bad.adaptDark = -3, bad.meterFocus = 4;
    const ModeParams s = SanitizeMode(bad);
    Check(s.exposureEv == 0 && s.saturation == 1.5 && s.adaptDark == 0 && s.meterFocus == 1,
          "colour mode: NaN / out-of-range parameters sanitised");
  }
  // Sun weights and the constant buffer.
  {
    Check(NightWeightOf(0.3) == 0 && NightWeightOf(0.05) == 0 && NightWeightOf(-0.1) == 1 && NightWeightOf(-0.5) == 1 &&
              std::fabs(NightWeightOf(-0.025) - 0.5) < 1e-9,
          "colour mode: night weight 0 at sun y >= 0.05, 1 at <= -0.10 (night tone's gate)");
    Check(WarmWeightOf(0.33) == 0 && WarmWeightOf(0.055) == 1 && WarmWeightOf(-0.2) == 0 &&
              std::fabs(WarmWeightOf(0.2) - 0.5) < 1e-9 && std::fabs(WarmWeightOf(-0.05) - 0.5) < 1e-9,
          "colour mode: warm weight 1 at golden hour (sun 0-6 deg), 0 at 19 deg (R41 day) and at night");
    ModeParams p = FixDefaults();
    p.nightFloor = 0.02;
    const ColorCb day = BuildCb(p, true, 0.3, 1, 0.01, false), night = BuildCb(p, true, -0.3, 1, 0.01, false);
    const ColorCb unknown = BuildCb(p, false, -0.3, 1, 0.01, false);
    p.nightGate = 0;
    const ColorCb ungated = BuildCb(p, true, 0.3, 1, 0.01, false);
    Check(std::fabs(day.floorLum - 0.01f) < 1e-6f && std::fabs(night.floorLum - 0.02f) < 1e-6f &&
              std::fabs(unknown.floorLum - 0.01f) < 1e-6f && std::fabs(ungated.floorLum - 0.02f) < 1e-6f,
          "colour mode: NightFloor gated by the sun (stock 0.01 by day or sun unknown), ungated with NightGate 0");
    {
      ModeParams q = FixDefaults();
      q.nightEv = 0.5;
      Check(BuildCb(q, true, -0.3, 1, 0.01, false).evOffset == 0.5f && BuildCb(q, true, 0.3, 1, 0.01, false).evOffset == 0.0f &&
                BuildCb(q, false, -0.3, 1, 0.01, false).evOffset == 0.0f,
            "colour mode: NightEV only at night (sun-gated; off with the sun unknown)");
    }
    const ModeParams e = EnhancedDefaults();
    ModeParams eNoNv = e;
    eNoNv.nightVision = 0;
    const ColorCb ed = BuildCb(e, true, 0.3, 1, 0.01, false), en = BuildCb(eNoNv, true, -0.3, 1, 0.01, false),
                  eg = BuildCb(e, true, 0.05, 1, 0.01, false), env = BuildCb(e, true, -0.3, 1, 0.01, false);
    Check(std::fabs(ed.desatAmt - 0.3f) < 1e-6f && std::fabs(en.desatAmt - 1.0f) < 1e-6f &&
              std::fabs(env.desatAmt - 1.0f) < 1e-6f,
          "colour mode: Enhanced's dark desaturation 0.3 by day, DCS's full amount at night (R50: kept with night "
          "vision on; the rod image is blended in after it)");
    const double gl = eg.warmR * 0.2126 + eg.warmG * 0.7152 + eg.warmB * 0.0722;
    Check(std::fabs(gl - 1) < 1e-5 && eg.warmR > 1.01f && eg.warmB < 0.97f && ed.warmR == 1.0f && ed.warmB == 1.0f,
          "colour mode: warmth luminance-neutral, on at golden hour, off at 19 deg");
    Check(BuildCb(e, false, 0, 7, 9.0, true).dt == 0.25f && BuildCb(e, false, 0, 7, 9.0, true).reset == 1.0f &&
              BuildCb(e, false, 0, 7, 0.01, false).useState == 1.0f && BuildCb(FixDefaults(), false, 0, 7, 0.01, false).useState == 0.0f,
          "colour mode: dt clamped, reset flag, adaptation state only when a mode uses it");
  }
  // CPU reference: stock emulation is DCS's shader; Fix shows DCS's intent; Enhanced keeps hues.
  {
    DcsTone t;
    ModeParams stock = FixDefaults();
    stock.srgbMatch = 0, stock.equalizeEc = 0, stock.dither = 0;
    const ColorCb sc = BuildCb(stock, false, 0, 3, 0, false);
    bool same = true;
    for (int k = 0; k < 400; ++k) {
      ShadeIn in;
      const float s = 0.18f * std::exp2(-10.0f + k * 0.04f);
      in.scene[0] = s, in.scene[1] = s * (0.3f + (k % 7) * 0.1f), in.scene[2] = s * (0.2f + (k % 5) * 0.2f);
      in.x = k % 13, in.y = k % 11;
      in.adapted = 0.05f + (k % 9) * 0.03f, in.ec = 0.3f;
      float a[3], b[3];
      Shade(t, nullptr, in, a);
      Shade(t, &sc, in, b);
      for (int i = 0; i < 3; ++i) same &= a[i] == b[i];
    }
    Check(same, "colour mode: stock parameters reproduce DCS's shader exactly (CPU reference)");
    // DCS's curve: mid grey (exposure 1) -> byte 120 (R40 table), true black stays 0.
    ShadeIn mid;
    mid.scene[0] = mid.scene[1] = mid.scene[2] = 0.18f / (1 - t.bloomLerp);
    mid.adapted = 0.18f;
    mid.x = 0, mid.y = 0;
    float o[3];
    Shade(t, nullptr, mid, o);
    const int midByte = ToUnorm8(o[1]);
    ShadeIn blk;
    float ob[3], of[3], oe[3];
    const ColorCb fc = BuildCb(FixDefaults(), false, 0, 5, 0, false), ec = BuildCb(EnhancedDefaults(), false, 0, 5, 0, false);
    Shade(t, nullptr, blk, ob);
    Shade(t, &fc, blk, of);
    Shade(t, &ec, blk, oe);
    Check(midByte >= 119 && midByte <= 121 && ob[0] == 0 && of[0] == 0 && oe[0] == 0 && of[2] == 0 && oe[2] == 0,
          "colour mode: DCS's mid grey -> byte 120; black stays exactly 0 in every mode (fine dither too)");
    ShadeIn wht;
    wht.scene[0] = wht.scene[1] = wht.scene[2] = 1000.0f;
    Shade(t, &fc, wht, of);
    Shade(t, &ec, wht, oe);
    Check(ToUnorm8(of[0]) == 255 && ToUnorm8(oe[1]) == 255, "colour mode: white stays 255 (no dither below peak)");
    // Fix: the light shown (sRGB decode) equals DCS's intent (its 2.2 power) within one code of the encode; stock is
    // lifted near black.
    int bad = 0, lifted = 0, n = 0;
    double worstMid = 0;
    for (int k = 0; k < 300; ++k) {
      ShadeIn in;
      const float s = 0.18f * std::exp2(-9.0f + k * 0.04f);
      in.scene[0] = in.scene[1] = in.scene[2] = s;
      in.adapted = 0.18f;
      in.x = 0, in.y = 0;  // Bayer 0 / 64
      float st[3], fx[3];
      Shade(t, nullptr, in, st);
      Shade(t, &fc, in, fx);
      const double intent = std::pow(std::max(0.0f, st[1]), 2.2);  // DCS's display-linear (after its own dither)
      const int code = ToUnorm8(fx[1]);
      const double lo = SrgbEotf(std::max(0, code - 1) / 255.0), hi = SrgbEotf(std::min(255, code + 1) / 255.0);
      bad += !(intent >= lo - 1e-9 && intent <= hi + 1e-9);
      if (intent > 2e-5 && intent < 2e-3) {
        ++n;
        lifted += Shown(st[1]) > 2.0 * intent;
      }
      if (ToUnorm8(st[1]) >= 96) worstMid = std::max(worstMid, std::fabs(Shown(fx[1]) / std::max(Shown(st[1]), 1e-9) - 1));
    }
    Check(bad == 0, "colour mode: Fix shows DCS's intended light within one code everywhere");
    Check(n > 20 && lifted == n, "colour mode: stock shows near-black at more than 2x DCS's intent (the lift Fix removes)");
    Check(worstMid < 0.03, "colour mode: Fix leaves codes >= 96 within 3 % of stock's shown light");
    // Enhanced: hue kept for an orange from -2 to +2 EV (stock drifts toward yellow); HUD green stays bright and green.
    const float orange[3] = {1.0f, 0.45f, 0.15f}, hud[3] = {0.1f, 1.0f, 0.1f};
    double maxDriftE = 0, driftS2 = 0, driftE2 = 0, hudLumRatio = 9, hudSat2 = 0;
    for (int ev = -2; ev <= 3; ++ev) {
      float x[3], te[3], ts[3], h[3], he[3], hs[3];
      const double k = 0.18 * std::exp2(ev) / LumOf(orange), kh = 0.18 * std::exp2(ev) / LumOf(hud);
      for (int i = 0; i < 3; ++i) x[i] = static_cast<float>(orange[i] * k), h[i] = static_cast<float>(hud[i] * kh);
      TmOf(t, &ec, x, te);
      TmOf(t, nullptr, x, ts);
      TmOf(t, &ec, h, he);
      TmOf(t, nullptr, h, hs);
      if (ev >= 0 && ev <= 2) maxDriftE = std::max(maxDriftE, std::fabs(HueDeg(te) - HueDeg(orange)));
      if (ev == 2)
        driftS2 = std::fabs(HueDeg(ts) - HueDeg(orange)), driftE2 = std::fabs(HueDeg(te) - HueDeg(orange)), hudSat2 = SatOf(he);
      if (ev >= 1) hudLumRatio = std::min(hudLumRatio, LumOf(he) / LumOf(hs));
    }
    Check(maxDriftE < 6.0 && driftS2 > 15.0 && driftE2 < 0.3 * driftS2,
          "colour mode: Enhanced keeps an orange's hue from 0 to +2 EV (< 6 deg, < 30 % of DCS's per-channel drift of > 15 deg)");
    Check(hudLumRatio > 0.95 && hudSat2 > 0.8, "colour mode: Enhanced keeps a bright HUD green as bright (>= 95 %) and green");
  }
}

// --- WARP rig: a stand-in of DCS's tone-map draw ---
constexpr UINT kW = 64, kH = 48;
struct Rig {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11Texture2D* hdr = nullptr;
  ID3D11ShaderResourceView* hdrSrv = nullptr;
  ID3D11Buffer* lum = nullptr;
  ID3D11ShaderResourceView* lumSrv = nullptr;
  ID3D11Texture2D* bloom = nullptr;
  ID3D11ShaderResourceView* bloomSrv = nullptr;
  ID3D11Buffer* b0[2] = {};  // view 0's (EC 0.861) and the others' (EC 0.941)
  ID3D11Texture2D* rt = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11Texture2D* rtStage = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* dcsPs = nullptr;
  ID3D11RasterizerState* cullNone = nullptr;  // DCS's tone map: cullNone
  ID3D11Buffer* sentCb = nullptr;
  ID3D11ShaderResourceView* sentSrv = nullptr;
  ID3D11Buffer* cb7[4] = {};  // R50: DCS's cPerView per view (gProj at byte 256), bound at VS b7 by DrawView
  bool bindB7 = true;
  std::vector<float> scene;  // the HDR as the GPU holds it (half-rounded), RGB
  cgrade::DcsTone tone[2];
  cpass::Objects obj;
  void Release() {
    auto rel = [](auto*& p) {
      if (p) p->Release();
      p = nullptr;
    };
    obj.Release();
    rel(hdrSrv), rel(hdr), rel(lumSrv), rel(lum), rel(bloomSrv), rel(bloom), rel(b0[0]), rel(b0[1]), rel(rtv), rel(rt);
    rel(rtStage), rel(vs), rel(dcsPs), rel(cullNone), rel(sentCb), rel(sentSrv);
    for (auto& b : cb7) rel(b);
    rel(ctx), rel(dev);
  }
};

inline const char kDcsVs[] = R"(
struct VSO { noperspective float4 pos : SV_POSITION0; noperspective float4 projPos : TEXCOORD0; noperspective float3 worldPos : TEXCOORD1; };
VSO main(uint vid : SV_VertexID) {
  const float2 quad[4] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  VSO o;
  o.pos = float4(quad[vid], 0, 1);
  o.projPos.xy = float2(o.pos.x, -o.pos.y) * 0.5 + 0.5;
  o.projPos.zw = o.projPos.xy;
  o.worldPos = float3(0, 0, 1);
  return o;
})";
inline const char kDcsPs[] = R"(
struct VSO { noperspective float4 pos : SV_POSITION0; noperspective float4 projPos : TEXCOORD0; noperspective float3 worldPos : TEXCOORD1; };
float4 main(VSO i) : SV_Target0 { return float4(1, 0, 1, 1); })";

inline bool Compile(const char* src, const char* target, std::vector<uint8_t>& out) {
  std::string err;
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  HMODULE m = LoadLibraryW((std::wstring(sys) + L"\\d3dcompiler_47.dll").c_str());
  const pD3DCompile c = m ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
  return c && cpass::CompileOne(c, src, target, nullptr, out, err);
}

// R50: the rig's projections: peripheries P00 0.78 (square pixels), mirrored centres; focus views a 0.5 section at
// 2x density centred off-axis (mirrored), as QVFR's focus views are sub-frusta of the eye's periphery.
inline void RigProj(int v, float q[4]) {
  const float p00 = 0.78f, p11 = 0.78f * kW / kH, sgn = (v & 1) ? -1.0f : 1.0f;
  const float pox = 0.08f * sgn;
  if (v < 2) {
    q[0] = p00, q[1] = p11, q[2] = pox, q[3] = 0;
    return;
  }
  const float cx = 0.25f * sgn, cy = -0.15f;  // focus centre in the periphery's NDC
  const float f00 = p00 / 0.5f, f11 = p11 / 0.5f;
  q[0] = f00, q[1] = f11, q[2] = -((cx - pox) / p00) * f00, q[3] = -(cy / p11) * f11;
}
inline void SetLum(Rig& r, float adapted, float meter) {
  float d[16] = {0, 1, adapted, 0, 0, 0, 0, 0, meter, meter * meter, 0, 0, 0, 0, 0, 0};
  r.ctx->UpdateSubresource(r.lum, 0, nullptr, d, 0, 0);
}

inline bool MakeRig(Rig& r) {
  const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &r.dev, nullptr,
                               &r.ctx)))
    return false;
  // HDR: rows = colours, columns = exposure -10 .. +6 EV around mid grey.
  const float cols[8][3] = {{1, 1, 1},       {1, 0.45f, 0.15f}, {1, 0.12f, 0.08f}, {0.35f, 0.55f, 1},
                            {0.1f, 1, 0.1f}, {1, 0.95f, 0.85f}, {0.2f, 0.3f, 0.1f}, {0.6f, 0.6f, 0.9f}};
  std::vector<uint16_t> half(kW * kH * 4);
  r.scene.assign(kW * kH * 3, 0);
  for (UINT y = 0; y < kH; ++y)
    for (UINT x = 0; x < kW; ++x) {
      const float* c = cols[(y / 6) % 8];
      const float l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
      const float s = 0.05f * std::exp2(-10.0f + 16.0f * x / (kW - 1)) / l * (1.0f + 0.01f * (y % 6));
      for (int i = 0; i < 4; ++i) {
        const float v = i < 3 ? (x == 0 && y % 6 == 0 ? 0.0f : c[i] * s) : 1.0f;
        half[(y * kW + x) * 4 + i] = FloatToHalf(v);
        if (i < 3) r.scene[(y * kW + x) * 3 + i] = cgrade::HalfToFloat(half[(y * kW + x) * 4 + i]);
      }
    }
  D3D11_TEXTURE2D_DESC td{};
  td.Width = kW, td.Height = kH, td.MipLevels = 1, td.ArraySize = 1, td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT, td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  D3D11_SUBRESOURCE_DATA sd{half.data(), kW * 8, 0};
  if (FAILED(r.dev->CreateTexture2D(&td, &sd, &r.hdr)) || FAILED(r.dev->CreateShaderResourceView(r.hdr, nullptr, &r.hdrSrv)))
    return false;
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = 64, bd.Usage = D3D11_USAGE_DEFAULT, bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, bd.StructureByteStride = 8;
  if (FAILED(r.dev->CreateBuffer(&bd, nullptr, &r.lum)) || FAILED(r.dev->CreateShaderResourceView(r.lum, nullptr, &r.lumSrv)))
    return false;
  SetLum(r, 0.18f, 0.18f);
  D3D11_TEXTURE2D_DESC bt = td;
  bt.Width = bt.Height = 1, bt.Format = DXGI_FORMAT_R8_UNORM, bt.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  const uint8_t zero[4] = {};
  D3D11_SUBRESOURCE_DATA bz{zero, 4, 0};
  if (FAILED(r.dev->CreateTexture2D(&bt, &bz, &r.bloom)) || FAILED(r.dev->CreateShaderResourceView(r.bloom, nullptr, &r.bloomSrv)))
    return false;
  r.tone[0].ec = 0.861f, r.tone[1].ec = 0.941f;
  for (int i = 0; i < 2; ++i) {  // DYNAMIC, as a Map-updated effect constant buffer
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = 256, cd.Usage = D3D11_USAGE_DYNAMIC, cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(r.dev->CreateBuffer(&cd, nullptr, &r.b0[i]))) return false;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(r.ctx->Map(r.b0[i], 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    cgrade::PackDcsTone(r.tone[i], static_cast<float*>(m.pData));
    r.ctx->Unmap(r.b0[i], 0);
  }
  D3D11_TEXTURE2D_DESC rd = td;
  rd.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS, rd.BindFlags = D3D11_BIND_RENDER_TARGET;  // as DCS's LDR image: UNORM view
  D3D11_RENDER_TARGET_VIEW_DESC rvd{};
  rvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM, rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  if (FAILED(r.dev->CreateTexture2D(&rd, nullptr, &r.rt)) || FAILED(r.dev->CreateRenderTargetView(r.rt, &rvd, &r.rtv)))
    return false;
  rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM, rd.Usage = D3D11_USAGE_STAGING, rd.BindFlags = 0, rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(r.dev->CreateTexture2D(&rd, nullptr, &r.rtStage))) return false;
  std::vector<uint8_t> b;
  if (!Compile(kDcsVs, "vs_5_0", b) || FAILED(r.dev->CreateVertexShader(b.data(), b.size(), nullptr, &r.vs))) return false;
  if (!Compile(kDcsPs, "ps_5_0", b) || FAILED(r.dev->CreatePixelShader(b.data(), b.size(), nullptr, &r.dcsPs))) return false;
  D3D11_RASTERIZER_DESC rs{};
  rs.FillMode = D3D11_FILL_SOLID, rs.CullMode = D3D11_CULL_NONE, rs.DepthClipEnable = TRUE;
  if (FAILED(r.dev->CreateRasterizerState(&rs, &r.cullNone))) return false;
  D3D11_BUFFER_DESC sc{};
  sc.ByteWidth = 16, sc.Usage = D3D11_USAGE_DEFAULT, sc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  if (FAILED(r.dev->CreateBuffer(&sc, nullptr, &r.sentCb))) return false;
  if (FAILED(r.dev->CreateShaderResourceView(r.bloom, nullptr, &r.sentSrv))) return false;
  for (int v = 0; v < 4; ++v) {  // R50: the views' projections (RigProj)
    float data[128] = {};
    float q[4];
    RigProj(v, q);
    cgrade::PackProj(q[0], q[1], q[2], q[3], data + 64);
    D3D11_BUFFER_DESC pd{};
    pd.ByteWidth = 512, pd.Usage = D3D11_USAGE_DEFAULT, pd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA pdd{data, 0, 0};
    if (FAILED(r.dev->CreateBuffer(&pd, &pdd, &r.cb7[v]))) return false;
  }
  cpass::Blobs blobs;
  std::string err;
  if (!cpass::CompileAll(blobs, err)) {
    printf("%s\n", err.c_str());
    return false;
  }
  return r.obj.Create(r.dev, blobs, err);
}

// DCS's state for view v's tone-map draw, then Pre / Draw / Post; returns Pre's result.
inline int DrawView(Rig& r, int v, DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN, ID3D11ShaderResourceView* extra = nullptr) {
  ID3D11DeviceContext* c = r.ctx;
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  c->IASetInputLayout(nullptr);
  c->VSSetShader(r.vs, nullptr, 0);
  c->PSSetShader(r.dcsPs, nullptr, 0);
  ID3D11ShaderResourceView* s[3] = {r.hdrSrv, r.lumSrv, r.bloomSrv};
  c->PSSetShaderResources(0, 3, s);
  c->PSSetShaderResources(5, 1, &extra);
  c->PSSetConstantBuffers(0, 1, &r.b0[v == 0 ? 0 : 1]);
  c->PSSetConstantBuffers(cpass::kPsCbSlot, 1, &r.sentCb);
  c->PSSetShaderResources(cpass::kPsStateSlot, 1, &r.sentSrv);
  {
    ID3D11Buffer* b7 = r.bindB7 ? r.cb7[v >= 0 && v < 4 ? v : 0] : nullptr;
    c->VSSetConstantBuffers(7, 1, &b7);
  }
  ID3D11RenderTargetView* rtv = r.rtv;
  ID3D11RenderTargetView* other = nullptr;
  if (rtvFormat != DXGI_FORMAT_UNKNOWN) {
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    d.Format = rtvFormat, d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    r.dev->CreateRenderTargetView(r.rt, &d, &other);
    rtv = other;
  }
  c->OMSetRenderTargets(1, &rtv, nullptr);
  const D3D11_VIEWPORT vp{0, 0, static_cast<float>(kW), static_cast<float>(kH), 0, 1};
  c->RSSetViewports(1, &vp);
  c->RSSetState(r.cullNone);
  const int act = cpass::ToneDrawPre(c, v);
  c->Draw(4, 0);
  cpass::ToneDrawPost(c, v, act);
  ID3D11RenderTargetView* none = nullptr;
  c->OMSetRenderTargets(1, &none, nullptr);
  if (other) other->Release();
  ID3D11ShaderResourceView* n1 = nullptr;
  c->PSSetShaderResources(5, 1, &n1);
  return act;
}
inline std::vector<uint8_t> ReadRt(Rig& r) {
  r.ctx->CopyResource(r.rtStage, r.rt);
  D3D11_MAPPED_SUBRESOURCE m{};
  std::vector<uint8_t> out(kW * kH * 4);
  if (SUCCEEDED(r.ctx->Map(r.rtStage, 0, D3D11_MAP_READ, 0, &m))) {
    for (UINT y = 0; y < kH; ++y) memcpy(&out[y * kW * 4], static_cast<const uint8_t*>(m.pData) + y * m.RowPitch, kW * 4);
    r.ctx->Unmap(r.rtStage, 0);
  }
  return out;
}
inline std::vector<float> ReadState(Rig& r) {
  std::vector<float> s(cpass::kStateBytes / 4, 0);
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = cpass::kStateBytes, d.Usage = D3D11_USAGE_STAGING, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer* st = nullptr;
  if (FAILED(r.dev->CreateBuffer(&d, nullptr, &st))) return s;
  r.ctx->CopyResource(st, r.obj.state);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(r.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
    memcpy(s.data(), m.pData, cpass::kStateBytes);
    r.ctx->Unmap(st, 0);
  }
  st->Release();
  return s;
}
// GPU bytes against the CPU reference: worst channel difference and the share within 1 code.
inline void Compare(Rig& r, const std::vector<uint8_t>& gpu, const cgrade::ColorCb* cb, int toneIdx, float adapted, float ec,
                    int* worst, double* within1) {
  int w = 0, ok = 0, n = 0;
  for (UINT y = 0; y < kH; ++y)
    for (UINT x = 0; x < kW; ++x) {
      cgrade::ShadeIn in;
      for (int i = 0; i < 3; ++i) in.scene[i] = r.scene[(y * kW + x) * 3 + i];
      in.adapted = adapted, in.ec = ec, in.x = x, in.y = y;
      float o[3];
      cgrade::Shade(r.tone[toneIdx], cb, in, o);
      for (int i = 0; i < 3; ++i) {
        const int d = std::abs(static_cast<int>(gpu[(y * kW + x) * 4 + i]) - cgrade::ToUnorm8(o[i]));
        w = std::max(w, d);
        ok += d <= 1;
        ++n;
      }
    }
  *worst = w;
  *within1 = n ? static_cast<double>(ok) / n : 0;
  printf("     (GPU vs CPU: worst %d codes, %.4f within 1)\n", w, *within1);
  if (w > 1) {
    int shown = 0;
    for (UINT y = 0; y < kH && shown < 6; ++y)
      for (UINT x = 0; x < kW && shown < 6; ++x) {
        cgrade::ShadeIn in;
        for (int i = 0; i < 3; ++i) in.scene[i] = r.scene[(y * kW + x) * 3 + i];
        in.adapted = adapted, in.ec = ec, in.x = x, in.y = y;
        float o[3];
        cgrade::Shade(r.tone[toneIdx], cb, in, o);
        for (int i = 0; i < 3; ++i)
          if (std::abs(static_cast<int>(gpu[(y * kW + x) * 4 + i]) - cgrade::ToUnorm8(o[i])) > 1 && shown < 6) {
            printf("     px %u,%u ch %d scene %g gpu %d cpu %d (%.4f)\n", x, y, i, in.scene[i], gpu[(y * kW + x) * 4 + i], cgrade::ToUnorm8(o[i]), o[i]);
            ++shown;
          }
      }
  }
}
inline void SetParams(const cgrade::ModeParams& p, int mode) {
  {
    std::lock_guard<std::mutex> lock(cpass::g_paramMutex);
    cpass::g_params[mode] = p;
  }
  cpass::g_paramVer++;
}

// ---------------------------------------------------------------------------
// R50: physiological night vision and glare (Enhanced). Pure parts, CPU model properties, and the pass on WARP
// against the CPU reference (glare chain, composite, focus mapping, stereo, fallbacks, state restore).
// ---------------------------------------------------------------------------
inline void NvOut(const cgrade::ColorCb& c, const float in[3], float out[3]) {
  for (int i = 0; i < 3; ++i) out[i] = in[i];
  cgrade::NightVision(c, in, in, out);
}

inline void R50PureTests() {
  using namespace cgrade;
  {
    const ModeParams f = FixDefaults(), e = EnhancedDefaults();
    Check(f.nightVision == 0 && f.glare == 0 && e.nightVision == 1 && e.glare == 1,
          "R50: night vision and glare on in Enhanced (night vision sun-gated; glare since R55), both off in Fix");
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring ini = std::wstring(tmp) + L"qv_r50_color_test.ini";
    WritePrivateProfileStringW(L"ColorEnhanced", L"NightVision", L"0", ini.c_str());
    WritePrivateProfileStringW(L"ColorEnhanced", L"NightVisionStrength", L"2", ini.c_str());
    WritePrivateProfileStringW(L"ColorEnhanced", L"NightVisionPhotopic", L"0.001", ini.c_str());
    WritePrivateProfileStringW(L"ColorEnhanced", L"Glare", L"1", ini.c_str());
    WritePrivateProfileStringW(L"ColorEnhanced", L"GlareRadius", L"0.2", ini.c_str());
    WritePrivateProfileStringW(L"ColorEnhanced", L"GlareThresholdEV", L"5", ini.c_str());
    WritePrivateProfileStringW(L"ColorEnhanced", L"GlareStreak", L"nonsense", ini.c_str());
    WritePrivateProfileStringW(L"ColorFix", L"Glare", L"1", ini.c_str());
    WritePrivateProfileStringW(L"ColorFix", L"NightVision", L"1", ini.c_str());
    const ModeParams ei = ReadModeIni(ini.c_str(), 2), fi = ReadModeIni(ini.c_str(), 1);
    DeleteFileW(ini.c_str());
    Check(ei.nightVision == 0 && ei.nvStrength == 1.0 && ei.nvPhotopic == 0.05 &&
              ei.glare == 1 && ei.glareRadiusDeg == 1.0 && ei.glareThresholdEv == 5.0 && ei.glareStreak == 0.3 &&
              fi.glare == 0 && fi.nightVision == 0,
          "R50: [ColorEnhanced] NightVision* / Glare* keys read and clamped (bad text keeps the default); "
          "[ColorFix] ignores them");
  }
  {
    bool mono = true;
    double prev = 1e300;
    for (double y = 0.6; y >= -0.6; y -= 0.005) {
      const double l = AdaptationCdm2(y);
      mono = mono && l <= prev * (1 + 1e-9);
      prev = l;
    }
    const ModeParams e = EnhancedDefaults();
    const double day = ScotopicShare(e, 0.326), gold = ScotopicShare(e, 0.058), dusk = ScotopicShare(e, -0.10),
                 night = ScotopicShare(e, -0.31);
    printf("     (adaptation cd/m2: day %.4g golden %.4g dusk %.4g night %.4g; scotopic share %.3f %.3f %.3f %.3f)\n",
           AdaptationCdm2(0.326), AdaptationCdm2(0.058), AdaptationCdm2(-0.10), AdaptationCdm2(-0.31), day, gold, dusk,
           night);
    Check(mono && AdaptationCdm2(0.326) > 500 && AdaptationCdm2(-0.31) < 0.005 && day == 0 && gold == 0 && dusk > 0.1 &&
              dusk < 0.9 && night == 1,
          "R50: adaptation from the sun (monotone; day / golden hour photopic, dusk mesopic, the R41 nights scotopic)");
    ModeParams e0 = e;  // glare off (the default is on since R55): the 1/4 pass then runs only for night vision
    e0.glare = 0;
    Check(BuildCb(e, false, -0.31, 1, 0, false).nvW == 0 && BuildCb(e, true, 0.326, 1, 0, false).nvW == 0 &&
              std::fabs(BuildCb(e, true, -0.31, 1, 0, false).nvW - 0.8f) < 1e-6f &&
              BuildCb(e0, true, 0.326, 1, 0, false).auxOn == 0 && BuildCb(e0, true, -0.31, 1, 0, false).auxOn == 1 &&
              BuildCb(e, true, 0.326, 1, 0, false).auxOn == 1,
          "R50: rod share = strength at night, 0 by day and with the sun unknown; the low-pass runs only when used "
          "(by day only for the glare)");
  }
  {
    const ColorCb cn = BuildCb(EnhancedDefaults(), true, -0.31, 1, 0, false);
    const float grey[3] = {0.3f, 0.3f, 0.3f}, red[3] = {1, 0, 0}, blue[3] = {0, 0, 1};
    Check(std::fabs(ScotopicOf(grey) / 0.3f - 1) < 2e-3f && ScotopicOf(red) < 0.25f * 0.2126f &&
              ScotopicOf(blue) > 2.0f * 0.0722f,
          "R50: scotopic luminance (Larson): a neutral keeps its luminance, red darkens (Purkinje), blue brightens");
    float o[3];
    const float dim[3] = {0.05f, 0.05f, 0.05f};
    NvOut(cn, dim, o);
    const double yi = LumOf(dim), yo = LumOf(o);
    Check(o[2] > 1.15f * o[0] && std::fabs(yo / yi - 1) < 0.02,
          "R50: a dim neutral at night turns blue-grey (rod tint) at its own luminance");
    const float brown[3] = {0.06f, 0.045f, 0.03f};
    NvOut(cn, brown, o);
    Check(SatOf(o) < 0.7 * SatOf(brown) || HueDeg(o) > 120.0,
          "R50: a dim moonlit colour loses its saturation (rods are colour-blind)");
    const float lamp[3] = {4.9f, 0.52f, 0.35f};  // a lit red instrument flood (+3 EV)
    NvOut(cn, lamp, o);
    Check(o[0] == lamp[0] && o[1] == lamp[1] && o[2] == lamp[2],
          "R50: a lit red instrument light (+3 EV) is untouched (cones: bright emissives keep their colour)");
    // HUD green: bright (+2 EV) as in R42's check, and dimmed to mid-grey; through the whole pipeline at night.
    const DcsTone t;
    ModeParams nnv = EnhancedDefaults();
    nnv.nightVision = 0;
    const ColorCb cOff = BuildCb(nnv, true, -0.31, 1, 0, false);
    const float hud[3] = {0.1f, 1.0f, 0.1f};
    double ratio = 9, sat2 = 0, satDim = 0;
    for (int ev = 0; ev <= 2; ++ev) {
      float x[3], a[3], b[3];
      const double k = 0.18 * std::exp2(ev) / LumOf(hud);
      for (int i = 0; i < 3; ++i) x[i] = static_cast<float>(hud[i] * k);
      TmOf(t, &cn, x, a);
      TmOf(t, &cOff, x, b);
      if (ev == 2) ratio = LumOf(a) / LumOf(b), sat2 = SatOf(a);
      if (ev == 0) satDim = SatOf(a);
    }
    printf("     (HUD green at night with night vision: +2 EV luminance %.3f of without, saturation %.3f; at 0 EV %.3f)\n",
           ratio, sat2, satDim);
    Check(ratio > 0.95 && ratio < 1.05 && sat2 > 0.8 && satDim > 0.6,
          "R50: an HDR HUD green stays green at night (+2 EV: luminance within 5 %, saturation > 0.8; dimmed to 0 EV: > 0.6)");
    // Day: night vision on is bit-identical to off (sun 19 deg, golden hour 3 deg).
    bool same = true;
    const ModeParams e = EnhancedDefaults();
    for (double sy : {0.326, 0.058}) {
      const ColorCb a = BuildCb(e, true, sy, 3, 0, false), b = BuildCb(nnv, true, sy, 3, 0, false);
      for (int k = 0; k < 300; ++k) {
        ShadeIn in;
        const float s = 0.18f * std::exp2(-9.0f + k * 0.05f);
        in.scene[0] = s, in.scene[1] = s * (0.3f + (k % 7) * 0.1f), in.scene[2] = s * (0.2f + (k % 5) * 0.2f);
        in.x = k % 13, in.y = k % 11, in.adapted = 0.1f;
        float oa[3], ob[3];
        Shade(t, &a, in, oa);
        Shade(t, &b, in, ob);
        for (int i = 0; i < 3; ++i) same = same && oa[i] == ob[i];
      }
    }
    Check(same, "R50: by day (19 deg, golden hour 3 deg) night vision changes nothing (bit-identical)");
  }
  {  // glare: bright pass, PSF
    ModeParams g = EnhancedDefaults();
    g.glare = 1;
    const ColorCb c = BuildCb(g, false, 0, 1, 0, false);
    float o1[3], o2[3], o3[3];
    const float lo[3] = {c.glareT * 0.49f, c.glareT * 0.49f, c.glareT * 0.49f};
    const float at[3] = {c.glareT, c.glareT, c.glareT}, hi[3] = {4 * c.glareT, 4 * c.glareT, 4 * c.glareT};
    BrightRef(c, lo, o1);
    BrightRef(c, at, o2);
    BrightRef(c, hi, o3);
    Check(o1[0] == 0 && std::fabs(o2[0] - 0.125f * c.glareT) < 1e-5f * c.glareT && std::fabs(o3[0] / hi[0] - 0.75f) < 1e-5f &&
              std::fabs(c.glareT - 0.18f * std::exp2(3.5f)) < 1e-5f,
          "R50: glare threshold relative to the adapted exposure (3.5 EV over mid-grey), soft knee, linear above");
    // Angular units: the same source direction at two periphery resolutions (same projection) glares alike
    // (outside the source's own level-0 texels).
    auto chain = [&](int W, int H, int sc, GlareLevels& L) {
      std::vector<float> img(static_cast<size_t>(W) * H * 3, 0.0f);
      for (int y = 128 * sc; y < 136 * sc; ++y)
        for (int x = 256 * sc; x < 264 * sc; ++x)
          for (int k = 0; k < 3; ++k) img[(static_cast<size_t>(y) * W + x) * 3 + k] = 40.0f;
      GlareChainRef(img.data(), W, H, 1.0f, c, 0.78f, 0.78f * W / H, L);
    };
    GlareLevels a, b;
    chain(1024, 768, 2, a);
    chain(2048, 1536, 4, b);
    double worst = 0;
    const float cu = 260.0f / 512, cv = 132.0f / 384;
    for (double d : {0.05, 0.08, 0.12, 0.2}) {
      const float du = static_cast<float>(d * 0.78 / 2), dv = static_cast<float>(d * 0.78 * 1024 / 768 / 2);
      for (int dir = 0; dir < 2; ++dir) {
        float ga[3], gb[3], l[3];
        const float u = cu + (dir == 0 ? du : 0), v = cv + (dir == 1 ? dv : 0);
        GlareAtRef(a, c, u, v, ga, l);
        GlareAtRef(b, c, u, v, gb, l);
        worst = std::max(worst, static_cast<double>(std::fabs(gb[1] / std::max(ga[1], 1e-12f) - 1)));
      }
    }
    printf("     (glare at 0.05-0.2 tan (3-11 deg) from a source, periphery 1024 vs 2048 px: worst relative difference %.3f)\n", worst);
    Check(worst < 0.10, "R50: glare is set in angular units: periphery 2x the resolution gives the same halo (< 10 %)");
    ModeParams g2 = g;
    g2.glareRadiusDeg = 10;
    const ColorCb c2 = BuildCb(g2, false, 0, 1, 0, false);
    Check(c2.sig2 > 1.9f * c.sig2 && std::fabs(c.sig2 - std::tan(5.0f * 3.14159265f / 180)) < 1e-6f &&
              std::fabs(c.w0 + c.w1 + c.w2 - 1) < 1e-6f,
          "R50: GlareRadius sets the PSF's widest sigma in degrees (5 deg -> tan 5 deg); weights sum to 1");
  }
}

// WARP: GPU bytes against the CPU reference with the glare / low-pass of eye `e`'s chain (periphery view e's
// image = the rig's scene), view v's pixel mapped to the periphery for v >= 2.
inline void CompareR50(Rig& r, const std::vector<uint8_t>& gpu, const cgrade::ColorCb& cb, int v, float adapted, float ec,
                       const cgrade::GlareLevels* L, bool glareValid, int* worst, double* within1) {
  int w = 0, ok = 0, n = 0;
  float qv[4], qp[4];
  RigProj(v, qv);
  RigProj(v & 1, qp);
  cgrade::ProjQ fq, pq;
  fq.p00 = qv[0], fq.p11 = qv[1], fq.ox = qv[2], fq.oy = qv[3];
  pq.p00 = qp[0], pq.p11 = qp[1], pq.ox = qp[2], pq.oy = qp[3];
  for (UINT y = 0; y < kH; ++y)
    for (UINT x = 0; x < kW; ++x) {
      cgrade::ShadeIn in;
      for (int i = 0; i < 3; ++i) in.scene[i] = r.scene[(y * kW + x) * 3 + i];
      in.adapted = adapted, in.ec = ec, in.x = x, in.y = y;
      if (L && glareValid) {
        float u = (x + 0.5f) / kW, vv = (y + 0.5f) / kH;
        if (v >= 2) cgrade::FocusToPeriUv(fq, pq, u, vv, &u, &vv);
        cgrade::GlareAtRef(*L, cb, u, vv, in.glare, in.low);
        in.hasLow = cb.nvBlur > 0;
      }
      float o[3];
      cgrade::Shade(r.tone[v == 0 ? 0 : 1], &cb, in, o);
      for (int i = 0; i < 3; ++i) {
        const int d = std::abs(static_cast<int>(gpu[(y * kW + x) * 4 + i]) - cgrade::ToUnorm8(o[i]));
        w = std::max(w, d);
        ok += d <= 1;
        ++n;
      }
    }
  *worst = w;
  *within1 = n ? static_cast<double>(ok) / n : 0;
  printf("     (R50 GPU vs CPU, view %d: worst %d codes, %.4f within 1)\n", v, w, *within1);
}

inline void R50GpuTests(Rig& r) {
  using namespace cgrade;
  cpass::g_glareDebug = 0;
  cpass::g_sunValid = false;
  cpass::g_rt.knownN = cpass::g_rt.knownNext = 0;
  for (int v = 0; v < 4; ++v) cpass::NoteLdr(v, kW, kH);
  SetLum(r, 0.18f, 0.18f);
  // DCS's cbuffer packing: our raw read of gProj (P[r][c] at [c * 4 + r]) is what an HLSL shader declaring DCS's
  // cPerView reads as gProj[r][c].
  {
    std::vector<uint8_t> b;
    const bool cok = Compile(
        "cbuffer cPerView : register(b0) { float4x4 gLocal, gLocalInv, gView, gViewInv, gProj; };\n"
        "RWByteAddressBuffer o : register(u0);\n"
        "[numthreads(1,1,1)] void main() { o.Store4(0, asuint(float4(gProj[0][0], gProj[1][1], gProj[2][0], gProj[2][1])));"
        " o.Store2(16, asuint(float2(gProj[2][3], gProj[3][2]))); }",
        "cs_5_0", b);
    ID3D11ComputeShader* cs = nullptr;
    if (cok) r.dev->CreateComputeShader(b.data(), b.size(), nullptr, &cs);
    D3D11_BUFFER_DESC ud{};
    ud.ByteWidth = 32, ud.Usage = D3D11_USAGE_DEFAULT, ud.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ud.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    ID3D11Buffer* ub = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    r.dev->CreateBuffer(&ud, nullptr, &ub);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = DXGI_FORMAT_R32_TYPELESS, uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER, uv.Buffer.NumElements = 8;
    uv.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (ub) r.dev->CreateUnorderedAccessView(ub, &uv, &uav);
    float got[8] = {};
    if (cs && uav) {
      r.ctx->CSSetShader(cs, nullptr, 0);
      r.ctx->CSSetConstantBuffers(0, 1, &r.cb7[2]);
      r.ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
      r.ctx->Dispatch(1, 1, 1);
      ID3D11UnorderedAccessView* nu = nullptr;
      r.ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
      ID3D11Buffer* nb = nullptr;
      r.ctx->CSSetConstantBuffers(0, 1, &nb);
      r.ctx->CSSetShader(nullptr, nullptr, 0);
      D3D11_BUFFER_DESC sd{};
      sd.ByteWidth = 32, sd.Usage = D3D11_USAGE_STAGING, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ID3D11Buffer* st = nullptr;
      r.dev->CreateBuffer(&sd, nullptr, &st);
      r.ctx->CopyResource(st, ub);
      D3D11_MAPPED_SUBRESOURCE m{};
      if (st && SUCCEEDED(r.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        memcpy(got, m.pData, 32);
        r.ctx->Unmap(st, 0);
      }
      if (st) st->Release();
    }
    float q[4];
    RigProj(2, q);
    float raw[16];
    PackProj(q[0], q[1], q[2], q[3], raw);
    const ProjQ pq = ProjFromRaw(raw);
    Check(got[0] == q[0] && got[1] == q[1] && got[2] == q[2] && got[3] == q[3] && got[4] == 1.0f && got[5] == 0.1f &&
              pq.ok && pq.p00 == q[0] && std::fabs(pq.ox - q[2]) < 1e-7f,
          "R50: gProj read raw at cPerView +256 as P[r][c] = raw[c * 4 + r] (HLSL's default column-major packing)");
    if (uav) uav->Release();
    if (ub) ub->Release();
    if (cs) cs->Release();
  }
  cpass::g_mode = 2;
  ModeParams eg = EnhancedDefaults();
  eg.nightVision = 0, eg.glare = 1, eg.glareStreak = 0.3, eg.dither = 0;
  SetParams(eg, 2);
  // First frame after the glare comes on: the periphery has no valid focus projection yet: no glare (seam-safe).
  {
    static const float zero[64] = {};
    const D3D11_BOX box{cpass::kStProj, 0, 0, cpass::kStProj + 256, 1, 1};
    r.ctx->UpdateSubresource(r.obj.state, 0, &box, zero, 0, 0);
    cpass::g_rt.lastView = 99;
    const uint64_t runs0 = cpass::g_glareRuns[0].load();
    DrawView(r, 0);
    const std::vector<uint8_t> g = ReadRt(r);
    const std::vector<float> s = ReadState(r);
    const ColorCb cb = BuildCb(eg, false, 0, cpass::g_rt.frame, 0, false, true);
    const float ec = std::fabs(s[cpass::kStEcRow / 4]) > 0 ? s[cpass::kStEcRow / 4 + 3] : 0.861f;
    int worst = 0;
    double w1 = 0;
    CompareR50(r, g, cb, 0, s[0], ec, nullptr, false, &worst, &w1);
    Check(cpass::g_glareRuns[0].load() == runs0 + 1 && s[cpass::kStStamp / 4] == cb.frame && worst <= 1,
          "R50: glare chain ran at the periphery view, but no glare while the eye's focus projection is unknown");
  }
  // Steady: frame of 4 views, then view 0 of the next frame against the CPU reference of the whole chain.
  {
    for (int v = 1; v < 4; ++v) DrawView(r, v);
    DrawView(r, 0);
    const std::vector<uint8_t> g = ReadRt(r);
    const std::vector<float> s = ReadState(r);
    const ColorCb cb = BuildCb(eg, false, 0, cpass::g_rt.frame, 0, false, true);
    const float ec = std::fabs(s[cpass::kStEcRow / 4]) > 0 ? s[cpass::kStEcRow / 4 + 3] : 0.861f;
    const float ex = std::exp2(std::log2(0.18f / std::clamp(s[0], 0.01f, 0.6f)) + ec + cb.evOffset);
    float q[4];
    RigProj(0, q);
    GlareLevels L;
    GlareChainRef(r.scene.data(), kW, kH, ex, cb, q[0], q[1], L);
    int worst = 0, worstOff = 0;
    double w1 = 0, w1Off = 0;
    CompareR50(r, g, cb, 0, s[0], ec, &L, true, &worst, &w1);
    CompareR50(r, g, cb, 0, s[0], ec, nullptr, false, &worstOff, &w1Off);
    Check(worst <= 2 && w1 > 0.99 && worstOff > 10,
          "R50: glare on WARP = CPU reference of the chain (bright pass, pyramid, Gaussians, streak, composite; "
          "> 99 % within 1 code), and it visibly changes the image");
    // State: PS b12 / t17-21 / s15 and CS u1 / u2 / b1 / b2 / s0 are DCS's again after the draw.
    D3D11_SAMPLER_DESC sm{};
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* sent = nullptr;
    r.dev->CreateSamplerState(&sm, &sent);
    ID3D11ShaderResourceView* srv5[5] = {r.sentSrv, r.sentSrv, r.sentSrv, r.sentSrv, r.sentSrv};
    r.ctx->PSSetConstantBuffers(cpass::kPsViewSlot, 1, &r.sentCb);
    r.ctx->PSSetShaderResources(cpass::kPsAuxSlot, 5, srv5);
    r.ctx->PSSetSamplers(cpass::kPsSamplerSlot, 1, &sent);
    r.ctx->CSSetConstantBuffers(1, 1, &r.sentCb);
    r.ctx->CSSetConstantBuffers(2, 1, &r.sentCb);
    r.ctx->CSSetSamplers(0, 1, &sent);
    DrawView(r, 1);
    ID3D11Buffer* b12 = nullptr;
    ID3D11ShaderResourceView* t[5] = {};
    ID3D11SamplerState* s15 = nullptr;
    ID3D11Buffer* cb12[2] = {};
    ID3D11SamplerState* cs0 = nullptr;
    r.ctx->PSGetConstantBuffers(cpass::kPsViewSlot, 1, &b12);
    r.ctx->PSGetShaderResources(cpass::kPsAuxSlot, 5, t);
    r.ctx->PSGetSamplers(cpass::kPsSamplerSlot, 1, &s15);
    r.ctx->CSGetConstantBuffers(1, 2, cb12);
    r.ctx->CSGetSamplers(0, 1, &cs0);
    bool back = b12 == r.sentCb && s15 == sent && cb12[0] == r.sentCb && cb12[1] == r.sentCb && cs0 == sent;
    for (auto* x : t) back = back && x == r.sentSrv;
    Check(back, "R50: PS b12, t17-t21, s15 and CS b1, b2, s0 are DCS's again after the draw / dispatches");
    for (IUnknown* p : {static_cast<IUnknown*>(b12), static_cast<IUnknown*>(s15), static_cast<IUnknown*>(cb12[0]),
                        static_cast<IUnknown*>(cb12[1]), static_cast<IUnknown*>(cs0)})
      if (p) p->Release();
    for (auto* x : t)
      if (x) x->Release();
    ID3D11ShaderResourceView* n5[5] = {};
    r.ctx->PSSetShaderResources(cpass::kPsAuxSlot, 5, n5);
    ID3D11SamplerState* ns = nullptr;
    r.ctx->PSSetSamplers(cpass::kPsSamplerSlot, 1, &ns);
    r.ctx->CSSetSamplers(0, 1, &ns);
    ID3D11Buffer* nb[2] = {};
    r.ctx->CSSetConstantBuffers(1, 2, nb);
    r.ctx->PSSetConstantBuffers(cpass::kPsViewSlot, 1, nb);
    if (sent) sent->Release();
  }
  // Seam and stereo (glare only, debug output): the focus view's glare equals the periphery's at the same
  // direction; the two eyes (same image, mirrored projections) are identical.
  {
    cpass::g_glareDebug = 1;
    ModeParams eg2 = eg;
    eg2.dither = 1;  // fine dither (+-0.5 code): DCS's Bayer (x 0.9-1.1) would differ between the two pixel grids
    SetParams(eg2, 2);
    DrawView(r, 2), DrawView(r, 3);  // finish the frame
    DrawView(r, 0);
    const std::vector<uint8_t> p0 = ReadRt(r);
    DrawView(r, 1);
    const std::vector<uint8_t> p1 = ReadRt(r);
    DrawView(r, 2);
    const std::vector<uint8_t> f2 = ReadRt(r);
    int stereo = 0, nz = 0;
    for (size_t i = 0; i < p0.size(); ++i) {
      if (i % 4 == 3) continue;
      stereo = std::max(stereo, std::abs(p0[i] - p1[i]));
      nz += p0[i] > 8;
    }
    float qf[4], qp[4];
    RigProj(2, qf);
    RigProj(0, qp);
    ProjQ fq, pq;
    fq.p00 = qf[0], fq.p11 = qf[1], fq.ox = qf[2], fq.oy = qf[3];
    pq.p00 = qp[0], pq.p11 = qp[1], pq.ox = qp[2], pq.oy = qp[3];
    int n = 0, within2 = 0, worst = 0, focusNz = 0;
    double sum = 0;
    for (UINT y = 0; y < kH; ++y)
      for (UINT x = 0; x < kW; ++x) {
        float u, v;
        FocusToPeriUv(fq, pq, (x + 0.5f) / kW, (y + 0.5f) / kH, &u, &v);
        const float px = u * kW - 0.5f, py = v * kH - 0.5f;
        if (px < 0 || py < 0 || px > kW - 1.001f || py > kH - 1.001f) continue;
        const int x0 = static_cast<int>(px), y0 = static_cast<int>(py);
        const float fx = px - x0, fy = py - y0;
        for (int c = 0; c < 3; ++c) {
          auto at = [&](int xx, int yy) { return static_cast<float>(p0[(yy * kW + xx) * 4 + c]); };
          const float b = (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) +
                          (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
          const float d = std::fabs(b - f2[(y * kW + x) * 4 + c]);
          sum += d, ++n, within2 += d <= 2.5f, worst = std::max(worst, static_cast<int>(d + 0.5f));
          focusNz += f2[(y * kW + x) * 4 + c] > 8;
        }
      }
    printf("     (seam: focus vs periphery glare at the same direction: %d channels, mean %.3f codes, worst %d, "
           "%.4f within 2.5; stereo worst %d; non-zero %d / %d)\n",
           n, n ? sum / n : 0.0, worst, n ? static_cast<double>(within2) / n : 0.0, stereo, nz, focusNz);
    Check(n > 1000 && sum / n < 0.6 && within2 > 0.95 * n && focusNz > 100 && nz > 100,
          "R50: seam-free: the focus view's glare = the periphery's at the same direction (mean < 0.6 code; the rest is "
          "the 64 px rig's byte interpolation)");
    Check(stereo == 0, "R50: stereo: both eyes get the same glare (same image, mirrored projections: identical bytes)");
    // The focus view against the CPU reference (mapping through both projections).
    const std::vector<float> s = ReadState(r);
    const ColorCb cb = [&] {
      ColorCb c = BuildCb(eg2, false, 0, cpass::g_rt.frame, 0, false, true);
      c.debug = 1;
      return c;
    }();
    const float ec = std::fabs(s[cpass::kStEcRow / 4]) > 0 ? s[cpass::kStEcRow / 4 + 3] : 0.861f;
    const float ex = std::exp2(std::log2(0.18f / std::clamp(s[0], 0.01f, 0.6f)) + ec + cb.evOffset);
    GlareLevels L;
    GlareChainRef(r.scene.data(), kW, kH, ex, cb, qp[0], qp[1], L);
    int wf = 0;
    double w1 = 0;
    CompareR50(r, f2, cb, 2, s[0], ec, &L, true, &wf, &w1);
    Check(wf <= 2 && w1 > 0.99, "R50: focus view's glare on WARP = CPU reference through both projections");
  }
  // Fallbacks: no cPerView at the draws (zeros written, stamp -1): no glare anywhere; an unknown view: no chain.
  {
    r.bindB7 = false;
    const uint64_t miss0 = cpass::g_projMissing[0].load();
    for (int v = 0; v < 4; ++v) DrawView(r, v);
    DrawView(r, 0);
    const std::vector<uint8_t> g = ReadRt(r);
    const std::vector<float> s = ReadState(r);
    int mx = 0;
    for (size_t i = 0; i < g.size(); ++i)
      if (i % 4 != 3) mx = std::max(mx, static_cast<int>(g[i]));
    Check(cpass::g_projMissing[0].load() > miss0 && s[cpass::kStStamp / 4] == -1.0f && mx == 0,
          "R50: without DCS's cPerView at the tone-map draw the eye is marked unusable: no glare (debug image black)");
    r.bindB7 = true;
    const uint64_t runs = cpass::g_glareRuns[0].load() + cpass::g_glareRuns[1].load();
    const int a = DrawView(r, cpass::kViewUnknown);
    Check(a == 1 && cpass::g_glareRuns[0].load() + cpass::g_glareRuns[1].load() == runs,
          "R50: an unknown-view tone map is swapped without the glare chain");
    cpass::g_glareDebug = 0;
  }
  // Night vision on WARP at night (sun -0.31): rod model and the low-pass (blur), against the CPU reference.
  {
    ModeParams en = EnhancedDefaults();
    en.dither = 0;
    SetParams(en, 2);
    cpass::g_sunValid = true;
    cpass::g_sunY = -0.31f;
    cpass::g_rt.lastView = 99;
    for (int v = 0; v < 4; ++v) DrawView(r, v);
    DrawView(r, 0);
    const std::vector<uint8_t> g = ReadRt(r);
    const std::vector<float> s = ReadState(r);
    const ColorCb cb = BuildCb(en, true, -0.31, cpass::g_rt.frame, 0, false, true);
    const float ec = std::fabs(s[cpass::kStEcRow / 4]) > 0 ? s[cpass::kStEcRow / 4 + 3] : 0.861f;
    const float ex = std::exp2(std::log2(0.18f / std::clamp(s[0], 0.01f, 0.6f)) + ec + cb.evOffset);
    float q[4];
    RigProj(0, q);
    GlareLevels L;
    GlareChainRef(r.scene.data(), kW, kH, ex, cb, q[0], q[1], L);
    int worst = 0, worstOff = 0;
    double w1 = 0, w1Off = 0;
    CompareR50(r, g, cb, 0, s[0], ec, &L, true, &worst, &w1);
    ColorCb off = BuildCb(en, false, 0, cpass::g_rt.frame, 0, false, true);
    CompareR50(r, g, off, 0, s[0], ec, nullptr, false, &worstOff, &w1Off);
    Check(cb.nvW > 0.5f && cb.nvBlur > 0 && worst <= 2 && w1 > 0.995 && worstOff > 3,
          "R50: night vision on WARP = CPU reference (rod blend, tint, low-pass; > 99.5 % within 1 code)");
    cpass::g_sunValid = false;
    cpass::g_sunY = 0.0f;
  }
  SetParams(EnhancedDefaults(), 2);
}

// R81: the live sequence of the R80 check, as the hooks really run it. Enhanced on scene X, then Mode 0 for 12 s:
// with VrFx and timing off, fixation depth is off, so there is no tone-map call of ours at all (no ToneDrawPre, no
// meter, no adapt CS). Then the brightness changes (a pitch change: scene Y and DCS's adapted luminance), then
// Enhanced again: fixation depth installs mid-frame (a partial frame of unknown views), then full frames of the four
// views at 90 Hz. Both meters see the change (scene x4: the second HDR). The image must settle within about a frame
// (the R80 failure: 2-3.5 s ramps from a snap onto a target built with view 2's focus meter from before Mode 0).
inline void R81LiveSequence(Rig& r) {
  std::vector<uint16_t> half(static_cast<size_t>(kW) * kH * 4);
  for (size_t i = 0; i < static_cast<size_t>(kW) * kH; ++i) {
    for (int c = 0; c < 3; ++c) half[i * 4 + c] = FloatToHalf(r.scene[i * 3 + c] * 4.0f);
    half[i * 4 + 3] = FloatToHalf(1.0f);
  }
  D3D11_TEXTURE2D_DESC td{};
  r.hdr->GetDesc(&td);
  D3D11_SUBRESOURCE_DATA sd{half.data(), kW * 8, 0};
  ID3D11Texture2D* hdrB = nullptr;
  ID3D11ShaderResourceView* srvB = nullptr;
  if (FAILED(r.dev->CreateTexture2D(&td, &sd, &hdrB)) || FAILED(r.dev->CreateShaderResourceView(hdrB, nullptr, &srvB))) {
    Check(false, "R81: the second HDR (scene x4)");
    if (hdrB) hdrB->Release();
    return;
  }
  ID3D11ShaderResourceView* const srvA = r.hdrSrv;
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  constexpr double kFrame = 1.0 / 90;
  auto scene = [&](bool bright) {  // the pitch change: the HDR and DCS's adapted luminance (x4 both)
    r.hdrSrv = bright ? srvB : srvA;
    SetLum(r, bright ? 0.2f : 0.05f, bright ? 0.2f : 0.05f);
  };
  auto frame = [&](double dt) {  // one frame of the four HMD views, dt s after the previous one (0: as it comes)
    if (dt > 0) {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      cpass::g_rt.lastQpc.QuadPart = now.QuadPart - static_cast<LONGLONG>(dt * f.QuadPart);
    }
    for (int v = 0; v < 4; ++v) DrawView(r, v);
  };
  auto adapted = [&] { return ReadState(r)[0]; };
  auto settle = [&] {  // Enhanced on, 15 s at 0.25 s steps: the settled value for the current scene
    for (int i = 0; i < 60; ++i) frame(0.25);
    return adapted();
  };
  // ln(adapted / settled) over the first 27 frames (0.3 s at 90 Hz) after a re-enable / event: worst from frame 2 on
  struct Run {
    double f1 = 0, f2 = 0, worst = 0, f27 = 0;
  };
  // R107: firstDt > 0 sets frame 1's interval explicitly (the no-gap runs: under CPU load the time "as it comes" since
  // the previous frame grew to a large part of the adaptation's tau and the ramp check failed in a parallel run);
  // 0 keeps the clock as it comes for the runs whose gap the test made itself (lastQpc moved back).
  auto runFrames = [&](double settled, double firstDt = 0) {
    Run o;
    for (int k = 1; k <= 27; ++k) {
      frame(k == 1 ? firstDt : kFrame);  // frame 1: the clock as it comes unless firstDt (the gap, if any, is real)
      const double e = std::fabs(std::log(adapted() / settled));
      if (k == 1) o.f1 = e;
      if (k == 2) o.f2 = e;
      if (k >= 2) o.worst = std::max(o.worst, e);
      o.f27 = e;
    }
    return o;
  };
  cpass::g_mode = 2;
  cpass::g_timing = 0;
  scene(false);
  const float settledX = settle();
  scene(true);
  const float settledY = settle();
  // (a) Enhanced on X, Mode 0 12 s (no calls; or calls with VrFx wanted), pitch to Y, Enhanced
  auto offOn = [&](bool fromBright, bool callsInMode0) {
    scene(fromBright);
    settle();
    cpass::g_mode = 0;
    if (callsInMode0) {  // Mode 0 with ToneDrawPre still called (VrFx wanted, its effects not active)
      scene(!fromBright);
      for (int i = 0; i < 270; ++i) frame(kFrame);
    } else {  // fixation depth off: nothing of ours runs; the clock goes on
      cpass::g_rt.lastQpc.QuadPart -= 12 * f.QuadPart;
      scene(!fromBright);
    }
    cpass::g_mode = 2;
    DrawView(r, cpass::kViewUnknown);  // the partial frame after fixation depth installs mid-frame
    DrawView(r, cpass::kViewUnknown);
    return runFrames(fromBright ? settledX : settledY);
  };
  const uint32_t logged0 = cpass::g_resetLogged.load();
  const Run up = offOn(false, false);
  // the reset log line of (a): reason gap (Mode 0 was never seen by the render thread), adapted X -> Y at the snap
  const cpass::ResetLog upLog = cpass::g_resetLog;
  const bool upLogged = cpass::g_resetLogged.load() == logged0 + 1;
  const Run down = offOn(true, false), upCalls = offOn(false, true);
  // (a') a short Mode 0 (0.5 s, under the gap) the render thread never sees: the config thread's mode switches
  // (Tick bumps g_modeGen at 2 -> 0 and 0 -> 2) reset it
  scene(false);
  settle();
  cpass::g_mode = 0;
  cpass::g_modeGen += 1;
  scene(true);
  cpass::g_modeFrom = 0;  // as Tick at 0 -> 2
  cpass::g_modeGen += 1;
  cpass::g_mode = 2;
  {  // R107: 0.5 s before now, taken right before the frame (was "-= 0.5 s" from the last frame: under CPU load the
     // real time on top of it crossed the 1 s gap and the reset was logged as a gap instead of the mode switch)
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    cpass::g_rt.lastQpc.QuadPart = now.QuadPart - f.QuadPart / 2;
  }
  DrawView(r, cpass::kViewUnknown);
  const uint32_t logged1 = cpass::g_resetLogged.load();
  const Run shortOff = runFrames(settledY);
  const cpass::ResetLog shortLog = cpass::g_resetLog;
  const bool shortLogged = cpass::g_resetLogged.load() == logged1 + 1;
  // (b) Enhanced stays on, no tone map of ours for 1.5 s (a load stall), then the brighter scene
  scene(false);
  settle();
  cpass::g_rt.lastQpc.QuadPart -= f.QuadPart * 3 / 2;
  scene(true);
  const Run gap = runFrames(settledY);
  // (c) a mission event, then the darker scene at once (no gap)
  cpass::NoteMissionEvent();
  scene(false);
  const Run mission = runFrames(settledX);
  // (d) normal eye-like adaptation stays: Enhanced on, X -> Y at 90 Hz (no reset): still far after 2 frames, and
  // part of the way after 0.3 s (tauBright 0.4 s: ~1 - e^-0.75 = 53 %)
  scene(false);
  settle();
  const double stepLn = std::log(settledY / settledX);
  scene(true);
  const Run normal = runFrames(settledY, kFrame);  // R107: an explicit first interval (no gap in this run)
  const double movedFrac = 1 - normal.f27 / stepLn;
  const bool snapOk = up.f2 < 0.01 && up.worst < 0.01 && down.f2 < 0.01 && down.worst < 0.01 && upCalls.worst < 0.01 &&
                      shortOff.worst < 0.01 && gap.worst < 0.01 && mission.worst < 0.01;
  auto lnNear = [](double a, double b) { return a > 0 && b > 0 && std::fabs(std::log(a / b)) < 0.01; };
  const bool logOk = upLogged && (upLog.why & cpass::kRsGap) && upLog.gapSec > 11 && upLog.toMode == 2 &&
                     lnNear(upLog.before, settledX) && lnNear(upLog.after, settledY) && lnNear(upLog.after, upLog.target) &&
                     upLog.validBefore == 1 && upLog.validAfter == 1 && upLog.focus > 0 && shortLogged &&
                     (shortLog.why & cpass::kRsMode) && shortLog.fromMode == 0 && !(shortLog.why & cpass::kRsGap) && lnNear(shortLog.after, settledY);
  const bool normalOk = normal.f2 > 0.9 * stepLn && movedFrac > 0.35 && movedFrac < 0.7;
  printf("     (R81 |ln(adapted/settled)| frame 1 / 2 / worst 2..27: off-on brighter %.3f/%.3f/%.3f, darker %.3f/%.3f/%.3f, "
         "with calls in Mode 0 %.3f/%.3f/%.3f, 0.5 s Mode 0 %.3f/%.3f/%.3f, gap %.3f/%.3f/%.3f, mission %.3f/%.3f/%.3f; "
         "normal step ln %.3f: frame 2 %.3f, moved %.2f in 0.3 s)\n",
         up.f1, up.f2, up.worst, down.f1, down.f2, down.worst, upCalls.f1, upCalls.f2, upCalls.worst, shortOff.f1,
         shortOff.f2, shortOff.worst, gap.f1, gap.f2, gap.worst, mission.f1, mission.f2, mission.worst, stepLn,
         normal.f2, movedFrac);
  printf("     (R81 reset log: off-on why 0x%x gap %.1f s adapted %.5f (valid %u) -> %.5f (valid %u) target %.5f focus "
         "%.5f, settled %.5f -> %.5f; 0.5 s Mode 0 why 0x%x after %.5f; lines %u)\n",
         upLog.why, upLog.gapSec, upLog.before, upLog.validBefore, upLog.after, upLog.validAfter, upLog.target,
         upLog.focus, settledX, settledY, shortLog.why, shortLog.after, cpass::g_resetLogged.load());
  Check(snapOk, "R81: live sequence (Enhanced -> Mode 0, no hook calls, 12 s -> brightness change -> Enhanced, focus "
                "views): settled within 1 frame, brighter and darker; also after a 1.5 s gap and a mission event");
  Check(normalOk, "R81: with Enhanced on and no reset, the eye-like adaptation still ramps (not a snap)");
  Check(logOk, "R81: the reset log: reason (gap / mode switch), adapted before = old settled, after = target = new "
               "settled, focus meter fresh");
  r.hdrSrv = srvA;
  SetLum(r, 0.05f, 0.05f);
  srvB->Release(), hdrB->Release();
}

inline void GpuTests() {
  Rig r;
  if (!MakeRig(r)) {
    Check(false, "colour pass: WARP rig (device, shaders, objects)");
    r.Release();
    return;
  }
  cpass::g_rt = cpass::Rt{};
  cpass::g_fault = false;
  cpass::g_obj.store(&r.obj);
  for (int v = 0; v < 4; ++v) cpass::NoteLdr(v, kW, kH);
  const cgrade::ModeParams fix = cgrade::FixDefaults(), enh = cgrade::EnhancedDefaults();
  SetParams(fix, 1);
  SetParams(enh, 2);
  // Fix, view 0 (its own EC: nothing copied yet), GPU against the CPU reference.
  cpass::g_mode = 1;
  int act = DrawView(r, 0);
  std::vector<uint8_t> g = ReadRt(r);
  cgrade::ColorCb cb = cgrade::BuildCb(fix, false, 0, cpass::g_rt.frame, 0, false);
  int worst = 0;
  double w1 = 0;
  Compare(r, g, &cb, 0, 0.18f, 0.861f, &worst, &w1);
  Check(act == 1 && worst <= 1, "colour pass: Fix on WARP = CPU reference (every channel within 1 code)");
  {
    ID3D11PixelShader* ps = nullptr;
    r.ctx->PSGetShader(&ps, nullptr, nullptr);
    ID3D11Buffer* b13 = nullptr;
    r.ctx->PSGetConstantBuffers(cpass::kPsCbSlot, 1, &b13);
    ID3D11ShaderResourceView* t16 = nullptr;
    r.ctx->PSGetShaderResources(cpass::kPsStateSlot, 1, &t16);
    Check(ps == r.dcsPs && b13 == r.sentCb && t16 == r.sentSrv,
          "colour pass: DCS's pixel shader, PS b13 and t16 back right after the draw");
    if (ps) ps->Release();
    if (b13) b13->Release();
    if (t16) t16->Release();
  }
  // Stock-emulating parameters: our HLSL is DCS's shader (against the CPU replica checked on R41's images).
  {
    cgrade::ModeParams st = fix;
    st.srgbMatch = 0, st.equalizeEc = 0, st.dither = 0;
    SetParams(st, 1);
    cpass::g_rt.lastView = 99;
    DrawView(r, 0);
    g = ReadRt(r);
    Compare(r, g, nullptr, 0, 0.18f, 0.861f, &worst, &w1);
    Check(worst <= 1, "colour pass: stock parameters on WARP = DCS's shader (CPU replica) within 1 code");
    SetParams(fix, 1);
  }
  // EqualizeEc: view 1 (EC 0.941) copies its EC; the next frame's view 0 (own EC 0.861) uses it.
  {
    cpass::g_rt.lastView = 99;
    DrawView(r, 0);
    DrawView(r, 1);
    std::vector<uint8_t> g1 = ReadRt(r);
    DrawView(r, 0);  // a new frame (view order wraps)
    std::vector<uint8_t> g0 = ReadRt(r);
    cb = cgrade::BuildCb(fix, false, 0, cpass::g_rt.frame, 0, false);
    Compare(r, g0, &cb, 0, 0.18f, 0.941f, &worst, &w1);
    const std::vector<float> s = ReadState(r);
    int diff01 = 0;
    for (size_t i = 0; i < g0.size(); ++i) diff01 = std::max(diff01, std::abs(g0[i] - g1[i]));
    Check(worst <= 1 && std::fabs(s[cpass::kStEcRow / 4 + 3] - 0.941f) < 1e-6f && diff01 <= 1,
          "colour pass: EqualizeEc: view 0 uses view 1's EC (0.941, not its own 0.861): views match within 1 code");
  }
  // Enhanced with its adaptation state and meters; DCS's CS state put back.
  {
    cpass::g_mode = 2;
    cpass::g_rt.lastView = 99;
    std::vector<uint8_t> b;
    ID3D11ComputeShader* sentCs = nullptr;
    Compile("RWByteAddressBuffer u : register(u0); [numthreads(1,1,1)] void main() { u.Store(0, 7); }", "cs_5_0", b);
    r.dev->CreateComputeShader(b.data(), b.size(), nullptr, &sentCs);
    D3D11_BUFFER_DESC ud{};
    ud.ByteWidth = 16, ud.Usage = D3D11_USAGE_DEFAULT, ud.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ud.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    ID3D11Buffer* ub = nullptr;
    ID3D11UnorderedAccessView* sentUav = nullptr;
    r.dev->CreateBuffer(&ud, nullptr, &ub);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = DXGI_FORMAT_R32_TYPELESS, uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER, uv.Buffer.NumElements = 4;
    uv.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    r.dev->CreateUnorderedAccessView(ub, &uv, &sentUav);
    r.ctx->CSSetShader(sentCs, nullptr, 0);
    r.ctx->CSSetUnorderedAccessViews(0, 1, &sentUav, nullptr);
    r.ctx->CSSetShaderResources(0, 1, &r.sentSrv);
    r.ctx->CSSetConstantBuffers(0, 1, &r.sentCb);
    SetLum(r, 0.05f, 0.05f);
    act = DrawView(r, 0);
    g = ReadRt(r);
    cb = cgrade::BuildCb(enh, false, 0, cpass::g_rt.frame, 0, true);
    std::vector<float> s = ReadState(r);
    Compare(r, g, &cb, 0, 0.05f, std::fabs(s[cpass::kStEcRow / 4]) > 0 ? s[cpass::kStEcRow / 4 + 3] : 0.861f, &worst, &w1);
    Check(act == 1 && worst <= 2 && w1 > 0.995, "colour pass: Enhanced on WARP = CPU reference (> 99.5 % within 1 code)");
    // R81: with focus views (four learnt) the reset frame's state waits for view 2's focus meter (valid 2: the pixel
    // shader uses DCS's adapted luminance); the next frame's dispatch makes it valid
    Check(std::fabs(s[0] - 0.05f) < 1e-6f && reinterpret_cast<const uint32_t*>(s.data())[1] == 2u,
          "colour pass: adaptation state starts at DCS's adapted luminance (reset; R81: valid after view 2's focus meter)");
    {
      ID3D11ComputeShader* cs = nullptr;
      r.ctx->CSGetShader(&cs, nullptr, nullptr);
      ID3D11UnorderedAccessView* u = nullptr;
      r.ctx->CSGetUnorderedAccessViews(0, 1, &u);
      ID3D11ShaderResourceView* t0 = nullptr;
      r.ctx->CSGetShaderResources(0, 1, &t0);
      ID3D11Buffer* c0 = nullptr;
      r.ctx->CSGetConstantBuffers(0, 1, &c0);
      Check(cs == sentCs && u == sentUav && t0 == r.sentSrv && c0 == r.sentCb,
            "colour pass: DCS's compute shader, CS t0, u0, b0 back after our dispatches");
      for (IUnknown* p : {static_cast<IUnknown*>(cs), static_cast<IUnknown*>(u), static_cast<IUnknown*>(t0), static_cast<IUnknown*>(c0)})
        if (p) p->Release();
    }
    // Periphery meter (view 0): the log-average of the 32 x 32 grid.
    double lsum = 0;
    for (int j = 0; j < 32; ++j)
      for (int i = 0; i < 32; ++i) {
        const UINT px = std::min<UINT>(static_cast<UINT>((i + 0.5) / 32 * kW), kW - 1);
        const UINT py = std::min<UINT>(static_cast<UINT>((j + 0.5) / 32 * kH), kH - 1);
        const float* c = &r.scene[(py * kW + px) * 3];
        double l = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
        l = l > 1e-5 ? l : 1e-5;
        lsum += std::log(l);
      }
    const double peri = std::exp(lsum / 1024);
    Check(std::fabs(s[cpass::kStPeri / 4] / peri - 1) < 1e-3, "colour pass: periphery meter = log-average of the view");
    // Brighter step (x4) after 0.25 s: the fast time constant (0.4 s); then darker: the slow one (1.5 s).
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    DrawView(r, 2);  // R81: the rest of the reset frame (focus meter), then the next frame: the state valid from here
    DrawView(r, 0);
    s = ReadState(r);
    const double start = s[0];
    const bool validNext = reinterpret_cast<const uint32_t*>(s.data())[1] == 1u;
    SetLum(r, 0.2f, 0.2f);
    cpass::g_rt.lastQpc.QuadPart -= f.QuadPart / 4;
    DrawView(r, 2);  // focus meter
    DrawView(r, 0);
    s = ReadState(r);
    const double target1 = s[cpass::kStTarget / 4];
    const double up = validNext ? std::log(s[0] / start) / std::log(target1 / start) : 0;
    SetLum(r, 0.05f, 0.05f);
    cpass::g_rt.lastQpc.QuadPart -= f.QuadPart / 4;
    DrawView(r, 2);
    const float before = s[0];
    DrawView(r, 0);
    s = ReadState(r);
    const double target2 = s[cpass::kStTarget / 4];
    const double down = std::log(s[0] / before) / std::log(target2 / before);
    Check(up > 0.35 && up < 0.65 && down > 0.08 && down < 0.30 && up > 2 * down,
          "colour pass: eye-like adaptation: brighter fast (~0.46 of the step in 0.25 s), darker slow (~0.15)");
    Check(s[cpass::kStFocus / 4] > 0 && std::fabs(target2 / 0.05 - std::pow(s[cpass::kStFocus / 4] / s[cpass::kStPeri / 4], enh.meterFocus)) < 1e-3,
          "colour pass: MeterFocus: target = DCS's adapted x (focus / periphery log-average)^weight");
    r.ctx->CSSetShader(nullptr, nullptr, 0);
    ID3D11UnorderedAccessView* nu = nullptr;
    r.ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
    sentUav->Release(), ub->Release(), sentCs->Release();
    // R77: the adaptation starts anew after Enhanced -> Off -> Enhanced (VrFx off), after a mission event, after a
    // switch first seen at view 1 (the reset waits for view 0's dispatch) and when DCS's luminance was unusable on
    // the reset frame.
    auto adaptedNow = [&](float lum) {  // a new frame with DCS's luminance lum: our adapted state (view 0's dispatch)
      SetLum(r, lum, lum);
      cpass::g_rt.lastView = 99;
      // R81: the four views, as the hooks run a frame with focus views (the rig learnt four): view 2's focus meter
      // (MeterFocus) is part of the adaptation's target, and after a reset the state waits for a fresh one
      for (int v = 0; v < 4; ++v) DrawView(r, v);
      return ReadState(r)[0];
    };
    auto targetNow = [&] { return ReadState(r)[cpass::kStTarget / 4]; };
    adaptedNow(0.05f);
    cpass::g_mode = 0;
    cpass::g_rt.lastView = 99;
    DrawView(r, 0);  // Off (VrFx off): DCS's own tone map
    cpass::g_mode = 2;
    const float a1 = adaptedNow(0.6f);
    const bool offOn = std::fabs(a1 / targetNow() - 1) < 1e-5f;
    adaptedNow(0.05f);
    cpass::NoteMissionEvent();
    const float a2 = adaptedNow(0.6f);
    const bool mission = std::fabs(a2 / targetNow() - 1) < 1e-5f;
    adaptedNow(0.05f);
    cpass::g_mode = 0;
    cpass::g_rt.lastView = 99;
    DrawView(r, 0);
    cpass::g_mode = 2;
    SetLum(r, 0.6f, 0.6f);
    DrawView(r, 1);  // the switch seen at view 1: constants built there, no adaptation dispatch
    const float a3 = adaptedNow(0.6f);
    const bool midFrame = std::fabs(a3 / targetNow() - 1) < 1e-5f;
    adaptedNow(0.05f);
    cpass::NoteMissionEvent();
    adaptedNow(0.0f);  // DCS's luminance unusable on the reset frame
    const float a4 = adaptedNow(0.6f);
    const bool badLum = std::fabs(a4 / targetNow() - 1) < 1e-5f;
    const float a5 = adaptedNow(0.05f);  // no reset: blends again
    const bool blends = a5 > 0.06f;
    Check(offOn && mission && midFrame && badLum && blends,
          "R77: colour adaptation resets after Off -> Enhanced, a mission event, a view-1 switch, a bad-luminance reset");
    if (!(offOn && mission && midFrame && badLum && blends))
      printf("     (adapted / target: off-on %.4g, mission %.4g, view 1 %.4g, bad lum %.4g, blend %.4g)\n", a1 / targetNow(),
             a2, a3, a4, a5);
  }
  R81LiveSequence(r);
  // R86 (F1): with focus views learnt and MeterFocus on, a reset followed by frames without view 2's tone map (no focus
  // meter: quad views off, or view 2 rejected): the state waits 8 frames, then stays valid on the periphery-only
  // target. Before R86 the valid word went 1 -> 2 .. 8 -> 1, so the state was used only 1 frame in 8.
  {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    cpass::g_mode = 2;
    for (int k = 0; k < 4; ++k)
      for (int v = 0; v < 4; ++v) DrawView(r, v);
    cpass::NoteMissionEvent();
    bool reached = false, back = false;
    int firstValid = -1;
    char seq[64] = {};
    for (int k = 0; k < 24; ++k) {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      cpass::g_rt.lastQpc.QuadPart = now.QuadPart - f.QuadPart / 90;
      DrawView(r, 0);
      DrawView(r, 1);
      DrawView(r, 3);
      const std::vector<float> s = ReadState(r);
      const uint32_t v = reinterpret_cast<const uint32_t*>(s.data())[1];
      seq[k] = static_cast<char>(v < 10 ? '0' + v : '?');
      if (v == 1) {
        if (!reached) firstValid = k;
        reached = true;
      } else if (reached) {
        back = true;
      }
    }
    if (!reached || back) printf("     (R86 F1: valid word per frame without view 2: %s)\n", seq);
    Check(reached && !back && firstValid <= 8,
          "R86 F1: without view 2's focus meter after a reset the adaptation state becomes valid within 8 frames and "
          "stays valid (periphery-only target), not 1 frame in 8");
    for (int k = 0; k < 4; ++k)
      for (int v = 0; v < 4; ++v) DrawView(r, v);
  }
  // Rejects: another target format, a learnt size that differs, a 3D LUT bound: DCS's own draw.
  {
    cpass::g_mode = 1;
    const uint64_t r0 = cpass::g_rejects[cpass::kRjTarget].load(), r1 = cpass::g_rejects[cpass::kRjSize].load(),
                   r2 = cpass::g_rejects[cpass::kRjLut].load();
    const int a1 = DrawView(r, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    g = ReadRt(r);
    const bool magenta = g[0] == 255 && g[1] == 0 && g[2] == 255;
    // A size no CopyToHMDPass showed (R46: the check is against the set of HMD image sizes, not one view's).
    cpass::g_rt.knownN = cpass::g_rt.knownNext = 0;
    cpass::NoteLdr(2, kW * 2, kH);
    const int a2 = DrawView(r, 2);
    cpass::NoteLdr(2, kW, kH);
    D3D11_TEXTURE3D_DESC d3{};
    d3.Width = d3.Height = d3.Depth = 4, d3.MipLevels = 1, d3.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d3.Usage = D3D11_USAGE_DEFAULT, d3.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture3D* lut = nullptr;
    ID3D11ShaderResourceView* lutSrv = nullptr;
    r.dev->CreateTexture3D(&d3, nullptr, &lut);
    if (lut) r.dev->CreateShaderResourceView(lut, nullptr, &lutSrv);
    const int a3 = DrawView(r, 3, DXGI_FORMAT_UNKNOWN, lutSrv);
    if (lutSrv) lutSrv->Release();
    if (lut) lut->Release();
    printf("     (rejects: a %d %d %d magenta %d, counts %llu %llu %llu)\n", a1, a2, a3, magenta ? 1 : 0,
           static_cast<unsigned long long>(cpass::g_rejects[cpass::kRjTarget].load() - r0),
           static_cast<unsigned long long>(cpass::g_rejects[cpass::kRjSize].load() - r1),
           static_cast<unsigned long long>(cpass::g_rejects[cpass::kRjLut].load() - r2));
    Check(a1 == 0 && magenta && a2 == 0 && a3 == 0 && cpass::g_rejects[cpass::kRjTarget].load() == r0 + 1 &&
              cpass::g_rejects[cpass::kRjSize].load() == r1 + 1 && cpass::g_rejects[cpass::kRjLut].load() == r2 + 1,
          "colour pass: not DCS's HMD tone map (sRGB view, other size, 3D LUT bound): DCS's draw, untouched");
  }
  // Timing: Mode 0 + Timing 1 times DCS's own draws without swapping; Mode 1 times ours.
  {
    cpass::g_mode = 0;
    cpass::g_timing = 1;
    cpass::g_timedFrames = 0;
    // A frame's query set is read (never waited for) kQSets frames later, and dropped when not ready then. R60:
    // each frame waits until the GPU finished it (event query, 30 s deadline) instead of a 2 ms sleep, which WARP
    // on a loaded CPU overran (every set lost: t0 < 2).
    int swaps = 0;
    for (int fr = 0; fr < 8; ++fr) {
      for (int v = 0; v < 4; ++v) swaps += DrawView(r, v) == 1;
      gpttest::WaitGpuIdle(r.dev, r.ctx);
    }
    const uint64_t t0 = cpass::g_timedFrames.load();
    cpass::g_mode = 1;
    for (int fr = 0; fr < 8; ++fr) {
      for (int v = 0; v < 4; ++v) DrawView(r, v);
      gpttest::WaitGpuIdle(r.dev, r.ctx);
    }
    Check(swaps == 0 && t0 >= 2 && cpass::g_timedFrames.load() > t0 && cpass::g_gpuUs.load() > 0,
          "colour pass: GPU timestamps around the tone-map draws (stock in Mode 0, ours in Mode 1)");
    cpass::g_timing = 0;
  }
  // R46: the size flip and the one-frame miss of the live log (20:31:13: "HMD view 0 image 2028x2004" then
  // "1040x1028", "not swapped: size not the view's 2"). Fixation depth installed mid-frame: the partial frame's views
  // were numbered from a stale hmdOrd, so a focus view's CopyToHMDPass taught view 0 the focus size. Here the rig's
  // image (kW x kH) is the periphery size, 2kW x 2kH the focus size.
  {
    cpass::g_mode = 1;
    cpass::g_rt.knownN = cpass::g_rt.knownNext = 0;
    for (int v = 0; v < 4; ++v) cpass::g_rt.ldrW[v] = cpass::g_rt.ldrH[v] = 0;
    cpass::NoteLdr(0, kW, kH), cpass::NoteLdr(1, kW, kH);
    cpass::NoteLdr(2, kW * 2, kH * 2), cpass::NoteLdr(3, kW * 2, kH * 2);
    cpass::NoteLdr(0, kW * 2, kH * 2);  // the flip: view 0 "learns" a focus view's size
    const bool flipped = cpass::g_rt.ldrW[0] == kW * 2;
    const uint64_t rs = cpass::g_rejects[cpass::kRjSize].load(), m0 = cpass::g_missTotal.load();
    cpass::g_rt.lastView = 99;
    int ok = 0;
    for (int v = 0; v < 4; ++v) {
      cpass::TonePassBegin(v);
      ok += DrawView(r, v) == 1;  // every view's tone map: the periphery size (the rig's), as DCS draws it
      cpass::TonePassEnd();
    }
    Check(flipped && ok == 4 && cpass::g_rejects[cpass::kRjSize].load() == rs && cpass::g_missTotal.load() == m0,
          "colour pass R46: a view whose learnt size flipped (view 0 taught a focus size) is still swapped, no miss");
  }
  // R46: the unknown view (first partial frame after fixation depth installs): swapped, no per-view extras.
  {
    cpass::g_mode = 2;
    const uint64_t u0 = cpass::g_swapUnknown.load(), sw0 = cpass::g_swaps[0].load(), m0 = cpass::g_missTotal.load();
    const int last = cpass::g_rt.lastView;
    const uint32_t fr = cpass::g_rt.frame;
    cpass::TonePassBegin(cpass::kViewUnknown);
    const int a = DrawView(r, cpass::kViewUnknown);
    cpass::TonePassEnd();
    Check(a == 1 && cpass::g_swapUnknown.load() == u0 + 1 && cpass::g_swaps[0].load() == sw0 &&
              cpass::g_rt.lastView == last && cpass::g_rt.frame == fr && cpass::g_missTotal.load() == m0,
          "colour pass R46: an unknown-view tone map is swapped (no grey frame), the frame count untouched");
    // The view mapping: not synced -> unknown; synced -> hmdOrd; synced past view 3 -> left alone at a begin.
    Check(fixdepth::ColorView(false, 0, true) == cpass::kViewUnknown && fixdepth::ColorView(false, 7, true) == cpass::kViewUnknown &&
              fixdepth::ColorView(true, 2, true) == 2 && fixdepth::ColorView(true, 4, true) == -1 &&
              fixdepth::ColorView(true, 4, false) == cpass::kViewUnknown,
          "colour pass R46: view of a boundary (unsynced -> unknown, synced -> CopyToHMDPass count, 5th view left alone)");
    // Mid-frame install replay: views 2 and 3 of the partial frame arrive with hmdOrd stale (0, 1) and not synced.
    {
      const uint64_t s2 = cpass::g_rejects[cpass::kRjSize].load(), mm = cpass::g_missTotal.load();
      int okN = 0, learnt = 0;
      for (int ord = 0; ord < 2; ++ord) {
        const int v = fixdepth::ColorView(false, ord, true);
        cpass::TonePassBegin(v);
        okN += DrawView(r, v) == 1;
        cpass::TonePassEnd();
        learnt += v <= 3;  // ColorBoundary arms the size learning only for v <= 3
      }
      Check(okN == 2 && learnt == 0 && cpass::g_rejects[cpass::kRjSize].load() == s2 && cpass::g_missTotal.load() == mm,
            "colour pass R46: mid-frame install: the partial frame's tone maps swapped, nothing learnt under a stale view");
    }
  }
  // R46 miss counter: a mode on and a tone map left unswapped (rejected, or the arm refused: no draw) counts per view;
  // Mode 0 counts nothing.
  {
    cpass::g_mode = 1;
    for (auto& m : cpass::g_miss) m = 0;
    const uint64_t t0 = cpass::g_missTotal.load();
    cpass::TonePassBegin(2);
    const int a = DrawView(r, 2, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);  // rejected: DCS's draw
    cpass::TonePassEnd();
    cpass::TonePassBegin(1);  // arm refused: no ToneDrawPre at all
    cpass::TonePassEnd();
    cpass::TonePassBegin(3);
    const int b = DrawView(r, 3);
    cpass::TonePassEnd();
    cpass::g_mode = 0;
    cpass::TonePassBegin(0);
    cpass::TonePassEnd();
    cpass::TonePassEnd();  // an end without a begin
    Check(a == 0 && b == 1 && cpass::g_miss[2].load() == 1 && cpass::g_miss[1].load() == 1 && cpass::g_miss[3].load() == 0 &&
              cpass::g_miss[0].load() == 0 && cpass::g_missTotal.load() == t0 + 2,
          "colour pass R46: missed HMD tone maps counted per view (reject, refused arm), none for a swap or in Mode 0");
    // R47: the miss causes; a tone map while the objects are being created is not a miss.
    {
      for (auto& w : cpass::g_missWhy) w = 0;
      cpass::g_missPending = 0;
      cpass::g_mode = 1;
      const uint64_t t1 = cpass::g_missTotal.load();
      cpass::TonePassBegin(2);  // armed, ToneDrawPre ran, rejected
      cpass::TonePassArmed(true);
      DrawView(r, 2, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
      cpass::TonePassEnd();
      cpass::TonePassBegin(1);  // arm refused
      cpass::TonePassArmed(false);
      cpass::TonePassEnd();
      cpass::TonePassBegin(0);  // armed, the draw never reached the hook (d3d11's after-flush entries)
      cpass::TonePassArmed(true);
      cpass::TonePassEnd();
      cpass::TonePassBegin(3);  // armed and swapped: nothing
      cpass::TonePassArmed(true);
      const int sw = DrawView(r, 3);
      cpass::TonePassEnd();
      cpass::Objects* keep = cpass::g_obj.exchange(nullptr);
      cpass::TonePassBegin(2);  // objects not created yet (the first switch to a mode): pending, not a miss
      cpass::TonePassArmed(true);
      cpass::TonePassEnd();
      cpass::g_obj.store(keep);
      Check(sw == 1 && cpass::g_missWhy[cpass::kMissReject].load() == 1 && cpass::g_missWhy[cpass::kMissArm].load() == 1 &&
                cpass::g_missWhy[cpass::kMissBypass].load() == 1 && cpass::g_missTotal.load() == t1 + 3 &&
                cpass::g_missPending.load() == 1,
            "colour pass R47: miss causes (rejected / arm refused / draw bypassed); objects being created is no miss");
      for (auto& w : cpass::g_missWhy) w = 0;
      cpass::g_missPending = 0;
    }
    for (auto& m : cpass::g_miss) m = 0;
    cpass::g_missTotal = 0, cpass::g_swapUnknown = 0;
    cpass::g_rt.knownN = cpass::g_rt.knownNext = 0;
    for (int v = 0; v < 4; ++v) cpass::NoteLdr(v, kW, kH);
  }
  // R47: the tone-map arm's state-call toggle points go only into slots holding d3d11's own entry (the split filter
  // re-hooks its slots over any other pointer, taking it as its original: mutual forwarding, stack overflow live).
  {
    void** real = *reinterpret_cast<void***>(r.ctx);
    std::vector<void*> fake(real, real + 160);
    for (int k = 0; k < fixdepth::kTpCount; ++k) fixdepth::g_tpSlot[k] = DcsQv_ContextSlot(fixdepth::kTpNames[k]);
    const int ps = fixdepth::g_tpSlot[fixdepth::kTpPSSetShader], srv = fixdepth::g_tpSlot[fixdepth::kTpPSSetSRV];
    static int notD3d11 = 0;
    void* foreign = &notD3d11;  // a non-d3d11 pointer, as the split filter's hook
    fake[ps] = foreign;
    for (auto& a : fixdepth::g_tpArmed) a = false;
    fixdepth::g_tpAddedExt = false;
    fixdepth::ArmTpExtendedD3d11Only(fake.data());
    const bool armed = fake[ps] == foreign && fake[srv] == fixdepth::TogglePointHook(fixdepth::kTpPSSetSRV) &&
                       fixdepth::g_tpArmed[fixdepth::kTpPSSetSRV] && !fixdepth::g_tpArmed[fixdepth::kTpPSSetShader] &&
                       fixdepth::g_tpAddedExt;
    fixdepth::DisarmTpExtended(fake.data());
    bool back = fake[ps] == foreign;
    for (int k = fixdepth::kTpBasic; k < fixdepth::kTpCount; ++k)
      if (k != fixdepth::kTpPSSetShader && fixdepth::g_tpSlot[k] >= 0) back = back && fake[fixdepth::g_tpSlot[k]] == real[fixdepth::g_tpSlot[k]];
    Check(ps >= 0 && srv >= 0 && fixdepth::InD3d11(real[srv]) && !fixdepth::InD3d11(foreign) && armed && back,
          "colour pass R47: tone-map toggle points only over d3d11's own entries (a foreign hook left alone), all back");
  }
  // R107: gpu_dedup.h (archived) held the Map / Unmap hooks R48 met; any function of this payload stands in for them
  // (the rule is d3ds::InPayload, not the census itself).
  // R48: one rule for every toggle-point arm, counted. The state-call points never go over the split filter's hook
  // (R47 / R49); the basic points (Flush, Map, GetData, Unmap) never over another hook of this payload (gpu_dedup.h's
  // Map hook re-arms after each call it forwards: the same cycle), while a third-party hook there is chained over.
  {
    using namespace fixdepth;
    void** real = *reinterpret_cast<void***>(r.ctx);
    std::vector<void*> fake(real, real + 160);
    OmSlots();
    const int ps = g_tpSlot[kTpPSSetShader], srv = g_tpSlot[kTpPSSetSRV], map = g_tpSlot[kTpMap],
              flush = g_tpSlot[kTpFlush];
    void* const sfHook = sfilt::kHooks[sfilt::kPS];      // the split filter's PSSetShader hook
    void* const gddMap = reinterpret_cast<void*>(&R107PayloadMapHook);  // stand-in: a Map hook of this payload
    void* const third = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Sleep"));
    fake[ps] = sfHook;
    fake[map] = gddMap;
    fake[flush] = third;
    const std::vector<void*> before = fake;
    const uint64_t skip0 = g_tpSkipForeign.load();
    bool own = true, back = true;
    if (!g_armTable) {  // an overlay pass's own arm (no arm of the colour pass open here)
      const OmFn om0 = g_omOrig;
      const OmUavFn omu0 = g_omUavOrig;
      const VpFn vp0 = g_vpOrig;
      g_tpExtended = true;
      g_tpCk = true;
      const bool armed = ArmOmEntries(fake.data());
      g_tpExtended = false;
      g_tpCk = false;
      own = armed && fake[ps] == sfHook && !g_tpArmed[kTpPSSetShader] && fake[srv] == TogglePointHook(kTpPSSetSRV) &&
            fake[map] == gddMap && !g_tpArmed[kTpMap] && fake[flush] == TogglePointHook(kTpFlush) &&
            g_tpOrig[kTpFlush] == third;
      DisarmOmEntries(fake.data());
      back = fake == before;
      g_omOrig = om0, g_omUavOrig = omu0, g_vpOrig = vp0;
    }
    for (auto& a : g_tpArmed) a = false;
    g_tpAddedUnmap = false;
    fake[g_tpSlot[kTpUnmap]] = reinterpret_cast<void*>(&R107PayloadUnmapHook);
    ArmTpUnmap(fake.data());  // R32's point in another feature's arm: not over gpu_dedup.h's Unmap hook either
    const bool unmap = fake[g_tpSlot[kTpUnmap]] == reinterpret_cast<void*>(&R107PayloadUnmapHook) && !g_tpArmed[kTpUnmap];
    DisarmTpUnmap(fake.data());
#ifndef R48_PREFIX
    const bool counted = g_tpSkipForeign.load() >= skip0 + 3;
#else
    const bool counted = true;
#endif
    Check(ps >= 0 && map >= 0 && flush >= 0 && third && own && back && unmap && counted,
          "R48 toggle points: none over a hook of ours (split filter, gpu dedup), a third-party Flush hook chained "
          "over, every skip counted, every entry back");
  }
  R50GpuTests(r);
  // Off again: nothing.
  cpass::g_mode = 0;
  Check(DrawView(r, 0) == 0, "colour pass: Mode 0 swaps nothing");
  cpass::g_obj.store(nullptr);
  cpass::g_rt = cpass::Rt{};
  cpass::g_timedFrames = 0, cpass::g_gpuUs = 0.0;
  for (auto& s : cpass::g_swaps) s = 0;
  for (auto& s : cpass::g_rejects) s = 0;
  SetParams(cgrade::FixDefaults(), 1);
  SetParams(cgrade::EnhancedDefaults(), 2);
  r.Release();
}

inline void Run() {
  PureTests();
  R50PureTests();
  GpuTests();
}

}  // namespace cmtest
