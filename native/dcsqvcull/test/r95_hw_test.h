// R95: the symbology export's GPU cost at DCS's real view sizes (QV_R95_HW=1: the hardware device, DCS's GPU; else
// WARP, timing printed but not judged). Per op, GPU timestamps (median of 15): the run-start copy of the window, the
// accumulation dispatch (first run), the scan (start + end, whole view), and the tone-map draw through color_pass.h
// (the stock replica with the QV_SYM shader) with an empty window (the base) and with the window and ~8 % symbol
// texels (the second evaluation's extra). Windows: periphery 215 x 215 in 1040 x 1028, focus 959 x 959 in
// 2028 x 2004 (R94's worst measured needs: the F/A-18 HUD + JHMCS).
#pragma once

namespace r95hw {

using namespace r95test;

struct Gpu {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* c = nullptr;
  ID3D11Query* dj = nullptr;
  ID3D11Query* ts[2] = {};
  bool Make(ID3D11Device* d, ID3D11DeviceContext* ctx) {
    dev = d, c = ctx;
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, qt{D3D11_QUERY_TIMESTAMP, 0};
    return SUCCEEDED(d->CreateQuery(&qd, &dj)) && SUCCEEDED(d->CreateQuery(&qt, &ts[0])) && SUCCEEDED(d->CreateQuery(&qt, &ts[1]));
  }
  void Release() { Rel(dj), Rel(ts[0]), Rel(ts[1]); }
  template <class F>
  double Us(F fn, int reps = 15) {
    std::vector<double> v;
    for (int i = 0; i < reps; ++i) {
      c->Begin(dj);
      c->End(ts[0]);
      fn();
      c->End(ts[1]);
      c->End(dj);
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
      while (c->GetData(dj, &d, sizeof(d), 0) == S_FALSE) YieldProcessor();
      UINT64 a = 0, b = 0;
      while (c->GetData(ts[0], &a, sizeof(a), 0) == S_FALSE) YieldProcessor();
      while (c->GetData(ts[1], &b, sizeof(b), 0) == S_FALSE) YieldProcessor();
      if (!d.Disjoint && d.Frequency) v.push_back((b - a) * 1e6 / double(d.Frequency));
    }
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  }
};

struct ViewRes {
  UINT W = 0, H = 0, win = 0;
  ID3D11Texture2D *s0 = nullptr, *post = nullptr, *ldr = nullptr;
  ID3D11ShaderResourceView *postSrv = nullptr, *ldrSrv = nullptr;
  ID3D11RenderTargetView* ldrRtv = nullptr;
  ID3D11Buffer* cb = nullptr;      // the compute constants
  ID3D11Buffer* psCb[2] = {};      // the tone map's: empty window / the window
  void Release() { Rel(s0), Rel(post), Rel(ldr), Rel(postSrv), Rel(ldrSrv), Rel(ldrRtv), Rel(cb), Rel(psCb[0]), Rel(psCb[1]); }
};

bool MakeView(ID3D11Device* dev, ViewRes& r, UINT W, UINT H, UINT win, UINT ax) {
  r.W = W, r.H = H, r.win = win;
  std::vector<uint16_t> s0(size_t(W) * H * 4), post;
  for (UINT y = 0; y < H; ++y)
    for (UINT x = 0; x < W; ++x) {
      const bool chk = ((x / 7 + y / 7) % 2) != 0;
      const float c[4] = {chk ? 0.08f : 0.03f, chk ? 0.11f : 0.05f, 0.03f, 0.0f};
      for (int k = 0; k < 4; ++k) s0[(size_t(y) * W + x) * 4 + k] = F2H(c[k]);
    }
  post = s0;
  const UINT x0 = (W - win) / 2, y0 = (H - win) / 2;
  for (UINT y = y0; y < y0 + win; ++y)
    for (UINT x = x0; x < x0 + win; ++x) {
      const UINT i = x - x0, j = y - y0;
      const float cov = (i % 25 < 2 || j % 23 < 1) ? 1.0f : 0.0f;  // ~8 % of the window's texels
      if (cov == 0) continue;
      const float h[3] = {0.2f, 5.0f, 0.3f};
      for (int k = 0; k < 3; ++k) {
        const size_t at = (size_t(y) * W + x) * 4 + k;
        post[at] = F2H(H2F(s0[at]) * (1 - cov) + h[k] * cov);
      }
      post[(size_t(y) * W + x) * 4 + 3] = F2H(cov);
    }
  D3D11_TEXTURE2D_DESC td{};
  td.Width = W, td.Height = H, td.MipLevels = td.ArraySize = 1, td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  td.SampleDesc.Count = 1, td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  D3D11_SUBRESOURCE_DATA a{s0.data(), W * 8, 0}, b{post.data(), W * 8, 0};
  if (FAILED(dev->CreateTexture2D(&td, &a, &r.s0)) || FAILED(dev->CreateTexture2D(&td, &b, &r.post)) ||
      FAILED(dev->CreateShaderResourceView(r.post, nullptr, &r.postSrv)))
    return false;
  D3D11_TEXTURE2D_DESC ld = td;
  ld.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS, ld.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  D3D11_RENDER_TARGET_VIEW_DESC rvd{};
  rvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM, rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  D3D11_SHADER_RESOURCE_VIEW_DESC lv{};
  lv.Format = DXGI_FORMAT_R8G8B8A8_UNORM, lv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, lv.Texture2D.MipLevels = 1;
  if (FAILED(dev->CreateTexture2D(&ld, nullptr, &r.ldr)) || FAILED(dev->CreateRenderTargetView(r.ldr, &rvd, &r.ldrRtv)) ||
      FAILED(dev->CreateShaderResourceView(r.ldr, &lv, &r.ldrSrv)))
    return false;
  const UINT tx = (W + 7) / 8, ty = (H + 7) / 8;
  uint32_t p[32] = {x0, y0, win, win, ax, 0, 0, 15, 0, 0, W, H, tx, ty, 0, 0};
  const float fov[4] = {-1.49f, 0.95f, 1.19f, -1.22f};  // R97: the tracking-space angles of the type 1 texels' groups
  memcpy(p + 16, fov, sizeof(fov));
  float q[4], rot[12];
  r95test::QuatYawPitch(20, -3, 0, q);
  fixdepth::cmask::sym::RotRows(q, rot);
  memcpy(p + 20, rot, sizeof(rot));
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = fixdepth::cmask::sym::kCbBytes, bd.Usage = D3D11_USAGE_DEFAULT, bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  D3D11_SUBRESOURCE_DATA pd{p, 0, 0};
  if (FAILED(dev->CreateBuffer(&bd, &pd, &r.cb))) return false;
  bd.ByteWidth = 64;
  const int32_t q0[16] = {int32_t(x0), int32_t(y0), 0, 0, int32_t(ax), 0, 1, 0};
  const int32_t q1[16] = {int32_t(x0), int32_t(y0), int32_t(win), int32_t(win), int32_t(ax), 0, 1, 0};
  D3D11_SUBRESOURCE_DATA d0{q0, 0, 0}, d1{q1, 0, 0};
  return SUCCEEDED(dev->CreateBuffer(&bd, &d0, &r.psCb[0])) && SUCCEEDED(dev->CreateBuffer(&bd, &d1, &r.psCb[1]));
}

struct HQ {
  ID3D11Buffer* cb = nullptr;
  ID3D11ShaderResourceView* acc = nullptr;
  ID3D11UnorderedAccessView* out = nullptr;
} g_hq;
bool HwQuery(int, ID3D11Resource*, cpass::SymBind* o) {
  o->cb = g_hq.cb, o->acc = g_hq.acc, o->out = g_hq.out;
  return true;
}
void HwDone(int, bool) {}

void Bench() {
  const char* e = getenv("QV_R95_HW");
  const bool hw = e && atoi(e) != 0;
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* c = nullptr;
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1};
  if (FAILED(D3D11CreateDevice(nullptr, hw ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 1,
                               D3D11_SDK_VERSION, &dev, nullptr, &c))) {
    Check(!hw, "R95 cost bench: a hardware 11.1 device (QV_R95_HW=1)");
    return;
  }
  if (!hw) {  // WARP: only that it runs (its timing says nothing about a GPU)
    Rel(c), Rel(dev);
    printf("     (R95 cost bench: set QV_R95_HW=1 for the GPU figures)\n");
    return;
  }
  {
    IDXGIDevice* dx = nullptr;
    IDXGIAdapter* ad = nullptr;
    DXGI_ADAPTER_DESC d{};
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dx))) && dx &&
        SUCCEEDED(dx->GetAdapter(&ad)) && ad)
      ad->GetDesc(&d);
    printf("     (R95 cost bench on %ls)\n", d.Description);
    Rel(ad), Rel(dx);
  }
  namespace sy = fixdepth::cmask::sym;
  const bool shaders = fixdepth::cmask::Compile() && sy::MakeBase(dev);
  ViewRes peri, focus;
  const bool views = MakeView(dev, peri, 1040, 1028, 215, 0) && MakeView(dev, focus, 2028, 2004, 959, 512);
  // The symbology's own textures (as the builder makes them for the default capacities).
  ID3D11Texture2D *pre = nullptr, *acc = nullptr, *sig = nullptr, *cls = nullptr, *atl = nullptr;
  ID3D11ShaderResourceView *preSrv = nullptr, *accSrv = nullptr;
  ID3D11UnorderedAccessView *accUav = nullptr, *sigUav = nullptr, *clsUav = nullptr, *atlUav = nullptr;
  {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = 1024, d.MipLevels = d.ArraySize = 1, d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1, d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    dev->CreateTexture2D(&d, nullptr, &pre);
    if (pre) dev->CreateShaderResourceView(pre, nullptr, &preSrv);
    d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT, d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    dev->CreateTexture2D(&d, nullptr, &acc);
    if (acc) dev->CreateShaderResourceView(acc, nullptr, &accSrv), dev->CreateUnorderedAccessView(acc, nullptr, &accUav);
    d.Width = d.Height = 512, d.Format = DXGI_FORMAT_R32_UINT, d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    dev->CreateTexture2D(&d, nullptr, &sig);
    if (sig) dev->CreateUnorderedAccessView(sig, nullptr, &sigUav);
    d.Width = 2560, d.Height = 1024, d.Format = DXGI_FORMAT_R8_UINT;
    dev->CreateTexture2D(&d, nullptr, &cls);
    if (cls) dev->CreateUnorderedAccessView(cls, nullptr, &clsUav);
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dev->CreateTexture2D(&d, nullptr, &atl);
    if (atl) dev->CreateUnorderedAccessView(atl, nullptr, &atlUav);
  }
  // The tone map: color_pass.h's objects and the QV_SYM shaders on this device, DCS-like state.
  vfxrig::Rig tr;  // (only its shaders, buffers and rasterizer state: made on this device below)
  cpass::Objects obj;
  std::string err;
  cpass::Blobs blobs;
  bool tone = cpass::CompileAll(blobs, err) && obj.Create(dev, blobs, err) && cpass::CompileSym(blobs, err);
  for (int i = 0; i < 3 && tone; ++i)
    tone = SUCCEEDED(dev->CreatePixelShader(blobs.psSym[i].data(), blobs.psSym[i].size(), nullptr, &obj.psSym[i]));
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* dcsPs = nullptr;
  ID3D11RasterizerState* rs = nullptr;
  ID3D11Buffer *lum = nullptr, *b0 = nullptr;
  ID3D11ShaderResourceView *lumSrv = nullptr, *bloomSrv = nullptr;
  ID3D11Texture2D* bloom = nullptr;
  if (tone) {
    std::vector<uint8_t> b;
    tone = vfxrig::CompileTo(vfxrig::kVs, "vs_5_0", b, err) && SUCCEEDED(dev->CreateVertexShader(b.data(), b.size(), nullptr, &vs)) &&
           vfxrig::CompileTo(vfxrig::kStockPs, "ps_5_0", b, err) && SUCCEEDED(dev->CreatePixelShader(b.data(), b.size(), nullptr, &dcsPs));
    D3D11_RASTERIZER_DESC rsd{};
    rsd.FillMode = D3D11_FILL_SOLID, rsd.CullMode = D3D11_CULL_NONE, rsd.DepthClipEnable = TRUE;
    tone = tone && SUCCEEDED(dev->CreateRasterizerState(&rsd, &rs));
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 64, bd.BindFlags = D3D11_BIND_SHADER_RESOURCE, bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 8;
    const float ld[16] = {0, 1, 0.05f, 0, 0, 0, 0, 0, 0.05f, 0.0025f};
    D3D11_SUBRESOURCE_DATA ldd{ld, 0, 0};
    tone = tone && SUCCEEDED(dev->CreateBuffer(&bd, &ldd, &lum)) && SUCCEEDED(dev->CreateShaderResourceView(lum, nullptr, &lumSrv));
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = 256, cd.Usage = D3D11_USAGE_DYNAMIC, cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    tone = tone && SUCCEEDED(dev->CreateBuffer(&cd, nullptr, &b0));
    D3D11_MAPPED_SUBRESOURCE m{};
    if (tone && SUCCEEDED(c->Map(b0, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
      cgrade::DcsTone t;
      cgrade::PackDcsTone(t, static_cast<float*>(m.pData));
      c->Unmap(b0, 0);
    }
    D3D11_TEXTURE2D_DESC bt{};
    bt.Width = bt.Height = 1, bt.MipLevels = bt.ArraySize = 1, bt.Format = DXGI_FORMAT_R8_UNORM, bt.SampleDesc.Count = 1;
    bt.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const uint8_t z[4] = {};
    D3D11_SUBRESOURCE_DATA bz{z, 4, 0};
    tone = tone && SUCCEEDED(dev->CreateTexture2D(&bt, &bz, &bloom)) && SUCCEEDED(dev->CreateShaderResourceView(bloom, nullptr, &bloomSrv));
  }
  Gpu g;
  const bool ok = shaders && views && tone && g.Make(dev, c) && accUav && sigUav && clsUav && atlUav && preSrv;
  Check(ok, "R95 cost bench: the shaders, views and textures on the GPU");
  double copyUs[2] = {}, accUs[2] = {}, scanUs[2] = {}, toneBase[2] = {}, toneSym[2] = {}, finUs[2] = {};
  if (ok) {
    const auto keepQ = cpass::g_symQuery;
    const auto keepD = cpass::g_symDone;
    cpass::g_symQuery = &HwQuery;
    cpass::g_symDone = &HwDone;
    cpass::g_symPsReady = 1;
    cpass::g_obj.store(&obj);
    cpass::g_rt = cpass::Rt{};
    cpass::g_mode = 0;
    ViewRes* vr[2] = {&peri, &focus};
    for (int k = 0; k < 2; ++k) {
      ViewRes& v = *vr[k];
      const UINT x0 = (v.W - v.win) / 2, y0 = (v.H - v.win) / 2;
      const D3D11_BOX box{x0, y0, 0, x0 + v.win, y0 + v.win, 1};
      // The run start: the window of the HDR before the HUD's records (s0) copied.
      copyUs[k] = g.Us([&] { c->CopySubresourceRegion(pre, 0, 0, 0, 0, v.s0, 0, &box); });
      // The run end: the accumulation over the window against the HDR after (post); first-run variant.
      ID3D11ShaderResourceView* srv[2] = {v.postSrv, preSrv};
      ID3D11UnorderedAccessView* uav[3] = {accUav, sy::g_b.statUav, clsUav};
      auto bindCs = [&](ID3D11ComputeShader* cs) {
        c->CSSetShader(cs, nullptr, 0);
        c->CSSetConstantBuffers(0, 1, &v.cb);
        c->CSSetShaderResources(0, 2, srv);
        c->CSSetUnorderedAccessViews(0, 3, uav, nullptr);
      };
      bindCs(sy::g_b.cs[0]);
      accUs[k] = g.Us([&] { c->Dispatch((v.win + 7) / 8, (v.win + 7) / 8, 1); });
      // The scan: signatures at the run's start and end over the whole view.
      ID3D11UnorderedAccessView* su[3] = {sigUav, sy::g_b.statUav, nullptr};
      const UINT tg = ((v.W + 7) / 8 + 7) / 8, th = ((v.H + 7) / 8 + 7) / 8;
      scanUs[k] = g.Us([&] {
        c->CSSetShader(sy::g_b.cs[4], nullptr, 0);
        c->CSSetConstantBuffers(0, 1, &v.cb);
        c->CSSetShaderResources(0, 1, &v.postSrv);
        c->CSSetUnorderedAccessViews(0, 3, su, nullptr);
        c->Dispatch(tg, th, 1);
        c->CSSetShader(sy::g_b.cs[5], nullptr, 0);
        c->Dispatch(tg, th, 1);
      });
      ID3D11UnorderedAccessView* nu[3] = {};
      ID3D11ShaderResourceView* ns[2] = {};
      c->CSSetUnorderedAccessViews(0, 3, nu, nullptr);
      c->CSSetShaderResources(0, 2, ns);
      bindCs(sy::g_b.cs[0]);  // the accumulator as the tone map reads it
      c->Dispatch((v.win + 7) / 8, (v.win + 7) / 8, 1);
      c->CSSetUnorderedAccessViews(0, 3, nu, nullptr);
      c->CSSetShaderResources(0, 2, ns);
      // The tone map through color_pass.h: the QV_SYM stock replica, an empty window (the base) / the window.
      cpass::NoteLdr(k == 0 ? 0 : 2, v.W, v.H);
      for (int w = 0; w < 2; ++w) {
        g_hq.cb = v.psCb[w], g_hq.acc = accSrv, g_hq.out = atlUav;
        const double us = g.Us([&] {
          c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
          c->IASetInputLayout(nullptr);
          c->VSSetShader(vs, nullptr, 0);
          c->PSSetShader(dcsPs, nullptr, 0);
          ID3D11ShaderResourceView* s3[3] = {v.postSrv, lumSrv, bloomSrv};
          c->PSSetShaderResources(0, 3, s3);
          c->PSSetConstantBuffers(0, 1, &b0);
          c->OMSetRenderTargets(1, &v.ldrRtv, nullptr);
          const D3D11_VIEWPORT vp{0, 0, float(v.W), float(v.H), 0, 1};
          c->RSSetViewports(1, &vp);
          c->RSSetState(rs);
          const int act = cpass::ToneDrawPre(c, k == 0 ? 0 : 2);
          c->Draw(4, 0);
          cpass::ToneDrawPost(c, k == 0 ? 0 : 2, act);
          ID3D11RenderTargetView* none = nullptr;
          c->OMSetRenderTargets(1, &none, nullptr);
        });
        (w ? toneSym : toneBase)[k] = us;
      }
      // The finalize after the draw: E = dec(F) - dec(C) from the target's bytes over the window.
      {
        ID3D11ShaderResourceView* fs[2] = {v.ldrSrv, accSrv};
        ID3D11UnorderedAccessView* fu[3] = {atlUav, clsUav, nullptr};  // R97: the class atlas first
        c->CSSetShader(sy::g_b.cs[7], nullptr, 0);
        c->CSSetConstantBuffers(0, 1, &v.cb);
        c->CSSetShaderResources(0, 2, fs);
        c->CSSetUnorderedAccessViews(0, 3, fu, nullptr);
        finUs[k] = g.Us([&] { c->Dispatch((v.win + 7) / 8, (v.win + 7) / 8, 1); });
        c->CSSetUnorderedAccessViews(0, 3, nu, nullptr);
        c->CSSetShaderResources(0, 2, ns);
      }
    }
    cpass::g_symQuery = keepQ;
    cpass::g_symDone = keepD;
    cpass::g_symPsReady = 0;
    cpass::g_obj.store(nullptr);
    cpass::g_rt = cpass::Rt{};
    // Four views: per view one HUD run (copy + accumulation) and the tone map's extra; the right eye also a helmet run;
    // the scan once per 64 frames per view (and while learning).
    const double perFrame = 2 * (copyUs[0] + accUs[0] + toneSym[0] - toneBase[0] + finUs[0]) +
                            2 * (copyUs[1] + accUs[1] + toneSym[1] - toneBase[1] + finUs[1]) + (copyUs[0] + accUs[0]) +
                            (copyUs[1] + accUs[1]) + (2 * scanUs[0] + 2 * scanUs[1]) / 64.0;
    printf("     (R95 GPU per op, us: periphery 215^2 in 1040x1028: copy %.2f, accumulation %.2f, scan (start + end) "
           "%.2f, tone map %.2f -> %.2f with E (+%.2f), finalize %.2f; focus 959^2 in 2028x2004: copy %.2f, "
           "accumulation %.2f, scan %.2f, tone map %.2f -> %.2f (+%.2f), finalize %.2f. Four views (HUD in each, helmet "
           "in the right eye, scans amortized): %.1f us / frame)\n",
           copyUs[0], accUs[0], scanUs[0], toneBase[0], toneSym[0], toneSym[0] - toneBase[0], finUs[0], copyUs[1],
           accUs[1], scanUs[1], toneBase[1], toneSym[1], toneSym[1] - toneBase[1], finUs[1], perFrame);
    char what[300];
    sprintf_s(what, "R95 cost (GPU, the worst measured windows): %.1f us / frame for four views (target <= 150 us)", perFrame);
    Check(perFrame > 0 && perFrame <= 150.0, what);
  }
  g.Release();
  sy::ReleaseBase();
  for (auto*& p : obj.psSym) Rel(p);
  obj.Release();
  peri.Release(), focus.Release();
  Rel(pre), Rel(acc), Rel(sig), Rel(cls), Rel(atl), Rel(preSrv), Rel(accSrv), Rel(accUav), Rel(sigUav), Rel(clsUav), Rel(atlUav);
  Rel(vs), Rel(dcsPs), Rel(rs), Rel(lum), Rel(b0), Rel(lumSrv), Rel(bloomSrv), Rel(bloom);
  (void)tr;
  Rel(c), Rel(dev);
}

}  // namespace r95hw

namespace r95test {
inline void Run() {
  printf("-- R95 symbology export --\n");
  TestContract();
  TestPure();
  TestOvlGlue();
  r95tone::Test();
  r95pipe::Test(D3D_DRIVER_TYPE_WARP, false);
  const char* e = getenv("QV_R95_HW");
  if (e && atoi(e) != 0) {
    r95tone::Test(D3D_DRIVER_TYPE_HARDWARE);  // the identities with DCS's GPU's rounding and codegen
    r95pipe::Test(D3D_DRIVER_TYPE_HARDWARE, true);  // the render thread's cost with a real driver
    r95hw::Bench();
  }
}
}  // namespace r95test
#include "r97_test.h"  // R97: the fixes after R96 (run after R95's with QV_R95_ONLY too)
