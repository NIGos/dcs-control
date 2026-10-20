// R51 VrFx: a WARP stand-in of one HMD view's tone-map draw with everything the effects read live (HDR t0, DCS's
// luminance t1, bloom t2, cbTonemapParams b0, the view's typeless R32G8X24 depth target, its cPerView with gProj at
// byte 256). Used by test\vrfx_test.h (GPU against the CPU reference) and tools\vrfx_preview.cpp (preview PNGs).
// Needs color_pass.h included before it. No DCS file is read or written.
#pragma once

#include <string>
#include <vector>

namespace vfxrig {

inline uint16_t ToHalf(float f) {
  uint32_t x;
  memcpy(&x, &f, 4);
  const uint32_t s = (x >> 16) & 0x8000u;
  int e = static_cast<int>((x >> 23) & 0xff) - 127 + 15;
  const uint32_t m = x & 0x7fffffu;
  if (!(f == f)) return 0;
  if (e <= 0) {
    if (e < -10) return static_cast<uint16_t>(s);
    const uint32_t mm = (m | 0x800000u) >> (1 - e);
    return static_cast<uint16_t>(s | ((mm + 0x1000u) >> 13));
  }
  if (e >= 31) return static_cast<uint16_t>(s | 0x7bffu);
  uint32_t h = s | (static_cast<uint32_t>(e) << 10) | (m >> 13);
  if ((m & 0x1fffu) > 0x1000u || ((m & 0x1fffu) == 0x1000u && (h & 1u))) ++h;
  return static_cast<uint16_t>(h);
}

// One view: linear HDR (W x H RGB), range in metres per pixel (<= 0 or inf = sky), FOV tangents, DCS's near plane,
// the adapted luminance and exposure correction its tone map uses.
struct Scene {
  unsigned w = 0, h = 0;
  std::vector<float> rgb, rangeM;
  float l = -0.5f, r = 0.5f, u = 0.5f, d = -0.5f, nearM = 0.02f;
  float adapted = 0.04f, ec = 0.0f;
};

inline const char kVs[] = R"(
struct VSO { noperspective float4 pos : SV_POSITION0; noperspective float4 projPos : TEXCOORD0; noperspective float3 worldPos : TEXCOORD1; };
VSO main(uint vid : SV_VertexID) {
  const float2 quad[4] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  VSO o;
  o.pos = float4(quad[vid], 0, 1);
  o.projPos = float4(float2(o.pos.x, -o.pos.y) * 0.5 + 0.5, float2(o.pos.x, -o.pos.y) * 0.5 + 0.5);
  o.worldPos = float3(0, 0, 1);
  return o;
})";
inline const char kStockPs[] = R"(
struct VSO { noperspective float4 pos : SV_POSITION0; noperspective float4 projPos : TEXCOORD0; noperspective float3 worldPos : TEXCOORD1; };
float4 main(VSO i) : SV_Target0 { return float4(1, 0, 1, 1); })";

struct Rig {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11Texture2D *hdr = nullptr, *depth = nullptr, *bloom = nullptr, *rt = nullptr, *stage = nullptr;
  ID3D11ShaderResourceView *hdrSrv = nullptr, *depthSrv = nullptr, *lumSrv = nullptr, *bloomSrv = nullptr;
  ID3D11Buffer *lum = nullptr, *b0 = nullptr, *perView = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* dcsPs = nullptr;
  ID3D11RasterizerState* rs = nullptr;
  cpass::Objects obj;
  Scene sc;
  std::vector<float> rgbGpu;     // the HDR as the GPU holds it (half-rounded)
  std::vector<float> depthDev;   // device depth per pixel (reverse-Z infinite)
  float projStored[16] = {};
  cgrade::DcsTone tone;
  void Release() {
    auto rel = [](auto*& p) {
      if (p) p->Release();
      p = nullptr;
    };
    obj.Release();
    rel(hdrSrv), rel(depthSrv), rel(lumSrv), rel(bloomSrv), rel(hdr), rel(depth), rel(bloom), rel(rtv), rel(rt);
    rel(stage), rel(lum), rel(b0), rel(perView), rel(vs), rel(dcsPs), rel(rs), rel(ctx), rel(dev);
  }
};

inline bool CompileTo(const char* src, const char* target, std::vector<uint8_t>& out, std::string& err) {
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  HMODULE m = LoadLibraryW((std::wstring(sys) + L"\\d3dcompiler_47.dll").c_str());
  const pD3DCompile c = m ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
  return c && cpass::CompileOne(c, src, target, nullptr, out, err);
}

inline bool Make(Rig& r, const Scene& s, std::string& err, D3D_DRIVER_TYPE drv = D3D_DRIVER_TYPE_WARP) {
  r.sc = s;
  const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, drv, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &r.dev, nullptr, &r.ctx)))
    return err = "WARP device", false;
  const unsigned W = s.w, H = s.h;
  std::vector<uint16_t> half(W * H * 4);
  r.rgbGpu.assign(W * H * 3, 0);
  for (unsigned i = 0; i < W * H; ++i)
    for (int k = 0; k < 4; ++k) {
      half[i * 4 + k] = ToHalf(k < 3 ? s.rgb[i * 3 + k] : 1.0f);
      if (k < 3) r.rgbGpu[i * 3 + k] = cgrade::HalfToFloat(half[i * 4 + k]);
    }
  D3D11_TEXTURE2D_DESC td{};
  td.Width = W, td.Height = H, td.MipLevels = 1, td.ArraySize = 1, td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT, td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  D3D11_SUBRESOURCE_DATA sd{half.data(), W * 8, 0};
  if (FAILED(r.dev->CreateTexture2D(&td, &sd, &r.hdr)) || FAILED(r.dev->CreateShaderResourceView(r.hdr, nullptr, &r.hdrSrv)))
    return err = "HDR texture", false;
  // Depth: DCS's format (R32G8X24_TYPELESS, read through R32_FLOAT_X8X24_TYPELESS), reverse-Z infinite: d = near / z.
  vrfx::StoreReverseInfinite(s.l, s.r, s.u, s.d, s.nearM, r.projStored);
  const vrfx::CProj p = vrfx::ProjFromStored(r.projStored);
  r.depthDev.assign(W * H, 0);
  std::vector<uint32_t> dd(W * H * 2, 0);
  for (unsigned y = 0; y < H; ++y)
    for (unsigned x = 0; x < W; ++x) {
      const float range = s.rangeM[y * W + x];
      float dv = 0;
      if (range > 0 && std::isfinite(range)) {
        const vrfx::F2 t = vrfx::Tan(p, (x + 0.5f) / W, (y + 0.5f) / H);
        const float z = range / std::sqrt(1 + t.x * t.x + t.y * t.y);
        dv = s.nearM / z;
      }
      r.depthDev[y * W + x] = dv;
      memcpy(&dd[(y * W + x) * 2], &dv, 4);
    }
  D3D11_TEXTURE2D_DESC dt = td;
  dt.Format = DXGI_FORMAT_R32G8X24_TYPELESS, dt.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA ds{dd.data(), W * 8, 0};
  D3D11_SHADER_RESOURCE_VIEW_DESC dvd{};
  dvd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, dvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  dvd.Texture2D.MipLevels = 1;
  if (FAILED(r.dev->CreateTexture2D(&dt, &ds, &r.depth)) || FAILED(r.dev->CreateShaderResourceView(r.depth, &dvd, &r.depthSrv)))
    return err = "depth texture", false;
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = 64, bd.Usage = D3D11_USAGE_DEFAULT, bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, bd.StructureByteStride = 8;
  const float lumData[16] = {0, 1, s.adapted, 0, 0, 0, 0, 0, s.adapted, s.adapted * s.adapted, 0, 0, 0, 0, 0, 0};
  D3D11_SUBRESOURCE_DATA ld{lumData, 0, 0};
  if (FAILED(r.dev->CreateBuffer(&bd, &ld, &r.lum)) || FAILED(r.dev->CreateShaderResourceView(r.lum, nullptr, &r.lumSrv)))
    return err = "luminance buffer", false;
  D3D11_TEXTURE2D_DESC bt = td;
  bt.Width = bt.Height = 1, bt.Format = DXGI_FORMAT_R8_UNORM, bt.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  const uint8_t zero[4] = {};
  D3D11_SUBRESOURCE_DATA bz{zero, 4, 0};
  if (FAILED(r.dev->CreateTexture2D(&bt, &bz, &r.bloom)) || FAILED(r.dev->CreateShaderResourceView(r.bloom, nullptr, &r.bloomSrv)))
    return err = "bloom", false;
  r.tone.ec = s.ec;
  {
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = 256, cd.Usage = D3D11_USAGE_DYNAMIC, cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(r.dev->CreateBuffer(&cd, nullptr, &r.b0))) return err = "b0", false;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(r.ctx->Map(r.b0, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return err = "b0 map", false;
    cgrade::PackDcsTone(r.tone, static_cast<float*>(m.pData));
    r.ctx->Unmap(r.b0, 0);
  }
  {  // cPerView: gProj at byte 256 (as DCS stores it), the rest zero; DYNAMIC as an effect constant buffer
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = 320, cd.Usage = D3D11_USAGE_DYNAMIC, cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(r.dev->CreateBuffer(&cd, nullptr, &r.perView))) return err = "cPerView", false;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(r.ctx->Map(r.perView, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return err = "cPerView map", false;
    memset(m.pData, 0, 320);
    memcpy(static_cast<uint8_t*>(m.pData) + 256, r.projStored, 64);
    r.ctx->Unmap(r.perView, 0);
  }
  D3D11_TEXTURE2D_DESC rd = td;
  rd.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS, rd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  D3D11_RENDER_TARGET_VIEW_DESC rvd{};
  rvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM, rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  if (FAILED(r.dev->CreateTexture2D(&rd, nullptr, &r.rt)) || FAILED(r.dev->CreateRenderTargetView(r.rt, &rvd, &r.rtv)))
    return err = "target", false;
  rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM, rd.Usage = D3D11_USAGE_STAGING, rd.BindFlags = 0, rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(r.dev->CreateTexture2D(&rd, nullptr, &r.stage))) return err = "staging", false;
  std::vector<uint8_t> b;
  if (!CompileTo(kVs, "vs_5_0", b, err) || FAILED(r.dev->CreateVertexShader(b.data(), b.size(), nullptr, &r.vs)))
    return err = "VS " + err, false;
  if (!CompileTo(kStockPs, "ps_5_0", b, err) || FAILED(r.dev->CreatePixelShader(b.data(), b.size(), nullptr, &r.dcsPs)))
    return err = "PS " + err, false;
  D3D11_RASTERIZER_DESC rsd{};
  rsd.FillMode = D3D11_FILL_SOLID, rsd.CullMode = D3D11_CULL_NONE, rsd.DepthClipEnable = TRUE;
  if (FAILED(r.dev->CreateRasterizerState(&rsd, &r.rs))) return err = "rasterizer", false;
  cpass::Blobs blobs;
  if (!cpass::CompileAll(blobs, err)) return false;
  return r.obj.Create(r.dev, blobs, err);
}

// The glue's inputs for view v (what fixation_depth.h's CloseRun / FxInputs leave live), then DCS's tone-map draw of
// view v through ToneDrawPre / Draw / ToneDrawPost. Returns ToneDrawPre's result.
inline int Draw(Rig& r, int v, ID3D11ShaderResourceView* const* psSentinels = nullptr, ID3D11Buffer* cbSentinel = nullptr) {
  ID3D11DeviceContext* c = r.ctx;
  cpass::g_obj.store(&r.obj);
  cpass::NoteLdr(v, r.sc.w, r.sc.h);
  cpass::FxNoteProj(c, v, r.perView, 0);
  cpass::FxViewIn& vi = cpass::g_fxView[v];
  vi.depth = r.depthSrv, vi.depthTex = r.depth;
  vi.dv[0] = 0, vi.dv[1] = 0, vi.dv[2] = static_cast<float>(r.sc.w), vi.dv[3] = static_cast<float>(r.sc.h);
  vi.valid = true;
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  c->IASetInputLayout(nullptr);
  c->VSSetShader(r.vs, nullptr, 0);
  c->PSSetShader(r.dcsPs, nullptr, 0);
  ID3D11ShaderResourceView* s3[3] = {r.hdrSrv, r.lumSrv, r.bloomSrv};
  c->PSSetShaderResources(0, 3, s3);
  if (psSentinels) c->PSSetShaderResources(vrfx::kPsFxSrvSlot, 3, psSentinels);
  if (cbSentinel) c->PSSetConstantBuffers(vrfx::kPsFxCbSlot, 1, &cbSentinel);
  c->PSSetConstantBuffers(0, 1, &r.b0);
  c->OMSetRenderTargets(1, &r.rtv, nullptr);
  const D3D11_VIEWPORT vp{0, 0, static_cast<float>(r.sc.w), static_cast<float>(r.sc.h), 0, 1};
  c->RSSetViewports(1, &vp);
  c->RSSetState(r.rs);
  const int act = cpass::ToneDrawPre(c, v);
  c->Draw(4, 0);
  cpass::ToneDrawPost(c, v, act);
  ID3D11RenderTargetView* none = nullptr;
  c->OMSetRenderTargets(1, &none, nullptr);
  return act;
}
inline std::vector<uint8_t> Read(Rig& r) {
  r.ctx->CopyResource(r.stage, r.rt);
  D3D11_MAPPED_SUBRESOURCE m{};
  std::vector<uint8_t> out(r.sc.w * r.sc.h * 4);
  if (SUCCEEDED(r.ctx->Map(r.stage, 0, D3D11_MAP_READ, 0, &m))) {
    for (unsigned y = 0; y < r.sc.h; ++y)
      memcpy(&out[y * r.sc.w * 4], static_cast<const uint8_t*>(m.pData) + y * m.RowPitch, r.sc.w * 4);
    r.ctx->Unmap(r.stage, 0);
  }
  return out;
}

// The effects' state for a draw, set by hand (tests, previews): FxFrame does not step while g_fxHold.
struct FxSet {
  bool dof = false, gv = false;
  double focusD = 0, gain = 1, pupil = 3, strength = 0.5, maxArcmin = 6, tol = 0.3, guard = 2;
  float gaze[2] = {0, 0};
  double loss = 0, red = 0, gvStrength = 1;
  int gvMode = 2, useGaze = 1;
};
inline void Apply(const FxSet& f) {
  vrfx::Params p;
  p.dof = f.dof, p.dofStrength = f.strength, p.dofMaxArcmin = f.maxArcmin, p.dofToleranceD = f.tol, p.dofGuardDeg = f.guard;
  p.gv = f.gv, p.gvStrength = f.gvStrength, p.gvMode = f.gvMode, p.gvUseGaze = f.useGaze;
  while (vrfx::g_paramLock.test_and_set()) {
  }
  vrfx::g_params = p;
  vrfx::g_paramLock.clear();
  vrfx::g_paramVer++;
  vrfx::g_wantDof = f.dof, vrfx::g_wantGv = f.gv;
  cpass::g_fxHold = true;
  cpass::FxRt& x = cpass::g_fx;
  x.p = p;
  x.ver = vrfx::g_paramVer.load();
  x.dof = vrfx::DofState{};
  x.dof.focusD = f.focusD, x.dof.gain = f.gain, x.dof.haveFocus = true;
  x.pupil = f.pupil;
  x.g = vrfx::GState{};
  x.g.loss = f.loss, x.g.red = f.red;
  x.gvMode = f.gvMode;
  x.shape = f.gv ? vrfx::ShapeOf(f.loss, f.red, f.gvStrength, f.gvMode) : vrfx::Shape{};
  x.dofOn = f.dof, x.gvOn = f.gv;
  cpass::FxFrameIn& in = cpass::g_fxIn;
  in = cpass::FxFrameIn{};
  in.gazeValid = true;
  for (int v = 0; v < 4; ++v) in.gaze[v][0] = f.gaze[0], in.gaze[v][1] = f.gaze[1];
}
inline void Clear() {
  vrfx::g_wantDof = false, vrfx::g_wantGv = false;
  cpass::g_fxHold = false;
  cpass::g_fx = cpass::FxRt{};
  cpass::g_fxIn = cpass::FxFrameIn{};
  for (int v = 0; v < 4; ++v) cpass::g_fxView[v] = cpass::FxViewIn{}, cpass::g_fxProjSeen[v] = false;
}

// R57: the pass's prep as the GPU left it after the last draw (half-resolution texels and tile maxima), for cb's sizes.
inline bool ReadPrep(Rig& r, const vrfx::FxCb& cb, vrfx::CPrep& out) {
  out = vrfx::CPrep{};
  out.hw = static_cast<unsigned>(cb.halfW), out.hh = static_cast<unsigned>(cb.halfH);
  out.tx = static_cast<unsigned>(cb.tilesX), out.ty = static_cast<unsigned>(cb.tilesY);
  out.px.assign(out.hw * out.hh * 4, 0);
  out.tiles.assign(out.tx * out.ty, 0);
  ID3D11Texture2D* src[2] = {r.obj.prepTex, r.obj.tileTex};
  for (int k = 0; k < 2; ++k) {
    if (!src[k]) return false;
    D3D11_TEXTURE2D_DESC td{};
    src[k]->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ, td.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(r.dev->CreateTexture2D(&td, nullptr, &st))) return false;
    r.ctx->CopyResource(st, src[k]);
    D3D11_MAPPED_SUBRESOURCE m{};
    const bool ok = SUCCEEDED(r.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
      const uint8_t* b = static_cast<const uint8_t*>(m.pData);
      if (k == 0)
        for (unsigned y = 0; y < out.hh; ++y)
          for (unsigned x = 0; x < out.hw * 4; ++x)
            out.px[y * out.hw * 4 + x] = cgrade::HalfToFloat(reinterpret_cast<const uint16_t*>(b + y * m.RowPitch)[x]);
      else
        for (unsigned y = 0; y < out.ty; ++y)
          for (unsigned x = 0; x < out.tx; ++x) out.tiles[y * out.tx + x] = reinterpret_cast<const float*>(b + y * m.RowPitch)[x];
      r.ctx->Unmap(st, 0);
    }
    st->Release();
    if (!ok) return false;
  }
  return true;
}

// The CPU reference of the whole draw (stock tone map + effects) as UNORM bytes.
struct MidCtx {
  const vrfx::FxCb* cb;
  const vrfx::CProj* p;
  unsigned x, y;
};
inline void MidFn(float lin[3], const void* ctx) {
  const MidCtx* m = static_cast<const MidCtx*>(ctx);
  vrfx::GVision(*m->cb, *m->p, m->x, m->y, lin);
}
// R57: prepIn (optional) replaces the CPU prep, e.g. the GPU's (ReadPrep): the gather alone is then compared, free of
// the device's float -> half rounding of the stored texels.
inline std::vector<uint8_t> Reference(const Rig& r, const vrfx::FxCb& cb, const vrfx::CPrep* prepIn = nullptr) {
  const unsigned W = r.sc.w, H = r.sc.h;
  const vrfx::CProj p = vrfx::ProjFromStored(r.projStored);
  vrfx::CView view;
  view.w = W, view.h = H, view.rgb = r.rgbGpu, view.depth = r.depthDev;
  const vrfx::CPrep prep = prepIn ? *prepIn : vrfx::Prep(view, cb, p);
  std::vector<uint8_t> out(W * H * 4, 255);
  for (unsigned y = 0; y < H; ++y)
    for (unsigned x = 0; x < W; ++x) {
      cgrade::ShadeIn in;
      for (int k = 0; k < 3; ++k) in.scene[k] = r.rgbGpu[(y * W + x) * 3 + k];
      vrfx::Dof(view, prep, cb, p, x, y, in.scene);
      in.adapted = r.sc.adapted, in.ec = r.sc.ec, in.x = x, in.y = y;
      MidCtx m{&cb, &p, x, y};
      float o[3];
      cgrade::Shade(r.tone, nullptr, in, o, &MidFn, &m);
      for (int k = 0; k < 3; ++k) out[(y * W + x) * 4 + k] = cgrade::ToUnorm8(o[k]);
    }
  return out;
}

}  // namespace vfxrig
