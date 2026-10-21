// R111: [Color] PanelGamut=0/1/2, an opt-in compensation for a wide-gamut panel (the Crystal Super micro-OLED, about
// DCI-P3) shown DCS's untagged sRGB content by a compositor that does not convert it (R111: the Pimax runtime's own
// 3x3 path is off at its default display_color_correction_screen_type 0). 1 = linear sRGB -> DCI-P3 (D65) on the
// display-linear tone, before the encode; 2 = half strength. Fix and Enhanced. Pure parts (keys, constant buffer),
// the matrix maths (against the primaries), greys exact, range kept, the pass on WARP against the CPU reference, and
// the GPU cost (QV_R111_HW=1: the hardware device, judged; else WARP, printed only).
#pragma once

namespace r111test {

using cmtest::Rig;

// The display-linear tone of one HDR colour (the encode inverted; fine dither and sRGB encode off; inputs bright
// enough that DCS's low-luminance Bayer factor is 1).
inline void ToneOf(const cgrade::ColorCb& c, const float in[3], float out[3]) {
  cgrade::DcsTone t;
  cgrade::ShadeIn s;
  for (int i = 0; i < 3; ++i) s.scene[i] = s.bloom[i] = in[i];
  s.adapted = 0.18f, s.ec = 0.0f;
  cgrade::ColorCb cc = c;
  cc.ditherMode = 0, cc.srgb = 0;
  float o[3];
  cgrade::Shade(t, &cc, s, o);
  for (int i = 0; i < 3; ++i) out[i] = std::pow(std::max(o[i], 0.0f), 2.2f);
}
inline cgrade::ModeParams Plain(cgrade::ModeParams p) {  // no state, no aux, no dither noise, DCS's encode
  p.adaptBright = p.adaptDark = p.meterFocus = 0;
  p.glare = 0, p.nightVision = 0, p.srgbMatch = 0, p.equalizeEc = 0, p.dither = 2;
  return p;
}
inline double Det3(const double a[3][3]) {
  return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
         a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
}
// Normalized primary matrix (RGB -> XYZ) from the primaries' and the white's xy.
inline void Npm(const double xy[3][2], const double w[2], double m[3][3]) {
  double P[3][3];
  const double W[3] = {w[0] / w[1], 1.0, (1 - w[0] - w[1]) / w[1]};
  for (int c = 0; c < 3; ++c) {
    P[0][c] = xy[c][0] / xy[c][1], P[1][c] = 1.0, P[2][c] = (1 - xy[c][0] - xy[c][1]) / xy[c][1];
  }
  const double d = Det3(P);
  double S[3];
  for (int c = 0; c < 3; ++c) {  // S = P^-1 W (Cramer)
    double Q[3][3];
    for (int r = 0; r < 3; ++r)
      for (int k = 0; k < 3; ++k) Q[r][k] = k == c ? W[r] : P[r][k];
    S[c] = Det3(Q) / d;
  }
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) m[r][c] = P[r][c] * S[c];
}
inline void Inv3(const double a[3][3], double o[3][3]) {
  const double d = Det3(a);
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) {
      const int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
      o[r][c] = (a[r1][c1] * a[r2][c2] - a[r1][c2] * a[r2][c1]) / d;
    }
}

inline void PureTests() {
  using namespace cgrade;
  {
    const ModeParams f = FixDefaults(), e = EnhancedDefaults();
    const ColorCb cf = BuildCb(f, true, 0.3, 1, 0.01, false), ce = BuildCb(e, true, 0.3, 1, 0.01, false);
    Check(f.panelGamut == 0 && e.panelGamut == 0 && cf.panelGamut == 0.0f && ce.panelGamut == 0.0f,
          "R111: PanelGamut defaults to 0 (off) in both modes; the constant buffer strength is 0");
  }
  {
    wchar_t dir[MAX_PATH], ini[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"r111", 0, ini);
    WritePrivateProfileStringW(L"Color", L"PanelGamut", L"1", ini);
    const ModeParams f1 = ReadModeIni(ini, 1), e1 = ReadModeIni(ini, 2);
    WritePrivateProfileStringW(L"Color", L"PanelGamut", L"2", ini);
    const ModeParams e2 = ReadModeIni(ini, 2);
    WritePrivateProfileStringW(L"Color", L"PanelGamut", L"3", ini);
    const ModeParams b3 = ReadModeIni(ini, 2);
    WritePrivateProfileStringW(L"Color", L"PanelGamut", L"-1", ini);
    const ModeParams bm = ReadModeIni(ini, 1);
    WritePrivateProfileStringW(L"Color", L"PanelGamut", nullptr, ini);
    WritePrivateProfileStringW(L"ColorFix", L"PanelGamut", L"1", ini);  // not a per-mode key
    WritePrivateProfileStringW(L"ColorEnhanced", L"PanelGamut", L"1", ini);
    const ModeParams fs = ReadModeIni(ini, 1), es = ReadModeIni(ini, 2);
    DeleteFileW(ini);
    const ColorCb c1 = BuildCb(e1, true, 0.3, 1, 0.01, false), c2 = BuildCb(e2, true, 0.3, 1, 0.01, false);
    Check(f1.panelGamut == 1 && e1.panelGamut == 1 && e2.panelGamut == 2 && b3.panelGamut == 0 && bm.panelGamut == 0 &&
              fs.panelGamut == 0 && es.panelGamut == 0 && c1.panelGamut == 1.0f && c2.panelGamut == 0.5f &&
              !SameMode(e1, e2) && !SameMode(e1, EnhancedDefaults()),
          "R111: [Color] PanelGamut read by both modes (1 -> strength 1, 2 -> 0.5), bad values -> 0, per-mode sections "
          "ignored, a change is a parameter change (hot reload)");
  }
  // The matrix: inv(NPM_P3) x NPM_709 from the primaries, every coefficient >= 0, rows summing to 1.
  {
    const double w[2] = {0.3127, 0.3290};
    const double p709[3][2] = {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}};
    const double pP3[3][2] = {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}};
    double n709[3][3], nP3[3][3], iP3[3][3];
    Npm(p709, w, n709), Npm(pP3, w, nP3), Inv3(nP3, iP3);
    double worst = 0, rowErr = 0;
    bool nonNeg = true;
    for (int r = 0; r < 3; ++r) {
      double sum = 0;
      for (int c = 0; c < 3; ++c) {
        const double m = iP3[r][0] * n709[0][c] + iP3[r][1] * n709[1][c] + iP3[r][2] * n709[2][c];
        worst = std::max(worst, std::fabs(m - kSrgbToP3[r][c]));
        nonNeg = nonNeg && kSrgbToP3[r][c] >= 0;
        sum += kSrgbToP3[r][c];
      }
      rowErr = std::max(rowErr, std::fabs(sum - 1));
    }
    // a colour converted and shown on P3 primaries has the sRGB colour's XYZ
    double xyzErr = 0;
    const double probe[4][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.9, 0.4, 0.1}};
    for (const auto& v : probe) {
      double conv[3];
      for (int r = 0; r < 3; ++r) conv[r] = kSrgbToP3[r][0] * v[0] + kSrgbToP3[r][1] * v[1] + kSrgbToP3[r][2] * v[2];
      for (int r = 0; r < 3; ++r) {
        const double a = n709[r][0] * v[0] + n709[r][1] * v[1] + n709[r][2] * v[2];
        const double b = nP3[r][0] * conv[0] + nP3[r][1] * conv[1] + nP3[r][2] * conv[2];
        xyzErr = std::max(xyzErr, std::fabs(a - b));
      }
    }
    printf("     (R111 matrix vs primaries: worst coefficient %.2e, row sum %.2e, XYZ round trip %.2e)\n", worst, rowErr,
           xyzErr);
    Check(worst < 1e-6 && rowErr < 3e-7 && nonNeg && xyzErr < 1e-6,
          "R111: kSrgbToP3 = inv(NPM P3-D65) x NPM sRGB (within 1e-6), coefficients >= 0, rows sum to 1, XYZ preserved");
  }
  // Greys exact; the range kept (each channel a weighted mean of the inputs); half strength is half the change.
  {
    bool grey = true, range = true, half = true;
    for (int k = 0; k <= 2000; ++k) {
      const float v = 2.0f * k / 2000;
      float t[3] = {v, v, v};
      ApplyPanelGamut(t, 1.0f);
      for (float c : t) grey = grey && std::fabs(c - v) <= 2.5e-7f * std::max(v, 1.0f);
    }
    unsigned seed = 11;
    for (int k = 0; k < 20000; ++k) {
      float in[3];
      for (float& c : in) {
        seed = seed * 1664525u + 1013904223u;
        c = 2.0f * static_cast<float>(seed >> 8) / 16777216.0f * ((seed & 7) == 0 ? 0.0f : 1.0f);
      }
      float a[3] = {in[0], in[1], in[2]}, b[3] = {in[0], in[1], in[2]};
      ApplyPanelGamut(a, 1.0f), ApplyPanelGamut(b, 0.5f);
      const float mn = std::min(in[0], std::min(in[1], in[2])), mx = std::max(in[0], std::max(in[1], in[2]));
      for (int i = 0; i < 3; ++i) {
        range = range && a[i] >= mn - 1e-6f && a[i] <= mx + 1e-6f && std::isfinite(a[i]);
        half = half && std::fabs((b[i] - in[i]) - 0.5f * (a[i] - in[i])) < 1e-6f;
      }
    }
    float z[3] = {0, 0, 0}, o[3] = {1, 1, 1};
    ApplyPanelGamut(z, 1.0f), ApplyPanelGamut(o, 1.0f);
    Check(grey && range && half && z[0] == 0 && z[1] == 0 && z[2] == 0 && std::fabs(o[0] - 1) < 2.5e-7f &&
              std::fabs(o[1] - 1) < 2.5e-7f && std::fabs(o[2] - 1) < 2.5e-7f,
          "R111: greys (0..2) exact, black 0, white 1; any input stays within its own min..max (in 0..1 -> in 0..1); "
          "strength 0.5 = half the change");
  }
  // In the pass: the display-linear tone with PanelGamut 1 = M x the tone without (Fix and Enhanced), i.e. applied in
  // linear light after the curve; greys give the same bytes through the whole shader (ramp -10..+6 EV, both encodes).
  {
    double worst = 0;
    int used = 0;
    bool bytesSame = true;
    for (int mode = 1; mode <= 2; ++mode) {
      ModeParams p = Plain(ModeDefaults(mode)), q = p;
      q.panelGamut = 1;
      const ColorCb c0 = BuildCb(p, false, 0, 1, 0, false), c1 = BuildCb(q, false, 0, 1, 0, false);
      const float cols[5][3] = {{1, 0.45f, 0.15f}, {0.1f, 1, 0.1f}, {1, 0.12f, 0.08f}, {0.35f, 0.55f, 1}, {0.4f, 0.55f, 0.2f}};
      for (const auto& c : cols)
        for (int k = 0; k <= 8; ++k) {
          const float l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2], s = 0.18f * std::exp2(0.5f * k) / l;
          const float in[3] = {c[0] * s, c[1] * s, c[2] * s};
          float t0[3], t1[3];
          ToneOf(c0, in, t0), ToneOf(c1, in, t1);
          auto encMean = [](const float t[3]) {  // DCS's mean encoded level: its low-luminance Bayer factor is 1 from 0.285
            return (std::pow(t[0], 1 / 2.2f) + std::pow(t[1], 1 / 2.2f) + std::pow(t[2], 1 / 2.2f)) / 3;
          };
          if (encMean(t0) < 0.29f || encMean(t1) < 0.29f) continue;
          ++used;
          float e[3] = {t0[0], t0[1], t0[2]};
          ApplyPanelGamut(e, 1.0f);
          for (int i = 0; i < 3; ++i) worst = std::max(worst, static_cast<double>(std::fabs(e[i] - t1[i])));
        }
      for (int enc = 0; enc < 2; ++enc) {
        ModeParams a = p, b = q;
        a.srgbMatch = b.srgbMatch = enc;
        a.dither = b.dither = 0;  // DCS's Bayer only: deterministic per pixel
        const ColorCb ca = BuildCb(a, false, 0, 1, 0, false), cbb = BuildCb(b, false, 0, 1, 0, false);
        for (int k = 0; k <= 320; ++k) {
          ShadeIn in;
          const float v = 0.18f * std::exp2(-10.0f + 16.0f * k / 320);
          for (int i = 0; i < 3; ++i) in.scene[i] = in.bloom[i] = v;
          in.adapted = 0.18f, in.ec = 0, in.x = k % 8, in.y = k / 8;
          float oa[3], ob[3];
          Shade(DcsTone{}, &ca, in, oa), Shade(DcsTone{}, &cbb, in, ob);
          for (int i = 0; i < 3; ++i) bytesSame = bytesSame && ToUnorm8(oa[i]) == ToUnorm8(ob[i]);
        }
      }
    }
    printf("     (R111 pass tone with PanelGamut 1 vs M x tone without: worst %.2e over %d colours)\n", worst, used);
    Check(worst < 2e-5 && used >= 60 && bytesSame,
          "R111: Fix and Enhanced apply M on the display-linear tone (before the encode); grey ramps give identical bytes");
  }
  // The direction of the change: a saturated HUD green (sRGB 90,255,110) is pre-desaturated, a grey does not move.
  {
    float hud[3] = {static_cast<float>(SrgbEotf(90 / 255.0)), 1.0f, static_cast<float>(SrgbEotf(110 / 255.0))};
    const float h0[3] = {hud[0], hud[1], hud[2]};
    ApplyPanelGamut(hud, 1.0f);
    printf("     (R111 HUD green 90,255,110 -> bytes %d,%d,%d with PanelGamut 1)\n", ToUnorm8(SrgbOetfF(hud[0])),
           ToUnorm8(SrgbOetfF(hud[1])), ToUnorm8(SrgbOetfF(hud[2])));
    Check(cmtest::SatOf(hud) < cmtest::SatOf(h0) && hud[1] < 1.0f, "R111: a saturated HUD green is pre-desaturated");
  }
}

inline void GpuTests() {
  Rig r;
  if (!cmtest::MakeRig(r)) {
    Check(false, "R111: WARP rig");
    r.Release();
    return;
  }
  cpass::g_rt = cpass::Rt{};
  cpass::g_fault = false;
  cpass::g_obj.store(&r.obj);
  for (int v = 0; v < 4; ++v) cpass::NoteLdr(v, cmtest::kW, cmtest::kH);
  for (int mode = 1; mode <= 2; ++mode) {
    cpass::g_mode = mode;
    for (int pg = 1; pg <= 2; ++pg)
      for (int enc = 0; enc < 2; ++enc) {
        cgrade::ModeParams p = Plain(cgrade::ModeDefaults(mode));
        p.panelGamut = pg, p.srgbMatch = enc, p.dither = enc ? 1 : 2;
        cmtest::SetParams(p, mode);
        cpass::g_rt.lastView = 99;
        const int act = cmtest::DrawView(r, 0);
        const std::vector<uint8_t> g = cmtest::ReadRt(r);
        const cgrade::ColorCb cb = cgrade::BuildCb(p, false, 0, cpass::g_rt.frame, 0, false);
        int worst = 0;
        double w1 = 0;
        cmtest::Compare(r, g, &cb, 0, 0.18f, 0.861f, &worst, &w1);
        char what[200];
        snprintf(what, sizeof what,
                 "R111: %s PanelGamut %d (%s encode) on WARP = CPU reference (worst <= 2 codes, > 99.5 %% within 1)",
                 mode == 2 ? "Enhanced" : "Fix", pg, enc ? "sRGB, fine dither" : "DCS's 2.2, Bayer");
        Check(act == 1 && worst <= 2 && w1 > 0.995, what);
      }
    // On vs off on the GPU (Bayer only, deterministic): the grey rows identical, the coloured rows changed.
    {
      cgrade::ModeParams p = Plain(cgrade::ModeDefaults(mode));
      p.dither = 0;
      std::vector<uint8_t> g[2];
      for (int pg = 0; pg < 2; ++pg) {
        p.panelGamut = pg;
        cmtest::SetParams(p, mode);
        cpass::g_rt.lastView = 99;
        cmtest::DrawView(r, 0);
        g[pg] = cmtest::ReadRt(r);
      }
      bool greySame = true;
      int changed = 0;
      for (UINT y = 0; y < cmtest::kH; ++y)
        for (UINT x = 0; x < cmtest::kW; ++x)
          for (int i = 0; i < 3; ++i) {
            const size_t o = (static_cast<size_t>(y) * cmtest::kW + x) * 4 + i;
            if ((y / 6) % 8 == 0)
              greySame = greySame && g[0][o] == g[1][o];
            else
              changed += g[0][o] != g[1][o];
          }
      char what[200];
      snprintf(what, sizeof what,
               "R111: %s on the GPU: PanelGamut 1 leaves the grey rows' bytes unchanged and changes the colours (%d)",
               mode == 2 ? "Enhanced" : "Fix", changed);
      Check(greySame && changed > 200, what);
    }
  }
  cpass::g_mode = 0;
  cpass::g_obj.store(nullptr);
  cpass::g_rt = cpass::Rt{};
  for (auto& s : cpass::g_swaps) s = 0;
  for (auto& s : cpass::g_rejects) s = 0;
  cmtest::SetParams(cgrade::FixDefaults(), 1);
  cmtest::SetParams(cgrade::EnhancedDefaults(), 2);
  r.Release();
}

// GPU cost: our tone-map pixel shader alone (cpass ps), full-screen at the live view sizes, DCS-like bindings, GPU
// timestamps (median of 15 samples of 10 draws). Fix and Enhanced, PanelGamut 0 and 1.
inline void CostBench() {
  const bool hw = getenv("QV_R111_HW") != nullptr;
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, hw ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1,
                               D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
    Check(false, "R111 cost: device");
    return;
  }
  auto rel = [](auto*& p) {
    if (p) p->Release();
    p = nullptr;
  };
  cpass::Blobs blobs;
  std::string err;
  std::vector<uint8_t> vsb;
  ID3D11PixelShader* ps = nullptr;
  ID3D11VertexShader* vs = nullptr;
  if (!cpass::CompileAll(blobs, err) || !cmtest::Compile(cmtest::kDcsVs, "vs_5_0", vsb) ||
      FAILED(dev->CreatePixelShader(blobs.ps.data(), blobs.ps.size(), nullptr, &ps)) ||
      FAILED(dev->CreateVertexShader(vsb.data(), vsb.size(), nullptr, &vs))) {
    Check(false, "R111 cost: shaders");
    rel(ctx), rel(dev);
    return;
  }
  ID3D11RasterizerState* cullNone = nullptr;
  {
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID, rs.CullMode = D3D11_CULL_NONE, rs.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rs, &cullNone);
  }
  ID3D11Query *dj = nullptr, *ts[2] = {};
  D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, qt{D3D11_QUERY_TIMESTAMP, 0};
  dev->CreateQuery(&qd, &dj), dev->CreateQuery(&qt, &ts[0]), dev->CreateQuery(&qt, &ts[1]);
  // shared bindings
  float b0d[64];
  cgrade::PackDcsTone(cgrade::DcsTone{}, b0d);
  auto mkCb = [&](const void* data, UINT bytes) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = (bytes + 15) & ~15u, d.Usage = D3D11_USAGE_DEFAULT, d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    std::vector<uint8_t> z(d.ByteWidth, 0);
    if (data) memcpy(z.data(), data, bytes);
    D3D11_SUBRESOURCE_DATA s{z.data(), 0, 0};
    ID3D11Buffer* b = nullptr;
    dev->CreateBuffer(&d, &s, &b);
    return b;
  };
  ID3D11Buffer* b0 = mkCb(b0d, 256);
  float amb[36] = {};
  for (int i = 0; i < 9; ++i) amb[i * 4] = amb[i * 4 + 1] = amb[i * 4 + 2] = 1;
  ID3D11Buffer* b8 = mkCb(amb, sizeof amb);
  const uint32_t view4[4] = {4, 0, 0, 0};
  ID3D11Buffer* b12 = mkCb(view4, 16);
  ID3D11Buffer *lum = nullptr, *st = nullptr;
  ID3D11ShaderResourceView *lumSrv = nullptr, *stSrv = nullptr;
  {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 64, bd.Usage = D3D11_USAGE_DEFAULT, bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, bd.StructureByteStride = 8;
    const float d[16] = {0, 1, 0.18f, 0, 0.18f, 0.0324f};
    D3D11_SUBRESOURCE_DATA s{d, 0, 0};
    dev->CreateBuffer(&bd, &s, &lum), dev->CreateShaderResourceView(lum, nullptr, &lumSrv);
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = cpass::kStateBytes, sd.Usage = D3D11_USAGE_DEFAULT, sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    std::vector<uint8_t> z(cpass::kStateBytes, 0);
    D3D11_SUBRESOURCE_DATA ss{z.data(), 0, 0};
    dev->CreateBuffer(&sd, &ss, &st);
    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = DXGI_FORMAT_R32_TYPELESS, v.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    v.BufferEx.NumElements = cpass::kStateBytes / 4, v.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    dev->CreateShaderResourceView(st, &v, &stSrv);
  }
  struct Cfg {
    const char* name;
    cgrade::ColorCb cb;
  };
  cgrade::ModeParams fix = cgrade::FixDefaults();
  fix.adaptBright = fix.adaptDark = fix.meterFocus = 0;
  cgrade::ModeParams enh = cgrade::EnhancedDefaults();
  enh.glare = 0, enh.nightVision = 0, enh.adaptBright = enh.adaptDark = enh.meterFocus = 0;  // the tone stage's cost
  cgrade::ModeParams fixP = fix, enhP = enh;
  fixP.panelGamut = 1, enhP.panelGamut = 1;
  Cfg cfgs[4] = {{"Fix", cgrade::BuildCb(fix, true, 0.33, 1, 0, false)},
                 {"Fix + PanelGamut 1", cgrade::BuildCb(fixP, true, 0.33, 1, 0, false)},
                 {"Enhanced", cgrade::BuildCb(enh, true, 0.33, 1, 0, false)},
                 {"Enhanced + PanelGamut 1", cgrade::BuildCb(enhP, true, 0.33, 1, 0, false)}};
  const UINT sizes[2][2] = {{1040, 1028}, {2028, 2004}};
  double us[4][2] = {};
  for (int si = 0; si < 2; ++si) {
    const UINT W = sizes[si][0], H = sizes[si][1];
    std::vector<uint16_t> half(size_t(W) * H * 4);
    unsigned seed = 1;
    for (size_t i = 0; i < size_t(W) * H; ++i) {  // varied colours over -8..+8 EV (both branches of every curve)
      seed = seed * 1664525u + 1013904223u;
      const float e = std::exp2(-8.0f + 16.0f * static_cast<float>((seed >> 8) & 0xffff) / 65535.0f) * 0.18f;
      for (int c = 0; c < 3; ++c) {
        seed = seed * 1664525u + 1013904223u;
        half[i * 4 + c] = cmtest::FloatToHalf(e * (0.1f + static_cast<float>((seed >> 8) & 0xff) / 255.0f));
      }
      half[i * 4 + 3] = cmtest::FloatToHalf(1.0f);
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W, td.Height = H, td.MipLevels = 1, td.ArraySize = 1, td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT, td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{half.data(), W * 8, 0};
    ID3D11Texture2D *hdr = nullptr, *rt = nullptr;
    ID3D11ShaderResourceView* hdrSrv = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateTexture2D(&td, &sd, &hdr), dev->CreateShaderResourceView(hdr, nullptr, &hdrSrv);
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM, td.BindFlags = D3D11_BIND_RENDER_TARGET;
    dev->CreateTexture2D(&td, nullptr, &rt), dev->CreateRenderTargetView(rt, nullptr, &rtv);
    for (int k = 0; k < 4; ++k) {
      ID3D11Buffer* b13 = mkCb(&cfgs[k].cb, sizeof(cgrade::ColorCb));
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      ctx->IASetInputLayout(nullptr);
      ctx->VSSetShader(vs, nullptr, 0), ctx->PSSetShader(ps, nullptr, 0);
      ID3D11ShaderResourceView* srv[3] = {hdrSrv, lumSrv, hdrSrv};
      ctx->PSSetShaderResources(0, 3, srv);
      ctx->PSSetShaderResources(cpass::kPsStateSlot, 1, &stSrv);
      ctx->PSSetConstantBuffers(0, 1, &b0), ctx->PSSetConstantBuffers(8, 1, &b8);
      ctx->PSSetConstantBuffers(cpass::kPsViewSlot, 1, &b12), ctx->PSSetConstantBuffers(cpass::kPsCbSlot, 1, &b13);
      ctx->OMSetRenderTargets(1, &rtv, nullptr);
      ctx->RSSetState(cullNone);  // DCS's tone map: cull none (the strip's winding)
      const D3D11_VIEWPORT vp{0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
      ctx->RSSetViewports(1, &vp);
      const int per = hw ? 10 : 1, reps = hw ? 15 : 3;
      std::vector<double> v;
      for (int i = 0; i < reps + 2; ++i) {
        ctx->Begin(dj), ctx->End(ts[0]);
        for (int d = 0; d < per; ++d) ctx->Draw(4, 0);
        ctx->End(ts[1]), ctx->End(dj);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{};
        while (ctx->GetData(dj, &dd, sizeof dd, 0) == S_FALSE) YieldProcessor();
        UINT64 a = 0, b = 0;
        while (ctx->GetData(ts[0], &a, sizeof a, 0) == S_FALSE) YieldProcessor();
        while (ctx->GetData(ts[1], &b, sizeof b, 0) == S_FALSE) YieldProcessor();
        if (i >= 2 && !dd.Disjoint && dd.Frequency) v.push_back((b - a) * 1e6 / double(dd.Frequency) / per);
      }
      std::sort(v.begin(), v.end());
      us[k][si] = v.empty() ? -1 : v[v.size() / 2];
      ID3D11RenderTargetView* none = nullptr;
      ctx->OMSetRenderTargets(1, &none, nullptr);
      rel(b13);
    }
    rel(rtv), rel(rt), rel(hdrSrv), rel(hdr);
  }
  double frame[4];
  for (int k = 0; k < 4; ++k) {
    frame[k] = 2 * us[k][0] + 2 * us[k][1];
    printf("     (R111 cost, %s: %s %.1f us periphery 1040x1028, %.1f us focus 2028x2004, %.1f us per frame of 4 views)\n",
           hw ? "GPU" : "WARP, not judged", cfgs[k].name, us[k][0], us[k][1], frame[k]);
  }
  if (hw)
    Check(frame[1] - frame[0] <= 10.0 && frame[3] - frame[2] <= 10.0,
          "R111 cost (GPU): PanelGamut 1 adds <= 0.01 ms per frame (4 views) in Fix and in Enhanced");
  else
    Check(frame[0] > 0 && frame[3] > 0, "R111 cost (WARP): the bench ran (set QV_R111_HW=1 to judge on the GPU)");
  rel(cullNone), rel(ps), rel(vs), rel(b0), rel(b8), rel(b12), rel(lumSrv), rel(lum), rel(stSrv), rel(st);
  rel(dj), rel(ts[0]), rel(ts[1]), rel(ctx), rel(dev);
}

inline void Run() {
  PureTests();
  GpuTests();
  CostBench();
}

}  // namespace r111test
