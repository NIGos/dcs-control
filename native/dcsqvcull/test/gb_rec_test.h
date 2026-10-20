// Offline tests for gb_rec.h (R18 S1-S3):
//  - scope ordinals, the island and segment-limit plan, CB override masks;
//  - the texture table: entries built from fake DX11Textures (fixed view, aux,
//    +0x190, mip set not ready, area thresholds), the pick equal to gb_batch's
//    getSRV model for every size, the exec check against every field that can
//    change, references held and released (eviction, wipe), versions;
//  - the VS reflection rules and the key checks (CheckKey);
//  - jobs from fake DCS memory (CPU only): per-item reasons (unclean shaders,
//    a material with an item of another pass, blend scope, missing reads,
//    keys, meshes and textures, islands, the segment limit, the verify
//    stride), the caster list, the CB window equal to the bytes DCS's own
//    writes upload (pso, animated property, matrix, ratios), the exec checks
//    (guard, textures, versions, pages), the after-pass restore equal to the
//    stock end state (materials, FX variables, triangle counter), the stock
//    fallback order of a segment;
//  - on a real device (hardware, else WARP), the S1 harness: 6 render targets
//    and a depth-stencil target with stencil refs; DCS's draws emulated on the
//    immediate context, keys and meshes learnt by probes read back after them;
//    jobs built and recorded by the worker pool into segment lists; the
//    lists executed with ExecuteCommandList(TRUE) between DCS's residual
//    draws (blended ones included) in vector order; every target compared bit
//    for bit with the stock run, the immediate state equal before and after
//    each Execute; a control (segments executed after the residual draws)
//    differs; the verify-stride plan (many segments) still exact.
// QV_GBREC_ONLY=1: only these tests.
#pragma once

namespace grtest {
using namespace gbrec;

using srtest::FakeView;

// ---------------------------------------------------------------------------
// Fakes of the DCS code the recorder reaches
// ---------------------------------------------------------------------------
// FX variable: [0] vtable (slot 31 SetResource), [8] -> storage of the view.
struct FakeVar {
  void** vt;
  void** store;
  void* value;
};
void* g_varVt[40];
std::vector<std::pair<void*, void*>> g_setRes;
long __fastcall FakeSetRes(void* var, void* v) {
  static_cast<FakeVar*>(var)->value = v;
  g_setRes.push_back({var, v});
  return 0;
}
// Animated property: vt[9](prop, span, dst) = args[[prop+0x18]] * 0.5 + 1 (a float).
void* g_propVt[12];
uint64_t __fastcall FakeProp(void* prop, void* span, void* dst) {
  const uint32_t* args = *static_cast<const uint32_t* const*>(span);
  const uint32_t idx = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(prop) + 0x18);
  const float v = static_cast<float>(args[idx]) * 0.5f + 1.0f;
  memcpy(dst, &v, 4);
  return 0;
}
// NGModel 0xeb60: per {dst, prop} pair, vt[9] with a fresh copy of the span.
void __fastcall FakeEvalAnim(void* mat, const void* span) {
  auto* m = static_cast<uint8_t*>(mat);
  const uint8_t* b = *reinterpret_cast<uint8_t**>(m + 8);
  const uint8_t* e = *reinterpret_cast<uint8_t**>(m + 0x10);
  for (const uint8_t* p = b; p < e; p += 16) {
    alignas(16) uint8_t copy[16];
    memcpy(copy, span, 16);
    void* prop = *reinterpret_cast<void* const*>(p + 8);
    (*reinterpret_cast<PropFn**>(prop))[9](prop, copy, *reinterpret_cast<void* const*>(p));
  }
}
std::map<std::pair<void*, uint64_t>, int> g_vt23;
uint64_t __fastcall FakeVt23(void* tex, uint64_t size) {
  ++g_vt23[{tex, size}];
  return 0;
}
alignas(16) uint8_t g_desc[0x40];
const uint8_t* __fastcall FakeGetDesc(void*) { return g_desc; }
bool g_compat = true;
bool __fastcall FakeCompat(int32_t, int32_t) { return g_compat; }
std::vector<void*> g_vt1Calls;
uint64_t __fastcall FakeVt1(void* self, void*) {
  g_vt1Calls.push_back(self);
  return 0;
}

// ---------------------------------------------------------------------------
// Fake DCS memory
// ---------------------------------------------------------------------------
constexpr int kShaders = 4;  // 0: opaque; 1, 2: drawn by DCS (unclean); 3: A2C
constexpr int kMats = 10;    // 0-3 shader 0; 4, 5 shader 1; 6, 7 shader 2; 8, 9 shader 3
constexpr int kMeshes = 4, kPages = 2, kTex = 6, kRecs = 4, kN = 300, kPerPage = 128;
constexpr int kHandles = 3;  // Diffuse (record 0, read), Normal (1, read), Specular (2, not read)
constexpr int kSbRec = 3;    // sbPositions' record
constexpr int kOtherEvery = 64, kOtherAt = 40, kDecalAt = 50;
constexpr int kUncleanItem = 70;   // shader 2: no Diffuse texture (inherits)
constexpr int kPass2Item = 66;     // material 5 drawn with model pass 2
constexpr int32_t kRecType = 7;
const float kRatio[3] = {1.0f, 0.5f, 0.25f};

int g_blendOf[kShaders] = {gbreccount::kBmNone, gbreccount::kBmNone, gbreccount::kBmNone, gbreccount::kBmAlphaTest};
uint8_t* g_shaderBase = nullptr;
int FakeBlend(uint8_t* sh) { return g_blendOf[(sh - g_shaderBase) / 0x200]; }
bool FakeCompiled(void*, uint64_t) { return true; }
bool FakeCbUsed(void*, uint64_t, uint64_t out[2]) {
  AllDwords(out);
  return true;
}

struct World {
  void* srVt[4] = {};
  void* otherVt[4] = {};
  void* decalVt[4] = {};
  void* modelVt[8] = {};
  void* shaderVt[40] = {};
  void* texVt[30] = {};
  void* innerVt[12] = {};
  uint8_t shader[kShaders][0x200] = {};
  uint8_t recs[kShaders][kRecs * 0x50] = {};
  FakeVar vars[kShaders][kRecs] = {};
  uint8_t tex[kTex][0x700] = {};
  uint8_t inner[kTex][0x20] = {};
  uint8_t texViews[kTex][0x40] = {};  // aux 0: the view at +0x10
  uint8_t mat[kMats][0x300] = {};
  uint8_t props[kMats][0x300] = {};
  uint8_t prop[kMats][0x40] = {};
  uint8_t animList[kMats][16] = {};
  uint8_t item[kN][0x100] = {};
  uint8_t rend[kN][0x80] = {};
  uint64_t entries[kN][3 * kHandles] = {};
  uint8_t* arrPtr[kN] = {};
  uint32_t args[kN][4] = {};
  uint8_t mesh[kMeshes][0x220] = {};
  uint8_t ibObj[kMeshes][0x40] = {};
  uint8_t vbObj[kMeshes][0x160] = {};
  uint32_t tris[kMeshes] = {};
  uint8_t globalsObj[0x200] = {};
  uint8_t* globalsPtr = nullptr;
  uint8_t pageArr[0x20 + kPages * 0x30] = {};
  uint8_t gpuBuf[kPages][0x40] = {};
  int kind[kN] = {};  // 0 model, 1 other renderable, 2 decal (another class, blended)
  std::vector<void*> vec;
  void* desc[3] = {};
  Env env;
  uint8_t matInit[kMats][0x300] = {};

  static int ShaderOfMat(int m) { return m < 4 ? 0 : m < 6 ? 1 : m < 8 ? 2 : 3; }
  int MatOf(int i) const { return static_cast<int>((*reinterpret_cast<uint8_t* const*>(item[i] + 0x10) - mat[0]) / 0x300); }
  static int MeshOf(int i) { return (i / 5) % kMeshes; }
  static int PageOf(int i) { return (i / 7) % kPages; }
  static int DiffuseOf(int i) { return (i / 3) % 3; }
  static int NormalOf(int i) { return 3 + (i % 2); }
  static int PlanMat(int i) {
    const int b = (i / 20) % 4;
    return b < 2 ? i % 4 : b == 2 ? 8 + (i % 2) : 4 + (i % 4);
  }
  void SetPageSrv(int p, void* srv) { *reinterpret_cast<void**>(gpuBuf[p] + 0x30) = srv; }
  void SetView(int t, void* v) { *reinterpret_cast<void**>(texViews[t] + 0x10) = v; }
  void SetMeshBuffers(int k, void* ib, void* vb) {
    *reinterpret_cast<void**>(ibObj[k] + 0x28) = ib;
    *reinterpret_cast<void**>(vbObj[k] + 0x130) = vb;
  }

  void Init() {
    uint32_t rng = 0x13579bd;
    auto rnd = [&]() {
      rng = rng * 1664525u + 1013904223u;
      return rng;
    };
    g_shaderBase = shader[0];
    for (auto*& p : g_varVt) p = nullptr;
    g_varVt[31] = reinterpret_cast<void*>(&FakeSetRes);
    for (auto*& p : g_propVt) p = nullptr;
    g_propVt[9] = reinterpret_cast<void*>(&FakeProp);
    srVt[1] = reinterpret_cast<void*>(&FakeVt1);
    for (int s = 0; s < kShaders; ++s) {
      *reinterpret_cast<void**>(shader[s]) = shaderVt;
      *reinterpret_cast<uintptr_t*>(shader[s] + 0x50) = 0xeff0 + s;
      *reinterpret_cast<uintptr_t*>(shader[s] + 0xb0) = 0x7ec0 + s;
      *reinterpret_cast<uint8_t**>(shader[s] + 0xc8) = recs[s];
      *reinterpret_cast<uint8_t**>(shader[s] + 0xd0) = recs[s] + sizeof(recs[s]);
      for (int h = 0; h < kRecs; ++h) {
        *reinterpret_cast<int32_t*>(recs[s] + h * 0x50 + 0xc) = kRecType;
        FakeVar& v = vars[s][h];
        v.vt = g_varVt;
        v.store = &v.value;
        v.value = nullptr;
        *reinterpret_cast<FakeVar**>(recs[s] + h * 0x50 + 0x40) = &v;
      }
    }
    texVt[23] = reinterpret_cast<void*>(&FakeVt23);
    for (int t = 0; t < kTex; ++t) {
      *reinterpret_cast<void**>(tex[t]) = texVt;
      *reinterpret_cast<uint8_t**>(tex[t] + 0x10) = inner[t];
      *reinterpret_cast<void**>(inner[t]) = innerVt;
      *reinterpret_cast<uint8_t**>(tex[t] + 0x90) = texViews[t];
    }
    *reinterpret_cast<int32_t*>(g_desc + 0x20) = 28;
    for (int m = 0; m < kMats; ++m) {
      uint8_t* mt = mat[m];
      *reinterpret_cast<void**>(mt) = modelVt;
      *reinterpret_cast<uint8_t**>(mt + 0x28) = props[m];
      *reinterpret_cast<uint8_t**>(mt + 0x30) = shader[ShaderOfMat(m)];
      *reinterpret_cast<int64_t*>(mt + 0x68) = kSbRec;
      *reinterpret_cast<uint64_t*>(mt + 0x1d8) = 1;
      for (uint32_t b = 0; b < kCbLen; b += 4) *reinterpret_cast<uint32_t*>(mt + kCbOff + b) = rnd() & 0x3f7fffff;
      *reinterpret_cast<uint32_t*>(mt + 0x2d8) = kHandles;
      for (int k = 0; k < kHandles; ++k) *reinterpret_cast<int64_t*>(mt + 0x240 + 8 * k) = k;
      *reinterpret_cast<uint32_t*>(props[m] + 0x26c) = 0;
      // One animated property: CB +0xc0 (mat+0x150) = args[1] * 0.5 + 1.
      *reinterpret_cast<void**>(prop[m]) = g_propVt;
      *reinterpret_cast<uint32_t*>(prop[m] + 0x18) = 1;
      *reinterpret_cast<uint8_t**>(animList[m]) = mt + 0x150;
      *reinterpret_cast<uint8_t**>(animList[m] + 8) = prop[m];
      *reinterpret_cast<uint8_t**>(mt + 8) = animList[m];
      *reinterpret_cast<uint8_t**>(mt + 0x10) = animList[m] + 16;
    }
    for (int k = 0; k < kMeshes; ++k) {
      uint8_t* me = mesh[k];
      *reinterpret_cast<uint8_t**>(me) = ibObj[k];
      *reinterpret_cast<uint8_t**>(me + 0x18) = vbObj[k];
      *reinterpret_cast<uint32_t*>(ibObj[k] + 0x3c) = (k & 1) ? 4 : 2;
      *reinterpret_cast<uint32_t*>(vbObj[k] + 0x150) = 12;
      *reinterpret_cast<uint32_t*>(vbObj[k] + 0x2c) = 6;
      *reinterpret_cast<uint32_t*>(me + 0x58) = 1;
      *reinterpret_cast<uint32_t*>(me + 0x130) = 0;
      *reinterpret_cast<uint32_t*>(me + 0x208) = 1;
      *reinterpret_cast<uint32_t*>(me + 0x20c) = shrec::kPrimTriList;
      tris[k] = 2 + k;
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
    for (int i = 0; i < kN; ++i) {
      uint8_t* it = item[i];
      uint8_t* r = rend[i];
      kind[i] = i % kOtherEvery == kOtherAt ? 1 : i % kOtherEvery == kDecalAt ? 2 : 0;
      *reinterpret_cast<void**>(r) = kind[i] == 1 ? static_cast<void*>(otherVt)
                                     : kind[i] == 2 ? static_cast<void*>(decalVt)
                                                    : static_cast<void*>(srVt);
      *reinterpret_cast<uint8_t**>(r + 0x10) = it;
      *reinterpret_cast<uint32_t*>(r + 0x60) = i == kPass2Item ? 2 : 1;
      *reinterpret_cast<uint64_t*>(r + 0x6c) = (static_cast<uint64_t>(16 + i % 40) << 32) | (8 + i % 24);
      *reinterpret_cast<uint64_t*>(r + 0x78) = 10 + i;
      int m = PlanMat(i);
      if (i == kPass2Item) m = 5;
      if (i == kUncleanItem) m = 6;
      for (int a = 0; a < 4; ++a) args[i][a] = static_cast<uint32_t>(i * 3 + a);
      *reinterpret_cast<uint32_t**>(it) = args[i];
      *reinterpret_cast<uint64_t*>(it + 8) = 4;
      *reinterpret_cast<uint8_t**>(it + 0x10) = mat[m];
      for (uint32_t b = 0x60; b < 0xa0; b += 4) *reinterpret_cast<uint32_t*>(it + b) = rnd() & 0x3f7fffff;
      *reinterpret_cast<uint8_t**>(it + 0xc0) = mesh[MeshOf(i)];
      *reinterpret_cast<uint32_t*>(it + 0xd0) = static_cast<uint32_t>(PageOf(i));
      *reinterpret_cast<uint32_t*>(it + 0xd4) = static_cast<uint32_t>((i * 7) % kPerPage);
      entries[i][0] = 0;
      entries[i][1] = i == kUncleanItem ? 0 : reinterpret_cast<uint64_t>(tex[DiffuseOf(i)]);
      entries[i][3] = 0;
      entries[i][4] = reinterpret_cast<uint64_t>(tex[NormalOf(i)]);
      entries[i][6] = 0;
      entries[i][7] = reinterpret_cast<uint64_t>(tex[5]);
      arrPtr[i] = reinterpret_cast<uint8_t*>(entries[i]);
      *reinterpret_cast<uint8_t***>(it + 0x18) = &arrPtr[i];
      vec.push_back(r);
    }
    desc[0] = vec.data();
    desc[1] = vec.data() + vec.size();
    desc[2] = desc[1];
    memcpy(matInit, mat, sizeof(mat));
    env = Env();
    env.srVt = srVt;
    env.modelVt = modelVt;
    env.shaderVt = shaderVt;
    env.globals = &globalsPtr;
    env.propVt[0] = g_propVt;
    env.propBytes[0] = 4;
    env.setResource = reinterpret_cast<void*>(&FakeSetRes);
    env.evalAnim = &FakeEvalAnim;
    env.compiled = &FakeCompiled;
    env.cbUsed = &FakeCbUsed;
    env.blend = &FakeBlend;
    env.tex.texVtbl = texVt;
    env.tex.inner[0] = env.tex.inner[1] = env.tex.inner[2] = innerVt;
    env.tex.getDesc = &FakeGetDesc;
    env.tex.compat = &FakeCompat;
  }

  // DCS's per-draw material writes of item i (0x16140 / 0x15e20, pass 1
  // opaque) and its FX variable sets (vt[27], slot 26); the uploaded bytes.
  void DcsWrites(int i, uint8_t out[kCbLen], const TexTable* tt) {
    uint8_t* it = item[i];
    uint8_t* mt = *reinterpret_cast<uint8_t**>(it + 0x10);
    uint8_t* sh = *reinterpret_cast<uint8_t**>(mt + 0x30);
    *reinterpret_cast<uint32_t*>(mt + 0x18c) = *reinterpret_cast<uint32_t*>(it + 0xd4);
    alignas(16) uint8_t span[16];
    memcpy(span, it, 16);
    FakeEvalAnim(mt, span);
    memcpy(mt + 0xb0, it + 0x60, 0x40);
    memcpy(mt + 0x110, kRatio, 12);
    memcpy(out, mt + kCbOff, kCbLen);
    // FX variables: sbPositions (page view), each handle's texture view.
    const int s = static_cast<int>((sh - shader[0]) / 0x200);
    vars[s][kSbRec].value = *reinterpret_cast<void**>(gpuBuf[PageOf(i)] + 0x30);
    for (int k = 0; k < kHandles; ++k) {
      auto* tx = reinterpret_cast<uint8_t*>(entries[i][3 * k + 1]);
      if (!tx) continue;  // slot 26 returns: the variable keeps an earlier draw's view
      ID3D11ShaderResourceView* v = *reinterpret_cast<ID3D11ShaderResourceView**>(texViews[(tx - tex[0]) / 0x700] + 0x10);
      if (tt) {
        const TexEntry* e = TexFind(*tt, tx, 0, kRecType);
        if (e) TexPick(*e, *reinterpret_cast<uint64_t*>(rend[i] + 0x6c), &v);
      }
      vars[s][k].value = v;
    }
  }
};

// Keys, reads and meshes as the probes and the reads resolution publish them.
GbKey* PubKey(Tables& t, World& w, int s, int state = 1) {
  GbKey* k = NewKey(t, w.shader[s], 1, 0);
  k->tech = 1;
  k->flags = 0;
  k->effect = *reinterpret_cast<void**>(w.shader[s] + 0x50);
  k->techBegin = *reinterpret_cast<void**>(w.shader[s] + 0xb0);
  k->st.cbSlot = 2;
  k->st.sbSlot = 3;
  k->st.ctxMask = 1u << 7;
  k->psCbMask = 1u << 2;
  k->psTexCount = 2;
  k->psTexH[0] = 0;
  k->psTexSlot[0] = 0;
  k->psTexH[1] = 1;
  k->psTexSlot[1] = 1;
  k->state.store(state);
  PublishKey(t, *k, w.shader[s]);
  return k;
}
void PubReads(Tables& t, World& w, int s, int state = 1) {
  ReadsEntry* e = NewReads(t, w.shader[s], 1);
  e->tech = 1;
  e->effect = *reinterpret_cast<void**>(w.shader[s] + 0x50);
  e->techBegin = *reinterpret_cast<void**>(w.shader[s] + 0xb0);
  e->state = state;
  e->mask[0] = 0x3;  // Diffuse, Normal
  e->count = 2;
  PublishReads(t, *e, w.shader[s]);
}
void PubMesh(Tables& t, World& w, int s, int m) {
  shrec::MeshLive live;
  shrec::ReadMesh(w.mesh[m], live);
  shrec::MeshEntry* e = shrec::NewMesh(t.mesh, w.mesh[m], w.shader[s], 1);
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
  e->state.store(1);
  shrec::PublishMesh(t.mesh, *e, w.mesh[m]);
}
void PubAll(Tables& t, World& w) {
  for (int s = 0; s < kShaders; ++s) {
    PubKey(t, w, s);
    PubReads(t, w, s);
    for (int m = 0; m < kMeshes; ++m) PubMesh(t, w, s, m);
  }
}
void FillTex(TexTable& tt, World& w, int skip = -1) {
  for (int k = 0; k < kTex; ++k) {
    if (k == skip) continue;
    bool fresh = false;
    TexEntry* e = TexInsert(tt, w.tex[k], 0, kRecType, &fresh);
    if (e) TexBuildRaw(w.env.tex, *e);
    if (e) e->dirty.store(0);
  }
}

LoopObj g_front = {nullptr, 0, -1};
LoopObj g_exec[kMaxSegments];

void InitJob(Job& j, World& w, const Tables& t, const TexTable& tt) {
  j.vec = w.desc;
  j.env = &w.env;
  j.tab = &t;
  j.tex = &tt;
  j.texLock = nullptr;
  j.flags = 0;
  j.scope = kDefaultScope;
  j.island = kDefaultIsland;
  j.maxSeg = kDefaultMaxSeg;
  j.splitMin = kSplitMinDraws;
  j.stride = 0;
  j.front = &g_front;
  for (int k = 0; k < kMaxSegments; ++k) {
    g_exec[k] = {nullptr, 0, k};
    j.execObj[k] = &g_exec[k];
  }
  memcpy(j.ratio, kRatio, kRatioLen);
  j.useGen = 1;
}

// ---------------------------------------------------------------------------
// Pure tests
// ---------------------------------------------------------------------------
void PureTests() {
  Check(OrdinalOfSlot(0x10004, 0) == 2 && OrdinalOfSlot(0x10004, 1) < 0 && SlotOfOrdinal(0x10004, 2) == 0 &&
            SlotOfOrdinal(0x10004, 0) < 0 && SlotOfOrdinal(0x10004, 16) < 0 && OrdinalOfSlot(0x50a, 2) == 8 &&
            SlotOfOrdinal(0x50a, 10) == 3 && SlotOfOrdinal(0x1f, 4) < 0,
        "gbuffer rec: scope bits map executions by ordinal to at most 4 slots (bit 16 is A2C)");
  // Island rule and segment limit.
  {
    // runs: [0,5) [6,40) [41,45) [46,100) [101,131)
    std::vector<uint8_t> f(131, 1);
    f[5] = f[40] = f[45] = f[100] = 0;
    std::vector<uint8_t> why(131, 0);
    uint32_t runs[kMaxSegments][2];
    const int n = PlanSegments(f.data(), f.size(), 30, 8, runs, why.data());
    bool ok = n == 3 && runs[0][0] == 6 && runs[0][1] == 40 && runs[1][0] == 46 && runs[1][1] == 100 &&
              runs[2][0] == 101 && runs[2][1] == 131;
    for (int i = 0; i < 5; ++i) ok = ok && why[i] == 1;
    for (int i = 41; i < 45; ++i) ok = ok && why[i] == 1;
    ok = ok && why[6] == 0 && why[130] == 0;
    Check(ok, "gbuffer rec: runs shorter than the island stay residual");
    // 12 runs of length 1 + k: the 8 longest are kept, in vector order.
    std::vector<uint8_t> g;
    for (int k = 0; k < 12; ++k) {
      for (int q = 0; q <= k; ++q) g.push_back(1);
      g.push_back(0);
    }
    std::vector<uint8_t> why2(g.size(), 0);
    const int n2 = PlanSegments(g.data(), g.size(), 1, 8, runs, why2.data());
    bool ok2 = n2 == 8;
    for (int k = 0; ok2 && k < n2; ++k) ok2 = runs[k][1] - runs[k][0] == static_cast<uint32_t>(k + 5);
    for (int k = 1; ok2 && k < n2; ++k) ok2 = runs[k][0] > runs[k - 1][0];
    ok2 = ok2 && why2[0] == 2 && why2[2] == 2;  // the shortest runs: beyond the limit
    Check(ok2, "gbuffer rec: at most 8 segments, the longest kept in vector order");
    // Stage 2's ownship items (flag 2) never share a segment with the others (flag 1): runs split where the value
    // changes, each run judged by the island on its own.
    std::vector<uint8_t> h;
    for (int q = 0; q < 40; ++q) h.push_back(1);
    for (int q = 0; q < 35; ++q) h.push_back(2);
    for (int q = 0; q < 5; ++q) h.push_back(1);
    for (int q = 0; q < 3; ++q) h.push_back(2);
    std::vector<uint8_t> why3(h.size(), 0);
    const int n3 = PlanSegments(h.data(), h.size(), 30, 12, runs, why3.data());
    bool ok3 = n3 == 2 && runs[0][0] == 0 && runs[0][1] == 40 && runs[1][0] == 40 && runs[1][1] == 75;
    for (size_t i = 75; i < h.size(); ++i) ok3 = ok3 && why3[i] == 1;
    ok3 = ok3 && why3[0] == 0 && why3[40] == 0;
    Check(ok3, "gbuffer rec: a run splits where the flag value changes (ownship 2 / other 1), each part against the "
               "island");
  }
  uint64_t m[2] = {};
  BaseOverrides(m);
  auto bit = [&](uint32_t off) { return (m[(off / 4) >> 6] >> ((off / 4) & 63) & 1) != 0; };
  uint64_t all[2];
  AllDwords(all);
  Check(bit(0xfc) && bit(0x20) && bit(0x5c) && !bit(0x60) && bit(0x80) && bit(0x88) && !bit(0x8c) && !bit(0x1c) &&
            all[1] == (1ull << (0x130 / 4 - 64)) - 1,
        "gbuffer rec: per-draw overrides are posStructOffset, the matrix and the ratio block");
}

// ---------------------------------------------------------------------------
// Texture table
// ---------------------------------------------------------------------------
// A fake DX11Texture whose getSRV state the test controls.
struct FakeTex {
  alignas(16) uint8_t t[0x700] = {};
  alignas(16) uint8_t inner[0x20] = {};
  alignas(16) uint8_t S[0x50] = {};
  alignas(16) uint8_t P[0x50] = {};
  alignas(16) uint8_t aux[0x40] = {};
  void* views[6] = {};
  int32_t thr[6] = {};
  void Init(World& w) {
    *reinterpret_cast<void**>(t) = w.texVt;
    *reinterpret_cast<uint8_t**>(t + 0x10) = inner;
    *reinterpret_cast<void**>(inner) = w.innerVt;
    *reinterpret_cast<uint8_t**>(t + 0x198) = S;
    *reinterpret_cast<uint8_t**>(t + 0x1a0) = P;
    *reinterpret_cast<uint8_t**>(t + 0x90) = aux;
    SetReady(S, false);
    SetReady(P, false);
  }
  static void SetReady(uint8_t* set, bool ready) {
    *reinterpret_cast<int32_t*>(set + 0x40) = ready ? 5 : 3;
    *reinterpret_cast<void**>(set + 0x20) = ready ? set : nullptr;
  }
  void SetAreas(int nv, int nt) {
    *reinterpret_cast<void**>(S + 8) = views;
    *reinterpret_cast<void**>(S + 0x10) = views + nv;
    *reinterpret_cast<void**>(S + 0x28) = thr;
    *reinterpret_cast<void**>(S + 0x30) = thr + nt;
  }
};

// getSRV's swap for the swap-ahead test: the back set becomes the front one.
FakeTex* g_swapTex = nullptr;
int g_swapCalls = 0;
void* __fastcall FakeGetSrv(void* tex, void*, const uint64_t*) {
  ++g_swapCalls;
  if (g_swapTex && tex == g_swapTex->t) {
    FakeTex::SetReady(g_swapTex->P, false);
    FakeTex::SetReady(g_swapTex->S, true);
  }
  return nullptr;
}

void TexTableTests() {
  World* w = new World;
  w->Init();
  FakeView fv[8];
  FakeTex* ft = new FakeTex;
  ft->Init(*w);
  for (int k = 0; k < 6; ++k) ft->views[k] = &fv[k];
  const int32_t thr[4] = {4096, 1024, 256, 0};
  memcpy(ft->thr, thr, sizeof(thr));
  ft->SetAreas(4, 4);
  *reinterpret_cast<void**>(ft->t + 0x1b8) = &fv[6];
  *reinterpret_cast<void**>(ft->t + 0x80) = &fv[7];
  TexTable tt;
  AllocTex(tt, 64);
  bool fresh = false;
  TexEntry* e = TexInsert(tt, ft->t, -1, kRecType, &fresh);
  bool ok = e && fresh && TexFind(tt, ft->t, -1, kRecType) == e && !TexFind(tt, ft->t, 0, kRecType);
  // S not ready: the fallback view (+0x1b8, else +0x80).
  ok = ok && TexBuildRaw(w->env.tex, *e) == kTwOk && e->usable && e->mode == kTmFixed && static_cast<void*>(e->views[0]) == &fv[6] &&
       fv[6].refs == 2;
  // S ready: area thresholds; every size picks what gb_batch's getSRV model picks.
  FakeTex::SetReady(ft->S, true);
  ok = ok && TexBuildRaw(w->env.tex, *e) == kTwOk && e->mode == kTmAreas && e->nviews == 4 && fv[6].refs == 1 &&
       fv[0].refs == 2 && fv[3].refs == 2;
  bool pick = true;
  for (uint32_t x = 0; x < 80; x += 7)
    for (uint32_t y = 0; y < 80; y += 9) {
      const uint64_t size = (static_cast<uint64_t>(y) << 32) | x;
      void* want = nullptr;
      ID3D11ShaderResourceView* got = nullptr;
      pick = pick && gbbatch::PredictSrv(ft->t, -1, size, &want) == gbbatch::kOk && TexPick(*e, size, &got) &&
             static_cast<void*>(got) == want;
    }
  Check(ok && pick, "gbuffer rec texture table: entries built from the texture, the pick equals getSRV's for every size");
  // The exec check sees every change that alters getSRV's answer.
  bool chk = TexCheckRaw(w->env.tex, *e);
  ft->views[2] = &fv[5];
  chk = chk && !TexCheckRaw(w->env.tex, *e);
  ft->views[2] = &fv[2];
  ft->thr[1] = 1000;
  chk = chk && !TexCheckRaw(w->env.tex, *e);
  ft->thr[1] = 1024;
  FakeTex::SetReady(ft->P, true);  // a swap is due
  chk = chk && !TexCheckRaw(w->env.tex, *e) && TexSwapDueNow(*e);
  FakeTex::SetReady(ft->P, false);
  *reinterpret_cast<void**>(ft->t + 0x190) = &fv[4];
  chk = chk && !TexCheckRaw(w->env.tex, *e);
  *reinterpret_cast<void**>(ft->t + 0x190) = nullptr;
  *reinterpret_cast<uint64_t*>(ft->t + 0x678) = 1;
  chk = chk && !TexCheckRaw(w->env.tex, *e);
  *reinterpret_cast<uint64_t*>(ft->t + 0x678) = 0;
  ft->SetAreas(3, 4);
  chk = chk && !TexCheckRaw(w->env.tex, *e);
  ft->SetAreas(4, 4);
  chk = chk && TexCheckRaw(w->env.tex, *e);
  Check(chk, "gbuffer rec texture table: the exec check catches view, threshold, swap-due, +0x190, +0x678 and vector changes");
  // Swap due at build: unusable; +0x678 with another type: unusable; aux path; compat false: a null view.
  FakeTex::SetReady(ft->P, true);
  bool u = TexBuildRaw(w->env.tex, *e) == kTwSwap && !e->usable && fv[0].refs == 1;
  FakeTex::SetReady(ft->P, false);
  *reinterpret_cast<uint64_t*>(ft->t + 0x678) = 1;
  u = u && TexBuildRaw(w->env.tex, *e) == kTw678;
  *reinterpret_cast<uint64_t*>(ft->t + 0x678) = 0;
  TexEntry* ea = TexInsert(tt, ft->t, 1, kRecType, &fresh);
  *reinterpret_cast<void**>(ft->aux + 24 + 0x10) = &fv[5];
  ID3D11ShaderResourceView* va = nullptr;
  u = u && ea && TexBuildRaw(w->env.tex, *ea) == kTwOk && TexPick(*ea, 0, &va) && va == static_cast<void*>(&fv[5]) &&
      TexCheckRaw(w->env.tex, *ea);
  *reinterpret_cast<void**>(ft->aux + 24 + 0x10) = &fv[4];
  u = u && !TexCheckRaw(w->env.tex, *ea);
  g_compat = false;
  TexEntry* ec = TexInsert(tt, ft->t, -1, 99, &fresh);
  u = u && ec && TexBuildRaw(w->env.tex, *ec) == kTwOk && ec->compatNull && TexPick(*ec, 12345, &va) && !va;
  g_compat = true;
  Check(u, "gbuffer rec texture table: swap due and the 0x678 path refused; aux views and compat-null views replicated");
  // Re-recording: a swap due makes the table's entry stale; the job's own
  // entry is built after doing the swap (getSRV) only when swaps ahead are on.
  {
    Job* j = NewJob();
    j->env = &w->env;
    Stat st;
    FakeTex::SetReady(ft->S, false);
    TexBuildRaw(w->env.tex, *e);
    FakeTex::SetReady(ft->P, true);
    g_swapTex = ft;
    g_swapCalls = 0;
    shadowtex::GetSrvFn was = shadowtex::g_getSrv;
    shadowtex::g_getSrv = &FakeGetSrv;
    const bool stale = EntryStaleRaw(w->env.tex, *e, e->version.load());
    const TexEntry* none = RedoEntryRaw(*j, *e, false, st);
    const TexEntry* r = RedoEntryRaw(*j, *e, true, st);
    const bool sw = stale && !none && g_swapCalls == 1 && st.swapsAhead.load() == 1 && r && r->usable &&
                    r->mode == kTmAreas && RedoOf(*j, e) == r && IsRedoEnt(*j, r) && !IsRedoEnt(*j, e) &&
                    e->dirty.load() == 1 && j->refreshCount >= 1 && !EntryStaleRaw(w->env.tex, *r, r->version.load()) &&
                    RedoEntryRaw(*j, *e, true, st) == r && g_swapCalls == 1;
    shadowtex::g_getSrv = was;
    g_swapTex = nullptr;
    ReleaseRedo(*j);
    FreeJob(j);
    e->dirty.store(0);
    Check(sw, "gbuffer rec re-record: a due swap is done ahead (getSRV) only when on; the job's own entry gets the new "
              "views and the table's entry is queued for its rebuild");
  }
  // Versions, eviction and wipe release every reference.
  const uint32_t v0 = e->version.load();
  TexBuildRaw(w->env.tex, *e);
  bool life = e->version.load() != v0 && fv[0].refs == 2;
  e->lastUse.store(1);
  ea->lastUse.store(1000);
  ec->lastUse.store(1000);
  TexEvict(tt, 1000, 100, 64);
  life = life && fv[0].refs == 1 && tt.live == 2 && tt.tomb == 1 && !TexFind(tt, ft->t, -1, kRecType);
  TexEntry* again = TexInsert(tt, ft->t, -1, kRecType, &fresh);  // reuses the tombstone
  life = life && again && fresh && tt.tomb == 0 && tt.live == 3;
  FreeTex(tt);
  bool zero = true;
  for (int k = 0; k < 8; ++k) zero = zero && fv[k].refs == 1;
  Check(life && zero, "gbuffer rec texture table: rebuilds bump the version; eviction and teardown release the views");
  delete ft;
  delete w;
}

// ---------------------------------------------------------------------------
// Reflection and key checks
// ---------------------------------------------------------------------------
const char kGbHlsl[] = R"(
cbuffer cPerView : register(b7) { float4 gScale; };
#ifdef PERFRAME_AT_9
cbuffer cPerFrame : register(b9) { float4 gTime; };
#else
cbuffer cPerFrame : register(b6) { float4 gTime; };
#endif
cbuffer def_uniforms : register(b2) { float4 c[19]; };
StructuredBuffer<float4> sbPositions : register(t3);
#ifdef EXTRA_CB
cbuffer extra : register(b4) { float4 e; };
#endif
#ifdef VS_TEX
Texture2D vsTex : register(t0);
SamplerState vsSamp : register(s0);
#endif
Texture2D Diffuse : register(t0);
Texture2D Normal : register(t1);
SamplerState gLinear : register(s0);
SamplerState gPool : register(s5);  // DCS's sampler pool (bound per pass, not by FX)
struct VOut { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; };
VOut vs(float3 p : POSITION) {
  const uint pso = asuint(c[15].w);
  const float4 o = sbPositions[pso];
  VOut v;
  v.pos = float4(p.xy * o.w * gScale.x + o.xy, saturate(o.z + p.z * 0.1), 1);
  v.col = c[2] + c[8] + c[12] + gTime * 0.001;
#ifdef EXTRA_CB
  v.col += e;
#endif
#ifdef VS_TEX
  v.col += vsTex.SampleLevel(vsSamp, p.xy, 0);
#endif
  v.uv = p.xy * 3 + 0.5;
  return v;
}
struct PSOut { float4 t0 : SV_Target0; float4 t1 : SV_Target1; float4 t2 : SV_Target2;
               float4 t3 : SV_Target3; float4 t4 : SV_Target4; float4 t5 : SV_Target5; };
PSOut ps(VOut v) {
  PSOut o;
  const float4 d = Diffuse.Sample(gLinear, v.uv);
  const float4 n = Normal.Sample(gLinear, v.uv);
  o.t0 = d;
  o.t1 = n;
  o.t2 = frac(v.col);
  o.t3 = c[0] * gScale.y + Diffuse.Sample(gPool, v.uv * 1.7) * 0.25;
  o.t4 = d * n + c[1];
  o.t5 = float4(v.pos.z, d.a, frac(c[12].x), 1);
  return o;
}
PSOut psA2c(VOut v) {
  PSOut o = ps(v);
  o.t0.a = frac(v.col.x * 3.7);
  return o;
}
float4 psDecal(VOut v) : SV_Target0 { return float4(frac(v.col.rgb * 1.7), 0.5); }
float4 vsOther(float3 p : POSITION) : SV_Position { return float4(p.xy * 2.5, 0.45, 1); }
PSOut psOther(float4 pos : SV_Position) {
  PSOut o;
  o.t0 = o.t1 = o.t2 = o.t3 = o.t4 = float4(0.2, 0.3, 0.4, 1);
  o.t5 = float4(0.45, 0, 0, 1);
  return o;
}
)";

void AnalyseTests(srtest::Compiler& c) {
  struct Case {
    const char* define;
    bool ok;
    uint32_t ctx;
    const char* what;
  } cases[] = {{nullptr, true, (1u << 6) | (1u << 7), "def_uniforms + sbPositions + cPerFrame b6 + cPerView b7"},
               {"PERFRAME_AT_9", false, 0, "a context buffer at another register"},
               {"EXTRA_CB", false, 0, "another constant buffer"},
               {"VS_TEX", false, 0, "a texture in the VS"}};
  bool all = true;
  for (const Case& k : cases) {
    const D3D_SHADER_MACRO defs[] = {{k.define, "1"}, {nullptr, nullptr}};
    ID3DBlob* b = c.Build(kGbHlsl, sizeof(kGbHlsl) - 1, "vs", "vs_5_0", k.define ? defs : nullptr);
    if (!b) {
      all = false;
      continue;
    }
    VsStatic st;
    const char* why = AnalyseGbVs(c.reflect, b->GetBufferPointer(), b->GetBufferSize(), "def_uniforms", st);
    b->Release();
    const bool good = (why == nullptr) == k.ok && (!k.ok || (st.cbSlot == 2 && st.sbSlot == 3 && st.ctxMask == k.ctx));
    if (!good) printf("     VS case \"%s\": %s\n", k.what, why ? why : "accepted");
    all = all && good;
  }
  Check(all, "gbuffer rec: the VS reflection allows only def_uniforms, sbPositions, b6-b8 at their registers, samplers");
}

void KeyCheckTests() {
  auto P = [](uintptr_t v) { return reinterpret_cast<void*>(v); };
  shrec::Capture c;
  c.vs = static_cast<ID3D11VertexShader*>(P(0x10));
  c.ps = static_cast<ID3D11PixelShader*>(P(0x11));
  c.cb = static_cast<ID3D11Buffer*>(P(0x30));
  c.sb = static_cast<ID3D11ShaderResourceView*>(P(0x40));
  c.psCb[2] = c.cb;
  c.psCb[5] = c.cb;  // a leftover of the same buffer: bound too (harmless)
  c.psCb[7] = static_cast<ID3D11Buffer*>(P(0x27));
  c.psSrv[0] = static_cast<ID3D11ShaderResourceView*>(P(0x50));
  c.psSrv[1] = static_cast<ID3D11ShaderResourceView*>(P(0x51));
  c.psSrv[9] = c.sb;
  Extra x;
  x.vsCtx[1] = static_cast<ID3D11Buffer*>(P(0x27));
  VsStatic st;
  st.cbSlot = 2;
  st.sbSlot = 3;
  st.ctxMask = 1u << 7;
  ReadsEntry rd;
  rd.state = 1;
  rd.mask[0] = 0x3;
  rd.count = 2;
  Facts f;
  f.dcsVs = P(0x10);
  f.pageSrv = c.sb;
  f.passCtx[0][1] = f.passCtx[1][1] = static_cast<ID3D11Buffer*>(P(0x27));
  f.reads = &rd;
  f.readCount = 2;
  f.readH[0] = 1;
  f.readView[0] = P(0x51);
  f.readH[1] = 0;
  f.readView[1] = P(0x50);
  PsMap ps;
  bool ok = CheckKey(c, x, st, f, &ps) == nullptr && ps.count == 2 && ps.h[0] == 1 && ps.slot[0] == 1 &&
            ps.h[1] == 0 && ps.slot[0] == 1 && ps.slot[1] == 0 && ps.cbMask == ((1u << 2) | (1u << 5)) &&
            ps.ctxMask == (1u << 7) && ps.sbMask[0] == (1ull << 9);
  Facts g = f;
  g.passCtx[0][1] = static_cast<ID3D11Buffer*>(P(0x99));
  ok = ok && CheckKey(c, x, st, g, &ps) != nullptr && CheckKey(c, x, st, g, &ps) != kInconclusive;
  g = f;
  g.readView[1] = P(0x51);  // two records with the same view: another draw maps them
  ok = ok && CheckKey(c, x, st, g, &ps) == kInconclusive;
  g = f;
  g.readOk = false;
  ok = ok && CheckKey(c, x, st, g, &ps) == kInconclusive;
  g = f;
  g.readView[0] = P(0x77);  // a read view at no PS slot
  ok = ok && CheckKey(c, x, st, g, &ps) != nullptr && CheckKey(c, x, st, g, &ps) != kInconclusive;
  shrec::Capture c2 = c;
  c2.psSrv[4] = static_cast<ID3D11ShaderResourceView*>(P(0x50));  // the Diffuse view twice
  ok = ok && CheckKey(c2, x, st, f, &ps) == kInconclusive;
  c2 = c;
  c2.psCb[6] = static_cast<ID3D11Buffer*>(P(0x26));  // a PS context buffer the front did not see
  ok = ok && CheckKey(c2, x, st, f, &ps) != nullptr;
  c2 = c;
  c2.gs = static_cast<ID3D11GeometryShader*>(P(0x12));
  ok = ok && CheckKey(c2, x, st, f, &ps) != nullptr;
  c2 = c;
  c2.ps = nullptr;
  ok = ok && CheckKey(c2, x, st, f, &ps) != nullptr;
  // Reads unknown or failed: never recordable from this draw.
  g = f;
  g.reads = nullptr;
  ok = ok && CheckKey(c, x, st, g, &ps) != nullptr && CheckKey(c, x, st, g, &ps) != kInconclusive;
  Check(ok, "gbuffer rec: key checks (VS, context buffers, PS slots of the read textures, inconclusive cases)");
}

// ---------------------------------------------------------------------------
// Jobs from fake DCS memory
// ---------------------------------------------------------------------------
bool IsCandShader(World& w, int i) {
  if (w.kind[i] != 0 || i == kPass2Item) return false;
  const int s = World::ShaderOfMat(w.MatOf(i));
  return s == 0 || s == 3;
}

void JobTests() {
  World* w = new World;
  w->Init();
  Tables t;
  AllocTables(t);
  TexTable tt;
  AllocTex(tt, 256);
  FakeView views[kTex], pages[kPages];
  for (int k = 0; k < kTex; ++k) w->SetView(k, &views[k]);
  for (int p = 0; p < kPages; ++p) w->SetPageSrv(p, &pages[p]);
  PubAll(t, *w);
  FillTex(tt, *w);
  Job* j = NewJob();
  InitJob(*j, *w, t, tt);
  bool built = BuildJob(*j) == kBuildOk;
  // Reasons: shaders 1 and 2 are drawn by DCS (a pass-2 item, an item that
  // inherits Diffuse); material 5 has the pass-2 item; foreign renderables;
  // runs below the island.
  bool reasons = built && j->n == kN;
  uint32_t sum = 0;
  for (int r = 0; r < kReasons; ++r) sum += j->reasons[r];
  reasons = reasons && sum == kN;
  for (int i = 0; i < kN && reasons; ++i) {
    const Item& it = j->items[i];
    if (w->kind[i]) {
      reasons = it.reason == kRNotModel;
      continue;
    }
    const int m = w->MatOf(i), s = World::ShaderOfMat(m);
    if (i == kPass2Item)
      reasons = it.reason == kRPass;
    else if (m == 5)
      reasons = it.reason == kRMatExcluded;
    else if (s == 1 || s == 2)
      reasons = it.reason == kRUnclean;
    else
      reasons = it.reason == kRecorded || it.reason == kRIsland || it.reason == kRSegments;
    if (!reasons) printf("     item %d (material %d): reason %d\n", i, m, it.reason);
  }
  bool runs = j->segCount > 0 && j->segCount <= kMaxSegments;
  uint32_t rec = 0;
  for (uint32_t k = 0; k < j->segCount && runs; ++k) {
    const Seg& s = j->segs[k];
    runs = s.end - s.first >= kDefaultIsland && s.draws == s.end - s.first;
    for (uint32_t i = s.first; i < s.end && runs; ++i) runs = j->items[i].seg == k && IsCandShader(*w, static_cast<int>(i));
    rec += s.draws;
  }
  runs = runs && rec == j->reasons[kRecorded] && rec == j->recorded;
  Check(reasons && runs, "gbuffer rec job: per-item reasons (unclean shaders, other-pass material, islands) and segments");
  // DCS's loop: [front, residual items in order, one exec entry at each segment's first item].
  bool list = j->swapList[0] == &g_front;
  size_t p = 1;
  for (int i = 0; i < kN && list; ++i) {
    const Item& it = j->items[i];
    if (it.seg == kNoSeg)
      list = j->swapList[p++] == w->rend[i];
    else if (static_cast<uint32_t>(i) == j->segs[it.seg].first)
      list = j->swapList[p++] == &g_exec[it.seg];
  }
  Check(list && p == j->swapCount, "gbuffer rec job: the caster list keeps the vector order around the exec entries");
  // CB windows equal what DCS's own writes upload, draw by draw in stock order.
  bool win = true;
  for (int i = 0; i < kN && win; ++i) {
    if (w->kind[i]) continue;
    uint8_t dcs[kCbLen];
    w->DcsWrites(i, dcs, &tt);
    const Item& it = j->items[i];
    if (it.seg == kNoSeg) continue;
    alignas(16) uint8_t ours[kCbLen];
    BuildWindow(*j, j->cands[it.cand], ours);
    win = memcmp(ours, dcs, kCbLen) == 0;
    if (!win) printf("     item %d: CB window differs\n", i);
  }
  Check(win, "gbuffer rec job: each CB window equals the bytes DCS uploads (pso, animated property, matrix, ratios)");
  // Stock end state (every draw by DCS, done above) vs our run: residual
  // draws by DCS, then the restore for the executed segments.
  uint8_t stockMat[kMats][0x300];
  memcpy(stockMat, w->mat, sizeof(stockMat));
  void* stockVar[kShaders][kRecs];
  for (int s = 0; s < kShaders; ++s)
    for (int h = 0; h < kRecs; ++h) stockVar[s][h] = w->vars[s][h].value;
  memcpy(w->mat, w->matInit, sizeof(w->mat));
  for (auto& row : w->vars)
    for (FakeVar& v : row) v.value = nullptr;
  for (int i = 0; i < kN; ++i) {
    if (w->kind[i] || j->items[i].seg != kNoSeg) continue;
    uint8_t dcs[kCbLen];
    w->DcsWrites(i, dcs, &tt);
  }
  *reinterpret_cast<uint64_t*>(w->globalsObj + 0x1e8) = 0;
  uint8_t executed[kMaxSegments];
  memset(executed, 1, sizeof(executed));
  g_setRes.clear();
  const bool restored = RestoreGuarded(*j, executed);
  bool end = restored && memcmp(stockMat, w->mat, sizeof(stockMat)) == 0;
  for (int s = 0; s < kShaders && end; ++s)
    for (int h = 0; h < kRecs && end; ++h) end = w->vars[s][h].value == stockVar[s][h];
  uint64_t tris = 0;
  for (int i = 0; i < kN; ++i)
    if (j->items[i].seg != kNoSeg) tris += *reinterpret_cast<uint64_t*>(w->rend[i] + 0x78);
  end = end && *reinterpret_cast<uint64_t*>(w->globalsObj + 0x1e8) == tris && !g_setRes.empty();
  Check(end, "gbuffer rec job: residual draws + restore leave the materials, FX variables and triangle count as stock");
  // Exec checks: overrides may differ, a guarded dword may not; textures,
  // versions and pages are compared.
  const Seg& s0 = j->segs[0];
  bool chk = CheckSegmentRaw(*j, s0) == kSExecuted;
  uint8_t* m0 = j->mats[j->segMats[s0.matFirst]].mat;
  m0[kCbOff + kMatrixCb] ^= 1;  // an override
  chk = chk && CheckSegmentRaw(*j, s0) == kSExecuted;
  m0[kCbOff + kMatrixCb] ^= 1;
  m0[kCbOff + 0x10] ^= 1;  // a resident dword the key reads
  chk = chk && CheckSegmentRaw(*j, s0) == kSGuard;
  m0[kCbOff + 0x10] ^= 1;
  const TexEntry* e0 = j->segEnts[s0.entFirst];
  const_cast<TexEntry*>(e0)->version.fetch_add(1);
  chk = chk && CheckSegmentRaw(*j, s0) == kSTexture;
  const_cast<TexEntry*>(e0)->version.fetch_sub(1);
  FakeView other;
  void* vsave = *reinterpret_cast<void**>(w->texViews[(e0->tex - w->tex[0]) / 0x700] + 0x10);
  w->SetView(static_cast<int>((e0->tex - w->tex[0]) / 0x700), &other);
  chk = chk && CheckSegmentRaw(*j, s0) == kSTexture && e0->dirty.load() == 1;
  w->SetView(static_cast<int>((e0->tex - w->tex[0]) / 0x700), vsave);
  const_cast<TexEntry*>(e0)->dirty.store(0);
  chk = chk && CheckSegmentRaw(*j, s0) == kSExecuted;
  void* psave = *reinterpret_cast<void**>(w->gpuBuf[j->segPages[s0.pageFirst].page] + 0x30);
  w->SetPageSrv(static_cast<int>(j->segPages[s0.pageFirst].page), &other);
  chk = chk && CheckSegmentRaw(*j, s0) == kSPage;
  w->SetPageSrv(static_cast<int>(j->segPages[s0.pageFirst].page), psave);
  Check(chk, "gbuffer rec job: exec checks (overrides ignored; guard, texture, version and page changes caught)");
  // Streaming replay: one request per (texture, size) of the segment's draws.
  g_vt23.clear();
  uint32_t replays = 0;
  ReplayAndCheckGuarded(*j, s0, &replays);
  std::set<std::pair<void*, uint64_t>> want;
  for (uint32_t c = s0.candFirst; c < s0.candEnd; ++c) {
    const Cand& cd = j->cands[c];
    for (uint32_t q = 0; q < cd.setCount; ++q) want.insert({SetsOf(*j, cd)[q].tex, SetsOf(*j, cd)[q].size});
  }
  bool rep = replays == want.size() && g_vt23.size() == want.size();
  for (auto& kv : g_vt23) rep = rep && kv.second == 1 && want.count(kv.first);
  Check(rep, "gbuffer rec job: the streaming replay makes one request per (texture, size), unread textures included");
  // Stock fallback of a segment: DCS's vt[1] of its items, in order.
  g_vt1Calls.clear();
  DrawSegmentStock(*j, 0, nullptr);
  bool order = g_vt1Calls.size() == s0.draws;
  for (size_t k = 0; k < g_vt1Calls.size() && order; ++k) order = g_vt1Calls[k] == w->rend[s0.first + k];
  Check(order, "gbuffer rec job: a segment drawn stock calls its items' vt[1] in vector order");
  // Missing pieces: reads, key, mesh, texture; blend scope; verify stride; island 1.
  {
    Tables t2;
    AllocTables(t2);
    PubKey(t2, *w, 0);
    PubKey(t2, *w, 3);
    PubReads(t2, *w, 0);  // shader 3's reads unknown
    for (int m = 0; m < kMeshes; ++m)
      if (m != 2) PubMesh(t2, *w, 0, m);  // mesh 2 of shader 0 not probed
    TexTable t3;
    AllocTex(t3, 256);
    FillTex(t3, *w, 4);  // texture 4 (a Normal) not in the table
    InitJob(*j, *w, t2, t3);
    BuildJob(*j);
    bool miss = j->missCount == 1 && j->misses[0].tex == w->tex[4];
    bool readWant = false;
    for (uint32_t k = 0; k < j->readWantCount; ++k) readWant |= j->readWants[k].shader == w->shader[3];
    bool ok = miss && readWant && j->wantCount > 0;
    int wanted = 0;
    for (int i = 0; i < kN && ok; ++i) {
      if (!IsCandShader(*w, i)) continue;
      const int s = World::ShaderOfMat(w->MatOf(i));
      const uint8_t r = j->items[i].reason;
      if (s == 3)
        ok = r == kRUnclean;
      else if (World::MeshOf(i) == 2)
        ok = r == kRMeshPending;
      else if (World::NormalOf(i) == 4)
        ok = r == kRTexMiss;
      wanted += j->items[i].want;
    }
    ok = ok && wanted == 1;  // one probe request for (shader 0, mesh 2)
    Check(ok, "gbuffer rec job: unknown reads (shader unclean, reads requested), unprobed mesh (one probe request), "
              "texture miss (requested once)");
    // The miss is built after the pass (maintenance), then found.
    MaintainTex(t3, w->env.tex, *j, 5);
    Check(TexFind(t3, w->tex[4], 0, kRecType) && TexFind(t3, w->tex[4], 0, kRecType)->usable,
          "gbuffer rec job: the render thread builds the missed texture entries after the pass");
    FreeTex(t3);
    FreeTables(t2);
    // A2C out of scope; the verify stride; island 1 and the segment limit.
    InitJob(*j, *w, t, tt);
    j->scope = 1u << 2;
    BuildJob(*j);
    bool a2c = true;
    for (int i = 0; i < kN && a2c; ++i)
      if (IsCandShader(*w, i) && World::ShaderOfMat(w->MatOf(i)) == 3) a2c = j->items[i].reason == kRBlend;
    Check(a2c, "gbuffer rec job: A2C draws are residual without scope bit 16");
    InitJob(*j, *w, t, tt);
    j->stride = 7;
    BuildJob(*j);
    uint32_t forced = j->reasons[kRForced];
    bool strideOk = forced > 0 && j->segCount == j->maxSeg && j->maxSeg == kDefaultMaxSeg;
    for (uint32_t k = 0; k < j->segCount && strideOk; ++k) strideOk = j->segs[k].end - j->segs[k].first >= 6;
    Check(strideOk, "gbuffer rec job: the verify stride forces every 7th draw residual (island 6, at most 12 segments)");
  }
  // Stage 2 ownship (cockpit_ipd.h, CockpitTrueIpdOwnship=2): a job started with the locked instance's key leaves
  // its items residual (kROwnship) and brackets each run of them in the handed list with the switch entries; every
  // other item keeps its decision unless its run got split below the island; no key: the list is today's.
  {
    InitJob(*j, *w, t, tt);
    j->ownKey = nullptr;
    BuildJob(*j);
    std::vector<uint8_t> base(j->n);
    for (size_t i = 0; i < j->n; ++i) base[i] = j->items[i].reason;
    std::vector<void*> baseList(j->swapList, j->swapList + j->swapCount);
    const Seg s0 = j->segs[0];
    // The instance identity is the item's +0x18 (its texture entries' array, one per instance in DCS; one per item
    // in this fixture): the key of item s0.first + 12 makes that one item the ownship.
    const int ownIdx = static_cast<int>(s0.first + 12);
    std::vector<int> own = {ownIdx};
    void* const ownTag = *reinterpret_cast<void**>(w->item[ownIdx] + 0x18);
    static int inE = 0, outE = 0;
    void* const inE0 = g_ownInE;
    void* const outE0 = g_ownOutE;
    g_ownInE = &inE;
    g_ownOutE = &outE;
    InitJob(*j, *w, t, tt);
    j->ownKey = ownTag;
    j->ownRec = false;  // 0.5.9: ownship items residual
    const bool built2 = BuildJob(*j) == kBuildOk;
    bool ok = built2 && j->ownItems == own.size() && j->ownRuns == 1;
    std::vector<bool> isOwn(j->n, false);
    for (int i : own) isOwn[i] = true;
    for (size_t i = 0; i < j->n && ok; ++i) {
      const uint8_t r = j->items[i].reason;
      if (isOwn[i])
        ok = r == kROwnship && j->items[i].seg == kNoSeg;
      else
        ok = r == base[i] || r == kRIsland || r == kRSegments || (base[i] == kRIsland && r == kRecorded) ||
             (base[i] == kRSegments && r == kRecorded);
      if (!ok) printf("     (ownship job: item %zu reason %u, before %u, own %d)\n", i, r, base[i], (int)isOwn[i]);
    }
    // The list: between in and out only ownship items; every ownship item between them; balanced.
    bool inside = false;
    int ins = 0, outs = 0;
    size_t ownSeen = 0;
    for (uint32_t k = 0; k < j->swapCount && ok; ++k) {
      void* e = j->swapList[k];
      if (e == &inE) {
        ok = !inside;
        inside = true;
        ++ins;
      } else if (e == &outE) {
        ok = inside;
        inside = false;
        ++outs;
      } else {
        int idx = -1;
        for (int i = 0; i < kN; ++i)
          if (e == w->rend[i]) idx = i;
        const bool o = idx >= 0 && isOwn[idx];
        ok = o == inside;
        ownSeen += o;
      }
    }
    const bool listOk = ok;
    ok = ok && !inside && ins == 1 && outs == 1 && ownSeen == own.size();
    if (!ok)
      printf("     (ownship job: built %d, items %u (want %zu), runs %u, list ok %d, in %d out %d, own seen %zu, "
             "segment 0 %u..%u)\n",
             built2, j->ownItems, own.size(), j->ownRuns, listOk, ins, outs, ownSeen, s0.first, s0.end);
    static int otherTag = 0;
    InitJob(*j, *w, t, tt);
    j->ownKey = &otherTag;  // a key with no item of it here: today's list
    j->ownRec = true;
    BuildJob(*j);
    const bool noneSame = j->ownItems == 0 && j->ownRuns == 0 && j->ownRecItems == 0 &&
                          std::vector<void*>(j->swapList, j->swapList + j->swapCount) == baseList;
    Check(ok && noneSame,
          "gbuffer rec job, ownship stage 2 (ownship items residual, 0.5.9): the locked instance's items residual "
          "(kROwnship), each run bracketed by the switch entries in the handed list (balanced, nothing else inside), "
          "the other items' decisions kept (but a run split below the island); with no item of the key here the list "
          "is today's");
    // Recorded ownship segments (the default): a single ownship item is a run below the island: residual (island),
    // still bracketed; nothing else changes from the 0.5.9 build.
    InitJob(*j, *w, t, tt);
    j->ownKey = ownTag;
    j->ownRec = true;
    bool oneOk = BuildJob(*j) == kBuildOk && j->ownItems == 1 && j->ownRuns == 1 && j->ownRecItems == 0 &&
                 j->items[ownIdx].reason == kRIsland && j->items[ownIdx].seg == kNoSeg;
    // An ownship run of island + 4 items inside the longest recorded segment: recorded as a segment of its own,
    // bracketed by exactly one switch-in / switch-out around its exec entry; the items before it (5) fall below the
    // island; the rest of the old segment stays recorded when long enough.
    uint32_t longest = 0;
    InitJob(*j, *w, t, tt);
    j->ownKey = nullptr;
    BuildJob(*j);
    for (uint32_t k = 1; k < j->segCount; ++k)
      if (j->segs[k].end - j->segs[k].first > j->segs[longest].end - j->segs[longest].first) longest = k;
    const Seg sl = j->segs[longest];
    const uint32_t runN = kDefaultIsland + 4;
    bool segOk = sl.end - sl.first >= runN + 5;
    const uint32_t a = sl.first + 5, b = a + runN;
    std::vector<void*> tag0(j->n);
    for (size_t i = 0; i < j->n; ++i) tag0[i] = *reinterpret_cast<void**>(w->item[i] + 0x18);
    void* const runTag = tag0[a];
    if (segOk)
      for (uint32_t i = a; i < b; ++i) *reinterpret_cast<void**>(w->item[i] + 0x18) = runTag;
    InitJob(*j, *w, t, tt);
    j->ownKey = runTag;
    j->ownRec = true;
    segOk = segOk && BuildJob(*j) == kBuildOk && j->ownItems == runN && j->ownRecItems == runN && j->ownRuns == 1;
    const uint8_t ownSeg = segOk ? j->items[a].seg : kNoSeg;
    segOk = segOk && ownSeg != kNoSeg && j->segs[ownSeg].first == a && j->segs[ownSeg].end == b;
    for (uint32_t i = a; i < b && segOk; ++i) segOk = j->items[i].reason == kRecorded && j->items[i].seg == ownSeg;
    for (uint32_t i = sl.first; i < a && segOk; ++i) segOk = j->items[i].reason == kRIsland;
    const bool restRec = sl.end - b >= kDefaultIsland;
    for (uint32_t i = b; i < sl.end && segOk; ++i)
      segOk = restRec ? (j->items[i].reason == kRecorded && j->items[i].seg != ownSeg) : j->items[i].reason == kRIsland;
    int ins2 = 0, outs2 = 0, inside2 = 0;
    bool inner = segOk;
    for (uint32_t k = 0; k < j->swapCount && inner; ++k) {
      void* e = j->swapList[k];
      if (e == &inE) {
        ++ins2;
        inner = !inside2;
        inside2 = 1;
      } else if (e == &outE) {
        ++outs2;
        inner = inside2 == 1 || inside2 == 2;
        inside2 = 0;
      } else if (inside2) {
        inner = inside2 == 1 && e == j->execObj[ownSeg];  // only the ownship segment's exec entry inside
        inside2 = 2;
      }
    }
    segOk = segOk && inner && ins2 == 1 && outs2 == 1 && !inside2;
    if (!segOk || !oneOk)
      printf("     (ownship segment: one-item ok %d; longest segment %u..%u, run %u..%u, seg %u, items %u rec %u runs %u, "
             "in %d out %d)\n",
             oneOk, sl.first, sl.end, a, b, ownSeg, j->ownItems, j->ownRecItems, j->ownRuns, ins2, outs2);
    for (size_t i = 0; i < j->n; ++i) *reinterpret_cast<void**>(w->item[i] + 0x18) = tag0[i];
    j->ownKey = nullptr;
    g_ownInE = inE0;
    g_ownOutE = outE0;
    Check(oneOk && segOk,
          "gbuffer rec job, ownship stage 2: the locked instance's run recorded as a segment of its own (exactly its "
          "items), its exec entry alone between one switch-in and one switch-out; the neighbours judged by the island "
          "on their own; a single ownship item below the island stays residual and bracketed");
  }
  // Stage 2's fallback: the request consumed once, the vector handed to the stage-1 swap.
  {
    static int calls = 0;
    calls = 0;
    auto* const fn0 = g_ownSwapFn;
    g_ownSwapFn = [](void** v) -> bool {
      ++calls;
      return v != nullptr;
    };
    void* vec[2] = {nullptr, nullptr};
    const uint64_t f0 = g_own2Fallback.load();
    const uint64_t w0 = g_own2FbWhy[kOwnFbScope].load(), w1 = g_own2FbWhy[kOwnFbStock].load(),
                   w2 = g_own2FbWhy[kOwnFbKey].load();
    t_own2 = true;
    Own2Fallback(vec);
    Own2Fallback(vec);
    const bool fb = calls == 1 && !t_own2 && g_own2Fallback.load() == f0 + 1;
    t_own2 = false;
    Own2Fallback(vec);
    t_own2 = true;
    Own2Fallback(vec, kOwnFbStock);
    t_own2 = true;
    Own2Fallback(vec, kOwnFbKey);
    const bool whyOk = g_own2FbWhy[kOwnFbScope].load() == w0 + 1 && g_own2FbWhy[kOwnFbStock].load() == w1 + 1 &&
                       g_own2FbWhy[kOwnFbKey].load() == w2 + 1 && g_own2Fallback.load() == f0 + 3 && calls == 3;
    g_ownSwapFn = fn0;
    Check(fb && whyOk, "gbuffer rec, ownship stage 2: the fallback hands the vector to stage 1 once per request, "
                       "never without one, counted by cause (outside the recorder's executions, stock anyway, a job "
                       "for another instance)");
  }
  FreeJob(j);
  FreeTex(tt);
  FreeTables(t);
  delete w;
}

// ---------------------------------------------------------------------------
// Device test: the S1 harness and S3 end to end
// ---------------------------------------------------------------------------
constexpr UINT kSize = 64;
constexpr int kTargets = 6;

struct Dev {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11VertexShader* vsOther = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11PixelShader* psA2c = nullptr;
  ID3D11PixelShader* psDecal = nullptr;
  ID3D11PixelShader* psOther = nullptr;
  ID3D11InputLayout* il = nullptr;
  ID3D11DepthStencilState* dss[3] = {};  // stencil refs 3 (shader 0), 5 (shader 3), 7 (shaders 1, 2)
  ID3D11DepthStencilState* dssDecal = nullptr;
  ID3D11BlendState* bsNone = nullptr;
  ID3D11BlendState* bsA2c = nullptr;
  ID3D11BlendState* bsDecal = nullptr;
  ID3D11RasterizerState* rs = nullptr;
  ID3D11SamplerState* samp = nullptr;
  ID3D11SamplerState* pool = nullptr;  // the pass's pool sampler at s5
  ID3D11Buffer* b6 = nullptr;
  ID3D11Buffer* b7 = nullptr;
  ID3D11Buffer* ours[2][2] = {};  // VS/PS copies of b6, b7
  ID3D11Buffer* matCb[kMats] = {};
  ID3D11Buffer* page[kPages] = {};
  ID3D11ShaderResourceView* pageSrv[kPages] = {};
  ID3D11Buffer* vb[kMeshes] = {};
  ID3D11Buffer* ib[kMeshes] = {};
  ID3D11Buffer* otherVb = nullptr;
  ID3D11Texture2D* texRes[kTex] = {};
  ID3D11ShaderResourceView* texSrv[kTex] = {};
  ID3D11Texture2D* rt = nullptr;
  ID3D11RenderTargetView* rtv[kTargets] = {};
  ID3D11Texture2D* depth = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11Texture2D* rtStage = nullptr;
  ID3D11Texture2D* dsStage = nullptr;
  VsStatic st;
};

template <class T>
void Rel(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

void Destroy(Dev& d) {
  Rel(d.vs), Rel(d.vsOther), Rel(d.ps), Rel(d.psA2c), Rel(d.psDecal), Rel(d.psOther), Rel(d.il);
  for (auto*& p : d.dss) Rel(p);
  Rel(d.dssDecal), Rel(d.bsNone), Rel(d.bsA2c), Rel(d.bsDecal), Rel(d.rs), Rel(d.samp), Rel(d.pool), Rel(d.b6), Rel(d.b7);
  for (auto& row : d.ours)
    for (auto*& p : row) Rel(p);
  for (auto*& p : d.matCb) Rel(p);
  for (auto*& p : d.page) Rel(p);
  for (auto*& p : d.pageSrv) Rel(p);
  for (auto*& p : d.vb) Rel(p);
  for (auto*& p : d.ib) Rel(p);
  Rel(d.otherVb);
  for (auto*& p : d.texRes) Rel(p);
  for (auto*& p : d.texSrv) Rel(p);
  Rel(d.rt);
  for (auto*& p : d.rtv) Rel(p);
  Rel(d.depth), Rel(d.dsv), Rel(d.rtStage), Rel(d.dsStage);
  Rel(d.imm), Rel(d.dev);
}

ID3D11Buffer* MakeBuffer(ID3D11Device* dev, UINT bytes, UINT bind, D3D11_USAGE usage, const void* init, UINT misc = 0,
                         UINT stride = 0) {
  return srtest::MakeBuffer(dev, bytes, bind, usage, init, misc, stride);
}

bool CreateDev(Dev& d, srtest::Compiler& c, World& w, bool* warp) {
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  *warp = false;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2, D3D11_SDK_VERSION, &d.dev,
                               nullptr, &d.imm))) {
    *warp = true;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 2, D3D11_SDK_VERSION, &d.dev, nullptr,
                                 &d.imm)))
      return false;
  }
  auto build = [&](const char* entry, const char* target) { return c.Build(kGbHlsl, sizeof(kGbHlsl) - 1, entry, target, nullptr); };
  ID3DBlob* b = build("vs", "vs_5_0");
  if (!b) return false;
  d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &d.vs);
  const D3D11_INPUT_ELEMENT_DESC el[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  d.dev->CreateInputLayout(el, 1, b->GetBufferPointer(), b->GetBufferSize(), &d.il);
  const char* why = AnalyseGbVs(c.reflect, b->GetBufferPointer(), b->GetBufferSize(), "def_uniforms", d.st);
  b->Release();
  if (why) return false;
  struct {
    const char* e;
    ID3D11PixelShader** out;
  } pss[] = {{"ps", &d.ps}, {"psA2c", &d.psA2c}, {"psDecal", &d.psDecal}, {"psOther", &d.psOther}};
  for (auto& p : pss) {
    if (!(b = build(p.e, "ps_5_0"))) return false;
    d.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, p.out);
    b->Release();
  }
  if (!(b = build("vsOther", "vs_5_0"))) return false;
  d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &d.vsOther);
  b->Release();
  D3D11_DEPTH_STENCIL_DESC dd = {};
  dd.DepthEnable = TRUE;
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
  dd.StencilEnable = TRUE;
  dd.StencilReadMask = dd.StencilWriteMask = 0xff;
  dd.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
  dd.BackFace = dd.FrontFace;
  for (auto*& p : d.dss) d.dev->CreateDepthStencilState(&dd, &p);
  dd.DepthEnable = FALSE;  // decals blend wherever they cover: their order against the model draws matters
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  dd.StencilEnable = FALSE;
  d.dev->CreateDepthStencilState(&dd, &d.dssDecal);
  D3D11_BLEND_DESC bd = {};
  bd.IndependentBlendEnable = TRUE;
  for (int k = 0; k < kTargets; ++k) bd.RenderTarget[k].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  d.dev->CreateBlendState(&bd, &d.bsNone);
  bd.AlphaToCoverageEnable = TRUE;
  d.dev->CreateBlendState(&bd, &d.bsA2c);
  D3D11_BLEND_DESC bdec = {};
  bdec.IndependentBlendEnable = TRUE;
  bdec.RenderTarget[0] = {TRUE,
                          D3D11_BLEND_SRC_ALPHA,
                          D3D11_BLEND_INV_SRC_ALPHA,
                          D3D11_BLEND_OP_ADD,
                          D3D11_BLEND_ONE,
                          D3D11_BLEND_ZERO,
                          D3D11_BLEND_OP_ADD,
                          D3D11_COLOR_WRITE_ENABLE_ALL};
  d.dev->CreateBlendState(&bdec, &d.bsDecal);
  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  d.dev->CreateRasterizerState(&rd, &d.rs);
  D3D11_SAMPLER_DESC sdsc = {};
  sdsc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sdsc.AddressU = sdsc.AddressV = sdsc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  sdsc.MaxLOD = D3D11_FLOAT32_MAX;
  d.dev->CreateSamplerState(&sdsc, &d.samp);
  sdsc.AddressU = sdsc.AddressV = sdsc.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
  d.dev->CreateSamplerState(&sdsc, &d.pool);
  const float b6[64] = {0.25f, 0.5f, 0.75f, 1.0f};
  const float b7[64] = {0.9f, 1.3f, 0, 0};
  d.b6 = MakeBuffer(d.dev, sizeof(b6), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT, b6);
  d.b7 = MakeBuffer(d.dev, sizeof(b7), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT, b7);
  for (auto& row : d.ours)
    for (auto*& p : row) p = MakeBuffer(d.dev, sizeof(b6), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT, nullptr);
  for (auto*& p : d.matCb) p = MakeBuffer(d.dev, kCbLen, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, nullptr);
  uint32_t rng = 99;
  auto rnd = [&](float a, float bb) {
    rng = rng * 1664525u + 1013904223u;
    return a + (bb - a) * static_cast<float>(rng >> 8) / 16777216.0f;
  };
  for (int p = 0; p < kPages; ++p) {
    std::vector<float> pos(kPerPage * 4);
    for (int k = 0; k < kPerPage; ++k) {
      pos[k * 4 + 0] = rnd(-0.8f, 0.8f);
      pos[k * 4 + 1] = rnd(-0.8f, 0.8f);
      pos[k * 4 + 2] = rnd(0.2f, 0.8f);
      pos[k * 4 + 3] = rnd(0.6f, 1.6f);
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
    const int verts = 12;
    std::vector<float> v(verts * 3);
    for (float& x : v) x = rnd(-0.25f, 0.25f);
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
  const float quad[] = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, -0.5f, 0.5f, 0, 0.5f, -0.5f, 0, 0.5f, 0.5f, 0, -0.5f, 0.5f, 0};
  d.otherVb = MakeBuffer(d.dev, sizeof(quad), D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, quad);
  for (int t = 0; t < kTex; ++t) {
    uint32_t px[8 * 8];
    for (uint32_t& v : px) v = static_cast<uint32_t>(rnd(0, 1) * 0xffffff) | (static_cast<uint32_t>(rnd(0, 255)) << 24);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 8;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {px, 8 * 4, 0};
    d.dev->CreateTexture2D(&td, &sd, &d.texRes[t]);
    if (d.texRes[t]) d.dev->CreateShaderResourceView(d.texRes[t], nullptr, &d.texSrv[t]);
    w.SetView(t, d.texSrv[t]);
  }
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = td.Height = kSize;
  td.MipLevels = 1;
  td.ArraySize = kTargets;
  td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  d.dev->CreateTexture2D(&td, nullptr, &d.rt);
  for (int k = 0; k < kTargets && d.rt; ++k) {
    D3D11_RENDER_TARGET_VIEW_DESC rv = {};
    rv.Format = td.Format;
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    rv.Texture2DArray.FirstArraySlice = k;
    rv.Texture2DArray.ArraySize = 1;
    d.dev->CreateRenderTargetView(d.rt, &rv, &d.rtv[k]);
  }
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.dev->CreateTexture2D(&td, nullptr, &d.rtStage);
  D3D11_TEXTURE2D_DESC dsd = {};
  dsd.Width = dsd.Height = kSize;
  dsd.MipLevels = dsd.ArraySize = 1;
  dsd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  dsd.SampleDesc.Count = 1;
  dsd.Usage = D3D11_USAGE_DEFAULT;
  dsd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  d.dev->CreateTexture2D(&dsd, nullptr, &d.depth);
  if (d.depth) d.dev->CreateDepthStencilView(d.depth, nullptr, &d.dsv);
  dsd.Usage = D3D11_USAGE_STAGING;
  dsd.BindFlags = 0;
  dsd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.dev->CreateTexture2D(&dsd, nullptr, &d.dsStage);
  bool ok = d.vs && d.vsOther && d.ps && d.psA2c && d.psDecal && d.psOther && d.il && d.dssDecal && d.bsNone &&
            d.bsA2c && d.bsDecal && d.rs && d.samp && d.b6 && d.b7 && d.otherVb && d.rt && d.dsv && d.rtStage &&
            d.dsStage;
  for (auto* p : d.dss) ok = ok && p;
  for (auto* p : d.rtv) ok = ok && p;
  for (auto* p : d.texSrv) ok = ok && p;
  for (auto* p : d.pageSrv) ok = ok && p;
  for (auto* p : d.matCb) ok = ok && p;
  for (int m = 0; m < kMeshes; ++m) ok = ok && d.vb[m] && d.ib[m];
  return ok;
}

const D3D11_VIEWPORT kVpFull = {0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0, 1};

// DCS's G-buffer pass setup: frame buffer (6 RTVs + DSV, viewport), clears,
// the context buffers b6 and b7 on both stages.
void PassSetup(Dev& d) {
  ID3D11DeviceContext* c = d.imm;
  c->ClearState();
  c->OMSetRenderTargets(kTargets, d.rtv, d.dsv);
  c->RSSetViewports(1, &kVpFull);
  const float clr[4] = {0.1f, 0.2f, 0.3f, 0.4f};
  for (auto* r : d.rtv) c->ClearRenderTargetView(r, clr);
  c->ClearDepthStencilView(d.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
  ID3D11Buffer* ctx[2] = {d.b6, d.b7};
  c->VSSetConstantBuffers(6, 2, ctx);
  c->PSSetConstantBuffers(6, 2, ctx);
  c->VSSetSamplers(5, 1, &d.pool);
  c->PSSetSamplers(5, 1, &d.pool);
}

// What DCS's draw leaves bound for model item i, and the draw.
void DrawDcs(Dev& d, World& w, int i, const TexTable& tt) {
  ID3D11DeviceContext* c = d.imm;
  const int m = w.MatOf(i), s = World::ShaderOfMat(m), k = World::MeshOf(i), p = World::PageOf(i);
  uint8_t bytes[kCbLen];
  w.DcsWrites(i, bytes, &tt);  // the material writes and the FX variable sets
  D3D11_MAPPED_SUBRESOURCE ms;
  if (SUCCEEDED(c->Map(d.matCb[m], 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
    memcpy(ms.pData, bytes, kCbLen);
    c->Unmap(d.matCb[m], 0);
  }
  // Apply: what the variables hold (an unset record keeps an earlier view).
  ID3D11ShaderResourceView* tv[2] = {static_cast<ID3D11ShaderResourceView*>(w.vars[s][0].value),
                                     static_cast<ID3D11ShaderResourceView*>(w.vars[s][1].value)};
  c->PSSetShaderResources(0, 2, tv);
  c->PSSetSamplers(0, 1, &d.samp);
  c->VSSetShader(d.vs, nullptr, 0);
  c->PSSetShader(s == 3 ? d.psA2c : d.ps, nullptr, 0);
  // A cockpit item (gb_rec_cockpit_test.h): normal_cockpit*'s stencil ref 40 (STENCIL_COMPOSITION_COCKPIT).
  c->OMSetDepthStencilState(d.dss[s == 0 ? 0 : s == 3 ? 1 : 2], w.rend[i][0x64] ? 40 : s == 0 ? 3 : s == 3 ? 5 : 7);
  c->OMSetBlendState(s == 3 ? d.bsA2c : d.bsNone, nullptr, 0xffffffff);
  c->RSSetState(d.rs);
  c->IASetInputLayout(d.il);
  const UINT stride = 12, zero = 0;
  c->IASetVertexBuffers(0, 1, &d.vb[k], &stride, &zero);
  c->IASetIndexBuffer(d.ib[k], (k & 1) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c->VSSetConstantBuffers(2, 1, &d.matCb[m]);
  c->PSSetConstantBuffers(2, 1, &d.matCb[m]);
  ID3D11ShaderResourceView* sb = static_cast<ID3D11ShaderResourceView*>(w.vars[s][kSbRec].value);
  c->VSSetShaderResources(3, 1, &sb);
  (void)p;
  c->DrawIndexed(3 * w.tris[k], 0, 0);
}

// Other renderables (terrain-like, opaque) and decals (blended over what is there).
void DrawOtherKind(Dev& d, int kind) {
  ID3D11DeviceContext* c = d.imm;
  c->VSSetShader(d.vsOther, nullptr, 0);
  c->PSSetShader(kind == 1 ? d.psOther : d.psDecal, nullptr, 0);
  c->OMSetDepthStencilState(kind == 1 ? d.dss[2] : d.dssDecal, 9);
  c->OMSetBlendState(kind == 1 ? d.bsNone : d.bsDecal, nullptr, 0xffffffff);
  c->RSSetState(d.rs);
  c->IASetInputLayout(d.il);
  const UINT stride = 12, zero = 0;
  c->IASetVertexBuffers(0, 1, &d.otherVb, &stride, &zero);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c->Draw(6, 0);
}

void DrawItem(Dev& d, World& w, int i, const TexTable& tt) {
  if (w.kind[i])
    DrawOtherKind(d, w.kind[i]);
  else
    DrawDcs(d, w, i, tt);
}

std::vector<uint8_t> ReadTargets(Dev& d) {
  std::vector<uint8_t> out;
  d.imm->CopyResource(d.rtStage, d.rt);
  d.imm->CopyResource(d.dsStage, d.depth);
  for (int k = 0; k <= kTargets; ++k) {
    ID3D11Texture2D* st = k < kTargets ? d.rtStage : d.dsStage;
    const UINT sub = k < kTargets ? static_cast<UINT>(k) : 0;
    const UINT bpp = k < kTargets ? 8 : 4;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(d.imm->Map(st, sub, D3D11_MAP_READ, 0, &m))) return {};
    for (UINT y = 0; y < kSize; ++y) {
      const uint8_t* row = static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
      out.insert(out.end(), row, row + kSize * bpp);
    }
    d.imm->Unmap(st, sub);
  }
  return out;
}

// The probe as the render thread makes it: DCS's draw, the read-back, the
// read sets from the FX variables, the checks and the publish.
void ProbeOn(Dev& d, World& w, Tables& t, int i, const TexTable& tt) {
  DrawDcs(d, w, i, tt);
  shrec::Capture c;
  Extra x;
  shrec::CaptureState(d.imm, d.st.cbSlot, d.st.sbSlot, c);
  CaptureExtra(d.imm, x);
  c.draws = 1;
  c.prim = static_cast<int>(shrec::kPrimTriList);
  c.a5 = static_cast<int>(w.tris[World::MeshOf(i)]);
  c.rendererOk = true;
  ProbeIn in;
  in.r = w.rend[i];
  in.item = w.item[i];
  in.mat = w.mat[w.MatOf(i)];
  in.shader = *reinterpret_cast<uint8_t**>(in.mat + 0x30);
  in.tech = w.rend[i][0x64] ? 2 : 1;  // a cockpit item: [mat+0x1e0] (gb_rec_cockpit_test.h)
  in.mesh = w.mesh[World::MeshOf(i)];
  in.page = static_cast<uint32_t>(World::PageOf(i));
  in.effect = *reinterpret_cast<void**>(in.shader + 0x50);
  in.techBegin = *reinterpret_cast<void**>(in.shader + 0xb0);
  Facts f;
  f.dcsVs = d.vs;
  f.pageSrv = PageSrv(w.env, in.page);
  f.meshWhy = shrec::ReadMesh(in.mesh, f.mesh);
  f.passCtx[0][0] = f.passCtx[1][0] = d.b6;
  f.passCtx[0][1] = f.passCtx[1][1] = d.b7;
  f.reads = FindReads(t, in.shader, in.tech, in.effect, in.techBegin);
  if (f.reads) ReadSetsRaw(in, *f.reads, f);
  static void* list[1];
  list[0] = f.mesh.vbObj;
  *reinterpret_cast<void***>(in.shader + 0x1a8) = list;
  *reinterpret_cast<uint64_t*>(in.shader + 0x1b0) = 1;
  *reinterpret_cast<uint32_t*>(in.shader + 0x110) = f.mesh.elems;
  for (uint32_t e = 0; e < f.mesh.elems; ++e)
    *reinterpret_cast<uint32_t*>(in.shader + 0x114 + 4 * e) = srtest::PackDesc(6, 0, 1 + e, 0, false);
  shrec::ReadStreams(in.shader, c);
  ProcessProbe(t, in, d.st, f, c, x);
  shrec::ReleaseCapture(c);
  ReleaseExtra(x);
}

// Records job j on the pool's workers 0-2 (lists by piece). parallelRoles:
// stage A on this thread, then the pieces for 3 workers recorded by RecordRole
// on workers 0, 1, 2 at once (deterministic split); else the production path
// (JobMain with HelperMain<1>, HelperMain<2>: the split depends on timing).
struct RoleArg {
  Job* j;
  int role;
  bool ok;
};
bool RoleFn(defrec::Worker& w, void* u) {
  auto* a = static_cast<RoleArg*>(u);
  a->ok = RecordRole(w, *a->j, a->role);
  return false;  // every piece is finished by RecordRole
}
bool RecordOnPool(defrec::Pool& pool, Job& j, std::vector<ID3D11CommandList*>& cls, bool parallelRoles) {
  cls.clear();
  if (parallelRoles) {
    if (BuildJob(j) != kBuildOk || !j.segCount) return false;
    PlanPieces(j, kThreads);
    RoleArg args[kThreads];
    for (int r = 0; r < kThreads; ++r) {
      args[r] = {&j, r, false};
      if (!pool.Submit(r, &RoleFn, &args[r], nullptr)) return false;
    }
    pool.Wait(60000);
    for (int r = 0; r < kThreads; ++r)
      if (!args[r].ok) return false;
  } else {
    j.startState.store(kStartArmed);
    ResetEvent(j.start);
    ResetEvent(j.preGo);
    ResetEvent(j.recGo);
    j.preState.store(kGoWait);
    j.recState.store(kGoWait);
    j.started.store(0);
    j.ready.store(0);
    j.recFail.store(0);
    j.recWorkers = 1;
    j.pieceCount = 0;
    if (!pool.Submit(1, &HelperMain<1>, &j, nullptr) || !pool.Submit(2, &HelperMain<2>, &j, nullptr) ||
        !pool.Submit(0, &JobMain, &j, nullptr))
      return false;
    if (!StartJob(j, j.vec, false)) return false;
    pool.Wait(60000);
    for (int w = 0; w < 3; ++w)
      if (pool.Busy(w)) return false;
    if (j.result != kBuildOk || !j.segCount || j.recFail.load()) return false;
  }
  for (uint32_t q = 0; q < j.pieceCount; ++q) {
    cls.push_back(j.pieces[q].cl);
    j.pieces[q].cl = nullptr;
  }
  for (auto* l : cls)
    if (!l) return false;
  return !cls.empty();
}

// Stage 2's ownship switch in the device harness: DCS's bound b7 (d.b7) holds the "true eye" bytes while in.
const float kB7Dcs[64] = {0.9f, 1.3f, 0, 0};
const float kB7True[64] = {0.83f, 1.45f, 0, 0};
void OwnSwitchB7(Dev& d, bool in) { d.imm->UpdateSubresource(d.b7, 0, nullptr, in ? kB7True : kB7Dcs, 0, 0); }

// Our pass: DCS's loop over the caster list; each exec entry copies DCS's
// bound b6/b7 into ours and executes its segment (state compared around it);
// stage 2's switch entries (g_ownInE / g_ownOutE) switch DCS's b7.
// late: every segment executed after the whole list (the order control).
bool RunRecorded(Dev& d, World& w, Job& j, const std::vector<ID3D11CommandList*>& cls, const TexTable& tt, bool late,
                 bool* stateSame) {
  PassSetup(d);
  *stateSame = true;
  auto exec = [&](uint32_t k) {
    ID3D11Buffer* now[2][3] = {};
    d.imm->VSGetConstantBuffers(6, 3, now[0]);
    d.imm->PSGetConstantBuffers(6, 3, now[1]);
    for (int s = 0; s < 2; ++s)
      for (int q = 0; q < 2; ++q)
        if (j.ctxBuf[s][q] && now[s][q]) d.imm->CopyResource(j.ctxBuf[s][q], now[s][q]);
    for (auto& row : now)
      for (auto*& p : row) Rel(p);
    defrec::PassState before = {}, after = {};
    defrec::Capture(d.imm, &before);
    if (j.redoState[k].load() == kRedoOk && j.redoCl[k])  // re-recorded during the pass
      d.imm->ExecuteCommandList(j.redoCl[k], TRUE);
    else
      for (uint32_t q = j.segs[k].pieceFirst; q < j.segs[k].pieceEnd; ++q) d.imm->ExecuteCommandList(cls[q], TRUE);
    defrec::Capture(d.imm, &after);
    *stateSame = *stateSame && defrec::Equal(before, after);
    defrec::Release(before);
    defrec::Release(after);
  };
  for (uint32_t e = 1; e < j.swapCount; ++e) {
    void* p = j.swapList[e];
    if (p && (p == g_ownInE || p == g_ownOutE)) {
      OwnSwitchB7(d, p == g_ownInE);
      continue;
    }
    bool isExec = false;
    for (uint32_t k = 0; k < j.segCount; ++k)
      if (p == j.execObj[k]) {
        isExec = true;
        if (!late) exec(k);
      }
    if (isExec) continue;
    int i = 0;
    while (i < kN && w.rend[i] != p) ++i;
    if (i == kN) return false;
    DrawItem(d, w, i, tt);
  }
  if (late)
    for (uint32_t k = 0; k < j.segCount; ++k) exec(k);
  return true;
}

void ResetDcsState(World& w) {
  memcpy(w.mat, w.matInit, sizeof(w.mat));
  for (auto& row : w.vars)
    for (FakeVar& v : row) v.value = nullptr;
}

// Issue #3 replay: the first access violation while armed bumps the frame-heap generation before the frame-based
// handlers run (DCS's reset_memory landing between RecordRange's check and its read).
std::atomic<int> g_raceResetArmed{0};
LONG CALLBACK RaceResetVeh(EXCEPTION_POINTERS* ep) {
  if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && g_raceResetArmed.exchange(0))
    g_frameHeapGen.fetch_add(1);
  return EXCEPTION_CONTINUE_SEARCH;
}

void DeviceTests(srtest::Compiler& comp) {
  World* w = new World;
  w->Init();
  Dev d;
  bool warp = false;
  if (!CreateDev(d, comp, *w, &warp)) {
    Check(false, "gbuffer rec device: test objects");
    Destroy(d);
    delete w;
    return;
  }
  printf("     gbuffer rec device test on %s\n", warp ? "WARP" : "the hardware device");
  Tables t;
  AllocTables(t);
  TexTable tt;
  AllocTex(tt, 256);
  FillTex(tt, *w);
  for (int s = 0; s < kShaders; ++s) PubReads(t, *w, s);
  // Stock: DCS draws every item in vector order.
  ResetDcsState(*w);
  PassSetup(d);
  for (int i = 0; i < kN; ++i) DrawItem(d, *w, i, tt);
  const std::vector<uint8_t> ref = ReadTargets(d);
  // Probes: the first item of every (shader, mesh), in a pass of their own.
  ResetDcsState(*w);
  PassSetup(d);
  std::set<std::pair<int, int>> probed;
  for (int i = 0; i < kN; ++i)
    if (IsCandShader(*w, i) && probed.insert({World::ShaderOfMat(w->MatOf(i)), World::MeshOf(i)}).second)
      ProbeOn(d, *w, t, i, tt);
  const GbKey* k0 = FindKey(t, w->shader[0], 1, 0, *reinterpret_cast<void**>(w->shader[0] + 0x50),
                            *reinterpret_cast<void**>(w->shader[0] + 0xb0));
  const GbKey* k3 = FindKey(t, w->shader[3], 1, 0, *reinterpret_cast<void**>(w->shader[3] + 0x50),
                            *reinterpret_cast<void**>(w->shader[3] + 0xb0));
  Check(k0 && k3 && k0->state.load() > 0 && k3->state.load() > 0 && k0->vs == d.vs && k0->ps == d.ps &&
            k3->ps == d.psA2c && k0->stencilRef == 3 && k3->stencilRef == 5 && k3->bs == d.bsA2c &&
            k0->psTexCount == 2 && k0->psCbMask == (1u << 2) && k0->psCtxMask == ((1u << 6) | (1u << 7)) &&
            t.mesh.meshesUsed == 2u * kMeshes,
        "gbuffer rec device: probes publish both keys (states, stencil refs, PS slots, context slots) and every mesh");
  defrec::Pool pool;
  defrec::PoolConfig pc;
  pc.workers = kThreads;
  pc.cb.mode = defrec::CbMode::kOffsets;
  pc.name = "gbuffer rec test";
  Job* j = NewJob();
  if (!pool.Start(d.dev, pc) || pool.At(0).ring.Mode() != defrec::CbMode::kOffsets || !j) {
    printf("SKIP gbuffer rec device: no worker with constant-buffer offsets on this device\n");
  } else {
    auto setup = [&](uint32_t stride) {
      InitJob(*j, *w, t, tt);
      j->stride = stride;
      ReleaseTargetRefs(*j);
      for (int k = 0; k < kTargets; ++k) {
        j->rtv[k] = d.rtv[k];
        j->rtv[k]->AddRef();
      }
      j->dsv = d.dsv;
      j->dsv->AddRef();
      j->nvp = 1;
      j->vp[0] = kVpFull;
      j->nsc = 0;
      j->ctxBuf[0][0] = d.ours[0][0];
      j->ctxBuf[0][1] = d.ours[0][1];
      j->ctxBuf[1][0] = d.ours[1][0];
      j->ctxBuf[1][1] = d.ours[1][1];
      for (int st = 0; st < 2; ++st) {
        j->pool[st][5] = d.pool;
        d.pool->AddRef();
      }
    };
    std::vector<ID3D11CommandList*> cls;
    auto runExact = [&](bool* stateSame) {
      ResetDcsState(*w);
      bool ok = RunRecorded(d, *w, *j, cls, tt, false, stateSame);
      const std::vector<uint8_t> got = ReadTargets(d);
      size_t diff = ref.size() == got.size() ? 0 : SIZE_MAX;
      for (size_t k = 0; diff != SIZE_MAX && k < ref.size(); ++k) diff += ref[k] != got[k];
      printf("     6 targets + depth-stencil: %zu bytes, %zu differ\n", ref.size(), diff);
      return ok && diff == 0;
    };
    // 1. The production path: JobMain with two helpers (split by arrival).
    setup(0);
    j->splitMin = 16;
    ResetDcsState(*w);  // the job reads the materials before the pass
    const bool rec = RecordOnPool(pool, *j, cls, false);
    printf("     %u of %d items recorded in %u segment(s), %u draws; %u recording worker(s), %u lists; stage A read %u + "
           "%u + %u items\n",
           j->recorded, kN, j->segCount, j->draws, j->recWorkers, j->pieceCount, j->preItems[0], j->preItems[1],
           j->preItems[2]);
    bool stateSame = false;
    const bool same = rec && runExact(&stateSame);
    Check(rec && j->segCount >= 1 && j->recorded > 60 && same && stateSame &&
              j->preItems[0] + j->preItems[1] + j->preItems[2] == static_cast<uint32_t>(kN),
          "gbuffer rec device: JobMain + helpers: segment lists between DCS's residual draws equal the stock pass bit "
          "for bit (6 RTs, depth, stencil); immediate state unchanged by each Execute");
    // Control: the lists executed after every residual draw (order lost) differ.
    if (rec) {
      ResetDcsState(*w);
      bool ss = false;
      RunRecorded(d, *w, *j, cls, tt, true, &ss);
      const std::vector<uint8_t> got = ReadTargets(d);
      size_t diff = 0;
      for (size_t k = 0; k < ref.size() && k < got.size(); ++k) diff += ref[k] != got[k];
      Check(diff > 0, "gbuffer rec device: (control) the same lists out of order differ: the harness sees order");
    }
    for (auto*& l : cls) Rel(l);
    // Stage 2's ownship recorded (live 2026-10-14: 0.5.9 left its items residual, so an execution showing mostly
    // the ownship went stock). A run of island + 6 items of one instance inside the longest segment: recorded as a
    // segment of its own between a switch-in and a switch-out of DCS's b7; the pass equals stock drawn with the
    // ownship's b7 switched around the same items, bit for bit; the same lists without the switch differ (control).
    {
      setup(0);
      ResetDcsState(*w);
      BuildJob(*j);
      uint32_t longest = 0;
      for (uint32_t k = 1; k < j->segCount; ++k)
        if (j->segs[k].end - j->segs[k].first > j->segs[longest].end - j->segs[longest].first) longest = k;
      const uint32_t f0 = j->segs[longest].first, e0 = j->segs[longest].end;
      const uint32_t runN = kDefaultIsland + 6, a = f0 + 3, b = a + runN;
      bool ownOk = j->segCount > 0 && e0 - f0 >= runN + 3;
      std::vector<void*> tag0(kN);
      for (int i = 0; i < kN; ++i) tag0[i] = *reinterpret_cast<void**>(w->item[i] + 0x18);
      void* const runTag = tag0[a];
      // (the fixture's instance key is the texture entries' array: the run's items share item a's entries, in the
      // stock draws too)
      std::vector<std::vector<uint64_t>> ent0(kN, std::vector<uint64_t>(3 * kHandles));
      for (int i = 0; i < kN; ++i) memcpy(ent0[i].data(), w->entries[i], sizeof(w->entries[i]));
      if (ownOk)
        for (uint32_t i = a; i < b; ++i) {
          *reinterpret_cast<void**>(w->item[i] + 0x18) = runTag;
          memcpy(w->entries[i], w->entries[a], sizeof(w->entries[i]));
        }
      // The stock reference: DCS's loop with the switch around the ownship's items.
      ResetDcsState(*w);
      PassSetup(d);
      OwnSwitchB7(d, false);
      for (int i = 0; i < kN; ++i) {
        if (i == static_cast<int>(a)) OwnSwitchB7(d, true);
        DrawItem(d, *w, i, tt);
        if (i == static_cast<int>(b) - 1) OwnSwitchB7(d, false);
      }
      const std::vector<uint8_t> refOwn = ReadTargets(d);
      ResetDcsState(*w);
      PassSetup(d);
      for (int i = 0; i < kN; ++i) DrawItem(d, *w, i, tt);
      const std::vector<uint8_t> refNoSw = ReadTargets(d);
      size_t diffRef = 0;
      for (size_t k = 0; k < refNoSw.size() && k < refOwn.size(); ++k) diffRef += refNoSw[k] != refOwn[k];
      static int inMark = 0, outMark = 0;
      void* const in0 = g_ownInE;
      void* const out0 = g_ownOutE;
      g_ownInE = &inMark;
      g_ownOutE = &outMark;
      setup(0);
      j->splitMin = 16;
      j->ownKey = runTag;
      j->ownRec = true;
      ResetDcsState(*w);
      // RecordOnPool's StartJob reads g_ownKey: published for the run's instance like cockpit_ipd.h does.
      const void* pub0 = g_ownKey.load();
      g_ownKey = runTag;
      const bool recO = ownOk && RecordOnPool(pool, *j, cls, false);
      g_ownKey = pub0;
      const uint8_t ownSeg = recO ? j->items[a].seg : kNoSeg;
      ownOk = recO && ownSeg != kNoSeg && j->segs[ownSeg].first == a && j->segs[ownSeg].end == b &&
              j->ownRecItems == runN && j->ownRuns == 1;
      bool ssO = false;
      ResetDcsState(*w);
      const bool runO = ownOk && RunRecorded(d, *w, *j, cls, tt, false, &ssO);
      std::vector<uint8_t> gotO = ReadTargets(d);
      size_t diffO = refOwn.size() == gotO.size() ? 0 : SIZE_MAX;
      for (size_t k = 0; diffO != SIZE_MAX && k < refOwn.size(); ++k) diffO += refOwn[k] != gotO[k];
      // Control: the same lists with the switch entries ignored (the ownship at DCS's b7) differ from refOwn.
      g_ownInE = nullptr;
      g_ownOutE = nullptr;
      size_t diffC = 0;
      if (runO) {
        bool ssC = false;
        ResetDcsState(*w);
        // (the switch entries are now unknown objects to the harness: run the lists by hand, skipping them)
        PassSetup(d);
        for (uint32_t e = 1; e < j->swapCount; ++e) {
          void* q = j->swapList[e];
          if (q == &inMark || q == &outMark) continue;
          bool isExec = false;
          for (uint32_t k = 0; k < j->segCount; ++k)
            if (q == j->execObj[k]) {
              isExec = true;
              ID3D11Buffer* now[2][3] = {};
              d.imm->VSGetConstantBuffers(6, 3, now[0]);
              d.imm->PSGetConstantBuffers(6, 3, now[1]);
              for (int st = 0; st < 2; ++st)
                for (int qq = 0; qq < 2; ++qq)
                  if (j->ctxBuf[st][qq] && now[st][qq]) d.imm->CopyResource(j->ctxBuf[st][qq], now[st][qq]);
              for (auto& row : now)
                for (auto*& pp : row) Rel(pp);
              for (uint32_t pc2 = j->segs[k].pieceFirst; pc2 < j->segs[k].pieceEnd; ++pc2)
                d.imm->ExecuteCommandList(cls[pc2], TRUE);
            }
          if (isExec) continue;
          int i = 0;
          while (i < kN && w->rend[i] != q) ++i;
          if (i < kN) DrawItem(d, *w, i, tt);
        }
        (void)ssC;
        const std::vector<uint8_t> gotC = ReadTargets(d);
        for (size_t k = 0; k < refOwn.size() && k < gotC.size(); ++k) diffC += refOwn[k] != gotC[k];
      }
      g_ownInE = in0;
      g_ownOutE = out0;
      j->ownKey = nullptr;
      for (int i = 0; i < kN; ++i) {
        *reinterpret_cast<void**>(w->item[i] + 0x18) = tag0[i];
        memcpy(w->entries[i], ent0[i].data(), sizeof(w->entries[i]));
      }
      OwnSwitchB7(d, false);
      printf("     ownship segment %u (%u..%u) of %u: %zu bytes differ from stock with the switch, control %zu, "
             "switch effect on stock %zu\n",
             ownSeg, a, b, j->segCount, diffO, diffC, diffRef);
      Check(ownOk && runO && ssO && diffO == 0 && diffC > 0 && diffRef > 0,
            "gbuffer rec device, ownship stage 2: the locked instance's run recorded as a segment of its own reads the "
            "switched b7 at its exec entry: the pass equals stock with the switch bit for bit (6 RTs, depth, "
            "stencil); without the switch entries the lists differ");
      for (auto*& l : cls) Rel(l);
    }
    // A texture's view changes after the job read it (another pass swapped its
    // mip sets): the segments using it are re-recorded with the new view on the
    // workers, and the pass equals stock drawn with the new view.
    setup(0);
    j->splitMin = 16;
    ResetDcsState(*w);
    const bool recR = RecordOnPool(pool, *j, cls, false);
    w->SetView(0, d.texSrv[1]);
    TexTable live;  // DCS's draws read the live texture (the harness's stock otherwise picks from tt)
    ResetDcsState(*w);
    PassSetup(d);
    for (int i = 0; i < kN; ++i) DrawItem(d, *w, i, live);
    const std::vector<uint8_t> ref2 = ReadTargets(d);
    Stat rst;
    const bool planned = PlanSegsGuarded(*j, 0, j->segCount, false, rst);
    uint32_t want = 0, done = 0;
    for (uint32_t k = 0; k < j->segCount; ++k) {
      if (j->redoState[k].load() != kRedoWant) continue;
      ++want;
      j->redoArg[k] = {j, static_cast<uint8_t>(k), 0};
      j->redoState[k].store(kRedoRun);
      if (pool.Submit(0, &RedoMain, &j->redoArg[k], nullptr)) pool.Wait(60000);
      done += j->redoState[k].load() == kRedoOk && j->redoCl[k];
    }
    bool ssR = false;
    ResetDcsState(*w);
    bool runR = RunRecorded(d, *w, *j, cls, live, false, &ssR);
    std::vector<uint8_t> gotR = ReadTargets(d);
    size_t diffR = ref2.size() == gotR.size() ? 0 : SIZE_MAX;
    for (size_t k = 0; diffR != SIZE_MAX && k < ref2.size(); ++k) diffR += ref2[k] != gotR[k];
    printf("     view changed: %u of %u segments re-recorded (%llu planned), %zu bytes differ\n", done, j->segCount,
           static_cast<unsigned long long>(rst.redoPlanned.load()), diffR);
    // Control: the original lists (old view) differ from that stock pass.
    for (auto& st : j->redoState) st.store(kRedoNone);
    ResetDcsState(*w);
    bool ssO = false;
    RunRecorded(d, *w, *j, cls, live, false, &ssO);
    gotR = ReadTargets(d);
    size_t diffO = 0;
    for (size_t k = 0; k < ref2.size() && k < gotR.size(); ++k) diffO += ref2[k] != gotR[k];
    Check(recR && planned && want > 0 && done == want && runR && ssR && diffR == 0 && diffO > 0,
          "gbuffer rec device: segments whose texture view changed after the job are re-recorded with the new view, "
          "bit-exact to stock; the old lists differ");
    // Issue #3 replay (0.5.8 field fault: RecordRange <- RedoMain <- Pool::RunGuarded, data address -1): a
    // re-record passes the heap-generation check, DCS resets its frame heap, the renderable's item pointer is then
    // another allocation's bytes (non-canonical). Before the fix the pool latched the whole recorder off; now the
    // fault is contained per job like JobMain's and the helpers'.
    {
      uint32_t rk = 0, rc = UINT32_MAX;
      for (uint32_t k = 0; k < j->segCount && rc == UINT32_MAX; ++k)
        for (uint32_t c = j->segs[k].candFirst; c < j->segs[k].candEnd; ++c)
          if (j->cands[c].alive) {
            rk = k;
            rc = c;
            break;
          }
      if (rc == UINT32_MAX) {
        Check(false, "gbuffer rec redo race: a recorded candidate");
      } else {
        alignas(16) static uint8_t recycled[0x200];
        memset(recycled, 0xcd, sizeof(recycled));
        *reinterpret_cast<uint64_t*>(recycled + 0x10) = 0xdead00000000beefull;  // non-canonical, as in the log
        void* const keep = j->snap[j->cands[rc].item];
        j->snap[j->cands[rc].item] = recycled;
        for (auto& st : j->redoState) st.store(kRedoNone);
        j->heapGen = g_frameHeapGen.load();  // started this frame: the check before the read passes
        j->redoStop = 0;
        j->tStart = 0;                       // no scene change near it
        const uint64_t contained0 = g_redoFaultsContained.load();
        g_raceResetArmed = 1;  // the first access violation stands for "DCS reset the heap right after the check"
        PVOID veh = AddVectoredExceptionHandler(1, &RaceResetVeh);
        Rel(j->redoCl[rk]);  // the list of the view-change re-record above
        j->redoArg[rk] = {j, static_cast<uint8_t>(rk), 0};
        j->redoState[rk].store(kRedoRun);
        const bool sub = pool.Submit(0, &RedoMain, &j->redoArg[rk], nullptr);
        if (sub) pool.Wait(60000);
        RemoveVectoredExceptionHandler(veh);
        g_raceResetArmed = 0;
        const bool failed = j->redoState[rk].load() == kRedoFail && !j->redoCl[rk];
        const bool poolOn = !pool.Disabled() && pool.Status(0) != defrec::kFaulted;
        printf("     redo race: fault %s, segment %u %s, pool %s\n",
               g_redoFaultsContained.load() == contained0 + 1 ? "contained" : "NOT contained", rk,
               failed ? "drawn stock" : "?", poolOn ? "on" : "LATCHED OFF");
        Check(sub && failed && poolOn && g_redoFaultsContained.load() == contained0 + 1,
              "gbuffer rec redo race (issue #3): a re-record that reads a renderable recycled after its generation "
              "check is contained per job (segment stock, pool and recorder stay on)");
        j->snap[j->cands[rc].item] = keep;
        j->heapGen = 0;
        for (auto& st : j->redoState) st.store(kRedoNone);
        g_recycledFaults = 0;
        g_redoFaultsContained = 0;
      }
    }
    w->SetView(0, d.texSrv[0]);
    ReleaseSegLists(*j);
    ReleaseRedo(*j);
    FillTex(tt, *w);  // the table's entry rebuilt (the re-record marked it for that)
    for (auto*& l : cls) Rel(l);
    // Control: lists recorded without the pass's sampler pool differ (the PS reads s5).
    setup(0);
    for (auto& row : j->pool)
      for (auto*& p : row) Rel(p);
    ResetDcsState(*w);
    const bool recNp = RecordOnPool(pool, *j, cls, false);
    bool ssNp = false;
    const bool sameNp = recNp && runExact(&ssNp);
    Check(recNp && !sameNp, "gbuffer rec device: (control) lists without the sampler pool in their prologue differ");
    for (auto*& l : cls) Rel(l);
    // 2. Three workers record pieces at once (segments cut across workers).
    setup(0);
    ResetDcsState(*w);
    const bool rec3 = RecordOnPool(pool, *j, cls, true);
    bool roles[kThreads] = {}, cut = false;
    for (uint32_t q = 0; q < j->pieceCount; ++q) roles[j->pieces[q].role] = true;
    for (uint32_t k = 0; k < j->segCount; ++k) cut |= j->segs[k].pieceEnd - j->segs[k].pieceFirst > 1;
    printf("     3 workers: %u lists for %u segment(s)\n", j->pieceCount, j->segCount);
    bool ss3 = false;
    const bool same3 = rec3 && runExact(&ss3);
    Check(rec3 && roles[0] && roles[1] && roles[2] && cut && j->pieceCount == j->segCount + 2 && same3 && ss3,
          "gbuffer rec device: a segment split over 3 workers' lists, executed in order, stays bit-exact");
    for (auto*& l : cls) Rel(l);
    // 3. The verify stride: the segment limit of short segments, still exact.
    setup(5);
    j->splitMin = 16;
    ResetDcsState(*w);
    const bool rec2 = RecordOnPool(pool, *j, cls, true);
    bool ss2 = false;
    const bool same2 = rec2 && runExact(&ss2);
    Check(rec2 && j->segCount == kDefaultMaxSeg && same2 && ss2,
          "gbuffer rec device: 12 segments interleaved with forced residual draws (verify stride), split over 3 "
          "workers, stay bit-exact");
    for (auto*& l : cls) Rel(l);
  }
  pool.Stop();
  FreeJob(j);
  FreeTex(tt);
  FreeTables(t);
  Destroy(d);
  delete w;
}

// ---------------------------------------------------------------------------
// Key state variants: FX dependencies, leftover slots, retire and relearn
// ---------------------------------------------------------------------------
// A fake effect: technique 1 of `shader` has a pass whose PS block binds a
// CB at slot 2 and samplers at slots 0-1 (dx11backend's layout).
struct FakeFx {
  alignas(16) uint8_t shader[0x100] = {};
  alignas(16) uint8_t techs[0x50] = {};
  alignas(16) uint8_t techObj[0x20] = {};
  alignas(16) uint8_t pass[0x100] = {};
  alignas(16) uint8_t psBlock[0x80] = {};
  alignas(16) uint8_t vsBlock[0x80] = {};
  alignas(16) uint8_t cbDeps[0x20] = {};
  alignas(16) uint8_t sampDeps[0x20] = {};
  ID3D11SamplerState* sampObjs[2] = {};
  void* techVt[8] = {};
};
FakeFx* g_fx = nullptr;
uint8_t* __fastcall FakeGetPass(void*, uint32_t) { return g_fx->pass; }
void InitFx(FakeFx& x, void* ps, void* vs, ID3D11SamplerState* s0, ID3D11SamplerState* s1) {
  g_fx = &x;
  x.techVt[7] = reinterpret_cast<void*>(&FakeGetPass);
  *reinterpret_cast<void**>(x.techObj) = x.techVt;
  *reinterpret_cast<uint8_t**>(x.techs + 0x20) = x.techObj;
  *reinterpret_cast<uint8_t**>(x.shader + 0xb0) = x.techs;
  *reinterpret_cast<uint8_t**>(x.shader + 0xb8) = x.techs + 0x50;
  *reinterpret_cast<uint8_t**>(x.pass + 0xc0) = x.vsBlock;
  *reinterpret_cast<uint8_t**>(x.pass + 0xc8) = x.psBlock;
  *reinterpret_cast<void**>(x.vsBlock + 0x18) = vs;
  *reinterpret_cast<void**>(x.psBlock + 0x18) = ps;
  *reinterpret_cast<uint32_t*>(x.psBlock + 0x20) = 1;
  *reinterpret_cast<uint8_t**>(x.psBlock + 0x28) = x.cbDeps;
  *reinterpret_cast<uint32_t*>(x.psBlock + 0x30) = 1;
  *reinterpret_cast<uint8_t**>(x.psBlock + 0x38) = x.sampDeps;
  *reinterpret_cast<uint32_t*>(x.cbDeps) = 2;
  *reinterpret_cast<uint32_t*>(x.cbDeps + 4) = 1;
  x.sampObjs[0] = s0;
  x.sampObjs[1] = s1;
  *reinterpret_cast<uint32_t*>(x.sampDeps) = 0;
  *reinterpret_cast<uint32_t*>(x.sampDeps + 4) = 2;
  *reinterpret_cast<ID3D11SamplerState***>(x.sampDeps + 0x10) = x.sampObjs;
}

void KeyVariantTests() {
  auto P = [](uintptr_t v) { return reinterpret_cast<void*>(v); };
  auto S = [&](uintptr_t v) { return static_cast<ID3D11SamplerState*>(P(v)); };
  FakeFx* fx = new FakeFx;
  InitFx(*fx, P(0x11), P(0x10), S(0xa0), S(0xa1));
  ID3D11SamplerState* bound[16] = {S(0xa0), S(0xa1), S(0xb2), S(0xb3)};
  shrec::FxDeps d = shrec::FxStageDepsGuarded(fx->shader, 1, 1, P(0x11), bound);
  bool ok = d.ok && d.samp == 0x3 && d.cb == (1u << 2);
  ok = ok && !shrec::FxStageDepsGuarded(fx->shader, 1, 1, P(0x12), bound).ok;  // another PS bound
  bound[1] = S(0xc1);
  ok = ok && !shrec::FxStageDepsGuarded(fx->shader, 1, 1, P(0x11), bound).ok;  // a sampler not the one Apply set
  bound[1] = S(0xa1);
  ok = ok && !shrec::FxStageDepsGuarded(fx->shader, 2, 1, P(0x11), bound).ok;  // no technique 2
  Check(ok, "gbuffer rec: FX dependencies of the pass's PS (CB and sampler slots), checked against the bound objects");
  // Objects the keys hold references to: counted fakes.
  FakeView fo[10];
  auto Q = [&](uintptr_t v) -> void* {
    static const uintptr_t kIds[10] = {0x10, 0x11, 0xa0, 0xa1, 0xb5, 0xc5, 0xd1, 0xb7, 0xc7, 0xd0};
    for (int i = 0; i < 10; ++i)
      if (kIds[i] == v) return static_cast<void*>(&fo[i]);
    return nullptr;
  };
  auto SQ = [&](uintptr_t v) { return static_cast<ID3D11SamplerState*>(Q(v)); };
  // G-buffer key: a leftover sampler at a slot the PS does not read does not
  // matter; one it reads retires the key; relearnt after the cooldown; keys per collection.
  Tables t;
  AllocTables(t);
  shrec::Capture c;
  c.vs = static_cast<ID3D11VertexShader*>(Q(0x10));
  c.ps = static_cast<ID3D11PixelShader*>(Q(0x11));
  c.cb = static_cast<ID3D11Buffer*>(P(0x30));
  c.sb = static_cast<ID3D11ShaderResourceView*>(P(0x40));
  c.psCb[2] = c.cb;
  c.psSrv[0] = static_cast<ID3D11ShaderResourceView*>(P(0x50));
  c.psSamp[0] = SQ(0xa0);
  c.psSamp[1] = SQ(0xa1);
  c.psSamp[5] = SQ(0xb5);  // leftover
  c.draws = 1;
  Extra x;
  VsStatic st;
  st.cbSlot = 2;
  st.sbSlot = 3;
  ReadsEntry rd;
  rd.state = 1;
  rd.mask[0] = 1;
  rd.count = 1;
  Facts f;
  f.dcsVs = Q(0x10);
  f.pageSrv = c.sb;
  f.reads = &rd;
  f.readCount = 1;
  f.readH[0] = 0;
  f.readView[0] = P(0x50);
  f.psDepsOk = true;
  f.psSampDeps = 0x3;
  f.psCbDeps = 1u << 2;
  ProbeIn in;
  in.shader = fx->shader;
  in.tech = 1;
  in.mesh = static_cast<uint8_t*>(P(0x7000));
  in.scope = 4;
  const uint64_t down0 = g_keyDowngrades.load();
  ProcessProbe(t, in, st, f, c, x, 100);
  const GbKey* k = FindKey(t, in.shader, 1, 0, nullptr, nullptr, 4);
  bool v = k && k->state.load() > 0 && k->psSampDeps == 0x3 && !FindKey(t, in.shader, 1, 0, nullptr, nullptr, 5);
  c.psSamp[5] = SQ(0xc5);  // another leftover at slot 5 (unread)
  c.psCb[9] = c.cb;       // the material CB left at a slot the PS does not read
  ProcessProbe(t, in, st, f, c, x, 110);
  v = v && FindKey(t, in.shader, 1, 0, nullptr, nullptr, 4) == k && k->state.load() > 0 &&
      g_keyDowngrades.load() == down0;
  // Another collection learns its own key, whatever this one holds.
  in.scope = 5;
  c.psSamp[1] = SQ(0xd1);
  ProcessProbe(t, in, st, f, c, x, 111);
  const GbKey* k5 = FindKey(t, in.shader, 1, 0, nullptr, nullptr, 5);
  v = v && k5 && k5 != k && k5->psSamp[1] == SQ(0xd1) && k->state.load() > 0;
  // A sampler the PS reads differs in collection 4: retired, not dropped.
  in.scope = 4;
  ProcessProbe(t, in, st, f, c, x, 120);
  v = v && !FindKey(t, in.shader, 1, 0, nullptr, nullptr, 4) && k->state.load() == kRetired &&
      g_keyDowngrades.load() == down0 + 1 && KeyCooling(t, in.shader, 1, 0, nullptr, nullptr, 4, 121, false);
  ProcessProbe(t, in, st, f, c, x, 121);  // within the cooldown: nothing learnt
  v = v && !FindKey(t, in.shader, 1, 0, nullptr, nullptr, 4);
  const uint64_t re0 = g_keyRelearnt.load();
  ProcessProbe(t, in, st, f, c, x, 120 + kKeyCooldown + 1);
  const GbKey* kr = FindKey(t, in.shader, 1, 0, nullptr, nullptr, 4);
  v = v && kr && kr != k && kr->state.load() > 0 && kr->psSamp[1] == SQ(0xd1) && g_keyRelearnt.load() == re0 + 1;
  Check(v, "gbuffer rec: keys per collection; unread leftover slots ignored; a read slot's change retires the key, "
           "which is relearnt after the cooldown");
  FreeTables(t);
  // The shadow recorder: the same rules for its PS keys.
  shrec::Tables st2;
  shrec::AllocTables(st2);
  shrec::Capture sc;
  sc.vs = static_cast<ID3D11VertexShader*>(Q(0x10));
  sc.ps = static_cast<ID3D11PixelShader*>(Q(0x11));
  sc.b7 = static_cast<ID3D11Buffer*>(P(0x27));
  sc.cb = static_cast<ID3D11Buffer*>(P(0x30));
  sc.sb = static_cast<ID3D11ShaderResourceView*>(P(0x40));
  sc.psCb[2] = sc.cb;
  sc.psSrv[0] = static_cast<ID3D11ShaderResourceView*>(P(0x50));
  sc.psSamp[0] = SQ(0xa0);
  sc.psSamp[1] = SQ(0xa1);
  sc.psSamp[7] = SQ(0xb7);
  shrec::ProbeFacts sf;
  sf.dcsVs = Q(0x10);
  sf.passB7 = sc.b7;
  sf.pageSrv = sc.sb;
  sf.readCount = 1;
  sf.readH[0] = 0;
  sf.readView[0] = P(0x50);
  sf.depsOk = true;
  sf.psSampDeps = 0x3;
  sf.psCbDeps = 1u << 2;
  shrec::ProbeIn si;
  si.shader = fx->shader;
  si.tech = 1;
  si.mesh = static_cast<uint8_t*>(P(0x7100));
  shrec::KeyStatic kst;
  const uint64_t sd0 = shrec::g_keyDowngrades.load();
  shrec::ProcessProbe(st2, si, kst, sf, sc, 50);
  const shrec::KeyEntry* sk = shrec::FindKey(st2, si.shader, 1, 0, nullptr, nullptr);
  bool sv = sk && sk->state.load() > 0 && sk->psSampDeps == 0x3;
  sc.psSamp[7] = SQ(0xc7);  // a leftover
  shrec::ProcessProbe(st2, si, kst, sf, sc, 60);
  sv = sv && shrec::FindKey(st2, si.shader, 1, 0, nullptr, nullptr) == sk && shrec::g_keyDowngrades.load() == sd0;
  sc.psSamp[0] = SQ(0xd0);  // a read slot
  shrec::ProcessProbe(st2, si, kst, sf, sc, 70);
  sv = sv && !shrec::FindKey(st2, si.shader, 1, 0, nullptr, nullptr) && sk->state.load() == shrec::kRetired &&
       shrec::KeyCooling(st2, si.shader, 1, 0, nullptr, nullptr, 71, false);
  shrec::ProcessProbe(st2, si, kst, sf, sc, 70 + shrec::kKeyCooldown + 1);
  const shrec::KeyEntry* sr = shrec::FindKey(st2, si.shader, 1, 0, nullptr, nullptr);
  sv = sv && sr && sr != sk && sr->psSamp[0] == SQ(0xd0);
  Check(sv, "shadow rec: unread leftover PS samplers ignored; a read slot's change retires the key, relearnt after "
            "the cooldown");
  shrec::FreeTables(st2);
  delete fx;
}

// ---------------------------------------------------------------------------
// Sampler pool and the state dump
// ---------------------------------------------------------------------------
void PoolAndDumpTests() {
  auto P = [](uintptr_t v) { return reinterpret_cast<void*>(v); };
  void* a[kSampSlots] = {};
  void* b[kSampSlots] = {};
  for (UINT q = 0; q < kSampSlots; ++q) a[q] = b[q] = P(0x100 + q);
  bool ok = PoolSame(a, b);
  b[3] = P(0x999);  // below the pool: leftovers do not count
  ok = ok && PoolSame(a, b);
  b[kPoolFirst] = P(0x998);
  ok = ok && !PoolSame(a, b);
  b[kPoolFirst] = a[kPoolFirst];
  b[kSampSlots - 1] = nullptr;
  ok = ok && !PoolSame(a, b);
  ok = ok && kPoolMask == 0xffe0u && KeyOwnSamplers(0xffff) == 0x1f && KeyOwnSamplers(0x23) == 0x3;
  Check(ok, "gbuffer rec: sampler pool s5-s15 compared alone; keys bind only their own slots below it");
  // The dump line: D3D11's defaults equal fx_states.py's (blend_desc, dss_desc, rs_desc).
  const float f0[4] = {0, 0, 0, 0};
  const std::string def = StateJson("model\\def_material.fx:A=\"1\";", "normal_cf", 0, 5, nullptr, f0, 0xffffffffu,
                                    nullptr, 0, nullptr);
  std::string rt;
  for (int i = 0; i < 8; ++i) rt += i ? ",[0,2,1,1,2,1,1,15]" : "[0,2,1,1,2,1,1,15]";
  const std::string want =
      "{\"key\":\"model\\\\def_material.fx:A=\\\"1\\\";\",\"tech\":\"normal_cf\",\"pass\":0,\"flags\":5,"
      "\"blend\":{\"a2c\":0,\"ind\":0,\"rt\":[" + rt + "]},\"factor\":[0,0,0,0],\"mask\":4294967295,"
      "\"dss\":{\"de\":1,\"dwm\":1,\"df\":2,\"se\":0,\"srm\":255,\"swm\":255,\"ff\":[1,1,1,8],\"bf\":[1,1,1,8]},"
      "\"ref\":0,\"rs\":{\"fill\":3,\"cull\":3,\"fcc\":0,\"db\":0,\"dbc\":0,\"ssdb\":0,\"dce\":1,\"se\":0,\"ms\":0,"
      "\"aal\":0}}\n";
  bool dv = def == want;
  if (!dv) printf("     got  %s     want %s", def.c_str(), want.c_str());
  D3D11_BLEND_DESC bd = CD3D11_BLEND_DESC(CD3D11_DEFAULT());
  bd.AlphaToCoverageEnable = TRUE;
  bd.RenderTarget[0].BlendEnable = TRUE;
  bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[1].RenderTargetWriteMask = 7;
  D3D11_DEPTH_STENCIL_DESC dd = CD3D11_DEPTH_STENCIL_DESC(CD3D11_DEFAULT());
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
  dd.StencilEnable = TRUE;
  dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
  D3D11_RASTERIZER_DESC rd = CD3D11_RASTERIZER_DESC(CD3D11_DEFAULT());
  rd.FillMode = D3D11_FILL_WIREFRAME;
  rd.DepthBias = -5;
  rd.SlopeScaledDepthBias = 1.5f;
  rd.ScissorEnable = TRUE;
  const float f1[4] = {0.25f, 1, 0, 0.5f};
  const std::string s = StateJson("k", "lockon_shadows", 0, 3, &bd, f1, 0xf, &dd, 0x28, &rd);
  dv = dv && s.find("\"blend\":{\"a2c\":1,\"ind\":0,\"rt\":[[1,5,6,1,2,1,1,15],[0,2,1,1,2,1,1,7],") != std::string::npos &&
       s.find("\"factor\":[0.25,1,0,0.5],\"mask\":15,") != std::string::npos &&
       s.find("\"dss\":{\"de\":1,\"dwm\":0,\"df\":7,\"se\":1,\"srm\":255,\"swm\":255,\"ff\":[1,1,3,8],\"bf\":[1,1,1,8]},"
              "\"ref\":40,") != std::string::npos &&
       s.find("\"rs\":{\"fill\":2,\"cull\":3,\"fcc\":0,\"db\":-5,\"dbc\":0,\"ssdb\":1.5,\"dce\":1,\"se\":1,") !=
           std::string::npos &&
       s.back() == '\n' && std::count(s.begin(), s.end(), '\n') == 1;
  Check(dv, "gbuffer rec: state dump lines in fx_states.py's format (D3D11 defaults for null objects, escaped key)");
  // Drawn-stock reason lists (yaw profile): per frame, most frequent first, from index `first`.
  const char* const names[4] = {"executed", "late", "texture", "flags"};
  const uint64_t ra[4] = {0, 1, 0, 0}, rb[4] = {50, 21, 5, 20};
  const std::string rl = shrec::ReasonList(names, ra, rb, 4, 1, 10, 2);
  const std::string none = shrec::ReasonList(names, ra, ra, 4, 1, 10, 2);
  Check(rl == "late 2.00, flags 2.00, ..." && none == "none",
        "recorders: drawn-stock reasons per frame, most frequent first (ties by index), the rest elided");
  // Adaptive helpers: none for short jobs, one, then two; capped; all before the first measurement.
  Check(HelpersFor(-1, 2) == 2 && HelpersFor(400, 2) == 0 && HelpersFor(1500, 2) == 1 && HelpersFor(9000, 2) == 2 &&
            HelpersFor(9000, 1) == 1 && HelpersFor(9000, 0) == 0,
        "gbuffer rec: helpers per job follow the slot's measured work (capped by GBufferRecorderHelpers)");
}

// ---------------------------------------------------------------------------
// R73: the R61 hazard sequence on the G-buffer recorder's render-entry start
// ---------------------------------------------------------------------------
// R61 (shadow_rec.h): in the F10 map DCS resets the frame heap many times per
// frame without sorting the cascades' collections again; the job started at
// render entry on the last world frame's vector (generation current, no
// scene-change mark) and faulted in PreOne. gb_rec.h's OnRenderBody had the
// same start (StartJob(..., false)) without a check. Sequence: a mission
// restart outside the SceneChanging window, heap resets with no sort, the
// items' memory reused, the render entry, stage A with the helpers.
std::atomic<int> g_r73Faults{0};
struct R73Helper {
  Job* j;
  int t;
};
void R73HelperRun(R73Helper* h) {
  __try {
    PreChunks(*h->j, h->t);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_r73Faults.fetch_add(1);
    h->j->recycled.store(1);
  }
}
int R73Primary(Job* j) {  // the job's result, -1 faulted
  __try {
    return PreAndCommit(*j, defrec::Qpc());
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_r73Faults.fetch_add(1);
    return -1;
  }
}
bool R73Faults(const void* p) {  // reading p faults (what PreOne's [mat] / [item+0x10] did)
  __try {
    return *static_cast<const volatile uint64_t*>(p) == 0x1234 && false;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return true;
  }
}
// Stage A as JobMain runs it: BeginBuild, helpers (live threads) and the primary under SEH.
int R73StageA(Job* j, int helpers) {
  if (BeginBuild(*j) != kBuildOk) return j->result;
  ResetEvent(j->preGo);
  std::atomic<bool> go{false};
  R73Helper hs[kThreads];
  std::thread th[kThreads];
  for (int h = 1; h <= helpers && h < kThreads; ++h) {
    hs[h] = {j, h};
    th[h] = std::thread([&go, p = &hs[h]] {
      while (!go.load()) _mm_pause();
      R73HelperRun(p);
    });
  }
  go = true;
  const int r = R73Primary(j);
  for (int h = 1; h <= helpers && h < kThreads; ++h) th[h].join();
  return r;
}

void RestartF10Tests() {
  World* w = new World;
  w->Init();
  Tables t;
  AllocTables(t);
  TexTable tt;
  AllocTex(tt, 256);
  FakeView views[kTex], pages[kPages];
  for (int k = 0; k < kTex; ++k) w->SetView(k, &views[k]);
  for (int p = 0; p < kPages; ++p) w->SetPageSrv(p, &pages[p]);
  PubAll(t, *w);
  FillTex(tt, *w);
  // Saved globals.
  const bool sortPatched0 = shrec::g_sortPatched, on0 = g_on.load(), dis0 = g_disabled.load();
  shrec::SortObserverFn const obs0 = shrec::g_sortObserver2.load();
  void* const sortRg0 = g_sortRg.load();
  Job* const pub0 = g_jobPub[0].load();
  const int64_t scene0 = g_sceneChangeQpc.load();
  const uint64_t gen0 = g_graphSortGen.load();
  const uint64_t impl0 = g_implausible.load();
  Job* j = NewJob();
  bool ok = j != nullptr;
  // The G-buffer graph's collections as RenderGraph+0xa18 holds them (the slot's collection is 2 of 4).
  alignas(16) static uint8_t rg[0xa30] = {};
  alignas(16) static uint8_t arr[4 * 24] = {};
  *reinterpret_cast<uint8_t**>(rg + 0xa18) = arr;
  *reinterpret_cast<uint8_t**>(rg + 0xa20) = arr + sizeof(arr);
  void** out = reinterpret_cast<void**>(arr + 2 * 24);
  if (ok) {
    g_graphSortGen.store(0);
    j->armRg = rg;
    j->armIdx = 2;
    j->armVol = nullptr;
    j->armById = false;
    g_jobPub[0].store(j);
    g_sortRg.store(rg);
    shrec::g_sortPatched = true;
    shrec::g_sortObserver2 = &OnSorted;
    g_on = true;
    g_disabled = false;
    const double us = defrec::QpcToUs();
    // 1. Mission restart, 10 s ago (its containment window is over); the new mission's frames sort the vector.
    g_frameHeapGen.fetch_add(1);
    g_sceneChangeQpc.store(defrec::Qpc() - static_cast<int64_t>(10e6 / us));
    j->startState.store(kStartIdle);
    OnSorted(out);  // not armed: still noted
    ok = SortRecorded() && g_graphSortGen.load() == g_frameHeapGen.load() && EntryVectorFresh();
    // An armed job is still started by the sort (early start unchanged).
    j->startState.store(kStartArmed);
    OnSorted(out);
    ok = ok && j->startState.load() == kStartRun && j->vec == out && j->startedEarly;
    // Graph-wide (live R73: DCS refills some G-buffer collections without the sort in a parsed frame): another
    // collection's sort of the same graph counts.
    g_graphSortGen.store(0);
    ok = ok && !EntryVectorFresh();
    OnSorted(reinterpret_cast<void**>(arr + 1 * 24));
    ok = ok && EntryVectorFresh();
    // A vector outside the published graph's array (another graph's collection) is not noted.
    void* foreign[3] = {};
    g_graphSortGen.store(0);
    OnSorted(foreign);
    ok = ok && g_graphSortGen.load() == 0 && !EntryVectorFresh();
    OnSorted(out);
    ok = ok && EntryVectorFresh();
    // Normal frame order: resets, then the sort, then render entry: fresh (steady state: no skip).
    for (int f = 0; f < 3 && ok; ++f) {
      g_frameHeapGen.fetch_add(1);
      g_frameHeapGen.fetch_add(1);  // two resets per frame (R61 live: 2.00 resets/frame in flight)
      OnSorted(out);
      ok = EntryVectorFresh();
    }
    Check(ok, "R73 restart: the G-buffer sort observer notes every sort of the graph's collections (armed or not, any "
              "collection, not another graph's), still starts an armed job early, and a normal frame (resets, sorts, "
              "entry) passes the gate");
    // 2. F10-like: the frame heap reset, the vector not sorted again; the old items' memory reused.
    for (int r = 0; r < 3; ++r) g_frameHeapGen.fetch_add(1);
    int bad[3] = {-1, -1, -1};
    for (int i = 0, b = 0; i < kN && b < 3; ++i)
      if (w->kind[i] == 0 && i > 40 * b + 10) bad[b++] = i;
    ok = bad[2] >= 0;
    if (ok) {
      *reinterpret_cast<uint64_t*>(w->item[bad[0]] + 0x10) = 0xc600000005ull;      // R60's data address
      *reinterpret_cast<uint64_t*>(w->item[bad[1]] + 0x10) = 0xdead0000beef0000ull;  // non-canonical (R55)
      *reinterpret_cast<uint64_t*>(w->rend[bad[2]] + 0x10) = 0x3ull;                 // an item pointer of junk
    }
    const bool wouldFault = R73Faults(reinterpret_cast<void*>(0xc600000005ull)) &&
                            R73Faults(reinterpret_cast<void*>(0xdead0000beef0000ull)) &&
                            R73Faults(reinterpret_cast<void*>(0x13ull));
    // 3. Render entry: the armed job's collection was not sorted since the reset: not started.
    ok = ok && wouldFault && !EntryVectorFresh() && !EntryVectorFreshAt(true, 5, 8) && EntryVectorFreshAt(false, 5, 8);
    Check(ok, "R73 F10 map: after heap resets with no G-buffer sort, the collection is not started at render entry");
    // With no sort since the reset: an empty header names nothing (started as before, the pass stock), a non-empty
    // one is gated, an unreadable header counts as non-empty (gated).
    {
      void* one[1] = {};
      void* empty[3] = {one, one, one};
      void* none[3] = {};
      void* full[3] = {one, one + 1, one + 1};
      ok = VectorEmptyGuarded(empty) && VectorEmptyGuarded(none) && !VectorEmptyGuarded(full) &&
           !VectorEmptyGuarded(reinterpret_cast<void**>(0x10)) && !VectorEmptyGuarded(w->desc);
      Check(ok, "R73 render-entry gate: only a non-empty unsorted vector is skipped (empty and null headers start as "
                "before; an unreadable header is skipped)");
    }
    // 4. Started anyway (no sort hook: the old start path): stage A with two helpers on the recycled items.
    shrec::g_sortPatched = false;
    ok = EntryVectorFresh();  // without the hook: no record, the old behaviour
    InitJob(*j, *w, t, tt);
    j->heapGen = g_frameHeapGen.load();  // started now: the generation is current
    j->tStart = defrec::Qpc();
    j->recycled.store(0);
    const bool notContained = !HeapRecycled(*j) && !shrec::SceneChanging(j->tStart) &&
                              RecycledFilter(EXCEPTION_ACCESS_VIOLATION, *j) == EXCEPTION_CONTINUE_SEARCH;
    const int faults0 = g_r73Faults.load();
    const int res = R73StageA(j, kThreads - 1);
    ok = ok && notContained && g_r73Faults.load() == faults0 && res == kBuildRecycled && j->recycled.load() == 1 &&
         g_implausible.load() >= impl0 + 3;
    Check(ok, "R73 F10 map: G-buffer stage A on recycled items (unaligned / non-canonical material, junk item) "
              "drops the job without a fault (primary and helpers), where neither the heap nor the scene check "
              "contains one");
    // 5. Back in the world: sorted again, a healthy vector builds as before, no implausible item.
    w->Init();
    for (int k = 0; k < kTex; ++k) w->SetView(k, &views[k]);
    for (int p = 0; p < kPages; ++p) w->SetPageSrv(p, &pages[p]);
    shrec::g_sortPatched = true;
    OnSorted(out);
    InitJob(*j, *w, t, tt);
    j->heapGen = g_frameHeapGen.load();
    j->recycled.store(0);
    const uint64_t impl1 = g_implausible.load();
    const int res2 = R73StageA(j, kThreads - 1);
    ok = EntryVectorFresh() && res2 == kBuildOk && j->recycled.load() == 0 && g_implausible.load() == impl1 &&
         j->candCount > 0 && g_r73Faults.load() == faults0;
    Check(ok, "R73 back from F10: the sorted collection starts at render entry and builds as before (no "
              "implausible item in a healthy vector)");
  } else {
    Check(false, "R73: test job");
  }
  g_jobPub[0].store(pub0);
  g_sortRg.store(sortRg0);
  shrec::g_sortPatched = sortPatched0;
  shrec::g_sortObserver2 = obs0;
  g_on = on0;
  g_disabled = dis0;
  g_sceneChangeQpc.store(scene0);
  g_graphSortGen.store(gen0);
  if (j) {
    j->startState.store(kStartIdle);
    FreeJob(j);
  }
  FreeTex(tt);
  FreeTables(t);
  delete w;
}

void Run() {
  PureTests();
  TexTableTests();
  srtest::Compiler c;
  if (!c.Load()) {
    Check(false, "gbuffer rec: d3dcompiler_47 for the reflection tests");
  } else {
    grtest::AnalyseTests(c);
  }
  KeyCheckTests();
  KeyVariantTests();
  PoolAndDumpTests();
  JobTests();
  RestartF10Tests();
  if (c.compile) grtest::DeviceTests(c);
  printf("     sizeof(gbrec::Job) = %.1f MB\n", sizeof(Job) / 1048576.0);
}

}  // namespace grtest
