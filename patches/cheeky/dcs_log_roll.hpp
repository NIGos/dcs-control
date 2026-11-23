// DCS VR Control: rotation of Cheeky's append-only logs (scripts/patch-cheeky.py).
// - The crash log (%TEMP%\CheekyFoveatedDLSS_crash.log) stopped taking lines at 8 MiB for good: a fault storm of one
//   session (first-chance HOOKDBG lines) left no room for a later real crash. The first write of each process rolls
//   a log above 1 MiB to CheekyFoveatedDLSS_crash.prev.log and starts with a header line (time, process id, exe).
// - The runtime logs (bin\CheekyFoveatedDLSS\CheekyFoveatedDLSS-Standalone.log, -Host.log) only appended between
//   applies: at runtime start one above 8 MiB is rolled to <name>.prev.log.
// Usable from an exception handler: no allocation, small stack buffers only.
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <iterator>

namespace cheeky::dcsvr {

constexpr unsigned long long dcs_crash_log_roll_bytes = 1ULL << 20;
constexpr unsigned long long dcs_runtime_log_roll_bytes = 8ULL << 20;

// "<dir>\name.log" -> "<dir>\name.prev.log" (a name without ".log" gets ".prev.log" appended). False when out is too small.
inline bool dcs_prev_log_path(const wchar_t* path, wchar_t* out, std::size_t capacity) noexcept {
    if (!path || !out || capacity == 0U) return false;
    const std::size_t length = std::wcslen(path);
    const bool log = length >= 4U && _wcsicmp(path + length - 4U, L".log") == 0;
    const std::size_t stem = log ? length - 4U : length;
    constexpr wchar_t suffix[] = L".prev.log";
    if (stem + std::size(suffix) > capacity) return false;
    std::wmemcpy(out, path, stem);
    std::wmemcpy(out + stem, suffix, std::size(suffix));
    return true;
}

// Moves the log to its .prev name (replacing an older one) when it holds at least `limit` bytes. True when moved.
inline bool dcs_roll_log_above(const wchar_t* path, unsigned long long limit) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!path || !*path || !GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return false;
    const unsigned long long size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    if (size < limit) return false;
    wchar_t prev[MAX_PATH + 16]{};
    if (!dcs_prev_log_path(path, prev, std::size(prev))) return false;
    return MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING) != 0;
}

// The crash log's per-process header line, "\r\n"-terminated: local time, process id, exe file name (UTF-8).
// Returns its length (0 when it does not fit).
inline std::size_t dcs_crash_log_header(char* out, std::size_t capacity, const SYSTEMTIME& time, DWORD pid,
    const wchar_t* exe_path) noexcept {
    if (!out || capacity == 0U) return 0U;
    char exe[128]{};
    if (exe_path && *exe_path) {
        const wchar_t* name = exe_path;
        for (const wchar_t* p = exe_path; *p; ++p)
            if (*p == L'\\' || *p == L'/') name = p + 1;
        if (WideCharToMultiByte(CP_UTF8, 0, name, -1, exe, static_cast<int>(sizeof(exe)), nullptr, nullptr) <= 0) exe[0] = '\0';
    }
    const int count = std::snprintf(out, capacity,
        "=== %04u-%02u-%02u %02u:%02u:%02u.%03u process %lu (%s) ===\r\n", time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, static_cast<unsigned long>(pid),
        exe[0] ? exe : "?");
    return count > 0 && static_cast<std::size_t>(count) < capacity ? static_cast<std::size_t>(count) : 0U;
}

}  // namespace cheeky::dcsvr
