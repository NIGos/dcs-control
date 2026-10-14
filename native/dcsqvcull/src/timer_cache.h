// Timer cache. dx11backend.dll calls edCore!ED_get_time (and through it
// QueryPerformanceCounter) on every texture bind (DX11Texture vtable slot 23,
// RVA 0x49e90), only to timestamp texture-streaming usage; these are its only
// two calls. Tens of thousands of timer reads per frame cost several percent of
// the render thread. The import is redirected to a function that returns a
// value refreshed by a helper thread every `cacheUs` microseconds, so the hot
// path is a single memory load. Only dx11backend.dll's import is patched;
// nothing else in DCS sees the cached clock.
// Included once from main.cpp after the globals it uses (Log).
#pragma once

namespace timercache {

using GetTimeFn = double (*)();
GetTimeFn g_orig = nullptr;
std::atomic<bool> g_on{false};
std::atomic<int> g_periodUs{1000};
std::atomic<double> g_cached{0.0};
std::atomic<uint64_t> g_refreshes{0};
std::atomic<bool> g_stopUpdater{false};
// edtime.h's stub (patched once per process) reads the g_cached of the payload
// that wrote it. After a hot reload that payload's updater is stopped, so this
// payload's updater also refreshes that copy (null when the stub is ours).
std::atomic<std::atomic<double>*> g_mirror{nullptr};

double Hook() {
  if (!g_on.load(std::memory_order_relaxed)) return g_orig();
  return g_cached.load(std::memory_order_relaxed);
}

// Refreshes the cached time. Always runs once installed, so switching the
// cache on never returns a stale value.
DWORD WINAPI Updater(void*) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  while (!g_stopUpdater.load()) {
    const double now = g_orig();
    g_cached.store(now, std::memory_order_relaxed);
    if (std::atomic<double>* m = g_mirror.load(std::memory_order_relaxed)) m->store(now, std::memory_order_relaxed);
    g_refreshes.fetch_add(1, std::memory_order_relaxed);
    int us = std::max(100, g_periodUs.load());
    if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = -10LL * us;
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, 100);
    } else {
      Sleep(1);
    }
  }
  if (timer) CloseHandle(timer);
  return 0;
}

// Finds the IAT slot of `dll!func` in `module`.
void** FindImport(HMODULE module, const char* dll, const char* func) {
  auto base = reinterpret_cast<uint8_t*>(module);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return nullptr;
  for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
    if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) continue;
    auto names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk);
    auto slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
      auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (strcmp(reinterpret_cast<const char*>(byName->Name), func) == 0)
        return reinterpret_cast<void**>(&slots->u1.Function);
    }
  }
  return nullptr;
}

// Installs the hook (cache disabled until g_on is set). Returns false if unavailable.
bool Install(int periodUs) {
  if (g_orig) return true;
  HMODULE dx = GetModuleHandleW(L"dx11backend.dll");
  if (!dx) {
    Log("timer cache: dx11backend.dll not loaded");
    return false;
  }
  void** slot = FindImport(dx, "edCore.dll", "ED_get_time");
  if (!slot) {
    Log("timer cache: ED_get_time import not found in dx11backend.dll");
    return false;
  }
  g_periodUs = periodUs;
  GetTimeFn orig = reinterpret_cast<GetTimeFn>(SlotOriginal(slot));
  g_orig = orig;
  g_cached = orig();
  HANDLE t = CreateThread(nullptr, 0, qvexit::Thread<Updater>, nullptr, 0, nullptr);
  if (!t) {
    g_orig = nullptr;
    return false;
  }
  CloseHandle(t);
  if (!HookSlot(slot, reinterpret_cast<void*>(&Hook), nullptr)) return false;
  Log("timer cache: dx11backend!ED_get_time redirected (refresh every %d us)", periodUs);
  return true;
}

}  // namespace timercache
