// R139: the measure-only VRS probe (src\vrs_probe.h). NvAPI itself cannot run on WARP: the pure parts are checked
// directly, the render-thread state machine on a WARP device with a fake nvapi_QueryInterface (its VRS calls recorded,
// the shading-rate view a plain R8_UINT SRV of the surface):
//   - the keys (defaults, values, clamps, bad values with their note), ParseRate, the NvAPI struct versions;
//   - the arm scheduler (ABBA / BAAB quartets) and the paired effect (a linear drift cancels), the kill gate;
//   - pass classes and the arms' pass sets;
//   - the ring tile map: R138's 10 degree share of a 0.25-share focus view, conservative tiles, whole view, an
//     off-centre gaze, GazePixel / PixelDir round trip;
//   - Bind's fail-safe paths (no NvAPI, Initialize, a missing entry, caps v2 refused then v1, no VRS bit);
//   - a simulated frame on WARP (cascades, height map, four HMD views identified by their TerrainContext upload): the
//     state only in the arm's passes of the selected views, cleared at every pass end, re-applied after OM / viewport /
//     ExecuteCommandList; identification by the learnt depth texture without the upload; cascades for A4; the ring's
//     coarse share for A3; the enlight gap opened and closed by g_vrsPre; the probe stopped with d3d11's entries back.
#pragma once

namespace vrstest {

std::wstring TempIni(const char* tag) {
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  return std::wstring(tmp) + L"qv_vrs_" + std::wstring(tag, tag + strlen(tag)) + L"_" +
         std::to_wstring(GetCurrentProcessId()) + L".ini";
}

void KeysParse() {
  using namespace vrsp;
  const std::wstring ini = TempIni("keys");
  DeleteFileW(ini.c_str());
  std::string n0;
  const Keys d = ReadKeys(ini.c_str(), &n0);
  const bool defOk = d.blockSec == 0 && d.rate == nvvrs::k2x2 && d.views == 0xC && d.passes == 0 && d.innerDeg == 0 &&
                     d.ringDeg == 10 && d.arms == 0x7E && d.repeats == 4 && d.startDelay == 10 && d.settleMs == 1500 &&
                     d.imageFrames == 0 && d.imageArms == 0x58 && d.imageWaitSec == 20 && n0.empty();
  const wchar_t* kv[][2] = {{L"VrsProbe", L"5"},           {L"VrsProbeRate", L" 4X4 "},    {L"VrsProbeViews", L"0x4"},
                            {L"VrsProbePasses", L"0x3"},   {L"VrsProbeInnerDeg", L"12.5"}, {L"VrsProbeRingDeg", L"8"},
                            {L"VrsProbeArms", L"0xFE"},    {L"VrsProbeRepeats", L"2"},     {L"VrsProbeStartDelay", L"0"},
                            {L"VrsProbeSettleMs", L"50"},  {L"VrsProbeImage", L"3"},       {L"VrsProbeImageArms", L"8"},
                            {L"VrsProbeImageWaitSec", L"1000"}};
  for (auto& p : kv) WritePrivateProfileStringW(L"Suite", p[0], p[1], ini.c_str());
  std::string n1;
  const Keys k = ReadKeys(ini.c_str(), &n1);
  const bool valOk = k.blockSec == 5 && k.rate == nvvrs::k4x4 && k.views == 0x4 && k.passes == 3 &&
                     std::fabs(k.innerDeg - 12.5) < 1e-9 && k.ringDeg == 8 && k.arms == 0xFE && k.repeats == 2 &&
                     k.startDelay == 0 && k.settleMs == 200 && k.imageFrames == 3 && k.imageArms == 8 &&
                     k.imageWaitSec == 120 && n1.empty();
  // R140: VrsProbePasses with the R139 custom bit 6 only: A6 runs, the custom arm (now bit 7) is not scheduled (noted).
  WritePrivateProfileStringW(L"Suite", L"VrsProbeArms", L"0x40", ini.c_str());
  std::string nl;
  const Keys l = ReadKeys(ini.c_str(), &nl);
  const bool legacy = l.arms == 0x40 && nl.find("custom arm A7") != std::string::npos;
  // Bad values: each replaced by its default with a note.
  WritePrivateProfileStringW(L"Suite", L"VrsProbeRate", L"3x3", ini.c_str());
  WritePrivateProfileStringW(L"Suite", L"VrsProbeViews", L"zz", ini.c_str());
  WritePrivateProfileStringW(L"Suite", L"VrsProbePasses", L"0", ini.c_str());
  WritePrivateProfileStringW(L"Suite", L"VrsProbeArms", L"0x80", ini.c_str());
  WritePrivateProfileStringW(L"Suite", L"VrsProbeInnerDeg", L"-3", ini.c_str());
  WritePrivateProfileStringW(L"Suite", L"VrsProbeRingDeg", L"nan", ini.c_str());
  WritePrivateProfileStringW(L"Suite", L"VrsProbe", L"999", ini.c_str());
  std::string n2;
  const Keys b = ReadKeys(ini.c_str(), &n2);
  const bool badOk = b.rate == nvvrs::k2x2 && b.views == 0xC && b.arms == 0x7E && b.innerDeg == 0 && b.ringDeg == 10 &&
                     b.blockSec == 60 && n2.find("VrsProbeRate") != std::string::npos &&
                     n2.find("VrsProbeViews") != std::string::npos && n2.find("custom arm") != std::string::npos &&
                     n2.find("VrsProbeInnerDeg") != std::string::npos && n2.find("VrsProbeRingDeg") != std::string::npos;
  DeleteFileW(ini.c_str());
  const bool rates = ParseRate(L"2x2") == nvvrs::k2x2 && ParseRate(L"1X2") == nvvrs::k1x2 &&
                     ParseRate(L"2x1") == nvvrs::k2x1 && ParseRate(L"2x4") == nvvrs::k2x4 &&
                     ParseRate(L"4x2") == nvvrs::k4x2 && ParseRate(L"4x4") == nvvrs::k4x4 && ParseRate(L"1x1") == -1 &&
                     ParseRate(L"") == -1 && ParseRate(nullptr) == -1 && RateArea(nvvrs::k2x2) == 4 &&
                     RateArea(nvvrs::k4x4) == 16 && !strcmp(RateName(nvvrs::k2x4), "2x4");
  // NvAPI versions: MAKE_NVAPI_VERSION(sizeof, ver) of the SDK's structs.
  const bool vers = nvvrs::kCapsVer2 == 0x20040u && nvvrs::kCapsVer1 == 0x10020u && nvvrs::kViewportsRateVer == 0x10010u &&
                    nvvrs::kShadingRateViewVer == 0x10018u && nvvrs::kId_D3D11_CreateShadingRateResourceView == 0x99ca2dffu &&
                    nvvrs::kId_D3D11_RSSetShadingRateResourceView == 0x1b0c2f83u &&
                    nvvrs::kId_D3D11_RSSetViewportsPixelShadingRates == 0x34f7938fu &&
                    nvvrs::kId_D3D1x_GetGraphicsCapabilities == 0x52b1499au;
  printf("     keys: notes for the bad set \"%s\"\n", n2.c_str());
  printf("     keys: defOk %d valOk %d legacy %d badOk %d rates %d vers %d\n", defOk, valOk, legacy, badOk, rates, vers);
  Check(defOk && valOk && legacy && badOk && rates && vers,
        "R139/R140 VRS probe keys: defaults (off, 2x2, views 0xC, arms 0x7E = A1..A6, image arms A3 A4 A6, 4 quartets, "
        "start delay 10 s), values read and clamped, bad values replaced with a note, the custom arm on bit 7 (bit 6 "
        "with VrsProbePasses noted); rates parsed; NvAPI ids = R138's driver ids, struct versions");
}

void ScheduleAndGate() {
  using namespace vrsp;
  const std::vector<Block> s = Schedule(0x3E, 4);
  bool shape = s.size() == 80;
  int on[kArmCount] = {}, off[kArmCount] = {};
  for (size_t i = 0; i + 3 < s.size() && shape; i += 4) {
    const Block& a = s[i];
    shape = shape && s[i + 1].arm == a.arm && s[i + 2].arm == a.arm && s[i + 3].arm == a.arm && s[i + 3].on == a.on &&
            s[i + 1].on == !a.on && s[i + 2].on == !a.on && a.on == ((a.rep & 1) != 0);
  }
  for (const Block& b : s) ++(b.on ? on : off)[b.arm];
  for (int a = 1; a <= 5; ++a) shape = shape && on[a] == 8 && off[a] == 8;
  shape = shape && on[6] == 0 && Schedule(0x44, 1).size() == 8 && Schedule(0x40, 3).size() == 12 &&
          Schedule(kDefArms, 4).size() == 96 && Schedule(0x80, 1).size() == 4 && Schedule(0x80, 1)[0].arm == 7;
  // A linear drift of 0.01 ms per block and a true effect of -0.3 ms (A2), -0.12 (A4): ABBA recovers them exactly.
  std::vector<double> v(s.size());
  std::vector<bool> ok(s.size(), true);
  for (size_t i = 0; i < s.size(); ++i)
    v[i] = 15.0 + 0.01 * i + (s[i].on ? (s[i].arm == 2 ? -0.3 : s[i].arm == 4 ? -0.12 : 0.0) : 0.0);
  const Effect e2 = PairedEffect(s, v, ok, 2), e4 = PairedEffect(s, v, ok, 4), e1 = PairedEffect(s, v, ok, 1);
  const bool drift = e2.n == 4 && std::fabs(e2.mean + 0.3) < 1e-9 && e2.se < 1e-9 && std::fabs(e4.mean + 0.12) < 1e-9 &&
                     std::fabs(e1.mean) < 1e-9;
  // A lost block: its quartet still counts with the remaining blocks; a quartet without ON blocks is dropped.
  ok[4 * 1 + 1] = false;  // arm 2, repeat 0, an ON block
  std::vector<bool> ok2 = ok;
  for (size_t i = 0; i < s.size(); ++i)
    if (s[i].arm == 3 && s[i].rep == 0 && s[i].on) ok2[i] = false;
  const Effect l2 = PairedEffect(s, v, ok, 2), l3 = PairedEffect(s, v, ok2, 3);
  const bool lost = l2.n == 4 && l3.n == 3;
  // Gates (R138 6).
  Effect a2, a3, a4;
  a2.n = a3.n = a4.n = 4;
  a2.mean = -0.3, a3.mean = -0.2, a4.mean = -0.1;
  const Gate g1 = Decide(a2, a3, a4);
  a2.mean = -0.4, a4.mean = -0.3;
  const Gate g2 = Decide(a2, a3, a4);
  a3.mean = -0.31, a4.mean = -0.05, a2.mean = -0.5;
  const Gate g3 = Decide(a2, a3, a4);
  Effect none;
  const Gate g4 = Decide(a2, a3, none);
  // R140: A6 alone at 0.3 ms offers too (A3 and A4 below the bar, A2 + A4 above the kill line).
  Effect a6;
  a6.n = 4, a6.mean = -0.32;
  a2.mean = -0.6, a3.mean = -0.1, a4.mean = -0.05;
  const Gate g5 = Decide(a2, a3, a4, a6), g6 = Decide(a2, a3, a4);
  const bool gates = g1.decided && g1.close && !g1.offer && g2.decided && !g2.close && g2.offer && g3.decided &&
                     !g3.close && g3.offer && !g4.decided && !g4.close && g5.offer && !g5.close &&
                     std::fabs(g5.saveA6 - 0.32) < 1e-12 && !g6.offer;
  Check(shape && drift && lost && gates,
        "R139 VRS probe schedule: per repeat and arm OFF ON ON OFF / ON OFF OFF ON (80 blocks for 5 arms x 4); the "
        "paired effect cancels a linear drift exactly and survives a lost block; gate CLOSE below 0.5 ms (A2 + A4), "
        "OFFER at 0.3 ms (A3 or A4), UNDECIDED without A4");
}

void Classes() {
  using namespace vrsp;
  struct C {
    const char* n;
    int c;
  };
  const C t[] = {{"GBufferPass", kCGBuffer},        {"SSAO", kCSsao},
                 {"SSAOPass", kCSsao},              {"GBufferLightingPass", kCLight},
                 {"LightsPrePass", kCLight},        {"LightsBuffersPass", kCLights},
                 {"SSLR", kCLight},                 {"TerrainShadowPass", kCLight},
                 {"DepthResolverPass", kCLight},    {"ComposedShadowBlur", kCLight},
                 {"SSSPass", kCLight},              {"TransparentDecalPass", kCTransp},
                 {"TransparentPass", kCTransp},     {"TransparentCockpitPass", kCExcl},
                 {"SimpleFlatShadowPass", kCOther}, {"HotAirPass", kCOther},
                 {"SkyPass", kCOther},              {"", kCOther},
                 {"WaterWaveMaskPass", kCExcl},     {"WaterSeaPass", kCExcl},
                 {"GUIPass", kCExcl},               {"DLSSPass", kCDlss},
                 {"ToneMappingPass", kCPost},       {"OverlayHDRPass", kCPost},
                 {"HistogramPass", kCPost},         {"DepthUpscalePass", kCExcl},
                 {"CopyToHMDPass", kCHmd},          {"HeightMapPass", kCFrame},
                 {"CascadeShadowPass2", kCCascade}};
  int bad = 0;
  for (const C& c : t)
    if (ClassOfName(c.n) != c.c) {
      ++bad;
      printf("     class of \"%s\": %d, want %d\n", c.n, ClassOfName(c.n), c.c);
    }
  const bool casc = CascadeOfName("CascadeShadowPass2") == 2 && CascadeOfName("CascadeShadowPass0") == 0 &&
                    CascadeOfName("CascadeShadowPass") == -1 && CascadeOfName("CascadeShadowPass12") == -1 &&
                    CascadeOfName("GBufferPass") == -1;
  const Keys k;
  const uint32_t a1 = ArmOf(1, k).passes, a2 = ArmOf(2, k).passes, a4 = ArmOf(4, k).passes, a5 = ArmOf(5, k).passes;
  const bool wants = Wants(a1, kCGBuffer, true) && !Wants(a1, kCGBuffer, false) && !Wants(a1, kCSsao, false) &&
                     Wants(a2, kCGBuffer, false) && Wants(a2, kCOther, false) && Wants(a2, kCTransp, false) &&
                     !Wants(a2, kCDlss, false) && !Wants(a2, kCExcl, false) && !Wants(a2, kCPost, false) &&
                     !Wants(a2, kCCascade, false) && Wants(a5, kCSsao, false) && !Wants(a5, kCLight, false) &&
                     !Wants(a5, kCGBuffer, true) && !Wants(a4, kCSsao, false) && ArmOf(3, k).innerDeg == 10 &&
                     ArmOf(2, k).innerDeg == 0 && (a2 & kPEnlight) && !(a2 & kPCascade) && a4 == kPCascade;
  // R140: A6 = SSAO + lighting group in the ring (VrsProbeRingDeg); A7 the custom arm.
  Keys kc;
  kc.passes = kPTransp, kc.innerDeg = 7, kc.ringDeg = 12;
  const ArmDef d6 = ArmOf(6, kc), d7 = ArmOf(7, kc);
  const bool a6ok = d6.passes == (kPSsao | kPLight) && d6.innerDeg == 12 && Wants(d6.passes, kCSsao, false) &&
                    Wants(d6.passes, kCLight, false) && !Wants(d6.passes, kCGBuffer, true) &&
                    !Wants(d6.passes, kCTransp, false) && !Wants(d6.passes, kCOther, false) && !(d6.passes & kPEnlight) &&
                    d7.passes == kPTransp && d7.innerDeg == 7 && !strcmp(ArmName(6), "SSAO + lighting, ring");
  Check(bad == 0 && casc && wants && a6ok,
        "R139/R140 VRS probe pass classes: G-buffer / SSAO / lighting group / transparents / other raster; cockpit, "
        "water, GUI, upscale excluded; DLSS and tone map / overlays end the region; the arms' pass sets (A1 main "
        "G-buffer only, A2 every pre-DLSS raster pass + enlight, A4 cascades only, A5 SSAO only, A6 SSAO + lighting in "
        "the ring, A7 custom)");
}

void Ring() {
  using namespace vrsp;
  const double d2r = 3.14159265358979323846 / 180.0;
  RingIn r;
  r.tw = 59, r.th = 59;  // R138: a 0.25-share focus view at DLSS Balanced, 944 x 931
  r.vw = 944, r.vh = 931;
  r.sx = 1.0 / std::tan(17.7 * d2r), r.sy = 1.0 / std::tan(17.5 * d2r);
  r.gpx = r.vw / 2, r.gpy = r.vh / 2;
  r.innerDeg = 10;
  std::vector<uint8_t> m(59 * 59);
  const RingStats st = BuildTiles(r, m.data());
  const double share = st.tilesIn ? static_cast<double>(st.coarseIn) / st.tilesIn : 0;
  // Conservative: every pixel of a coarse tile (2 px grid inside the viewport) is at >= 10 degrees.
  double g[3];
  PixelDir(r, r.gpx, r.gpy, g);
  const double cosIn = std::cos(10 * d2r);
  int violations = 0;
  double minFineDeg = 1e9;
  for (int ty = 0; ty < 59; ++ty)
    for (int tx = 0; tx < 59; ++tx) {
      double maxCos = -1;
      for (int y = ty * 16; y < ty * 16 + 16 && y < 931; y += 2)
        for (int x = tx * 16; x < tx * 16 + 16 && x < 944; x += 2) {
          double d[3];
          PixelDir(r, x + 0.5, y + 0.5, d);
          maxCos = (std::max)(maxCos, CosBetween(d, g));
        }
      if (m[ty * 59 + tx] && maxCos > cosIn + 1e-9) ++violations;
      if (!m[ty * 59 + tx]) minFineDeg = (std::min)(minFineDeg, 0.0);
    }
  (void)minFineDeg;
  const int ct = 29 * 59 + 29;  // the centre tile
  const bool centreFine = m[ct] == 0 && m[0] == 1 && m[58] == 1 && m[58 * 59] == 1;
  // Whole view (inner 0): every tile coarse; 59 x 58 tile centres inside 944 x 931.
  RingIn w = r;
  w.innerDeg = 0;
  std::vector<uint8_t> mw(59 * 59);
  const RingStats sw = BuildTiles(w, mw.data());
  bool allOne = true;
  for (uint8_t x : mw) allOne = allOne && x == 1;
  // Off-centre gaze (0.25, -0.15), 16 degrees off the axis: the fine disc follows it.
  double px = 0, py = 0;
  GazePixel(r.sx, r.sy, r.cx, r.cy, 0, 0, r.vw, r.vh, 0.25, -0.15, &px, &py);
  double back[3];
  PixelDir(r, px, py, back);
  RingIn o = r;
  o.gpx = px, o.gpy = py;
  std::vector<uint8_t> mo(59 * 59);
  BuildTiles(o, mo.data());
  const int gt = static_cast<int>(py / 16) * 59 + static_cast<int>(px / 16);
  const bool off = std::fabs(back[0] - 0.25) < 1e-12 && std::fabs(back[1] + 0.15) < 1e-12 && mo[gt] == 0 && mo[ct] == 1 &&
                   px > 472 && py > 466;
  printf("     ring checks: centre fine %d, whole %d/%d all one %d, off-centre %d\n", centreFine, sw.coarseIn,
         sw.tilesIn, allOne, off);
  printf("     ring 10 deg of a 944x931 focus view (+-17.7 x 17.5 deg): %d of %d tiles coarse (%.1f %%, R138: 76 %%), "
         "conservative violations %d\n",
         st.coarseIn, st.tilesIn, 100 * share, violations);
  Check(st.tilesIn == 59 * 58 && share > 0.70 && share < 0.78 && violations == 0 && centreFine && sw.coarseIn == 59 * 58 &&
            allOne && off,
        "R139 VRS probe ring map: 10 degrees in a 0.25-share focus view leaves ~76 % coarse (R138 table), no coarse "
        "tile reaches inside the radius, whole view all coarse, the disc follows an off-centre gaze (GazePixel / "
        "PixelDir round trip)");
}

// R140: the pure parts of the fixes (default frame buffer vote, content key, per-view upload budget).
void CacheParts() {
  using namespace vrsp;
  int a = 0, b = 0, c = 0;  // identities
  FbTrack fb;
  for (int i = 0; i < 7; ++i) fb.Note(&a, &b);
  const bool notYet = !fb.Known() && !fb.Is(&a, &b);
  fb.Note(&a, &b);
  // Known after 8 starts; a few starts under another target (a pass that left its own bound) do not overturn it.
  fb.Note(&c, nullptr), fb.Note(&c, nullptr), fb.Note(&c, nullptr);
  const bool fbOk = notYet && fb.Known() && fb.Is(&a, &b) && fb.Is(&a, nullptr) && fb.Is(nullptr, &b) &&
                    !fb.Is(&c, &b) && !fb.Is(nullptr, &c) && !fb.Is(nullptr, nullptr);
  // The key: a whole-view surface ignores the gaze; a ring moves only with the gaze-centre tile (16 px), the size, the
  // projection (coarsely) and the radius.
  const uint64_t w1 = ContentKey(false, 71, 70, 1132, 1120, 566, 560, 3, 3, 0, 0, 0),
                 w2 = ContentKey(false, 71, 70, 1132, 1120, 100, 90, 2, 2, 0.3, 0.1, 0);
  const uint64_t r1 = ContentKey(true, 71, 70, 1132, 1120, 566, 560, 3.1, 3.1, 0.01, 0, 10),
                 r2 = ContentKey(true, 71, 70, 1132, 1120, 570.9, 575.5, 3.1004, 3.1, 0.012, 0.001, 10),  // same tiles
                 r3 = ContentKey(true, 71, 70, 1132, 1120, 582, 560, 3.1, 3.1, 0.01, 0, 10),  // next tile
                 r4 = ContentKey(true, 71, 70, 1130, 1120, 566, 560, 3.1, 3.1, 0.01, 0, 10),  // size
                 r5 = ContentKey(true, 71, 70, 1132, 1120, 566, 560, 3.1, 3.1, 0.01, 0, 12),  // radius
                 r6 = ContentKey(true, 71, 70, 1132, 1120, 566, 560, 3.1, 3.1, 0.2, 0, 10);   // projection
  const bool keyOk = w1 == w2 && r1 == r2 && r1 != r3 && r1 != r4 && r1 != r5 && r1 != r6 && r1 != w1;
  // Budget: one upload per view per frame; the stale surfaces take turns.
  UploadBudget u;
  int up[3] = {};
  std::string order;
  for (uint64_t f = 0; f < 6; ++f)
    for (int i = 0; i < 3; ++i)  // every surface changed every frame (a moving gaze), applied in the order 0, 1, 2
      if (TakeBudget(u, i, f)) ++up[i], order += static_cast<char>('0' + i);
  UploadBudget v;
  const bool once = TakeBudget(v, 4, 7) && !TakeBudget(v, 4, 7) && !TakeBudget(v, 5, 7);
  BudgetUpToDate(v, 5);  // surface 5 found its content current after all: not waiting any more
  const bool next = v.lastWant[5] <= v.lastUp[5] && TakeBudget(v, 4, 8);  // nobody waits: no yield
  // A waiting surface that is never applied again stops counting after a frame (the others are not blocked).
  UploadBudget x;
  const bool stale = TakeBudget(x, 1, 10) && !TakeBudget(x, 2, 10) && TakeBudget(x, 1, 13);
  printf("     cache parts: fb %d key %d budget order \"%s\" (%d/%d/%d) once %d next %d\n", fbOk, keyOk, order.c_str(),
         up[0], up[1], up[2], once, next);
  Check(fbOk && keyOk && order == "012012" && up[0] == 2 && up[1] == 2 && up[2] == 2 && once && next && stale,
        "R140 VRS probe caching parts: DCS's default frame buffer learnt by vote at pass starts; the surface content key "
        "moves only with the gaze-centre tile, size, projection and radius (not the viewport, not the gaze for a whole "
        "view); one upload per view per frame, stale surfaces of a view taking turns");
}

// ---------------------------------------------------------------------------
// Fake NvAPI
// ---------------------------------------------------------------------------
struct Fake {
  int initRet = 0, capsV2Ret = 0, capsV1Ret = 0;
  uint32_t capsBits = nvvrs::kCapVrsBit;
  bool noCreate = false;
  uint32_t lastCapsVer = 0;
  int setView = 0, setRates = 0, creates = 0;
  bool viewBound = false, ratesOn = false;
  int lastRate = 0;
  uint32_t lastN = 0, lastVer = 0;
  bool badVersion = false;
};
Fake g_fk;
int __cdecl FInit() { return g_fk.initRet; }
int __cdecl FCaps(IUnknown*, uint32_t ver, void* caps) {
  g_fk.lastCapsVer = ver;
  if (ver == nvvrs::kCapsVer2) {
    if (g_fk.capsV2Ret) return g_fk.capsV2Ret;
    static_cast<nvvrs::GraphicsCapsV2*>(caps)->bits = g_fk.capsBits;
    return 0;
  }
  if (ver == nvvrs::kCapsVer1) {
    if (g_fk.capsV1Ret) return g_fk.capsV1Ret;
    static_cast<nvvrs::GraphicsCapsV1*>(caps)->bits = g_fk.capsBits;
    return 0;
  }
  return nvvrs::kIncompatibleStructVersion;
}
int __cdecl FSetRates(IUnknown*, nvvrs::ViewportsRateDesc* d) {
  ++g_fk.setRates;
  if (!d || d->version != nvvrs::kViewportsRateVer) {
    g_fk.badVersion = true;
    return nvvrs::kIncompatibleStructVersion;
  }
  g_fk.lastN = d->numViewports;
  g_fk.ratesOn = d->numViewports > 0 && d->pViewports && d->pViewports[0].enableVariablePixelShadingRate;
  if (g_fk.ratesOn) g_fk.lastRate = d->pViewports[0].shadingRateTable[1];
  return 0;
}
int __cdecl FCreate(ID3D11Device* dev, ID3D11Resource* res, nvvrs::ShadingRateViewDesc* d, nvvrs::ShadingRateView** out) {
  if (!d || d->version != nvvrs::kShadingRateViewVer || d->Format != DXGI_FORMAT_R8_UINT ||
      d->ViewDimension != nvvrs::kSrrvTexture2D) {
    g_fk.badVersion = true;
    return nvvrs::kIncompatibleStructVersion;
  }
  D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
  sd.Format = DXGI_FORMAT_R8_UINT, sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, sd.Texture2D.MipLevels = 1;
  ID3D11ShaderResourceView* v = nullptr;
  if (FAILED(dev->CreateShaderResourceView(res, &sd, &v))) return nvvrs::kError;
  *out = v;
  ++g_fk.creates;
  return 0;
}
int __cdecl FSetView(IUnknown*, nvvrs::ShadingRateView* v) {
  ++g_fk.setView;
  g_fk.viewBound = v != nullptr;
  return 0;
}
void* __cdecl FQuery(uint32_t id) {
  switch (id) {
    case nvvrs::kId_Initialize: return reinterpret_cast<void*>(&FInit);
    case nvvrs::kId_D3D1x_GetGraphicsCapabilities: return reinterpret_cast<void*>(&FCaps);
    case nvvrs::kId_D3D11_RSSetViewportsPixelShadingRates: return reinterpret_cast<void*>(&FSetRates);
    case nvvrs::kId_D3D11_CreateShadingRateResourceView: return g_fk.noCreate ? nullptr : reinterpret_cast<void*>(&FCreate);
    case nvvrs::kId_D3D11_RSSetShadingRateResourceView: return reinterpret_cast<void*>(&FSetView);
  }
  return nullptr;
}
bool Armed() { return g_fk.viewBound && g_fk.ratesOn; }

void BindPaths() {
  using namespace vrsp;
  char why[256];
  Api a;
  uint8_t dummy = 0;
  ID3D11Device* dev = reinterpret_cast<ID3D11Device*>(&dummy);  // never dereferenced by the fakes
  g_fk = Fake{};
  const int r0 = Bind(nullptr, dev, &a, why, sizeof(why));
  const int r1 = Bind(&FQuery, nullptr, &a, why, sizeof(why));
  g_fk = Fake{};
  g_fk.initRet = -1;
  const int r2 = Bind(&FQuery, dev, &a, why, sizeof(why));
  g_fk = Fake{};
  g_fk.noCreate = true;
  const int r3 = Bind(&FQuery, dev, &a, why, sizeof(why));
  const bool r3txt = strstr(why, "create 0") != nullptr;
  g_fk = Fake{};
  g_fk.capsV2Ret = nvvrs::kIncompatibleStructVersion;  // an older driver: v1 only
  const int r4 = Bind(&FQuery, dev, &a, why, sizeof(why));
  const bool v1 = r4 == kBfOk && a.ok && a.capsVer == 1 && g_fk.lastCapsVer == nvvrs::kCapsVer1;
  g_fk = Fake{};
  g_fk.capsV2Ret = g_fk.capsV1Ret = nvvrs::kIncompatibleStructVersion;
  const int r5 = Bind(&FQuery, dev, &a, why, sizeof(why));
  g_fk = Fake{};
  g_fk.capsBits = 1;  // exclusive scissor only
  const int r6 = Bind(&FQuery, dev, &a, why, sizeof(why));
  const bool r6ok = r6 == kBfNoVrs && !a.ok;
  g_fk = Fake{};
  const int r7 = Bind(&FQuery, dev, &a, why, sizeof(why));
  const bool v2 = r7 == kBfOk && a.ok && a.capsVer == 2 && a.setRates && a.createView && a.setView;
  Check(r0 == kBfNoQuery && r1 == kBfNoDevice && r2 == kBfInit && r3 == kBfEntry && r3txt && v1 && r5 == kBfCaps && r6ok &&
            v2,
        "R139 VRS probe NvAPI binding fails safe: no nvapi64 / no device / Initialize failed / an entry point missing / "
        "caps refused for both struct versions / no VRS bit each stop it with its reason; caps v1 accepted when v2 is not");
}

// ---------------------------------------------------------------------------
// The render-thread state machine on WARP
// ---------------------------------------------------------------------------
struct FakePass {
  void* vt = nullptr;
  uint64_t n = 0;
  char name[40] = {};
};
FakePass MakePass(const char* nm) {
  FakePass p;
  p.n = strlen(nm);
  memcpy(p.name, nm, p.n);
  return p;
}
// A TerrainContext of the R53 shape: P rows 16 / 17 / 19, the render size in row 44, the eye in row 38.
void MakeTc(uint8_t* tc, double sx, double sy, double cx, double cy, float w, float h, int eye) {
  memset(tc, 0, ftwin::kTcBytes);
  auto row = [&](int r, float a, float b, float c, float d) {
    float* f = reinterpret_cast<float*>(tc + 16 * r);
    f[0] = a, f[1] = b, f[2] = c, f[3] = d;
  };
  row(16, static_cast<float>(sx), 0, static_cast<float>(cx), 0);
  row(17, 0, static_cast<float>(sy), static_cast<float>(cy), 0);
  row(18, 0, 0, 0.0001f, 1);
  row(19, 0, 0, 1, 0);
  row(38, eye ? -0.032f : 0.032f, 1.2f, 3.4f, 1);
  row(44, 0, 0, w, h);
  row(60, 7.0f + eye, 8, 9, 10);  // a shared terrain row that differs between the eyes
}
struct Rig {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11Texture2D* rt[2] = {};    // periphery size, focus size
  ID3D11RenderTargetView* rtv[2] = {};
  ID3D11Texture2D* depth[4] = {};  // one per HMD view (periphery 624 x 616, focus 944 x 931)
  ID3D11DepthStencilView* dsv[4] = {};
  ID3D11Texture2D* shadow = nullptr;
  ID3D11DepthStencilView* shadowDsv = nullptr;
  ID3D11Texture2D* dummyDepth = nullptr;  // DCS's default frame buffer between passes
  ID3D11DepthStencilView* dummyDsv = nullptr;
  ID3D11Texture2D* dummyRt = nullptr;  // R140: its colour target (live: 1076x1060, the periphery's DLSS output size)
  ID3D11RenderTargetView* dummyRtv = nullptr;
  ID3D11Texture2D* half[2] = {};  // R140: half-size targets (a half-resolution SSAO pass)
  ID3D11RenderTargetView* halfRtv[2] = {};
  ID3D11Buffer* tcBuf = nullptr;
  uint8_t tc[4][ftwin::kTcBytes] = {};
  FakePass casc[4], height, lights, gb, ssao, light, tdecal, transp, dlss, tone, hmd;
  // R140 options (all off: the R139 frame).
  bool live = false;        // the default frame buffer with its colour target, bound again at each pass's end inside it
  bool foreignGb = false;   // a hook of ours (fixation_depth.h's in-pass arm) holds OM / viewport in G-buffer passes
  bool halfSsao = false;    // SSAO into a half-size target, after a viewport of the full size (R139's thrash shape)
};
// R140: fixation_depth.h's in-pass OM / viewport hooks as the probe meets them: not d3d11's entries (refused), they
// forward to d3d11 and report through ptiming's observers.
void* g_fhOrig[3] = {};
int g_fhCalls = 0;
void STDMETHODCALLTYPE FhOm(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d) {
  ++g_fhCalls;
  reinterpret_cast<vrsp::OmFn>(g_fhOrig[0])(c, n, r, d);
  if (auto o = ptiming::g_omObserver.load()) o(c, n, r, d, false, nullptr);
}
void STDMETHODCALLTYPE FhOmUav(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* r,
                               ID3D11DepthStencilView* d, UINT us, UINT un, ID3D11UnorderedAccessView* const* u,
                               const UINT* ic) {
  ++g_fhCalls;
  reinterpret_cast<vrsp::OmUavFn>(g_fhOrig[1])(c, n, r, d, us, un, u, ic);
  if (auto o = ptiming::g_omObserver.load()) o(c, n, r, d, true, nullptr);
}
void STDMETHODCALLTYPE FhVp(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v) {
  ++g_fhCalls;
  reinterpret_cast<vrsp::VpFn>(g_fhOrig[2])(c, n, v);
  if (auto o = ptiming::g_vpObserver.load()) o(c, n, v);
}
void ForeignArm(ID3D11DeviceContext* ctx, bool on) {
  void** vt = *reinterpret_cast<void***>(ctx);
  const int sl[3] = {DcsQv_ContextSlot("OMSetRenderTargets"), DcsQv_ContextSlot("OMSetRenderTargetsAndUnorderedAccessViews"),
                     DcsQv_ContextSlot("RSSetViewports")};
  void* const fn[3] = {reinterpret_cast<void*>(&FhOm), reinterpret_cast<void*>(&FhOmUav), reinterpret_cast<void*>(&FhVp)};
  for (int k = 0; k < 3; ++k) {
    if (on && vt[sl[k]] != fn[k]) {
      g_fhOrig[k] = vt[sl[k]];  // as fixation_depth.h: whatever is there (d3d11's entry here)
      d3ds::WriteTablePointer(&vt[sl[k]], fn[k]);
    } else if (!on && vt[sl[k]] == fn[k]) {
      d3ds::WriteTablePointer(&vt[sl[k]], g_fhOrig[k]);
    }
  }
}
bool MakeRig(Rig& g) {
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &g.dev, nullptr,
                               &g.ctx)))
    return false;
  auto tex = [&](UINT w, UINT h, DXGI_FORMAT f, UINT bind, ID3D11Texture2D** t) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w, d.Height = h, d.MipLevels = 1, d.ArraySize = 1, d.Format = f, d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT, d.BindFlags = bind;
    return SUCCEEDED(g.dev->CreateTexture2D(&d, nullptr, t));
  };
  const UINT W[2] = {624, 944}, H[2] = {616, 931};
  for (int i = 0; i < 2; ++i) {
    if (!tex(W[i], H[i], DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, &g.rt[i])) return false;
    g.dev->CreateRenderTargetView(g.rt[i], nullptr, &g.rtv[i]);
  }
  for (int v = 0; v < 4; ++v) {
    if (!tex(W[v / 2], H[v / 2], DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_BIND_DEPTH_STENCIL, &g.depth[v])) return false;
    g.dev->CreateDepthStencilView(g.depth[v], nullptr, &g.dsv[v]);
  }
  if (!tex(2048, 2048, DXGI_FORMAT_D32_FLOAT, D3D11_BIND_DEPTH_STENCIL, &g.shadow)) return false;
  g.dev->CreateDepthStencilView(g.shadow, nullptr, &g.shadowDsv);
  if (!tex(816, 804, DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_BIND_DEPTH_STENCIL, &g.dummyDepth)) return false;
  g.dev->CreateDepthStencilView(g.dummyDepth, nullptr, &g.dummyDsv);
  if (!tex(816, 804, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, &g.dummyRt)) return false;
  g.dev->CreateRenderTargetView(g.dummyRt, nullptr, &g.dummyRtv);
  for (int i = 0; i < 2; ++i) {
    if (!tex(W[i] / 2, H[i] / 2, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, &g.half[i])) return false;
    g.dev->CreateRenderTargetView(g.half[i], nullptr, &g.halfRtv[i]);
  }
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = ftwin::kTcBytes, bd.Usage = D3D11_USAGE_DEFAULT, bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &g.tcBuf))) return false;
  MakeTc(g.tc[0], 0.781, 0.79, 0.19, 0.0, 624, 616, 0);
  MakeTc(g.tc[1], 0.781, 0.79, -0.19, 0.0, 624, 616, 1);
  MakeTc(g.tc[2], 3.13, 3.17, 0.01, 0.0, 944, 931, 0);
  MakeTc(g.tc[3], 3.13, 3.17, -0.02, 0.01, 944, 931, 1);
  for (int c = 0; c < 4; ++c) {
    char n[32];
    snprintf(n, sizeof(n), "CascadeShadowPass%d", c);
    g.casc[c] = MakePass(n);
  }
  g.height = MakePass("HeightMapPass"), g.lights = MakePass("LightsBuffersPass"), g.gb = MakePass("GBufferPass");
  g.ssao = MakePass("SSAO"), g.light = MakePass("GBufferLightingPass"), g.tdecal = MakePass("TransparentDecalPass");
  g.transp = MakePass("TransparentPass"), g.dlss = MakePass("DLSSPass"), g.tone = MakePass("ToneMappingPass");
  g.hmd = MakePass("CopyToHMDPass");
  return true;
}
void FreeRig(Rig& g) {
  auto rel = [](IUnknown* p) {
    if (p) p->Release();
  };
  for (int i = 0; i < 2; ++i) rel(g.rtv[i]), rel(g.rt[i]);
  for (int v = 0; v < 4; ++v) rel(g.dsv[v]), rel(g.depth[v]);
  rel(g.shadowDsv), rel(g.shadow), rel(g.dummyDsv), rel(g.dummyDepth), rel(g.tcBuf);
  rel(g.dummyRtv), rel(g.dummyRt);
  for (int i = 0; i < 2; ++i) rel(g.halfRtv[i]), rel(g.half[i]);
  if (g.ctx) g.ctx->ClearState(), g.ctx->Flush();
  rel(g.ctx), rel(g.dev);
}
// What the frame saw: per pass occurrence, whether the state was bound inside it and whether it was clear after it.
struct Seen {
  std::string armedIn;  // "name@view " for every pass with the state bound inside
  int leaks = 0;        // pass ends after which the state was still bound
  int reOm = 0, reVp = 0, reEcl = 0;
  std::vector<int> gbViews;  // R140: the probe's view after each G-buffer pass body (render order)
};
void BindFb(Rig& g) {
  if (g.live)
    g.ctx->OMSetRenderTargets(1, &g.dummyRtv, g.dummyDsv);
  else
    g.ctx->OMSetRenderTargets(0, nullptr, g.dummyDsv);
}
void Pass(Rig& g, FakePass& p, int view, Seen& s, const std::function<void()>& body, bool ecl = false) {
  const bool foreign = g.foreignGb && !strcmp(p.name, "GBufferPass");
  if (foreign) ForeignArm(g.ctx, true);  // fixation_depth.h's boundary callback runs before the probe's at a start
  vrsp::Pre(&p, true, 0);
  vrsp::Boundary(&p, true, 0);
  bool armed = Armed();
  if (body) body();
  armed = armed || Armed();
  if (ecl && Armed()) {
    const int before = g_fk.setRates;
    sfilt::NotifyEcl(g.ctx);
    if (g_fk.setRates > before) ++s.reEcl;
  }
  if (g.live) BindFb(g);  // DCS's popFrameBuffer inside the pass
  vrsp::Boundary(&p, false, 0);
  if (foreign) ForeignArm(g.ctx, false);  // ... and after it at an end
  // DCS's default frame buffer between passes.
  BindFb(g);
  if (Armed()) ++s.leaks;
  if (armed) s.armedIn += std::string(p.name) + "@" + std::to_string(view) + " ";
}
// One frame: cascades, height map, then periphery L / R, focus L / R (or the views in order[]). noTc: the view whose
// G-buffer pass uploads no TerrainContext (-1 none).
Seen Frame(Rig& g, int noTc = -1, const int* order = nullptr) {
  Seen s;
  BindFb(g);
  for (int c = 0; c < 4; ++c)
    Pass(g, g.casc[c], -1, s, [&] {
      g.ctx->OMSetRenderTargets(0, nullptr, g.shadowDsv);
      D3D11_VIEWPORT vp{0, 0, 2048, 2048, 0, 1};
      g.ctx->RSSetViewports(1, &vp);
    });
  Pass(g, g.height, -1, s, nullptr);
  for (int vi = 0; vi < 4; ++vi) {
    const int v = order ? order[vi] : vi;
    const int f = v / 2;
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(f ? 944 : 624), static_cast<float>(f ? 931 : 616), 0, 1};
    Pass(g, g.lights, v, s, nullptr);
    Pass(g, g.gb, v, s, [&] {
      const int r0 = g_fk.setRates;
      g.ctx->OMSetRenderTargets(1, &g.rtv[f], g.dsv[v]);
      g.ctx->RSSetViewports(1, &vp);
      if (v != noTc) g.ctx->UpdateSubresource(g.tcBuf, 0, nullptr, g.tc[v], 0, 0);
      const int r1 = g_fk.setRates;
      if (Armed()) {
        g.ctx->OMSetRenderTargets(1, &g.rtv[f], g.dsv[v]);  // DCS binds again: the state follows
        if (g_fk.setRates > r1) ++s.reOm;
        const int r2 = g_fk.setRates;
        g.ctx->RSSetViewports(1, &vp);
        if (g_fk.setRates > r2) ++s.reVp;
      }
      (void)r0;
      s.gbViews.push_back(vrsp::g_s.view);
    }, true);
    Pass(g, g.ssao, v, s, [&] {
      if (g.halfSsao) {  // R139's shape: the full viewport still set, then the half target and its own viewport
        g.ctx->RSSetViewports(1, &vp);
        g.ctx->OMSetRenderTargets(1, &g.halfRtv[f], nullptr);
        const D3D11_VIEWPORT hv{0, 0, vp.Width / 2, vp.Height / 2, 0, 1};
        g.ctx->RSSetViewports(1, &hv);
        g.ctx->RSSetViewports(1, &vp);  // and back and forth
        g.ctx->RSSetViewports(1, &hv);
        return;
      }
      g.ctx->OMSetRenderTargets(1, &g.rtv[f], nullptr);
      g.ctx->RSSetViewports(1, &vp);
    });
    Pass(g, g.light, v, s, [&] { g.ctx->OMSetRenderTargets(1, &g.rtv[f], nullptr); });
    Pass(g, g.tdecal, v, s, [&] { g.ctx->OMSetRenderTargets(1, &g.rtv[f], g.dsv[v]); });
    Pass(g, g.transp, v, s, [&] { g.ctx->OMSetRenderTargets(1, &g.rtv[f], g.dsv[v]); });
    Pass(g, g.dlss, v, s, nullptr);
    Pass(g, g.tone, v, s, [&] { g.ctx->OMSetRenderTargets(1, &g.rtv[f], nullptr); });
    Pass(g, g.hmd, v, s, nullptr);
  }
  return s;
}

int SurfIdx(int slot, UINT tw) {
  for (int i = 0; i < vrsp::g_nSurf.load(); ++i)
    if (vrsp::g_surf[i].slot == slot && vrsp::g_surf[i].tw == tw) return i;
  return -1;
}

void RenderThread() {
  using namespace vrsp;
  Rig g;
  if (!MakeRig(g)) {
    FreeRig(g);
    QvSkip("R139 VRS probe render thread: no WARP device\n");
    return;
  }
  // The probe's globals, as DCS's session would set them (restored at the end).
  ID3D11Device* saveDev = shadowinst::g_device;
  const bool saveSt = shadowinst::g_deviceSingleThreaded;
  const DWORD saveTid = ptiming::g_topTid.load();
  const ptiming::ExecFn saveOrig = ptiming::g_orig;
  shadowinst::g_device = g.dev;
  shadowinst::g_deviceSingleThreaded = false;
  ptiming::g_topTid = GetCurrentThreadId();
  if (!ptiming::g_orig) ptiming::g_orig = reinterpret_cast<ptiming::ExecFn>(&FInit);  // "installed" (never called here)
  g_fk = Fake{};
  g_queryOverride = &FQuery;
  g_bindState = 0;
  void** vt = *reinterpret_cast<void***>(g.ctx);
  void* stock[kHCount];
  for (int k = 0; k < kHCount; ++k) stock[k] = vt[DcsQv_ContextSlot(kHookSlotNames[k])];
  StartReportCapture();
  const bool started = Start();
  // An earlier test's GazeOut block may still be mapped in this process: the ring checks want the viewport centre.
  const void* saveGaze = g_gazeView;
  g_gazeView = nullptr;
  Keys k;
  k.views = 0xC;
  // A2: every pre-DLSS raster pass of the focus views, whole view.
  SetArm(2, k);
  const Seen f1 = Frame(g);
  ServiceCreates();  // the suite thread creates the surfaces the render thread asked for
  const int created = g_fk.creates;
  const Seen f2 = Frame(g);
  const std::string want2 =
      "GBufferPass@2 SSAO@2 GBufferLightingPass@2 TransparentDecalPass@2 TransparentPass@2 GBufferPass@3 SSAO@3 "
      "GBufferLightingPass@3 TransparentDecalPass@3 TransparentPass@3 ";
  const bool a2 = started && f1.armedIn.empty() && created == 2 && f2.armedIn == want2 && f2.leaks == 0 &&
                  f2.reOm == 2 && f2.reVp == 2 && f2.reEcl == 2 && g_fk.lastRate == nvvrs::k2x2 && !g_fk.badVersion &&
                  g_c.tcSeen.load() >= 8 && g_s.slotW[2] == 944 && g_s.slotH[3] == 931 && g_s.slotW[0] == 624;
  // The enlight gap: open after TransparentDecalPass in a focus view, closed by the next pass start's g_vrsPre.
  bool gap = false;
  {
    Seen s;
    Pass(g, g.lights, 2, s, nullptr);
    Pass(g, g.gb, 2, s, [&] {
      g.ctx->OMSetRenderTargets(1, &g.rtv[1], g.dsv[2]);
      g.ctx->UpdateSubresource(g.tcBuf, 0, nullptr, g.tc[2], 0, 0);
    });
    vrsp::Pre(&g.tdecal, true, 0);
    vrsp::Boundary(&g.tdecal, true, 0);
    vrsp::Boundary(&g.tdecal, false, 0);
    const bool open = g_s.gap && g_s.gapSlot == 2 && !Armed();
    vrsp::Pre(&g.transp, true, 0);
    const bool closed = !g_s.gap && !Armed();
    vrsp::Boundary(&g.transp, true, 0);
    const bool armedT = Armed();
    vrsp::Boundary(&g.transp, false, 0);
    gap = open && closed && armedT && !Armed();
    Pass(g, g.dlss, 2, s, nullptr);
    Pass(g, g.hmd, 2, s, nullptr);
  }
  // The learnt depth texture names the view when its upload is missing (focus L here).
  SetArm(1, k);  // A1: main G-buffer only
  const uint64_t hits0 = g_c.dsvHits.load();
  const Seen f3 = Frame(g, 2);
  const bool a1 = f3.armedIn == "GBufferPass@2 GBufferPass@3 " && f3.leaks == 0 && g_c.dsvHits.load() > hits0;
  // A4: cascades c0 / c1 only (their own surfaces, sized at the first ON frame's binds).
  SetArm(4, k);
  Frame(g);
  ServiceCreates();
  const Seen f4 = Frame(g);
  const bool a4 = f4.armedIn == "CascadeShadowPass0@-1 CascadeShadowPass1@-1 " && f4.leaks == 0 && g_s.slotW[4] == 2048;
  // A3: the ring (no gaze mapping in the test: the viewport centre); ~76 % of the focus surface coarse.
  SetArm(3, k);
  const Seen f5 = Frame(g);
  const int si2 = SurfIdx(2, 59);
  const int in = si2 >= 0 ? g_si[si2].tilesIn.load() : 0, co = si2 >= 0 ? g_si[si2].coarseIn.load() : 0;
  const double share = in ? static_cast<double>(co) / in : 0;
  const bool a3 = f5.armedIn == want2 && f5.leaks == 0 && si2 >= 0 && g_si[si2].src.load() == 1 && share > 0.68 &&
                  share < 0.80 && g_surf[si2].w == 944 && g_surf[si2].h == 931;
  // A0 (off): nothing anywhere.
  SetArm(0, k);
  const Seen f6 = Frame(g);
  const bool a0 = f6.armedIn.empty() && f6.leaks == 0;
  printf("     frames: A2 \"%s\" (re-applied OM %d, viewport %d, command list %d), A1 \"%s\", A4 \"%s\", A3 ring share "
         "%.1f %%\n",
         f2.armedIn.c_str(), f2.reOm, f2.reVp, f2.reEcl, f3.armedIn.c_str(), f4.armedIn.c_str(), 100 * share);
  LogCounters("(test)");
  // Stop: the pre-callback clears at the next pass start, then everything goes; d3d11's entries are back.
  SetArm(2, k);
  {
    Seen s;
    Pass(g, g.lights, 2, s, nullptr);
    vrsp::Pre(&g.gb, true, 0);
    vrsp::Boundary(&g.gb, true, 0);
    g.ctx->OMSetRenderTargets(1, &g.rtv[1], g.dsv[2]);
    g.ctx->UpdateSubresource(g.tcBuf, 0, nullptr, g.tc[2], 0, 0);
  }
  const bool armedAtStop = Armed();
  // The stopping thread waits for the render thread's next pass start: run one on another thread's behalf here.
  std::thread stopper([] { StopProbe(); });
  for (int i = 0; i < 20 && armedAtStop && Armed(); ++i) {
    Sleep(20);
    vrsp::Pre(&g.ssao, true, 0);
  }
  stopper.join();
  // None of our hooks is left in the table (d3d11 may have switched an entry to another variant of its own meanwhile).
  bool entriesBack = true;
  for (int kk = 0; kk < kHCount; ++kk) {
    const void* cur = vt[DcsQv_ContextSlot(kHookSlotNames[kk])];
    entriesBack = entriesBack && cur != HookFn(kk) && fixdepth::InD3d11(cur);
  }
  (void)stock;
  const bool stopped = armedAtStop && !Armed() && entriesBack && !ptiming::g_vrsBoundary.load() &&
                       !ptiming::g_vrsPre.load() && !ptiming::g_uploadObserver.load() &&
                       !ptiming::g_omObserver.load() && !ptiming::g_vpObserver.load() &&
                       !sfilt::g_afterEclProbe.load() && g_nSurf.load() == 0 && !g_ctx;
  const std::string log = StopReportCapture();
  const bool logged = log.find("vrs probe: NvAPI VRS available (caps v2") != std::string::npos &&
                      log.find("vrs probe: HMD view 2 (focus L) depth target 944x931") != std::string::npos &&
                      log.find("vrs probe: cascade c0 depth target 2048x2048") != std::string::npos &&
                      log.find("vrs probe: cascade c0 depth target 944x931") == std::string::npos;
  // A second phase in the session: the binding is not retried after a failure (one line only).
  g_bindState = 0;
  g_fk = Fake{};
  g_fk.capsBits = 0;
  StartReportCapture();
  const bool s1 = Start(), s2 = Start();
  const std::string log2 = StopReportCapture();
  size_t lines = 0;
  for (size_t at = log2.find("probe disabled for this session"); at != std::string::npos;
       at = log2.find("probe disabled for this session", at + 1))
    ++lines;
  const bool noVrs = !s1 && !s2 && lines == 1 && !ptiming::g_vrsBoundary.load();
  g_queryOverride = nullptr;
  g_bindState = 0;
  g_gazeView = saveGaze;
  shadowinst::g_device = saveDev;
  shadowinst::g_deviceSingleThreaded = saveSt;
  ptiming::g_topTid = saveTid;
  ptiming::g_orig = saveOrig;
  FreeRig(g);
  Check(a2, "R139 VRS probe on WARP (fake NvAPI), A2: views identified by their TerrainContext upload; the state only "
            "in the focus views' G-buffer .. transparent passes (not DLSS / tone map / HMD copy, not the periphery, "
            "not the cascades), cleared at every pass end, re-applied after OM / viewport / ExecuteCommandList; "
            "surfaces created by the suite thread on request, sized from the depth bound in the G-buffer pass");
  Check(gap, "R139 VRS probe: the enlight gap opens after TransparentDecalPass (nothing bound yet) and g_vrsPre "
             "closes it before the next pass's boundary work");
  Check(a1 && a4 && a3 && a0,
        "R139 VRS probe arms: A1 main G-buffer only, also when its upload is missing (the learnt depth texture); A4 "
        "cascades c0 / c1 only; A3 ring ~76 % coarse around the viewport centre without a gaze; A0 nothing");
  Check(stopped && logged && noVrs,
        "R139 VRS probe stop: the state cleared at the next pass start, callbacks and observer out, d3d11's context "
        "entries back, surfaces released; sizes logged; a GPU without VRS: one line, never started, not retried");
}

// R140: the live R139 shape. fixation_depth.h's in-pass hooks hold OM / viewport in every G-buffer pass (the probe's
// own refused), DCS's default frame buffer has a colour target and is bound again at each pass's end, SSAO draws into a
// half-size target with viewports going back and forth. Checks: A1 applies (the G-buffer seen through the observers,
// its size and depth learnt), the default frame buffer sizes nothing, the ring content is uploaded once and then
// cached, one upload per view per frame with every surface getting its turn, the views mapped by their
// TerrainContext in any render order, A6 in SSAO + lighting only.
void RenderThreadR140() {
  using namespace vrsp;
  Rig g;
  if (!MakeRig(g)) {
    FreeRig(g);
    QvSkip("R140 VRS probe render thread: no WARP device\n");
    return;
  }
  ID3D11Device* saveDev = shadowinst::g_device;
  const bool saveSt = shadowinst::g_deviceSingleThreaded;
  const DWORD saveTid = ptiming::g_topTid.load();
  const ptiming::ExecFn saveOrig = ptiming::g_orig;
  shadowinst::g_device = g.dev;
  shadowinst::g_deviceSingleThreaded = false;
  ptiming::g_topTid = GetCurrentThreadId();
  if (!ptiming::g_orig) ptiming::g_orig = reinterpret_cast<ptiming::ExecFn>(&FInit);
  g_fk = Fake{};
  g_queryOverride = &FQuery;
  g_bindState = 0;
  g_c.armForeign = 0, g_c.obsOm = 0, g_c.obsVp = 0, g_c.fbBinds = 0, g_c.dsvHits = 0, g_c.uploads = 0;
  g_fhCalls = 0;
  StartReportCapture();
  const bool started = Start();
  const void* saveGaze = g_gazeView;
  g_gazeView = nullptr;
  g.live = g.foreignGb = g.halfSsao = true;
  Keys k;
  k.views = 0xC;
  // 1. A1 with the G-buffer's OM / viewport held by another hook of ours.
  SetArm(1, k);
  Frame(g);
  ServiceCreates();
  const Seen s1 = Frame(g);
  const bool a1 = started && s1.armedIn == "GBufferPass@2 GBufferPass@3 " && s1.leaks == 0 && s1.reOm == 2 &&
                  s1.reVp == 2 && g_c.armForeign.load() > 0 && g_c.obsOm.load() > 0 && g_c.obsVp.load() > 0 &&
                  g_fhCalls > 0 && g_s.slotW[2] == 944 && g_s.slotH[2] == 931 && g_s.slotW[0] == 624 &&
                  g_s.slotH[1] == 616;
  //    ... and its learnt depth names the view when the upload is missing (R139: depth-map hits 0).
  const uint64_t hits0 = g_c.dsvHits.load();
  const Seen s1b = Frame(g, 3);
  const bool a1dsv = s1b.armedIn == "GBufferPass@2 GBufferPass@3 " && g_c.dsvHits.load() > hits0;
  // 2. Cascades: sized from their own depth (2048), never from the default frame buffer bound at the pass end.
  SetArm(4, k);
  Frame(g);
  ServiceCreates();
  const Seen s2 = Frame(g);
  bool noFbSurface = true;
  for (int i = 0; i < g_nSurf.load(); ++i) noFbSurface = noFbSurface && g_surf[i].tw != 51;  // 816 / 16
  const bool casc = s2.armedIn == "CascadeShadowPass0@-1 CascadeShadowPass1@-1 " && s2.leaks == 0 &&
                    g_s.slotW[4] == 2048 && g_s.slotH[5] == 2048 && g_c.fbBinds.load() > 0 && noFbSurface;
  // 3. A3 ring: the content settles (two surfaces per focus view: one per frame), then no upload at all.
  SetArm(3, k);
  Frame(g);
  ServiceCreates();
  for (int i = 0; i < 3; ++i) Frame(g);
  const uint64_t u0 = g_c.uploads.load();
  Seen s3;
  for (int i = 0; i < 3; ++i) s3 = Frame(g);
  const uint64_t settledUploads = g_c.uploads.load() - u0;
  const int full2 = SurfIdx(2, 59), half2 = SurfIdx(2, 30), full3 = SurfIdx(3, 59), half3 = SurfIdx(3, 30);
  auto shareOf = [](int i) {
    return i >= 0 && g_si[i].tilesIn.load() ? static_cast<double>(g_si[i].coarseIn.load()) / g_si[i].tilesIn.load() : 0.0;
  };
  const bool cached = settledUploads == 0 && full2 >= 0 && half2 >= 0 && full3 >= 0 && half3 >= 0 &&
                      g_surf[half2].w == 472 && g_surf[half2].h == 465 && shareOf(full2) > 0.68 && shareOf(full2) < 0.80 &&
                      shareOf(half2) > 0.55 && shareOf(half2) < 0.85 && g_si[half2].cxPx.load() == 236 &&
                      g_si[full2].cxPx.load() == 472 && s3.leaks == 0;
  // 4. The focus projection moves every frame (the focus view follows the gaze): every surface of a focus view
  //    changes every frame, at most one upload per view per frame, and both surfaces of each view take turns.
  uint64_t perSurf0[4] = {}, perSurf1[4] = {};
  const int idx[4] = {full2, half2, full3, half3};
  for (int j = 0; j < 4; ++j) perSurf0[j] = idx[j] >= 0 ? g_si[idx[j]].uploads.load() : 0;
  uint64_t maxPerFrame = 0;
  for (int fr = 0; fr < 4; ++fr) {
    MakeTc(g.tc[2], 3.13, 3.17, 0.01 + 0.05 * (fr + 1), 0.0, 944, 931, 0);
    MakeTc(g.tc[3], 3.13, 3.17, -0.02 - 0.05 * (fr + 1), 0.01, 944, 931, 1);
    const uint64_t a = g_c.uploads.load();
    Frame(g);
    maxPerFrame = (std::max)(maxPerFrame, g_c.uploads.load() - a);
    if (getenv("QV_VRS_DEBUG")) {
      const UploadBudget& bb = g_s.budget[2];
      printf("     dbg frame %llu: budget frame %lld full(%d) up %lld want %lld half(%d) up %lld want %lld\n",
             static_cast<unsigned long long>(g_c.frames.load()), static_cast<long long>(bb.frame), full2,
             static_cast<long long>(bb.lastUp[full2]), static_cast<long long>(bb.lastWant[full2]), half2,
             static_cast<long long>(bb.lastUp[half2]), static_cast<long long>(bb.lastWant[half2]));
    }
  }
  bool turns = true;
  for (int j = 0; j < 4; ++j) {
    perSurf1[j] = idx[j] >= 0 ? g_si[idx[j]].uploads.load() : 0;
    turns = turns && perSurf1[j] - perSurf0[j] == 2;
  }
  const bool budget = maxPerFrame == 2 && turns && g_c.upDeferred.load() > 0;
  printf("     R140 budget: uploads per surface over 4 moving frames (focus L full/half, focus R full/half) %llu %llu %llu "
         "%llu\n",
         static_cast<unsigned long long>(perSurf1[0] - perSurf0[0]), static_cast<unsigned long long>(perSurf1[1] - perSurf0[1]),
         static_cast<unsigned long long>(perSurf1[2] - perSurf0[2]), static_cast<unsigned long long>(perSurf1[3] - perSurf0[3]));
  MakeTc(g.tc[2], 3.13, 3.17, 0.01, 0.0, 944, 931, 0);
  MakeTc(g.tc[3], 3.13, 3.17, -0.02, 0.01, 944, 931, 1);
  // 5. View mapping: periphery R, periphery L, focus R, focus L. Each view is named by its TerrainContext (focus by
  //    its projection, the eye by the shared rows), never by the order; the state lands in the focus views only.
  SetArm(2, k);
  Frame(g);
  ServiceCreates();
  Frame(g);
  const int order[4] = {1, 0, 3, 2};
  const uint64_t fr0 = g_c.frames.load();
  const Seen s5 = Frame(g, -1, order);
  const uint64_t frameStarts = g_c.frames.load() - fr0;  // cascades then height map: one frame (R139 counted two)
  const std::vector<int> wantViews = {1, 0, 3, 2};
  const bool mapping = frameStarts == 1 && s5.gbViews == wantViews &&
                       s5.armedIn == "GBufferPass@3 SSAO@3 GBufferLightingPass@3 TransparentDecalPass@3 TransparentPass@3 "
                                     "GBufferPass@2 SSAO@2 GBufferLightingPass@2 TransparentDecalPass@2 TransparentPass@2 " &&
                       s5.leaks == 0;
  // 6. A6: SSAO + the lighting group in the ring, nothing else.
  SetArm(6, k);
  Frame(g);
  ServiceCreates();
  Frame(g);
  const Seen s6 = Frame(g);
  const bool a6 = s6.armedIn == "SSAO@2 GBufferLightingPass@2 SSAO@3 GBufferLightingPass@3 " && s6.leaks == 0 &&
                  g_si[full2].src.load() == 1 && shareOf(full2) > 0.68 && shareOf(full2) < 0.80;
  LogSurfaces();
  LogCounters("(R140 test)");
  SetArm(0, k);
  const Seen s0 = Frame(g);
  StopProbe();
  const std::string log = StopReportCapture();
  const bool logged = log.find("vrs probe: HMD view 2 (focus L) depth target 944x931") != std::string::npos &&
                      log.find("vrs probe: HMD view 0 (periphery L) depth target 624x616") != std::string::npos &&
                      log.find("vrs probe: cascade c0 depth target 2048x2048") != std::string::npos &&
                      log.find("depth target 816x804") == std::string::npos &&
                      log.find("surface: HMD view 2 (focus L) target 944x931, 59x59 tiles, ") != std::string::npos &&
                      log.find("surface: HMD view 3 (focus R) target 472x465, 30x30 tiles, ") != std::string::npos &&
                      log.find(" of 0 target tiles") == std::string::npos;
  printf("     R140 frames: A1 \"%s\" (observer OM %llu / viewport %llu, refused %llu), cascades \"%s\", settled uploads "
         "%llu, max uploads per frame %llu, view order 1 0 3 2 -> %d %d %d %d, A6 \"%s\"\n",
         s1.armedIn.c_str(), static_cast<unsigned long long>(g_c.obsOm.load()),
         static_cast<unsigned long long>(g_c.obsVp.load()), static_cast<unsigned long long>(g_c.armForeign.load()),
         s2.armedIn.c_str(), static_cast<unsigned long long>(settledUploads),
         static_cast<unsigned long long>(maxPerFrame), s5.gbViews.size() > 0 ? s5.gbViews[0] : -9,
         s5.gbViews.size() > 1 ? s5.gbViews[1] : -9, s5.gbViews.size() > 2 ? s5.gbViews[2] : -9,
         s5.gbViews.size() > 3 ? s5.gbViews[3] : -9, s6.armedIn.c_str());
  printf("     R140 checks: a1 %d dsv %d casc %d cached %d budget %d mapping %d a6 %d logged %d off %d\n", a1, a1dsv, casc,
         cached, budget, mapping, a6, logged, s0.armedIn.empty());
  g_queryOverride = nullptr;
  g_bindState = 0;
  g_gazeView = saveGaze;
  shadowinst::g_device = saveDev;
  shadowinst::g_deviceSingleThreaded = saveSt;
  ptiming::g_topTid = saveTid;
  ptiming::g_orig = saveOrig;
  FreeRig(g);
  Check(a1 && a1dsv,
        "R140 VRS probe G-buffer view resolution: with fixation_depth.h's in-pass hooks holding OM / viewport (the "
        "probe's refused, as live in R139), A1 applies in the focus views' main G-buffer passes, seen through the "
        "observers; the view's size and depth are learnt and the depth names the view when its upload is missing");
  Check(casc && logged,
        "R140 VRS probe sizes: DCS's default frame buffer (bound at each pass end) sizes no surface; cascades sized "
        "2048 from their own depth; the surface lines carry each surface's own target and real coverage");
  Check(cached && budget,
        "R140 VRS probe surface caching: the ring is uploaded once per surface and then cached (viewports going back "
        "and forth change nothing); a moving projection uploads at most one surface per view per frame, the view's "
        "surfaces taking turns");
  Check(mapping && a6,
        "R140 VRS probe view mapping: views named by their TerrainContext in any render order (periphery R first: 1 0 3 "
        "2), the state only in the focus views; one frame counted per frame; A6 arms SSAO + lighting only, ring ~76 % "
        "coarse");
}

void Run() {
  KeysParse();
  ScheduleAndGate();
  Classes();
  Ring();
  CacheParts();
  BindPaths();
  RenderThread();
  RenderThreadR140();
}

}  // namespace vrstest
