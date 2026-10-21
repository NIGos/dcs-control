}  // namespace
void SetBenchVariant(bool on);
void BenchSetEngineOff(bool off);
void RestoreD3dHooks();
#include "exit_guard.h"  // DCS shutdown: quiesce, late faults on our threads end the thread only
#include "patch_threads.h"  // R90: this process's threads without a system-wide snapshot; suspended code writes
#include "disk_prune.h"     // R90: output caps at payload start (newest-N families, .qvc LRU, orphaned .tmp)
namespace {
// R90: the log writer thread (qvalog above). Started by DcsQvPayload_Start, stopped (and the ring drained) by
// Stop and Quiesce.
// R115 (L3): g_logWriter and qvalog::g_on change only under this lock: Start / Stop set g_on from whether their writer
// runs, and a config reload (ApplyR90Config, the worker thread) only records [Log] Async and turns g_on on / off while
// a writer runs (SetAsyncLogWanted), so a reload racing Stop can no longer leave the ring on with no writer.
std::mutex g_logWriterMutex;
HANDLE g_logWriter = nullptr;  // under g_logWriterMutex
std::atomic<bool> g_logWriterStop{false};
DWORD WINAPI LogWriterThread(void*) {
  while (!g_logWriterStop.load(std::memory_order_acquire)) {
    if (!qvalog::g_testHold.load(std::memory_order_relaxed)) qvalog::Drain();
    if (qvalog::g_wake)
      WaitForSingleObject(qvalog::g_wake, 10);  // R93: a thread waiting for ring space wakes the writer at once
    else
      Sleep(10);
  }
  qvalog::Drain();
  return 0;
}
void StartAsyncLog() {
  std::lock_guard<std::mutex> lw(g_logWriterMutex);
  if (g_logWriter) return;
  if (g_api && !qvalog::g_logAt) {  // the loader's DcsQvCull_LogAt (R90 loaders): lines keep their call time
    HMODULE loader = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(g_api->log), &loader) &&
        loader)
      qvalog::g_logAt = reinterpret_cast<qvalog::LogAtFn>(GetProcAddress(loader, "DcsQvCull_LogAt"));
  }
  g_logWriterStop = false;
  if (!qvalog::g_wake) qvalog::g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // R93 (kept for the payload)
  qvalog::g_stallDeq = -1;  // R115 (L1): no stall latched from an earlier writer
  g_logWriter = CreateThread(nullptr, 0, qvexit::Thread<LogWriterThread>, nullptr, 0, nullptr);
  qvalog::g_on = g_logWriter && qvalog::g_want.load();
}
// R115 (L3): [Log] Async from a config read (any thread). Records the wish; g_on follows it only while a writer runs.
void SetAsyncLogWanted(bool want) {
  std::lock_guard<std::mutex> lw(g_logWriterMutex);
  qvalog::g_want = want;
  qvalog::g_on = g_logWriter && want;
}
// Every later line is synchronous; the queued ones are written first (in order).
void StopAsyncLog() {
  {
    // R115 (L3): held until the writer is gone (it never takes this lock), so a reload cannot turn g_on back on.
    std::lock_guard<std::mutex> lw(g_logWriterMutex);
    qvalog::g_on = false;
    if (g_logWriter) {
      g_logWriterStop = true;
      if (qvalog::g_wake) SetEvent(qvalog::g_wake);  // R93: out of its wait now
      WaitForSingleObject(g_logWriter, 5000);
      CloseHandle(g_logWriter);
      g_logWriter = nullptr;
    }
  }
  qvalog::Drain();
}
}  // namespace
