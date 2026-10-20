// R30 L3: the environment a field log needs (read once at start and in the
// 30-minute footer): DCS version, CPU, the active OpenXR runtime and API
// layers from the registry (or the XR_* variables this process sees), and
// the VR streamers/runtimes running next to DCS. Plain Windows calls, no DCS
// state; every string is UTF-8 for the log.
#pragma once

#include <windows.h>
#include <intrin.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")  // registry (the test build links only kernel32/user32 by default)

namespace envinfo {

inline std::string Utf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n : 0, '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}

inline std::wstring BaseName(const std::wstring& p) {
  const size_t s = p.find_last_of(L"\\/");
  return s == std::wstring::npos ? p : p.substr(s + 1);
}

// File version of the process image (DCS.exe), "a.b.c.d"; empty when absent.
// version.dll is loaded on demand (no import of it in the payload).
inline std::string ExeVersion() {
  wchar_t path[MAX_PATH * 2] = {};
  if (!GetModuleFileNameW(nullptr, path, MAX_PATH * 2)) return std::string();
  HMODULE v = LoadLibraryW(L"version.dll");
  if (!v) return std::string();
  using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
  using InfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
  using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
  auto size = reinterpret_cast<SizeFn>(GetProcAddress(v, "GetFileVersionInfoSizeW"));
  auto info = reinterpret_cast<InfoFn>(GetProcAddress(v, "GetFileVersionInfoW"));
  auto query = reinterpret_cast<QueryFn>(GetProcAddress(v, "VerQueryValueW"));
  std::string out;
  DWORD h = 0;
  const DWORD n = size && info && query ? size(path, &h) : 0;
  if (n) {
    std::vector<uint8_t> buf(n);
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (info(path, 0, n, buf.data()) && query(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&fi), &len) && fi &&
        len >= sizeof(VS_FIXEDFILEINFO)) {
      char b[64];
      snprintf(b, sizeof(b), "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
               HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
      out = b;
    }
  }
  FreeLibrary(v);
  return out;
}

// The rest of the first line containing `marker` within the first maxBytes
// of a text file (read with full sharing); empty when absent.
inline std::string FirstLineWith(const std::wstring& path, const char* marker, size_t maxBytes = 256 * 1024) {
  FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
  if (!f) return std::string();
  std::string text(maxBytes, '\0');
  text.resize(fread(&text[0], 1, maxBytes, f));
  fclose(f);
  const size_t p = text.find(marker);
  if (p == std::string::npos) return std::string();
  size_t e = text.find_first_of("\r\n", p);
  if (e == std::string::npos) e = text.size();
  return text.substr(p, e - p);
}

inline std::string CpuBrand() {
  int r[4] = {};
  __cpuid(r, 0x80000000);
  if (static_cast<unsigned>(r[0]) < 0x80000004u) return std::string();
  char brand[49] = {};
  for (int i = 0; i < 3; ++i) {
    __cpuid(r, 0x80000002 + i);
    memcpy(brand + 16 * i, r, 16);
  }
  std::string s = brand;
  const size_t b = s.find_first_not_of(' ');
  if (b == std::string::npos) return std::string();
  return s.substr(b, s.find_last_not_of(' ') - b + 1);
}

inline std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value) {
  wchar_t buf[1024] = {};
  DWORD bytes = sizeof(buf) - sizeof(wchar_t);
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS)
    return std::wstring();
  return buf;
}

inline std::wstring EnvVar(const wchar_t* name) {
  wchar_t buf[2048] = {};
  const DWORD n = GetEnvironmentVariableW(name, buf, 2048);
  return n && n < 2048 ? std::wstring(buf) : std::wstring();
}

// "name" of the runtime object in an OpenXR runtime manifest (simple scan:
// the first "name" key after "runtime"); empty when unreadable.
inline std::string ManifestName(const std::wstring& json) {
  FILE* f = _wfsopen(json.c_str(), L"rb", _SH_DENYNO);
  if (!f) return std::string();
  char buf[8192] = {};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  std::string t(buf, n);
  size_t p = t.find("\"runtime\"");
  p = t.find("\"name\"", p == std::string::npos ? 0 : p);
  if (p == std::string::npos) return std::string();
  p = t.find(':', p);
  if (p == std::string::npos) return std::string();
  p = t.find('"', p);
  if (p == std::string::npos) return std::string();
  const size_t e = t.find('"', p + 1);
  return e == std::string::npos ? std::string() : t.substr(p + 1, e - p - 1);
}

constexpr wchar_t kXrKey[] = L"SOFTWARE\\Khronos\\OpenXR\\1";

// The active OpenXR runtime: "<name> (<manifest file>)", from XR_RUNTIME_JSON
// when this process has it, else the registry's ActiveRuntime.
inline std::string XrRuntime() {
  std::wstring json = EnvVar(L"XR_RUNTIME_JSON");
  const bool fromEnv = !json.empty();
  if (!fromEnv) json = RegString(HKEY_LOCAL_MACHINE, kXrKey, L"ActiveRuntime");
  if (json.empty()) return "none registered";
  std::string name = ManifestName(json);
  return (name.empty() ? std::string("?") : name) + " (" + Utf8(BaseName(json)) + (fromEnv ? ", XR_RUNTIME_JSON" : "") +
         ")";
}

// The string value of `key` in a manifest's text (simple scan, as ManifestName); empty when absent.
inline std::string JsonString(const std::string& t, const char* key) {
  const std::string k = std::string("\"") + key + "\"";
  size_t p = t.find(k);
  if (p == std::string::npos) return std::string();
  p = t.find(':', p + k.size());
  if (p == std::string::npos) return std::string();
  p = t.find_first_not_of(" \t\r\n", p + 1);
  if (p == std::string::npos || t[p] != '"') return std::string();
  const size_t e = t.find('"', p + 1);
  return e == std::string::npos ? std::string() : t.substr(p + 1, e - p - 1);
}

inline std::string ReadManifest(const std::wstring& path) {
  FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
  if (!f) return std::string();
  char buf[8192] = {};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  return std::string(buf, n);
}

inline bool EnvSet(const std::string& name) {
  if (name.empty()) return false;
  return GetEnvironmentVariableA(name.c_str(), nullptr, 0) > 0;
}

// R63: what the OpenXR loader does with an enabled implicit layer in this process, from its manifest:
// disable_environment set -> " (disabled by VAR)"; enable_environment unset -> " (off: VAR not set)"; else "".
// (DCS VR Control disables the user's implicit Quad Views / gaze bridge this way and adds its own explicitly.)
template <class IsSet>
std::string ImplicitLayerNote(const std::string& manifest, IsSet isSet) {
  const std::string dis = JsonString(manifest, "disable_environment");
  if (!dis.empty() && isSet(dis)) return " (disabled by " + dis + ")";
  const std::string en = JsonString(manifest, "enable_environment");
  if (!en.empty() && !isSet(en)) return " (off: " + en + " not set)";
  return std::string();
}

// Enabled implicit API layers (HKLM and HKCU, value 0 = enabled): manifest
// file names without ".json", comma separated, each marked when this
// process's environment turns it off (R63); plus XR_ENABLE_API_LAYERS and
// XR_API_LAYER_PATH.
inline std::string XrLayers() {
  std::string out;
  for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0, KEY_READ, &k) != ERROR_SUCCESS)
      continue;
    for (DWORD i = 0;; ++i) {
      wchar_t name[1024];
      DWORD nameLen = 1024, type = 0, data = 1, dataLen = sizeof(data);
      const LONG r = RegEnumValueW(k, i, name, &nameLen, nullptr, &type, reinterpret_cast<BYTE*>(&data), &dataLen);
      if (r == ERROR_NO_MORE_ITEMS) break;
      if (r != ERROR_SUCCESS || type != REG_DWORD || data != 0) continue;
      std::wstring b = BaseName(name);
      if (b.size() > 5 && _wcsicmp(b.c_str() + b.size() - 5, L".json") == 0) b.resize(b.size() - 5);
      out += (out.empty() ? "" : ", ") + Utf8(b) + (root == HKEY_CURRENT_USER ? " (user)" : "") +
             ImplicitLayerNote(ReadManifest(name), [](const std::string& v) { return EnvSet(v); });
    }
    RegCloseKey(k);
  }
  const std::wstring en = EnvVar(L"XR_ENABLE_API_LAYERS");
  if (!en.empty()) out += (out.empty() ? "" : ", ") + std::string("XR_ENABLE_API_LAYERS=") + Utf8(en);
  const std::wstring lp = EnvVar(L"XR_API_LAYER_PATH");
  if (!lp.empty()) out += (out.empty() ? "" : ", ") + std::string("XR_API_LAYER_PATH=") + Utf8(lp);
  return out.empty() ? "none" : out;
}

// VR streamers and runtimes running (process snapshot).
inline std::string Streamers() {
  static const struct {
    const wchar_t* exe;
    const char* what;
  } kKnown[] = {
      {L"VirtualDesktop.Streamer.exe", "Virtual Desktop streamer"},
      {L"VirtualDesktop.Service.exe", "Virtual Desktop service"},
      {L"vrserver.exe", "SteamVR"},
      {L"vrcompositor.exe", "SteamVR compositor"},
      {L"OVRServer_x64.exe", "Oculus runtime"},
      {L"OculusDash.exe", "Oculus Dash"},
      {L"PiService.exe", "Pimax service"},
      {L"PimaxClient.exe", "Pimax Play"},
      {L"MixedRealityPortal.exe", "Mixed Reality portal"},
      {L"alvr_dashboard.exe", "ALVR"},
      {L"SteamLink.exe", "Steam Link"},
  };
  bool seen[sizeof(kKnown) / sizeof(kKnown[0])] = {};
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return "unknown";
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
    for (size_t i = 0; i < sizeof(kKnown) / sizeof(kKnown[0]); ++i)
      if (_wcsicmp(pe.szExeFile, kKnown[i].exe) == 0) seen[i] = true;
  CloseHandle(snap);
  std::string out;
  for (size_t i = 0; i < sizeof(kKnown) / sizeof(kKnown[0]); ++i)
    if (seen[i]) out += (out.empty() ? "" : ", ") + std::string(kKnown[i].what);
  return out.empty() ? "none" : out;
}

}  // namespace envinfo
