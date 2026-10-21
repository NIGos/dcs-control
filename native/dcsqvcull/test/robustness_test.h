// Offline regression tests for the 2026-10-11 robustness round: payload stop
// ordering (worker joined before the teardown), UTF-8 log formatting of
// non-ASCII paths, and the per-fix checks added with them.
#pragma once

namespace rbtest {

// log_utf8.h: a Cyrillic path no longer empties the log line; long lines are
// truncated and terminated.
void LogUtf8() {
  char buf[128];
  memset(buf, 'X', sizeof(buf));
  qvlog::Format(buf, sizeof(buf), "path %ls end", L"C:/Users/\u0416\u0435\u043d\u044f/x");
  const char want[] = "path C:/Users/\xd0\x96\xd0\xb5\xd0\xbd\xd1\x8f/x end";
  Check(strcmp(buf, want) == 0, "log utf8: a Cyrillic %ls path is written as UTF-8 (was an empty line)");
  memset(buf, 'X', sizeof(buf));
  qvlog::Format(buf, sizeof(buf), "%ls", L"J\u00f6rg");
  Check(strcmp(buf, "J\xc3\xb6rg") == 0, "log utf8: U+0080-U+00FF as UTF-8, not Latin-1");
  char tiny[8];
  memset(tiny, 'X', sizeof(tiny));
  qvlog::Format(tiny, sizeof(tiny), "%s", "0123456789abcdef");
  Check(memchr(tiny, 0, sizeof(tiny)) != nullptr && strlen(tiny) == 7,
        "log utf8: a truncated line is NUL-terminated");
  memset(tiny, 'X', sizeof(tiny));
  qvlog::Format(tiny, sizeof(tiny), "%ls", L"\xd800 lone surrogate");
  Check(memchr(tiny, 0, sizeof(tiny)) != nullptr, "log utf8: an unconvertible %ls leaves a terminated line");
}

// main.cpp StopWorkerThreads: Stop waits for the worker to leave its loop
// (it may be inside an Install) before the features are torn down.
void StopJoinsWorker() {
  static std::atomic<bool> finished{false};
  finished = false;
  g_stop = false;
  g_benchAbort = false;
  // A worker that, once it sees g_stop, still finishes an "install" (150 ms).
  HANDLE t = CreateThread(
      nullptr, 0,
      [](void*) -> DWORD {
        while (!g_stop.load()) Sleep(1);
        Sleep(150);
        finished = true;
        return 0;
      },
      nullptr, 0, nullptr);
  g_workerThread = t;
  const bool idle = StopWorkerThreads(60000);  // generous: only a hang would reach it
  Check(idle && finished.load() && g_workerThread == nullptr,
        "payload stop: the worker is joined before the teardown (it was not waited for)");
  Check(g_benchAbort.load(), "payload stop: a running suite is told to abort");

  // A worker that never stops: the wait is bounded and reported.
  static std::atomic<bool> release{false};
  release = false;
  g_stop = false;
  HANDLE stuck = CreateThread(
      nullptr, 0,
      [](void*) -> DWORD {
        while (!release.load()) Sleep(1);
        return 0;
      },
      nullptr, 0, nullptr);
  g_workerThread = stuck;
  const ULONGLONG t0 = GetTickCount64();
  const bool idle2 = StopWorkerThreads(200);
  const ULONGLONG waited = GetTickCount64() - t0;
  Check(!idle2 && waited < 60000, "payload stop: a stuck worker does not hang the stop (bounded wait)");
  release = true;
  WaitForSingleObject(stuck, 60000);
  CloseHandle(stuck);
  g_workerThread = nullptr;
  g_stop = false;
  g_benchAbort = false;
}

// shadow_rec.h / gb_rec.h JobMain: a start that won the exchange but has not
// published j.vec yet (StartJob preempted between its exchange and its store)
// must not be built on: before the fix the worker read j.vec[0] from null, the
// pool faulted and latched off for the session.
template <class JobT, class NewFn, class FreeFn, class MainFn>
bool StartRaceOnce(defrec::Pool& pool, NewFn newJob, FreeFn freeJob, MainFn jobMain, int runState) {
  JobT* j = newJob();
  if (!j) return false;
  j->vec = nullptr;
  j->startState.store(runState);  // as if StartJob won the start and was preempted before `vec = ...`
  const bool sub = pool.Submit(0, jobMain, j, nullptr);
  pool.Wait(60000);  // the worker returns after its 250 + 100 ms waits; generous for a loaded machine
  const bool clean = sub && !pool.Busy(0) && pool.At(0).faultCode == 0 && !pool.Disabled();
  j->startState.store(0);
  freeJob(j);
  return clean;
}

void RecorderStartRace() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &imm))) {
    QvSkip("recorder start race: no WARP device\n");
    return;
  }
  defrec::PoolConfig pc;
  pc.workers = 1;
  pc.name = "start race test";
  {
    defrec::Pool pool;
    const bool ok = pool.Start(dev, pc) &&
                    StartRaceOnce<shrec::Job>(pool, &shrec::NewJob, [](shrec::Job* j) { shrec::FreeJob(j); },
                                              &shrec::JobMain, shrec::kStartRun);
    pool.Stop();
    Check(ok, "shadow rec start race: a started job whose vector is not yet published is dropped, no fault/latch");
  }
  {
    defrec::Pool pool;
    const bool ok = pool.Start(dev, pc) &&
                    StartRaceOnce<gbrec::Job>(pool, &gbrec::NewJob, [](gbrec::Job* j) { gbrec::FreeJob(j); },
                                              &gbrec::JobMain, gbrec::kStartRun);
    pool.Stop();
    Check(ok, "gbuffer rec start race: a started job whose vector is not yet published is dropped, no fault/latch");
  }
  // gb_rec.h Shutdown: a pass still inside after the drain wait keeps the
  // pool running (it may be in TakeList/Wait); it used to be torn down under
  // the render thread. The objects stay alive and Shutdown reports it.
  {
    pc.name = "shutdown busy test";
    const bool started = gbrec::g_pool.Start(dev, pc);
    gbrec::g_state = 1;
    gbrec::g_inside = 1;  // a pass that never leaves within the 2 s wait
    const bool released = gbrec::Shutdown();
    const bool kept = gbrec::g_pool.Started();
    gbrec::g_inside = 0;
    gbrec::g_pool.Stop();
    gbrec::g_state = 0;
    gbrec::g_shutdown = false;
    Check(started && !released && kept,
          "gbuffer rec shutdown: a pass still inside keeps the worker pool alive (no teardown under it)");
  }
  imm->Release();
  dev->Release();
}

// main.cpp ProcessImageDir: DCS's bin folder comes from the process image
// (the configuration report used a hard-coded install path).
void ImageDir() {
  const std::wstring d = ProcessImageDir();
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::wstring e = exe;
  Check(!d.empty() && d.back() == L'\\' && e.compare(0, d.size(), d) == 0 &&
            e.find_first_of(L"\\/", d.size()) == std::wstring::npos,
        "image dir: the folder of the process image, with trailing backslash");
}

// shadow_inst.h MarkSeen: forgotten shaders' slots are reused, and a full set
// answers at once instead of scanning 4096 slots per caster.
void SeenSetReuse() {
  using namespace shadowinst;
  std::vector<void*> saved(kSeenSize);
  for (size_t i = 0; i < kSeenSize; ++i) saved[i] = g_seen[i].load();
  const bool savedFull = g_seenFull[0].load();
  for (auto& s : g_seen) s.store(nullptr);
  g_seenFull[0] = false;
  auto fake = [](size_t k) { return reinterpret_cast<void*>(0x10000000ull + k * 0x40); };
  bool ok = true;
  for (size_t k = 0; k < kSeenSize; ++k) ok = ok && MarkSeen(fake(k));
  ok = ok && !MarkSeen(fake(5)) && !MarkSeen(fake(kSeenSize)) && g_seenFull[0].load();
  // Full: answered from the flag (no 4096-slot scan per caster); checked by
  // the flag itself rather than by timing.
  ok = ok && !MarkSeen(fake(kSeenSize + 1)) && g_seenFull[0].load();
  // ForgetShader's effect on one entry: tombstone + flag cleared.
  for (auto& s : g_seen)
    if (s.load() == fake(7)) s.store(reinterpret_cast<void*>(uintptr_t{1}));
  g_seenFull[0] = false;
  ok = ok && MarkSeen(fake(kSeenSize)) && !MarkSeen(fake(kSeenSize)) && !MarkSeen(fake(6));
  for (size_t i = 0; i < kSeenSize; ++i) g_seen[i].store(saved[i]);
  g_seenFull[0] = savedFull;
  Check(ok, "shadow inst seen set: forgotten slots are reused; a full set is not rescanned per caster");
}

// par_upload.h: (1) a size whose third is a multiple of 64 plus 1-2 bytes
// copies its last bytes; (2) with no helper running (stopped by a payload
// stop between the wake and the wait) the update completes instead of
// spinning forever.
void ParUploadBounds() {
  void* vt[8] = {};
  vt[3] = reinterpret_cast<void*>(&putest::FakeMap);
  vt[4] = reinterpret_cast<void*>(&putest::FakeUnmap);
  struct Obj {
    void** vtbl;
    uint8_t pad[0x3c];
    int32_t mode;
  } obj{vt, {}, 3};
  parupload::g_orig = &putest::FakeOrig;
  const size_t sz = 3u * 64u * 3000u + 2u;  // floor(sz / 3) = 64 * 3000: 3 chunks are 2 bytes short
  std::vector<uint8_t> src(sz);
  for (size_t i = 0; i < sz; ++i) src[i] = static_cast<uint8_t>(i * 13 + 5);
  bool tail = parupload::StartHelpers();
  parupload::g_on = true;
  putest::g_mapped.assign(sz + 64, 0xEE);
  parupload::Hook(&obj, 0, src.data(), static_cast<int32_t>(sz));
  tail = tail && memcmp(putest::g_mapped.data(), src.data(), sz) == 0 && putest::g_mapped[sz] == 0xEE;
  parupload::StopHelpers();
  Check(tail, "parallel upload: the last 1-2 bytes of a size just above 3 x 64k are copied");
  // No helpers (stopped): the queued chunks are taken back after the bound.
  parupload::g_stop = false;
  putest::g_mapped.assign(sz + 64, 0xEE);
  putest::g_unmaps = 0;
  parupload::Hook(&obj, 0, src.data(), static_cast<int32_t>(sz));  // returning at all is the check
  const bool done = memcmp(putest::g_mapped.data(), src.data(), sz) == 0 && putest::g_unmaps == 1;
  for (auto& j : parupload::g_jobs) j.done.store(1);
  parupload::g_on = false;
  parupload::g_orig = nullptr;
  Check(done, "parallel upload: without running helpers the update completes (no unbounded render-thread spin)");
}

// shadow_rec.h TexBuildRaw: a fault while copying a mip set's thresholds
// (after its view vector) must leave no unreferenced view in the entry; the
// entry's release used to drop DCS's own references.
void TexBuildFault() {
  using namespace grtest;
  World* w = new World;
  w->Init();
  FakeView fv[8];
  FakeTex* ft = new FakeTex;
  ft->Init(*w);
  for (int k = 0; k < 6; ++k) ft->views[k] = &fv[k];
  ft->SetAreas(4, 4);
  FakeTex::SetReady(ft->S, true);
  // The thresholds vector points at unmapped memory: the second copy faults.
  *reinterpret_cast<void**>(ft->S + 0x28) = reinterpret_cast<void*>(0x1000);
  *reinterpret_cast<void**>(ft->S + 0x30) = reinterpret_cast<void*>(0x1010);
  TexTable tt;
  AllocTex(tt, 64);
  bool fresh = false;
  TexEntry* e = TexInsert(tt, ft->t, -1, kRecType, &fresh);
  bool ok = e && shrec::TexBuildGuarded(w->env.tex, *e) == shrec::kTwFault && !e->usable;
  for (int k = 0; ok && k < shrec::kTexViews; ++k) ok = e->views[k] == nullptr;
  FreeTex(tt);
  for (int k = 0; k < 8; ++k) ok = ok && fv[k].refs == 1;
  Check(ok, "shadow rec texture build: a fault mid-build leaves no unreferenced view (DCS's references intact)");
  delete ft;
  delete w;
}

// gb_rec.h GbFilter: our own C++ exceptions (bad_alloc) are contained at the
// entry's level; DCS's (raised above it) are left to DCS.
void GbFilterCpp() {
  const int lvl = gbrec::t_dcs;
  const bool own = gbrec::GbFilter(0xE06D7363, lvl) == EXCEPTION_EXECUTE_HANDLER;
  const bool dcs = gbrec::GbFilter(0xE06D7363, lvl - 1) == EXCEPTION_CONTINUE_SEARCH;
  bool caught = false;
  __try {
    RaiseException(0xE06D7363, 0, 0, nullptr);
  } __except (gbrec::GbFilter(GetExceptionCode(), lvl)) {
    caught = true;
  }
  Check(own && dcs && caught, "gbuffer rec fault filter: a C++ exception of our own code is contained, DCS's is not");
}

void Run() {
  ImageDir();
  SeenSetReuse();
  ParUploadBounds();
  TexBuildFault();
  GbFilterCpp();
  LogUtf8();
  StopJoinsWorker();
  RecorderStartRace();
}

}  // namespace rbtest
