// Offline tests for prefetch_fix.h (port of dlss5vrframegen's
// prefetch_fix_tests.cpp + probe.cpp, plus unload safety and the take-over
// of the old dxgi2 fix).
//
// The test executable is its own "game module": g_testModules = {exe}, so the
// scan finds the probe's stored pointer below in its .data and its
// GetProcAddress import slot (both restored before Run returns). The fix's
// own state is stored encoded, so scanning the image that holds it never
// redirects the fix itself. The statistics log goes to a per-process temp
// folder (removed at the end; nothing else is deleted).
#pragma once

namespace pfxtest {

using pffix::PrefetchFn;
using pffix::GetProcFn;

// ---- probe (stand-in for edCore) ----
// A data pointer resolved once (edCore keeps it in its data), and a lookup
// through this module's GetProcAddress import on demand.
PrefetchFn g_probeStored = nullptr;

BOOL ProbeStored(void* address, SIZE_T bytes) {
  WIN32_MEMORY_RANGE_ENTRY range{address, bytes};
  PrefetchFn fn = *static_cast<PrefetchFn volatile*>(&g_probeStored);
  return fn(GetCurrentProcess(), 1, &range, 0);
}
PrefetchFn ProbeLookupFn() {
  return reinterpret_cast<PrefetchFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "PrefetchVirtualMemory"));
}
BOOL ProbeLookup(void* address, SIZE_T bytes) {
  WIN32_MEMORY_RANGE_ENTRY range{address, bytes};
  return ProbeLookupFn()(GetCurrentProcess(), 1, &range, 0);
}

// The executable's GetProcAddress import slot.
void** ImportSlot() {
  auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
    if (!d->OriginalFirstThunk) continue;
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
    auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto* n = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (strcmp(reinterpret_cast<const char*>(n->Name), "GetProcAddress") == 0)
        return reinterpret_cast<void**>(&slots->u1.Function);
    }
  }
  return nullptr;
}
void* ReadSlot(void** slot) { return *static_cast<void* volatile*>(slot); }

// ---- counting real function (the kernel's, through its encoded copy) ----
std::atomic<uint64_t> g_realCalls{0};
uintptr_t g_kernelRealEnc = 0;
BOOL WINAPI CountingReal(HANDLE p, ULONG_PTR c, PWIN32_MEMORY_RANGE_ENTRY e, ULONG f) {
  g_realCalls.fetch_add(1);
  return reinterpret_cast<PrefetchFn>(pffix::Dec(g_kernelRealEnc))(p, c, e, f);
}
void UseReal(PrefetchFn fn) { pffix::g_realEnc = pffix::Enc(reinterpret_cast<void*>(fn)); }

// Blocks inside the "system call" until released (an in-flight caller).
HANDLE g_release = nullptr;
std::atomic<int> g_inside{0};
BOOL WINAPI BlockingReal(HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG) {
  g_inside.fetch_add(1);
  WaitForSingleObject(g_release, 10000);
  g_inside.fetch_sub(1);
  return TRUE;
}

// ---- simulated old dxgi2 fix ----
std::atomic<uint64_t> g_oldCalls{0}, g_oldLookups{0};
BOOL WINAPI FakeOldPrefetch(HANDLE p, ULONG_PTR c, PWIN32_MEMORY_RANGE_ENTRY e, ULONG f) {
  g_oldCalls.fetch_add(1);
  return reinterpret_cast<PrefetchFn>(pffix::Dec(g_kernelRealEnc))(p, c, e, f);
}
FARPROC WINAPI FakeOldGetProc(HMODULE m, LPCSTR name) {
  const FARPROC r = reinterpret_cast<GetProcFn>(pffix::Dec(pffix::g_kbGetProcEnc))(m, name);
  if (pffix::IsKernelPrefetch(reinterpret_cast<const void*>(r))) {
    g_oldLookups.fetch_add(1);
    return reinterpret_cast<FARPROC>(&FakeOldPrefetch);
  }
  return r;
}
BOOL WINAPI OtherPrefetch(HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG) { return TRUE; }

alignas(4096) char g_buffer[1 << 20];

std::wstring g_tmpDir;
std::string ReadLog() {
  std::string out;
  FILE* f = nullptr;
  if (_wfopen_s(&f, pffix::g_logPath.c_str(), L"rb") != 0 || !f) return out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  return out;
}
int CountOf(const std::string& s, const char* what) {
  int n = 0;
  for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1)) ++n;
  return n;
}

uint64_t Calls() { return pffix::SumCalls(); }
uint64_t Skipped() { return pffix::SumSkipped(); }

void Pure() {
  // Repeat window: the first pass-through starts it; repeats do not extend it.
  for (auto& e : pffix::g_table) e.store(0);
  Check(!pffix::SeenRecently(0x10000, 1, 4096, 1000, 1000), "prefetch: first range passes");
  Check(pffix::SeenRecently(0x10000, 1, 4096, 1500, 1000), "prefetch: same range within the window is a repeat");
  Check(!pffix::SeenRecently(0x10000, 2, 4096, 1500, 1000) && !pffix::SeenRecently(0x10000, 1, 8192, 1500, 1000) &&
            !pffix::SeenRecently(0x20000, 1, 4096, 1500, 1000),
        "prefetch: another count, size or address is not a repeat");
  Check(pffix::SeenRecently(0x10000, 1, 4096, 1999, 1000) && !pffix::SeenRecently(0x10000, 1, 4096, 2000, 1000),
        "prefetch: the window counts from the last pass-through (not extended by repeats)");
  Check(pffix::SeenRecently(0x10000, 1, 4096, 2500, 1000), "prefetch: and restarts at the next pass-through");
  // 24-bit tick wrap.
  Check(!pffix::SeenRecently(0x30000, 1, 64, 0xFFFFF0, 1000) && pffix::SeenRecently(0x30000, 1, 64, 0x1000000 + 10, 1000),
        "prefetch: the window survives the 24-bit tick wrap");
  for (auto& e : pffix::g_table) e.store(0);

  // Install root and the candidate path rule.
  Check(pffix::RootOf(L"C:\\Games\\DCS World\\bin\\DCS.exe") == L"C:\\Games\\DCS World\\" &&
            pffix::RootOf(L"D:\\DCS\\bin-mt\\DCS.exe") == L"D:\\DCS\\" && pffix::RootOf(L"DCS.exe").empty(),
        "prefetch: install root is the parent of the executable's folder");
  Check(pffix::UnderRoot(L"c:\\games\\dcs world\\bin\\edCore.dll", L"C:\\Games\\DCS World\\") &&
            !pffix::UnderRoot(L"C:\\Games\\DCS WorldX\\bin\\edCore.dll", L"C:\\Games\\DCS World\\") &&
            !pffix::UnderRoot(L"C:\\Windows\\System32\\kernel32.dll", L"C:\\Games\\DCS World\\") &&
            !pffix::UnderRoot(L"C:\\x.dll", L""),
        "prefetch: only modules under the install root (case-insensitive) are game modules");
  Check(pffix::ParseMode(L"SKIP") == pffix::kSkip && pffix::ParseMode(L"observe") == pffix::kObserve &&
            pffix::ParseMode(L"off") == pffix::kOff && pffix::ParseMode(L"x") == pffix::kOff,
        "prefetch: mode names");

  // Faults are contained: a bogus image header, a no-access slot.
  void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  auto* dos = static_cast<IMAGE_DOS_HEADER*>(page);
  dos->e_magic = IMAGE_DOS_SIGNATURE;
  dos->e_lfanew = 0x7FFFFF00;
  pffix::Hit hits[4];
  int n = 0;
  void* pf[1] = {page};
  Check(!pffix::CollectGuarded(static_cast<HMODULE>(page), pf, 1, hits, 4, &n) && n == 0,
        "prefetch: a fault while reading a module is contained");
  DWORD old = 0;
  VirtualProtect(page, 4096, PAGE_NOACCESS, &old);
  Check(pffix::CasGuarded(static_cast<void**>(page), nullptr, page, false) == -1,
        "prefetch: a fault while writing a slot is contained");
  VirtualFree(page, 0, MEM_RELEASE);
}

// Port of prefetch_fix_tests.cpp's cases (off, observe, skip, window), in
// one process: the mode is live here.
void Modes(void** imp, void* impKernel) {
  void* const kernelStored = reinterpret_cast<void*>(g_probeStored);
  pffix::Apply(pffix::kOff, 1000);
  Check(pffix::g_state.load() == 0 && ReadSlot(reinterpret_cast<void**>(&g_probeStored)) == kernelStored &&
            ReadSlot(imp) == impKernel,
        "prefetch off: the process is not touched");

  pffix::Apply(pffix::kObserve, 1000);
  Check(pffix::g_state.load() == 1 && pffix::g_pinned, "prefetch observe: installed, payload module pinned");
  Check(pffix::g_livePointers.load() == 1 && pffix::g_liveImports.load() == 1,
        "prefetch observe: the stored pointer and the GetProcAddress import redirected");
  Check(ReadSlot(reinterpret_cast<void**>(&g_probeStored)) == reinterpret_cast<void*>(&pffix::Replacement) &&
            ReadSlot(imp) == reinterpret_cast<void*>(&pffix::FixedGetProc),
        "prefetch observe: slots hold the replacements");
  // Our real function is the kernel's, never a slot value.
  Check(pffix::IsKernelPrefetch(pffix::Dec(pffix::g_realEnc.load())), "prefetch: real function resolved from the kernel");
  g_kernelRealEnc = pffix::g_realEnc.load();
  UseReal(&CountingReal);
  uint64_t c0 = Calls(), s0 = Skipped(), r0 = g_realCalls.load();
  const bool ok = ProbeStored(g_buffer, sizeof(g_buffer)) && ProbeStored(g_buffer, sizeof(g_buffer)) &&
                  ProbeLookup(g_buffer + 4096, 4096) && ProbeLookup(g_buffer + 4096, 4096);
  Check(ok && Calls() - c0 == 4 && Skipped() - s0 == 0 && g_realCalls.load() - r0 == 4,
        "prefetch observe: every call goes through the fix and reaches the kernel");
  Check(ProbeLookupFn() == &pffix::Replacement && pffix::g_lookups.load() >= 2,
        "prefetch: GetProcAddress lookups are answered with the replacement");

  pffix::Apply(pffix::kSkip, 1000);
  c0 = Calls(), s0 = Skipped(), r0 = g_realCalls.load();
  ProbeStored(g_buffer, sizeof(g_buffer));
  ProbeStored(g_buffer, sizeof(g_buffer));
  ProbeLookup(g_buffer + 4096, 4096);
  ProbeLookup(g_buffer + 4096, 4096);
  Check(Calls() - c0 == 4 && Skipped() - s0 == 2 && g_realCalls.load() - r0 == 2,
        "prefetch skip: exactly the two repeats dropped (live switch from observe)");
  Check(pffix::Active(), "prefetch skip: active for the status word");
  // A window change applies live; outside the window the range passes again.
  pffix::Apply(pffix::kSkip, 40);
  Sleep(60);
  s0 = Skipped();
  ProbeStored(g_buffer, sizeof(g_buffer));
  Check(Skipped() == s0, "prefetch skip: a repeat outside the window passes");
  // Another process handle (a real handle, even to this process) or an
  // empty range list: never skipped.
  WIN32_MEMORY_RANGE_ENTRY range{g_buffer + 16384, 4096};
  HANDLE self = nullptr;
  DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self, 0, FALSE,
                  DUPLICATE_SAME_ACCESS);
  s0 = Skipped();
  pffix::Replacement(self, 1, &range, 0);
  pffix::Replacement(self, 1, &range, 0);
  pffix::Replacement(GetCurrentProcess(), 0, &range, 0);
  pffix::Replacement(GetCurrentProcess(), 0, &range, 0);
  Check(Skipped() == s0, "prefetch skip: only this process's own non-empty prefetches are skipped");
  if (self) CloseHandle(self);
  // Spin guard: 256 back-to-back skips on one thread reset the run (one tick yielded).
  pffix::Apply(pffix::kSkip, 60000);
  ProbeStored(g_buffer + 8192, 4096);
  for (int i = 0; i < 300; ++i) ProbeStored(g_buffer + 8192, 4096);
  Check(pffix::t_skipRun == 300 - 256, "prefetch skip: spin guard yields after 256 back-to-back skips");
  ProbeStored(g_buffer + 12288, 4096);  // a pass-through ends the run
  Check(pffix::t_skipRun == 0, "prefetch skip: a pass-through resets the spin guard");
  pffix::Apply(pffix::kSkip, 1000);
}

// Uninstall, reinstall (hot reload), escaped pointers, restore-only-if-ours,
// in-flight drain, kill switch.
void Unload(void** imp, void* impKernel) {
  void* const kernelStored = pffix::Dec(pffix::g_k32PrefetchEnc);
  // A copy DCS made of a lookup result (heap): cannot be found again.
  PrefetchFn escaped = ProbeLookupFn();
  Check(escaped == &pffix::Replacement, "prefetch unload: an escaped copy of the replacement exists");
  pffix::Apply(pffix::kOff, 1000);
  Check(pffix::g_state.load() == 0 && ReadSlot(reinterpret_cast<void**>(&g_probeStored)) == kernelStored &&
            ReadSlot(imp) == impKernel && pffix::g_livePointers.load() == 0 && pffix::g_liveImports.load() == 0,
        "prefetch unload: every slot restored to the real kernel function");
  Check(ProbeLookupFn() != &pffix::Replacement, "prefetch unload: lookups return the kernel function again");
  uint64_t s0 = Skipped(), r0 = g_realCalls.load();
  WIN32_MEMORY_RANGE_ENTRY range{g_buffer, 4096};
  escaped(GetCurrentProcess(), 1, &range, 0);
  escaped(GetCurrentProcess(), 1, &range, 0);
  Check(Skipped() == s0 && g_realCalls.load() - r0 == 2 && pffix::InFlight() == 0,
        "prefetch unload: an escaped copy passes through (module pinned, mode off)");

  // Hot reload: install again, no double wrapping.
  pffix::Apply(pffix::kSkip, 1000);
  Check(pffix::g_livePointers.load() == 1 && pffix::g_liveImports.load() == 1 &&
            ReadSlot(reinterpret_cast<void**>(&g_probeStored)) == reinterpret_cast<void*>(&pffix::Replacement),
        "prefetch reload: reinstalled, same slots");
  r0 = g_realCalls.load();
  ProbeStored(g_buffer + 65536, 4096);
  Check(g_realCalls.load() - r0 == 1 &&
            pffix::Dec(pffix::g_realEnc.load()) != reinterpret_cast<void*>(&pffix::Replacement),
        "prefetch reload: one call reaches the real function once (never itself)");

  // A slot someone else changed is left alone (and dropped by the verify).
  *static_cast<PrefetchFn volatile*>(&g_probeStored) = &OtherPrefetch;
  pffix::Verify();
  Check(pffix::g_livePointers.load() == 0 && ReadSlot(reinterpret_cast<void**>(&g_probeStored)) ==
                                               reinterpret_cast<void*>(&OtherPrefetch),
        "prefetch: a slot changed to an unknown value is not re-taken and no longer ours");
  pffix::Apply(pffix::kOff, 1000);
  Check(ReadSlot(reinterpret_cast<void**>(&g_probeStored)) == reinterpret_cast<void*>(&OtherPrefetch) &&
            ReadSlot(imp) == impKernel,
        "prefetch unload: restores only slots that still hold our value");
  g_probeStored = reinterpret_cast<PrefetchFn>(kernelStored);

  // In flight: a thread inside the replacement while the fix is uninstalled.
  pffix::Apply(pffix::kObserve, 1000);
  g_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  UseReal(&BlockingReal);
  std::thread caller([] { ProbeStored(g_buffer, 4096); });
  for (int i = 0; i < 2000 && g_inside.load() == 0; ++i) Sleep(1);
  Check(g_inside.load() == 1 && pffix::InFlight() == 1, "prefetch in flight: one caller inside the replacement");
  const bool drained = pffix::Uninstall(50);
  Check(!drained && ReadSlot(reinterpret_cast<void**>(&g_probeStored)) == kernelStored,
        "prefetch in flight: uninstall restores slots and reports the caller still inside");
  SetEvent(g_release);
  caller.join();
  Check(pffix::InFlight() == 0 && g_inside.load() == 0, "prefetch in flight: the caller left; nothing in flight");
  CloseHandle(g_release);
  g_release = nullptr;
  UseReal(&CountingReal);

  // Kill switch (SetEngineOff) and ini mode, live.
  const int cfgMode = g_cfg.prefetchMode;
  g_cfg.prefetchMode = pffix::kSkip;
  ApplyPrefetch();
  Check(pffix::g_state.load() == 1 && pffix::Active(), "prefetch: Mode=skip installs");
  const uint64_t w1 = ComputeStatus();
  pffix::Apply(pffix::kOff, 1000);
  const uint64_t w2 = ComputeStatus();
  Check(qvstatus::ActiveCount(w1) == qvstatus::ActiveCount(w2) + 1 &&
            qvstatus::ConfiguredCount(w1) == qvstatus::ConfiguredCount(w2) && qvstatus::LatchedCount(w1) == qvstatus::LatchedCount(w2),
        "prefetch status: counted active while skipping with slots, configured whatever its state");
  ApplyPrefetch();
  SetEngineOff(true);
  Check(pffix::g_state.load() == 0 && ReadSlot(reinterpret_cast<void**>(&g_probeStored)) == kernelStored &&
            ReadSlot(imp) == impKernel,
        "prefetch kill switch: off restores every slot");
  SetEngineOff(false);
  Check(pffix::g_state.load() == 1 && pffix::g_livePointers.load() == 1, "prefetch kill switch: on reinstalls");
  g_cfg.prefetchMode = cfgMode;
  ApplyPrefetch();
  Check(pffix::g_state.load() == (cfgMode != 0 ? 1 : 0), "prefetch: ini mode back");
  pffix::Apply(pffix::kOff, 1000);
}

// Users migrating from DCS Control's dxgi2 fix: its replacements already sit
// in the slots; ours takes over, re-takes after a re-patch (logged once), and
// restores the real kernel functions on unload.
void OldFix(void** imp, void* impKernel) {
  void* const kernelStored = reinterpret_cast<void*>(g_probeStored);
  void* const kb = pffix::Dec(pffix::g_kbPrefetchEnc);
  // The old fix patched both slots before we load.
  g_probeStored = &FakeOldPrefetch;
  Check(pffix::CasGuarded(imp, impKernel, reinterpret_cast<void*>(&FakeOldGetProc), true) == 1,
        "prefetch old fix: simulated import patch");
  Check(ProbeLookupFn() == &FakeOldPrefetch, "prefetch old fix: lookups answered by the old fix");
  // Resolution never goes through the (redirected) import.
  pffix::g_resolved = false;
  pffix::g_testForeign = {pffix::Foreign{reinterpret_cast<void*>(&FakeOldPrefetch),
                                         reinterpret_cast<void*>(&FakeOldGetProc), true, L"test\\dxgi2.dll"}};
  const uint64_t t0 = pffix::g_takeovers.load();
  pffix::Apply(pffix::kSkip, 1000);
  Check(pffix::IsKernelPrefetch(pffix::Dec(pffix::g_realEnc.load())) &&
            pffix::Dec(pffix::g_realEnc.load()) != reinterpret_cast<void*>(&FakeOldPrefetch) &&
            pffix::IsKernelGetProc(pffix::Dec(pffix::g_kbGetProcEnc)),
        "prefetch old fix: real targets from the kernel, never chained through the old fix");
  Check(ReadSlot(reinterpret_cast<void**>(&g_probeStored)) == reinterpret_cast<void*>(&pffix::Replacement) &&
            ReadSlot(imp) == reinterpret_cast<void*>(&pffix::FixedGetProc) && pffix::g_takeovers.load() - t0 == 2,
        "prefetch old fix: both slots taken over");
  std::string log = ReadLog();
  Check(CountOf(log, "prefetch: old dxgi2 fix detected (module test\\dxgi2.dll), taken over") == 1,
        "prefetch old fix: one take-over line in the log");
  g_kernelRealEnc = pffix::Enc(kb);
  const uint64_t o0 = g_oldCalls.load(), ol0 = g_oldLookups.load(), c0 = Calls();
  ProbeStored(g_buffer + 131072, 4096);
  Check(ProbeLookupFn() == &pffix::Replacement && g_oldCalls.load() == o0 && g_oldLookups.load() == ol0 &&
            Calls() - c0 == 1,
        "prefetch old fix: calls and lookups now go through ours only");

  // The old fix re-patches: re-taken by the next verify, logged once.
  const uint64_t r0 = pffix::g_retakes.load();
  for (int round = 0; round < 3; ++round) {
    g_probeStored = &FakeOldPrefetch;
    pffix::CasGuarded(imp, reinterpret_cast<void*>(&pffix::FixedGetProc), reinterpret_cast<void*>(&FakeOldGetProc), true);
    pffix::Verify();
  }
  log = ReadLog();
  Check(ReadSlot(reinterpret_cast<void**>(&g_probeStored)) == reinterpret_cast<void*>(&pffix::Replacement) &&
            ReadSlot(imp) == reinterpret_cast<void*>(&pffix::FixedGetProc) && pffix::g_retakes.load() - r0 == 6,
        "prefetch old fix: re-patched slots re-taken, ours kept");
  Check(CountOf(log, "re-taken, ours kept (logged once)") == 1, "prefetch old fix: the patch war is logged once");
  // A slot written back to the kernel function (another fix's restore) is re-taken too.
  g_probeStored = reinterpret_cast<PrefetchFn>(kb);
  pffix::Verify();
  Check(ReadSlot(reinterpret_cast<void**>(&g_probeStored)) == reinterpret_cast<void*>(&pffix::Replacement),
        "prefetch old fix: a slot reset to the kernel function is re-taken");

  // Unload: the real kernel functions, never the old fix's.
  pffix::Apply(pffix::kOff, 1000);
  Check(ReadSlot(reinterpret_cast<void**>(&g_probeStored)) == kb &&
            pffix::IsKernelGetProc(ReadSlot(imp)) && ReadSlot(imp) != reinterpret_cast<void*>(&FakeOldGetProc),
        "prefetch old fix: unload restores the real kernel functions");
  pffix::g_testForeign.clear();
  // Back to the executable's own import value and the kernel32 pointer.
  pffix::CasGuarded(imp, ReadSlot(imp), impKernel, true);
  g_probeStored = reinterpret_cast<PrefetchFn>(kernelStored);
  Check(ReadSlot(imp) == impKernel, "prefetch old fix: test import slot back to its loaded value");
}

// The 10 s statistics line and the worker tick (load notification, rescan).
void LogAndTick() {
  pffix::Apply(pffix::kSkip, 1000);
  // A module loaded after install is reported by the ntdll notification.
  const wchar_t* candidates[] = {L"msimg32.dll", L"mscms.dll", L"wtsapi32.dll", L"dhcpcsvc6.dll"};
  for (const wchar_t* name : candidates) {
    if (GetModuleHandleW(name)) continue;
    pffix::g_loadEvent = false;
    HMODULE m = LoadLibraryW(name);
    if (!m) continue;
    bool seen = false;
    for (auto& s : pffix::g_loaded)
      if (s.load() == reinterpret_cast<uintptr_t>(m)) seen = true;
    Check(pffix::g_notifCookie && pffix::g_loadEvent.load() && seen,
          "prefetch: a module loaded after install is queued for the next scan");
    pffix::Tick();
    FreeLibrary(m);
    break;
  }
  pffix::g_lastLogMs = GetTickCount64() - 10000;
  ProbeStored(g_buffer + 262144, 4096);
  ProbeStored(g_buffer + 262144, 4096);
  pffix::Tick();
  const std::string log = ReadLog();
  // Last statistics line: "YYYY-MM-DD HH:MM:SS calls/s=<n> skipped=<x.x>% pointers=1 imports=1 lookups=<n> faults=0".
  const size_t p = log.rfind("calls/s=");
  bool ok = p != std::string::npos && p >= 20;
  if (ok) {
    const std::string line = log.substr(p - 20, log.find('\n', p) - (p - 20));
    int y, mo, d, h, mi, s;
    unsigned long long cps, lookups, faults;
    double pct;
    unsigned ptrs, imps;
    ok = sscanf_s(line.c_str(), "%4d-%2d-%2d %2d:%2d:%2d calls/s=%llu skipped=%lf%% pointers=%u imports=%u lookups=%llu faults=%llu",
                  &y, &mo, &d, &h, &mi, &s, &cps, &pct, &ptrs, &imps, &lookups, &faults) == 12 &&
         y >= 2024 && ptrs == 1 && imps == 1 && faults == 0 && pct > 0.0 && line.back() == '\r';
    printf("     %s\n", line.c_str());
  }
  Check(ok, "prefetch: statistics line format (local time, calls/s, skipped %, pointers, imports, lookups, faults)");
  pffix::Apply(pffix::kOff, 1000);
}

// Many threads calling through the slots while the fix is switched on and
// off (hot reload / kill switch under load): no fault, nothing left in flight,
// slots back to the kernel function.
void Stress(void** imp, void* impKernel) {
  void* const kernelStored = reinterpret_cast<void*>(g_probeStored);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> fails{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([&, t] {
      unsigned i = 0;
      while (!stop.load()) {
        char* a = g_buffer + ((i++ & 7) + t * 8) * 4096;
        if (!(i & 1 ? ProbeStored(a, 4096) : ProbeLookup(a, 4096))) fails.fetch_add(1);
      }
    });
  for (int k = 0; k < 40; ++k) {
    pffix::Apply(k % 3 == 2 ? pffix::kOff : (k & 1) ? pffix::kSkip : pffix::kObserve, 1000);
    Sleep(5);
  }
  pffix::Apply(pffix::kOff, 1000);
  stop = true;
  for (auto& t : threads) t.join();
  Check(fails.load() == 0 && pffix::InFlight() == 0 && ReadSlot(reinterpret_cast<void**>(&g_probeStored)) ==
                                                           kernelStored && ReadSlot(imp) == impKernel,
        "prefetch stress: switching under load never faults, leaves nothing in flight, restores the slots");
}

// R39 [Prefetch] TableBits: K distinct ranges cycling within the window. With
// the 2^16 table, collisions let about e^(-K/2^16)... of the repeats through;
// with 2^20 nearly every repeat is found. Same keys and windows either way.
double RepeatSkipShare(unsigned bits, int keys) {
  Check(pffix::SetTableBits(bits) && pffix::TableBits() == pffix::ClampTableBits(bits), "prefetch table: size set");
  int skipped = 0, total = 0;
  uint64_t now = 5000;
  for (int cycle = 0; cycle < 3; ++cycle)
    for (int k = 0; k < keys; ++k, ++now) {
      const bool s = pffix::SeenRecently(0x10000000ull + static_cast<ULONG_PTR>(k) * 0x10000, 1, 0x10000, now / 1000,
                                         1000000);
      if (cycle > 0) {
        skipped += s;
        ++total;
      }
    }
  return total ? double(skipped) / total : 0;
}
void TableBits() {
  const int kKeys = 30000;
  const double s16 = RepeatSkipShare(16, kKeys);
  const double s20 = RepeatSkipShare(20, kKeys);
  printf("     (prefetch table: %d ranges cycling, repeats found %.1f %% with 2^16, %.1f %% with 2^20)\n", kKeys,
         100 * s16, 100 * s20);
  Check(s16 < 0.80 && s20 > 0.95, "prefetch table: a 2^20 table finds the repeats a 2^16 table loses to collisions");
  // First calls are never skipped, at any size (the hints DCS gets per window are unchanged).
  pffix::SetTableBits(22);
  bool firstPass = true;
  for (int k = 0; k < 1000; ++k)
    firstPass &= !pffix::SeenRecently(0x7000000000ull + static_cast<ULONG_PTR>(k) * 4096, 2, 8192, 100, 1000);
  Check(firstPass && pffix::TableBits() == 22, "prefetch table: 2^22 keeps first calls passing");
  Check(pffix::ClampTableBits(3) == 16 && pffix::ClampTableBits(40) == 22, "prefetch table: bits clamped to 16-22");
  pffix::SetTableBits(16);
  Check(pffix::TableBits() == 16 && pffix::g_tab.load() == &pffix::g_staticTable,
        "prefetch table: back to the static 2^16 table");
  for (auto& e : pffix::g_table) e.store(0);
}

void Run() {
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  g_tmpDir = std::wstring(tmp) + L"qv_pffix_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
  CreateDirectoryW(g_tmpDir.c_str(), nullptr);
  pffix::g_logPath = g_tmpDir + L"DcsQvCullPrefetch.log";
  pffix::g_testModules = {GetModuleHandleW(nullptr)};
  g_probeStored =
      reinterpret_cast<PrefetchFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "PrefetchVirtualMemory"));
  void** imp = ImportSlot();
  void* const impKernel = imp ? ReadSlot(imp) : nullptr;
  Check(imp && g_probeStored, "prefetch: probe stored pointer and GetProcAddress import found");
  if (!imp || !g_probeStored) return;

  Pure();
  TableBits();
  Modes(imp, impKernel);
  Unload(imp, impKernel);
  OldFix(imp, impKernel);
  LogAndTick();
  Stress(imp, impKernel);
  Check(pffix::g_faults.load() == 0, "prefetch: no contained faults during the tests");

  pffix::Apply(pffix::kOff, 1000);
  UseReal(reinterpret_cast<PrefetchFn>(pffix::Dec(pffix::g_kbPrefetchEnc)));
  pffix::g_testModules.clear();
  DeleteFileW(pffix::g_logPath.c_str());
  RemoveDirectoryW(g_tmpDir.c_str());
  pffix::g_logPath.clear();
}

}  // namespace pfxtest
