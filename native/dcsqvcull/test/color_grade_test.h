// [Color] pure parts: the R41 probes (copy classification, half floats, downsample, luminance, key defaults).
#pragma once

namespace cgtest {

void Run() {
  using namespace cgrade;
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
