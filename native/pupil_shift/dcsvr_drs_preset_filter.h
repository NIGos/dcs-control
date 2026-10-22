// dcsvr_drs_preset_filter.h - in-process, read-only filter that hides the NVIDIA driver profile's forced DLSS-SR
// render preset from DLSS (the R122 "[DLSS] IgnoreDriverPreset" logic of DcsQvCull, made self-contained).
//
// Self-contained: only Windows headers and <atomic>. No NvAPI SDK headers or libs are needed; the few ids it uses are
// defined below with their source in the public NVAPI SDK (R580, MIT licence: nvapi_interface.h,
// NvApiDriverSettings.h, nvapi_lite_common.h). x64 only.
//
// ---- What it does --------------------------------------------------------------------------------------------------
// DLSS (the "snippet": nvngx_dlss.dll, or the driver's OTA copy under ...\NGX\models\...) reads the DRS setting
// NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION (0x10E41DF3) once, inside NVSDK_NGX_D3D11_Init, through NvAPI:
// LoadLibrary("nvapi64.dll") -> GetProcAddress("nvapi_QueryInterface") -> QueryInterface(0x73BF8338) =
// NvAPI_DRS_GetSetting -> GetSetting(session, profile, 0x10E41DF3, &setting). A value other than off/default (e.g. the
// NVIDIA App's "DLSS Override - Model presets = M") then wins over the application's per-feature preset hint.
//
// While the gate is ON and the caller is a DLSS module, a GetSetting read of a FILTERED id returns
// NVAPI_SETTING_NOT_FOUND (-160): exactly what a profile without that setting returns, so DLSS falls back to the app's
// hint. Every other call passes through untouched: other setting ids, other callers, every other NvAPI function.
//
// Filtered setting ids (kFilteredSettingIds, NvApiDriverSettings.h, enum ESetting):
//   0x10E41DF3  NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION_ID  ("Override DLSS-SR presets")
// Deliberately NOT filtered (all pass through): 0x10E41E01 NGX_DLSS_SR_OVERRIDE_ID (the "latest DLL" swap, read by the
// NGX core), 0x10AFB768 NGX_DLSS_SR_MODE_ID, 0x10E41DF7 NGX_DLSS_RR_OVERRIDE_RENDER_PRESET_SELECTION_ID,
// 0x10E41DF1 NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_ID, 0x10E41DF8 NGX_DLSS_NR_OVERRIDE_RENDER_PRESET_SELECTION_ID.
//
// ---- Hook mechanism --------------------------------------------------------------------------------------------------
// No code is patched. Install() redirects ONE entry of nvapi64.dll's EXPORT ADDRESS TABLE: the RVA of
// "nvapi_QueryInterface" is pointed at a 14-byte thunk (jmp [rip+0]; dq QiHook) allocated in the 4 GB above
// nvapi64.dll (an EAT entry is a 32-bit RVA). The write is one aligned 32-bit InterlockedExchange (atomic for any
// concurrent GetProcAddress). Effects:
//   - every GetProcAddress(nvapi64, "nvapi_QueryInterface") made AFTER Install(), and every static import of it bound
//     after Install(), gets QiHook. Pointers resolved before Install() keep the original (so: install BEFORE NGX
//     initialises; DCS's NGX core reads only 0x10E41E01 and is irrelevant).
//   - QiHook calls the real nvapi_QueryInterface for every id and returns its result unchanged, except for
//     0x73BF8338 (NvAPI_DRS_GetSetting), for which it returns GetSettingHook (the real pointer is kept and called).
//   - GetSettingHook: id not filtered -> real GetSetting (one compare). Filtered id -> gate + caller check (SEH-guarded)
//     -> NOT_FOUND or the real GetSetting.
// The filter itself never calls nvapi_QueryInterface for its own purposes, never calls any NvAPI_DRS_Set*/Save*/
// Create*/Delete*/Restore* function, and never opens a DRS session: the driver profile cannot be modified by it.
// kHookTable lists the wrapped id and the write ids that are counted (passed through, never called by the filter).
//
// ---- Lifetime: never unhooks -------------------------------------------------------------------------------------------
// There is no Uninstall. Once installed, the EAT entry, the thunk page and the hook stay for the life of the process;
// "off" = the gate says no = pure pass-through. To make that safe Install() pins nvapi64.dll and the module that
// contains this header (GET_MODULE_HANDLE_EX_FLAG_PIN): the host DLL can no longer be unloaded (FreeLibrary becomes a
// no-op for the last reference). There is therefore no unhook race: no pointer handed out is ever invalidated.
// Install() is one-time per host module (InitOnce); concurrent callers block until the first finishes and all get the
// same result. If another module already installed this filter (the thunk carries a "DCSVRDRS" marker), Install()
// returns kOwnedByOtherModule and does nothing: one filter per process. A foreign EAT redirection (not ours) is
// chained: our QiHook calls whatever the entry pointed to.
//
// ---- Gating ----------------------------------------------------------------------------------------------------------
// Config (Install(cfg), SetConfig(cfg) at any time; the latest wins; old configs are intentionally leaked, ~2 KB each,
// so a reader on another thread never sees freed memory):
//   gate / gateCtx   host predicate, called on the reading thread for a filtered id only (once or twice per process
//                    in practice: inside NGX init). Must be cheap and must not call NvAPI. Takes precedence over ini.
//   iniPath          else: the gate is ON when [DLSS] PeripheryPreset is a usable preset (A-F, J-M or NGX numbers
//                    1-6, 10-13; "0"/empty/anything else = off) AND [DLSS] IgnoreDriverPreset is a nonzero integer
//                    (1). Read with GetPrivateProfile*W at the time of the read, so the file's current state counts.
//                    Same parsing as DcsQvCull's config_load.inc / dlss_views.h ParsePreset.
//   neither          gate OFF.
//   requireDlssCaller (default true) the caller of GetSetting must be a DLSS module: its file base name starts with
//                    "nvngx_dlss" or its path contains "\NGX\models\" (case-insensitive). Others always pass through.
//   publishHiddenEnv (default true) on a hide, set the process environment variable DCSQV_DLSS_DRIVER_PRESET_HIDDEN=1
//                    (DcsQvCull's CreateFeature hook reads it to make the driver's preset explicit on the focus views).
// A fault (access violation, C++ exception) inside the predicate / ini read / caller lookup is caught (SEH):
// the read passes through unchanged and Stats.gateFaults counts it.
//
// ---- Usage -----------------------------------------------------------------------------------------------------------
//   #include "dcsvr_drs_preset_filter.h"
//   dcsvr_drs::Config cfg; cfg.iniPath = L"...\\DcsQvCull.ini";        // or cfg.gate = &MyGate; cfg.gateCtx = ...
//   const dcsvr_drs::InstallResult r = dcsvr_drs::Install(cfg);      // NOT from DllMain (it calls LoadLibrary)
//   if (r.status != dcsvr_drs::kInstalled) log(dcsvr_drs::StatusText(r.status));
//   if (r.dlssLoadedAtInstall) log("too late: DLSS already read the profile in this process");
// Compile as C++17 or later, x64, MSVC (uses __try/__except and _ReturnAddress).
//
// ---- DCS Control copy (native/pupil_shift, from the DcsQvCull peer's R122b) ------------------------------------------
// Differences from the peer's file, from DCS Control's review:
//   - The thunk page must become executable before anything can be handed out: if VirtualProtect(PAGE_EXECUTE_READ)
//     fails (a process with a dynamic-code policy), the page is freed and Install() returns kNoThunkMemory. The peer's
//     copy ignored the result and would have pointed the export at non-executable memory.
//   - The export table entry is made writable without dropping execute: PAGE_EXECUTE_READWRITE when its page is
//     executable (an export directory merged into a code section), PAGE_READWRITE otherwise; the old protection is
//     restored afterwards as before.
//   - The entry must be 4-byte aligned for the one InterlockedExchange to be atomic; otherwise kNoExport.
// Everything else (the filtered id, the gate, the caller check, pinning, never unhooking) is the peer's.
#pragma once
#if !defined(_M_X64)
#error "dcsvr_drs_preset_filter.h: x64 only (the thunk is x64 code)"
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#include <psapi.h>  // K32EnumProcessModules / K32GetModuleFileNameExW (kernel32, no psapi.lib needed)
#include <stdint.h>
#include <string.h>
#include <atomic>

#pragma intrinsic(_ReturnAddress)

namespace dcsvr_drs {

// ---- NVAPI constants (public NVAPI SDK R580, MIT) ---------------------------------------------------------------------
// NvApiDriverSettings.h, enum ESetting:
constexpr uint32_t kSettingDlssSrPreset = 0x10E41DF3;   // NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION_ID
constexpr uint32_t kFilteredSettingIds[] = {kSettingDlssSrPreset};
// nvapi_lite_common.h, enum _NvAPI_Status:
constexpr int32_t kNvapiOk = 0;                // NVAPI_OK
constexpr int32_t kNvapiSettingNotFound = -160;  // NVAPI_SETTING_NOT_FOUND
// nvapi_interface.h (nvapi_QueryInterface ids):
constexpr uint32_t kQiDrsGetSetting = 0x73bf8338;  // NvAPI_DRS_GetSetting
// "nvapi_QueryInterface" is the single export of nvapi64.dll through which the SDK's nvapi64.lib (and DLSS) obtain
// every NvAPI function by the ids of nvapi_interface.h.
constexpr const char* kQiExportName = "nvapi_QueryInterface";

using QueryFn = void*(__cdecl*)(uint32_t id);
using GetSettingFn = int(__cdecl*)(void* session, void* profile, uint32_t settingId, void* setting);
using GatePredicate = bool (*)(void* ctx);

// ---- hook table ------------------------------------------------------------------------------------------------------
// Every nvapi_QueryInterface id this filter treats specially. Wrapped: QiHook returns GetSettingHook. Write: passed
// through unchanged (the filter never calls them), but counted, so a test can assert nobody in the process obtained
// a DRS write function. Any id not listed: passed through unchanged, not counted. Ids from nvapi_interface.h.
enum Treatment : uint8_t { kWrapped = 1, kWritePassThrough = 2 };
struct HookEntry {
  uint32_t qiId;
  const char* name;
  Treatment treatment;
};
inline constexpr HookEntry kHookTable[] = {
    {0x73bf8338, "NvAPI_DRS_GetSetting", kWrapped},
    {0x577dd202, "NvAPI_DRS_SetSetting", kWritePassThrough},
    {0xfcbc7e14, "NvAPI_DRS_SaveSettings", kWritePassThrough},
    {0x2be25df8, "NvAPI_DRS_SaveSettingsToFile", kWritePassThrough},
    {0xcc176068, "NvAPI_DRS_CreateProfile", kWritePassThrough},
    {0x17093206, "NvAPI_DRS_DeleteProfile", kWritePassThrough},
    {0x1c89c5df, "NvAPI_DRS_SetCurrentGlobalProfile", kWritePassThrough},
    {0x16abd3a9, "NvAPI_DRS_SetProfileInfo", kWritePassThrough},
    {0x4347a9de, "NvAPI_DRS_CreateApplication", kWritePassThrough},
    {0xc5ea85a1, "NvAPI_DRS_DeleteApplicationEx", kWritePassThrough},
    {0x2c694bc6, "NvAPI_DRS_DeleteApplication", kWritePassThrough},
    {0xe4a26362, "NvAPI_DRS_DeleteProfileSetting", kWritePassThrough},
    {0x5927b094, "NvAPI_DRS_RestoreAllDefaults", kWritePassThrough},
    {0xfa5f6134, "NvAPI_DRS_RestoreProfileDefault", kWritePassThrough},
    {0x53f0381e, "NvAPI_DRS_RestoreProfileDefaultSetting", kWritePassThrough},
};
constexpr size_t kHookTableSize = sizeof(kHookTable) / sizeof(kHookTable[0]);

// ---- public types ----------------------------------------------------------------------------------------------------
struct Config {
  GatePredicate gate = nullptr;
  void* gateCtx = nullptr;
  const wchar_t* iniPath = nullptr;  // copied (up to 1023 characters)
  bool requireDlssCaller = true;
  bool publishHiddenEnv = true;
};
enum Status : int {
  kInstalled = 1,             // this call (or an earlier one in this module) installed it
  kOwnedByOtherModule = 2,    // another module in this process already installed this filter; nothing done
  kNoNvapi = -1,              // nvapi64.dll not loadable (no NVIDIA driver)
  kNoExport = -2,             // no nvapi_QueryInterface export / unreadable export directory / forwarded export
  kNoThunkMemory = -3,        // no free page within 4 GB above nvapi64.dll
  kPatchFailed = -4,          // VirtualProtect of the export table failed
  kFault = -5,                // an exception while parsing nvapi64.dll's headers
};
struct InstallResult {
  Status status = kNoNvapi;
  bool ngxCoreLoadedAtInstall = false;  // _nvngx.dll was already loaded (NGX may already be initialised)
  bool dlssLoadedAtInstall = false;     // a DLSS module was already loaded: it already read the profile (too late)
  void* originalQueryInterface = nullptr;  // what the EAT entry pointed to before
  void* thunk = nullptr;
};
struct Stats {
  uint32_t qiCalls, getSettingCalls, filteredIdReads, hiddenReads, gateFaults, getSettingMismatch;
  uint32_t tableHits[kHookTableSize];  // per kHookTable entry: QueryInterface requests seen through the hook
};

inline const char* StatusText(Status s) {
  switch (s) {
    case kInstalled: return "installed";
    case kOwnedByOtherModule: return "already installed by another module in this process";
    case kNoNvapi: return "nvapi64.dll not available";
    case kNoExport: return "nvapi_QueryInterface export not found or not patchable";
    case kNoThunkMemory: return "no memory for the thunk near nvapi64.dll";
    case kPatchFailed: return "the export table could not be made writable";
    case kFault: return "fault while reading nvapi64.dll";
  }
  return "?";
}

// ---- pure helpers (also used by tests) --------------------------------------------------------------------------------
inline bool IsFilteredSettingId(uint32_t id) {
  for (uint32_t f : kFilteredSettingIds)
    if (f == id) return true;
  return false;
}
// DLSS module: base name "nvngx_dlss*" (DCS's copy, nvngx_dlssg too) or a file under an NGX models folder (the
// driver's OTA copy, "...\NGX\models\dlss\versions\<n>\files\<x>.bin"). Case-insensitive, '/' == '\'.
inline bool IsDlssModulePath(const wchar_t* path) {
  if (!path || !*path) return false;
  wchar_t lo[1024];
  size_t n = 0;
  for (; path[n] && n < 1023; ++n) {
    wchar_t c = path[n];
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    if (c == L'/') c = L'\\';
    lo[n] = c;
  }
  if (path[n]) return false;  // longer than we handle: not ours
  lo[n] = 0;
  const wchar_t* base = lo;
  for (size_t i = 0; i < n; ++i)
    if (lo[i] == L'\\') base = lo + i + 1;
  return wcsncmp(base, L"nvngx_dlss", 10) == 0 || wcsstr(lo, L"\\ngx\\models\\") != nullptr;
}
// [DLSS] PeripheryPreset value: >0 a usable preset, 0 off (empty / "0"), -1 not usable (treated as off).
inline bool PresetAllowed(int v) { return (v >= 1 && v <= 6) || (v >= 10 && v <= 13); }
inline int ParsePreset(const wchar_t* s) {
  if (!s) return 0;
  while (*s == L' ' || *s == L'\t') ++s;
  size_t n = wcslen(s);
  while (n && (s[n - 1] == L' ' || s[n - 1] == L'\t' || s[n - 1] == L'\r' || s[n - 1] == L'\n')) --n;
  if (n == 0) return 0;
  if (n == 1 && ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z'))) {
    const int v = (s[0] >= L'a' ? s[0] - L'a' : s[0] - L'A') + 1;
    return PresetAllowed(v) ? v : -1;
  }
  int v = 0;
  for (size_t i = 0; i < n; ++i) {
    if (s[i] < L'0' || s[i] > L'9' || i >= 3) return -1;
    v = v * 10 + (s[i] - L'0');
  }
  if (v == 0) return 0;
  return PresetAllowed(v) ? v : -1;
}
inline bool IniGateOn(const wchar_t* ini) {
  if (!ini || !*ini) return false;
  wchar_t v[32] = {};
  GetPrivateProfileStringW(L"DLSS", L"PeripheryPreset", L"0", v, 32, ini);
  if (ParsePreset(v) <= 0) return false;
  return GetPrivateProfileIntW(L"DLSS", L"IgnoreDriverPreset", 0, ini) != 0;
}

// ---- internals -------------------------------------------------------------------------------------------------------
namespace detail {
struct GateBlock {
  GatePredicate gate;
  void* ctx;
  bool requireDlssCaller;
  bool publishEnv;
  wchar_t ini[1024];
};
#pragma pack(push, 1)
struct Thunk {
  uint8_t jmp[6];  // FF 25 00 00 00 00 = jmp qword ptr [rip+0]
  void* target;    // QiHook
  char magic[8];   // "DCSVRDRS"
  uint32_t version;
};
#pragma pack(pop)
constexpr char kMagic[8] = {'D', 'C', 'S', 'V', 'R', 'D', 'R', 'S'};
constexpr uint32_t kThunkVersion = 1;
constexpr const wchar_t* kHiddenEnv = L"DCSQV_DLSS_DRIVER_PRESET_HIDDEN";

inline std::atomic<QueryFn> g_realQi{nullptr};
inline std::atomic<GetSettingFn> g_realGetSetting{nullptr};
inline std::atomic<const GateBlock*> g_gate{nullptr};
inline std::atomic<uint32_t> g_qiCalls{0}, g_getSettingCalls{0}, g_filteredReads{0}, g_hiddenReads{0},
    g_gateFaults{0}, g_getSettingMismatch{0};
inline std::atomic<uint32_t> g_tableHits[kHookTableSize];
inline INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
inline InstallResult g_result;

inline const GateBlock* MakeGate(const Config& c) {
  auto* g = static_cast<GateBlock*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(GateBlock)));
  if (!g) return nullptr;
  g->gate = c.gate;
  g->ctx = c.gateCtx;
  g->requireDlssCaller = c.requireDlssCaller;
  g->publishEnv = c.publishHiddenEnv;
  if (c.iniPath) {
    size_t i = 0;
    for (; c.iniPath[i] && i < 1023; ++i) g->ini[i] = c.iniPath[i];
    g->ini[i] = 0;
  }
  return g;
}

inline bool CallerIsDlss(void* ret) {
  HMODULE m = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(ret), &m) ||
      !m)
    return false;
  wchar_t p[1024];
  const DWORD n = GetModuleFileNameW(m, p, 1024);
  return n > 0 && n < 1024 && IsDlssModulePath(p);
}

// SEH-guarded decision. No C++ objects with destructors in this function.
__declspec(noinline) inline bool DecideHide(void* ret) {
  const GateBlock* g = g_gate.load(std::memory_order_acquire);
  if (!g) return false;
  __try {
    const bool on = g->gate ? g->gate(g->ctx) : IniGateOn(g->ini);
    if (!on) return false;
    if (g->requireDlssCaller && !CallerIsDlss(ret)) return false;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_gateFaults.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
}

__declspec(noinline) inline int __cdecl GetSettingHook(void* session, void* profile, uint32_t id, void* setting) {
  g_getSettingCalls.fetch_add(1, std::memory_order_relaxed);
  if (IsFilteredSettingId(id)) {
    g_filteredReads.fetch_add(1, std::memory_order_relaxed);
    if (DecideHide(_ReturnAddress())) {
      g_hiddenReads.fetch_add(1, std::memory_order_relaxed);
      const GateBlock* g = g_gate.load(std::memory_order_acquire);
      if (g && g->publishEnv) SetEnvironmentVariableW(kHiddenEnv, L"1");
      return kNvapiSettingNotFound;
    }
  }
  return g_realGetSetting.load(std::memory_order_acquire)(session, profile, id, setting);
}

inline void* __cdecl QiHook(uint32_t id) {
  g_qiCalls.fetch_add(1, std::memory_order_relaxed);
  for (size_t i = 0; i < kHookTableSize; ++i)
    if (kHookTable[i].qiId == id) g_tableHits[i].fetch_add(1, std::memory_order_relaxed);
  void* p = g_realQi.load(std::memory_order_acquire)(id);
  if (id == kQiDrsGetSetting && p) {
    GetSettingFn expected = nullptr;
    const GetSettingFn real = reinterpret_cast<GetSettingFn>(p);
    if (g_realGetSetting.compare_exchange_strong(expected, real, std::memory_order_acq_rel) || expected == real)
      return reinterpret_cast<void*>(&GetSettingHook);
    g_getSettingMismatch.fetch_add(1, std::memory_order_relaxed);  // NvAPI returned another pointer: hand it out as is
  }
  return p;
}

// The EAT slot of nvapi_QueryInterface, SEH-guarded. 0 ok, else a Status.
inline int FindQiSlot(uint8_t* base, DWORD** slotOut, size_t* imageSizeOut) {
  __try {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return kNoExport;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
      return kNoExport;
    const IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return kNoExport;
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    const auto* names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
    const auto* ords = reinterpret_cast<const WORD*>(base + exp->AddressOfNameOrdinals);
    auto* funcs = reinterpret_cast<DWORD*>(base + exp->AddressOfFunctions);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
      if (strcmp(reinterpret_cast<const char*>(base + names[i]), kQiExportName) != 0) continue;
      if (ords[i] >= exp->NumberOfFunctions) return kNoExport;
      DWORD* slot = &funcs[ords[i]];
      if (reinterpret_cast<uintptr_t>(slot) & 3) return kNoExport;  // one aligned 32-bit exchange, or nothing
      const DWORD rva = *slot;
      if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) return kNoExport;  // forwarder
      *slotOut = slot;
      *imageSizeOut = nt->OptionalHeader.SizeOfImage;
      return 0;
    }
    return kNoExport;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return kFault;
  }
}

// True when p is a thunk of this filter (any module's copy).
inline bool IsOurThunk(const void* p) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
  if (!(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_READONLY | PAGE_READWRITE))) return false;
  if (static_cast<const uint8_t*>(p) + sizeof(Thunk) >
      static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize)
    return false;
  __try {
    const auto* t = static_cast<const Thunk*>(p);
    return t->jmp[0] == 0xFF && t->jmp[1] == 0x25 && memcmp(t->magic, kMagic, 8) == 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// One page within [base + imageSize, base + 4 GB): reachable by a 32-bit RVA.
inline void* AllocAbove(uint8_t* base, size_t imageSize) {
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  const uintptr_t gran = si.dwAllocationGranularity;
  const uintptr_t hi = reinterpret_cast<uintptr_t>(base) + 0xFFFF0000ull;
  uintptr_t a = (reinterpret_cast<uintptr_t>(base) + imageSize + gran - 1) & ~(gran - 1);
  while (a < hi) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) break;
    const uintptr_t rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress), re = rb + mbi.RegionSize;
    if (mbi.State == MEM_FREE) {
      const uintptr_t start = (rb + gran - 1) & ~(gran - 1);
      if (start + si.dwPageSize <= re && start + si.dwPageSize <= hi) {
        void* p = VirtualAlloc(reinterpret_cast<void*>(start), si.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p) return p;
      }
    }
    a = re > a ? re : a + gran;
  }
  return nullptr;
}

inline void ScanLoaded(InstallResult* r) {
  r->ngxCoreLoadedAtInstall = GetModuleHandleW(L"_nvngx.dll") != nullptr;
  HMODULE mods[1024];
  DWORD need = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
  const DWORD n = need / sizeof(HMODULE) < 1024 ? need / sizeof(HMODULE) : 1024;
  for (DWORD i = 0; i < n; ++i) {
    wchar_t p[1024];
    const DWORD len = GetModuleFileNameW(mods[i], p, 1024);
    if (len > 0 && len < 1024 && IsDlssModulePath(p)) r->dlssLoadedAtInstall = true;
  }
}

inline InstallResult DoInstall(const Config& cfg) {
  InstallResult r;
  ScanLoaded(&r);
  g_gate.store(MakeGate(cfg), std::memory_order_release);
  HMODULE nv = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!nv) {
    r.status = kNoNvapi;
    return r;
  }
  HMODULE pinned = nullptr;  // nvapi64.dll stays: the EAT edit must never be lost to an unload/reload
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                     reinterpret_cast<LPCWSTR>(nv), &pinned);
  auto* base = reinterpret_cast<uint8_t*>(nv);
  DWORD* slot = nullptr;
  size_t imageSize = 0;
  const int f = FindQiSlot(base, &slot, &imageSize);
  if (f) {
    r.status = static_cast<Status>(f);
    return r;
  }
  void* current = base + *slot;
  r.originalQueryInterface = current;
  if (IsOurThunk(current)) {  // another module's copy of this filter owns it
    r.status = kOwnedByOtherModule;
    return r;
  }
  auto* t = static_cast<Thunk*>(AllocAbove(base, imageSize));
  if (!t) {
    r.status = kNoThunkMemory;
    return r;
  }
  t->jmp[0] = 0xFF, t->jmp[1] = 0x25, t->jmp[2] = t->jmp[3] = t->jmp[4] = t->jmp[5] = 0;
  t->target = reinterpret_cast<void*>(&QiHook);
  memcpy(t->magic, kMagic, 8);
  t->version = kThunkVersion;
  DWORD old = 0;
  if (!VirtualProtect(t, sizeof(Thunk), PAGE_EXECUTE_READ, &old)) {  // never hand out a non-executable entry
    VirtualFree(t, 0, MEM_RELEASE);
    r.status = kNoThunkMemory;
    return r;
  }
  FlushInstructionCache(GetCurrentProcess(), t, sizeof(Thunk));
  // Pin this module: QiHook / GetSettingHook live here and are handed out for the life of the process.
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                     reinterpret_cast<LPCWSTR>(&QiHook), &self);
  g_realQi.store(reinterpret_cast<QueryFn>(current), std::memory_order_release);  // before the slot is published
  // Writable without losing execute: an export directory can share a page with code another thread is running.
  MEMORY_BASIC_INFORMATION slotInfo{};
  const bool slotExec = VirtualQuery(slot, &slotInfo, sizeof(slotInfo)) &&
                        (slotInfo.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                             PAGE_EXECUTE_WRITECOPY)) != 0;
  if (!VirtualProtect(slot, sizeof(DWORD), slotExec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old)) {
    r.status = kPatchFailed;  // the thunk page stays allocated (unused)
    return r;
  }
  InterlockedExchange(reinterpret_cast<volatile LONG*>(slot),
                      static_cast<LONG>(reinterpret_cast<uint8_t*>(t) - base));
  DWORD tmp = 0;
  VirtualProtect(slot, sizeof(DWORD), old, &tmp);
  r.thunk = t;
  r.status = kInstalled;
  return r;
}

inline BOOL CALLBACK OnceCb(PINIT_ONCE, PVOID param, PVOID*) {
  g_result = DoInstall(*static_cast<const Config*>(param));  // the winning caller's cfg
  return TRUE;
}
}  // namespace detail

// ---- public API ------------------------------------------------------------------------------------------------------
// One-time install (thread-safe; later calls return the first result and ignore cfg - use SetConfig to change it).
// Call before the process initialises NGX, and not from DllMain.
inline InstallResult Install(const Config& cfg) {
  InitOnceExecuteOnce(&detail::g_once, &detail::OnceCb, const_cast<Config*>(&cfg), nullptr);
  return detail::g_result;
}
// Replaces the gate (any time, any thread). Ignored for kOwnedByOtherModule (that module's gate rules).
inline void SetConfig(const Config& cfg) {
  const detail::GateBlock* g = detail::MakeGate(cfg);
  if (g) detail::g_gate.store(g, std::memory_order_release);  // the previous block is leaked on purpose
}
inline Stats GetStats() {
  Stats s{};
  s.qiCalls = detail::g_qiCalls.load();
  s.getSettingCalls = detail::g_getSettingCalls.load();
  s.filteredIdReads = detail::g_filteredReads.load();
  s.hiddenReads = detail::g_hiddenReads.load();
  s.gateFaults = detail::g_gateFaults.load();
  s.getSettingMismatch = detail::g_getSettingMismatch.load();
  for (size_t i = 0; i < kHookTableSize; ++i) s.tableHits[i] = detail::g_tableHits[i].load();
  return s;
}
// For tests / diagnostics: the address QiHook hands out for NvAPI_DRS_GetSetting.
inline void* GetSettingHookAddress() { return reinterpret_cast<void*>(&detail::GetSettingHook); }

}  // namespace dcsvr_drs
