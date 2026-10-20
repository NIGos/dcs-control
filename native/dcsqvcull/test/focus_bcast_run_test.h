// R53 (focus_bcast_run.h): pure parts (projection shape, affine, same-eye check, jitter prediction, learning, formats,
// merge shader), the archive scan, the upload observer, and an end-to-end GPU check on this PC's NVIDIA device: a
// DCS-like eye (periphery 476x468 + focus 1176x1164, 5 R8G8 G-buffer slices + R16G16F motion vectors, D32S8 reverse-Z
// GREATER_EQUAL with stencil writes, a terrain draw then indirect instanced "tree" draws with alpha test) rendered
// twice per frame: stock (two passes) and through fbcr's own callbacks (learning frame, prediction, block, FastGS
// broadcast, copy-back, merge with the motion-vector fixup, skipped focus draws). Periphery must be bit-identical,
// the focus within float rounding; also a command-list close mid-block, a DCS rebind mid-block (abort merge, learned
// start), and a gaze jump (prediction miss: the focus stays stock).
#pragma once

namespace fbcrtest {

using namespace fbcr;

constexpr UINT kWP = 476, kHP = 468, kWF = 1176, kHF = 1164;
constexpr int kTrees = 6;  // indirect draws (buckets)

const char kSceneHlsl[] = R"(
cbuffer TC : register(b11) { float4 tc[136]; };
cbuffer cPerView : register(b7) { float4 pv[36]; };  // DCS's layout: gPrevFrameTransform at registers 32-35
StructuredBuffer<float4> inst : register(t0);
struct VO {
  float4 pos : SV_Position;
  float4 cur : TEXCOORD0;
  float4 prev : TEXCOORD1;
  float2 uv : TEXCOORD2;
  nointerpolation uint id : TEXCOORD3;
};
VO Xf(float3 w, float3 wPrev, float2 uv, uint id) {
  float4 x = float4(w, 1), xp = float4(wPrev, 1);
  VO o;
  o.pos = float4(dot(tc[20], x), dot(tc[21], x), dot(tc[22], x), dot(tc[23], x));
  o.cur = o.pos;
  // DCS: the previous position through this frame's view-projection, then gPrevFrameTransform in the PS
  o.prev = float4(dot(tc[20], xp), dot(tc[21], xp), dot(tc[22], xp), dot(tc[23], xp));
  o.uv = uv;
  o.id = id;
  return o;
}
VO VsInst(float3 p : POSITION, uint iid : SV_InstanceID) {
  float4 I = inst[iid];
  float3 w = I.xyz + p * I.w;
  float3 wind = float3(0.03 * sin(iid * 1.7), 0, 0.02 * cos(iid * 0.9)) * saturate(p.y + 0.5);  // leaves sway
  return Xf(w, w - wind, p.xy + 0.5, iid);
}
VO VsGround(float3 p : POSITION) { return Xf(p, p, p.xz * 0.01, 0); }
struct PO {
  float2 t0 : SV_Target0; float2 t1 : SV_Target1; float2 t2 : SV_Target2; float2 t3 : SV_Target3;
  float2 t4 : SV_Target4; float2 mv : SV_Target5;
};
PO Out(VO i, float k) {
  PO o;
  float2 dims = tc[44].zw;
  float4 pc = float4(dot(i.prev, pv[32]), dot(i.prev, pv[33]), dot(i.prev, pv[34]), dot(i.prev, pv[35]));
  float2 v = (pc.xy / pc.w - i.cur.xy / i.cur.w) * 0.5 * dims;
  o.mv = float2(v.x, -v.y);
  o.t0 = frac(i.uv);
  o.t1 = float2(frac(i.id * 0.37 + k), 0.5);
  o.t2 = float2(saturate(i.cur.z / i.cur.w * 20), k);
  o.t3 = frac(i.uv * 3.7);
  o.t4 = float2(0.25 + 0.5 * k, frac(i.id * 0.11));
  return o;
}
PO PsInst(VO i) {
  float2 q = frac(i.uv * 3);
  if (dot(q - 0.5, q - 0.5) > 0.18) discard;
  return Out(i, 0.75);
}
PO PsGround(VO i) { return Out(i, 0.25); }
)";

struct Targets {  // one view's "DCS" G-buffer
  UINT w = 0, h = 0;
  ID3D11Texture2D *gb = nullptr, *mv = nullptr, *dep = nullptr;
  ID3D11RenderTargetView* rtv[6] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  void Release() {
    for (auto& p : rtv) Rel(p);
    Rel(gb), Rel(mv), Rel(dep), Rel(dsv);
  }
};
bool MakeTargets(ID3D11Device* d, UINT w, UINT h, Targets& t) {
  t.w = w, t.h = h;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = w, td.Height = h, td.MipLevels = 1, td.ArraySize = 5, td.Format = DXGI_FORMAT_R8G8_UNORM;  // as DCS (49)
  td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(d->CreateTexture2D(&td, nullptr, &t.gb))) return false;
  td.ArraySize = 1, td.Format = DXGI_FORMAT_R16G16_FLOAT;  // as DCS (34)
  if (FAILED(d->CreateTexture2D(&td, nullptr, &t.mv))) return false;
  td.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(d->CreateTexture2D(&td, nullptr, &t.dep))) return false;
  for (UINT k = 0; k < 5; ++k) {
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = DXGI_FORMAT_R8G8_UNORM;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    rd.Texture2DArray.FirstArraySlice = k, rd.Texture2DArray.ArraySize = 1;
    if (FAILED(d->CreateRenderTargetView(t.gb, &rd, &t.rtv[k]))) return false;
  }
  D3D11_RENDER_TARGET_VIEW_DESC rd{};
  rd.Format = DXGI_FORMAT_R16G16_FLOAT;
  rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  if (FAILED(d->CreateRenderTargetView(t.mv, &rd, &t.rtv[5]))) return false;
  D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
  dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
  return SUCCEEDED(d->CreateDepthStencilView(t.dep, &dd, &t.dsv));
}

// Row-major 4x4 helpers (rows as float4 in the CB, clip.c = dot(row c, x)).
struct M4 {
  double m[4][4] = {};
};
M4 Inverse(const M4& a) {  // Gauss-Jordan, partial pivoting
  double m[4][8];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 8; ++j) m[i][j] = j < 4 ? a.m[i][j] : (j - 4 == i ? 1 : 0);
  for (int col = 0; col < 4; ++col) {
    int piv = col;
    for (int r = col + 1; r < 4; ++r)
      if (std::fabs(m[r][col]) > std::fabs(m[piv][col])) piv = r;
    for (int j = 0; j < 8; ++j) std::swap(m[col][j], m[piv][j]);
    const double d = m[col][col];
    for (int j = 0; j < 8; ++j) m[col][j] /= d;
    for (int r = 0; r < 4; ++r)
      if (r != col) {
        const double f = m[r][col];
        for (int j = 0; j < 8; ++j) m[r][j] -= f * m[col][j];
      }
  }
  M4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) r.m[i][j] = m[i][j + 4];
  return r;
}
M4 Mul(const M4& a, const M4& b) {
  M4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
  return r;
}
// Reverse-Z infinite projection (near 0.02), scales and centres (the jitter inside the centres).
M4 Proj(double sx, double cx, double sy, double cy) {
  M4 p;
  p.m[0][0] = sx, p.m[0][2] = cx, p.m[1][1] = sy, p.m[1][2] = cy, p.m[2][3] = 0.02, p.m[3][2] = 1;
  return p;
}
M4 View(double yaw, double pitch, double tx, double ty, double tz) {  // world -> view (z forward)
  const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
  M4 ry, rp, t;
  ry.m[0][0] = cy, ry.m[0][2] = -sy, ry.m[1][1] = 1, ry.m[2][0] = sy, ry.m[2][2] = cy, ry.m[3][3] = 1;
  rp.m[0][0] = 1, rp.m[1][1] = cp, rp.m[1][2] = -sp, rp.m[2][1] = sp, rp.m[2][2] = cp, rp.m[3][3] = 1;
  for (int i = 0; i < 4; ++i) t.m[i][i] = 1;
  t.m[0][3] = -tx, t.m[1][3] = -ty, t.m[2][3] = -tz;
  return Mul(Mul(rp, ry), t);
}
void FillTc(float* tc, const M4& P, const M4& VP, double w, double h) {  // 136 float4 rows
  memset(tc, 0, 136 * 16);
  for (int r = 0; r < 4; ++r)
    for (int k = 0; k < 4; ++k) tc[(16 + r) * 4 + k] = static_cast<float>(P.m[r][k]), tc[(20 + r) * 4 + k] = static_cast<float>(VP.m[r][k]);
  tc[38 * 4 + 3] = 1;
  tc[44 * 4 + 2] = static_cast<float>(w), tc[44 * 4 + 3] = static_cast<float>(h);
}

struct Scene {
  ID3D11Device* d = nullptr;
  ID3D11DeviceContext* c = nullptr;
  ID3D11VertexShader *vsInst = nullptr, *vsGround = nullptr;
  ID3D11PixelShader *psInst = nullptr, *psGround = nullptr;
  ID3D11InputLayout* il = nullptr;
  ID3D11Buffer *vbTree = nullptr, *ibTree = nullptr, *vbGround = nullptr, *ibGround = nullptr, *instB = nullptr,
               *args = nullptr, *tc = nullptr, *pv = nullptr;
  ID3D11ShaderResourceView* instSrv[kTrees] = {};
  ID3D11DepthStencilState* dss = nullptr;
  ID3D11RasterizerState* rs = nullptr;
  ID3D11Texture2D* dummy = nullptr;
  ID3D11RenderTargetView* dummyRtv = nullptr;
  std::vector<uint8_t> vsInstCode;
  UINT treeIdx = 0;
  void Release() {
    Rel(vsInst), Rel(vsGround), Rel(psInst), Rel(psGround), Rel(il), Rel(vbTree), Rel(ibTree), Rel(vbGround), Rel(ibGround);
    Rel(instB), Rel(args), Rel(tc), Rel(pv), Rel(dss), Rel(rs), Rel(dummyRtv), Rel(dummy);
    for (auto& s : instSrv) Rel(s);
  }
};
bool MakeScene(ID3D11Device* d, ID3D11DeviceContext* c, Scene& s) {
  s.d = d, s.c = c;
  std::string e;
  auto comp = [&](const char* entry, const char* tgt) { return fbc::CompileHlsl(kSceneHlsl, entry, tgt, &e); };
  ID3DBlob *a = comp("VsInst", "vs_5_0"), *b = comp("VsGround", "vs_5_0"), *p1 = comp("PsInst", "ps_5_0"),
           *p2 = comp("PsGround", "ps_5_0");
  bool ok = a && b && p1 && p2;
  if (!ok) printf("     (scene compile: %s)\n", e.c_str());
  ok = ok && SUCCEEDED(d->CreateVertexShader(a->GetBufferPointer(), a->GetBufferSize(), nullptr, &s.vsInst)) &&
       SUCCEEDED(d->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.vsGround)) &&
       SUCCEEDED(d->CreatePixelShader(p1->GetBufferPointer(), p1->GetBufferSize(), nullptr, &s.psInst)) &&
       SUCCEEDED(d->CreatePixelShader(p2->GetBufferPointer(), p2->GetBufferSize(), nullptr, &s.psGround));
  if (ok) {
    const uint8_t* q = static_cast<const uint8_t*>(a->GetBufferPointer());
    s.vsInstCode.assign(q, q + a->GetBufferSize());
    const D3D11_INPUT_ELEMENT_DESC ie{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    ok = SUCCEEDED(d->CreateInputLayout(&ie, 1, a->GetBufferPointer(), a->GetBufferSize(), &s.il));
  }
  for (ID3DBlob* x : {a, b, p1, p2})
    if (x) x->Release();
  if (!ok) return false;
  // A "tree": 6 crossed leaf cards (quads) of unit size.
  std::vector<float> v;
  std::vector<uint16_t> ix;
  for (int k = 0; k < 6; ++k) {
    const double ang = k * 0.5236, ca = std::cos(ang) * 0.5, sa = std::sin(ang) * 0.5;
    const double y0 = -0.5 + 0.15 * (k % 3);
    const uint16_t base = static_cast<uint16_t>(v.size() / 3);
    const double pts[4][3] = {{-ca, y0, -sa}, {ca, y0, sa}, {ca, y0 + 0.9, sa}, {-ca, y0 + 0.9, -sa}};
    for (auto& pt : pts) v.push_back(float(pt[0])), v.push_back(float(pt[1])), v.push_back(float(pt[2]));
    for (int t : {0, 1, 2, 0, 2, 3}) ix.push_back(static_cast<uint16_t>(base + t));
  }
  s.treeIdx = static_cast<UINT>(ix.size());
  auto mk = [&](UINT bind, const void* data, UINT bytes, ID3D11Buffer** out, UINT misc = 0, UINT stride = 0) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes, bd.Usage = D3D11_USAGE_DEFAULT, bd.BindFlags = bind, bd.MiscFlags = misc;
    bd.StructureByteStride = stride;
    D3D11_SUBRESOURCE_DATA sd{data, 0, 0};
    return SUCCEEDED(d->CreateBuffer(&bd, data ? &sd : nullptr, out));
  };
  const float ground[12] = {-400, -2, 1, 400, -2, 1, 400, -2, 800, -400, -2, 800};
  const uint16_t gix[6] = {0, 2, 1, 0, 3, 2};
  // instances: kTrees buckets x 120 trees over the field ahead
  std::vector<float> in;
  uint32_t rng = 12345;
  auto rnd = [&] { return (rng = rng * 1664525u + 1013904223u) / 4294967296.0; };
  for (int i = 0; i < kTrees * 120; ++i) {
    const double z = 4 + 120 * rnd() * rnd(), x = (rnd() - 0.5) * z * 2.4;
    in.push_back(float(x)), in.push_back(float(-1.2 + rnd() * 0.3)), in.push_back(float(z)), in.push_back(float(0.6 + 1.4 * rnd()));
  }
  ok = mk(D3D11_BIND_VERTEX_BUFFER, v.data(), UINT(v.size() * 4), &s.vbTree) &&
       mk(D3D11_BIND_INDEX_BUFFER, ix.data(), UINT(ix.size() * 2), &s.ibTree) &&
       mk(D3D11_BIND_VERTEX_BUFFER, ground, sizeof(ground), &s.vbGround) &&
       mk(D3D11_BIND_INDEX_BUFFER, gix, sizeof(gix), &s.ibGround) &&
       mk(D3D11_BIND_SHADER_RESOURCE, in.data(), UINT(in.size() * 4), &s.instB, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16);
  for (int k = 0; ok && k < kTrees; ++k) {
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_UNKNOWN, sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sv.Buffer.FirstElement = k * 120, sv.Buffer.NumElements = 120;
    ok = SUCCEEDED(d->CreateShaderResourceView(s.instB, &sv, &s.instSrv[k]));
  }
  std::vector<uint32_t> ar;
  for (int k = 0; k < kTrees; ++k) ar.insert(ar.end(), {s.treeIdx, 120u, 0u, 0u, 0u, 0u, 0u, 0u});  // 32-byte entries
  ok = ok && mk(0, ar.data(), UINT(ar.size() * 4), &s.args, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS);
  ok = ok && mk(D3D11_BIND_CONSTANT_BUFFER, nullptr, 136 * 16, &s.tc) && mk(D3D11_BIND_CONSTANT_BUFFER, nullptr, 36 * 16, &s.pv);
  D3D11_DEPTH_STENCIL_DESC ds{};
  ds.DepthEnable = TRUE, ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL, ds.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
  ds.StencilEnable = TRUE, ds.StencilReadMask = 0xff, ds.StencilWriteMask = 0x30;
  ds.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
  ds.BackFace = ds.FrontFace;
  ok = ok && SUCCEEDED(d->CreateDepthStencilState(&ds, &s.dss));
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = TRUE;
  ok = ok && SUCCEEDED(d->CreateRasterizerState(&rd, &s.rs));
  D3D11_TEXTURE2D_DESC td{};
  td.Width = 816, td.Height = 804, td.MipLevels = 1, td.ArraySize = 1, td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT, td.BindFlags = D3D11_BIND_RENDER_TARGET;
  ok = ok && SUCCEEDED(d->CreateTexture2D(&td, nullptr, &s.dummy)) &&
       SUCCEEDED(d->CreateRenderTargetView(s.dummy, nullptr, &s.dummyRtv));
  return ok;
}

struct FrameIn {
  M4 Pp, Pf, V, Vprev;  // projections (jittered) and view; previous-frame VPs come in the PV buffer
  M4 PpPrev, PfPrev;
};
// One view's G-buffer pass: TerrainContext upload (the first event), ground (not hooked), the tree buckets through
// fbcr's callbacks when `hooked` (view = HMD view index), one more ground-like draw after them (stock ordering).
// rebindAfter >= 0: DCS rebinds its targets after that bucket; eclAfter >= 0: a command list "executes" there.
void Pass(Scene& s, Targets& t, const M4& P, const M4& V, const M4& Pprev, const M4& Vprev, bool hooked, int view,
          int rebindAfter = -1, int eclAfter = -1) {
  ID3D11DeviceContext* c = s.c;
  float tcv[136 * 4];
  const M4 VP = Mul(P, V), VPprev = Mul(Pprev, Vprev);
  FillTc(tcv, P, VP, t.w, t.h);
  c->UpdateSubresource(s.tc, 0, nullptr, tcv, 0, 0);  // through the table: the observer when armed
  // gPrevFrameTransform = previous view-projection x inverse(current) (clip -> previous clip), registers 32-35
  const M4 T = Mul(VPprev, Inverse(VP));
  float pv[36 * 4] = {};
  for (int r = 0; r < 4; ++r)
    for (int k = 0; k < 4; ++k) pv[(32 + r) * 4 + k] = static_cast<float>(T.m[r][k]);
  c->UpdateSubresource(s.pv, 0, nullptr, pv, 0, 0);
  c->OMSetRenderTargets(6, t.rtv, t.dsv);
  const D3D11_VIEWPORT vp{0, 0, float(t.w), float(t.h), 0, 1};
  c->RSSetViewports(1, &vp);
  c->RSSetState(s.rs);
  c->OMSetDepthStencilState(s.dss, 0x10);
  c->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
  c->IASetInputLayout(s.il);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c->VSSetConstantBuffers(11, 1, &s.tc);
  c->PSSetConstantBuffers(11, 1, &s.tc);
  c->VSSetConstantBuffers(7, 1, &s.pv);
  c->PSSetConstantBuffers(7, 1, &s.pv);
  const UINT stride = 12, off = 0;
  c->IASetVertexBuffers(0, 1, &s.vbGround, &stride, &off);
  c->IASetIndexBuffer(s.ibGround, DXGI_FORMAT_R16_UINT, 0);
  c->VSSetShader(s.vsGround, nullptr, 0);
  c->PSSetShader(s.psGround, nullptr, 0);
  c->DrawIndexed(6, 0, 0);
  c->OMSetDepthStencilState(s.dss, 0x30);
  c->IASetVertexBuffers(0, 1, &s.vbTree, &stride, &off);
  c->IASetIndexBuffer(s.ibTree, DXGI_FORMAT_R16_UINT, 0);
  c->VSSetShader(s.vsInst, nullptr, 0);
  c->PSSetShader(s.psInst, nullptr, 0);
  for (int k = 0; k < kTrees; ++k) {
    c->VSSetShaderResources(0, 1, &s.instSrv[k]);
    const UINT aoff = static_cast<UINT>(k * 32);
    bool skip = false;
    if (hooked) skip = fbcr::Pre(c, 0, s.args, aoff, view);
    if (!skip) c->DrawIndexedInstancedIndirect(s.args, aoff);
    if (hooked) fbcr::Post(c, 0, s.args, aoff, view);
    if (k == rebindAfter) c->OMSetRenderTargets(6, t.rtv, t.dsv);  // DCS's own targets again (same views)
    if (k == eclAfter && hooked) fbcr::BeforeEcl(c);
    if (k == 2) {  // a non-covered draw inside the block (ground again: into slice 0 as DCS's own)
      c->OMSetDepthStencilState(s.dss, 0x10);
      c->IASetVertexBuffers(0, 1, &s.vbGround, &stride, &off);
      c->IASetIndexBuffer(s.ibGround, DXGI_FORMAT_R16_UINT, 0);
      c->VSSetShader(s.vsGround, nullptr, 0);
      c->PSSetShader(s.psGround, nullptr, 0);
      c->DrawIndexed(6, 0, 0);
      c->OMSetDepthStencilState(s.dss, 0x30);
      c->IASetVertexBuffers(0, 1, &s.vbTree, &stride, &off);
      c->IASetIndexBuffer(s.ibTree, DXGI_FORMAT_R16_UINT, 0);
      c->VSSetShader(s.vsInst, nullptr, 0);
      c->PSSetShader(s.psInst, nullptr, 0);
    }
  }
  // DCS's pass end: its default frame buffer (popFrameBuffer) before our boundary callback
  c->OMSetRenderTargets(1, &s.dummyRtv, nullptr);
}
void Clear(ID3D11DeviceContext* c, Targets& t) {
  const float z[4] = {};
  for (auto* r : t.rtv) c->ClearRenderTargetView(r, z);
  c->ClearDepthStencilView(t.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
}
// Readback of a whole target set (5 slices R8G8, MV R16G16, depth + stencil) as bytes.
struct Img {
  std::vector<uint8_t> gb[5], mv, dep;
  UINT w = 0, h = 0;
};
bool Read(ID3D11Device* d, ID3D11DeviceContext* c, Targets& t, Img& img) {
  img.w = t.w, img.h = t.h;
  auto rd = [&](ID3D11Texture2D* tex, UINT sub, UINT bpp, std::vector<uint8_t>& out) {
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    td.ArraySize = 1, td.BindFlags = 0, td.Usage = D3D11_USAGE_STAGING, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ, td.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(d->CreateTexture2D(&td, nullptr, &st))) return false;
    c->CopySubresourceRegion(st, 0, 0, 0, 0, tex, sub, nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(c->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
      st->Release();
      return false;
    }
    out.resize(size_t(t.w) * t.h * bpp);
    for (UINT y = 0; y < t.h; ++y) memcpy(&out[size_t(y) * t.w * bpp], static_cast<uint8_t*>(m.pData) + y * m.RowPitch, t.w * bpp);
    c->Unmap(st, 0);
    st->Release();
    return true;
  };
  bool ok = true;
  for (UINT k = 0; k < 5; ++k) ok = ok && rd(t.gb, k, 2, img.gb[k]);
  return ok && rd(t.mv, 0, 4, img.mv) && rd(t.dep, 0, 8, img.dep);
}
inline float Half(uint16_t h) {
  const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
  float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? INFINITY : std::ldexp(float(m | 1024), int(e) - 25);
  return s ? -v : v;
}
struct Cmp {
  uint64_t covered = 0, gbDiff = 0, depDiff = 0, depRel = 0, cover = 0, stenDiff = 0, mvBad = 0;
  double mvMax = 0, depMaxRel = 0;
};
Cmp Compare(const Img& a, const Img& b) {
  Cmp r;
  const size_t n = size_t(a.w) * a.h;
  for (size_t i = 0; i < n; ++i) {
    float da, db;
    memcpy(&da, &a.dep[i * 8], 4);
    memcpy(&db, &b.dep[i * 8], 4);
    if (da != 0 || db != 0) ++r.covered;
    if ((da == 0) != (db == 0)) ++r.cover;
    if (da != db) {
      ++r.depDiff;
      if (std::fabs(da - db) > 1e-4 * std::max(std::fabs(da), std::fabs(db))) ++r.depRel;
      r.depMaxRel = std::max(r.depMaxRel, double(std::fabs(da - db)) / std::max(std::fabs(da), std::fabs(db)));
    }
    if (a.dep[i * 8 + 4] != b.dep[i * 8 + 4]) ++r.stenDiff;
    bool g = false;
    for (int k = 0; k < 5; ++k) g = g || memcmp(&a.gb[k][i * 2], &b.gb[k][i * 2], 2) != 0;
    if (g) ++r.gbDiff;
    if (da == db && da != 0) {  // motion vectors where the same surface won
      uint16_t ha[2], hb[2];
      memcpy(ha, &a.mv[i * 4], 4);
      memcpy(hb, &b.mv[i * 4], 4);
      for (int k = 0; k < 2; ++k) {
        const double va = Half(ha[k]), vb = Half(hb[k]);
        const double dd = std::fabs(va - vb);
        r.mvMax = std::max(r.mvMax, dd);
        if (dd > 0.01 + 2e-3 * std::fabs(va)) ++r.mvBad;
      }
    }
  }
  return r;
}
bool Exact(const Img& a, const Img& b) {
  for (int k = 0; k < 5; ++k)
    if (a.gb[k] != b.gb[k]) return false;
  return a.mv == b.mv && a.dep == b.dep;
}

void RunPure() {
  // Census rows (R38 3.1): periphery L / focus L of one frame.
  Tc p, f;
  p.ok = f.ok = true;
  const float pr[4][4] = {{0.780744f, 0, 0.195804f, 0}, {0, 0.790642f, 0.001029f, 0}, {0, 0, 0, 0.02f}, {0, 0, 1, 0}};
  const float fr[4][4] = {{2.296307f, 0, 0.096306f, 0}, {0, 2.325419f, -0.00054f, 0}, {0, 0, 0, 0.02f}, {0, 0, 1, 0}};
  memcpy(p.r[16], pr, sizeof(pr));
  memcpy(f.r[16], fr, sizeof(fr));
  for (int r = 0; r < 4; ++r)
    for (int k = 0; k < 4; ++k) p.r[20 + r][k] = pr[r][k], f.r[20 + r][k] = fr[r][k];
  fbc::Affine a;
  Check(ProjShape(p) && ProjShape(f) && AffineOf(p, f, a) && std::fabs(a.ax - 2.9412) < 1e-3 && std::fabs(a.ay - 2.9412) < 1e-3,
        "fbcr: census projections have the expected shape; focus = affine of the periphery (x 2.9412)");
  double dev = 1;
  Check(SameEye(p, f, a, 1e-5, &dev) && dev < 1e-6, "fbcr: same eye (z/w rows, VP rows x/y = affine of the periphery's)");
  Tc g = f;
  g.r[23][2] = 1.001f;
  Check(!SameEye(p, g, a, 1e-5, &dev), "fbcr: a different w row is refused");
  g = f;
  g.r[38][0] = 0.07f;
  Check(!SameEye(p, g, a, 1e-5, &dev), "fbcr: a different camera position is refused");
  Tc bad = p;
  bad.r[16][1] = 0.01f;
  Check(!ProjShape(bad), "fbcr: a cross term in P is refused");
  // Jitter prediction: the same pixel jitter j in both views (census: 0.25 / 0.278 px steps in both).
  auto withJ = [](const Tc& base, double jx, double jy, double w, double h) {
    Tc t = base;
    t.r[16][2] = static_cast<float>(base.r[16][2] + 2 * jx / w);
    t.r[17][2] = static_cast<float>(base.r[17][2] + 2 * jy / h);
    return t;
  };
  Tc pPrev = withJ(p, 0.25, -0.1, 476, 468), fPrev = withJ(f, 0.25, -0.1, 1176, 1164);
  Tc pNow = withJ(p, -0.375, 0.41, 476, 468), fNow = withJ(f, -0.375, 0.41, 1176, 1164);
  Pred pd;
  const bool okp = PredictFocus(pPrev, fPrev, pNow, 476, 468, 1176, 1164, pd);
  Check(okp && PredMatches(pd, fNow, 2e-6, &dev), "fbcr: focus predicted from the previous frame + the periphery's jitter step (within 2e-6 NDC)");
  Tc fGaze = fNow;
  fGaze.r[16][2] += 0.01f;
  Check(!PredMatches(pd, fGaze, 2e-6, &dev), "fbcr: a gaze move (focus centre +0.01 NDC) is a prediction miss");
  Tc pFov = pNow;
  pFov.r[16][0] *= 1.01f;
  Check(!PredictFocus(pPrev, fPrev, pFov, 476, 468, 1176, 1164, pd), "fbcr: a periphery FOV change refuses the prediction");
  // Learning.
  StartLearn sl;
  Check(sl.start == -1, "fbcr: start unlearned at first");
  sl.BeginLearn();
  sl.Saw(3);
  sl.Saw(12);
  sl.EndLearn(40);
  Check(sl.start == 12, "fbcr: learned start = the last rebind / command list seen");
  sl.Abort(20);
  Check(sl.start == 21, "fbcr: an aborted block moves the start past it");
  sl.BeginLearn();
  sl.EndLearn(40);
  Check(sl.start == 0, "fbcr: a clean learning frame resets the start to the first draw");
  // Formats.
  Check(FamilyOf(DXGI_FORMAT_R8G8_UNORM).raw == DXGI_FORMAT_R8G8_UINT && IsTypeless(DXGI_FORMAT_R8G8_TYPELESS) &&
            !IsTypeless(DXGI_FORMAT_R8G8_UNORM) &&
            RawFmt(DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, false) == DXGI_FORMAT_R8G8_UINT &&
            RawFmt(DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT, true) == DXGI_FORMAT_R16G16_FLOAT &&
            RawFmt(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false) == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        "fbcr: raw formats (integer views of typeless families, float MV, a typed texture keeps its format)");
  {
    const DXGI_FORMAT raw[6] = {DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8_UINT,
                                DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R16G16_FLOAT};
    std::string e;
    ID3DBlob* b1 = fbc::CompileHlsl(std::string(kHlsl) + MergeHlsl(6, raw, 5), "PsMerge", "ps_5_0", &e);
    ID3DBlob* b2 = fbc::CompileHlsl(std::string(kHlsl) + MergeHlsl(6, raw, -1), "PsMerge", "ps_5_0", &e);
    ID3DBlob* b3 = fbc::CompileHlsl(kHlsl, "PsStencil", "ps_5_0", &e);
    if (!b1 || !b2 || !b3) printf("     (merge HLSL: %s)\n", e.c_str());
    Check(b1 && b2 && b3, "fbcr: merge (DCS's layout, with and without the MV fixup) and stencil-plane shaders compile");
    for (ID3DBlob* b : {b1, b2, b3})
      if (b) b->Release();
  }
  // Keys off by default.
  {
    Check(fbcr::g_mode.load() == 0 && fbcr::g_suiteProbe.load() == 0 && fbcr::g_suiteTiming.load() == 0 &&
              fbcr::g_suiteVerify.load() == 0 && !fbcr::g_installed.load(),
          "fbcr: FocusBroadcast and its suite keys off by default; nothing installed");
    fbcr::ReadConfig(L"Z:\\no\\such\\none.ini");
    Check(fbcr::g_mode.load() == 0 && fbcr::g_kinds.load() == 1, "fbcr: a missing ini reads FocusBroadcast=0, Kinds=1");
    Check(fbcr::g_startDelay.load() == 10 && fbc::g_readyGate == &fbcr::ReadyGate && fbc::g_readyGate(),
          "fbcr: start gate 10 s by default; the R38 readiness check is not held at FocusBroadcast=0");
  }
  // Start gate (session 6 defect): a run of ticks with >= 15 frames per second; a stall (loading) resets it.
  {
    int run = 0;
    for (int i = 0; i < 10; ++i) run = fbcr::SteadyStep(run, 60, 1000);
    const int full = run;
    run = fbcr::SteadyStep(run, 2, 1000);
    const int stalled = run;
    run = fbcr::SteadyStep(fbcr::SteadyStep(run, 15, 1000), 30, 2100);
    Check(full == 10 && stalled == 0 && run == 0 && fbcr::SteadyStep(4, 0, 0) == 4,
          "fbcr: steady-frame run counts ticks at >= 15 fps, resets on a stall or 14 fps, ignores a zero interval");
    fbcr::g_mode = 2;
    const bool held = !fbcr::ReadyGate();
    fbcr::g_started = true;
    const bool open = fbcr::ReadyGate();
    fbcr::g_started = false;
    fbcr::g_mode = 0;
    Check(held && open, "fbcr: FocusBroadcast=2 holds the readiness check until frames flowed once");
  }
}

void RunArchive() {
  // An instancer-like VS (mainBuffer named as DCS's, 64-byte stride; a wind buffer for the SpeedTree class) inside
  // junk bytes, twice, plus an unrelated VS: two distinct blobs found, classes right, duplicates dropped.
  const char* tree = R"(
struct I { float4 a, b, c, d; };
StructuredBuffer<I> instancer2_inputMainBuffer : register(t4);
StructuredBuffer<float4> instancer2_airflowBuffer : register(t5);
float4 main(uint v : SV_VertexID, uint i : SV_InstanceID, out float2 uv : TEXCOORD0) : SV_Position {
  I x = instancer2_inputMainBuffer[i]; uv = x.b.xy + instancer2_airflowBuffer[v].xy; return x.a + float4(v, 0, 0, 1); })";
  const char* house = R"(
struct I { float4 a, b, c, d; };
StructuredBuffer<I> instancer2_inputMainBuffer : register(t3);
float4 main(uint i : SV_InstanceID, out float3 n : TEXCOORD0) : SV_Position { I x = instancer2_inputMainBuffer[i]; n = x.c.xyz; return x.a; })";
  const char* other = R"(float4 main(float4 p : POSITION) : SV_Position { return p; })";
  std::string e;
  ID3DBlob* a = fbc::CompileHlsl(tree, "main", "vs_5_0", &e);
  ID3DBlob* b = fbc::CompileHlsl(house, "main", "vs_5_0", &e);
  ID3DBlob* o = fbc::CompileHlsl(other, "main", "vs_5_0", &e);
  if (!a || !b || !o) {
    Check(false, "fbcr: archive test shaders compile");
    return;
  }
  std::vector<uint8_t> f(100, 0x5a);
  auto add = [&](ID3DBlob* x) {
    const uint8_t* p = static_cast<const uint8_t*>(x->GetBufferPointer());
    f.insert(f.end(), p, p + x->GetBufferSize());
    f.insert(f.end(), 37, 0x11);
  };
  add(a), add(o), add(b), add(a);
  f.insert(f.end(), {'D', 'X', 'B', 'C', 1, 2, 3});  // a truncated header at the end
  std::set<std::string> seen;
  std::vector<ArcBlob> out;
  ScanBlobs(f, seen, out);
  const bool two = out.size() == 2;
  Check(two && out[0].cat == 1 && out[1].cat == 2 && vsig::DeclaresMain(out[0].b, 4) && vsig::DeclaresMain(out[1].b, 3),
        "fbcr: archive scan finds the instancer VS (SpeedTree / other by its resources), skips others and duplicates");
  // Files in a folder (*.meta2 and *.dxbc).
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring dir = std::wstring(tmp) + L"fbcr_arc_test";
  CreateDirectoryW(dir.c_str(), nullptr);
  FILE* fp = nullptr;
  _wfopen_s(&fp, (dir + L"\\x.meta2").c_str(), L"wb");
  if (fp) fwrite(f.data(), 1, f.size(), fp), fclose(fp);
  _wfopen_s(&fp, (dir + L"\\y.dxbc").c_str(), L"wb");
  if (fp) fwrite(b->GetBufferPointer(), 1, b->GetBufferSize(), fp), fclose(fp);
  size_t nf = 0;
  const std::vector<ArcBlob> l = LoadArchiveFrom(dir, &nf);
  Check(nf == 2 && l.size() == 2, "fbcr: archive folder: *.meta2 and *.dxbc read, distinct blobs only");
  DeleteFileW((dir + L"\\x.meta2").c_str());
  DeleteFileW((dir + L"\\y.dxbc").c_str());
  RemoveDirectoryW(dir.c_str());
  a->Release(), b->Release(), o->Release();
}

// The end-to-end GPU check (NVIDIA hardware: FastGS).
void RunGpu() {
  nvfgs::Api api;
  ID3D11Device* d = nullptr;
  ID3D11DeviceContext* c = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (!nvfgs::Load(api) || FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                                    D3D11_SDK_VERSION, &d, nullptr, &c))) {
    printf("     (fbcr: no NVIDIA driver / hardware device: the end-to-end broadcast check is skipped)\n");
    return;
  }
  Scene s;
  Targets refP, refF, bP, bF, mfd;
  bool ok = MakeScene(d, c, s) && MakeTargets(d, kWP, kHP, refP) && MakeTargets(d, kWF, kHF, refF) &&
            MakeTargets(d, 512, 512, mfd) &&
            MakeTargets(d, kWP, kHP, bP) && MakeTargets(d, kWF, kHF, bF);
  Check(ok, "fbcr gpu: scene and targets");
  if (!ok) {
    s.Release();
    c->Release(), d->Release();
    return;
  }
  // fbcr on this device: pifilt's device / context, the TerrainContext buffer, the render thread.
  ID3D11Device* savedDev = pifilt::g_dev;
  ID3D11DeviceContext* savedCtx = pifilt::g_ctx;
  if (savedDev) savedDev->AddRef();
  if (savedCtx) savedCtx->AddRef();
  pifilt::SetDeviceForTest(d);
  ID3D11Buffer* savedTc = pifilt::g_d.tc;
  pifilt::g_d.tc = s.tc;
  const DWORD savedTid = ptiming::g_topTid.load();
  ptiming::g_topTid = GetCurrentThreadId();
  // The tree VS object: identified (its signature from its own bytecode), FastGS created.
  Obj* obj = new Obj;
  obj->vs = s.vsInst;
  obj->vs->AddRef();
  std::vector<fbc::SigElem> sig;
  fbc::ParseOutputSignature(s.vsInstCode.data(), s.vsInstCode.size(), sig);
  int st = 0;
  obj->gs = fbc::CreateFastGs(d, sig, 3, true, &st);
  obj->cat = 1;
  obj->elems = static_cast<int>(sig.size());
  obj->st = obj->gs ? kReady : kGsFail;
  {
    std::lock_guard<std::mutex> lock(g_objMu);
    g_objs[s.vsInst] = obj;
  }
  Check(obj->gs != nullptr, "fbcr gpu: FastGS for the test VS signature (mask 0b11, RT offset)");
  fbcr::g_on = true;
  fbcr::g_kinds = 1;
  fbcr::g_verify = true;  // same-frame compares on every 4th frame (stock key draws redrawn into a copy)
  fbcr::g_statsOn = true;  // [Suite] FocusBcastTiming=2 diagnostics: pass statistics and our own GPU timestamps
  // Frames: eye 0 only (views 0 and 2). Jitter: the same pixel jitter in both views; a fixed gaze until frame 7.
  const double hal[10][2] = {{0, -0.1667}, {-0.25, 0.1667}, {0.25, -0.3889}, {-0.375, -0.0556}, {0.125, 0.2778},
                             {-0.125, -0.2778}, {0.375, 0.0556}, {-0.4375, 0.3889}, {0.0625, -0.4815}, {-0.1875, 0.1852}};
  const double pcx = 0.1958, pcy = 0.0010288, fcx0 = 0.096306, fcy0 = -0.00054;
  M4 PpPrev, PfPrev, Vprev = View(0.0, 0.03, 0, 0, 0);
  int frames = 0;
  int periphExact = 0, periphFrames = 0;
  std::vector<Cmp> focusCmp;
  std::vector<bool> merged;
  uint64_t m0 = g_c.merges.load(), skip0 = g_c.skipped.load();
  for (int fr = 0; fr < 10; ++fr) {
    const double jx = hal[fr][0], jy = hal[fr][1];
    const double gaze = fr >= 5 ? 0.02 : 0.0;  // frame 5: the gaze moves (prediction miss expected)
    const M4 Pp = Proj(0.780744, pcx + 2 * jx / kWP, 0.790642, pcy + 2 * jy / kHP);
    const M4 Pf = Proj(2.296307, fcx0 + gaze + 2 * jx / kWF, 2.325419, fcy0 + 2 * jy / kHF);
    const M4 V = View(0.002 * fr, 0.03, 0.01 * fr, 0, 0.05 * fr);  // the camera moves (motion vectors non-zero)
    if (fr == 0) PpPrev = Pp, PfPrev = Pf, Vprev = V;
    // stock reference
    Clear(c, refP), Clear(c, refF);
    Pass(s, refP, Pp, V, PpPrev, Vprev, false, 0);
    Pass(s, refF, Pf, V, PfPrev, Vprev, false, 2);
    // the broadcast path: fbcr's boundaries and callbacks
    Clear(c, bP), Clear(c, bF);
    fbcr::OnBoundary(nullptr, true, plod::kHeight, -1, 0);
    fbcr::OnBoundary(nullptr, false, plod::kHeight, -1, 0);
    // odd frames: a sensor view (an MFD camera: another position, wide FOV, 512x512) renders first and shifts every
    // LightsBuffersPass ordinal by one, as in F1 (R53 s3/s4); it must be left alone
    const int sh = (fr & 1) ? 1 : 0;
    if (sh) {
      const M4 Pm = Proj(0.9, 0.0, 0.9, 0.0), Vm = View(0.4, -0.2, 3.0, 1.0, -2.0);
      fbcr::OnBoundary(nullptr, true, plod::kLights, 0, 0);
      fbcr::OnBoundary(nullptr, false, plod::kLights, 0, 0);
      fbcr::OnBoundary(nullptr, true, plod::kGBuffer, 0, 1);
      Clear(c, mfd);
      Pass(s, mfd, Pm, Vm, Pm, Vm, true, 0);
      fbcr::OnBoundary(nullptr, false, plod::kGBuffer, 0, 1);
    }
    fbcr::OnBoundary(nullptr, true, plod::kLights, 0 + sh, 0);
    fbcr::OnBoundary(nullptr, false, plod::kLights, 0 + sh, 0);
    fbcr::OnBoundary(nullptr, true, plod::kGBuffer, 0 + sh, 1);
    // frame 6: a command list after bucket 3 (the block closes exactly, reopens; the start moves to 4); frame 7: DCS
    // rebinds its targets after bucket 4 (abort merge at bucket 5; the start moves to 6: frames 8-9 have no block)
    const int rebind = fr == 7 ? 4 : -1;
    const int ecl = fr == 6 ? 3 : -1;
    Pass(s, bP, Pp, V, PpPrev, Vprev, true, 0 + sh, rebind, ecl);
    fbcr::OnBoundary(nullptr, false, plod::kGBuffer, 0 + sh, 1);
    fbcr::OnBoundary(nullptr, true, plod::kLights, 2 + sh, 0);
    fbcr::OnBoundary(nullptr, false, plod::kLights, 2 + sh, 0);
    fbcr::OnBoundary(nullptr, true, plod::kGBuffer, 2 + sh, 1);
    const uint64_t mb = g_c.merges.load();
    Pass(s, bF, Pf, V, PfPrev, Vprev, true, 2 + sh);
    fbcr::OnBoundary(nullptr, false, plod::kGBuffer, 2 + sh, 1);
    merged.push_back(g_c.merges.load() > mb);
    Img a, b;
    if (Read(d, c, refP, a) && Read(d, c, bP, b)) {
      ++periphFrames;
      // frame 7's abort merge is NEAR (depth-tested composite), every other frame bit-identical
      if (Exact(a, b) || fr == 7) ++periphExact;
      if (fr == 7) {
        const Cmp q = Compare(a, b);
        printf("     (fbcr gpu: frame 7 abort: periphery G-buffer px differing %llu of %llu covered, depth %llu)\n",
               static_cast<unsigned long long>(q.gbDiff), static_cast<unsigned long long>(q.covered),
               static_cast<unsigned long long>(q.depDiff));
      }
    }
    Img fa, fb;
    if (Read(d, c, refF, fa) && Read(d, c, bF, fb)) focusCmp.push_back(Compare(fa, fb));
    PpPrev = Pp, PfPrev = Pf, Vprev = V;
    ++frames;
  }
  Check(periphFrames == 10 && periphExact == 10, "fbcr gpu: periphery bit-identical to stock in every frame (block, copy-in/out, ECL close)");
  // Frames 0-1 warm up (main pass, focus keys), frame 1 learns, broadcast from frame 2; frame 5 the gaze moved
  // (prediction miss); frames 8-9 none (the start past every draw after frame 7's abort).
  const bool mergedOk = !merged[0] && !merged[1] && merged[2] && merged[3] && merged[4] && !merged[5] && merged[6] &&
                        merged[7] && !merged[8] && !merged[9];
  fbcr::g_verify = false;
  c->Flush();
  for (int i = 0; i < 50; ++i) {
    Sleep(5);
    fbcr::VerPoll(c);
  }
  fbcr::g_statsOn = false;
  for (int i = 0; i < 20; ++i) {
    Sleep(5);
    fbcr::TsPoll(c);
    fbcr::StatPoll(c);
  }
  const bool tsOk = fbcr::g_tsFrames > 0 && fbcr::g_tsSum[fbcr::kTsIn] > 0 && fbcr::g_tsSum[fbcr::kTsMerge] > 0;
  const bool stOk = fbcr::g_vstat[0].n > 0 && fbcr::g_vstat[0].psi > 0;
  fbcr::TsLog("test");
  fbcr::StatLog("test");
  Check(tsOk && stOk, "fbcr gpu: timing diagnostics (own GPU timestamps, main-pass pipeline statistics) read back");
  const bool verOk = fbcr::LogVerify();
  Check(verOk && g_vt.frames > 0 && g_vt.drawn > 0,
        "fbcr gpu: [Suite] FocusBcastVerify machinery: same-frame compares read back and PASS the gates");
  fbcr::LogCounters("test");
  printf("     (fbcr gpu: merged per frame:");
  for (bool m : merged) printf(" %d", m ? 1 : 0);
  printf("; merges %llu, focus draws not issued %llu, aborts %llu, ECL closes %llu, pred miss %llu)\n",
         static_cast<unsigned long long>(g_c.merges.load() - m0), static_cast<unsigned long long>(g_c.skipped.load() - skip0),
         static_cast<unsigned long long>(g_c.blockAborts.load()), static_cast<unsigned long long>(g_c.eclCloses.load()),
         static_cast<unsigned long long>(g_c.predMiss.load()));
  Check(mergedOk, "fbcr gpu: merged in frames 2-4 and 6-7, not before learning, at the gaze jump (5) or past the learned start (8-9)");
  double worstG = 0, worstD = 0, worstMv = 0;
  uint64_t cov = 0, mvBad = 0, coverD = 0, stenD = 0;
  for (int fr : {2, 3, 4, 6, 7}) {
    const Cmp& q = focusCmp[fr];
    worstG = std::max(worstG, double(q.gbDiff) / std::max<uint64_t>(1, q.covered));
    worstD = std::max(worstD, double(q.depRel) / std::max<uint64_t>(1, q.covered));
    worstMv = std::max(worstMv, q.mvMax);
    cov += q.covered, mvBad += q.mvBad, coverD += q.cover, stenD += q.stenDiff;
    printf("     (fbcr gpu: frame %d focus: covered %llu, G-buffer px differ %llu (%.3f %%), depth differ %llu (rel >1e-4 "
           "%llu, max rel %.2g), coverage %llu, stencil %llu, MV max %.5f px (bad %llu))\n",
           fr, static_cast<unsigned long long>(q.covered), static_cast<unsigned long long>(q.gbDiff),
           100.0 * q.gbDiff / std::max<uint64_t>(1, q.covered), static_cast<unsigned long long>(q.depDiff),
           static_cast<unsigned long long>(q.depRel), q.depMaxRel, static_cast<unsigned long long>(q.cover),
           static_cast<unsigned long long>(q.stenDiff), q.mvMax, static_cast<unsigned long long>(q.mvBad));
  }
  Check(cov > 100000 && worstG < 0.01 && worstD < 0.002 && coverD < cov / 500 && stenD < cov / 500,
        "fbcr gpu: merged focus = stock within float rounding (G-buffer < 1 %, depth rel > 1e-4 < 0.2 %, coverage / "
        "stencil < 0.2 %)");
  Check(mvBad < cov / 1000 && worstMv < 0.05, "fbcr gpu: focus motion vectors fixed up (moving camera, jitter)");
  const bool stockAfterMiss = focusCmp[5].gbDiff == 0 && focusCmp[5].depDiff == 0 && focusCmp[5].mvMax == 0 &&
                              focusCmp[8].gbDiff == 0 && focusCmp[8].depDiff == 0;
  Check(stockAfterMiss && g_c.predMiss.load() > 0, "fbcr gpu: gaze jump = prediction miss, the focus rendered stock (bit-identical)");
  Check(g_c.eclCloses.load() > 0 && g_c.blockAborts.load() > 0, "fbcr gpu: the command-list close and the rebind abort ran");
  Check(g_eye[0].sl.start == 6, "fbcr gpu: the command list and the abort moved the learned start past them (6)");
  // cleanup
  fbcr::g_on = false;
  fbcr::Shutdown();
  pifilt::g_d.tc = savedTc;
  ptiming::g_topTid = savedTid;
  pifilt::SetDeviceForTest(nullptr);
  pifilt::g_dev = savedDev, pifilt::g_ctx = savedCtx;
  for (Targets* t : {&refP, &refF, &bP, &bF, &mfd}) t->Release();
  s.Release();
  c->Release();
  d->Release();
}

// Identification through fbcr's own steps (worker prepare, render-thread tests, poll, tally, FastGS): DCS's object
// runs vsigtest::kVs; the archive holds it, a shorter prefix variant (its last output dropped) and a decoy.
void RunIdent() {
  nvfgs::Api api;
  ID3D11Device* d = nullptr;
  ID3D11DeviceContext* c = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (!nvfgs::Load(api) || FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                                    D3D11_SDK_VERSION, &d, nullptr, &c))) {
    printf("     (fbcr: no NVIDIA driver / hardware device: identification check skipped)\n");
    return;
  }
  const char kPrefix[] = R"(
struct Inst { uint4 a, b, c, d; };
StructuredBuffer<Inst> mainB : register(t3);
Buffer<uint> idx : register(t4);
struct O { float4 pos : SV_Position; float4 n : NORMAL; float2 uv : TEXCOORD0; float3 w : TEXCOORD1; };
O main(uint v : SV_VertexID, uint i : SV_InstanceID) {
  Inst r = mainB[idx[i]];
  O o;
  o.pos = float4(asfloat(r.a.zw), asfloat(r.b.x), 1) + v;
  o.n = asfloat(r.c);
  o.uv = asfloat(r.d.xy);
  o.w = asfloat(r.b.yzw);
  return o;
})";
  const char kDecoy[] = R"(
struct Inst { uint4 a, b, c, d; };
StructuredBuffer<Inst> mainB : register(t3);
Buffer<uint> idx : register(t4);
struct O { float4 pos : SV_Position; float4 n : NORMAL; float2 uv : TEXCOORD0; float3 w : TEXCOORD1; uint id : TEXCOORD2; };
O main(uint v : SV_VertexID, uint i : SV_InstanceID) {
  Inst r = mainB[idx[i]];
  O o;
  o.pos = float4(asfloat(r.a.zw), asfloat(r.b.x), 1) + v;
  o.n = asfloat(r.c) * 2;
  o.uv = asfloat(r.d.xy);
  o.w = asfloat(r.b.yzw);
  o.id = r.a.x;
  return o;
})";
  std::vector<uint8_t> vs, pre, dec;
  const bool built = vsigtest::CompileStage(vsigtest::kVs, "vs_5_0", vs) && vsigtest::CompileStage(kPrefix, "vs_5_0", pre) &&
                     vsigtest::CompileStage(kDecoy, "vs_5_0", dec);
  ID3D11Device* savedDev = pifilt::g_dev;
  ID3D11DeviceContext* savedCtx = pifilt::g_ctx;
  if (savedDev) savedDev->AddRef();
  if (savedCtx) savedCtx->AddRef();
  pifilt::SetDeviceForTest(d);
  std::vector<float> inst(64 * 16);
  for (size_t i = 0; i < inst.size(); ++i) inst[i] = 0.25f * float(i % 97) + 1.0f;
  std::vector<uint32_t> ix(64);
  for (uint32_t i = 0; i < 64; ++i) ix[i] = (i * 7) % 64;
  ID3D11Buffer* mb = piftest::MkBuf(d, 64 * 64, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 64, inst.data());
  ID3D11Buffer* ib = piftest::MkBuf(d, 64 * 4, D3D11_BIND_SHADER_RESOURCE, 0, 0, ix.data());
  ID3D11ShaderResourceView* sv[2] = {piftest::MkSrv(d, mb, 64, true), piftest::MkSrv(d, ib, 64, false)};
  ID3D11Buffer* savedMain = pifilt::g_d.mainB;
  pifilt::g_d.mainB = mb;
  ID3D11VertexShader* obj = nullptr;
  if (built) d->CreateVertexShader(vs.data(), vs.size(), nullptr, &obj);
  bool ok = built && obj;
  if (ok) {
    c->VSSetShaderResources(3, 2, sv);
    c->VSSetShader(obj, nullptr, 0);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_arc.clear();
    for (auto* code : {&vs, &pre, &dec}) {
      ArcBlob a;
      vsig::Analyze(code->data(), code->size(), a.b);
      a.code = *code;
      a.cat = 2;
      g_arc.push_back(a);
    }
    g_arcState = 2;
    Obj* o = ObjOf(c, obj);
    ok = o && o->mainSlot == 3;
    if (ok) PrepareObj(*o);
    ok = ok && o->st.load() == kTesting && o->tests.size() == 3;
    for (int frame = 0; ok && frame < 400 && o->st.load() == kTesting; ++frame) {
      g_testsThisFrame = 0;
      for (int k = 0; k < 4; ++k) TestStep(c, *o);
      c->Flush();
      Sleep(2);
      PollTests(c);
    }
    if (ok && o->st.load() == kTallied) FinishObj(*o);
    char what[300];
    sprintf_s(what, "fbcr: identification through the worker / render-thread steps: state %d (4 = ready), %d matches (%d "
                    "shorter prefix), %d elements, FastGS %s, checksum of the object's own program %d",
              o ? o->st.load() : -1, o ? o->matches : -1, o ? o->prefixes : -1, o ? o->elems : -1,
              o && o->gs ? "created" : "none",
              o && o->sum == vsig::SumHex(vs.data() + 4) ? 1 : 0);
    Check(ok && o->st.load() == kReady && o->gs && o->elems == 5 && o->matches == 2 && o->prefixes == 1 &&
              o->sum == vsig::SumHex(vs.data() + 4),
          what);
    {
      std::lock_guard<std::mutex> lock(g_objMu);
      g_objs.erase(obj);
    }
    if (o) {
      Rel(o->gs);
      o->vs->Release();
      delete o;
    }
  } else {
    Check(false, "fbcr: identification test setup");
  }
  g_arc.clear();
  g_arcState = 0;
  pifilt::g_d.mainB = savedMain;
  Rel(obj);
  for (auto& s : sv) Rel(s);
  Rel(mb), Rel(ib);
  pifilt::SetDeviceForTest(nullptr);
  pifilt::g_dev = savedDev, pifilt::g_ctx = savedCtx;
  c->Release();
  d->Release();
}

void Run() {
  RunPure();
  RunArchive();
  RunIdent();
  RunGpu();
}

}  // namespace fbcrtest
