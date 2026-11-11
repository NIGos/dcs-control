// One geometry pass per eye, stage 0 (src\per_eye_pass.h; native/dcsqvcull/PER_EYE_PASS_DESIGN.md), offline:
//   - the exact focus rows: BuildVpRows / PredictFocusRows on TerrainContext rows of the R26 census (four
//     periphery + focus pairs: both eyes, both jitter phases), byte for byte; the jittered centre on the census's
//     x values; the verify refuses a one-bit difference;
//   - the viewport-mask packing, the two-slice plan, the merge order rule;
//   - the DXBC checksum against fxc's own;
//   - the vertex-shader clone on WARP: stock shader with the periphery rows, stock shader with the focus rows, the
//     clone with the periphery rows and the focus rows injected, all three stream-captured: the clone's own outputs
//     equal the stock periphery run byte for byte, its second position equals the stock FOCUS run's position byte for
//     byte, the mask output is the injected word. Shader shapes: plain, instanced with a buffer load / a branch on
//     the shared flag / sincos / a loop, a view-dependent varying (cloned, flagged), the render size in the position;
//   - the refusals: a branch on a per-view value, a computed row index, no per-view row, a pixel shader, a damaged
//     container, a clone of a clone;
//   - the cache census on a folder of fake cache files (junk between blobs, a duplicate, a wrong checksum).
// QV_PEP_ARCHIVE=<folder>: also prints the census of that folder (copies of Saved Games\DCS\metashaders2; read-only).
#pragma once

namespace peptest {

// TerrainContext rows of the R26 census (gdd_cb.csv, F-4E Germany F2): rows 0-2 of the periphery (shared with its
// focus), then rows 16, 17, 20, 21 of the periphery and of the focus of the same eye and frame.
struct CensusPair {
  uint32_t shared[12], periph[16], focus[16];
};
const CensusPair kPairs[] = {
    {{0x3e52294c, 0x3c9da20b, 0x3f7a8083, 0x416add47, 0x3e8161de, 0x3f770152, 0xbd9367a0, 0x410ea8af, 0x3f720e0e, 0xbe862ab3, 0xbe45cc40, 0x3f84da8c},
     {0x3f47dedb, 0x00000000, 0xbe44bd07, 0x00000000, 0x00000000, 0x3f4a678c, 0x3a86d904, 0x00000000, 0xbcaf8380, 0x3d85e04e, 0x3f4d141f, 0x41342dac, 0x3e4d961e, 0x3f433948, 0xbd69e728, 0x40e19e5b},
     {0x4012f6b2, 0x00000000, 0xbdc3ed06, 0x00000000, 0x00000000, 0x4014d3ac, 0x39d95be9, 0x00000000, 0x3ec2fbf2, 0x3d8dd5b1, 0x4010fd73, 0x42066ed9, 0x3f1688cf, 0x400f9737, 0xbe2b7892, 0x41a5dfd6}},
    {{0x3e52294c, 0x3c9da20b, 0x3f7a8083, 0x416be36c, 0x3e8161de, 0x3f770152, 0xbd9367a0, 0x410ea8b0, 0x3f720e0e, 0xbe862ab3, 0xbe45cc40, 0x3f84da88},
     {0x3f47dedb, 0x00000000, 0x3e4880cb, 0x00000000, 0x00000000, 0x3f4a678c, 0x3a86d904, 0x00000000, 0x3eb0d4e8, 0xbd14a09f, 0x3f39e547, 0x413b6b8f, 0x3e4d961e, 0x3f433948, 0xbd69e728, 0x40e19e5d},
     {0x4012f6b2, 0x00000000, 0x3dc6fa0a, 0x00000000, 0x00000000, 0x4014d3ac, 0x39d95be9, 0x00000000, 0x3f102a7c, 0x3c996923, 0x400e9b36, 0x4207d247, 0x3f1688cf, 0x400f9737, 0xbe2b7892, 0x41a5dfd7}},
    {{0x3e522847, 0x3c9dbc28, 0x3f7a808c, 0x416be3cc, 0x3e81621b, 0x3f77013b, 0xbd936db4, 0x410ea80c, 0x3f720e13, 0xbe862b3a, 0xbe45ca5c, 0x3f84dcb0},
     {0x3f47dedb, 0x00000000, 0x3e465a12, 0x00000000, 0x00000000, 0x3f4a678c, 0xbab056df, 0x00000000, 0x3eafd028, 0xbd1255fe, 0x3f39fffd, 0x413b62f8, 0x3e4b4a05, 0x3f4361fc, 0xbd680fe8, 0x40e18929},
     {0x4012f6b2, 0x00000000, 0x3dc53c38, 0x00000000, 0x00000000, 0x4014d3ac, 0xba0d8dab, 0x00000000, 0x3f0ff536, 0x3c9b7798, 0x400e9df0, 0x4207d198, 0x3f164def, 0x400f9b44, 0xbe2b4f4e, 0x41a5dd11}},
    {{0x3e522847, 0x3c9dbc28, 0x3f7a808c, 0x416adda6, 0x3e81621b, 0x3f77013b, 0xbd936db4, 0x410ea80d, 0x3f720e13, 0xbe862b3a, 0xbe45ca5c, 0x3f84dcae},
     {0x3f47dedb, 0x00000000, 0xbe46e3c0, 0x00000000, 0x00000000, 0x3f4a678c, 0xbab056df, 0x00000000, 0xbcbfcfc8, 0x3d870671, 0x3f4d2ea7, 0x413424fa, 0x3e4b4a05, 0x3f4361fc, 0xbd680fe8, 0x40e1892b},
     {0x4012f6b2, 0x00000000, 0xbdc5aad8, 0x00000000, 0x00000000, 0x4014d3ac, 0xba0d8dab, 0x00000000, 0x3ec29164, 0x3d8e59b6, 0x40110027, 0x42066e27, 0x3f164def, 0x400f9b44, 0xbe2b4f4e, 0x41a5dd12}},
};

float F(uint32_t bits) {
  float f;
  memcpy(&f, &bits, 4);
  return f;
}
uint32_t U(float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}

// A TerrainContext with the pair's census rows; every other row pseudo-random; the per-view rows of the focus differ.
void MakeTc(const CensusPair& cp, bool focus, uint8_t* tc) {
  uint32_t seed = 0x1234567;
  float* f = reinterpret_cast<float*>(tc);
  for (int i = 0; i < pep::kTcBytes / 4; ++i) {
    seed = seed * 1664525u + 1013904223u;
    f[i] = static_cast<float>(static_cast<int>(seed >> 8 & 0xffff) - 32768) / 16384.0f;
  }
  memcpy(tc, cp.shared, 48);
  memcpy(tc + 16 * 23, cp.shared + 8, 16);                           // the clip w row = row 2
  const float z[4] = {0, 0, 0, 0.02f};
  memcpy(tc + 16 * 22, z, 16);
  const float size[4] = {0, 0, focus ? 1176.0f : 476.0f, focus ? 1164.0f : 468.0f};
  memcpy(tc + 16 * 44, size, 16);
  const uint32_t* v = focus ? cp.focus : cp.periph;
  memcpy(tc + 16 * 16, v, 32);
  memcpy(tc + 16 * 20, v + 8, 32);
  if (focus)
    for (const pep::ViewRow& r : pep::kTcViewRows)
      if (r.row != 16 && r.row != 17 && r.row != 20 && r.row != 21 && r.row != 44)
        for (int k = 0; k < 4; ++k)
          if (r.comps >> k & 1) f[r.row * 4 + k] += 0.37f;
}

void FocusRows() {
  bool vp = true, pred = true, verify = true;
  for (const CensusPair& cp : kPairs) {
    float s[12];
    memcpy(s, cp.shared, 48);
    for (int view = 0; view < 2; ++view) {
      const uint32_t* v = view ? cp.focus : cp.periph;
      const pep::FocusP p{F(v[0]), F(v[2]), F(v[5]), F(v[6])};
      float x[4], y[4];
      pep::BuildVpRows(s, s + 4, s + 8, p, x, y);
      vp &= memcmp(x, v + 8, 16) == 0 && memcmp(y, v + 12, 16) == 0;
    }
    uint8_t tcP[pep::kTcBytes], tcF[pep::kTcBytes], out[64];
    MakeTc(cp, false, tcP);
    MakeTc(cp, true, tcF);
    const pep::FocusP pf{F(cp.focus[0]), F(cp.focus[2]), F(cp.focus[5]), F(cp.focus[6])};
    pep::PredictFocusRows(tcP, pf, out);
    pred &= memcmp(out, cp.focus, 64) == 0;
    verify &= pep::VerifyFocusRows(out, tcF) == 0;
    tcF[16 * 21 + 4] ^= 1;  // one bit of row 21
    verify &= pep::VerifyFocusRows(out, tcF) == 1 + 21;
    pep::FocusP off = pf;
    off.cx = F(U(pf.cx) + 1);  // a centre one ulp away: the rows no longer match DCS's
    pep::PredictFocusRows(tcP, off, out);
    tcF[16 * 21 + 4] ^= 1;
    verify &= pep::VerifyFocusRows(out, tcF) == 1 + 16;
  }
  Check(vp, "per-eye pass: view-projection rows 20 / 21 rebuilt byte for byte from the shared rows and the "
            "projection scalars (census, 4 periphery and 4 focus contexts)");
  Check(pred, "per-eye pass: the focus rows predicted from the periphery's upload equal DCS's focus upload byte for byte");
  Check(verify, "per-eye pass: the verify accepts equal bytes, names the row of a one-bit difference and refuses a "
                "centre one ulp away");
  // The census's unjittered and jittered projection centres (x): periphery 476 px wide, focus 1176 px.
  const bool jit = U(pep::JitterCentre(0.19396557f, -0.0625f, 476.0f)) == U(0.19370296597480774f) &&
                   U(pep::JitterCentre(0.19396557f, 0.4375f, 476.0f)) == U(0.19580380618572235f) &&
                   U(pep::JitterCentre(-0.19396557f, -0.0625f, 476.0f)) == U(-0.1942281723022461f) &&
                   U(pep::JitterCentre(0.096412554f, -0.0625f, 1176.0f)) == U(0.09630626440048218f) &&
                   U(pep::JitterCentre(0.096412554f, 0.4375f, 1176.0f)) == U(0.0971565991640091f) &&
                   U(pep::JitterCentre(-0.096410975f, 0.4375f, 1176.0f)) == U(-0.09566693007946014f);
  Check(jit, "per-eye pass: jittered projection centre = c0 + 2 j / size on the census's x values (both view types)");
}

void Rules() {
  Check(pep::PackViewportMasks(1, 2) == 0x00020001u && pep::PackViewportMasks(0x1ffff, 3) == 0x0003ffffu,
        "per-eye pass: viewport masks packed low 16 bits view 0, high 16 bits view 1");
  const pep::SlicePlan a = pep::PlanSlices(476, 468, 1176, 1164, 1, 1, true);
  const pep::SlicePlan b = pep::PlanSlices(1300, 468, 1176, 1164, 1, 1, true);
  Check(a.why == pep::kPlOk && a.w == 1176 && a.h == 1164 && a.vp0w == 476 && a.vp1h == 1164 && a.mask == 0x20001 &&
            b.why == pep::kPlOk && b.w == 1300 && b.h == 1164,
        "per-eye pass: two-slice plan at the larger size per axis, both viewports at the origin");
  Check(pep::PlanSlices(476, 468, 1176, 1164, 4, 4, true).why == pep::kPlSamples &&
            pep::PlanSlices(476, 468, 1176, 1164, 1, 1, false).why == pep::kPlFormat &&
            pep::PlanSlices(0, 468, 1176, 1164, 1, 1, true).why == pep::kPlSize,
        "per-eye pass: no plan with MSAA targets, differing formats or an empty view");
  const uint8_t first[] = {1, 1, 1, 0, 0}, last[] = {0, 0, 1, 1}, mixed[] = {1, 0, 1}, mid[] = {0, 1, 1, 0}, none[] = {0, 0},
                all[] = {1, 1};
  Check(pep::ClassifyOrder(first, 5) == pep::kMoFirst && pep::ClassifyOrder(last, 4) == pep::kMoLast &&
            pep::ClassifyOrder(mixed, 3) == pep::kMoMixed && pep::ClassifyOrder(mid, 4) == pep::kMoMixed &&
            pep::ClassifyOrder(none, 2) == pep::kMoNone && pep::ClassifyOrder(all, 2) == pep::kMoFirst &&
            pep::ClassifyOrder(nullptr, 0) == pep::kMoNone,
        "per-eye pass: merge order (merged run first: copy-in; last: depth-tested composite; anything else: stock)");
}

const char kHlsl[] = R"(
cbuffer cPerFrame : register(b6) { float4 gFrame[4]; };
cbuffer TerrainContext : register(b11) { float4 tc[136]; };
Buffer<float4> gInst : register(t0);
struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; uint iid : SV_InstanceID; };
struct VSOut { float4 pos : SV_Position; float3 wpos : TEXCOORD0; float2 uv : TEXCOORD1; };
struct VSOutV { float4 pos : SV_Position; float3 wpos : TEXCOORD0; float2 uv : TEXCOORD1; float4 proj : TEXCOORD2; };
float4 Project(float4 w) { return float4(dot(w, tc[20]), dot(w, tc[21]), dot(w, tc[22]), dot(w, tc[23])); }
VSOut vs_plain(VSIn i) {
  VSOut o;
  float4 w = float4(i.pos + gFrame[0].xyz, 1);
  o.pos = Project(w);
  o.wpos = w.xyz;
  o.uv = i.uv * gFrame[1].xy;
  return o;
}
VSOut vs_inst(VSIn i) {
  VSOut o;
  float4 inst = gInst.Load(i.iid);
  float3 p = i.pos * inst.w + inst.xyz;
  float dist = length(p - tc[38].xyz);
  if (tc[44].x == 1) dist = tc[44].y;           // the shared flag and its fixed distance
  float s, c;
  sincos(dist * gFrame[2].x + i.uv.x, s, c);
  for (uint k = 0; k < (i.iid & 3); ++k) p.xz += float2(s, c) * 0.01 * gFrame[2].y;
  float4 w = float4(p, 1);
  float4 clip = Project(w);
  clip.z = clip.z * 0.5 + clip.w * 0.25;        // per-view values through temps, shared values mixed in
  if (dist > gFrame[2].z) clip.xy += clip.w * gFrame[3].xy;
  o.pos = clip;
  o.wpos = p;
  o.uv = i.uv + float2(s, dist);
  return o;
}
VSOutV vs_vary(VSIn i) {
  VSOutV o;
  float4 w = float4(i.pos, 1);
  o.pos = Project(w);
  o.wpos = w.xyz;
  o.uv = i.uv;
  o.proj = o.pos * float4(0.5, -0.5, 1, 1);     // a view-dependent value for the pixel shader
  return o;
}
VSOut vs_size(VSIn i) {
  VSOut o;
  float4 clip = Project(float4(i.pos, 1));
  clip.xy = floor(clip.xy / clip.w * tc[44].zw) / tc[44].zw * clip.w;   // the render size (per view) in the position
  o.pos = clip;
  o.wpos = i.pos;
  o.uv = i.uv;
  return o;
}
VSOut vs_flow(VSIn i) {
  VSOut o;
  float4 w = float4(i.pos, 1);
  [branch] if (dot(w, tc[20]) > 0) w.y += gFrame[0].x;   // a branch on a per-view value
  o.pos = Project(w);
  o.wpos = w.xyz;
  o.uv = i.uv;
  return o;
}
VSOut vs_dyn(VSIn i) {
  VSOut o;
  float4 w = float4(i.pos, 1);
  o.pos = float4(dot(w, tc[20 + (i.iid & 1)]), dot(w, tc[21]), dot(w, tc[22]), dot(w, tc[23]));
  o.wpos = w.xyz;
  o.uv = i.uv;
  return o;
}
VSOut vs_lod(VSIn i) {
  VSOut o;
  int l = max(min((int)gFrame[3].z, 3), 0);     // DCS's surfaceLod pattern: rows 45..48, shared by both views
  float4 lod = tc[45 + l];
  float4 w = float4(i.pos * (1 + lod.x * 0.125) + lod.yzw * 0.01, 1);
  o.pos = Project(w);
  o.wpos = w.xyz;
  o.uv = i.uv;
  return o;
}
VSOut vs_lod_view(VSIn i) {
  VSOut o;
  int l = max(min((int)gFrame[3].z, 3), 0);     // rows 41..44: row 44 holds the render size (per view)
  float4 w = float4(i.pos + tc[41 + l].xyz * 0.01, 1);
  o.pos = Project(w);
  o.wpos = w.xyz;
  o.uv = i.uv;
  return o;
}
VSOut vs_lod_open(VSIn i) {
  VSOut o;
  int l = min((int)gFrame[3].z, 3);             // no lower bound
  float4 w = float4(i.pos + tc[45 + l].xyz * 0.01, 1);
  o.pos = Project(w);
  o.wpos = w.xyz;
  o.uv = i.uv;
  return o;
}
VSOut vs_noview(VSIn i) {
  VSOut o;
  float4 w = float4(i.pos, 1);
  o.pos = float4(dot(w, gFrame[0]), dot(w, gFrame[1]), dot(w, tc[22]), dot(w, tc[23])) + tc[44].x;
  o.wpos = w.xyz;
  o.uv = i.uv;
  return o;
}
float4 ps_main(VSOut i) : SV_Target { return float4(i.wpos, tc[20].x); }
)";

struct Rig {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  decltype(&D3DCompile) compile = nullptr;
  ID3D11Buffer* vb = nullptr;
  ID3D11Buffer* frame = nullptr;
  ID3D11Buffer* inst = nullptr;
  ID3D11ShaderResourceView* instSrv = nullptr;
  bool ok = false;
} g_rig;
constexpr UINT kVerts = 96, kInst = 4;

std::vector<uint8_t> Compile(const char* entry, const char* target) {
  std::vector<uint8_t> out;
  ID3DBlob* code = nullptr;
  ID3DBlob* err = nullptr;
  if (SUCCEEDED(g_rig.compile(kHlsl, sizeof(kHlsl) - 1, "pep", nullptr, nullptr, entry, target, 0, 0, &code, &err)) && code)
    out.assign(static_cast<const uint8_t*>(code->GetBufferPointer()),
               static_cast<const uint8_t*>(code->GetBufferPointer()) + code->GetBufferSize());
  else if (err)
    printf("     compile %s: %s\n", entry, static_cast<const char*>(err->GetBufferPointer()));
  if (code) code->Release();
  if (err) err->Release();
  return out;
}
bool MakeRig() {
  if (g_rig.ok) return true;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  // QV_PEP_HW=1: the same checks on the PC's GPU instead of WARP (a dev run; the build's tests use WARP).
  const char* hw = getenv("QV_PEP_HW");
  const D3D_DRIVER_TYPE type = hw && *hw == '1' ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
  if (FAILED(D3D11CreateDevice(nullptr, type, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &g_rig.dev, nullptr,
                               &g_rig.ctx)))
    return false;
  HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
  g_rig.compile = dll ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dll, "D3DCompile")) : nullptr;
  if (!g_rig.compile) return false;
  float verts[kVerts * 5];
  uint32_t seed = 99;
  for (float& v : verts) {
    seed = seed * 1664525u + 1013904223u;
    v = static_cast<float>(static_cast<int>(seed >> 8 & 0xffff) - 32768) / 257.0f;
  }
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = sizeof(verts), bd.Usage = D3D11_USAGE_IMMUTABLE, bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA sd{verts, 0, 0};
  if (FAILED(g_rig.dev->CreateBuffer(&bd, &sd, &g_rig.vb))) return false;
  const float frame[16] = {3.5f, -1.25f, 7.0f, 0.5f, 0.75f, 1.5f, 0, 0, 0.013f, 2.0f, 40.0f, 0, 0.004f, -0.002f, 0, 0};
  bd.ByteWidth = sizeof(frame), bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  sd.pSysMem = frame;
  if (FAILED(g_rig.dev->CreateBuffer(&bd, &sd, &g_rig.frame))) return false;
  const float inst[kInst * 4] = {1, 2, 3, 1.5f, -40, 0.5f, 12, 0.75f, 9, -3, -60, 2.0f, 0.25f, 0.5f, 0.125f, 1};
  bd.ByteWidth = sizeof(inst), bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  sd.pSysMem = inst;
  if (FAILED(g_rig.dev->CreateBuffer(&bd, &sd, &g_rig.inst))) return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
  vd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT, vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  vd.Buffer.NumElements = kInst;
  if (FAILED(g_rig.dev->CreateShaderResourceView(g_rig.inst, &vd, &g_rig.instSrv))) return false;
  g_rig.ok = true;
  return true;
}

// The stream-output declaration of every element of a blob's output signature, in order.
struct SoDecl {
  std::vector<std::string> names;
  std::vector<D3D11_SO_DECLARATION_ENTRY> e;
  UINT stride = 0;
};
bool MakeSoDecl(const std::vector<uint8_t>& blob, SoDecl& d) {
  std::vector<pep::Chunk> chunks;
  if (!pep::ParseContainer(blob.data(), blob.size(), chunks)) return false;
  for (const pep::Chunk& c : chunks) {
    if (c.fourcc != pep::FourCc('O', 'S', 'G', 'N')) continue;
    const uint8_t* p = blob.data() + c.off;
    const uint32_t n = pep::Rd32(p);
    d.names.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t* el = p + 8 + 24 * i;
      d.names.emplace_back(reinterpret_cast<const char*>(p + pep::Rd32(el)));
      const uint8_t mask = el[20];
      BYTE start = 0, count = 0;
      while (start < 4 && !(mask >> start & 1)) ++start;
      while (start + count < 4 && (mask >> (start + count) & 1)) ++count;
      D3D11_SO_DECLARATION_ENTRY e{};
      e.SemanticIndex = pep::Rd32(el + 4), e.StartComponent = start, e.ComponentCount = count;
      d.e.push_back(e);
      d.stride += 4u * count;
    }
    for (size_t i = 0; i < d.e.size(); ++i) d.e[i].SemanticName = d.names[i].c_str();
    return true;
  }
  return false;
}
// Runs a vertex shader over the rig's vertices (kInst instances) and returns every output of every vertex.
bool RunVs(const std::vector<uint8_t>& blob, const uint8_t* tc, UINT injectSlot, const std::vector<uint8_t>* inject,
           std::vector<uint8_t>& out, UINT* stride) {
  SoDecl decl;
  if (!MakeSoDecl(blob, decl)) return false;
  *stride = decl.stride;
  ID3D11VertexShader* vs = nullptr;
  ID3D11GeometryShader* gs = nullptr;
  ID3D11InputLayout* il = nullptr;
  ID3D11Buffer* so = nullptr;
  ID3D11Buffer* stage = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11Buffer* cbInj = nullptr;
  const D3D11_INPUT_ELEMENT_DESC ied[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  bool ok = SUCCEEDED(g_rig.dev->CreateVertexShader(blob.data(), blob.size(), nullptr, &vs)) &&
            SUCCEEDED(g_rig.dev->CreateGeometryShaderWithStreamOutput(blob.data(), blob.size(), decl.e.data(),
                                                                      static_cast<UINT>(decl.e.size()), &decl.stride, 1,
                                                                      D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &gs)) &&
            SUCCEEDED(g_rig.dev->CreateInputLayout(ied, 2, blob.data(), blob.size(), &il));
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = decl.stride * kVerts * kInst, bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
  ok = ok && SUCCEEDED(g_rig.dev->CreateBuffer(&bd, nullptr, &so));
  bd.BindFlags = 0, bd.Usage = D3D11_USAGE_STAGING, bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ok = ok && SUCCEEDED(g_rig.dev->CreateBuffer(&bd, nullptr, &stage));
  D3D11_BUFFER_DESC cd{};
  cd.ByteWidth = pep::kTcBytes, cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  D3D11_SUBRESOURCE_DATA sd{tc, 0, 0};
  ok = ok && SUCCEEDED(g_rig.dev->CreateBuffer(&cd, &sd, &cb));
  if (ok && inject) {
    cd.ByteWidth = static_cast<UINT>(inject->size());
    sd.pSysMem = inject->data();
    ok = SUCCEEDED(g_rig.dev->CreateBuffer(&cd, &sd, &cbInj));
  }
  if (ok) {
    ID3D11DeviceContext* c = g_rig.ctx;
    c->ClearState();
    const UINT vstride = 20, off = 0;
    c->IASetInputLayout(il);
    c->IASetVertexBuffers(0, 1, &g_rig.vb, &vstride, &off);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    c->VSSetShader(vs, nullptr, 0);
    c->GSSetShader(gs, nullptr, 0);
    c->VSSetConstantBuffers(6, 1, &g_rig.frame);
    c->VSSetConstantBuffers(11, 1, &cb);
    if (cbInj) c->VSSetConstantBuffers(injectSlot, 1, &cbInj);
    c->VSSetShaderResources(0, 1, &g_rig.instSrv);
    c->SOSetTargets(1, &so, &off);
    c->DrawInstanced(kVerts, kInst, 0, 0);
    c->ClearState();
    c->CopyResource(stage, so);
    D3D11_MAPPED_SUBRESOURCE m{};
    ok = SUCCEEDED(c->Map(stage, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
      out.assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + bd.ByteWidth);
      c->Unmap(stage, 0);
    }
  }
  for (IUnknown* u : {static_cast<IUnknown*>(vs), static_cast<IUnknown*>(gs), static_cast<IUnknown*>(il),
                      static_cast<IUnknown*>(so), static_cast<IUnknown*>(stage), static_cast<IUnknown*>(cb),
                      static_cast<IUnknown*>(cbInj)})
    if (u) u->Release();
  return ok;
}

constexpr uint32_t kMaskWord = 0x00020001;
// The injected buffer of a clone: the focus context's rows it reads, then the mask word.
std::vector<uint8_t> Injected(const pep::PatchResult& r, const uint8_t* tcFocus) {
  std::vector<uint8_t> b(16 * (r.rows.size() + 1), 0);
  for (size_t i = 0; i < r.rows.size(); ++i) memcpy(b.data() + 16 * i, tcFocus + 16 * r.rows[i], 16);
  memcpy(b.data() + 16 * r.rows.size(), &kMaskWord, 4);
  return b;
}
// The clone against the stock shader: 0 = every byte as required, else a step number.
int CloneCheck(const std::vector<uint8_t>& stock, const pep::PatchResult& r, const CensusPair& cp) {
  uint8_t tcP[pep::kTcBytes], tcF[pep::kTcBytes];
  MakeTc(cp, false, tcP);
  MakeTc(cp, true, tcF);
  std::vector<uint8_t> a, b, c;
  UINT sa = 0, sb = 0, sc = 0;
  if (!RunVs(stock, tcP, 0, nullptr, a, &sa) || !RunVs(stock, tcF, 0, nullptr, b, &sb)) return 1;
  const std::vector<uint8_t> inj = Injected(r, tcF);
  if (!RunVs(r.blob, tcP, r.slot, &inj, c, &sc)) return 2;
  if (sc != sa + 20 || a.size() != static_cast<size_t>(sa) * kVerts * kInst) return 3;
  bool differs = false;  // the two stock runs must differ in position, or the test proves nothing
  for (UINT v = 0; v < kVerts * kInst; ++v) {
    const uint8_t* pa = a.data() + static_cast<size_t>(v) * sa;
    const uint8_t* pb = b.data() + static_cast<size_t>(v) * sb;
    const uint8_t* pc = c.data() + static_cast<size_t>(v) * sc;
    if (memcmp(pc, pa, sa) != 0) return 4;            // the clone's own outputs = the stock periphery run
    if (memcmp(pc + sa, pb, 16) != 0) return 5;       // the second position = the stock focus run's position
    if (memcmp(pc + sa + 16, &kMaskWord, 4) != 0) return 6;
    differs |= memcmp(pa, pb, 8) != 0;
  }
  return differs ? 0 : 7;
}

void Clone() {
  if (!MakeRig()) {
    Check(false, "per-eye pass: WARP device and compiler");
    return;
  }
  const std::vector<uint8_t> plain = Compile("vs_plain", "vs_5_0"), inst = Compile("vs_inst", "vs_5_0"),
                             vary = Compile("vs_vary", "vs_5_0"), size = Compile("vs_size", "vs_5_0"),
                             flow = Compile("vs_flow", "vs_5_0"), dyn = Compile("vs_dyn", "vs_5_0"),
                             noview = Compile("vs_noview", "vs_5_0"), ps = Compile("ps_main", "ps_5_0"),
                             plain4 = Compile("vs_plain", "vs_4_0"), lod = Compile("vs_lod", "vs_5_0"),
                             lodView = Compile("vs_lod_view", "vs_5_0"), lodOpen = Compile("vs_lod_open", "vs_5_0");
  if (lod.empty() || lodView.empty() || lodOpen.empty() || plain.empty() || inst.empty() || vary.empty() || size.empty() || flow.empty() || dyn.empty() || noview.empty() ||
      ps.empty() || plain4.empty()) {
    Check(false, "per-eye pass: test shaders compile");
    return;
  }
  bool sums = true;
  for (const std::vector<uint8_t>* b : {&plain, &inst, &vary, &size, &flow, &dyn, &noview, &ps, &plain4})
    sums &= pep::ChecksumOk(b->data(), b->size());
  std::vector<uint8_t> bad = plain;
  bad[bad.size() - 9] ^= 0x40;
  Check(sums && !pep::ChecksumOk(bad.data(), bad.size()),
        "per-eye pass: the DXBC checksum equals the compiler's on nine shaders and notices a changed byte");

  struct Case {
    const char* name;
    const std::vector<uint8_t>* blob;
    bool varyings;
  } cases[] = {{"plain", &plain, false}, {"instanced (buffer load, shared-flag branch, sincos, loop)", &inst, false},
               {"view-dependent varying", &vary, true}, {"render size in the position", &size, false},
               {"vs_4_0", &plain4, false}, {"surface-LOD row picked by a clamped index", &lod, false}};
  for (const Case& cs : cases) {
    const pep::PatchResult r = pep::PatchVsDualPosition(cs.blob->data(), cs.blob->size());
    int step = r.why == pep::kPwOk ? 0 : 100 + r.why;
    if (!step && (!pep::ChecksumOk(r.blob.data(), r.blob.size()) || r.twinned == 0 || r.twinned >= r.ins)) step = 90;
    if (!step && (r.viewVaryings != 0) != cs.varyings) step = 91;
    for (const CensusPair& cp : kPairs)
      if (!step) step = CloneCheck(*cs.blob, r, cp);
    if (step) printf("     %s: step %d (opcode %u), %u of %u instructions repeated\n", cs.name, step, r.badOp, r.twinned, r.ins);
    char what[256];
    snprintf(what, sizeof(what),
             "per-eye pass: clone of a %s shader: own outputs = stock periphery run, second position = stock focus "
             "run, byte for byte (WARP, 384 vertices x 4 census pairs)", cs.name);
    Check(step == 0, what);
    if (!step && cs.blob == &inst)
      Check(r.rows.size() == 2 && r.rows[0] + r.rows[1] == 41 && r.rows[0] != r.rows[1] && r.slot == 13 &&
                r.twinned * 2 < r.ins,
            "per-eye pass: the instanced clone injects rows 20 / 21 only, at b13, and repeats under half its instructions");
    if (!step && cs.blob == &size)
      Check(r.rows.size() == 3 && r.rows[0] + r.rows[1] + r.rows[2] == 85,
            "per-eye pass: the render-size row is injected when the position reads it");
    if (!step && cs.blob == &plain) {
      const pep::PatchResult again = pep::PatchVsDualPosition(r.blob.data(), r.blob.size());
      Check(again.why == pep::kPwSignature && again.blob.empty(), "per-eye pass: a clone is not cloned again");
    }
  }
  const pep::PatchResult rf = pep::PatchVsDualPosition(flow.data(), flow.size());
  const pep::PatchResult rd = pep::PatchVsDualPosition(dyn.data(), dyn.size());
  const pep::PatchResult rn = pep::PatchVsDualPosition(noview.data(), noview.size());
  const pep::PatchResult rp = pep::PatchVsDualPosition(ps.data(), ps.size());
  const pep::PatchResult rb = pep::PatchVsDualPosition(bad.data(), 40);
  Check(rf.why == pep::kPwFlow && rd.why == pep::kPwDynamicRow && rn.why == pep::kPwNoViewRows &&
            rp.why == pep::kPwNotVs && rb.why == pep::kPwContainer && rf.blob.empty() && rd.blob.empty() &&
            rn.blob.empty() && rp.blob.empty(),
        "per-eye pass: no clone for a branch on a per-view value, a computed row index, a position without per-view "
        "rows, a pixel shader, a cut container");
  if (rf.why != pep::kPwFlow || rd.why != pep::kPwDynamicRow || rn.why != pep::kPwNoViewRows)
    printf("     flow %d, dyn %d, noview %d, ps %d, cut %d\n", rf.why, rd.why, rn.why, rp.why, rb.why);
  // A computed row: cloned only when its index is clamped onto rows both views share.
  const pep::PatchResult rl = pep::PatchVsDualPosition(lod.data(), lod.size());
  const pep::PatchResult rlv = pep::PatchVsDualPosition(lodView.data(), lodView.size());
  const pep::PatchResult rlo = pep::PatchVsDualPosition(lodOpen.data(), lodOpen.size());
  Check(rl.why == pep::kPwOk && rl.boundedRows >= 1 && rlv.why == pep::kPwDynamicRow && rlv.blob.empty() &&
            rlo.why == pep::kPwDynamicRow && rlo.blob.empty() && rd.boundedRows == 0,
        "per-eye pass: a computed row is cloned when imin / imax clamp it onto shared rows (45..48), refused when "
        "the range reaches a per-view row (41..44) or has no lower bound");
  if (rl.why != pep::kPwOk || rlv.why != pep::kPwDynamicRow || rlo.why != pep::kPwDynamicRow)
    printf("     lod %d (bounded %u), lod on a view row %d, lod without a lower bound %d\n", rl.why, rl.boundedRows, rlv.why,
           rlo.why);
  // A slot the shader already declares is never used for the injected rows.
  pep::PatchOpts o6;
  o6.preferSlot = 6;
  const pep::PatchResult r6 = pep::PatchVsDualPosition(plain.data(), plain.size(), o6);
  Check(r6.why == pep::kPwOk && r6.slot == 13 && CloneCheck(plain, r6, kPairs[0]) == 0,
        "per-eye pass: a declared constant-buffer slot is not taken for the injected rows");

  // The census on a folder of fake cache files.
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring dir = std::wstring(tmp) + L"qv_pep_" + std::to_wstring(GetCurrentProcessId());
  CreateDirectoryW(dir.c_str(), nullptr);
  std::vector<uint8_t> file1, file2;
  const auto put = [](std::vector<uint8_t>& f, const std::vector<uint8_t>& b, const char* junk) {
    f.insert(f.end(), junk, junk + strlen(junk));
    f.insert(f.end(), b.begin(), b.end());
  };
  std::vector<uint8_t> badSum = inst;
  badSum[badSum.size() - 5] ^= 1;
  put(file1, plain, "meta2 header DXBC not a blob D");
  put(file1, inst, "DXB");
  put(file1, ps, "");
  put(file1, vary, "name\0x");
  put(file1, badSum, "DD");
  put(file2, plain, "second file ");  // a duplicate
  put(file2, flow, "");
  put(file2, dyn, "");
  put(file2, noview, "");
  file2.insert(file2.end(), {'D', 'X', 'B', 'C', 1, 2, 3});
  const auto save = [&](const wchar_t* name, const std::vector<uint8_t>& f) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, (dir + L"\\" + name).c_str(), L"wb") == 0 && fp) {
      fwrite(f.data(), 1, f.size(), fp);
      fclose(fp);
    }
  };
  save(L"a.meta2", file1);
  save(L"b.dxbc", file2);
  save(L"c.txt", file1);  // not a cache file
  pep::Census c;
  const bool found = pep::CensusFolder(dir, pep::PatchOpts(), c);
  const std::string text = pep::CensusText(c);
  Check(found && c.files == 2 && c.blobs == 9 && c.badSum == 1 && c.duplicates == 1 && c.vs == 6 && c.declares == 6 &&
            c.exact == 2 && c.varyings == 1 && c.why[pep::kPwFlow] == 1 && c.why[pep::kPwDynamicRow] == 1 &&
            c.why[pep::kPwNoViewRows] == 1 && text.find("cloned exactly 2 (40.0 %)") != std::string::npos,
        "per-eye pass: cache census (blobs between junk, a duplicate, a wrong checksum, a pixel shader; exact / "
        "varying / refused counts)");
  if (!(found && c.blobs == 9 && c.exact == 2)) printf("%s", text.c_str());
  pep::Census none;
  Check(!pep::CensusFolder(dir + L"\\missing", pep::PatchOpts(), none) && none.files == 0,
        "per-eye pass: a folder without cache files counts nothing");
  for (const wchar_t* n : {L"\\a.meta2", L"\\b.dxbc", L"\\c.txt"}) DeleteFileW((dir + n).c_str());
  RemoveDirectoryW(dir.c_str());

  wchar_t arc[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(L"QV_PEP_ARCHIVE", arc, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    pep::Census real;
    if (pep::CensusFolder(arc, pep::PatchOpts(), real))
      printf("%s", pep::CensusText(real).c_str());
    else
      printf("     QV_PEP_ARCHIVE: no *.meta2 / *.dxbc files there\n");
  }
}

void Run() {
  FocusRows();
  Rules();
  Clone();
}

}  // namespace peptest
