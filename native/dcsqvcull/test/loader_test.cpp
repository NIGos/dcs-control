// Offline checks of the loader (loader.cpp): payload swap against DCS exit,
// previous session log kept. Built and run by build_test.bat with
// qv_fake_payload.dll next to it.
#include "../src/loader.cpp"

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}
std::atomic<int> g_starts{0};
std::atomic<bool> g_exitDuringStop{false};

bool WriteText(const std::wstring& path, const char* text) {
  FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f) return false;
  fputs(text, f);
  fclose(f);
  return true;
}
std::string ReadText(const std::wstring& path) {
  std::string s;
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return s;
  char buf[256];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}
}  // namespace

std::atomic<int> g_startResult{0};
extern "C" __declspec(dllexport) void QvTestStarted() { g_starts.fetch_add(1); }
extern "C" __declspec(dllexport) int QvTestStartResult() { return g_startResult.load(); }

namespace {
// R62: the loader's lines written while f runs (g_log pointed at a temporary file).
template <class F>
std::string Logged(const std::wstring& dir, F f) {
  const std::wstring path = dir + L"captured.log";
  FILE* saved = g_log;
  FILE* cap = nullptr;
  if (_wfopen_s(&cap, path.c_str(), L"wb") != 0) cap = nullptr;
  g_log = cap;
  f();
  g_log = saved;
  if (cap) fclose(cap);
  std::string s = ReadText(path);
  DeleteFileW(path.c_str());
  fputs(s.c_str(), stdout);
  return s;
}
}  // namespace
// The old payload's Stop is running when DCS starts exiting (DcsQvCull_Shutdown
// sets g_exiting first, then gives up on the swap lock after 3 s).
extern "C" __declspec(dllexport) void QvTestStopping() {
  if (g_exitDuringStop.load()) g_exiting = true;
}

int main() {
  g_log = stdout;
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring here = exe;
  here = here.substr(0, here.find_last_of(L"\\/") + 1);
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  g_dir = std::wstring(tmp) + L"qv_loader_test_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
  CreateDirectoryW(g_dir.c_str(), nullptr);
  CreateDirectoryW((g_dir + L"payload\\").c_str(), nullptr);
  const std::wstring fake = here + L"qv_fake_payload.dll";

  // L2: DCS exits while the previous payload stops -> the new one is not started.
  {
    const LoadResult r1 = LoadPayload(fake);
    const bool first = r1 == kLoadOk && g_starts.load() == 1 && g_stopActive != nullptr;
    Sleep(50);  // a new active_<tick>.dll name
    g_exitDuringStop = true;
    const LoadResult r2 = LoadPayload(fake);
    Check(first && r2 == kLoadFailed && g_starts.load() == 1 && g_stopActive == nullptr &&
              g_quiesceActive.load() == nullptr,
          "R59 L2 loader: DCS exiting during the previous payload's Stop -> the new payload is not started");
    g_exiting = false;
    g_exitDuringStop = false;
    // Not exiting: a swap still starts the new payload.
    Sleep(50);
    const LoadResult r3 = LoadPayload(fake);
    Sleep(50);
    const LoadResult r4 = LoadPayload(fake);
    Check(r3 == kLoadOk && r4 == kLoadOk && g_starts.load() == 3,
          "R59 L2 loader: an ordinary swap still stops the old payload and starts the new one");
  }

  // log-truncated-no-old: the previous session's log survives as DcsQvCull.prev.log.
  {
    const std::wstring log = g_dir + L"DcsQvCull.log", prev = g_dir + L"DcsQvCull.prev.log";
    WriteText(prev, "older session\n");
    WriteText(log, "crashed session\n");
    const bool moved = KeepPreviousLog(g_dir);
    const bool kept = ReadText(prev) == "crashed session\n" && GetFileAttributesW(log.c_str()) == INVALID_FILE_ATTRIBUTES;
    DeleteFileW(prev.c_str());
    const bool noLog = !KeepPreviousLog(g_dir);  // no log yet (first start): nothing to keep, no error
    Check(moved && kept && noLog, "R59 loader log: the previous session's DcsQvCull.log is kept as DcsQvCull.prev.log");
  }

  // R62 d1-loader-failure-invisible: a failed Start is not "starting payload" and not the active payload.
  {
    Sleep(50);
    g_startResult = 1;  // loader API version mismatch
    LoadResult r = kLoadOk;
    const std::string log = Logged(g_dir, [&] { r = LoadPayload(fake); });
    g_startResult = 0;
    Check(r == kLoadFailed && g_stopActive == nullptr && g_quiesceActive.load() == nullptr &&
              g_missionActive.load() == nullptr && log.find("loader: starting payload") == std::string::npos &&
              log.find("loader: payload start reported an error (1)") != std::string::npos,
          "R62 loader: a payload whose Start fails is neither logged as started nor kept as the active payload");
    Sleep(50);
    const std::string ok = Logged(g_dir, [&] { r = LoadPayload(fake); });
    Check(r == kLoadOk && g_stopActive != nullptr && ok.find("loader: starting payload active_") != std::string::npos,
          "R62 loader: a payload that starts is logged as 'loader: starting payload' (the app's 'loaded' line)");
    // Out of retries on one file version: one terminal line.
    int retries = 0;
    bool done[kLoadAttempts] = {};
    const std::string gave = Logged(g_dir, [&] {
      for (int i = 0; i < kLoadAttempts; ++i) done[i] = DoneWithVersion(kLoadRetry, &retries);
    });
    bool early = false;
    for (int i = 0; i + 1 < kLoadAttempts; ++i) early = early || done[i];
    Check(!early && done[kLoadAttempts - 1] && retries == 0 &&
              gave.find("loader: payload not loaded (5 attempts failed)") != std::string::npos &&
              gave.find("loader started") == std::string::npos,
          "R62 loader: giving up on a payload version after 5 failed attempts is logged");
    retries = 2;
    Check(DoneWithVersion(kLoadOk, &retries) && retries == 0, "R62 loader: a load ends the version's retries");
  }

  // R62 d3-dev-reload-stale-timestamp: the load waits for an unchanged write time and keeps the settled one.
  {
    const std::wstring f = g_dir + L"settle.dll";
    WriteText(f, "x");
    WIN32_FILE_ATTRIBUTE_DATA a{};
    GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &a);
    FILETIME now{};
    const bool stable = SettledWriteTime(f, a.ftLastWriteTime, 10, &now) &&
                        CompareFileTime(&now, &a.ftLastWriteTime) == 0;
    // The writer touches the file during the settle (a later write time).
    FILETIME later = a.ftLastWriteTime;
    ULARGE_INTEGER u{};
    u.LowPart = later.dwLowDateTime, u.HighPart = later.dwHighDateTime;
    u.QuadPart += 10000000ull;  // +1 s
    later.dwLowDateTime = u.LowPart, later.dwHighDateTime = u.HighPart;
    HANDLE h = CreateFileW(f.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           0, nullptr);
    const bool touched = h != INVALID_HANDLE_VALUE && SetFileTime(h, nullptr, nullptr, &later);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    const bool moving = !SettledWriteTime(f, a.ftLastWriteTime, 10, &now) && CompareFileTime(&now, &later) == 0;
    DeleteFileW(f.c_str());
    const bool gone = !SettledWriteTime(f, later, 10, &now);
    Check(stable && touched && moving && gone,
          "R62 loader: a payload still being written is not loaded until its write time holds over the settle");
  }

  // R62 d2-concurrent-dcs-log-lost: another process holding DcsQvCull.log -> this session logs to DcsQvCull.<pid>.log.
  {
    const std::wstring log = g_dir + L"DcsQvCull.log";
    FILE* other = _wfsopen(log.c_str(), L"w", _SH_DENYWR);  // the earlier DCS's handle
    const bool kept = !KeepPreviousLog(g_dir);  // the move fails while it is held
    std::wstring used;
    FILE* mine = OpenLog(g_dir, &used);
    const std::wstring pidLog = g_dir + L"DcsQvCull." + std::to_wstring(GetCurrentProcessId()) + L".log";
    const bool fallback = other && mine && used == pidLog;
    if (mine) fclose(mine);
    if (other) fclose(other);
    FILE* normal = OpenLog(g_dir, &used);
    const bool primary = normal && used == log;
    if (normal) fclose(normal);
    DeleteFileW(log.c_str());
    DeleteFileW(pidLog.c_str());
    Check(kept && fallback && primary,
          "R62 loader: a log held by another DCS process -> DcsQvCull.<pid>.log instead of no log at all");
  }

  // R77 pid-log-files-accumulate: old DcsQvCull.<pid>.log files are pruned at start (the newest kept, a held one
  // kept, our own open one kept, a stale one under our pid removed; other names untouched).
  {
    auto mk = [&](const std::wstring& name, int ageMin) {
      const std::wstring p = g_dir + name;
      WriteText(p, "x\n");
      HANDLE f = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
      FILETIME ft;
      GetSystemTimeAsFileTime(&ft);
      ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
      u.QuadPart -= static_cast<ULONGLONG>(ageMin) * 60ull * 10000000ull;
      ft.dwLowDateTime = u.LowPart, ft.dwHighDateTime = u.HighPart;
      if (f != INVALID_HANDLE_VALUE) SetFileTime(f, nullptr, nullptr, &ft), CloseHandle(f);
      return p;
    };
    auto exists = [](const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
    const std::wstring a = mk(L"DcsQvCull.111.log", 30), b = mk(L"DcsQvCull.222.log", 20), c = mk(L"DcsQvCull.333.log", 10);
    const std::wstring held = mk(L"DcsQvCull.444.log", 40), prev = mk(L"DcsQvCull.prev.log", 50);
    const std::wstring stale = mk(L"DcsQvCull." + std::to_wstring(GetCurrentProcessId()) + L".log", 5);
    const std::wstring own = mk(L"DcsQvCull.555.log", 60);
    FILE* other = _wfsopen(held.c_str(), L"a", _SH_DENYWR);  // a DCS still running holds its fallback log
    const int n = PrunePidLogs(g_dir, own);
    if (other) fclose(other);
    const bool ok = n == 3 && !exists(a) && !exists(b) && exists(c) && exists(held) && exists(prev) && !exists(stale) &&
                    exists(own);
    for (const std::wstring& p : {a, b, c, held, prev, stale, own}) DeleteFileW(p.c_str());
    Check(ok, "R77 loader: old DcsQvCull.<pid>.log files are pruned at start (newest, held and own kept)");
  }

  // R62 a1-dev-path-maxpath: [Dev] PayloadPath read whole; too long / missing are told apart and reported once.
  {
    const std::wstring ini = g_dir + L"dev.ini";
    const std::wstring existing = g_dir + L"payload\\";  // a directory that exists
    std::wstring longPath = g_dir;
    while (longPath.size() < 300) longPath += L"deep_build_folder\\";
    longPath += L"DcsQvCullPayload.dll";
    WritePrivateProfileStringW(L"Dev", L"PayloadPath", longPath.c_str(), ini.c_str());
    const qvdev::DevPath tooLong = qvdev::ReadDevPath(L"PayloadPath", ini);
    WritePrivateProfileStringW(L"Dev", L"PayloadPath", (g_dir + L"nope.dll").c_str(), ini.c_str());
    const qvdev::DevPath missing = qvdev::ReadDevPath(L"PayloadPath", ini);
    WritePrivateProfileStringW(L"Dev", L"PayloadPath", existing.c_str(), ini.c_str());
    const qvdev::DevPath ok = qvdev::ReadDevPath(L"PayloadPath", ini);
    WritePrivateProfileStringW(L"Dev", L"PayloadPath", L"", ini.c_str());
    const qvdev::DevPath empty = qvdev::ReadDevPath(L"PayloadPath", ini);
    DeleteFileW(ini.c_str());
    Check(tooLong.state == qvdev::kDevTooLong && tooLong.path == longPath && missing.state == qvdev::kDevMissing &&
              ok.state == qvdev::kDevOk && empty.state == qvdev::kDevEmpty,
          "R62 dev path: a 260+ character path is read whole and refused as too long (was cut to 259, silently)");
    std::wstring last;
    const bool first = qvdev::ReportOnce(tooLong, &last), again = qvdev::ReportOnce(tooLong, &last);
    const bool other = qvdev::ReportOnce(missing, &last), cleared = !qvdev::ReportOnce(ok, &last) && last.empty();
    const bool back = qvdev::ReportOnce(missing, &last);
    Check(first && !again && other && cleared && back, "R62 dev path: an unusable path is logged once per value");
  }

  printf("%d failure(s)\n", g_fail);
  g_log = nullptr;
  return g_fail;
}
