// R90 lossless hitch fixes (docs/research/R90_hitch_hunting.md, "Implementation"): one regression test per fix.
//   F3  code-site writes without the system-wide snapshot (patch_threads.h, [General] PatchMode)
//   F1  placement re-check over tracked handles (thread_place.h, [Threads] PlaceRecheckMode)
//   F2  asynchronous log path for DCS's threads (main.cpp qvalog, [Log] Async)
//   F4  self-test readbacks polled with DO_NOT_WAIT, one private device (chain_fast.h, shader_fast.h)
//   F7  the Lua hook's onSimulationFrame returns early, allocation-free, while GVision is off
//   --  shader_fast InThisModule from the cached image range
//   s7  disk hygiene at payload start (disk_prune.h)
//   H2  shadow_inst: VS objects created outside g_mutex, the render thread's queue push lock-free
// H1 (the shadow plan wait cap) is tested in shadow_plan_test.h, where the plan fixtures live.
#pragma once

namespace r90 {

// ---------------------------------------------------------------------------
// F3: suspended code writes
// ---------------------------------------------------------------------------

// Site under test: 6 bytes then ret. A = nop; mov eax, 1 / B = mov eax, 2; nop. A thread stopped after A's
// nop (site+1) would execute garbage in B: exactly what the instruction-pointer check must prevent.
const uint8_t kSiteA[6] = {0x90, 0xB8, 0x01, 0x00, 0x00, 0x00};
const uint8_t kSiteB[6] = {0xB8, 0x02, 0x00, 0x00, 0x00, 0x90};

struct Spin {
  uint8_t* code = nullptr;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> calls{0}, bad{0};
};
DWORD WINAPI SpinCall(void* arg) {
  auto* s = static_cast<Spin*>(arg);
  auto fn = reinterpret_cast<int (*)()>(s->code);
  while (!s->stop.load(std::memory_order_relaxed)) {
    const int v = fn();
    if (v != 1 && v != 2) s->bad.fetch_add(1, std::memory_order_relaxed);
    s->calls.fetch_add(1, std::memory_order_relaxed);
  }
  return 0;
}
DWORD WINAPI WaitOn(void* ev) {
  WaitForSingleObject(static_cast<HANDLE>(ev), INFINITE);
  return 0;
}

// R115 P5: NtGetNextThread that fails with STATUS_ACCESS_DENIED after g_nextLeft threads.
std::atomic<int> g_nextLeft{0};
LONG NTAPI FailingNextThread(HANDLE p, HANDLE t, ACCESS_MASK a, ULONG at, ULONG fl, PHANDLE nx) {
  if (g_nextLeft.fetch_sub(1) <= 0) return static_cast<LONG>(0xC0000022L);
  return qvpatch::NextThreadFn()(p, t, a, at, fl, nx);
}
// Suspend count of a thread (0 = running), restored.
DWORD SuspendCount(HANDLE t) {
  const DWORD c = SuspendThread(t);
  if (c != static_cast<DWORD>(-1)) ResumeThread(t);
  return c;
}

int ToolhelpCount() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return -1;
  THREADENTRY32 te{sizeof(te)};
  int n = 0;
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    n += te.th32OwnerProcessID == GetCurrentProcessId();
  CloseHandle(snap);
  return n;
}

void Patch() {
  auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!page) {
    Check(false, "R90 F3: code page");
    return;
  }
  memset(page, 0xCC, 4096);
  memcpy(page, kSiteA, 6);
  page[6] = 0xC3;
  DWORD old;
  VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  // 200 idle threads (DCS runs ~220) and 4 threads executing the site.
  HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::vector<HANDLE> idle;
  for (int i = 0; i < 200; ++i)
    if (HANDLE t = CreateThread(nullptr, 0, &WaitOn, ev, 0, nullptr)) idle.push_back(t);
  Spin spin;
  spin.code = page;
  HANDLE spinners[4];
  for (HANDLE& t : spinners) t = CreateThread(nullptr, 0, &SpinCall, &spin, 0, nullptr);
  Sleep(20);

  const int viaNt = qvpatch::ForEachProcessThread(THREAD_QUERY_LIMITED_INFORMATION, [](HANDLE) { return false; });
  const int viaTh = ToolhelpCount();
  printf("     (R90 F3: this process has %d threads by NtGetNextThread, %d by the Toolhelp snapshot)\n", viaNt, viaTh);
  Check(viaNt > 200 && viaTh > 0 && std::abs(viaNt - viaTh) <= 2,
        "R90 F3: NtGetNextThread lists this process's threads as the system-wide snapshot does");

  const int mode0 = qvpatch::g_mode.load();
  double medWall[2] = {}, medHeld[2] = {};
  bool allOk = true, bytesOk = true;
  for (int mode = 0; mode < 2; ++mode) {
    qvpatch::g_mode = mode;
    std::vector<double> wall, held;
    for (int i = 0; i < 20; ++i) {
      const uint8_t* want = (i & 1) ? kSiteA : kSiteB;
      const qvpatch::Site s{page, want, 6};
      const uint64_t t0 = qvpatch::NowUs();
      allOk &= qvpatch::WriteSuspended(&s, 1);
      wall.push_back((qvpatch::NowUs() - t0) / 1000.0);
      held.push_back(qvpatch::g_lastHeldUs.load() / 1000.0);
      bytesOk &= memcmp(page, want, 6) == 0;
    }
    std::sort(wall.begin(), wall.end());
    std::sort(held.begin(), held.end());
    medWall[mode] = wall[wall.size() / 2];
    medHeld[mode] = held[held.size() / 2];
  }
  // R115 P5: a walk that fails midway (access denied after 100 threads) is not the end of the list.
  bool p5Bytes = true, p5Ok = true, p5Held = true;
  int p5Visited = 0;
  if (qvpatch::NextThreadFn()) {
    qvpatch::g_testNextThread = &FailingNextThread;
    g_nextLeft = 100;
    p5Visited = qvpatch::ForEachProcessThread(THREAD_QUERY_LIMITED_INFORMATION, [](HANDLE) { return false; });
    qvpatch::g_mode = 1;
    for (int i = 0; i < 6; ++i) {
      const uint8_t* want = (i & 1) ? kSiteA : kSiteB;
      const qvpatch::Site s{page, want, 6};
      g_nextLeft = 100;
      p5Ok &= qvpatch::WriteSuspended(&s, 1);
      p5Bytes &= memcmp(page, want, 6) == 0;
      p5Held &= qvpatch::g_lastThreads.load() >= idle.size() + 4;  // the write held every thread (Toolhelp path)
    }
    qvpatch::g_testNextThread = nullptr;
  } else {
    printf("     (R115 P5: no NtGetNextThread: failing-walk checks skipped)\n");
  }
  qvpatch::g_mode = mode0;
  bool p5Running = true;
  for (HANDLE t : idle) p5Running &= SuspendCount(t) == 0;
  for (HANDLE t : spinners) p5Running &= SuspendCount(t) == 0;
  {
    const uint64_t c0 = spin.calls.load();
    Sleep(20);
    p5Running &= spin.calls.load() > c0;
  }
  if (qvpatch::NextThreadFn()) {
    Check(p5Visited == -1, "R115 P5: NtGetNextThread failing mid-walk (access denied) makes ForEachProcessThread "
                           "return -1, not a short list");
    Check(p5Ok && p5Bytes && p5Held,
          "R115 P5: with the walk failing mid-list, WriteSuspended falls back to the Toolhelp path: sites written, "
          "every other thread held during the write");
  }
  Check(p5Running && spin.bad.load() == 0,
        "R115 P5: after the failed walks no thread is left suspended (suspend counts 0, the site threads progress)");
  const uint64_t calls0 = spin.calls.load();
  Sleep(20);
  const bool stillRunning = spin.calls.load() > calls0;
  spin.stop = true;
  WaitForMultipleObjects(4, spinners, TRUE, 5000);
  for (HANDLE t : spinners) CloseHandle(t);
  SetEvent(ev);
  for (HANDLE t : idle) {
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
  }
  CloseHandle(ev);
  VirtualFree(page, 0, MEM_RELEASE);
  printf("     (R90 F3: %zu+4+main threads; per write, median: PatchMode 0 %.2f ms wall (threads held %.2f ms), "
         "PatchMode 1 %.2f ms wall (held %.2f ms); %llu site calls, %llu bad)\n",
         idle.size(), medWall[0], medHeld[0], medWall[1], medHeld[1],
         static_cast<unsigned long long>(spin.calls.load()), static_cast<unsigned long long>(spin.bad.load()));
  Check(allOk && bytesOk && spin.bad.load() == 0 && stillRunning,
        "R90 F3: 40 rewrites of a 6-byte site executed by 4 threads (PatchMode 0 and 1): every write done, no thread "
        "ever saw a torn instruction, the site keeps running");
  Check(medWall[1] < medWall[0] && medWall[1] < 20.0,
        "R90 F3: PatchMode 1 (process-only enumeration, batched suspension) is faster than the Toolhelp path and "
        "below 20 ms with 200+ threads");
}

// ---------------------------------------------------------------------------
// F1: tracked placement
// ---------------------------------------------------------------------------

void Placement() {
  using namespace tplace;
  DWORD_PTR proc = 0, sys = 0;
  GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
  uint64_t cores[64];
  const int nc = LoadCores(cores, 64);
  if (nc < 4 || Popcount(proc) < 8) {
    printf("     (R90 F1: fewer than 8 logical processors: tracked placement round trip skipped)\n");
    return;
  }
  ResolveOurs();
  const uint64_t rcore = cores[1], target = proc & ~rcore;
  std::atomic<bool> stopA{false}, stopB{false};
  HANDLE render = CreateThread(nullptr, 0, &tptest::Idle, &stopB, 0, nullptr);
  SetThreadAffinityMask(render, static_cast<DWORD_PTR>(rcore));
  const DWORD rtid = GetThreadId(render);
  const uint32_t head0 = qvexit::g_startHead.load();
  std::vector<HANDLE> ours;  // the first 50 stop early (stopA)
  for (int i = 0; i < 200; ++i)
    if (HANDLE t = CreateThread(nullptr, 0, qvexit::Thread<&tptest::Idle>, i < 50 ? &stopA : &stopB, 0, nullptr))
      ours.push_back(t);
  HANDLE plain = CreateThread(nullptr, 0, &tptest::Idle, &stopB, 0, nullptr);  // ours, not through qvexit
  for (int i = 0; i < 200 && qvexit::g_startHead.load() - head0 < ours.size(); ++i) Sleep(5);
  // Fresh tracked state; only the ring (no walk) in the first pass.
  CloseTracked();
  g_saved.clear();
  g_mask = 0;
  g_mode = kPlaceOff;
  g_lastWhy = -1;
  g_ringRead = head0;
  g_lastDiscoverMs = GetTickCount64();
  PlaceTracked(rtid, kPlaceAll, false);
  bool moved = g_mask == target;
  for (HANDLE t : ours) moved &= Query(GetThreadId(t)).affinity == target;
  const bool plainLeft = Query(GetThreadId(plain)).affinity == proc;
  Check(moved && plainLeft && Query(rtid).affinity == rcore,
        "R90 F1: tracked placement adopts the threads noted in qvexit's start ring (no thread walk) and moves them "
        "off the render core; the render thread's mask is left alone");
  PlaceTracked(rtid, kPlaceAll, true);  // the walk of this process's own thread list
  Check(Query(GetThreadId(plain)).affinity == target,
        "R90 F1: a thread of ours that did not start through qvexit is found by the process-only walk");
  // Steady re-check: nothing to move; old pass for comparison.
  std::vector<double> passUs;
  for (int i = 0; i < 9; ++i) {
    PlaceTracked(rtid, kPlaceAll, false);
    passUs.push_back(static_cast<double>(g_lastPassUs));
  }
  std::sort(passUs.begin(), passUs.end());
  const size_t tracked = g_tracked.size();
  const uint64_t t0 = qvpatch::NowUs();
  Place(rtid, kPlaceAll, false);  // the pre-R90 pass (system-wide snapshot), same masks: changes nothing
  const double oldUs = static_cast<double>(qvpatch::NowUs() - t0);
  g_saved.clear();
  printf("     (R90 F1: steady re-check of %zu tracked threads %.0f us median; the snapshot pass took %.0f us)\n",
         tracked, passUs[passUs.size() / 2], oldUs);
  Check(tracked >= 202 && passUs[passUs.size() / 2] < 1000.0 && g_lastMoved == 0,
        "R90 F1: the steady re-check of 200+ tracked threads takes < 1 ms and moves nothing");
  // A new thread of ours is adopted from the ring at the next pass.
  HANDLE late = CreateThread(nullptr, 0, qvexit::Thread<&tptest::Idle>, &stopB, 0, nullptr);
  for (int i = 0; i < 200 && !IsTracked(GetThreadId(late)); ++i) {
    Sleep(5);
    PlaceTracked(rtid, kPlaceAll, false);
  }
  Check(Query(GetThreadId(late)).affinity == target, "R90 F1: a thread of ours started later is placed at the next pass");
  // 50 exit: their handles are dropped.
  stopA = true;
  for (int i = 0; i < 50; ++i) WaitForSingleObject(ours[i], 5000);
  PlaceTracked(rtid, kPlaceAll, false);
  Check(g_tracked.size() == tracked + 1 - 50, "R90 F1: exited threads leave the tracked set (their handles signal)");
  Restore("test");
  bool back = g_mask == 0 && !AnyTrackedSaved();
  for (size_t i = 50; i < ours.size(); ++i) back &= Query(GetThreadId(ours[i])).affinity == proc;
  back &= Query(GetThreadId(plain)).affinity == proc && Query(GetThreadId(late)).affinity == proc;
  Check(back, "R90 F1: restore puts every tracked thread's first mask back");
  stopB = true;
  for (HANDLE t : ours) {
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
  }
  for (HANDLE t : {render, plain, late}) {
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
  }
  CloseTracked();
  g_lastWhy = -1;
  g_lastDiscoverMs = 0;
}

// ---------------------------------------------------------------------------
// F2: asynchronous log path
// ---------------------------------------------------------------------------

std::vector<std::string> ReadLines(const std::wstring& path) {
  std::vector<std::string> out;
  FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
  if (!f) return out;
  char line[4096];
  while (fgets(line, sizeof(line), f)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    out.push_back(line);
  }
  fclose(f);
  return out;
}

constexpr int kT = 4, kN = 400;  // AsyncLog: paced threads and lines per thread

bool Stamped(const std::string& l) {  // "HH:MM:SS.mmm text"
  return l.size() > 13 && isdigit(static_cast<unsigned char>(l[0])) && l[2] == ':' && l[5] == ':' && l[8] == '.' &&
         l[12] == ' ';
}

// R115 (L2): the async log ring's state as the process starts (main() calls RingAtLoad before any test).
struct RingAtLoad {
  bool taken = false, queried = false;
  uint32_t lastRaw = 0;
  int pages = 0, resident = 0;  // whole pages inside the ring's second half, and how many were in the working set
} g_ringAtLoad;
void NoteRingAtLoad() {
  RingAtLoad& r = g_ringAtLoad;
  r.taken = true;
  r.lastRaw = static_cast<uint32_t>(qvalog::g_ring[qvalog::kAsyncSlots - 1].seq);
  const uintptr_t a = (reinterpret_cast<uintptr_t>(&qvalog::g_ring[qvalog::kAsyncSlots / 2]) + 4095) &
                      ~static_cast<uintptr_t>(4095);
  const uintptr_t e = reinterpret_cast<uintptr_t>(&qvalog::g_ring[qvalog::kAsyncSlots]) & ~static_cast<uintptr_t>(4095);
  r.queried = true;
  for (uintptr_t p = a; p < e; p += 4096) {
    PSAPI_WORKING_SET_EX_INFORMATION wi{};
    wi.VirtualAddress = reinterpret_cast<void*>(p);
    if (!K32QueryWorkingSetEx(GetCurrentProcess(), &wi, sizeof(wi))) {
      r.queried = false;
      break;
    }
    ++r.pages;
    r.resident += wi.VirtualAttributes.Valid ? 1 : 0;
  }
}

void AsyncLog() {
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring path = std::wstring(tmp) + L"r90_log_" + std::to_wstring(GetCurrentProcessId()) + L".log";
  FILE* f = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
  if (!f) {
    Check(false, "R90 F2: temp log file");
    return;
  }
  FILE* const saved = g_log;
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_log = f;
  }
  const bool want0 = qvalog::g_want.load();
  qvalog::g_want = true;
  qvalog::g_dropped = 0;
  qvalog::g_queued = 0;
  qvalog::g_written = 0;
  // R115 (L2): no initialiser wrote the ring at load (RingAtLoad, taken first thing in main(): the pages of the
  // ring's second half were not in the working set and the last slot's stored sequence was 0, zero-initialised =
  // free); and before this test (the first to start the async log; a synchronous line reads slot 0 only) the last
  // slot is still 0. (Residency is judged at main(): some earlier test reads the image's data pages wholesale.)
  {
    const uint32_t lastRaw = static_cast<uint32_t>(qvalog::g_ring[qvalog::kAsyncSlots - 1].seq);
    printf("     (R115 L2: at load: last slot's stored sequence %u, ring pages resident %d of %d (query %d); before "
           "first use: last slot %u)\n",
           g_ringAtLoad.lastRaw, g_ringAtLoad.resident, g_ringAtLoad.pages, g_ringAtLoad.queried ? 1 : 0, lastRaw);
    Check(g_ringAtLoad.taken && g_ringAtLoad.lastRaw == 0 && g_ringAtLoad.queried && g_ringAtLoad.pages > 0 &&
              g_ringAtLoad.resident == 0 && lastRaw == 0,
          "R115 L2: the async log ring is untouched at load and before its first use (zero-initialised slots are "
          "free; no page of it committed by an initialiser)");
  }
  StartAsyncLog();
  const bool on = qvalog::g_on.load();

  // 1. Order and latency: 4 DCS-like threads (not started through qvexit) at a paced rate.
  std::vector<double> lat[kT];
  {
    std::vector<std::thread> th;
    for (int t = 0; t < kT; ++t)
      th.emplace_back([t, &lat] {
        for (int i = 0; i < kN; ++i) {
          const uint64_t a = qvpatch::NowUs();
          Log("r90 paced t=%d n=%d", t, i);
          lat[t].push_back(static_cast<double>(qvpatch::NowUs() - a));
          if (i % 10 == 9) Sleep(5);
        }
      });
    for (auto& x : th) x.join();
  }
  // 2. A burst far beyond the ring: lines are dropped and counted, never blocking. R93: from frame threads (only
  //    they drop; a loading thread waits for ring space, r93_test.h), with the writer held so the ring (4096 lines
  //    since R93) surely overflows.
  {
    qvalog::g_testHold = true;
    Sleep(30);  // the writer is past its last drain
    std::vector<std::thread> th;
    for (int t = 0; t < 8; ++t)
      th.emplace_back([t] {
        qvalog::NoteFrameThread();
        for (int i = 0; i < 3000; ++i) Log("r90 burst t=%d n=%d", t, i);
      });
    for (auto& x : th) x.join();
    qvalog::g_testHold = false;
  }
  // 3. Writer held: a DCS thread's queued line, then a synchronous line of one of our threads: in that order.
  for (int i = 0; i < 1000 && qvalog::g_written.load() < qvalog::g_queued.load(); ++i) Sleep(2);  // burst out
  qvalog::g_testHold = true;
  Sleep(30);  // the writer is past its last drain
  std::thread([] { Log("r90 queued-before-sync"); }).join();
  std::thread([] {
    g_tOurThread = true;
    Log("r90 sync-after-queued");
  }).join();
  qvalog::g_testHold = false;
  StopAsyncLog();
  const uint64_t queued = qvalog::g_queued.load(), dropped = qvalog::g_dropped.load(), written = qvalog::g_written.load();
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_log = saved;
  }
  fclose(f);
  qvalog::g_want = want0;

  const std::vector<std::string> lines = ReadLines(path);
  DeleteFileW(path.c_str());
  int pacedSeen = 0, burstSeen = 0, posQueued = -1, posSync = -1, dropLines = 0;
  bool stamped = true, ordered = true;
  int lastPaced[kT], lastBurst[8];
  for (int& x : lastPaced) x = -1;
  for (int& x : lastBurst) x = -1;
  uint64_t droppedReported = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& l = lines[i];
    stamped &= Stamped(l);
    if (l.size() < 13) continue;
    const char* text = l.c_str() + 13;
    int t = 0, n = 0;
    if (sscanf_s(text, "r90 paced t=%d n=%d", &t, &n) == 2 && t >= 0 && t < kT) {
      ordered &= n > lastPaced[t];
      lastPaced[t] = n;
      ++pacedSeen;
    } else if (sscanf_s(text, "r90 burst t=%d n=%d", &t, &n) == 2 && t >= 0 && t < 8) {
      ordered &= n > lastBurst[t];
      lastBurst[t] = n;
      ++burstSeen;
    } else if (!strcmp(text, "r90 queued-before-sync")) {
      posQueued = static_cast<int>(i);
    } else if (!strcmp(text, "r90 sync-after-queued")) {
      posSync = static_cast<int>(i);
    } else if (!strncmp(text, "log: ", 5) && strstr(text, "dropped")) {
      ++dropLines;
      unsigned long long k = 0;
      if (sscanf_s(text, "log: %llu", &k) == 1) droppedReported += k;
    }
  }
  std::vector<double> all;
  for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());
  std::sort(all.begin(), all.end());
  const double p99 = all.empty() ? 0 : all[all.size() * 99 / 100], mx = all.empty() ? 0 : all.back();
  printf("     (R90 F2: %d paced lines, %d of 24000 burst lines written, %llu dropped (%d drop lines); Log() on a DCS "
         "thread p99 %.1f us, max %.1f us)\n",
         pacedSeen, burstSeen, static_cast<unsigned long long>(dropped), dropLines, p99, mx);
  Check(on && stamped && ordered && pacedSeen == kT * kN,
        "R90 F2: DCS threads' lines go through the ring: every paced line written, each thread's lines in order, "
        "the time stamp format unchanged");
  Check(queued + dropped >= static_cast<uint64_t>(kT * kN + 8 * 3000 + 1) && written == queued &&
            droppedReported == dropped && dropped > 0 && dropLines > 0,
        "R90 F2: a full ring drops lines without blocking; every drop is counted and reported in a log line");
  Check(posQueued >= 0 && posSync > posQueued,
        "R90 F2: a synchronous line of ours is written after the DCS thread's lines queued before it");
  Check(p99 < 50.0, "R90 F2: Log() on a DCS thread: p99 below 50 us (no file write on the calling thread)");
}

// ---------------------------------------------------------------------------
// F4 and the InThisModule one-liner
// ---------------------------------------------------------------------------

void FastReadback() {
  ID3D11Device* d = nullptr;
  ID3D11DeviceContext* c = nullptr;
  const uint32_t created0 = chainfast::g_privCreated.load();
  const bool got = chainfast::AcquirePrivate(&d, &c);
  ID3D11Device* d2 = nullptr;
  const bool again = chainfast::AcquirePrivate(&d2);
  if (!got) {
    printf("     (R90 F4: no hardware D3D11 device: private-device reuse not checked)\n");
  } else {
    ID3D11Device* d3 = nullptr;
    const uint32_t created1 = chainfast::g_privCreated.load();
    Check(again && d2 == d && created1 - created0 <= 1 && chainfast::AcquirePrivate(&d3) && d3 == d &&
              chainfast::g_privCreated.load() == created1,
          "R90 F4: the self tests reuse one private device (created once)");
  }
  ID3D11Device* wd = nullptr;
  ID3D11DeviceContext* wc = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (!got && SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &wd,
                                          nullptr, &wc))) {
    d = wd;
    c = wc;
  }
  bool readOk = false;
  if (d && c) {
    std::vector<uint32_t> data(65536);
    for (uint32_t i = 0; i < data.size(); ++i) data[i] = i * 2654435761u;
    chainfast::TBuf b;
    if (chainfast::MakeBuf(d, static_cast<UINT>(data.size()), 0, data.data(), false, true, b)) {
      std::vector<uint32_t> out;
      readOk = chainfast::ReadBuf(d, c, b.b, b.bytes, out) && out == data;
    }
    b.Release();
  }
  Check(readOk, "R90 F4: ReadBuf polls with DO_NOT_WAIT and returns the buffer's contents");
  if (wc) wc->Release();
  if (wd) wd->Release();
  chainfast::ReleasePrivate();
  {  // R115 P6b: worker start creates the private device only with the engine kill switch off.
    const uint32_t cf0 = g_cfg.chainFast, sf0 = g_cfg.shaderFast;
    const bool off0 = g_engineOff.load(), po0 = g_perceptionOnly.load();
    g_cfg.chainFast = 0;  // no creation hook from the test, only the device
    g_cfg.shaderFast = 3;
    g_perceptionOnly = false;
    g_engineOff = true;
    const uint32_t n0 = chainfast::g_privCreated.load();
    ChainFastAtStart();
    const bool noneOff = chainfast::g_privDev == nullptr && chainfast::g_privCreated.load() == n0;
    g_engineOff = false;
    ChainFastAtStart();
    const bool madeOn = chainfast::g_privDev != nullptr;
    chainfast::ReleasePrivate();
    g_cfg.chainFast = cf0;
    g_cfg.shaderFast = sf0;
    g_engineOff = off0;
    g_perceptionOnly = po0;
    Check(noneOff, "R115 P6b: with the engine kill switch on, worker start creates no private D3D11 device");
    if (got)
      Check(madeOn, "R115 P6b: with the engine on and ShaderFast set, worker start creates the private device");
  }
  Check(sfast::InThisModule(reinterpret_cast<void*>(&sfast::Rearm)) &&
            !sfast::InThisModule(reinterpret_cast<void*>(&GetTickCount64)) && !sfast::InThisModule(nullptr),
        "R90: shader_fast InThisModule from the cached image range (no GetModuleHandleExW on the render thread)");
}

// ---------------------------------------------------------------------------
// F7: the Lua hook
// ---------------------------------------------------------------------------

void LuaHook() {
  HMODULE lua = dcsinstall::Found() ? LoadLibraryW((dcsinstall::Bin() + L"lua.dll").c_str()) : nullptr;
  if (!lua) {
    dcsinstall::Skip("R90 F7 Lua hook early return (DCS's lua.dll)");
    return;
  }
  using namespace r63lua;
  auto newState = reinterpret_cast<NewStateFn>(GetProcAddress(lua, "luaL_newstate"));
  auto openLibs = reinterpret_cast<OpenLibsFn>(GetProcAddress(lua, "luaL_openlibs"));
  auto loadString = reinterpret_cast<LoadStringFn>(GetProcAddress(lua, "luaL_loadstring"));
  auto loadFile = reinterpret_cast<LoadFileFn>(GetProcAddress(lua, "luaL_loadfile"));
  auto pcall = reinterpret_cast<PcallFn>(GetProcAddress(lua, "lua_pcall"));
  auto toLString = reinterpret_cast<ToLStringFn>(GetProcAddress(lua, "lua_tolstring"));
  auto setTop = reinterpret_cast<SetTopFn>(GetProcAddress(lua, "lua_settop"));
  auto closeState = reinterpret_cast<CloseFn>(GetProcAddress(lua, "lua_close"));
  const std::wstring exe = ExeDir();
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring root = std::wstring(tmp) + L"qv_r90_lua" + std::to_wstring(GetCurrentProcessId()) + L"\\";
  const std::wstring mod = root + L"Scripts\\DcsQvCull\\";
  CreateDirectoryW(root.c_str(), nullptr);
  CreateDirectoryW((root + L"Scripts").c_str(), nullptr);
  CreateDirectoryW(mod.c_str(), nullptr);
  const bool copied = CopyFileW((exe + L"qv_fake_payload.dll").c_str(), (mod + L"DcsQvCull.dll").c_str(), FALSE) != 0;
  if (!newState || !openLibs || !loadString || !loadFile || !pcall || !toLString || !setTop || !closeState || !copied) {
    QvSkip("R90 F7 Lua hook: lua.dll exports or the stand-in module unavailable\n");
  } else {
    State L = newState();
    openLibs(L);
    const std::string prelude =
        "lfs = {writedir = function() return [[" + Narrow(root) +
        "]] end}\nlog = {INFO = 1, ERROR = 2, write = function() end}\nEXPORT_CALLS = 0\n"
        "Export = {LoGetAccelerationUnits = function() EXPORT_CALLS = EXPORT_CALLS + 1; return {x = 0, y = 1, z = 0} "
        "end}\nDCS = {setUserCallbacks = function(cb) CB = cb end, getModelTime = function() return 1 end}\n";
    bool ok = loadString(L, prelude.c_str()) == 0 && pcall(L, 0, 0, 0) == 0;
    const std::string hook = Narrow(exe) + "..\\lua\\DcsQvCull.lua";
    ok = ok && loadFile(L, hook.c_str()) == 0 && pcall(L, 0, 0, 0) == 0;
    // 1000 simulation frames with the GC stopped: Export calls and the Lua heap growth (KB).
    const char* frames =
        "collectgarbage('collect'); collectgarbage('stop'); EXPORT_CALLS = 0\n"
        "local before = collectgarbage('count')\n"
        "for i = 1, 1000 do CB.onSimulationFrame() end\n"
        "local grew = collectgarbage('count') - before\n"
        "collectgarbage('restart')\n"
        "return tostring(EXPORT_CALLS) .. ' ' .. string.format('%.3f', grew)";
    auto run = [&](const char* wanted, int* calls, double* grewKb) {
      SetEnvironmentVariableA("QV_TEST_GLOAD_WANTED", wanted);
      *calls = -1;
      *grewKb = -1;
      if (!(loadString(L, frames) == 0 && pcall(L, 0, 1, 0) == 0)) return false;
      size_t n = 0;
      const char* s = toLString(L, -1, &n);
      const bool parsed = s && sscanf_s(s, "%d %lf", calls, grewKb) == 2;
      setTop(L, 0);
      return parsed;
    };
    int callsOff = -1, callsOn = -1;
    double grewOff = -1, grewOn = -1;
    ok = ok && run("0", &callsOff, &grewOff) && run("1", &callsOn, &grewOn);
    using CountFn = long (*)();
    HMODULE fake = GetModuleHandleW((mod + L"DcsQvCull.dll").c_str());
    auto count = fake ? reinterpret_cast<CountFn>(GetProcAddress(fake, "QvFakeGLoadCalls")) : nullptr;
    const long gloads = count ? count() : -1;
    printf("     (R90 F7: GVision off: %d Export calls, Lua heap +%.3f KB over 1000 frames; on: %d Export calls, "
           "%ld G samples passed, heap +%.3f KB)\n",
           callsOff, grewOff, callsOn, gloads, grewOn);
    Check(ok && callsOff == 0 && grewOff == 0.0,
          "R90 F7: GVision off: onSimulationFrame returns before any Export call and allocates nothing");
    Check(ok && callsOn == 1000 && gloads == 1000, "R90 F7: GVision on: every frame samples and passes G, as before");
    closeState(L);
    SetEnvironmentVariableA("QV_TEST_GLOAD_WANTED", nullptr);
  }
  DeleteFileW((mod + L"DcsQvCull.dll").c_str());
  RemoveDirectoryW(mod.c_str());
  RemoveDirectoryW((root + L"Scripts").c_str());
  RemoveDirectoryW(root.c_str());
  FreeLibrary(lua);
}

// ---------------------------------------------------------------------------
// Section 7: disk hygiene
// ---------------------------------------------------------------------------

void MakeFile(const std::wstring& p, size_t bytes, uint64_t ageSec) {
  HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  std::string b(bytes, 'x');
  DWORD w;
  WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &w, nullptr);
  FILETIME ft;
  const uint64_t t = prune::NowTicks() - ageSec * 10000000ull;
  ft.dwLowDateTime = static_cast<DWORD>(t);
  ft.dwHighDateTime = static_cast<DWORD>(t >> 32);
  SetFileTime(h, nullptr, nullptr, &ft);
  CloseHandle(h);
}
void AgeDir(const std::wstring& d, uint64_t ageSec) {
  HANDLE h = CreateFileW(d.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  FILETIME ft;
  const uint64_t t = prune::NowTicks() - ageSec * 10000000ull;
  ft.dwLowDateTime = static_cast<DWORD>(t);
  ft.dwHighDateTime = static_cast<DWORD>(t >> 32);
  SetFileTime(h, nullptr, nullptr, &ft);
  CloseHandle(h);
}
bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
void RemoveTree(const std::wstring& dir) {  // test cleanup (two levels)
  for (const prune::Entry& e : prune::List(dir, L"*", true)) {
    for (const prune::Entry& f : prune::List(e.path + L"\\", L"*", true)) prune::RemoveDumpDir(f.path);
    prune::RemoveDumpDir(e.path);
  }
  for (const prune::Entry& f : prune::List(dir, L"*", false)) DeleteFileW(f.path.c_str());
  RemoveDirectoryW(dir.c_str());
}

void Prune() {
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring root = std::wstring(tmp) + L"r90_prune_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
  const std::wstring dev = root + L"dev\\";
  CreateDirectoryW(root.c_str(), nullptr);
  CreateDirectoryW(dev.c_str(), nullptr);
  CreateDirectoryW((root + L"cache").c_str(), nullptr);
  CreateDirectoryW((root + L"tone_probe").c_str(), nullptr);
  CreateDirectoryW((dev + L"cockpit_frames").c_str(), nullptr);
  for (int i = 0; i < 30; ++i) MakeFile(root + L"report_" + std::to_wstring(1000 + i) + L".txt", 10, 1000 - i);
  for (int i = 0; i < 25; ++i) MakeFile(root + L"bench_" + std::to_wstring(1000 + i) + L".csv", 10, 1000 - i);
  for (const wchar_t* keep : {L"DcsQvCull.log", L"DcsQvCull.prev.log", L"DcsQvCull.4242.log", L"DcsQvCullPrefetch.log",
                              L"DcsQvCull.ini", L"run_suite.flag"})
    MakeFile(root + keep, 10, 99999);
  for (int i = 0; i < 5; ++i) {
    const std::wstring d = root + L"tone_probe\\" + std::to_wstring(i);
    CreateDirectoryW(d.c_str(), nullptr);
    MakeFile(d + L"\\hdr.f32", 100, 0);
    AgeDir(d, 100 - i);
    const std::wstring c = dev + L"cockpit_frames\\" + std::to_wstring(i);
    CreateDirectoryW(c.c_str(), nullptr);
    MakeFile(c + L"\\frames.csv", 100, 0);
    AgeDir(c, 100 - i);
  }
  for (int i = 0; i < 5000; ++i) MakeFile(root + L"cache\\s_" + std::to_wstring(i) + L".qvc", 16, 90ull * 86400 + i);
  for (int i = 0; i < 10; ++i) MakeFile(root + L"cache\\g_hot" + std::to_wstring(i) + L".qvc", 16, 3600);
  MakeFile(root + L"cache\\s_x.qvc.123.tmp", 16, 7200);
  MakeFile(root + L"cache\\s_y.qvc.456.tmp", 16, 60);
  MakeFile(dev + L"gbrec_states.jsonl", (4u << 20) + 1, 10);
  const prune::Stats st = prune::RunAtStart(root, dev);
  bool kept = true;
  for (const wchar_t* keep : {L"DcsQvCull.log", L"DcsQvCull.prev.log", L"DcsQvCull.4242.log", L"DcsQvCullPrefetch.log",
                              L"DcsQvCull.ini", L"run_suite.flag"})
    kept &= Exists(root + keep);
  Check(kept, "R90 s7: the logs (main, .prev, <pid>, prefetch), the ini and the flags are never touched");
  Check(prune::List(root, L"report_*.txt", false).size() == prune::kKeepFiles && Exists(root + L"report_1029.txt") &&
            !Exists(root + L"report_1000.txt") && prune::List(root, L"bench_*.csv", false).size() == prune::kKeepFiles,
        "R90 s7: report_*.txt and bench_*.csv stay at the module root, the newest 20 kept");
  Check(prune::List(root + L"tone_probe\\", L"*", true).size() == prune::kKeepDumpDirs &&
            Exists(root + L"tone_probe\\4") && !Exists(root + L"tone_probe\\0") &&
            prune::List(dev + L"cockpit_frames\\", L"*", true).size() == prune::kKeepDumpDirs,
        "R90 s7: dump folders (module and dev folder) keep the newest 3");
  bool hot = true;
  for (int i = 0; i < 10; ++i) hot &= Exists(root + L"cache\\g_hot" + std::to_wstring(i) + L".qvc");
  const size_t qvc = prune::List(root + L"cache\\", L"*.qvc", false).size();
  Check(hot && qvc <= prune::kQvcCapFiles * 3 / 4 && Exists(root + L"cache\\s_0.qvc") &&
            !Exists(root + L"cache\\s_4999.qvc"),
        "R90 s7: cache\\*.qvc past 4000 files pruned to 75 %, least recently used first; entries used in the last 30 "
        "days kept");
  Check(!Exists(root + L"cache\\s_x.qvc.123.tmp") && Exists(root + L"cache\\s_y.qvc.456.tmp"),
        "R90 s7: an orphaned cache .tmp older than 1 h is deleted, a fresh one (a write in progress) kept");
  Check(Exists(dev + L"gbrec_states.prev.jsonl") && !Exists(dev + L"gbrec_states.jsonl"),
        "R90 s7: gbrec_states.jsonl past 4 MiB rotates to gbrec_states.prev.jsonl");
  // Touch on hit: an old entry becomes the newest.
  WIN32_FILE_ATTRIBUTE_DATA a{};
  prune::TouchOnHit(root + L"cache\\s_100.qvc");
  GetFileAttributesExW((root + L"cache\\s_100.qvc").c_str(), GetFileExInfoStandard, &a);
  Check(prune::NowTicks() - prune::Ticks(a.ftLastWriteTime) < 600000000ull,
        "R90 s7: a cache hit stamps the entry's last use (last-write time)");
  printf("     (R90 s7: pruned %u files, %u folders, %.1f MB; %zu .qvc left)\n", st.files, st.dirs,
         st.bytes / 1048576.0, qvc);
  {  // R115 P7: pruned dump folders count their files' bytes; a folder that stays still counts what went.
    const std::wstring dd = root + L"p7_dumps\\";
    CreateDirectoryW(dd.c_str(), nullptr);
    const size_t sizes0[3] = {1000, 2000, 3000};
    for (int i = 0; i < 5; ++i) {
      const std::wstring d = dd + std::to_wstring(i);
      CreateDirectoryW(d.c_str(), nullptr);
      if (i == 0) {
        for (int k = 0; k < 3; ++k) MakeFile(d + L"\\f" + std::to_wstring(k) + L".bin", sizes0[k], 0);
      } else if (i == 1) {
        MakeFile(d + L"\\a.bin", 500, 0);
        MakeFile(d + L"\\held.bin", 700, 0);
      } else {
        MakeFile(d + L"\\keep.bin", 64, 0);
      }
      AgeDir(d, 100 - i);
    }
    // Held open without FILE_SHARE_DELETE: it cannot be deleted, so folder 1 cannot be removed.
    HANDLE held = CreateFileW((dd + L"1\\held.bin").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              0, nullptr);
    prune::Stats s7;
    prune::KeepNewest(dd, L"*", prune::kKeepDumpDirs, true, s7);
    const bool heldStays = held != INVALID_HANDLE_VALUE && Exists(dd + L"1\\held.bin") && !Exists(dd + L"1\\a.bin");
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    printf("     (R115 P7: old dump folders: %u folders, %u files, %llu bytes counted)\n", s7.dirs, s7.files,
           static_cast<unsigned long long>(s7.bytes));
    Check(s7.dirs == 1 && !Exists(dd + L"0") && Exists(dd + L"4") && s7.bytes == 1000 + 2000 + 3000 + 500,
          "R115 P7: a pruned dump folder counts its files' bytes (not the folder entry's 0), so \"disk: pruned ... MB\" "
          "is the real total");
    Check(heldStays && s7.files == 1,
          "R115 P7: a dump folder that cannot be removed (a file held open) still counts the files deleted in it");
  }
  RemoveTree(root + L"cache\\");
  RemoveTree(root + L"tone_probe\\");
  RemoveTree(dev);
  RemoveTree(root);
}

// ---------------------------------------------------------------------------
// H2: shadow_inst g_mutex
// ---------------------------------------------------------------------------

void ShadowInstMutex() {
  using namespace shadowinst;
  bool workers = false;
  for (HANDLE h : g_workers) workers |= h != nullptr;
  if (workers) {
    printf("     (R90 H2: shadow inst compile workers running: inbox test skipped)\n");
    return;
  }
  std::atomic<bool> held{false}, release{false};
  std::thread holder([&] {
    std::lock_guard<std::mutex> lock(g_mutex);
    held = true;
    while (!release.load()) Sleep(1);
  });
  while (!held.load()) Sleep(1);
  Snapshot* s[3];
  const size_t q0 = g_queue.size();
  const uint64_t t0 = qvpatch::NowUs();
  for (int i = 0; i < 3; ++i) {
    s[i] = new Snapshot;
    memset(s[i], 0, sizeof(Snapshot));
    s[i]->shader = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + i));
    InboxPush(s[i]);
  }
  const uint64_t pushUs = qvpatch::NowUs() - t0;
  const bool waiting = g_inboxCount.load() == 3;
  // VS objects created without g_mutex (it is held by the other thread meanwhile).
  bool created = false;
  ID3D11Device* dev = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (chainfast::LoadCompiler() &&
      SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                                  nullptr))) {
    const char* src = "float4 main(float4 p : POSITION) : SV_Position { return p; }";
    ID3DBlob* code = nullptr;
    if (SUCCEEDED(chainfast::g_compile(src, strlen(src), "r90", nullptr, nullptr, "main", "vs_5_0", 0, 0, &code,
                                       nullptr)) &&
        code) {
      KeyResult r;
      r.ok = true;
      r.vs.resize(1);
      const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
      r.vs[0].bytecode.assign(p, p + code->GetBufferSize());
      code->Release();
      CreateVsOn(dev, r);
      created = r.vs[0].vs != nullptr;
      if (r.vs[0].vs) r.vs[0].vs->Release();
    }
    dev->Release();
  }
  release = true;
  holder.join();
  bool fifo = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    DrainInboxLocked();
    fifo = g_queue.size() == q0 + 3 && g_queue[q0] == s[0] && g_queue[q0 + 1] == s[1] && g_queue[q0 + 2] == s[2] &&
           g_inboxCount.load() == 0;
    g_queue.resize(q0);
  }
  for (Snapshot* x : s) delete x;
  printf("     (R90 H2: 3 render-thread queue pushes with g_mutex held elsewhere took %llu us)\n",
         static_cast<unsigned long long>(pushUs));
  Check(waiting && pushUs < 5000 && fifo,
        "R90 H2 shadow inst: the render thread's queue push never waits for g_mutex; the snapshots reach the queue "
        "in arrival order");
  Check(created, "R90 H2 shadow inst: a key's VS objects are created without g_mutex (held by another thread)");
}

void Run() {
  Patch();
  Placement();
  AsyncLog();
  FastReadback();
  LuaHook();
  Prune();
  ShadowInstMutex();
}

}  // namespace r90
