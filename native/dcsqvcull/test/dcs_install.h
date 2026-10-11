// Where DCS World is installed, for the tests that read its binaries and
// shader sources. No machine-specific path: the release build runs these
// tests on whatever PC builds it.
//   1. DCS_BIN (the bin folder) or DCS_ROOT (the install folder). When set,
//      it is the only place looked at: DCS_ROOT=none (or any path without
//      DCS) makes every DCS-dependent check SKIP, to test that path.
//   2. The Steam libraries: HKCU\Software\Valve\Steam SteamPath, every "path"
//      in steamapps\libraryfolders.vdf, then steamapps\common\DCSWorld.
//   3. The standalone install: HKCU\Software\Eagle Dynamics\DCS World (and
//      the openbeta key), value "Path".
// A folder counts when bin\NGModel.dll exists. Not found: the dependent
// checks print "SKIP <name>: DCS install not found" and count as neither
// pass nor fail.
// Included from test_main.cpp before the test headers.
#pragma once

#include <string>
#include <vector>
#pragma comment(lib, "advapi32.lib")  // RegGetValueW

namespace dcsinstall {

inline std::wstring WithSlash(std::wstring p) {
  for (wchar_t& c : p)
    if (c == L'/') c = L'\\';
  if (!p.empty() && p.back() != L'\\') p += L'\\';
  return p;
}

inline bool IsInstall(const std::wstring& root) {
  return !root.empty() && GetFileAttributesW((root + L"bin\\NGModel.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline std::wstring EnvVar(const wchar_t* name) {
  wchar_t b[MAX_PATH * 2] = {};
  const DWORD n = GetEnvironmentVariableW(name, b, static_cast<DWORD>(sizeof(b) / sizeof(b[0])));
  return n && n < sizeof(b) / sizeof(b[0]) ? std::wstring(b) : std::wstring();
}

inline std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value) {
  wchar_t b[MAX_PATH * 2] = {};
  DWORD size = sizeof(b) - sizeof(wchar_t);
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, b, &size) != ERROR_SUCCESS)
    return std::wstring();
  return b;
}

// The Steam folder and the "path" values of its libraryfolders.vdf
// (backslashes escaped as \\ there).
inline std::vector<std::wstring> SteamLibraries(const std::wstring& steam) {
  std::vector<std::wstring> out;
  out.push_back(steam);
  HANDLE h = CreateFileW((steam + L"steamapps\\libraryfolders.vdf").c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return out;
  std::string text;
  char buf[4096];
  DWORD got = 0;
  while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) text.append(buf, got);
  CloseHandle(h);
  for (size_t at = text.find("\"path\""); at != std::string::npos; at = text.find("\"path\"", at + 6)) {
    const size_t open = text.find('"', at + 6);
    if (open == std::string::npos) break;
    std::string v;
    for (size_t i = open + 1; i < text.size() && text[i] != '"' && text[i] != '\n'; ++i) {
      if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '\\') ++i;
      v += text[i];
    }
    if (v.empty()) continue;
    const int n = MultiByteToWideChar(CP_UTF8, 0, v.c_str(), static_cast<int>(v.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, v.c_str(), static_cast<int>(v.size()), &w[0], n);
    out.push_back(WithSlash(w));
  }
  return out;
}

inline std::wstring Find() {
  const std::wstring bin = EnvVar(L"DCS_BIN");
  if (!bin.empty()) {
    const std::wstring root = WithSlash(bin) + L"..\\";
    return IsInstall(root) ? root : std::wstring();
  }
  const std::wstring envRoot = EnvVar(L"DCS_ROOT");
  if (!envRoot.empty()) return IsInstall(WithSlash(envRoot)) ? WithSlash(envRoot) : std::wstring();
  const std::wstring steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
  if (!steam.empty())
    for (const std::wstring& lib : SteamLibraries(WithSlash(steam))) {
      const std::wstring root = lib + L"steamapps\\common\\DCSWorld\\";
      if (IsInstall(root)) return root;
    }
  for (const wchar_t* key : {L"Software\\Eagle Dynamics\\DCS World", L"Software\\Eagle Dynamics\\DCS World OpenBeta"}) {
    const std::wstring p = RegString(HKEY_CURRENT_USER, key, L"Path");
    if (IsInstall(WithSlash(p))) return WithSlash(p);
  }
  return std::wstring();
}

// The install folder with a trailing backslash, empty when not found.
inline const std::wstring& Root() {
  static const std::wstring root = [] {
    const std::wstring r = Find();
    if (r.empty())
      printf("     DCS install: not found (DCS_BIN / DCS_ROOT, Steam libraries, Eagle Dynamics registry)\n");
    else
      printf("     DCS install: %ls\n", r.c_str());
    return r;
  }();
  return root;
}
inline bool Found() { return !Root().empty(); }
// bin\ and Bazar\shaders\ of the install (empty when not found).
inline std::wstring Bin() { return Found() ? Root() + L"bin\\" : std::wstring(); }
inline std::wstring Shaders() { return Found() ? Root() + L"Bazar\\shaders\\" : std::wstring(); }
// Loads a DCS binary for inspection (no imports resolved); nullptr when not found.
inline HMODULE LoadBinary(const wchar_t* name) {
  return Found() ? LoadLibraryExW((Bin() + name).c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES) : nullptr;
}
inline void Skip(const char* name) { printf("SKIP %s: DCS install not found\n", name); }

}  // namespace dcsinstall
