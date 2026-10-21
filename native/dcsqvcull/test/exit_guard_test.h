// Field report 0.5.5 (GitHub NIGos/dcs-control#1): both crashes replayed offline.
//  - Crash 2, exit: DCS's Lua frees the loader at exit while the payload's
//    worker still publishes the status word into the loader's data (AV at
//    payload+0x136D1A, WorkerThread -> Sink::Publish). Covered by: the loader
//    pins itself (module_pin.h), the Lua exit finalizer quiesces the payload
//    (QuiesceForExit), and every payload thread runs under qvexit::Run.
//  - Crash 1, mission restart: a G-buffer recorder worker read [item+0xd4]
//    (RecordRange -> BuildWindow) after DCS freed the mission's objects.
//    Covered by the frame-heap generation (and now the mission events that
//    bump it, and the recorders staying off without the reset hooks).
#pragma once

#include "../src/module_pin.h"

namespace extest {

// ---- crash 2: pinning ----
void PinSurvivesFreeLibrary() {
  // A system DLL this process has not loaded yet (skipped if all are loaded).
  const wchar_t* candidates[] = {L"dciman32.dll", L"mscms.dll", L"msimg32.dll", L"icmui.dll"};
  for (const wchar_t* name : candidates) {
    if (GetModuleHandleW(name)) continue;
    HMODULE m = LoadLibraryW(name);
    if (!m) continue;
    // Without the pin: one FreeLibrary unmaps it (what Lua's loadlib finalizer did to the loader).
    FreeLibrary(m);
    const bool unmappedBefore = GetModuleHandleW(name) == nullptr;
    m = LoadLibraryW(name);
    if (!m) continue;
    const bool pinned = qvpin::PinModuleOf(reinterpret_cast<const uint8_t*>(m) + 0x1000);
    for (int i = 0; i < 4; ++i) FreeLibrary(m);
    Check(unmappedBefore && pinned && GetModuleHandleW(name) != nullptr,
          "exit: a pinned module stays mapped after FreeLibrary (Lua's loadlib finalizer can no longer unmap the "
          "loader under the payload)");
    return;
  }
  QvSkip("exit pin: every candidate DLL already loaded\n");
}

// ---- crash 2: the status-word race ----
// The loader's data page (the status word) and a payload thread publishing
// to it about every millisecond, as WorkerThread does every second.
struct Race {
  uint8_t* page = nullptr;
  qvstatus::Sink sink;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> published{0};
};
Race* g_race = nullptr;

DWORD WINAPI PublisherBody(void*) {
  Race& r = *g_race;
  while (!r.stop.load() && !qvexit::Exiting()) {
    r.sink.Publish(0x1234);
    r.published.fetch_add(1);
    Sleep(1);
  }
  return 0;
}

void StatusWordRace() {
  const uint64_t contained0 = qvexit::g_contained.load();
  // (a) The 0.5.5 order: the loader is unmapped while the thread runs. Without
  // the guard this was the field AV; now the thread ends and the process lives.
  {
    Race r;
    g_race = &r;
    r.page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    r.sink.Attach(reinterpret_cast<std::atomic<uint64_t>*>(r.page + 0x50));
    HANDLE t = CreateThread(nullptr, 0, qvexit::Thread<PublisherBody>, nullptr, 0, nullptr);
    while (r.published.load() < 5) Sleep(1);
    VirtualFree(r.page, 0, MEM_RELEASE);  // "FreeLibrary(DcsQvCull.dll)"
    const bool ended = WaitForSingleObject(t, 10000) == WAIT_OBJECT_0;
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    CloseHandle(t);
    Check(ended && code == qvexit::kFaultExit && qvexit::g_contained.load() == contained0 + 1,
          "exit race: the worker's write into an unmapped loader page ends that thread only (was the 0.5.5 exit AV)");
    g_race = nullptr;
  }
  // (b) The new order: the Lua exit finalizer quiesces first, then the module
  // may go: the thread has left its loop before the page disappears.
  {
    Race r;
    g_race = &r;
    r.page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    r.sink.Attach(reinterpret_cast<std::atomic<uint64_t>*>(r.page + 0x50));
    HANDLE t = CreateThread(nullptr, 0, qvexit::Thread<PublisherBody>, nullptr, 0, nullptr);
    while (r.published.load() < 5) Sleep(1);
    const bool first = qvexit::Begin();
    const bool second = qvexit::Begin();
    const bool ended = WaitForSingleObject(t, 10000) == WAIT_OBJECT_0;
    r.sink.Detach();
    VirtualFree(r.page, 0, MEM_RELEASE);
    DWORD code = 1;
    GetExitCodeThread(t, &code);
    CloseHandle(t);
    Check(first && !second && ended && code == 0 && qvexit::g_contained.load() == contained0 + 1,
          "exit race: quiesced first, the thread leaves its loop before the loader goes (no fault at all)");
    qvexit::g_exiting = false;
    g_race = nullptr;
  }
}

// A fault that is not a hardware fault (a C++ throw would be one) is not
// swallowed by the thread guard; the containable set is the late-read set.
void GuardScope() {
  Check(qvexit::Containable(EXCEPTION_ACCESS_VIOLATION) && qvexit::Containable(EXCEPTION_IN_PAGE_ERROR) &&
            qvexit::Containable(EXCEPTION_ILLEGAL_INSTRUCTION) && !qvexit::Containable(EXCEPTION_BREAKPOINT) &&
            !qvexit::Containable(EXCEPTION_STACK_OVERFLOW) && !qvexit::Containable(0xE06D7363u),
        "exit guard: only hardware faults of freed memory or unloaded code end a thread quietly");
  // The fault line names where it happened ("payload+0x..." for our code).
  char where[160] = {};
  qvexit::Where(reinterpret_cast<const void*>(&GuardScope), where, sizeof(where));
  Check(strstr(where, "payload+0x") != nullptr, "exit guard: our own addresses are logged as payload+offset");
  char none[160] = {};
  uint8_t* gone = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS));
  VirtualFree(gone, 0, MEM_RELEASE);
  qvexit::Where(gone, none, sizeof(none));
  Check(strstr(none, "no module") != nullptr, "exit guard: an address in no module (an unloaded DLL) says so");
}

// ---- crash 2: the payload's exit quiesce ----
void PayloadQuiesce() {
  static std::atomic<int> loops{0};
  loops = 0;
  g_stop = false;
  g_benchAbort = false;
  const bool refresher = d3ds::g_stopRefresher.load();
  // The worker stand-in: loops like WorkerThread (g_stop, Exiting) and reads "DCS" every pass.
  HANDLE t = CreateThread(
      nullptr, 0,
      [](void*) -> DWORD {
        while (!g_stop.load() && !qvexit::Exiting()) {
          loops++;
          Sleep(2);
        }
        return 0;
      },
      nullptr, 0, nullptr);
  g_workerThread = t;
  while (loops.load() < 3) Sleep(1);
  std::atomic<uint64_t> word{0};
  g_statusSink.Attach(&word);
  g_statusSink.Publish(qvstatus::kRunning);
  const uint64_t gen0 = g_frameHeapGen.load();
  const bool idle = QuiesceForExit(10000);
  const int after = loops.load();
  Sleep(20);
  Check(idle && g_workerThread == nullptr && loops.load() == after && qvexit::Exiting() && g_stop.load() &&
            g_benchAbort.load(),
        "exit quiesce: the worker has left its loop when the Lua exit callback returns (no more ticks into DCS)");
  Check(g_frameHeapGen.load() == gen0 + 1 && word.load() == 0 && d3ds::g_stopRefresher.load(),
        "exit quiesce: recorder jobs in flight become stale, the status word reads 'not running', the refresher stops");
  g_statusSink.Publish(qvstatus::kRunning);
  Check(word.load() == 0, "exit quiesce: the payload never writes the loader's status word again");
  Check(QuiesceForExit(10) && DcsQvPayload_Quiesce(10) == 1, "exit quiesce: a second call is a no-op");
  // Back to a running payload for the remaining tests.
  qvexit::g_exiting = false;
  g_stop = false;
  g_benchAbort = false;
  d3ds::g_stopRefresher = refresher;
}

// ---- crash 1: mission restart while a recorder job is in flight ----
int ReadUnder(gbrec::Job& j, volatile int* p, bool* handled) {
  __try {
    return *p;
  } __except ((*handled = gbrec::RecycledFilter(GetExceptionCode(), j) == EXCEPTION_EXECUTE_HANDLER) ? 1 : 0) {
    return -1;
  }
}

void MissionRestart() {
  using namespace gbrec;
  Job* j = NewJob();
  if (!j) {
    QvSkip("exit mission restart: no job memory\n");
    return;
  }
  // The job's renderable -> item (persistent mission object, not on the frame heap).
  uint8_t* item = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  j->heapGen = g_frameHeapGen.load();
  bool handled = false;
  const int live = ReadUnder(*j, reinterpret_cast<volatile int*>(item + 0xd4), &handled);
  // DCS stops the simulation and frees the mission: no frame renders, so no
  // frame-heap reset. Before, the job's generation still matched and the
  // late read latched the recorder off ("disabled for this session").
  const uint64_t faults0 = g_recycledFaults.load();
  Check(!HeapRecycled(*j), "mission restart: a job of the last frame is current until DCS says otherwise");
  DcsQvPayload_MissionEvent(1);  // onSimulationStop through the loader
  VirtualFree(item, 0, MEM_RELEASE);
  bool handled2 = false;
  const int late = ReadUnder(*j, reinterpret_cast<volatile int*>(item + 0xd4), &handled2);
  Check(live == 0 && !handled && HeapRecycled(*j) && late == -1 && handled2 && g_recycledFaults.load() == faults0 + 1,
        "mission restart: after onSimulationStop a late read of a freed item is contained per job (0.5.5: the "
        "recorder was disabled for the session)");
  shrec::Job* sj = shrec::NewJob();
  if (sj) {
    sj->heapGen = g_frameHeapGen.load();
    DcsQvPayload_MissionEvent(2);  // onMissionLoadBegin
    Check(shrec::HeapRecycled(*sj) &&
              shrec::RecycledFilter(EXCEPTION_ACCESS_VIOLATION, *sj) == EXCEPTION_EXECUTE_HANDLER,
          "mission restart: the next mission's load makes the shadow recorder's jobs stale too");
    shrec::g_recycledFaults = 0;
    shrec::FreeJob(sj);
  }
  // The field case: the local player changed slot (21:54:13.258), the
  // G-buffer executions moved to a new render graph 166 ms later, and a job
  // started on that graph's frames (its generation current) faulted 66 ms
  // after: DCS was still freeing the previous unit's objects.
  {
    const double us = defrec::QpcToUs();
    const int64_t change = 1000000;
    auto at = [&](double ms) { return change + static_cast<int64_t>(ms * 1000.0 / us); };
    Check(shrec::SceneChangingAt(at(66), change, us) && shrec::SceneChangingAt(at(-400), change, us) &&
              !shrec::SceneChangingAt(at(5000), change, us) && !shrec::SceneChangingAt(at(-3000), change, us) &&
              !shrec::SceneChangingAt(at(66), 0, us) && !shrec::SceneChangingAt(0, change, us),
          "slot change: jobs started from 1 s before to 2 s after a scene change are in its window");
    const int64_t saved = g_sceneChangeQpc.load();
    j->heapGen = g_frameHeapGen.load();  // current: the heap check alone does not see it
    j->tStart = defrec::Qpc();
    g_sceneChangeQpc = j->tStart - static_cast<int64_t>(66.0 * 1000.0 / us);
    Check(!HeapRecycled(*j) && RecycledFilter(EXCEPTION_ACCESS_VIOLATION, *j) == EXCEPTION_EXECUTE_HANDLER,
          "slot change: a worker fault 66 ms after the render graph moved is contained per job (0.5.5: the "
          "G-buffer recorder was disabled for the session)");
    g_sceneChangeQpc = j->tStart - static_cast<int64_t>(10000.0 * 1000.0 / us);
    Check(RecycledFilter(EXCEPTION_ACCESS_VIOLATION, *j) == EXCEPTION_CONTINUE_SEARCH,
          "slot change: long after the change a fault on a live heap still latches the recorder (a real bug)");
    g_sceneChangeQpc = saved;
  }
  g_recycledFaults = 0;
  FreeJob(j);
}

// The recorders need the frame-heap reset hooks: without them the generation
// never moves and the recycled-heap check is blind (the 0.5.5 crash path).
void RecordersNeedResetHooks() {
  Check(!RecycleGuardReadyFor(-1) && !RecycleGuardReadyFor(0) && RecycleGuardReadyFor(1),
        "recorders: the recycled-heap guard is ready only with the reset hooks installed");
  govtest::RecSave save;
  g_cfg.shadowRecorder = true;
  g_cfg.gbufferRecorder = true;
  g_cfg.shadowInst = true;
  g_cfg.gbufferBatch = false;
  shrec::g_state = 1;
  gbrec::g_state = 1;
  shadowinst::g_onGb = true;
  shadowinst::g_gbInstalled = true;
  g_engineOff = false;
  perfmon::SetStage(0);
  frameheap::g_state = -1;  // reset_memory imports not hooked (another DCS build)
  ApplyShadowRecorder();
  ApplyGBufferRecorder();
  const bool off = !shrec::g_on.load() && !gbrec::g_on.load();
  frameheap::g_state = 1;
  ApplyShadowRecorder();
  ApplyGBufferRecorder();
  const bool on = shrec::g_on.load() && gbrec::g_on.load();
  Check(off && on, "recorders: off without the frame-heap reset hooks, on with them");
}

void Run() {
  PinSurvivesFreeLibrary();
  GuardScope();
  StatusWordRace();
  PayloadQuiesce();
  MissionRestart();
  RecordersNeedResetHooks();
}

}  // namespace extest
