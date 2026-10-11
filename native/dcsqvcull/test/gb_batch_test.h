// Offline tests of gb_batch.h (R13 stage 2): build checks on the analysed
// binaries, the view predictor (fakes, and against the real getSRV machine
// code), the animated-argument key (against the real 0xeb60 and ModelDesc
// property code), the planner and the restores on a fake pass, the member
// path, and the G-buffer compare (WARP, single-sample and MSAA).
// Included from test_main.cpp after shadow_batch_test.h.
#pragma once

namespace gbbtest {

using namespace gbbatch;

// nullptr when DCS is not installed (dcs_install.h): the callers SKIP.
uint8_t* LoadBin(const wchar_t* name) { return reinterpret_cast<uint8_t*>(dcsinstall::LoadBinary(name)); }

// Called from main() before the shadow-tex test patches NGModel's image.
void BuildChecks() {
  uint8_t* gc = LoadBin(L"GraphicsCore.dll");
  uint8_t* ng = LoadBin(L"NGModel.dll");
  uint8_t* dx = LoadBin(L"dx11backend.dll");
  uint8_t* md = LoadBin(L"ModelDesc.dll");
  if (!gc || !ng || !dx || !md) {
    printf("SKIP gbuffer batch: DCS binaries not found\n");
    return;
  }
  const char* a = gbpass::VerifyBuild(gc);
  const char* b = gbbatch::VerifyBuild(ng, dx, md);
  Check(!a && !b,
        "gbuffer batch: build checks pass (G-buffer pass vtable, thunk, item vector and loop; slot 4, 0x15e20, 0xeb60, "
        "0xcf80, submit; slot 26/getSRV; vt[27]; SetResource; renderer getters; ModelDesc property classes)");
  if (a) printf("     %s\n", a);
  if (b) printf("     %s\n", b);
}

// ---------------------------------------------------------------------------
// View predictor
// ---------------------------------------------------------------------------
void* g_texVt[32];
void* g_innerVt[3][16];
void* g_otherVt[16];
alignas(16) uint8_t g_desc[0x40];
bool g_compatOk = true;
const uint8_t* __fastcall FGetDesc(void*) { return g_desc; }
bool __fastcall FCompat(int32_t, int32_t f) { return g_compatOk && f == 28; }

TexEnv FakeEnv() {
  TexEnv e;
  e.texVtbl = g_texVt;
  for (int i = 0; i < 3; ++i) e.inner[i] = g_innerVt[i];
  e.getDesc = &FGetDesc;
  e.compat = &FCompat;
  *reinterpret_cast<int32_t*>(g_desc + 0x20) = 28;
  return e;
}

struct TexFake {
  alignas(16) uint8_t tex[0x700] = {};
  alignas(16) uint8_t inner[0x20] = {};
  alignas(16) uint8_t s[0x60] = {};
  alignas(16) uint8_t p[0x60] = {};
  void* views[8] = {};
  int32_t thr[8] = {};
  alignas(16) uint8_t aux[24 * 4] = {};
  void Init(int innerClass = 0) {
    *reinterpret_cast<void**>(tex) = g_texVt;
    *reinterpret_cast<void**>(inner) = g_innerVt[innerClass];
    *reinterpret_cast<void**>(tex + 0x10) = inner;
    *reinterpret_cast<uint8_t**>(tex + 0x198) = s;
    *reinterpret_cast<uint8_t**>(tex + 0x1a0) = p;
    *reinterpret_cast<uint8_t**>(tex + 0x90) = aux;
    *reinterpret_cast<uintptr_t*>(tex + 0x80) = 0x80;
    for (int i = 0; i < 4; ++i) *reinterpret_cast<uintptr_t*>(aux + i * 24 + 0x10) = 0xa000 + i;
    for (uintptr_t i = 0; i < 8; ++i) views[i] = reinterpret_cast<void*>(0x1000 + i * 16);
    Views(0);
    Thr({});
  }
  static void Ready(uint8_t* set, int state, bool data) {
    *reinterpret_cast<int32_t*>(set + 0x40) = state;
    *reinterpret_cast<uintptr_t*>(set + 0x20) = data ? 1 : 0;
  }
  void Views(int n) {
    *reinterpret_cast<void***>(s + 8) = views;
    *reinterpret_cast<void***>(s + 0x10) = views + n;
  }
  void Thr(std::initializer_list<int32_t> t) {
    int n = 0;
    for (int32_t v : t) thr[n++] = v;
    *reinterpret_cast<int32_t**>(s + 0x28) = thr;
    *reinterpret_cast<int32_t**>(s + 0x30) = thr + n;
  }
};

uint64_t Size(int32_t x, int32_t y) { return static_cast<uint32_t>(x) | static_cast<uint64_t>(static_cast<uint32_t>(y)) << 32; }

void Predictor() {
  const TexEnv env = FakeEnv();
  auto* f = new TexFake;
  f->Init();
  void* v = reinterpret_cast<void*>(1);
  bool ok = true;
  auto pv = [&](int32_t type, int64_t aux, uint64_t size) {
    v = reinterpret_cast<void*>(1);
    return PredictView(env, f->tex, aux, type, size, &v);
  };
  // Classes.
  ok &= PredictView(env, nullptr, -1, 7, 0, &v) == kNullTex;
  *reinterpret_cast<void**>(f->tex) = g_otherVt;
  ok &= pv(7, -1, 0) == kTexClass;
  f->Init(2);
  *reinterpret_cast<void**>(f->inner) = g_otherVt;
  ok &= pv(7, -1, 0) == kTexClass;
  *reinterpret_cast<void**>(f->tex + 0x10) = nullptr;
  ok &= pv(7, -1, 0) == kTexClass;
  f->Init(2);
  TexFake::Ready(f->s, 3, false);
  TexFake::Ready(f->p, 3, false);
  *reinterpret_cast<uintptr_t*>(f->tex + 0x1b8) = 0x1b8;
  ok &= pv(7, -1, 0) == kOk && v == reinterpret_cast<void*>(0x1b8);  // Dummy inner class accepted
  const bool classes = ok;
  // compat false: null view, before any swap check.
  TexFake::Ready(f->p, 5, true);
  g_compatOk = false;
  ok &= pv(7, -1, 0) == kOk && v == nullptr;
  g_compatOk = true;
  ok &= pv(7, -1, 0) == kSwapPending;
  // aux and [tex+0x190] come first.
  ok &= pv(7, 2, 0) == kOk && v == reinterpret_cast<void*>(0xa002);
  *reinterpret_cast<uintptr_t*>(f->tex + 0x190) = 0x190;
  ok &= pv(7, -1, 0) == kOk && v == reinterpret_cast<void*>(0x190);
  *reinterpret_cast<uintptr_t*>(f->tex + 0x190) = 0;
  // Pending set ready -> swap, whether or not the current set is.
  TexFake::Ready(f->s, 5, true);
  ok &= pv(7, -1, 0) == kSwapPending;
  // State 5 without data is not ready.
  TexFake::Ready(f->p, 5, false);
  TexFake::Ready(f->s, 5, false);
  ok &= pv(7, -1, 0) == kOk && v == reinterpret_cast<void*>(0x1b8);
  *reinterpret_cast<uintptr_t*>(f->tex + 0x1b8) = 0;
  ok &= pv(7, -1, 0) == kOk && v == reinterpret_cast<void*>(0x80);
  const bool branches = ok;
  // Scan.
  TexFake::Ready(f->s, 5, true);
  f->Views(0);
  ok &= pv(7, -1, Size(10, 10)) == kOk && v == nullptr;
  f->Views(3);
  ok &= pv(7, -1, Size(10, 10)) == kOk && v == f->views[0];
  f->Thr({1000, 100, 10});
  ok &= pv(7, -1, Size(5, 10)) == kOk && v == f->views[2];
  ok &= pv(7, -1, Size(50, 40)) == kOk && v == f->views[0];
  ok &= pv(7, -1, Size(10, 15)) == kOk && v == f->views[1];
  ok &= pv(7, -1, Size(1, 5)) == kOk && v == f->views[0];  // none matches
  f->Thr({1000, 100, 10, 1});
  ok &= pv(7, -1, Size(1, 1)) == kMipSet;  // index 3 past 3 views
  f->Thr({0, -2000000000});
  ok &= pv(7, -1, Size(50000, 50000)) == kOk && v == f->views[1];  // int32 area wraps negative
  const bool scan = ok;
  // [tex+0x678]: types 0x20/0x21 scan with size 0, others solo.
  *reinterpret_cast<uintptr_t*>(f->tex + 0x678) = 1;
  f->Thr({100, 0});
  ok &= pv(7, -1, Size(50, 50)) == kTex678;
  ok &= pv(0x20, -1, Size(50, 50)) == kOk && v == f->views[1];
  ok &= pv(0x21, -1, Size(50, 50)) == kOk && v == f->views[1];
  *reinterpret_cast<uintptr_t*>(f->tex + 0x678) = 0;
  ok &= pv(0x20, -1, Size(50, 50)) == kOk && v == f->views[0];
  *reinterpret_cast<uint8_t**>(f->tex + 0x198) = nullptr;
  ok &= pv(7, -1, 0) == kMipSet;
  Check(classes, "gbuffer batch: predictor accepts only DX11Texture with File/Array/Dummy inner classes; null texture solo");
  Check(branches, "gbuffer batch: predictor branch order (compat, aux, +0x190, pending swap, not-ready fallback +0x1b8/+0x80)");
  Check(scan && ok, "gbuffer batch: predictor scan (empty, no thresholds, first match, none, past the views, int32 "
                    "area wrap) and the +0x678 path (0x20/0x21 with size 0, others solo)");
  delete f;
}

// Real getSRV (dx11backend 0x47c60) on random fake textures, against PredictSrv.
void GetSrvDifferential() {
  uint8_t* dx = LoadBin(L"dx11backend.dll");
  if (!dx || !shadowtex::CodeIs(dx, shadowtex::kGetSrv, shadowtex::kGetSrvEnd, shadowtex::kGetSrvHash)) {
    printf("SKIP gbuffer batch: dx11backend getSRV not available\n");
    return;
  }
  using RealFn = void*(__fastcall*)(void* tex, int64_t aux, const uint64_t* size);
  auto real = reinterpret_cast<RealFn>(dx + shadowtex::kGetSrv);
  auto* f = new TexFake;
  uint32_t seed = 12345, compared = 0, swaps = 0, solo = 0;
  auto rnd = [&]() {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;  // the low bits of this LCG have short periods
  };
  bool same = true;
  for (int it = 0; it < 4000; ++it) {
    *f = TexFake();
    f->Init();
    const int64_t aux = rnd() % 5 == 0 ? static_cast<int64_t>(rnd() % 4) : -1;
    if (rnd() % 6 == 0) *reinterpret_cast<uintptr_t*>(f->tex + 0x190) = 0x190;
    if (rnd() % 2) *reinterpret_cast<uintptr_t*>(f->tex + 0x1b8) = 0x1b8;
    TexFake::Ready(f->s, rnd() % 4 ? 5 : 2, rnd() % 5 != 0);
    TexFake::Ready(f->p, rnd() % 8 == 0 ? 5 : 1, rnd() % 2 != 0);
    const int nv = static_cast<int>(rnd() % 5);
    f->Views(nv);
    int32_t t[5];
    const int nt = static_cast<int>(rnd() % 6);
    for (int i = 0; i < nt; ++i) t[i] = static_cast<int32_t>(rnd() % 4000) - 500;
    int n = 0;
    for (int i = 0; i < nt; ++i) f->thr[n++] = t[i];
    *reinterpret_cast<int32_t**>(f->s + 0x28) = f->thr;
    *reinterpret_cast<int32_t**>(f->s + 0x30) = f->thr + n;
    const uint64_t size = Size(static_cast<int32_t>(rnd() % 80), static_cast<int32_t>(rnd() % 80));
    void* p = nullptr;
    const uint8_t why = PredictSrv(f->tex, aux, size, &p);
    if (why == kSwapPending) {
      ++swaps;
      continue;  // the real code would swap (0x49ca0)
    }
    if (why) {
      ++solo;
      continue;
    }
    void* r = real(f->tex, aux, &size);
    same &= r == p;
    ++compared;
  }
  printf("     getSRV differential: %u predicted cases equal the real code, %u swaps and %u out-of-shape cases skipped\n",
         compared, swaps, solo);
  Check(same && compared > 2000 && swaps > 100,
        "gbuffer batch: PredictSrv returns exactly what the real getSRV machine code returns; pending swaps are solo");
  delete f;
}

// ---------------------------------------------------------------------------
// Animated arguments: real 0xeb60 + ModelDesc property code
// ---------------------------------------------------------------------------
struct AnimFake {
  alignas(16) uint8_t mat[0x300] = {};
  alignas(16) uint8_t item[0x100] = {};
  alignas(16) uint8_t props[3][0x40] = {};
  alignas(16) uint8_t vec[3 * 16] = {};
  float keys[4] = {0.0f, 10.0f, 1.0f, 20.0f};  // (t, v) pairs
  float args[4] = {};
  void Init(uint8_t* md) {
    // 0: ArgumentProperty -> mat+0x100 = args[2]; 1: AnimatedProperty<float>
    // (2 keys) -> mat+0x104 = lerp(args[0]); 2: AnimatedProperty<float>
    // without keys -> mat+0x108 left as it was.
    *reinterpret_cast<void**>(props[0]) = md + kProps[4].vtbl;
    *reinterpret_cast<uint32_t*>(props[0] + 0x18) = 2;
    for (int i = 1; i < 3; ++i) {
      *reinterpret_cast<void**>(props[i]) = md + kProps[0].vtbl;
      *reinterpret_cast<uint32_t*>(props[i] + 0x18) = i == 1 ? 0 : 1;
      *reinterpret_cast<float**>(props[i] + 0x28) = keys;
      *reinterpret_cast<uint32_t*>(props[i] + 0x30) = i == 1 ? 2 : 0;
    }
    const uint32_t dst[3] = {0x100, 0x104, 0x108};
    for (int i = 0; i < 3; ++i) {
      *reinterpret_cast<uint8_t**>(vec + i * 16) = mat + dst[i];
      *reinterpret_cast<uint8_t**>(vec + i * 16 + 8) = props[i];
    }
    *reinterpret_cast<uint8_t**>(mat + 8) = vec;
    *reinterpret_cast<uint8_t**>(mat + 0x10) = vec + sizeof(vec);
    *reinterpret_cast<float**>(item) = args;
    *reinterpret_cast<uint64_t*>(item + 8) = 3;
    *reinterpret_cast<uint32_t*>(mat + 0x108) = 0x5e5e5e5e;
  }
};

void Animated() {
  uint8_t* ng = LoadBin(L"NGModel.dll");
  uint8_t* md = LoadBin(L"ModelDesc.dll");
  if (!ng || !md || !shadowtex::CodeIs(ng, 0xeb60, 0xebb3, 0x913bcabc38ea3e7cull)) {
    printf("SKIP gbuffer batch: NGModel / ModelDesc not available\n");
    return;
  }
  AnimEnv env;
  for (int c = 0; c < kPropClasses; ++c) {
    env.vtbl[c] = md + kProps[c].vtbl;
    env.bytes[c] = kProps[c].bytes;
  }
  auto eval = reinterpret_cast<EvalAnimFn>(ng + kEvalAnim);
  auto* a = new AnimFake;
  auto* b = new AnimFake;
  a->Init(md);
  b->Init(md);
  const float va[3] = {0.25f, 7.0f, 3.5f};
  memcpy(a->args, va, sizeof(va));
  memcpy(b->args, va, sizeof(va));
  uint32_t ka[kMaxAnim], kb[kMaxAnim], na = 0, nb = 0;
  bool ok = AnimInputs(env, a->mat, a->item, ka, kMaxAnim, &na) == kOk &&
            AnimInputs(env, b->mat, b->item, kb, kMaxAnim, &nb) == kOk && na == 3 && nb == 3 &&
            memcmp(ka, kb, sizeof(uint32_t) * 3) == 0;
  uint32_t bits = 0;
  memcpy(&bits, &va[2], 4);
  ok &= ka[0] == bits;
  alignas(16) uint8_t span[16];
  memcpy(span, a->item, 16);
  eval(a->mat, span);
  memcpy(span, b->item, 16);
  eval(b->mat, span);
  float out[2];
  memcpy(out, a->mat + 0x100, 8);
  ok &= out[0] == 3.5f && out[1] == 12.5f && *reinterpret_cast<uint32_t*>(a->mat + 0x108) == 0x5e5e5e5e &&
        memcmp(a->mat + 0x90, b->mat + 0x90, 0x130) == 0;
  // Another argument value: another key, another output.
  b->args[0] = 0.75f;
  AnimInputs(env, b->mat, b->item, kb, kMaxAnim, &nb);
  memcpy(span, b->item, 16);
  eval(b->mat, span);
  ok &= memcmp(ka, kb, sizeof(uint32_t) * 3) != 0 && memcmp(a->mat + 0x90, b->mat + 0x90, 0x130) != 0;
  Check(ok, "gbuffer batch: equal argument values give equal CB bytes from the real 0xeb60 + ModelDesc property code "
            "(and the key differs when they differ; a keyless property writes nothing)");
  // Rejections.
  bool rej = true;
  *reinterpret_cast<uint64_t*>(a->item + 8) = 2;  // index 2 out of range
  rej &= AnimInputs(env, a->mat, a->item, ka, kMaxAnim, &na) == kAnimRange;
  *reinterpret_cast<uint64_t*>(a->item + 8) = 3;
  *reinterpret_cast<uint8_t**>(a->vec) = a->mat + 0x18c;  // over posStructOffset
  rej &= AnimInputs(env, a->mat, a->item, ka, kMaxAnim, &na) == kAnimRange;
  *reinterpret_cast<uint8_t**>(a->vec) = a->mat + 0x50;  // outside the CB
  rej &= AnimInputs(env, a->mat, a->item, ka, kMaxAnim, &na) == kAnimRange;
  *reinterpret_cast<uint8_t**>(a->vec) = a->mat + 0x100;
  *reinterpret_cast<void**>(a->props[0]) = g_otherVt;
  rej &= AnimInputs(env, a->mat, a->item, ka, kMaxAnim, &na) == kAnimClass;
  Check(rej, "gbuffer batch: animated key rejects an index past the arguments, a destination over posStructOffset or "
             "outside the CB, and an unknown property class");
  delete a;
  delete b;
}

// ---------------------------------------------------------------------------
// Planner, restores, member path on a fake pass
// ---------------------------------------------------------------------------
void* g_srVt[4];
void* g_otherRVt[4];
void* g_matVt[8];
void* g_shVt[40];
void* g_varVt[40];
std::vector<std::pair<void*, void*>> g_setRes;  // (var, value)
long __fastcall FSetRes(void* var, void* v) {
  g_setRes.push_back({var, v});
  **reinterpret_cast<void***>(static_cast<uint8_t*>(var) + 8) = v;
  return 0;
}
std::vector<std::pair<void*, uint64_t>> g_tex23;
uint64_t __fastcall FTex23(void* tex, uint64_t size) {
  g_tex23.push_back({tex, size});
  return 0;
}
void* g_rendVt[64];
void* g_fakeFb = reinterpret_cast<void*>(0xfb);
void* __fastcall FFb(void*) { return g_fakeFb; }
int32_t* __fastcall FVp(void*, int32_t* out) {
  out[0] = 640;
  out[1] = 480;
  return out;
}

struct World {
  static constexpr int kShaders = 3, kMats = 4, kObjs = 16, kTex = 8, kItems = 32;
  std::vector<uint8_t> ng = std::vector<uint8_t>(0x80000), dx = std::vector<uint8_t>(0xc0000);
  alignas(16) uint8_t globals[0x300] = {};
  alignas(16) uint8_t pages[0x20 + 4 * 0x30] = {};
  alignas(16) uint8_t bufs[4][0x40] = {};
  alignas(16) uint8_t api[0x3000] = {};
  alignas(16) uint8_t sh[kShaders][0x100] = {};
  alignas(16) uint8_t recs[kShaders][0x50 * 4] = {};
  alignas(16) uint8_t vars[kShaders][4][0x20] = {};
  void* varData[kShaders][4] = {};
  alignas(16) uint8_t mat[kMats][0x300] = {};
  alignas(16) uint8_t props[kMats][0x300] = {};
  alignas(16) uint8_t objVec[kObjs][16] = {};
  alignas(16) uint8_t entries[kObjs][4 * 0x18] = {};
  TexFake tex[kTex];
  alignas(16) uint8_t item[kItems][0x100] = {};
  alignas(16) uint8_t r[kItems][0x80] = {};
  float args[kItems][2] = {};
  alignas(16) uint8_t animVec[kMats][16] = {};
  void* rv[kItems] = {};
  int n = 0;
  uint8_t* md = nullptr;
  void* vs = reinterpret_cast<void*>(0x5151);

  void Init(uint8_t* modelDesc) {
    md = modelDesc;
    g_ng = ng.data();
    g_dx = dx.data();
    *reinterpret_cast<uint8_t**>(g_ng + kGlobals) = globals;
    *reinterpret_cast<uint8_t**>(globals + 0x80) = pages;
    for (int i = 0; i < 4; ++i) {
      *reinterpret_cast<uint8_t**>(pages + 0x20 + i * 0x30) = bufs[i];
      *reinterpret_cast<uintptr_t*>(bufs[i] + 0x30) = 0x5b00 + i;
    }
    *reinterpret_cast<uint8_t**>(g_dx + kApiGlobal) = api;
    g_srVtbl = g_srVt;
    g_modelMatVtbl = g_matVt;
    g_shaderVtbl = g_shVt;
    g_setResource = reinterpret_cast<void*>(&FSetRes);
    g_varVt[31] = reinterpret_cast<void*>(&FSetRes);
    g_texVt[23] = reinterpret_cast<void*>(&FTex23);
    g_texEnv = FakeEnv();
    for (int c = 0; c < kPropClasses; ++c) {
      g_animEnv.vtbl[c] = md + kProps[c].vtbl;
      g_animEnv.bytes[c] = kProps[c].bytes;
    }
    for (int s = 0; s < kShaders; ++s) {
      *reinterpret_cast<void**>(sh[s]) = g_shVt;
      *reinterpret_cast<uintptr_t*>(sh[s] + 0x50) = 0xeff0 + s;
      *reinterpret_cast<uintptr_t*>(sh[s] + 0xb0) = 0x7ec0 + s;
      *reinterpret_cast<uint8_t**>(sh[s] + 0xc8) = recs[s];
      for (int h = 0; h < 4; ++h) {
        *reinterpret_cast<int32_t*>(recs[s] + h * 0x50 + 0xc) = 7;
        *reinterpret_cast<uint8_t**>(recs[s] + h * 0x50 + 0x40) = vars[s][h];
        *reinterpret_cast<void**>(vars[s][h]) = g_varVt;
        *reinterpret_cast<void***>(vars[s][h] + 8) = &varData[s][h];
      }
    }
    // Materials: m0, m2 on opaque mapped shaders s0, s2; m1 on s1 (blend mode not none); m3 on s0.
    const int shOf[kMats] = {0, 1, 2, 0};
    for (int m = 0; m < kMats; ++m) {
      *reinterpret_cast<void**>(mat[m]) = g_matVt;
      *reinterpret_cast<uint8_t**>(mat[m] + 0x28) = props[m];
      *reinterpret_cast<uint8_t**>(mat[m] + 0x30) = sh[shOf[m]];
      *reinterpret_cast<int64_t*>(mat[m] + 0x68) = 2;  // sbPositions handle
      *reinterpret_cast<uint64_t*>(mat[m] + 0x1d8) = 1;
      *reinterpret_cast<int64_t*>(mat[m] + 0x240) = 0;
      *reinterpret_cast<int64_t*>(mat[m] + 0x248) = 1;
      *reinterpret_cast<uint32_t*>(mat[m] + 0x2d8) = 2;
      *reinterpret_cast<uint32_t*>(props[m] + 0x26c) = m == 2 ? 2 : 0;  // each material's entry range of an object
      // One ArgumentProperty: mat+0x100 = args[0].
      *reinterpret_cast<uint8_t**>(animVec[m]) = mat[m] + 0x100;
      *reinterpret_cast<uint8_t**>(animVec[m] + 8) = props[m] + 0x200;  // the property object lives in props
      *reinterpret_cast<void**>(props[m] + 0x200) = md + kProps[4].vtbl;
      *reinterpret_cast<uint32_t*>(props[m] + 0x218) = 0;
      *reinterpret_cast<uint8_t**>(mat[m] + 8) = animVec[m];
      *reinterpret_cast<uint8_t**>(mat[m] + 0x10) = animVec[m] + 16;
    }
    shadowinst::g_map = new shadowinst::MapEntry[shadowinst::kMapSize];
    const uint8_t all = shadowinst::kFlagGate | shadowinst::kFlagPrevUnused | shadowinst::kFlagPsoVsOnly |
                        shadowinst::kFlagBlendNone | shadowinst::kFlagDeferredP0;
    Publish(sh[0], all);
    Publish(sh[1], all & ~shadowinst::kFlagBlendNone);
    Publish(sh[2], all);
    for (int t = 0; t < kTex; ++t) {
      tex[t].Init();
      TexFake::Ready(tex[t].s, 3, false);
      TexFake::Ready(tex[t].p, 3, false);
      *reinterpret_cast<uintptr_t*>(tex[t].tex + 0x1b8) = 0x7000 + t;  // view = 0x7000 + t
    }
    *reinterpret_cast<uintptr_t*>(tex[7].tex + 0x1b8) = 0x7000;  // texture 7: another object, texture 0's view
  }
  void Publish(uint8_t* shader, uint8_t flags) {
    using namespace shadowinst;
    size_t i = MapHash(shader, 1, 0);
    while (g_map[i].shader.load()) i = (i + 1) & (kMapSize - 1);
    MapEntry& e = g_map[i];
    e.tech = 1;
    e.pass = 0;
    e.effect = *reinterpret_cast<void**>(shader + 0x50);
    e.techBegin = *reinterpret_cast<void**>(shader + 0xb0);
    e.flags = flags;
    e.vs.store(static_cast<ID3D11VertexShader*>(vs));
    e.shader.store(shader);
  }
  void Reset() { n = 0; }
  // One model draw: material, object, its two texture indices (-1 = none), argument value.
  int Add(int m, int obj, int t0, int t1, float arg, uint32_t pass = 1, uint32_t page = 1) {
    const int i = n++;
    uint8_t* it = item[i];
    memset(it, 0, 0x100);
    memset(r[i], 0, 0x80);
    *reinterpret_cast<void**>(r[i]) = g_srVt;
    *reinterpret_cast<uint8_t**>(r[i] + 0x10) = it;
    *reinterpret_cast<uint32_t*>(r[i] + 0x60) = pass;
    *reinterpret_cast<uint64_t*>(r[i] + 0x6c) = Size(100 + i, 50);
    *reinterpret_cast<int64_t*>(r[i] + 0x78) = 10 + i;
    args[i][0] = arg;
    *reinterpret_cast<float**>(it) = args[i];
    *reinterpret_cast<uint64_t*>(it + 8) = 1;
    *reinterpret_cast<uint8_t**>(it + 0x10) = mat[m];
    *reinterpret_cast<uint8_t**>(it + 0x18) = objVec[obj];
    *reinterpret_cast<uint8_t**>(objVec[obj]) = entries[obj];
    uint8_t* e = entries[obj] + *reinterpret_cast<uint32_t*>(props[m] + 0x26c) * 0x18;
    *reinterpret_cast<uint8_t**>(e + 8) = t0 < 0 ? nullptr : tex[t0].tex;
    *reinterpret_cast<int64_t*>(e) = -1;
    *reinterpret_cast<uint8_t**>(e + 0x18 + 8) = t1 < 0 ? nullptr : tex[t1].tex;
    *reinterpret_cast<int64_t*>(e + 0x18) = -1;
    for (int k = 0; k < 16; ++k) it[0x60 + k * 4] = static_cast<uint8_t>(i * 16 + k);
    *reinterpret_cast<uintptr_t*>(it + 0xc0) = 0x3e50;  // mesh
    *reinterpret_cast<uintptr_t*>(it + 0xc8) = 0x3e58;
    *reinterpret_cast<uint32_t*>(it + 0xd0) = page;
    *reinterpret_cast<uint32_t*>(it + 0xd4) = 1000 + i;
    rv[i] = r[i];
    return i;
  }
  int AddForeign() {
    const int i = n++;
    memset(r[i], 0, 0x80);
    *reinterpret_cast<void**>(r[i]) = g_otherRVt;
    rv[i] = r[i];
    return i;
  }
  uint32_t Plan() { return PlanCore(rv, static_cast<size_t>(n)); }
};

std::string Roles(const World& w) {
  std::string s;
  for (int i = 0; i < w.n; ++i) {
    const Rec& rc = g_rec[i];
    s += rc.role == kLeader ? 'L' : rc.role == kMember ? 'M' : '.';
  }
  return s;
}

void Planner() {
  uint8_t* md = LoadBin(L"ModelDesc.dll");
  uint8_t* ng = LoadBin(L"NGModel.dll");
  if (!md || !ng) {
    printf("SKIP gbuffer batch: ModelDesc / NGModel not available\n");
    return;
  }
  // Saved globals (restored at the end).
  uint8_t* const ng0 = g_ng;
  uint8_t* const dx0 = g_dx;
  shadowinst::MapEntry* const map0 = shadowinst::g_map;
  auto* w = new World;
  w->Init(md);

  // 1. Same key joins; another view or another argument value does not.
  w->Reset();
  w->Add(0, 0, 0, 1, 1.0f);
  w->Add(0, 1, 0, 1, 1.0f);
  w->Add(0, 2, 7, 1, 1.0f);  // texture 7: other object, same view
  w->Add(0, 3, 2, 1, 1.0f);  // other view
  w->Add(0, 4, 0, 1, 2.0f);  // other argument value
  uint32_t cur = w->Plan();
  bool basic = cur == 3 && Roles(*w) == "LMM.." && g_offsets[0] == 1000 && g_offsets[1] == 1001 &&
               g_offsets[2] == 1002 && g_rec[3].reason == kAlone && g_rec[4].reason == kAlone &&
               g_grp[g_rec[1].group].base == 0 && g_grp[g_rec[1].group].count == 3;
  Check(basic, "gbuffer batch: planner groups equal keys (predicted views, not texture objects) in original order; "
               "another view or argument value stays alone");
  if (!basic) printf("     roles %s, cursor %u\n", Roles(*w).c_str(), cur);

  // 2. Barriers: a foreign item or a blend-mode draw splits; an opaque mapped draw does not.
  const uint64_t sb0 = g_splitBarrier.load();
  w->Reset();
  w->Add(0, 0, 0, 1, 1.0f);
  w->AddForeign();
  w->Add(0, 1, 0, 1, 1.0f);
  w->Add(1, 5, 0, 1, 1.0f);  // s1: BLEND_MODE != none
  w->Add(0, 2, 0, 1, 1.0f);
  w->Add(2, 6, 3, 4, 1.0f);  // opaque, another key
  w->Add(0, 3, 0, 1, 1.0f);
  w->Plan();
  bool barrierOk = Roles(*w) == "....L.M" && g_rec[1].reason == kForeign && g_rec[3].reason == kBlend &&
                   g_rec[0].reason == kAlone && g_splitBarrier.load() - sb0 == 2;
  Check(barrierOk, "gbuffer batch: a foreign renderable or a blended draw is a barrier; an opaque mapped draw is not");
  if (!barrierOk) printf("     roles %s\n", Roles(*w).c_str());

  // 3. Same-object rule.
  const uint64_t so0 = g_splitObject.load();
  w->Reset();
  w->Add(0, 0, 0, 1, 1.0f);  // L
  w->Add(2, 1, 3, 4, 1.0f);  // Y: object 1, other key
  w->Add(0, 1, 0, 1, 1.0f);  // M: object 1 -> may not jump Y
  w->Plan();
  bool obj = Roles(*w) == "..." && g_splitObject.load() - so0 == 1;
  w->Reset();
  w->Add(2, 5, 3, 4, 1.0f);  // L2 (key k2)
  w->Add(0, 6, 0, 1, 1.0f);  // L1 (key k1)
  w->Add(0, 7, 0, 1, 1.0f);  // M joins L1
  w->Add(2, 7, 3, 4, 1.0f);  // X: key k2, object 7: would jump M
  w->Plan();
  obj &= Roles(*w) == ".LM." && g_splitObject.load() - so0 == 2;
  w->Reset();
  w->Add(0, 0, 0, 1, 1.0f);
  w->Add(0, 1, 0, 1, 1.0f);
  w->Add(0, 1, 0, 1, 1.0f);  // same object as the previous member: same group, order kept
  w->Plan();
  obj &= Roles(*w) == "LMM";
  Check(obj, "gbuffer batch: same-object rule ([item+0x18]): no member jumps a draw of its object in another group, "
             "in either direction; draws of one object in one group are fine");

  // 4. Solo reasons: swap pending, null texture (and the rest of its shader), pass, transparent, cockpit.
  w->Reset();
  TexFake::Ready(w->tex[5].p, 5, true);  // texture 5: swap pending
  w->Add(2, 0, 3, 4, 1.0f);
  w->Add(2, 1, 5, 4, 1.0f);
  w->Add(2, 2, 3, 4, 1.0f);
  w->Plan();
  bool solo = Roles(*w) == "L.M" && g_rec[1].reason == kSwapPending;
  TexFake::Ready(w->tex[5].p, 3, false);
  w->Reset();
  w->Add(0, 0, 0, 1, 1.0f);
  w->Add(0, 1, 0, -1, 1.0f);  // null texture in shader s0
  w->Add(3, 2, 0, 1, 1.0f);   // m3 also on s0
  w->Add(0, 3, 0, 1, 1.0f);
  w->Add(2, 4, 3, 4, 1.0f);
  w->Add(2, 5, 3, 4, 1.0f);
  w->Plan();
  solo &= Roles(*w) == "....LM" && g_rec[1].reason == kNullTex && g_rec[0].reason == kNullBind &&
          g_rec[2].reason == kNullBind && g_rec[3].reason == kNullBind;
  w->Reset();
  w->Add(2, 0, 3, 4, 1.0f, 2);  // pass 2
  w->Add(2, 1, 3, 4, 1.0f);
  w->Add(2, 2, 3, 4, 1.0f);
  w->r[2][0x64] = 1;  // cockpit
  w->Add(2, 3, 3, 4, 1.0f);
  w->Plan();
  solo &= g_rec[0].reason == kPassNum && g_rec[2].reason == kCockpit && Roles(*w) == "....";
  Check(solo, "gbuffer batch: solo reasons: pending swap, null texture (and every draw of that shader in the pass), "
              "pass number and cockpit as barriers");

  // 5. Restores after a batched pass: the last draw of m0 is a skipped member.
  w->Reset();
  w->Add(2, 5, 3, 4, 1.0f);  // solo-ish other shader first
  w->Add(0, 0, 0, 1, 1.0f);  // L
  w->Add(0, 1, 0, 1, 1.0f);  // M
  w->Add(0, 2, 7, 1, 1.0f);  // M, last of m0 and of s0's variables
  w->Plan();
  g_evalAnim = reinterpret_cast<EvalAnimFn>(ng + kEvalAnim);
  memset(w->mat[0] + 0x90, 0xcd, 0x130);  // the leader's leftovers
  for (auto& vd : w->varData)
    for (void*& p : vd) p = reinterpret_cast<void*>(0xdead);
  g_setRes.clear();
  g_skippedPass = 2;
  RestoreGuarded();
  float a0 = 0;
  memcpy(&a0, w->mat[0] + 0x100, 4);
  bool rest = Roles(*w) == ".LMM" && *reinterpret_cast<uint32_t*>(w->mat[0] + 0x18c) == 1003 &&
              memcmp(w->mat[0] + 0xb0, w->item[3] + 0x60, 0x40) == 0 && a0 == 1.0f &&
              *reinterpret_cast<uint32_t*>(w->mat[2] + 0x18c) == 1000 && g_setRes.size() == 3 &&
              w->varData[0][0] == reinterpret_cast<void*>(0x7000) && w->varData[0][1] == reinterpret_cast<void*>(0x7001) &&
              w->varData[0][2] == reinterpret_cast<void*>(0x5b01) && w->varData[2][0] == reinterpret_cast<void*>(0xdead);
  // A later executed draw of the same shader leaves nothing to restore.
  w->Add(3, 3, 2, 1, 5.0f);
  w->Plan();
  g_setRes.clear();
  g_skippedPass = 2;
  RestoreGuarded();
  rest &= Roles(*w) == ".LMM." && g_setRes.empty();
  Check(rest, "gbuffer batch: after the pass each material gets its last draw's offset, matrix and animated values "
              "(real 0xeb60), and each variable whose last writer was skipped gets that draw's view / sbPositions");

  // 6. Member path through the override: triangle counter and streaming requests with the member's size.
  w->Reset();
  w->Add(0, 0, 0, 1, 1.0f);
  w->Add(0, 1, 7, 1, 1.0f);
  w->Plan();
  void* api = nullptr;
  void** const rApi0 = reinterpret_cast<void**>(shadowbatch::g_rendererApi);
  void** const rVt0 = shadowbatch::g_rendererVtbl;
  void* fakeRenderer = reinterpret_cast<void*>(0x4e4e);
  api = fakeRenderer;
  shadowbatch::g_rendererApi = reinterpret_cast<void***>(&api);
  g_rendVt[54] = reinterpret_cast<void*>(&FFb);
  g_rendVt[40] = reinterpret_cast<void*>(&FVp);
  shadowbatch::g_rendererVtbl = g_rendVt;
  g_renderThread = GetCurrentThreadId();
  g_planActive = true;
  g_batching = true;
  g_firstItem = true;
  g_onFirstItem = nullptr;
  g_tex23.clear();
  *reinterpret_cast<int64_t*>(w->globals + 0x1e8) = 100;
  uint64_t ret = 77;
  const bool solo0 = Override(w->r[0] + 0, nullptr, &ret);  // the leader path needs D3D: only its first-item capture
  (void)solo0;
  g_grp[g_rec[0].group].failed = 0;
  ret = 77;
  g_tex23.clear();
  const bool handled = Override(w->r[1], nullptr, &ret);
  const bool member = handled && ret == 0 && g_tex23.size() == 2 && g_tex23[0].first == w->tex[7].tex &&
                      g_tex23[1].first == w->tex[1].tex && g_tex23[0].second == Size(101, 50) &&
                      g_tex23[1].second == Size(101, 50) && *reinterpret_cast<int64_t*>(w->globals + 0x1e8) == 111 &&
                      g_fb0 == g_fakeFb && !g_disabled.load();
  // A framebuffer change inside the pass latches off.
  g_fakeFb = reinterpret_cast<void*>(0xfc);
  Override(w->r[1], nullptr, &ret);
  const bool latched = g_disabled.load() && g_rendererChanged.load() >= 1;
  g_fakeFb = reinterpret_cast<void*>(0xfb);
  Check(member, "gbuffer batch: a skipped member adds its triangle count and makes slot 26's streaming request "
                "(vt[23], its own size) for each bound texture, in order");
  Check(latched, "gbuffer batch: a framebuffer/viewport change inside the pass latches batching off");
  g_disabled = false;
  g_planActive = g_batching = false;
  g_renderThread = 0;
  shadowbatch::g_rendererApi = reinterpret_cast<void***>(rApi0);
  shadowbatch::g_rendererVtbl = rVt0;
  delete[] shadowinst::g_map;
  shadowinst::g_map = map0;
  g_ng = ng0;
  g_dx = dx0;
  ResetCounters();
  delete w;
}

// ---------------------------------------------------------------------------
// G-buffer compare (WARP)
// ---------------------------------------------------------------------------
ID3D11Texture2D* MakeTarget(ID3D11Device* dev, DXGI_FORMAT f, UINT w, UINT h, UINT slices, UINT samples, UINT bind) {
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = slices;
  d.Format = f;
  d.SampleDesc.Count = samples;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = bind;
  ID3D11Texture2D* t = nullptr;
  dev->CreateTexture2D(&d, nullptr, &t);
  return t;
}

// One round: capture, change, copy A, restore, copy B, compare.
bool Round(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, bool change,
           int64_t* total, uint64_t* texels) {
  using namespace gbverify;
  ctx->OMSetRenderTargets(1, &rtv, dsv);
  const float c1[4] = {0.25f, 0.5f, 0.75f, 1.0f}, c2[4] = {0.3f, 0.5f, 0.75f, 1.0f};
  ctx->ClearRenderTargetView(rtv, c1);
  ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.25f, 0);
  const char* why = Capture();
  if (why) {
    printf("     capture: %s\n", why);
    return false;
  }
  CopyAll(true, false);  // init = state 1
  if (change) {
    ctx->ClearRenderTargetView(rtv, c2);
    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.75f, 0);
  }
  CopyAll(true, true);    // A
  CopyAll(false, false);  // targets = state 1
  CopyAll(true, false);   // B
  int64_t bad[kMaxTargets];
  *texels = 0;
  *total = CompareGuarded(bad, texels);
  ReleaseViews();
  ID3D11RenderTargetView* none = nullptr;
  ctx->OMSetRenderTargets(1, &none, nullptr);
  return true;
}

void Compare() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
    Check(false, "gbuffer verify: WARP device");
    return;
  }
  if (!shadowinst::g_compiler.Load()) printf("     (no d3dcompiler: MSAA compare untested)\n");
  ID3D11DeviceContext* const ctx0 = g_ctx;
  g_ctx = ctx;
  gbverify::g_dev = dev;
  dev->AddRef();
  const UINT W = 37, H = 19;
  // Single sample: a 3-slice RTV range of a 4-slice array, typed D32_FLOAT depth.
  ID3D11Texture2D* rt = MakeTarget(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, 4, 1, D3D11_BIND_RENDER_TARGET);
  ID3D11Texture2D* ds = MakeTarget(dev, DXGI_FORMAT_D32_FLOAT, W, H, 4, 1, D3D11_BIND_DEPTH_STENCIL);
  D3D11_RENDER_TARGET_VIEW_DESC rd = {};
  rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
  rd.Texture2DArray.FirstArraySlice = 1;
  rd.Texture2DArray.ArraySize = 3;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  if (rt) dev->CreateRenderTargetView(rt, &rd, &rtv);
  D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
  dd.Format = DXGI_FORMAT_D32_FLOAT;
  dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
  dd.Texture2DArray.FirstArraySlice = 1;
  dd.Texture2DArray.ArraySize = 3;  // RTV and DSV must cover the same number of slices
  if (ds) dev->CreateDepthStencilView(ds, &dd, &dsv);
  int64_t diff = -1, same = -1;
  uint64_t texels = 0, texels2 = 0;
  bool ok = rtv && dsv && Round(ctx, rtv, dsv, true, &diff, &texels) && Round(ctx, rtv, dsv, false, &same, &texels2);
  ok &= diff == static_cast<int64_t>(W * H * 6) && same == 0 && texels == texels2 && texels == W * H * 6;
  Check(ok, "gbuffer verify: single-sample targets: restore round trip and bit compare over the views' slices "
            "(RTV and DSV array ranges, typed D32 depth)");
  if (!ok) printf("     diff %lld same %lld texels %llu\n", static_cast<long long>(diff), static_cast<long long>(same),
                  static_cast<unsigned long long>(texels));
  for (IUnknown* u : {static_cast<IUnknown*>(rtv), static_cast<IUnknown*>(dsv), static_cast<IUnknown*>(rt),
                      static_cast<IUnknown*>(ds)})
    if (u) u->Release();
  // MSAA 4x: per-sample compare on the GPU, CS state restored.
  UINT q = 0;
  dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R16G16B16A16_FLOAT, 4, &q);
  if (q && shadowinst::g_compiler.compile2) {
    ID3D11Texture2D* mrt = MakeTarget(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, 1, 4, D3D11_BIND_RENDER_TARGET);
    ID3D11Texture2D* mds = MakeTarget(dev, DXGI_FORMAT_D32_FLOAT, W, H, 1, 4, D3D11_BIND_DEPTH_STENCIL);
    ID3D11RenderTargetView* mrtv = nullptr;
    ID3D11DepthStencilView* mdsv = nullptr;
    if (mrt) dev->CreateRenderTargetView(mrt, nullptr, &mrtv);
    if (mds) dev->CreateDepthStencilView(mds, nullptr, &mdsv);
    // A CS SRV bound before must still be bound after.
    ID3D11Texture2D* probe = MakeTarget(dev, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 1, D3D11_BIND_SHADER_RESOURCE);
    ID3D11ShaderResourceView* probeSrv = nullptr;
    if (probe) dev->CreateShaderResourceView(probe, nullptr, &probeSrv);
    ctx->CSSetShaderResources(1, 1, &probeSrv);
    int64_t mdiff = -1, msame = -1;
    uint64_t mt = 0, mt2 = 0;
    bool mok = mrtv && mdsv && Round(ctx, mrtv, mdsv, true, &mdiff, &mt) && Round(ctx, mrtv, mdsv, false, &msame, &mt2);
    ID3D11ShaderResourceView* after[2] = {};
    ctx->CSGetShaderResources(0, 2, after);
    ID3D11ComputeShader* csAfter = nullptr;
    ctx->CSGetShader(&csAfter, nullptr, nullptr);
    mok &= mdiff == static_cast<int64_t>(W * H * 4 * 2) && msame == 0 && mt == W * H * 4 * 2 && after[0] == nullptr &&
           after[1] == probeSrv && csAfter == nullptr;
    for (ID3D11ShaderResourceView* v : after)
      if (v) v->Release();
    if (csAfter) csAfter->Release();
    Check(mok, "gbuffer verify: MSAA 4x targets compared per sample by the compute shader (UINT views); CS state "
               "restored");
    if (!mok) printf("     ms diff %lld same %lld samples %llu\n", static_cast<long long>(mdiff),
                     static_cast<long long>(msame), static_cast<unsigned long long>(mt));
    ID3D11ShaderResourceView* none = nullptr;
    ctx->CSSetShaderResources(1, 1, &none);
    // A format without a UINT view is refused for MSAA.
    ID3D11Texture2D* brt = MakeTarget(dev, DXGI_FORMAT_B8G8R8A8_UNORM, W, H, 1, 4, D3D11_BIND_RENDER_TARGET);
    ID3D11RenderTargetView* brtv = nullptr;
    if (brt) dev->CreateRenderTargetView(brt, nullptr, &brtv);
    ctx->OMSetRenderTargets(1, &brtv, nullptr);
    const char* why = gbverify::Capture();
    gbverify::ReleaseViews();
    ID3D11RenderTargetView* nrt = nullptr;
    ctx->OMSetRenderTargets(1, &nrt, nullptr);
    if (brtv)
      Check(why && strncmp(why, "MSAA", 4) == 0,
            "gbuffer verify: an MSAA target without an exact per-sample compare (B8G8R8A8) is refused");
    else
      printf("SKIP gbuffer verify: WARP has no 4x B8G8R8A8 target\n");
    for (IUnknown* u : {static_cast<IUnknown*>(mrtv), static_cast<IUnknown*>(mdsv), static_cast<IUnknown*>(mrt),
                        static_cast<IUnknown*>(mds), static_cast<IUnknown*>(probeSrv), static_cast<IUnknown*>(probe),
                        static_cast<IUnknown*>(brtv), static_cast<IUnknown*>(brt)})
      if (u) u->Release();
  } else {
    printf("SKIP gbuffer verify: WARP without 4x MSAA for R16G16B16A16_FLOAT, or no compiler\n");
  }
  gbverify::Shutdown();
  g_ctx = ctx0;
  ctx->Release();
  dev->Release();
}

void Run() {
  Predictor();
  GetSrvDifferential();
  Animated();
  Planner();
  Compare();
}

}  // namespace gbbtest
