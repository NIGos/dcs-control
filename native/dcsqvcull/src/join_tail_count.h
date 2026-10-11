// Culling-join tail counter (R15 F3, measurement only; no behaviour change).
//
// Scene.dll's collectSceneObjectsRenderables (IView slot 3, our HookCollect)
// runs on a pool thread, splits the objects into up to 16 contiguous chunks
// and launches them through 0x102e0: each chunk is a std::function task
// (_Func_impl_no_alloc<lambda_1> vtable 0x2acc0, or its _Fake_no_copy
// adapter 0x2b1d0; slot 2 = 0x1a4c0: add rcx,8; jmp 0x17400), and the chunks
// the launcher keeps run inline through the call at 0x10547 [V]. The thread
// that called collectRenderablesAggregated (the render thread) waits in
// GraphicsCore 0xbd9b0-0xbda5d and, per iteration, runs one queued task
// through ed::ThreadPool slot 8 (edCore vtable 0x19f3e8, try-run-one 0x4c190,
// returns true if it ran one); return address GraphicsCore+0xbda59 (R6 §4).
//
// While [Suite] JoinTailCount runs (5 s): both task vtables' slot 2, the
// inline call site and ThreadPool slot 8 are wrapped. Every chunk is logged
// (join, thread, start, end, worker chunks running at its start); every
// slot-8 call from the wait loop is logged (start, end, ran a task, a chunk
// inside). Per join (one collect call): the time by which a render-thread
// chunk ends after the last worker chunk ends (F3's tail), how often the
// render thread takes a chunk while pool threads are not running chunks,
// and how late the render thread's last task ends after the collect returns.
// Everything is restored afterwards.
// Included once from main.cpp inside its anonymous namespace, after pacer.h
// (AllocNear), pose_sweep.h (PatchAllSuspended) and shadow_tex.h (CodeIs).
#pragma once

#include <intrin.h>

namespace jointail {

// Scene.dll 2.9.30
constexpr uint32_t kChunkFn = 0x17400;  // chunk lambda body
constexpr uint32_t kChunkEnd = 0x17580;
constexpr uint64_t kChunkHash = 0xe083aeecc69636bcull;
constexpr uint32_t kThunk = 0x1a4c0;  // _Do_call: add rcx, 8; jmp kChunkFn
const uint32_t kTaskVtbls[2] = {0x2acc0, 0x2b1d0};
constexpr uint32_t kInlineSite = 0x10547;  // call kChunkFn in the launcher
// edCore.dll ed::ThreadPool
constexpr uint32_t kPoolVtbl = 0x19f3e8;
constexpr int kTrySlot = 8, kThreadsSlot = 6;
constexpr uint32_t kTryRun = 0x4c190, kTryRunEnd = 0x4c217;
constexpr uint64_t kTryRunHash = 0x9d6d94e1871f957bull;
// GraphicsCore.dll: return address of tq->vt[8] in the collect wait loop.
constexpr uint32_t kSpinRet = 0xbda59;
const uint8_t kSpinCall[5] = {0xFF, 0x50, 0x40, 0xF3, 0x90};  // call [rax+0x40]; pause

using DoCallFn = uint64_t(__fastcall*)(void* self, void* a2);
using ChunkFn = uint64_t(__fastcall*)(void* lambda);
using TryFn = bool(__fastcall*)(void* tq, const char* name);
DoCallFn g_doCall[2] = {};
void** g_taskSlot[2] = {};
ChunkFn g_chunk = nullptr;
TryFn g_try = nullptr;
void** g_trySlot = nullptr;
uint8_t* g_spinRet = nullptr;
uint8_t* g_stub = nullptr;
uint8_t* g_scene = nullptr;
uint8_t g_savedSite[5] = {};
std::atomic<bool> g_sitePatched{false};

std::atomic<bool> g_on{false};
std::atomic<uint32_t> g_join{0};
std::atomic<int> g_workersBusy{0};
std::atomic<DWORD> g_renderTid{0};
std::atomic<uint32_t> g_poolThreads{0};
thread_local int t_inSpin = 0;
thread_local bool t_chunkInSpin = false;

enum Kind : uint8_t { kPool, kInline };
struct ChunkEv {
  uint32_t join;
  DWORD tid;
  int64_t t0, t1;
  uint8_t render, kind;
  int16_t busy;  // worker chunks running when it started (itself excluded)
};
struct SpinEv {
  uint32_t join;
  int64_t t0, t1;
  bool ran, chunk;
};
struct JoinRec {
  int64_t begin = 0, end = 0;
};
constexpr uint32_t kMaxChunks = 1 << 16, kMaxSpins = 1 << 16, kMaxJoins = 4096;
ChunkEv* g_chunks = nullptr;
SpinEv* g_spins = nullptr;
JoinRec* g_joins = nullptr;
std::atomic<uint32_t> g_nChunks{0}, g_nSpins{0};
std::atomic<uint64_t> g_droppedChunks{0}, g_droppedSpins{0};

inline int64_t Qpc() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

// Called by HookCollect around the original collect (culling-top thread).
uint32_t Begin() {
  if (!g_on.load(std::memory_order_relaxed)) return 0;
  const uint32_t id = g_join.fetch_add(1) + 1;
  if (id < kMaxJoins) g_joins[id].begin = Qpc();
  return id;
}
void End(uint32_t id) {
  if (id && id < kMaxJoins) g_joins[id].end = Qpc();
}

template <typename F>
uint64_t RunChunk(uint8_t kind, F&& call) {
  if (!g_on.load(std::memory_order_relaxed)) return call();
  const bool render = t_inSpin > 0;
  if (render) t_chunkInSpin = true;
  const int busy = render ? g_workersBusy.load(std::memory_order_relaxed)
                          : g_workersBusy.fetch_add(1, std::memory_order_relaxed);
  const uint32_t join = g_join.load(std::memory_order_relaxed);
  const int64_t t0 = Qpc();
  const uint64_t r = call();
  const int64_t t1 = Qpc();
  if (!render) g_workersBusy.fetch_sub(1, std::memory_order_relaxed);
  const uint32_t i = g_nChunks.fetch_add(1, std::memory_order_relaxed);
  if (i < kMaxChunks)
    g_chunks[i] = ChunkEv{join, GetCurrentThreadId(), t0, t1, static_cast<uint8_t>(render), kind,
                          static_cast<int16_t>(busy)};
  else
    g_droppedChunks.fetch_add(1, std::memory_order_relaxed);
  return r;
}

template <int I>
uint64_t __fastcall DoCallHook(void* self, void* a2) {
  return RunChunk(kPool, [&] { return g_doCall[I](self, a2); });
}
uint64_t __fastcall InlineHook(void* lambda) {
  return RunChunk(kInline, [&] { return g_chunk(lambda); });
}

bool __fastcall TryHook(void* tq, const char* name) {
  if (!g_on.load(std::memory_order_relaxed) || static_cast<uint8_t*>(_ReturnAddress()) != g_spinRet)
    return g_try(tq, name);
  g_renderTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
  if (!g_poolThreads.load(std::memory_order_relaxed)) {
    __try {
      using ThreadsFn = uint32_t(__fastcall*)(void*);
      g_poolThreads = reinterpret_cast<ThreadsFn>((*static_cast<void***>(tq))[kThreadsSlot])(tq);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  const uint32_t join = g_join.load(std::memory_order_relaxed);
  ++t_inSpin;
  t_chunkInSpin = false;
  const int64_t t0 = Qpc();
  const bool ran = g_try(tq, name);
  const int64_t t1 = Qpc();
  --t_inSpin;
  if (ran) {  // idle polls are only counted
    const uint32_t i = g_nSpins.fetch_add(1, std::memory_order_relaxed);
    if (i < kMaxSpins)
      g_spins[i] = SpinEv{join, t0, t1, ran, t_chunkInSpin};
    else
      g_droppedSpins.fetch_add(1, std::memory_order_relaxed);
  }
  return ran;
}

void* const kDoCallHooks[2] = {reinterpret_cast<void*>(&DoCallHook<0>), reinterpret_cast<void*>(&DoCallHook<1>)};

const char* Check(uint8_t* scene, uint8_t* ed, uint8_t* gc) {
  if (!shadowtex::CodeIs(scene, kChunkFn, kChunkEnd, kChunkHash))
    return "join tail counter: Scene.dll culling chunk differs from the analysed build; skipped";
  uint8_t th[9];
  const uint8_t want[4] = {0x48, 0x83, 0xC1, 0x08};
  int32_t rel;
  if (!allocslab::ReadBytes(scene + kThunk, th, 9) || memcmp(th, want, 4) != 0 || th[4] != 0xE9)
    return "join tail counter: Scene.dll task thunk differs; skipped";
  memcpy(&rel, th + 5, 4);
  if (scene + kThunk + 9 + rel != scene + kChunkFn) return "join tail counter: task thunk target differs; skipped";
  for (uint32_t vt : kTaskVtbls) {
    void** v = reinterpret_cast<void**>(scene + vt);
    char name[512] = {};
    void* col = nullptr;
    uint32_t td = 0;
    if (!allocslab::ReadBytes(v - 1, &col, 8) || !col || !allocslab::ReadBytes(static_cast<uint8_t*>(col) + 12, &td, 4) ||
        !allocslab::ReadBytes(scene + td + 16, name, sizeof(name) - 1) || !strstr(name, "_Func_impl_no_alloc") ||
        !strstr(name, "collectSceneObjectsRenderables") || !strstr(name, "@DCSSceneCollections@@"))
      return "join tail counter: Scene.dll chunk task vtable RTTI differs; skipped";
    if (reinterpret_cast<uint8_t*>(SlotOriginal(&v[2])) != scene + kThunk)
      return "join tail counter: chunk task vtable slot 2 is not the thunk (or hooked); skipped";
  }
  uint8_t b[5];
  if (!allocslab::ReadBytes(scene + kInlineSite, b, 5) || b[0] != 0xE8)
    return "join tail counter: unexpected bytes at the inline chunk call; skipped";
  memcpy(&rel, b + 1, 4);
  if (scene + kInlineSite + 5 + rel != scene + kChunkFn) return "join tail counter: inline call target differs; skipped";
  auto** pv = reinterpret_cast<void**>(ed + kPoolVtbl);
  if (!allocslab::RttiIs(ed, pv, ".?AVThreadPool@ed@@") ||
      reinterpret_cast<uint8_t*>(SlotOriginal(&pv[kTrySlot])) != ed + kTryRun ||
      !shadowtex::CodeIs(ed, kTryRun, kTryRunEnd, kTryRunHash))
    return "join tail counter: edCore ThreadPool differs from the analysed build (or slot 8 is hooked); skipped";
  uint8_t sc[5];
  if (!allocslab::ReadBytes(gc + kSpinRet - 3, sc, 5) || memcmp(sc, kSpinCall, 5) != 0)
    return "join tail counter: GraphicsCore collect wait loop differs; skipped";
  return nullptr;
}

bool PatchSite(bool on) {
  uint8_t* site = g_scene + kInlineSite;
  uint8_t b[5];
  if (on) {
    memcpy(g_savedSite, site, 5);
    b[0] = 0xE8;
    const int32_t rel = static_cast<int32_t>(g_stub - (site + 5));
    memcpy(b + 1, &rel, 4);
  } else {
    memcpy(b, g_savedSite, 5);
  }
  return posesweep::PatchAllSuspended(site, b, 5);
}

void Unhook() {
  if (g_sitePatched.exchange(false) && !PatchSite(false))
    Log("  join tail counter: WARNING could not restore the inline chunk call");
  for (int i = 0; i < 2; ++i)
    if (g_taskSlot[i]) {
      UnhookSlot(g_taskSlot[i], reinterpret_cast<void*>(g_doCall[i]));
      g_taskSlot[i] = nullptr;
    }
  if (g_trySlot) {
    UnhookSlot(g_trySlot, reinterpret_cast<void*>(g_try));
    g_trySlot = nullptr;
  }
}

// Restores everything if a measurement was interrupted (payload stop).
void Shutdown() {
  g_on = false;
  Unhook();
}

double Pct(const std::vector<double>& sorted, double q) {
  if (sorted.empty()) return 0;
  size_t i = static_cast<size_t>(q * (sorted.size() - 1) + 0.5);
  return sorted[i < sorted.size() ? i : sorted.size() - 1];
}

void Measure(int ms, std::atomic<uint64_t>& frames, double qpcToUs) {
  auto* scene = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"Scene.dll"));
  auto* ed = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"edCore.dll"));
  auto* gc = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"GraphicsCore.dll"));
  if (!scene || !ed || !gc) {
    Log("  join tail counter: Scene, edCore or GraphicsCore not loaded");
    return;
  }
  if (const char* why = Check(scene, ed, gc)) {
    Log("  %s", why);
    return;
  }
  // All three or none: a partial failure used to leave g_chunks set, and the
  // next run skipped the allocation and wrote through a null g_joins.
  if (!g_chunks || !g_spins || !g_joins) {
    if (!g_chunks)
      g_chunks = static_cast<ChunkEv*>(VirtualAlloc(nullptr, sizeof(ChunkEv) * kMaxChunks, MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_READWRITE));
    if (!g_spins)
      g_spins = static_cast<SpinEv*>(VirtualAlloc(nullptr, sizeof(SpinEv) * kMaxSpins, MEM_COMMIT | MEM_RESERVE,
                                                  PAGE_READWRITE));
    if (!g_joins)
      g_joins = static_cast<JoinRec*>(VirtualAlloc(nullptr, sizeof(JoinRec) * kMaxJoins, MEM_COMMIT | MEM_RESERVE,
                                                   PAGE_READWRITE));
    if (!g_chunks || !g_spins || !g_joins) {
      Log("  join tail counter: no memory for the event log");
      return;
    }
  }
  g_scene = scene;
  g_chunk = reinterpret_cast<ChunkFn>(scene + kChunkFn);
  g_spinRet = gc + kSpinRet;
  if (!g_stub) {
    uint8_t* page = pacer::AllocNear(scene);
    if (!page) {
      Log("  join tail counter: no memory within reach of Scene.dll");
      return;
    }
    memset(page, 0xCC, 4096);
    page[0] = 0x48;
    page[1] = 0xB8;  // mov rax, imm64
    void* hook = reinterpret_cast<void*>(&InlineHook);
    memcpy(page + 2, &hook, 8);
    page[10] = 0xFF;
    page[11] = 0xE0;  // jmp rax
    DWORD old;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    g_stub = page;
  }
  memset(g_joins, 0, sizeof(JoinRec) * kMaxJoins);
  g_nChunks = g_nSpins = 0;
  g_droppedChunks = g_droppedSpins = 0;
  g_join = 0;
  g_workersBusy = 0;
  g_renderTid = 0;
  g_poolThreads = 0;
  bool ok = true;
  for (int i = 0; i < 2 && ok; ++i) {
    void** slot = &reinterpret_cast<void**>(scene + kTaskVtbls[i])[2];
    if (HookSlot(slot, kDoCallHooks[i], reinterpret_cast<void**>(&g_doCall[i])))
      g_taskSlot[i] = slot;
    else
      ok = false;
  }
  if (ok) {
    void** slot = &reinterpret_cast<void**>(ed + kPoolVtbl)[kTrySlot];
    if (HookSlot(slot, reinterpret_cast<void*>(&TryHook), reinterpret_cast<void**>(&g_try)))
      g_trySlot = slot;
    else
      ok = false;
  }
  if (ok) {
    ok = PatchSite(true);
    if (ok) g_sitePatched = true;
  }
  if (!ok) {
    Unhook();
    Log("  join tail counter: could not install the hooks");
    return;
  }
  Sleep(200);
  const uint64_t f0 = frames.load();
  g_on = true;
  Sleep(ms);
  g_on = false;
  const double f = static_cast<double>(frames.load() - f0);
  Sleep(100);
  Unhook();
  Sleep(50);

  const uint32_t nJoins = std::min<uint32_t>(g_join.load(), kMaxJoins - 1);
  const uint32_t nChunks = std::min<uint32_t>(g_nChunks.load(), kMaxChunks);
  const uint32_t nSpins = std::min<uint32_t>(g_nSpins.load(), kMaxSpins);
  const int pool = static_cast<int>(g_poolThreads.load());
  if (f <= 0 || nJoins < 2 || !nChunks) {
    Log("  join tail counter: no culling joins seen (%.0f frames, %u joins, %u chunks)", f, nJoins, nChunks);
    return;
  }
  // The first and last joins may be cut by the window: use joins 2..n-1.
  struct PerJoin {
    int64_t lastWorker = 0, lastRender = 0, lastSpinTask = 0;
    uint32_t workers = 0, renders = 0, inl = 0, spinTasks = 0, spinOther = 0;
    double spinOtherUs = 0;
  };
  std::vector<PerJoin> pj(nJoins + 2);
  uint64_t renderChunks = 0, renderIdle = 0, renderUs = 0;
  uint64_t busyHist[17] = {};
  double renderChunkUs = 0;
  for (uint32_t i = 0; i < nChunks; ++i) {
    const ChunkEv& e = g_chunks[i];
    if (e.join < 2 || e.join >= nJoins) continue;
    PerJoin& p = pj[e.join];
    if (e.render) {
      p.renders++;
      if (e.t1 > p.lastRender) p.lastRender = e.t1;
      renderChunks++;
      renderChunkUs += (e.t1 - e.t0) * qpcToUs;
      busyHist[e.busy < 0 ? 0 : (e.busy > 16 ? 16 : e.busy)]++;
      if (pool > 0 && e.busy < pool) renderIdle++;
    } else {
      p.workers++;
      if (e.kind == kInline) p.inl++;
      if (e.t1 > p.lastWorker) p.lastWorker = e.t1;
    }
  }
  (void)renderUs;
  for (uint32_t i = 0; i < nSpins; ++i) {
    const SpinEv& s = g_spins[i];
    uint32_t j = s.join;
    // A task taken before this frame's collect began belongs to the next join.
    if (j && j + 1 <= nJoins && g_joins[j].end && s.t0 > g_joins[j].end) ++j;
    if (j < 2 || j >= nJoins) continue;
    PerJoin& p = pj[j];
    p.spinTasks++;
    if (s.t1 > p.lastSpinTask) p.lastSpinTask = s.t1;
    if (!s.chunk) {
      p.spinOther++;
      p.spinOtherUs += (s.t1 - s.t0) * qpcToUs;
    }
  }
  std::vector<double> tails, lates, windows, gaps;
  uint32_t used = 0, withRender = 0, renderLast = 0;
  double chunksW = 0, chunksR = 0, inl = 0, other = 0, otherUs = 0;
  for (uint32_t j = 2; j < nJoins; ++j) {
    const PerJoin& p = pj[j];
    const JoinRec& jr = g_joins[j];
    if (!jr.begin || !jr.end || !p.workers) continue;
    ++used;
    chunksW += p.workers;
    chunksR += p.renders;
    inl += p.inl;
    other += p.spinOther;
    otherUs += p.spinOtherUs;
    double tail = 0;
    if (p.renders) {
      ++withRender;
      if (p.lastRender > p.lastWorker) {
        ++renderLast;
        tail = (p.lastRender - p.lastWorker) * qpcToUs / 1000.0;
      }
    }
    tails.push_back(tail);
    lates.push_back(p.lastSpinTask > jr.end ? (p.lastSpinTask - jr.end) * qpcToUs / 1000.0 : 0.0);
    windows.push_back((jr.end - jr.begin) * qpcToUs / 1000.0);
    gaps.push_back(p.lastWorker < jr.end ? (jr.end - p.lastWorker) * qpcToUs / 1000.0 : 0.0);
  }
  if (!used) {
    Log("  join tail counter: no complete joins (%u joins, %u chunks)", nJoins, nChunks);
    return;
  }
  auto mean = [](const std::vector<double>& v) {
    double s = 0;
    for (double x : v) s += x;
    return v.empty() ? 0.0 : s / v.size();
  };
  auto sorted = [](std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v;
  };
  const std::vector<double> st = sorted(tails), sl = sorted(lates), sw = sorted(windows), sg = sorted(gaps);
  const double u = static_cast<double>(used);
  Log("  join tail: %.0f frames, %u complete joins (%.2f per frame); render thread %lu, ThreadPool threads %d; "
      "dropped events %llu chunks, %llu tasks",
      f, used, (nJoins - 2) / f, static_cast<unsigned long>(g_renderTid.load()), pool,
      static_cast<unsigned long long>(g_droppedChunks.load()), static_cast<unsigned long long>(g_droppedSpins.load()));
  Log("  join tail: chunks per join %.2f on workers (%.2f of them inline in the launcher), %.2f on the render thread "
      "(in %.1f%% of joins); render-thread chunk mean %.3f ms",
      chunksW / u, inl / u, chunksR / u, 100.0 * withRender / u, renderChunks ? renderChunkUs / 1000.0 / renderChunks : 0.0);
  {
    char line[512];
    int n = snprintf(line, sizeof(line), "  join tail: worker chunks running when the render thread took one:");
    for (int k = 0; k <= 16 && n < static_cast<int>(sizeof(line)) - 24; ++k)
      if (busyHist[k]) n += snprintf(line + n, sizeof(line) - n, " %d:%.1f%%", k, 100.0 * busyHist[k] / renderChunks);
    Log("%s", renderChunks ? line : "  join tail: the render thread took no chunk");
  }
  Log("  join tail: render thread took a chunk while at least one pool thread ran none: %.1f%% of its chunks "
      "(%.2f per frame)",
      renderChunks ? 100.0 * renderIdle / renderChunks : 0.0, renderIdle / f);
  Log("  join tail: render chunk ends after the last worker chunk in %.1f%% of joins; tail per join mean %.3f ms, "
      "p50 %.3f, p95 %.3f, max %.3f ms; per frame %.3f ms (R15 F3 gate mean >= 0.2 ms: %s)",
      100.0 * renderLast / u, mean(tails), Pct(st, 0.5), Pct(st, 0.95), st.back(), mean(tails) * used / f,
      mean(tails) >= 0.2 ? "PASS" : "below");
  Log("  join tail: render thread's last pool task ends after the collect returned: mean %.3f ms, p50 %.3f, p95 %.3f "
      "ms; non-chunk tasks it ran in the wait loop %.2f per join, %.3f ms per join",
      mean(lates), Pct(sl, 0.5), Pct(sl, 0.95), other / u, otherUs / 1000.0 / u);
  Log("  join tail: collect window mean %.3f ms (p95 %.3f); last worker chunk end to collect return mean %.3f ms "
      "(p95 %.3f)",
      mean(windows), Pct(sw, 0.95), mean(gaps), Pct(sg, 0.95));
}

}  // namespace jointail
