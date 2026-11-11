// Offline test for fx_skip.h (A3, FX pass Apply repeat counter and lean Apply).
//  1. Pure parts: the signature, the refusal reasons on hand-built passes, the
//     memory expectation check and the context-state compare (mismatch found,
//     equal passes).
//  2. When DCS is installed (dcs_install.h): a relocated copy of dx11backend.dll
//     whose code runs for real (stock SPassBlock::Apply 0x65850 and the record
//     evaluator 0x67cc0; no import is reached). The kCode hashes against the
//     image; the evaluator's return value against WouldEvaluate on random
//     records; then random FX worlds (pass, state blocks, records, shader
//     blocks with CB / sampler / SRV dependencies and tbuffers, a renderer with
//     its SRV cache, a fake context and fake per-stage call tables that log
//     every call): for every world Check calls eligible, stock Apply and the
//     lean Apply on the same starting bytes must make the same calls with the
//     same arguments in the same order and leave every byte equal, and the
//     verify prediction (PredictMem) must hold after stock.
#pragma once

namespace fxstest {

namespace fs = fxskip;

uint64_t g_rng = 0x9e3779b97f4a7c15ull;
uint32_t Rand() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return static_cast<uint32_t>(g_rng >> 11);
}

// ---------------------------------------------------------------------------
// Call log (fake context table and fake per-stage call tables)
// ---------------------------------------------------------------------------

struct Ev {
  int op;
  uint64_t a, b, c, h;
  bool operator==(const Ev& o) const { return op == o.op && a == o.a && b == o.b && c == o.c && h == o.h; }
};
std::vector<Ev> g_log;
uint64_t HashPtrs(void* const* v, UINT n) {
  uint64_t h = 1469598103934665603ull;
  for (UINT i = 0; v && i < n; ++i) h = (h ^ reinterpret_cast<uintptr_t>(v[i])) * 1099511628211ull;
  return h;
}
void Note(int op, uint64_t a, uint64_t b, uint64_t c, uint64_t h) { g_log.push_back({op, a, b, c, h}); }
#define U(x) static_cast<uint64_t>(reinterpret_cast<uintptr_t>(x))

void STDMETHODCALLTYPE CtxBlend(void*, void* s, const float* f, UINT m) {
  uint64_t h = 0;
  memcpy(&h, f, 8);
  uint64_t h2 = 0;
  memcpy(&h2, f + 2, 8);
  Note(1, U(s), h ^ (h2 * 31), m, 0);
}
void STDMETHODCALLTYPE CtxDepth(void*, void* s, UINT r) { Note(2, U(s), r, 0, 0); }
void STDMETHODCALLTYPE CtxRaster(void*, void* s) { Note(3, U(s), 0, 0, 0); }
void STDMETHODCALLTYPE CtxUpdate(void*, void* res, UINT sub, const void* box, const void* data, UINT row, UINT dp) {
  Note(4, U(res), U(data), row | static_cast<uint64_t>(dp) << 32, sub + U(box));
}
void STDMETHODCALLTYPE CtxHs(void*, void* s, void* const*, UINT n) { Note(5, U(s), n, 0, 0); }
void STDMETHODCALLTYPE CtxDs(void*, void* s, void* const*, UINT n) { Note(6, U(s), n, 0, 0); }
void STDMETHODCALLTYPE CtxOther(void*) { Note(99, 0, 0, 0, 0); }

template <int K>
void STDMETHODCALLTYPE TblShader(void*, void* s, void* const* ci, UINT n) {
  Note(10 + K * 10, U(s), U(ci), n, 0);
}
template <int K>
void STDMETHODCALLTYPE TblCb(void*, UINT s, UINT n, void* const* v) {
  Note(11 + K * 10, s, n, U(v), HashPtrs(v, n));
}
template <int K>
void STDMETHODCALLTYPE TblSamp(void*, UINT s, UINT n, void* const* v) {
  Note(12 + K * 10, s, n, U(v), HashPtrs(v, n));
}
template <int K>
void STDMETHODCALLTYPE TblSrv(void*, UINT s, UINT n, void* const* v) {
  Note(13 + K * 10, s, n, 0, HashPtrs(v, n));  // the array is the renderer cache (same address either way)
}
void* g_tables[5][5] = {
    {reinterpret_cast<void*>(&TblShader<0>), reinterpret_cast<void*>(&TblCb<0>), reinterpret_cast<void*>(&TblSamp<0>),
     reinterpret_cast<void*>(&TblSrv<0>), nullptr},
    {reinterpret_cast<void*>(&TblShader<1>), reinterpret_cast<void*>(&TblCb<1>), reinterpret_cast<void*>(&TblSamp<1>),
     reinterpret_cast<void*>(&TblSrv<1>), nullptr},
    {reinterpret_cast<void*>(&TblShader<2>), reinterpret_cast<void*>(&TblCb<2>), reinterpret_cast<void*>(&TblSamp<2>),
     reinterpret_cast<void*>(&TblSrv<2>), nullptr},
    {reinterpret_cast<void*>(&TblShader<3>), reinterpret_cast<void*>(&TblCb<3>), reinterpret_cast<void*>(&TblSamp<3>),
     reinterpret_cast<void*>(&TblSrv<3>), nullptr},
    {reinterpret_cast<void*>(&TblShader<4>), reinterpret_cast<void*>(&TblCb<4>), reinterpret_cast<void*>(&TblSamp<4>),
     reinterpret_cast<void*>(&TblSrv<4>), nullptr},
};

void* g_ctxVt[0x100];
struct FakeCtx {
  void** vt;
} g_ctx;

void InitCtx() {
  for (auto& e : g_ctxVt) e = reinterpret_cast<void*>(&CtxOther);
  g_ctxVt[0x118 / 8] = reinterpret_cast<void*>(&CtxBlend);
  g_ctxVt[0x120 / 8] = reinterpret_cast<void*>(&CtxDepth);
  g_ctxVt[0x158 / 8] = reinterpret_cast<void*>(&CtxRaster);
  g_ctxVt[0x180 / 8] = reinterpret_cast<void*>(&CtxUpdate);
  g_ctxVt[0x1e0 / 8] = reinterpret_cast<void*>(&CtxHs);
  g_ctxVt[0x200 / 8] = reinterpret_cast<void*>(&CtxDs);
  g_ctx.vt = g_ctxVt;
}

// ---------------------------------------------------------------------------
// FX world in one arena (snapshot / restore by memcpy: every pointer stays valid)
// ---------------------------------------------------------------------------

struct Arena {
  alignas(64) uint8_t mem[1 << 18];
  size_t used = 0;
  uint8_t* Get(size_t n) {
    used = (used + 15) & ~size_t(15);
    uint8_t* p = mem + used;
    used += n;
    memset(p, 0, n);
    return p;
  }
};

struct World {
  Arena* a;
  uint8_t* pass;
  uint8_t* fx;
  uint8_t* r;
  uint32_t rs;
};

// A variable usable by every record type: +8 data (u32 index, then values), +0x18 info (+0xc count, +0x1c stride), +0x40 time.
uint8_t* MakeVar(Arena& a, uint64_t mod) {
  uint8_t* v = a.Get(0x48);
  uint8_t* data = a.Get(0x80);
  uint8_t* info = a.Get(0x20);

  fs::At<uint8_t*>(v, 8) = data;
  fs::At<uint8_t*>(v, 0x18) = info;
  fs::At<uint32_t>(info, 0xc) = 4;
  fs::At<uint32_t>(info, 0x1c) = 8;
  fs::At<uint64_t>(v, 0x40) = mod;
  return v;
}

// n records; quiet = none would be evaluated at timer `t` (last > every dependency's time).
uint8_t* MakeRecs(Arena& a, uint32_t n, uint64_t t, bool quiet) {
  if (!n) return nullptr;
  uint8_t* recs = a.Get(n * fs::kRecSize);
  const uint32_t types[] = {1, 2, 4, 8, 3};
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t* r = recs + i * fs::kRecSize;
    fs::At<uint32_t>(r, 4) = types[Rand() % 5];
    const uint64_t last = t - (Rand() % 50);
    fs::At<uint64_t>(r, 8) = last;
    auto** deps = reinterpret_cast<uint8_t**>(a.Get(16));
    const bool evaluate = !quiet && i == 0;
    deps[0] = MakeVar(a, evaluate ? last + (Rand() % 3) : last - 1 - Rand() % 40);
    deps[1] = MakeVar(a, evaluate && Rand() % 2 ? last : last - 1 - Rand() % 40);
    if (evaluate && fs::At<uint32_t>(r, 4) != 2 && fs::At<uint32_t>(r, 4) != 4 && fs::At<uint32_t>(r, 4) != 8)
      fs::At<uint32_t>(r, 4) = 2;
    fs::At<uint8_t**>(r, 0x18) = deps;
    uint8_t* dest = a.Get(0x40);
    fs::At<uint8_t*>(r, 0x20) = dest;  // type 8 writes a pointer into *dest: dest must hold a pointer slot
    fs::At<uint8_t*>(dest, 0) = a.Get(0x40);
    fs::At<uint8_t*>(r, 0x28) = a.Get(0x80);
    fs::At<uint16_t>(r, 0x30) = 8;
    fs::At<uint16_t>(r, 0x32) = 4;
  }
  return recs;
}

uint8_t* MakeStateBlock(Arena& a, uint64_t t, bool quiet, int objs) {
  uint8_t* b = a.Get(0x18 + 8 * objs + 0x10);
  fs::At<uint32_t>(b, 0) = 1 + Rand() % 4;
  fs::At<uint8_t>(b, 4) = Rand() % 4 == 0 ? 1 : 0;  // user-managed
  const uint32_t n = Rand() % 3;
  fs::At<uint32_t>(b, 8) = n;
  fs::At<uint8_t*>(b, 0x10) = MakeRecs(a, n, t, quiet);
  for (int i = 0; i < objs; ++i) fs::At<uint64_t>(b, 0x18 + 8 * i) = 0x5000000 + Rand() % 64 * 16;
  return b;
}

uint8_t* MakeCb(Arena& a) {
  uint8_t* cb = a.Get(0x60);
  fs::At<uint64_t>(cb, fs::kCbBuffer) = 0x7000000 + Rand() % 64 * 16;
  fs::At<uint8_t*>(cb, fs::kCbBytes) = a.Get(32);
  fs::At<uint32_t>(cb, fs::kCbSize) = 32;
  const uint8_t flags[] = {0, 1, 0x81, 0x80, 1};
  fs::At<uint8_t>(cb, fs::kCbFlags) = flags[Rand() % 5];
  return cb;
}

uint8_t* MakeDeps(Arena& a, uint32_t n) { return n ? a.Get(n * fs::kDepSize) : nullptr; }

// A shader stage block with random dependencies; SRV views come from `pool`.
uint8_t* MakeStage(Arena& a, int k, uint64_t t, void* const* pool, bool cacheHit, uint8_t* r, bool noDeps) {
  uint8_t* blk = a.Get(0x80);
  fs::At<void**>(blk, fs::kBTable) = g_tables[k];
  fs::At<uint64_t>(blk, fs::kBShader) = Rand() % 6 == 0 ? 0 : 0x3000000 + k * 0x1000 + Rand() % 8 * 16;
  if (noDeps) return blk;
  const uint32_t nCb = Rand() % 3, nSamp = Rand() % 3, nTb = Rand() % 2;
  uint32_t nSrv = Rand() % 4;
  if (cacheHit) nSrv = 1;
  fs::At<uint32_t>(blk, fs::kBCbN) = nCb;
  uint8_t* cbDeps = MakeDeps(a, nCb);
  fs::At<uint8_t*>(blk, fs::kBCb) = cbDeps;
  for (uint32_t d = 0; d < nCb; ++d) {
    uint8_t* dep = cbDeps + d * fs::kDepSize;
    const uint32_t n = 1 + Rand() % 3;
    fs::At<uint32_t>(dep, 0) = Rand() % 8;
    fs::At<uint32_t>(dep, 4) = n;
    auto** fx = reinterpret_cast<uint8_t**>(a.Get(8 * n));
    auto* bound = reinterpret_cast<uint64_t*>(a.Get(8 * n));
    for (uint32_t i = 0; i < n; ++i) {
      fx[i] = MakeCb(a);
      bound[i] = fs::At<uint64_t>(fx[i], fs::kCbBuffer);
    }
    fs::At<uint8_t**>(dep, 8) = fx;
    fs::At<uint64_t*>(dep, 0x10) = bound;
  }
  fs::At<uint32_t>(blk, fs::kBSampN) = nSamp;
  uint8_t* spDeps = MakeDeps(a, nSamp);
  fs::At<uint8_t*>(blk, fs::kBSamp) = spDeps;
  for (uint32_t d = 0; d < nSamp; ++d) {
    uint8_t* dep = spDeps + d * fs::kDepSize;
    const uint32_t n = 1 + Rand() % 2;
    fs::At<uint32_t>(dep, 0) = Rand() % 8;
    fs::At<uint32_t>(dep, 4) = n;
    auto** fx = reinterpret_cast<uint8_t**>(a.Get(8 * n));
    auto* bound = reinterpret_cast<uint64_t*>(a.Get(8 * n));
    for (uint32_t i = 0; i < n; ++i) {
      fx[i] = MakeStateBlock(a, t, true, 1);
      bound[i] = fs::At<uint64_t>(fx[i], fs::kSbObj);
    }
    fs::At<uint8_t**>(dep, 8) = fx;
    fs::At<uint64_t*>(dep, 0x10) = bound;
  }
  fs::At<uint32_t>(blk, fs::kBTbN) = nTb;
  auto** tb = reinterpret_cast<uint8_t**>(a.Get(8 + 8 * nTb));
  for (uint32_t i = 0; i < nTb; ++i) tb[i] = MakeCb(a);
  fs::At<uint8_t**>(blk, fs::kBTb) = tb;
  fs::At<uint32_t>(blk, fs::kBSrvN) = nSrv;
  uint8_t* svDeps = MakeDeps(a, nSrv);
  fs::At<uint8_t*>(blk, fs::kBSrv) = svDeps;
  const int st = fs::kStageIdx[k];
  void** cache = reinterpret_cast<void**>(r + fs::kRSrvCache + st * fs::kRSrvStride);
  uint32_t c = 0;
  for (uint32_t d = 0; d < nSrv; ++d) {
    uint8_t* dep = svDeps + d * fs::kDepSize;
    const uint32_t n = Rand() % 5;
    fs::At<uint32_t>(dep, 0) = Rand() % 4;
    fs::At<uint32_t>(dep, 4) = n;
    auto** fx = reinterpret_cast<void***>(a.Get(8 * n + 8));
    auto** bound = reinterpret_cast<void**>(a.Get(8 * n + 8));
    for (uint32_t i = 0; i < n; ++i, ++c) {
      auto** slot = reinterpret_cast<void**>(a.Get(8));
      *slot = pool[Rand() % 6];
      fx[i] = slot;
      bound[i] = Rand() % 2 ? *slot : pool[Rand() % 6];  // stale bound arrays are refreshed by Apply
      if (cacheHit) cache[c] = *slot;
    }
    fs::At<void***>(dep, 8) = fx;
    fs::At<void**>(dep, 0x10) = bound;
  }
  uint32_t& cnt = fs::At<uint32_t>(r, fs::kRSrvCount + st * 4);
  if (cacheHit)
    cnt = c - (c && Rand() % 3 == 0 ? Rand() % (c + 1) : 0);  // count <= n: nothing bound
  else
    for (uint32_t i = 0, m = Rand() % 6; i < m; ++i) cache[i] = pool[Rand() % 6];
  return blk;
}

struct Shape {
  bool rt = false, cs = false, uav = false, assign = false, stateAssign = false, tessFlag = false;
};

World MakeWorld(Arena& a, const Shape& s) {
  a.used = 0;
  World w{};
  w.a = &a;
  const uint64_t t = 1000 + Rand() % 1000;
  w.fx = a.Get(0x200);
  fs::At<uint64_t>(w.fx, fs::kETimer) = t;
  fs::At<uint64_t>(w.fx, 0x148) = 0x1234;
  w.r = a.Get(fs::kRSrvCache + 6 * fs::kRSrvStride);
  fs::At<void*>(w.r, fs::kRCtx) = &g_ctx;
  w.pass = a.Get(0x110);
  fs::At<uint8_t*>(w.pass, fs::kPEffect) = w.fx;
  const uint32_t nRec = Rand() % 4 + (s.assign ? 1 : 0);
  fs::At<uint32_t>(w.pass, fs::kPAssignN) = nRec;
  fs::At<uint8_t*>(w.pass, fs::kPAssign) = MakeRecs(a, nRec, t, !s.assign);
  float f[4] = {1.0f, 0.5f, static_cast<float>(Rand() % 4), 0.25f};
  memcpy(w.pass + fs::kPFactor, f, sizeof(f));
  fs::At<uint32_t>(w.pass, fs::kPMask) = Rand() % 2 ? 0xffffffffu : 0xf;
  fs::At<uint32_t>(w.pass, fs::kPRef) = Rand() % 2 ? 0xffffffffu : Rand() % 256;
  fs::At<uint64_t>(w.pass, fs::kPBlendObj) = 0xdead;  // stale: Apply writes it
  fs::At<uint64_t>(w.pass, fs::kPDepthObj) = 0xbeef;
  w.rs = Rand() % 2;
  if (Rand() % 5) fs::At<uint8_t*>(w.pass, fs::kPBlendBlk) = MakeStateBlock(a, t, true, 1);
  if (Rand() % 5) fs::At<uint8_t*>(w.pass, fs::kPDepthBlk) = MakeStateBlock(a, t, true, 1);
  if (Rand() % 5) fs::At<uint8_t*>(w.pass, fs::kPRasterBlk) = MakeStateBlock(a, t, !s.stateAssign, 2);
  if (s.stateAssign) {  // a raster block, not user-managed, whose one record is evaluated
    uint8_t* b = fs::At<uint8_t*>(w.pass, fs::kPRasterBlk);
    if (!b) fs::At<uint8_t*>(w.pass, fs::kPRasterBlk) = b = MakeStateBlock(a, t, true, 2);
    fs::At<uint8_t>(b, 4) = 0;
    fs::At<uint32_t>(b, 8) = 1;
    fs::At<uint8_t*>(b, 0x10) = MakeRecs(a, 1, t, false);
  }
  void* pool[6];
  for (int i = 0; i < 6; ++i) pool[i] = reinterpret_cast<void*>(static_cast<uintptr_t>(0x9000000 + i * 0x40));
  pool[5] = nullptr;
  for (int k = 0; k < fs::kStages; ++k) {
    const bool present = k < 2 ? Rand() % 8 != 0 : Rand() % 4 == 0;
    if (!present) continue;
    const bool hit = Rand() % 2 == 0;
    fs::At<uint8_t*>(w.pass, fs::kStageOff[k]) = MakeStage(a, k, t, pool, hit, w.r, k >= 2 && Rand() % 2);
  }
  const uint8_t* hs = fs::At<uint8_t*>(w.pass, fs::kStageOff[3]);
  const uint8_t* ds = fs::At<uint8_t*>(w.pass, fs::kStageOff[4]);
  const bool tess = (hs && fs::At<void*>(hs, fs::kBShader)) || (ds && fs::At<void*>(ds, fs::kBShader));
  fs::At<uint8_t>(w.r, fs::kRTess) = s.tessFlag ? 1 : (tess ? static_cast<uint8_t>(Rand() % 2) : 0);
  if (s.tessFlag) {
    fs::At<uint8_t*>(w.pass, fs::kStageOff[3]) = nullptr;
    fs::At<uint8_t*>(w.pass, fs::kStageOff[4]) = nullptr;
  }
  if (s.rt) fs::At<uint64_t>(w.pass, fs::kPRtBlk) = 0x1000;
  if (s.cs) fs::At<uint64_t>(w.pass, fs::kPCs) = 0x1000;
  if (s.uav) {
    uint8_t* blk = fs::At<uint8_t*>(w.pass, fs::kStageOff[1]);
    if (!blk) fs::At<uint8_t*>(w.pass, fs::kStageOff[1]) = blk = MakeStage(a, 1, t, pool, false, w.r, true);
    fs::At<uint32_t>(blk, fs::kBUavN) = 1;
  }
  return w;
}

// ---------------------------------------------------------------------------
// 1. Pure parts
// ---------------------------------------------------------------------------

void Pure() {
  InitCtx();
  static Arena a;
  // Refusal reasons.
  struct Case {
    Shape s;
    uint8_t why;
    const char* name;
  } cases[] = {
      {{}, fs::kOk, "plain"},
      {{true}, fs::kRt, "RT block"},
      {{false, true}, fs::kCompute, "compute block"},
      {{false, false, true}, fs::kUav, "UAV dependency"},
      {{false, false, false, true}, fs::kAssign, "pass record to evaluate"},
      {{false, false, false, false, true}, fs::kStateAssign, "state record to evaluate"},
      {{false, false, false, false, false, true}, fs::kTess, "tessellation unbind"},
  };
  for (const Case& c : cases) {
    bool ok = true;
    for (int i = 0; i < 200 && ok; ++i) {
      World w = MakeWorld(a, c.s);
      fs::Plan p;
      fs::Check(w.pass, w.rs, w.r, p);
      ok = p.why == c.why;
    }
    char what[160];
    snprintf(what, sizeof(what), "fx repeat: Check reason on %s worlds is \"%s\" (200 worlds)", c.name, fs::kWhyNames[c.why]);
    Check(ok, what);
  }
  // Signature: same bytes -> same; a bound pointer / a view / a dirty upload changes it.
  {
    World w = MakeWorld(a, {});
    fs::Plan p1, p2;
    fs::Check(w.pass, w.rs, w.r, p1);
    fs::Check(w.pass, w.rs, w.r, p2);
    bool ok = p1.sig == p2.sig;
    fs::Check(w.pass, w.rs ^ 1, w.r, p2);
    ok = ok && p2.sig != p1.sig;
    fs::At<uint64_t>(w.pass, fs::kPMask) ^= 1;
    fs::Check(w.pass, w.rs, w.r, p2);
    fs::At<uint64_t>(w.pass, fs::kPMask) ^= 1;
    ok = ok && (fs::At<uint8_t*>(w.pass, fs::kPBlendBlk) ? p2.sig != p1.sig : p2.sig == p1.sig);
    Check(ok, "fx repeat: signature is stable and follows the raster index and the blend mask");
  }
  // CheckMem: equal -> -1; one changed byte -> its index.
  {
    static fs::ExpList x;
    uint64_t mem[4] = {1, 2, 3, 4};
    x.n = 0;
    x.Add(&mem[0], 1, 8);
    x.Add(&mem[1], 2, 4);
    x.Add(&mem[2], 3, 1);
    bool ok = fs::CheckMem(x) == -1;
    mem[1] = 0x100000002ull;  // high half not compared at size 4
    ok = ok && fs::CheckMem(x) == -1;
    mem[2] = 0x103;  // low byte compared at size 1
    ok = ok && fs::CheckMem(x) == -1;
    mem[2] = 4;
    ok = ok && fs::CheckMem(x) == 2;
    Check(ok, "fx repeat: memory expectations: sizes 1/4/8, first unmet index");
  }
  // CompareDev: equal; kAny slots ignored; each kind of difference found.
  {
    static fs::DevState want, got;
    memset(&want, 0, sizeof(want));
    want.blend = reinterpret_cast<void*>(uintptr_t{0x10});
    want.depth = reinterpret_cast<void*>(uintptr_t{0x20});
    want.ref = 0xff;
    want.rs = true;
    want.raster = reinterpret_cast<void*>(uintptr_t{0x30});
    want.stages = 3;
    for (int k = 0; k < 2; ++k) {
      want.shader[k] = reinterpret_cast<void*>(uintptr_t{0x40} + k);
      for (auto& v : want.cb[k]) v = fs::kAny;
      for (auto& v : want.samp[k]) v = fs::kAny;
      want.cb[k][3] = reinterpret_cast<void*>(uintptr_t{0x50});
      want.samp[k][0] = reinterpret_cast<void*>(uintptr_t{0x60});
      want.nSrv[k] = 2;
      want.srv[k][1] = reinterpret_cast<void*>(uintptr_t{0x70});
    }
    got = want;
    for (int k = 0; k < 2; ++k) {
      got.cb[k][5] = reinterpret_cast<void*>(uintptr_t{0x999});  // not bound by the pass: ignored
      got.nSrv[k] = 2;
    }
    char why[200];
    bool ok = fs::CompareDev(want, got, why, sizeof(why)) && !why[0];
    auto expectDiff = [&](void (*mut)(fs::DevState&), const char* word) {
      fs::DevState* g2 = new fs::DevState(got);
      mut(*g2);
      char w2[200];
      const bool same = fs::CompareDev(want, *g2, w2, sizeof(w2));
      delete g2;
      return !same && strstr(w2, word) != nullptr;
    };
    ok = ok && expectDiff([](fs::DevState& d) { d.blend = nullptr; }, "blend");
    ok = ok && expectDiff([](fs::DevState& d) { d.factor[2] = 3.0f; }, "blend");
    ok = ok && expectDiff([](fs::DevState& d) { d.ref = 0x7f; }, "depth");
    ok = ok && expectDiff([](fs::DevState& d) { d.raster = nullptr; }, "raster");
    ok = ok && expectDiff([](fs::DevState& d) { d.shader[1] = nullptr; }, "PS shader");
    ok = ok && expectDiff([](fs::DevState& d) { d.cb[0][3] = nullptr; }, "VS CB slot 3");
    ok = ok && expectDiff([](fs::DevState& d) { d.samp[1][0] = nullptr; }, "PS sampler slot 0");
    ok = ok && expectDiff([](fs::DevState& d) { d.srv[0][1] = nullptr; }, "VS SRV slot 1");
    Check(ok, "fx repeat: context-state compare: equal, unbound slots ignored, every kind of difference named");
  }
  // Call-site decoding (Via6 / Via5).
  {
    alignas(8) uint8_t site[16] = {0xFF, 0x90, 0x58, 0x01, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC};
    bool ok = fs::Via6(site, &g_ctx) == g_ctxVt[0x158 / 8];
    site[0] = 0x90;
    site[1] = 0xE8;
    const int32_t rel = 0x100;
    memcpy(site + 2, &rel, 4);
    ok = ok && fs::Via6(site, &g_ctx) == site + 6 + 0x100;
    site[0] = 0x0F;
    ok = ok && fs::Via6(site, &g_ctx) == nullptr;
    site[0] = 0xE8;
    memcpy(site + 1, &rel, 4);
    ok = ok && fs::Via5(site) == site + 5 + 0x100;
    site[0] = 0x90;
    ok = ok && fs::Via5(site) == nullptr;
    Check(ok, "fx repeat: call sites decoded: context table, split-filter stub, call rel32, anything else refused");
  }
}

// ---------------------------------------------------------------------------
// 2. Real dx11backend code
// ---------------------------------------------------------------------------

// The image mapped as data, copied to executable memory, base relocations applied.
uint8_t* CopyImage(const std::wstring& path, size_t& size) {
  HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
  if (!m) return nullptr;
  const uint8_t* src = sigscan::ImageBase(m);
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(src + reinterpret_cast<const IMAGE_DOS_HEADER*>(src)->e_lfanew);
  size = nt->OptionalHeader.SizeOfImage;
  auto* img = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!img) {
    FreeLibrary(m);
    return nullptr;
  }
  memcpy(img, src, size);
  const uint64_t delta = reinterpret_cast<uint64_t>(img) - nt->OptionalHeader.ImageBase;
  const IMAGE_DATA_DIRECTORY& rd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
  for (uint32_t at = 0; at + sizeof(IMAGE_BASE_RELOCATION) <= rd.Size;) {
    auto* b = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(img + rd.VirtualAddress + at);
    if (b->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
    const uint32_t n = (b->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
    auto* e = reinterpret_cast<const uint16_t*>(b + 1);
    for (uint32_t i = 0; i < n; ++i)
      if ((e[i] >> 12) == IMAGE_REL_BASED_DIR64) *reinterpret_cast<uint64_t*>(img + b->VirtualAddress + (e[i] & 0xfff)) += delta;
    at += b->SizeOfBlock;
  }
  // Imports: the C runtime and system ones are bound (memcpy and memset are import thunks); DCS's own stay null.
  const IMAGE_DATA_DIRECTORY& id = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(img + id.VirtualAddress); id.Size && d->Name; ++d) {
    const char* dll = reinterpret_cast<const char*>(img + d->Name);
    HMODULE h = GetModuleHandleA(dll);
    if (!h && (_strnicmp(dll, "api-ms-win-crt", 14) == 0 || _strnicmp(dll, "vcruntime", 9) == 0 ||
               _stricmp(dll, "kernel32.dll") == 0))
      h = LoadLibraryExA(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    auto* names = reinterpret_cast<const uint64_t*>(img + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
    auto* slots = reinterpret_cast<uint64_t*>(img + d->FirstThunk);
    for (int i = 0; names[i]; ++i) {
      void* fn = nullptr;
      if (h && !(names[i] & IMAGE_ORDINAL_FLAG64))
        fn = reinterpret_cast<void*>(GetProcAddress(h, reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(img + names[i])->Name));
      slots[i] = reinterpret_cast<uint64_t>(fn);
    }
  }
  FreeLibrary(m);
  FlushInstructionCache(GetCurrentProcess(), img, size);
  return img;
}

void Real() {
  if (!dcsinstall::Found()) {
    dcsinstall::Skip("fx repeat (real dx11backend Apply)");
    return;
  }
  size_t size = 0;
  uint8_t* img = CopyImage(dcsinstall::Bin() + L"dx11backend.dll", size);
  if (!img) {
    QvSkip("fx repeat: dx11backend.dll not mapped\n");
    return;
  }
  bool hashes = true;
  for (const fs::CodeRange& cr : fs::kCode) hashes = hashes && fs::MaskedCodeIs(img, cr);
  if (!hashes) {
    QvSkip("fx repeat: dx11backend.dll is not the analysed build\n");
    VirtualFree(img, 0, MEM_RELEASE);
    return;
  }
  Check(true, "fx repeat: every checked dx11backend range matches the analysed build (masked FNV)");
  InitCtx();
  static Arena a;

  // 0x67cc0 (state block records): its return value = WouldEvaluate on any record; stamps = timer.
  {
    using EvalFn = uint8_t(__fastcall*)(uint8_t* blk, uint8_t* effect);
    const auto eval = reinterpret_cast<EvalFn>(img + 0x67cc0);
    bool ok = true;
    int evaluated = 0;
    for (int i = 0; i < 20000 && ok; ++i) {
      a.used = 0;
      uint8_t* fx = a.Get(0x200);
      const uint64_t t = 1000 + Rand() % 1000;
      fs::At<uint64_t>(fx, fs::kETimer) = t;
      const uint32_t n = 1 + Rand() % 3;
      uint8_t* blk = a.Get(0x40);
      fs::At<uint32_t>(blk, 8) = n;
      fs::At<uint8_t*>(blk, 0x10) = MakeRecs(a, n, t, Rand() % 2 == 0);
      const bool predicted = !fs::RecordsQuiet(fs::At<uint8_t*>(blk, 0x10), n);
      const bool real = eval(blk, fx) != 0;
      evaluated += real;
      ok = predicted == real;
      for (uint32_t k = 0; k < n && ok; ++k) ok = fs::At<uint64_t>(fs::At<uint8_t*>(blk, 0x10) + k * fs::kRecSize, 8) == t;
    }
    char what[160];
    snprintf(what, sizeof(what), "fx repeat: record evaluation predicted exactly as dx11backend 0x67cc0 decides (20k "
             "blocks, %d evaluated), every record stamped", evaluated);
    Check(ok && evaluated > 1000, what);
  }

  // Renderer read by 0x15da0: [rip+0x9aaf1] = import slot 0xb0898 (a data import) -> pointer -> renderer.
  static void* rendererSlot = nullptr;
  *reinterpret_cast<void**>(img + 0xb0898) = &rendererSlot;
  void** rendererAt = &rendererSlot;
  const auto stock = reinterpret_cast<fs::ApplyFn>(img + fs::kApply);
  const fs::Sites sites = {img + fs::kSiteBlend, img + fs::kSiteDepth, img + fs::kSiteRaster, img + fs::kSiteSrv};
  static uint8_t before[sizeof(Arena::mem)], afterStock[sizeof(Arena::mem)];
  static fs::ExpList x;
  int worlds = 0, fastStages = 0, slowStages = 0, uploads = 0, refused = 0, tessStages = 0;
  bool calls = true, bytes = true, predicted = true, ran = true;
  for (int i = 0; i < 20000 && calls && bytes && predicted && ran; ++i) {
    World w = MakeWorld(a, {});
    *rendererAt = w.r;
    fs::Plan p;
    fs::Check(w.pass, w.rs, w.r, p);
    if (p.why != fs::kOk) {
      ++refused;
      continue;
    }
    ++worlds;
    for (int k = 0; k < fs::kStages; ++k) {
      const uint8_t* blk = fs::At<uint8_t*>(w.pass, fs::kStageOff[k]);
      if (blk && fs::At<uint32_t>(blk, fs::kBSrvN)) ((p.srvFast >> k) & 1 ? fastStages : slowStages)++;
      if (blk && k >= 3 && fs::At<void*>(blk, fs::kBShader)) ++tessStages;
    }
    uploads += p.uploads;
    memcpy(before, a.mem, a.used);
    fs::PredictMem(w.pass, w.rs, w.r, p, x);
    g_log.clear();
    const uint64_t r0 = stock(w.pass, 0, &g_ctx, w.rs);
    std::vector<Ev> stockLog = g_log;
    memcpy(afterStock, a.mem, a.used);
    predicted = !x.overflow && fs::CheckMem(x) == -1 && r0 == 0;
    memcpy(a.mem, before, a.used);
    g_log.clear();
    ran = fs::Run(w.pass, &g_ctx, w.rs, w.r, p, sites);
    calls = g_log == stockLog;
    bytes = memcmp(a.mem, afterStock, a.used) == 0;
    if (!calls || !bytes || !predicted || !ran)
      printf("     fx repeat world %d: calls %zu vs %zu equal %d, bytes %d, predicted %d (exp %d), ran %d\n", i,
             stockLog.size(), g_log.size(), calls, bytes, predicted, fs::CheckMem(x), ran);
  }
  char what[300];
  snprintf(what, sizeof(what), "fx repeat: lean Apply = dx11backend's stock Apply on %d random eligible worlds (%d "
           "refused): same calls, same arguments, same order, every byte equal (SRV stages %d cache hits / %d bound, "
           "%d CB uploads, %d HS/DS shaders)", worlds, refused, fastStages, slowStages, uploads, tessStages);
  Check(calls && bytes && ran && worlds > 5000 && fastStages > 1000 && slowStages > 1000 && uploads > 1000, what);
  Check(predicted, "fx repeat: verify prediction (PredictMem) holds after stock Apply on every eligible world");

  VirtualFree(img, 0, MEM_RELEASE);
}

void Run() {
  Pure();
  Real();
}

#undef U

}  // namespace fxstest
