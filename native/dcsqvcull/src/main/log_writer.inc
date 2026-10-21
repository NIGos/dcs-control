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
HANDLE g_logWriter = nullptr;
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
  g_logWriter = CreateThread(nullptr, 0, qvexit::Thread<LogWriterThread>, nullptr, 0, nullptr);
  qvalog::g_on = g_logWriter && qvalog::g_want.load();
}
// Every later line is synchronous; the queued ones are written first (in order).
void StopAsyncLog() {
  qvalog::g_on = false;
  if (g_logWriter) {
    g_logWriterStop = true;
    if (qvalog::g_wake) SetEvent(qvalog::g_wake);  // R93: out of its wait now
    WaitForSingleObject(g_logWriter, 5000);
    CloseHandle(g_logWriter);
    g_logWriter = nullptr;
  }
  qvalog::Drain();
}
}  // namespace
