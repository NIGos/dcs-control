// warmup.json: the shader warm-up's state for the DCS Control app (its DCS Engine page shows "Shader warm-up: 120 of
// 200 ready" while DCS runs). Next to DcsQvCull.log in the module folder; written by perf_mon.h's WarmTick on the
// perf worker thread (never the render thread): once at the first tick (idle, so a file from an earlier session is
// not read as current), about once per second while the prewarm or a warm-up runs, and once when it ends.
//
//   {"version":1,"state":"warming","done":120,"total":200,"hits":80,"compiled":40,"seconds":35}
//
//   state     idle | prewarm (stale cache entries rechecked from payload start, usually in the main menu) |
//             warming (shader variants of the scene compiling; bit 3 of the status word) | done (the last
//             warm-up ended; its numbers stay)
//   done      keys finished (warming) or prewarm entries checked (prewarm)
//   total     done plus the ones still queued or compiling (warming), entries found (prewarm); it may still grow
//   hits      read from the cache (warming) / entries still current (prewarm)
//   compiled  compiled (warming) / stale entries recompiled (prewarm)
//   seconds   since it began
// Atomic: a temp file, then MoveFileEx over the old one, so a reader never sees half a file.
// Pure: <windows.h> only, no logging (unit tested).
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace qvwarm {

struct State {
  const char* state = "idle";
  uint32_t done = 0, total = 0, hits = 0, compiled = 0;
  double seconds = 0;
};

inline std::string Json(const State& s) {
  char b[256];
  const double sec = s.seconds > 0 && s.seconds < 1e7 ? s.seconds : 0;
  snprintf(b, sizeof(b), "{\"version\":1,\"state\":\"%s\",\"done\":%u,\"total\":%u,\"hits\":%u,\"compiled\":%u,\"seconds\":%u}\n",
           s.state, s.done, s.total < s.done ? s.done : s.total, s.hits, s.compiled, static_cast<unsigned>(sec + 0.5));
  return b;
}

// dir: the module folder with a trailing backslash. False when it could not be written (nothing else happens).
inline bool Write(const std::wstring& dir, const std::string& json) {
  if (dir.empty()) return false;
  const std::wstring path = dir + L"warmup.json", tmp = dir + L"warmup.json.tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD wrote = 0;
  const bool ok = WriteFile(h, json.data(), static_cast<DWORD>(json.size()), &wrote, nullptr) && wrote == json.size();
  CloseHandle(h);
  if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
  DeleteFileW(tmp.c_str());
  return false;
}

}  // namespace qvwarm
