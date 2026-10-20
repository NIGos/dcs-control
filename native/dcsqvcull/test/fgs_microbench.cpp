// R38 rank 2 offline microbenchmark: one eye's periphery + focus G-buffer in
// one geometry pass (viewport broadcast) against DCS's two passes. No DCS.
//
// Workload (a stand-in for DCS's GPU-driven forest in the F-4E Germany F2
// bench, R26/R38): a camera 3 m above a ground plane, pitched 3 deg down;
// ~140k trees of 24 alpha-tested leaf cards (48 triangles each) spread
// uniformly over the ground out to 4 km, in 64 instanced draws (DCS's
// buckets). Per-instance data from an instance stream; a VS with a little
// wind ALU and cur/prev clip outputs (motion vectors, as GBuffer.hlsl); a PS
// with one biased texture sample, alpha test and DCS's G-buffer layout:
// SV_Target0-4 = slices of one R8G8_UNORM Texture2DArray, SV_Target5 =
// R16G16_FLOAT motion vectors, D32_FLOAT_S8X24 depth, reverse-Z,
// GREATER_EQUAL [V census gdd_res: 476x468 / 1176x1164, fmt 49 x5, 34, 19].
//
// Views (DCS with DLSS, R38 3.1 / memory dcs-depth-targets): periphery
// 476x468; focus 1176x1164 = the periphery's NDC window of half-size 0.34
// centred on the gaze (on the horizon), projection x_f = (x_p - cx w)/s.
//
// Variants (each timed with D3D11 timestamps, median of the rounds; the
// pipeline statistics of one run are printed):
//   A    two passes, DCS-style: periphery (all instances, P_p) into the
//        periphery targets, then focus (the focus-frustum list, P_f) into
//        the focus targets. Also each pass alone (Ap, Af).
//   Ag   the focus pass with a 16x16 viewport at (-64,-64): every triangle
//        fetched, shaded and set up, no pixel (the live GeoOnlyProbe).
//   B    NVAPI FastGS (explicit passthrough, maxvertexcount(1), viewport
//        mask 0b11): all periphery instances once; viewport 0 = periphery,
//        viewport 1 = the focus as an affine of the periphery's NDC;
//        2D atlas (focus at x 0, periphery at x 1176) with a scissor per
//        viewport.
//   B1   the same FastGS with mask 0b01 (periphery only): the GS cost alone.
//   B2   FastGS mask 0b11 + OFFSET_RT_INDEX_BY_VP_INDEX into 2-slice arrays
//        at focus size (periphery in slice 0's corner): no scissor needed.
//   C    NVAPI VS custom semantic NV_VIEWPORT_MASK (no GS), atlas.
//   D    no NVAPI, D3D11.3 VPAndRTArrayIndexFromAnyShaderFeedingRasterizer:
//        instance count x2, VS writes SV_ViewportArrayIndex = id & 1
//        (instance stream step rate 2): every vertex shaded twice.
//   E    no NVAPI: a regular GS emitting each triangle twice (viewport 0, 1).
// Exactness: the focus outputs of B/B2/C/D/E against A's (bit compare of the
// five RG8 slices and depth; motion vectors after the linear fixup
// MV_f = MV_p * (dims_f / dims_p) / s); the periphery outputs against A's.
//
// Usage: fgs_microbench.exe [rounds] [instances]   (defaults 40, 140000)
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "../src/nvapi_fgs_min.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

template <class T>
void Release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}
#define CHECK(x)                                                                \
  do {                                                                          \
    HRESULT hr_ = (x);                                                          \
    if (FAILED(hr_)) {                                                          \
      printf("FAILED 0x%08lx at %s:%d: %s\n", (unsigned long)hr_, __FILE__, __LINE__, #x); \
      exit(2);                                                                  \
    }                                                                           \
  } while (0)

double Median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;

// ------------------------------------------------------------------ views --
constexpr UINT kPw = 476, kPh = 468;     // periphery (DLSS internal)
constexpr UINT kFw = 1176, kFh = 1164;   // focus (DLSS internal)
constexpr UINT kAw = kFw + kPw, kAh = kFh;  // atlas: focus at x 0, periphery at x kFw
constexpr float kSec = 0.34f;            // focus half-size in periphery NDC
constexpr float kNear = 0.02f;

struct M4 {
  float m[4][4];
};
M4 Mul(const M4& a, const M4& b) {
  M4 r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
  return r;
}
// Row-vector convention: clip = p * M. View: camera at (0, h, 0) looking +z, pitched down.
M4 View(float h, float pitchDeg) {
  const float a = pitchDeg * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
  // world -> camera: translate by -eye, then rotate about x by +pitch (look down = positive a)
  M4 t{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, -h, 0, 1}}};
  M4 r{{{1, 0, 0, 0}, {0, c, s, 0}, {0, -s, c, 0}, {0, 0, 0, 1}}};
  return Mul(t, r);
}
// Reverse-Z infinite, asymmetric (DCS periphery: row x (0.78074, 0, 0.1958, 0)).
M4 Proj(float sx, float sy, float ox, float oy) {
  return M4{{{sx, 0, 0, 0}, {0, sy, 0, 0}, {ox, oy, 0, 1}, {0, 0, kNear, 0}}};
}
// The focus as an NDC affine of the periphery: x_f = (x_p - cx w) / s.
M4 FocusOf(const M4& p, float cx, float cy) {
  M4 f = p;
  for (int r = 0; r < 4; ++r) {
    f.m[r][0] = (p.m[r][0] - cx * p.m[r][3]) / kSec;
    f.m[r][1] = (p.m[r][1] - cy * p.m[r][3]) / kSec;
  }
  return f;
}
// Viewport that maps periphery NDC onto the focus target placed at (ox, oy).
D3D11_VIEWPORT FocusViewport(float cx, float cy, float ox, float oy) {
  D3D11_VIEWPORT v{};
  v.Width = kFw / kSec;
  v.Height = kFh / kSec;
  v.TopLeftX = ox + 0.5f * kFw * (1.0f - (cx + 1.0f) / kSec);
  v.TopLeftY = oy + 0.5f * kFh * (1.0f - (1.0f - cy) / kSec);
  v.MinDepth = 0, v.MaxDepth = 1;
  return v;
}

// ----------------------------------------------------------------- shaders --
const char kHlsl[] = R"(
cbuffer CB : register(b0) {
  row_major float4x4 VP;
  row_major float4x4 PrevVP;
  float4 Dims;    // target w, h
  float4 Misc;    // x time, y mip bias
};
struct VSIn {
  float3 pos : POSITION; float2 uv : TEXCOORD0; float3 nrm : NORMAL;
  float4 inst : INST0;   // xyz position, w scale
  float4 inst2 : INST1;  // x yaw sin, y yaw cos, z phase, w tint
  uint iid : SV_InstanceID;
};
struct VSOut {
  float4 pos : SV_Position; float2 uv : TEXCOORD0; float3 nrm : TEXCOORD1;
  float4 cur : TEXCOORD2; float4 prev : TEXCOORD3; float tint : TEXCOORD4;
};
VSOut Body(VSIn i) {
  float3 p = i.pos * i.inst.w;
  float3 r = float3(p.x * i.inst2.y - p.z * i.inst2.x, p.y, p.x * i.inst2.x + p.z * i.inst2.y);
  float sway = sin(Misc.x * 1.7 + i.inst2.z) * 0.02 * p.y + sin(Misc.x * 3.1 + i.inst2.z * 2.0) * 0.01 * p.y;
  r.x += sway; r.z += 0.5 * sway;
  float3 w = r + i.inst.xyz;
  VSOut o;
  o.cur = mul(float4(w, 1), VP);
  o.prev = mul(float4(w + float3(0.01, 0, 0), 1), PrevVP);
  o.pos = o.cur;
  o.uv = i.uv;
  o.nrm = float3(i.nrm.x * i.inst2.y - i.nrm.z * i.inst2.x, i.nrm.y, i.nrm.x * i.inst2.x + i.nrm.z * i.inst2.y);
  o.tint = i.inst2.w;
  return o;
}
VSOut VS(VSIn i) { return Body(i); }

struct VSOutVp { VSOut v; uint vp : SV_ViewportArrayIndex; };
VSOutVp VSVp(VSIn i) { VSOutVp o; o.v = Body(i); o.vp = i.iid & 1; return o; }

struct VSOutMask { VSOut v; uint mask : NV_VIEWPORT_MASK; };
VSOutMask VSMask(VSIn i) { VSOutMask o; o.v = Body(i); o.mask = 3; return o; }

struct GSOut { VSOut v; uint vp : SV_ViewportArrayIndex; };
struct GSOutRt { VSOut v; uint vp : SV_ViewportArrayIndex; uint rt : SV_RenderTargetArrayIndex; };
[maxvertexcount(1)] void GSFast(triangle VSOut i[3], inout TriangleStream<GSOut> s) {
  GSOut o; o.v = i[0]; o.vp = MASK; s.Append(o);
}
[maxvertexcount(1)] void GSFastRt(triangle VSOut i[3], inout TriangleStream<GSOutRt> s) {
  GSOutRt o; o.v = i[0]; o.vp = 3; o.rt = 0; s.Append(o);
}
[maxvertexcount(6)] void GSDup(triangle VSOut i[3], inout TriangleStream<GSOut> s) {
  [unroll] for (uint v = 0; v < 2; ++v) {
    [unroll] for (uint k = 0; k < 3; ++k) { GSOut o; o.v = i[k]; o.vp = v; s.Append(o); }
    s.RestartStrip();
  }
}

Texture2D Tex : register(t0);
SamplerState Smp : register(s0);
struct PSOut {
  float2 g0 : SV_Target0; float2 g1 : SV_Target1; float2 g2 : SV_Target2; float2 g3 : SV_Target3;
  float2 g4 : SV_Target4; float2 mv : SV_Target5;
};
PSOut PS(VSOut i) {
  float4 a = Tex.SampleBias(Smp, i.uv, Misc.y);
  clip(a.a - 0.5);
  float3 n = normalize(i.nrm);
  float2 c = i.cur.xy / i.cur.w, p = i.prev.xy / i.prev.w;
  PSOut o;
  o.g0 = a.rg * i.tint; o.g1 = float2(a.b, 1.0); o.g2 = n.xy * 0.5 + 0.5; o.g3 = float2(n.z * 0.5 + 0.5, 0.3);
  o.g4 = float2(0.5, a.a);
  o.mv = 0.5 * Dims.xy * (p - c);
  return o;
}
)";

ID3DBlob* Compile(const char* entry, const char* target, const char* mask = "3") {
  ID3DBlob* code = nullptr;
  ID3DBlob* err = nullptr;
  D3D_SHADER_MACRO defs[] = {{"MASK", mask}, {nullptr, nullptr}};
  HRESULT hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "fgs", defs, nullptr, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
  if (FAILED(hr)) {
    printf("compile %s failed: %s\n", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    Release(err);
    return nullptr;
  }
  Release(err);
  return code;
}

// ---------------------------------------------------------------- targets --
struct Targets {
  UINT w = 0, h = 0, slices = 1;  // slices per logical target (2 for B2's arrays)
  ID3D11Texture2D* g = nullptr;   // R8G8 array, 5 * slices
  ID3D11Texture2D* mv = nullptr;  // R16G16F array, slices
  ID3D11Texture2D* ds = nullptr;  // R32G8X24 array, slices
  ID3D11RenderTargetView* rtv[6] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  void Create(UINT w_, UINT h_, UINT s_) {
    w = w_, h = h_, slices = s_;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w, d.Height = h, d.MipLevels = 1, d.SampleDesc.Count = 1, d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.ArraySize = 5 * slices, d.Format = DXGI_FORMAT_R8G8_UNORM;
    CHECK(g_dev->CreateTexture2D(&d, nullptr, &g));
    d.ArraySize = slices, d.Format = DXGI_FORMAT_R16G16_FLOAT;
    CHECK(g_dev->CreateTexture2D(&d, nullptr, &mv));
    d.Format = DXGI_FORMAT_R32G8X24_TYPELESS, d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    CHECK(g_dev->CreateTexture2D(&d, nullptr, &ds));
    for (UINT k = 0; k < 5; ++k) {
      // Layout of the 5*slices array: target k of view v at slice k*slices + v (B2: RTV k spans both views).
      D3D11_RENDER_TARGET_VIEW_DESC rd{};
      rd.Format = DXGI_FORMAT_R8G8_UNORM, rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
      rd.Texture2DArray.FirstArraySlice = k * slices, rd.Texture2DArray.ArraySize = slices;
      CHECK(g_dev->CreateRenderTargetView(g, &rd, &rtv[k]));
    }
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = DXGI_FORMAT_R16G16_FLOAT, rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    rd.Texture2DArray.ArraySize = slices;
    CHECK(g_dev->CreateRenderTargetView(mv, &rd, &rtv[5]));
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT, dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
    dd.Texture2DArray.ArraySize = slices;
    CHECK(g_dev->CreateDepthStencilView(ds, &dd, &dsv));
  }
  void Clear() {
    const float z[4] = {0, 0, 0, 0};
    for (auto* r : rtv) g_ctx->ClearRenderTargetView(r, z);
    g_ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
  }
  void Bind() { g_ctx->OMSetRenderTargets(6, rtv, dsv); }
  void Free() {
    for (auto*& r : rtv) Release(r);
    Release(dsv), Release(g), Release(mv), Release(ds);
  }
};

// Readback of a region of one view (slice v) of a Targets: 5 x RG8, MV RG16F as float, depth float.
struct Image {
  UINT w = 0, h = 0;
  std::vector<uint16_t> g[5];
  std::vector<float> mvx, mvy, depth;
};
float HalfToFloat(uint16_t h) {
  const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
  float v;
  if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
  else if (e == 31) v = m ? NAN : INFINITY;
  else v = std::ldexp(static_cast<float>(m | 1024), static_cast<int>(e) - 25);
  return s ? -v : v;
}
template <class T>
void ReadSub(ID3D11Texture2D* tex, UINT sub, UINT x0, UINT y0, UINT w, UINT h, UINT bpp, std::vector<T>& out,
             UINT comps) {
  D3D11_TEXTURE2D_DESC d{};
  tex->GetDesc(&d);
  D3D11_TEXTURE2D_DESC s = d;
  s.ArraySize = 1, s.BindFlags = 0, s.Usage = D3D11_USAGE_STAGING, s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* st = nullptr;
  CHECK(g_dev->CreateTexture2D(&s, nullptr, &st));
  g_ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, sub, nullptr);  // whole subresource (depth-stencil rule)
  D3D11_MAPPED_SUBRESOURCE m{};
  CHECK(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
  out.resize(static_cast<size_t>(w) * h * comps);
  for (UINT y = 0; y < h; ++y)
    memcpy(&out[static_cast<size_t>(y) * w * comps], static_cast<uint8_t*>(m.pData) + (y0 + y) * m.RowPitch + x0 * bpp,
           static_cast<size_t>(w) * bpp);
  g_ctx->Unmap(st, 0);
  Release(st);
}
Image Read(const Targets& t, UINT view, UINT x0, UINT y0, UINT w, UINT h) {
  Image im;
  im.w = w, im.h = h;
  for (UINT k = 0; k < 5; ++k) ReadSub(t.g, k * t.slices + view, x0, y0, w, h, 2, im.g[k], 1);
  std::vector<uint16_t> mv;
  ReadSub(t.mv, view, x0, y0, w, h, 4, mv, 2);
  im.mvx.resize(static_cast<size_t>(w) * h), im.mvy.resize(static_cast<size_t>(w) * h);
  for (size_t i = 0; i < im.mvx.size(); ++i) im.mvx[i] = HalfToFloat(mv[2 * i]), im.mvy[i] = HalfToFloat(mv[2 * i + 1]);
  std::vector<float> ds;
  ReadSub(t.ds, view, x0, y0, w, h, 8, ds, 2);
  im.depth.resize(static_cast<size_t>(w) * h);
  for (size_t i = 0; i < im.depth.size(); ++i) im.depth[i] = ds[2 * i];
  return im;
}
struct Diff {
  size_t px = 0, gDiff = 0, depthDiff = 0, depthBig = 0, covDiff = 0, mvBad = 0;
  double mvMaxRel = 0, mvMaxAbs = 0, depthMaxRel = 0;
};
// mvScale: the fixup applied to b's motion vectors before the compare.
Diff Compare(const Image& a, const Image& b, float mvScale) {
  Diff d;
  d.px = static_cast<size_t>(a.w) * a.h;
  for (size_t i = 0; i < d.px; ++i) {
    bool g = false;
    for (int k = 0; k < 5; ++k) g |= a.g[k][i] != b.g[k][i];
    d.gDiff += g;
    const bool ca = a.depth[i] != 0, cb = b.depth[i] != 0;
    d.covDiff += ca != cb;
    if (a.depth[i] != b.depth[i]) {
      ++d.depthDiff;
      if (ca && cb && std::fabs(a.depth[i] - b.depth[i]) > 1e-5 * std::fabs(a.depth[i])) ++d.depthBig;
      if (ca && cb) d.depthMaxRel = std::max(d.depthMaxRel, static_cast<double>(std::fabs(a.depth[i] - b.depth[i]) / std::fabs(a.depth[i])));
    }
    if (ca && cb && a.depth[i] == b.depth[i]) {  // same surface: motion vectors comparable
      const float bx = b.mvx[i] * mvScale, by = b.mvy[i] * mvScale;  // mvScale <= 0: the x/y scales follow
      const double ex = std::fabs(bx - a.mvx[i]), ey = std::fabs(by - a.mvy[i]);
      const double ref = std::max(1e-3, static_cast<double>(std::max(std::fabs(a.mvx[i]), std::fabs(a.mvy[i]))));
      const double rel = std::max(ex, ey) / ref;
      d.mvMaxRel = std::max(d.mvMaxRel, rel);
      d.mvMaxAbs = std::max(d.mvMaxAbs, std::max(ex, ey));
      if (rel > 2e-3) ++d.mvBad;
    }
  }
  return d;
}

// ------------------------------------------------------------------- mesh --
struct Vtx {
  float pos[3], uv[2], nrm[3];
};
struct Inst {
  float pos[4], rot[4];
};
constexpr int kCards = 24;
constexpr int kDraws = 64;

}  // namespace

int main(int argc, char** argv) {
  const int rounds = argc > 1 ? std::max(5, atoi(argv[1])) : 40;
  const int nInst = argc > 2 ? std::max(1000, atoi(argv[2])) : 140000;

  // Device on the default adapter (hardware), no debug layer.
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1, got{};
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &g_dev, &got,
                          &g_ctx));
  {
    IDXGIDevice* xd = nullptr;
    IDXGIAdapter* ad = nullptr;
    DXGI_ADAPTER_DESC ade{};
    if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&xd))) &&
        SUCCEEDED(xd->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&ade)))
      printf("adapter: %ls\n", ade.Description);
    Release(ad), Release(xd);
  }
  D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3{};
  g_dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof(o3));
  printf("D3D11.3 VPAndRTArrayIndexFromAnyShaderFeedingRasterizer: %d\n",
         static_cast<int>(o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer));

  D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2{};
  g_dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof(o2));
  printf("D3D11.3 PSSpecifiedStencilRefSupported: %d\n", static_cast<int>(o2.PSSpecifiedStencilRefSupported));
  nvfgs::Api nv;
  const bool haveNv = nvfgs::Load(nv);
  printf("NVAPI: %s (init status %d)\n", haveNv ? "loaded" : "not available", nv.initStatus);
  if (haveNv) {
    nvfgs::QueryBits q{nvfgs::kQueryMultiViewVer, 0};
    int st = nv.queryMultiView ? nv.queryMultiView(g_dev, &q) : -999;
    printf("NvAPI_D3D_QueryMultiViewSupport: status %d, MultiView %u, SinglePassStereo %u, SPS XYZW %u\n", st,
           q.bits & 1, (q.bits >> 1) & 1, (q.bits >> 2) & 1);
    nvfgs::QueryBits s{nvfgs::kQuerySpsVer, 0};
    st = nv.querySps ? nv.querySps(g_dev, &s) : -999;
    printf("NvAPI_D3D_QuerySinglePassStereoSupport: status %d, SPS %u, XYZW %u\n", st, s.bits & 1, (s.bits >> 1) & 1);
    nvfgs::QueryBits w{nvfgs::kQueryModifiedWVer, 0};
    st = nv.queryModifiedW ? nv.queryModifiedW(g_dev, &w) : -999;
    printf("NvAPI_D3D_QueryModifiedWSupport: status %d, supported %u\n", st, w.bits);
  }

  // ---- shaders
  ID3DBlob* vsB = Compile("VS", "vs_5_0");
  ID3DBlob* vsVpB = o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer ? Compile("VSVp", "vs_5_0") : nullptr;
  ID3DBlob* vsMaskB = Compile("VSMask", "vs_5_0");
  ID3DBlob* gsFastB = Compile("GSFast", "gs_5_0", "3");
  ID3DBlob* gsFast1B = Compile("GSFast", "gs_5_0", "1");
  ID3DBlob* gsFastRtB = Compile("GSFastRt", "gs_5_0");
  ID3DBlob* gsDupB = Compile("GSDup", "gs_5_0");
  ID3DBlob* psB = Compile("PS", "ps_5_0");
  if (!vsB || !gsDupB || !psB) return 2;
  ID3D11VertexShader *vs = nullptr, *vsVp = nullptr, *vsMask = nullptr;
  ID3D11GeometryShader *gsFast = nullptr, *gsFast1 = nullptr, *gsFastRt = nullptr, *gsDup = nullptr;
  ID3D11PixelShader* ps = nullptr;
  CHECK(g_dev->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &vs));
  if (vsVpB) CHECK(g_dev->CreateVertexShader(vsVpB->GetBufferPointer(), vsVpB->GetBufferSize(), nullptr, &vsVp));
  CHECK(g_dev->CreateGeometryShader(gsDupB->GetBufferPointer(), gsDupB->GetBufferSize(), nullptr, &gsDup));
  CHECK(g_dev->CreatePixelShader(psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &ps));
  if (haveNv && nv.createFastGs) {
    nvfgs::FastGsExplicitDesc d{nvfgs::kFastGsExplicitVer, nvfgs::kFastGsUseViewportMask, nullptr};
    int st = nv.createFastGs(g_dev, gsFastB->GetBufferPointer(), gsFastB->GetBufferSize(), nullptr, &d, &gsFast);
    printf("NvAPI_D3D11_CreateFastGeometryShaderExplicit (mask 0b11): status %d\n", st);
    if (st) gsFast = nullptr;
    st = nv.createFastGs(g_dev, gsFast1B->GetBufferPointer(), gsFast1B->GetBufferSize(), nullptr, &d, &gsFast1);
    if (st) gsFast1 = nullptr;
    nvfgs::FastGsExplicitDesc d2{nvfgs::kFastGsExplicitVer,
                                 nvfgs::kFastGsUseViewportMask | nvfgs::kFastGsOffsetRtIndexByVpIndex, nullptr};
    st = nv.createFastGs(g_dev, gsFastRtB->GetBufferPointer(), gsFastRtB->GetBufferSize(), nullptr, &d2, &gsFastRt);
    printf("  with OFFSET_RT_INDEX_BY_VP_INDEX: status %d\n", st);
    if (st) gsFastRt = nullptr;
  }
  if (haveNv && nv.createVsEx && vsMaskB) {
    nvfgs::CustomSemantic cs{};
    cs.version = nvfgs::kCustomSemanticVer;
    cs.type = nvfgs::kSemViewportMask;
    strcpy_s(cs.name, "NV_VIEWPORT_MASK");
    nvfgs::CreateVsExV3 a{};
    a.version = nvfgs::kCreateVsExVer;
    a.numCustomSemantics = 1;
    a.pCustomSemantics = &cs;
    int st = nv.createVsEx(g_dev, vsMaskB->GetBufferPointer(), vsMaskB->GetBufferSize(), nullptr, &a, &vsMask);
    printf("NvAPI_D3D11_CreateVertexShaderEx (NV_VIEWPORT_MASK): status %d\n", st);
    if (st) vsMask = nullptr;
  }

  D3D11_INPUT_ELEMENT_DESC il[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"INST", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"INST", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
  };
  ID3D11InputLayout *layout = nullptr, *layout2 = nullptr;
  CHECK(g_dev->CreateInputLayout(il, 5, vsB->GetBufferPointer(), vsB->GetBufferSize(), &layout));
  il[3].InstanceDataStepRate = il[4].InstanceDataStepRate = 2;  // D: each instance drawn twice
  CHECK(g_dev->CreateInputLayout(il, 5, vsB->GetBufferPointer(), vsB->GetBufferSize(), &layout2));

  // ---- mesh: 24 leaf cards (4 vertices, 2 triangles each) around a trunk, unit height
  std::mt19937 rng(12345);
  std::uniform_real_distribution<float> U(0.0f, 1.0f);
  std::vector<Vtx> verts;
  std::vector<uint16_t> idx;
  for (int c = 0; c < kCards; ++c) {
    const float y = 0.15f + 0.8f * U(rng), a = 6.2831853f * U(rng), r = 0.12f + 0.25f * U(rng) * (1.0f - y);
    const float cx = r * std::cos(a), cz = r * std::sin(a), half = 0.12f + 0.1f * U(rng);
    const float dx = -std::sin(a) * half, dz = std::cos(a) * half;
    const uint16_t b = static_cast<uint16_t>(verts.size());
    const float n[3] = {std::cos(a), 0.3f, std::sin(a)};
    verts.push_back({{cx - dx, y - half, cz - dz}, {0, 1}, {n[0], n[1], n[2]}});
    verts.push_back({{cx + dx, y - half, cz + dz}, {1, 1}, {n[0], n[1], n[2]}});
    verts.push_back({{cx + dx, y + half, cz + dz}, {1, 0}, {n[0], n[1], n[2]}});
    verts.push_back({{cx - dx, y + half, cz - dz}, {0, 0}, {n[0], n[1], n[2]}});
    for (int k : {0, 1, 2, 0, 2, 3}) idx.push_back(static_cast<uint16_t>(b + k));
  }
  const UINT triPerInst = static_cast<UINT>(idx.size() / 3);

  // ---- instances: uniform over the ground wedge inside the periphery frustum, z in [10, 4000] m
  const M4 view = View(3.0f, 3.0f);
  const M4 projP = Proj(0.78074f, 0.79064f, 0.1958f, 0.0010288f);  // DCS periphery L rows x/y [V census]
  // Gaze on the horizon: NDC of a far point straight ahead at eye height.
  float cx, cy;
  {
    const M4 vp = Mul(view, projP);
    const float p[4] = {0, 3.0f, 100000.0f, 1};
    float c[4] = {};
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) c[j] += p[k] * vp.m[k][j];
    cx = c[0] / c[3], cy = c[1] / c[3];
  }
  const M4 projF = FocusOf(projP, cx, cy);
  const M4 vpP = Mul(view, projP), vpF = Mul(view, projF);
  auto inside = [](const M4& m, const float c[3], float r, bool sides) {
    // planes w+x, w-x, w+y, w-y (and near: w>0 handled by z>0 in placement)
    float pl[4][4];
    for (int k = 0; k < 4; ++k) {
      pl[0][k] = m.m[k][3] + m.m[k][0], pl[1][k] = m.m[k][3] - m.m[k][0];
      pl[2][k] = m.m[k][3] + m.m[k][1], pl[3][k] = m.m[k][3] - m.m[k][1];
    }
    (void)sides;
    for (auto& p : pl) {
      const float l = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
      if ((p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3]) / l < -r) return false;
    }
    return true;
  };
  std::vector<Inst> all, foc;
  all.reserve(nInst);
  while (static_cast<int>(all.size()) < nInst) {
    const float z = 10.0f + 3990.0f * std::sqrt(U(rng));  // uniform per ground area
    const float x = (2.0f * U(rng) - 1.0f) * 1.35f * z;
    const float s = 6.0f + 8.0f * U(rng), a = 6.2831853f * U(rng);
    Inst in{{x, 0.0f, z, s}, {std::sin(a), std::cos(a), 6.2831853f * U(rng), 0.6f + 0.4f * U(rng)}};
    const float c[3] = {x, 0.5f * s, z};
    if (!inside(vpP, c, 0.75f * s, true)) continue;
    all.push_back(in);
  }
  std::shuffle(all.begin(), all.end(), rng);  // buckets are not spatially sorted
  for (const Inst& in : all) {
    const float c[3] = {in.pos[0], 0.5f * in.pos[3], in.pos[2]};
    if (inside(vpF, c, 0.75f * in.pos[3], true)) foc.push_back(in);
  }
  printf("workload: %zu instances x %u triangles = %.2f M periphery triangles; focus list %zu (%.0f %%) = %.2f M\n",
         all.size(), triPerInst, all.size() * triPerInst / 1e6, foc.size(), 100.0 * foc.size() / all.size(),
         foc.size() * triPerInst / 1e6);
  printf("gaze (horizon) in periphery NDC: (%.4f, %.4f); focus row x = %.4f x_p %+.4f w\n", cx, cy, 1.0f / kSec,
         -cx / kSec);

  ID3D11Buffer *vb = nullptr, *ib = nullptr, *instAll = nullptr, *instFoc = nullptr;
  auto mkbuf = [](const void* p, UINT bytes, UINT bind, ID3D11Buffer** out) {
    D3D11_BUFFER_DESC d{bytes, D3D11_USAGE_IMMUTABLE, bind, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA s{p, 0, 0};
    CHECK(g_dev->CreateBuffer(&d, &s, out));
  };
  mkbuf(verts.data(), static_cast<UINT>(verts.size() * sizeof(Vtx)), D3D11_BIND_VERTEX_BUFFER, &vb);
  mkbuf(idx.data(), static_cast<UINT>(idx.size() * 2), D3D11_BIND_INDEX_BUFFER, &ib);
  mkbuf(all.data(), static_cast<UINT>(all.size() * sizeof(Inst)), D3D11_BIND_VERTEX_BUFFER, &instAll);
  mkbuf(foc.data(), static_cast<UINT>(foc.size() * sizeof(Inst)), D3D11_BIND_VERTEX_BUFFER, &instFoc);

  // ---- texture: 256x256 RGBA8 leaf mask with mips (alpha ~60 % coverage)
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = 256, d.MipLevels = 0, d.ArraySize = 1, d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1, d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    CHECK(g_dev->CreateTexture2D(&d, nullptr, &tex));
    std::vector<uint32_t> px(256 * 256);
    for (int y = 0; y < 256; ++y)
      for (int x = 0; x < 256; ++x) {
        const float u = (x - 128) / 128.0f, v = (y - 128) / 128.0f;
        const bool leaf = u * u + v * v < 0.9f && ((x / 16 + y / 16) % 5 != 0);
        const uint32_t g = 80 + (x * 7 + y * 3) % 120;
        px[y * 256 + x] = (leaf ? 0xff000000u : 0u) | (g << 8) | 0x20 | (0x30u << 16);
      }
    g_ctx->UpdateSubresource(tex, 0, nullptr, px.data(), 256 * 4, 0);
    CHECK(g_dev->CreateShaderResourceView(tex, nullptr, &srv));
    g_ctx->GenerateMips(srv);
  }
  ID3D11SamplerState* smp = nullptr;
  {
    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_ANISOTROPIC, s.MaxAnisotropy = 8;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    CHECK(g_dev->CreateSamplerState(&s, &smp));
  }
  ID3D11DepthStencilState* dss = nullptr;
  {
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = TRUE, d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL, d.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    CHECK(g_dev->CreateDepthStencilState(&d, &dss));
  }
  ID3D11RasterizerState *rs = nullptr, *rsSc = nullptr;
  {
    D3D11_RASTERIZER_DESC r{};
    r.FillMode = D3D11_FILL_SOLID, r.CullMode = D3D11_CULL_NONE, r.DepthClipEnable = TRUE;  // foliage: two-sided
    CHECK(g_dev->CreateRasterizerState(&r, &rs));
    r.ScissorEnable = TRUE;
    CHECK(g_dev->CreateRasterizerState(&r, &rsSc));
  }
  // Constant buffers: periphery (P_p, dims_p), focus (P_f, dims_f).
  struct CBData {
    float vp[4][4], prev[4][4], dims[4], misc[4];
  };
  auto mkcb = [&](const M4& m, float w, float h, float bias) {
    CBData c{};
    memcpy(c.vp, m.m, 64);
    memcpy(c.prev, m.m, 64);  // the VS offsets the previous position by 1 cm (a known, small motion)
    c.dims[0] = w, c.dims[1] = h, c.misc[0] = 1.25f, c.misc[1] = bias;
    D3D11_BUFFER_DESC d{sizeof(CBData), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA s{&c, 0, 0};
    ID3D11Buffer* b = nullptr;
    CHECK(g_dev->CreateBuffer(&d, &s, &b));
    return b;
  };
  ID3D11Buffer* cbP = mkcb(vpP, kPw, kPh, -1.7776f);
  ID3D11Buffer* cbF = mkcb(vpF, kFw, kFh, -1.7862f);

  Targets tP, tF, tAtlas, tArr;
  tP.Create(kPw, kPh, 1);
  tF.Create(kFw, kFh, 1);
  tAtlas.Create(kAw, kAh, 1);
  tArr.Create(kFw, kFh, 2);

  // ---- passes
  auto common = [&](ID3D11InputLayout* lay) {
    g_ctx->IASetInputLayout(lay);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
    g_ctx->PSSetShader(ps, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->PSSetSamplers(0, 1, &smp);
    g_ctx->OMSetDepthStencilState(dss, 0);
  };
  // kDraws instanced draws over a list; each draw rebinds the instance stream offset (DCS: per-bucket bindings).
  auto draws = [&](ID3D11Buffer* inst, size_t n, UINT instMul) {
    const size_t per = (n + kDraws - 1) / kDraws;
    for (size_t first = 0; first < n; first += per) {
      const UINT cnt = static_cast<UINT>(std::min(per, n - first));
      ID3D11Buffer* bufs[2] = {vb, inst};
      UINT strides[2] = {sizeof(Vtx), sizeof(Inst)};
      UINT offs[2] = {0, static_cast<UINT>(first * sizeof(Inst))};
      g_ctx->IASetVertexBuffers(0, 2, bufs, strides, offs);
      g_ctx->DrawIndexedInstanced(static_cast<UINT>(idx.size()), cnt * instMul, 0, 0, 0);
    }
  };
  auto vp1 = [&](float x, float y, float w, float h) {
    D3D11_VIEWPORT v{x, y, w, h, 0, 1};
    g_ctx->RSSetViewports(1, &v);
  };
  auto periphPass = [&](Targets& t, float ox, bool scissor) {
    common(layout);
    g_ctx->VSSetShader(vs, nullptr, 0);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &cbP);
    g_ctx->PSSetConstantBuffers(0, 1, &cbP);
    g_ctx->RSSetState(scissor ? rsSc : rs);
    if (scissor) {
      D3D11_RECT r{static_cast<LONG>(ox), 0, static_cast<LONG>(ox + kPw), static_cast<LONG>(kPh)};
      g_ctx->RSSetScissorRects(1, &r);
    }
    t.Bind();
    vp1(ox, 0, kPw, kPh);
    draws(instAll, all.size(), 1);
  };
  auto focusPass = [&](Targets& t, float ox, bool scissor, bool geoOnly) {
    common(layout);
    g_ctx->VSSetShader(vs, nullptr, 0);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &cbF);
    g_ctx->PSSetConstantBuffers(0, 1, &cbF);
    g_ctx->RSSetState(scissor ? rsSc : rs);
    if (scissor) {
      D3D11_RECT r{static_cast<LONG>(ox), 0, static_cast<LONG>(ox + kFw), static_cast<LONG>(kFh)};
      g_ctx->RSSetScissorRects(1, &r);
    }
    t.Bind();
    if (geoOnly)
      vp1(-64, -64, 16, 16);
    else
      vp1(ox, 0, kFw, kFh);
    draws(instFoc, foc.size(), 1);
  };
  // Broadcast passes: CB = periphery's (the focus mapping is all in viewport 1).
  enum Bc { kFast, kFast1, kFastRt, kVsMask, kInstVp, kGsDup };
  auto broadcast = [&](Bc kind) {
    const bool arr = kind == kFastRt;
    Targets& t = arr ? tArr : tAtlas;
    common(kind == kInstVp ? layout2 : layout);
    g_ctx->VSSetShader(kind == kVsMask ? vsMask : kind == kInstVp ? vsVp : vs, nullptr, 0);
    g_ctx->GSSetShader(kind == kFast ? gsFast : kind == kFast1 ? gsFast1 : kind == kFastRt ? gsFastRt
                       : kind == kGsDup ? gsDup : nullptr,
                       nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &cbP);
    g_ctx->PSSetConstantBuffers(0, 1, &cbP);
    D3D11_VIEWPORT v[2];
    const float pox = arr ? 0.0f : static_cast<float>(kFw);
    v[0] = D3D11_VIEWPORT{pox, 0, kPw, kPh, 0, 1};
    v[1] = FocusViewport(cx, cy, 0, 0);
    g_ctx->RSSetViewports(2, v);
    if (arr) {
      g_ctx->RSSetState(rs);  // slice bounds clip the focus; viewport 0 clips the periphery
    } else {
      g_ctx->RSSetState(rsSc);
      D3D11_RECT r[2] = {{static_cast<LONG>(pox), 0, static_cast<LONG>(pox + kPw), static_cast<LONG>(kPh)},
                         {0, 0, static_cast<LONG>(kFw), static_cast<LONG>(kFh)}};
      g_ctx->RSSetScissorRects(2, r);
    }
    t.Bind();
    draws(instAll, all.size(), kind == kInstVp ? 2 : 1);
  };

  struct Variant {
    const char* name;
    std::function<void()> run;
    bool ok;
    std::vector<double> ms;
    D3D11_QUERY_DATA_PIPELINE_STATISTICS st{};
  };
  std::vector<Variant> vs_;
  vs_.push_back({"A  two passes (periphery + focus list)", [&] { tP.Clear(), tF.Clear(), periphPass(tP, 0, false), focusPass(tF, 0, false, false); }, true});
  vs_.push_back({"Ap periphery pass alone", [&] { tP.Clear(), periphPass(tP, 0, false); }, true});
  vs_.push_back({"Af focus pass alone", [&] { tF.Clear(), focusPass(tF, 0, false, false); }, true});
  vs_.push_back({"Ag focus pass, 16x16 off-target viewport", [&] { tF.Clear(), focusPass(tF, 0, false, true); }, true});
  vs_.push_back({"Aa two passes into the atlas (scissor)", [&] { tAtlas.Clear(), periphPass(tAtlas, kFw, true), focusPass(tAtlas, 0, true, false); }, true});
  vs_.push_back({"B  FastGS mask 0b11, atlas", [&] { tAtlas.Clear(), broadcast(kFast); }, gsFast != nullptr});
  vs_.push_back({"B1 FastGS mask 0b01 (periphery only)", [&] { tAtlas.Clear(), broadcast(kFast1); }, gsFast1 != nullptr});
  vs_.push_back({"B2 FastGS mask 0b11 + RT offset, arrays", [&] { tArr.Clear(), broadcast(kFastRt); }, gsFastRt != nullptr});
  vs_.push_back({"C  VS NV_VIEWPORT_MASK, atlas", [&] { tAtlas.Clear(), broadcast(kVsMask); }, vsMask != nullptr});
  vs_.push_back({"D  instanced x2, VS SV_ViewportArrayIndex", [&] { tAtlas.Clear(), broadcast(kInstVp); }, vsVp != nullptr});
  vs_.push_back({"E  regular GS, 2 triangles out", [&] { tAtlas.Clear(), broadcast(kGsDup); }, true});
  // Clear-only baselines subtracted from each variant's time (the clears are not part of the comparison).
  std::vector<double> clrP, clrF, clrA, clrArr, clrPF;

  ID3D11Query *disj = nullptr, *t0 = nullptr, *t1 = nullptr, *stats = nullptr, *ev = nullptr;
  D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
  CHECK(g_dev->CreateQuery(&qd, &disj));
  qd.Query = D3D11_QUERY_TIMESTAMP;
  CHECK(g_dev->CreateQuery(&qd, &t0));
  CHECK(g_dev->CreateQuery(&qd, &t1));
  qd.Query = D3D11_QUERY_PIPELINE_STATISTICS;
  CHECK(g_dev->CreateQuery(&qd, &stats));
  qd.Query = D3D11_QUERY_EVENT;
  CHECK(g_dev->CreateQuery(&qd, &ev));
  auto drain = [&] {
    g_ctx->End(ev);
    BOOL done = FALSE;
    while (g_ctx->GetData(ev, &done, sizeof(done), 0) != S_OK || !done) SwitchToThread();
  };
  auto timed = [&](const std::function<void()>& f, D3D11_QUERY_DATA_PIPELINE_STATISTICS* st) {
    drain();
    g_ctx->Begin(disj);
    if (st) g_ctx->Begin(stats);
    g_ctx->End(t0);
    f();
    g_ctx->End(t1);
    if (st) g_ctx->End(stats);
    g_ctx->End(disj);
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    while (g_ctx->GetData(disj, &dj, sizeof(dj), 0) != S_OK) SwitchToThread();
    UINT64 a = 0, b = 0;
    while (g_ctx->GetData(t0, &a, sizeof(a), 0) != S_OK) SwitchToThread();
    while (g_ctx->GetData(t1, &b, sizeof(b), 0) != S_OK) SwitchToThread();
    if (st)
      while (g_ctx->GetData(stats, st, sizeof(*st), 0) != S_OK) SwitchToThread();
    if (dj.Disjoint || !dj.Frequency) return -1.0;
    return (b - a) * 1000.0 / dj.Frequency;
  };

  // Warm-up (clocks up, shaders compiled by the driver).
  for (int i = 0; i < 6; ++i)
    for (auto& v : vs_)
      if (v.ok) timed(v.run, nullptr);
  for (auto& v : vs_)
    if (v.ok) timed(v.run, &v.st);
  for (int r = 0; r < rounds; ++r) {
    for (auto& v : vs_) {
      if (!v.ok) continue;
      const double ms = timed(v.run, nullptr);
      if (ms >= 0) v.ms.push_back(ms);
    }
    double c;
    if ((c = timed([&] { tP.Clear(); }, nullptr)) >= 0) clrP.push_back(c);
    if ((c = timed([&] { tF.Clear(); }, nullptr)) >= 0) clrF.push_back(c);
    if ((c = timed([&] { tP.Clear(), tF.Clear(); }, nullptr)) >= 0) clrPF.push_back(c);
    if ((c = timed([&] { tAtlas.Clear(); }, nullptr)) >= 0) clrA.push_back(c);
    if ((c = timed([&] { tArr.Clear(); }, nullptr)) >= 0) clrArr.push_back(c);
  }
  const double cP = Median(clrP), cF = Median(clrF), cPF = Median(clrPF), cA = Median(clrA), cArr = Median(clrArr);
  printf("\nclears (subtracted): periphery %.3f, focus %.3f, both %.3f, atlas %.3f, arrays %.3f ms\n", cP, cF, cPF, cA,
         cArr);
  printf("\n%-44s %8s %8s %7s %7s %7s %7s %7s\n", "variant (GPU ms, median)", "ms", "p10-p90", "IA M", "VS M", "GS M",
         "Cprim M", "PS M");
  std::vector<double> med(vs_.size(), 0);
  for (size_t i = 0; i < vs_.size(); ++i) {
    auto& v = vs_[i];
    if (!v.ok || v.ms.empty()) {
      printf("%-44s   (not available)\n", v.name);
      continue;
    }
    const double clr = i == 0 ? cPF : i == 1 ? cP : (i == 2 || i == 3) ? cF : i == 7 ? cArr : cA;
    std::vector<double> s = v.ms;
    std::sort(s.begin(), s.end());
    med[i] = s[s.size() / 2] - clr;
    printf("%-44s %8.3f %4.2f-%4.2f %7.2f %7.2f %7.2f %7.2f %7.2f\n", v.name, med[i], s[s.size() / 10] - clr,
           s[s.size() * 9 / 10] - clr, v.st.IAPrimitives / 1e6, v.st.VSInvocations / 1e6, v.st.GSInvocations / 1e6,
           v.st.CPrimitives / 1e6, v.st.PSInvocations / 1e6);
  }
  const double A = med[0], Ap = med[1], Af = med[2], Ag = med[3];
  printf("\nfocus pass: %.3f ms, of which geometry-only (off-target viewport) %.3f ms = %.0f %% (the live probe's "
         "upper bound of the shareable part)\n",
         Af, Ag, Af > 0 ? 100.0 * Ag / Af : 0.0);
  const double ideal = Ap + (Af - Ag);  // periphery + the focus's raster/PS remainder
  for (size_t i = 5; i < vs_.size(); ++i) {
    if (!med[i] || i == 6) continue;
    printf("%-44s saves %+.3f ms vs A (%.0f %% of the focus pass); overhead over ideal broadcast (%.3f) %+.0f %%\n",
           vs_[i].name, A - med[i], Af > 0 ? 100.0 * (A - med[i]) / Af : 0.0, ideal,
           ideal > 0 ? 100.0 * (med[i] - ideal) / ideal : 0.0);
  }
  if (med[6]) printf("FastGS cost alone (B1 - Ap): %+.3f ms\n", med[6] - Ap);

  // ---- exactness: focus and periphery outputs against A's
  printf("\nexactness against A (two passes, separate targets; focus = DCS's own focus projection):\n");
  // A' : A with every instance list in another order (DCS's GPU-driven buckets come out in a different order
  // every frame: atomics in the cull kernels, R34/R38 "ORDER"). Its difference to A is stock's own noise floor.
  ID3D11Buffer *instAllR = nullptr, *instFocR = nullptr;
  {
    std::vector<Inst> a2 = all, f2;
    std::shuffle(a2.begin(), a2.end(), rng);
    for (const Inst& in : a2) {
      const float c[3] = {in.pos[0], 0.5f * in.pos[3], in.pos[2]};
      if (inside(vpF, c, 0.75f * in.pos[3], true)) f2.push_back(in);
    }
    mkbuf(a2.data(), static_cast<UINT>(a2.size() * sizeof(Inst)), D3D11_BIND_VERTEX_BUFFER, &instAllR);
    mkbuf(f2.data(), static_cast<UINT>(f2.size() * sizeof(Inst)), D3D11_BIND_VERTEX_BUFFER, &instFocR);
  }
  Image refF, refP;
  auto makeRef = [&] {
    tP.Clear(), tF.Clear(), periphPass(tP, 0, false), focusPass(tF, 0, false, false);
    refF = Read(tF, 0, 0, 0, kFw, kFh), refP = Read(tP, 0, 0, 0, kPw, kPh);
  };
  makeRef();
  // MV fixup: the broadcast PS used the periphery's dims and NDC: MV_f = MV_p * dims_f / dims_p / s, per axis.
  const float sx = static_cast<float>(kFw) / kPw / kSec, sy = static_cast<float>(kFh) / kPh / kSec;
  auto report = [&](const char* name, Targets& t, UINT fx, UINT px, UINT fview, UINT pview, bool fixup) {
    Image f = Read(t, fview, fx, 0, kFw, kFh), p = Read(t, pview, px, 0, kPw, kPh);
    if (fixup)
      for (size_t i = 0; i < f.mvx.size(); ++i) f.mvx[i] *= sx, f.mvy[i] *= sy;
    const Diff df = Compare(refF, f, 1.0f), dp = Compare(refP, p, 1.0f);
    size_t cov = 0;
    for (float z : refF.depth) cov += z != 0;
    printf("  %-32s focus (%zu px covered): G-buffer px differ %6zu (%.3f %%), depth differ %6zu (>1e-5 rel %5zu, "
           "coverage %3zu), MV%s max abs %.4f px, max rel %.1e (>0.2 %%: %zu) | periphery: G %zu, depth %zu\n",
           name, cov, df.gDiff, 100.0 * df.gDiff / df.px, df.depthDiff, df.depthBig, df.covDiff,
           fixup ? " fixed up" : "", df.mvMaxAbs, df.mvMaxRel, df.mvBad, dp.gDiff, dp.depthDiff);
  };
  auto reportAll = [&] {
    tP.Clear(), tF.Clear(), periphPass(tP, 0, false), focusPass(tF, 0, false, false);
    {
      Image f = Read(tF, 0, 0, 0, kFw, kFh);
      const Diff df = Compare(refF, f, 1.0f);
      printf("  %-32s focus: G-buffer px differ %zu, depth differ %zu\n", "A again (determinism)", df.gDiff,
             df.depthDiff);
    }
    std::swap(instAll, instAllR), std::swap(instFoc, instFocR);
    tP.Clear(), tF.Clear(), periphPass(tP, 0, false), focusPass(tF, 0, false, false);
    {
      Image f = Read(tF, 0, 0, 0, kFw, kFh), p = Read(tP, 0, 0, 0, kPw, kPh);
      const Diff df = Compare(refF, f, 1.0f), dp = Compare(refP, p, 1.0f);
      printf("  %-32s focus: G-buffer px differ %6zu (%.3f %%), depth differ %6zu | periphery: G %zu (%.3f %%), "
             "depth %zu  <- stock's own order noise\n",
             "A' (instance order reshuffled)", df.gDiff, 100.0 * df.gDiff / df.px, df.depthDiff, dp.gDiff,
             100.0 * dp.gDiff / dp.px, dp.depthDiff);
    }
    std::swap(instAll, instAllR), std::swap(instFoc, instFocR);
    tAtlas.Clear(), periphPass(tAtlas, kFw, true), focusPass(tAtlas, 0, true, false);
    report("Aa two passes, atlas", tAtlas, 0, kFw, 0, 0, false);
    if (gsFast) tAtlas.Clear(), broadcast(kFast), report("B FastGS, atlas", tAtlas, 0, kFw, 0, 0, true);
    if (gsFastRt) tArr.Clear(), broadcast(kFastRt), report("B2 FastGS, arrays + RT offset", tArr, 0, 0, 1, 0, true);
    if (vsMask) tAtlas.Clear(), broadcast(kVsMask), report("C VS viewport mask, atlas", tAtlas, 0, kFw, 0, 0, true);
    if (vsVp) tAtlas.Clear(), broadcast(kInstVp), report("D instanced VPAI, atlas", tAtlas, 0, kFw, 0, 0, true);
    tAtlas.Clear(), broadcast(kGsDup), report("E regular GS, atlas", tAtlas, 0, kFw, 0, 0, true);
  };
  reportAll();
  // Diagnostic: the periphery's mip bias set to the focus's (DCS: -1.7776 vs -1.7862) in A and in the broadcast,
  // so only the geometry path differs: what remains is the broadcast's own rounding.
  printf("\nsame with the periphery's mip bias equal to the focus's (isolates the 0.0086-level bias step):\n");
  Release(cbP);
  cbP = mkcb(vpP, kPw, kPh, -1.7862f);
  makeRef();
  reportAll();
  Release(instAllR), Release(instFocR);
  printf("done\n");
  return 0;
}
