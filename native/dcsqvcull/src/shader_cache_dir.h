// Where the shader-variant cache (shadow_inst.h "Disk cache") lives, and how many compile workers fill it.
//
// The cache used to be <module dir>\cache\ only. DCS Control's Back to stock DCS removes the module folder and
// everything in it, so every restore (and the first start after it) paid the whole compile again: ~200 keys of two
// D3DCompile2 runs (~1.4 s each) on two idle-priority workers, 4.5 to 5 minutes in which shadow instancing, batching,
// the texture skip and the recorders are only partly active (render thread 16.4-16.9 ms instead of 12.1 ms, measured
// 2026-11-11). Now:
//   [Model] ShaderCacheDir       absolute folder, written by DCS Control (its own state folder, which Back to stock
//                                DCS does not touch). Empty, relative, too long or not creatable: <module dir>\cache\
//                                as before. Read from the deployed DcsQvCull.ini (like [Dev]), once at payload start.
//   [Model] ShaderCompileWorkers 0 (default) = by CPU: a quarter of the logical processors, 2 to 6. 1..8 = that many.
//   [Model] ShaderCachePrewarm   1 (default): entries a DCS or module update made stale are recompiled from payload
//                                start on (the main menu), not only when the mission first draws them. 0 = off.
// Stale entries are rejected by the cache itself (format, edit version, compiler, every source's hash), so a folder
// that outlives the module is safe: a DCS or module update just recompiles what changed.
// Pure: <windows.h> only, no logging (unit tested).
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace qvcachedir {

constexpr int kMaxWorkers = 8;
// Room for the entry name ("g_" + 16 hex + ".qvc." + thread id + ".tmp") below MAX_PATH: DCS.exe is not long-path aware.
constexpr size_t kMaxDirChars = MAX_PATH - 48;

// "C:\..." or "\\server\share\...".
inline bool Absolute(const std::wstring& p) {
  if (p.size() >= 3 && ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':' &&
      (p[2] == L'\\' || p[2] == L'/'))
    return true;
  return p.size() >= 5 && (p[0] == L'\\' || p[0] == L'/') && (p[1] == L'\\' || p[1] == L'/') && p[2] != L'\\' &&
         p[2] != L'/' && p[2] != L'?' && p[2] != L'.';
}

// Surrounding blanks and quotes off, '/' -> '\', one trailing backslash.
inline std::wstring Normalize(std::wstring p) {
  auto blank = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'"'; };
  while (!p.empty() && blank(p.back())) p.pop_back();
  size_t a = 0;
  while (a < p.size() && blank(p[a])) ++a;
  p.erase(0, a);
  for (wchar_t& c : p)
    if (c == L'/') c = L'\\';
  while (p.size() > 3 && p.back() == L'\\') p.pop_back();
  if (!p.empty() && p.back() != L'\\') p += L'\\';
  return p;
}

struct Choice {
  std::wstring dir;           // trailing backslash; empty = no cache (no module folder either)
  bool configured = false;    // dir is [Model] ShaderCacheDir
  const char* why = nullptr;  // why a non-empty ShaderCacheDir was not used
};

// moduleDir: the payload's folder with trailing backslash (may be empty). configured: the ini value as read.
inline Choice Resolve(const std::wstring& moduleDir, const std::wstring& configured) {
  Choice c;
  c.dir = moduleDir.empty() ? std::wstring() : moduleDir + L"cache\\";
  const std::wstring want = Normalize(configured);
  if (want.empty()) return c;
  if (!Absolute(want)) {
    c.why = "not an absolute path";
    return c;
  }
  if (want.size() > kMaxDirChars) {
    c.why = "path too long";
    return c;
  }
  if (want.find(L"..\\") != std::wstring::npos) {
    c.why = "path has a .. part";
    return c;
  }
  c.dir = want;
  c.configured = true;
  return c;
}

inline bool IsDir(const std::wstring& p) {
  const DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Creates dir (trailing backslash) and its missing parents. True when it is a folder afterwards.
inline bool CreateTree(const std::wstring& dir) {
  if (dir.empty()) return false;
  if (IsDir(dir)) return true;
  size_t start = 3;  // after "C:\"
  if (dir[0] == L'\\') {  // after "\\server\share\"
    start = dir.find(L'\\', 2);
    start = start == std::wstring::npos ? dir.size() : dir.find(L'\\', start + 1);
    start = start == std::wstring::npos ? dir.size() : start + 1;
  }
  for (size_t at = dir.find(L'\\', start); at != std::wstring::npos; at = dir.find(L'\\', at + 1))
    CreateDirectoryW(dir.substr(0, at).c_str(), nullptr);
  return IsDir(dir);
}

// [Model] ShaderCacheDir of the deployed ini, whole (a path cut at a buffer's end would name another folder).
inline std::wstring ReadConfigured(const std::wstring& ini) {
  std::vector<wchar_t> buf(32768);
  const DWORD n =
      GetPrivateProfileStringW(L"Model", L"ShaderCacheDir", L"", buf.data(), static_cast<DWORD>(buf.size()), ini.c_str());
  return std::wstring(buf.data(), n);
}

// Compile workers for `logical` processors: a quarter of them, 2 to 6. They compile at idle priority, so they only
// take cores DCS leaves free; the bound keeps them off most SMT siblings of DCS's own threads.
inline int AutoWorkers(unsigned logical) {
  const int n = static_cast<int>(logical / 4);
  return n < 2 ? 2 : n > 6 ? 6 : n;
}

// configured: [Model] ShaderCompileWorkers (0 or out of range = automatic).
inline int Workers(int configured, unsigned logical) {
  return configured >= 1 && configured <= kMaxWorkers ? configured : AutoWorkers(logical);
}

inline int ReadWorkers(const std::wstring& ini) {
  return static_cast<int>(GetPrivateProfileIntW(L"Model", L"ShaderCompileWorkers", 0, ini.c_str()));
}

// [Model] ShaderCachePrewarm (default 1): stale cache entries are recompiled from payload start on (shadow_inst.h).
inline bool ReadPrewarm(const std::wstring& ini) {
  return GetPrivateProfileIntW(L"Model", L"ShaderCachePrewarm", 1, ini.c_str()) != 0;
}

inline unsigned LogicalProcessors() {
  const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  return n ? n : 1;
}

}  // namespace qvcachedir
