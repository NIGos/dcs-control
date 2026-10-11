// DCS Control prefetch fix.
//
// DCS's terrain workers (edterrain4 through edCore) call PrefetchVirtualMemory in a tight loop over the same
// ranges: about 840k calls/s, 1.7 CPU cores of kernel time in measurements. Prefetching is only a hint to the
// memory manager, so dropping a repeat changes no results: pages are faulted in on first access instead.
//
// Loaded by DCS itself as the next DXGI proxy (Cheeky's dxgi.dll loads bin\dxgi2.dll and falls back to System32
// for every export this DLL does not provide, which is all of them). No code is patched: edCore resolves the
// function with GetProcAddress, so the fix replaces stored function pointers in the game's own modules' writable
// data and their GetProcAddress import, never instructions. Modules outside the game's install folder (Windows,
// runtimes, overlays, antivirus hooks) are never touched.
//
// Settings, so the fix also works when DCS is started without DCS Control (Steam, a desktop shortcut):
//   DcsControlPrefetchFix.ini next to this DLL, section [PrefetchFix], written when the profile is applied:
//     Mode=off (default) | observe | skip,  WindowMs=repeat window (default 1000),  Log=statistics file (every 10 s)
//   The environment of a DCS Control launch overrides each of them when set:
//     DCSVR_PREFETCH_MODE, DCSVR_PREFETCH_WINDOW_MS, DCSVR_PREFETCH_LOG
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
using PrefetchFn = BOOL(WINAPI*)(HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG);
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);

enum class Mode { Off, Observe, Skip };
Mode g_mode = Mode::Off;
ULONGLONG g_window_ms = 1000;
constexpr unsigned kSpinSkips = 256;     // consecutive skips on one thread...
constexpr ULONGLONG kSpinWindowMs = 10;  // ...within this time mean the caller is spinning: yield one tick
wchar_t g_log[MAX_PATH]{};
wchar_t g_install_root[MAX_PATH]{};

PrefetchFn g_real{};
void* g_real_targets[2]{};  // KernelBase and kernel32 addresses DCS may hold
GetProcAddressFn g_real_get_proc{};

std::atomic<unsigned long long> g_calls{}, g_skipped{}, g_pointers{}, g_imports{}, g_resolved{}, g_faults{};
thread_local unsigned t_consecutive_skips{};
thread_local ULONGLONG t_spin_start{};

struct Key {
    ULONG_PTR address, count, bytes;
    bool operator==(const Key& o) const { return address == o.address && count == o.count && bytes == o.bytes; }
};
struct KeyHash {
    size_t operator()(const Key& k) const noexcept {
        return std::hash<ULONG_PTR>{}(k.address) ^ (std::hash<ULONG_PTR>{}(k.count) * 31) ^ (std::hash<ULONG_PTR>{}(k.bytes) * 131);
    }
};
// Measured at 10–30M calls/s across 1–16 threads, far above DCS's ~840k calls/s, so one lock is enough.
SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<Key, ULONGLONG, KeyHash>* g_seen{};

BOOL WINAPI fixed_prefetch(HANDLE process, ULONG_PTR count, PWIN32_MEMORY_RANGE_ENTRY entries, ULONG flags) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_mode == Mode::Skip && process == GetCurrentProcess() && entries && count) {
        ULONG_PTR bytes = 0;
        for (ULONG_PTR i = 0; i < count; ++i) bytes += entries[i].NumberOfBytes;
        const Key key{reinterpret_cast<ULONG_PTR>(entries[0].VirtualAddress), count, bytes};
        const ULONGLONG now = GetTickCount64();
        bool repeat = false;
        AcquireSRWLockExclusive(&g_lock);
        auto found = g_seen->find(key);
        if (found != g_seen->end() && now - found->second < g_window_ms) repeat = true;
        else {
            if (g_seen->size() > 65536) g_seen->clear();
            (*g_seen)[key] = now;
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (repeat) {
            g_skipped.fetch_add(1, std::memory_order_relaxed);
            // A caller spinning on these calls would turn a dropped prefetch into a user-mode spin: yield one scheduler
            // tick, but only when the skips come back to back (a frame-paced thread never reaches this).
            if (t_consecutive_skips++ == 0) t_spin_start = now;
            if (t_consecutive_skips >= kSpinSkips) {
                if (now - t_spin_start < kSpinWindowMs) Sleep(1);
                t_consecutive_skips = 0;
            }
            return TRUE;
        }
    }
    t_consecutive_skips = 0;
    return g_real(process, count, entries, flags);
}

FARPROC WINAPI fixed_get_proc(HMODULE module, LPCSTR name) {
    FARPROC result = g_real_get_proc(module, name);
    // Only the system function is replaced, never another module's export with the same name.
    if (result && (reinterpret_cast<void*>(result) == g_real_targets[0] || reinterpret_cast<void*>(result) == g_real_targets[1])) {
        g_resolved.fetch_add(1, std::memory_order_relaxed);
        return reinterpret_cast<FARPROC>(&fixed_prefetch);
    }
    return result;
}

bool is_game_module(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH || !g_install_root[0]) return false;
    return _wcsnicmp(path, g_install_root, wcslen(g_install_root)) == 0;
}

// Section flags are not the runtime protection: only committed read-write pages without a guard are used.
bool writable_data(const void* address) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD access = info.Protect & 0xFF;
    return access == PAGE_READWRITE || access == PAGE_WRITECOPY;
}

// Replaces stored copies of PrefetchVirtualMemory's address in a module's writable, non-executable, private sections.
void replace_pointers(HMODULE module) {
    auto base = reinterpret_cast<BYTE*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    auto section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        const DWORD c = section->Characteristics;
        if (!(c & IMAGE_SCN_MEM_WRITE) || (c & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_SHARED))) continue;
        auto begin = reinterpret_cast<void**>(base + section->VirtualAddress);
        const size_t slots = section->Misc.VirtualSize / sizeof(void*);
        ULONG_PTR checked_page = ~static_cast<ULONG_PTR>(0);
        bool page_ok = false;
        for (size_t s = 0; s < slots; ++s) {
            const ULONG_PTR page = reinterpret_cast<ULONG_PTR>(&begin[s]) & ~static_cast<ULONG_PTR>(0xFFF);
            if (page != checked_page) { checked_page = page; page_ok = writable_data(reinterpret_cast<const void*>(page)); }
            if (!page_ok) continue;
            void* value = begin[s];
            if (value && (value == g_real_targets[0] || value == g_real_targets[1])) {
                // Aligned pointer-sized stores are atomic on x64: a concurrent caller gets either function.
                InterlockedCompareExchangePointer(&begin[s], reinterpret_cast<void*>(&fixed_prefetch), value);
                g_pointers.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

// Redirects the module's GetProcAddress import so later lookups of PrefetchVirtualMemory get the fix.
void patch_get_proc_import(HMODULE module) {
    auto base = reinterpret_cast<BYTE*>(module);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    for (auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
        if (!desc->OriginalFirstThunk) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), "GetProcAddress") != 0) continue;
            // kernel32's, or KernelBase's when the module imports it through an API set.
            static const auto kernelbase = reinterpret_cast<ULONG_PTR>(GetProcAddress(GetModuleHandleW(L"KernelBase.dll"), "GetProcAddress"));
            if (slots->u1.Function != reinterpret_cast<ULONG_PTR>(g_real_get_proc) && (kernelbase == 0 || slots->u1.Function != kernelbase)) continue;
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(&slots->u1.Function, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD)) continue;
            // Code sharing the page stays executable while the slot is written.
            const bool executable = (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
            DWORD old{};
            if (VirtualProtect(&slots->u1.Function, sizeof(void*), executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old)) {
                InterlockedExchangePointer(reinterpret_cast<void**>(&slots->u1.Function), reinterpret_cast<void*>(&fixed_get_proc));
                VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
                g_imports.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

void log_line(const char* text) {
    if (!g_log[0]) return;
    const HANDLE file = CreateFileW(g_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char line[512]{};
    const int n = sprintf_s(line, "%04u-%02u-%02u %02u:%02u:%02u %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, text);
    DWORD written{};
    if (n > 0) WriteFile(file, line, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(file);
}

// A fault in another module's memory is contained instead of taking the game down.
bool scan_module_guarded(HMODULE module) {
    __try {
        replace_pointers(module);
        patch_get_proc_import(module);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The module list holds no references: take one for the scan so the module can't be unloaded under it.
void scan_module(HMODULE listed) {
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(listed), &pinned)) return;
    // Another copy of this fix must never wrap this one.
    if (pinned == listed && is_game_module(pinned) && !GetProcAddress(pinned, "DcsVrPrefetchFix_Stats") && !scan_module_guarded(pinned)) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        log_line("module scan fault contained");
    }
    FreeLibrary(pinned);
}

DWORD WINAPI worker(void*) {
    std::unordered_set<HMODULE> done;
    char text[512]{};
    sprintf_s(text, "prefetch fix started mode=%s window_ms=%llu", g_mode == Mode::Skip ? "skip" : "observe", g_window_ms);
    log_line(text);
    unsigned long long last_calls = 0, last_skipped = 0;
    for (unsigned tick = 0;; ++tick) {
        // Game modules keep loading during startup: scan every second for a minute, then every 10 s.
        if (tick < 60 || tick % 10 == 0) {
            std::vector<HMODULE> modules(4096);
            DWORD needed{};
            if (EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed)) {
                modules.resize(min(modules.size(), needed / sizeof(HMODULE)));
                for (HMODULE m : modules) {
                    if (done.count(m)) continue;
                    done.insert(m);
                    scan_module(m);
                }
            }
        }
        if (tick % 10 == 9) {
            const auto calls = g_calls.load(), skipped = g_skipped.load();
            const auto dc = calls - last_calls, ds = skipped - last_skipped;
            sprintf_s(text, "calls/s=%llu skipped=%.1f%% pointers=%llu imports=%llu lookups=%llu faults=%llu",
                dc / 10, dc ? 100.0 * static_cast<double>(ds) / static_cast<double>(dc) : 0.0,
                g_pointers.load(), g_imports.load(), g_resolved.load(), g_faults.load());
            log_line(text);
            last_calls = calls; last_skipped = skipped;
        }
        Sleep(1000);
    }
}

// One setting: the launch environment when it has it, otherwise DcsControlPrefetchFix.ini next to this DLL.
bool setting(const wchar_t* variable, const wchar_t* key, const wchar_t* ini, wchar_t* out, DWORD size) {
    const DWORD n = GetEnvironmentVariableW(variable, out, size);
    if (n > 0 && n < size) return true;
    if (!ini[0]) return false;
    const DWORD m = GetPrivateProfileStringW(L"PrefetchFix", key, L"", out, size, ini);
    return m > 0 && m < size - 1;
}

void configure() {
    wchar_t ini[MAX_PATH]{};
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&configure), &self)) {
        const DWORD length = GetModuleFileNameW(self, ini, ARRAYSIZE(ini));
        wchar_t* slash = length && length < ARRAYSIZE(ini) ? wcsrchr(ini, L'\\') : nullptr;
        if (slash && wcslen(ini) + 32 < ARRAYSIZE(ini)) wcscpy_s(slash + 1, ARRAYSIZE(ini) - (slash + 1 - ini), L"DcsControlPrefetchFix.ini");
        else ini[0] = 0;
        if (ini[0] && GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) ini[0] = 0;
    }
    wchar_t value[64]{};
    if (setting(L"DCSVR_PREFETCH_MODE", L"Mode", ini, value, ARRAYSIZE(value))) {
        if (_wcsicmp(value, L"skip") == 0) g_mode = Mode::Skip;
        else if (_wcsicmp(value, L"observe") == 0) g_mode = Mode::Observe;
    }
    if (setting(L"DCSVR_PREFETCH_WINDOW_MS", L"WindowMs", ini, value, ARRAYSIZE(value))) {
        const auto ms = wcstoull(value, nullptr, 10);
        if (ms >= 1 && ms <= 60000) g_window_ms = ms;
    }
    if (!setting(L"DCSVR_PREFETCH_LOG", L"Log", ini, g_log, ARRAYSIZE(g_log))) g_log[0] = 0;
    // Install folder: the parent of the executable's folder (DCSWorld\ for DCSWorld\bin\DCS.exe).
    const DWORD length = GetModuleFileNameW(nullptr, g_install_root, ARRAYSIZE(g_install_root));
    wchar_t* slash = length && length < ARRAYSIZE(g_install_root) ? wcsrchr(g_install_root, L'\\') : nullptr;
    if (slash) { *slash = 0; slash = wcsrchr(g_install_root, L'\\'); }
    if (slash) slash[1] = 0; else g_install_root[0] = 0;
}
}  // namespace

extern "C" __declspec(dllexport) unsigned long long DcsVrPrefetchFix_Stats(unsigned which) {
    switch (which) {
    case 0: return g_calls.load();
    case 1: return g_skipped.load();
    case 2: return g_pointers.load();
    case 3: return g_imports.load();
    case 4: return g_resolved.load();
    default: return g_faults.load();
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(instance);
    configure();
    if (g_mode == Mode::Off || !g_install_root[0]) { g_mode = Mode::Off; return TRUE; }
    const HMODULE kernelbase = GetModuleHandleW(L"KernelBase.dll"), kernel32 = GetModuleHandleW(L"kernel32.dll");
    g_real = kernelbase ? reinterpret_cast<PrefetchFn>(GetProcAddress(kernelbase, "PrefetchVirtualMemory")) : nullptr;
    g_real_get_proc = kernel32 ? reinterpret_cast<GetProcAddressFn>(GetProcAddress(kernel32, "GetProcAddress")) : nullptr;
    if (!g_real || !g_real_get_proc) { g_mode = Mode::Off; return TRUE; }
    g_real_targets[0] = reinterpret_cast<void*>(g_real);
    g_real_targets[1] = reinterpret_cast<void*>(GetProcAddress(kernel32, "PrefetchVirtualMemory"));
    // Redirected pointers and imports lead into this DLL, so it must never unload.
    HMODULE pinned{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&fixed_prefetch), &pinned);
    g_seen = new std::unordered_map<Key, ULONGLONG, KeyHash>();
    g_seen->reserve(8192);
    // Module scanning and logging never run under the loader lock.
    if (const HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr)) CloseHandle(thread);
    return TRUE;
}
