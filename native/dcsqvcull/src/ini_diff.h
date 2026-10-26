// R142 (docs/research/R142_live_reload.md): what an ini reload changed, key by key.
//
// DCS Control's Live mode rewrites DcsQvCull.ini while DCS runs (visual realism, depth boost, ...). Before R142 any
// new mtime turned the in-flight kill switch ([Hotkeys] Toggle, toggle_engine.flag) back ON, so a live write of a
// colour value re-enabled the engine optimizations the pilot had switched off. Now LoadConfig takes a snapshot of
// the ini's keys at every reload, diffs it with the previous one, logs the changed keys, and resets the kill switch
// only when an engine key (IsEngineKey below) changed value.
//
// Pure parts (no main.cpp globals), so the offline tests drive them directly:
//   - Snapshot / ParseText / ReadFile: the keys of one ini as GetPrivateProfile* sees them (sections and keys
//     case-insensitive, the first section of a name and the first key of a name win, values trimmed, one pair of
//     surrounding quotes removed, ';' and '#' lines are comments; UTF-16 LE with BOM, UTF-8 with BOM, else ANSI);
//   - Diff: the keys whose value differs (added and removed keys included), values compared case-insensitively
//     (every parser in the payload reads values case-insensitively: numbers, on/off words, hotkeys, paths);
//   - Classify / IsEngineKey: the engine key set (below);
//   - FormatChanges: "Section.Key old->new, ..." for the reload line ("(none)" for an absent key);
//   - reloadft: the frame-time windows around a reload ([Log] ReloadFrameTimes, default 0).
//
// The engine key set (the keys whose change means "the pilot re-configured the engine optimizations"):
//   - whole sections: [Cull] [Scene] [Model] [Texture] [Effects] [D3D] [Shadows] [Shadow] [Timing] [Prefetch]
//     [Threads] [Governor] [Suite] [Bench]: every optimization the kill switch gates (engine.inc SetEngineOff,
//     worker.inc's retries), the governor that steps them, and the suite / benchmark keys;
//   - [General] LowPowerPacer and LowPowerPacerTimeoutUs (the pacer is gated by the kill switch).
// Not engine keys (a change keeps the kill switch as it is): [Color] [ColorFix] [ColorEnhanced] [Night] [VrFx]
// [DLSS] [Perception] [DepthBoost] [Log] [Perf] [Profile] [Hotkeys] [Dev], and [General] PerceptionOnly (latched
// at start), Diagnostics (measurement hooks; R144: a live 0 -> 1 installs them), Beeps, SigScan, PatchMode. [DLSS]: the per-view preset passes
// through while the kill switch is off and acts only at the next mission load; a Live write of it must not turn the
// engine back on. Unknown sections (not read by the payload) are not engine keys.
#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace inidiff {

struct Entry {
  std::wstring sec, key, val;  // as written (the first spelling seen)
};
// Keyed by lower-case "section\x01key".
struct Snapshot {
  bool ok = false;  // false: the file exists but could not be read (sharing violation, ...)
  std::map<std::wstring, Entry> keys;
};

inline std::wstring Lower(std::wstring s) {
  for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
  return s;
}
inline std::wstring Trim(const std::wstring& s) {
  size_t a = 0, b = s.size();
  while (a < b && iswspace(s[a])) ++a;
  while (b > a && iswspace(s[b - 1])) --b;
  return s.substr(a, b - a);
}
inline std::wstring MapKey(const std::wstring& sec, const std::wstring& key) { return Lower(sec) + L'\x01' + Lower(key); }

inline Snapshot ParseText(const std::wstring& text) {
  Snapshot s;
  s.ok = true;
  std::vector<std::wstring> seenSections;
  std::wstring sec;
  bool skip = true;  // keys before any section header, or in a repeated section, are not read by GetPrivateProfile*
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t eol = text.find_first_of(L"\r\n", pos);
    if (eol == std::wstring::npos) eol = text.size();
    const std::wstring line = Trim(text.substr(pos, eol - pos));
    pos = eol + 1;
    if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
    if (line[0] == L'[') {
      const size_t close = line.find(L']');
      sec = Trim(line.substr(1, (close == std::wstring::npos ? line.size() : close) - 1));
      const std::wstring low = Lower(sec);
      skip = std::find(seenSections.begin(), seenSections.end(), low) != seenSections.end();
      if (!skip) seenSections.push_back(low);
      continue;
    }
    if (skip) continue;
    const size_t eq = line.find(L'=');
    if (eq == std::wstring::npos) continue;
    const std::wstring key = Trim(line.substr(0, eq));
    if (key.empty()) continue;
    std::wstring val = Trim(line.substr(eq + 1));
    if (val.size() >= 2 && ((val.front() == L'"' && val.back() == L'"') || (val.front() == L'\'' && val.back() == L'\'')))
      val = val.substr(1, val.size() - 2);
    const std::wstring mk = MapKey(sec, key);
    if (s.keys.find(mk) == s.keys.end()) s.keys[mk] = Entry{sec, key, val};
  }
  return s;
}

// The bytes of an ini file to text, the way the profile API decodes them.
inline std::wstring Decode(const std::string& b) {
  if (b.size() >= 2 && static_cast<uint8_t>(b[0]) == 0xFF && static_cast<uint8_t>(b[1]) == 0xFE) {
    std::wstring w((b.size() - 2) / 2, L'\0');
    if (!w.empty()) memcpy(&w[0], b.data() + 2, w.size() * 2);
    return w;
  }
  UINT cp = CP_ACP;
  size_t off = 0;
  if (b.size() >= 3 && static_cast<uint8_t>(b[0]) == 0xEF && static_cast<uint8_t>(b[1]) == 0xBB &&
      static_cast<uint8_t>(b[2]) == 0xBF) {
    cp = CP_UTF8;
    off = 3;
  }
  if (b.size() <= off) return std::wstring();
  const int n = MultiByteToWideChar(cp, 0, b.data() + off, static_cast<int>(b.size() - off), nullptr, 0);
  std::wstring w(n > 0 ? n : 0, L'\0');
  if (n > 0) MultiByteToWideChar(cp, 0, b.data() + off, static_cast<int>(b.size() - off), &w[0], n);
  return w;
}

// A missing file is an empty snapshot (ok: every key takes its default, as LoadConfig reads it); a file that exists
// but cannot be opened or read is ok == false. At most 4 MiB (the ini is a few KB).
inline Snapshot ReadFile(const wchar_t* path) {
  Snapshot s;
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD e = GetLastError();
    s.ok = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
    return s;
  }
  LARGE_INTEGER size{};
  std::string bytes;
  bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart <= (4 << 20);
  if (ok) {
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    ok = bytes.empty() || (::ReadFile(h, &bytes[0], static_cast<DWORD>(bytes.size()), &got, nullptr) != 0);
    bytes.resize(ok ? got : 0);
  }
  CloseHandle(h);
  if (!ok) return s;
  return ParseText(Decode(bytes));
}

// Adds `from`'s keys of one section that `to` lacks (the deployed ini's [Dev] keys under dev mode, as LoadConfig
// reads [Dev] SymbologyPublishAlways).
inline void MergeSection(Snapshot& to, const Snapshot& from, const wchar_t* section) {
  const std::wstring low = Lower(section);
  for (const auto& kv : from.keys)
    if (Lower(kv.second.sec) == low && to.keys.find(kv.first) == to.keys.end()) to.keys[kv.first] = kv.second;
}

enum Class : int { kNotEngine = 0, kEngine = 1, kUnknown = 2 };

inline bool In(const std::wstring& low, std::initializer_list<const wchar_t*> names) {
  for (const wchar_t* n : names)
    if (low == n) return true;
  return false;
}

// kUnknown: a section the payload does not read (not an engine key either; reported by the tests).
inline Class Classify(const std::wstring& sec, const std::wstring& key) {
  const std::wstring s = Lower(sec), k = Lower(key);
  if (In(s, {L"cull", L"scene", L"model", L"texture", L"effects", L"d3d", L"shadows", L"shadow", L"timing",
             L"prefetch", L"threads", L"governor", L"suite", L"bench"}))
    return kEngine;
  if (s == L"general") return In(k, {L"lowpowerpacer", L"lowpowerpacertimeoutus"}) ? kEngine : kNotEngine;
  if (In(s, {L"color", L"colorfix", L"colorenhanced", L"night", L"vrfx", L"dlss", L"perception", L"depthboost",
             L"log", L"perf", L"profile", L"hotkeys", L"dev"}))
    return kNotEngine;
  return kUnknown;
}
inline bool IsEngineKey(const std::wstring& sec, const std::wstring& key) { return Classify(sec, key) == kEngine; }

struct Change {
  std::wstring sec, key, oldVal, newVal;
  bool hadOld = false, hasNew = false;
  bool engine = false;
};

inline std::vector<Change> Diff(const Snapshot& a, const Snapshot& b) {
  std::vector<Change> out;
  auto ia = a.keys.begin(), ib = b.keys.begin();
  while (ia != a.keys.end() || ib != b.keys.end()) {
    Change c;
    if (ib == b.keys.end() || (ia != a.keys.end() && ia->first < ib->first)) {
      c.sec = ia->second.sec, c.key = ia->second.key, c.oldVal = ia->second.val, c.hadOld = true;
      ++ia;
    } else if (ia == a.keys.end() || ib->first < ia->first) {
      c.sec = ib->second.sec, c.key = ib->second.key, c.newVal = ib->second.val, c.hasNew = true;
      ++ib;
    } else {
      if (_wcsicmp(ia->second.val.c_str(), ib->second.val.c_str()) != 0) {
        c.sec = ib->second.sec, c.key = ib->second.key;
        c.oldVal = ia->second.val, c.newVal = ib->second.val;
        c.hadOld = c.hasNew = true;
      }
      ++ia, ++ib;
      if (!c.hadOld) continue;
    }
    c.engine = IsEngineKey(c.sec, c.key);
    out.push_back(c);
  }
  return out;
}

inline bool AnyEngine(const std::vector<Change>& ch) {
  for (const Change& c : ch)
    if (c.engine) return true;
  return false;
}

inline std::string Utf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n : 0, '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}

// One "Section.Key old->new" item; a value longer than 80 characters (a path) is cut with "...".
inline std::string Item(const Change& c) {
  auto v = [](bool has, const std::wstring& s) {
    if (!has) return std::string("(none)");
    std::string u = Utf8(s);
    if (u.empty()) return std::string("\"\"");
    if (u.size() > 80) u = u.substr(0, 77) + "...";
    return u;
  };
  return Utf8(c.sec) + "." + Utf8(c.key) + " " + v(c.hadOld, c.oldVal) + "->" + v(c.hasNew, c.newVal);
}

// The changed keys as lines of at most maxChars each ("A.b 0->1, C.d 2->3"); "none" when nothing changed.
inline std::vector<std::string> FormatChanges(const std::vector<Change>& ch, size_t maxChars = 1500) {
  std::vector<std::string> lines;
  if (ch.empty()) {
    lines.push_back("none");
    return lines;
  }
  std::string cur;
  for (const Change& c : ch) {
    const std::string it = Item(c);
    if (!cur.empty() && cur.size() + 2 + it.size() > maxChars) {
      lines.push_back(cur);
      cur.clear();
    }
    cur += (cur.empty() ? "" : ", ") + it;
  }
  lines.push_back(cur);
  return lines;
}

}  // namespace inidiff

// [Log] ReloadFrameTimes (default 0): the frame intervals around an ini reload, from perfgov's frame ring.
namespace reloadft {

struct Win {
  int n = 0;
  double p50 = 0, p99 = 0, max = 0;
};
struct Result {
  Win base, pre, post;  // base: 12 s .. 2 s before the reload; pre: the 2 s before; post: the 2 s after
};

// Nearest-rank quantile of a copy.
inline Win Stats(std::vector<float> v) {
  Win w;
  w.n = static_cast<int>(v.size());
  if (v.empty()) return w;
  std::sort(v.begin(), v.end());
  auto q = [&](double p) {
    size_t r = static_cast<size_t>(std::ceil(p * v.size()));
    if (r < 1) r = 1;
    if (r > v.size()) r = v.size();
    return static_cast<double>(v[r - 1]);
  };
  w.p50 = q(0.5);
  w.p99 = q(0.99);
  w.max = v.back();
  return w;
}

// before: intervals (ms) of the frames before the reload, oldest first, the last one ending at the reload;
// after: the frames from the reload on, oldest first. Windows by summed frame time.
inline Result Compute(const std::vector<float>& before, const std::vector<float>& after, double preMs = 2000,
                      double postMs = 2000, double baseMs = 10000) {
  Result r;
  std::vector<float> pre, base, post;
  double t = 0;
  for (size_t i = before.size(); i-- > 0;) {
    const float v = before[i];
    if (!(v > 0)) continue;
    t += v;
    if (t <= preMs) pre.push_back(v);
    else if (t <= preMs + baseMs) base.push_back(v);
    else break;
  }
  t = 0;
  for (const float v : after) {
    if (!(v > 0)) continue;
    if (t >= postMs) break;
    post.push_back(v);
    t += v;
  }
  r.base = Stats(base);
  r.pre = Stats(pre);
  r.post = Stats(post);
  return r;
}

}  // namespace reloadft
