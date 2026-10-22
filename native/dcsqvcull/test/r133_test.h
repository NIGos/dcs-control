// R133: the G-buffer recorder's motion-vector difference at the burst onset (R131 Part 2), reproduced on a small
// rig with DCS's motion-vector inputs and checked against the verify's bracketed control (gb_rec.h VerifyPass,
// gb_batch.h gbverify).
// The rig: DCS's model VS formula (vertex_shader.hlsl: prevFrameProjPos = (pos * M(pos)) * M(prevPos) *
// gPrevFrameViewProj) with sbPositions (pos[4], prevPos[4]) in a DYNAMIC structured buffer written with
// Map(WRITE_DISCARD) on the immediate context (DCS's slot-8 upload), cPerView (gViewProj, gPrevFrameViewProj,
// gTargetDims) in a DEFAULT buffer written with UpdateSubresource (DCS's binder), and GBuffer.hlsl's calcMotionVector
// into an R16G16_FLOAT target (RT5) plus the depth (R32_FLOAT). The recorded draw is a deferred-context command list
// that binds the page view and our copy of b7, which the exec entry fills with CopyResource from DCS's b7 (ExecSegment).
// Checks:
//  1. Freshness: the "previous frame" data advances between the recording and the execution (both the page and
//     b7, DCS writing them after the job recorded). The list's output equals a stock draw made at the same point,
//     bit for bit, and differs from one made before the change (so the check is not vacuous): RT5 differs, depth
//     does not, R131's signature. A list that snapshots b7 at record time (an UpdateSubresource on the deferred
//     context) equals the earlier stock run instead: a stale replay looks OLDER than every stock run, never newer.
//  2. The verify's classification over the four runs A, C1 (before the change), B (recorded, after it), C2 (stock,
//     after B): the R129 rule (no C2) calls all of B's RT5 differences unexplained, exactly R131's onset; with C2 they
//     are "equal to the later stock control only" and nothing is unexplained. A texel that matches no stock run
//     still counts. The mask includes the A-vs-C2 texels.
//  3. The dump (GBufferRecVerifyDump): the four runs cropped, the header, the bytes.
// QV_R95_HW=1 (the R95 HW suite): check 1 also on the hardware device (driver command lists, DCS's GPU).
#pragma once

namespace r133test {

template <typename T>
void Rel(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

const char kHlsl[] = R"(
cbuffer cPerView : register(b7) { float4x4 gViewProj; float4x4 gPrevFrameViewProj; float4 gTargetDims; };
cbuffer def_uniforms : register(b2) { uint4 pso; };
struct PositionStruct { float3 pos[4]; float3 prevPos[4]; };
StructuredBuffer<PositionStruct> sbPositions : register(t3);
float4x4 convert_matrix(float3 p[4]) {
  float4x4 m = { p[0].x, p[0].y, p[0].z, 0, p[1].x, p[1].y, p[1].z, 0, p[2].x, p[2].y, p[2].z, 0,
                 p[3].x, p[3].y, p[3].z, 1 };
  return m;
}
struct VOut { float4 pos : SV_Position; float4 projPos : TEXCOORD0; float4 prevFrameProjPos : TEXCOORD7; };
VOut vs(uint id : SV_VertexID) {
  // Two triangles of a slanted quad, so depth and the motion vector vary over the target.
  const float2 c[6] = { float2(-1, -1), float2(-1, 1), float2(1, -1), float2(1, -1), float2(-1, 1), float2(1, 1) };
  const float2 q = c[id % 6];
  const float4 p = float4(q.x * 8.0, q.y * 6.0, 10.0 + q.x * 2.0 + q.y, 1);
  const float4x4 posMat = convert_matrix(sbPositions[pso.x].pos);
  const float4x4 prevPosMat = convert_matrix(sbPositions[pso.x].prevPos);
  VOut o;
  const float4 w = mul(p, posMat);
  o.projPos = o.pos = mul(w, gViewProj);
  o.prevFrameProjPos = mul(mul(w, prevPosMat), gPrevFrameViewProj);
  return o;
}
struct POut { float2 mv : SV_Target0; float depth : SV_Target1; };
POut ps(VOut i) {
  float2 v = (i.prevFrameProjPos.xy / i.prevFrameProjPos.w - i.projPos.xy / i.projPos.w) * 0.5 * gTargetDims.xy;
  POut o;
  o.mv = float2(v.x, -v.y);
  o.depth = i.projPos.z / i.projPos.w;
  return o;
}
)";

constexpr UINT kW = 61, kH = 47;

struct PerView {
  float vp[16], pvp[16], dims[4];
};
struct PosStruct {
  float pos[12], prev[12];
};

// Row-vector perspective (DCS's mul(v, M)): x' = (x - camX) * f / a, y' = y * f, w = z. Stored as HLSL's default
// column-major cbuffer packing (logical M[r][c] at f[c * 4 + r], as DCS's cPerView).
void Proj(float* m, float camX) {
  memset(m, 0, 16 * sizeof(float));
  const float f = 1.2f, a = static_cast<float>(kW) / kH, n = 0.5f, fa = 1000.0f;
  auto at = [&](int r, int c) -> float& { return m[c * 4 + r]; };
  at(0, 0) = f / a;
  at(1, 1) = f;
  at(2, 2) = fa / (fa - n);
  at(2, 3) = 1.0f;
  at(3, 2) = -n * fa / (fa - n);
  at(3, 0) = -camX * f / a;  // a camera moved along x by camX (translation folded in)
}
void Translation(float* p, float tx, float ty, float tz) {  // pos[4] rows: identity rotation, translation in row 3
  const float r[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, tx, ty, tz};
  memcpy(p, r, sizeof(r));
}

struct Rig {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  ID3D11DeviceContext* def = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11Buffer* sb = nullptr;  // sbPositions page (DYNAMIC, Map DISCARD)
  ID3D11ShaderResourceView* sbSrv = nullptr;
  ID3D11Buffer* b7Dcs = nullptr;   // DCS's cPerView (DEFAULT, UpdateSubresource)
  ID3D11Buffer* b7Ours = nullptr;  // the recorder's copy (filled at the exec entry)
  ID3D11Buffer* mat = nullptr;     // def_uniforms (posStructOffset)
  ID3D11RasterizerState* rs = nullptr;  // no culling (the quad's winding does not matter)
  ID3D11Texture2D* rt[2] = {};
  ID3D11RenderTargetView* rtv[2] = {};
  bool hw = false;
  ~Rig() {
    for (auto*& v : rtv) Rel(v);
    for (auto*& t : rt) Rel(t);
    Rel(rs), Rel(mat), Rel(b7Ours), Rel(b7Dcs), Rel(sbSrv), Rel(sb), Rel(ps), Rel(vs), Rel(def), Rel(imm), Rel(dev);
  }
  bool Create(D3D_DRIVER_TYPE type) {
    hw = type == D3D_DRIVER_TYPE_HARDWARE;
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, type, nullptr, 0, want, 2, D3D11_SDK_VERSION, &dev, nullptr, &imm)))
      return false;
    if (FAILED(dev->CreateDeferredContext(0, &def))) return false;
    srtest::Compiler c;
    if (!c.Load()) return false;
    ID3DBlob* b = c.Build(kHlsl, sizeof(kHlsl) - 1, "vs", "vs_5_0", nullptr);
    if (!b) return false;
    dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs);
    b->Release();
    if (!(b = c.Build(kHlsl, sizeof(kHlsl) - 1, "ps", "ps_5_0", nullptr))) return false;
    dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps);
    b->Release();
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 4 * sizeof(PosStruct);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(PosStruct);
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &sb)) || FAILED(dev->CreateShaderResourceView(sb, nullptr, &sbSrv)))
      return false;
    D3D11_BUFFER_DESC cd = {};
    cd.ByteWidth = sizeof(PerView);
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(dev->CreateBuffer(&cd, nullptr, &b7Dcs)) || FAILED(dev->CreateBuffer(&cd, nullptr, &b7Ours))) return false;
    const uint32_t pso[4] = {2, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {pso, 0, 0};
    cd.ByteWidth = 16;
    if (FAILED(dev->CreateBuffer(&cd, &sd, &mat))) return false;
    const DXGI_FORMAT fmt[2] = {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT};
    for (int i = 0; i < 2; ++i) {
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = kW, td.Height = kH, td.MipLevels = 1, td.ArraySize = 1, td.Format = fmt[i];
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = D3D11_BIND_RENDER_TARGET;
      if (FAILED(dev->CreateTexture2D(&td, nullptr, &rt[i])) || FAILED(dev->CreateRenderTargetView(rt[i], nullptr, &rtv[i])))
        return false;
    }
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, &rs))) return false;
    return vs && ps;
  }
  // DCS's upload of frame data: the page (Map DISCARD, all elements) and the binder's b7.
  void Upload(float camX, float camPrevX, float objX, float objPrevX) {
    PosStruct p[4] = {};
    for (PosStruct& e : p) Translation(e.pos, 0, 0, 0), Translation(e.prev, 0, 0, 0);
    Translation(p[2].pos, objX, 0.25f, 0);
    Translation(p[2].prev, objPrevX, 0.25f, 0);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(imm->Map(sb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
      memcpy(m.pData, p, sizeof(p));
      imm->Unmap(sb, 0);
    }
    PerView v = {};
    Proj(v.vp, camX);
    Proj(v.pvp, camPrevX);
    v.dims[0] = static_cast<float>(kW), v.dims[1] = static_cast<float>(kH);
    imm->UpdateSubresource(b7Dcs, 0, nullptr, &v, 0, 0);
  }
  void Bind(ID3D11DeviceContext* c, ID3D11Buffer* b7) {
    c->OMSetRenderTargets(2, rtv, nullptr);
    D3D11_VIEWPORT vp = {0, 0, static_cast<float>(kW), static_cast<float>(kH), 0, 1};
    c->RSSetViewports(1, &vp);
    c->RSSetState(rs);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->IASetInputLayout(nullptr);
    c->VSSetShader(vs, nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);
    c->VSSetShaderResources(3, 1, &sbSrv);
    ID3D11Buffer* vcb[8] = {nullptr, nullptr, mat, nullptr, nullptr, nullptr, nullptr, b7};
    c->VSSetConstantBuffers(0, 8, vcb);
    c->PSSetConstantBuffers(7, 1, &b7);
  }
  void Clear() {
    const float z[4] = {0, 0, 0, 0};
    for (auto* v : rtv) imm->ClearRenderTargetView(v, z);
  }
  void Stock() {  // DCS's own draw, on the immediate context with its b7
    Bind(imm, b7Dcs);
    imm->Draw(6, 0);
  }
  // The recorder's list. stale: the job writes b7's bytes of record time into ours inside the list (what a replay
  // that snapshots the previous-frame data would do).
  ID3D11CommandList* Record(bool stale) {
    if (stale) {
      PerView v = {};
      ID3D11Buffer* tmp = nullptr;
      D3D11_BUFFER_DESC sd = {};
      sd.ByteWidth = sizeof(PerView);
      sd.Usage = D3D11_USAGE_STAGING;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &tmp))) {
        imm->CopyResource(tmp, b7Dcs);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(imm->Map(tmp, 0, D3D11_MAP_READ, 0, &m))) {
          memcpy(&v, m.pData, sizeof(v));
          imm->Unmap(tmp, 0);
        }
        tmp->Release();
      }
      def->UpdateSubresource(b7Ours, 0, nullptr, &v, 0, 0);
    }
    Bind(def, b7Ours);
    def->Draw(6, 0);
    ID3D11CommandList* cl = nullptr;
    def->FinishCommandList(FALSE, &cl);
    return cl;
  }
  void Replay(ID3D11CommandList* cl, bool copyB7) {  // the exec entry: DCS's bound b7 into ours, then the list
    if (copyB7) imm->CopyResource(b7Ours, b7Dcs);
    imm->ExecuteCommandList(cl, FALSE);
  }
  // Both targets read back into one byte vector (RT5 then depth).
  std::vector<uint8_t> Read() {
    std::vector<uint8_t> out;
    for (int i = 0; i < 2; ++i) {
      D3D11_TEXTURE2D_DESC td;
      rt[i]->GetDesc(&td);
      td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ID3D11Texture2D* s = nullptr;
      if (FAILED(dev->CreateTexture2D(&td, nullptr, &s))) return {};
      imm->CopyResource(s, rt[i]);
      D3D11_MAPPED_SUBRESOURCE m;
      if (SUCCEEDED(imm->Map(s, 0, D3D11_MAP_READ, 0, &m))) {
        const UINT bpt = i == 0 ? 4 : 4;
        for (UINT y = 0; y < kH; ++y) {
          const auto* r = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
          out.insert(out.end(), r, r + kW * bpt);
        }
        imm->Unmap(s, 0);
      }
      s->Release();
    }
    return out;
  }
};

// Texels (4 bytes each) that differ in RT5 (first half) and in depth (second half).
void Diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int* mv, int* depth) {
  *mv = *depth = -1;
  if (a.size() != b.size() || a.size() != 2u * kW * kH * 4) return;
  *mv = *depth = 0;
  for (size_t t = 0; t < 2u * kW * kH; ++t)
    if (memcmp(&a[t * 4], &b[t * 4], 4)) ++(t < static_cast<size_t>(kW) * kH ? *mv : *depth);
}

// The R131 frame: frame N's data, the job records, stock A; then DCS advances the previous-frame data (the camera
// and the object kept, only the "previous" terms move by a tiny step); recorded B; stock C2.
void Freshness(D3D_DRIVER_TYPE type, const char* tag) {
  Rig r;
  if (!r.Create(type)) {
    if (type == D3D_DRIVER_TYPE_HARDWARE)
      Check(false, "R133 rig: a hardware device with deferred contexts and the compiler (QV_R95_HW=1)");
    else
      QvSkip("R133 rig (%s): no device, deferred context or compiler\n", tag);
    return;
  }
  D3D11_FEATURE_DATA_THREADING th = {};
  r.dev->CheckFeatureSupport(D3D11_FEATURE_THREADING, &th, sizeof(th));
  const float cam = 0.02f, obj = 0.3f;
  r.Upload(cam, cam - 1.0e-3f, obj, obj - 2.0e-3f);  // frame N: previous terms one small step back
  ID3D11CommandList* cl = r.Record(false);
  r.Clear(), r.Stock();
  const std::vector<uint8_t> a = r.Read();
  r.Clear(), r.Stock();
  const std::vector<uint8_t> c1 = r.Read();
  // DCS advances the previous-frame terms (page Map DISCARD + binder upload), current terms unchanged.
  r.Upload(cam, cam - 1.0e-3f + 2.0e-6f, obj, obj - 2.0e-3f + 3.0e-6f);
  r.Clear(), r.Replay(cl, true);
  const std::vector<uint8_t> b = r.Read();
  r.Clear(), r.Stock();
  const std::vector<uint8_t> c2 = r.Read();
  // The stale model, on its own frame: b7 snapshotted at record time, then only b7's previous-frame matrix moves.
  r.Upload(cam, cam - 1.0e-3f, obj, obj - 2.0e-3f);
  ID3D11CommandList* stale = r.Record(true);
  r.Clear(), r.Stock();
  const std::vector<uint8_t> sa = r.Read();
  r.Upload(cam, cam - 1.0e-3f + 2.0e-6f, obj, obj - 2.0e-3f);
  r.Clear(), r.Replay(stale, false);
  const std::vector<uint8_t> bs = r.Read();
  r.Clear(), r.Stock();
  const std::vector<uint8_t> sc2 = r.Read();
  int mvAC1, dAC1, mvBC2, dBC2, mvAB, dAB, mvSA, dSA, mvSC2, dSC2;
  Diff(a, c1, &mvAC1, &dAC1);
  Diff(b, c2, &mvBC2, &dBC2);
  Diff(a, b, &mvAB, &dAB);
  Diff(bs, sa, &mvSA, &dSA);
  Diff(bs, sc2, &mvSC2, &dSC2);
  const bool ok = cl && mvAC1 == 0 && dAC1 == 0 && mvBC2 == 0 && dBC2 == 0 && mvAB > 0 && dAB == 0;
  char what[400];
  snprintf(what, sizeof(what),
           "R133 rig (%s, %s): previous-frame data advanced after the recording (page Map DISCARD + b7 upload): the "
           "list replayed with the exec entry's b7 copy equals a stock draw at the same point bit for bit (RT5 and "
           "depth), and differs from the stock draws before it in RT5 only (R131's signature)",
           tag, th.DriverCommandLists ? "driver command lists" : "runtime-emulated command lists");
  Check(ok, what);
  // A stale replay (b7 snapshotted at record time) reproduces the earlier stock run, not the later one.
  snprintf(what, sizeof(what),
           "R133 rig (%s): a replay that snapshots b7 at record time equals the EARLIER stock run (only b7's "
           "previous-frame matrix moved), never the later one: a stale replay looks older than every stock run",
           tag);
  Check(stale && mvSA == 0 && dSA == 0 && mvSC2 > 0 && dSC2 == 0, what);
  int drawn = 0;
  float mvMax = 0;
  for (size_t t = 0; t < static_cast<size_t>(kW) * kH && a.size() >= 8u * kW * kH; ++t) {
    float dz;
    memcpy(&dz, &a[(static_cast<size_t>(kW) * kH + t) * 4], 4);
    if (dz != 0.0f) ++drawn;
    uint16_t h;
    memcpy(&h, &a[t * 4], 2);
    mvMax = (std::max)(mvMax, std::fabs(gbverify::HalfToFloat(h)));
  }
  printf("     R133 rig %s: drawn %d of %u texels (|mv.x| max %.3g px); RT5 texels A vs B %d, stale vs A %d, stale vs "
         "C2 %d; depth A vs B %d\n", tag, drawn, kW * kH, mvMax, mvAB, mvSA, mvSC2, dAB);
  Rel(cl);
  Rel(stale);
}

// gbverify's classification over A, C1, B, C2 (the rig's sequence, through gb_batch.h's real capture, copies, mask
// and compare), and the dump.
void Classify() {
  Rig r;
  if (!r.Create(D3D_DRIVER_TYPE_WARP)) {
    QvSkip("R133 classify: no WARP device, deferred context or compiler\n");
    return;
  }
  using namespace gbverify;
  ID3D11DeviceContext* const ctx0 = gbbatch::g_ctx;
  gbbatch::g_ctx = r.imm;
  g_dev = r.dev;
  r.dev->AddRef();
  const float cam = 0.02f, obj = 0.3f;
  r.Upload(cam, cam - 1.0e-3f, obj, obj - 2.0e-3f);
  ID3D11CommandList* cl = r.Record(false);
  r.Clear();
  r.Bind(r.imm, r.b7Dcs);
  bool ok = Capture() == nullptr;
  int64_t rawOld = -1, unexOld = -1, total = -1, noStock = -1, later = -1, neither = -1, totalBad = -1,
          noStockBad = -1, laterBad = -1;
  size_t maskPx = 0, maskOld = 0;
  int files = -1;
  int box[4] = {0, 0, -1, -1};  // the crop the dump used
  std::wstring dir;
  if (ok) {
    CopyAll(true, false);  // init (cleared targets)
    r.Stock();             // A
    CopyAll(true, true);
    CopyAll(false, false);
    r.Stock();  // C1: equal to A
    ok &= CopyAllMask();
    CopyAll(false, false);
    r.Upload(cam, cam - 1.0e-3f + 2.0e-6f, obj, obj - 2.0e-3f + 3.0e-6f);  // DCS's state moves on
    r.Bind(r.imm, r.b7Dcs);
    r.Replay(cl, true);  // B
    // R129's rule first (no C2): B (kept in .b) against A, the mask from A vs C1 only.
    ok &= CopyAllTo(&Target::b);
    g_bracket = true;
    g_c2Valid = false;
    ok &= BuildMask(false);
    for (uint8_t v : g_mask) maskOld += v;
    int64_t bad[kMaxTargets];
    uint64_t tx = 0;
    g_maskOn = g_maskControl = true;
    rawOld = CompareGuarded(bad, &tx);
    unexOld = g_badNoStock;
    // R133: init back, C2 after B, bracketed compare.
    CopyAll(false, false);
    r.Bind(r.imm, r.b7Dcs);
    r.Stock();  // C2
    ok &= CopyAllTo(&Target::c2);
    g_c2Valid = true;
    ok &= BuildMask(true);
    for (uint8_t v : g_mask) maskPx += v;
    tx = 0;
    total = CompareGuarded(bad, &tx);
    noStock = g_badNoStock, later = g_badLater, neither = g_badNeither;
    // The dump of this compare (A, C1, B, C2).
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    dir = std::wstring(tmp) + L"qv_r133_dump_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
    box[0] = g_badBox[0] - 2, box[1] = g_badBox[1] - 2, box[2] = g_badBox[2] + 2, box[3] = g_badBox[3] + 2;
    files =DumpCompare(dir, "test header\n", box, true);
    // A recorder error on top: one texel of B's RT5 that no stock run has.
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(r.imm->Map(g_t[0].b, 0, D3D11_MAP_READ_WRITE, 0, &m))) {
      static_cast<uint32_t*>(m.pData)[3] ^= 0x00010001u;
      r.imm->Unmap(g_t[0].b, 0);
    }
    tx = 0;
    totalBad = CompareGuarded(bad, &tx);
    noStockBad = g_badNoStock, laterBad = g_badLater;
    g_maskOn = g_maskControl = g_bracket = g_c2Valid = false;
  }
  ok = ok && rawOld > 0 && unexOld == rawOld && maskOld == 0 && total == rawOld && noStock == 0 && later == total &&
       neither == 0 && maskPx >= static_cast<size_t>(total) && totalBad == total + 1 && noStockBad == 1 &&
       laterBad == total;
  Check(ok, "R133 verify bracket: the burst-onset compare (C1 = A, B later) is all unexplained under R129's rule, all "
            "'equal to the later stock control only' with C2 (0 unexplained); a texel no stock run has still counts");
  if (!ok)
    printf("     classify: raw %lld unexplained(R129) %lld mask %zu; bracketed total %lld noStock %lld later %lld "
           "neither %lld mask %zu; with an error %lld / %lld / %lld\n",
           static_cast<long long>(rawOld), static_cast<long long>(unexOld), maskOld, static_cast<long long>(total),
           static_cast<long long>(noStock), static_cast<long long>(later), static_cast<long long>(neither), maskPx,
           static_cast<long long>(totalBad), static_cast<long long>(noStockBad), static_cast<long long>(laterBad));
  // The dump: header + 2 targets x 4 runs; RT0_B.raw's bytes are B's crop.
  bool dok = files == 9;
  if (dok) {
    std::string hdr;
    FILE* f = nullptr;
    if (!_wfopen_s(&f, (dir + L"header.txt").c_str(), L"rb") && f) {
      char buf[2048];
      const size_t n = fread(buf, 1, sizeof(buf), f);
      hdr.assign(buf, n);
      fclose(f);
    }
    dok = hdr.find("test header") == 0 && hdr.find("RT0 format 34") != std::string::npos &&
          hdr.find("RT1 format 41") != std::string::npos && hdr.find("RT0_C2.raw") != std::string::npos;
    const int x0 = (std::max)(0, box[0]), y0 = (std::max)(0, box[1]);
    const int x1 = (std::min)(static_cast<int>(kW) - 1, box[2]);
    const int y1 = (std::min)(static_cast<int>(kH) - 1, box[3]);
    const size_t want = static_cast<size_t>(x1 - x0 + 1) * (y1 - y0 + 1) * 4;
    std::vector<uint8_t> got;
    if (!_wfopen_s(&f, (dir + L"RT0_B.raw").c_str(), L"rb") && f) {
      got.resize(want + 1);
      got.resize(fread(got.data(), 1, got.size(), f));
      fclose(f);
    }
    // Compare against .b (with the planted texel flipped back: the dump was written before it).
    D3D11_MAPPED_SUBRESOURCE m;
    bool same = got.size() == want;
    if (same && SUCCEEDED(r.imm->Map(g_t[0].b, 0, D3D11_MAP_READ, 0, &m))) {
      for (int y = y0; y <= y1 && same; ++y)
        for (int x = x0; x <= x1 && same; ++x) {
          uint32_t v;
          memcpy(&v, static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch + x * 4, 4);
          if (y == 0 && x == 3) v ^= 0x00010001u;
          uint32_t d;
          memcpy(&d, got.data() + (static_cast<size_t>(y - y0) * (x1 - x0 + 1) + (x - x0)) * 4, 4);
          same = v == d;
        }
      r.imm->Unmap(g_t[0].b, 0);
    } else {
      same = false;
    }
    dok = dok && same;
  }
  Check(dok, "R133 verify dump: header and the four runs of both targets written, cropped to the box, bytes as held");
  if (!dir.empty()) {
    for (const wchar_t* n : {L"header.txt", L"RT0_A.raw", L"RT0_C1.raw", L"RT0_B.raw", L"RT0_C2.raw", L"RT1_A.raw",
                             L"RT1_C1.raw", L"RT1_B.raw", L"RT1_C2.raw"})
      DeleteFileW((dir + n).c_str());
    RemoveDirectoryW(dir.c_str());
  }
  Rel(cl);
  ReleaseViews();
  for (Target& t : g_t) ReleaseCopies(t);
  SafeRelease(g_dev);
  ID3D11RenderTargetView* none[2] = {};
  r.imm->OMSetRenderTargets(2, none, nullptr);
  gbbatch::g_ctx = ctx0;
}

// The dump's arming: count, folder, and the "unexplained only" rule (pure parts of gb_rec.h).
void DumpArm() {
  gbrec::ArmVerifyDump(2, false, L"X:\\logs\\");
  bool ok = gbrec::g_verifyDumpLeft.load() == 2 && gbrec::g_verifyDumpDir == L"X:\\logs\\gbverify_dump\\";
  ok &= !gbrec::VerifyDump("execution #0", 5, 0, "", "", true);  // nothing unexplained: not dumped
  ok &= gbrec::g_verifyDumpLeft.load() == 2;
  gbrec::ArmVerifyDump(2, false, L"X:\\logs\\");  // the same N again: no re-arm
  ok &= gbrec::g_verifyDumpLeft.load() == 2;
  gbrec::ArmVerifyDump(0, false, L"");
  ok &= gbrec::g_verifyDumpLeft.load() == 0;
  Check(ok, "R133 verify dump arming: N per new value, folder under the log folder, compares without unexplained "
            "texels not dumped (GBufferRecVerifyDumpAll=0)");
}

void Run(bool hw = false) {
  printf("-- R133 gbuffer recorder motion vectors --\n");
  if (!hw) {
    DumpArm();
    Freshness(D3D_DRIVER_TYPE_WARP, "WARP");
    Classify();
  } else {
    Freshness(D3D_DRIVER_TYPE_HARDWARE, "hardware");
  }
}

}  // namespace r133test
