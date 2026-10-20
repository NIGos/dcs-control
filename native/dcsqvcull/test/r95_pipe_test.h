// R95: the symbology export's whole path on WARP through fixation_depth.h's boundary callback and Tick (the live
// install path), the cockpit mask export as its master (mask view 0 sampled; the symbology covers all four HMD views):
// DCS's HDR targets of four views (periphery 160 x 150, focus 240 x 220) with a world scene; per view the overlay
// replay's records reported to the export (types 1 / 0 / 2 in DCS's order: the HUD's lines, an additive MFD block,
// the helmet's symbols in the right-eye views) and drawn with DCS's blend states; DCS's tone-map draw through our draw
// arm; the xrEndFrame commit with the submitted poses / FOVs; xrLocateViews calls of the frame (the window's pose).
// See r95_test.h for the list of checks.
#pragma once

namespace r95pipe {

using namespace r95test;
using namespace fixdepth;
using fdtest::FakePass;
using fdtest::MakePass;
using fdtest::View;

constexpr UINT kOW[4] = {160, 160, 240, 240}, kOH[4] = {150, 150, 220, 220};
// The submitted FOVs (angles, radians): periphery views wide, focus views a sub-frustum each (QV: same orientation).
const float kFov[4][4] = {{-0.95f, 0.80f, 0.85f, -0.85f},
                          {-0.80f, 0.95f, 0.85f, -0.85f},
                          {-0.30f, 0.24f, 0.26f, -0.30f},
                          {-0.24f, 0.30f, 0.26f, -0.30f}};
// The HUD's field in the tracking space (degrees: azimuth, elevation) and the helmet's screen-locked boxes.
constexpr double kHudAz0 = -7, kHudAz1 = 7, kHudEl0 = -13, kHudEl1 = 2;
const cmask::sym::Box kHelmet[4] = {{}, {true, 112, 18, 140, 40}, {}, {true, 168, 26, 214, 58}};

float g_lq[4][4], g_lf[4][4];  // the fake runtime's located views
int32_t __stdcall FakeLocate4(void*, const void*, void*, uint32_t cap, uint32_t* count, void* views) {
  if (count) *count = 4;
  auto* v = static_cast<uint8_t*>(views);
  for (uint32_t i = 0; v && i < 4 && i < cap; ++i) {
    *reinterpret_cast<int32_t*>(v + 64 * i) = 7;
    memcpy(v + 64 * i + 16, g_lq[i], 16);
    memcpy(v + 64 * i + 44, g_lf[i], 16);
  }
  return 0;
}

const char kSymVs[] = R"(
cbuffer R : register(b0) { float4 rect; float4 color; float4 vp; uint4 mode; };
float4 main(uint id : SV_VertexID) : SV_Position {
  float2 c = float2((id & 1) ? rect.z : rect.x, (id & 2) ? rect.w : rect.y);
  return float4(c.x / vp.x * 2 - 1, 1 - c.y / vp.y * 2, 0, 1);
})";
// The symbols' coverage: HUD (mode 0) a frame of 2 px and lines (11 / 13 px periods, 1 and 0.45 / 0.8 coverage); the
// helmet (2) a frame with 0.6 corners; the MFD block (1) solid.
const char kSymPs[] = R"(
cbuffer R : register(b0) { float4 rect; float4 color; float4 vp; uint4 mode; };
float4 main(float4 pos : SV_Position) : SV_Target0 {
  float2 l = floor(pos.xy) - rect.xy;
  float2 sz = rect.zw - rect.xy;
  float c = 1;
  bool frame = l.x < 2 || l.y < 2 || l.x >= sz.x - 2 || l.y >= sz.y - 2;
  if (mode.x == 0) {
    uint ix = (uint)l.x % 11, iy = (uint)l.y % 13;
    c = frame ? 1 : (ix == 3 || ix == 4) ? 1 : ix == 5 ? 0.45 : iy == 6 ? 0.8 : 0;
  } else if (mode.x == 2) {
    c = frame ? ((l.x < 2 || l.x >= sz.x - 2) && (l.y < 2 || l.y >= sz.y - 2) ? 0.6 : 1) : 0;
  }
  return float4(color.rgb, color.a * c);
})";

struct Rig {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  ID3D11DeviceContext1* c = nullptr;
  View g0;  // view 0's G-buffer depth (the mask's sample)
  ID3D11Texture2D* g0scene = nullptr;
  View def;
  ID3D11Texture2D *hdr[4] = {}, *scene[4] = {}, *ldr[4] = {}, *ldrStage[4] = {}, *hdrStage[4] = {};
  ID3D11RenderTargetView *hdrRtv[4] = {}, *ldrRtv[4] = {};
  ID3D11ShaderResourceView* hdrSrv[4] = {};
  ID3D11Buffer *lum = nullptr, *b0 = nullptr, *symCb = nullptr;
  ID3D11ShaderResourceView *lumSrv = nullptr, *bloomSrv = nullptr;
  ID3D11Texture2D* bloom = nullptr;
  ID3D11VertexShader *tmVs = nullptr, *symVs = nullptr;
  ID3D11PixelShader *tmPs = nullptr, *symPs = nullptr;
  ID3D11RasterizerState* rs = nullptr;
  ID3D11BlendState *bHud = nullptr, *bMfd = nullptr;
  cpass::Objects obj;
  void Release() {
    for (auto*& p : obj.psSym) Rel(p);
    obj.Release();
    for (int v = 0; v < 4; ++v)
      Rel(hdr[v]), Rel(scene[v]), Rel(ldr[v]), Rel(ldrStage[v]), Rel(hdrStage[v]), Rel(hdrRtv[v]), Rel(ldrRtv[v]),
          Rel(hdrSrv[v]);
    Rel(lum), Rel(b0), Rel(symCb), Rel(lumSrv), Rel(bloomSrv), Rel(bloom), Rel(tmVs), Rel(symVs), Rel(tmPs), Rel(symPs);
    Rel(rs), Rel(bHud), Rel(bMfd), Rel(g0scene), Rel(g0.dsv), Rel(g0.tex), Rel(g0.cb), Rel(def.dsv), Rel(def.tex);
    Rel(c), Rel(imm), Rel(dev);
  }
};

bool CompileTo(const char* src, const char* target, std::vector<uint8_t>& out) {
  std::string err;
  return vfxrig::CompileTo(src, target, out, err);
}

bool MakeRig(Rig& r, D3D_DRIVER_TYPE drv) {
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1};
  if (FAILED(D3D11CreateDevice(nullptr, drv, nullptr, 0, want, 1, D3D11_SDK_VERSION, &r.dev, nullptr, &r.imm))) return false;
  r.imm->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&r.c));
  if (!r.c) return false;
  // View 0's G-buffer (render resolution 64 x 60, the mask's view).
  r.g0.w = 64, r.g0.h = 60;
  fdtest::MakeProj(r.g0.P, -1.3, 1.1, 1.2, -1.2, 0.02, 0, true);
  if (!mask65test::MakeTarget(r.dev, r.g0, DXGI_FORMAT_R32G8X24_TYPELESS, 1, 0) || !fdtest::MakeCb(r.dev, r.g0, 0)) return false;
  r.g0scene = mask65test::MakeScene(r.dev, r.g0, DXGI_FORMAT_R32G8X24_TYPELESS, 1, 0, 0, false);
  r.def.w = r.def.h = 64;
  fdtest::MakeProj(r.def.P, -1, 1, 1, -1, 0.05, 0, true);
  if (!r.g0scene || !fdtest::MakeDepth(r.dev, r.c, r.def, DXGI_FORMAT_R32_TYPELESS, 1, 0)) return false;
  for (int v = 0; v < 4; ++v) {
    const UINT W = kOW[v], H = kOH[v];
    std::vector<uint16_t> px(size_t(W) * H * 4);
    for (UINT y = 0; y < H; ++y)
      for (UINT x = 0; x < W; ++x) {  // a world: sky gradient over terrain checks, alpha 0 (DLSS's output)
        const float t = float(y) / H;
        const bool terr = y > H * 0.55f;
        const bool chk = ((x / 5 + y / 5 + v) % 2) != 0;
        const float rgb[3] = {terr ? (chk ? 0.07f : 0.03f) : 0.10f + 0.1f * t, terr ? (chk ? 0.10f : 0.05f) : 0.18f + 0.1f * t,
                              terr ? 0.02f : 0.40f - 0.1f * t};
        for (int k = 0; k < 3; ++k) px[(size_t(y) * W + x) * 4 + k] = F2H(rgb[k]);
        px[(size_t(y) * W + x) * 4 + 3] = 0;
      }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W, td.Height = H, td.MipLevels = td.ArraySize = 1, td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT, td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px.data(), W * 8, 0};
    if (FAILED(r.dev->CreateTexture2D(&td, &sd, &r.scene[v]))) return false;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(r.dev->CreateTexture2D(&td, nullptr, &r.hdr[v])) || FAILED(r.dev->CreateRenderTargetView(r.hdr[v], nullptr, &r.hdrRtv[v])) ||
        FAILED(r.dev->CreateShaderResourceView(r.hdr[v], nullptr, &r.hdrSrv[v])))
      return false;
    D3D11_TEXTURE2D_DESC hs = td;
    hs.Usage = D3D11_USAGE_STAGING, hs.BindFlags = 0, hs.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(r.dev->CreateTexture2D(&hs, nullptr, &r.hdrStage[v]))) return false;
    D3D11_TEXTURE2D_DESC ld = td;
    ld.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS, ld.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC rvd{};
    rvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM, rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(r.dev->CreateTexture2D(&ld, nullptr, &r.ldr[v])) || FAILED(r.dev->CreateRenderTargetView(r.ldr[v], &rvd, &r.ldrRtv[v])))
      return false;
    ld.Format = DXGI_FORMAT_R8G8B8A8_UNORM, ld.Usage = D3D11_USAGE_STAGING, ld.BindFlags = 0, ld.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(r.dev->CreateTexture2D(&ld, nullptr, &r.ldrStage[v]))) return false;
  }
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = 64, bd.Usage = D3D11_USAGE_DEFAULT, bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, bd.StructureByteStride = 8;
  const float lumData[16] = {0, 1, 0.05f, 0, 0, 0, 0, 0, 0.05f, 0.0025f};
  D3D11_SUBRESOURCE_DATA ldd{lumData, 0, 0};
  if (FAILED(r.dev->CreateBuffer(&bd, &ldd, &r.lum)) || FAILED(r.dev->CreateShaderResourceView(r.lum, nullptr, &r.lumSrv))) return false;
  {
    D3D11_TEXTURE2D_DESC bt{};
    bt.Width = bt.Height = 1, bt.MipLevels = bt.ArraySize = 1, bt.Format = DXGI_FORMAT_R8_UNORM, bt.SampleDesc.Count = 1;
    bt.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const uint8_t z[4] = {};
    D3D11_SUBRESOURCE_DATA bz{z, 4, 0};
    if (FAILED(r.dev->CreateTexture2D(&bt, &bz, &r.bloom)) || FAILED(r.dev->CreateShaderResourceView(r.bloom, nullptr, &r.bloomSrv)))
      return false;
  }
  {
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = 256, cd.Usage = D3D11_USAGE_DYNAMIC, cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(r.dev->CreateBuffer(&cd, nullptr, &r.b0))) return false;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(r.c->Map(r.b0, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    cgrade::DcsTone t;
    t.ec = 0.2f;
    cgrade::PackDcsTone(t, static_cast<float*>(m.pData));
    r.c->Unmap(r.b0, 0);
    D3D11_BUFFER_DESC sb{};
    sb.ByteWidth = 64, sb.Usage = D3D11_USAGE_DEFAULT, sb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(r.dev->CreateBuffer(&sb, nullptr, &r.symCb))) return false;
  }
  std::vector<uint8_t> b;
  if (!CompileTo(vfxrig::kVs, "vs_5_0", b) || FAILED(r.dev->CreateVertexShader(b.data(), b.size(), nullptr, &r.tmVs))) return false;
  if (!CompileTo(vfxrig::kStockPs, "ps_5_0", b) || FAILED(r.dev->CreatePixelShader(b.data(), b.size(), nullptr, &r.tmPs))) return false;
  if (!CompileTo(kSymVs, "vs_5_0", b) || FAILED(r.dev->CreateVertexShader(b.data(), b.size(), nullptr, &r.symVs))) return false;
  if (!CompileTo(kSymPs, "ps_5_0", b) || FAILED(r.dev->CreatePixelShader(b.data(), b.size(), nullptr, &r.symPs))) return false;
  D3D11_RASTERIZER_DESC rsd{};
  rsd.FillMode = D3D11_FILL_SOLID, rsd.CullMode = D3D11_CULL_NONE, rsd.DepthClipEnable = TRUE;
  if (FAILED(r.dev->CreateRasterizerState(&rsd, &r.rs))) return false;
  D3D11_BLEND_DESC bl{};
  D3D11_RENDER_TARGET_BLEND_DESC& rb = bl.RenderTarget[0];
  rb.BlendEnable = TRUE, rb.BlendOp = rb.BlendOpAlpha = D3D11_BLEND_OP_ADD, rb.RenderTargetWriteMask = 0xf;
  rb.SrcBlend = D3D11_BLEND_SRC_ALPHA, rb.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;  // R94: HUD / helmet
  rb.SrcBlendAlpha = D3D11_BLEND_ONE, rb.DestBlendAlpha = D3D11_BLEND_ONE;        // R94: the alpha ADDs
  if (FAILED(r.dev->CreateBlendState(&bl, &r.bHud))) return false;
  rb.DestBlend = D3D11_BLEND_ONE;  // R94: F-16 COMMON displays SRC_ALPHA / ONE
  if (FAILED(r.dev->CreateBlendState(&bl, &r.bMfd))) return false;
  cpass::Blobs blobs;
  std::string err;
  if (!cpass::CompileAll(blobs, err) || !r.obj.Create(r.dev, blobs, err) || !cpass::CompileSym(blobs, err)) return false;
  for (int i = 0; i < 3; ++i)
    if (FAILED(r.dev->CreatePixelShader(blobs.psSym[i].data(), blobs.psSym[i].size(), nullptr, &r.obj.psSym[i]))) return false;
  return true;
}

// The HUD's pixel box in view v with pose (q, fov): its tracking-space field projected (the test's DCS draws it there).
cmask::sym::Box HudBox(int v, const float q[4]) {
  cmask::sym::Cone cone;
  cone.ok = true;
  const double k = 3.14159265358979 / 180;
  cone.az0 = kHudAz0 * k, cone.az1 = kHudAz1 * k, cone.el0 = kHudEl0 * k, cone.el1 = kHudEl1 * k;
  const cmask::sym::Box b = cmask::sym::ProjectCone(cone, cmask::sym::PoseOf(q, kFov[v]), kOW[v], kOH[v]);
  return cmask::sym::Clip(b, int(kOW[v]), int(kOH[v]));
}

void DrawRect(Rig& r, int v, const cmask::sym::Box& b, int mode, const float col[4], ID3D11BlendState* bs) {
  if (!b.ok) return;
  ID3D11DeviceContext* c = r.c;
  const float cb[16] = {float(b.x0), float(b.y0), float(b.x1), float(b.y1), col[0], col[1], col[2], col[3],
                        float(kOW[v]), float(kOH[v]), 0, 0};
  uint32_t m[16];
  memcpy(m, cb, sizeof(m));
  m[12] = static_cast<uint32_t>(mode);
  c->UpdateSubresource(r.symCb, 0, nullptr, m, 0, 0);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  c->IASetInputLayout(nullptr);
  c->VSSetShader(r.symVs, nullptr, 0);
  c->PSSetShader(r.symPs, nullptr, 0);
  c->VSSetConstantBuffers(0, 1, &r.symCb);
  c->PSSetConstantBuffers(0, 1, &r.symCb);
  c->OMSetBlendState(bs, nullptr, 0xffffffffu);
  c->RSSetState(r.rs);
  c->Draw(4, 0);
  c->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
}

struct FrameOut {
  std::vector<uint8_t> F[4];   // DCS's tone-mapped bytes (the view's LDR target)
  std::vector<float> P[4];     // DCS's HDR after the overlay pass (rgba)
  cmask::sym::Box hud[4];
};

struct Pipe {
  Rig r;
  FakePass casc = MakePass("CascadeShadowPass0"), lights = MakePass("LightsBuffersPass"), gbuf = MakePass("GBufferPass"),
           resolve = MakePass("DepthResolverPass"), sea = MakePass("WaterSeaPass"), lit = MakePass("GBufferLightingPass"),
           trans = MakePass("TransparentPass"), ovl = MakePass("OverlayHDRPass"), hist = MakePass("HistogramPass"),
           tone = MakePass("ToneMappingPass"), nvg = MakePass("NVGPass"), hmd = MakePass("CopyToHMDPass");
  cm::Header* mblk = nullptr;
  csy::Header* sblk = nullptr;
  HANDLE mmap = nullptr, smap = nullptr;  // the test reader's own mappings (they outlive our reinstalls)
  bool mBeat = true, sBeat = true;
  bool flight = false;
  int vpCut = 0;
  bool hudOn = true, helmetOn = true, mfdOn = true, nvgPass = false;
  float alphaBefore = 0;  // (unused: the scene's alpha is 0)
  double yaw = 0;
  cmask::XrFrameInfo xi{};
  bool readback = false;
  bool noPoses = false;  // the commit's XrFrameInfo without poses (an unreadable submission)
  DWORD settleMs = 0;    // WARP: a pause after each frame (the statistics' read-back must not lag the test's frames)
};

void Bind(Rig& r, ID3D11RenderTargetView* rtv, UINT w, UINT h) {
  r.c->OMSetRenderTargets(1, &rtv, nullptr);
  const D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
  r.c->RSSetViewports(1, &vp);
}
void BindDefault(Rig& r) {
  ID3D11RenderTargetView* none = nullptr;
  r.c->OMSetRenderTargets(1, &none, r.def.dsv);
  const D3D11_VIEWPORT vp{0, 0, 64, 64, 0, 1};
  r.c->RSSetViewports(1, &vp);
}
std::vector<uint8_t> ReadLdr(Rig& r, int v) {
  r.c->CopyResource(r.ldrStage[v], r.ldr[v]);
  std::vector<uint8_t> out(size_t(kOW[v]) * kOH[v] * 4);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(r.c->Map(r.ldrStage[v], 0, D3D11_MAP_READ, 0, &m))) {
    for (UINT y = 0; y < kOH[v]; ++y) memcpy(&out[size_t(y) * kOW[v] * 4], static_cast<uint8_t*>(m.pData) + y * m.RowPitch, kOW[v] * 4);
    r.c->Unmap(r.ldrStage[v], 0);
  }
  return out;
}
std::vector<float> ReadHdr(Rig& r, int v) {
  r.c->CopyResource(r.hdrStage[v], r.hdr[v]);
  std::vector<float> out(size_t(kOW[v]) * kOH[v] * 4);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(r.c->Map(r.hdrStage[v], 0, D3D11_MAP_READ, 0, &m))) {
    for (UINT y = 0; y < kOH[v]; ++y) {
      const uint16_t* row = reinterpret_cast<const uint16_t*>(static_cast<uint8_t*>(m.pData) + y * m.RowPitch);
      for (UINT x = 0; x < kOW[v] * 4; ++x) out[size_t(y) * kOW[v] * 4 + x] = H2F(row[x]);
    }
    r.c->Unmap(r.hdrStage[v], 0);
  }
  return out;
}

FrameOut Frame(Pipe& p, int64_t displayTime) {
  Rig& r = p.r;
  ID3D11DeviceContext1* c = r.c;
  FrameOut fo;
  if (p.flight) g_quadFrame++;
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  if (p.mBeat && p.mblk) cm::reader_heartbeat(p.mblk, q.QuadPart);
  if (p.sBeat && p.sblk) csy::reader_heartbeat(p.sblk, q.QuadPart);
  // This frame's head pose (all views: QV's focus views share their eye's orientation) and DCS's locate call.
  float hq[4];
  QuatYawPitch(p.yaw, 0, 0, hq);
  for (int v = 0; v < 4; ++v) memcpy(g_lq[v], hq, 16), memcpy(g_lf[v], kFov[v], 16);
  {
    alignas(8) uint8_t info[40] = {};
    *reinterpret_cast<int32_t*>(info) = 6;
    *reinterpret_cast<int64_t*>(info + 24) = displayTime;
    alignas(8) uint8_t views[64 * 4] = {};
    uint32_t n = 0;
    cmask::ctime::H_LocateViews(reinterpret_cast<void*>(0x77), info, nullptr, 4, &n, views);
  }
  p.xi = cmask::XrFrameInfo{};
  p.xi.displayTime = displayTime;
  p.xi.poses = !p.noPoses;
  p.xi.viewCount = 4;
  for (int v = 0; v < 4; ++v) memcpy(p.xi.q[v], hq, 16), memcpy(p.xi.fov[v], kFov[v], 16);
  OnBoundary(&p.casc, true, 0);
  OnBoundary(&p.casc, false, 0);
  const std::vector<float> cbData = fartest::CbData(r.g0, r.g0.P);
  for (int v = 0; v < 4; ++v) {
    if (v == 0) {  // the mask's view: its G-buffer run, sampled at WaterSeaPass's end
      c->CopyResource(r.g0.tex, r.g0scene);
      OnBoundary(&p.lights, true, 0);
      OnBoundary(&p.lights, false, 0);
      for (int pass = 0; pass < 2; ++pass) {
        OnBoundary(&p.gbuf, true, 0);
        c->OMSetRenderTargets(0, nullptr, r.g0.dsv);
        const D3D11_VIEWPORT vp{0, 0, float(r.g0.w - p.vpCut), float(r.g0.h - p.vpCut), 0, 1};
        c->RSSetViewports(1, &vp);
        c->UpdateSubresource(r.g0.cb, 0, nullptr, cbData.data(), 0, 0);
        c->VSSetConstantBuffers(7, 1, &r.g0.cb);
        BindDefault(r);
        OnBoundary(&p.gbuf, false, 0);
      }
      OnBoundary(&p.resolve, true, 0);
      OnBoundary(&p.resolve, false, 0);
      OnBoundary(&p.sea, true, 0);
      OnBoundary(&p.sea, false, 0);
      OnBoundary(&p.lit, true, 0);
      OnBoundary(&p.lit, false, 0);
    } else {
      OnBoundary(&p.gbuf, true, 0);
      BindDefault(r);
      OnBoundary(&p.gbuf, false, 0);
    }
    OnBoundary(&p.trans, true, 0);
    OnBoundary(&p.trans, false, 0);
    BindDefault(r);
    // OverlayHDRPass: DCS's replay (records in DCS's order, each reported before it is drawn)
    c->CopyResource(r.hdr[v], r.scene[v]);
    OnBoundary(&p.ovl, true, 0);
    Bind(r, r.hdrRtv[v], kOW[v], kOH[v]);
    fo.hud[v] = HudBox(v, hq);
    const float hudCol[4] = {0.15f, 4.0f, 0.2f, 1.0f}, mfdCol[4] = {0.3f, 0.12f, 0.05f, 1.0f}, helCol[4] = {0.1f, 1.5f, 0.1f, 1.0f};
    // R94's F-16 replay: 129 HUD records, 37 COMMON, 2 helmet (right eye); each reported before it is drawn
    for (int k = 0; k < 129; ++k) cmask::sym::Record(v, 1);
    if (p.hudOn) DrawRect(r, v, fo.hud[v], 0, hudCol, r.bHud);
    for (int k = 0; k < 37; ++k) cmask::sym::Record(v, 0);
    if (p.mfdOn && fo.hud[v].ok) {  // an MFD block just right of the HUD's lower edge (inside the window's margin)
      const cmask::sym::Box m{true, fo.hud[v].x1 + 1, fo.hud[v].y1 - 8, fo.hud[v].x1 + 6, fo.hud[v].y1 + 3};
      DrawRect(r, v, cmask::sym::Clip(m, int(kOW[v]), int(kOH[v])), 1, mfdCol, r.bMfd);
    }
    if (v & 1) {
      cmask::sym::Record(v, 2);
      cmask::sym::Record(v, 2);
      if (p.helmetOn) DrawRect(r, v, kHelmet[v], 2, helCol, r.bHud);
    }
    cmask::sym::ReplayEnd(v);
    BindDefault(r);
    OnBoundary(&p.ovl, false, 0);
    if (v == 0) {
      OnBoundary(&p.hist, true, 0);
      OnBoundary(&p.hist, false, 0);
    }
    if (p.nvgPass && v == 2) {  // a pass of unknown effect between the overlay and the tone map
      OnBoundary(&p.nvg, true, 0);
      OnBoundary(&p.nvg, false, 0);
    }
    // ToneMappingPass: DCS's draw (through our arm: the swap, the second evaluation)
    OnBoundary(&p.tone, true, 0);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    c->IASetInputLayout(nullptr);
    c->VSSetShader(r.tmVs, nullptr, 0);
    c->PSSetShader(r.tmPs, nullptr, 0);
    ID3D11ShaderResourceView* s3[3] = {r.hdrSrv[v], r.lumSrv, r.bloomSrv};
    c->PSSetShaderResources(0, 3, s3);
    c->PSSetConstantBuffers(0, 1, &r.b0);
    Bind(r, r.ldrRtv[v], kOW[v], kOH[v]);
    c->RSSetState(r.rs);
    c->Draw(4, 0);
    ID3D11ShaderResourceView* n3[3] = {};
    c->PSSetShaderResources(0, 3, n3);
    BindDefault(r);
    OnBoundary(&p.tone, false, 0);
    OnBoundary(&p.hmd, true, 0);
    OnBoundary(&p.hmd, false, 0);
    if (p.readback) fo.F[v] = ReadLdr(r, v), fo.P[v] = ReadHdr(r, v);
  }
  cmask::OnXrEndFrame(displayTime, &p.xi);
  c->Flush();
  if (p.settleMs) Sleep(p.settleMs);  // WARP runs the frame on its worker threads: the read-back of the frame before
  return fo;
}

// The symbology record as the test's reader sees it.
struct SymRec {
  bool ok = false;
  csy::Header h{};
};
SymRec ReadSym(Pipe& p) {
  SymRec s;
  s.ok = p.sblk && csy::seqlock_read(p.sblk, s.h);
  return s;
}
struct Atl {
  std::vector<float> E;
  std::vector<uint8_t> cls;
  uint32_t W = 0, H = 0;
};
// The slot's atlases through their NT handles, after the shared fence reached seq (a reader on DCS's device).
bool ReadAtlas(Pipe& p, const csy::Header& h, int slot, Atl& a) {
  ID3D11Device1* d1 = nullptr;
  ID3D11Device5* d5 = nullptr;
  p.r.dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&d1));
  p.r.dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&d5));
  ID3D11Texture2D *e = nullptr, *k = nullptr;
  ID3D11Fence* f = nullptr;
  bool ok = d1 && d5 &&
            SUCCEEDED(d1->OpenSharedResource1(reinterpret_cast<HANDLE>(h.textureHandle[slot]), __uuidof(ID3D11Texture2D),
                                              reinterpret_cast<void**>(&e))) &&
            SUCCEEDED(d1->OpenSharedResource1(reinterpret_cast<HANDLE>(h.classTextureHandle[slot]), __uuidof(ID3D11Texture2D),
                                              reinterpret_cast<void**>(&k))) &&
            SUCCEEDED(d5->OpenSharedFence(reinterpret_cast<HANDLE>(h.fenceHandle), __uuidof(ID3D11Fence), reinterpret_cast<void**>(&f)));
  if (ok) {
    p.r.c->Flush();
    for (int i = 0; i < 400 && f->GetCompletedValue() < h.slot[slot].seq; ++i) Sleep(5);
    ok = f->GetCompletedValue() >= h.slot[slot].seq;
  }
  if (ok) {
    D3D11_TEXTURE2D_DESC d{};
    e->GetDesc(&d);
    a.W = d.Width, a.H = d.Height;
    d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.MiscFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    p.r.dev->CreateTexture2D(&d, nullptr, &st);
    D3D11_TEXTURE2D_DESC kd = d;
    kd.Format = DXGI_FORMAT_R8_UINT;
    ID3D11Texture2D* kt = nullptr;
    p.r.dev->CreateTexture2D(&kd, nullptr, &kt);
    ok = st && kt;
    if (ok) {
      p.r.c->CopyResource(st, e);
      p.r.c->CopyResource(kt, k);
      D3D11_MAPPED_SUBRESOURCE m{};
      a.E.assign(size_t(a.W) * a.H * 4, 0);
      a.cls.assign(size_t(a.W) * a.H, 0);
      if (SUCCEEDED(p.r.c->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < a.H; ++y) {
          const uint16_t* row = reinterpret_cast<const uint16_t*>(static_cast<uint8_t*>(m.pData) + y * m.RowPitch);
          for (UINT x = 0; x < a.W * 4; ++x) a.E[size_t(y) * a.W * 4 + x] = H2F(row[x]);
        }
        p.r.c->Unmap(st, 0);
      }
      if (SUCCEEDED(p.r.c->Map(kt, 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < a.H; ++y) memcpy(&a.cls[size_t(y) * a.W], static_cast<uint8_t*>(m.pData) + y * m.RowPitch, a.W);
        p.r.c->Unmap(kt, 0);
      }
    }
    Rel(st), Rel(kt);
  }
  Rel(e), Rel(k), Rel(f), Rel(d1), Rel(d5);
  return ok;
}

// E of frame A against the frame without the symbols (B: the same pose, the MFD only), per view.
struct ViewCheck {
  int symTex = 0, idBad = 0, nearOne = 0, zeroBad = 0, clsBad = 0, alphaBad = 0, mfdInWin = 0, outside = 0;
  double worst = 0;
};
ViewCheck CheckView(const csy::EmissionView& m, const Atl& a, const FrameOut& A, const FrameOut& B, int v) {
  ViewCheck k;
  const UINT W = kOW[v];
  for (uint32_t j = 0; j < m.winH; ++j)
    for (uint32_t i = 0; i < m.winW; ++i) {
      const uint32_t x = m.winX + i, y = m.winY + j;
      const size_t tp = (size_t(y) * W + x) * 4, at = (size_t(m.y + j) * a.W + m.x + i);
      bool sym = false;
      for (int c = 0; c < 4; ++c) sym = sym || A.P[v][tp + c] != B.P[v][tp + c];
      const cmask::sym::Box& h = A.hud[v];
      const bool inHud = h.ok && int(x) >= h.x0 && int(x) < h.x1 && int(y) >= h.y0 && int(y) < h.y1;
      const bool inHel = kHelmet[v].ok && int(x) >= kHelmet[v].x0 && int(x) < kHelmet[v].x1 && int(y) >= kHelmet[v].y0 &&
                         int(y) < kHelmet[v].y1;
      if (!sym) {
        for (int c = 0; c < 4; ++c) k.zeroBad += a.E[at * 4 + c] != 0.0f;
        k.clsBad += a.cls[at] != 0;
        if (!inHud && !inHel && h.ok && int(x) >= h.x1 && int(x) <= h.x1 + 6 && int(y) >= h.y1 - 8 && int(y) < h.y1 + 3)
          ++k.mfdInWin;
        continue;
      }
      ++k.symTex;
      k.clsBad += a.cls[at] != (inHel ? 2 : 1);
      for (int c = 0; c < 3; ++c) {
        const double e = a.E[at * 4 + c];
        const double d = std::fabs(Dec8(A.F[v][tp + c]) - e - Dec8(B.F[v][tp + c]));
        k.worst = std::max(k.worst, d);
        // re-encoded to the swapchain's byte against the frame without the symbols: equal, or one code where the GPU's
        // own rounding of that frame differs from the shader's (hardware; never on WARP)
        const double lin = Dec8(A.F[v][tp + c]) - e;
        int best = 0;
        double bd = 1e9;
        for (int b = 0; b < 256; ++b)
          if (std::fabs(Dec8(static_cast<uint8_t>(b)) - lin) < bd) bd = std::fabs(Dec8(static_cast<uint8_t>(b)) - lin), best = b;
        const int diff = std::abs(best - int(B.F[v][tp + c]));
        k.idBad += diff > 1;
        k.nearOne += diff == 1;
      }
      const float wantA = std::min(1.0f, std::max(0.0f, A.P[v][tp + 3] - B.P[v][tp + 3]));
      k.alphaBad += std::fabs(a.E[at * 4 + 3] - wantA) > 2e-3f;
    }
  // Symbol texels outside the window (the window must hold the whole HUD / helmet).
  for (UINT y = 0; y < kOH[v]; ++y)
    for (UINT x = 0; x < W; ++x) {
      if (x >= m.winX && x < m.winX + m.winW && y >= m.winY && y < m.winY + m.winH) continue;
      const size_t tp = (size_t(y) * W + x) * 4;
      bool sym = false;
      for (int c = 0; c < 4; ++c) sym = sym || A.P[v][tp + c] != B.P[v][tp + c];
      k.outside += sym;
    }
  return k;
}

#include "r95_pipe_run.h"
}  // namespace r95pipe
