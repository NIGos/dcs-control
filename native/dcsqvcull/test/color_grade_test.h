// R40 [Color]: pure parts of the opt-in grading (curve, saturation, ini defaults, shader compiles).
#pragma once

namespace cgtest {

void Run() {
  using namespace cgrade;
  uint8_t lut[256];

  // All keys 0: nothing active, and the curve is the identity (no byte changes).
  Params off;
  Check(!Active(off), "color: default params inactive");
  BuildCurve(off, lut);
  bool ident = true;
  for (int i = 0; i < 256; ++i) ident &= lut[i] == i;
  Check(ident, "color: curve with all keys 0 is the identity");

  // Missing ini: every key 0.
  const Params fromNone = ReadIni(L"Z:\\no\\such\\dir\\none.ini");
  Check(!Active(fromNone) && fromNone.outputGamma == 2.2f, "color: missing ini reads as off, gamma 2.2");

  // SrgbMatch: ends fixed, monotonic, the displayed light (sRGB decode) matches DCS's 2.2 intent
  // within one 8-bit step of the sRGB encoding everywhere, and near-black codes are lowered.
  Params m;
  m.srgbMatch = 1;
  BuildCurve(m, lut);
  bool mono = true, close = true;
  for (int i = 1; i < 256; ++i) mono &= lut[i] >= lut[i - 1];
  for (int i = 0; i < 256; ++i) {
    const double want = Intended(i, 2.2);
    const double lo = SrgbEotf(std::max(0, lut[i] - 1) / 255.0), hi = SrgbEotf(std::min(255, lut[i] + 1) / 255.0);
    close &= want >= lo - 1e-9 && want <= hi + 1e-9;
  }
  Check(lut[0] == 0 && lut[255] == 255, "color: SrgbMatch keeps 0 and 255");
  Check(mono, "color: SrgbMatch curve monotonic");
  Check(close, "color: SrgbMatch shows DCS's intended light within one sRGB step");
  Check(lut[5] < 5 && lut[16] < 16 && lut[128] >= 126 && lut[128] <= 130,
        "color: SrgbMatch lowers near-black codes, leaves mid-grey");

  // BlackFloor: lowers the bottom, 0 and 255 fixed, monotonic.
  Params bf;
  bf.blackFloor = 10;
  BuildCurve(bf, lut);
  mono = true;
  for (int i = 1; i < 256; ++i) mono &= lut[i] >= lut[i - 1];
  Check(lut[0] == 0 && lut[255] == 255 && mono && lut[8] < 8, "color: BlackFloor monotonic, ends fixed, crushes floor");

  // Contrast: ends fixed, monotonic both ways, darker shadows / brighter highlights for +.
  for (int c : {-30, 15, 30}) {
    Params ct;
    ct.contrast = c;
    BuildCurve(ct, lut);
    mono = true;
    for (int i = 1; i < 256; ++i) mono &= lut[i] >= lut[i - 1];
    Check(lut[0] == 0 && lut[255] == 255 && mono, "color: Contrast monotonic with ends fixed");
    if (c > 0) Check(lut[60] <= 60 && lut[220] >= 220, "color: Contrast + deepens shadows, lifts highlights");
  }

  // Sanitize clamps typos.
  Params bad;
  bad.blackFloor = 99999, bad.contrast = 500, bad.saturation = -500, bad.outputGamma = 9.0f;
  const Params s = Sanitize(bad);
  Check(s.blackFloor == 200 && s.contrast == 50 && s.saturation == -50 && s.outputGamma == 2.2f,
        "color: Sanitize clamps out-of-range keys");

  // Saturation keeps greys and luma.
  double g[3] = {0.3, 0.3, 0.3};
  Saturate(g, 20);
  Check(std::fabs(g[0] - 0.3) < 1e-12 && std::fabs(g[2] - 0.3) < 1e-12, "color: Saturation keeps grey");
  double c3[3] = {0.4, 0.2, 0.1};
  const double y0 = 0.2126 * 0.4 + 0.7152 * 0.2 + 0.0722 * 0.1;
  Saturate(c3, 20);
  const double y1 = 0.2126 * c3[0] + 0.7152 * c3[1] + 0.0722 * c3[2];
  Check(std::fabs(y0 - y1) < 1e-9 && c3[0] > 0.4, "color: Saturation keeps luma, spreads chroma");

  // The pass's pixel shader compiles (ps_5_0).
  ID3DBlob* code = nullptr;
  ID3DBlob* err = nullptr;
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  HMODULE dc = LoadLibraryW((std::wstring(sys) + L"\\d3dcompiler_47.dll").c_str());
  const pD3DCompile compile = dc ? reinterpret_cast<pD3DCompile>(GetProcAddress(dc, "D3DCompile")) : nullptr;
  const HRESULT hr = compile ? compile(kPsHlsl, strlen(kPsHlsl), "color_grade", nullptr, nullptr, "main", "ps_5_0", 0,
                                       0, &code, &err)
                             : E_FAIL;
  if (FAILED(hr) && err) printf("%s\n", static_cast<const char*>(err->GetBufferPointer()));
  Check(SUCCEEDED(hr) && code, "color: grading PS compiles");
  if (code) code->Release();
  if (err) err->Release();

  // R41 probes (pure parts): copy classification, half floats, downsample, luminance statistics, key defaults.
  {
    std::vector<uint8_t> a(256 * 4), same(256 * 4), enc(256 * 4), junk(256 * 4);
    for (int i = 0; i < 256; ++i)
      for (int ch = 0; ch < 4; ++ch) {
        a[i * 4 + ch] = static_cast<uint8_t>(i);
        same[i * 4 + ch] = static_cast<uint8_t>(i);
        enc[i * 4 + ch] = SrgbEncodeByte(static_cast<uint8_t>(i));
        junk[i * 4 + ch] = static_cast<uint8_t>(255 - i);
      }
    const CopyClass ce = ClassifyCopy(a.data(), same.data(), 256), cs = ClassifyCopy(a.data(), enc.data(), 256),
                    cj = ClassifyCopy(a.data(), junk.data(), 256);
    Check(ce.n == 768 && ce.equal == 768 && CopyVerdict(ce) == 'A', "probe: unchanged copy classified A");
    Check(cs.srgb > 600 && CopyVerdict(cs) == 'B', "probe: sRGB re-encode classified B");
    Check(CopyVerdict(cj) == '?' && CopyVerdict(CopyClass{}) == '?', "probe: unrelated bytes / empty give ?");
    Check(SrgbEncodeByte(0) == 0 && SrgbEncodeByte(255) == 255 && SrgbEncodeByte(50) > 110,
          "probe: sRGB byte encode lifts darks, keeps ends");
    Check(HalfToFloat(0x3c00) == 1.0f && HalfToFloat(0xc000) == -2.0f && HalfToFloat(0) == 0.0f &&
              std::fabs(HalfToFloat(0x0001) - 5.96046e-8f) < 1e-12f && std::isinf(HalfToFloat(0x7c00)),
          "probe: half to float (normal, negative, zero, denormal, inf)");
    // 8 x 4 RGBA16F image: left half 1.0, right half 0.5, one NaN texel ignored as 0.
    std::vector<uint16_t> img(8 * 4 * 4, 0);
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 8; ++x)
        for (int c = 0; c < 4; ++c) img[(y * 8 + x) * 4 + c] = x < 4 ? 0x3c00 : 0x3800;
    img[(0 * 8 + 7) * 4 + 0] = 0x7e00;  // NaN
    std::vector<float> ds;
    unsigned ow = 0, oh = 0;
    DownsampleHalf(reinterpret_cast<const uint8_t*>(img.data()), 8 * 8, 8, 4, 4, ds, &ow, &oh);
    Check(ow == 2 && oh == 1 && ds.size() == 6 && ds[0] == 1.0f && ds[3] == 0.5f * 15 / 16 && ds[4] == 0.5f,
          "probe: HDR box downsample (factor 4, NaN as 0)");
    const LumStats ls = LumStatsOf(std::vector<float>{1, 1, 1, 0.25f, 0.25f, 0.25f});
    Check(ls.n == 2 && std::fabs(ls.mean - 0.625) < 1e-9 && std::fabs(ls.logAvg - 0.5) < 1e-6 && ls.max == 1.0 &&
              ls.p1 == 0.25,
          "probe: luminance mean, log-average, percentiles");
    Check(g_toneProbeReq.load() == 0 && g_swapProbeReq.load() == 0 && g_geoOnlyProbe.load() == 0 &&
              g_toneProbeHdr.load() == 0,
          "probe: every probe key defaults to off");
  }
}

}  // namespace cgtest
