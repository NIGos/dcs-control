// R62: [Dev] IniPath / PayloadPath read from the deployed ini (loader and payload).
//
// The keys used to be read into MAX_PATH buffers: a path of 260 characters or
// more came back cut to 259, the cut file did not exist, and developer mode
// stayed off without a line in the log (the app accepts any length). DCS.exe
// is not long-path aware, so such a path cannot be used at all: it is read
// whole, refused, and the caller logs why once per value.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace qvdev {

enum DevPathState { kDevEmpty = 0, kDevOk, kDevTooLong, kDevMissing };

struct DevPath {
  DevPathState state = kDevEmpty;
  std::wstring path;
};

inline DevPath ReadDevPath(const wchar_t* key, const std::wstring& ini) {
  std::vector<wchar_t> buf(32768);
  const DWORD n = GetPrivateProfileStringW(L"Dev", key, L"", buf.data(), static_cast<DWORD>(buf.size()), ini.c_str());
  DevPath d;
  d.path.assign(buf.data(), n);
  if (d.path.empty()) return d;
  if (d.path.size() >= MAX_PATH) {
    d.state = kDevTooLong;
    return d;
  }
  d.state = GetFileAttributesW(d.path.c_str()) != INVALID_FILE_ATTRIBUTES ? kDevOk : kDevMissing;
  return d;
}

// True once for each unusable value (too long / not found) until the key is
// cleared or resolves again; *last remembers the value reported.
inline bool ReportOnce(const DevPath& d, std::wstring* last) {
  if (d.state != kDevTooLong && d.state != kDevMissing) {
    last->clear();
    return false;
  }
  if (*last == d.path) return false;
  *last = d.path;
  return true;
}

inline const char* Why(const DevPath& d) {
  return d.state == kDevTooLong ? "path too long (max 259 characters)" : "file not found";
}

}  // namespace qvdev
