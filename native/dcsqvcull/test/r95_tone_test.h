// R95: the tone-map stage of the symbology export on WARP (color_pass.h's real draw path: ToneDrawPre / DCS's draw /
// ToneDrawPost with the export's query answered by the test). One view of the R51 VrFx rig's scene (a near panel, far
// terrain, sky; its depth for GazeDof) with HUD lines (alpha-blended, anti-aliased coverage, the overlay's alpha ADD),
// an additive MFD block (a COMMON draw: no accumulated change) and a region whose alpha before the HUD is not 0 (the
// F-4E case), inside a window that maps to an offset in the emission atlas. Per configuration (Mode 0 = the stock
// replica, Fix, Enhanced with glare, Enhanced at night with night vision, GazeDof, GVision):
//  - E == 0 exactly (all four channels) where nothing of type 1 / 2 changed the HDR (the MFD block included);
//  - dec(F) - E == dec(C) at every other window texel (C from the test-only QV_SYM_TEST variant: its second
//    evaluation's bytes), within E's half-float rounding: E's first term is the byte DCS's target stores;
//  - the production shader's F and E bit-equal to the test variant's;
//  - where the tone map is local (Mode 0, Fix, GVision): dec(F) - E == dec(F_ref), F_ref an independent frame of the
//    HDR without the symbols (the same shader, nothing accumulated);
//  - the coverage alpha (the added alpha, 0 where the alpha before was not 0, 0 with the switch off); the atlas outside
//    the window untouched; DCS's b10 / t25 / OM UAV slots back after the draw.
#pragma once

namespace r95tone {

using namespace r95test;

struct TQ {
  bool on = false;
  int view = 0;
  ID3D11Buffer* cb = nullptr;
  ID3D11ShaderResourceView* acc = nullptr;
  ID3D11UnorderedAccessView* out = nullptr;
  int done = -1;
  // the export's finalize after the draw (symbology.h ToneDone): the view's compute constants, DCS's target
  ID3D11Buffer* finCb = nullptr;
  ID3D11ShaderResourceView* ldrSrv = nullptr;
  ID3D11Resource* ldr = nullptr;
  ID3D11UnorderedAccessView* cls = nullptr;  // R97: the class atlas the finalize reads first
  uint32_t ww = 0, wh = 0;
};
TQ g_tq;
bool Query(int v, ID3D11Resource*, cpass::SymBind* o) {
  if (!g_tq.on || v != g_tq.view) return false;
  o->cb = g_tq.cb, o->acc = g_tq.acc, o->out = g_tq.out;
  return true;
}
void Done(int v, bool w) {
  g_tq.done = w ? v : -2;
  if (w) fixdepth::cmask::sym::FinalizeE(g_tq.finCb, g_tq.ldrSrv, g_tq.ldr, g_tq.acc, g_tq.out, g_tq.cls, g_tq.ww, g_tq.wh);
}

struct Stage {
  vfxrig::Rig r;
  unsigned W = 120, H = 80;
  int wx = 18, wy = 9, ww = 84, wh = 64, ax = 105, ay = 17, AW = 200, AH = 90;
  ID3D11Texture2D *post = nullptr, *preRef = nullptr, *acc = nullptr, *accZero = nullptr, *atlas = nullptr,
                  *testC = nullptr, *atlasStage = nullptr, *testStage = nullptr;
  ID3D11ShaderResourceView *postSrv = nullptr, *preRefSrv = nullptr, *accSrv = nullptr, *accZeroSrv = nullptr,
                           *sentSrv = nullptr;
  ID3D11UnorderedAccessView *atlasUav = nullptr, *testCUav = nullptr;
  ID3D11Texture2D* cls = nullptr;  // R97: the class atlas (1 where the symbology changed the texel)
  ID3D11UnorderedAccessView* clsUav = nullptr;
  ID3D11Buffer* psCb[2] = {};  // alpha on / off
  ID3D11Buffer* sentCb = nullptr;
  ID3D11Buffer* finCb = nullptr;
  ID3D11ShaderResourceView* ldrSrv = nullptr;
  ID3D11DeviceContext1* ctx1 = nullptr;
  ID3D11PixelShader* prodPs[3] = {};
  ID3D11PixelShader* testPs[3] = {};
  std::vector<float> D;      // window texels, rgba (fp32 as the accumulation stores it)
  std::vector<float> cov;    // the HUD coverage per window texel (0 none)
  std::vector<uint8_t> bad;  // the alpha-before-not-0 region
  std::vector<uint8_t> mfd;  // the MFD block
  void Release() {
    for (auto*& p : prodPs) Rel(p);
    for (auto*& p : testPs) Rel(p);
    // obj owns its psSym slots only while a run swapped them in: cleared before obj.Release
    for (auto*& p : r.obj.psSym) p = nullptr;
    Rel(post), Rel(preRef), Rel(acc), Rel(accZero), Rel(atlas), Rel(testC), Rel(atlasStage), Rel(testStage);
    Rel(postSrv), Rel(preRefSrv), Rel(accSrv), Rel(accZeroSrv), Rel(sentSrv), Rel(atlasUav), Rel(testCUav);
    Rel(psCb[0]), Rel(psCb[1]), Rel(sentCb), Rel(finCb), Rel(ldrSrv), Rel(ctx1), Rel(cls), Rel(clsUav);
    fixdepth::cmask::sym::ReleaseBase();
    r.Release();
  }
};

inline vfxrig::Scene MakeScene() {
  vfxrig::Scene sc;  // as vrfx_test.h's: a panel at 0.7 m on the left, terrain at 2 km, sky top right
  sc.w = 120, sc.h = 80;
  sc.l = -0.06f, sc.r = 0.06f, sc.u = 0.04f, sc.d = -0.04f;
  sc.adapted = 0.05f, sc.ec = 0.2f;
  sc.rgb.assign(sc.w * sc.h * 3, 0);
  sc.rangeM.assign(sc.w * sc.h, 0);
  for (unsigned y = 0; y < sc.h; ++y)
    for (unsigned x = 0; x < sc.w; ++x) {
      float* c = &sc.rgb[(y * sc.w + x) * 3];
      float& rm = sc.rangeM[y * sc.w + x];
      if (x < 50) {
        const bool line = (x % 6 < 2) || (y % 9 == 0);
        c[0] = line ? 0.9f : 0.03f, c[1] = line ? 0.8f : 0.03f, c[2] = line ? 0.3f : 0.035f;
        rm = 0.7f;
      } else if (y < 25) {
        c[0] = 0.12f, c[1] = 0.2f, c[2] = 0.45f;
        rm = 0;
      } else {
        const bool f = ((x / 3 + y / 3) % 2) != 0;
        c[0] = f ? 0.08f : 0.03f, c[1] = f ? 0.12f : 0.05f, c[2] = 0.02f;
        rm = 2000;
      }
    }
  return sc;
}

inline ID3D11Texture2D* MakeTex(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT f, UINT bind, const void* data, UINT pitch,
                                ID3D11ShaderResourceView** srv = nullptr, ID3D11UnorderedAccessView** uav = nullptr,
                                bool staging = false) {
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w, d.Height = h, d.MipLevels = d.ArraySize = 1, d.Format = f, d.SampleDesc.Count = 1;
  d.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
  d.BindFlags = staging ? 0 : bind;
  d.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
  D3D11_SUBRESOURCE_DATA sd{data, pitch, 0};
  ID3D11Texture2D* t = nullptr;
  if (FAILED(dev->CreateTexture2D(&d, data ? &sd : nullptr, &t))) return nullptr;
  if (srv && FAILED(dev->CreateShaderResourceView(t, nullptr, srv))) return t;
  if (uav && FAILED(dev->CreateUnorderedAccessView(t, nullptr, uav))) return t;
  return t;
}

bool Make(Stage& s, std::string& err, D3D_DRIVER_TYPE drv) {
  if (!vfxrig::Make(s.r, MakeScene(), err, drv)) return false;
  ID3D11Device* dev = s.r.dev;
  const unsigned W = s.W, H = s.H;
  // S0: the scene as DCS's DLSS output (alpha 0; 0.4 in the F-4E-like region).
  std::vector<uint16_t> post(W * H * 4), pre(W * H * 4);
  s.D.assign(size_t(s.ww) * s.wh * 4, 0.0f);
  s.cov.assign(size_t(s.ww) * s.wh, 0.0f);
  s.bad.assign(size_t(s.ww) * s.wh, 0);
  s.mfd.assign(size_t(s.ww) * s.wh, 0);
  for (unsigned y = 0; y < H; ++y)
    for (unsigned x = 0; x < W; ++x) {
      const int i = int(x) - s.wx, j = int(y) - s.wy;
      const bool in = i >= 0 && j >= 0 && i < s.ww && j < s.wh;
      const size_t wi = in ? size_t(j) * s.ww + i : 0;
      const bool bad = in && i >= 4 && i < 16 && j >= 44 && j < 56;
      const bool mfd = in && i >= 52 && i < 68 && j >= 40 && j < 54;
      float s0[4];
      for (int k = 0; k < 3; ++k) s0[k] = s.r.rgbGpu[(y * W + x) * 3 + k];
      s0[3] = Rh(bad ? 0.4f : 0.0f);
      float c = 0;
      if (in && !mfd) {
        if (i % 11 == 3 || i % 11 == 4) c = 1.0f;
        else if (i % 11 == 5) c = 0.45f;
        if (j % 13 == 6) c = std::max(c, 0.8f);
      }
      const float hc[3] = {j < 20 ? 0.2f : 0.04f, j < 20 ? 6.0f : 0.9f, j < 20 ? 0.3f : 0.06f};
      float p[4];
      if (mfd) {
        const float m[3] = {0.25f, 0.08f, 0.02f};
        for (int k = 0; k < 3; ++k) p[k] = Rh(s0[k] + m[k]);
        p[3] = Rh(s0[3] + 1.0f);
      } else {
        for (int k = 0; k < 3; ++k) p[k] = Rh(s0[k] * (1 - c) + hc[k] * c);
        p[3] = Rh(s0[3] + c);
      }
      for (int k = 0; k < 4; ++k) {
        post[(y * W + x) * 4 + k] = F2H(p[k]);
        pre[(y * W + x) * 4 + k] = F2H(mfd ? p[k] : s0[k]);
      }
      if (in) {
        s.cov[wi] = c;
        s.bad[wi] = bad;
        s.mfd[wi] = mfd;
        if (c > 0)
          for (int k = 0; k < 4; ++k) s.D[wi * 4 + k] = p[k] - s0[k];  // h - a in fp32 (both halves: exact)
      }
    }
  const std::vector<float> zero(size_t(s.ww) * s.wh * 4, 0.0f);
  std::vector<float> sent(size_t(s.AW) * s.AH * 4, -7.0f);
  std::vector<uint16_t> sentH(size_t(s.AW) * s.AH * 4, F2H(-7.0f));
  s.post = MakeTex(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, post.data(), W * 8, &s.postSrv);
  s.preRef = MakeTex(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, pre.data(), W * 8, &s.preRefSrv);
  s.acc = MakeTex(dev, s.ww, s.wh, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, s.D.data(), s.ww * 16, &s.accSrv);
  s.accZero = MakeTex(dev, s.ww, s.wh, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, zero.data(), s.ww * 16,
                      &s.accZeroSrv);
  s.atlas = MakeTex(dev, s.AW, s.AH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_UNORDERED_ACCESS, sentH.data(), s.AW * 8,
                    nullptr, &s.atlasUav);
  s.testC = MakeTex(dev, s.AW, s.AH, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, sent.data(), s.AW * 16,
                    nullptr, &s.testCUav);
  s.atlasStage = MakeTex(dev, s.AW, s.AH, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, nullptr, 0, nullptr, nullptr, true);
  {  // R97: the class atlas as the accumulation leaves it (1 where D != 0 in the window, at the window's atlas origin)
    std::vector<uint8_t> k(size_t(s.AW) * s.AH, 0);
    for (int j = 0; j < s.wh; ++j)
      for (int i = 0; i < s.ww; ++i) {
        const size_t wi = size_t(j) * s.ww + i;
        if (s.D[wi * 4] != 0 || s.D[wi * 4 + 1] != 0 || s.D[wi * 4 + 2] != 0 || s.D[wi * 4 + 3] != 0)
          k[size_t(s.ay + j) * s.AW + s.ax + i] = 1;
      }
    s.cls = MakeTex(dev, s.AW, s.AH, DXGI_FORMAT_R8_UINT, D3D11_BIND_UNORDERED_ACCESS, k.data(), s.AW, nullptr, &s.clsUav);
  }
  s.testStage = MakeTex(dev, s.AW, s.AH, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, nullptr, 0, nullptr, nullptr, true);
  for (int a = 0; a < 2; ++a) {
    const int32_t cb[16] = {s.wx, s.wy, s.ww, s.wh, s.ax, s.ay, a == 0 ? 1 : 0, 0};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 64, bd.Usage = D3D11_USAGE_DEFAULT, bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{cb, 0, 0};
    if (FAILED(dev->CreateBuffer(&bd, &sd, &s.psCb[a]))) return err = "psCb", false;
  }
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = 16, bd.Usage = D3D11_USAGE_DEFAULT, bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  if (FAILED(dev->CreateBuffer(&bd, nullptr, &s.sentCb)) || FAILED(dev->CreateShaderResourceView(s.r.bloom, nullptr, &s.sentSrv)))
    return err = "sentinels", false;
  if (!s.post || !s.preRef || !s.accSrv || !s.accZeroSrv || !s.atlasUav || !s.testCUav || !s.atlasStage || !s.testStage ||
      !s.clsUav)
    return err = "textures", false;
  // The export's finalize (its compute shaders on this device) and the target's UNORM view.
  if (!fixdepth::cmask::Compile() || !fixdepth::cmask::sym::MakeBase(dev)) return err = "symbology compute shaders", false;
  {
    const uint32_t f[32] = {uint32_t(s.wx), uint32_t(s.wy), uint32_t(s.ww), uint32_t(s.wh), uint32_t(s.ax), uint32_t(s.ay),
                            0, 0, 0, 0, W, H, 0, 0, 0, 0};
    D3D11_BUFFER_DESC fd{};
    fd.ByteWidth = fixdepth::cmask::sym::kCbBytes, fd.Usage = D3D11_USAGE_DEFAULT, fd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA fs{f, 0, 0};
    D3D11_SHADER_RESOURCE_VIEW_DESC lv{};
    lv.Format = DXGI_FORMAT_R8G8B8A8_UNORM, lv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, lv.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateBuffer(&fd, &fs, &s.finCb)) || FAILED(dev->CreateShaderResourceView(s.r.rt, &lv, &s.ldrSrv)))
      return err = "finalize inputs", false;
    s.r.ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&s.ctx1));
  }
  // The production and the test variant of the three QV_SYM shaders.
  cpass::Blobs bp, bt;
  if (!cpass::CompileSym(bp, err) || !cpass::CompileSym(bt, err, true)) return false;
  for (int i = 0; i < 3; ++i)
    if (FAILED(dev->CreatePixelShader(bp.psSym[i].data(), bp.psSym[i].size(), nullptr, &s.prodPs[i])) ||
        FAILED(dev->CreatePixelShader(bt.psSym[i].data(), bt.psSym[i].size(), nullptr, &s.testPs[i])))
      return err = "QV_SYM pixel shaders", false;
  return true;
}

struct Out {
  std::vector<uint8_t> F;
  std::vector<float> E, C;
  int act = 0, done = -1;
  bool slotsBack = false;
};
// One tone-map draw of view 0 (a new frame of the colour pass each time), as vfxrig::Draw with this HDR source.
Out Run(Stage& s, bool sym, bool test, ID3D11ShaderResourceView* t0, ID3D11ShaderResourceView* acc, bool alpha) {
  ID3D11DeviceContext* c = s.r.ctx;
  Out o;
  const std::vector<uint16_t> sentH(size_t(s.AW) * s.AH * 4, F2H(-7.0f));
  const std::vector<float> sent(size_t(s.AW) * s.AH * 4, -7.0f);
  c->UpdateSubresource(s.atlas, 0, nullptr, sentH.data(), s.AW * 8, 0);
  c->UpdateSubresource(s.testC, 0, nullptr, sent.data(), s.AW * 16, 0);
  for (int i = 0; i < 3; ++i) s.r.obj.psSym[i] = test ? s.testPs[i] : s.prodPs[i];
  cpass::g_symTestC = test ? s.testCUav : nullptr;
  g_tq = TQ{};
  g_tq.on = sym, g_tq.view = 0, g_tq.cb = s.psCb[alpha ? 0 : 1], g_tq.acc = acc, g_tq.out = s.atlasUav;
  g_tq.finCb = s.finCb, g_tq.ldrSrv = s.ldrSrv, g_tq.ldr = s.r.rt, g_tq.ww = s.ww, g_tq.wh = s.wh, g_tq.cls = s.clsUav;
  cpass::g_obj.store(&s.r.obj);
  cpass::NoteLdr(0, s.W, s.H);
  cpass::FxNoteProj(c, 0, s.r.perView, 0);
  cpass::FxViewIn& vi = cpass::g_fxView[0];
  vi.depth = s.r.depthSrv, vi.depthTex = s.r.depth;
  vi.dv[0] = 0, vi.dv[1] = 0, vi.dv[2] = float(s.W), vi.dv[3] = float(s.H);
  vi.valid = true;
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  c->IASetInputLayout(nullptr);
  c->VSSetShader(s.r.vs, nullptr, 0);
  c->PSSetShader(s.r.dcsPs, nullptr, 0);
  ID3D11ShaderResourceView* s3[3] = {t0, s.r.lumSrv, s.r.bloomSrv};
  c->PSSetShaderResources(0, 3, s3);
  c->PSSetConstantBuffers(0, 1, &s.r.b0);
  c->PSSetConstantBuffers(cpass::kPsSymCbSlot, 1, &s.sentCb);   // DCS's (sentinels): put back after the draw
  c->PSSetShaderResources(cpass::kPsSymSrvSlot, 1, &s.sentSrv);
  c->OMSetRenderTargets(1, &s.r.rtv, nullptr);
  const D3D11_VIEWPORT vp{0, 0, float(s.W), float(s.H), 0, 1};
  c->RSSetViewports(1, &vp);
  c->RSSetState(s.r.rs);
  o.act = cpass::ToneDrawPre(c, 0);
  c->Draw(4, 0);
  cpass::ToneDrawPost(c, 0, o.act);
  ID3D11Buffer* cb = nullptr;
  ID3D11ShaderResourceView* sv = nullptr;
  ID3D11UnorderedAccessView* uv[2] = {};
  c->PSGetConstantBuffers(cpass::kPsSymCbSlot, 1, &cb);
  c->PSGetShaderResources(cpass::kPsSymSrvSlot, 1, &sv);
  c->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, 2, uv);
  o.slotsBack = cb == s.sentCb && sv == s.sentSrv && !uv[0] && !uv[1];
  Rel(cb), Rel(sv), Rel(uv[0]), Rel(uv[1]);
  ID3D11Buffer* nb = nullptr;
  ID3D11ShaderResourceView* ns = nullptr;
  c->PSSetConstantBuffers(cpass::kPsSymCbSlot, 1, &nb);
  c->PSSetShaderResources(cpass::kPsSymSrvSlot, 1, &ns);
  ID3D11RenderTargetView* none = nullptr;
  c->OMSetRenderTargets(1, &none, nullptr);
  o.done = g_tq.done;
  o.F = vfxrig::Read(s.r);
  c->CopyResource(s.atlasStage, s.atlas);
  c->CopyResource(s.testStage, s.testC);
  D3D11_MAPPED_SUBRESOURCE m{};
  o.E.assign(size_t(s.AW) * s.AH * 4, 0.0f);
  if (SUCCEEDED(c->Map(s.atlasStage, 0, D3D11_MAP_READ, 0, &m))) {
    for (int y = 0; y < s.AH; ++y) {
      const uint16_t* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + y * m.RowPitch);
      for (int x = 0; x < s.AW * 4; ++x) o.E[size_t(y) * s.AW * 4 + x] = H2F(row[x]);
    }
    c->Unmap(s.atlasStage, 0);
  }
  o.C.assign(size_t(s.AW) * s.AH * 4, 0.0f);
  if (SUCCEEDED(c->Map(s.testStage, 0, D3D11_MAP_READ, 0, &m))) {
    for (int y = 0; y < s.AH; ++y)
      memcpy(&o.C[size_t(y) * s.AW * 4], static_cast<const uint8_t*>(m.pData) + y * m.RowPitch, size_t(s.AW) * 16);
    c->Unmap(s.testStage, 0);
  }
  return o;
}

struct Cfg {
  const char* name;
  int mode;
  bool local;      // the tone map is per pixel (an independent reference frame applies)
  int fx;          // 0 none, 1 GazeDof, 2 GVision
  bool night;      // the sun below the horizon (night vision)
};

void Test(D3D_DRIVER_TYPE drv = D3D_DRIVER_TYPE_WARP) {
  Stage s;
  std::string err;
  const char* dname = drv == D3D_DRIVER_TYPE_HARDWARE ? " (hardware)" : "";
  if (!Make(s, err, drv)) {
    printf("     %s\n", err.c_str());
    Check(false, "R95 tone stage: WARP rig (device, the scene, the QV_SYM shaders and their test variant)");
    s.Release();
    vfxrig::Clear();
    return;
  }
  const auto keepQ = cpass::g_symQuery;
  const auto keepD = cpass::g_symDone;
  ID3D11DeviceContext* const keepCtx = fixdepth::g_s.ctx;
  ID3D11DeviceContext1* const keepCtx1 = fixdepth::g_s.ctx1;
  fixdepth::g_s.ctx = s.r.ctx;
  fixdepth::g_s.ctx1 = s.ctx1;
  cpass::g_symQuery = &Query;
  cpass::g_symDone = &Done;
  cpass::g_symPsReady = 1;
  const Cfg cfgs[] = {{"Mode 0 (stock replica)", 0, true, 0, false},
                      {"Fix", 1, true, 0, false},
                      {"Enhanced (glare)", 2, false, 0, false},
                      {"Enhanced at night (night vision, its low-pass)", 2, false, 0, true},
                      {"Mode 0 + GazeDof (focused near: the far HUD background blurred)", 0, false, 1, false},
                      {"Mode 0 + GVision", 0, true, 2, false}};
  bool allOk = true;
  for (const Cfg& cf : cfgs) {
    cpass::g_rt = cpass::Rt{};
    cpass::g_fault = false;
    vfxrig::Clear();
    cpass::g_mode = cf.mode;
    cpass::g_sunValid = cf.night;
    cpass::g_sunY = cf.night ? -0.25f : 0.5f;
    cgrade::ModeParams p = cf.mode == 2 ? cgrade::EnhancedDefaults() : cgrade::FixDefaults();
    p.dither = 0;                              // frame-independent (the ordered dither): the reference frame applies
    p.adaptBright = p.adaptDark = 0.0001;      // the adaptation snaps (no drift between the frames)
    p.meterFocus = 0;
    if (cf.night) p.nightVision = 1, p.nvBlur = 0.5;
    if (cf.mode) cmtest::SetParams(p, cf.mode);
    if (cf.fx == 1) {
      vfxrig::FxSet f;
      f.dof = true, f.focusD = 1 / 0.7, f.gain = 1, f.strength = 1, f.maxArcmin = 20, f.guard = 0, f.pupil = 4;
      vfxrig::Apply(f);
    } else if (cf.fx == 2) {
      vfxrig::FxSet f;
      f.gv = true, f.loss = 0.6, f.gvStrength = 1, f.gvMode = 2;
      vfxrig::Apply(f);
    }
    for (int k = 0; k < 3; ++k) Run(s, true, false, s.postSrv, s.accSrv, true);  // settle the state
    const Out A = Run(s, true, false, s.postSrv, s.accSrv, true);
    const Out B = Run(s, true, true, s.postSrv, s.accSrv, true);
    const Out R = cf.local ? Run(s, true, false, s.preRefSrv, s.accZeroSrv, true) : Out{};
    const Out N = Run(s, true, false, s.postSrv, s.accSrv, false);  // the alpha switch off
    int zeroBad = 0, cBad = 0, idBad = 0, prodBad = 0, refFar = 0, refBytes = 0, aBad = 0, sentBad = 0, a0Bad = 0;
    int hud = 0, mfdN = 0;
    double idWorst = 0, refWorst = 0, eMax = 0;
    for (int j = 0; j < s.wh; ++j)
      for (int i = 0; i < s.ww; ++i) {
        const size_t wi = size_t(j) * s.ww + i;
        const size_t at = (size_t(s.ay + j) * s.AW + s.ax + i) * 4;
        const size_t tp = (size_t(s.wy + j) * s.W + s.wx + i) * 4;
        const bool dz = s.D[wi * 4] == 0 && s.D[wi * 4 + 1] == 0 && s.D[wi * 4 + 2] == 0 && s.D[wi * 4 + 3] == 0;
        mfdN += s.mfd[wi];
        if (dz) {
          for (int k = 0; k < 4; ++k) zeroBad += A.E[at + k] != 0.0f || std::signbit(A.E[at + k]);
          for (int k = 0; k < 3; ++k) cBad += static_cast<int>(std::lround(B.C[at + k] * 255.0)) != B.F[tp + k];
          cBad += B.C[at + 3] != 0.0f;
          continue;
        }
        ++hud;
        for (int k = 0; k < 3; ++k) {
          const double e = B.E[at + k];
          const double cDec = Dec8(static_cast<uint8_t>(std::lround(B.C[at + k] * 255.0)));
          const double d = std::fabs(Dec8(B.F[tp + k]) - e - cDec);
          idWorst = std::max(idWorst, d);
          eMax = std::max(eMax, std::fabs(e));
          // dec(C) passes the atlas as a half float before the finalize: one ulp of the larger of dec(C) / E; and
          // re-encoded to the swapchain's byte, C's own byte exactly
          idBad += d > ETol(std::max(std::fabs(e), cDec));
          const double lin = Dec8(B.F[tp + k]) - e;
          const int cb = static_cast<int>(std::lround(B.C[at + k] * 255.0));
          idBad += std::fabs(lin - cDec) > 0.5 * std::fabs(Dec8(static_cast<uint8_t>(std::min(cb + 1, 255))) -
                                                           Dec8(static_cast<uint8_t>(std::max(cb - 1, 0)))) * 0.5;
          if (cf.local) {
            const double r = std::fabs(Dec8(A.F[tp + k]) - A.E[at + k] - Dec8(R.F[tp + k]));
            refWorst = std::max(refWorst, r);
            // re-encoded to the swapchain's byte: R's own byte (within one code: the GPU rounds R itself)
            const double lin = Dec8(A.F[tp + k]) - A.E[at + k];
            int best = 0;
            double bd = 1e9;
            for (int b = 0; b < 256; ++b)
              if (std::fabs(Dec8(static_cast<uint8_t>(b)) - lin) < bd) bd = std::fabs(Dec8(static_cast<uint8_t>(b)) - lin), best = b;
            refBytes += best != R.F[tp + k];
            refFar += std::abs(best - int(R.F[tp + k])) > 1;
          }
        }
        for (int k = 0; k < 4; ++k) prodBad += A.E[at + k] != B.E[at + k];
        for (int k = 0; k < 3; ++k) prodBad += A.F[tp + k] != B.F[tp + k];
        // the alpha: the added alpha where the alpha before was 0, else 0; 0 with the switch off
        const float wantA = s.bad[wi] ? 0.0f : Rh(std::min(1.0f, std::max(0.0f, s.D[wi * 4 + 3])));
        aBad += std::fabs(A.E[at + 3] - wantA) > 1e-3f;
        a0Bad += N.E[at + 3] != 0.0f;
      }
    for (int y = 0; y < s.AH; ++y)
      for (int x = 0; x < s.AW; ++x) {
        if (x >= s.ax && x < s.ax + s.ww && y >= s.ay && y < s.ay + s.wh) continue;
        for (int k = 0; k < 4; ++k) sentBad += A.E[(size_t(y) * s.AW + x) * 4 + k] != -7.0f;
      }
    const bool hwDev = drv == D3D_DRIVER_TYPE_HARDWARE;
    const bool ok = A.act == 1 && A.done == 0 && B.done == 0 && A.slotsBack && B.slotsBack && zeroBad == 0 &&
                    (hwDev || cBad == 0) && idBad == 0 && prodBad == 0 &&
                    (!cf.local || (refFar == 0 && (hwDev || refBytes == 0))) && aBad == 0 &&
                    a0Bad == 0 && sentBad == 0 && hud > 500 && mfdN > 100 && eMax > 0.05;
    allOk = allOk && ok;
    char what[700];
    sprintf_s(what,
              "R95 tone stage%s, %s: E == 0 exactly where nothing of type 1 / 2 changed the HDR (MFD %d texels; bad %d; the "
              "shader's round-half-up of F vs the GPU's own UNORM bytes differ at %d channels: why E uses the bytes); "
              "dec(F) - E == dec(C) at %d symbol texels (worst %.2e, max |E| %.3f, %d over E's half-float "
              "rounding); production == test variant (%d differ)%s; alpha %d / switch off %d wrong; atlas outside the "
              "window %d touched; DCS's slots back",
              dname, cf.name, mfdN, zeroBad, cBad, hud, idWorst, eMax, idBad, prodBad,
              cf.local ? "; against the frame without the symbols" : " (non-local: no independent reference)", aBad, a0Bad,
              sentBad);
    if (cf.local) {
      char more[160];
      sprintf_s(more, " [reference: worst %.2e; re-encoded bytes differ %d (exact on WARP), by more than one code %d]",
                refWorst, refBytes, refFar);
      strcat_s(what, more);
    }
    Check(ok, what);
  }
  // The query refused: our shader is not bound in Mode 0 (DCS's own draw), nothing written.
  {
    cpass::g_rt = cpass::Rt{};
    vfxrig::Clear();
    cpass::g_mode = 0;
    const Out O = Run(s, false, false, s.postSrv, s.accSrv, true);
    int touched = 0;
    for (float e : O.E) touched += e != -7.0f;
    Check(O.act == 0 && O.done == -1 && touched == 0 && O.F[0] == 255 && O.slotsBack,
          "R95 tone stage: no E due (the query refuses): Mode 0 leaves DCS's own tone map (nothing swapped, nothing "
          "written, DCS's slots as they were)");
  }
  cpass::g_symTestC = nullptr;
  fixdepth::g_s.ctx = keepCtx;
  fixdepth::g_s.ctx1 = keepCtx1;
  cpass::g_symQuery = keepQ;
  cpass::g_symDone = keepD;
  cpass::g_symPsReady = 0;
  cpass::g_obj.store(nullptr);
  cpass::g_mode = 0;
  cpass::g_sunValid = false;
  cpass::g_rt = cpass::Rt{};
  vfxrig::Clear();
  for (int i = 0; i < 4; ++i) cpass::g_fxView[i] = cpass::FxViewIn{};
  s.Release();
  (void)allOk;
}

}  // namespace r95tone
