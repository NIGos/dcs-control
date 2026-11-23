// 0.7 early shaders (early_shaders.h): the SSAO source patch and its refusals, the identity decision, the synthetic
// cases, the idle-pass hints and tests, and on WARP: the identification by output (our build of each SSAO kernel
// standing in for DCS's object, a variant, an unrelated shader), the draw-time swap through the context table and its
// removal. With a DCS install (dcs_install.h): our builds of DCS's own deferred\SSAO.fx must reproduce the proven
// code hashes (the stock builds = DCS 2.9.31's archive blobs); QV_EARLY_OUT=<folder> also writes the four blobs there
// for the offline equivalence proof (tools\shader_exact, `pair`). Without an install those checks SKIP.
// QV_EARLY_ONLY=1 runs only these tests.
#pragma once

namespace estest {
using namespace eshd;

// A stand-in with the structure the patch needs (not DCS's text).
const char kFixture[] =
    "float2 Other() { return 0; }\r\n"
    "float2 SSAO_Value(uint2 pix, float3 vPos, uniform int KERNEL) {\r\n"
    "\tfloat acc = 0;\r\n"
    "\t\r\n"
    "\tfor (uint k = 0; k < KERNEL; ++k) {\r\n"
    "\t\tfloat depth = SampleMap(DepthMap, pix, 0).x;\r\n"
    "\t\tuint stv = SampleMap(StencilMap, pix, 0).g & STENCIL_COMPOSITION_MASK;\r\n"
    "\t\tif (stv != STENCIL_COMPOSITION_WATER) {\r\n"
    "\t\t\tacc += depth;\r\n"
    "\t\t}\r\n"
    "\t}\r\n"
    "\tacc = 1 - acc / KERNEL;\r\n"
    "\treturn float2(acc, vPos.z);\r\n"
    "}\r\n";

std::string Replace(std::string s, const std::string& a, const std::string& b) {
  const size_t p = s.find(a);
  if (p != std::string::npos) s.replace(p, a.size(), b);
  return s;
}

void PatchTests() {
  std::string out, why;
  Check(PatchSsaoSource(kFixture, &out, &why), "early: SSAO patch accepts the expected structure");
  const std::string want = std::string("\t") + kSsaoBranch + "\r\n\tfor (uint k = 0; k < KERNEL; ++k) {\r\n";
  Check(out.find(want) != std::string::npos, "early: the branch line goes right before the loop, same indent and CRLF");
  Check(out.size() == strlen(kFixture) + strlen(kSsaoBranch) + 3, "early: nothing else changes (one line added)");
  Check(Replace(out, std::string("\t") + kSsaoBranch + "\r\n", "") == kFixture, "early: removing the line gives the input back");
  // LF-only source keeps LF
  std::string lf = kFixture;
  for (size_t p; (p = lf.find("\r\n")) != std::string::npos;) lf.erase(p, 1);
  Check(PatchSsaoSource(lf, &out, &why) && out.find(std::string(kSsaoBranch) + "\n") != std::string::npos &&
            out.find('\r') == std::string::npos,
        "early: LF line endings are kept");
  // refusals
  Check(!PatchSsaoSource(Replace(kFixture, "float2 SSAO_Value(", "float2 SSAO_Other("), &out, &why),
        "early: refused without SSAO_Value");
  Check(!PatchSsaoSource(std::string(kFixture) + "\tfor (uint k = 0; k < KERNEL; ++k) {\n", &out, &why),
        "early: refused with the loop line twice");
  Check(!PatchSsaoSource(Replace(kFixture, "SampleMap(StencilMap, pix, 0)", "SampleMap(StencilMap, pixelPos, 0)"), &out, &why),
        "early: refused when the stencil is read at the sample position (not loop-invariant)");
  Check(!PatchSsaoSource(Replace(kFixture, "if (stv != STENCIL_COMPOSITION_WATER) {", "if (stv == STENCIL_COMPOSITION_WATER) {"),
                         &out, &why),
        "early: refused when the water test differs");
  Check(!PatchSsaoSource(Replace(kFixture, "\tacc = 1 - acc / KERNEL;", "\tacc = 1 - acc / 64;"), &out, &why),
        "early: refused without the loop's end line");
  Check(!PatchSsaoSource(Replace(kFixture, "\t\t\tacc += depth;", "\t\t\tacc += depth;\r\n\t\t\tuint s2 = StencilMap.Load(int3(pix, 0)).g;"),
                         &out, &why),
        "early: refused with a second stencil use in the loop");
}

void DecideTests() {
  using V = std::vector<uint32_t>;
  const std::vector<V> dcs = {{1, 2}, {3, 4}};
  Check(DecideIdentity(dcs, {{{1, 2}, {3, 4}}, {{1, 2}, {3, 5}}}) == 0, "early: identity = the only equal candidate");
  Check(DecideIdentity(dcs, {{{1, 2}, {3, 5}}, {{1, 2}, {3, 4}}}) == 1, "early: identity, second candidate");
  Check(DecideIdentity(dcs, {{{1, 2}, {3, 5}}, {{0, 2}, {3, 4}}}) == -1, "early: no candidate equal in every case");
  Check(DecideIdentity(dcs, {{{1, 2}, {3, 4}}, {{1, 2}, {3, 4}}}) == -2, "early: two equal candidates = ambiguous");
  Check(DecideIdentity(dcs, {{{1, 2}}}) == -1, "early: a candidate with fewer cases is not equal");
  Check(DecideIdentity({}, {{}}) == -1, "early: no cases never identifies");
}

void CaseTests() {
  const SsaoCase a = MakeSsaoCase(7), b = MakeSsaoCase(7), c = MakeSsaoCase(8);
  Check(a.depth == b.depth && a.stencil == b.stencil && a.cb7 == b.cb7, "early: synthetic cases are deterministic");
  Check(a.depth != c.depth, "early: another seed, another case");
  int water = 0, cockpit = 0, ones = 0, zeros = 0;
  for (size_t i = 1; i < a.stencil.size(); i += 2) water += (a.stencil[i] & 56u) == 16u, cockpit += (a.stencil[i] & 56u) == 40u;
  for (float d : a.depth) ones += d == 1.0f, zeros += d == 0.0f;
  const int n = static_cast<int>(a.depth.size());
  Check(water > n / 5 && water < n / 2 && cockpit > n / 20, "early: cases hold water (about 35 %) and cockpit pixels");
  Check(ones > n / 20 && zeros > n / 20, "early: cases hold depth 0 and 1 exactly");
  // gProj x gProjInv = identity (columns stored as rows)
  float p[4][4], q[4][4];
  for (int col = 0; col < 4; ++col)
    for (int r = 0; r < 4; ++r) memcpy(&p[r][col], &a.cb7[64 + 4 * col + r], 4), memcpy(&q[r][col], &a.cb7[80 + 4 * col + r], 4);
  double err = 0;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += static_cast<double>(p[i][k]) * q[k][j];
      err = (std::max)(err, fabs(s - (i == j ? 1.0 : 0.0)));
    }
  Check(err < 1e-3, "early: the case's gProj and gProjInv are inverse");
  Check(a.cb0[6] == kFpW && a.cb0[7] == kFpH, "early: the case's viewport is the target size");
}

void HintTests() {
  BindSig s;
  auto srv = [&](int slot, int dim, uint32_t w = 512, uint32_t h = 512) {
    SrvDesc d;
    d.slot = static_cast<uint8_t>(slot), d.dim = static_cast<uint8_t>(dim), d.w = w, d.h = h, d.d = 1;
    s.srv.push_back(d);
  };
  // overload: t0, t1 2D, b0, b7
  srv(0, D3D11_SRV_DIMENSION_TEXTURE2D);
  srv(1, D3D11_SRV_DIMENSION_TEXTURE2D);
  s.cbMask = (1u << 0) | (1u << 7);
  Check(HintFits(kHints[kHOverload], s), "early: the G-effect hint fits its bindings");
  Check(!HintFits(kHints[kHHotAir], s), "early: hot air needs a 3D texture at t1");
  Check(!HintFits(kHints[kHDroplets], s), "early: the droplet hint needs the 2048 x 2048 map");
  Check(!HintFits(kHints[kHCirrus], s), "early: cirrus binds no $Globals and needs b6");
  s.srv[1].dim = D3D11_SRV_DIMENSION_TEXTURE3D;
  Check(HintFits(kHints[kHHotAir], s), "early: the hot-air hint fits a 3D heat texture at t1");
  s = BindSig();
  srv(0, D3D11_SRV_DIMENSION_TEXTURE2D, 2048, 2048);
  srv(1, D3D11_SRV_DIMENSION_TEXTURE2D, 2048, 2048);
  s.cbMask = 1u;
  Check(HintFits(kHints[kHDroplets], s), "early: the droplet hint fits the 2048 x 2048 canopy map");
  s = BindSig();
  srv(103, D3D11_SRV_DIMENSION_TEXTURE2D);
  srv(104, D3D11_SRV_DIMENSION_TEXTURE2D);
  s.cbMask = 1u << 6;
  Check(HintFits(kHints[kHGlass], s), "early: the canopy glass hint fits t103 / t104 / b6");
  s.cbMask = 0;
  Check(!HintFits(kHints[kHGlass], s), "early: the glass hint needs cPerFrame");
  Check(SigText(s).find("t103:2d") != std::string::npos, "early: the binding text names slot and dimension");
  // idle tests
  const float zero[4] = {0, 1, 1, 1}, neg[4] = {-0.5f, 1, 1, 1}, ice[4] = {0.25f, 1, 1, 1};
  float nan4[4] = {std::numeric_limits<float>::quiet_NaN(), 0, 0, 0};
  Check(IcingIdle(zero) && IcingIdle(neg) && IcingIdle(nan4) && !IcingIdle(ice), "early: icing idle at gCockpitIcing.x <= 0 or NaN");
  Check(OverloadIdle(0, 0, 0) && OverloadIdle(-1, 0, 0) && OverloadIdle(0, -0.0f, -0.0f), "early: G-effect idle at zero");
  Check(!OverloadIdle(0.01f, 0, 0) && !OverloadIdle(0, 0.1f, 0) && !OverloadIdle(0, 0, -0.2f),
        "early: G-effect active with defocus, black-white or overload");
  Check(OverloadIdle(std::numeric_limits<float>::quiet_NaN(), 0, 0), "early: NaN defocus does not blur (defocus > 0 is false)");
  Check(DropletsCpuIdle(0.011f, 0) && DropletsCpuIdle(0, 3) && !DropletsCpuIdle(0.011f, 2.5f), "early: droplet streaking needs wind and time");
  Check(HotAirIdle(0) && !HotAirIdle(3), "early: hot air idle with no heat particle drawn");
  // hot-air idle skip (bit 1): pass labels as the census printed them on 2.9.31, and the skip rule
  Check(HeatKindOf("SimplePassData:HotAirPass") == 1 && HeatKindOf("HotAirModule:HotAirCopySourcePass") == 2 &&
            HeatKindOf("HotAirModule:HotAirComposePass") == 3,
        "early: hot-air pass labels recognised");
  Check(HeatKindOf("SimplePassData:TransparentPass") == 0 && HeatKindOf("HotAirModule") == 0 &&
            HeatKindOf("PassData:HotAirComposePass") == 0,
        "early: other passes are not hot-air passes");
  Check(HeatSkipOk(2, 1, 0, true) && HeatSkipOk(3, 1, 1, true), "early: hot air skipped after an empty heat pass");
  Check(!HeatSkipOk(2, 1, 0, false) && !HeatSkipOk(3, 1, 1, false), "early: hot air kept when a heat pass drew");
  Check(!HeatSkipOk(2, 0, 0, true) && !HeatSkipOk(3, 0, 1, true), "early: hot air kept with no heat pass seen this frame");
  Check(!HeatSkipOk(3, 1, 2, true) && !HeatSkipOk(2, 1, 1, true), "early: hot air kept for a second view before its heat pass");
  Check(HeatSkipOk(3, 4, 1, true) && HeatSkipOk(3, 4, 4, true), "early: four heat passes first, then four composes");
  Check(!HeatSkipOk(0, 4, 0, true) && !HeatSkipOk(1, 4, 0, true), "early: only copy and compose passes are skipped");
  Check(kIcingOff == 496 && kOverloadOff == 36, "early: the reflected offsets (2.9.31) stay as proven");
}

// --- WARP: identification by output and the draw-time swap -----------------
struct Warp {
  ID3D11Device* d = nullptr;
  ID3D11DeviceContext* c = nullptr;
  bool Make() {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &d, nullptr, &c));
  }
  ~Warp() {
    if (c) c->Release();
    if (d) d->Release();
  }
};
// Runs one fingerprint job synchronously on WARP; returns FpVerdict.
int RunJob(Warp& w, ID3D11PixelShader* dcs) {
  FpJob j;
  j.dcs = dcs;
  if (!FpRecord(j, w.d)) {
    printf("  early: fingerprint job not recorded\n");
    return -100;
  }
  w.c->ExecuteCommandList(j.cl, FALSE);
  for (ID3D11Texture2D* st : j.stage) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(w.c->Map(st, 0, D3D11_MAP_READ, 0, &m))) return -101;
    std::vector<uint32_t> v(static_cast<size_t>(kFpW) * kFpH * 4);
    for (UINT y = 0; y < kFpH; ++y)
      memcpy(&v[static_cast<size_t>(y) * kFpW * 4], static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, kFpW * 16);
    w.c->Unmap(st, 0);
    j.data.push_back(std::move(v));
  }
  const int r = FpVerdict(j);
  printf("  early: fingerprint verdict %d (%zu read-backs)\n", r, j.data.size());
  j.dcs = nullptr;
  j.ReleaseGpu();
  return r;
}

void WriteBlob(const std::wstring& dir, const wchar_t* name, const std::vector<uint8_t>& b) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, (dir + L"\\" + name).c_str(), L"wb") == 0 && f) {
    fwrite(b.data(), 1, b.size(), f);
    fclose(f);
  }
}

void InstallTests() {
  if (!dcsinstall::Found()) {
    QvSkip("early: DCS install not found: the SSAO build, identification and swap checks\n");
    return;
  }
  SsaoBuild b;
  const bool built = BuildSsao(dcsinstall::Shaders(), &b);
  if (!built) printf("  early: build refused: %s\n", b.why.c_str());
  Check(built, "early: our builds of DCS's installed deferred\\SSAO.fx reproduce the proven code hashes (64 and 128, stock and variant)");
  if (!built) return;
  wchar_t outDir[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"QV_EARLY_OUT", outDir, MAX_PATH)) {
    const std::wstring od = outDir;
    for (size_t p = od.find_first_of(L"\\/", 3); p != std::wstring::npos; p = od.find_first_of(L"\\/", p + 1))
      CreateDirectoryW(od.substr(0, p).c_str(), nullptr);
    CreateDirectoryW(outDir, nullptr);
    WriteBlob(outDir, L"ssao64_stock.dxbc", b.stock[0]);
    WriteBlob(outDir, L"ssao128_stock.dxbc", b.stock[1]);
    WriteBlob(outDir, L"ssao64_variant.dxbc", b.variant[0]);
    WriteBlob(outDir, L"ssao128_variant.dxbc", b.variant[1]);
    printf("  early: blobs written to %ls\n", outDir);
  }
  Warp w;
  if (!w.Make()) {
    QvSkip("early: WARP not available\n");
    return;
  }
  // the module's objects on the WARP device
  ID3D11Device* const savedDev = g_dev;
  g_dev = w.d;
  g_ssao.blobs = b;
  Check(CreateSsaoObjects(), "early: our SSAO builds and the fingerprint VS create on a device");
  ID3D11PixelShader *dcs64 = nullptr, *dcs128 = nullptr, *other = nullptr;
  w.d->CreatePixelShader(b.stock[0].data(), b.stock[0].size(), nullptr, &dcs64);  // "DCS's" objects: separate objects
  w.d->CreatePixelShader(b.stock[1].data(), b.stock[1].size(), nullptr, &dcs128);
  {
    std::vector<uint8_t> o;
    std::string err;
    const char* src = "float4 a : register(c0); struct O { float4 x : SV_Target0; float4 y : SV_Target1; };"
                      "O main(float4 p : SV_Position) { O o; o.x = p; o.y = 1; return o; }";
    ID3DBlob* code = nullptr;
    if (chainfast::LoadCompiler() &&
        SUCCEEDED(chainfast::g_compile(src, strlen(src), "x", nullptr, nullptr, "main", "ps_5_0", 0, 0, &code, nullptr)) && code) {
      w.d->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &other);
      code->Release();
    }
  }
  Check(RunJob(w, dcs64) == 0, "early: DCS's 64-sample SSAO object is identified as SSAO 64 (WARP)");
  Check(RunJob(w, dcs128) == 1, "early: DCS's 128-sample SSAO object is identified as SSAO 128 (WARP)");
  Check(RunJob(w, g_ssao.variant[0]) == 0, "early: the water-skip variant gives the stock output on every case (WARP)");
  Check(other && RunJob(w, other) == -1, "early: an unrelated pixel shader is not identified");
  // A variant broken on purpose (skips the loop on cockpit pixels instead of water): its output differs.
  {
    std::string src, var, why;
    TreeInclude::ReadAll(dcsinstall::Shaders() + L"deferred\\SSAO.fx", &src);
    PatchSsaoSource(src, &var, &why);
    var = Replace(var, std::string(kSsaoBranch), Replace(kSsaoBranch, "!= STENCIL_COMPOSITION_WATER", "!= STENCIL_COMPOSITION_COCKPIT"));
    var += kSsaoEntries;
    std::vector<uint8_t> bad;
    std::string err;
    ID3D11PixelShader* badPs = nullptr;
    if (CompileSsao(var, dcsinstall::Shaders(), kSsaoEntry[0], &bad, &err))
      w.d->CreatePixelShader(bad.data(), bad.size(), nullptr, &badPs);
    Check(badPs && RunJob(w, badPs) == -1, "early: a variant broken on purpose (cockpit instead of water) is not taken as SSAO 64");
    if (badPs) badPs->Release();
  }
  // The draw-time swap through the WARP immediate context's table.
  ID3D11DeviceContext* const savedCtx = g_ctx;
  g_ctx = w.c;
  for (int k = 0; k < kDkCount; ++k) g_slot[k] = DcsQv_ContextSlot(kDkNames[k]);
  g_ssao.ident[0] = dcs64;
  g_ssao.identKind[0] = 0;
  g_ssao.nIdent = 1;
  g_mask = 1;
  void** vt = *reinterpret_cast<void***>(w.c);
  void* const toggled = vt[g_slot[kDDraw]];  // after the fingerprint's ExecuteCommandList: d3d11's "after flush" entry
  Normalize();
  void* before = vt[g_slot[kDDraw]];
  printf("  early: draw entry %s after ExecuteCommandList\n", toggled == before ? "unchanged" : "toggled (normalized)");
  const bool armed = Arm(0, kDDisp);
  g_armDepth = armed ? 0 : -1;
  Check(armed && vt[g_slot[kDDraw]] == kHooks[kDDraw], "early: the draw entries are armed over d3d11's own");
  g_swapPass = true;
  const uint64_t s0 = g_swaps.load();
  w.c->PSSetShader(dcs64, nullptr, 0);
  w.c->VSSetShader(g_ssao.fpVs, nullptr, 0);
  w.c->Draw(4, 0);
  ID3D11PixelShader* after = nullptr;
  w.c->PSGetShader(&after, nullptr, nullptr);
  Check(g_swaps.load() == s0 + 1 && after == dcs64, "early: an identified SSAO draw runs the variant and DCS's shader is bound again after it");
  if (after) after->Release();
  w.c->PSSetShader(dcs128, nullptr, 0);  // not identified in this rig: offered once, not swapped
  const size_t probed0 = g_ssao.probed.size();
  const bool still = vt[g_slot[kDDraw]] == kHooks[kDDraw];
  w.c->Draw(4, 0);
  w.c->Draw(4, 0);
  printf("  early: hook %s before the unknown-shader draws; probed %zu -> %zu\n", still ? "in" : "GONE", probed0,
         g_ssao.probed.size());
  Check(g_swaps.load() == s0 + 1 && g_ssao.probed.size() == probed0 + 1, "early: an unknown SSAO-pass shader is offered once, never swapped");
  g_mask = 0;
  w.c->PSSetShader(dcs64, nullptr, 0);
  w.c->Draw(4, 0);
  Check(g_swaps.load() == s0 + 1, "early: with the key off nothing is swapped");
  g_swapPass = false;
  Disarm();
  g_armDepth = -1;
  Check(vt[g_slot[kDDraw]] != kHooks[kDDraw] && fixdepth::InD3d11(vt[g_slot[kDDraw]]),
        "early: disarm puts d3d11's entry back");
  printf("  early: re-arms after d3d11 toggles in the rig: %llu\n", static_cast<unsigned long long>(g_rearms.load()));
  Normalize();
  before = vt[g_slot[kDDraw]];
  // nested refusal: a slot holding another hook of ours is never armed over
  d3ds::WriteTablePointer(&vt[g_slot[kDDraw]], reinterpret_cast<void*>(&InstallTests));
  const uint64_t ref0 = g_armRefused.load();
  Arm(0, 1);
  Check(!g_armed[kDDraw] && g_armRefused.load() == ref0 + 1, "early: a slot held by another hook of ours is refused");
  d3ds::WriteTablePointer(&vt[g_slot[kDDraw]], before);
  // clean up the rig
  {
    std::lock_guard<std::mutex> lock(g_ssao.candMu);
    for (auto* p : g_ssao.cands) p->Release();
    g_ssao.cands.clear();
  }
  for (void* p : g_ssao.probed) static_cast<ID3D11PixelShader*>(p)->Release();
  g_ssao.probed.clear();
  g_ssao.ident[0] = nullptr;
  g_ssao.nIdent = 0;
  ReleaseSsaoObjects();
  if (g_tiny) g_tiny->Release(), g_tiny = nullptr;
  g_ctx = savedCtx;
  g_dev = savedDev;
  if (dcs64) dcs64->Release();
  if (dcs128) dcs128->Release();
  if (other) other->Release();
}

void CensusTextTests() {
  // the CSV's idle text for a G-effect class with a $Globals snapshot
  ClassRec r;
  r.hint = kHOverload;
  r.cbData[0].assign(16, 0);
  std::unordered_map<std::string, uint64_t> heat;
  Check(IdleText(r, heat).find("IDLE") != std::string::npos, "early: census idle text, G-effect at zero");
  const float ov = 0.3f;
  memcpy(&r.cbData[0][11], &ov, 4);
  Check(IdleText(r, heat).find("active") != std::string::npos, "early: census idle text, G-effect active");
  ClassRec h;
  h.hint = kHHotAir;
  h.occ = 2;
  heat["2"] = 5;
  Check(IdleText(h, heat).find("active") != std::string::npos, "early: hot air with particles in its view is active");
  heat.clear();
  Check(IdleText(h, heat).find("IDLE") != std::string::npos, "early: hot air with no particle drawn is idle");
}

// The census on WARP: one "frame" of draws in a pass with two pixel shaders, one bound like the G-effect (its
// $Globals snapshot read back and decoded), the CSV written and read.
void CensusWarpTest() {
  Warp w;
  if (!w.Make() || !chainfast::LoadCompiler()) {
    QvSkip("early: WARP or the compiler not available: census rig\n");
    return;
  }
  auto ps = [&](const char* src) {
    ID3DBlob* code = nullptr;
    ID3D11PixelShader* p = nullptr;
    if (SUCCEEDED(chainfast::g_compile(src, strlen(src), "x", nullptr, nullptr, "main", "ps_5_0", 0, 0, &code, nullptr)) && code) {
      w.d->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &p);
      code->Release();
    }
    return p;
  };
  ID3D11PixelShader* a = ps("float4 main() : SV_Target { return 1; }");
  ID3D11PixelShader* b = ps("float4 main() : SV_Target { return 0.5; }");
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  D3D11_TEXTURE2D_DESC td{64, 32, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
  w.d->CreateTexture2D(&td, nullptr, &tex);
  if (tex) w.d->CreateShaderResourceView(tex, nullptr, &srv);
  float globals[16] = {};
  globals[9] = 0.0f, globals[10] = 0.0f, globals[11] = 0.0f;  // defocus, blackwhitening, overload: idle
  ID3D11Buffer *cb0 = nullptr, *cb7 = nullptr;
  D3D11_BUFFER_DESC bd{64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
  D3D11_SUBRESOURCE_DATA sd{globals};
  w.d->CreateBuffer(&bd, &sd, &cb0);
  bd.ByteWidth = kCb7Bytes;
  w.d->CreateBuffer(&bd, nullptr, &cb7);
  ID3D11DeviceContext* const savedCtx = g_ctx;
  ID3D11Device* const savedDev = g_dev;
  g_ctx = w.c, g_dev = w.d;
  for (int k = 0; k < kDkCount; ++k) g_slot[k] = DcsQv_ContextSlot(kDkNames[k]);
  g_classes.clear(), g_classIdx.clear();
  g_cPasses = g_cDrawsTotal = g_cLostPasses = g_cRefusedPasses = 0;
  g_cbSnaps = 0, g_cMapped = 0, g_cWaitFrames = 0;
  g_cFrame = 7;
  g_cState = 2;
  g_curPass = "PassData:TestPass", g_curOcc = 1, g_cPasses = 1;
  const bool armed = Arm(0, kDkCount);
  g_armDepth = armed ? 0 : -1;
  Check(armed, "early: census arm on the WARP context");
  w.c->PSSetShader(a, nullptr, 0);
  ID3D11ShaderResourceView* two[2] = {srv, srv};
  w.c->PSSetShaderResources(0, 2, two);
  ID3D11Buffer* cbs[8] = {cb0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, cb7};
  w.c->PSSetConstantBuffers(0, 8, cbs);
  w.c->Draw(3, 0);
  w.c->Draw(6, 0);
  w.c->PSSetShader(b, nullptr, 0);
  ID3D11ShaderResourceView* none[2] = {};
  w.c->PSSetShaderResources(0, 2, none);
  w.c->DrawInstanced(4, 2, 0, 0);
  Disarm();
  g_armDepth = -1;
  g_cState = 3;
  for (int i = 0; i < 100 && g_cState.load() == 3; ++i) {
    w.c->Flush();  // DCS's frames submit; the rig has to
    Sleep(1);
    CensusFrameStep(w.c, 8);
  }
  Check(g_cState.load() == 4, "early: census read-back done");
  Check(g_classes.size() == 2 && g_cDrawsTotal == 3, "early: two classes, three draws");
  if (g_classes.size() == 2) {
    Check(g_classes[0].draws == 2 && g_classes[0].verts == 9 && g_classes[1].insts == 2, "early: per-class counts");
    Check(g_classes[0].hint == kHOverload && g_classes[1].hint < 0, "early: the G-effect-like class fits its hint, the other none");
    Check(!g_classes[0].cbData[0].empty(), "early: the hinted class's $Globals was read back");
  }
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  g_logDir = tmp;
  std::string summary;
  Check(WriteCensus(4242, &summary), "early: census file written");
  std::string text;
  TreeInclude::ReadAll(g_logDir + L"early_census_4242.csv", &text);
  Check(text.find("PassData:TestPass") != std::string::npos && text.find("G-effect") != std::string::npos &&
            text.find("IDLE") != std::string::npos && text.find("ps:early") != std::string::npos,
        "early: the CSV names the pass, the hint, the idle verdict and the early shader");
  printf("  %s\n", summary.c_str());
  DeleteFileW((g_logDir + L"early_census_4242.csv").c_str());
  for (ClassRec& r : g_classes)
    for (auto*& p : r.cbStage)
      if (p) p->Release(), p = nullptr;
  g_classes.clear(), g_classIdx.clear();
  g_cState = 0;
  if (g_tiny) g_tiny->Release(), g_tiny = nullptr;
  g_ctx = savedCtx, g_dev = savedDev;
  for (IUnknown* p : std::initializer_list<IUnknown*>{a, b, tex, srv, cb0, cb7})
    if (p) p->Release();
}

// The hot-air intercept on a pass object whose cached kind is stale: another pass made at a freed compose pass's
// address (a mission load) must be executed, not skipped.
struct FakeHeatPass {
  void* vt;
  uint64_t len;
  char name[32];
};
char g_fakeVt[2];
void HeatInterceptTests() {
  const uint32_t savedMask = g_mask.load();
  g_mask = 2;
  FakeHeatPass p{&g_fakeVt[0], 17, "HotAirComposePass"};
  void* key = &p;
  g_heatKind[key] = 3;
  g_passLabel[key] = "HotAirModule:HotAirComposePass";
  g_passSsao[key] = false;
  auto arm = [] {  // this frame: one watched, empty heat pass seen, no compose yet
    HeatFrame(ptiming::g_execFrame.load());
    g_heatSeen = 1, g_composeSeen = 0, g_heatAllIdle = true;
  };
  arm();
  const bool skipped = HeatIntercept(key, 0) == &HeatNoExecute;
  memset(p.name, 0, sizeof(p.name));
  memcpy(p.name, "SomeOtherPass", 13);
  p.len = 13;
  arm();
  const bool other = HeatIntercept(key, 0) == nullptr;
  const auto it = g_heatKind.find(key);
  Check(skipped && other && it != g_heatKind.end() && it->second == 0,
        "early: hot air: a compose pass is skipped; another pass at its address afterwards runs (kind re-checked)");
  g_heatKind.erase(key), g_passLabel.erase(key), g_passSsao.erase(key);
  g_mask = savedMask;
}

// bugfix-07: a mission event drops the per-pass caches (pass objects are rebuilt; a new one at an old address must not
// keep the old pass's hot-air kind, label or SSAO flag).
void CacheResetTests() {
  TakeCacheReset();  // an earlier suite's mission event (OnMissionEvent) may still be pending
  int dummy = 0;
  void* key = &dummy;
  g_heatKind[key] = 1;
  g_passLabel[key] = "SimplePassData:HotAirPass";
  g_passSsao[key] = true;
  TakeCacheReset();  // no event: kept
  const bool kept = g_heatKind.count(key) && g_passLabel.count(key) && g_passSsao.count(key);
  NoteMissionEvent();
  TakeCacheReset();
  Check(kept && g_heatKind.empty() && g_passLabel.empty() && g_passSsao.empty() && !g_cacheReset.load(),
        "early: a mission event drops the pass caches at the render thread's next pass (none without one)");
}

// r2-nodlss5 (the route without the Cheeky layer: every draw slot refused, the colour pass's tone map past its hook).
// A code page outside every module stands in for another module's hook: an absolute jump to d3d11's entry.
struct ForeignThunk {
  uint8_t* p = nullptr;
  explicit ForeignThunk(void* target) {
    p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!p) return;
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0]
    memcpy(p, jmp, 6);
    memcpy(p + 6, &target, 8);
    FlushInstructionCache(GetCurrentProcess(), p, 14);
  }
  ~ForeignThunk() {
    if (p) VirtualFree(p, 0, MEM_RELEASE);
  }
};
// NGX-style state save / restore around a piece of work: d3d11 leaves every entry in its first-call state.
bool SwapRoundTrip(Warp& w) {
  ID3D11Device1* d1 = nullptr;
  ID3D11DeviceContext1* c1 = nullptr;
  ID3DDeviceContextState *other = nullptr, *prev = nullptr, *mine = nullptr;
  bool ok = SUCCEEDED(w.d->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&d1))) &&
            SUCCEEDED(w.c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1)));
  const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL got{};
  ok = ok && SUCCEEDED(d1->CreateDeviceContextState(0, fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), &got, &other));
  if (ok) {
    c1->SwapDeviceContextState(other, &prev);
    c1->SwapDeviceContextState(prev, &mine);
  }
  for (IUnknown* u : std::initializer_list<IUnknown*>{mine, prev, other, c1, d1})
    if (u) u->Release();
  return ok;
}
void OwnerTests(Warp& w) {
  void** vt = *reinterpret_cast<void***>(w.c);
  Normalize();
  void* const d3dDraw = vt[g_slot[kDDraw]];
  Check(TableInContext(vt), "early: d3d11 keeps the immediate context's table inside the context object");
  // another module's hook in d3d11's own table: chained in front of it, every draw seen, put back at the disarm
  {
    ForeignThunk th(d3dDraw);
    d3ds::WriteTablePointer(&vt[g_slot[kDDraw]], th.p);
    const uint64_t f0 = g_armForeign.load(), r0 = g_armRefused.load();
    const bool armed = Arm(0, 1);
    g_armDepth = armed ? 0 : -1;
    const bool over = g_armed[kDDraw] && vt[g_slot[kDDraw]] == kHooks[kDDraw] && g_orig[kDDraw] == th.p;
    g_heatPass = true;
    g_heatDraws = 0;
    w.c->Draw(3, 0);
    const bool seen = g_heatDraws == 1;
    g_heatPass = false;
    const bool kept = Disarm();
    g_armDepth = -1;
    const bool back = vt[g_slot[kDDraw]] == th.p;
    d3ds::WriteTablePointer(&vt[g_slot[kDDraw]], d3dDraw);
    Check(over && g_armForeign.load() == f0 + 1 && g_armRefused.load() == r0,
          "early: a slot holding another module's hook is armed in front of it (no longer refused)");
    Check(seen && kept && back, "early: DCS's draw reaches our hook and the other module's; the disarm puts its hook back");
  }
  // our own hook still in a slot from an earlier arm: taken back as armed, d3d11's entry back at the disarm
  {
    g_orig[kDDraw] = d3dDraw;
    d3ds::WriteTablePointer(&vt[g_slot[kDDraw]], kHooks[kDDraw]);
    const uint64_t s0 = g_armStaleOwn.load(), r0 = g_armRefused.load();
    Arm(0, 1);
    const bool taken = g_armed[kDDraw] && g_armStaleOwn.load() == s0 + 1 && g_armRefused.load() == r0;
    Disarm();
    Check(taken && vt[g_slot[kDDraw]] == d3dDraw, "early: our own stale hook in a slot is taken back as armed and removed at the disarm");
    d3ds::WriteTablePointer(&vt[g_slot[kDDraw]], d3dDraw);
  }
  // a proxy's table (a class vtable outside the context object): another module's entry there stays refused
  {
    ForeignThunk th(d3dDraw);
    std::vector<void*> copy(vt, vt + DcsQv_ContextSlotCount());
    copy[g_slot[kDDraw]] = th.p;
    struct FakeCtx {
      void** vptr;
    } fake{copy.data()};
    Ctx* const realCtx = g_ctx;
    ID3D11Device* const realDev = g_dev;
    if (g_tiny) g_tiny->Release(), g_tiny = nullptr;
    g_dev = nullptr;  // Normalize makes no call through the fake table
    g_ctx = reinterpret_cast<Ctx*>(&fake);
    const uint64_t r0 = g_armRefused.load();
    const bool armed = Arm(0, 1);
    const bool refused = !armed && !g_armed[kDDraw] && copy[g_slot[kDDraw]] == th.p && g_armRefused.load() == r0 + 1;
    g_ctx = realCtx, g_dev = realDev;
    Check(refused, "early: another module's entry in a proxy's table (outside the context) is still refused");
  }
  // d3d11's first-call state (a SwapDeviceContextState round trip, as DCS's own DLSS on the route without Cheeky)
  if (!SwapRoundTrip(w)) {
    QvSkip("early: SwapDeviceContextState not available: first-call state checks\n");
    return;
  }
  {  // control: the old normalize (one upload) and an arm: the first state call takes the draw arm out
    static const uint32_t zero[4] = {};
    if (!g_tiny && g_dev) {
      D3D11_BUFFER_DESC bd{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
      g_dev->CreateBuffer(&bd, nullptr, &g_tiny);
    }
    if (g_tiny) w.c->UpdateSubresource(g_tiny, 0, nullptr, zero, 0, 0);
    void* const cur = vt[g_slot[kDDraw]];
    d3ds::WriteTablePointer(&vt[g_slot[kDDraw]], kHooks[kDDraw]);
    ID3D11ShaderResourceView* none = nullptr;
    w.c->PSSetShaderResources(0, 1, &none);
    const bool gone = vt[g_slot[kDDraw]] != kHooks[kDDraw];
    if (!gone) d3ds::WriteTablePointer(&vt[g_slot[kDDraw]], cur);
    // Informational: on this PC's hardware driver the hook is gone here (offline probe 2026-11-23); WARP at feature
    // level 11_0 may re-validate at the upload already. The checks below hold either way.
    printf("  early: after a SwapDeviceContextState an upload alone %s\n",
           gone ? "left the first-call state (the first state call took the draw hook out)" : "re-validated the table");
  }
  if (!SwapRoundTrip(w)) return;
  const uint64_t v0 = fixdepth::g_tblRevalidated.load();
  const bool armed = Arm(0, kDDisp);
  g_armDepth = armed ? 0 : -1;
  ID3D11ShaderResourceView* none = nullptr;
  w.c->PSSetShaderResources(0, 1, &none);
  w.c->PSSetShader(nullptr, nullptr, 0);
  const bool kept = armed && vt[g_slot[kDDraw]] == kHooks[kDDraw] && vt[g_slot[kDIdx]] == kHooks[kDIdx];
  g_heatPass = true;
  g_heatDraws = 0;
  w.c->Draw(3, 0);
  const bool seen = g_heatDraws == 1;
  g_heatPass = false;
  Disarm();
  g_armDepth = -1;
  Check(fixdepth::g_tblRevalidated.load() == v0 + 1, "early: the arm found the first-call state and re-validated the table (counted)");
  Check(kept && seen, "early: after a SwapDeviceContextState the arm survives DCS's state calls and sees the draw");
}

// bugfix-07: on a WARP context, a HotAirPass whose draw entries are armed only in part (DrawIndexed held by another
// hook) is not watched: a heat draw through the refused slot would not be counted and the frame would read idle.
// And a fault inside a draw hook mid-pass: the pass end still takes the arms out.
void ArmTests() {
  Warp w;
  if (!w.Make()) {
    QvSkip("early: WARP not available: arm tests\n");
    return;
  }
  ID3D11DeviceContext* const savedCtx = g_ctx;
  ID3D11Device* const savedDev = g_dev;
  const uint32_t savedMask = g_mask.load();
  const DWORD savedTop = ptiming::g_topTid.load();
  g_ctx = w.c, g_dev = w.d;
  for (int k = 0; k < kDkCount; ++k) g_slot[k] = DcsQv_ContextSlot(kDkNames[k]);
  void** vt = *reinterpret_cast<void***>(w.c);
  Normalize();
  void* const idx = vt[g_slot[kDIdx]];
  d3ds::WriteTablePointer(&vt[g_slot[kDIdx]], reinterpret_cast<void*>(&InstallTests));  // another hook of ours
  int passObj = 0;
  void* pass = &passObj;
  g_heatKind[pass] = 1;
  g_passLabel[pass] = "SimplePassData:HotAirPass";
  g_passSsao[pass] = false;
  g_mask = 2;
  HeatFrame(~0ull - 1);
  OnBoundaryBody(pass, true, 0);
  const bool partial = g_armed[kDDraw] && !g_armed[kDIdx];
  const bool watched = g_heatPass, idle = g_heatAllIdle;
  OnBoundaryBody(pass, false, 0);
  d3ds::WriteTablePointer(&vt[g_slot[kDIdx]], idx);
  Check(partial && !watched && !idle && !AnyArmed(),
        "early: hot air: a pass armed only in part (DrawIndexed held by another hook) is not watched; the frame is not idle");
  // fault mid-pass
  ptiming::g_topTid = GetCurrentThreadId();
  g_stopping = false;
  const bool armed = Arm(0, kDDisp);
  g_armDepth = armed ? 0 : -1;
  g_fault = true;
  OnBoundary(pass, false, 0);
  const bool out = !AnyArmed() && vt[g_slot[kDDraw]] != kHooks[kDDraw] && g_armDepth == -1;
  g_fault = false;
  Check(armed && out, "early: a fault in a draw hook mid-pass: the pass end takes the arms out of DCS's table");
  g_heatKind.erase(pass), g_passLabel.erase(pass), g_passSsao.erase(pass);
  OwnerTests(w);
  if (g_tiny) g_tiny->Release(), g_tiny = nullptr;
  g_mask = savedMask;
  ptiming::g_topTid = savedTop;
  g_ctx = savedCtx, g_dev = savedDev;
}

void Run() {
  CacheResetTests();
  ArmTests();
  HeatInterceptTests();
  CensusWarpTest();
  PatchTests();
  DecideTests();
  CaseTests();
  HintTests();
  CensusTextTests();
  InstallTests();
}

}  // namespace estest
