// Offline tests for shadow_rec.h (R17 S1-S3):
//  - the mesh fields and their fingerprint, the key and mesh tables, scope;
//  - the static key facts from reflection of (b)-like VS blobs (what the VS
//    may read, which def_uniforms dwords it reads);
//  - the probe checks (CheckKey, CheckMesh) and publishing (ProcessProbe);
//  - job building from fake DCS memory: per-caster reasons (group-invariant),
//    the records, material snapshots and last casters, pages, probe
//    requests; the pass checks (vector identity, page views, CB guard) and
//    the mat+0x18c restore;
//  - on a real device (hardware, else WARP): DCS's caster draws emulated on
//    the immediate context (material CB Map DISCARD per draw, per-view b7,
//    sbPositions pages), the probe read-back published as keys and meshes,
//    the job built and recorded by the worker pool, the list executed at the
//    first caster between DCS's residual draws (our b7 late-copied), and the
//    depth compared bit for bit with the stock draws.
// QV_SHREC_ONLY=1: only these tests.
#pragma once

#include <set>

namespace srtest {
using namespace shrec;

constexpr int kMeshes = 6, kMats = 8, kPages = 3, kObjs = 400, kPerPage = 128;
constexpr int kTexturedMat = 7;  // binds textures (S4; residual unless scope bit 9), DCS shader 2
constexpr int kTex = 5;          // textures 0-2: Diffuse (read), 3: Specular (skipped), 4: spare (tests)
constexpr UINT kDepth = 512;

// Fakes of the texture code slot 5 reaches: streaming requests counted per
// texture; getDesc/compat as the view prediction needs them.
std::map<void*, int> g_vt23;
bool g_countVt23 = true;
uint64_t __fastcall FakeVt23(void* tex, uint64_t) {
  if (g_countVt23) ++g_vt23[tex];
  return 0;
}
alignas(16) uint8_t g_desc[0x40];
const uint8_t* __fastcall FakeGetDesc(void*) { return g_desc; }
bool __fastcall FakeCompat(int32_t, int32_t) { return true; }
// The texture-skip mask of shader 2: record 0 (Diffuse) read, the others not.
shadowtex::MaskEntry g_mask;
const shadowtex::MaskEntry* FakeMaskFor(uint8_t*) { return &g_mask; }
const char* const kRecNames[4] = {"Diffuse", "Specular", "NormalMap", "Other"};

// A COM object for views in the CPU-only tests (reference counted).
struct FakeView : IUnknown {
  ULONG refs = 1;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
};

// ---------------------------------------------------------------------------
// Fake DCS memory
// ---------------------------------------------------------------------------
struct World {
  void* smrVt[4] = {};
  void* otherVt[4] = {};
  void* modelVt[8] = {};
  void* shaderVt[40] = {};
  uint8_t shader[3][0x200] = {};
  uint8_t recs[4 * 0x50] = {};
  uint8_t tex[kTex][0x700] = {};
  uint8_t inner[kTex][0x20] = {};
  uint8_t views[kTex][0x40] = {};
  void* texVt[30] = {};
  void* innerVt[12] = {};
  uint64_t entries[kObjs][6] = {};
  uint8_t* arrPtr[kObjs] = {};
  uint8_t mat[kMats][0x300] = {};
  uint8_t props[kMats][0x300] = {};
  uint8_t item[kObjs][0x100] = {};
  uint8_t rend[kObjs][0x20] = {};
  uint8_t mesh[kMeshes][0x220] = {};
  uint8_t ibObj[kMeshes][0x40] = {};
  uint8_t vbObj[kMeshes][0x160] = {};
  uint8_t globalsObj[0x100] = {};
  uint8_t* globalsPtr = nullptr;
  uint8_t pageArr[0x20 + kPages * 0x30] = {};
  uint8_t gpuBuf[kPages][0x40] = {};
  uint32_t tris[kMeshes] = {};
  std::vector<void*> vec;
  void* desc[3] = {};
  Env env;

  static int MatOf(int i) { return i % kMats; }
  static int MeshOf(int i) { return (i / kMats) % kMeshes; }
  static int PageOf(int i) { return i % kPages; }
  static int ShaderOf(int m) { return m == kTexturedMat ? 2 : m < 4 ? 0 : 1; }
  static int DiffuseOf(int i) { return (i / 3) % 3; }
  void SetView(int t, void* v) { *reinterpret_cast<void**>(views[t] + 0x10) = v; }
  bool Smr(int i) const { return *reinterpret_cast<void* const*>(rend[i]) == smrVt; }

  void SetMeshBuffers(int k, void* ib, void* vb) {
    *reinterpret_cast<void**>(ibObj[k] + 0x28) = ib;
    *reinterpret_cast<void**>(vbObj[k] + 0x130) = vb;
  }
  void SetPageSrv(int p, void* srv) { *reinterpret_cast<void**>(gpuBuf[p] + 0x30) = srv; }

  void Init() {
    uint32_t rng = 0x2468ace;
    auto rnd = [&]() {
      rng = rng * 1664525u + 1013904223u;
      return rng;
    };
    for (int s = 0; s < 3; ++s) {
      *reinterpret_cast<void**>(shader[s]) = shaderVt;
      *reinterpret_cast<uintptr_t*>(shader[s] + 0x50) = 0xeff0 + s;
      *reinterpret_cast<uintptr_t*>(shader[s] + 0xb0) = 0x7ec0 + s;
      *reinterpret_cast<uint8_t**>(shader[s] + 0xc8) = recs;
      *reinterpret_cast<uint8_t**>(shader[s] + 0xd0) = recs + sizeof(recs);
    }
    for (int h = 0; h < 4; ++h) {
      *reinterpret_cast<int32_t*>(recs + h * 0x50 + 0xc) = 7;
      *reinterpret_cast<const char**>(recs + h * 0x50 + 0x30) = kRecNames[h];
    }
    texVt[23] = reinterpret_cast<void*>(&FakeVt23);
    for (int t = 0; t < kTex; ++t) {
      *reinterpret_cast<void**>(tex[t]) = texVt;
      *reinterpret_cast<uint8_t**>(tex[t] + 0x10) = inner[t];
      *reinterpret_cast<void**>(inner[t]) = innerVt;
      *reinterpret_cast<uint8_t**>(tex[t] + 0x90) = views[t];  // aux 0: view at [+0x90] + 0x10
    }
    *reinterpret_cast<int32_t*>(g_desc + 0x20) = 28;
    g_mask = shadowtex::MaskEntry();
    g_mask.state = 1;
    g_mask.recCount = 4;
    g_mask.read[0] = 1;  // Diffuse only
    for (int m = 0; m < kMats; ++m) {
      uint8_t* mt = mat[m];
      *reinterpret_cast<void**>(mt) = modelVt;
      *reinterpret_cast<uint8_t**>(mt + 0x28) = props[m];
      *reinterpret_cast<uint8_t**>(mt + 0x30) = shader[ShaderOf(m)];
      *reinterpret_cast<uint64_t*>(mt + 0x210) = 1;
      *reinterpret_cast<uint64_t*>(mt + 0x218) = 2;
      for (uint32_t b = 0; b < kCbLen; b += 4) *reinterpret_cast<uint32_t*>(mt + kCbOff + b) = rnd() & 0x3f7fffff;
      *reinterpret_cast<float*>(mt + kCbOff + 0xc0) = 0.6f + 0.1f * m;  // "scale", read by the test VS
      *reinterpret_cast<int32_t*>(props[m] + 8) = m == kTexturedMat ? 1 : 0;
      props[m][0x33] = 0;
      *reinterpret_cast<uint32_t*>(mt + 0x2d8) = 2;   // two texture handles: Diffuse (0), Specular (1)
      *reinterpret_cast<int64_t*>(mt + 0x240) = 0;
      *reinterpret_cast<int64_t*>(mt + 0x248) = 1;
      *reinterpret_cast<uint32_t*>(props[m] + 0x26c) = 0;
    }
    for (int k = 0; k < kMeshes; ++k) {
      uint8_t* me = mesh[k];
      *reinterpret_cast<uint8_t**>(me) = ibObj[k];
      *reinterpret_cast<uint8_t**>(me + 0x18) = vbObj[k];
      *reinterpret_cast<uint32_t*>(ibObj[k] + 0x3c) = (k & 1) ? 4 : 2;
      *reinterpret_cast<uint32_t*>(vbObj[k] + 0x150) = 12;
      *reinterpret_cast<uint32_t*>(vbObj[k] + 0x2c) = 6;  // element format words (hashed only)
      *reinterpret_cast<uint32_t*>(vbObj[k] + 0xac) = 0;
      *reinterpret_cast<uint32_t*>(me + 0x58) = 1;
      *reinterpret_cast<uint32_t*>(me + 0x130) = 0;
      *reinterpret_cast<uint32_t*>(me + 0x208) = 1;
      *reinterpret_cast<uint32_t*>(me + 0x20c) = kPrimTriList;
      tris[k] = 10 + 4 * k;
      *reinterpret_cast<uint32_t*>(me + 0x210) = tris[k];
      SetMeshBuffers(k, reinterpret_cast<void*>(uintptr_t{0x1000} + k * 0x10),
                     reinterpret_cast<void*>(uintptr_t{0x2000} + k * 0x10));
    }
    globalsPtr = globalsObj;
    *reinterpret_cast<uint8_t**>(globalsObj + 0x80) = pageArr;
    for (int p = 0; p < kPages; ++p) {
      *reinterpret_cast<uint8_t**>(pageArr + 0x20 + p * 0x30) = gpuBuf[p];
      SetPageSrv(p, reinterpret_cast<void*>(uintptr_t{0x3000} + p * 0x10));
    }
    vec.clear();
    for (int i = 0; i < kObjs; ++i) {
      uint8_t* it = item[i];
      *reinterpret_cast<uint8_t**>(it + 0x10) = mat[MatOf(i)];
      *reinterpret_cast<uint8_t**>(it + 0xc0) = mesh[MeshOf(i)];
      *reinterpret_cast<uint32_t*>(it + 0xd0) = static_cast<uint32_t>(PageOf(i));
      *reinterpret_cast<uint32_t*>(it + 0xd4) = static_cast<uint32_t>((i * 7) % kPerPage);
      *reinterpret_cast<void**>(rend[i]) = (i % 23 == 5) ? static_cast<void*>(otherVt) : static_cast<void*>(smrVt);
      *reinterpret_cast<uint8_t**>(rend[i] + 0x10) = it;
      entries[i][0] = 0;  // aux, texture: Diffuse
      entries[i][1] = reinterpret_cast<uint64_t>(tex[DiffuseOf(i)]);
      entries[i][3] = 0;  // Specular
      entries[i][4] = reinterpret_cast<uint64_t>(tex[3]);
      arrPtr[i] = reinterpret_cast<uint8_t*>(entries[i]);
      *reinterpret_cast<uint8_t***>(it + 0x18) = &arrPtr[i];
      vec.push_back(rend[i]);
    }
    desc[0] = vec.data();
    desc[1] = vec.data() + vec.size();
    desc[2] = desc[1];
    env.smrVt = smrVt;
    env.modelVt = modelVt;
    env.shaderVt = shaderVt;
    env.globals = &globalsPtr;
    env.compiled = [](void*, uint64_t) { return true; };
    env.tex.texVtbl = texVt;
    env.tex.inner[0] = env.tex.inner[1] = env.tex.inner[2] = innerVt;
    env.tex.getDesc = &FakeGetDesc;
    env.tex.compat = &FakeCompat;
  }
};

// instcount::IsTextured through fakes: props+8 decides (no valid texture).
alignas(16) uint8_t g_texProps[16];
const void* __fastcall FakeGetTex(const void*, uint32_t) { return g_texProps; }
bool __fastcall FakeValid(const void*) { return false; }

struct TexFakes {
  instcount::GetTexFn g0 = instcount::g_getTex;
  instcount::ValidFn v0 = instcount::g_valid;
  TexFakes() {
    instcount::g_getTex = &FakeGetTex;
    instcount::g_valid = &FakeValid;
  }
  ~TexFakes() {
    instcount::g_getTex = g0;
    instcount::g_valid = v0;
  }
};

// A recordable key for shader s (fake or real objects), static facts of the test VS.
KeyStatic TestStatic() {
  KeyStatic st;
  st.cbSlot = 2;
  st.sbSlot = 3;
  st.cbBytes = kCbLen;
  st.psoOff = kPsoCb;
  MarkDwords(st.used, 0xc0, 16);  // "scale"
  return st;
}

KeyEntry* PublishTestKey(Tables& t, World& w, int s, int state) {
  KeyEntry* k = NewKey(t, w.shader[s], 1, 0);
  k->tech = 1;
  k->flags = 0;
  k->effect = *reinterpret_cast<void**>(w.shader[s] + 0x50);
  k->techBegin = *reinterpret_cast<void**>(w.shader[s] + 0xb0);
  k->st = TestStatic();
  k->guard[0] = k->st.used[0];
  k->guard[1] = k->st.used[1];
  k->state.store(state);
  PublishKey(t, *k, w.shader[s]);
  return k;
}

void PublishTestMesh(Tables& t, World& w, int s, int m, int state) {
  MeshLive live;
  ReadMesh(w.mesh[m], live);
  MeshEntry* e = NewMesh(t, w.mesh[m], w.shader[s], 1);
  e->shader = w.shader[s];
  e->tech = 1;
  e->effect = *reinterpret_cast<void**>(w.shader[s] + 0x50);
  e->techBegin = *reinterpret_cast<void**>(w.shader[s] + 0xb0);
  e->fp = live.fp;
  e->vb = live.vb;
  e->stride = live.stride;
  e->ib = live.ib;
  e->ibFormat = live.ibFormat;
  e->indexCount = 3 * live.count;
  e->state.store(state);
  PublishMesh(t, *e, w.mesh[m]);
}

ExecObj g_testExec = {nullptr, 0};

void InitJob(Job& j, World& w, const Tables& t) {
  j.vec = w.desc;
  j.execObj = &g_testExec;
  j.flags = 0;
  j.scope = kDefaultScope;
  j.env = &w.env;
  j.tab = &t;
}

// ---------------------------------------------------------------------------
// Pure tests
// ---------------------------------------------------------------------------
void MeshTests() {
  World* w = new World;
  w->Init();
  MeshLive a, b;
  bool ok = !ReadMesh(w->mesh[1], a) && a.ib == reinterpret_cast<ID3D11Buffer*>(0x1010) &&
            a.vb == reinterpret_cast<ID3D11Buffer*>(0x2010) && a.ibFormat == DXGI_FORMAT_R32_UINT && a.stride == 12 &&
            a.elems == 1 && a.prim == kPrimTriList && a.count == w->tris[1] && a.fp != 0;
  ok = ok && !ReadMesh(w->mesh[0], b) && b.ibFormat == DXGI_FORMAT_R16_UINT && b.fp != a.fp;
  Check(ok, "shadow rec: mesh fields read as DCS's draw reads them (buffers, index size, stride, prim, count)");
  // Every field DCS's draw depends on changes the fingerprint.
  uint8_t* me = w->mesh[2];
  MeshLive base;
  ReadMesh(me, base);
  struct Poke {
    uint8_t* p;
    uint32_t v;
  } pokes[] = {{me + 0x58, 2},          {me + 0x130, 1},          {w->vbObj[2] + 0x2c, 7}, {w->vbObj[2] + 0xac, 3},
               {w->vbObj[2] + 0x150, 16}, {me + 0x210, 99},       {w->ibObj[2] + 0x3c, 4}};
  bool fpOk = true;
  for (const Poke& k : pokes) {
    const uint32_t old = *reinterpret_cast<uint32_t*>(k.p);
    *reinterpret_cast<uint32_t*>(k.p) = k.v;
    MeshLive m;
    fpOk &= !ReadMesh(me, m) && m.fp != base.fp;
    *reinterpret_cast<uint32_t*>(k.p) = old;
  }
  MeshLive again;
  fpOk &= !ReadMesh(me, again) && again.fp == base.fp;
  Check(fpOk, "shadow rec: the mesh fingerprint covers elements, formats, stride, count and index size");
  // Not recordable meshes.
  bool rej = true;
  auto expectWhy = [&](uint8_t* p, uint32_t v) {
    const uint32_t old = *reinterpret_cast<uint32_t*>(p);
    *reinterpret_cast<uint32_t*>(p) = v;
    MeshLive m;
    rej &= ReadMesh(me, m) != nullptr;
    *reinterpret_cast<uint32_t*>(p) = old;
  };
  expectWhy(me + 0x20c, 5);    // triangle strip
  expectWhy(me + 0x208, 0);    // no elements
  expectWhy(me + 0x208, 28);   // arrays would overlap +0x208
  expectWhy(me + 0x130, 32);   // element index out of range
  expectWhy(me + 0x210, 0);    // no primitives
  {
    void* old = *reinterpret_cast<void**>(me);
    *reinterpret_cast<void**>(me) = nullptr;  // not indexed: DCS uses Draw
    MeshLive m;
    rej &= ReadMesh(me, m) != nullptr;
    *reinterpret_cast<void**>(me) = old;
  }
  MeshLive m0;
  rej &= ReadMeshGuarded(reinterpret_cast<uint8_t*>(16), m0) != nullptr;  // faults are caught
  Check(rej, "shadow rec: non-indexed, non-list, element-less and faulting meshes are not recordable");
  delete w;
}

void TableTests() {
  Tables t;
  Check(AllocTables(t), "shadow rec: tables allocated");
  World* w = new World;
  w->Init();
  PublishTestKey(t, *w, 0, 1);
  void* eff = *reinterpret_cast<void**>(w->shader[0] + 0x50);
  void* tb = *reinterpret_cast<void**>(w->shader[0] + 0xb0);
  bool ok = FindKey(t, w->shader[0], 1, 0, eff, tb) != nullptr && !FindKey(t, w->shader[0], 2, 0, eff, tb) &&
            !FindKey(t, w->shader[0], 1, 1, eff, tb) && !FindKey(t, w->shader[0], 1, 0, tb, tb) &&
            !FindKey(t, w->shader[1], 1, 0, eff, tb);
  PublishTestMesh(t, *w, 0, 3, 1);
  ok = ok && FindMesh(t, w->mesh[3], w->shader[0], 1, eff, tb) && !FindMesh(t, w->mesh[3], w->shader[1], 1, eff, tb) &&
       !FindMesh(t, w->mesh[2], w->shader[0], 1, eff, tb) && !FindMesh(t, w->mesh[3], w->shader[0], 1, eff, eff);
  Check(ok, "shadow rec: key and mesh lookups need the same shader, technique, flags and fingerprint");
  // The key table refuses inserts at 3/4.
  std::vector<uint8_t> fakes(kKeyTable);
  size_t added = 1;
  for (size_t i = 0; i < fakes.size(); ++i) {
    KeyEntry* k = NewKey(t, &fakes[i], 1, 0);
    if (!k) break;
    k->tech = 1;
    PublishKey(t, *k, &fakes[i]);
    ++added;
  }
  Check(added == kKeyTable * 3 / 4 && !NewKey(t, w->shader[1], 9, 0), "shadow rec: key table full at 3/4");
  FreeTables(t);
  Check(!t.keys && !t.meshes, "shadow rec: tables freed");
  bool scopeOk = InScope(kDefaultScope, 0) && !InScope(kDefaultScope, 1) && !InScope(0x100, 0) && !InScope(0x30f, 4) &&
                 !InScope(0x30f, -1);
  for (int c = 0; c < 4; ++c) scopeOk &= InScope(0x30f, c);
  Check(scopeOk && (0x30f & kScopeTextured) && (0x30f & kScopeUntextured) && !(kDefaultScope & kScopeTextured),
        "shadow rec: scope 0x101 = cascade 0 untextured, 0x30f = cascades 0-3, both kinds");
  delete w;
}

// HLSL like DCS's shadow VS: (b) for the reflection checks (qvPsoBase,
// qvInstOffsets), (a) for the device test (posStructOffset).
const char kVsB[] = R"(
cbuffer cPerView : register(b7) { float4x4 gViewProj; };
cbuffer def_uniforms : register(b2) {
  float4 pad0[12]; float4 scale; float4 pad1[2]; float3 pad2; uint qvPsoBase; float4 pad3[3];
#ifdef BIG
  float4 pad4;
#endif
};
StructuredBuffer<float4> sbPositions : register(t3);
StructuredBuffer<uint> qvInstOffsets : register(t127);
#ifdef EXTRA_TEX
Texture2D extraTex : register(t0);
SamplerState extraSamp : register(s0);
#endif
#ifdef EXTRA_CB
cbuffer extra : register(b3) { float4 e; };
#endif
float4 vs(float3 p : POSITION, uint id : SV_InstanceID) : SV_Position {
  float4 o = sbPositions[qvInstOffsets[qvPsoBase + id]];
  float3 w = p * o.w * scale.x + o.xyz;
#ifdef EXTRA_TEX
  w += extraTex.SampleLevel(extraSamp, p.xy, 0).xyz;
#endif
#ifdef EXTRA_CB
  w += e.xyz;
#endif
#ifdef BIG
  w += pad4.xyz;
#endif
  return mul(float4(w, 1), gViewProj);
}
)";

const char kVsA[] = R"(
cbuffer cPerView : register(b7) { float4x4 gViewProj; };
cbuffer def_uniforms : register(b2) {
  float4 pad0[12]; float4 scale; float4 pad1[2]; float3 pad2; uint posStructOffset; float4 pad3[3];
};
StructuredBuffer<float4> sbPositions : register(t3);
float4 vs(float3 p : POSITION) : SV_Position {
  float4 o = sbPositions[posStructOffset];
  float3 w = p * o.w * scale.x + o.xyz;
  return mul(float4(w, 1), gViewProj);
}
float4 vsOther(float3 p : POSITION) : SV_Position { return float4(p.xy, 0.2 + 0.6 * p.z, 1); }
float4 psAny(float4 pos : SV_Position) : SV_Target { return pos.z; }
// shadow_inst's instanced variants: posStructOffset holds the group base (qvPsoBase).
StructuredBuffer<uint> qvInstOffsets : register(t127);
float4 vsI(float3 p : POSITION, uint iid : SV_InstanceID) : SV_Position {
  float4 o = sbPositions[qvInstOffsets[posStructOffset + iid]];
  float3 w = p * o.w * scale.x + o.xyz;
  return mul(float4(w, 1), gViewProj);
}
struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VOut vsTex(float3 p : POSITION) {
  VOut o;
  float4 q = sbPositions[posStructOffset];
  float3 w = p * q.w * scale.x + q.xyz;
  o.pos = mul(float4(w, 1), gViewProj);
  o.uv = p.xy * 4 + 0.5;
  return o;
}
Texture2D Diffuse : register(t2);
SamplerState gAnisotropicWrapSampler : register(s15);
void psAlpha(VOut i) { clip(Diffuse.Sample(gAnisotropicWrapSampler, i.uv).a - 0.5); }
VOut vsTexI(float3 p : POSITION, uint iid : SV_InstanceID) {
  VOut o;
  float4 q = sbPositions[qvInstOffsets[posStructOffset + iid]];
  float3 w = p * q.w * scale.x + q.xyz;
  o.pos = mul(float4(w, 1), gViewProj);
  o.uv = p.xy * 4 + 0.5;
  return o;
}
)";

struct Compiler {
  decltype(&D3DCompile) compile = nullptr;
  decltype(&D3DReflect) reflect = nullptr;
  bool Load() {
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!dll) return false;
    compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dll, "D3DCompile"));
    reflect = reinterpret_cast<decltype(&D3DReflect)>(GetProcAddress(dll, "D3DReflect"));
    return compile && reflect;
  }
  ID3DBlob* Build(const char* src, size_t n, const char* entry, const char* target, const D3D_SHADER_MACRO* defs) {
    ID3DBlob *b = nullptr, *err = nullptr;
    if (FAILED(compile(src, n, "shrec", defs, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err))) {
      printf("     compile %s: %s\n", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
      b = nullptr;
    }
    if (err) err->Release();
    return b;
  }
};

void AnalyseTests(Compiler& c) {
  struct Case {
    const char* define;
    const char* cbName;
    UINT psoOff;
    bool ok;
    const char* what;
  } cases[] = {{nullptr, "def_uniforms", kPsoCb, true, "b7 + def_uniforms + sbPositions (+ qvInstOffsets)"},
               {"EXTRA_TEX", "def_uniforms", kPsoCb, false, "a texture and a sampler"},
               {"EXTRA_CB", "def_uniforms", kPsoCb, false, "another constant buffer"},
               {"BIG", "def_uniforms", kPsoCb, false, "def_uniforms of another size"},
               {nullptr, "other_uniforms", kPsoCb, false, "the material buffer under another name"},
               {nullptr, "def_uniforms", 0xf8, false, "posStructOffset elsewhere"}};
  bool all = true;
  KeyStatic good;
  for (const Case& k : cases) {
    const D3D_SHADER_MACRO defs[] = {{k.define, "1"}, {nullptr, nullptr}};
    ID3DBlob* b = c.Build(kVsB, sizeof(kVsB) - 1, "vs", "vs_5_0", k.define ? defs : nullptr);
    if (!b) {
      all = false;
      continue;
    }
    KeyStatic st;
    const char* why = AnalyseVs(c.reflect, b->GetBufferPointer(), b->GetBufferSize(), k.cbName, k.psoOff, st);
    const bool pass = (why == nullptr) == k.ok;
    if (!pass) printf("     analyse (%s): %s\n", k.what, why ? why : "accepted");
    all &= pass;
    if (k.ok && !why) good = st;
    b->Release();
  }
  Check(all, "shadow rec: VS reflection accepts only b7, def_uniforms (0x130, pso at +0xfc) and sbPositions");
  uint64_t want[2] = {};
  MarkDwords(want, 0xc0, 16);  // "scale"; posStructOffset (+0xfc) is excluded, the pads are unused
  Check(good.cbSlot == 2 && good.sbSlot == 3 && good.cbBytes == kCbLen && good.used[0] == want[0] &&
            good.used[1] == want[1],
        "shadow rec: reflection gives the CB and SRV slots and the read dwords (pso excluded)");
}

void ProbeCheckTests() {
  auto P = [](uintptr_t v) { return reinterpret_cast<void*>(v); };
  Capture c;
  c.draws = 1;
  c.vs = static_cast<ID3D11VertexShader*>(P(0x10));
  c.b7 = static_cast<ID3D11Buffer*>(P(0x20));
  c.cb = static_cast<ID3D11Buffer*>(P(0x30));
  c.sb = static_cast<ID3D11ShaderResourceView*>(P(0x40));
  c.prim = kPrimTriList;
  c.a5 = 12;
  c.topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  c.ib = static_cast<ID3D11Buffer*>(P(0x50));
  c.ibFormat = DXGI_FORMAT_R16_UINT;
  c.vb = static_cast<ID3D11Buffer*>(P(0x60));
  c.stride = 12;
  c.il = static_cast<ID3D11InputLayout*>(P(0x70));
  c.streamsOk = true;
  c.streamCount = 1;
  c.stream0 = P(0x90);
  c.descCount = 2;
  c.descSlot0 = true;
  ProbeFacts f;
  f.dcsVs = P(0x10);
  f.passB7 = c.b7;
  f.pageSrv = c.sb;
  f.mesh.vbObj = P(0x90);
  f.mesh.elems = 2;
  f.mesh.prim = kPrimTriList;
  f.mesh.count = 12;
  f.mesh.ib = c.ib;
  f.mesh.ibFormat = DXGI_FORMAT_R16_UINT;
  f.mesh.vb = c.vb;
  f.mesh.stride = 12;
  PsMap pm;
  bool ok = !CheckKey(c, f, &pm) && !CheckMesh(c, f);
  auto keyFails = [&](auto&& mutate) {
    Capture x = c;
    ProbeFacts y = f;
    mutate(x, y);
    PsMap q;
    return CheckKey(x, y, &q) != nullptr;
  };
  auto meshFails = [&](auto&& mutate) {
    Capture x = c;
    ProbeFacts y = f;
    mutate(x, y);
    return CheckMesh(x, y) != nullptr;
  };
  ok = ok && keyFails([&](Capture& x, ProbeFacts&) { x.ps = static_cast<ID3D11PixelShader*>(P(0x80)); });
  ok = ok && keyFails([&](Capture& x, ProbeFacts&) { x.gs = static_cast<ID3D11GeometryShader*>(P(0x80)); });
  ok = ok && keyFails([&](Capture&, ProbeFacts& y) { y.dcsVs = P(0x11); });
  ok = ok && keyFails([&](Capture&, ProbeFacts& y) { y.passB7 = static_cast<ID3D11Buffer*>(P(0x21)); });
  ok = ok && keyFails([&](Capture& x, ProbeFacts&) { x.cb = nullptr; });
  ok = ok && keyFails([&](Capture&, ProbeFacts& y) { y.pageSrv = static_cast<ID3D11ShaderResourceView*>(P(0x41)); });
  ok = ok && keyFails([&](Capture& x, ProbeFacts&) { x.classInstances = 1; });
  ok = ok && meshFails([&](Capture& x, ProbeFacts&) { x.a5 = 13; });
  ok = ok && meshFails([&](Capture& x, ProbeFacts&) { x.a4 = 1; });
  ok = ok && meshFails([&](Capture& x, ProbeFacts&) { x.instances = 4; });
  ok = ok && meshFails([&](Capture& x, ProbeFacts&) { x.topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; });
  ok = ok && meshFails([&](Capture& x, ProbeFacts&) { x.ibFormat = DXGI_FORMAT_R32_UINT; });
  ok = ok && meshFails([&](Capture& x, ProbeFacts&) { x.ibOffset = 2; });
  ok = ok && meshFails([&](Capture& x, ProbeFacts&) { x.vb = static_cast<ID3D11Buffer*>(P(0x61)); });
  ok = ok && meshFails([&](Capture& x, ProbeFacts&) { x.vbOffset = 4; });
  ok = ok && meshFails([&](Capture& x, ProbeFacts&) { x.streamCount = 2; });
  ok = ok && meshFails([&](Capture& x, ProbeFacts&) { x.streamCount = 0; });
  ok = ok && meshFails([&](Capture& x, ProbeFacts&) { x.streamsOk = false; });
  ok = ok && meshFails([&](Capture& x, ProbeFacts&) { x.stream0 = P(0x91); });
  ok = ok && meshFails([&](Capture& x, ProbeFacts&) { x.descCount = 1; });
  ok = ok && meshFails([&](Capture& x, ProbeFacts&) { x.descSlot0 = false; });
  ok = ok && meshFails([&](Capture& x, ProbeFacts&) { x.il = nullptr; });
  ok = ok && meshFails([&](Capture&, ProbeFacts& y) { y.meshWhy = "not indexed"; });
  Check(ok, "shadow rec: probe checks reject PS/GS, foreign VS, b7, CB, page view, draw args, buffers, streams, "
            "layout");
}

// The drawing shader's stream state as vt[38] 0x1dbc0 leaves it (the packing
// below is that function's), read by ReadStreams.
uint32_t PackDesc(uint32_t fmtA, uint32_t fmtB, uint32_t sem, uint32_t slot, bool inst) {
  const uint32_t hi = ((inst ? 0x8000u : 0u) | (slot & 0x1f)) << 16;
  return ((((fmtA & 0x7f) << 12) | (fmtB & 0x7f)) << 9) | (sem & 0x1ff) | hi;
}

void StreamTests() {
  alignas(16) static uint8_t sh[0x220];
  void* list[3] = {reinterpret_cast<void*>(uintptr_t{0xb0}), reinterpret_cast<void*>(uintptr_t{0xb1}), nullptr};
  auto setup = [&](uint64_t count, uint32_t descs, uint32_t slotOf2, bool inst2) {
    memset(sh, 0, sizeof(sh));
    *reinterpret_cast<void***>(sh + 0x1a8) = list;
    *reinterpret_cast<uint64_t*>(sh + 0x1b0) = count;
    *reinterpret_cast<uint32_t*>(sh + 0x110) = descs;
    for (uint32_t i = 0; i < descs; ++i)
      *reinterpret_cast<uint32_t*>(sh + 0x114 + 4 * i) =
          PackDesc(0x7f, 0x55, 0x1ff - i, i == 2 ? slotOf2 : 0, i == 2 && inst2);
  };
  Capture c;
  setup(1, 3, 0, false);
  bool ok = ReadStreamsGuarded(sh, c) && c.streamsOk && c.streamCount == 1 && c.stream0 == list[0] &&
            c.descCount == 3 && c.descSlot0;
  Capture c2;
  setup(2, 3, 1, false);  // the third element in a second buffer (slot 1)
  ok = ok && ReadStreamsGuarded(sh, c2) && c2.streamCount == 2 && !c2.descSlot0;
  Capture c3;
  setup(1, 3, 0, true);  // per-instance element
  ok = ok && ReadStreamsGuarded(sh, c3) && !c3.descSlot0;
  Capture c4;
  setup(0, 0, 0, false);  // after vt[6]: everything zeroed
  ok = ok && ReadStreamsGuarded(sh, c4) && c4.streamCount == 0 && !c4.descSlot0;
  Capture c5;
  ok = ok && !ReadStreamsGuarded(reinterpret_cast<uint8_t*>(uintptr_t{16}), c5) && !c5.streamsOk;
  Check(ok, "shadow rec: stream state read as vt[38] packs it (one buffer, slot 0, per-vertex; others rejected)");
}

void JobTests() {
  TexFakes fakes;
  World* w = new World;
  w->Init();
  Tables t;
  AllocTables(t);
  Job* j = NewJob();
  InitJob(*j, *w, t);
  // Nothing known yet: every untextured model caster asks for a probe, once per (shader, mesh).
  BuildJob(*j);
  bool ok = j->result == kBuildOk && j->n == kObjs && j->recCount == 0;
  uint32_t pending = 0, wants = 0, other = 0, textured = 0;
  std::set<std::pair<int, int>> pairs;
  for (int i = 0; i < kObjs; ++i) {
    const int r = j->reason[i];
    if (!w->Smr(i)) {
      other += r == kRNotSmr;
      continue;
    }
    if (World::MatOf(i) == kTexturedMat) {
      textured += r == kRTextured;
      continue;
    }
    pending += r == kRKeyPending;
    if (j->want[i]) {
      ++wants;
      ok = ok && pairs.insert({World::ShaderOf(World::MatOf(i)), World::MeshOf(i)}).second;
    }
  }
  ok = ok && wants == j->wantCount && wants == pairs.size() && wants == 2u * kMeshes && pending > 0 &&
       other == j->reasons[kRNotSmr] && textured == j->reasons[kRTextured] && pending == j->reasons[kRKeyPending];
  Check(ok, "shadow rec job: unknown keys ask for one probe per (shader, technique, mesh); others and textured are "
            "residual");
  // Not compiled by shadow_inst: residual, no probe.
  w->env.compiled = [](void*, uint64_t) { return false; };
  BuildJob(*j);
  Check(j->reasons[kRNoCompile] == pending && j->wantCount == 0, "shadow rec job: keys without a compile are residual");
  w->env.compiled = [](void*, uint64_t) { return true; };
  // Keys and meshes known; mesh 5 not recordable.
  for (int s = 0; s < 2; ++s) {
    PublishTestKey(t, *w, s, 1);
    for (int m = 0; m < kMeshes; ++m) PublishTestMesh(t, *w, s, m, m == 5 ? -1 : 1);
  }
  BuildJob(*j);
  FinishJob(*j, false);
  uint32_t expectRec = 0;
  bool reasonsOk = true, groupOk = true;
  std::map<std::tuple<int, int, int>, int> group;
  for (int i = 0; i < kObjs; ++i) {
    int want = kRecorded;
    if (!w->Smr(i))
      want = kRNotSmr;
    else if (World::MatOf(i) == kTexturedMat)
      want = kRTextured;
    else if (World::MeshOf(i) == 5)
      want = kRMeshRejected;
    expectRec += want == kRecorded;
    reasonsOk &= j->reason[i] == want;
    if (w->Smr(i)) {
      auto key = std::make_tuple(World::MatOf(i), World::MeshOf(i), World::PageOf(i));
      auto it = group.find(key);
      if (it == group.end())
        group[key] = j->reason[i];
      else
        groupOk &= it->second == j->reason[i];
    }
  }
  Check(reasonsOk && j->recCount == expectRec && j->reasons[kRecorded] == expectRec && j->wantCount == 0,
        "shadow rec job: known keys and meshes are recorded; rejected meshes and textured casters are residual");
  Check(groupOk, "shadow rec job: casters of one (material, mesh, page) share one outcome (batching groups stay whole)");
  // Records: vector order, pso, page view; materials: CB bytes, last caster (any reason).
  bool recOk = true;
  uint32_t prev = 0;
  for (uint32_t r = 0; r < j->recCount; ++r) {
    const Rec& rc = j->recs[r];
    const int i = static_cast<int>(rc.caster);
    recOk &= (r == 0 || rc.caster > prev) && rc.pso == static_cast<uint32_t>((i * 7) % kPerPage) &&
             rc.srv == reinterpret_cast<ID3D11ShaderResourceView*>(uintptr_t{0x3000} + World::PageOf(i) * 0x10) &&
             j->mats[rc.mat].mat == w->mat[World::MatOf(i)] && rc.mesh->indexCount == 3 * w->tris[World::MeshOf(i)];
    prev = rc.caster;
  }
  bool matOk = j->matCount == kMats - 1;
  for (uint32_t m = 0; m < j->matCount; ++m) {
    const RecMat& rm = j->mats[m];
    const int mi = static_cast<int>((rm.mat - w->mat[0]) / sizeof(w->mat[0]));
    int last = -1;
    for (int i = 0; i < kObjs; ++i)
      if (w->Smr(i) && World::MatOf(i) == mi) last = i;
    matOk &= memcmp(rm.cb, rm.mat + kCbOff, kCbLen) == 0 && rm.last == last;
  }
  // Groups: every recorded caster in exactly one, members share material,
  // mesh and page view; instanced only with a variant, from two members up.
  uint32_t members = 0;
  bool groupsOk = j->groupCount > 0 && j->groupCount < j->recorded;
  std::vector<uint8_t> seen(j->recCount, 0);
  for (uint32_t g = 0; g < j->groupCount; ++g) {
    const Group& gr = j->groups[g];
    const Rec& f = j->recs[gr.first];
    for (uint32_t r = gr.first, k = 0; k < gr.count; ++k, r = j->recNext[r]) {
      const Rec& m = j->recs[r];
      groupsOk &= m.alive && m.mat == f.mat && m.mesh == f.mesh && m.srv == f.srv && !seen[r];
      seen[r] = 1;
      ++members;
    }
    groupsOk &= !gr.instVs;  // no variant in this environment
  }
  groupsOk &= members == j->recorded;
  Check(groupsOk, "shadow rec job: records grouped by material, mesh and page view; each in one group");
  Check(recOk && matOk && j->pageCount == kPages,
        "shadow rec job: records keep vector order, pso and page view; materials keep their CB bytes and last caster");
  // Pass checks.
  Check(CheckJobRaw(*j) == kPExecuted, "shadow rec pass: an unchanged frame passes the checks");
  std::swap(w->vec[10], w->vec[11]);
  const int idChk = CheckJobRaw(*j);
  std::swap(w->vec[10], w->vec[11]);
  w->desc[1] = w->vec.data() + w->vec.size() - 1;
  const int lenChk = CheckJobRaw(*j);
  w->desc[1] = w->vec.data() + w->vec.size();
  w->SetPageSrv(1, reinterpret_cast<void*>(0x9999));
  const int pageChk = CheckJobRaw(*j);
  w->SetPageSrv(1, reinterpret_cast<void*>(0x3010));
  uint8_t* m2 = w->mat[2];
  const uint32_t pad = *reinterpret_cast<uint32_t*>(m2 + kCbOff + 0x10);
  *reinterpret_cast<uint32_t*>(m2 + kCbOff + 0x10) = pad ^ 1;  // not read by the VS
  *reinterpret_cast<uint32_t*>(m2 + 0x18c) = 0x77;              // posStructOffset: rewritten per caster anyway
  const int unusedChk = CheckJobRaw(*j);
  *reinterpret_cast<uint32_t*>(m2 + kCbOff + 0x10) = pad;
  const uint32_t scaleY = *reinterpret_cast<uint32_t*>(m2 + kCbOff + 0xc4);
  *reinterpret_cast<uint32_t*>(m2 + kCbOff + 0xc4) = scaleY ^ 0x10;  // read by the VS ("scale".y: the variable is used)
  const int guardChk = CheckJobRaw(*j);
  *reinterpret_cast<uint32_t*>(m2 + kCbOff + 0xc4) = scaleY;
  Check(idChk == kPIdentity && lenChk == kPIdentity && pageChk == kPPage && unusedChk == kPExecuted &&
            guardChk == kPGuard && CheckJobRaw(*j) == kPExecuted,
        "shadow rec pass: vector, page view and VS-read CB dwords are checked; unread dwords and the pso are not");
  // mat+0x18c as the stock loop leaves it.
  for (int m = 0; m < kMats; ++m) *reinterpret_cast<uint32_t*>(w->mat[m] + 0x18c) = 0xdead;
  RestoreMatsRaw(*j);
  bool restoreOk = true;
  for (uint32_t m = 0; m < j->matCount; ++m) {
    const RecMat& rm = j->mats[m];
    restoreOk &= *reinterpret_cast<uint32_t*>(rm.mat + 0x18c) == *reinterpret_cast<uint32_t*>(w->item[rm.last] + 0xd4);
  }
  restoreOk &= *reinterpret_cast<uint32_t*>(w->mat[kTexturedMat] + 0x18c) == 0xdead;  // DCS drew it: not ours
  Check(restoreOk, "shadow rec pass: mat+0x18c restored to the last caster's [item+0xd4]");
  // A mesh changed since its probe; scope without untextured casters; oversized vectors.
  *reinterpret_cast<uint32_t*>(w->mesh[0] + 0x210) += 1;
  BuildJob(*j);
  uint32_t changed = 0, onMesh0 = 0;
  for (int i = 0; i < kObjs; ++i)
    if (w->Smr(i) && World::MatOf(i) != kTexturedMat && World::MeshOf(i) == 0) {
      changed += j->reason[i] == kRMeshChanged;
      ++onMesh0;
    }
  *reinterpret_cast<uint32_t*>(w->mesh[0] + 0x210) -= 1;
  j->scope = 0x001;
  BuildJob(*j);
  const uint32_t scoped = j->reasons[kRScope];
  j->scope = kDefaultScope;
  Check(changed > 0 && changed == onMesh0 && scoped > 0 && j->recCount == 0,
        "shadow rec job: changed meshes and casters outside the scope are residual");
  std::vector<void*> big(kMaxCasters + 1, w->rend[0]);
  void* bigDesc[3] = {big.data(), big.data() + big.size(), big.data() + big.size()};
  j->vec = bigDesc;
  Check(BuildJob(*j) == kBuildOversize, "shadow rec job: oversized vectors are not built");
  j->vec = w->desc;
  FreeJob(j);
  FreeTables(t);
  delete w;
}

// ---------------------------------------------------------------------------
// Device test: stock draws vs probe, job, record, execute
// ---------------------------------------------------------------------------
struct Dev {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11VertexShader* vsOther = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11InputLayout* il = nullptr;
  ID3D11DepthStencilState* dss = nullptr;
  ID3D11RasterizerState* rs[2] = {};
  ID3D11BlendState* bs = nullptr;
  ID3D11Buffer* dcsB7 = nullptr;
  ID3D11Buffer* ourB7 = nullptr;
  ID3D11Buffer* matCb[kMats] = {};
  ID3D11Buffer* page[kPages] = {};
  ID3D11ShaderResourceView* pageSrv[kPages] = {};
  ID3D11Buffer* vb[kMeshes] = {};
  ID3D11Buffer* ib[kMeshes] = {};
  ID3D11Buffer* otherVb = nullptr;
  ID3D11Texture2D* depth = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11Texture2D* staging = nullptr;
  KeyStatic st;
  // Textured (alpha-tested) casters: DCS shader 2's VS and PS, sampler, Diffuse textures.
  bool textured = false;  // DrawDcs draws material 7 the alpha-tested way
  ID3D11VertexShader* vsTex = nullptr;
  ID3D11PixelShader* psTex = nullptr;
  ID3D11SamplerState* samp = nullptr;
  ID3D11Texture2D* texRes[3] = {};
  ID3D11ShaderResourceView* texSrv[3] = {};
  // Four cascades.
  ID3D11Texture2D* depthC[4] = {};
  ID3D11DepthStencilView* dsvC[4] = {};
  ID3D11Buffer* ourB7C[4] = {};
  // Instancing: the instanced VS variants and a t127 buffer per worker.
  ID3D11VertexShader* vsI = nullptr;
  ID3D11VertexShader* vsTexI = nullptr;
  ID3D11Buffer* offBuf[kThreads * kSlots] = {};
  ID3D11ShaderResourceView* offSrv[kThreads * kSlots] = {};
};

// The instanced variant of a key's VS (what shadow_inst's map gives the recorder).
Dev* g_instDev = nullptr;
ID3D11VertexShader* FakeInstVs(const KeyEntry& k) {
  if (!g_instDev) return nullptr;
  if (k.vs == g_instDev->vs) return g_instDev->vsI;
  if (k.vs == g_instDev->vsTex) return g_instDev->vsTexI;
  return nullptr;
}

template <class T>
void Rel(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

void Destroy(Dev& d) {
  Rel(d.vsTex), Rel(d.psTex), Rel(d.samp), Rel(d.vsI), Rel(d.vsTexI);
  for (auto*& p : d.offBuf) Rel(p);
  for (auto*& p : d.offSrv) Rel(p);
  for (auto*& p : d.texRes) Rel(p);
  for (auto*& p : d.texSrv) Rel(p);
  for (auto*& p : d.depthC) Rel(p);
  for (auto*& p : d.dsvC) Rel(p);
  for (auto*& p : d.ourB7C) Rel(p);
  Rel(d.vs), Rel(d.vsOther), Rel(d.ps), Rel(d.il), Rel(d.dss), Rel(d.rs[0]), Rel(d.rs[1]), Rel(d.bs);
  Rel(d.dcsB7), Rel(d.ourB7), Rel(d.otherVb), Rel(d.depth), Rel(d.dsv), Rel(d.staging);
  for (auto*& p : d.matCb) Rel(p);
  for (auto*& p : d.page) Rel(p);
  for (auto*& p : d.pageSrv) Rel(p);
  for (auto*& p : d.vb) Rel(p);
  for (auto*& p : d.ib) Rel(p);
  Rel(d.imm), Rel(d.dev);
}

ID3D11Buffer* MakeBuffer(ID3D11Device* dev, UINT bytes, UINT bind, D3D11_USAGE usage, const void* init,
                         UINT misc = 0, UINT stride = 0) {
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = bytes;
  bd.Usage = usage;
  bd.BindFlags = bind;
  bd.CPUAccessFlags = usage == D3D11_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : 0;
  bd.MiscFlags = misc;
  bd.StructureByteStride = stride;
  D3D11_SUBRESOURCE_DATA sd = {init, 0, 0};
  ID3D11Buffer* b = nullptr;
  dev->CreateBuffer(&bd, init ? &sd : nullptr, &b);
  return b;
}

bool CreateDev(Dev& d, Compiler& c, World& w, bool* warp) {
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  *warp = false;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2, D3D11_SDK_VERSION, &d.dev,
                               nullptr, &d.imm))) {
    *warp = true;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 2, D3D11_SDK_VERSION, &d.dev, nullptr,
                                 &d.imm)))
      return false;
  }
  ID3DBlob* b = c.Build(kVsA, sizeof(kVsA) - 1, "vs", "vs_5_0", nullptr);
  if (!b) return false;
  d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &d.vs);
  const D3D11_INPUT_ELEMENT_DESC el[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  d.dev->CreateInputLayout(el, 1, b->GetBufferPointer(), b->GetBufferSize(), &d.il);
  b->Release();
  if (!(b = c.Build(kVsA, sizeof(kVsA) - 1, "vsOther", "vs_5_0", nullptr))) return false;
  d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &d.vsOther);
  b->Release();
  if (!(b = c.Build(kVsA, sizeof(kVsA) - 1, "psAny", "ps_5_0", nullptr))) return false;
  d.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &d.ps);
  b->Release();
  // The static facts the recorder would take from (b): reflection of kVsB.
  if (!(b = c.Build(kVsB, sizeof(kVsB) - 1, "vs", "vs_5_0", nullptr))) return false;
  const char* why = AnalyseVs(c.reflect, b->GetBufferPointer(), b->GetBufferSize(), "def_uniforms", kPsoCb, d.st);
  b->Release();
  if (why) return false;
  D3D11_DEPTH_STENCIL_DESC dd = {};
  dd.DepthEnable = TRUE;
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dd.DepthFunc = D3D11_COMPARISON_GREATER;
  d.dev->CreateDepthStencilState(&dd, &d.dss);
  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.SlopeScaledDepthBias = -1.0f;
  rd.DepthClipEnable = FALSE;
  d.dev->CreateRasterizerState(&rd, &d.rs[0]);
  rd.CullMode = D3D11_CULL_BACK;
  d.dev->CreateRasterizerState(&rd, &d.rs[1]);
  D3D11_BLEND_DESC bdsc = {};
  bdsc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  d.dev->CreateBlendState(&bdsc, &d.bs);
  const float ident[64] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // 256 bytes, like a per-view buffer
  d.dcsB7 = MakeBuffer(d.dev, sizeof(ident), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT, ident);
  d.ourB7 = MakeBuffer(d.dev, sizeof(ident), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT, nullptr);  // zeros
  for (auto*& p : d.matCb) p = MakeBuffer(d.dev, kCbLen, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, nullptr);
  uint32_t rng = 77;
  auto rnd = [&](float a, float bb) {
    rng = rng * 1664525u + 1013904223u;
    return a + (bb - a) * static_cast<float>(rng >> 8) / 16777216.0f;
  };
  for (int p = 0; p < kPages; ++p) {
    std::vector<float> pos(kPerPage * 4);
    for (int k = 0; k < kPerPage; ++k) {
      pos[k * 4 + 0] = rnd(-0.75f, 0.75f);
      pos[k * 4 + 1] = rnd(-0.75f, 0.75f);
      pos[k * 4 + 2] = rnd(0.15f, 0.85f);
      pos[k * 4 + 3] = rnd(0.5f, 1.5f);
    }
    d.page[p] = MakeBuffer(d.dev, kPerPage * 16, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, pos.data(),
                           D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.NumElements = kPerPage;
    if (d.page[p]) d.dev->CreateShaderResourceView(d.page[p], &sd, &d.pageSrv[p]);
    w.SetPageSrv(p, d.pageSrv[p]);
  }
  for (int m = 0; m < kMeshes; ++m) {
    const int verts = 24;
    std::vector<float> v(verts * 3);
    for (float& x : v) x = rnd(-0.12f, 0.12f);
    d.vb[m] = MakeBuffer(d.dev, verts * 12, D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, v.data());
    const uint32_t n = 3 * w.tris[m];
    std::vector<uint32_t> i32(n);
    std::vector<uint16_t> i16(n);
    for (uint32_t k = 0; k < n; ++k) i32[k] = i16[k] = static_cast<uint16_t>(rnd(0, verts - 0.01f));
    const bool r32 = (m & 1) != 0;
    d.ib[m] = MakeBuffer(d.dev, n * (r32 ? 4 : 2), D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_DEFAULT,
                         r32 ? static_cast<const void*>(i32.data()) : static_cast<const void*>(i16.data()));
    w.SetMeshBuffers(m, d.ib[m], d.vb[m]);
  }
  const float tri[] = {-0.9f, -0.9f, 0.3f, 0.9f, -0.8f, 0.9f, -0.2f, 0.9f, 0.1f};
  d.otherVb = MakeBuffer(d.dev, sizeof(tri), D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, tri);
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = td.Height = kDepth;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R32_TYPELESS;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  d.dev->CreateTexture2D(&td, nullptr, &d.depth);
  D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {};
  dvd.Format = DXGI_FORMAT_D32_FLOAT;
  dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
  if (d.depth) d.dev->CreateDepthStencilView(d.depth, &dvd, &d.dsv);
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.dev->CreateTexture2D(&td, nullptr, &d.staging);
  bool ok = d.vs && d.vsOther && d.ps && d.il && d.dss && d.rs[0] && d.rs[1] && d.bs && d.dcsB7 && d.ourB7 &&
            d.otherVb && d.dsv && d.staging;
  for (int m = 0; m < kMeshes; ++m) ok = ok && d.vb[m] && d.ib[m];
  for (int p = 0; p < kPages; ++p) ok = ok && d.pageSrv[p];
  for (auto* p : d.matCb) ok = ok && p;
  // Alpha-tested casters.
  if (!(b = c.Build(kVsA, sizeof(kVsA) - 1, "vsTex", "vs_5_0", nullptr))) return false;
  d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &d.vsTex);
  b->Release();
  if (!(b = c.Build(kVsA, sizeof(kVsA) - 1, "psAlpha", "ps_5_0", nullptr))) return false;
  d.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &d.psTex);
  b->Release();
  D3D11_SAMPLER_DESC sdsc = {};
  sdsc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sdsc.AddressU = sdsc.AddressV = sdsc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  sdsc.MaxLOD = D3D11_FLOAT32_MAX;
  d.dev->CreateSamplerState(&sdsc, &d.samp);
  for (int t = 0; t < 3; ++t) {
    uint32_t px[16 * 16];
    for (uint32_t& v : px) v = (rnd(0, 1) < 0.6f ? 0xff000000u : 0u) | 0x00808080u;
    D3D11_TEXTURE2D_DESC tdsc = {};
    tdsc.Width = tdsc.Height = 16;
    tdsc.MipLevels = tdsc.ArraySize = 1;
    tdsc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tdsc.SampleDesc.Count = 1;
    tdsc.Usage = D3D11_USAGE_DEFAULT;
    tdsc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {px, 16 * 4, 0};
    d.dev->CreateTexture2D(&tdsc, &sd, &d.texRes[t]);
    if (d.texRes[t]) d.dev->CreateShaderResourceView(d.texRes[t], nullptr, &d.texSrv[t]);
    w.SetView(t, d.texSrv[t]);
    ok = ok && d.texSrv[t];
  }
  for (int c4 = 0; c4 < 4; ++c4) {
    D3D11_TEXTURE2D_DESC td2 = {};
    td2.Width = td2.Height = kDepth;
    td2.MipLevels = td2.ArraySize = 1;
    td2.Format = DXGI_FORMAT_R32_TYPELESS;
    td2.SampleDesc.Count = 1;
    td2.Usage = D3D11_USAGE_DEFAULT;
    td2.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    d.dev->CreateTexture2D(&td2, nullptr, &d.depthC[c4]);
    D3D11_DEPTH_STENCIL_VIEW_DESC dv2 = {};
    dv2.Format = DXGI_FORMAT_D32_FLOAT;
    dv2.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (d.depthC[c4]) d.dev->CreateDepthStencilView(d.depthC[c4], &dv2, &d.dsvC[c4]);
    d.ourB7C[c4] = MakeBuffer(d.dev, 256, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT, nullptr);
    ok = ok && d.dsvC[c4] && d.ourB7C[c4];
  }
  if (!(b = c.Build(kVsA, sizeof(kVsA) - 1, "vsI", "vs_5_0", nullptr))) return false;
  d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &d.vsI);
  b->Release();
  if (!(b = c.Build(kVsA, sizeof(kVsA) - 1, "vsTexI", "vs_5_0", nullptr))) return false;
  d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &d.vsTexI);
  b->Release();
  for (int k = 0; k < kThreads * kSlots; ++k) ok = ok && CreateOffsets(d.dev, &d.offBuf[k], &d.offSrv[k]);
  return ok && d.vsTex && d.psTex && d.samp && d.vsI && d.vsTexI;
}

// DCS's cascade pass setup: frame buffer (no RTV, the depth view, viewport),
// clear to 0, the binder's per-view b7.
const D3D11_VIEWPORT kVpFull = {0, 0, static_cast<float>(kDepth), static_cast<float>(kDepth), 0, 1};
void PassSetup(Dev& d, ID3D11DepthStencilView* dsv = nullptr) {
  ID3D11DeviceContext* c = d.imm;
  if (!dsv) dsv = d.dsv;
  c->ClearState();
  c->OMSetRenderTargets(0, nullptr, dsv);
  c->RSSetViewports(1, &kVpFull);
  c->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
  ID3D11Buffer* b7 = d.dcsB7;  // the binder binds b7 on every stage
  c->VSSetConstantBuffers(kB7, 1, &b7);
  c->PSSetConstantBuffers(kB7, 1, &b7);
}

// What DCS's slot 5, submit and draw leave bound for caster i, and the draw.
void DrawDcs(Dev& d, World& w, int i, ID3D11PixelShader* ps = nullptr) {
  ID3D11DeviceContext* c = d.imm;
  const int m = World::MatOf(i), k = World::MeshOf(i), p = World::PageOf(i);
  uint8_t* mt = w.mat[m];
  *reinterpret_cast<uint32_t*>(mt + 0x18c) = *reinterpret_cast<uint32_t*>(w.item[i] + 0xd4);
  D3D11_MAPPED_SUBRESOURCE ms;
  if (SUCCEEDED(c->Map(d.matCb[m], 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
    memcpy(ms.pData, mt + kCbOff, kCbLen);
    c->Unmap(d.matCb[m], 0);
  }
  const bool alpha = d.textured && m == kTexturedMat;
  if (alpha) {
    // Slot 5's texture loop: a streaming request per set (Diffuse, Specular),
    // the read one bound by Apply (PS t2), plus the PS's sampler and CB.
    FakeVt23(w.tex[World::DiffuseOf(i)], kTexSize);
    FakeVt23(w.tex[3], kTexSize);
    ps = d.psTex;
    c->PSSetShaderResources(2, 1, &d.texSrv[World::DiffuseOf(i)]);
    c->PSSetSamplers(15, 1, &d.samp);
    c->PSSetConstantBuffers(2, 1, &d.matCb[m]);
  }
  c->VSSetShader(alpha ? d.vsTex : d.vs, nullptr, 0);
  c->PSSetShader(ps, nullptr, 0);
  c->OMSetDepthStencilState(d.dss, 0);
  c->OMSetBlendState(d.bs, nullptr, 0xffffffff);
  c->RSSetState(d.rs[World::ShaderOf(m) == 0 ? 0 : 1]);
  c->IASetInputLayout(d.il);
  const UINT stride = 12, zero = 0;
  c->IASetVertexBuffers(0, 1, &d.vb[k], &stride, &zero);
  c->IASetIndexBuffer(d.ib[k], (k & 1) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c->VSSetConstantBuffers(2, 1, &d.matCb[m]);
  c->VSSetShaderResources(3, 1, &d.pageSrv[p]);
  c->DrawIndexed(3 * w.tris[k], 0, 0);
}

// A caster of another class (terrain-like), drawn by DCS in both runs.
void DrawOther(Dev& d) {
  ID3D11DeviceContext* c = d.imm;
  c->VSSetShader(d.vsOther, nullptr, 0);
  c->PSSetShader(nullptr, nullptr, 0);
  c->OMSetDepthStencilState(d.dss, 0);
  c->RSSetState(d.rs[0]);
  c->IASetInputLayout(d.il);
  const UINT stride = 12, zero = 0;
  c->IASetVertexBuffers(0, 1, &d.otherVb, &stride, &zero);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c->Draw(3, 0);
}

std::vector<uint32_t> ReadDepth(Dev& d, ID3D11Texture2D* depth = nullptr) {
  std::vector<uint32_t> out(static_cast<size_t>(kDepth) * kDepth);
  d.imm->CopyResource(d.staging, depth ? depth : d.depth);
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(d.imm->Map(d.staging, 0, D3D11_MAP_READ, 0, &m))) return {};
  for (UINT y = 0; y < kDepth; ++y)
    memcpy(&out[static_cast<size_t>(y) * kDepth], static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
           kDepth * 4);
  d.imm->Unmap(d.staging, 0);
  return out;
}

// Probes caster i on the immediate context as the render thread does: DCS's
// draw, then the read-back, the checks and the publish.
void ProbeOn(Dev& d, World& w, Tables& t, int i, ID3D11PixelShader* ps = nullptr) {
  DrawDcs(d, w, i, ps);
  Capture c;
  CaptureState(d.imm, d.st.cbSlot, d.st.sbSlot, c);
  c.draws = 1;
  c.prim = static_cast<int>(kPrimTriList);
  c.a4 = 0;
  c.a5 = static_cast<int>(w.tris[World::MeshOf(i)]);
  c.instances = 0;
  c.flags = 0;
  c.dbg = 0;
  c.rendererOk = true;
  ProbeIn in;
  in.item = w.item[i];
  in.mat = w.mat[World::MatOf(i)];
  in.shader = w.shader[World::ShaderOf(World::MatOf(i))];
  in.tech = 1;
  in.mesh = w.mesh[World::MeshOf(i)];
  in.page = static_cast<uint32_t>(World::PageOf(i));
  in.effect = *reinterpret_cast<void**>(in.shader + 0x50);
  in.techBegin = *reinterpret_cast<void**>(in.shader + 0xb0);
  ProbeFacts f;
  const bool alpha = d.textured && World::MatOf(i) == kTexturedMat;
  f.dcsVs = alpha ? d.vsTex : d.vs;
  f.passB7 = d.dcsB7;
  f.pageSrv = PageSrv(w.env, in.page);
  f.meshWhy = ReadMesh(in.mesh, f.mesh);
  if (alpha) ReadSetsRaw(w.env.tex, in, g_mask, f);  // the views slot 26 bound, as TryProbe reads them
  // The drawing shader's stream state as vt[38] leaves it for the draw (the
  // mesh's buffer object alone, its element at slot 0), read as DrawAfter does.
  static void* list[1];
  list[0] = f.mesh.vbObj;
  *reinterpret_cast<void***>(in.shader + 0x1a8) = list;
  *reinterpret_cast<uint64_t*>(in.shader + 0x1b0) = 1;
  *reinterpret_cast<uint32_t*>(in.shader + 0x110) = f.mesh.elems;
  for (uint32_t e = 0; e < f.mesh.elems; ++e)
    *reinterpret_cast<uint32_t*>(in.shader + 0x114 + 4 * e) = PackDesc(6, 0, 1 + e, 0, false);
  ReadStreams(in.shader, c);
  ProcessProbe(t, in, d.st, f, c);
  ReleaseCapture(c);
}

void SortSiteTests() {
  std::vector<uint8_t> img(0x30000, 0x90);
  uint8_t* b = img.data();
  memcpy(b + kSortRva, kSortFnBytes, sizeof(kSortFnBytes));
  for (const SortSite& s : kSortSites) {
    memcpy(b + s.rva - 8, s.before, 8);
    CallBytes(b + s.rva, b + kSortRva, b + s.rva);
    memcpy(b + s.rva + 5, s.after, 8);
  }
  bool ok = CheckSortSites(b) == nullptr;
  b[kSortSites[1].rva + 6] ^= 1;  // the second site's following code differs
  ok = ok && CheckSortSites(b) != nullptr;
  b[kSortSites[1].rva + 6] ^= 1;
  CallBytes(b + kSortSites[2].rva, b + kSortRva + 16, b + kSortSites[2].rva);  // calls something else
  ok = ok && CheckSortSites(b) != nullptr;
  CallBytes(b + kSortSites[2].rva, b + kSortRva, b + kSortSites[2].rva);
  b[kSortRva + 3] ^= 1;  // the sort function differs
  ok = ok && CheckSortSites(b) != nullptr;
  b[kSortRva + 3] ^= 1;
  ok = ok && CheckSortSites(b) == nullptr;
  Check(ok, "shadow rec sort hook: the three Scene call sites are checked byte for byte, with their call target");
  // An armed job starts once (sort hook or render entry, whichever is first);
  // an unarmed or aborted one never.
  Job* j = NewJob();
  void* v[3] = {};
  bool st = j && !StartJob(*j, v, true);
  if (j) {
    j->startState.store(kStartArmed);
    st = st && StartJob(*j, v, true) && j->vec == v && j->startedEarly && !StartJob(*j, v, false) && j->tStart;
    j->startState.store(kStartAbort);
    st = st && !StartJob(*j, v, false);
    j->startState.store(kStartIdle);
    FreeJob(j);
  }
  Check(st, "shadow rec start: an armed job starts exactly once, never when idle or aborted");
}

void DeviceTests(Compiler& comp) {
  TexFakes fakes;
  World* w = new World;
  w->Init();
  Dev d;
  bool warp = false;
  if (!CreateDev(d, comp, *w, &warp)) {
    Check(false, "shadow rec device: test objects");
    Destroy(d);
    delete w;
    return;
  }
  printf("     shadow rec device test on %s\n", warp ? "WARP" : "the hardware device");
  // A pixel shader bound on a caster that sets no read texture: inconclusive
  // (its PS slots cannot be learned from it), nothing is published.
  {
    Tables t0;
    AllocTables(t0);
    PassSetup(d);
    ProbeOn(d, *w, t0, 0, d.ps);
    const KeyEntry* k = FindKey(t0, w->shader[0], 1, 0, *reinterpret_cast<void**>(w->shader[0] + 0x50),
                                *reinterpret_cast<void**>(w->shader[0] + 0xb0));
    Check(!k && t0.keysUsed == 0 && t0.meshesUsed == 0 && g_probeInconclusive.load() > 0,
          "shadow rec device: a PS-bound probe of an untextured caster publishes nothing (inconclusive)");
    FreeTables(t0);
  }
  // Stock: every caster drawn by DCS, in vector order.
  PassSetup(d);
  for (int i = 0; i < kObjs; ++i)
    if (w->Smr(i))
      DrawDcs(d, *w, i);
    else
      DrawOther(d);
  const std::vector<uint32_t> ref = ReadDepth(d);
  size_t written = 0;
  for (uint32_t v : ref) written += v != 0;
  // Probes: the first caster of every (shader, mesh), as the probe path learns them.
  Tables t;
  AllocTables(t);
  PassSetup(d);
  std::set<std::pair<int, int>> probed;
  for (int i = 0; i < kObjs; ++i)
    if (w->Smr(i) && World::MatOf(i) != kTexturedMat &&
        probed.insert({World::ShaderOf(World::MatOf(i)), World::MeshOf(i)}).second)
      ProbeOn(d, *w, t, i);
  Check(t.keysUsed == 2 && t.meshesUsed == 2u * kMeshes && g_keysOk.load() >= 2,
        "shadow rec device: probes publish both keys and every mesh");
  const KeyEntry* k0 = FindKey(t, w->shader[0], 1, 0, *reinterpret_cast<void**>(w->shader[0] + 0x50),
                               *reinterpret_cast<void**>(w->shader[0] + 0xb0));
  Check(k0 && k0->state.load() > 0 && k0->vs == d.vs && k0->dss == d.dss && k0->rs == d.rs[0] && k0->bs == d.bs &&
            k0->st.cbSlot == 2 && k0->st.sbSlot == 3,
        "shadow rec device: a key holds DCS's VS and state objects and the reflected slots");
  // Job on the worker pool.
  defrec::Pool pool;
  defrec::PoolConfig pc;
  pc.workers = 1;
  pc.cb.mode = defrec::CbMode::kOffsets;
  pc.name = "shadow rec test";
  Job* j = NewJob();
  if (!pool.Start(d.dev, pc) || pool.At(0).ring.Mode() != defrec::CbMode::kOffsets || !j) {
    printf("SKIP shadow rec device: no worker with constant-buffer offsets on this device\n");
  } else {
    InitJob(*j, *w, t);
    g_instDev = &d;
    w->env.instVs = &FakeInstVs;
    j->offBuf[0] = d.offBuf[0];
    j->offSrv[0] = d.offSrv[0];
    j->dsv = d.dsv;
    j->dsv->AddRef();
    j->nvp = 1;
    j->vp[0] = kVpFull;
    j->nsc = 0;
    j->b7 = d.ourB7;
    j->pool[15] = d.samp;  // DCS's pool: the textured PS's sampler at s15
    j->pool[15]->AddRef();
    j->startState.store(kStartArmed);
    const bool sub = pool.Submit(0, &JobMain, j, nullptr);
    const bool started = StartJob(*j, j->vec, false) && !StartJob(*j, j->vec, false);  // the first start wins
    const bool waited = sub && started && pool.Wait(10000);
    ID3D11CommandList* cl = pool.TakeList(0);
    uint32_t expectRec = 0;
    for (int i = 0; i < kObjs; ++i) expectRec += w->Smr(i) && World::MatOf(i) != kTexturedMat;
    g_instDev = nullptr;
    Check(sub && waited && cl && j->recCount == expectRec && j->recorded == expectRec && j->recCount > 0 &&
              CheckJobRaw(*j) == kPExecuted && j->draws < j->recorded,
          "shadow rec device: the worker builds and records every untextured model caster, groups instanced");
    printf("     %u draws for %u recorded casters in %u groups\n", j->draws, j->recorded, j->groupCount);
    // Recorded: DCS's pass, the list executed at the first ShadowMapRenderable
    // caster (our b7 late-copied), DCS draws the residual casters around it.
    PassSetup(d);
    bool executed = false, restored = true;
    for (int i = 0; i < kObjs && cl; ++i) {
      if (w->Smr(i) && !executed) {
        defrec::PassState before = {}, after = {};
        defrec::Capture(d.imm, &before);
        d.imm->CopyResource(d.ourB7, d.dcsB7);
        d.imm->ExecuteCommandList(cl, TRUE);
        defrec::Capture(d.imm, &after);
        restored = defrec::Equal(before, after);
        defrec::Release(before);
        defrec::Release(after);
        executed = true;
      }
      if (j->reason[i] == kRecorded) continue;
      if (w->Smr(i))
        DrawDcs(d, *w, i);
      else
        DrawOther(d);
    }
    const std::vector<uint32_t> got = ReadDepth(d);
    size_t diff = ref.size() == got.size() ? 0 : SIZE_MAX;
    for (size_t k = 0; diff != SIZE_MAX && k < ref.size(); ++k) diff += ref[k] != got[k];
    printf("     %u casters recorded of %d, %zu texels written, %zu differ\n", j->recorded, kObjs, written, diff);
    Check(executed && restored && diff == 0 && written > kDepth * kDepth / 50,
          "shadow rec device: recorded + residual depth equals DCS's stock draws bit for bit; state restored");
    // Without the late b7 copy the list draws with our (zero) buffer: the check above depends on it.
    PassSetup(d);
    float zeros[64] = {};
    d.imm->UpdateSubresource(d.ourB7, 0, nullptr, zeros, 0, 0);
    d.imm->ExecuteCommandList(cl, TRUE);
    for (int i = 0; i < kObjs; ++i)
      if (j->reason[i] != kRecorded) {
        if (w->Smr(i))
          DrawDcs(d, *w, i);
        else
          DrawOther(d);
      }
    const std::vector<uint32_t> noCopy = ReadDepth(d);
    size_t diff2 = 0;
    for (size_t k = 0; k < ref.size() && k < noCopy.size(); ++k) diff2 += ref[k] != noCopy[k];
    Check(diff2 > 0, "shadow rec device: (control) without the b7 copy the depth differs");
    SafeRel(cl);
  }
  pool.Stop();
  FreeJob(j);
  FreeTables(t);
  Destroy(d);
  delete w;
}

// ---------------------------------------------------------------------------
// S4: PS mapping, textured jobs, snapshot, streaming replay
// ---------------------------------------------------------------------------
void PsMapTests() {
  auto P = [](uintptr_t v) { return reinterpret_cast<void*>(v); };
  Capture c;
  c.vs = static_cast<ID3D11VertexShader*>(P(0x10));
  c.b7 = static_cast<ID3D11Buffer*>(P(0x20));
  c.cb = static_cast<ID3D11Buffer*>(P(0x30));
  c.sb = static_cast<ID3D11ShaderResourceView*>(P(0x40));
  c.ps = static_cast<ID3D11PixelShader*>(P(0x80));
  c.psSrv[5] = static_cast<ID3D11ShaderResourceView*>(P(0xa0));  // Diffuse at t5
  c.psSrv[9] = static_cast<ID3D11ShaderResourceView*>(P(0xa8));  // an earlier draw's view, unread
  c.psCb[2] = c.cb;
  c.psCb[kB7] = c.b7;
  ProbeFacts f;
  f.dcsVs = P(0x10);
  f.passB7 = c.b7;
  f.pageSrv = c.sb;
  f.readCount = 1;
  f.readH[0] = 3;
  f.readView[0] = P(0xa0);
  PsMap m;
  bool ok = !CheckKey(c, f, &m) && m.count == 1 && m.h[0] == 3 && m.slot[0] == 5 && m.cbMask == (1u << 2);
  auto why = [&](auto&& mutate) {
    Capture x = c;
    ProbeFacts y = f;
    mutate(x, y);
    PsMap q;
    return CheckKey(x, y, &q);
  };
  ok = ok && why([](Capture&, ProbeFacts& y) { y.readCount = 0; }) == kInconclusive;   // untextured caster
  ok = ok && why([](Capture&, ProbeFacts& y) { y.readOk = false; }) == kInconclusive;  // unpredictable view
  ok = ok && why([](Capture&, ProbeFacts& y) { y.readView[0] = nullptr; }) == kInconclusive;
  ok = ok && why([](Capture& x, ProbeFacts&) { x.psSrv[9] = x.psSrv[5]; }) == kInconclusive;  // two slots
  const char* w1 = why([](Capture& x, ProbeFacts&) { x.psSrv[5] = nullptr; });  // not bound at all
  const char* w2 = why([](Capture&, ProbeFacts& y) { y.namesWhy = "x"; });
  const char* w3 = why([&](Capture& x, ProbeFacts&) { x.psCb[kB7] = static_cast<ID3D11Buffer*>(P(0x21)); });
  const char* w4 = why([](Capture& x, ProbeFacts&) { x.ps = nullptr; });  // read textures without a PS
  ok = ok && w1 && w1 != kInconclusive && w2 && w3 && w3 != kInconclusive && w4 && w4 != kInconclusive;
  Check(ok, "shadow rec S4: PS texture slots found by the view DCS bound; ambiguous or untextured probes are "
            "inconclusive; foreign names, PS b7 and texture reads without a PS reject the key");
}

// Helpers per job and chunk sizes (pure).
void SchedulingTests() {
  bool ok = HelpersFor(0, false, 3) == 3 && HelpersFor(100, false, 3) == 1 && HelpersFor(399, false, 3) == 1 &&
            HelpersFor(400, false, 3) == 2 && HelpersFor(1499, false, 3) == 2 && HelpersFor(1500, false, 3) == 3 &&
            HelpersFor(100, true, 3) == 2 && HelpersFor(5000, true, 3) == 3 && HelpersFor(5000, false, 1) == 1 &&
            HelpersFor(5000, true, 0) == 0 && HelpersFor(5000, false, 9) == static_cast<uint32_t>(kThreads - 1);
  ok = ok && ChunkFor(0, 1) == kMinChunk && ChunkFor(10, 4) == kMinChunk && ChunkFor(1200, 4) == 1200 / 24 &&
       ChunkFor(1200, 2) == 100 && ChunkFor(1200, 0) == 200;
  Check(ok, "shadow rec scheduling: helpers by caster count (+1 after a late or tight job, capped), chunks of a "
            "phase about 6 per thread");
}

uint32_t CountReason(const Job& j, int r) {
  uint32_t n = 0;
  for (size_t i = 0; i < j.n; ++i) n += j.reason[i] == r;
  return n;
}

int g_vt18Calls = 0;
void __fastcall FakeVt18(void*) { ++g_vt18Calls; }

void TextureJobTests() {
  TexFakes fakes;
  World* w = new World;
  w->Init();
  w->env.maskFor = &FakeMaskFor;
  FakeView fv[kTex];
  for (int t = 0; t < kTex; ++t) w->SetView(t, static_cast<IUnknown*>(&fv[t]));
  FakeView fakePs;
  Tables t;
  AllocTables(t);
  for (int s = 0; s < 3; ++s) {
    KeyEntry* k = PublishTestKey(t, *w, s, 1);
    for (int m = 0; m < kMeshes; ++m) PublishTestMesh(t, *w, s, m, 1);
    if (s == 2) {  // alpha-tested: the PS reads Diffuse (record 0) at t2
      k->ps = reinterpret_cast<ID3D11PixelShader*>(static_cast<IUnknown*>(&fakePs));
      fakePs.AddRef();
      k->psTexCount = 1;
      k->psTexH[0] = 0;
      k->psTexSlot[0] = 2;
      AllCbDwords(k->guard);
    }
  }
  Job* j = NewJob();
  InitJob(*j, *w, t);
  j->scope = 0x30f;
  // Stage A: textured casters keyed, waiting for the snapshot.
  BuildJob(*j);
  uint32_t tex = 0;
  for (int i = 0; i < kObjs; ++i) tex += w->Smr(i) && World::MatOf(i) == kTexturedMat;
  bool ok = j->needGo && j->textured == tex && j->texKeyCount == 4 && j->reasons[kRecorded] == j->recCount;
  uint32_t reads = 0;
  for (uint32_t k = 0; k < j->texKeyCount; ++k) reads += j->texKeys[k].read;
  ok = ok && reads == 3;  // the three Diffuse textures; Specular is skipped (its streaming request only)
  Check(ok, "shadow rec S4 job: textured casters collect their (texture, aux, type) keys, read and skipped");
  // Another build of the same vector classifies exactly as j.
  auto sameAs = [&](const Job& j2) {
    const uint32_t n = static_cast<uint32_t>(j2.n);
    bool same = n == j->n && j2.result == kBuildOk && j2.recCount == j->recCount && j2.setCount == j->setCount &&
                j2.texKeyCount == j->texKeyCount && j2.matCount == j->matCount && j2.needGo == j->needGo &&
                memcmp(j2.reason, j->reason, n) == 0 && memcmp(j2.group, j->group, n * sizeof(uint64_t)) == 0 &&
                memcmp(j2.setKeys, j->setKeys, j->setCount * sizeof(uint16_t)) == 0 &&
                memcmp(j2.reasons, j->reasons, sizeof(j->reasons)) == 0;
    for (uint32_t r = 0; same && r < j->recCount; ++r) {
      const Rec &a = j->recs[r], &b = j2.recs[r];
      same = a.key == b.key && a.mesh == b.mesh && a.srv == b.srv && a.caster == b.caster && a.mat == b.mat &&
             a.pso == b.pso && a.setFirst == b.setFirst && a.setCount == b.setCount && a.tex == b.tex &&
             memcmp(a.psKey, b.psKey, sizeof(a.psKey)) == 0;
    }
    for (uint32_t m = 0; same && m < j->matCount; ++m)
      same = j->mats[m].mat == j2.mats[m].mat && j->mats[m].last == j2.mats[m].last &&
             memcmp(j->mats[m].cb, j2.mats[m].cb, kCbLen) == 0;
    for (uint32_t k = 0; same && k < j->texKeyCount; ++k)
      same = j->texKeys[k].tex == j2.texKeys[k].tex && j->texKeys[k].aux == j2.texKeys[k].aux &&
             j->texKeys[k].type == j2.texKeys[k].type && j->texKeys[k].read == j2.texKeys[k].read;
    return same;
  };
  {
    // Stage A's reads split over two threads (chunks alternating, taken in
    // reverse order): the commit gives the single-thread result exactly.
    Job* j2 = NewJob();
    bool same = j2 != nullptr;
    if (j2) {
      InitJob(*j2, *w, t);
      j2->scope = 0x30f;
      same = BeginBuild(*j2) == kBuildOk && j2->n == j->n;
      const uint32_t n = static_cast<uint32_t>(j2->n);
      for (uint32_t c = (n + kPreChunk - 1) / kPreChunk; same && c-- > 0;)
        for (uint32_t i = c * kPreChunk; i < n && i < (c + 1) * kPreChunk; ++i) PreOne(*j2, c & 1, i);
      j2->preDone.store(n);
      same = same && WaitPre(*j2) && CommitBuild(*j2) == kBuildOk && j2->preSetCount[1] > 0 && sameAs(*j2);
      FreeJob(j2);
    }
    Check(same, "shadow rec stage A: reads split over two threads commit to the single-thread classification");
  }
  {
    // The pipelined commit (PreAndCommit), deterministic: helpers 1-3 have
    // read chunks 1.. from the back; the primary reads chunk 0 itself (it is
    // not ready), then commits every chunk in vector order.
    Job* j2 = NewJob();
    bool same = j2 != nullptr;
    if (j2) {
      InitJob(*j2, *w, t);
      j2->scope = 0x30f;
      same = BeginBuild(*j2) == kBuildOk;
      const uint32_t n = static_cast<uint32_t>(j2->n);
      const uint32_t chunks = (n + kPreChunk - 1) / kPreChunk;
      for (uint32_t c = chunks; same && c-- > 1;) PreChunk(*j2, 1 + static_cast<int>(c % 3), c * kPreChunk);
      same = same && chunks > 3 && PreAndCommit(*j2) && j2->th[0].pre == kPreChunk &&
             j2->preSetCount[1] + j2->preSetCount[2] + j2->preSetCount[3] > 0 && sameAs(*j2);
      FreeJob(j2);
    }
    // Live threads: three helpers read while the primary commits (and reads), many interleavings.
    for (int rep = 0; rep < 64 && same; ++rep) {
      Job* j3 = NewJob();
      if (!j3) {
        same = false;
        break;
      }
      InitJob(*j3, *w, t);
      j3->scope = 0x30f;
      same = BeginBuild(*j3) == kBuildOk;
      std::atomic<bool> go{false};
      std::thread hs[kThreads - 1];
      for (int h = 1; h < kThreads; ++h)
        hs[h - 1] = std::thread([&, h] {
          while (!go.load()) _mm_pause();
          PreChunks(*j3, h);
        });
      go = true;
      same = PreAndCommit(*j3) && same;
      for (auto& th : hs) th.join();
      same = same && sameAs(*j3) && j3->commitUs >= 0;
      FreeJob(j3);
    }
    Check(same, "shadow rec stage A: the commit pipelined with the reads (helpers 1-3 reading chunks while the "
                "primary commits in vector order) gives the single-thread classification, deterministic and threaded");
  }
  // Snapshot (render thread) and stage B.
  TakeSnapshot(*j);
  bool held = true;
  for (int tt = 0; tt < 3; ++tt) held &= fv[tt].refs == 2;
  held &= fv[3].refs == 1;  // skipped: no view taken
  FinishJob(*j, true);
  bool views = j->vt23Count == 4;
  uint32_t texRecs = 0;
  for (uint32_t r = 0; r < j->recCount; ++r) {
    const Rec& rc = j->recs[r];
    if (!rc.setCount) continue;
    ++texRecs;
    views &= static_cast<void*>(rc.psView[0]) ==
             static_cast<void*>(static_cast<IUnknown*>(&fv[World::DiffuseOf(static_cast<int>(rc.caster))]));
  }
  Check(held && views && texRecs == tex,
        "shadow rec S4 job: the snapshot references each read view once; records bind their Diffuse view");
  // Streaming requests: one per texture per cascade, the same textures stock requests.
  std::map<void*, int> stock;
  for (uint32_t r = 0; r < j->recCount; ++r) {
    const int i = static_cast<int>(j->recs[r].caster);
    if (World::MatOf(i) != kTexturedMat) continue;
    ++stock[w->tex[World::DiffuseOf(i)]];
    ++stock[w->tex[3]];
  }
  g_vt23.clear();
  const uint32_t replays = ReplayStreamingRaw(*j);
  bool replayOk = replays == stock.size() && g_vt23.size() == stock.size();
  for (const auto& kv : stock) replayOk &= g_vt23[kv.first] == 1 && kv.second >= 1;
  Check(replayOk, "shadow rec S4 pass: one streaming request per recorded texture (stock makes one per set)");
  // Pass checks: unchanged; a read view changed; a skipped texture's class changed.
  const int same = CheckTexturesRaw(*j);
  w->SetView(1, static_cast<IUnknown*>(&fv[4]));
  const int viewChg = CheckTexturesRaw(*j);
  w->SetView(1, static_cast<IUnknown*>(&fv[1]));
  void* innerVt0 = *reinterpret_cast<void**>(w->inner[3]);
  *reinterpret_cast<void**>(w->inner[3]) = w->otherVt;
  const int classChg = CheckTexturesRaw(*j);
  *reinterpret_cast<void**>(w->inner[3]) = innerVt0;
  Check(same == kPExecuted && viewChg == kPTexture && classChg == kPTexture,
        "shadow rec S4 pass: a changed read view or skipped-texture class sends the pass stock");
  ReleaseSnapshot(*j);
  bool released = true;
  for (int tt = 0; tt < kTex; ++tt) released &= fv[tt].refs == 1;
  Check(released, "shadow rec S4: snapshot references released");
  // Snapshot fallback: Diffuse 0 streamed (aux -1) with a swap due -> its casters residual.
  alignas(16) static uint8_t setS[0x50], setP[0x50];
  memset(setS, 0, sizeof(setS));
  memset(setP, 0, sizeof(setP));
  *reinterpret_cast<int32_t*>(setS + 0x40) = 3;
  *reinterpret_cast<int32_t*>(setP + 0x40) = 5;  // back set ready: getSRV would swap
  *reinterpret_cast<void**>(setP + 0x20) = setP;
  *reinterpret_cast<uint8_t**>(w->tex[0] + 0x198) = setS;
  *reinterpret_cast<uint8_t**>(w->tex[0] + 0x1a0) = setP;
  uint32_t onTex0 = 0;
  for (int i = 0; i < kObjs; ++i)
    if (World::DiffuseOf(i) == 0) {
      w->entries[i][0] = ~0ull;  // aux -1: the streamed mip sets
      onTex0 += w->Smr(i) && World::MatOf(i) == kTexturedMat;
    }
  BuildJob(*j);
  TakeSnapshot(*j);
  FinishJob(*j, true);
  Check(onTex0 > 0 && CountReason(*j, kRTexView) == onTex0 && j->vt23Count == 3,
        "shadow rec S4 snapshot: a texture with a swap due makes its casters residual (DCS draws them)");
  ReleaseSnapshot(*j);
  for (int i = 0; i < kObjs; ++i) w->entries[i][0] = 0;
  // Group classes: one caster's skipped texture of an unreplicated class
  // makes its batching group (same material, mesh, page, read textures) residual.
  int i0 = -1, partner = -1;
  for (int a = 0; a < kObjs && partner < 0; ++a) {
    if (!w->Smr(a) || World::MatOf(a) != kTexturedMat) continue;
    for (int b = a + 1; b < kObjs; ++b)
      if (w->Smr(b) && World::MatOf(b) == kTexturedMat && World::MeshOf(b) == World::MeshOf(a) &&
          World::PageOf(b) == World::PageOf(a) && World::DiffuseOf(b) == World::DiffuseOf(a)) {
        i0 = a;
        partner = b;
        break;
      }
  }
  bool groupOk = i0 >= 0;
  if (groupOk) {
    *reinterpret_cast<void**>(w->inner[4]) = w->otherVt;
    w->entries[i0][4] = reinterpret_cast<uint64_t>(w->tex[4]);
    BuildJob(*j);
    const int stageA = j->reason[partner];
    TakeSnapshot(*j);
    FinishJob(*j, true);
    groupOk = j->reason[i0] == kRTexClass && stageA == kRecorded && j->reason[partner] == kRGroup;
    ReleaseSnapshot(*j);
    w->entries[i0][4] = reinterpret_cast<uint64_t>(w->tex[3]);
  }
  Check(groupOk, "shadow rec S4: a residual caster makes its whole batching group residual (no leader/member split)");
  // The list handed to DCS's loop: [exec entry, every residual caster in vector order].
  BuildJob(*j);
  TakeSnapshot(*j);
  FinishJob(*j, true);
  bool list = j->swapCount == 1 + (j->n - j->recorded) && j->swapList[0] == j->execObj;
  size_t pos = 1;
  for (size_t i = 0; i < j->n && list; ++i)
    if (j->reason[i] != kRecorded) list = j->swapList[pos++] == j->snap[i];
  Check(list, "shadow rec S6: the loop's list is the exec entry, then the residual casters in their order");
  ReleaseSnapshot(*j);
  // Untextured casters under a key whose PS reads a texture: residual.
  KeyEntry* k1 = const_cast<KeyEntry*>(FindKey(t, w->shader[1], 1, 0, *reinterpret_cast<void**>(w->shader[1] + 0x50),
                                               *reinterpret_cast<void**>(w->shader[1] + 0xb0)));
  k1->psTexCount = 1;
  BuildJob(*j);
  uint32_t on1 = 0;
  for (int i = 0; i < kObjs; ++i) on1 += w->Smr(i) && World::ShaderOf(World::MatOf(i)) == 1;
  Check(CountReason(*j, kRTexMissing) == on1,
        "shadow rec S4: an untextured caster whose key's PS reads a texture is residual");
  k1->psTexCount = 0;
  // Without the snapshot (timeout / abort) the textured casters are residual, the rest recorded.
  BuildJob(*j);
  FinishJob(*j, false);
  Check(CountReason(*j, kRTexView) == tex && j->recorded > 0 && j->texRecorded == 0,
        "shadow rec S4: no snapshot -> textured casters residual");
  // Texture table (production): stage A takes the read views from the table's
  // entries (no snapshot); misses are built by the render thread after the
  // pass; the exec check compares the entries with the live textures.
  {
    TexTable tab;
    AllocTex(tab, 64);
    j->tex = &tab;
    j->useGen = 5;
    BuildJob(*j);
    ResolveTexKeys(*j);
    bool tb = j->resolved && j->missCount == 3;  // the three Diffuse textures; Specular needs no view
    for (uint32_t k = 0; k < j->missCount; ++k) {
      bool fresh = false;
      TexEntry* e = TexInsert(tab, j->misses[k].tex, j->misses[k].aux, j->misses[k].type, &fresh);
      tb = tb && e && fresh && TexBuildRaw(w->env.tex, *e) == kTwOk;
      if (e) e->dirty.store(0);
    }
    BuildJob(*j);
    ResolveTexKeys(*j);
    FinishJob(*j, true);
    tb = tb && j->missCount == 0 && j->refreshCount == 0 && j->texRecorded == tex;
    for (uint32_t r = 0; r < j->recCount; ++r) {
      const Rec& rc = j->recs[r];
      if (!rc.setCount || !rc.alive) continue;
      tb = tb && static_cast<void*>(rc.psView[0]) ==
                     static_cast<void*>(static_cast<IUnknown*>(&fv[World::DiffuseOf(static_cast<int>(rc.caster))]));
    }
    for (int tt = 0; tt < 3; ++tt) tb = tb && fv[tt].refs == 2;  // the table's references only
    tb = tb && CheckTexturesRaw(*j) == kPExecuted;
    w->SetView(1, static_cast<IUnknown*>(&fv[4]));
    const TexEntry* e1 = TexFind(tab, w->tex[1], 0, j->texKeys[0].type);
    tb = tb && CheckTexturesRaw(*j) == kPTexture && e1 && e1->dirty.load() == 1;
    w->SetView(1, static_cast<IUnknown*>(&fv[1]));
    if (e1) const_cast<TexEntry*>(e1)->dirty.store(0);  // as its rebuild would leave it
    // A render-target texture (unread Specular here): accepted once its class is
    // known; slot 26's vt[18] is replayed at the exec entry.
    void* rtVt[12] = {};
    g_vt18Calls = 0;
    w->texVt[18] = reinterpret_cast<void*>(&FakeVt18);
    void* inner3 = *reinterpret_cast<void**>(w->inner[3]);
    *reinterpret_cast<void**>(w->inner[3]) = rtVt;
    const bool refused = !TexClassOk(w->env.tex, w->tex[3]);
    const void* was = g_innerRt;
    g_innerRt = rtVt;
    BuildJob(*j);
    ResolveTexKeys(*j);
    FinishJob(*j, true);
    uint32_t calls = 0;
    tb = tb && refused && TexClassOk(w->env.tex, w->tex[3]) && j->texRecorded == tex &&
         ReplayRtGuarded(*j, &calls) && calls == 1 && g_vt18Calls == 1;
    TexEntry probe;
    probe.tex = w->tex[3];
    probe.aux = 0;
    probe.type = j->texKeys[0].type;
    tb = tb && TexBuildRaw(w->env.tex, probe) == kTwOk && probe.rt == 1;
    TexReleaseViews(probe);
    g_innerRt = was;
    *reinterpret_cast<void**>(w->inner[3]) = inner3;
    w->texVt[18] = nullptr;
    j->tex = nullptr;
    FreeTex(tab);
    for (int tt = 0; tt < kTex; ++tt) tb = tb && fv[tt].refs == 1;
    Check(tb, "shadow rec texture table: read views from the table without a snapshot, misses built after the pass, "
              "a changed view caught at exec; a render-target texture accepted with its vt[18] replayed");
  }
  FreeJob(j);
  FreeTables(t);  // releases the fake PS
  delete w;
}

// ---------------------------------------------------------------------------
// S4 + S5 + S6 on a device: four cascades on four workers, textured and
// untextured casters, DCS's loop over the exec list
// ---------------------------------------------------------------------------
void DeviceMultiTests(Compiler& comp) {
  TexFakes fakes;
  World* w = new World;
  w->Init();
  w->env.maskFor = &FakeMaskFor;
  Dev d;
  bool warp = false;
  if (!CreateDev(d, comp, *w, &warp)) {
    Check(false, "shadow rec S5 device: test objects");
    Destroy(d);
    delete w;
    return;
  }
  d.textured = true;
  // Cascade c: the casters i with i % 4 == c.
  std::vector<void*> vec[4];
  void* desc[4][3];  // contiguous like RenderGraph's per-collection array at [rg+0xa18]
  alignas(16) static uint8_t fakeRg[0xa20];
  *reinterpret_cast<void**>(fakeRg + 0xa18) = desc;
  for (int i = 0; i < kObjs; ++i) vec[i % 4].push_back(w->rend[i]);
  for (int c = 0; c < 4; ++c) {
    desc[c][0] = vec[c].data();
    desc[c][1] = desc[c][2] = vec[c].data() + vec[c].size();
  }
  auto indexOf = [&](void* r) {
    return static_cast<int>((static_cast<uint8_t*>(r) - w->rend[0]) / sizeof(w->rend[0]));
  };
  // Probes: the first caster of every (shader, mesh), textured ones included.
  Tables t;
  AllocTables(t);
  PassSetup(d);
  std::set<std::pair<int, int>> probed;
  g_countVt23 = false;
  for (int i = 0; i < kObjs; ++i)
    if (w->Smr(i) && probed.insert({World::ShaderOf(World::MatOf(i)), World::MeshOf(i)}).second) ProbeOn(d, *w, t, i);
  g_countVt23 = true;
  const KeyEntry* k2 = FindKey(t, w->shader[2], 1, 0, *reinterpret_cast<void**>(w->shader[2] + 0x50),
                               *reinterpret_cast<void**>(w->shader[2] + 0xb0));
  Check(t.keysUsed == 3 && t.meshesUsed == 3u * kMeshes && k2 && k2->state.load() > 0 && k2->ps == d.psTex &&
            k2->psTexCount == 1 && k2->psTexH[0] == 0 && k2->psTexSlot[0] == 2 && k2->psSamp[15] == d.samp &&
            (k2->psCbMask & (1u << 2)),
        "shadow rec S4 device: the alpha-tested key maps Diffuse to PS t2, its sampler s15 and its CB slot");
  defrec::Pool pool;
  defrec::PoolConfig pc;
  pc.workers = kThreads * kSlots;  // Wk(c, t): cascade c's primary (t 0) and helpers
  pc.cb.mode = defrec::CbMode::kOffsets;
  pc.name = "shadow rec test (4 cascades)";
  g_instDev = &d;
  w->env.instVs = &FakeInstVs;
  Job* jobs[4] = {};
  ExecObj exec[4] = {};
  bool started = pool.Start(d.dev, pc) && pool.At(0).ring.Mode() == defrec::CbMode::kOffsets;
  for (int c = 0; c < 4 && started; ++c) started = (jobs[c] = NewJob()) != nullptr;
  TexTable stab;
  SRWLOCK stabLock = SRWLOCK_INIT;
  AllocTex(stab, 64);
  if (!started) {
    printf("SKIP shadow rec S5 device: no workers with constant-buffer offsets on this device\n");
  } else {
    bool sub = true;
    // The texture table as the render thread leaves it (production path: no snapshot).
    for (int tx = 0; tx < kTex; ++tx) {
      bool fresh = false;
      TexEntry* e = TexInsert(stab, w->tex[tx], 0, 7, &fresh);
      if (e) TexBuildRaw(w->env.tex, *e);
      if (e) e->dirty.store(0);
    }
    for (int c = 0; c < 4; ++c) {
      Job& j = *jobs[c];
      InitJob(j, *w, t);
      j.tex = &stab;
      j.texLock = &stabLock;
      j.vec = nullptr;  // given by the sort hook below
      j.armRg = fakeRg;
      j.armIdx = c;
      j.startState.store(kStartArmed);
      g_casc[c].job = &j;
      j.scope = 0x30f;
      j.dsv = d.dsvC[c];
      j.dsv->AddRef();
      j.nvp = 1;
      j.vp[0] = kVpFull;
      j.b7 = d.ourB7C[c];
      ReleasePool(j.pool);  // DCS's pool: the textured PS's sampler at s15
      j.pool[15] = d.samp;
      j.pool[15]->AddRef();
      exec[c] = {nullptr, c};
      j.execObj = &exec[c];
      for (int th = 0; th < kThreads; ++th) {
        j.offBuf[th] = d.offBuf[Wk(c, th)];
        j.offSrv[th] = d.offSrv[Wk(c, th)];
      }
      // Cascade 0 alone, 1 with one helper, 2 with three, 3 with two (only 3 has textured groups).
      static const uint32_t kHelpers[4] = {0, 1, 3, 2};
      j.helpers = kHelpers[c];
      j.splitAllowed = j.helpers > 0;
      j.minSplit = 2;
      ResetEvents(j);
      j.preState.store(kGoWait);
      for (uint32_t th = 1; th <= j.helpers; ++th) sub &= pool.Submit(Wk(c, th), kHelperMain[th], &j, nullptr);
      sub &= pool.Submit(c, &JobMain, &j, nullptr);
    }
    g_testP1Wait = true;  // the helpers take a share of the untextured groups (deterministic)
    // Scene's sort finishing each collection (pass order c3..c0) starts its job;
    // another collection's vector starts nothing.
    g_on = false;  // off: Scene's sorts start nothing
    OnSorted(desc[0]);
    g_on = true;
    OnSorted(reinterpret_cast<void**>(fakeRg));
    bool early = true;
    for (int c = 0; c < 4; ++c) early &= jobs[c]->startState.load() == kStartArmed;
    for (int c = 3; c >= 0; --c) OnSorted(desc[c]);
    g_on = false;
    for (int c = 0; c < 4; ++c) {
      early &= jobs[c]->vec == desc[c] && jobs[c]->startedEarly && jobs[c]->startState.load() == kStartRun;
      g_casc[c].job = nullptr;
    }
    Check(early, "shadow rec S5 device: the sort hook starts each armed job on its own collection's vector only");
    // Render thread: each job's snapshot once its keys are final.
    for (int c = 0; c < 4 && sub; ++c) {
      Job& j = *jobs[c];
      for (int spin = 0; spin < 5000 && pool.Busy(c) && !j.stageA.load(); ++spin) Sleep(1);
      if (j.stageA.load() && j.needGo) TakeSnapshot(j);
    }
    pool.Wait(10000);  // false when a helper had nothing to record: check the workers instead
    g_testP1Wait = false;
    bool waited = sub;
    for (int c = 0; c < kThreads * kSlots; ++c) waited &= !pool.Busy(c);
    ID3D11CommandList* cl[4][kThreads] = {};
    for (int c = 0; c < 4; ++c)
      for (int th = 0; th < kThreads; ++th) cl[c][th] = pool.TakeList(Wk(c, th));
    bool allOk = waited, replayOk = true, restored = true;
    size_t diffs = 0, written = 0;
    uint32_t recorded = 0, texRecorded = 0, draws = 0, split = 0, lists = 0;
    std::vector<uint32_t> refs[4];
    for (int c = 0; c < 4 && waited; ++c) {
      Job& j = *jobs[c];
      const uint32_t joined = j.joined.load();
      allOk &= cl[c][0] != nullptr && j.recorded > 0;
      for (int th = 1; th < kThreads; ++th)  // a list exactly for each helper that took a chunk
        allOk &= (((joined >> th) & 1) != 0) == (cl[c][th] != nullptr) && (cl[c][th] != nullptr) == j.th[th].drew;
      for (auto* l : cl[c]) lists += l != nullptr;
      recorded += j.recorded;
      texRecorded += j.texRecorded;
      draws += j.draws + HelperDraws(j);
      split += joined != 0;
      // Stock: DCS draws the cascade's casters in order.
      PassSetup(d, d.dsvC[c]);
      g_vt23.clear();
      for (void* r : vec[c]) {
        const int i = indexOf(r);
        if (w->Smr(i))
          DrawDcs(d, *w, i);
        else
          DrawOther(d);
      }
      const std::map<void*, int> stockVt23 = g_vt23;
      const std::vector<uint32_t> ref = ReadDepth(d, d.depthC[c]);
      refs[c] = ref;
      for (uint32_t v : ref) written += v != 0;
      // Recorded: DCS's loop over the exec list.
      PassSetup(d, d.dsvC[c]);
      g_vt23.clear();
      for (uint32_t e = 0; e < j.swapCount && cl[c][0]; ++e) {
        void* entry = j.swapList[e];
        if (entry == j.execObj) {
          ReplayStreamingRaw(j);
          allOk &= CheckTexturesRaw(j) == kPExecuted;
          defrec::PassState before = {}, after = {};
          defrec::Capture(d.imm, &before);
          d.imm->CopyResource(d.ourB7C[c], d.dcsB7);
          for (ID3D11CommandList* l : cl[c])
            if (l) d.imm->ExecuteCommandList(l, TRUE);
          defrec::Capture(d.imm, &after);
          restored &= defrec::Equal(before, after);
          defrec::Release(before);
          defrec::Release(after);
          continue;
        }
        const int i = indexOf(entry);
        if (w->Smr(i))
          DrawDcs(d, *w, i);
        else
          DrawOther(d);
      }
      const std::vector<uint32_t> got = ReadDepth(d, d.depthC[c]);
      if (ref.size() != got.size() || ref.empty()) diffs = SIZE_MAX;
      for (size_t k = 0; diffs != SIZE_MAX && k < ref.size(); ++k) diffs += ref[k] != got[k];
      // The same textures requested; each recorded one at least once.
      replayOk &= g_vt23.size() == stockVt23.size();
      for (const auto& kv : stockVt23) replayOk &= g_vt23.count(kv.first) && g_vt23.at(kv.first) >= 1;
      for (uint32_t v = 0; v < j.vt23Count; ++v) replayOk &= g_vt23[j.vt23[v]] >= 1;
      ReleaseSnapshot(j);
    }
    auto bits = [](uint32_t m) {
      uint32_t n = 0;
      for (; m; m &= m - 1) ++n;
      return n;
    };
    const uint32_t j2 = jobs[2]->joined.load(), j3 = jobs[3]->joined.load();
    printf("     4 cascades: %u casters recorded (%u textured) in %u draws on %u lists, %u split over helpers (cascade 2: "
           "%u helpers took chunks, cascade 3: %u), %zu texels written, %zu differ\n",
           recorded, texRecorded, draws, lists, split, bits(j2), bits(j3), written, diffs);
    // Cascade 2 (untextured only) shared phase 1 with two or three helpers (its chunks run out); cascade 3 shared
    // with its two (textured views from the table, no snapshot).
    // How many helpers win chunks is scheduling: under CPU load a helper can
    // wake after the chunks ran out. The structure and the depth must hold
    // whatever the split; the expected distribution is only reported.
    const bool shared = bits(j2) >= 2 && HelperDraws(*jobs[2]) > 0 && jobs[2]->texTo == jobs[2]->groupsTex &&
                        bits(j3) == 2 && split >= 2;
    const bool structure = jobs[3]->groupCount > jobs[3]->groupsTex && jobs[3]->resolved && !jobs[3]->snapped &&
                           jobs[0]->joined.load() == 0;
    if (!shared)
      printf("     note: helpers took fewer chunks than the unloaded schedule (split %u, cascade 2: %u, cascade 3: %u); "
             "depth is still checked bit for bit\n",
             split, bits(j2), bits(j3));
    Check(allOk && texRecorded > 0 && draws < recorded && structure && diffs == 0 &&
              written > 4u * kDepth * kDepth / 100 && restored,
          "shadow rec S5 device: four cascades on 0-3 helpers each, textured and untextured (both phases' groups "
          "shared in chunks, one list per helper that took a chunk), DCS's loop over the exec list: depth equals "
          "stock bit for bit; state restored");
    Check(replayOk, "shadow rec S4 device: streaming requests reach the same textures as stock");
    for (auto& row : cl)
      for (auto*& l : row) SafeRel(l);
    // Free-running rounds (no test wait): the helpers race the primary for
    // stage A chunks, both phases' chunks, the closes and the end; helper
    // counts rotate. Every round's lists + residual casters equal stock.
    int rounds = 0, badRounds = 0;
    uint32_t joinedSeen = 0;
    for (int round = 0; round < 40 && waited && allOk; ++round, ++rounds) {
      for (int c = 0; c < 4; ++c) {
        Job& j = *jobs[c];
        j.helpers = static_cast<uint32_t>((c + round) % kThreads);
        j.splitAllowed = j.helpers > 0;
        j.result = kBuildNone;
        j.snapped = j.needGo = j.startedEarly = false;
        j.stageA.store(0);
        j.goState.store(kGoWait);
        j.recPhase.store(kRpWait);
        j.joined.store(0);
        j.preState.store(kGoWait);
        j.phase.store(kJobEmpty);
        ResetEvents(j);
        j.startState.store(kStartArmed);
        bool ok = true;
        for (uint32_t th = 1; th <= j.helpers; ++th) ok &= pool.Submit(Wk(c, th), kHelperMain[th], &j, nullptr);
        ok &= pool.Submit(c, &JobMain, &j, nullptr);
        allOk &= ok;
      }
      for (int c = 3; c >= 0; --c) StartJob(*jobs[c], desc[c], false);
      for (int spin = 0; spin < 10000; ++spin) {
        bool busy = false;
        for (int k = 0; k < kThreads * kSlots; ++k) busy |= pool.Busy(k);
        if (!busy) break;
        Sleep(1);
      }
      ID3D11CommandList* rl[4][kThreads] = {};
      for (int c = 0; c < 4; ++c)
        for (int th = 0; th < kThreads; ++th) rl[c][th] = pool.TakeList(Wk(c, th));
      bool good = true;
      for (int c = 0; c < 4; ++c) {
        Job& j = *jobs[c];
        const uint32_t joined = j.joined.load();
        joinedSeen |= joined;
        good &= rl[c][0] != nullptr && j.recorded > 0 && !j.chunkFail.load();
        for (int th = 1; th < kThreads; ++th) good &= (((joined >> th) & 1) != 0) == (rl[c][th] != nullptr);
        PassSetup(d, d.dsvC[c]);
        for (uint32_t e = 0; e < j.swapCount && rl[c][0]; ++e) {
          void* entry = j.swapList[e];
          if (entry == j.execObj) {
            d.imm->CopyResource(d.ourB7C[c], d.dcsB7);
            for (ID3D11CommandList* l : rl[c])
              if (l) d.imm->ExecuteCommandList(l, TRUE);
            continue;
          }
          const int i = indexOf(entry);
          if (w->Smr(i))
            DrawDcs(d, *w, i);
          else
            DrawOther(d);
        }
        good &= ReadDepth(d, d.depthC[c]) == refs[c];
      }
      badRounds += good ? 0 : 1;
      for (auto& row : rl)
        for (auto*& l : row) SafeRel(l);
    }
    printf("     %d free-running rounds (helpers rotating 0-3 per cascade), %d differ or fail; helper slots that took "
           "chunks: 0x%x\n",
           rounds, badRounds, joinedSeen);
    Check(rounds == 40 && badRounds == 0 && joinedSeen != 0,
          "shadow rec S5 device: free-running helpers (no test wait, 0-3 per job): every round equals stock bit for "
          "bit");
  }
  g_instDev = nullptr;
  pool.Stop();
  for (Job*& j : jobs) FreeJob(j);
  FreeTex(stab);
  FreeTables(t);
  Destroy(d);
  delete w;
}

void Run() {
  MeshTests();
  TableTests();
  ProbeCheckTests();
  StreamTests();
  JobTests();
  PsMapTests();
  SchedulingTests();
  TextureJobTests();
  SortSiteTests();
  Compiler c;
  if (!c.Load()) {
    printf("SKIP shadow rec: d3dcompiler_47.dll not available\n");
    return;
  }
  AnalyseTests(c);
  DeviceTests(c);
  DeviceMultiTests(c);
}

}  // namespace srtest
