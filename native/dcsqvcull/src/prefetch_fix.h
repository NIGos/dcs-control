// Terrain prefetch fix ([Prefetch] Mode=off|observe|skip, WindowMs).
//
// Port of DCS Control's dxgi2.dll prefetch fix (dlss5vrframegen
// native\prefetch_fix\prefetch_fix.cpp) into the payload, so DCS Control no
// longer needs a dxgi.dll/dxgi2.dll loader chain in DCS's bin folder.
//
// DCS's terrain workers (edterrain4 through edCore) call
// PrefetchVirtualMemory about 840k times/s over the same ranges, about 1.7
// cores of kernel time. Prefetching is only a hint to the memory manager:
// dropping a repeat changes no result (pages fault in on first access).
//
// No code is patched. edCore resolves the function with GetProcAddress, so the
// fix replaces stored function pointers in the game's own modules' writable
// data, and their GetProcAddress import slots (kernel32's or KernelBase's),
// never instructions. Only modules under the DCS install root (the parent of
// the executable's folder) are touched; never this payload, an older payload
// generation, or the old dxgi2 fix's own module.
//
// Replacement: a call over the same (first address, range count, total
// bytes) as one seen less than WindowMs ago returns TRUE without the system
// call. 256 back-to-back skips on one thread within 10 ms yield one tick
// (spin guard). Observe = every call passes through (counted).
//
// Lifetime (hot reload, kill switch, Mode=off):
//   * Every patched slot is recorded and restored on uninstall to the real
//     kernel function, only when it still holds our value (CAS, under SEH,
//     with VirtualProtect for import slots), and only when its module is
//     still the same loaded image (pinned for the write).
//   * Pointers DCS copied elsewhere (heap copies of a GetProcAddress result)
//     cannot be found again, so the replacement must stay callable forever:
//     the payload module is pinned (GET_MODULE_HANDLE_EX_FLAG_PIN) at the
//     first install, and after uninstall the replacement is a plain
//     pass-through (g_mode = off). The loader never unloads payloads either.
//   * In-flight calls are counted (per-thread-striped enter/exit counters);
//     uninstall waits for them to drain (bounded) before it returns.
//
// Old dxgi2 fix (users migrating from DCS Control's loader chain): when it is
// loaded and has patched slots, ours takes over. Its GetProcAddress
// replacement is learned from a game module's import slot that points into
// its image, its PrefetchVirtualMemory replacement from what that function
// returns; slots holding either are replaced with ours (restored to the real
// kernel function on our uninstall). Its worker scans each module once, so it
// never re-patches a module we took over; should it (or anything) write a
// slot again, the per-second verify re-takes it and logs that once. Its
// module is never written to. Previous payload generations are recognized
// the same way through their DcsQvPayload_PrefetchHooks export.
//
// Timing: DcsQvCull starts from a Lua hook after edCore is loaded; terrain
// DLLs load per mission. New modules are caught through an ntdll DLL load
// notification (scanned within a second, by the payload worker) plus a full
// module enumeration every second for the first minute after install, then
// every 10 s (the original's schedule).
//
// Statistics: one line every 10 s to DcsQvCullPrefetch.log next to
// DcsQvCull.log: "YYYY-MM-DD HH:MM:SS calls/s=<n> skipped=<x.x>% pointers=<n>
// imports=<n> lookups=<n> faults=<n>" (local time, UTF-8).
//
// Included once from main.cpp inside its anonymous namespace.
#pragma once

namespace pffix {

using PrefetchFn = BOOL(WINAPI*)(HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG);
using GetProcFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);

enum Mode : int { kOff = 0, kObserve = 1, kSkip = 2 };

constexpr unsigned kSpinSkips = 256;    // consecutive skips on one thread...
constexpr uint64_t kSpinWindowMs = 10;  // ...within this time: yield one tick
constexpr uint32_t kDefaultWindowMs = 1000;

// Function addresses kept in this module's own data are stored XOR-encoded:
// a scan of a module that contains this code (the offline tests scan their
// own executable) must never find and redirect the fix's own real pointer.
constexpr uintptr_t kKey = 0x9E3779B97F4A7C15ull;
inline uintptr_t Enc(const void* p) { return reinterpret_cast<uintptr_t>(p) ^ kKey; }
inline void* Dec(uintptr_t v) { return reinterpret_cast<void*>(v ^ kKey); }

// ---- hot path state ----
std::atomic<int> g_mode{kOff};  // effective mode read by the replacement
std::atomic<uint32_t> g_windowMs{kDefaultWindowMs};
std::atomic<uintptr_t> g_realEnc{0};  // PrefetchFn (KernelBase), Enc()
uintptr_t g_kbPrefetchEnc = 0, g_k32PrefetchEnc = 0;  // kernel targets DCS may hold
uintptr_t g_kbGetProcEnc = 0, g_k32GetProcEnc = 0;    // GetProcAddress import values

#pragma warning(push)
#pragma warning(disable : 4324)  // padded to a cache line on purpose
struct alignas(64) Stripe {
  std::atomic<uint64_t> calls{0};    // entered the replacement
  std::atomic<uint64_t> exits{0};    // left it
  std::atomic<uint64_t> skipped{0};  // returned without the system call
};
#pragma warning(pop)
constexpr unsigned kStripes = 64;
Stripe g_stripes[kStripes];
// Thread id from the TEB (ClientId.UniqueThread, gs:[0x48] on x64): no call.
inline Stripe& MyStripe() { return g_stripes[(__readgsdword(0x48) >> 2) & (kStripes - 1)]; }

// Repeat table, lock-free: one 64-bit word per entry, tag (40 bits of the
// key's hash, never 0) | tick of the last pass-through (24 bits, ms). A lost
// race or a collision only makes a call pass through (or, at 2^-40 odds,
// skips a hint): never a wrong result.
constexpr unsigned kTableBits = 16;
std::atomic<uint64_t> g_table[1u << kTableBits];

// R39: [Prefetch] TableBits (16-22, default 20 since R41; 16 = the static table above). The
// table is direct-mapped, so a range whose slot another range took since its
// last call passes through although it is a repeat. Live (F-4 Germany,
// 2026-10-16) about 2.7-3.1 M calls/s still pass 29 % to the kernel with 2^16
// entries, which a collision rate of e^(-K/2^16) explains for K = about 22 k
// distinct ranges per loader cycle [I]. A larger table only finds more true
// repeats (same key, inside the window), so the hints DCS sees per window are
// unchanged. Larger tables are allocated on first use and never freed: a
// caller may still hold the old pointer, and the module is pinned anyway.
constexpr unsigned kMinTableBits = kTableBits, kMaxTableBits = 22;
struct Table {
  std::atomic<uint64_t>* e;
  uint64_t mask;
  unsigned bits;
};
Table g_staticTable{g_table, (1u << kTableBits) - 1, kTableBits};
Table g_bigTables[kMaxTableBits + 1] = {};  // [bits]: e == nullptr until first use
std::atomic<const Table*> g_tab{&g_staticTable};

inline unsigned ClampTableBits(unsigned bits) {
  return bits < kMinTableBits ? kMinTableBits : bits > kMaxTableBits ? kMaxTableBits : bits;
}
unsigned TableBits() { return g_tab.load(std::memory_order_acquire)->bits; }
void ClearTable(const Table& t) {
  for (uint64_t i = 0; i <= t.mask; ++i) t.e[i].store(0, std::memory_order_relaxed);
}
// Worker / config thread. False (table unchanged) when the allocation fails.
bool SetTableBits(unsigned bits) {
  bits = ClampTableBits(bits);
  const Table* cur = g_tab.load(std::memory_order_acquire);
  if (cur->bits == bits) return true;
  Table* t = &g_staticTable;
  if (bits != kTableBits) {
    t = &g_bigTables[bits];
    if (!t->e) {
      const size_t n = size_t(1) << bits;
      void* m = VirtualAlloc(nullptr, n * sizeof(std::atomic<uint64_t>), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      if (!m) return false;
      t->e = static_cast<std::atomic<uint64_t>*>(m);  // zero-filled by the system
      t->mask = n - 1;
      t->bits = bits;
    } else {
      ClearTable(*t);  // not published now: stale writes from a caller that still holds it are only hints
    }
  } else {
    ClearTable(*t);
  }
  g_tab.store(t, std::memory_order_release);
  return true;
}

thread_local uint32_t t_skipRun = 0;
thread_local uint64_t t_spinStart = 0;

inline bool SeenRecently(ULONG_PTR address, ULONG_PTR count, ULONG_PTR bytes, uint64_t now, uint32_t window) {
  uint64_t h = address * 0x9E3779B97F4A7C15ull ^ count * 0xC2B2AE3D27D4EB4Full ^ bytes * 0x165667B19E3779F9ull;
  h ^= h >> 31;
  h *= 0xD6E8FEB86659FD93ull;
  h ^= h >> 32;
  // Index bits 0..21 and tag bits 24..63 never overlap (kMaxTableBits <= 24).
  const Table* tab = g_tab.load(std::memory_order_acquire);
  std::atomic<uint64_t>& e = tab->e[h & tab->mask];
  const uint64_t tag = (h >> 24) | 1;
  const uint64_t cur = e.load(std::memory_order_relaxed);
  if ((cur >> 24) == tag && ((now - cur) & 0xFFFFFF) < window) return true;
  e.store((tag << 24) | (now & 0xFFFFFF), std::memory_order_relaxed);
  return false;
}

BOOL WINAPI Replacement(HANDLE process, ULONG_PTR count, PWIN32_MEMORY_RANGE_ENTRY entries, ULONG flags) {
  Stripe& s = MyStripe();
  s.calls.fetch_add(1);
  if (g_mode.load(std::memory_order_relaxed) == kSkip && process == reinterpret_cast<HANDLE>(-1) && entries &&
      count) {
    ULONG_PTR bytes = 0;
    for (ULONG_PTR i = 0; i < count; ++i) bytes += entries[i].NumberOfBytes;
    const uint64_t now = GetTickCount64();
    if (SeenRecently(reinterpret_cast<ULONG_PTR>(entries[0].VirtualAddress), count, bytes, now,
                     g_windowMs.load(std::memory_order_relaxed))) {
      s.skipped.fetch_add(1, std::memory_order_relaxed);
      // A caller spinning on these calls would turn a dropped prefetch into a
      // user-mode spin: yield one tick, only for back-to-back skips.
      if (t_skipRun++ == 0) t_spinStart = now;
      if (t_skipRun >= kSpinSkips) {
        if (now - t_spinStart < kSpinWindowMs) Sleep(1);
        t_skipRun = 0;
      }
      s.exits.fetch_add(1, std::memory_order_release);
      return TRUE;
    }
  }
  t_skipRun = 0;
  const BOOL r = reinterpret_cast<PrefetchFn>(Dec(g_realEnc.load(std::memory_order_relaxed)))(process, count,
                                                                                              entries, flags);
  s.exits.fetch_add(1, std::memory_order_release);
  return r;
}

std::atomic<uint64_t> g_lookups{0};  // GetProcAddress lookups answered with the replacement

bool IsKernelPrefetch(const void* p) {
  return p && (p == Dec(g_kbPrefetchEnc) || p == Dec(g_k32PrefetchEnc));
}
bool IsKernelGetProc(const void* p) { return p && (p == Dec(g_kbGetProcEnc) || p == Dec(g_k32GetProcEnc)); }

FARPROC WINAPI FixedGetProc(HMODULE module, LPCSTR name) {
  const FARPROC r = reinterpret_cast<GetProcFn>(Dec(g_kbGetProcEnc))(module, name);
  // Only the system function is replaced, never another module's export.
  if (IsKernelPrefetch(reinterpret_cast<const void*>(r)) && g_mode.load(std::memory_order_relaxed) != kOff) {
    g_lookups.fetch_add(1, std::memory_order_relaxed);
    return reinterpret_cast<FARPROC>(&Replacement);
  }
  return r;
}

uint64_t SumCalls() {
  uint64_t n = 0;
  for (const Stripe& s : g_stripes) n += s.calls.load(std::memory_order_relaxed);
  return n;
}
uint64_t SumSkipped() {
  uint64_t n = 0;
  for (const Stripe& s : g_stripes) n += s.skipped.load(std::memory_order_relaxed);
  return n;
}
// Threads inside the replacement (0 = none at some instant of each stripe's
// read: exits are read before calls, so the result never undercounts).
uint64_t InFlight() {
  uint64_t e = 0, c = 0;
  for (const Stripe& s : g_stripes) e += s.exits.load(std::memory_order_acquire);
  for (const Stripe& s : g_stripes) c += s.calls.load(std::memory_order_acquire);
  return c > e ? c - e : 0;
}

// ---- install state (payload worker; tests) ----
std::atomic<int> g_state{0};  // 0 = not installed, 1 = installed, -1 = unavailable this session (latched)
std::atomic<uint32_t> g_livePointers{0}, g_liveImports{0};
std::atomic<uint64_t> g_faults{0}, g_takeovers{0}, g_retakes{0};
bool g_resolved = false;
bool g_pinned = false;
bool g_warLogged = false;
bool g_oldLogged = false;
std::wstring g_root;     // install root with trailing backslash
std::wstring g_logPath;  // DcsQvCullPrefetch.log (set from g_dir at install)
uint64_t g_installMs = 0, g_lastFullMs = 0, g_lastLogMs = 0;
uint64_t g_logCalls0 = 0, g_logSkipped0 = 0;

// Test hooks: a non-empty module list replaces enumeration and the candidate
// filter; extra foreign replacements (a simulated old fix).
std::vector<HMODULE> g_testModules;
struct Foreign {
  void* prefetch;  // its PrefetchVirtualMemory replacement (0 = not learned)
  void* getProc;   // its GetProcAddress replacement
  bool oldFix;     // the dxgi2 fix (else a previous payload generation)
  std::wstring module;
};
std::vector<Foreign> g_testForeign;
std::vector<Foreign> g_foreign;  // this scan's (heap: never inside a scanned image)

struct Rec {
  void** slot;
  void* restore;  // real kernel function
  HMODULE mod;
  uint32_t stamp, size;  // image identity at patch time
  bool import;
};
std::vector<Rec> g_recs;
struct Seen {
  uint32_t stamp, size;
};
std::unordered_map<HMODULE, Seen> g_done;  // modules scanned (or rejected) at this identity

// ---- DLL load notifications (ntdll) ----
struct LdrNotifData {
  ULONG Flags;
  const void* FullDllName;
  const void* BaseDllName;
  void* DllBase;
  ULONG SizeOfImage;
};
using LdrNotifFn = VOID(CALLBACK*)(ULONG, const LdrNotifData*, PVOID);
using LdrRegFn = LONG(NTAPI*)(ULONG, LdrNotifFn, PVOID, PVOID*);
using LdrUnregFn = LONG(NTAPI*)(PVOID);
void* g_notifCookie = nullptr;
std::atomic<uintptr_t> g_loaded[32];  // bases loaded since the last tick
std::atomic<uint32_t> g_loadedHead{0};
std::atomic<bool> g_loadEvent{false}, g_loadOverflow{false};

// Runs under the loader lock: only records the base (no lock, no call).
VOID CALLBACK OnDllNotification(ULONG reason, const LdrNotifData* data, PVOID) {
  if (reason != 1 || !data) return;  // 1 = loaded (unloads are caught by the liveness check)
  const uint32_t i = g_loadedHead.fetch_add(1) & 31;
  if (g_loaded[i].exchange(reinterpret_cast<uintptr_t>(data->DllBase)) != 0) g_loadOverflow = true;
  g_loadEvent = true;
}

// ---- guarded memory access (no C++ objects: __try) ----
bool ImageIdentity(HMODULE m, uint32_t* stamp, uint32_t* size) {
  __try {
    auto* base = reinterpret_cast<const BYTE*>(m);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    *stamp = nt->FileHeader.TimeDateStamp;
    *size = nt->OptionalHeader.SizeOfImage;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Section flags are not the runtime protection: only committed read-write
// pages without a guard are scanned. One query per region (same protection
// up to *end), not per page.
bool WritableData(const void* address, ULONG_PTR* end) {
  MEMORY_BASIC_INFORMATION info{};
  if (!VirtualQuery(address, &info, sizeof(info))) {
    *end = (reinterpret_cast<ULONG_PTR>(address) | 0xFFF) + 1;
    return false;
  }
  *end = reinterpret_cast<ULONG_PTR>(info.BaseAddress) + info.RegionSize;
  if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
  const DWORD access = info.Protect & 0xFF;
  return access == PAGE_READWRITE || access == PAGE_WRITECOPY;
}

struct Hit {
  void** slot;
  void* value;
  bool import;  // GetProcAddress import slot (else a data pointer)
};

// Data slots holding one of pf[] (writable, non-executable, non-shared
// sections) and every GetProcAddress import slot with its value. False on a
// contained fault.
bool CollectGuarded(HMODULE module, void* const* pf, int npf, Hit* out, int cap, int* n) {
  *n = 0;
  __try {
    auto* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return true;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return true;
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
      const DWORD c = section->Characteristics;
      if (!(c & IMAGE_SCN_MEM_WRITE) || (c & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_SHARED))) continue;
      auto** begin = reinterpret_cast<void**>(base + section->VirtualAddress);
      const size_t slots = section->Misc.VirtualSize / sizeof(void*);
      ULONG_PTR regionEnd = 0;
      bool regionOk = false;
      for (size_t s = 0; s < slots; ++s) {
        const ULONG_PTR at = reinterpret_cast<ULONG_PTR>(&begin[s]);
        if (at >= regionEnd) regionOk = WritableData(&begin[s], &regionEnd);
        if (!regionOk) continue;
        void* v = begin[s];
        if (!v) continue;
        for (int k = 0; k < npf; ++k)
          if (v == pf[k]) {
            if (*n < cap) out[(*n)++] = Hit{&begin[s], v, false};
            break;
          }
      }
    }
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return true;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
      if (!desc->OriginalFirstThunk) continue;
      auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
      auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
      for (; names->u1.AddressOfData; ++names, ++slots) {
        if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
        auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
        if (strcmp(reinterpret_cast<const char*>(byName->Name), "GetProcAddress") != 0) continue;
        if (*n < cap)
          out[(*n)++] = Hit{reinterpret_cast<void**>(&slots->u1.Function), reinterpret_cast<void*>(slots->u1.Function),
                            true};
      }
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Writes value into *slot if it still holds expect. Import slots may sit on
// read-only (or code-sharing) pages: made writable for the store only.
// 1 = written, 0 = slot held something else (or not writable), -1 = fault.
int CasGuarded(void** slot, void* expect, void* value, bool import) {
  __try {
    DWORD old = 0;
    bool reprotect = false;
    if (import) {
      MEMORY_BASIC_INFORMATION info{};
      if (!VirtualQuery(slot, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD))
        return 0;
      const DWORD access = info.Protect & 0xFF;
      if (access != PAGE_READWRITE && access != PAGE_EXECUTE_READWRITE) {
        const bool exec = (access & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_WRITECOPY)) != 0;
        if (!VirtualProtect(slot, sizeof(void*), exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old)) return 0;
        reprotect = true;
      }
    }
    // Aligned pointer-sized stores are atomic on x64: a concurrent caller
    // gets either function.
    void* prev = InterlockedCompareExchangePointer(slot, value, expect);
    if (reprotect) VirtualProtect(slot, sizeof(void*), old, &old);
    return prev == expect ? 1 : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

bool ReadGuarded(void* const* slot, void** out) {
  __try {
    *out = *reinterpret_cast<void* volatile const*>(slot);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Asks the old fix's GetProcAddress replacement what it hands out for
// PrefetchVirtualMemory (its own replacement while it is active).
void* AskGetProcGuarded(void* getProc, HMODULE kernel32) {
  __try {
    return reinterpret_cast<void*>(reinterpret_cast<GetProcFn>(getProc)(kernel32, "PrefetchVirtualMemory"));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

// Export by name without GetProcAddress (which may itself be redirected by
// an old fix in some import table): never chains through another fix.
// Null for forwarders.
void* ExportByName(HMODULE module, const char* name) {
  if (!module) return nullptr;
  auto* base = reinterpret_cast<const BYTE*>(module);
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (!dir.VirtualAddress) return nullptr;
  auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
  auto* names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
  auto* ords = reinterpret_cast<const WORD*>(base + exp->AddressOfNameOrdinals);
  auto* funcs = reinterpret_cast<const DWORD*>(base + exp->AddressOfFunctions);
  for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
    if (strcmp(reinterpret_cast<const char*>(base + names[i]), name) != 0) continue;
    const DWORD rva = funcs[ords[i]];
    if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) return nullptr;  // forwarder
    return const_cast<BYTE*>(base + rva);
  }
  return nullptr;
}

// ---- helpers ----
HMODULE SelfModule() {
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&Replacement), &self);
  return self;
}

std::wstring ModulePath(HMODULE m) {
  wchar_t path[MAX_PATH * 2] = {};
  const DWORD n = GetModuleFileNameW(m, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
  return n && n < sizeof(path) / sizeof(path[0]) ? std::wstring(path, n) : std::wstring();
}

// Install root: the parent of the executable's folder (DCSWorld\ for
// DCSWorld\bin\DCS.exe or bin-mt\DCS.exe), with a trailing backslash.
std::wstring RootOf(const std::wstring& exe) {
  const size_t a = exe.find_last_of(L"\\/");
  if (a == std::wstring::npos || a == 0) return std::wstring();
  const size_t b = exe.find_last_of(L"\\/", a - 1);
  if (b == std::wstring::npos) return std::wstring();
  return exe.substr(0, b + 1);
}

bool UnderRoot(const std::wstring& path, const std::wstring& root) {
  return !root.empty() && path.size() > root.size() && _wcsnicmp(path.c_str(), root.c_str(), root.size()) == 0;
}

// A game module: under the install root, and none of ours or the old fix's
// (those keep the real function in their own data).
bool IsCandidate(HMODULE m, const std::wstring& path) {
  if (!UnderRoot(path, g_root) || m == SelfModule()) return false;
  return !ExportByName(m, "DcsVrPrefetchFix_Stats") && !ExportByName(m, "DcsQvPayload_Start") &&
         !ExportByName(m, "luaopen_DcsQvCull") && !ExportByName(m, "DcsQvPayload_PrefetchHooks");
}

// The stats log is appended to across sessions (a line every 10 s while DCS
// runs): when it has grown past kLogCap it becomes <name>.prev.log (one
// previous file kept, replaced) and a new one starts. Called at Install.
constexpr uint64_t kLogCap = 1u << 20;
std::wstring PrevLogPath(const std::wstring& path) {
  const size_t dot = path.find_last_of(L'.');
  const size_t sep = path.find_last_of(L"\\/");
  const bool ext = dot != std::wstring::npos && (sep == std::wstring::npos || dot > sep);
  return (ext ? path.substr(0, dot) : path) + L".prev.log";
}
bool RotateLogIfLarge(const std::wstring& path, uint64_t cap) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
  const uint64_t size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
  if (size <= cap) return false;
  return MoveFileExW(path.c_str(), PrevLogPath(path).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

void LogFileLine(const char* text) {
  if (g_logPath.empty()) return;
  const HANDLE file = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  SYSTEMTIME t{};
  GetLocalTime(&t);
  char line[600];
  const int n = snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u %s\r\n", t.wYear, t.wMonth, t.wDay,
                         t.wHour, t.wMinute, t.wSecond, text);
  DWORD written = 0;
  if (n > 0) WriteFile(file, line, static_cast<DWORD>(std::min(n, static_cast<int>(sizeof(line)) - 1)), &written, nullptr);
  CloseHandle(file);
}

void FormatStats(char* out, size_t n, uint64_t calls, uint64_t skipped, double seconds) {
  snprintf(out, n, "calls/s=%llu skipped=%.1f%% pointers=%u imports=%u lookups=%llu faults=%llu",
           static_cast<unsigned long long>(seconds > 0 ? static_cast<double>(calls) / seconds + 0.5 : 0),
           calls ? 100.0 * static_cast<double>(skipped) / static_cast<double>(calls) : 0.0, g_livePointers.load(),
           g_liveImports.load(), static_cast<unsigned long long>(g_lookups.load()),
           static_cast<unsigned long long>(g_faults.load()));
}

void UpdateLiveCounts() {
  uint32_t p = 0, i = 0;
  for (const Rec& r : g_recs) (r.import ? i : p)++;
  g_livePointers = p;
  g_liveImports = i;
}

bool HasRecord(void** slot) {
  for (const Rec& r : g_recs)
    if (r.slot == slot) return true;
  return false;
}

// Pins the module that holds rec's slot if it is still the image we patched.
bool PinRecModule(const Rec& r, HMODULE* pinned) {
  HMODULE h = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(r.slot), &h)) return false;
  uint32_t stamp = 0, size = 0;
  if (h != r.mod || !ImageIdentity(h, &stamp, &size) || stamp != r.stamp || size != r.size) {
    FreeLibrary(h);
    return false;
  }
  *pinned = h;
  return true;
}

const Foreign* ForeignPrefetch(const void* v) {
  for (const Foreign& f : g_foreign)
    if (f.prefetch && f.prefetch == v) return &f;
  return nullptr;
}
const Foreign* ForeignGetProc(const void* v) {
  for (const Foreign& f : g_foreign)
    if (f.getProc && f.getProc == v) return &f;
  return nullptr;
}

// Old dxgi2 fix modules (exporting DcsVrPrefetchFix_Stats), learned or not.
struct OldModule {
  HMODULE mod;
  const BYTE* lo;
  const BYTE* hi;
  std::wstring path;
};

void NoteTakeover(const Foreign* f) {
  g_takeovers.fetch_add(1);
  if (f && f->oldFix && !g_oldLogged) {
    g_oldLogged = true;
    char line[700];
    qvlog::Format(line, sizeof(line), "prefetch: old dxgi2 fix detected (module %ls), taken over", f->module.c_str());
    Log("%s", line);
    LogFileLine(line);
  }
}

// One module: replaces the real function (or a foreign replacement) in its
// data and GetProcAddress imports, records slots already ours. Returns true
// when an old fix's GetProcAddress replacement was learned from an import
// (the caller then rescans with the new foreign set).
bool ScanModule(HMODULE m, const std::vector<OldModule>& olds) {
  uint32_t stamp = 0, size = 0;
  if (!ImageIdentity(m, &stamp, &size)) return false;
  void* pf[20];
  int npf = 0;
  pf[npf++] = Dec(g_kbPrefetchEnc);
  if (Dec(g_k32PrefetchEnc) != pf[0]) pf[npf++] = Dec(g_k32PrefetchEnc);
  pf[npf++] = reinterpret_cast<void*>(&Replacement);
  for (const Foreign& f : g_foreign)
    if (f.prefetch && npf < 20) pf[npf++] = f.prefetch;
  // On the heap: a static buffer of found values would itself be found when
  // a scanned image contains this code (the offline tests).
  std::vector<Hit> hits(512);
  int n = 0;
  if (!CollectGuarded(m, pf, npf, hits.data(), static_cast<int>(hits.size()), &n)) {
    g_faults.fetch_add(1);
    Log("prefetch: module scan fault contained (%ls)", ModulePath(m).c_str());
  }
  bool learned = false;
  void* const ours[2] = {reinterpret_cast<void*>(&Replacement), reinterpret_cast<void*>(&FixedGetProc)};
  for (int i = 0; i < n; ++i) {
    const Hit h = hits[i];
    if (HasRecord(h.slot)) continue;
    void* restore = nullptr;
    const Foreign* taken = nullptr;
    if (!h.import) {
      if (h.value == ours[0]) {
        restore = Dec(g_kbPrefetchEnc);  // copied into module data after a lookup: ours already
      } else if (IsKernelPrefetch(h.value)) {
        restore = h.value;
      } else if ((taken = ForeignPrefetch(h.value)) != nullptr) {
        restore = Dec(g_kbPrefetchEnc);
      }
    } else {
      if (h.value == ours[1]) {
        restore = Dec(g_k32GetProcEnc);
      } else if (IsKernelGetProc(h.value)) {
        restore = h.value;
      } else if ((taken = ForeignGetProc(h.value)) != nullptr) {
        restore = Dec(g_k32GetProcEnc);
      } else {
        // An import pointing into an old fix's image: its GetProcAddress
        // replacement. Learn its PrefetchVirtualMemory replacement too.
        for (const OldModule& o : olds) {
          auto* v = static_cast<const BYTE*>(h.value);
          if (v < o.lo || v >= o.hi) continue;
          void* p = AskGetProcGuarded(h.value, GetModuleHandleW(L"kernel32.dll"));
          auto* pb = static_cast<const BYTE*>(p);
          g_foreign.push_back(Foreign{pb >= o.lo && pb < o.hi ? p : nullptr, h.value, true, o.path});
          learned = true;
          taken = &g_foreign.back();
          restore = Dec(g_k32GetProcEnc);
          break;
        }
      }
    }
    if (!restore) continue;
    void* want = ours[h.import ? 1 : 0];
    int r = 1;
    if (h.value != want) r = CasGuarded(h.slot, h.value, want, h.import);
    if (r < 0) g_faults.fetch_add(1);
    if (r != 1) continue;
    g_recs.push_back(Rec{h.slot, restore, m, stamp, size, h.import});
    if (taken) NoteTakeover(taken);
  }
  return learned;
}

// Modules to scan, each pinned for the scan (FreeLibrary afterwards).
std::vector<HMODULE> PinnedModules() {
  std::vector<HMODULE> out;
  if (!g_testModules.empty()) {
    for (HMODULE m : g_testModules) {
      HMODULE h = nullptr;
      if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(m), &h)) out.push_back(h);
    }
    return out;
  }
  std::vector<HMODULE> listed(4096);
  DWORD needed = 0;
  if (!EnumProcessModules(GetCurrentProcess(), listed.data(), static_cast<DWORD>(listed.size() * sizeof(HMODULE)),
                          &needed))
    return out;
  listed.resize(std::min<size_t>(listed.size(), needed / sizeof(HMODULE)));
  for (HMODULE m : listed) {
    HMODULE h = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(m), &h)) continue;
    if (h != m) {
      FreeLibrary(h);
      continue;
    }
    out.push_back(h);
  }
  return out;
}

// Foreign replacements: previous payload generations (their export) and the
// old dxgi2 fix (learned from import slots in ScanModule). Learned entries
// are kept across scans of one install.
void RefreshForeign(const std::vector<HMODULE>& mods, std::vector<OldModule>& olds) {
  const HMODULE self = SelfModule();
  for (const Foreign& t : g_testForeign)
    if (!ForeignPrefetch(t.prefetch) && !ForeignGetProc(t.getProc)) g_foreign.push_back(t);
  if (!g_testModules.empty()) return;
  for (HMODULE m : mods) {
    if (m == self) continue;
    if (auto hooks = reinterpret_cast<void* (*)(unsigned)>(ExportByName(m, "DcsQvPayload_PrefetchHooks"))) {
      void* p = hooks(0);
      void* g = hooks(1);
      if (p && !ForeignPrefetch(p)) g_foreign.push_back(Foreign{p, g, false, ModulePath(m)});
      continue;
    }
    if (ExportByName(m, "DcsVrPrefetchFix_Stats")) {
      uint32_t stamp = 0, size = 0;
      if (!ImageIdentity(m, &stamp, &size)) continue;
      auto* lo = reinterpret_cast<const BYTE*>(m);
      olds.push_back(OldModule{m, lo, lo + size, ModulePath(m)});
    }
  }
}

// Scans modules not yet seen at their current identity (all of them when
// full or after a load-notification overflow).
void Scan() {
  for (auto& slot : g_loaded) {
    const uintptr_t base = slot.exchange(0);
    if (base) g_done.erase(reinterpret_cast<HMODULE>(base));
  }
  if (g_loadOverflow.exchange(false)) g_done.clear();
  std::vector<HMODULE> mods = PinnedModules();
  std::vector<OldModule> olds;
  RefreshForeign(mods, olds);
  for (int pass = 0; pass < 2; ++pass) {
    bool learned = false;
    for (HMODULE m : mods) {
      uint32_t stamp = 0, size = 0;
      if (!ImageIdentity(m, &stamp, &size)) continue;
      auto it = g_done.find(m);
      if (it != g_done.end() && it->second.stamp == stamp && it->second.size == size) continue;
      g_done[m] = Seen{stamp, size};
      if (g_testModules.empty() && !IsCandidate(m, ModulePath(m))) continue;
      learned |= ScanModule(m, olds);
    }
    if (!learned) break;
    g_done.clear();  // a learned replacement may sit in modules scanned before
  }
  for (HMODULE m : mods) FreeLibrary(m);
  UpdateLiveCounts();
}

// Every recorded slot still ours? A slot written back to the real function or
// a foreign replacement is re-taken (logged once: no patch war, ours is
// kept); anything else, or a module that went away, drops the record.
void Verify() {
  void* const ours[2] = {reinterpret_cast<void*>(&Replacement), reinterpret_cast<void*>(&FixedGetProc)};
  for (size_t i = 0; i < g_recs.size();) {
    Rec& r = g_recs[i];
    HMODULE pinned = nullptr;
    bool keep = PinRecModule(r, &pinned);
    if (keep) {
      void* v = nullptr;
      const void* want = ours[r.import ? 1 : 0];
      if (!ReadGuarded(r.slot, &v)) {
        keep = false;
      } else if (v != want) {
        const bool retake =
            r.import ? (IsKernelGetProc(v) || ForeignGetProc(v)) : (IsKernelPrefetch(v) || ForeignPrefetch(v));
        if (retake && CasGuarded(r.slot, v, const_cast<void*>(want), r.import) == 1) {
          g_retakes.fetch_add(1);
          if (!g_warLogged) {
            g_warLogged = true;
            char line[700];
            qvlog::Format(line, sizeof(line),
                          "prefetch: a patched slot was written again by another fix or the game (module %ls); "
                          "re-taken, ours kept (logged once)",
                          ModulePath(r.mod).c_str());
            Log("%s", line);
            LogFileLine(line);
          }
        } else {
          keep = false;
        }
      }
      FreeLibrary(pinned);
    }
    if (keep) {
      ++i;
    } else {
      g_recs[i] = g_recs.back();
      g_recs.pop_back();
    }
  }
  UpdateLiveCounts();
}

bool Resolve() {
  if (g_resolved) return true;
  const HMODULE kb = GetModuleHandleW(L"KernelBase.dll"), k32 = GetModuleHandleW(L"kernel32.dll");
  auto getProc = reinterpret_cast<GetProcFn>(ExportByName(kb, "GetProcAddress"));
  if (!getProc || !k32) return false;
  void* kbPf = reinterpret_cast<void*>(getProc(kb, "PrefetchVirtualMemory"));
  void* k32Pf = reinterpret_cast<void*>(getProc(k32, "PrefetchVirtualMemory"));
  void* k32Gp = reinterpret_cast<void*>(getProc(k32, "GetProcAddress"));
  if (!kbPf || !k32Pf || !k32Gp) return false;
  g_kbPrefetchEnc = Enc(kbPf);
  g_k32PrefetchEnc = Enc(k32Pf);
  g_kbGetProcEnc = Enc(reinterpret_cast<void*>(getProc));
  g_k32GetProcEnc = Enc(k32Gp);
  g_realEnc = Enc(kbPf);
  g_resolved = true;
  return true;
}

bool Install(int mode) {
  if (g_state.load() != 0) return g_state.load() > 0;
  if (!Resolve()) {
    g_state = -1;
    Log("prefetch: PrefetchVirtualMemory or GetProcAddress not found; off for this session");
    return false;
  }
  if (g_testModules.empty()) {
    g_root = RootOf(ModulePath(nullptr));
    if (g_root.empty()) {
      g_state = -1;
      Log("prefetch: install folder unknown; off for this session");
      return false;
    }
  }
  if (!g_pinned) {
    // Redirected pointers lead into this module, and copies DCS made of them
    // cannot be found again: it must never unload.
    HMODULE pinned = nullptr;
    g_pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                  reinterpret_cast<LPCWSTR>(&Replacement), &pinned) != 0;
    if (!g_pinned) {
      g_state = -1;
      Log("prefetch: payload module could not be pinned; off for this session");
      return false;
    }
  }
  if (g_logPath.empty()) g_logPath = g_dir + L"DcsQvCullPrefetch.log";
  RotateLogIfLarge(g_logPath, kLogCap);
  for (auto& e : g_table) e.store(0, std::memory_order_relaxed);
  if (const Table* t = g_tab.load(std::memory_order_acquire); t != &g_staticTable) ClearTable(*t);
  g_warLogged = false;
  g_oldLogged = false;
  g_mode = mode;
  if (!g_notifCookie) {
    auto reg = reinterpret_cast<LdrRegFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
    if (reg && reg(0, &OnDllNotification, nullptr, &g_notifCookie) != 0) g_notifCookie = nullptr;
  }
  g_state = 1;
  g_installMs = g_lastFullMs = g_lastLogMs = GetTickCount64();
  g_logCalls0 = SumCalls();
  g_logSkipped0 = SumSkipped();
  Scan();
  char line[256];
  snprintf(line, sizeof(line), "prefetch fix started mode=%s window_ms=%u pointers=%u imports=%u",
           mode == kSkip ? "skip" : "observe", g_windowMs.load(), g_livePointers.load(), g_liveImports.load());
  Log("prefetch: %s (root %ls)", line, g_testModules.empty() ? g_root.c_str() : L"test modules");
  LogFileLine(line);
  return true;
}

// Restores every slot that still holds our value to the real kernel
// function, then waits (bounded) until no thread is inside the replacement.
// True when drained.
bool Uninstall(DWORD drainMs) {
  if (g_state.load() != 1) return InFlight() == 0;
  g_mode = kOff;  // pass-through first: in-flight and escaped callers stop skipping
  if (g_notifCookie) {
    auto unreg = reinterpret_cast<LdrUnregFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrUnregisterDllNotification"));
    if (unreg) unreg(g_notifCookie);
    g_notifCookie = nullptr;
  }
  void* const ours[2] = {reinterpret_cast<void*>(&Replacement), reinterpret_cast<void*>(&FixedGetProc)};
  uint32_t restored = 0;
  for (const Rec& r : g_recs) {
    HMODULE pinned = nullptr;
    if (!PinRecModule(r, &pinned)) continue;  // module gone: nothing to restore
    if (CasGuarded(r.slot, ours[r.import ? 1 : 0], r.restore, r.import) == 1) ++restored;
    FreeLibrary(pinned);
  }
  g_recs.clear();
  g_done.clear();
  g_foreign.clear();
  UpdateLiveCounts();
  g_state = 0;
  const ULONGLONG until = GetTickCount64() + drainMs;
  bool drained = InFlight() == 0;
  while (!drained && GetTickCount64() < until) {
    Sleep(1);
    drained = InFlight() == 0;
  }
  char line[160];
  snprintf(line, sizeof(line), "prefetch fix stopped: %u slot(s) restored%s", restored,
           drained ? "" : ", calls still in flight (the module stays pinned)");
  Log("prefetch: %s", line);
  LogFileLine(line);
  return drained;
}

// Live configuration (ini reload, kill switch). mode = effective mode.
void Apply(int mode, uint32_t windowMs) {
  g_windowMs = std::max<uint32_t>(1, std::min<uint32_t>(60000, windowMs));
  if (mode != kObserve && mode != kSkip) {
    if (g_state.load() == 1) Uninstall(2000);
    return;
  }
  if (g_state.load() == 0) {
    Install(mode);
    return;
  }
  if (g_state.load() == 1 && g_mode.exchange(mode) != mode)
    Log("prefetch: mode %s", mode == kSkip ? "skip" : "observe");
}

// Payload worker, about once per second.
void Tick() {
  if (g_state.load() != 1) return;
  const uint64_t now = GetTickCount64();
  const bool load = g_loadEvent.exchange(false);
  const bool periodic = now - g_installMs < 60000 || now - g_lastFullMs >= 10000;
  if (load || periodic) {
    if (periodic) g_lastFullMs = now;
    Scan();
  }
  Verify();
  if (now - g_lastLogMs >= 10000) {
    const uint64_t calls = SumCalls(), skipped = SumSkipped();
    char line[256];
    FormatStats(line, sizeof(line), calls - g_logCalls0, skipped - g_logSkipped0, (now - g_lastLogMs) / 1000.0);
    LogFileLine(line);
    g_logCalls0 = calls;
    g_logSkipped0 = skipped;
    g_lastLogMs = now;
  }
}

// Status word (status_word.h): installed, skipping, and holding slots.
bool Active() {
  return g_state.load() == 1 && g_mode.load() == kSkip && g_livePointers.load() + g_liveImports.load() > 0;
}

int ParseMode(const wchar_t* s) {
  if (_wcsicmp(s, L"skip") == 0) return kSkip;
  if (_wcsicmp(s, L"observe") == 0) return kObserve;
  return kOff;
}

}  // namespace pffix
