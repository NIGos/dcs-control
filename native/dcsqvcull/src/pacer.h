// Low-power pacer wait. Visualizer's frame pacer (Main thread, loop at RVA
// 0x8de50..0x8de86 in DCS 2.9.30, 0xba5e0..0xba616 in 2.9.31) spins until the next frame deadline, exiting early when
// another thread clears [rsi] or sets [rsi+0xC]; each iteration runs four
// PAUSE instructions and reads the clock. That burns a whole core.
//
// The four PAUSEs (8 bytes at 0x8de5e; 2.9.31: 0xba5ee, R147) are replaced by "call [slot]; nop".
// The slot points either to a stub that does exactly the four PAUSEs (off),
// or to one that arms MONITORX on [rsi]'s cache line, re-checks both exit
// flags and then MWAITX-sleeps for at most TimeoutUs (on). A write to the
// flags wakes it at once, so the loop's exit conditions and the deadline
// check are unchanged; only the time between clock reads grows to at most
// TimeoutUs. Code and slot are on separate pages, and the code patch is
// written only while the Main thread is suspended outside the patched bytes.
// Included once from main.cpp inside its anonymous namespace.
#pragma once

#include <intrin.h>
#include <tlhelp32.h>

namespace pacer {

// The site per analysed Visualizer.dll build (DCS 2.9.30, 2.9.31: R147, the same loop bytes at .text +0x2c790); the
// first one whose surrounding bytes match is used.
constexpr uint32_t kSiteRvas[] = {0x8de5e, 0xba5ee};
const uint8_t kPauses[8] = {0xF3, 0x90, 0xF3, 0x90, 0xF3, 0x90, 0xF3, 0x90};
// Bytes around the site that must match this Visualizer.dll build.
const uint8_t kBefore[8] = {0x0F, 0xB6, 0x46, 0x0C, 0x90, 0x84, 0xC0, 0x75};  // at site - 9
const uint8_t kAfter[5] = {0x48, 0x8D, 0x4C, 0x24, 0x70};                     // at site + 8

struct CodePage {
  uint8_t pauseStub[16];
  uint8_t mwaitStub[64];
  uint32_t magic;
  uint32_t pad;
  uint8_t* site;
};
constexpr uint32_t kMagic = 0x31435044;  // "DPC1"
constexpr size_t kPage = 4096;

CodePage* g_code = nullptr;
void** g_slot = nullptr;  // data page, next to the code page
std::atomic<bool> g_on{false};
std::atomic<int> g_state{0};  // 0 = not installed, 1 = installed, -1 = unavailable
uint32_t g_timeoutTicks = 0;

bool HasMonitorx() {
  int r[4];
  __cpuid(r, 0x80000000);
  if (static_cast<unsigned>(r[0]) < 0x80000001u) return false;
  __cpuid(r, 0x80000001);
  return (r[2] >> 29) & 1;  // ECX bit 29: MONITORX/MWAITX
}

// Allocates the code+data pages within +-2 GB of `target` (rel32 reach).
uint8_t* AllocNear(uint8_t* target) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  const uintptr_t gran = si.dwAllocationGranularity;
  uintptr_t start = reinterpret_cast<uintptr_t>(target);
  for (uintptr_t off = gran; off < (1ull << 30); off += gran) {
    for (int dir = -1; dir <= 1; dir += 2) {
      uintptr_t a = (dir < 0 ? start - off : start + off) & ~(gran - 1);
      void* p = VirtualAlloc(reinterpret_cast<void*>(a), 2 * kPage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      if (p) return static_cast<uint8_t*>(p);
    }
  }
  return nullptr;
}

void Emit(uint8_t*& p, std::initializer_list<uint8_t> bytes) {
  for (uint8_t b : bytes) *p++ = b;
}

// The MWAITX stub: preserves rax, rcx, rdx, rbx; expects rsi = the pacer
// object (exit flags at +0 and +0xC). Returns at once when either flag
// already asks to exit, otherwise sleeps until a write to that cache line or
// `ticks` TSC ticks.
void EmitMwaitStub(uint8_t* p, uint32_t ticks) {
  Emit(p, {0x50, 0x51, 0x52, 0x53});              // push rax, rcx, rdx, rbx
  Emit(p, {0x48, 0x89, 0xF0});                    // mov rax, rsi
  Emit(p, {0x31, 0xC9, 0x31, 0xD2});              // xor ecx, ecx; xor edx, edx
  Emit(p, {0x0F, 0x01, 0xFA});                    // monitorx
  Emit(p, {0x80, 0x3E, 0x00});                    // cmp byte [rsi], 0
  Emit(p, {0x74, 0x15});                          // je done (+21)
  Emit(p, {0x80, 0x7E, 0x0C, 0x00});              // cmp byte [rsi+0xc], 0
  Emit(p, {0x75, 0x0F});                          // jne done (+15)
  Emit(p, {0x31, 0xC0});                          // xor eax, eax (C0 hint)
  Emit(p, {0xB9, 0x02, 0x00, 0x00, 0x00});        // mov ecx, 2 (timer enable)
  uint8_t t[4];
  memcpy(t, &ticks, 4);
  Emit(p, {0xBB, t[0], t[1], t[2], t[3]});        // mov ebx, ticks
  Emit(p, {0x0F, 0x01, 0xFB});                    // mwaitx
  Emit(p, {0x5B, 0x5A, 0x59, 0x58, 0xC3});        // done: pop rbx, rdx, rcx, rax; ret
}

DWORD FindMainThread() {
  using GetDesc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  auto getDesc = reinterpret_cast<GetDesc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  if (!getDesc) return 0;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  THREADENTRY32 te{sizeof(te)};
  DWORD found = 0;
  for (BOOL ok = Thread32First(snap, &te); ok && !found; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
    if (!h) continue;
    PWSTR desc = nullptr;
    if (SUCCEEDED(getDesc(h, &desc)) && desc) {
      if (wcscmp(desc, L"Main") == 0) found = te.th32ThreadID;
      LocalFree(desc);
    }
    CloseHandle(h);
  }
  CloseHandle(snap);
  return found;
}

// Writes `bytes` over the 8-byte site with the Main thread suspended and its
// instruction pointer outside the site.
bool PatchSite(uint8_t* site, const uint8_t* bytes) {
  DWORD tid = FindMainThread();
  if (!tid) return false;
  HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
  if (!h) return false;
  bool done = false;
  for (int attempt = 0; attempt < 50 && !done; ++attempt) {
    if (SuspendThread(h) == static_cast<DWORD>(-1)) break;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(h, &ctx)) {
      uintptr_t rip = ctx.Rip;
      uintptr_t s = reinterpret_cast<uintptr_t>(site);
      if (rip <= s || rip >= s + 8) {
        DWORD old;
        if (VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &old)) {
          memcpy(site, bytes, 8);
          VirtualProtect(site, 8, old, &old);
          FlushInstructionCache(GetCurrentProcess(), site, 8);
          done = true;
        }
      }
    }
    ResumeThread(h);
    if (!done) Sleep(1);
  }
  CloseHandle(h);
  return done;
}

bool Install(double tscHz, int timeoutUs) {
  if (g_state.load() != 0) return g_state.load() > 0;
  g_state = -1;
  if (!HasMonitorx()) {
    Log("pacer: CPU has no MONITORX/MWAITX; skipped");
    return false;
  }
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"Visualizer.dll"));
  if (!base) return false;
  uint8_t* site = nullptr;
  uint32_t siteRva = 0;
  uint8_t cur[8];
  for (uint32_t rva : kSiteRvas) {
    uint8_t* s = base + rva;
    uint8_t before[8], after[5];
    if (allocslab::ReadBytes(s, cur, 8) && allocslab::ReadBytes(s - 9, before, 8) && allocslab::ReadBytes(s + 8, after, 5) &&
        memcmp(before, kBefore, 8) == 0 && memcmp(after, kAfter, 5) == 0) {
      site = s;
      siteRva = rva;
      break;
    }
  }
  if (!site) {
    Log("pacer: Visualizer.dll does not match this build; skipped");
    return false;
  }
  g_timeoutTicks = static_cast<uint32_t>(std::min(1e9, tscHz * std::max(2, timeoutUs) / 1e6));
  if (cur[0] == 0xFF && cur[1] == 0x15) {
    // Patched by an earlier payload: find its slot and code page.
    int32_t rel;
    memcpy(&rel, cur + 2, 4);
    g_slot = reinterpret_cast<void**>(site + 6 + rel);
    g_code = reinterpret_cast<CodePage*>(reinterpret_cast<uint8_t*>(g_slot) - kPage);
    if (g_code->magic != kMagic || g_code->site != site) {
      Log("pacer: unknown patch at Visualizer+0x%x; skipped", siteRva);
      return false;
    }
    // Keep its stubs as they are: rewriting the code page now could change
    // its protection while the Main thread runs it.
    g_state = 1;
    Log("pacer: existing pacer patch reused");
    return true;
  } else if (memcmp(cur, kPauses, 8) == 0) {
    uint8_t* mem = AllocNear(site);
    if (!mem) {
      Log("pacer: no memory within reach of Visualizer.dll; skipped");
      return false;
    }
    g_code = reinterpret_cast<CodePage*>(mem);
    g_slot = reinterpret_cast<void**>(mem + kPage);
    memset(mem, 0xCC, kPage);
    uint8_t* p = g_code->pauseStub;
    Emit(p, {0xF3, 0x90, 0xF3, 0x90, 0xF3, 0x90, 0xF3, 0x90, 0xC3});  // 4x pause; ret
    g_code->magic = kMagic;
    g_code->site = site;
  } else {
    Log("pacer: Visualizer+0x%x has unexpected bytes; skipped", siteRva);
    return false;
  }
  // Write the MWAITX stub (fresh pages: nothing executes them yet).
  DWORD old;
  EmitMwaitStub(g_code->mwaitStub, g_timeoutTicks);
  VirtualProtect(g_code, kPage, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), g_code, kPage);
  {
    *g_slot = g_code->pauseStub;  // starts off: the four PAUSEs, unchanged behaviour
    uint8_t patch[8] = {0xFF, 0x15, 0, 0, 0, 0, 0x66, 0x90};  // call [rip+rel]; nop
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(g_slot) - (site + 6));
    memcpy(patch + 2, &rel, 4);
    if (!PatchSite(site, patch)) {
      Log("pacer: could not patch the pacer loop safely; skipped");
      return false;
    }
  }
  g_state = 1;
  Log("pacer: Main-thread pacer loop patched (MWAITX timeout %d us)", std::max(2, timeoutUs));
  return true;
}

// Switches between the original four PAUSEs and the MWAITX wait by
// pointing the slot (data page) at the matching stub.
void SetLowPower(bool on) {
  if (g_state.load() <= 0) return;
  void* target = on ? static_cast<void*>(g_code->mwaitStub) : static_cast<void*>(g_code->pauseStub);
  InterlockedExchangePointer(g_slot, target);
  g_on = on;
}

}  // namespace pacer
