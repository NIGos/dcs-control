// 0.7 three views, the fill inside the engine (three_views.h; included from fixation_depth.h inside namespace fixdepth,
// before tvexp). In the right periphery's TransparentCockpitPass, right after DCS binds its render targets and before its
// first draw (the cockpit glass), every pixel of the view that DCS did not draw (neither the outer strip nor the cockpit:
// the stencil composition COCKPIT, (s & 0x38) == 0x28, already in the depth-stencil) gets the left periphery's lit colour,
// copied at the same point of the left view, reprojected with the left depth; its depth (the reprojected point's) and
// its motion vector (the left view's at the source pixel) go into DCS's depth buffer and motion target. DCS then draws
// the right eye's own glass, HUD, DLSS, tone mapping and overlays over a coherent view: the glass and the symbology are
// DCS's own, at the right disparity, by construction.
// The fill: a compute splat of the left depth into the right view (nearest wins), then one full-screen draw at the
// view's render resolution with our own pipeline state (ID3DDeviceContextState swapped in and out: DCS's complete state
// is back after it), stencil test NOT_EQUAL COCKPIT, depth test ALWAYS with writes, colour blended over DCS's only in a
// short cross-fade at the strip's inner edge. A sample the left eye sees covered (its depth, bilinear footprint) is
// refused; a pixel without a usable sample takes the farther of its nearest usable neighbours on the row (background
// stretch), none within 24 texels: the direction at infinity.
#pragma once

namespace tvfill {

struct Inputs {
  // Left view, at its TransparentCockpitPass: our copies. Colour and motion: the view's viewport box at (0, 0).
  ID3D11Texture2D* leftCol = nullptr;
  DXGI_FORMAT leftColFmt = DXGI_FORMAT_UNKNOWN;  // the SRV format (DCS's render target view format)
  ID3D11Texture2D* leftMv = nullptr;             // may be null
  DXGI_FORMAT leftMvFmt = DXGI_FORMAT_UNKNOWN;
  ID3D11Texture2D* leftDepth = nullptr;  // whole depth-stencil copy (typeless), the view at vpL
  DXGI_FORMAT depthFmt = DXGI_FORMAT_UNKNOWN;
  D3D11_VIEWPORT vpL{}, vpR{};
  // Right view: DCS's own targets as bound in the pass, and its depth-stencil texture.
  ID3D11RenderTargetView* rCol = nullptr;
  ID3D11RenderTargetView* rMv = nullptr;  // may be null
  ID3D11Texture2D* rDepth = nullptr;
  UINT rSlice = 0;
  float M01[16] = {}, M10[16] = {};  // left NDC -> right homogeneous NDC and back (row vectors)
  float strip = 0.55f;
};

const char kSrc[] = R"(
cbuffer C : register(b0) {
  row_major float4x4 M01;
  row_major float4x4 M10;
  float4 vpL;    // left depth viewport (x, y, w, h)
  float4 vpR;    // right viewport in its targets
  float4 strip;  // x strip NDC, y blend start, z blend end, w search texels
};
Texture2D<float> depL : register(t0);
Texture2D<float4> colL : register(t1);
Texture2D<float4> mvL : register(t2);
Texture2D<uint> splatR : register(t3);
RWTexture2D<uint> splat : register(u0);
SamplerState lin : register(s0);

[numthreads(8, 8, 1)] void Splat(uint3 id : SV_DispatchThreadID) {
  if (id.x >= (uint)vpL.z || id.y >= (uint)vpL.w) return;
  float d = max(depL.Load(int3(int2(vpL.xy) + int2(id.xy), 0)), 1e-7);
  float2 uv = (float2(id.xy) + 0.5) / vpL.zw;
  float4 c = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1), M01);
  if (!(c.w > 0)) return;
  float3 p = c.xyz / c.w;
  float2 f = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5) * vpR.zw - 0.5;
  int y = (int)floor(f.y + 0.5), x0 = (int)floor(f.x);
  if (y < 0 || y >= (int)vpR.w || !(p.z > 0)) return;
  uint v = asuint(p.z);
  [unroll] for (int k = 0; k < 2; ++k) {
    int x = x0 + k;
    if (x >= 0 && x < (int)vpR.z) InterlockedMax(splat[int2(x, y)], v);
  }
}

float3 ToLeft(float2 ndc, float d) {
  float4 c = mul(float4(ndc, d, 1), M10);
  float3 p = c.xyz / max(c.w, 1e-12);
  return float3(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5, p.z);
}
bool LeftOccluded(float3 l) {
  float2 t = l.xy * vpL.zw;
  int2 a = int2(clamp(floor(t - 0.5), 0, vpL.zw - 1)) + int2(vpL.xy), b = int2(clamp(floor(t + 0.5), 0, vpL.zw - 1)) + int2(vpL.xy);
  float m = max(max(depL.Load(int3(a.x, a.y, 0)), depL.Load(int3(b.x, a.y, 0))),
                max(depL.Load(int3(a.x, b.y, 0)), depL.Load(int3(b.x, b.y, 0))));
  return m > l.z * 1.15 + 1e-6;
}

void VS(uint i : SV_VertexID, out float4 pos : SV_Position) {
  float2 t = float2((i << 1) & 2, i & 2);
  pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}

struct Out { float4 col : SV_Target0; float4 mv : SV_Target1; float depth : SV_Depth; };
Out PS(float4 pos : SV_Position) {
  float2 p = pos.xy - vpR.xy;
  float2 uv = p / vpR.zw;
  float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
  float w = saturate((ndc.x - strip.y) / max(strip.z - strip.y, 1e-4));
  if (w >= 1) discard;
  int2 rt = int2(clamp(floor(p), 0, vpR.zw - 1));
  uint s = splatR.Load(int3(rt, 0));
  float3 l = 0;
  float d = 0;
  bool found = false;
  if (s != 0) {
    d = asfloat(s);
    l = ToLeft(ndc, d);
    found = !LeftOccluded(l);
  }
  if (!found) {
    int n = (int)strip.w;
    bool fl = false, fr = false;
    float dl = 0, dr = 0;
    float3 ul = 0, ur = 0;
    for (int i = 1; i <= n && !(fl && fr); ++i) {
      [unroll] for (int side = 0; side < 2; ++side) {
        if (side == 0 ? fl : fr) continue;
        int x = side == 0 ? rt.x - i : rt.x + i;
        if (x < 0 || x >= (int)vpR.z) continue;
        uint a = splatR.Load(int3(x, rt.y, 0));
        if (a == 0) continue;
        float3 lu = ToLeft(float2(((x + 0.5) / vpR.z) * 2 - 1, ndc.y), asfloat(a));
        if (LeftOccluded(lu)) continue;
        if (side == 0) { fl = true; dl = asfloat(a); ul = lu; } else { fr = true; dr = asfloat(a); ur = lu; }
      }
    }
    if (!fl && !fr) {
      d = 1e-7;
      l = ToLeft(ndc, d);
    } else if (!fr || (fl && dl <= dr)) {
      d = dl; l = ul;
    } else {
      d = dr; l = ur;
    }
  }
  float2 uvL = saturate(l.xy);
  Out o;
  o.col = float4(colL.SampleLevel(lin, uvL, 0).rgb, 1 - w);
  o.mv = mvL.Load(int3(clamp(int2(uvL * vpL.zw), 0, int2(vpL.zw) - 1), 0));
  o.depth = d;
  return o;
}
)";

struct Constants {
  float M01[16], M10[16];
  float vpL[4], vpR[4], strip[4];
};

class Fill {
 public:
  std::atomic<uint64_t> runs{0}, failures{0};
  std::string lastError;

  // Our pipeline state, shaders and the splat target (render thread; the device of DCS's context).
  bool Ensure(ID3D11Device* dev) {
    if (m_failed) return false;
    if (m_ps) return true;
    bool ok = chainfast::LoadCompiler();
    std::vector<uint8_t> cs, vs, ps;
    auto compile = [&](const char* entry, const char* profile, std::vector<uint8_t>& out) {
      ID3DBlob *code = nullptr, *err = nullptr;
      const HRESULT hr = chainfast::g_compile(kSrc, strlen(kSrc), "three_views_fill", nullptr, nullptr, entry, profile,
                                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
      if (FAILED(hr) || !code) {
        lastError = err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()) : "compile failed";
        if (err) err->Release();
        if (code) code->Release();
        return false;
      }
      if (err) err->Release();
      const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
      out.assign(p, p + code->GetBufferSize());
      code->Release();
      return true;
    };
    ok = ok && compile("Splat", "cs_5_0", cs) && compile("VS", "vs_5_0", vs) && compile("PS", "ps_5_0", ps);
    ok = ok && SUCCEEDED(dev->CreateComputeShader(cs.data(), cs.size(), nullptr, &m_cs)) &&
         SUCCEEDED(dev->CreateVertexShader(vs.data(), vs.size(), nullptr, &m_vs)) &&
         SUCCEEDED(dev->CreatePixelShader(ps.data(), ps.size(), nullptr, &m_ps));
    if (ok) {
      D3D11_BUFFER_DESC b{};
      b.ByteWidth = sizeof(Constants);
      b.Usage = D3D11_USAGE_DYNAMIC;
      b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      ok = SUCCEEDED(dev->CreateBuffer(&b, nullptr, &m_cb));
      D3D11_SAMPLER_DESC sd{};
      sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
      sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
      sd.MaxLOD = D3D11_FLOAT32_MAX;
      ok = ok && SUCCEEDED(dev->CreateSamplerState(&sd, &m_samp));
      D3D11_DEPTH_STENCIL_DESC ds{};
      ds.DepthEnable = TRUE;
      ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
      ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
      ds.StencilEnable = TRUE;
      ds.StencilReadMask = 0x38;
      ds.StencilWriteMask = 0;
      ds.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_NOT_EQUAL};
      ds.BackFace = ds.FrontFace;
      ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&ds, &m_ds));
      D3D11_BLEND_DESC bd{};
      bd.IndependentBlendEnable = TRUE;
      bd.RenderTarget[0].BlendEnable = TRUE;
      bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
      bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
      bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
      bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
      bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
      bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
      bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
      bd.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
      ok = ok && SUCCEEDED(dev->CreateBlendState(&bd, &m_blend));
      D3D11_RASTERIZER_DESC rd{};
      rd.FillMode = D3D11_FILL_SOLID;
      rd.CullMode = D3D11_CULL_NONE;
      rd.DepthClipEnable = FALSE;
      ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rd, &m_rs));
      ID3D11Device1* d1 = nullptr;
      if (ok && SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&d1)))) {
        const D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
        ok = SUCCEEDED(d1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &m_state));
        d1->Release();
      } else {
        ok = false;
      }
    }
    if (!ok) {
      m_failed = true;
      if (lastError.empty()) lastError = "resources failed";
    }
    return ok;
  }

  // One fill (render thread, DCS's immediate context). False: nothing was drawn (DCS's state untouched).
  bool Run(ID3D11DeviceContext* ctx, const Inputs& in) {
    if (!in.leftCol || !in.leftDepth || !in.rCol || !in.rDepth) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    bool ok = Ensure(dev);
    ID3D11ShaderResourceView *sDepL = nullptr, *sColL = nullptr, *sMvL = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    const UINT w = static_cast<UINT>(in.vpR.Width + 0.5f), h = static_cast<UINT>(in.vpR.Height + 0.5f);
    ok = ok && w >= 16 && h >= 16 && Srv(dev, in.leftDepth, DepthSrvFmt(in.depthFmt), &sDepL) &&
         Srv(dev, in.leftCol, in.leftColFmt, &sColL) && (!in.leftMv || Srv(dev, in.leftMv, in.leftMvFmt, &sMvL)) &&
         Dsv(dev, in.rDepth, in.rSlice, &dsv) && Splat(dev, w, h);
    dev->Release();
    if (!ok) {
      failures++;
      return false;
    }
    ID3D11DeviceContext1* c1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1)))) {
      failures++;
      return false;
    }
    Constants k{};
    memcpy(k.M01, in.M01, sizeof(k.M01));
    memcpy(k.M10, in.M10, sizeof(k.M10));
    k.vpL[0] = in.vpL.TopLeftX, k.vpL[1] = in.vpL.TopLeftY, k.vpL[2] = in.vpL.Width, k.vpL[3] = in.vpL.Height;
    k.vpR[0] = in.vpR.TopLeftX, k.vpR[1] = in.vpR.TopLeftY, k.vpR[2] = in.vpR.Width, k.vpR[3] = in.vpR.Height;
    k.strip[0] = in.strip, k.strip[1] = in.strip + 0.01f, k.strip[2] = in.strip + 0.06f, k.strip[3] = 24.f;
    ID3DDeviceContextState* prev = nullptr;
    c1->SwapDeviceContextState(m_state, &prev);  // our state from here on; DCS's is back after the second swap
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
      memcpy(m.pData, &k, sizeof(k));
      ctx->Unmap(m_cb, 0);
    }
    const UINT zero[4] = {};
    ctx->ClearUnorderedAccessViewUint(m_splatUav, zero);
    ctx->CSSetShader(m_cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &m_cb);
    ctx->CSSetShaderResources(0, 1, &sDepL);
    ctx->CSSetUnorderedAccessViews(0, 1, &m_splatUav, nullptr);
    ctx->Dispatch((static_cast<UINT>(in.vpL.Width) + 7) / 8, (static_cast<UINT>(in.vpL.Height) + 7) / 8, 1);
    ID3D11UnorderedAccessView* nu = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
    ID3D11RenderTargetView* rtv[2] = {in.rCol, sMvL ? in.rMv : nullptr};
    ctx->OMSetRenderTargets(2, rtv, dsv);
    ctx->OMSetDepthStencilState(m_ds, 0x28);
    const float bf[4] = {};
    ctx->OMSetBlendState(m_blend, bf, 0xffffffff);
    ctx->RSSetState(m_rs);
    ctx->RSSetViewports(1, &in.vpR);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_vs, nullptr, 0);
    ctx->PSSetShader(m_ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &m_cb);
    ID3D11ShaderResourceView* ps[4] = {sDepL, sColL, sMvL, m_splatSrv};
    ctx->PSSetShaderResources(0, 4, ps);
    ctx->PSSetSamplers(0, 1, &m_samp);
    ctx->Draw(3, 0);
    ctx->ClearState();  // our state keeps no reference to DCS's targets
    c1->SwapDeviceContextState(prev, nullptr);
    if (prev) prev->Release();
    c1->Release();
    runs++;
    return true;
  }

  void Release() {
    for (IUnknown** p : {reinterpret_cast<IUnknown**>(&m_cs), reinterpret_cast<IUnknown**>(&m_vs), reinterpret_cast<IUnknown**>(&m_ps),
                         reinterpret_cast<IUnknown**>(&m_cb), reinterpret_cast<IUnknown**>(&m_samp), reinterpret_cast<IUnknown**>(&m_ds),
                         reinterpret_cast<IUnknown**>(&m_blend), reinterpret_cast<IUnknown**>(&m_rs), reinterpret_cast<IUnknown**>(&m_state),
                         reinterpret_cast<IUnknown**>(&m_splat), reinterpret_cast<IUnknown**>(&m_splatUav), reinterpret_cast<IUnknown**>(&m_splatSrv)})
      if (*p) (*p)->Release(), *p = nullptr;
    for (auto& e : m_srvs) e.v->Release();
    m_srvs.clear();
    for (auto& e : m_dsvs) e.v->Release();
    m_dsvs.clear();
    m_sw = m_sh = 0;
    m_failed = false;
  }

  static DXGI_FORMAT DepthSrvFmt(DXGI_FORMAT f) {
    switch (f) {
      case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
      case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
      case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
      default: return DXGI_FORMAT_UNKNOWN;
    }
  }
  static DXGI_FORMAT DsvFmt(DXGI_FORMAT f) {
    switch (f) {
      case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
      case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_D24_UNORM_S8_UINT;
      default: return DXGI_FORMAT_UNKNOWN;  // no stencil: the cockpit cannot be kept
    }
  }

 private:
  template <class V>
  struct Entry {
    ID3D11Resource* r;
    UINT key;
    V* v;
  };
  bool Srv(ID3D11Device* dev, ID3D11Texture2D* t, DXGI_FORMAT f, ID3D11ShaderResourceView** out) {
    if (f == DXGI_FORMAT_UNKNOWN) return false;
    for (auto& e : m_srvs)
      if (e.r == t && e.key == static_cast<UINT>(f)) return *out = e.v, true;
    if (m_srvs.size() > 24) {
      for (auto& e : m_srvs) e.v->Release();
      m_srvs.clear();
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = f;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* v = nullptr;
    if (FAILED(dev->CreateShaderResourceView(t, &sd, &v)) || !v) return false;
    m_srvs.push_back({t, static_cast<UINT>(f), v});
    *out = v;
    return true;
  }
  bool Dsv(ID3D11Device* dev, ID3D11Texture2D* t, UINT slice, ID3D11DepthStencilView** out) {
    for (auto& e : m_dsvs)
      if (e.r == t && e.key == slice) return *out = e.v, true;
    D3D11_TEXTURE2D_DESC td{};
    t->GetDesc(&td);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DsvFmt(td.Format);
    if (dd.Format == DXGI_FORMAT_UNKNOWN || td.SampleDesc.Count != 1) return false;
    if (td.ArraySize > 1) {
      dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
      dd.Texture2DArray.FirstArraySlice = slice;
      dd.Texture2DArray.ArraySize = 1;
    } else {
      dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    }
    ID3D11DepthStencilView* v = nullptr;
    if (FAILED(dev->CreateDepthStencilView(t, &dd, &v)) || !v) return false;
    if (m_dsvs.size() > 8) {
      for (auto& e : m_dsvs) e.v->Release();
      m_dsvs.clear();
    }
    m_dsvs.push_back({t, slice, v});
    *out = v;
    return true;
  }
  bool Splat(ID3D11Device* dev, UINT w, UINT h) {
    if (m_splat && m_sw == w && m_sh == h) return true;
    for (IUnknown** p : {reinterpret_cast<IUnknown**>(&m_splat), reinterpret_cast<IUnknown**>(&m_splatUav), reinterpret_cast<IUnknown**>(&m_splatSrv)})
      if (*p) (*p)->Release(), *p = nullptr;
    D3D11_TEXTURE2D_DESC d{w, h, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                           D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &m_splat)) || FAILED(dev->CreateUnorderedAccessView(m_splat, nullptr, &m_splatUav)) ||
        FAILED(dev->CreateShaderResourceView(m_splat, nullptr, &m_splatSrv)))
      return false;
    m_sw = w, m_sh = h;
    return true;
  }

  bool m_failed = false;
  ID3D11ComputeShader* m_cs = nullptr;
  ID3D11VertexShader* m_vs = nullptr;
  ID3D11PixelShader* m_ps = nullptr;
  ID3D11Buffer* m_cb = nullptr;
  ID3D11SamplerState* m_samp = nullptr;
  ID3D11DepthStencilState* m_ds = nullptr;
  ID3D11BlendState* m_blend = nullptr;
  ID3D11RasterizerState* m_rs = nullptr;
  ID3DDeviceContextState* m_state = nullptr;
  ID3D11Texture2D* m_splat = nullptr;
  ID3D11UnorderedAccessView* m_splatUav = nullptr;
  ID3D11ShaderResourceView* m_splatSrv = nullptr;
  UINT m_sw = 0, m_sh = 0;
  std::vector<Entry<ID3D11ShaderResourceView>> m_srvs;
  std::vector<Entry<ID3D11DepthStencilView>> m_dsvs;
};

// Left NDC (post-divide, reverse-Z depth) to right homogeneous NDC: inv(Pl) * VIl * inv(VIr) * Pr, row vectors; the camera
// part in double (the rows are DCS's floats).
inline bool Inverse4(const double* m, double* out) {
  double a[4][8];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
    if (std::fabs(a[p][c]) < 1e-300) return false;
    if (p != c)
      for (int j = 0; j < 8; ++j) std::swap(a[p][j], a[c][j]);
    const double d = a[c][c];
    for (int j = 0; j < 8; ++j) a[c][j] /= d;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      const double f = a[r][c];
      if (f != 0)
        for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
    }
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out[i * 4 + j] = a[i][j + 4];
  return true;
}
inline void Mul4(const double* a, const double* b, double* out) {
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
      out[i * 4 + j] = s;
    }
}
inline bool Transform(const float* projA, const float* viewInvA, const float* projB, const float* viewInvB, float out[16]) {
  double Pa[16], Pb[16], VIa[16], VIb[16], iPa[16], iVIb[16], t1[16], t2[16], M[16];
  for (int i = 0; i < 16; ++i) Pa[i] = projA[i], Pb[i] = projB[i], VIa[i] = viewInvA[i], VIb[i] = viewInvB[i];
  if (!Inverse4(Pa, iPa) || !Inverse4(VIb, iVIb)) return false;
  Mul4(VIa, iVIb, t1);
  Mul4(iPa, t1, t2);
  Mul4(t2, Pb, M);
  for (int i = 0; i < 16; ++i) {
    if (!std::isfinite(M[i])) return false;
    out[i] = static_cast<float>(M[i]);
  }
  return true;
}

}  // namespace tvfill
