// R39: thread census and worker placement ([Threads] Census, CensusSec,
// WorkerPlacement; Census default 0; WorkerPlacement default 2 since R44).
//
// Why. DCS's own thread manager (edCore DCSThreadManager, Config/mt.lua keys
// multithreading.*) defaults to manageThreadPriority, manageThreadAffinity,
// manageSysThreadAffinity and exclusiveMainRenderCore all on [V: edCore
// 0x6bd48-0x6be06, defaults at 0x1faa60-0x1faa63 = 1,1,1,1]. dcs.log on this
// PC: render cores {4,5,6,7,8,9,2,3}, common cores {10,11,0,1,12,13,14,15}.
// With exclusiveMainRenderCore the render pool's mask is the render cores
// minus the main render core (edCore 0x716c3: not rcx; and rcx, rax), so DCS
// keeps one physical core for its render thread. Our payload's threads (16
// shadow recorder + 12 G-buffer recorder workers, 2 planners, 2 compile
// workers, the timer-cache updater, the payload worker) are created with the
// process mask and can run on that core, beside or instead of the render
// thread [V: no affinity call in deferred_rec.h / shadow_batch.h /
// shadow_inst.h]. Whether that costs render-thread time is not measured.
//
// Census (measure only, worker thread, every CensusSec, default 60 s):
//   one NtQuerySystemInformation snapshot (run_threads.h's walker) for each
//   DCS thread's CPU time over the interval, then for the threads above 1 %
//   of a core (and always the render thread and ours in total): affinity mask
//   and priority (NtQueryInformationThread ThreadBasicInformation), ideal
//   processor, start address as module+RVA (ThreadQuerySetWin32StartAddress).
//   Plus one summary: the render thread's mask, its physical core(s), how much
//   CPU other DCS threads and ours spent with that core in their mask.
//   No thread is changed by the census.
//
// WorkerPlacement=1 (behaviour, scheduling only; the frames are unchanged):
//   when DCS pinned its render thread (its mask is a strict subset of the
//   process mask covering at most 2 physical cores), every thread that
//   started in our payload or loader gets the process mask minus those
//   physical cores (both SMT siblings). Previous masks are restored when the
//   key goes back to 0, the kill switch is on, or the payload stops. Nothing
//   happens (one log line) when DCS does not pin the render thread, or fewer
//   than 4 logical processors would remain. DCS's threads are never touched.
//
// WorkerPlacement=2 (R44, selective; scheduling only, frames unchanged):
//   R41 section 9 measured 1 slower than 0 in the CPU-bound airfield (-1.5 fps,
//   p95 +1.6 ms): moving everything also moved the threads the render thread
//   waits for, and those lost the render thread's idle SMT sibling. Mode 2
//   splits our threads by role, from their start address (each CreateThread
//   body has its own qvexit::Thread<Body> entry; RenderPathStarts):
//     render path (the render thread waits for them; they get the full
//     process mask, so DCS's racy one-time affinity pass cannot leave them
//     off the render core either):
//       - defrec::Pool workers: shadow recorder and G-buffer recorder record
//         and replay jobs (shadow_rec.h / gb_rec.h; the render thread spins,
//         then blocks on their done events);
//       - shadowbatch planners ([Model] ShadowPlanAsync): the render thread
//         waits for a plan still in kBuilding (shadow_batch.h);
//       - parupload helpers: the render thread waits for their chunks.
//     everything else (moved off the render core as in mode 1): payload
//     worker loop (census, perf_mon lines, config tick, placement itself),
//     suite / profile threads, timer-cache updater, shadow_inst compile
//     workers, d3dstate refresher, the loader's watcher, and every
//     std::thread (fixation_depth publisher worker, cockpit dump writer,
//     gpu_split measurement threads; all share the CRT's thread_start).
//   Switching between 1 and 2 at run time re-targets the affected threads;
//   0 puts every saved mask back, as before.
//   R44 live (airfield, CPU-bound): with 1 or 2 the payload worker went from
//   0.7 % to 8.5 % of a core (the 1 s placement pass: system-wide thread
//   snapshot plus an open/query of every DCS thread) and p95 rose 1-2 ms in
//   both modes, even where mode 2's masks equalled mode 0's. Since then start
//   addresses are cached per thread id and passes back off to 10 s once
//   nothing moves (PlaceEveryMs). Re-measured with that: mode 2 vs 0 +1.26 fps,
//   p95 -0.57 ms (CPU-bound), neutral GPU-bound: main.cpp default is now 2
//   (docs\research\R44_autonomous_session.md).
//
// R90 ([Threads] PlaceRecheckMode, default 1; docs\research\R90_hitch_hunting.md F1): the pass above took a
// system-wide CreateToolhelp32Snapshot (~7,500 threads; 59.6 ms p50 on the worker in R52) and 65 % of the
// passes coincided with a render frame 4-12 ms over its median. Mode 1 keeps a handle to each of our threads
// and re-checks only those (one NtQueryInformationThread per handle) plus the render thread:
//   - our threads are adopted from qvexit's start ring (every Run / RunVoid thread notes its id there);
//   - threads of ours that do not start through qvexit (the loader's watcher, std::threads that call their
//     body directly) are found by a walk of this process's own thread list (NtGetNextThread, no system-wide
//     snapshot) at the first pass, after a mode switch, when the start ring overflowed, and every
//     kDiscoverMs (10 min);
//   - a handle names its thread for its lifetime: an exited thread is dropped (its handle is signalled), and a
//     reused id can never be mistaken for it (the R48 / R58 cases cannot arise).
// Masks, roles, cadence and log lines are those of mode 0. PlaceRecheckMode=0 runs the snapshot pass as before.
// Included once from main.cpp after perf_mon.h (perfmon::RenderTid), run_threads.h (rthreads::QueryAll /
// ForEachThread / g_nqit) and patch_threads.h (qvpatch::ForEachProcessThread).
#pragma once

namespace tplace {

// ---------------------------------------------------------------------------
// Pure parts (offline tested)
// ---------------------------------------------------------------------------

inline int Popcount(uint64_t m) {
  int n = 0;
  for (; m; m &= m - 1) ++n;
  return n;
}

// Union of the physical cores (each an LP mask) that share a logical
// processor with `lps`; *nCores (optional) = how many cores that is.
inline uint64_t CoreMaskOf(const uint64_t* cores, int n, uint64_t lps, int* nCores = nullptr) {
  uint64_t out = 0;
  int k = 0;
  for (int i = 0; i < n; ++i)
    if (cores[i] & lps) {
      out |= cores[i];
      ++k;
    }
  if (nCores) *nCores = k;
  return out;
}

enum Why : int { kOk = 0, kNoMask, kNotPinned, kOutside, kTooManyCores, kTooFewLeft };
inline const char* WhyText(int w) {
  switch (w) {
    case kOk: return "ok";
    case kNoMask: return "render thread or process mask unknown";
    case kNotPinned: return "DCS does not pin its render thread (its mask is the process mask): nothing to keep free";
    case kOutside: return "render-thread mask outside the process mask";
    case kTooManyCores: return "render-thread mask spans more than 2 physical cores: not an exclusive core";
    case kTooFewLeft: return "fewer than 4 logical processors would remain";
  }
  return "?";
}

// The mask for our threads that leaves the render thread's physical core(s)
// to it; 0 (and *why) when nothing should change.
inline uint64_t WorkerMask(uint64_t process, uint64_t render, const uint64_t* cores, int n, int* why = nullptr,
                           int minLeft = 4, int maxCores = 2) {
  auto fail = [&](int w) {
    if (why) *why = w;
    return uint64_t(0);
  };
  if (!process || !render || n <= 0) return fail(kNoMask);
  if ((render & process) != render) return fail(kOutside);
  if (render == process) return fail(kNotPinned);
  int k = 0;
  const uint64_t excl = CoreMaskOf(cores, n, render, &k);
  if (k > maxCores) return fail(kTooManyCores);
  const uint64_t w = process & ~excl;
  if (Popcount(w) < minLeft) return fail(kTooFewLeft);
  if (why) *why = kOk;
  return w;
}

// Placement modes ([Threads] WorkerPlacement).
enum Mode : int { kPlaceOff = 0, kPlaceAll = 1, kPlaceSelective = 2 };
inline int ClampMode(int m) { return m < kPlaceOff || m > kPlaceSelective ? kPlaceOff : m; }

// True when `start` is one of the n entries of `list` (null entries ignored).
inline bool InList(const void* start, const void* const* list, int n) {
  if (!start) return false;
  for (int i = 0; i < n; ++i)
    if (list[i] && list[i] == start) return true;
  return false;
}

// The mask one of our threads gets: mode 1 everything on `worker`; mode 2 the
// render-path threads on the whole process mask, the others on `worker`.
inline uint64_t TargetMask(int mode, bool renderPath, uint64_t worker, uint64_t process) {
  return mode == kPlaceSelective && renderPath ? process : worker;
}

// "0x0030 {4,5}" (at most 64 logical processors, group 0).
inline void FormatMask(char* out, size_t cap, uint64_t m) {
  int p = snprintf(out, cap, "0x%04llx {", static_cast<unsigned long long>(m));
  bool first = true;
  for (int i = 0; i < 64 && p > 0 && static_cast<size_t>(p) + 5 < cap; ++i)
    if (m >> i & 1) {
      p += snprintf(out + p, cap - p, first ? "%d" : ",%d", i);
      first = false;
    }
  if (p > 0 && static_cast<size_t>(p) + 2 <= cap) snprintf(out + p, cap - p, "}");
}

// ---------------------------------------------------------------------------
// System queries
// ---------------------------------------------------------------------------

struct BasicInfo {  // THREAD_BASIC_INFORMATION (x64)
  LONG exitStatus;
  void* teb;
  HANDLE pid, tid;
  ULONG_PTR affinity;
  LONG priority, basePriority;
};
static_assert(sizeof(BasicInfo) == 0x30, "THREAD_BASIC_INFORMATION size");
constexpr int kThreadBasicInformation = 0, kThreadWin32StartAddress = 9;

struct ThreadInfo {
  bool ok = false;
  uint64_t affinity = 0;
  int priority = 0;
  int ideal = -1;
  void* start = nullptr;
};

ThreadInfo Query(DWORD tid) {
  ThreadInfo r;
  if (!rthreads::ResolveNt() || !rthreads::g_nqit) return r;
  HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
  if (!h) return r;
  BasicInfo bi{};
  ULONG len = 0;
  if (rthreads::g_nqit(h, kThreadBasicInformation, &bi, sizeof(bi), &len) >= 0) {
    r.ok = true;
    r.affinity = bi.affinity;
    r.priority = bi.priority;
  }
  void* start = nullptr;
  if (rthreads::g_nqit(h, kThreadWin32StartAddress, &start, sizeof(start), &len) >= 0) r.start = start;
  PROCESSOR_NUMBER pn{};
  if (GetThreadIdealProcessorEx(h, &pn)) r.ideal = pn.Group == 0 ? pn.Number : -1;
  CloseHandle(h);
  return r;
}

// Physical cores of processor group 0 (LP masks). Count, 0 on failure.
int LoadCores(uint64_t* cores, int cap) {
  DWORD len = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
  if (!len) return 0;
  std::vector<uint8_t> buf(len);
  auto* p = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore, p, &len)) return 0;
  int n = 0;
  for (DWORD off = 0; off < len && n < cap;) {
    auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
    if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount >= 1 &&
        e->Processor.GroupMask[0].Group == 0)
      cores[n++] = e->Processor.GroupMask[0].Mask;
    if (!e->Size) break;
    off += e->Size;
  }
  return n;
}

struct ModRange {
  uintptr_t base = 0, end = 0;
};
ModRange RangeOf(HMODULE m) {
  ModRange r;
  if (!m) return r;
  __try {
    auto* b = reinterpret_cast<const BYTE*>(m);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew);
    r.base = reinterpret_cast<uintptr_t>(m);
    r.end = r.base + nt->OptionalHeader.SizeOfImage;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    r = ModRange{};
  }
  return r;
}
ModRange g_payload, g_loader;
void ResolveOurs() {
  if (!g_payload.base) {
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ResolveOurs), &m))
      g_payload = RangeOf(m);
  }
  if (!g_loader.base) g_loader = RangeOf(GetModuleHandleW(L"DcsQvCull.dll"));
}
bool IsOurs(const void* start) {
  const uintptr_t a = reinterpret_cast<uintptr_t>(start);
  return a && ((a >= g_payload.base && a < g_payload.end) || (a >= g_loader.base && a < g_loader.end));
}

// R44: start addresses of our threads on the render thread's critical path
// (see the header). g_testRenderPath: one extra entry for the offline test.
const void* g_testRenderPath = nullptr;
constexpr int kRenderPathStarts = 4;
inline void RenderPathStarts(const void* out[kRenderPathStarts]) {
  out[0] = defrec::Pool::ThreadStart();
  out[1] = reinterpret_cast<const void*>(&qvexit::Thread<&shadowbatch::PlanWorker>);
  out[2] = reinterpret_cast<const void*>(&qvexit::Thread<&parupload::Helper>);
  out[3] = g_testRenderPath;
}
bool IsRenderPath(const void* start) {
  const void* l[kRenderPathStarts];
  RenderPathStarts(l);
  return InList(start, l, kRenderPathStarts);
}

// "nvwgf2umx.dll+0x5b5fe0" (module of a start address; worker thread).
void Describe(const void* p, char* out, size_t cap) {
  HMODULE m = nullptr;
  wchar_t path[MAX_PATH] = L"";
  if (p && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(p), &m) &&
      GetModuleFileNameW(m, path, MAX_PATH)) {
    const wchar_t* b = wcsrchr(path, L'\\');
    b = b ? b + 1 : path;
    snprintf(out, cap, "%ls+0x%llx", b,
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(m)));
  } else {
    snprintf(out, cap, "%p", p);
  }
}

// ---------------------------------------------------------------------------
// Placement (worker thread)
// ---------------------------------------------------------------------------

struct Saved {
  DWORD tid;
  uint64_t prev;
  const void* start = nullptr;  // R48: the thread's start address when saved (a reused id is another thread)
};
std::vector<Saved> g_saved;
uint64_t g_mask = 0;     // applied worker mask (0 = placement not active)
int g_mode = 0;          // mode of the applied mask (kPlaceAll / kPlaceSelective; 0 = not active)
int g_lastMoved = -1;    // threads the last pass changed (-1 = no pass yet)
int g_lastWhy = -1;      // last logged refusal (logged once per reason)
uint64_t g_cores[64];
int g_nCores = 0;

// R90 (PlaceRecheckMode=1): our threads by handle (see the header).
struct Tracked {
  HANDLE h;           // THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION | SYNCHRONIZE
  DWORD tid;
  const void* start;  // ours (IsOurs) when adopted
  uint64_t prev = 0;  // the first mask we replaced (restored)
  bool saved = false;
};
std::vector<Tracked> g_tracked;
uint32_t g_ringRead = 0;        // next qvexit::g_startRing index to adopt
uint64_t g_lastDiscoverMs = 0;  // last process walk (0 = none yet)
constexpr uint64_t kDiscoverMs = 600000;
uint64_t g_lastPassUs = 0;      // duration of the last pass (either mode; tests and the event log)

bool AnyTrackedSaved() {
  for (const Tracked& t : g_tracked)
    if (t.saved) return true;
  return false;
}

int RestoreTracked() {
  int back = 0;
  for (Tracked& t : g_tracked) {
    if (!t.saved) continue;
    if (WaitForSingleObject(t.h, 0) != WAIT_OBJECT_0 && SetThreadAffinityMask(t.h, static_cast<DWORD_PTR>(t.prev)))
      ++back;
    t.saved = false;
  }
  return back;
}

void Restore(const char* why) {
  if (g_saved.empty() && !g_mask && !AnyTrackedSaved()) return;
  size_t n = g_saved.size();
  for (const Tracked& t : g_tracked) n += t.saved ? 1 : 0;
  int back = RestoreTracked();
  for (const Saved& s : g_saved) {
    // R48: only the thread that was saved. Its id may belong to a DCS thread by now (ours exited, the id was
    // reused): restoring our saved mask there pinned a DCS thread to our worker cores.
    if (s.start && Query(s.tid).start != s.start) continue;
    HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, s.tid);
    if (!h) continue;  // exited
    if (SetThreadAffinityMask(h, static_cast<DWORD_PTR>(s.prev))) ++back;
    CloseHandle(h);
  }
  Log("threads: worker placement off (%s): %d of %zu thread masks restored", why, back, n);
  g_saved.clear();
  g_mask = 0;
  g_mode = kPlaceOff;
}

// R44 live fix: thread id -> start address, queried once per id (a pass used to open and query every DCS
// thread, ~180, each second). Ids are reused only after a thread exits; an entry whose thread is gone is
// dropped when a pass no longer lists it.
std::unordered_map<DWORD, const void*> g_startOf;

// R58: a cached start that says "not ours" is trusted only while no thread of ours started since the last
// pass. A new thread of ours can get the id of a DCS thread that exited between two passes (ids are reused
// quickly); its stale "not ours" entry used to be kept for as long as the id was listed, so mode 2 never moved
// it off the render core. oursStarted: qvexit::g_threadStarts changed since the last pass.
inline bool CachedStartValid(bool cachedOurs, bool oursStarted) { return cachedOurs || !oursStarted; }

// Our threads (started in the payload or loader), from one snapshot.
template <class F>
void ForEachOurs(F f, bool oursStarted = false) {
  ResolveOurs();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  const DWORD pid = GetCurrentProcessId();
  std::unordered_map<DWORD, const void*> seen;
  THREADENTRY32 te{sizeof(te)};
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    auto it = g_startOf.find(te.th32ThreadID);
    const void* start = nullptr;
    if (it != g_startOf.end() && CachedStartValid(IsOurs(it->second), oursStarted)) {
      start = it->second;
    } else {
      const ThreadInfo q = Query(te.th32ThreadID);
      if (!q.ok) continue;
      start = q.start;
    }
    seen[te.th32ThreadID] = start;
    if (!IsOurs(start)) continue;
    const ThreadInfo ti = Query(te.th32ThreadID);  // ours only: current mask
    if (!ti.ok) continue;
    if (ti.start != start) {
      // R48: the cached start belonged to an earlier thread with this id (ours exited, DCS reused the id between
      // two passes, up to 10 s apart): the fresh start decides. Before, a DCS thread was moved as ours.
      seen[te.th32ThreadID] = ti.start;
      if (!IsOurs(ti.start)) continue;
    }
    f(te.th32ThreadID, ti);
  }
  CloseHandle(snap);
  g_startOf.swap(seen);
}

// oursStarted (R58): a thread of ours started since the last pass: re-query the cached "not ours" ids too.
void Place(DWORD renderTid, int mode = kPlaceAll, bool oursStarted = false) {
  mode = ClampMode(mode) == kPlaceSelective ? kPlaceSelective : kPlaceAll;
  if (!g_nCores) g_nCores = LoadCores(g_cores, 64);
  DWORD_PTR proc = 0, sys = 0;
  GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
  const ThreadInfo rt = renderTid ? Query(renderTid) : ThreadInfo{};
  int why = kNoMask;
  const uint64_t w = rt.ok ? WorkerMask(proc, rt.affinity, g_cores, g_nCores, &why) : 0;
  if (!w) {
    if (g_mask) Restore(WhyText(why));
    if (why != g_lastWhy) {
      char rm[160];
      FormatMask(rm, sizeof(rm), rt.affinity);
      Log("threads: worker placement idle: %s (render tid %lu, mask %s)", WhyText(why), renderTid, rm);
      g_lastWhy = why;
    }
    return;
  }
  g_lastWhy = kOk;
  const bool changed = w != g_mask || mode != g_mode;
  if (changed) {
    char a[200], b[200];
    FormatMask(a, sizeof(a), w);
    FormatMask(b, sizeof(b), rt.affinity);
    Log("threads: worker placement%s: our threads on %s (render thread %lu on %s keeps its physical core)%s",
        mode == kPlaceSelective ? " (selective, mode 2)" : "", a, renderTid, b,
        mode == kPlaceSelective ? "; render-path threads (recorder workers, planners, upload helpers) on the "
                                  "whole process mask"
                                : "");
  }
  int moved = 0, widened = 0;
  ForEachOurs([&](DWORD tid, const ThreadInfo& ti) {
    if (tid == renderTid) return;  // never the render thread (it is DCS's anyway)
    const bool rp = mode == kPlaceSelective && IsRenderPath(ti.start);
    const uint64_t target = TargetMask(mode, rp, w, static_cast<uint64_t>(proc));
    if (ti.affinity == target) return;
    HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) return;
    const DWORD_PTR prev = SetThreadAffinityMask(h, static_cast<DWORD_PTR>(target));
    CloseHandle(h);
    if (!prev) return;
    bool known = false;
    for (Saved& s : g_saved)
      if (s.tid == tid) {
        if (s.start != ti.start) s = Saved{tid, prev, ti.start};  // R48: a reused id: the new thread's mask
        known = true;
      }
    if (!known) g_saved.push_back({tid, prev, ti.start});  // the first mask seen is the one restored
    ++(rp ? widened : moved);
  }, oursStarted);
  if ((moved || widened) && changed)
    Log("threads: worker placement: %d of our threads moved off the render core, %d render-path threads on the "
        "process mask",
        moved, widened);
  g_mask = w;
  g_mode = mode;
  g_lastMoved = moved + widened;
}

// ---------------------------------------------------------------------------
// R90 tracked placement (PlaceRecheckMode=1)
// ---------------------------------------------------------------------------

constexpr ACCESS_MASK kTrackAccess = THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION | SYNCHRONIZE;

bool IsTracked(DWORD tid) {
  for (const Tracked& t : g_tracked)
    if (t.tid == tid) return true;
  return false;
}

// Start address through a handle (one system call).
const void* StartOf(HANDLE h) {
  void* start = nullptr;
  ULONG len = 0;
  if (!rthreads::ResolveNt() || !rthreads::g_nqit) return nullptr;
  if (rthreads::g_nqit(h, kThreadWin32StartAddress, &start, sizeof(start), &len) < 0) return nullptr;
  return start;
}

// Takes ownership of h when the thread is ours, alive and not tracked yet; else closes it. True when adopted.
bool AdoptHandle(HANDLE h, DWORD tid) {
  if (!h) return false;
  const void* start = tid && !IsTracked(tid) ? StartOf(h) : nullptr;
  if (!start || !IsOurs(start) || WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
    CloseHandle(h);
    return false;
  }
  g_tracked.push_back({h, tid, start});
  return true;
}

// The ids qvexit noted since the last call. False when the ring overflowed (a walk is then due).
bool AdoptStarted() {
  const uint32_t head = qvexit::g_startHead.load(std::memory_order_acquire);
  bool complete = true;
  if (head - g_ringRead > qvexit::kStartRing) {
    g_ringRead = head - qvexit::kStartRing;
    complete = false;
  }
  while (g_ringRead != head) {
    const uint64_t v = qvexit::g_startRing[g_ringRead % qvexit::kStartRing].load(std::memory_order_acquire);
    if (static_cast<uint32_t>(v >> 32) != g_ringRead + 1) break;  // claimed, not written yet: next pass
    const DWORD tid = static_cast<DWORD>(v);
    // A thread that exited already cannot be opened (or its id names another thread now: the start check).
    if (!IsTracked(tid)) AdoptHandle(OpenThread(kTrackAccess, FALSE, tid), tid);
    ++g_ringRead;
  }
  return complete;
}

// Every thread of ours in this process's own thread list (NtGetNextThread; no system-wide snapshot).
int Discover() {
  ResolveOurs();
  int adopted = 0;
  qvpatch::ForEachProcessThread(kTrackAccess | THREAD_QUERY_LIMITED_INFORMATION, [&](HANDLE h) {
    const DWORD tid = GetThreadId(h);
    if (IsTracked(tid)) return false;
    const void* start = StartOf(h);
    if (!start || !IsOurs(start)) return false;
    g_tracked.push_back({h, tid, start});
    ++adopted;
    return true;  // kept
  });
  return adopted;
}

// Drops exited threads (their handles are signalled).
void DropExited() {
  size_t k = 0;
  for (size_t i = 0; i < g_tracked.size(); ++i) {
    if (WaitForSingleObject(g_tracked[i].h, 0) == WAIT_OBJECT_0) {
      CloseHandle(g_tracked[i].h);
      continue;
    }
    g_tracked[k++] = g_tracked[i];
  }
  g_tracked.resize(k);
}

void CloseTracked() {
  for (Tracked& t : g_tracked) CloseHandle(t.h);
  g_tracked.clear();
}

// Mode 1's pass: the same masks and lines as Place, over the tracked handles. discover: walk the process's
// thread list too (first pass, mode switch); a walk also runs when the start ring overflowed or kDiscoverMs
// passed.
void PlaceTracked(DWORD renderTid, int mode = kPlaceAll, bool discover = false) {
  const uint64_t t0 = qvpatch::NowUs();
  mode = ClampMode(mode) == kPlaceSelective ? kPlaceSelective : kPlaceAll;
  if (!g_nCores) g_nCores = LoadCores(g_cores, 64);
  ResolveOurs();
  DWORD_PTR proc = 0, sys = 0;
  GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
  const ThreadInfo rt = renderTid ? Query(renderTid) : ThreadInfo{};
  int why = kNoMask;
  const uint64_t w = rt.ok ? WorkerMask(proc, rt.affinity, g_cores, g_nCores, &why) : 0;
  if (!w) {
    if (g_mask) Restore(WhyText(why));
    if (why != g_lastWhy) {
      char rm[160];
      FormatMask(rm, sizeof(rm), rt.affinity);
      Log("threads: worker placement idle: %s (render tid %lu, mask %s)", WhyText(why), renderTid, rm);
      g_lastWhy = why;
    }
    g_lastPassUs = qvpatch::NowUs() - t0;
    return;
  }
  g_lastWhy = kOk;
  const bool changed = w != g_mask || mode != g_mode;
  if (changed) {
    char a[200], b[200];
    FormatMask(a, sizeof(a), w);
    FormatMask(b, sizeof(b), rt.affinity);
    Log("threads: worker placement%s: our threads on %s (render thread %lu on %s keeps its physical core)%s",
        mode == kPlaceSelective ? " (selective, mode 2)" : "", a, renderTid, b,
        mode == kPlaceSelective ? "; render-path threads (recorder workers, planners, upload helpers) on the "
                                  "whole process mask"
                                : "");
  }
  const uint64_t nowMs = GetTickCount64();
  if (!AdoptStarted()) discover = true;
  if (!g_lastDiscoverMs || nowMs - g_lastDiscoverMs >= kDiscoverMs) discover = true;
  if (discover) {
    Discover();
    g_lastDiscoverMs = nowMs ? nowMs : 1;
  }
  DropExited();
  int moved = 0, widened = 0;
  for (Tracked& t : g_tracked) {
    if (t.tid == renderTid) continue;  // never the render thread (it is DCS's anyway)
    BasicInfo bi{};
    ULONG len = 0;
    if (!rthreads::g_nqit || rthreads::g_nqit(t.h, kThreadBasicInformation, &bi, sizeof(bi), &len) < 0) continue;
    const bool rp = mode == kPlaceSelective && IsRenderPath(t.start);
    const uint64_t target = TargetMask(mode, rp, w, static_cast<uint64_t>(proc));
    if (static_cast<uint64_t>(bi.affinity) == target) continue;
    const DWORD_PTR prev = SetThreadAffinityMask(t.h, static_cast<DWORD_PTR>(target));
    if (!prev) continue;
    if (!t.saved) {
      t.prev = prev;  // the first mask seen is the one restored
      t.saved = true;
    }
    ++(rp ? widened : moved);
  }
  if ((moved || widened) && changed)
    Log("threads: worker placement: %d of our threads moved off the render core, %d render-path threads on the "
        "process mask",
        moved, widened);
  g_mask = w;
  g_mode = mode;
  g_lastMoved = moved + widened;
  g_lastPassUs = qvpatch::NowUs() - t0;
}

// ---------------------------------------------------------------------------
// Census (worker thread)
// ---------------------------------------------------------------------------

struct Prev {
  uint64_t t100ns;
};
std::unordered_map<DWORD, Prev> g_prev;
std::vector<uint8_t> g_snap;
uint64_t g_lastCensusMs = 0, g_lastSnapMs = 0;

void Census(DWORD renderTid) {
  const uint64_t now = GetTickCount64();
  const size_t len = rthreads::QueryAll(g_snap);
  if (!len) {
    Log("threads: census: NtQuerySystemInformation failed");
    return;
  }
  const double sec = g_lastSnapMs ? (now - g_lastSnapMs) / 1000.0 : 0.0;
  g_lastSnapMs = now;
  struct Row {
    DWORD tid;
    double pct;
  };
  std::vector<Row> rows;
  std::unordered_map<DWORD, Prev> next;
  rthreads::ForEachThread(g_snap.data(), len, GetCurrentProcessId(), [&](const rthreads::SysThread& t) {
    const DWORD tid = static_cast<DWORD>(reinterpret_cast<uintptr_t>(t.tid));
    const uint64_t c = static_cast<uint64_t>(t.kernel.QuadPart + t.user.QuadPart);
    next[tid] = {c};
    auto it = g_prev.find(tid);
    if (sec > 0 && it != g_prev.end() && c >= it->second.t100ns)
      rows.push_back({tid, (c - it->second.t100ns) * 1e-5 / sec});  // 100 ns units -> % of one core
  });
  g_prev.swap(next);
  if (sec <= 0) return;  // first snapshot: the baseline
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.pct > b.pct; });
  if (!g_nCores) g_nCores = LoadCores(g_cores, 64);
  ResolveOurs();
  const ThreadInfo rt = renderTid ? Query(renderTid) : ThreadInfo{};
  int nrc = 0;
  const uint64_t rcore = rt.ok ? CoreMaskOf(g_cores, g_nCores, rt.affinity, &nrc) : 0;
  double oursPct = 0, oursOnCore = 0, dcsOnCore = 0, rpPct = 0, rpOnCore = 0;
  int oursN = 0, rpN = 0, logged = 0;
  char mask[200], where[160];
  Log("threads: census over %.0f s (%zu threads, %% of one core; mask = affinity, ideal = ideal processor):", sec,
      rows.size());
  for (const Row& r : rows) {
    const ThreadInfo ti = Query(r.tid);
    const bool ours = ti.ok && IsOurs(ti.start);
    const bool onCore = rcore && ti.ok && r.tid != renderTid && (ti.affinity & rcore);
    if (ours) {
      oursPct += r.pct;
      ++oursN;
      if (onCore) oursOnCore += r.pct;
      if (IsRenderPath(ti.start)) {
        rpPct += r.pct;
        ++rpN;
        if (onCore) rpOnCore += r.pct;
      }
    } else if (onCore) {
      dcsOnCore += r.pct;
    }
    if (r.tid != renderTid && (r.pct < 1.0 || logged >= 40)) continue;
    FormatMask(mask, sizeof(mask), ti.affinity);
    Describe(ti.start, where, sizeof(where));
    Log("  %-6s tid %6lu %5.1f%%  prio %2d  ideal %2d  mask %s  start %s", r.tid == renderTid ? "RENDER" : ours ? "ours" : "",
        r.tid, r.pct, ti.priority, ti.ideal, mask, where);
    ++logged;
  }
  FormatMask(mask, sizeof(mask), rcore);
  Log("threads: render tid %lu, its physical core(s) %s (%d); CPU there by other DCS threads allowed on it %.1f%%, "
      "by ours %.1f%% (ours in total %.1f%% over %d threads); worker placement %s",
      renderTid, mask, nrc, dcsOnCore, oursOnCore, oursPct, oursN,
      !g_mask ? "off" : g_mode == kPlaceSelective ? "ON (selective)" : "ON");
  Log("threads: ours by role: render path %.1f%% over %d threads (%.1f%% allowed on the render core), other %.1f%% "
      "over %d threads (%.1f%% allowed on the render core)",
      rpPct, rpN, rpOnCore, oursPct - rpPct, oursN - rpN, oursOnCore - rpOnCore);
}

// ---------------------------------------------------------------------------
// Driver (payload worker, once per second)
// ---------------------------------------------------------------------------

// R41 live fix: Tick runs on every worker loop pass, but a placement pass (a system-wide thread snapshot plus
// one query per process thread) is meant once per second (section 4.1): unthrottled it cost ~0.5 core and
// raised the render thread's busy time by 8 % live. True when a pass is due (the first call always).
inline bool PlaceDue(uint64_t nowMs, uint64_t& lastMs, uint64_t everyMs = 1000) {
  if (lastMs && nowMs - lastMs < everyMs) return false;
  lastMs = nowMs ? nowMs : 1;
  return true;
}
// R44 live fix: 1 s while passes still move threads (start-up, new threads, a mode switch), 10 s once a pass
// changed nothing. New threads of ours (a recorder pool restart) are then placed within 10 s.
// R52: idleMs is the steady-state re-check ([Threads] PlaceRecheckSec, default 60 s). Each pass takes a
// system-wide thread snapshot (CreateToolhelp32Snapshot: ~7,500 threads on the test PC, 11-21 ms of kernel
// time); R43's per-frame data showed the 1 s pass of that build adding ~8.6 ms of render-thread time per pass.
// New threads of ours are seen through qvexit::g_threadStarts (every payload thread starts in Run/RunVoid),
// so they are placed at the next 1 s pass without polling; the slow re-check only catches an outside change
// of our masks (DCS's own affinity pass).
inline uint64_t PlaceEveryMs(int lastMoved, uint64_t idleMs = 10000) { return lastMoved == 0 ? idleMs : 1000; }
inline uint64_t RecheckMs(int recheckSec) {
  return static_cast<uint64_t>(recheckSec < 10 ? 10 : recheckSec > 3600 ? 3600 : recheckSec) * 1000;
}
uint64_t g_lastPlaceMs = 0;
uint32_t g_startsAtPass = 0;  // qvexit::g_threadStarts when the last pass ran
int g_recheckModeActive = -1;  // R90: the PlaceRecheckMode of the passes so far (-1 = none yet)

// place: [Threads] WorkerPlacement (0 off, 1 all, 2 selective); recheckSec: [Threads] PlaceRecheckSec;
// recheckMode: [Threads] PlaceRecheckMode (R90: 1 tracked handles, 0 the system-wide snapshot pass).
void Tick(bool census, int censusSec, int place, int recheckSec = 60, int recheckMode = 1) {
  place = ClampMode(place);
  recheckMode = recheckMode ? 1 : 0;
  const DWORD rt = perfmon::RenderTid();
  if (place && rt) {
    bool discover = false;
    if (g_mode != kPlaceOff && place != g_mode) {  // a mode switch applies at once
      g_lastPlaceMs = 0;
      g_lastMoved = -1;
      discover = true;
    }
    if (recheckMode != g_recheckModeActive) {  // R90: the other mode's saved masks back first
      if (g_recheckModeActive >= 0) Restore("placement re-check mode changed");
      g_recheckModeActive = recheckMode;
      g_lastPlaceMs = 0;
      g_lastMoved = -1;
      discover = true;
    }
    const uint32_t starts = qvexit::g_threadStarts.load(std::memory_order_relaxed);
    const uint64_t idle = starts != g_startsAtPass ? 1000 : RecheckMs(recheckSec);
    if (PlaceDue(GetTickCount64(), g_lastPlaceMs, PlaceEveryMs(g_lastMoved, idle))) {
      const bool oursStarted = starts != g_startsAtPass;
      g_startsAtPass = starts;  // read before the pass: a thread started during it triggers the next one
      if (recheckMode) {
        PlaceTracked(rt, place, discover);
      } else {
        const uint64_t t0 = qvpatch::NowUs();
        Place(rt, place, oursStarted);
        g_lastPassUs = qvpatch::NowUs() - t0;
      }
    }
  } else {
    g_lastPlaceMs = 0;
    g_lastMoved = -1;
    if (g_mask || !g_saved.empty() || AnyTrackedSaved()) Restore(place ? "render thread unknown" : "off");
  }
  if (census && rt) {
    // The first call takes the baseline snapshot; each later one (every
    // censusSec) logs the CPU of the interval since the previous snapshot.
    const uint64_t now = GetTickCount64();
    const uint64_t every = static_cast<uint64_t>(std::max(10, censusSec)) * 1000;
    if (!g_lastSnapMs || now - g_lastCensusMs >= every) {
      Census(rt);
      g_lastCensusMs = now;
    }
  } else if (!census) {
    g_lastSnapMs = 0;
    g_prev.clear();
  }
}

void Shutdown() {
  Restore("payload stop");
  CloseTracked();
  g_recheckModeActive = -1;
}

}  // namespace tplace
