// W1 ([Water] SkipHiddenWater, w1_water.h): the decision logic, a replay of recorded live frames through it, and the
// whole path on a D3D11 device (hardware, WARP fallback) with fake DCS passes: the predicated block, the upload
// replay, the start guard, the water-seen switch, the fallbacks, verify mode, the key going off.
#pragma once

namespace w1test {

// ---------------------------------------------------------------------------
// Recorded frames (live verify runs of 2026-11-18, ATMOSX, DCS 2.9.31; artifacts\gpu-review\w1\runs): one byte per
// frame, bits 0-3 = view 0-3's test saw a water sample, bits 4-7 = view 0-3's real water passes had one; run-length
// encoded. Recorded in verify mode (M ran every view), so the sea test's inputs were fresher than mode 1 leaves them.
// ---------------------------------------------------------------------------
struct FxRun {
  uint8_t bits;
  uint32_t n;
};
// s3_coast: F-16 over the Marianas coast, yaw sweeps at pitch -25/-8/-15, F1/F2/F10 switches: 48066 frames.
const FxRun kFxCoast[] = {
    {0xfe, 1}, {0xff, 317}, {0xee, 1}, {0xcc, 1}, {0xff, 48}, {0xee, 1}, {0xff, 2}, {0xcc, 1}, {0xff, 7104},
    {0x33, 13}, {0x00, 28}, {0x44, 1}, {0x00, 85}, {0x44, 1}, {0x00, 89}, {0x11, 1}, {0x00, 11}, {0x11, 2},
    {0x00, 3}, {0x88, 1}, {0x00, 4}, {0x11, 2}, {0x00, 7}, {0x11, 2}, {0x00, 16}, {0x11, 2}, {0x00, 7}, {0x11, 2},
    {0x00, 4}, {0x11, 1}, {0x00, 10}, {0x88, 1}, {0x11, 2}, {0x00, 8}, {0x11, 2}, {0xff, 40285}};
// s2_low part 1: F-16 at 500 m over Germany, rivers in view, sweeps: 5615 frames.
const FxRun kFxLowA[] = {
    {0xff, 525}, {0xbb, 1}, {0xff, 1}, {0xbb, 1}, {0x33, 1}, {0xff, 2}, {0xbb, 3}, {0xff, 1}, {0xbb, 5}, {0xff, 1},
    {0xbb, 3}, {0x33, 1}, {0xbb, 1}, {0x33, 2}, {0xbb, 1}, {0x33, 2}, {0xbb, 1}, {0x33, 2}, {0xbb, 2}, {0x33, 1},
    {0xbb, 8}, {0x33, 1}, {0xbb, 5}, {0x33, 1}, {0xbb, 1}, {0x33, 3}, {0xbb, 1}, {0x33, 191}, {0xbb, 1}, {0x33, 3},
    {0xbb, 2}, {0x33, 2}, {0xbb, 1}, {0x33, 3}, {0xbb, 1}, {0x33, 2}, {0xbb, 1}, {0x33, 1}, {0xbb, 1}, {0xff, 1},
    {0x33, 1}, {0xbb, 1}, {0x33, 1}, {0xbb, 2}, {0x33, 1}, {0xbb, 2}, {0x33, 2}, {0xbb, 3}, {0x33, 2}, {0xbb, 1},
    {0x33, 2}, {0xbb, 2}, {0x33, 1}, {0xbb, 6}, {0xff, 1}, {0xbb, 5}, {0x77, 1}, {0xbb, 2}, {0x33, 1}, {0xff, 1},
    {0xbb, 1}, {0xff, 1}, {0xbb, 1}, {0xff, 2}, {0x33, 1}, {0x77, 1}, {0xff, 3}, {0x33, 1}, {0xbb, 1}, {0xff, 1},
    {0xbb, 1}, {0xff, 1}, {0xbb, 1}, {0xff, 2}, {0xbb, 1}, {0xff, 1}, {0xbb, 1}, {0xff, 1}, {0xbb, 1}, {0xff, 4767}};
// s2_low part 2: the same flight later, 594 dry frames among the sweeps: 19924 frames.
const FxRun kFxLowB[] = {{0xff, 3736}, {0x33, 8}, {0x00, 594}, {0xff, 15586}};
// s1a_ramp: F-4E on the Germany ramp, F2, paused (no water): 6090 frames.
const FxRun kFxRamp[] = {{0x00, 6090}};

struct ReplayOut {
  uint64_t frames = 0, tested = 0, skipped = 0, stock = 0, forced = 0, wrong = 0, wrongWithoutGuard = 0;
};
// Mode 1 over the recorded frames: each frame's result reaches the gate `lat` frames later (live: 1-3).
template <size_t N>
ReplayOut Replay(const FxRun (&runs)[N], int lat) {
  std::vector<uint8_t> seq;
  for (const FxRun& r : runs) seq.insert(seq.end(), r.n, r.bits);
  std::vector<bool> water(seq.size(), false);
  w1::Gate g;
  g.Start();
  ReplayOut o;
  for (size_t i = 0; i < seq.size(); ++i) {
    if (i >= static_cast<size_t>(lat)) g.OnFrameRead(water[i - lat]);
    g.BeginFrame();
    bool w = false;
    for (int v = 0; v < 4; ++v) {
      const bool test = (seq[i] >> v) & 1, real = (seq[i] >> (4 + v)) & 1;
      const w1::ViewPlan p = w1::PlanView(g);
      if (p == w1::kPlanStock) {
        ++o.stock;
        w = w || real;
        continue;
      }
      ++o.tested;
      if (p == w1::kPlanForced) ++o.forced;
      if (w1::Skips(p, test)) {
        ++o.skipped;
        if (real) ++o.wrong;
      }
      if (!test && real) ++o.wrongWithoutGuard;
      w = w || test;
    }
    water[i] = w;
    ++o.frames;
  }
  return o;
}

void Pure() {
  w1::Gate g;
  g.Start();
  int forced = 0;
  for (int i = 0; i < 6; ++i) {
    g.BeginFrame();
    forced += g.forced ? 1 : 0;
    if (i < 3 && !g.forced) forced = -100;
  }
  g.Arm();
  int again = 0;
  for (int i = 0; i < 5; ++i) g.BeginFrame(), again += g.forced ? 1 : 0;
  Check(forced == 3 && again == 3 && w1::PlanView(g) == w1::kPlanTest,
        "W1 gate: the start guard runs exactly the first 3 frames after the key goes on, and 3 after a cut or pause");
  g.OnFrameRead(true);
  const bool stock = g.stock && w1::PlanView(g) == w1::kPlanStock && g.toStock == 1;
  for (int i = 0; i < 89; ++i) g.OnFrameRead(false);
  const bool still = g.stock;
  g.OnFrameRead(true);  // water again resets the dry count
  for (int i = 0; i < 89; ++i) g.OnFrameRead(false);
  const bool still2 = g.stock;
  g.OnFrameRead(false);
  Check(stock && still && still2 && !g.stock && g.toTest == 1 && g.toStock == 1,
        "W1 gate: water seen -> every view stock; tested again only after 90 frames in a row without water");
  Check(w1::Skips(w1::kPlanTest, 0) && !w1::Skips(w1::kPlanTest, 1) && !w1::Skips(w1::kPlanForced, 0) &&
            !w1::Skips(w1::kPlanStock, 0) && w1::SanitizeMode(1) == 1 && w1::SanitizeMode(2) == 2 &&
            w1::SanitizeMode(3) == 0 && w1::SanitizeMode(-1) == 0 && w1::SanitizeMode(0) == 0,
        "W1 decisions: only a tested view whose test saw nothing is skipped; key values other than 1 and 2 are off "
        "(the development-only 3 is gone)");
}

void Fixtures() {
  bool ok = true;
  for (int lat = 1; lat <= 4; ++lat) {
    const ReplayOut c = Replay(kFxCoast, lat), a = Replay(kFxLowA, lat), b = Replay(kFxLowB, lat), r = Replay(kFxRamp, lat);
    const bool l = c.wrong == 0 && a.wrong == 0 && b.wrong == 0 && r.wrong == 0 && c.wrongWithoutGuard >= 1 &&
                   r.skipped == (r.frames - w1::kStartFrames) * 4 && b.skipped >= (594 - 90 - 4) * 4 &&
                   b.skipped <= 594 * 4;
    if (!l || lat == 2)
      printf("     replay latency %d: coast %llu frames, %llu skipped, %llu wrong (%llu without the start guard); low A %llu "
             "skipped, %llu wrong; low B %llu skipped, %llu wrong; ramp %llu skipped of %llu view-frames, %llu wrong\n",
             lat, (unsigned long long)c.frames, (unsigned long long)c.skipped, (unsigned long long)c.wrong,
             (unsigned long long)c.wrongWithoutGuard, (unsigned long long)a.skipped, (unsigned long long)a.wrong,
             (unsigned long long)b.skipped, (unsigned long long)b.wrong, (unsigned long long)r.skipped,
             (unsigned long long)r.frames * 4, (unsigned long long)r.wrong);
    ok = ok && l;
  }
  Check(ok, "W1 replay of recorded live frames (coast, rivers, towns, ramp; read-back latency 1-4 frames): no view "
            "skipped while its real water passes had a sample; the coast's first frame needs the start guard; the "
            "ramp skips every view after the guard; 594 dry frames over towns skip after 90");
}

// ---------------------------------------------------------------------------
// The whole path on a device, with fake DCS passes
// ---------------------------------------------------------------------------
struct FakePass {
  void* vt;
  uint64_t len;
  char name[32];
};
char g_vtTag[5];  // distinct fake vtable addresses
FakePass g_pTerrain{&g_vtTag[0], 0, {}}, g_pM{&g_vtTag[1], 17, "WaterWaveMaskPass"}, g_pR{&g_vtTag[2], 0, {}},
    g_pRiv{&g_vtTag[3], 0, {}}, g_pSea{&g_vtTag[4], 0, {}}, g_pOther{&g_vtTag[0], 0, {}};

struct Rig {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11Texture2D *ds = nullptr, *gb = nullptr, *mask = nullptr, *refr = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11RenderTargetView *gbRtv = nullptr, *maskRtv = nullptr, *refrRtv = nullptr;
  ID3D11Buffer *cbM = nullptr, *cbW = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11DepthStencilState *dssMark = nullptr, *dssOff = nullptr, *dssPlain = nullptr;
  float waterDepth = 0.3f;  // reversed Z, GREATER_EQUAL: visible above the terrain's 0.5
  bool unknownDraw = false;
  uint32_t tag = 0;  // this frame's value: M's colour, R's clear, the uploads
};
Rig g_rig;

void Upload(ID3D11Buffer* b, float d, uint32_t tag) {
  const float v[4] = {d, (tag & 0xff) / 255.0f, ((tag >> 8) & 0xff) / 255.0f, 0.5f};
  g_rig.ctx->UpdateSubresource(b, 0, nullptr, v, 0, 0);
}
void Draw(ID3D11Buffer* cb) {
  ID3D11DeviceContext* c = g_rig.ctx;
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c->IASetInputLayout(nullptr);
  c->VSSetShader(g_rig.vs, nullptr, 0);
  c->PSSetShader(g_rig.ps, nullptr, 0);
  c->VSSetConstantBuffers(0, 1, &cb);
  c->PSSetConstantBuffers(0, 1, &cb);
  D3D11_VIEWPORT vp{0, 0, 64, 64, 0, 1};
  c->RSSetViewports(1, &vp);
  c->Draw(3, 0);
}
void __fastcall FakeExec(void* pass, uint64_t) {
  ID3D11DeviceContext* c = g_rig.ctx;
  if (pass == &g_pTerrain) {
    c->ClearDepthStencilView(g_rig.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.5f, 0);
    const float black[4] = {0, 0, 0, 0};
    c->ClearRenderTargetView(g_rig.gbRtv, black);
  } else if (pass == &g_pM) {
    Upload(g_rig.cbM, 0.5f, g_rig.tag);
    c->OMSetRenderTargets(1, &g_rig.maskRtv, nullptr);
    c->OMSetDepthStencilState(g_rig.dssOff, 0);
    c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    Draw(g_rig.cbM);
  } else if (pass == &g_pR) {
    const float col[4] = {(g_rig.tag & 0xff) / 255.0f, 0, 0, 1};
    c->ClearRenderTargetView(g_rig.refrRtv, col);
  } else if (pass == &g_pRiv || pass == &g_pSea) {
    Upload(g_rig.cbW, 0.95f, 0xffff);  // a first upload to the same buffer: only the last one may count
    Upload(g_rig.cbW, g_rig.waterDepth, g_rig.tag);
    c->OMSetRenderTargets(1, &g_rig.gbRtv, g_rig.dsv);
    c->OMSetDepthStencilState(g_rig.unknownDraw && pass == &g_pRiv ? g_rig.dssPlain : g_rig.dssMark, 0x10);
    c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    Draw(g_rig.cbW);
  }
}

uint32_t ReadTexel(ID3D11Texture2D* t) {
  D3D11_TEXTURE2D_DESC d{};
  t->GetDesc(&d);
  d.BindFlags = 0, d.Usage = D3D11_USAGE_STAGING, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ, d.MiscFlags = 0;
  ID3D11Texture2D* s = nullptr;
  if (FAILED(g_rig.dev->CreateTexture2D(&d, nullptr, &s))) return 0xdeadbeef;
  g_rig.ctx->CopyResource(s, t);
  D3D11_MAPPED_SUBRESOURCE m{};
  uint32_t v = 0xdeadbeef;
  if (SUCCEEDED(g_rig.ctx->Map(s, 0, D3D11_MAP_READ, 0, &m))) {
    v = static_cast<const uint32_t*>(m.pData)[32 * (m.RowPitch / 4) + 32];  // the centre texel
    g_rig.ctx->Unmap(s, 0);
  }
  s->Release();
  return v;
}
bool ReadBuffer(ID3D11Buffer* b, float out[4]) {
  D3D11_BUFFER_DESC d{};
  b->GetDesc(&d);
  d.BindFlags = 0, d.Usage = D3D11_USAGE_STAGING, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ, d.MiscFlags = 0;
  ID3D11Buffer* s = nullptr;
  if (FAILED(g_rig.dev->CreateBuffer(&d, nullptr, &s))) return false;
  g_rig.ctx->CopyResource(s, b);
  D3D11_MAPPED_SUBRESOURCE m{};
  const bool ok = SUCCEEDED(g_rig.ctx->Map(s, 0, D3D11_MAP_READ, 0, &m));
  if (ok) memcpy(out, m.pData, 16), g_rig.ctx->Unmap(s, 0);
  s->Release();
  return ok;
}
inline uint32_t MaskTag(uint32_t texel) { return texel & 0xffff; }  // M draws (tag low byte, next byte) into R, G

bool Setup() {
  Rig& r = g_rig;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &r.dev, nullptr,
                               &r.ctx)) &&
      FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &r.dev, nullptr,
                               &r.ctx)))
    return false;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = td.Height = 64;
  td.MipLevels = td.ArraySize = 1;
  td.SampleDesc.Count = 1;
  td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  if (FAILED(r.dev->CreateTexture2D(&td, nullptr, &r.ds)) || FAILED(r.dev->CreateDepthStencilView(r.ds, nullptr, &r.dsv)))
    return false;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  for (auto p : {std::make_pair(&r.gb, &r.gbRtv), std::make_pair(&r.mask, &r.maskRtv), std::make_pair(&r.refr, &r.refrRtv)})
    if (FAILED(r.dev->CreateTexture2D(&td, nullptr, p.first)) || FAILED(r.dev->CreateRenderTargetView(*p.first, nullptr, p.second)))
      return false;
  D3D11_BUFFER_DESC bd{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  if (FAILED(r.dev->CreateBuffer(&bd, nullptr, &r.cbM)) || FAILED(r.dev->CreateBuffer(&bd, nullptr, &r.cbW))) return false;
  HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = dll ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dll, "D3DCompile")) : nullptr;
  if (!compile) return false;
  static const char kSrc[] =
      "cbuffer C : register(b0) { float4 v; };\n"
      "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
      "  float2 p = float2((id << 1) & 2, id & 2);\n"
      "  return float4(p * float2(2, -2) + float2(-1, 1), v.x, 1);\n"
      "}\n"
      "float4 ps() : SV_Target { return float4(v.y, v.z, v.w, 1); }\n";
  ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
  compile(kSrc, sizeof(kSrc) - 1, "w1", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vb, &err);
  if (err) err->Release(), err = nullptr;
  compile(kSrc, sizeof(kSrc) - 1, "w1", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &pb, &err);
  if (err) err->Release();
  if (!vb || !pb) return false;
  const bool sh = SUCCEEDED(r.dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &r.vs)) &&
                  SUCCEEDED(r.dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &r.ps));
  vb->Release(), pb->Release();
  if (!sh) return false;
  D3D11_DEPTH_STENCIL_DESC d{};
  d.DepthEnable = TRUE, d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL, d.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
  d.StencilEnable = TRUE, d.StencilReadMask = 0xff, d.StencilWriteMask = 0x38;
  d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
  d.BackFace = d.FrontFace;
  if (FAILED(r.dev->CreateDepthStencilState(&d, &r.dssMark))) return false;
  d.StencilEnable = FALSE;
  if (FAILED(r.dev->CreateDepthStencilState(&d, &r.dssPlain))) return false;
  d.DepthEnable = FALSE;
  return SUCCEEDED(r.dev->CreateDepthStencilState(&d, &r.dssOff));
}

void Frame(bool otherBetween = false) {
  ++g_rig.tag;
  ++g_quadFrame;
  ptiming::Hook(&g_pTerrain, 0);
  for (int v = 0; v < 4; ++v) {
    ptiming::Hook(&g_pM, 0);
    ptiming::Hook(&g_pR, 0);
    if (otherBetween) ptiming::Hook(&g_pOther, 0);
    ptiming::Hook(&g_pRiv, 0);
    ptiming::Hook(&g_pSea, 0);
  }
  gpttest::WaitGpuIdle(g_rig.dev, g_rig.ctx);  // the next frame reads this one's queries (latency 1)
}

uint64_t Sum(std::atomic<uint64_t>* a, int n = 4) {
  uint64_t s = 0;
  for (int i = 0; i < n; ++i) s += a[i].load();
  return s;
}

void Device() {
  if (!Setup()) {
    Check(false, "W1 device: create a D3D11 device, targets and shaders");
    return;
  }
  Rig& r = g_rig;
  void** vt = *reinterpret_cast<void***>(r.ctx);
  const int sDss = DcsQv_ContextSlot("OMSetDepthStencilState"), sBs = DcsQv_ContextSlot("OMSetBlendState"),
            sUpd = DcsQv_ContextSlot("UpdateSubresource");
  void* before[3] = {vt[sDss], vt[sBs], vt[sUpd]};
  auto tableStock = [&] { return vt[sDss] == before[0] && vt[sBs] == before[1] && vt[sUpd] == before[2]; };
  // The module's state, saved and replaced for this test.
  const ptiming::ExecFn origSave = ptiming::g_orig;
  ID3D11Device* devSave = shadowinst::g_device;
  const uint32_t opSave = sfilt::g_opMask.load();
  ptiming::g_orig = &FakeExec;
  shadowinst::g_device = r.dev;
  sfilt::g_opMask = sfilt::kDefaultOps;
  w1::g_vtKind[g_pTerrain.vt] = w1::kOther;
  w1::g_vtKind[g_pM.vt] = w1::kNone;  // PassData: the inline name decides
  w1::g_vtKind[g_pR.vt] = w1::kRefr;
  w1::g_vtKind[g_pRiv.vt] = w1::kRivers;
  w1::g_vtKind[g_pSea.vt] = w1::kSea;
  w1::g_gapMs = 600000;  // a slow test machine is not a pause
  ptiming::g_intercept.store(&w1::Intercept);
  w1::g_mode = 1;

  // 1. Water hidden behind the terrain: 3 start-guard frames drawn, then every view skipped.
  r.waterDepth = 0.3f;
  for (int i = 0; i < 3; ++i) Frame();
  const uint32_t guardTag = r.tag;
  const uint32_t maskGuard = MaskTag(ReadTexel(r.mask)), refrGuard = ReadTexel(r.refr) & 0xff;
  for (int i = 0; i < 5; ++i) Frame();
  float cbM[4] = {}, cbW[4] = {};
  const bool bufs = ReadBuffer(r.cbM, cbM) && ReadBuffer(r.cbW, cbW);
  const uint32_t want = r.tag;
  const bool upOk = bufs && fabsf(cbM[1] - (want & 0xff) / 255.0f) < 1e-6f && fabsf(cbW[0] - 0.3f) < 1e-6f &&
                    fabsf(cbW[1] - (want & 0xff) / 255.0f) < 1e-6f;
  const uint32_t mask1 = MaskTag(ReadTexel(r.mask)), refr1 = ReadTexel(r.refr) & 0xff, ds1 = ReadTexel(r.ds);
  const uint64_t skipped1 = Sum(w1::g_cSkipped), forced1 = Sum(w1::g_cForced);
  Check(maskGuard == (guardTag & 0xffff) && refrGuard == (guardTag & 0xff) && mask1 == maskGuard && refr1 == refrGuard &&
            (ds1 >> 24) == 0 && skipped1 >= 12 && forced1 == 12 && w1::g_cReasons[w1::kRsStart].load() == 12,
        "W1 device: water hidden by the terrain: the 3 start-guard frames draw the mask and refraction, then every "
        "view's mask, refraction and water passes are skipped by the GPU (no stencil WATER)");
  Check(upOk, "W1 device: in skipped views every constant buffer still ends with the last bytes DCS uploaded (the "
              "replay after the predicated block; a second upload to the same buffer wins)");
  Check(tableStock(), "W1 device: DCS's context table holds d3d11's entries again after every block");

  // 2. Water in view: the test sees it, the same frame draws everything, then every view runs stock.
  r.waterDepth = 0.7f;
  Frame();
  const uint32_t mask2 = MaskTag(ReadTexel(r.mask)), ds2 = ReadTexel(r.ds), tag2 = r.tag;
  Frame();
  const bool stockNow = w1::g_gate.stock && w1::g_cSwitchStock.load() >= 1;
  Frame();
  Check(mask2 == (tag2 & 0xffff) && ((ds2 >> 24) & 0x38) == 0x10 && stockNow &&
            MaskTag(ReadTexel(r.mask)) == (r.tag & 0xffff) && Sum(w1::g_cStock) >= 4,
        "W1 device: water in view: the frame it appears draws mask and water (stencil WATER); then every view runs "
        "stock");

  // 3. Hidden again: stock for 90 dry frames, then tested and skipped again.
  r.waterDepth = 0.3f;
  for (int i = 0; i < 88; ++i) Frame();
  const bool stillStock = w1::g_gate.stock;
  for (int i = 0; i < 8; ++i) Frame();
  const uint32_t mask3 = MaskTag(ReadTexel(r.mask));
  Check(stillStock && !w1::g_gate.stock && w1::g_cSwitchTest.load() >= 1 && mask3 != (r.tag & 0xffff) &&
            Sum(w1::g_cSkipped) > skipped1,
        "W1 device: after 90 frames without water the views are tested again and skipped");

  // 4. A depth-stencil state during the test that does not mark water: the view runs stock ("unknown draw").
  r.unknownDraw = true;
  const uint64_t unk0 = w1::g_cReasons[w1::kRsUnknownDraw].load();
  Frame();
  Frame();
  const bool unk = w1::g_cReasons[w1::kRsUnknownDraw].load() >= unk0 + 4 && MaskTag(ReadTexel(r.mask)) == (r.tag & 0xffff);
  r.unknownDraw = false;
  // 5. Another pass between a view's water passes: what is held runs at once ("pass order").
  const uint64_t ord0 = w1::g_cReasons[w1::kRsPassOrder].load();
  Frame(true);
  Frame(true);
  const bool ord = w1::g_cReasons[w1::kRsPassOrder].load() >= ord0 + 4 && MaskTag(ReadTexel(r.mask)) == (r.tag & 0xffff);
  Frame();
  Check(unk && ord && tableStock(),
        "W1 device: a non-water depth-stencil state in the test and a foreign pass between the water passes both run "
        "the view stock (mask drawn), counted by reason");

  // 6. Verify: nothing skipped, the test and the real passes counted; M's target found for the mode-1 view.
  w1::g_mode = 2;
  const uint64_t kept0 = Sum(w1::g_cKept), sk0 = Sum(w1::g_cSkipped);
  for (int i = 0; i < 6; ++i) Frame();
  // (the last mode-1 frame, read back in the first verify frame, may add its 4 skipped views)
  const bool ver = Sum(w1::g_cKept) >= kept0 + 16 && Sum(w1::g_cSkipped) <= sk0 + 4 && MaskTag(ReadTexel(r.mask)) == (r.tag & 0xffff) &&
                   w1::g_nMt == 1 && Sum(w1::g_cViolR) == 0 && Sum(w1::g_cViolS) == 0;
  r.waterDepth = 0.7f;
  for (int i = 0; i < 3; ++i) Frame();
  Check(ver && Sum(w1::g_cViolR) == 0 && Sum(w1::g_cViolS) == 0 && ((ReadTexel(r.ds) >> 24) & 0x38) == 0x10,
        "W1 device: verify mode draws everything (stock picture), counts the test next to the real passes (no "
        "'test 0, real > 0'), and finds the mask pass's target for its mode-1 view of the test");

  // 7. Key off: stock at the next pass, then Shutdown releases everything.
  w1::g_mode = 0;
  r.waterDepth = 0.3f;
  Frame();
  const bool off = w1::g_rMode == 0 && MaskTag(ReadTexel(r.mask)) == (r.tag & 0xffff) && tableStock();
  w1::Shutdown();
  Check(off && !ptiming::g_intercept.load() && !w1::g_ctx && w1::g_dssMap.empty() && w1::g_nMt == -1,
        "W1 device: key 0 runs DCS's passes untouched; Shutdown takes the intercept out and releases its objects");

  // Restore the module's state.
  ptiming::g_orig = origSave;
  shadowinst::g_device = devSave;
  sfilt::g_opMask = opSave;
  w1::g_vtKind.clear();
  w1::g_gapMs = 500;
  for (auto* p : {&r.ds, &r.gb, &r.mask, &r.refr}) if (*p) (*p)->Release(), *p = nullptr;
  if (r.dsv) r.dsv->Release();
  for (auto* p : {r.gbRtv, r.maskRtv, r.refrRtv}) if (p) p->Release();
  for (auto* p : {r.cbM, r.cbW}) if (p) p->Release();
  for (auto* p : {r.dssMark, r.dssOff, r.dssPlain}) if (p) p->Release();
  if (r.vs) r.vs->Release();
  if (r.ps) r.ps->Release();
  r.ctx->ClearState();
  r.ctx->Release();
  r.dev->Release();
  g_rig = Rig{};
}

void Run() {
  Pure();
  Fixtures();
  Device();
}

}  // namespace w1test
