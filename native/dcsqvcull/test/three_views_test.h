// 0.7 three views, the fill inside the engine (src/three_views_fill.h) on WARP: a synthetic pair of periphery views
// with DCS's frusta (left -56.8..+45.9 deg, right mirrored, 64 mm apart), a textured wall at 10 m and a cockpit panel at
// 0.6 m seen by both eyes (stencil COCKPIT). The right view's targets hold what a culled DCS view holds (sky colour,
// far depth, a sky motion vector) outside its strip and its cockpit. Checks: the cockpit and the strip untouched (colour,
// depth, motion), the filled colour against the wall rendered directly, the filled depth the wall's, the motion
// vectors the left view's, the panel not leaking into the world away from its edge, and DCS's pipeline state as it was.
#pragma once

namespace tvtest {

struct Cam {
  double l, r, u, d, x;
};
constexpr double kNear = 0.05, kWallZ = 10.0, kOrigin = 2000.0;
double Wall(double X, double Y) { return 0.5 + 0.35 * std::sin(X * 2.0) * std::cos(Y * 1.5); }
void Proj(const Cam& c, float* m) {
  for (int i = 0; i < 16; ++i) m[i] = 0;
  m[0] = float(2 / (c.r - c.l));
  m[5] = float(2 / (c.u - c.d));
  m[8] = float(-(c.r + c.l) / (c.r - c.l));
  m[9] = float(-(c.u + c.d) / (c.u - c.d));
  m[11] = 1.f;
  m[14] = float(kNear);
}
uint16_t Half(float f) {
  uint32_t x;
  memcpy(&x, &f, 4);
  const uint32_t s = (x >> 16) & 0x8000, e = (x >> 23) & 0xff, m = x & 0x7fffff;
  if (e < 113) return uint16_t(s);
  return uint16_t(s | ((e - 112) << 10) | (m >> 13));
}
float FromHalf(uint16_t h) {
  const uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1f, mm = h & 0x3ff;
  uint32_t x = e ? (s | ((e + 112) << 23) | (mm << 13)) : s;
  float f;
  memcpy(&f, &x, 4);
  return f;
}

void Run() {
  printf("\n-- 0.7 three views: the fill inside the engine (WARP) --\n");
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
    Check(false, "three views fill: WARP device");
    return;
  }
  const double pi = 3.14159265358979;
  const double t1 = std::tan(56.8 * pi / 180), t2 = std::tan(45.9 * pi / 180), tv = std::tan(50.0 * pi / 180);
  const Cam L{-t1, t2, tv, -tv, kOrigin - 0.032}, R{-t2, t1, tv, -tv, kOrigin + 0.032};
  const UINT W = 312, H = 308;  // half of DCS's 624x616 periphery render size
  const float strip = 0.55f;
  auto panel = [&](const Cam& c, double tx, double ty) {
    const double X = c.x + tx * 0.6 - L.x;
    return X > -0.35 * 0.6 && X < -0.05 * 0.6 && ty > -0.5 && ty < -0.2;
  };
  auto ownCockpit = [&](double tx, double ty) { return tx > 0.6 && tx < 0.8 && ty > -0.6 && ty < -0.3; };
  // Left: colour (RGBA16F), motion (RG16F, constant), depth+stencil (R32G8X24). Right: what a culled DCS view holds.
  std::vector<uint16_t> colL(W * H * 4), colR(W * H * 4), mvL(W * H * 2), mvR(W * H * 2);
  std::vector<uint8_t> depL(W * H * 8), depR(W * H * 8);
  for (UINT y = 0; y < H; ++y)
    for (UINT x = 0; x < W; ++x) {
      const size_t i = y * W + x;
      for (int e = 0; e < 2; ++e) {
        const Cam& c = e ? R : L;
        const double u = (x + 0.5) / W, v = (y + 0.5) / H;
        const double tx = c.l + u * (c.r - c.l), ty = c.u + v * (c.d - c.u);
        bool ck = panel(c, tx, ty) || (e && ownCockpit(tx, ty));
        double z = ck ? 0.6 : kWallZ, val = ck ? (panel(c, tx, ty) ? 1.0 : 0.0) : Wall(c.x + tx * kWallZ - kOrigin, ty * kWallZ);
        const bool culled = e && !ck && (u * 2 - 1) < strip;
        if (culled) val = 0.25, z = 1e6;  // sky
        uint16_t* p = &(e ? colR : colL)[i * 4];
        p[0] = p[1] = p[2] = Half(float(val)), p[3] = Half(1.f);
        uint16_t* m = &(e ? mvR : mvL)[i * 2];
        m[0] = Half(e && culled ? 9.f : 0.25f), m[1] = Half(e && culled ? 9.f : -0.5f);
        uint8_t* d = &(e ? depR : depL)[i * 8];
        const float df = float(kNear / z);
        memcpy(d, &df, 4);
        d[4] = ck ? 0x28 : 0x08, d[5] = d[6] = d[7] = 0;
      }
    }
  auto tex = [&](DXGI_FORMAT f, const void* data, UINT pitch, UINT bind) {
    D3D11_TEXTURE2D_DESC d{W, H, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
    D3D11_SUBRESOURCE_DATA s{data, pitch, 0};
    ID3D11Texture2D* t = nullptr;
    dev->CreateTexture2D(&d, &s, &t);
    return t;
  };
  ID3D11Texture2D* tColL = tex(DXGI_FORMAT_R16G16B16A16_FLOAT, colL.data(), W * 8, D3D11_BIND_SHADER_RESOURCE);
  ID3D11Texture2D* tMvL = tex(DXGI_FORMAT_R16G16_FLOAT, mvL.data(), W * 4, D3D11_BIND_SHADER_RESOURCE);
  ID3D11Texture2D* tDepL = tex(DXGI_FORMAT_R32G8X24_TYPELESS, depL.data(), W * 8, D3D11_BIND_SHADER_RESOURCE);
  ID3D11Texture2D* tColR = tex(DXGI_FORMAT_R16G16B16A16_FLOAT, colR.data(), W * 8, D3D11_BIND_RENDER_TARGET);
  ID3D11Texture2D* tMvR = tex(DXGI_FORMAT_R16G16_FLOAT, mvR.data(), W * 4, D3D11_BIND_RENDER_TARGET);
  ID3D11Texture2D* tDepR =
      tex(DXGI_FORMAT_R32G8X24_TYPELESS, depR.data(), W * 8, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
  if (!tColL || !tMvL || !tDepL || !tColR || !tMvR || !tDepR) {
    Check(false, "three views fill: test textures");
    return;
  }
  ID3D11RenderTargetView *rCol = nullptr, *rMv = nullptr;
  dev->CreateRenderTargetView(tColR, nullptr, &rCol);
  dev->CreateRenderTargetView(tMvR, nullptr, &rMv);
  // DCS's state at the bind: its targets bound, a read-only DSV, a viewport, some pipeline state of its own.
  D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
  dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
  dd.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
  ID3D11DepthStencilView* roDsv = nullptr;
  dev->CreateDepthStencilView(tDepR, &dd, &roDsv);
  ID3D11RenderTargetView* bound[2] = {rCol, rMv};
  ctx->OMSetRenderTargets(2, bound, roDsv);
  const D3D11_VIEWPORT dcsVp{0, 0, float(W), float(H), 0, 1};
  ctx->RSSetViewports(1, &dcsVp);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);

  fixdepth::tvfill::Inputs in;
  float pL[16], pR[16], viL[16] = {}, viR[16] = {};
  Proj(L, pL);
  Proj(R, pR);
  viL[0] = viL[5] = viL[10] = viL[15] = viR[0] = viR[5] = viR[10] = viR[15] = 1.f;
  viL[12] = float(L.x), viR[12] = float(R.x);
  const bool mOk = fixdepth::tvfill::Transform(pL, viL, pR, viR, in.M01) && fixdepth::tvfill::Transform(pR, viR, pL, viL, in.M10);
  in.leftCol = tColL, in.leftColFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
  in.leftMv = tMvL, in.leftMvFmt = DXGI_FORMAT_R16G16_FLOAT;
  in.leftDepth = tDepL, in.depthFmt = DXGI_FORMAT_R32G8X24_TYPELESS;
  in.vpL = in.vpR = dcsVp;
  in.rCol = rCol, in.rMv = rMv, in.rDepth = tDepR, in.rSlice = 0, in.strip = strip;
  fixdepth::tvfill::Fill fill;
  const bool ran = mOk && fill.Run(ctx, in);
  Check(ran, ran ? "three views fill: ran" : ("three views fill: did not run: " + fill.lastError).c_str());
  // DCS's state is back.
  ID3D11RenderTargetView* now[2] = {};
  ID3D11DepthStencilView* nowDsv = nullptr;
  ctx->OMGetRenderTargets(2, now, &nowDsv);
  UINT nv = 1;
  D3D11_VIEWPORT vpNow{};
  ctx->RSGetViewports(&nv, &vpNow);
  D3D11_PRIMITIVE_TOPOLOGY topo{};
  ctx->IAGetPrimitiveTopology(&topo);
  Check(now[0] == rCol && now[1] == rMv && nowDsv == roDsv && nv == 1 && vpNow.Width == float(W) &&
            topo == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,
        "three views fill: DCS's targets, read-only DSV, viewport and topology are bound again after the fill");
  for (auto* p : now)
    if (p) p->Release();
  if (nowDsv) nowDsv->Release();
  ctx->ClearState();
  // Read back.
  auto readback = [&](ID3D11Texture2D* t, std::vector<uint8_t>& out, UINT bpp) {
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* s = nullptr;
    dev->CreateTexture2D(&d, nullptr, &s);
    ctx->CopyResource(s, t);
    D3D11_MAPPED_SUBRESOURCE m{};
    ctx->Map(s, 0, D3D11_MAP_READ, 0, &m);
    out.resize(W * H * bpp);
    for (UINT y = 0; y < H; ++y) memcpy(&out[y * W * bpp], static_cast<uint8_t*>(m.pData) + y * m.RowPitch, W * bpp);
    ctx->Unmap(s, 0);
    s->Release();
  };
  std::vector<uint8_t> oc, om, od;
  readback(tColR, oc, 8);
  readback(tMvR, om, 4);
  readback(tDepR, od, 8);
  int keptBad = 0, keptN = 0, fillN = 0, leak = 0, mvBad = 0, depthBad = 0;
  double err = 0;
  for (UINT y = 2; y + 2 < H; ++y)
    for (UINT x = 2; x + 2 < W; ++x) {
      const size_t i = y * W + x;
      const double u = (x + 0.5) / W, v = (y + 0.5) / H;
      const double tx = R.l + u * (R.r - R.l), ty = R.u + v * (R.d - R.u);
      const float c = FromHalf(reinterpret_cast<const uint16_t*>(oc.data())[i * 4]);
      const float m0 = FromHalf(reinterpret_cast<const uint16_t*>(om.data())[i * 2]);
      float dz;
      memcpy(&dz, &od[i * 8], 4);
      const bool ck = panel(R, tx, ty) || ownCockpit(tx, ty);
      if (ck || (u * 2 - 1) >= strip + 0.06) {
        ++keptN;
        if (memcmp(&oc[i * 8], &colR[i * 4], 8) || memcmp(&om[i * 4], &mvR[i * 2], 4) || memcmp(&od[i * 8], &depR[i * 8], 5)) ++keptBad;
        continue;
      }
      if ((u * 2 - 1) >= strip) continue;  // the cross-fade
      const double X = R.x + tx * kWallZ - kOrigin, txL = (X + kOrigin - L.x) / kWallZ;
      if (txL < L.l + 0.02 || txL > L.r - 0.02) continue;  // the left eye does not see it
      const double txP = (R.x + tx * 0.6 - L.x) / 0.6;
      const bool edge = std::fabs(ty + 0.35) < 0.18 && txP > -0.42 && txP < 0.02;  // 1-2 texels round the panel
      if (edge || (std::fabs(ty + 0.45) < 0.2 && tx > 0.55 && tx < 0.85)) continue;
      if (c > 0.95f) ++leak;
      ++fillN;
      err += std::fabs(c - Wall(X, ty * kWallZ));
      if (std::fabs(m0 - 0.25f) > 1e-3) ++mvBad;
      if (std::fabs(dz - float(kNear / kWallZ)) > 2e-4f) ++depthBad;
    }
  char b[300];
  snprintf(b, sizeof(b), "three views fill: the cockpit and the strip keep DCS's colour, motion, depth and stencil (%d of %d differ)",
           keptBad, keptN);
  Check(keptN > 5000 && keptBad == 0, b);
  snprintf(b, sizeof(b), "three views fill: the filled colour matches the wall rendered directly (mean error %.4f over %d px)",
           fillN ? err / fillN : 1.0, fillN);
  Check(fillN > 20000 && err / fillN < 0.01, b);
  snprintf(b, sizeof(b), "three views fill: filled depth is the wall's (%d off) and the motion the left view's (%d off)", depthBad, mvBad);
  Check(depthBad == 0 && mvBad == 0, b);
  snprintf(b, sizeof(b), "three views fill: the cockpit panel does not leak into the world away from its edge (%d px)", leak);
  Check(leak == 0, b);
  fill.Release();
  for (IUnknown* p : {static_cast<IUnknown*>(rCol), static_cast<IUnknown*>(rMv), static_cast<IUnknown*>(roDsv), static_cast<IUnknown*>(tColL),
                      static_cast<IUnknown*>(tMvL), static_cast<IUnknown*>(tDepL), static_cast<IUnknown*>(tColR), static_cast<IUnknown*>(tMvR),
                      static_cast<IUnknown*>(tDepR), static_cast<IUnknown*>(ctx), static_cast<IUnknown*>(dev)})
    if (p) p->Release();
}

}  // namespace tvtest
