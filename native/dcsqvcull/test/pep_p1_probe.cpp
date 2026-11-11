// Probe P1 of PER_EYE_PASS_DESIGN.md (section 7): can a pixel shader learn which view it shades under NVIDIA
// single-pass stereo with a full second position? A toy app on the PC's NVIDIA GPU; no DCS, no driver setting.
//
// One draw (indexed, instanced, 3 render targets + D32S8 two-slice arrays, GREATER_EQUAL depth, stencil REPLACE, two
// viewports of different sizes at the origin, RT index offset 1, independent viewport masks) is rendered
//   - once with the stereo vertex shader (NV_XYZW_RIGHT + NV_VIEWPORT_MASK) inside a stereo block, and
//   - twice with the plain shader, one view at a time, into a second set of arrays (the "stock" reference).
// Per variant it reports what the pixel shader read as SV_RenderTargetArrayIndex / SV_ViewportArrayIndex /
// NV_PACKED_EYE_INDEX in each slice, and whether the stereo slices equal the reference byte for byte (attributes,
// SV_Position, derivatives, depth, stencil).
//
// Each variant runs in its own process (a driver fault in one cannot hide the others): `pep_p1_probe` runs all,
// `pep_p1_probe <n>` runs one. Exit code 0 = ran (read the lines), 3 = no NVIDIA adapter / no NVAPI (skipped).
// Build: target DcsQvCullPepP1Probe (not a ctest test: it needs an NVIDIA GPU).
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <string>
#include <unordered_set>
#include <vector>

#include "../src/nvapi_sps_min.h"
#define DCSQV_PEP_NO_RUNTIME
#include "../src/per_eye_pass.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

struct Variant {
  const char* name;
  bool vsRt, vsVp, psRt, psVp, eye;
};
// vs*: the vertex shader writes the system value (0); ps*: the pixel shader declares it as an input without any
// earlier stage writing it; eye: NV_PACKED_EYE_INDEX custom semantic.
const Variant kVariants[] = {
    {"base", false, false, false, false, false},
    {"rt_vs_writes", true, false, false, false, false},
    {"vp_vs_writes", false, true, false, false, false},
    {"rt_vp_vs_writes", true, true, false, false, false},
    {"rt_ps_only", false, false, true, false, false},
    {"vp_ps_only", false, false, false, true, false},
    {"packed_eye", false, false, false, false, true},
};
const int kVariantCount = static_cast<int>(sizeof(kVariants) / sizeof(kVariants[0]));

const char kHlsl[] = R"(
cbuffer C : register(b0) { float4 sL; float4 sR; uint4 misc; };
struct VO {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
#if NVOUT
  float4 posR : NV_XYZW_RIGHT;
  nointerpolation uint m : NV_VIEWPORT_MASK;
#endif
#if VS_RT
  nointerpolation uint rt : SV_RenderTargetArrayIndex;
#endif
#if VS_VP
  nointerpolation uint vp : SV_ViewportArrayIndex;
#endif
#if EYE && NVOUT
  nointerpolation uint eye : NV_PACKED_EYE_INDEX;
#endif
};
VO vs(float3 p : POSITION, float2 uv : TEXCOORD0, uint iid : SV_InstanceID) {
  VO o;
  float w = 1.0 + 0.4 * p.z + 0.25 * iid;
  float2 q = p.xy + 0.3 * iid;
  float zc = 0.2 + 0.1 * p.z + 0.1 * iid;
  o.pos = float4((q * sL.xy + sL.zw) * w, zc * w, w);
  o.uv = uv;
#if NVOUT
  o.posR = float4((q * sR.xy + sR.zw) * w, zc * w, w);
  o.m = misc.x;
#endif
#if VS_RT
  o.rt = 0;
#endif
#if VS_VP
  o.vp = 0;
#endif
#if EYE && NVOUT
  o.eye = 7;
#endif
  return o;
}
struct PO { uint4 o0 : SV_Target0; float4 o1 : SV_Target1; float4 o2 : SV_Target2; };
PO ps(VO i
#if PS_RT
  , nointerpolation uint prt : SV_RenderTargetArrayIndex
#endif
#if PS_VP
  , nointerpolation uint pvp : SV_ViewportArrayIndex
#endif
) {
  PO o;
  uint rt = 0xffff, vp = 0xffff, eye = 0xffff;
#if VS_RT
  rt = i.rt;
#endif
#if VS_VP
  vp = i.vp;
#endif
#if PS_RT
  rt = prt;
#endif
#if PS_VP
  vp = pvp;
#endif
#if EYE && NVOUT
  eye = i.eye;
#endif
  o.o0 = uint4(rt, vp, eye, 0xC0DE);
  o.o1 = float4(i.uv, i.pos.z, i.pos.w);
  o.o2 = float4(i.pos.xy, ddx(i.uv.x), ddy(i.uv.y));
  return o;
}
)";

template <class T>
void Rel(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

ID3DBlob* Compile(const Variant& v, bool nvOut, const char* entry, const char* target) {
  const D3D_SHADER_MACRO m[] = {{"NVOUT", nvOut ? "1" : "0"},     {"VS_RT", v.vsRt ? "1" : "0"},
                                {"VS_VP", v.vsVp ? "1" : "0"},    {"PS_RT", v.psRt ? "1" : "0"},
                                {"PS_VP", v.psVp ? "1" : "0"},    {"EYE", v.eye ? "1" : "0"},
                                {nullptr, nullptr}};
  ID3DBlob *code = nullptr, *err = nullptr;
  const HRESULT hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "p1", m, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                &code, &err);
  if (FAILED(hr)) {
    printf("P1 %s: compile %s failed 0x%08lx: %s\n", v.name, entry, static_cast<unsigned long>(hr),
           err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    fflush(stdout);
  }
  if (err) err->Release();
  return code;
}

constexpr UINT kW = 640, kH = 512;  // the two-slice arrays: the larger size per axis
const D3D11_VIEWPORT kVp[2] = {{0, 0, 640, 400, 0, 1}, {0, 0, 512, 512, 0, 1}};
const D3D11_RECT kSc[2] = {{0, 0, 640, 400}, {0, 0, 512, 512}};
const DXGI_FORMAT kFmt[3] = {DXGI_FORMAT_R32G32B32A32_UINT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT};

struct Targets {
  ID3D11Texture2D* col[3] = {};
  ID3D11Texture2D* depth = nullptr;
  ID3D11RenderTargetView* rtvAll[3] = {};
  ID3D11RenderTargetView* rtvSlice[2][3] = {};
  ID3D11DepthStencilView* dsvAll = nullptr;
  ID3D11DepthStencilView* dsvSlice[2] = {};
};

bool MakeTargets(ID3D11Device* dev, Targets& t) {
  for (int c = 0; c < 3; ++c) {
    D3D11_TEXTURE2D_DESC d = {kW, kH, 1, 2, kFmt[c], {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &t.col[c]))) return false;
    D3D11_RENDER_TARGET_VIEW_DESC rv = {};
    rv.Format = kFmt[c];
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    rv.Texture2DArray.ArraySize = 2;
    if (FAILED(dev->CreateRenderTargetView(t.col[c], &rv, &t.rtvAll[c]))) return false;
    for (UINT s = 0; s < 2; ++s) {
      rv.Texture2DArray.FirstArraySlice = s;
      rv.Texture2DArray.ArraySize = 1;
      if (FAILED(dev->CreateRenderTargetView(t.col[c], &rv, &t.rtvSlice[s][c]))) return false;
    }
  }
  D3D11_TEXTURE2D_DESC d = {kW, kH, 1, 2, DXGI_FORMAT_R32G8X24_TYPELESS, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL, 0, 0};
  if (FAILED(dev->CreateTexture2D(&d, nullptr, &t.depth))) return false;
  D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
  dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
  dv.Texture2DArray.ArraySize = 2;
  if (FAILED(dev->CreateDepthStencilView(t.depth, &dv, &t.dsvAll))) return false;
  for (UINT s = 0; s < 2; ++s) {
    dv.Texture2DArray.FirstArraySlice = s;
    dv.Texture2DArray.ArraySize = 1;
    if (FAILED(dev->CreateDepthStencilView(t.depth, &dv, &t.dsvSlice[s]))) return false;
  }
  return true;
}

void Clear(ID3D11DeviceContext* ctx, Targets& t) {
  const float zero[4] = {0, 0, 0, 0};
  for (int c = 0; c < 3; ++c) ctx->ClearRenderTargetView(t.rtvAll[c], zero);
  ctx->ClearDepthStencilView(t.dsvAll, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
}

// One slice of a texture as tightly packed rows of `bpp` bytes per texel.
bool ReadSlice(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, UINT slice, UINT bpp, std::vector<uint8_t>& out) {
  D3D11_TEXTURE2D_DESC d;
  tex->GetDesc(&d);
  d.ArraySize = 1;
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* st = nullptr;
  if (FAILED(dev->CreateTexture2D(&d, nullptr, &st))) return false;
  ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, slice, nullptr);
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
    st->Release();
    return false;
  }
  out.resize(static_cast<size_t>(kW) * kH * bpp);
  for (UINT y = 0; y < kH; ++y) memcpy(&out[static_cast<size_t>(y) * kW * bpp], static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, static_cast<size_t>(kW) * bpp);
  ctx->Unmap(st, 0);
  st->Release();
  return true;
}

struct CbData {
  float sL[4], sR[4];
  uint32_t misc[4];
};

int RunVariant(int index) {
  const Variant& v = kVariants[index];
  IDXGIFactory1* fac = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&fac)))) return 3;
  IDXGIAdapter1* ad = nullptr;
  DXGI_ADAPTER_DESC1 desc = {};
  for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
    ad->GetDesc1(&desc);
    if (desc.VendorId == 0x10de) break;
    Rel(ad);
  }
  if (!ad) {
    printf("P1 %s: SKIP no NVIDIA adapter\n", v.name);
    return 3;
  }
  LARGE_INTEGER umd = {};
  ad->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;  // DCS's level
  if (FAILED(D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
    printf("P1 %s: SKIP no device\n", v.name);
    return 3;
  }
  nvsps::Api api;
  const int bind = nvsps::Bind(LoadLibraryW(L"nvapi64.dll"), api);
  if (bind != 0) {
    printf("P1 %s: SKIP NVAPI bind %d\n", v.name, bind);
    return 3;
  }
  nvsps::QuerySps q = {nvsps::kQuerySpsVer, 0};
  const int qs = api.query(dev, &q);
  D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3 = {};
  dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof(o3));
  if (index == 0) {
    printf("P1 gpu=\"%ls\" driver=%u.%u.%u.%u spsQuery=%d bits=0x%x(sps=%u xyzw=%u) vpRtIndexFromAnyShader=%d featureLevel=11_0\n",
           desc.Description, HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart), qs, q.bits,
           q.bits & 1, (q.bits >> 1) & 1, o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer);
  }
  if (qs != 0 || (q.bits & 3) != 3) {
    printf("P1 %s: FAIL single-pass stereo with XYZW not supported\n", v.name);
    return 0;
  }

  ID3DBlob* vsS = Compile(v, true, "vs", "vs_5_0");
  ID3DBlob* psS = Compile(v, true, "ps", "ps_5_0");
  ID3DBlob* vsR = Compile(v, false, "vs", "vs_5_0");
  ID3DBlob* psR = Compile(v, false, "ps", "ps_5_0");
  if (!vsS || !psS || !vsR || !psR) return 0;

  nvsps::CustomSemantic sem[3] = {nvsps::MakeSemantic(nvsps::kSemXyzwRight, "NV_XYZW_RIGHT"),
                                  nvsps::MakeSemantic(nvsps::kSemViewportMask, "NV_VIEWPORT_MASK"),
                                  nvsps::MakeSemantic(nvsps::kSemPackedEyeIndex, "NV_PACKED_EYE_INDEX")};
  nvsps::CreateVsEx args = {nvsps::kCreateVsExVer, v.eye ? 3u : 2u, sem, FALSE, FALSE};
  ID3D11VertexShader *vsStereo = nullptr, *vsRef = nullptr;
  ID3D11PixelShader *psStereo = nullptr, *psRef = nullptr;
  const int cs = api.createVsEx(dev, vsS->GetBufferPointer(), vsS->GetBufferSize(), nullptr, &args, &vsStereo);
  if (cs != 0 || !vsStereo) {
    printf("P1 %s: FAIL CreateVertexShaderEx status %d\n", v.name, cs);
    return 0;
  }
  if (FAILED(dev->CreateVertexShader(vsR->GetBufferPointer(), vsR->GetBufferSize(), nullptr, &vsRef)) ||
      FAILED(dev->CreatePixelShader(psS->GetBufferPointer(), psS->GetBufferSize(), nullptr, &psStereo)) ||
      FAILED(dev->CreatePixelShader(psR->GetBufferPointer(), psR->GetBufferSize(), nullptr, &psRef))) {
    printf("P1 %s: FAIL plain shader creation\n", v.name);
    return 0;
  }
  const D3D11_INPUT_ELEMENT_DESC il[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                         {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  ID3D11InputLayout* layout = nullptr;
  dev->CreateInputLayout(il, 2, vsR->GetBufferPointer(), vsR->GetBufferSize(), &layout);

  // A 24 x 24 grid with a wavy depth: perspective-correct interpolation, depth fights between the two instances.
  constexpr int kN = 24;
  std::vector<float> vb;
  std::vector<uint16_t> ib;
  for (int y = 0; y <= kN; ++y)
    for (int x = 0; x <= kN; ++x) {
      const float fx = -1.0f + 2.0f * x / kN, fy = -1.0f + 2.0f * y / kN;
      const float fz = ((x * 7 + y * 13) % 11) / 11.0f;
      const float vtx[5] = {fx, fy, fz, static_cast<float>(x) / kN, static_cast<float>(y) / kN};
      vb.insert(vb.end(), vtx, vtx + 5);
    }
  for (int y = 0; y < kN; ++y)
    for (int x = 0; x < kN; ++x) {
      const uint16_t a = static_cast<uint16_t>(y * (kN + 1) + x), b = a + 1, c = static_cast<uint16_t>(a + kN + 1), d = c + 1;
      const uint16_t tri[6] = {a, b, c, b, d, c};
      ib.insert(ib.end(), tri, tri + 6);
    }
  D3D11_BUFFER_DESC bd = {static_cast<UINT>(vb.size() * 4), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
  D3D11_SUBRESOURCE_DATA sd = {vb.data(), 0, 0};
  ID3D11Buffer *vbuf = nullptr, *ibuf = nullptr, *cb = nullptr;
  dev->CreateBuffer(&bd, &sd, &vbuf);
  bd = {static_cast<UINT>(ib.size() * 2), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
  sd.pSysMem = ib.data();
  dev->CreateBuffer(&bd, &sd, &ibuf);
  bd = {sizeof(CbData), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  dev->CreateBuffer(&bd, nullptr, &cb);

  D3D11_DEPTH_STENCIL_DESC dsd = {};
  dsd.DepthEnable = TRUE;
  dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dsd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
  dsd.StencilEnable = TRUE;
  dsd.StencilReadMask = dsd.StencilWriteMask = 0xff;
  dsd.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
  dsd.BackFace = dsd.FrontFace;
  ID3D11DepthStencilState* dss = nullptr;
  dev->CreateDepthStencilState(&dsd, &dss);
  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  rd.ScissorEnable = TRUE;
  ID3D11RasterizerState* rs = nullptr;
  dev->CreateRasterizerState(&rd, &rs);

  Targets tS, tR;
  if (!MakeTargets(dev, tS) || !MakeTargets(dev, tR) || !vbuf || !ibuf || !cb || !dss || !rs || !layout) {
    printf("P1 %s: FAIL toy resources\n", v.name);
    return 0;
  }
  Clear(ctx, tS);
  Clear(ctx, tR);

  const float kL[4] = {0.7f, 0.6f, -0.1f, 0.05f};   // periphery: the whole grid in view
  const float kR[4] = {1.9f, 1.7f, 1.1f, -0.2f};    // focus: zoomed and off-centre, clipped by its viewport
  const UINT stride = 20, off = 0;
  ctx->IASetInputLayout(layout);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->IASetVertexBuffers(0, 1, &vbuf, &stride, &off);
  ctx->IASetIndexBuffer(ibuf, DXGI_FORMAT_R16_UINT, 0);
  ctx->VSSetConstantBuffers(0, 1, &cb);
  ctx->OMSetDepthStencilState(dss, 0x45);
  ctx->RSSetState(rs);
  const UINT nIdx = static_cast<UINT>(ib.size());

  // Reference: one view at a time, plain shaders.
  for (int view = 0; view < 2; ++view) {
    CbData c = {};
    memcpy(c.sL, view ? kR : kL, 16);
    memcpy(c.sR, kR, 16);
    ctx->UpdateSubresource(cb, 0, nullptr, &c, 0, 0);
    ctx->OMSetRenderTargets(3, tR.rtvSlice[view], tR.dsvSlice[view]);
    ctx->RSSetViewports(1, &kVp[view]);
    ctx->RSSetScissorRects(1, &kSc[view]);
    ctx->VSSetShader(vsRef, nullptr, 0);
    ctx->PSSetShader(psRef, nullptr, 0);
    ctx->DrawIndexedInstanced(nIdx, 2, 0, 0, 0);
  }
  // Stereo block: both views in one draw.
  {
    CbData c = {};
    memcpy(c.sL, kL, 16);
    memcpy(c.sR, kR, 16);
    c.misc[0] = 1u | (2u << 16);  // view 0 -> viewport 0, view 1 -> viewport 1 (pep::PackViewportMasks)
    ctx->UpdateSubresource(cb, 0, nullptr, &c, 0, 0);
    ctx->OMSetRenderTargets(3, tS.rtvAll, tS.dsvAll);
    ctx->RSSetViewports(2, kVp);
    ctx->RSSetScissorRects(2, kSc);
    const int on = api.setMode(ctx, 2, 1, 1);
    ctx->VSSetShader(vsStereo, nullptr, 0);
    ctx->PSSetShader(psStereo, nullptr, 0);
    ctx->DrawIndexedInstanced(nIdx, 2, 0, 0, 0);
    ctx->VSSetShader(nullptr, nullptr, 0);  // the stereo shader is never bound outside the block
    const int offS = api.setMode(ctx, 1, 0, 0);
    if (on != 0 || offS != 0) printf("P1 %s: SetSinglePassStereoMode on=%d off=%d\n", v.name, on, offS);
  }
  ctx->OMSetRenderTargets(0, nullptr, nullptr);
  ctx->Flush();

  bool viewKnown = true, exact = true;
  std::string line;
  char buf[512];
  for (UINT s = 0; s < 2; ++s) {
    std::vector<uint8_t> a0, a1, a2, adp, b0, b1, b2, bdp;
    if (!ReadSlice(dev, ctx, tS.col[0], s, 16, a0) || !ReadSlice(dev, ctx, tS.col[1], s, 16, a1) || !ReadSlice(dev, ctx, tS.col[2], s, 16, a2) ||
        !ReadSlice(dev, ctx, tS.depth, s, 8, adp) || !ReadSlice(dev, ctx, tR.col[0], s, 16, b0) || !ReadSlice(dev, ctx, tR.col[1], s, 16, b1) ||
        !ReadSlice(dev, ctx, tR.col[2], s, 16, b2) || !ReadSlice(dev, ctx, tR.depth, s, 8, bdp)) {
      printf("P1 %s: FAIL readback\n", v.name);
      return 0;
    }
    const size_t px = static_cast<size_t>(kW) * kH;
    size_t covered = 0, refCovered = 0, dCov = 0, dAttr = 0, dPos = 0, dDepth = 0, dStencil = 0;
    uint32_t lo[3] = {~0u, ~0u, ~0u}, hi[3] = {0, 0, 0};
    for (size_t i = 0; i < px; ++i) {
      uint32_t w[4], r[4];
      memcpy(w, &a0[i * 16], 16);
      memcpy(r, &b0[i * 16], 16);
      const bool cov = w[3] == 0xC0DE, rcov = r[3] == 0xC0DE;
      covered += cov;
      refCovered += rcov;
      dCov += cov != rcov;
      if (cov)
        for (int k = 0; k < 3; ++k) {
          if (w[k] < lo[k]) lo[k] = w[k];
          if (w[k] > hi[k]) hi[k] = w[k];
        }
      dAttr += memcmp(&a1[i * 16], &b1[i * 16], 16) != 0;
      dPos += memcmp(&a2[i * 16], &b2[i * 16], 16) != 0;
      dDepth += memcmp(&adp[i * 8], &bdp[i * 8], 4) != 0;
      dStencil += adp[i * 8 + 4] != bdp[i * 8 + 4];
    }
    if (covered == 0 || dCov || dAttr || dPos || dDepth || dStencil) exact = false;
    const bool wantsRt = v.vsRt || v.psRt, wantsVp = v.vsVp || v.psVp;
    if (wantsRt && !(lo[0] == s && hi[0] == s)) viewKnown = false;
    if (wantsVp && !(lo[1] == s && hi[1] == s)) viewKnown = false;
    if (v.eye && !(lo[2] == s && hi[2] == s)) viewKnown = false;
    snprintf(buf, sizeof(buf), " | slice%u covered=%zu ref=%zu rt=[%x..%x] vp=[%x..%x] eye=[%x..%x] diff(cov/attr/pos/depth/stencil)=%zu/%zu/%zu/%zu/%zu",
             s, covered, refCovered, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], dCov, dAttr, dPos, dDepth, dStencil);
    line += buf;
  }
  const bool asks = v.vsRt || v.psRt || v.vsVp || v.psVp || v.eye;
  printf("P1 %s: view=%s exact=%s%s\n", v.name, !asks ? "n/a" : viewKnown ? "KNOWN" : "unknown", exact ? "yes" : "NO", line.c_str());
  fflush(stdout);
  return 0;
}

// ---------------------------------------------------------------------------
// The merged pass in small ("patched_clone"): a DCS-shaped vertex shader (TerrainContext at b11, the position from
// rows 20-23, a surfaceLod row picked by a clamped index, an instance id) is cloned by pep::PatchVsDualPosition and
// created through NvAPI; one stereo draw with the periphery's TerrainContext bound and the focus rows injected is
// compared with two stock draws (stock shader, periphery / focus TerrainContext). The pixel shader of the merged
// draw picks its view's constants by SV_RenderTargetArrayIndex (the remedy of design 4.3); the stock one reads
// constant 0. TerrainContext rows: census pair 1 of per_eye_pass_test.h (R26, F-4E Germany F2, left eye).
// With QV_PEP_ARCHIVE=<copy of metashaders2>: every clone of that cache is also offered to
// NvAPI_D3D11_CreateVertexShaderEx (created and released; nothing drawn).
// ---------------------------------------------------------------------------
const char kCloneHlsl[] = R"(
cbuffer cPerFrame : register(b6) { float4 gFrame[4]; };
cbuffer TerrainContext : register(b11) { float4 tc[136]; };
cbuffer cView : register(b0) { float4 viewConst[2]; };
struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; uint iid : SV_InstanceID; };
struct VSOut { float4 pos : SV_Position; float3 wpos : TEXCOORD0; float2 uv : TEXCOORD1; };
VSOut vs(VSIn i) {
  VSOut o;
  int l = max(min((int)gFrame[3].z, 3), 0);
  float3 p = i.pos * gFrame[0].w + gFrame[0].xyz + tc[45 + l].xyz * (float)i.iid;
  float4 w = float4(p, 1);
  o.pos = float4(dot(w, tc[20]), dot(w, tc[21]), dot(w, tc[22]), dot(w, tc[23]));
  o.wpos = p;
  o.uv = i.uv;
  return o;
}
struct PO { uint4 o0 : SV_Target0; float4 o1 : SV_Target1; float4 o2 : SV_Target2; };
PO ps(VSOut i
#if MERGED
  , nointerpolation uint rt : SV_RenderTargetArrayIndex
#endif
) {
#if !MERGED
  uint rt = 0;
#endif
  PO o;
  o.o0 = uint4(rt, 0, 0, 0xC0DE);
  o.o1 = float4(i.wpos, i.pos.w);
  o.o2 = float4(i.pos.xyz, viewConst[rt].x * i.uv.x + viewConst[rt].y);
  return o;
}
)";
const uint32_t kShared[12] = {0x3e52294c, 0x3c9da20b, 0x3f7a8083, 0x416be36c, 0x3e8161de, 0x3f770152,
                              0xbd9367a0, 0x410ea8b0, 0x3f720e0e, 0xbe862ab3, 0xbe45cc40, 0x3f84da88};
const uint32_t kPeriphRows[16] = {0x3f47dedb, 0x00000000, 0x3e4880cb, 0x00000000, 0x00000000, 0x3f4a678c,
                                  0x3a86d904, 0x00000000, 0x3eb0d4e8, 0xbd14a09f, 0x3f39e547, 0x413b6b8f,
                                  0x3e4d961e, 0x3f433948, 0xbd69e728, 0x40e19e5d};
const uint32_t kFocusRows[16] = {0x4012f6b2, 0x00000000, 0x3dc6fa0a, 0x00000000, 0x00000000, 0x4014d3ac,
                                 0x39d95be9, 0x00000000, 0x3f102a7c, 0x3c996923, 0x400e9b36, 0x4207d247,
                                 0x3f1688cf, 0x400f9737, 0xbe2b7892, 0x41a5dfd7};

void MakeTc(bool focus, uint8_t* tc) {
  memset(tc, 0, pep::kTcBytes);
  memcpy(tc, kShared, 48);
  memcpy(tc + 16 * 23, kShared + 8, 16);  // the clip w row = row 2
  const float z[4] = {0, 0, 0, 0.02f};
  memcpy(tc + 16 * 22, z, 16);
  const uint32_t* v = focus ? kFocusRows : kPeriphRows;
  memcpy(tc + 16 * 16, v, 32);
  memcpy(tc + 16 * 20, v + 8, 32);
  const float size[4] = {0, 0, focus ? 512.0f : 640.0f, focus ? 512.0f : 400.0f};
  memcpy(tc + 16 * 44, size, 16);
  for (int l = 0; l < 4; ++l) {
    const float lod[4] = {3.0f + l, 1.0f, 2.0f - l, 0};
    memcpy(tc + 16 * (45 + l), lod, 16);
  }
}

ID3DBlob* CompileClone(const char* entry, const char* target, bool merged) {
  const D3D_SHADER_MACRO m[] = {{"MERGED", merged ? "1" : "0"}, {nullptr, nullptr}};
  ID3DBlob *code = nullptr, *err = nullptr;
  if (FAILED(D3DCompile(kCloneHlsl, sizeof(kCloneHlsl) - 1, "p1clone", m, nullptr, entry, target, 0, 0, &code, &err)))
    printf("P1 patched_clone: compile %s failed: %s\n", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "");
  if (err) err->Release();
  return code;
}

// Every clone of a shader cache folder through NvAPI's creation (the start-up self-test of design 4.8, offline).
void OfferCacheClones(ID3D11Device* dev, const nvsps::Api& api, const std::wstring& dir) {
  uint32_t clones = 0, accepted = 0, files = 0;
  int firstStatus = 0;
  std::unordered_set<uint64_t> seen;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*.meta2").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    printf("P1 dcs_clones: no *.meta2 files in the folder of QV_PEP_ARCHIVE\n");
    return;
  }
  do {
    std::vector<uint8_t> f;
    if (!pep::ReadFileBytes(dir + L"\\" + fd.cFileName, f, 256ull << 20)) continue;
    ++files;
    for (size_t i = 0; i + 32 <= f.size();) {
      const void* hit = memchr(&f[i], 'D', f.size() - i - 3);
      if (!hit) break;
      i = static_cast<size_t>(static_cast<const uint8_t*>(hit) - f.data());
      const uint32_t size = pep::ContainerSize(&f[i], f.size() - i);
      if (!size) {
        ++i;
        continue;
      }
      const uint8_t* b = &f[i];
      i += size;
      uint64_t key = 0;
      memcpy(&key, b + 4, 8);
      if (!pep::ChecksumOk(b, size) || !seen.insert(key).second) continue;
      const pep::PatchResult r = pep::PatchVsDualPosition(b, size);
      if (r.why != pep::kPwOk) continue;
      ++clones;
      nvsps::CustomSemantic sem[2] = {nvsps::MakeSemantic(nvsps::kSemXyzwRight, "NV_XYZW_RIGHT"),
                                      nvsps::MakeSemantic(nvsps::kSemViewportMask, "NV_VIEWPORT_MASK")};
      nvsps::CreateVsEx args = {nvsps::kCreateVsExVer, 2u, sem, FALSE, FALSE};
      ID3D11VertexShader* vs = nullptr;
      const int st = api.createVsEx(dev, r.blob.data(), r.blob.size(), nullptr, &args, &vs);
      if (st == 0 && vs)
        ++accepted;
      else if (!firstStatus)
        firstStatus = st ? st : -9999;
      if (vs) vs->Release();
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  printf("P1 dcs_clones: %u cache files, %u vertex shaders cloned, NvAPI_D3D11_CreateVertexShaderEx accepted %u, refused "
         "%u (first status %d)\n", files, clones, accepted, clones - accepted, firstStatus);
}

int RunClone() {
  const char* name = "patched_clone";
  IDXGIFactory1* fac = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&fac)))) return 3;
  IDXGIAdapter1* ad = nullptr;
  DXGI_ADAPTER_DESC1 desc = {};
  for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
    ad->GetDesc1(&desc);
    if (desc.VendorId == 0x10de) break;
    Rel(ad);
  }
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  nvsps::Api api;
  if (!ad || FAILED(D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx)) ||
      nvsps::Bind(LoadLibraryW(L"nvapi64.dll"), api) != 0) {
    printf("P1 %s: SKIP no NVIDIA device or NVAPI\n", name);
    return 3;
  }
  ID3DBlob* vsB = CompileClone("vs", "vs_5_0", false);
  ID3DBlob* psStockB = CompileClone("ps", "ps_5_0", false);
  ID3DBlob* psMergedB = CompileClone("ps", "ps_5_0", true);
  if (!vsB || !psStockB || !psMergedB) return 0;
  const pep::PatchResult r = pep::PatchVsDualPosition(static_cast<const uint8_t*>(vsB->GetBufferPointer()), vsB->GetBufferSize());
  if (r.why != pep::kPwOk) {
    printf("P1 %s: FAIL the patcher refused the shader: %s\n", name, pep::PatchWhyName(r.why));
    return 0;
  }
  nvsps::CustomSemantic sem[2] = {nvsps::MakeSemantic(nvsps::kSemXyzwRight, "NV_XYZW_RIGHT"),
                                  nvsps::MakeSemantic(nvsps::kSemViewportMask, "NV_VIEWPORT_MASK")};
  nvsps::CreateVsEx args = {nvsps::kCreateVsExVer, 2u, sem, FALSE, FALSE};
  ID3D11VertexShader *vsClone = nullptr, *vsStock = nullptr;
  ID3D11PixelShader *psStock = nullptr, *psMerged = nullptr;
  const int cs = api.createVsEx(dev, r.blob.data(), r.blob.size(), nullptr, &args, &vsClone);
  if (cs != 0 || !vsClone) {
    printf("P1 %s: FAIL CreateVertexShaderEx refused the clone, status %d\n", name, cs);
    return 0;
  }
  dev->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &vsStock);
  dev->CreatePixelShader(psStockB->GetBufferPointer(), psStockB->GetBufferSize(), nullptr, &psStock);
  dev->CreatePixelShader(psMergedB->GetBufferPointer(), psMergedB->GetBufferSize(), nullptr, &psMerged);
  const D3D11_INPUT_ELEMENT_DESC il[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                         {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  ID3D11InputLayout* layout = nullptr;
  dev->CreateInputLayout(il, 2, vsB->GetBufferPointer(), vsB->GetBufferSize(), &layout);

  // A grid in front of the census camera: view coordinates (x, y, depth) taken back to the world (R transposed).
  float rs[12];
  memcpy(rs, kShared, 48);
  constexpr int kN = 32;
  std::vector<float> vb;
  std::vector<uint16_t> ib;
  for (int y = 0; y <= kN; ++y)
    for (int x = 0; x <= kN; ++x) {
      const float u = -1.0f + 2.0f * x / kN, w = -1.0f + 2.0f * y / kN;
      const float view[3] = {u * 12.0f - rs[3], w * 12.0f - rs[7], 20.0f + 8.0f * ((x * 7 + y * 13) % 11) / 11.0f - rs[11]};
      float vtx[5] = {0, 0, 0, static_cast<float>(x) / kN, static_cast<float>(y) / kN};
      for (int k = 0; k < 3; ++k) vtx[k] = rs[k] * view[0] + rs[4 + k] * view[1] + rs[8 + k] * view[2];
      vb.insert(vb.end(), vtx, vtx + 5);
    }
  for (int y = 0; y < kN; ++y)
    for (int x = 0; x < kN; ++x) {
      const uint16_t a = static_cast<uint16_t>(y * (kN + 1) + x), b = a + 1, c = static_cast<uint16_t>(a + kN + 1), d = c + 1;
      const uint16_t tri[6] = {a, b, c, b, d, c};
      ib.insert(ib.end(), tri, tri + 6);
    }
  uint8_t tcP[pep::kTcBytes], tcF[pep::kTcBytes];
  MakeTc(false, tcP);
  MakeTc(true, tcF);
  std::vector<float> inject((r.rows.size() + 1) * 4, 0.0f);  // row i = the focus row r.rows[i]; the last row's x = the mask
  for (size_t i = 0; i < r.rows.size(); ++i) memcpy(&inject[i * 4], tcF + 16 * r.rows[i], 16);
  const uint32_t mask = pep::PackViewportMasks(1, 2);
  memcpy(&inject[r.rows.size() * 4], &mask, 4);
  const float frame[16] = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2.0f, 0};
  const float kP[2] = {0.25f, 1.5f}, kF[2] = {0.75f, -2.0f};

  auto buffer = [&](UINT bytes, UINT bind, const void* data) {
    D3D11_BUFFER_DESC bd = {bytes, D3D11_USAGE_DEFAULT, bind, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {data, 0, 0};
    ID3D11Buffer* b = nullptr;
    dev->CreateBuffer(&bd, data ? &sd : nullptr, &b);
    return b;
  };
  ID3D11Buffer* vbuf = buffer(static_cast<UINT>(vb.size() * 4), D3D11_BIND_VERTEX_BUFFER, vb.data());
  ID3D11Buffer* ibuf = buffer(static_cast<UINT>(ib.size() * 2), D3D11_BIND_INDEX_BUFFER, ib.data());
  ID3D11Buffer* cbP = buffer(pep::kTcBytes, D3D11_BIND_CONSTANT_BUFFER, tcP);
  ID3D11Buffer* cbF = buffer(pep::kTcBytes, D3D11_BIND_CONSTANT_BUFFER, tcF);
  ID3D11Buffer* cbInject = buffer(static_cast<UINT>(inject.size() * 4), D3D11_BIND_CONSTANT_BUFFER, inject.data());
  ID3D11Buffer* cbFrame = buffer(64, D3D11_BIND_CONSTANT_BUFFER, frame);
  ID3D11Buffer* cbView = buffer(32, D3D11_BIND_CONSTANT_BUFFER, nullptr);
  D3D11_DEPTH_STENCIL_DESC dsd = {};
  dsd.DepthEnable = TRUE;
  dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dsd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
  dsd.StencilEnable = TRUE;
  dsd.StencilReadMask = dsd.StencilWriteMask = 0xff;
  dsd.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
  dsd.BackFace = dsd.FrontFace;
  ID3D11DepthStencilState* dss = nullptr;
  dev->CreateDepthStencilState(&dsd, &dss);
  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  rd.ScissorEnable = TRUE;
  ID3D11RasterizerState* rs2 = nullptr;
  dev->CreateRasterizerState(&rd, &rs2);
  Targets tS, tR;
  if (!MakeTargets(dev, tS) || !MakeTargets(dev, tR) || !vbuf || !ibuf || !cbP || !cbF || !cbInject || !cbFrame || !cbView ||
      !dss || !rs2 || !layout || !vsStock || !psStock || !psMerged) {
    printf("P1 %s: FAIL toy resources\n", name);
    return 0;
  }
  Clear(ctx, tS);
  Clear(ctx, tR);
  const UINT stride = 20, off = 0;
  ctx->IASetInputLayout(layout);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->IASetVertexBuffers(0, 1, &vbuf, &stride, &off);
  ctx->IASetIndexBuffer(ibuf, DXGI_FORMAT_R16_UINT, 0);
  ctx->VSSetConstantBuffers(6, 1, &cbFrame);
  ctx->PSSetConstantBuffers(0, 1, &cbView);
  ctx->OMSetDepthStencilState(dss, 0x45);
  ctx->RSSetState(rs2);
  const UINT nIdx = static_cast<UINT>(ib.size());
  for (int view = 0; view < 2; ++view) {  // stock: one view at a time
    const float vc[8] = {(view ? kF : kP)[0], (view ? kF : kP)[1], 0, 0, 0, 0, 0, 0};
    ctx->UpdateSubresource(cbView, 0, nullptr, vc, 0, 0);
    ctx->VSSetConstantBuffers(11, 1, view ? &cbF : &cbP);
    ctx->OMSetRenderTargets(3, tR.rtvSlice[view], tR.dsvSlice[view]);
    ctx->RSSetViewports(1, &kVp[view]);
    ctx->RSSetScissorRects(1, &kSc[view]);
    ctx->VSSetShader(vsStock, nullptr, 0);
    ctx->PSSetShader(psStock, nullptr, 0);
    ctx->DrawIndexedInstanced(nIdx, 2, 0, 0, 0);
  }
  {  // merged: the clone, the periphery's TerrainContext, the focus rows injected
    const float vc[8] = {kP[0], kP[1], 0, 0, kF[0], kF[1], 0, 0};
    ctx->UpdateSubresource(cbView, 0, nullptr, vc, 0, 0);
    ctx->VSSetConstantBuffers(11, 1, &cbP);
    ctx->VSSetConstantBuffers(r.slot, 1, &cbInject);
    ctx->OMSetRenderTargets(3, tS.rtvAll, tS.dsvAll);
    ctx->RSSetViewports(2, kVp);
    ctx->RSSetScissorRects(2, kSc);
    api.setMode(ctx, 2, 1, 1);
    ctx->VSSetShader(vsClone, nullptr, 0);
    ctx->PSSetShader(psMerged, nullptr, 0);
    ctx->DrawIndexedInstanced(nIdx, 2, 0, 0, 0);
    ctx->VSSetShader(nullptr, nullptr, 0);
    api.setMode(ctx, 1, 0, 0);
  }
  ctx->OMSetRenderTargets(0, nullptr, nullptr);
  ctx->Flush();

  bool exact = true, views = true;
  std::string line;
  char buf[384];
  for (UINT s = 0; s < 2; ++s) {
    std::vector<uint8_t> a[4], b[4];
    for (int c = 0; c < 4; ++c)
      if (!ReadSlice(dev, ctx, c < 3 ? tS.col[c] : tS.depth, s, c < 3 ? 16 : 8, a[c]) ||
          !ReadSlice(dev, ctx, c < 3 ? tR.col[c] : tR.depth, s, c < 3 ? 16 : 8, b[c])) {
        printf("P1 %s: FAIL readback\n", name);
        return 0;
      }
    const size_t px = static_cast<size_t>(kW) * kH;
    size_t covered = 0, refCovered = 0, dCov = 0, dAttr = 0, dPos = 0, dDepth = 0, dStencil = 0, wrongView = 0;
    for (size_t i = 0; i < px; ++i) {
      uint32_t w[4], q[4];
      memcpy(w, &a[0][i * 16], 16);
      memcpy(q, &b[0][i * 16], 16);
      const bool cov = w[3] == 0xC0DE, rcov = q[3] == 0xC0DE;
      covered += cov, refCovered += rcov, dCov += cov != rcov;
      wrongView += cov && w[0] != s;
      dAttr += memcmp(&a[1][i * 16], &b[1][i * 16], 16) != 0;
      dPos += memcmp(&a[2][i * 16], &b[2][i * 16], 16) != 0;
      dDepth += memcmp(&a[3][i * 8], &b[3][i * 8], 4) != 0;
      dStencil += a[3][i * 8 + 4] != b[3][i * 8 + 4];
    }
    if (covered == 0 || dCov || dAttr || dPos || dDepth || dStencil) exact = false;
    if (wrongView) views = false;
    snprintf(buf, sizeof(buf), " | slice%u covered=%zu stock=%zu wrongView=%zu diff(cov/attr/pos+viewConst/depth/stencil)=%zu/%zu/%zu/%zu/%zu",
             s, covered, refCovered, wrongView, dCov, dAttr, dPos, dDepth, dStencil);
    line += buf;
  }
  printf("P1 %s: clone repeats %u of %u instructions, %u computed-row reads proven shared, %zu focus rows at b%u, "
         "CreateVertexShaderEx=%d | merged = two stock passes: %s, view index: %s%s\n",
         name, r.twinned, r.ins, r.boundedRows, r.rows.size(), r.slot, cs, exact ? "EXACT" : "DIFFERENT", views ? "right" : "WRONG",
         line.c_str());
  wchar_t arc[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(L"QV_PEP_ARCHIVE", arc, MAX_PATH);
  if (n > 0 && n < MAX_PATH) OfferCacheClones(dev, api, arc);
  fflush(stdout);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) {
    if (strcmp(argv[1], "clone") == 0) return RunClone();
    const int i = atoi(argv[1]);
    return i >= 0 && i < kVariantCount ? RunVariant(i) : 2;
  }
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  for (int i = 0; i <= kVariantCount; ++i) {  // the last one: the patched clone
    const char* name = i < kVariantCount ? kVariants[i].name : "patched_clone";
    wchar_t cmd[MAX_PATH + 32];
    if (i < kVariantCount)
      swprintf_s(cmd, L"\"%s\" %d", exe, i);
    else
      swprintf_s(cmd, L"\"%s\" clone", exe);
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    fflush(stdout);
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
      printf("P1 %s: could not start\n", name);
      continue;
    }
    DWORD code = STILL_ACTIVE;
    if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 99);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code == 3) return 3;
    if (code != 0) printf("P1 %s: CRASH exit code 0x%08lx\n", name, code);
  }
  return 0;
}
