// Offline tests for thread_place.h (R39): the worker-mask rule on synthetic
// topologies, mask formatting, thread queries on this process, and the
// placement round trip on this process's own threads (a stand-in "render
// thread" pinned to one physical core; every mask restored at the end).
#pragma once

namespace tptest {

using namespace tplace;

void Pure() {
  // 8 cores, SMT siblings adjacent (LP 2k, 2k+1), as Windows numbers a 9800X3D.
  uint64_t cores[8];
  for (int i = 0; i < 8; ++i) cores[i] = 3ull << (2 * i);
  const uint64_t proc = 0xFFFF;
  int why = -1, k = 0;
  Check(CoreMaskOf(cores, 8, 0x10, &k) == 0x30 && k == 1, "threads: the physical core of LP 4 is {4,5}");
  Check(CoreMaskOf(cores, 8, 0x30, &k) == 0x30 && k == 1 && CoreMaskOf(cores, 8, 0x0110, &k) == 0x0330 && k == 2,
        "threads: core union of several LPs");
  // DCS pins its render thread to core 2 (LPs 4,5): our threads get the 14 others.
  Check(WorkerMask(proc, 0x30, cores, 8, &why) == (proc & ~0x30ull) && why == kOk,
        "threads: workers leave the render thread's physical core (both siblings)");
  Check(WorkerMask(proc, 0x10, cores, 8, &why) == (proc & ~0x30ull) && why == kOk,
        "threads: a render thread pinned to one sibling still frees the whole core");
  // Not pinned: nothing to respect.
  Check(WorkerMask(proc, proc, cores, 8, &why) == 0 && why == kNotPinned, "threads: unpinned render thread: no change");
  // Render cores group (4 cores) instead of one exclusive core: not an exclusive core.
  Check(WorkerMask(proc, 0x03F0, cores, 8, &why) == 0 && why == kTooManyCores,
        "threads: a render mask over 3+ cores is not an exclusive core: no change");
  Check(WorkerMask(proc, 0x1'0000, cores, 8, &why) == 0 && why == kOutside, "threads: mask outside the process: no change");
  Check(WorkerMask(0x3F, 0x3, cores, 8, &why) == 0x3C && why == kOk, "threads: small process mask still leaves 4 LPs");
  Check(WorkerMask(0x0F, 0x3, cores, 8, &why) == 0 && why == kTooFewLeft, "threads: fewer than 4 LPs left: no change");
  Check(WorkerMask(0, 0x3, cores, 8, &why) == 0 && why == kNoMask && WorkerMask(proc, 0x3, cores, 0, &why) == 0,
        "threads: unknown masks or topology: no change");
  char b[200];
  FormatMask(b, sizeof(b), 0x30);
  {  // R41: a placement pass at most once per second, however often Tick runs
    uint64_t last = 0;
    int due = 0;
    for (uint64_t t = 5000; t < 5000 + 2500; t += 10) due += PlaceDue(t, last) ? 1 : 0;
    uint64_t l0 = 0;
    Check(due == 3 && PlaceDue(0, l0) && l0 != 0, "threads: placement throttled to 1 pass/s (R41 live fix)");
  }
  {  // R44 live fix: 10 s between passes once a pass moved nothing; 1 s while threads still move
    uint64_t last = 0;
    int due = 0;
    for (uint64_t t = 5000; t < 5000 + 25000; t += 10) due += PlaceDue(t, last, PlaceEveryMs(0)) ? 1 : 0;
    Check(due == 3 && PlaceEveryMs(-1) == 1000 && PlaceEveryMs(3) == 1000 && PlaceEveryMs(0) == 10000,
          "threads: placement backs off to 1 pass per 10 s when nothing moved (R44 live fix)");
  }
  Check(strcmp(b, "0x0030 {4,5}") == 0, "threads: mask format");
  // R44: WorkerPlacement=2 (selective)
  Check(ClampMode(0) == kPlaceOff && ClampMode(1) == kPlaceAll && ClampMode(2) == kPlaceSelective &&
            ClampMode(3) == kPlaceOff && ClampMode(-1) == kPlaceOff,
        "threads: placement modes 0/1/2, anything else off");
  {
    int a = 0, b2 = 0, c = 0;
    const void* l[3] = {&a, nullptr, &b2};
    Check(InList(&a, l, 3) && InList(&b2, l, 3) && !InList(&c, l, 3) && !InList(nullptr, l, 3),
          "threads: start-address list lookup (null entries ignored)");
  }
  Check(TargetMask(kPlaceAll, true, 0xffcf, 0xffff) == 0xffcf && TargetMask(kPlaceAll, false, 0xffcf, 0xffff) == 0xffcf &&
            TargetMask(kPlaceSelective, true, 0xffcf, 0xffff) == 0xffff &&
            TargetMask(kPlaceSelective, false, 0xffcf, 0xffff) == 0xffcf,
        "threads: mode 1 moves every thread, mode 2 keeps render-path threads on the process mask");
  {
    const void* l[kRenderPathStarts];
    RenderPathStarts(l);
    ResolveOurs();
    Check(l[0] && l[1] && l[2] && l[0] != l[1] && l[1] != l[2] && l[0] != l[2] && IsOurs(l[0]) && IsOurs(l[1]) &&
              IsOurs(l[2]),
          "threads: render-path starts (recorder pool, planners, upload helpers) are distinct and ours");
    Check(IsRenderPath(defrec::Pool::ThreadStart()) &&
              !IsRenderPath(reinterpret_cast<const void*>(&qvexit::Thread<&timercache::Updater>)) &&
              !IsRenderPath(reinterpret_cast<const void*>(&qvexit::Thread<&shadowinst::Worker>)) &&
              !IsRenderPath(nullptr),
          "threads: timer updater and compile workers are not render path");
  }
  FormatMask(b, sizeof(b), 0);
  Check(strcmp(b, "0x0000 {}") == 0, "threads: empty mask format");
  FormatMask(b, 12, 0xFFFF);  // truncated, still terminated
  Check(strlen(b) < 12, "threads: mask format bounded");
}

DWORD WINAPI Idle(void* stop) {
  while (!static_cast<std::atomic<bool>*>(stop)->load()) Sleep(1);
  return 0;
}
// A second body: its start address stands in for a render-path thread (R44).
DWORD WINAPI IdleRp(void* stop) {
  while (!static_cast<std::atomic<bool>*>(stop)->load()) Sleep(2);
  return 0;
}

void Live() {
  const ThreadInfo me = Query(GetCurrentThreadId());
  DWORD_PTR proc = 0, sys = 0;
  GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
  Check(me.ok && me.affinity == proc && me.start, "threads: query of this thread (mask = process mask, start address)");
  ResolveOurs();
  Check(IsOurs(me.start) && !IsOurs(reinterpret_cast<void*>(&GetTickCount64)),
        "threads: this executable's threads are 'ours', kernel32 code is not");
  uint64_t cores[64];
  const int n = LoadCores(cores, 64);
  Check(n > 0 && Popcount(CoreMaskOf(cores, n, proc)) == Popcount(proc), "threads: physical cores cover the process mask");
  char where[160];
  Describe(reinterpret_cast<void*>(&GetTickCount64), where, sizeof(where));
  Check(_strnicmp(where, "kernel32.dll+0x", 15) == 0, "threads: start address described as module+RVA");
  if (n < 4 || Popcount(proc) < 8) {
    printf("     (threads: fewer than 8 logical processors: placement round trip skipped)\n");
    return;
  }

  // Stand-in render thread pinned to the second physical core; two of "our"
  // threads (this executable) must move off it and come back afterwards.
  std::atomic<bool> stop{false};
  HANDLE render = CreateThread(nullptr, 0, &Idle, &stop, 0, nullptr);
  HANDLE worker = CreateThread(nullptr, 0, &Idle, &stop, 0, nullptr);
  const uint64_t rcore = cores[1];
  SetThreadAffinityMask(render, static_cast<DWORD_PTR>(rcore));
  const DWORD rtid = GetThreadId(render), wtid = GetThreadId(worker);
  g_saved.clear();
  g_mask = 0;
  g_lastWhy = -1;
  Place(rtid);
  const ThreadInfo w1 = Query(wtid), r1 = Query(rtid), m1 = Query(GetCurrentThreadId());
  Check(g_mask == (proc & ~rcore) && w1.affinity == g_mask && m1.affinity == g_mask,
        "threads: placement moves our threads off the render core");
  Check(r1.affinity == rcore, "threads: the render thread's own mask is left alone");
  Place(rtid);  // idempotent: nothing new saved
  Check(g_saved.size() >= 2 && Query(rtid).affinity == rcore, "threads: previous masks saved once per thread");
  Restore("test");
  const ThreadInfo w2 = Query(wtid), m2 = Query(GetCurrentThreadId());
  Check(w2.affinity == proc && m2.affinity == proc && g_mask == 0 && g_saved.empty(),
        "threads: restore puts the previous masks back");
  // A render thread with the process mask: placement idles and changes nothing.
  SetThreadAffinityMask(render, proc);
  Place(rtid);
  Check(g_mask == 0 && g_saved.empty() && Query(wtid).affinity == proc && g_lastWhy == kNotPinned,
        "threads: unpinned render thread: placement idle");
  // R44 mode 2: a stand-in render-path thread (start IdleRp) gets the process
  // mask (widened from a narrower one), the others move; 2 -> 1 -> 2
  // re-targets it; a restore puts back every first-seen mask.
  {
    SetThreadAffinityMask(render, static_cast<DWORD_PTR>(rcore));
    std::atomic<bool> stopRp{false};
    HANDLE rp = CreateThread(nullptr, 0, &IdleRp, &stopRp, 0, nullptr);
    const DWORD rptid = GetThreadId(rp);
    const uint64_t narrow = proc & ~rcore & ~cores[0];  // as if DCS's affinity pass had restricted it
    SetThreadAffinityMask(rp, static_cast<DWORD_PTR>(narrow));
    g_testRenderPath = reinterpret_cast<const void*>(&IdleRp);
    g_saved.clear();
    g_mask = 0;
    g_mode = kPlaceOff;
    g_lastWhy = -1;
    Place(rtid, kPlaceSelective);
    Check(g_mode == kPlaceSelective && g_mask == (proc & ~rcore) && Query(wtid).affinity == g_mask &&
              Query(rptid).affinity == proc && Query(rtid).affinity == rcore,
          "threads: mode 2 moves the others off the render core, render-path thread on the process mask");
    Place(rtid, kPlaceAll);
    Check(g_mode == kPlaceAll && Query(rptid).affinity == (proc & ~rcore) && Query(wtid).affinity == (proc & ~rcore),
          "threads: switching 2 -> 1 moves the render-path thread too");
    Place(rtid, kPlaceSelective);
    Check(Query(rptid).affinity == proc && Query(wtid).affinity == (proc & ~rcore),
          "threads: switching 1 -> 2 gives the render-path thread the process mask back");
    size_t rpSaved = 0;
    for (const Saved& sv : g_saved) rpSaved += sv.tid == rptid ? 1 : 0;
    bool rpPrev = false;
    for (const Saved& sv : g_saved) rpPrev |= sv.tid == rptid && sv.prev == narrow;
    Check(rpSaved == 1 && rpPrev, "threads: the first-seen mask is saved once across mode switches");
    Place(rtid, kPlaceSelective);  // nothing left to move: the pass reports 0 (backs off), start addresses cached
    Check(g_lastMoved == 0 && g_startOf.count(rptid) == 1 && g_startOf.count(wtid) == 1,
          "threads: an idle pass moves nothing and keeps the start-address cache (R44 live fix)");
    Restore("test");
    Check(Query(rptid).affinity == narrow && Query(wtid).affinity == proc && g_mask == 0 && g_mode == kPlaceOff,
          "threads: restore after mode 2 puts every first-seen mask back");
    g_testRenderPath = nullptr;
    stopRp = true;
    WaitForSingleObject(rp, 2000);
    CloseHandle(rp);
  }
  // Census on this process: baseline, then one line over a busy interval.
  g_prev.clear();
  g_lastSnapMs = 0;
  Census(rtid);
  const size_t base = g_prev.size();
  const uint64_t t0 = GetTickCount64();
  volatile uint64_t v = 1;
  while (GetTickCount64() - t0 < 300) v = v * 6364136223846793005ull + 1;  // this thread busy
  Census(rtid);
  Check(base > 0 && g_prev.size() > 0 && g_lastSnapMs >= t0, "threads: census baseline and one line on this process");
  g_prev.clear();
  g_lastSnapMs = 0;
  stop = true;
  WaitForSingleObject(render, 2000);
  WaitForSingleObject(worker, 2000);
  CloseHandle(render);
  CloseHandle(worker);
  g_lastWhy = -1;
}

// R48: a thread id that one of our threads had and a foreign thread (kernel32 code) has now.
void IdReuse() {
  ResolveOurs();
  HANDLE t = CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(&SleepEx), reinterpret_cast<void*>(3000),
                          0, nullptr);
  if (!t) return;
  const DWORD tid = GetThreadId(t);
  DWORD_PTR proc = 0, sys = 0;
  GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
  const DWORD_PTR one = proc & (~proc + 1);  // the lowest processor of the process mask
  const void* ours = reinterpret_cast<const void*>(&Idle);
  // Placement pass: the cached start says ours.
  g_startOf[tid] = ours;
  bool visited = false;
  ForEachOurs([&](DWORD id, const ThreadInfo&) { visited |= id == tid; });
  // Restore: a mask saved for "our" thread with this id.
  g_saved.clear();
#ifndef R48_PREFIX
  g_saved.push_back({tid, static_cast<uint64_t>(one), ours});
#else
  g_saved.push_back({tid, static_cast<uint64_t>(one)});
#endif
  g_mask = 1;
  Restore("test");
  const ThreadInfo after = Query(tid);
  Check(!visited && after.ok && after.affinity == static_cast<uint64_t>(proc),
        "R48 threads: a reused thread id (now a foreign thread) is neither placed nor restored");
  TerminateThread(t, 0);
  CloseHandle(t);
  g_startOf.clear();
}

// R58: the reverse reuse: a thread of ours got an id whose cached start says "not ours" (a DCS thread that
// exited between two passes). The cache is re-checked when a thread of ours started since the last pass.
void IdReuseNotOurs() {
  ResolveOurs();
  Check(CachedStartValid(true, false) && CachedStartValid(true, true) && CachedStartValid(false, false) &&
            !CachedStartValid(false, true),
        "R58 threads: a cached 'not ours' start is re-queried after a thread of ours started");
  std::atomic<bool> stop{false};
  HANDLE t = CreateThread(nullptr, 0, &Idle, &stop, 0, nullptr);
  if (!t) return;
  const DWORD tid = GetThreadId(t);
  const void* foreign = reinterpret_cast<const void*>(&GetTickCount64);  // the earlier (DCS) thread's start
  g_startOf.clear();
  g_startOf[tid] = foreign;
  bool quiet = false;
  ForEachOurs([&](DWORD id, const ThreadInfo&) { quiet |= id == tid; });  // no thread of ours started: cache kept
  g_startOf[tid] = foreign;
  bool fresh = false;
  ForEachOurs([&](DWORD id, const ThreadInfo&) { fresh |= id == tid; }, true);
  Check(!quiet && fresh && g_startOf[tid] == reinterpret_cast<const void*>(&Idle),
        "R58 threads: our new thread on a reused id with a stale 'not ours' cache entry is found and re-cached");
  stop = true;
  WaitForSingleObject(t, 2000);
  CloseHandle(t);
  g_startOf.clear();
}

void Run() {
  Pure();
  Live();
  IdReuse();
  IdReuseNotOurs();
}

}  // namespace tptest
