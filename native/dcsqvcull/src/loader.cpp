// DcsQvCull loader: loaded once by Saved Games\DCS\Scripts\Hooks\DcsQvCull.lua.
// Loads payload\DcsQvCullPayload.dll and reloads it whenever the file changes
// while DCS runs, so measurements and optimisations can be updated without
// restarting the game. See loader_api.h for the hook registry contract.

#include <windows.h>
#include <share.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include "loader_api.h"
#include "log_utf8.h"
#include "module_pin.h"

namespace {

std::wstring g_dir;
std::mutex g_logMutex;
FILE* g_log = nullptr;

void LogLine(const char* line) {
  std::lock_guard<std::mutex> lock(g_logMutex);
  if (!g_log) return;
  SYSTEMTIME st;
  GetLocalTime(&st);
  fprintf(g_log, "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
  fflush(g_log);
}

void Logf(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  qvlog::FormatV(buf, sizeof(buf), fmt, ap);  // %ls as UTF-8 (log_utf8.h)
  va_end(ap);
  LogLine(buf);
}

// ---- hook registry ----
struct Entry {
  void* original;
  uint32_t generation;  // payload generation that last patched the slot
};
std::mutex g_regMutex;
std::map<void**, Entry> g_registry;
uint32_t g_generation = 0;

bool WriteSlot(void** slot, void* value) {
  DWORD old;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
  InterlockedExchangePointer(slot, value);
  VirtualProtect(slot, sizeof(void*), old, &old);
  return true;
}

// A slot read with SEH and no lock held (no C++ object in this frame): a payload may pass a slot it has not
// verified, and a fault while g_regMutex is held would leave the registry locked for the session (every later
// patch / restore / original lookup, and the next payload's install, would block).
bool ReadSlotSafe(void** slot, void** out) {
  __try {
    *out = *slot;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool ApiPatch(void** slot, void* hook, void** original) {
  void* cur = nullptr;
  if (!ReadSlotSafe(slot, &cur)) return false;  // before the lock: nothing faults while it is held
  std::lock_guard<std::mutex> lock(g_regMutex);
  auto it = g_registry.find(slot);
  if (it == g_registry.end()) it = g_registry.emplace(slot, Entry{cur, 0}).first;
  it->second.generation = g_generation;
  if (original) *original = it->second.original;
  return WriteSlot(slot, hook);
}

void* ApiOriginal(void** slot) {
  {
    std::lock_guard<std::mutex> lock(g_regMutex);
    auto it = g_registry.find(slot);
    if (it != g_registry.end()) return it->second.original;
  }
  void* cur = nullptr;  // not registered: the slot's own value, read after the lock is released
  return ReadSlotSafe(slot, &cur) ? cur : nullptr;
}

void ApiRestore(void** slot) {
  std::lock_guard<std::mutex> lock(g_regMutex);
  auto it = g_registry.find(slot);
  if (it == g_registry.end()) return;
  WriteSlot(slot, it->second.original);
  g_registry.erase(it);
}

// Restores slots the current generation did not patch.
void RestoreStale() {
  std::lock_guard<std::mutex> lock(g_regMutex);
  for (auto it = g_registry.begin(); it != g_registry.end();) {
    if (it->second.generation != g_generation) {
      WriteSlot(it->first, it->second.original);
      it = g_registry.erase(it);
    } else {
      ++it;
    }
  }
}

DcsQvLoaderApi g_api;

// ---- payload management ----
DcsQvPayloadStopFn g_stopActive = nullptr;
// The active payload's optional exit/mission exports (null for older payloads).
std::atomic<DcsQvPayloadQuiesceFn> g_quiesceActive{nullptr};
std::atomic<DcsQvPayloadMissionEventFn> g_missionActive{nullptr};
// DCS is shutting down (DcsQvCull_Shutdown): the watcher loads nothing more.
std::atomic<bool> g_exiting{false};
// Serializes a payload swap (watcher) with the exit quiesce (Lua thread).
std::timed_mutex g_swapMutex;
FILETIME g_payloadTime{};
// Status word (layout in status_word.h): written only by the active payload,
// read by DcsQvCull_Status. Owned here so it outlives every payload.
std::atomic<uint64_t> g_status{0};
// Dual camera status word (bits in loader_api.h): the same plumbing, read by
// DcsQvCull_DualCameraStatus.
std::atomic<uint64_t> g_dualStatus{0};

// LoadPayload results. A copy or LoadLibrary failure is usually transient
// (the file still being written, or held by a scanner without read sharing):
// the watcher retries it instead of waiting for the next change of the file.
enum LoadResult { kLoadOk = 1, kLoadFailed = 0, kLoadRetry = -1 };

LoadResult LoadPayload(const std::wstring& src) {
  // Load a uniquely named copy so the deployed file is never locked.
  wchar_t name[64];
  swprintf_s(name, L"active_%llu.dll", GetTickCount64());
  std::wstring copy = g_dir + L"payload\\" + name;
  if (!CopyFileW(src.c_str(), copy.c_str(), FALSE)) {
    Logf("loader: copy of payload failed (%lu)", GetLastError());
    DeleteFileW(copy.c_str());  // a partial copy, if any
    return kLoadRetry;
  }
  HMODULE m = LoadLibraryW(copy.c_str());
  if (!m) {
    Logf("loader: LoadLibrary of payload failed (%lu)", GetLastError());
    DeleteFileW(copy.c_str());  // not mapped: removable
    return kLoadRetry;
  }
  auto start = reinterpret_cast<DcsQvPayloadStartFn>(GetProcAddress(m, "DcsQvPayload_Start"));
  auto stop = reinterpret_cast<DcsQvPayloadStopFn>(GetProcAddress(m, "DcsQvPayload_Stop"));
  if (!start || !stop) {
    Logf("loader: payload exports missing");
    return kLoadFailed;  // module stays loaded but inert
  }
  // Optional (payloads built before the status word lack it: status stays 0).
  auto setStatus = reinterpret_cast<DcsQvPayloadSetStatusWordFn>(GetProcAddress(m, "DcsQvPayload_SetStatusWord"));
  auto setDual = reinterpret_cast<DcsQvPayloadSetDualCameraStatusWordFn>(
      GetProcAddress(m, "DcsQvPayload_SetDualCameraStatusWord"));
  auto quiesce = reinterpret_cast<DcsQvPayloadQuiesceFn>(GetProcAddress(m, "DcsQvPayload_Quiesce"));
  auto mission = reinterpret_cast<DcsQvPayloadMissionEventFn>(GetProcAddress(m, "DcsQvPayload_MissionEvent"));
  std::unique_lock<std::timed_mutex> swap(g_swapMutex);
  if (g_exiting.load()) {
    Logf("loader: DCS is shutting down; payload not started");
    return kLoadFailed;
  }
  g_quiesceActive.store(nullptr);
  g_missionActive.store(nullptr);
  g_status.store(0, std::memory_order_release);  // not ready during the swap
  g_dualStatus.store(0, std::memory_order_release);
  DcsQvPayloadStopFn previous = g_stopActive;
  if (previous) {
    Logf("loader: stopping previous payload");
    previous();  // its worker threads exit; its hooks stay valid until repointed
    // Its Stop detached it from the status word: it never writes again.
    g_status.store(0, std::memory_order_release);
    g_dualStatus.store(0, std::memory_order_release);
  }
  {
    std::lock_guard<std::mutex> lock(g_regMutex);
    ++g_generation;
  }
  Logf("loader: starting payload %ls (generation %u)", name, g_generation);
  if (setStatus) setStatus(&g_status);
  if (setDual) setDual(&g_dualStatus);
  if (start(&g_api) != 0) Logf("loader: payload start reported an error");
  g_stopActive = stop;
  g_quiesceActive.store(quiesce);
  g_missionActive.store(mission);
  // The new payload patches from its own worker thread; slots it does not
  // claim within 60 s are restored by the watcher (see Watcher()).
  return kLoadOk;
}

DWORD WINAPI Watcher(void*) {
  std::wstring payloadDir = g_dir + L"payload\\";
  CreateDirectoryW(payloadDir.c_str(), nullptr);
  // Remove copies left by earlier sessions (not loaded now).
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((payloadDir + L"active_*.dll").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do DeleteFileW((payloadDir + fd.cFileName).c_str());
    while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  const std::wstring deployed = payloadDir + L"DcsQvCullPayload.dll";
  const std::wstring ini = g_dir + L"DcsQvCull.ini";
  std::wstring lastSrc;
  ULONGLONG staleCheckAt = 0;
  int retries = 0;  // transient load failures of the current file version
  while (!g_exiting.load()) {
    // Dev mode: [Dev] PayloadPath in the ini points at a developer build
    // (for example the build folder), hot-reloaded from there; the deployed
    // payload is used again when the key is cleared or the file is missing.
    wchar_t dev[MAX_PATH] = {};
    GetPrivateProfileStringW(L"Dev", L"PayloadPath", L"", dev, MAX_PATH, ini.c_str());
    std::wstring src = deployed;
    if (dev[0] && GetFileAttributesW(dev) != INVALID_FILE_ATTRIBUTES) src = dev;
    if (src != lastSrc) {
      if (!lastSrc.empty() || src != deployed) Logf("loader: payload source %ls", src.c_str());
      lastSrc = src;
      g_payloadTime = FILETIME{};  // force a load from the new source
      retries = 0;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fad) &&
        CompareFileTime(&fad.ftLastWriteTime, &g_payloadTime) != 0) {
      Sleep(500);  // let the writer finish
      const LoadResult r = LoadPayload(src);
      if (r == kLoadRetry && ++retries < 5) {
        // g_payloadTime left as is: the same version is tried again in 2 s.
        // (Before, a failed copy or load was not retried until the file
        // changed again, which could leave DCS without a payload.)
      } else {
        g_payloadTime = fad.ftLastWriteTime;
        retries = 0;
      }
      if (r == kLoadOk) staleCheckAt = GetTickCount64() + 60000;
    }
    if (staleCheckAt && GetTickCount64() > staleCheckAt) {
      staleCheckAt = 0;
      RestoreStale();
    }
    Sleep(2000);
  }
  return 0;
}

}  // namespace

extern "C" __declspec(dllexport) int luaopen_DcsQvCull(void* /*lua_State*/) { return 0; }

// Lua callbacks, fetched by the hook with package.loadlib (lua_CFunction
// signature; they never touch the Lua state and return no values).
//
// DcsQvCull_Shutdown: DCS is exiting. The hook calls it from the finalizer of
// a userdata it creates after loading this module, so it runs when DCS
// closes the hooks' Lua state, before Lua's loadlib finalizer would
// FreeLibrary this module (0.5.5 field crash: the payload's worker then
// wrote the status word in this module's unmapped data, AV at
// payload+0x136D1A right after "render thread has stopped"). The module is
// pinned anyway (DllMain); this stops the watcher and quiesces the payload
// (its threads stop touching DCS) while DCS's objects still exist.
// Idempotent; bounded (3 s).
extern "C" __declspec(dllexport) int DcsQvCull_Shutdown(void* /*lua_State*/) {
  if (g_exiting.exchange(true)) return 0;
  Logf("loader: DCS is shutting down (Lua state closing); quiescing the payload");
  std::unique_lock<std::timed_mutex> swap(g_swapMutex, std::defer_lock);
  if (!swap.try_lock_for(std::chrono::milliseconds(3000))) {
    Logf("loader: a payload swap is still running; payload not quiesced");
    return 0;
  }
  if (DcsQvPayloadQuiesceFn q = g_quiesceActive.load()) {
    if (!q(3000)) Logf("loader: payload worker still busy after 3 s; left running");
  }
  g_status.store(0, std::memory_order_release);
  Logf("loader: quiesced for exit");
  return 0;
}

// DcsQvCull_SimulationStop / DcsQvCull_MissionLoadBegin: the hook's
// onSimulationStop / onMissionLoadBegin callbacks. DCS is about to free the
// mission's objects; the payload drops recorder jobs in flight.
extern "C" __declspec(dllexport) int DcsQvCull_SimulationStop(void* /*lua_State*/) {
  if (DcsQvPayloadMissionEventFn f = g_missionActive.load()) f(1);
  return 0;
}
extern "C" __declspec(dllexport) int DcsQvCull_MissionLoadBegin(void* /*lua_State*/) {
  if (DcsQvPayloadMissionEventFn f = g_missionActive.load()) f(2);
  return 0;
}
// The local player changed slot (onPlayerChangeSlot for our own id): DCS
// frees the previous unit's objects (0.5.5 field report, crash 1).
extern "C" __declspec(dllexport) int DcsQvCull_SlotChange(void* /*lua_State*/) {
  if (DcsQvPayloadMissionEventFn f = g_missionActive.load()) f(3);
  return 0;
}

// Status for the DCS Control app's in-headset panel (bit layout in
// status_word.h); lock-free, callable from any thread. 0 = not running.
extern "C" __declspec(dllexport) unsigned long long DcsQvCull_Status() {
  return g_status.load(std::memory_order_acquire);
}

// Dual camera status ([Perception] CockpitTrueIpd, bit layout in
// loader_api.h); lock-free, callable from any thread. 0 = not configured or
// not running (no payload, payload swap, after stop).
extern "C" __declspec(dllexport) uint64_t DcsQvCull_DualCameraStatus() {
  return g_dualStatus.load(std::memory_order_acquire);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    // Never unmapped before the process ends: DCS's Lua frees C modules when
    // it closes the hooks' state at exit, while the payload (loaded by us,
    // never freed) still runs and uses our status word, log and registry,
    // and the watcher thread runs our code.
    if (!qvpin::PinModuleOf(reinterpret_cast<const void*>(&DcsQvCull_Status)))
      OutputDebugStringW(L"DcsQvCull: could not pin the loader module\n");
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(module, path, MAX_PATH);
    g_dir = path;
    g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1);
    g_log = _wfsopen((g_dir + L"DcsQvCull.log").c_str(), L"w", _SH_DENYWR);
    g_api = DcsQvLoaderApi{kDcsQvLoaderApiVersion, &ApiPatch, &ApiOriginal, &ApiRestore, &LogLine, nullptr};
    static std::wstring dirCopy = g_dir;
    g_api.dir = dirCopy.c_str();
    Logf("DcsQvCull loader started from %ls", path);
    HANDLE t = CreateThread(nullptr, 0, Watcher, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
  }
  return TRUE;
}
