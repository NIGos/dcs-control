// Forward pass split (fable-perf-audit A5 / B8, measurement only): the
// render-thread time and draws of DCS's SimplePassData passes (forward,
// transparent, cockpit transparent, flat shadows, mirrors; R21 §1), per frame
// and per view, split into glass / forward terrain / flat shadows / models /
// other, while flying (F1 or F2). [Suite] ForwardSplit = seconds between two
// "fwd split:" lines (default 0: nothing is installed, production unchanged).
// Nothing is drawn differently and no DCS memory is written.
//
// What R21's one-shot S0 counter (fwd_rec_count.h) left open: it ran in F2
// only (yaw 270: GlassMaterialMT 1.12 ms / 1,600 draws, TerrainObject 0.96 ms,
// flat_shadow 0.56 ms, NOTES:104), never in the cockpit view, where the canopy
// and cockpit glass are drawn in TransparentCockpit*Pass (R23 §6, N3/N7).
//
// Categories (per item call of a pass, render thread):
//  glass    SceneRenderable whose material's RTTI names GlassMaterial
//           (GlassMaterialMT: ONE/SRC1_COLOR dual-source blend, t103/t104);
//  terrain  renderables whose class RTTI names TerrainObject (vt[1] hooked
//           once the class is seen in a pass; untimed when another module
//           holds that slot: the line says so);
//  flat     ModelMaterialMT items with model pass [r+0x60] = 8 (flat_shadow);
//  models   every other ModelMaterialMT item;
//  other    the rest of the pass: other materials and renderables (particles,
//           effects), the frame-buffer push, clears, the binder, the pop.
//           Its ms = execute - the four timed categories; its draws = items
//           in the vector - the timed items.
// View: the occurrence of the pass kind (name suffix, fwd_rec_count.h) since
// the render graph's RenderGraph::render entry (one entry per frame for the
// main graph; the line prints entries/frame to check it). v0..v3 follow the
// HMD view order; "x" = a 5th occurrence and every other render graph (MFD,
// map). The main graph is the one with the most forward time in the window.
// Cost: two rdtsc and a few loads per item call inside these passes (about
// 9k/frame at the airfield): ~0.1-0.2 ms/frame on the render thread, included
// in the numbers (no probe subtraction).
//
// Hooks (all through the loader's slot registry, taken out at payload stop):
// SimplePassData slot 19 (GraphicsCore 0xd9fc0, the build checked as
// fwd_rec_count.h; skipped while ForwardRecCount's phase owns it and the
// other way round), gbcount's SceneRenderable override chain (in front,
// forwarding to the previous override), RenderGraph::render's observer
// (shadow_batch.h; its import hook installed if no one else did), and
// TerrainObject vt[1] per class.
//
// ---- Step 2 of A5: glass-only recorder, decision (not built) ----
// No glass number exists for the cockpit view. The one measured number
// (1.12 ms) is F2 at the airfield ramp (parked aircraft canopies, about
// 0.7 us/draw), at a yaw R23 calls co-limited (<= 5 % fps). Whether F1 has
// >= 0.5 ms of glass in a render-thread-bound frame is not known (own canopy
// and instrument glass are tens of draws per view; the ramp glass behind the
// cockpit may or may not survive), so it is measured first. Rule, on the
// lines of this counter plus the perf: line of the same seconds:
//   BUILD the glass recorder only if, in F1 (qvbench_cockpit or
//   qvbench_f16_takeoff) or at F2 yaw 270, glass >= 0.5 ms/frame in at least
//   3 of 4 consecutive lines AND the perf: line's verdict is CPU (render
//   thread busy, not GPU-bound) in those seconds. Else park it (A5 closed as
//   "below the gate"). Look at "ck" (glass in cockpit passes) to see which
//   collection would be recorded first.
// Plan if built: reuse gb_rec.h's scaffolding (key compile, job snapshot at
// the render entry, worker record, island rule, residual items in place):
//   G1 S2-style dry run behind [Model] GlassRecorder=0: glass items only of
//      TransparentPass / TransparentCockpitPass, segment = a run of glass
//      items between non-glass items (blended order kept exactly: the
//      recorded list replaces only that run, at its own position; no
//      reordering, no instancing); log runs/call and run lengths. Gate:
//      median run >= 8 draws, else Execute cost (16 us) eats the gain;
//   G2 capture per run: OM state including the dual-source blend, t103
//      (CockpitRefraction) and t104 (RainDroplets) bound at the run start,
//      material CB window; the run stays DCS's when any differs from the
//      snapshot (the stock fallback);
//   G3 same-frame verify as the other recorders ([Suite] GlassRecVerify,
//      GBufferRecVerify's method): draw stock into the target, copy, draw the
//      recorded run, compare bit-exact; latch off on any difference;
//   G4 bench A/B 24 blocks at the render-thread-bound scene; default off.
// Included once from main.cpp inside its anonymous namespace, after
// fwd_rec_count.h.
#pragma once

namespace fwdsplit {

using fwdreccount::kKinds;
enum Cat : int { kGlass = 0, kTerrain, kFlat, kModel, kOther, kCats };
const char* const kCatName[kCats] = {"glass", "terrain", "flat", "models", "other"};
constexpr int kViews = 4;   // v0..v3; index kViews = "x"
constexpr int kGraphs = 4;  // render graphs tracked; index kGraphs = not tracked
constexpr int kMaxNs = 8;   // TerrainObject classes timed
constexpr int kMaxCand = 64;
constexpr int kMatCache = 32;

// ---------------------------------------------------------------------------
// Pure helpers (offline tested)
// ---------------------------------------------------------------------------
inline int MatCatOfName(const char* rtti) { return rtti && strstr(rtti, "GlassMaterial") ? kGlass : kOther; }
inline bool IsTerrainName(const char* rtti) { return rtti && strstr(rtti, "TerrainObject") != nullptr; }
inline int ModelCat(uint32_t pass) { return pass == 8 ? kFlat : kModel; }
inline int ViewOf(uint32_t occurrence) { return occurrence < kViews ? static_cast<int>(occurrence) : kViews; }
inline bool CockpitKind(int kind) {
  return kind == fwdreccount::kTransparentCockpit || kind == fwdreccount::kTransparentCockpitDecal;
}

struct PCell {
  uint64_t calls, cyc[kCats], items[kCats];
};
struct Snap {
  PCell c[kGraphs + 1][kKinds][kViews + 1];
  uint64_t entries[kGraphs + 1];
  uint32_t gen[kGraphs + 1];
};
inline void Diff(const Snap& now, const Snap& prev, Snap& d) {
  for (int g = 0; g <= kGraphs; ++g) {
    const bool same = now.gen[g] == prev.gen[g];  // a replaced graph starts from 0
    d.gen[g] = now.gen[g];
    d.entries[g] = now.entries[g] - (same ? prev.entries[g] : 0);
    for (int k = 0; k < kKinds; ++k)
      for (int v = 0; v <= kViews; ++v) {
        const PCell& a = now.c[g][k][v];
        const PCell& b = prev.c[g][k][v];
        PCell& o = d.c[g][k][v];
        o.calls = a.calls - (same ? b.calls : 0);
        for (int i = 0; i < kCats; ++i) {
          o.cyc[i] = a.cyc[i] - (same ? b.cyc[i] : 0);
          o.items[i] = a.items[i] - (same ? b.items[i] : 0);
        }
      }
  }
}

struct Sum {
  uint64_t calls = 0, cyc[kCats] = {}, items[kCats] = {};
  void Add(const PCell& p) {
    calls += p.calls;
    for (int i = 0; i < kCats; ++i) {
      cyc[i] += p.cyc[i];
      items[i] += p.items[i];
    }
  }
  uint64_t Cyc() const {
    uint64_t s = 0;
    for (uint64_t x : cyc) s += x;
    return s;
  }
  uint64_t Items() const {
    uint64_t s = 0;
    for (uint64_t x : items) s += x;
    return s;
  }
};
struct Agg {
  Sum view[kViews + 1], total, cockpit;  // cockpit = TransparentCockpit* passes, every view
  int mainGraph = -1;
  uint64_t mainEntries = 0;
};
// Main graph = the most forward cycles; its views by occurrence; everything
// else into "x".
inline Agg Aggregate(const Snap& d) {
  Agg a;
  uint64_t best = 0;
  for (int g = 0; g < kGraphs; ++g) {
    Sum s;
    for (int k = 0; k < kKinds; ++k)
      for (int v = 0; v <= kViews; ++v) s.Add(d.c[g][k][v]);
    if (s.Cyc() > best) {
      best = s.Cyc();
      a.mainGraph = g;
    }
  }
  if (a.mainGraph >= 0) a.mainEntries = d.entries[a.mainGraph];
  for (int g = 0; g <= kGraphs; ++g)
    for (int k = 0; k < kKinds; ++k)
      for (int v = 0; v <= kViews; ++v) {
        const PCell& p = d.c[g][k][v];
        a.view[g == a.mainGraph ? v : kViews].Add(p);
        a.total.Add(p);
        if (CockpitKind(k)) a.cockpit.Add(p);
      }
  return a;
}

// The one greppable line ("fwd split:").
inline std::string FormatLine(const Agg& a, double secs, double frames, double cycPerMs, int terrainTimed,
                              int terrainUntimed, bool viewsOk) {
  const double f = frames > 0 ? frames : 1, c = cycPerMs > 0 ? cycPerMs : 1;
  auto ms = [&](uint64_t cyc) { return cyc / c / f; };
  auto n = [&](uint64_t items) { return items / f; };
  char buf[512];
  std::string s;
  snprintf(buf, sizeof(buf), "fwd split: %.0f s, %.0f frames, %.1f calls/frame, main graph entries/frame %.2f%s |",
           secs, frames, n(a.total.calls), a.mainEntries / f, viewsOk ? "" : " (no render entry: views not split)");
  s += buf;
  for (int i = 0; i < kCats; ++i) {
    snprintf(buf, sizeof(buf), " %s %.3f ms %.0f", kCatName[i], ms(a.total.cyc[i]), n(a.total.items[i]));
    s += buf;
  }
  snprintf(buf, sizeof(buf), " total %.3f ms %.0f | ck glass %.3f ms %.0f, ck all %.3f ms %.0f |", ms(a.total.Cyc()),
           n(a.total.Items()), ms(a.cockpit.cyc[kGlass]), n(a.cockpit.items[kGlass]), ms(a.cockpit.Cyc()),
           n(a.cockpit.Items()));
  s += buf;
  for (int v = 0; v <= kViews; ++v) {
    const Sum& w = a.view[v];
    if (v < kViews) snprintf(buf, sizeof(buf), " v%d", v);
    else snprintf(buf, sizeof(buf), " x");
    s += buf;
    snprintf(buf, sizeof(buf), " %.3f/%.3f/%.3f/%.3f/%.3f n %.0f/%.0f/%.0f/%.0f/%.0f", ms(w.cyc[kGlass]),
             ms(w.cyc[kTerrain]), ms(w.cyc[kFlat]), ms(w.cyc[kModel]), ms(w.cyc[kOther]), n(w.items[kGlass]),
             n(w.items[kTerrain]), n(w.items[kFlat]), n(w.items[kModel]), n(w.items[kOther]));
    s += buf;
  }
  snprintf(buf, sizeof(buf), " (ms/frame g/t/f/m/o, draws/frame) | terrain classes timed %d untimed %d", terrainTimed,
           terrainUntimed);
  s += buf;
  return s;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
using ExecFn = void(__fastcall*)(void* pass, void* ctx);
using VtFn = uint64_t(__fastcall*)(void*, void*, void*, void*);
ExecFn g_orig = nullptr;
void** g_slot19 = nullptr;
std::atomic<bool> g_on{false};
std::atomic<int> g_inside{0};
std::atomic<DWORD> g_rt{0};
int g_state = 0;  // 0 = not tried, 1 = installed, -1 = given up, 2 = shut down
ULONGLONG g_nextTry = 0;
void* g_srVt = nullptr;
void* g_modelMatVt = nullptr;
gbcount::OverrideFn g_prevOverride = nullptr;
shadowbatch::RenderObserverFn g_prevObserver = nullptr;
bool g_ownRenderHook = false, g_viewsOk = false;

inline void Add(std::atomic<uint64_t>& a, uint64_t d) {
  a.store(a.load(std::memory_order_relaxed) + d, std::memory_order_relaxed);  // one writer: the render thread
}
struct Cell {
  std::atomic<uint64_t> calls{0}, cyc[kCats], items[kCats];
};
struct Graph {
  std::atomic<void*> rg{nullptr};
  std::atomic<uint32_t> gen{0};
  std::atomic<uint64_t> entries{0};
  uint64_t lastEntry = 0;  // rdtsc, render thread
  uint32_t occ[kKinds] = {};
  Cell cell[kKinds][kViews + 1];
};
Graph g_graph[kGraphs + 1];  // [kGraphs]: calls outside the tracked graphs
uint64_t g_staleCyc = 0;      // a graph without a render entry for this long can be replaced

// Material classes -> category (render thread).
struct MatCache {
  void* vt;
  int cat;
};
MatCache g_mat[kMatCache];
int g_matN = 0;

// Non-SceneRenderable classes seen in a pass (render thread writes, worker hooks).
std::atomic<void*> g_cand[kMaxCand];
std::atomic<int> g_candN{0};
int g_candDone = 0;
struct NsClass {
  void** slot;
  int state;  // 1 = hooked, -1 = slot held by another module
};
NsClass g_ns[kMaxNs];
void* g_nsOrig[kMaxNs] = {};
int g_nsN = 0, g_terrTimed = 0, g_terrUntimed = 0;

thread_local bool t_inPass = false;
thread_local int t_depth = 0;
thread_local uint64_t t_cyc[kCats];
thread_local uint64_t t_items[kCats];
thread_local uint32_t t_scan = 0;

// ---------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------
int MatCat(void* vt) {
  for (int i = 0; i < g_matN; ++i)
    if (g_mat[i].vt == vt) return g_mat[i].cat;
  char rtti[128];
  gbreccount::VtName(vt, rtti, sizeof(rtti));
  const int c = MatCatOfName(rtti);
  if (g_matN < kMatCache) g_mat[g_matN++] = {vt, c};
  return c;
}

int SrCatRaw(void* self) {
  __try {
    auto* r = static_cast<uint8_t*>(self);
    auto* item = *reinterpret_cast<uint8_t**>(r + 0x10);
    auto* mat = item ? *reinterpret_cast<uint8_t**>(item + 0x10) : nullptr;
    if (!mat) return kOther;
    void* vt = *reinterpret_cast<void**>(mat);
    if (vt == g_modelMatVt) return ModelCat(*reinterpret_cast<uint32_t*>(r + 0x60));
    return MatCat(vt);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return kOther;
  }
}

bool Override(void* self, void* ctx, uint64_t* ret) {
  if (!t_inPass || t_depth) {
    if (gbcount::OverrideFn p = g_prevOverride) return p(self, ctx, ret);
    return false;
  }
  const int c = SrCatRaw(self);
  ++t_depth;
  const uint64_t t0 = __rdtsc();
  uint64_t r = 0;
  bool done = false;
  if (gbcount::OverrideFn p = g_prevOverride) done = p(self, ctx, &r);
  if (!done) r = gbcount::g_orig(self, ctx);
  const uint64_t t1 = __rdtsc();
  --t_depth;
  if (c != kOther) {
    t_cyc[c] += t1 - t0;
    t_items[c]++;
  }
  *ret = r;
  return true;
}

template <int I>
uint64_t __fastcall NsHook(void* a, void* b, void* c, void* d) {
  auto orig = reinterpret_cast<VtFn>(g_nsOrig[I]);
  if (!t_inPass || t_depth) return orig(a, b, c, d);
  ++t_depth;
  const uint64_t t0 = __rdtsc();
  const uint64_t r = orig(a, b, c, d);
  const uint64_t t1 = __rdtsc();
  --t_depth;
  t_cyc[kTerrain] += t1 - t0;
  t_items[kTerrain]++;
  return r;
}
static_assert(kMaxNs == 8, "kNsHooks lists one thunk per class");
void* const kNsHooks[kMaxNs] = {
    reinterpret_cast<void*>(&NsHook<0>), reinterpret_cast<void*>(&NsHook<1>), reinterpret_cast<void*>(&NsHook<2>),
    reinterpret_cast<void*>(&NsHook<3>), reinterpret_cast<void*>(&NsHook<4>), reinterpret_cast<void*>(&NsHook<5>),
    reinterpret_cast<void*>(&NsHook<6>), reinterpret_cast<void*>(&NsHook<7>)};

// Items in the pass's vector (0 for the static empty one); new non-SceneRenderable
// classes go to the candidates when scan is set.
size_t ItemsRaw(void** vec, bool scan) {
  __try {
    auto* b = static_cast<void* const*>(vec[0]);
    auto* e = static_cast<void* const*>(vec[1]);
    if (!b || e < b || e - b > 200000) return 0;
    const size_t n = static_cast<size_t>(e - b);
    if (scan) {
      void* last = g_srVt;
      for (size_t k = 0; k < n; ++k) {
        void* vt = b[k] ? *static_cast<void**>(b[k]) : nullptr;
        if (!vt || vt == g_srVt || vt == last) continue;
        last = vt;
        const int m = g_candN.load(std::memory_order_relaxed);
        bool known = false;
        for (int i = 0; i < m && !known; ++i) known = g_cand[i].load(std::memory_order_relaxed) == vt;
        if (!known && m < kMaxCand) {
          g_cand[m].store(vt, std::memory_order_relaxed);
          g_candN.store(m + 1, std::memory_order_release);
        }
      }
    }
    return n;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

void* GraphOfRaw(void* ctx) {
  __try {
    return *static_cast<void**>(ctx);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

int FindGraph(void* rg) {
  for (int g = 0; g < kGraphs; ++g)
    if (g_graph[g].rg.load(std::memory_order_relaxed) == rg) return g;
  return -1;
}

// A new graph takes an empty entry or the one without a render entry for the
// longest time (when stale); its counters restart under a new generation.
int TakeGraph(void* rg, uint64_t now) {
  int pick = -1;
  uint64_t oldest = ~0ull;
  for (int g = 0; g < kGraphs; ++g) {
    if (!g_graph[g].rg.load(std::memory_order_relaxed)) {
      pick = g;
      break;
    }
    if (g_graph[g].lastEntry < oldest) {
      oldest = g_graph[g].lastEntry;
      pick = g;
    }
  }
  if (pick < 0) return -1;
  Graph& G = g_graph[pick];
  if (G.rg.load(std::memory_order_relaxed) && now - G.lastEntry < g_staleCyc) return -1;
  for (auto& row : G.cell)
    for (Cell& c : row) {
      c.calls.store(0, std::memory_order_relaxed);
      for (int i = 0; i < kCats; ++i) {
        c.cyc[i].store(0, std::memory_order_relaxed);
        c.items[i].store(0, std::memory_order_relaxed);
      }
    }
  G.entries.store(0, std::memory_order_relaxed);
  memset(G.occ, 0, sizeof(G.occ));
  G.lastEntry = now;
  G.rg.store(rg, std::memory_order_relaxed);
  G.gen.store(G.gen.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  return pick;
}

void OnRender(void* rg, void* renderables) {
  if (shadowbatch::RenderObserverFn p = g_prevObserver) p(rg, renderables);
  if (!g_on.load(std::memory_order_relaxed)) return;
  const DWORD rt = g_rt.load(std::memory_order_relaxed);
  if (!rt || GetCurrentThreadId() != rt) return;
  const uint64_t now = __rdtsc();
  int g = FindGraph(rg);
  if (g < 0) g = TakeGraph(rg, now);
  if (g < 0) return;
  Graph& G = g_graph[g];
  memset(G.occ, 0, sizeof(G.occ));
  G.lastEntry = now;
  Add(G.entries, 1);
}

void __fastcall Hook(void* pass, void* ctx) {
  if (!g_on.load(std::memory_order_relaxed) || t_inPass) {
    g_orig(pass, ctx);
    return;
  }
  const DWORD tid = GetCurrentThreadId();
  DWORD rt = g_rt.load(std::memory_order_relaxed);
  if (!rt) {
    g_rt.store(tid);  // only the render thread runs these passes
    rt = tid;
  }
  if (tid != rt) {
    g_orig(pass, ctx);
    return;
  }
  g_inside.fetch_add(1);
  char name[40];
  fwdreccount::PassNameRaw(pass, name);
  const int kind = fwdreccount::KindOfName(name);
  void* rg = GraphOfRaw(ctx);
  const uint64_t tNow = __rdtsc();
  int g = rg ? FindGraph(rg) : -1;
  if (g < 0 && rg) g = TakeGraph(rg, tNow);
  int view = kViews;
  if (g >= 0) view = ViewOf(g_graph[g].occ[kind]++);
  else g = kGraphs;
  void** vec = fwdreccount::ItemVector(pass, ctx);
  const bool scan = (t_scan++ % 512) < 64 && g_candN.load(std::memory_order_relaxed) < kMaxCand;
  const size_t n = vec ? ItemsRaw(vec, scan) : 0;
  for (int i = 0; i < kCats; ++i) t_cyc[i] = t_items[i] = 0;
  t_inPass = true;
  const uint64_t a = __rdtsc();
  g_orig(pass, ctx);
  const uint64_t b = __rdtsc();
  t_inPass = false;
  uint64_t timed = 0, counted = 0;
  for (int i = 0; i < kOther; ++i) {
    timed += t_cyc[i];
    counted += t_items[i];
  }
  t_cyc[kOther] = b - a > timed ? b - a - timed : 0;
  t_items[kOther] = n > counted ? n - counted : 0;
  Cell& c = g_graph[g].cell[kind][view];
  Add(c.calls, 1);
  for (int i = 0; i < kCats; ++i) {
    Add(c.cyc[i], t_cyc[i]);
    Add(c.items[i], t_items[i]);
  }
  g_inside.fetch_sub(1);
}

// ---------------------------------------------------------------------------
// Worker thread: install, class hooks, report
// ---------------------------------------------------------------------------
void ReleaseRenderHook() {
  if (g_ownRenderHook && shadowbatch::g_state.load() != 1) {
    if (shadowbatch::g_renderSlot) UnhookSlot(shadowbatch::g_renderSlot, reinterpret_cast<void*>(shadowbatch::g_origRender));
    if (shadowbatch::g_renderSlot2) UnhookSlot(shadowbatch::g_renderSlot2, reinterpret_cast<void*>(shadowbatch::g_origRender));
    shadowbatch::g_renderSlot = nullptr;
    shadowbatch::g_renderSlot2 = nullptr;
  }
  g_ownRenderHook = false;
}

// 1 = installed, 0 = try again later, -1 = give up.
int Install(double tscHz) {
  auto* gc = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"GraphicsCore.dll"));
  auto* ng = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  if (!gc || !ng || tscHz <= 0) return 0;
  if (fwdreccount::g_chained.load()) return 0;  // its suite phase holds slot 19
  if (const char* why = fwdreccount::VerifyBuild(gc)) {
    Log("fwd split: %s; off", why);
    return -1;
  }
  if (!gbcount::Install()) {
    Log("fwd split: SceneRenderable hook unavailable (this build); off");
    return -1;
  }
  g_srVt = ng + gbcount::kVtableRva;
  g_modelMatVt = gbcount::g_modelMatVtbl;
  g_staleCyc = static_cast<uint64_t>(tscHz * 2.0);
  g_ownRenderHook = false;
  if (!shadowbatch::g_renderSlot && !shadowbatch::g_renderSlot2) g_ownRenderHook = shadowbatch::InstallRenderHook();
  g_viewsOk = shadowbatch::g_renderSlot || shadowbatch::g_renderSlot2;
  g_slot19 = &reinterpret_cast<void**>(gc + fwdreccount::kVtableRva)[fwdreccount::kExecSlot];
  g_orig = reinterpret_cast<ExecFn>(SlotOriginal(g_slot19));
  if (!HookSlot(g_slot19, reinterpret_cast<void*>(&Hook), nullptr)) {
    g_slot19 = nullptr;
    ReleaseRenderHook();
    Log("fwd split: SimplePassData execute slot not hooked; off");
    return -1;
  }
  g_prevObserver = shadowbatch::g_renderObserver.load();
  while (!shadowbatch::g_renderObserver.compare_exchange_weak(g_prevObserver, &OnRender)) {
  }
  g_prevOverride = gbcount::g_override.load();
  while (!gbcount::g_override.compare_exchange_weak(g_prevOverride, &Override)) {
  }
  g_on = true;
  Log("fwd split: on (SimplePassData execute, SceneRenderable override, render entry %s); one line per interval",
      g_viewsOk ? "observed" : "NOT hooked: views not split");
  return 1;
}

void HookCandidates() {
  const int m = g_candN.load(std::memory_order_acquire);
  for (; g_candDone < m; ++g_candDone) {
    void* vt = g_cand[g_candDone].load(std::memory_order_relaxed);
    char rtti[160];
    gbreccount::VtName(vt, rtti, sizeof(rtti));
    if (!IsTerrainName(rtti)) continue;
    void** slot = static_cast<void**>(vt) + 1;
    if (g_nsN >= kMaxNs) {
      ++g_terrUntimed;
      Log("fwd split: %s untimed (class table full)", rtti);
      continue;
    }
    if (*slot != SlotOriginal(slot)) {
      ++g_terrUntimed;
      Log("fwd split: %s untimed (vt[1] hooked by another module)", rtti);
      continue;
    }
    const int i = g_nsN;
    g_nsOrig[i] = *slot;
    if (!HookSlot(slot, kNsHooks[i], nullptr)) {
      ++g_terrUntimed;
      continue;
    }
    g_ns[i] = {slot, 1};
    g_nsN = i + 1;
    ++g_terrTimed;
    Log("fwd split: timing %s (vt[1])", rtti);
  }
}

void TakeSnap(Snap& s) {
  for (int g = 0; g <= kGraphs; ++g) {
    const Graph& G = g_graph[g];
    s.gen[g] = G.gen.load(std::memory_order_acquire);
    s.entries[g] = G.entries.load(std::memory_order_relaxed);
    for (int k = 0; k < kKinds; ++k)
      for (int v = 0; v <= kViews; ++v) {
        const Cell& c = G.cell[k][v];
        PCell& p = s.c[g][k][v];
        p.calls = c.calls.load(std::memory_order_relaxed);
        for (int i = 0; i < kCats; ++i) {
          p.cyc[i] = c.cyc[i].load(std::memory_order_relaxed);
          p.items[i] = c.items[i].load(std::memory_order_relaxed);
        }
      }
    // A replacement during the read: count this graph from 0 next time.
    if (G.gen.load(std::memory_order_acquire) != s.gen[g]) s.gen[g] = ~0u;
  }
}

Snap g_prev, g_cur, g_delta;
ULONGLONG g_last = 0;
uint64_t g_lastFrames = 0;

// Worker loop, about every 100 ms while [Suite] ForwardSplit > 0.
void Tick(ULONGLONG now, int intervalSec, uint64_t frames, double tscHz) {
  if (g_state == -1 || g_state == 2) return;
  if (g_state == 0) {
    if (now < g_nextTry) return;
    g_nextTry = now + 5000;
    const int r = Install(tscHz);
    if (r < 0) g_state = -1;
    if (r <= 0) return;
    g_state = 1;
    memset(&g_prev, 0, sizeof(g_prev));
    TakeSnap(g_prev);
    g_last = now;
    g_lastFrames = frames;
    return;
  }
  HookCandidates();
  if (now - g_last < static_cast<ULONGLONG>(intervalSec) * 1000) return;
  const double secs = (now - g_last) / 1000.0;
  const double f = static_cast<double>(frames - g_lastFrames);
  g_last = now;
  g_lastFrames = frames;
  TakeSnap(g_cur);
  Diff(g_cur, g_prev, g_delta);
  g_prev = g_cur;
  if (f <= 0) return;  // menu, loading or paused
  const Agg a = Aggregate(g_delta);
  if (!a.total.calls) return;
  Log("%s", FormatLine(a, secs, f, tscHz / 1000.0, g_terrTimed, g_terrUntimed, g_viewsOk).c_str());
}

// Payload stop.
void Shutdown() {
  if (g_state != 1) return;
  g_state = 2;
  g_on = false;
  if (g_slot19) UnhookSlot(g_slot19, reinterpret_cast<void*>(g_orig));  // g_orig stays for calls in flight
  g_slot19 = nullptr;
  gbcount::OverrideFn ov = &Override;
  gbcount::g_override.compare_exchange_strong(ov, g_prevOverride);  // else it stays chained, pass-through
  shadowbatch::RenderObserverFn ob = &OnRender;
  shadowbatch::g_renderObserver.compare_exchange_strong(ob, g_prevObserver);
  for (int i = 0; i < g_nsN; ++i)
    if (g_ns[i].state == 1) UnhookSlot(g_ns[i].slot, g_nsOrig[i]);
  g_nsN = 0;
  ReleaseRenderHook();
  for (int i = 0; i < 400 && g_inside.load() != 0; ++i) Sleep(5);
  if (g_inside.load() != 0) Log("fwd split: a pass was still running at unload");
}

}  // namespace fwdsplit
