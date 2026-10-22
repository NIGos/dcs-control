// R128: cockpit ipd's dormant mode ([Perception] CockpitIpdDormant; src/cockpit_ipd.h, docs/research
// R128_ipd_dormant.md). Included from cockpit_ipd_test.h inside namespace cktest, after ovl_cpu_test.h (its DCS
// replica of the symbology records) and before cktest::Run.
//
// One rig, the whole option-3 path of a frame on a D3D11 device (WARP; QV_R128_HW=1 or the R95 HW suite: also this
// PC's GPU): BoostOut written per frame by a fake layer, the culling call's eye separation, per HMD view the lights
// pass, the three G-buffer executions through gbpass::Hook (cockpit sm 4 with stencil 40 and depth, world, decal),
// filler passes with an upload each, GBufferLightingPass with a full-screen draw reading the cockpit depth (the
// phase-2 split), TransparentCockpitPass, the overlay passes (OverlayHDRPass / OverlayLDRPass / GUIPass) with
// ovl_cpu_test.h's record replay through the render point (DCS 2.9.30: no filter point), the effect uploads, the
// HUD / display / label / GUI draws with stream output, and CopyToHMDPass; every boundary through
// fixdepth::OnBoundary as DCS's pass callback calls it.
// Per frame a digest of everything the GPU sees from us: the view's cPerView inside and after each G-buffer
// execution and in the forward pass, the lighting target, every overlay draw's clip position (stream output), and
// the context's VP bytes after each replay; plus the frame's per-view outcome (patched, failed, lit, forward and
// overlay passes, replays moved, records at the true eye) and the symbology export's records per view.
//  X1 exactness: the same frame sequence with CockpitIpdDormant=0 (today's code path) and =1: every frame's digest
//     and outcome equal. The sequence: scale 1 (the learner commits at h 0), ramps 1 -> 1.1 -> 1.25 -> 1, single-
//     frame and 3-frame steps, the in-flight pause while boosted, BoostOut stale (no record), external-view frames
//     (no cockpit pass), symbology off and on, a restart (Stop / Start, reinstall: the learner relearns), a boost
//     after it; transitions on arbitrary frames.
//  X2 the first boosted frame after dormant frames is complete: every view patched and lit, the forward pass moved,
//     the overlay replays moved with records at the true eye (the learner carried through the dormant frames).
//  X3 symbology: in dormant frames the export still gets its record types (render point) for every HMD view, as in
//     today's frames; none while its views are off.
//  X4 the cost (QV_R128_BENCH=1 or with QV_R128_HW / the HW suite: printed and judged): CockpitIpdDormant 0 vs 1 at
//     scale 1, the cost line's parts per frame (median, mean), interleaved blocks; and the payload's own estimate
//     (reference frames) against it.
#pragma once

namespace r128t {

namespace ck = ::fixdepth::ckipd;
namespace oc = ::fixdepth::ckipd::ocpu;
using cktest::Get;
using cktest::Ident;
using cktest::Inv;
using cktest::Mat;
using cktest::Mul;
using cktest::Put;

uint64_t Fnv(uint64_t h, const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
  return h;
}

// The symbology export's record point (ovl_cpu.h g_symRec): records per HMD view this frame, and their types.
int g_symRecs[4] = {};
uint64_t g_ctxDigest = 0;  // DCS's context VP bytes after each replay of the frame
uint64_t g_symTypes = 0;
void SymRec(int view, int c9) {
  if (view >= 0 && view < 4) ++g_symRecs[view];
  g_symTypes = Fnv(g_symTypes, &c9, sizeof(c9));
}
void SymEnd(int) {}

// One frame's plan.
struct Plan {
  double scale = 1.0;
  bool writeBoost = true;  // the layer writes a record (false: BoostOut goes stale)
  bool noCockpit = false;  // an external view (F2 / F10): no cockpit G-buffer pass, no overlay replays
  bool symOn = true;       // the symbology export's view bits
  bool paused = false;     // the in-flight key (CockpitTrueIpdToggle)
  bool restart = false;    // Stop / Start and the replica reinstalled before this frame
};
// The frame's outcome (for X1 / X2).
struct Outcome {
  uint64_t digest = 0;
  uint64_t part[4] = {};
  float litPx[4][4] = {};  // per view: x and y of pixel (8, 8) (cockpit stencil) and (40, 8) (world)  // the digest's parts: cPerView captures, lighting targets, overlay draws, context bytes
  bool latched = false, dormant = false, eligible = false, anyPatched = false;
  double h = 0;
  bool patched[4] = {}, failed[4] = {}, lit[4] = {};
  int fwdN[4] = {}, cpuMoved[4] = {}, cpuRecT[4] = {}, cpuRecB[4] = {}, sym[4] = {};
  uint64_t symTypes = 0;
  // lightingOk: the lighting targets may differ (the hardware variant, below).
  bool SameAs(const Outcome& o, bool lightingOk = false) const {
    if (lightingOk ? part[0] != o.part[0] || part[2] != o.part[2] || part[3] != o.part[3] : digest != o.digest)
      return false;
    if (latched != o.latched || anyPatched != o.anyPatched || h != o.h || symTypes != o.symTypes)
      return false;
    for (int t = 0; t < 4; ++t)
      if (patched[t] != o.patched[t] || failed[t] != o.failed[t] || (!lightingOk && lit[t] != o.lit[t]) || fwdN[t] != o.fwdN[t] ||
          cpuMoved[t] != o.cpuMoved[t] || cpuRecT[t] != o.cpuRecT[t] || cpuRecB[t] != o.cpuRecB[t] ||
          sym[t] != o.sym[t])
        return false;
    return true;
  }
};

constexpr int kFill = 24;     // filler passes per HMD view (DCS draws ~100 other passes per frame)
constexpr int kStage = 40;    // buffer captures per frame
constexpr UINT kStageBytes = 4096;

struct Rig {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  ID3D11DeviceContext1* c = nullptr;
  ID3D11Buffer* cb[3] = {};  // DCS's cPerView buffers: A, B (0x970, plain b7), C (struct at constant 16)
  ID3D11Buffer* filler = nullptr;
  ID3D11Texture2D* dtex[4] = {};
  ID3D11DepthStencilView* dsv[4] = {};
  ID3D11ShaderResourceView* dsrv[4] = {};
  ID3D11Texture2D* defTex = nullptr;
  ID3D11DepthStencilView* defDsv = nullptr;
  ID3D11DepthStencilState* ckDss = nullptr;
  ID3D11DepthStencilState* noDepth = nullptr;
  ID3D11Texture2D* litTex = nullptr;
  ID3D11RenderTargetView* litRtv = nullptr;
  ID3D11VertexShader* litVs = nullptr;
  ID3D11PixelShader* litPs = nullptr;
  ID3D11BlendState* addBlend = nullptr;
  ID3D11Texture2D* litStage[4] = {};
  ID3D11Buffer* stage[kStage] = {};
  int stageN = 0;
  UINT stageBytes[kStage] = {};
  ID3D11Texture2D* rtTex = nullptr;  // ocpt::g_rtv's (the replica's toggles clear it)
  ocpt::Harness H;
  bool ok = false;
};
Rig* g_r = nullptr;

// What DCS's slot-19 body does in the rig: the frame buffer, the viewport, the binder's cPerView upload, b7 bound, the
// cockpit's stencil state; the GPU's view of the buffer captured as a draw would see it (a copy, no Map in the pass).
struct GbX {
  ID3D11Buffer* cb = nullptr;
  UINT first = 0, num = 0;
  const uint8_t* data = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  bool ck = false;
} g_gx;
void Cap(ID3D11Resource* src, UINT bytes) {
  Rig& r = *g_r;
  if (r.stageN >= kStage) return;
  D3D11_BOX box{0, 0, 0, std::min(bytes, kStageBytes), 1, 1};
  r.stageBytes[r.stageN] = box.right;
  r.c->CopySubresourceRegion(r.stage[r.stageN++], 0, 0, 0, 0, src, 0, &box);
}
UINT BytesOf(ID3D11Buffer* b) {
  D3D11_BUFFER_DESC d{};
  b->GetDesc(&d);
  return d.ByteWidth;
}
void Upload(ID3D11Buffer* cb, UINT first, const uint8_t* s) {
  std::vector<uint8_t> full(BytesOf(cb), 0x5a);
  memcpy(full.data() + first * 16, s, ck::kCbBytes);
  g_r->c->UpdateSubresource(cb, 0, nullptr, full.data(), 0, 0);
}
void __fastcall Exec(void*, void*) {
  Rig& r = *g_r;
  ID3D11RenderTargetView* none = nullptr;
  r.c->OMSetRenderTargets(1, &none, g_gx.dsv);
  D3D11_VIEWPORT vp{0, 0, 64, 64, 0, 1};
  r.c->RSSetViewports(1, &vp);
  Upload(g_gx.cb, g_gx.first, g_gx.data);
  cktest::BindB7(g_gx.cb, g_gx.first, g_gx.num);
  if (g_gx.ck) r.c->OMSetDepthStencilState(r.ckDss, 40);
  Cap(g_gx.cb, BytesOf(g_gx.cb));
}

bool MakeRig(Rig& r, D3D_DRIVER_TYPE drv) {
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1};
  if (FAILED(D3D11CreateDevice(nullptr, drv, nullptr, 0, want, 1, D3D11_SDK_VERSION, &r.dev, nullptr, &r.imm)))
    return false;
  r.imm->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&r.c));
  if (!r.c) return false;
  auto mkCb = [&](UINT bytes, D3D11_USAGE u = D3D11_USAGE_DEFAULT, UINT bind = D3D11_BIND_CONSTANT_BUFFER,
                  UINT cpu = 0) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = bytes;
    d.Usage = u;
    d.BindFlags = bind;
    d.CPUAccessFlags = cpu;
    ID3D11Buffer* b = nullptr;
    r.dev->CreateBuffer(&d, nullptr, &b);
    return b;
  };
  r.cb[0] = mkCb(ck::kCbBytes);
  r.cb[1] = mkCb(ck::kCbBytes);
  r.cb[2] = mkCb(176 * 16);
  r.filler = mkCb(256);
  for (int i = 0; i < kStage; ++i) r.stage[i] = mkCb(kStageBytes, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ);
  for (int i = 0; i < 4; ++i) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R24G8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    r.dev->CreateTexture2D(&td, nullptr, &r.dtex[i]);
    std::vector<uint32_t> px(64 * 64);
    for (int y = 0; y < 64; ++y)
      for (int x = 0; x < 64; ++x) px[y * 64 + x] = (x < 32 ? 0x28u : 0x10u) << 24 | 0x400000u;
    D3D11_TEXTURE2D_DESC sd = td;
    sd.BindFlags = 0;
    D3D11_SUBRESOURCE_DATA init{px.data(), 64 * 4, 0};
    ID3D11Texture2D* src = nullptr;
    r.dev->CreateTexture2D(&sd, &init, &src);
    if (src && r.dtex[i]) r.imm->CopyResource(r.dtex[i], src);
    if (src) src->Release();
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (r.dtex[i]) r.dev->CreateDepthStencilView(r.dtex[i], &dd, &r.dsv[i]);
    D3D11_SHADER_RESOURCE_VIEW_DESC rd{};
    rd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    rd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    rd.Texture2D.MipLevels = 1;
    if (r.dtex[i]) r.dev->CreateShaderResourceView(r.dtex[i], &rd, &r.dsrv[i]);
  }
  {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    r.dev->CreateTexture2D(&td, nullptr, &r.defTex);
    if (r.defTex) r.dev->CreateDepthStencilView(r.defTex, nullptr, &r.defDsv);
    D3D11_DEPTH_STENCIL_DESC d = CD3D11_DEPTH_STENCIL_DESC(D3D11_DEFAULT);
    d.StencilEnable = TRUE;
    d.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    d.BackFace = d.FrontFace;
    r.dev->CreateDepthStencilState(&d, &r.ckDss);
    D3D11_DEPTH_STENCIL_DESC nd = CD3D11_DEPTH_STENCIL_DESC(D3D11_DEFAULT);
    nd.DepthEnable = FALSE;
    nd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    r.dev->CreateDepthStencilState(&nd, &r.noDepth);
  }
  {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    r.dev->CreateTexture2D(&td, nullptr, &r.litTex);
    if (r.litTex) r.dev->CreateRenderTargetView(r.litTex, nullptr, &r.litRtv);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& s : r.litStage) r.dev->CreateTexture2D(&td, nullptr, &s);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    r.dev->CreateBlendState(&bd, &r.addBlend);
  }
  HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = dll ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dll, "D3DCompile")) : nullptr;
  {
    // The lighting: a full-screen triangle, gCameraPos.x of b7 into .x (the split's region shows which b7 lit it),
    // a count into .y; it reads the cockpit pass's depth at t3.
    static const char kSrc[] =
        "cbuffer cPerView : register(b7) { float4 pad[98]; float4 camPos; };\n"
        "Texture2D<float> depthTex : register(t3);\n"
        "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
        "  float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "}\n"
        "float4 ps(float4 pos : SV_Position) : SV_Target {\n"
        "  return float4(camPos.x + 0 * depthTex.Load(int3(pos.xy, 0)), 1, camPos.z, 0);\n"
        "}\n";
    ID3DBlob* b[2] = {};
    if (compile) {
      compile(kSrc, sizeof(kSrc) - 1, "lit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &b[0], nullptr);
      compile(kSrc, sizeof(kSrc) - 1, "lit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &b[1], nullptr);
    }
    if (b[0]) r.dev->CreateVertexShader(b[0]->GetBufferPointer(), b[0]->GetBufferSize(), nullptr, &r.litVs);
    if (b[1]) r.dev->CreatePixelShader(b[1]->GetBufferPointer(), b[1]->GetBufferSize(), nullptr, &r.litPs);
    for (ID3DBlob* x : b)
      if (x) x->Release();
  }
  {
    // The replica's ILS-like symbology vertex shader with stream output (ovl_cpu_test.h).
    static const char kSrc[] =
        "cbuffer cGlobals : register(b0) { float4x4 matWorldViewProj; float4x4 matWorld; float4 other[8]; };\n"
        "float4 vs(uint id : SV_VertexID) : SV_Position { return mul(float4(0, 0, 0, 1), matWorldViewProj); }\n";
    ID3DBlob* b = nullptr;
    if (compile) compile(kSrc, sizeof(kSrc) - 1, "ils", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &b, nullptr);
    if (b) {
      r.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &r.H.vs);
      D3D11_SO_DECLARATION_ENTRY e = {0, "SV_Position", 0, 0, 4, 0};
      UINT stride = 16;
      r.dev->CreateGeometryShaderWithStreamOutput(b->GetBufferPointer(), b->GetBufferSize(), &e, 1, &stride, 1,
                                                  D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &r.H.gs);
      b->Release();
    }
  }
  ocpt::g_so = mkCb(16 * 16384, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT);
  ocpt::g_soStage = mkCb(16 * 16384, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ);
  for (int i = 0; i < 16; ++i) {
    ocpt::g_fx[i].cb = mkCb(256);
    ocpt::g_fxDyn[i].cb = mkCb(256, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE);
    ocpt::g_fxDyn[i].dyn = true;
  }
  {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 16;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    r.dev->CreateTexture2D(&td, nullptr, &r.rtTex);
    if (r.rtTex) r.dev->CreateRenderTargetView(r.rtTex, nullptr, &ocpt::g_rtv);
  }
  bool ok = r.cb[0] && r.cb[1] && r.cb[2] && r.filler && r.defDsv && r.ckDss && r.noDepth && r.litRtv && r.litVs &&
            r.litPs && r.addBlend && r.H.vs && r.H.gs && ocpt::g_so && ocpt::g_soStage && ocpt::g_rtv;
  for (int i = 0; i < kStage; ++i) ok = ok && r.stage[i];
  for (int i = 0; i < 4; ++i) ok = ok && r.dsv[i] && r.dsrv[i] && r.litStage[i];
  for (int i = 0; i < 16; ++i) ok = ok && ocpt::g_fx[i].cb && ocpt::g_fxDyn[i].cb;
  r.ok = ok;
  return ok;
}
ULONG ReleaseRig(Rig& r) {
  if (!r.dev) return 0;
  r.c->ClearState();
  r.c->Flush();
  auto rel = [](auto*& p) {
    if (p) p->Release();
    p = nullptr;
  };
  for (auto& b : r.cb) rel(b);
  rel(r.filler);
  for (auto& b : r.stage) rel(b);
  for (int i = 0; i < 4; ++i) rel(r.dtex[i]), rel(r.dsv[i]), rel(r.dsrv[i]), rel(r.litStage[i]);
  rel(r.defTex), rel(r.defDsv), rel(r.ckDss), rel(r.noDepth), rel(r.litTex), rel(r.litRtv), rel(r.litVs), rel(r.litPs);
  rel(r.addBlend), rel(r.rtTex), rel(r.H.vs), rel(r.H.gs);
  rel(ocpt::g_so), rel(ocpt::g_soStage), rel(ocpt::g_rtv);
  for (int i = 0; i < 16; ++i) rel(ocpt::g_fx[i].cb), rel(ocpt::g_fxDyn[i].cb);
  if (fixdepth::g_tinyCb && fixdepth::g_tinyDev == r.dev) {  // the table normalizer's private buffer on our device
    fixdepth::g_tinyCb->Release();
    fixdepth::g_tinyCb = nullptr;
    fixdepth::g_tinyDev = nullptr;
  }
  if (fixdepth::g_passDsv) rel(fixdepth::g_passDsv);  // the last G-buffer pass's pick (released at the next one)
  if (fixdepth::g_passMv) rel(fixdepth::g_passMv);
  rel(r.c), rel(r.imm);
  const ULONG last = r.dev->Release();
  r.dev = nullptr;
  return last;
}

// The module and the scene, per run.
struct Seq {
  cktest::Graph gr;
  cktest::GbPass gbp[4][3];
  fdtest::FakePass casc = fdtest::MakePass("CascadeShadowPass0"), lights = fdtest::MakePass("LightsBuffersPass"),
                   lighting = fdtest::MakePass("GBufferLightingPass"), tc = fdtest::MakePass("TransparentCockpitPass"),
                   hmd = fdtest::MakePass("CopyToHMDPass");
  fdtest::FakePass fill[kFill];
  ocpt::FnObj* fn = nullptr;
  ID3D11Buffer* bufOf[4] = {};
  UINT firstOf[4] = {0, 0, 0, 16}, numOf[4] = {0, 0, 0, 160};
  Mat prevVP[4];
  bool havePrev[4] = {};
  psh_BoostOut* bview = nullptr;
  uint32_t layerFrame = 0;
  int frameIdx = 0;
  Seq() {
    int idx[4][3];
    for (int t = 0; t < 4; ++t) {
      idx[t][0] = gr.Add(gbrec::kSmCockpit, 600);
      idx[t][1] = gr.Add(gbrec::kSmGbuffer, 700 + t);
      idx[t][2] = gr.Add(gbrec::kSmDecal, 700 + t);
    }
    gr.Finish();
    for (int t = 0; t < 4; ++t)
      for (int k = 0; k < 3; ++k) gbp[t][k] = gr.Pass(idx[t][k]);
    static const char* const kNames[] = {"ForestPass",   "TreesPass",      "WaterSeaPass",  "RiversPass",
                                         "CloudsPass",   "SkyPass",        "ParticlesPass", "TerrainDecals",
                                         "VolumetricFog", "LensFlarePass", "BloomPass",     "ExposurePass"};
    for (int i = 0; i < kFill; ++i) {
      char n[40];
      snprintf(n, sizeof(n), "%s%d", kNames[i % 12], i / 12);
      fill[i] = fdtest::MakePass(n);
    }
  }
};

double g_base = 0.0655;
void WriteBoost(Seq& u, double scale) {
  if (!u.bview) return;
  ++u.layerFrame;
  psh_BoostOut b{};
  b.displayTime = 1000 + u.layerFrame;
  b.frame = u.layerFrame;
  b.scale = static_cast<float>(scale);
  b.baseIpdM = static_cast<float>(g_base);
  b.renderedIpdM = static_cast<float>(g_base * scale);
  b.flags = cktest::psh_enabled() | (std::fabs(scale - 1) >= 1e-3 ? cktest::psh_applied() : 0);
  ::dcsvr_perception::seqlock_write(u.bview, b);
}

void Ob(void* p, bool begin) { fixdepth::OnBoundary(p, begin, 0); }

// One frame of the sequence (the frame's end comes at the next frame's cascade boundary).
Outcome RunFrame(Seq& u, const Plan& p, bool record) {
  Rig& r = *g_r;
  ++ocpt::g_frameNo;
  ocpt::g_log.clear();
  r.stageN = 0;
  memset(g_symRecs, 0, sizeof(g_symRecs));
  g_ctxDigest = 0;
  g_symTypes = 0;
  oc::g_symViews = 0;
  ck::g_userOff = p.paused;
  // The head as the replica moves it.
  const bool still = (ocpt::g_frameNo % 37) < 6;
  if (!still) {
    ocpt::g_yaw += 0.35 * std::sin(ocpt::g_frameNo * 0.05);
    ocpt::g_px += 0.0004 * std::cos(ocpt::g_frameNo * 0.03);
  }
  cktest::Rot(ocpt::g_yaw, 2, 0, ocpt::g_HR);
  ocpt::g_H[0] = 0.1 + ocpt::g_px, ocpt::g_H[1] = 0.6, ocpt::g_H[2] = -0.2;
  ocpt::g_cockpit[0] = 0.1, ocpt::g_cockpit[1] = 0.65, ocpt::g_cockpit[2] = -0.15;
  ocpt::g_noisyEyeRel = ocpt::R01() < 0.5;
  if (ocpt::g_frameNo % 2 == 0) ++ocpt::g_mfdPage;
  const double boosted = g_base * p.scale, h = (boosted - g_base) / 2;
  const double fovs[4][4] = {{-1.3, 1.0, 1.1, -1.2}, {-1.0, 1.3, 1.1, -1.2}, {-0.6, 0.5, 0.55, -0.6},
                             {-0.5, 0.6, 0.55, -0.6}};
  for (int t = 0; t < 4; ++t) {
    ocpt::ViewScene& v = ocpt::g_vs[t];
    const double sg = (t & 1) ? 1.0 : -1.0;
    for (int i = 0; i < 3; ++i) v.E[i] = ocpt::g_H[i] + sg * boosted / 2 * ocpt::g_HR[0][i];
    for (int i = 0; i < 3; ++i) v.dw[i] = -sg * h * ocpt::g_HR[0][i];
    v.V = Inv(cktest::ViewInvOf(ocpt::g_HR, v.E));
    v.P = cktest::ProjOf(fovs[t], 0, 0);
    alignas(16) uint8_t tmp[0x300];
    ocpt::SetCameraCtx(tmp, v.V, v.P);
    v.VPf = ocpt::GetVp(tmp);
    ck::M4 m;
    memcpy(&m, &v.VPf, sizeof(m));
    const ck::M4 mt = oc::TrueVp(m, v.dw);
    float f[16];
    oc::ToFloats(mt, f, false);
    uint8_t fb[64];
    memcpy(fb, f, 64);
    v.VPtrueF = Get(fb, 0);
  }
  ocpt::FrameOpts fo;
  for (int t = 0; t < 4; ++t) ocpt::g_list[t] = ocpt::BuildList(t, fo);
  ocpt::PackRecords(r.H.handler);
  if (p.writeBoost) WriteBoost(u, p.scale);
  ck::NoteCullSep(boosted);  // the culling call: this frame's eyes, before its passes
  Ob(&u.casc, true);         // the previous frame's end; BoostOut polled
  Ob(&u.casc, false);
  alignas(16) uint8_t gs[4][ck::kCbBytes];
  bool soBound = false;
  for (int t = 0; t < 4; ++t) {
    const ocpt::ViewScene& v = ocpt::g_vs[t];
    // DCS's cockpit G-buffer camera: the context's camera; the focus views jittered (DLSS).
    const Mat Pg = t >= 2 ? cktest::ProjOf(fovs[t], 0.0007, -0.0004) : v.P;
    const Mat VI = Inv(v.V);
    const Mat pvp = u.havePrev[t] ? u.prevVP[t] : Mul(v.V, Pg);
    cktest::BuildStruct(gs[t], VI, Pg, pvp, 100 + t);
    u.prevVP[t] = Get(gs[t], ck::kOffViewProj);
    u.havePrev[t] = true;
    Ob(&u.lights, true);
    Ob(&u.lights, false);
    {
      ID3D11RenderTargetView* none = nullptr;
      r.c->OMSetRenderTargets(1, &none, r.defDsv);
    }
    for (int k = 0; k < 3; ++k) {
      if (k == 0 && p.noCockpit) continue;
      Ob(&u.gbp[t][k], true);
      g_gx = GbX{u.bufOf[t], u.firstOf[t], u.numOf[t], gs[t], r.dsv[t], k == 0};
      gbpass::Hook(&u.gbp[t][k], u.gr.ctxBlock);
      Cap(u.bufOf[t], BytesOf(u.bufOf[t]));
      Ob(&u.gbp[t][k], false);
      if (k == 0) r.c->OMSetDepthStencilState(nullptr, 0);
    }
    for (int i = 0; i < kFill; ++i) {  // other passes: one upload each into a buffer no one learns
      Ob(&u.fill[i], true);
      const float val[4] = {static_cast<float>(i), static_cast<float>(t), 0, 1};
      std::vector<float> fb(64, 0.0f);
      memcpy(fb.data(), val, sizeof(val));
      r.c->UpdateSubresource(r.filler, 0, nullptr, fb.data(), 0, 0);
      Ob(&u.fill[i], false);
    }
    // GBufferLightingPass: DCS's state, the binder's upload, one full-screen draw reading the cockpit depth.
    Ob(&u.lighting, true);
    {
      const float zero[4] = {0, 0, 0, 0};
      r.c->ClearRenderTargetView(r.litRtv, zero);
      r.c->OMSetRenderTargets(1, &r.litRtv, nullptr);
      D3D11_VIEWPORT vp{0, 0, 64, 64, 0, 1};
      r.c->RSSetViewports(1, &vp);
      r.c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      r.c->IASetInputLayout(nullptr);
      r.c->VSSetShader(r.litVs, nullptr, 0);
      r.c->GSSetShader(nullptr, nullptr, 0);
      r.c->PSSetShader(r.litPs, nullptr, 0);
      r.c->PSSetShaderResources(3, 1, &r.dsrv[t]);
      r.c->OMSetBlendState(r.addBlend, nullptr, 0xffffffff);
      r.c->OMSetDepthStencilState(r.noDepth, 0);
      Upload(u.bufOf[t], u.firstOf[t], gs[t]);
      cktest::BindB7(u.bufOf[t], u.firstOf[t], u.numOf[t]);
      r.c->Draw(3, 0);
    }
    Ob(&u.lighting, false);
    {
      ID3D11ShaderResourceView* nullSrv = nullptr;
      r.c->PSSetShaderResources(3, 1, &nullSrv);
      r.c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
      r.c->OMSetDepthStencilState(nullptr, 0);
      r.c->CopyResource(r.litStage[t], r.litTex);
    }
    // TransparentCockpitPass: its binder's upload.
    Ob(&u.tc, true);
    Upload(u.bufOf[t], u.firstOf[t], gs[t]);
    cktest::BindB7(u.bufOf[t], u.firstOf[t], u.numOf[t]);
    Cap(u.bufOf[t], BytesOf(u.bufOf[t]));
    Ob(&u.tc, false);
    // The overlay passes after DLSS: the context's camera, updateBuffers' b7, the record replays (render point).
    if (!p.noCockpit) {
      r.c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
      r.c->IASetInputLayout(nullptr);
      r.c->VSSetShader(r.H.vs, nullptr, 0);
      r.c->GSSetShader(r.H.gs, nullptr, 0);
      r.c->PSSetShader(nullptr, nullptr, 0);
      const UINT off = soBound ? static_cast<UINT>(-1) : 0;
      r.c->SOSetTargets(1, &ocpt::g_so, &off);
      soBound = true;
      ocpt::g_curCtx = ocpt::g_ctxA;
      ocpt::SetCameraCtx(ocpt::g_ctxA, v.V, v.P);
      fdtest::FakePass* passes[3] = {&r.H.hdr, &r.H.ldr, &r.H.gui};
      for (int pass = 0; pass < 3; ++pass) {
        Ob(passes[pass], true);
        {
          alignas(16) uint8_t pv[ck::kCbBytes] = {};
          memcpy(pv + ck::kOffView, ocpt::g_curCtx + 0xf4, 64);
          memcpy(pv + ck::kOffViewInv, ocpt::g_curCtx + 0x1f4, 64);
          memcpy(pv + ck::kOffProj, ocpt::g_curCtx + 0x134, 64);
          memcpy(pv + ck::kOffProjInv, ocpt::g_curCtx + 0x234, 64);
          Put(pv, ck::kOffViewProj, ocpt::GetVp(ocpt::g_curCtx));
          Upload(u.bufOf[t], u.firstOf[t], pv);
          cktest::BindB7(u.bufOf[t], u.firstOf[t], u.numOf[t]);
        }
        if (ocpt::R01() < 0.15) ocpt::ToggleBehindOurBack();  // between the binder and the first record
        if (pass < 2) {
          ocpt::g_togglePlan = ocpt::R01() < 0.3 ? static_cast<int>(ocpt::R01() * 8) : -1;
          if (p.symOn && pass == 0) oc::g_symViews |= 1u << t;  // the export's bit, set at its view's HDR pass begin
          ocpt::Replay(t, u.fn);
          ocpt::g_togglePlan = -1;
          if (record) {  // DCS's bytes back in the context after the replay
            const uint8_t* vp = ocpt::g_curCtx + 0x1b4;
            g_ctxDigest = Fnv(g_ctxDigest, vp, 64);
          }
          if (pass == 0) {  // a world label: world-projected with the context's camera
            Mat W = Ident();
            W.a[3][0] = ocpt::g_H[0] + 900, W.a[3][1] = ocpt::g_H[1] + 50, W.a[3][2] = ocpt::g_H[2] + 2500;
            ocpt::Effect& e = ocpt::g_fx[13];
            Put(e.cpu, 0, Mul(W, ocpt::GetVp(ocpt::g_curCtx)));
            Put(e.cpu, 64, W);
            e.dirty = true;
            ocpt::Apply(e);
            ocpt::DrawOne(t, 1000, -1, W, nullptr);
          }
        } else {  // GUIPass: 2D text, no camera
          Mat M = Ident();
          M.a[3][0] = 0.25, M.a[3][1] = -0.5;
          ocpt::Effect& e = ocpt::g_fx[12];
          Put(e.cpu, 0, M);
          e.dirty = true;
          ocpt::Apply(e);
          ocpt::DrawOne(t, 2000, -2, M, nullptr);
        }
        Ob(passes[pass], false);
      }
      ID3D11Buffer* none = nullptr;
      const UINT z = 0;
      r.c->SOSetTargets(1, &none, &z);
      r.c->GSSetShader(nullptr, nullptr, 0);
    }
    Ob(&u.hmd, true);
    Ob(&u.hmd, false);
  }
  ++u.frameIdx;
  Outcome o;
  if (!record) return o;
  // The frame's outcome before its end (EndFrame resets it at the next cascade boundary).
  const ck::Frame& f = ck::g_f;
  o.latched = f.latched;
  o.dormant = f.dormant;
  o.eligible = f.dormEligible;
  o.anyPatched = f.anyPatched;
  o.h = f.h;
  for (int t = 0; t < 4; ++t) {
    o.patched[t] = f.v[t].patched;
    o.failed[t] = f.v[t].failed;
    o.lit[t] = f.v[t].lit;
    o.fwdN[t] = f.v[t].fwdN;
    o.cpuMoved[t] = f.v[t].cpuMoved;
    o.cpuRecT[t] = f.v[t].cpuRecT;
    o.cpuRecB[t] = f.v[t].cpuRecB;
    o.sym[t] = g_symRecs[t];
  }
  o.symTypes = g_symTypes;
  // The digest: the captures, the lighting targets, every overlay draw, DCS's context bytes.
  uint64_t d = 0xcbf29ce484222325ull;
  for (int i = 0; i < r.stageN; ++i) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(r.c->Map(r.stage[i], 0, D3D11_MAP_READ, 0, &m))) {
      d = Fnv(d, m.pData, r.stageBytes[i]);
      o.part[0] = Fnv(o.part[0] ? o.part[0] : 1, m.pData, r.stageBytes[i]);
      r.c->Unmap(r.stage[i], 0);
    }
  }
  for (int t = 0; t < 4; ++t) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(r.c->Map(r.litStage[t], 0, D3D11_MAP_READ, 0, &m))) {
      {
        const float* row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(m.pData) + 8 * m.RowPitch);
        o.litPx[t][0] = row[8 * 4], o.litPx[t][1] = row[8 * 4 + 1], o.litPx[t][2] = row[40 * 4], o.litPx[t][3] = row[40 * 4 + 1];
      }
      for (int y = 0; y < 64; ++y) {
        d = Fnv(d, static_cast<const uint8_t*>(m.pData) + y * m.RowPitch, 64 * 16);
        o.part[1] = Fnv(o.part[1] ? o.part[1] : 1, static_cast<const uint8_t*>(m.pData) + y * m.RowPitch, 64 * 16);
      }
      r.c->Unmap(r.litStage[t], 0);
    }
  }
  const std::vector<std::array<float, 4>> draws = ocpt::ReadDraws();
  for (const auto& a : draws) d = Fnv(d, a.data(), 16), o.part[2] = Fnv(o.part[2] ? o.part[2] : 1, a.data(), 16);
  const uint64_t nd = draws.size();
  d = Fnv(d, &nd, sizeof(nd));
  d = Fnv(d, &g_ctxDigest, sizeof(g_ctxDigest));
  o.part[3] = g_ctxDigest;
  o.digest = d;
  return o;
}

// The module on for a run: CockpitTrueIpd=1 as shipped (lighting, forward, overlays on the cpu path, helmet,
// ownship stage 1), dormant per key; the replica's slots hooked (render point).
void StartModule(Seq& u, bool dormant) {
  ck::Configure(1, true, true, 0, 0, false, 0, true, true, 0, 0, std::wstring(), false, 0x0C, ck::kOvlPathCpu);
  ck::g_cfgHelmet = true;
  ck::SetOwnship(1);
  ck::SetDormant(dormant);
  ck::g_userOff = false;
  // The globals ResetRender keeps (the culling call's sample count and its precision): as at a payload start.
  ck::g_cullSeqLatched = ck::g_cullSeq.load();
  ck::g_cullDevEma = ck::g_cullDevMax = 0;
  ck::g_cullDevN = 0;
  ck::g_fault = false;
  fixdepth::g_fault = false;
  ck::Start();
  ocpt::g_handlerVt[3] = reinterpret_cast<void*>(&ocpt::ReplayImpl);
  ocpt::g_visVt[2] = reinterpret_cast<void*>(&ocpt::VisCall);
  ocpt::g_compVt[2] = reinterpret_cast<void*>(&ocpt::CompCall);
  ocpt::g_unkVt[2] = reinterpret_cast<void*>(&ocpt::UnkCall);
  ocpt::g_renderIat[0] = reinterpret_cast<void*>(&ocpt::RenderImpl);
  oc::InstallAt(&ocpt::g_handlerVt[3], &ocpt::g_visVt[2], &ocpt::g_compVt[2], &ocpt::g_curCtx, ocpt::g_visVt, false,
                ocpt::g_renderIat, nullptr);
  oc::g_symRec = &SymRec;
  oc::g_symReplayEnd = &SymEnd;
  (void)u;
}
void StopModule() {
  oc::g_symRec = nullptr;
  oc::g_symReplayEnd = nullptr;
  oc::g_symViews = 0;
  ck::Stop();  // the replica's slots back too (ocpu::Uninstall)
}

// A run of the sequence from a clean state: two frames with the module off (DCS's own frames: the pass callback's
// view counts settle), the scene reset, the module on.
std::vector<Outcome> RunSequence(Seq& u, bool dormant, const std::vector<Plan>& plan) {
  std::vector<Outcome> out;
  ocpt::ResetScene();
  for (int i = 0; i < 2; ++i) RunFrame(u, Plan{}, false);
  ocpt::ResetScene();
  u.layerFrame = 0;
  if (u.bview) memset(static_cast<void*>(u.bview), 0, sizeof(*u.bview));
  for (bool& b : u.havePrev) b = false;
  // fixation_depth.h's learnt "normal" upload entries of the context table: from an earlier test's device they
  // would make the first run's toggle checks differ from the second's (a payload start has none).
  fixdepth::g_usNormal = fixdepth::g_us1Normal = nullptr;
  StartModule(u, dormant);
  for (const Plan& p : plan) {
    if (p.restart) {
      StopModule();
      StartModule(u, dormant);
    }
    out.push_back(RunFrame(u, p, true));
  }
  Ob(&u.casc, true);  // the last frame's end
  Ob(&u.casc, false);
  StopModule();
  return out;
}

// The sequence (frame index -> plan): transitions on arbitrary frames.
std::vector<Plan> Sequence() {
  std::vector<Plan> s;
  auto ramp = [](int f, int a, int b, double x, double y) { return x + (y - x) * (f - a) / static_cast<double>(b - a); };
  for (int f = 0; f < 520; ++f) {
    Plan p;
    if (f < 151) p.scale = 1.0;                                    // the learner commits at h 0
    else if (f < 155) p.scale = ramp(f, 151, 155, 1.0, 1.1);       // gaze to the far field: a ramp
    else if (f < 203) p.scale = 1.1;
    else if (f < 213) p.scale = ramp(f, 203, 213, 1.1, 1.25);
    else if (f < 262) p.scale = 1.25;
    else if (f < 279) p.scale = ramp(f, 262, 279, 1.25, 1.0);      // gaze lost / near: back to 1
    else if (f < 331) p.scale = 1.0;
    else if (f == 331) p.scale = 1.1;                              // a single boosted frame (a step)
    else if (f < 377) p.scale = 1.0;
    else if (f < 380) p.scale = 1.25;                              // three stepped frames
    else if (f < 437) p.scale = 1.0;
    else if (f < 447) p.scale = ramp(f, 437, 447, 1.0, 1.25);      // after the restart (frame 400)
    else if (f < 497) p.scale = 1.25;
    else p.scale = ramp(f, 497, 520, 1.25, 1.0);
    p.symOn = !(f >= 97 && f < 121);                               // the symbology export's views off and on
    p.paused = f >= 229 && f < 241;                                // the in-flight key while boosted
    p.writeBoost = !(f >= 291 && f < 327);                         // BoostOut stale (no record), scale 1
    p.noCockpit = f >= 341 && f < 352;                             // an external view (F2 / F10)
    p.restart = f == 400;                                          // Stop / Start, the learner reset
    s.push_back(p);
  }
  return s;
}

// X4: the cost line's parts per frame at scale 1 (key 0 vs 1), interleaved blocks.
void Bench(Seq& u, const char* tag) {
  const double tps = ck::TscPerSec();
  std::vector<double> c[2];
  double part[2][ck::kCostParts] = {};  // per part, summed (us)
  ocpt::ResetScene();
  for (int i = 0; i < 2; ++i) RunFrame(u, Plan{}, false);
  ocpt::ResetScene();
  u.layerFrame = 0;
  if (u.bview) memset(static_cast<void*>(u.bview), 0, sizeof(*u.bview));
  for (bool& b : u.havePrev) b = false;
  StartModule(u, true);
  for (int i = 0; i < 40; ++i) RunFrame(u, Plan{}, false);  // warm: the learner's first commits
  constexpr int kBlocks = 8, kPer = 60;
  for (int blk = 0; blk < kBlocks; ++blk) {
    const int key = blk & 1;
    ck::SetDormant(key != 0);
    for (int i = 0; i < kPer; ++i) {
      uint64_t a = 0, b = 0, pa[ck::kCostParts];
      for (int k = 0; k < ck::kCostParts; ++k) a += pa[k] = ck::g_cost[k].load();
      RunFrame(u, Plan{}, false);
      for (int k = 0; k < ck::kCostParts; ++k) b += ck::g_cost[k].load();
      if (i >= 2) {
        c[key].push_back((b - a) * 1e6 / tps);
        for (int k = 0; k < ck::kCostParts; ++k) part[key][k] += (ck::g_cost[k].load() - pa[k]) * 1e6 / tps;
      }  // (the frame end at the next cascade: a 1-frame lag)
    }
  }
  const double estDorm = ck::g_dormCostN.load() ? ck::g_dormCostTicks.load() * 1e6 / tps / ck::g_dormCostN.load() : 0;
  const double estFull = ck::g_fullCostN.load() ? ck::g_fullCostTicks.load() * 1e6 / tps / ck::g_fullCostN.load() : 0;
  const uint64_t estDn = ck::g_dormCostN.load(), estFn = ck::g_fullCostN.load();
  StopModule();
  auto med = [](std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
  };
  auto mean = [](const std::vector<double>& v) {
    double s = 0;
    for (double x : v) s += x;
    return v.empty() ? 0.0 : s / v.size();
  };
  const double m0 = med(c[0]), m1 = med(c[1]), a0 = mean(c[0]), a1 = mean(c[1]);
  printf("     (R128 cost %s, scale 1, cockpit ipd's own scopes per frame: CockpitIpdDormant=0 median %.1f us mean %.1f "
         "us (n %zu); =1 median %.1f us mean %.1f us (n %zu): saved median %.1f us (%.0f %%), mean %.1f us; the "
         "payload's estimate: full %.1f us (n %llu) dormant %.1f us (n %llu))\n",
         tag, m0, a0, c[0].size(), m1, a1, c[1].size(), m0 - m1, m0 > 0 ? 100.0 * (m0 - m1) / m0 : 0.0, a0 - a1, estFull,
         static_cast<unsigned long long>(estFn), estDorm, static_cast<unsigned long long>(estDn));
  for (int key = 0; key < 2; ++key) {
    const double n = c[key].empty() ? 1.0 : static_cast<double>(c[key].size());
    printf("       key %d per part (mean us): uploads %.2f, draw hooks %.2f, G-buffer observer %.2f, pass boundaries %.2f, "
           "cpu overlay path %.2f\n",
           key, part[key][ck::kCostUpload] / n, part[key][ck::kCostDraw] / n, part[key][ck::kCostObserver] / n,
           part[key][ck::kCostBoundary] / n, part[key][ck::kCostOvlCpu] / n);
  }
  Check(m1 < m0 && a1 < a0 && estDn > 0 && estFn > 0,
        "R128 X4: at scale 1 dormant frames cost less of cockpit ipd's render-thread time than the full work (median "
        "and mean, interleaved blocks); the payload's own estimate has both kinds of frames");
}

void Test(D3D_DRIVER_TYPE drv, bool hw, bool bench) {
  const char* tag = hw ? "hardware" : "WARP";
  Rig r;
  g_r = &r;
  if (!MakeRig(r, drv)) {
    if (hw) {
      QvSkip("R128 (hardware): no D3D11.1 hardware device\n");
    } else {
      Check(false, "R128 rig: WARP device, cPerView buffers, D24S8 depth, lighting and symbology shaders");
    }
    ReleaseRig(r);
    g_r = nullptr;
    return;
  }
  // The module: only cockpit ipd (no cone, grid, far depth or mask), DCS's slot-19 body replaced by the rig's.
  const int oldGbState = gbpass::g_state;
  const gbpass::ExecFn oldOrig = gbpass::g_orig;
  const gbpass::WrapFn oldWrap = gbpass::g_wrap.load();
  ID3D11Device* oldDev = shadowinst::g_device;
  ID3D11DeviceContext* oldCtx = fixdepth::g_s.ctx;
  ID3D11DeviceContext1* oldCtx1 = fixdepth::g_s.ctx1;
  const bool oldInstalled = fixdepth::g_installed.load(), oldCkWith = fixdepth::g_ckWith;
  const std::wstring oldGaze = fixdepth::g_gazeName;
  ocpt::Harness* oldH = ocpt::g_h;
  ID3D11DeviceContext1* oldOc = ocpt::g_c;
  ID3D11DeviceContext1* oldCk = cktest::g_c;
  wchar_t gname[64], bname[64], aname[64];
  swprintf_s(gname, L"Local\\R128TestGaze_%lu_%d", GetCurrentProcessId(), hw ? 1 : 0);
  swprintf_s(bname, L"Local\\R128TestBoost_%lu_%d", GetCurrentProcessId(), hw ? 1 : 0);
  swprintf_s(aname, L"Local\\R128TestAck_%lu_%d", GetCurrentProcessId(), hw ? 1 : 0);
  fixdepth::g_gazeName = gname;
  ck::g_boostNameOverride = bname;
  ck::g_ackNameOverride = aname;
  HANDLE bm = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, bname);
  Seq u;
  u.bview = bm ? static_cast<psh_BoostOut*>(MapViewOfFile(bm, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(psh_BoostOut))) : nullptr;
  u.bufOf[0] = r.cb[0], u.bufOf[1] = r.cb[1], u.bufOf[2] = r.cb[0], u.bufOf[3] = r.cb[2];
  u.fn = &r.H.fnNull;  // DCS 2.9.30's HMD overlay replays: an empty std::function (the render point decides)
  r.H.fnNull.impl = nullptr;
  r.H.handler.composite = nullptr;
  ocpt::g_h = &r.H;
  ocpt::g_handlerVt[3] = reinterpret_cast<void*>(&ocpt::ReplayImpl);  // DCS's own slots (the module-off frames)
  ocpt::g_visVt[2] = reinterpret_cast<void*>(&ocpt::VisCall);
  ocpt::g_compVt[2] = reinterpret_cast<void*>(&ocpt::CompCall);
  ocpt::g_renderIat[0] = reinterpret_cast<void*>(&ocpt::RenderImpl);
  ocpt::g_c = r.c;
  cktest::g_c = r.c;
  ocpt::g_fxMode = ocpt::kFxDirty;
  gbpass::g_wrap = nullptr;
  gbpass::g_state = 1;  // "hooked" (no GraphicsCore here): the rig's slot-19 body
  gbpass::g_orig = &Exec;
  shadowinst::g_device = r.dev;
  fixdepth::g_s.ctx = r.imm;
  fixdepth::g_s.ctx1 = r.c;
  ptiming::g_topTid = GetCurrentThreadId();
  fixdepth::g_coneOn = false;
  fixdepth::g_gridOn = false;
  fdexport::g_on = false;
  fixdepth::g_fault = false;
  fixdepth::g_installed = true;
  fixdepth::g_ckWith = true;

  if (!bench) {
    const std::vector<Plan> plan = Sequence();
    const uint64_t c0 = oc::g_learn.commits;  // (a session counter: per run below)
    const std::vector<Outcome> a = RunSequence(u, false, plan);
    const uint64_t keysTa = oc::g_learn.keysT, keysBa = oc::g_learn.keysB, commitsA = oc::g_learn.commits - c0;
    const uint64_t c1 = oc::g_learn.commits;
    // QV_R128_AA=1: both runs with the key 0 (the rig's own determinism on this device).
    const std::vector<Outcome> b = RunSequence(u, getenv("QV_R128_AA") == nullptr, plan);
    const uint64_t keysTb = oc::g_learn.keysT, keysBb = oc::g_learn.keysB, commitsB = oc::g_learn.commits - c1;
    // X1
    int lightTiming = 0;
    int diff = 0, firstDiff = -1, dormA = 0, dormB = 0, eligB = 0, toDorm = 0, toActive = 0, boosted = 0, patched = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
      if (!a[i].SameAs(b[i])) {
        // The hardware variant: the driver submits on its own schedule, and the table toggle that follows can send
        // a lighting pass's cPerView upload past the hook (that view lit from DCS's eye: live lightMiss). An A/A run
        // (QV_R128_AA=1: the key 0 twice) shows it on this PC's driver in 1-3 boosted frames per 520. Such a frame,
        // dormant in neither run (nothing of the mode runs in it), differing in its lighting target alone, is
        // counted apart; any other difference fails.
        if (hw && !a[i].dormant && !b[i].dormant && a[i].h != 0 && a[i].SameAs(b[i], true)) {
          ++lightTiming;
        } else {
          ++diff;
          if (firstDiff < 0) firstDiff = static_cast<int>(i);
        }
      }
      dormA += a[i].dormant;
      dormB += b[i].dormant;
      eligB += b[i].eligible;
      if (i > 0 && b[i].latched && b[i - 1].latched && b[i].eligible != b[i - 1].eligible)
        (b[i].eligible ? toDorm : toActive)++;
      boosted += b[i].h != 0;
      patched += b[i].anyPatched;
    }
    if (firstDiff >= 0) {
      const Outcome &x = a[static_cast<size_t>(firstDiff)], &y = b[static_cast<size_t>(firstDiff)];
      printf("     (R128 %s: first difference at frame %d: digest %016llx / %016llx, h %.4f / %.4f mm, dormant %d / %d, "
             "patched %d%d%d%d / %d%d%d%d, moved %d %d %d %d / %d %d %d %d, sym %d / %d; differing: captures %d lighting %d draws %d context %d)\n",
             tag, firstDiff, static_cast<unsigned long long>(x.digest), static_cast<unsigned long long>(y.digest),
             x.h * 1e3, y.h * 1e3, x.dormant, y.dormant, x.patched[0], x.patched[1], x.patched[2], x.patched[3],
             y.patched[0], y.patched[1], y.patched[2], y.patched[3], x.cpuMoved[0], x.cpuMoved[1], x.cpuMoved[2],
             x.cpuMoved[3], y.cpuMoved[0], y.cpuMoved[1], y.cpuMoved[2], y.cpuMoved[3], x.sym[0], y.sym[0],
             x.part[0] != y.part[0], x.part[1] != y.part[1], x.part[2] != y.part[2], x.part[3] != y.part[3]);
      for (int t = 0; t < 4; ++t)
        printf("       view %d lighting (8,8) %.9g n %g / %.9g n %g, (40,8) %.9g n %g / %.9g n %g\n", t, x.litPx[t][0],
               x.litPx[t][1], y.litPx[t][0], y.litPx[t][1], x.litPx[t][2], x.litPx[t][3], y.litPx[t][2], y.litPx[t][3]);
    }
    printf("     (R128 %s: %zu frames; boosted %d, patched %d; CockpitIpdDormant=1: eligible %d, dormant %d (key 0: %d), "
           "transitions to dormant %d, to active %d; learner T/B keys %llu/%llu, commits %llu (key 0: %llu/%llu, "
           "%llu); differing frames %d; boosted frames whose lighting alone differs (hardware driver timing) %d)\n",
           tag, b.size(), boosted, patched, eligB, dormB, dormA, toDorm, toActive,
           static_cast<unsigned long long>(keysTb), static_cast<unsigned long long>(keysBb),
           static_cast<unsigned long long>(commitsB), static_cast<unsigned long long>(keysTa),
           static_cast<unsigned long long>(keysBa), static_cast<unsigned long long>(commitsA), diff, lightTiming);
    char what[512];
    snprintf(what, sizeof(what),
             "R128 X1 (%s): CockpitIpdDormant=0 vs 1 over %zu frames (scale 1 -> 1.1 -> 1.25 -> 1 ramps and steps, "
             "pause, stale BoostOut, external view, symbology off / on, a restart): every frame's GPU bytes (cPerView "
             "in / after each pass, lighting, every overlay draw, DCS's context) and outcome identical; the learner "
             "ends identical%s",
             tag, b.size(),
             hw ? " (hardware: boosted frames whose lighting alone differs, the driver's submit timing as in an A/A "
                  "run, at most 10)"
                : "");
    Check(a.size() == plan.size() && b.size() == plan.size() && diff == 0 && lightTiming <= 10 && keysTa == keysTb &&
              keysBa == keysBb &&
              commitsA == commitsB,
          what);
    snprintf(what, sizeof(what),
             "R128 X1 (%s): the sequence exercises the mode: dormant frames with key 1 only (%d of %d eligible, the "
             "rest full-work references), transitions both ways on arbitrary frames, boosted and patched frames",
             tag, dormB, eligB);
    Check(dormA == 0 && dormB >= 250 && eligB > dormB && toDorm >= 4 && toActive >= 4 && boosted >= 150 &&
              patched >= 150 && keysTb > 0,
          what);
    // X2: the first boosted frame after a dormant one.
    int onsets = 0, complete = 0, learnt = 0;
    for (size_t i = 1; i < b.size(); ++i) {
      if (!(b[i].h != 0 && b[i - 1].dormant)) continue;
      ++onsets;
      bool ok = b[i].anyPatched;
      int recT = 0;
      for (int t = 0; t < 4; ++t) {
        // (lit: WARP; the hardware driver's submit timing can miss a lighting upload in any boosted frame, X1)
        ok = ok && b[i].patched[t] && !b[i].failed[t] && (b[i].lit[t] || hw) && b[i].fwdN[t] > 0;
        recT += b[i].cpuRecT[t];
      }
      complete += ok;
      bool moved = true;
      for (int t = 0; t < 4; ++t) moved = moved && b[i].cpuMoved[t] > 0;
      learnt += moved && recT > 0;
    }
    snprintf(what, sizeof(what),
             "R128 X2 (%s): the first boosted frame after a dormant frame is complete: all four views' cockpit "
             "patched and lit at the true eye, the forward pass moved (%d of %d onsets); the overlay replays moved "
             "with records at the true eye (the learner carried through the dormant frames) at %d of them",
             tag, complete, onsets, learnt);
    Check(onsets >= 3 && complete == onsets && learnt >= onsets - 1 && learnt >= 2, what);
    // X3: symbology in dormant frames.
    int dormSym = 0, dormSymOk = 0, offSym = 0, offSymZero = 0;
    for (size_t i = 0; i < b.size(); ++i) {
      if (!b[i].dormant) continue;
      if (plan[i].symOn) {
        ++dormSym;
        bool ok = true;
        for (int t = 0; t < 4; ++t) ok = ok && b[i].sym[t] > 0 && b[i].sym[t] == a[i].sym[t];
        dormSymOk += ok && b[i].symTypes == a[i].symTypes;
      } else {
        ++offSym;
        offSymZero += b[i].sym[0] + b[i].sym[1] + b[i].sym[2] + b[i].sym[3] == 0;
      }
    }
    snprintf(what, sizeof(what),
             "R128 X3 (%s): in dormant frames the symbology export still gets every HMD view's record types through "
             "the render point (%d of %d frames, as with key 0), none while its views are off (%d of %d)",
             tag, dormSymOk, dormSym, offSymZero, offSym);
    Check(dormSym >= 100 && dormSymOk == dormSym && offSym >= 5 && offSymZero == offSym, what);
  }
  if (bench) Bench(u, tag);

  // Teardown.
  fixdepth::g_installed = oldInstalled;
  fixdepth::g_ckWith = oldCkWith;
  gbpass::g_state = oldGbState;
  gbpass::g_orig = oldOrig;
  gbpass::g_wrap = oldWrap;
  shadowinst::g_device = oldDev;
  fixdepth::g_s.ctx = oldCtx;
  fixdepth::g_s.ctx1 = oldCtx1;
  fixdepth::g_gazeName = oldGaze;
  ck::g_boostNameOverride.clear();
  ck::g_ackNameOverride.clear();
  ck::SetDormant(true);
  ck::Configure(0, false, true, 0, 0, false);
  ocpt::g_h = oldH;
  ocpt::g_c = oldOc;
  cktest::g_c = oldCk;
  ocpt::g_curCtx = nullptr;
  if (u.bview) UnmapViewOfFile(u.bview);
  if (bm) CloseHandle(bm);
  const ULONG last = ReleaseRig(r);
  g_r = nullptr;
  char what[160];
  snprintf(what, sizeof(what), "R128 rig (%s): no D3D reference leaked (device released to 0)", tag);
  Check(last == 0, what);
}

void Run(bool hw) {
  printf("-- R128: cockpit ipd dormant mode --\n");
  const bool bench = getenv("QV_R128_BENCH") != nullptr;
  Test(D3D_DRIVER_TYPE_WARP, false, false);
  if (hw) {
    Test(D3D_DRIVER_TYPE_HARDWARE, true, false);
    Test(D3D_DRIVER_TYPE_HARDWARE, true, true);
  } else if (bench) {
    Test(D3D_DRIVER_TYPE_WARP, false, true);
  }
}

}  // namespace r128t
