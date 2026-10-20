// R90 section 7 (docs/research/R90_hitch_hunting.md): disk hygiene for DcsQvCull's output families. Runs once
// on the payload worker at start (never the render thread), before the config read applies shadow instancing
// (whose compile workers read cache\), plus a touch-on-hit for the .qvc cache (CacheLoad, compile worker).
//
// Every family stays where its writer and DCS Control expect it (the module folder <mod>, or the dev folder
// <devOr> in dev mode); nothing is moved. Shipped defaults write none of the diagnostic families (each needs a
// key or a run_*.flag); these caps bound them once one was used:
//   keep newest kKeepFiles by last-write time (at <mod>): report_*.txt, bench_*.csv, profile_*.txt,
//                                       motion_*.csv, rotation_*.csv, timeline_*.csv, events_*.csv
//   keep newest kKeepFiles (<mod> and <devOr>): fixdepth_*.pgm; gdd_snap_*.bin: newest 64
//   keep newest kKeepDumpDirs folders (<mod> and <devOr>): fg_dump\*, cockpit_frames\*, tone_probe\*
//   size-rotate past 4 MiB to gbrec_states.prev.jsonl (<mod> and <devOr>): gbrec_states.jsonl
//   LRU + size/count cap (<mod>\cache): *.qvc; orphaned *.tmp older than 1 h deleted
// Never touched: DcsQvCull.log, DcsQvCull.prev.log, DcsQvCull.<pid>.log (the loader prunes those),
// DcsQvCullPrefetch*.log, the ini files, the flags, payload\.
//
// .qvc keys: CachePathLocked() names a file s_/g_<FNV64(identity)>.qvc; identity = the snapshot's
// shader key, so the same shader variant always maps to the same file and a rebuilt entry (edit
// version, compiler or source change) overwrites it in place. Files only accumulate when the set of
// identities changes (DCS updates, mods, new aircraft/terrains, setting combinations). A cap must
// therefore evict by LAST USE, not by creation: NTFS last-access time is unreliable (often disabled
// by the volume's policy), so CacheLoad's hit path stamps the file's last-write time (one
// SetFileTime through a FILE_WRITE_ATTRIBUTES handle, compile worker thread, ~20 us). A hit can
// never be evicted in the session that used it: the prune runs before any load of this session and
// never touches files stamped within kQvcMinAgeDays. A wrong eviction costs one recompile on an
// idle-priority worker at the next mission start (the first-install path), never a pixel.
// Included once from main.cpp (after patch_threads.h); the offline test is test/r90_test.h.
#pragma once
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace prune {

struct Entry {
  std::wstring path;
  uint64_t size = 0;
  uint64_t written = 0;  // FILETIME as 100 ns ticks
  bool dir = false;
};

constexpr uint32_t kKeepFiles = 20;               // per timestamped family
constexpr uint32_t kKeepDumpDirs = 3;             // per dump-folder family
constexpr uint64_t kJsonlCap = 4ull << 20;        // gbrec_states.jsonl rotates to .prev.jsonl
constexpr uint64_t kQvcCapBytes = 64ull << 20;    // whole cache\ folder
constexpr uint32_t kQvcCapFiles = 4000;
constexpr uint32_t kQvcMinAgeDays = 30;           // never evict an entry used in the last 30 days
constexpr uint32_t kTmpMinAgeHours = 1;           // orphaned .tmp from a crash mid-write

inline uint64_t Ticks(const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

inline uint64_t NowTicks() {
  FILETIME f;
  GetSystemTimeAsFileTime(&f);
  return Ticks(f);
}

// dir with trailing backslash; pattern like L"report_*.txt".
inline std::vector<Entry> List(const std::wstring& dir, const wchar_t* pattern, bool wantDirs) {
  std::vector<Entry> out;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileExW((dir + pattern).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                              FIND_FIRST_EX_LARGE_FETCH);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (isDir != wantDirs) continue;
    if (isDir && (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))) continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // never follow links
    Entry e;
    e.path = dir + fd.cFileName;
    e.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    e.written = Ticks(fd.ftLastWriteTime);
    e.dir = isDir;
    out.push_back(std::move(e));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return out;
}

// Deletes a dump folder: its files, one level deep (our dump folders have no subfolders).
inline bool RemoveDumpDir(const std::wstring& dir) {
  for (const Entry& f : List(dir + L"\\", L"*", false)) DeleteFileW(f.path.c_str());
  return RemoveDirectoryW(dir.c_str()) != 0;
}

struct Stats {
  uint32_t files = 0, dirs = 0;
  uint64_t bytes = 0;
};

// Keeps the newest `keep` (by last-write time); deletes the rest.
inline void KeepNewest(const std::wstring& dir, const wchar_t* pattern, uint32_t keep, bool dirs, Stats& st) {
  std::vector<Entry> v = List(dir, pattern, dirs);
  if (v.size() <= keep) return;
  std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) { return a.written > b.written; });
  for (size_t i = keep; i < v.size(); ++i) {
    if (dirs ? RemoveDumpDir(v[i].path) : DeleteFileW(v[i].path.c_str()) != 0) {
      dirs ? ++st.dirs : ++st.files;
      st.bytes += v[i].size;
    }
  }
}

inline void RotateIfOver(const std::wstring& path, const std::wstring& prev, uint64_t cap) {
  WIN32_FILE_ATTRIBUTE_DATA a;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return;
  const uint64_t sz = (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
  if (sz > cap) MoveFileExW(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// cacheDir with trailing backslash. LRU by last-write (stamped on every hit, see TouchOnHit).
inline void PruneQvc(const std::wstring& cacheDir, uint64_t now, Stats& st) {
  const uint64_t day = 864000000000ull, hour = 36000000000ull;
  for (const Entry& t : List(cacheDir, L"*.tmp", false))
    if (now > t.written && now - t.written > kTmpMinAgeHours * hour && DeleteFileW(t.path.c_str())) {
      ++st.files;
      st.bytes += t.size;
    }
  std::vector<Entry> v = List(cacheDir, L"*.qvc", false);
  uint64_t total = 0;
  for (const Entry& e : v) total += e.size;
  if (total <= kQvcCapBytes && v.size() <= kQvcCapFiles) return;
  std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) { return a.written < b.written; });  // oldest first
  size_t n = v.size();
  for (const Entry& e : v) {
    if (total <= kQvcCapBytes * 3 / 4 && n <= kQvcCapFiles * 3 / 4) break;  // hysteresis: prune to 75 %
    if (now > e.written && now - e.written < kQvcMinAgeDays * day) break;  // everything newer is in use
    if (DeleteFileW(e.path.c_str())) {
      total -= e.size;
      --n;
      ++st.files;
      st.bytes += e.size;
    }
  }
}

// CacheLoad hit path (compile worker): mark the entry as used. Best effort.
inline void TouchOnHit(const std::wstring& path) {
  HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  SetFileTime(h, nullptr, nullptr, &now);
  CloseHandle(h);
}

// Payload worker, once at start. mod / devOr: folders with trailing backslash (devOr may equal mod).
inline Stats RunAtStart(const std::wstring& mod, const std::wstring& devOr) {
  Stats st;
  const uint64_t now = NowTicks();
  for (const wchar_t* p : {L"report_*.txt", L"bench_*.csv", L"profile_*.txt", L"motion_*.csv", L"rotation_*.csv",
                           L"timeline_*.csv", L"events_*.csv"})
    KeepNewest(mod, p, kKeepFiles, false, st);
  for (const std::wstring& d : {mod, devOr}) {
    KeepNewest(d, L"fixdepth_*.pgm", kKeepFiles, false, st);
    KeepNewest(d, L"gdd_snap_*.bin", 64, false, st);
    KeepNewest(d + L"fg_dump\\", L"*", kKeepDumpDirs, true, st);
    RotateIfOver(d + L"gbrec_states.jsonl", d + L"gbrec_states.prev.jsonl", kJsonlCap);
    if (d == mod && devOr == mod) break;
  }
  for (const std::wstring& d : {mod, devOr}) {
    KeepNewest(d + L"cockpit_frames\\", L"*", kKeepDumpDirs, true, st);
    KeepNewest(d + L"tone_probe\\", L"*", kKeepDumpDirs, true, st);
    if (d == mod && devOr == mod) break;
  }
  PruneQvc(mod + L"cache\\", now, st);
  return st;
}

}  // namespace prune
