// R33 far shadow cascade cache (cascade_cache.h): the projection math on the
// matrices DCS wrote in the 2026-10-11 census (F-4E Germany, CascadeShadowPass3),
// the refresh/reuse decision and its invalidation triggers, the caster lists,
// and a WARP replica of DCS's cascade setup and pass: ShadowMapModule's parse
// through our hook, the pass execute through shadow_pass.h's innermost hook,
// static casters cached, dynamic casters drawn every frame. Each cached slice
// is compared texel for texel with a stock render of the same projection.
#pragma once

namespace cctest {
using namespace cascache;

float Fb(uint32_t u) {
  float f;
  memcpy(&f, &u, 4);
  return f;
}

// Census frame 0, CascadeShadowPass3 [M gdd_cb.csv 6e0575 / 891986]: light
// view V, ortho P (pass b7, row-major), the per-view ShadowMatrix[3].
const uint32_t kV[16] = {0x3f752f79, 0x3d0b291a, 0xbe923627, 0x00000000, 0xbc2ab057, 0xbf7cfdb7,
                         0xbe1c2ca3, 0x00000000, 0xbe932561, 0x3e189f93, 0xbf723688, 0x00000000,
                         0x00000000, 0x00000000, 0x00000000, 0x3f800000};
const uint32_t kP[16] = {0x3a7f830f, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xb9ed4d72,
                         0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xb94d74a7, 0x00000000,
                         0xbf977c4e, 0x3e625449, 0x3efa7afb, 0x3f800000};
const uint32_t kSM[16] = {0x39f4b7cf, 0x3700ff26, 0x386aaffa, 0x00000000, 0xb6aa5d09, 0xb96a836b,
                          0x37faadd1, 0x00000000, 0xb912dd90, 0x380d79e7, 0x3942640a, 0x00000000,
                          0xbdbbe270, 0x3ec76aee, 0x3efa7afb, 0x3f800000};

void Load(const uint32_t* u, float* f) {
  for (int i = 0; i < 16; ++i) f[i] = Fb(u[i]);
}

bool Near(const float* a, const float* b, double rel, double abs) {
  for (int i = 0; i < 16; ++i)
    if (std::fabs(double(a[i]) - b[i]) > rel * std::fabs(double(b[i])) + abs) return false;
  return true;
}

void MathTests() {
  float V[16], P[16], SM[16], out[16];
  Load(kV, V);
  Load(kP, P);
  Load(kSM, SM);
  ShadowMatrixOf(V, P, out);
  Check(Near(out, SM, 2e-6, 1e-10), "cascade cache: V*P*B equals DCS's ShadowMatrix[3] of the census (float precision)");
  Box b;
  const bool ok = BoxFromOrtho(P, &b);
  Check(ok && std::fabs(Extent(b, 0) - 2051.9) < 1.0 && std::fabs(Extent(b, 1) - 4418.7) < 1.0 &&
            std::fabs(Extent(b, 2) - 5103.7) < 1.0,
        "cascade cache: census c3 box 2052 x 4419 m, 5104 m deep (0.50 x 1.08 m texels at 4096)");
  float P2[16];
  OrthoFromBox(b, P2);
  Check(Near(P2, P, 1e-6, 1e-12), "cascade cache: box -> ortho reproduces DCS's projection");
  const Box e = Expand(b, 0.03, 0.05);
  Check(std::fabs(Extent(e, 0) / Extent(b, 0) - 1.06) < 1e-9 && std::fabs(Extent(e, 2) / Extent(b, 2) - 1.10) < 1e-9 &&
            Inside(e, b) && !Inside(b, e),
        "cascade cache: window margin per side, inside test");
  float Q[16];
  memcpy(Q, P, sizeof(Q));
  Q[1] = 1e-3f;
  Box bad;
  Check(!BoxFromOrtho(Q, &bad), "cascade cache: a projection that is not DCS's ortho form is refused");
}

void DecideTests() {
  float V[16], P[16];
  Load(kV, V);
  Load(kP, P);
  Box F;
  BoxFromOrtho(P, &F);
  Params p;  // margin 3 %, max age 4
  Tag t;
  t.valid = true;
  t.gen = 7;
  t.frame = 10;
  t.size = 4096;
  memcpy(t.V, V, sizeof(V));
  t.win = Expand(F, p.margin, p.zMargin);
  OrthoFromBox(t.win, t.P);
  Box tight = F;
  tight.hi[0] -= 1;  // DCS's unsnapped box lies inside its snapped one
  In in{V, F, true, tight, 4096, 11, false, false};
  Reason why;
  bool good = Decide(t, in, p, &why) == kReuse && why == kRReuse;
  in.frame = 13;
  good = good && Decide(t, in, p, &why) == kReuse;
  in.frame = 14;
  good = good && Decide(t, in, p, &why) == kRefresh && why == kRAge;
  in.otherRefreshed = true;
  good = good && Decide(t, in, p, &why) == kReuse;  // one frame of grace
  in.frame = 15;
  good = good && Decide(t, in, p, &why) == kRefresh && why == kRAge;
  Check(good, "cascade cache: reuse for frames 1-3 after the capture, age refresh at 4 (one frame later when another "
              "cascade refreshed)");
  in.frame = 11;
  in.otherRefreshed = false;
  float V2[16];
  memcpy(V2, V, sizeof(V));
  uint32_t u;
  memcpy(&u, &V2[0], 4);
  ++u;
  memcpy(&V2[0], &u, 4);
  In l = in;
  l.V = V2;
  good = Decide(t, l, p, &why) == kRefresh && why == kRLight;
  In s = in;
  s.size = 2048;
  good = good && Decide(t, s, p, &why) == kRefresh && why == kRSize;
  In f = in;
  f.forced = true;
  good = good && Decide(t, f, p, &why) == kRefresh && why == kRForced;
  In n = in;
  n.tightValid = false;
  good = good && Decide(t, n, p, &why) == kRefresh && why == kRNoTight;
  Tag none = t;
  none.valid = false;
  good = good && Decide(none, in, p, &why) == kRefresh && why == kRNoCache;
  Check(good, "cascade cache: refresh when the light view, the map size, the scene (forced), DCS's box or the capture "
              "is missing");
  // Motion: 2 % of the extent stays inside the 3 % margin, 4 % leaves it.
  const double ex = Extent(F, 0);
  In m = in;
  m.tight.lo[0] += 0.02 * ex;
  m.tight.hi[0] += 0.02 * ex;
  good = Decide(t, m, p, &why) == kReuse;
  m.tight.lo[0] += 0.02 * ex;
  m.tight.hi[0] += 0.02 * ex;
  good = good && Decide(t, m, p, &why) == kRefresh && why == kROutside;
  In mz = in;
  mz.tight.hi[2] += 0.2 * Extent(F, 2);
  good = good && Decide(t, mz, p, &why) == kRefresh && why == kROutside;
  // DCS's box shrinks to half (the view turned): the held texel is twice as coarse.
  In sc = in;
  Box half = F;
  half.hi[0] = half.lo[0] + 0.5 * ex;
  half.hi[1] = half.lo[1] + 0.5 * Extent(F, 1);
  sc.fresh = half;
  sc.tight = half;
  good = good && Decide(t, sc, p, &why) == kRefresh && why == kRScale;
  Check(good, "cascade cache: motion inside the margin reuses, beyond it (xy or z) refreshes; a much finer fresh box "
              "refreshes");
}

// ---------------------------------------------------------------------------
// Fake items for the list tests and the WARP replica
// ---------------------------------------------------------------------------
struct FakeItem {
  void** vt;
  int geo;  // geometry index
};
void* g_staticVt[8];   // the "terrain module" range
void* g_dynVt[8];
void* g_otherVt[8];    // a class from another module (e.g. the recorder's exec entry)
std::vector<int> g_trace;  // item geometries drawn, in order

void ListTests() {
  const uintptr_t lo0 = g_staticLo, hi0 = g_staticHi;
  g_staticLo = reinterpret_cast<uintptr_t>(&g_staticVt[0]);
  g_staticHi = reinterpret_cast<uintptr_t>(&g_staticVt[8]);
  InitSteps();
  FakeItem s1{g_staticVt, 1}, d1{g_dynVt, 2}, s2{g_staticVt, 3}, o1{g_otherVt, 4}, d2{g_dynVt, 5};
  void* items[5] = {&s1, &d1, &s2, &o1, &d2};
  std::vector<void*> out, st;
  const size_t ns = BuildList(items, items + 5, kRefresh, out, st);
  bool good = ns == 2 && out.size() == 6 && out[0] == &s1 && out[1] == &s2 && out[2] == &g_snapItem && out[3] == &d1 &&
              out[4] == &o1 && out[5] == &d2;
  BuildList(items, items + 5, kReuse, out, st);
  good = good && out.size() == 4 && out[0] == &g_restoreItem && out[1] == &d1 && out[2] == &o1 && out[3] == &d2 &&
         st.size() == 2 && st[0] == &s1 && st[1] == &s2;
  Check(good, "cascade cache: refresh list [static, snap, dynamic], reuse list [restore, dynamic]; unknown classes are "
              "dynamic");
  // R49: a class outside the terrain module whose RTTI name is TerrainParser::TerrainObject (2.9.30's terrain caster
  // in Visualizer.dll) is static; another RTTI name, a vtable without a locator, and a bad locator are dynamic.
  {
    alignas(16) static uint8_t img[0x800];
    memset(img, 0, sizeof(img));
    auto mkClass = [&](uint32_t colOff, uint32_t tdOff, uint32_t vtOff, const char* name, uint32_t sig) {
      uint8_t* col = img + colOff;
      memcpy(col + 0, &sig, 4);
      memcpy(col + 12, &tdOff, 4);
      memcpy(col + 20, &colOff, 4);  // pSelf: the locator's own RVA
      memcpy(img + tdOff + 16, name, strlen(name) + 1);
      void* p = col;
      memcpy(img + vtOff - 8, &p, 8);
      return reinterpret_cast<void**>(img + vtOff);
    };
    void** terr = mkClass(0x100, 0x200, 0x300, ".?AVTerrainObject@TerrainParser@@", 1);
    void** other = mkClass(0x400, 0x480, 0x540, ".?AVSomethingElse@TerrainParser@@", 1);
    void** badSig = mkClass(0x600, 0x680, 0x740, ".?AVTerrainObject@TerrainParser@@", 0);
    g_rttiMemoN = 0;
    FakeItem t1{terr, 6}, x1{other, 7}, b1{badSig, 8}, d3{g_dynVt, 9};
    const bool cls = Classify(&t1) == kStatic && Classify(&x1) == kDynamic && Classify(&b1) == kDynamic &&
                     Classify(&d3) == kDynamic && Classify(&t1) == kStatic;  // (the second from the memo)
    void* items2[4] = {&d3, &t1, &x1, &b1};
    std::vector<void*> out2, st2;
    const size_t ns2 = BuildList(items2, items2 + 4, kReuse, out2, st2);
    g_rttiMemoN = 0;
    Check(cls && ns2 == 1 && st2.size() == 1 && st2[0] == &t1 && out2.size() == 4 && out2[0] == &g_restoreItem,
          "cascade cache R49: TerrainParser::TerrainObject (by RTTI, outside the terrain module) is static; other RTTI "
          "names and a bad locator are dynamic");
  }
  g_staticLo = lo0;
  g_staticHi = hi0;
}

// ---------------------------------------------------------------------------
// WARP replica
// ---------------------------------------------------------------------------
constexpr UINT kRes = 256;
constexpr int kCasc = 4;
constexpr uint32_t kCtxView = 0x100, kCtxProj = 0x1c4;  // where the fake parse stores view and proj in the context

struct Rig {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11Texture2D* arr = nullptr;  // the cascade array (cache path)
  ID3D11Texture2D* ref = nullptr;  // stock renders
  ID3D11DepthStencilView* dsv[kCasc] = {};
  ID3D11DepthStencilView* refDsv[kCasc] = {};
  ID3D11VertexShader* vs = nullptr;
  ID3D11InputLayout* il = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11Buffer* vb = nullptr;
  ID3D11DepthStencilState* ds = nullptr;
  ID3D11RasterizerState* rs = nullptr;
  void Release() {
    auto rel = [](auto*& p) {
      if (p) p->Release();
      p = nullptr;
    };
    for (auto*& d : dsv) rel(d);
    for (auto*& d : refDsv) rel(d);
    rel(arr), rel(ref), rel(vs), rel(il), rel(cb), rel(vb), rel(ds), rel(rs), rel(ctx), rel(dev);
  }
};
Rig g_rig;

// Geometry: 6 static triangles, 2 dynamic ones, 1 "wind-moved" copy of static 0.
constexpr int kGeoStatic = 6, kGeoDyn = 2, kGeo = kGeoStatic + kGeoDyn + 1;
float g_geo[kGeo][9];
float g_dynOffset = 0;

void BuildGeo(float cx) {
  for (int g = 0; g < kGeo; ++g) {
    const float x = (g % 3 - 1) * 4.0f, y = (g / 3 - 1) * 3.0f, z = -10.0f + 5.0f * g;
    const float tri[9] = {x - 3, y - 2, z, x + 3, y - 1, z + 4, x, y + 3, z - 4};
    memcpy(g_geo[g], tri, sizeof(tri));
  }
  for (int g = kGeoStatic; g < kGeoStatic + kGeoDyn; ++g)
    for (int v = 0; v < 3; ++v) g_geo[g][v * 3] += cx + g_dynOffset;
  for (int v = 0; v < 3; ++v) g_geo[kGeo - 1][v * 3] = g_geo[0][v * 3] + 0.37f;  // static 0 swayed
  for (int v = 0; v < 3; ++v) g_geo[kGeo - 1][v * 3 + 1] = g_geo[0][v * 3 + 1], g_geo[kGeo - 1][v * 3 + 2] = g_geo[0][v * 3 + 2] + 6.0f;
}

void UploadGeo() { g_rig.ctx->UpdateSubresource(g_rig.vb, 0, nullptr, g_geo, 0, 0); }

void SetVP(const float* view, const float* proj) {
  float vp[16];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += double(view[r * 4 + k]) * proj[k * 4 + c];
      vp[r * 4 + c] = float(s);
    }
  g_rig.ctx->UpdateSubresource(g_rig.cb, 0, nullptr, vp, 0, 0);
}

void DrawGeo(int g) {
  ID3D11DeviceContext* c = g_rig.ctx;
  const UINT stride = 12, off = 0;
  c->IASetInputLayout(g_rig.il);
  c->IASetVertexBuffers(0, 1, &g_rig.vb, &stride, &off);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c->VSSetShader(g_rig.vs, nullptr, 0);
  c->VSSetConstantBuffers(7, 1, &g_rig.cb);
  c->PSSetShader(nullptr, nullptr, 0);
  c->OMSetDepthStencilState(g_rig.ds, 0);
  c->RSSetState(g_rig.rs);
  c->Draw(3, static_cast<UINT>(g) * 3);
  g_trace.push_back(g);
}

void __fastcall DrawItem(void* self, void*) { DrawGeo(static_cast<FakeItem*>(self)->geo); }

// Fake DCS objects: the module (SceneRender subobject), the Shadows impl, the
// passes (data index, culling node, context with view/proj) and the render
// context whose +0x438 holds the caster vectors.
alignas(16) uint8_t g_module[0x2000];
alignas(16) uint8_t g_impl[0x600];
void* g_moduleVt[4];
alignas(16) uint8_t g_pass[kCasc][0x700];
alignas(16) uint8_t g_node[kCasc][0x40];
alignas(16) uint8_t g_ctx0[0x500];
void* g_ctxObj = g_ctx0;  // the pass's ctx: [ctx] = ctx0
void* g_vecs[kCasc * 3];  // {begin, end, cap} per data index
std::vector<void*> g_items[kCasc];
FakeItem g_itemPool[kCasc][kGeo];
int g_parseCalls = 0;
float g_parseGot[kCasc][16];

// Stand-in for ShadowMapModule::parseShadowMap: stores view and proj in the
// pass context of data index count-1-idx (DCS copies them verbatim).
void __fastcall FakeParse(void* self, int idx, const float* view, const float* proj) {
  ++g_parseCalls;
  const int count = *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(self) + kModCount);
  const int d = count - 1 - idx;
  uint8_t* c = g_pass[d] + kPassData + kDataCtx;
  memcpy(c + kCtxView, view, 64);
  memcpy(c + kCtxProj, proj, 64);
  memcpy(g_parseGot[idx], proj, 64);
}

// Stand-in for the cascade execute (GC 0xa5500): framebuffer, clear, b7 from
// the context, then item->vt[1](item, data + 0x10) over the vector.
void __fastcall FakeExec(void* pass, void* ctx) {
  auto* ps = static_cast<uint8_t*>(pass);
  const int d = *reinterpret_cast<int32_t*>(ps + kPassData);
  uint8_t* c = ps + kPassData + kDataCtx;
  D3D11_VIEWPORT vp{0, 0, float(kRes), float(kRes), 0, 1};
  g_rig.ctx->OMSetRenderTargets(0, nullptr, g_rig.dsv[d]);
  g_rig.ctx->RSSetViewports(1, &vp);
  g_rig.ctx->ClearDepthStencilView(g_rig.dsv[d], D3D11_CLEAR_DEPTH, 0.0f, 0);
  SetVP(reinterpret_cast<float*>(c + kCtxView), reinterpret_cast<float*>(c + kCtxProj));
  void** vec = shadowpass::CasterVector(pass, ctx);
  if (vec) {
    auto** b = static_cast<void**>(vec[0]);
    auto** e = static_cast<void**>(vec[1]);
    using ItemFn = void(__fastcall*)(void*, void*);
    for (void** p = b; p < e; ++p) (*reinterpret_cast<ItemFn**>(*p))[1](*p, c);
  }
  g_rig.ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

// The stock render of cascade d with projection P: every static and dynamic caster.
void RenderRef(int d, const float* view, const float* P, bool sway) {
  D3D11_VIEWPORT vp{0, 0, float(kRes), float(kRes), 0, 1};
  g_rig.ctx->OMSetRenderTargets(0, nullptr, g_rig.refDsv[d]);
  g_rig.ctx->RSSetViewports(1, &vp);
  g_rig.ctx->ClearDepthStencilView(g_rig.refDsv[d], D3D11_CLEAR_DEPTH, 0.0f, 0);
  SetVP(view, P);
  for (int g = 0; g < kGeoStatic; ++g) DrawGeo(g == 0 && sway ? kGeo - 1 : g);
  for (int g = kGeoStatic; g < kGeoStatic + kGeoDyn; ++g) DrawGeo(g);
  g_rig.ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

// Texels of slice d that differ between the cache path and the stock render.
int64_t DiffSlice(int d) {
  ID3D11Texture2D* a = ToStaging(g_rig.arr, D3D11CalcSubresource(0, d, 1));
  ID3D11Texture2D* b = ToStaging(g_rig.ref, D3D11CalcSubresource(0, d, 1));
  uint64_t texels = 0;
  float md = 0;
  const int64_t bad = a && b ? CompareDepth(g_rig.ctx, a, b, &texels, &md) : -1;
  if (a) a->Release();
  if (b) b->Release();
  return bad;
}

int64_t NonZero(int d) {
  ID3D11Texture2D* a = ToStaging(g_rig.arr, D3D11CalcSubresource(0, d, 1));
  if (!a) return -1;
  D3D11_MAPPED_SUBRESOURCE m;
  int64_t n = 0;
  if (SUCCEEDED(g_rig.ctx->Map(a, 0, D3D11_MAP_READ, 0, &m))) {
    for (UINT y = 0; y < kRes; ++y) {
      const uint16_t* r = reinterpret_cast<const uint16_t*>(static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch);
      for (UINT x = 0; x < kRes; ++x) n += r[x] != 0;
    }
    g_rig.ctx->Unmap(a, 0);
  }
  a->Release();
  return n;
}

bool MakeRig() {
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &g_rig.dev, nullptr,
                               &g_rig.ctx)))
    return false;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = td.Height = kRes;
  td.MipLevels = 1;
  td.ArraySize = kCasc;
  td.Format = DXGI_FORMAT_R16_TYPELESS;  // DCS's cascade array [M gdd_res.csv: 4096 x 4096 x 4, format 53]
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(g_rig.dev->CreateTexture2D(&td, nullptr, &g_rig.arr)) ||
      FAILED(g_rig.dev->CreateTexture2D(&td, nullptr, &g_rig.ref)))
    return false;
  for (int d = 0; d < kCasc; ++d) {
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
    vd.Format = DXGI_FORMAT_D16_UNORM;
    vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
    vd.Texture2DArray.FirstArraySlice = d;
    vd.Texture2DArray.ArraySize = 1;
    if (FAILED(g_rig.dev->CreateDepthStencilView(g_rig.arr, &vd, &g_rig.dsv[d])) ||
        FAILED(g_rig.dev->CreateDepthStencilView(g_rig.ref, &vd, &g_rig.refDsv[d])))
      return false;
  }
  HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = dll ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dll, "D3DCompile")) : nullptr;
  if (!compile) return false;
  static const char kSrc[] =
      "cbuffer cPerView : register(b7) { row_major float4x4 gViewProj; };\n"
      "float4 main(float3 p : POSITION) : SV_Position { return mul(float4(p, 1), gViewProj); }\n";
  ID3DBlob* blob = nullptr;
  ID3DBlob* err = nullptr;
  compile(kSrc, sizeof(kSrc) - 1, "cc", nullptr, nullptr, "main", "vs_5_0", 0, 0, &blob, &err);
  if (err) err->Release();
  if (!blob) return false;
  const D3D11_INPUT_ELEMENT_DESC ie{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
  bool ok = SUCCEEDED(g_rig.dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_rig.vs)) &&
            SUCCEEDED(g_rig.dev->CreateInputLayout(&ie, 1, blob->GetBufferPointer(), blob->GetBufferSize(), &g_rig.il));
  blob->Release();
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = 64;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ok = ok && SUCCEEDED(g_rig.dev->CreateBuffer(&bd, nullptr, &g_rig.cb));
  bd.ByteWidth = sizeof(g_geo);
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  ok = ok && SUCCEEDED(g_rig.dev->CreateBuffer(&bd, nullptr, &g_rig.vb));
  D3D11_DEPTH_STENCIL_DESC dd{};
  dd.DepthEnable = TRUE;
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dd.DepthFunc = D3D11_COMPARISON_GREATER;  // DCS's cascades: reverse depth, GREATER, writes
  ok = ok && SUCCEEDED(g_rig.dev->CreateDepthStencilState(&dd, &g_rig.ds));
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  ok = ok && SUCCEEDED(g_rig.dev->CreateRasterizerState(&rd, &g_rig.rs));
  return ok;
}

// The fake Shadows state of one frame: light view V, per-cascade unsnapped
// boxes around the camera x, then DCS's loop: snap (here: as is), ortho,
// ShadowMatrix, SceneRender slot 1 (our hook) per cascade.
float g_V[16];
float g_camX = 0;
float g_anchor = 0;
int g_size = kRes;
float g_dcsP[kCasc][16];  // DCS's own proj of the frame

Box TightBox(int i) {
  const double hx = 9.0 + 3.0 * i, hy = 7.0 + 2.0 * i;
  Box b{{g_camX - hx, -hy, -40.0}, {g_camX + hx, hy, 40.0}};
  return b;
}

void ParseFrame() {
  *reinterpret_cast<int32_t*>(g_impl + kImplSize) = g_size;
  *reinterpret_cast<int32_t*>(g_impl + kImplFirst) = 0;
  memcpy(g_impl + kImplView, g_V, 64);
  const float an[3] = {g_anchor, 0, 0};
  memcpy(g_impl + kImplAnchor, an, sizeof(an));
  for (int i = 0; i < kCasc; ++i) {
    const Box b = TightBox(i);
    float* f = reinterpret_cast<float*>(g_impl + kImplBox + kBoxStride * i);
    for (int a = 0; a < 3; ++a) f[a] = float(b.lo[a]), f[3 + a] = float(b.hi[a]);
  }
  for (int i = 0; i < kCasc; ++i) {
    // DCS: snap box i (unchanged here), proj, ShadowMatrix row-major, then slot 1.
    float* f = reinterpret_cast<float*>(g_impl + kImplBox + kBoxStride * i);
    Box s{{f[0], f[1], f[2]}, {f[3], f[4], f[5]}};
    s.lo[0] -= 0.25, s.hi[0] += 0.5;  // DCS's snapped box encloses the unsnapped one
    for (int a = 0; a < 3; ++a) f[a] = float(s.lo[a]), f[3 + a] = float(s.hi[a]);
    OrthoFromBox(s, g_dcsP[i]);
    float sm[16];
    ShadowMatrixOf(g_V, g_dcsP[i], sm);
    memcpy(g_impl + kImplSM + 64 * i, sm, 64);
    using Slot1 = void(__fastcall*)(void*, int, const float*, const float*);
    reinterpret_cast<Slot1>(g_moduleVt[1])(g_module, i, g_V, g_dcsP[i]);
  }
}

// The four cascade passes in DCS's order (finest first).
void RunPasses() {
  for (int k = 0; k < kCasc; ++k) {
    const int d = kCasc - 1 - k;
    shadowpass::CallOrig(g_pass[d], &g_ctxObj);
  }
}

void SetupFakes() {
  memset(g_module, 0, sizeof(g_module));
  memset(g_impl, 0, sizeof(g_impl));
  memset(g_ctx0, 0, sizeof(g_ctx0));
  for (auto& v : g_moduleVt) v = nullptr;
  g_moduleVt[1] = reinterpret_cast<void*>(&ParseHook);
  *reinterpret_cast<void**>(g_module) = g_moduleVt;
  *reinterpret_cast<int32_t*>(g_module + kModCount) = kCasc;
  *reinterpret_cast<void**>(g_module + kModShadows) = g_impl;
  *reinterpret_cast<void**>(g_ctx0 + 0x438) = g_vecs;
  for (int d = 0; d < kCasc; ++d) {
    memset(g_pass[d], 0, sizeof(g_pass[d]));
    memset(g_node[d], 0, sizeof(g_node[d]));
    *reinterpret_cast<int32_t*>(g_pass[d] + kPassData) = d;
    *reinterpret_cast<void**>(g_pass[d] + 0x48 + 0x10) = g_node[d];
    *reinterpret_cast<int16_t*>(g_node[d] + 0x38) = static_cast<int16_t>(d);
    g_items[d].clear();
    // DCS's vector order: model casters and terrain interleaved.
    for (int g = 0; g < kGeoStatic + kGeoDyn; ++g) {
      const bool isStatic = g < kGeoStatic;
      g_itemPool[d][g] = {isStatic ? g_staticVt : g_dynVt, g};
    }
    const int order[kGeoStatic + kGeoDyn] = {6, 0, 1, 7, 2, 3, 4, 5};
    for (int g : order) g_items[d].push_back(&g_itemPool[d][g]);
    g_vecs[d * 3 + 0] = g_items[d].data();
    g_vecs[d * 3 + 1] = g_items[d].data() + g_items[d].size();
    g_vecs[d * 3 + 2] = g_vecs[d * 3 + 1];
  }
  for (int i = 0; i < 8; ++i) g_staticVt[i] = g_dynVt[i] = g_otherVt[i] = reinterpret_cast<void*>(&DrawItem);
  memset(g_V, 0, sizeof(g_V));
  g_V[0] = g_V[5] = g_V[10] = g_V[15] = 1.0f;  // light space = world
}

// One frame: geometry, parse, passes; then the stock render of each cascade
// with the projection its pass used, compared texel for texel.
struct FrameResult {
  int64_t bad[kCasc];
  int64_t nonZero[kCasc];
};
FrameResult Frame(bool sway = false) {
  BuildGeo(g_camX);
  if (sway) memcpy(g_geo[0], g_geo[kGeo - 1], sizeof(g_geo[0]));  // a static caster moved (wind) after the capture
  UploadGeo();
  ParseFrame();
  RunPasses();
  FrameResult r{};
  for (int d = 0; d < kCasc; ++d) {
    const uint8_t* c = g_pass[d] + kPassData + kDataCtx;
    BuildGeo(g_camX);
    if (sway) memcpy(g_geo[0], g_geo[kGeo - 1], sizeof(g_geo[0]));
    UploadGeo();
    RenderRef(d, reinterpret_cast<const float*>(c + kCtxView), reinterpret_cast<const float*>(c + kCtxProj), false);
    r.bad[d] = DiffSlice(d);
    r.nonZero[d] = NonZero(d);
  }
  return r;
}

bool g_probeOk = false;
void __fastcall ProbeFn(void*, void*) {
  ID3D11DepthStencilView* d = nullptr;
  D3D11_VIEWPORT vp[2];
  UINT n = 2;
  g_rig.ctx->OMGetRenderTargets(0, nullptr, &d);
  g_rig.ctx->RSGetViewports(&n, vp);
  g_probeOk = d == g_rig.dsv[0] && n == 1 && vp[0].Width == float(kRes);
  if (d) d->Release();
}

void ResetState() {
  g_fs = FrameState{};
  ResetCaches();
  for (auto& s : g_slot) {
    if (s.tex) s.tex->Release();
    s.tex = nullptr;
    s.desc = {};
  }
  g_projOff = -1;
  g_genNext = 0;
  using CcStats = cascache::Stats;
  g_st.~CcStats();
  new (&g_st) CcStats();
}

void ReplicaTests() {
  if (!MakeRig()) {
    Check(false, "cascade cache: WARP rig (device, depth array, shaders)");
    g_rig.Release();
    return;
  }
  // Save and redirect the globals the replica uses.
  const auto origSave = shadowpass::g_orig;
  const auto innerSave = shadowpass::g_inner.load();
  const auto wrapSave = shadowpass::g_wrap.load();
  const auto parseSave = g_parseOrig;
  const auto expectSave = g_expectVtbl;
  const uintptr_t lo0 = g_staticLo, hi0 = g_staticHi;
  void* smrSave = g_smrVtbl;
  ID3D11DeviceContext* ctxSave = g_ctx;
  const int64_t gapSave = g_gapQpc;
  // The verify's second run is refused while shadow batching or the recorder is chained: neither is here.
  const bool sbOnSave = shadowbatch::g_on.load();
  const bool srDisSave = shrec::g_disabled.load();
  shadowbatch::g_on = false;
  shrec::g_disabled = true;
  shadowpass::g_orig = &FakeExec;
  shadowpass::g_wrap = nullptr;
  g_parseOrig = &FakeParse;
  g_expectVtbl = g_moduleVt;
  g_staticLo = reinterpret_cast<uintptr_t>(&g_staticVt[0]);
  g_staticHi = reinterpret_cast<uintptr_t>(&g_staticVt[8]);
  g_smrVtbl = nullptr;
  g_ctx = g_rig.ctx;
  g_ctx->AddRef();
  g_gapQpc = 0;
  InitSteps();
  SetupFakes();
  ResetState();

  // Off: DCS's projections reach the parse unchanged and every caster is drawn in DCS's order.
  g_on = false;
  shadowpass::g_inner = &Exec;
  g_trace.clear();
  g_parseCalls = 0;
  FrameResult r = Frame();
  bool same = g_parseCalls == kCasc;
  for (int i = 0; i < kCasc; ++i) same = same && memcmp(g_parseGot[i], g_dcsP[i], 64) == 0;
  bool order = true;
  {
    // Each pass draws 6,0,1,7,2,3,4,5 (cache path), then the reference 0..7.
    const int want[8] = {6, 0, 1, 7, 2, 3, 4, 5};
    for (int i = 0; i < 8 && i < static_cast<int>(g_trace.size()); ++i) order = order && g_trace[i] == want[i];
  }
  bool exact = true;
  for (int d = 0; d < kCasc; ++d) exact = exact && r.bad[d] == 0 && r.nonZero[d] > 0;
  Check(same && order && exact && g_st.passes.load() == 0,
        "cascade cache: off = DCS's projections, DCS's caster order, nothing of ours runs");

  // On, cascades 2 and 3 (data indices 1 and 0).
  g_mask = 0xc;
  g_maxAge = 4;
  g_marginPm = 30;
  g_zMarginPm = 50;
  g_verifyEvery = 0;
  g_on = true;
  int64_t worst = 0;
  bool allNonZero = true;
  const uint64_t ref0 = g_st.planRefresh[3].load();
  for (int f = 0; f < 4; ++f) {
    g_dynOffset = 0.5f * f;  // the dynamic casters move every frame
    g_camX = 0.02f * f;      // a slow camera, inside the margin
    r = Frame();
    for (int d = 0; d < kCasc; ++d) worst = (std::max)(worst, r.bad[d] < 0 ? 1 << 30 : r.bad[d]), allNonZero &= r.nonZero[d] > 0;
  }
  const uint64_t refresh3 = g_st.planRefresh[3].load() - ref0, reuse3 = g_st.planReuse[3].load();
  Check(worst == 0 && allNonZero && refresh3 == 1 && reuse3 == 3 && g_st.captured.load() == 2 &&
            g_st.restored.load() == 6 && g_st.restoreFallback.load() == 0 && g_st.noPlan.load() == 0,
        "cascade cache: WARP replica, 1 refresh + 3 reuses per cached cascade, every slice texel-identical to a stock "
        "render with the same projection (dynamic casters moving)");
  printf("     (replica: refresh %llu / reuse %llu for CascadeShadowPass3, proj offset learned at +0x%x)\n",
         (unsigned long long)refresh3, (unsigned long long)reuse3, g_projOff.load());

  // The matrix the receivers use is the one the pass rendered with (P' in both).
  bool smOk = true;
  for (int i = 0; i < kCasc; ++i) {
    const int d = kCasc - 1 - i;
    float want[16];
    ShadowMatrixOf(g_V, reinterpret_cast<const float*>(g_pass[d] + kPassData + kDataCtx + kCtxProj), want);
    smOk = smOk && memcmp(g_impl + kImplSM + 64 * i, want, 64) == 0;
    const bool held = (0xc >> i) & 1;
    smOk = smOk && (held ? memcmp(g_parseGot[i], g_dcsP[i], 64) != 0 : memcmp(g_parseGot[i], g_dcsP[i], 64) == 0);
  }
  Check(smOk, "cascade cache: ShadowMatrix = V * P' * B of the projection the pass used; other cascades untouched");

  // Age: the 5th frame after the capture refreshes cascade 2; cascade 3 waits one frame.
  g_camX = 0.1f;
  Frame();
  Check(g_st.reason[2][kRAge].load() == 1 && g_st.reason[3][kRAge].load() == 0 && g_st.planReuse[3].load() == 4,
        "cascade cache: age refresh staggered (cascade 2 at age 4, cascade 3 one frame later)");
  Frame();
  Check(g_st.reason[3][kRAge].load() == 1, "cascade cache: cascade 3's deferred age refresh");

  // Wind: a static caster moves after the capture; the reuse misses it; verify sees it.
  g_verifyEvery = 1;
  const uint64_t vb0 = g_st.verifyBad.load();
  r = Frame(true);  // reuse frame for both (captured one/two frames ago)
  Check(g_st.verifyPasses.load() >= 1 && g_st.verifyBad.load() > vb0,
        "cascade cache: verify (cached composite vs full redraw) counts the texels of a static caster that moved");
  // The verify's full redraw leaves the slice stock for that frame.
  bool stockAfterVerify = true;
  for (int d = 0; d < 2; ++d) {
    const uint8_t* c = g_pass[d] + kPassData + kDataCtx;
    BuildGeo(g_camX);
    memcpy(g_geo[0], g_geo[kGeo - 1], sizeof(g_geo[0]));
    UploadGeo();
    RenderRef(d, reinterpret_cast<const float*>(c + kCtxView), reinterpret_cast<const float*>(c + kCtxProj), false);
    stockAfterVerify = stockAfterVerify && DiffSlice(d) == 0;
  }
  Check(stockAfterVerify, "cascade cache: after a verify the slice holds the full redraw");
  g_verifyEvery = 0;
  // Without verify the reuse holds the old static depth: the documented loss (wind, LOD).
  r = Frame(true);
  const bool lossy = r.bad[0] > 0 || r.bad[1] > 0;
  Check(lossy && r.bad[2] == 0 && r.bad[3] == 0,
        "cascade cache: negative control, a moved static caster differs in the cached cascades only");

  // R49: the shadow recorder installed (ready) but switched off at runtime does not block the verify; on, it does.
  {
    const int st0 = shrec::g_state.load();
    const bool on0 = shrec::g_on.load();
    shrec::g_state = 1;
    shrec::g_on = false;
    g_verifyEvery = 1;
    const uint64_t p0 = g_st.verifyPasses.load(), k0 = g_st.verifySkipped.load();
    for (int i = 0; i < 6 && g_st.verifyPasses.load() == p0; ++i) Frame(true);
    const bool offRuns = g_st.verifyPasses.load() > p0 && g_st.verifySkipped.load() == k0;
    shrec::g_on = true;
    const bool dis0 = shrec::g_disabled.load(), sd0 = shrec::g_shutdown.load();
    shrec::g_disabled = false;
    shrec::g_shutdown = false;
    const bool activeNow = shrec::Active();
    if (!activeNow)
      printf("     (shrec::Active false with on + ready: gbbatch %d, shadowbatch verify %d, shadowrec chained %d)\n",
             (int)gbbatch::g_on.load(), (int)shadowbatch::g_verify.load(), (int)shadowrec::g_chained.load());
    const uint64_t p1 = g_st.verifyPasses.load(), k1 = g_st.verifySkipped.load();
    for (int i = 0; i < 6 && g_st.verifySkipped.load() == k1; ++i) Frame(true);
    const bool onSkips = g_st.verifySkipped.load() > k1 && g_st.verifyPasses.load() == p1;
    shrec::g_on = on0;
    shrec::g_state = st0;
    shrec::g_disabled = dis0;
    shrec::g_shutdown = sd0;
    g_verifyEvery = 0;
    if (!(offRuns && onSkips))
      printf("     (recorder off: verify passes +%llu skipped +%llu; on: passes +%llu skipped +%llu)\n",
             (unsigned long long)(p1 - p0), (unsigned long long)(k1 - k0),
             (unsigned long long)(g_st.verifyPasses.load() - p1), (unsigned long long)(g_st.verifySkipped.load() - k1));
    Check(offRuns && onSkips, "cascade cache R49: verify runs with the shadow recorder ready but switched off at runtime, "
                              "skipped while it is on");
  }

  // Triggers.
  const uint64_t f0 = g_st.reason[3][kRForced].load(), l0 = g_st.reason[3][kRLight].load(),
                 o0 = g_st.reason[3][kROutside].load(), s0 = g_st.reason[3][kRSize].load();
  g_V[0] = 0.9999999f;  // DCS's sun hysteresis stepped
  Frame();
  g_V[0] = 1.0f;
  Frame();  // back: also a light change
  g_camX = 3.0f;  // beyond the margin
  Frame();
  g_anchor = 100.0f;  // origin shift / teleport
  Frame();
  g_missionGen.fetch_add(1);
  Frame();
  Check(g_st.reason[3][kRLight].load() - l0 == 2 && g_st.reason[3][kROutside].load() - o0 == 1 &&
            g_st.reason[3][kRForced].load() - f0 == 2 && g_st.reason[3][kRSize].load() == s0,
        "cascade cache: refresh on light step, leaving the window, anchor jump and mission event");

  // A pass whose context holds no plan of ours (another parse): stock, nothing restored.
  {
    const uint64_t np0 = g_st.noPlan.load();
    BuildGeo(g_camX);
    UploadGeo();
    ParseFrame();
    uint8_t* c = g_pass[0] + kPassData + kDataCtx;
    c[kCtxProj + 12] ^= 1;  // the pass was parsed with something else
    RunPasses();
    RenderRef(0, reinterpret_cast<const float*>(c + kCtxView), reinterpret_cast<const float*>(c + kCtxProj), false);
    Check(g_st.noPlan.load() == np0 + 1 && DiffSlice(0) == 0,
          "cascade cache: a pass without a matching plan draws every caster (stock)");
  }

  // Map size change and the restore fallback (cache texture gone).
  {
    Frame();
    Frame();  // a reuse is due next
    const uint64_t fb0 = g_st.restoreFallback.load();
    {
      std::lock_guard<std::mutex> lock(g_lock);
      for (Slot& s : g_slot) {
        if (s.tex) s.tex->Release();
        s.tex = nullptr;
      }
    }
    r = Frame();
    Check(g_st.reuseMiss.load() >= 1 && r.bad[0] == 0 && r.bad[1] == 0 && g_st.restoreFallback.load() == fb0,
          "cascade cache: a reuse whose texture is gone draws every caster");
    g_size = kRes * 2;
    Frame();
    Check(g_st.reason[3][kRSize].load() == s0 + 1, "cascade cache: refresh on a map size change");
    g_size = kRes;
  }
  // OM state after our steps: DCS's framebuffer is bound again.
  {
    g_camX = 0;
    Frame();
    BuildGeo(0);
    UploadGeo();
    ParseFrame();
    // A dynamic probe item in cascade 3's pass checks the bound DSV after the restore step.
    static void* probeVt[8];
    for (auto& v : probeVt) v = reinterpret_cast<void*>(&ProbeFn);
    FakeItem probe{probeVt, 0};
    g_items[0].push_back(&probe);
    g_vecs[1] = g_items[0].data() + g_items[0].size();
    g_vecs[0] = g_items[0].data();
    g_probeOk = false;
    RunPasses();
    g_items[0].pop_back();
    g_vecs[0] = g_items[0].data();
    g_vecs[1] = g_items[0].data() + g_items[0].size();
    Check(g_probeOk, "cascade cache: the depth target and viewport are bound again after the snap/restore copy");
  }

  // Back to stock.
  g_on = false;
  ResetState();
  shadowpass::g_orig = origSave;
  shadowpass::g_inner = innerSave;
  shadowpass::g_wrap = wrapSave;
  g_parseOrig = parseSave;
  g_expectVtbl = expectSave;
  g_staticLo = lo0;
  g_staticHi = hi0;
  g_smrVtbl = smrSave;
  g_ctx->Release();
  g_ctx = ctxSave;
  g_gapQpc = gapSave;
  shadowbatch::g_on = sbOnSave;
  shrec::g_disabled = srDisSave;
  g_mask = 0xc;
  g_rig.Release();
}
void Run() {
  MathTests();
  DecideTests();
  ListTests();
  ReplicaTests();
}

}  // namespace cctest
