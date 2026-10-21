// R107: the log phrases DCS Control parses from our module log (DcsQvCull.log) and the prefetch log
// (DcsQvCullPrefetch.log) stay in the source byte-exact. The app's readers (dlss5vrframegen
// src\DcsVr.Core\EngineOptimizations.cs, LastFlight.cs) match these substrings; a reworded Log() call would
// silently blank a status in the app. This test greps the payload and loader sources (..\src next to the
// test binary) for the format strings that produce each phrase.
#pragma once

namespace lptest {

std::string ReadAllSources() {
  wchar_t p[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, p, MAX_PATH);
  std::wstring dir(p);
  dir = dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"..\\src\\";
  // DCSQV_SRC_DIR overrides the layout (DCS Control's build tree has test_main in <build>\test).
  wchar_t env[MAX_PATH] = {};
  const DWORD envLen = GetEnvironmentVariableW(L"DCSQV_SRC_DIR", env, MAX_PATH);
  if (envLen > 0 && envLen < MAX_PATH) {
    dir = env;
    if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
  }
  std::string all;
  for (const wchar_t* pat : {L"*.h", L"*.cpp", L"*.inc"}) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + pat).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      FILE* f = nullptr;
      if (_wfopen_s(&f, (dir + fd.cFileName).c_str(), L"rb") != 0 || !f) continue;
      char buf[65536];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
      all += '\n';
      fclose(f);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  // R107 split: main.cpp's parts live in src\main\ (same unity build).
  {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"main\\*.inc").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        FILE* f = nullptr;
        if (_wfopen_s(&f, (dir + L"main\\" + fd.cFileName).c_str(), L"rb") != 0 || !f) continue;
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
        all += '\n';
        fclose(f);
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
  }
  return all;
}

void Run() {
  const std::string src = ReadAllSources();
  if (src.size() < 100000) {
    Check(false, "log phrases: the sources next to the test binary (..\\src) were found");
    return;
  }
  // { the app's phrase, the format-string text in our source that writes it }
  struct Phrase {
    const char* app;
    const char* source;
  };
  const Phrase phrases[] = {
      {"loader: starting payload", "\"loader: starting payload %ls"},
      {"loader: payload start reported an error", "\"loader: payload start reported an error ("},
      {"loader: payload exports missing", "\"loader: payload exports missing\""},
      {"loader: payload source ", "\"loader: payload source %ls\""},
      {"config: dev mode / dev mode, settings from ", "\"config: dev mode, settings from %ls\""},
      {"stats:  (the last one: boost=off)", "Log(\"stats: %s%s boost=%s "},
      {"stats:  (the last one: timer=cache)", " timer=%s "},
      {"stats:  timer=cache", "timercache::g_on.load() ? \"cache\" : \"real\""},
      {"the D3D11 device lacks what the recorder needs",
       "\"the D3D11 device lacks what the recorder needs: DriverCommandLists=%d ConstantBufferOffsetting=%d \""},
      {"PREVENT_INTERNAL_THREADING_OPTIMIZATIONS", "created with PREVENT_INTERNAL_THREADING_OPTIMIZATIONS (check"},
      {"DriverCommandLists=0/1 (workers line)", "(DriverCommandLists=%d, CB mode %s"},
      {"vendor 0x (recorder adapter)", "(vendor 0x%04x, LUID"},
      {"vendor 0x (env line)", ", vendor 0x%04x), driver %s"},
      {"==== DcsQvCull test suite", "\"==== DcsQvCull test suite %04d-"},
      {"report written to report_", "\"suite: report written to %ls\""},
      {"report written to report_ (name)", "L\"report_%04d%02d%02d_%02d%02d%02d.txt\""},
      {"==== suite aborted", "\"==== suite aborted: "},
      {" perf: fps=", "Log(\"perf: fps=%.1f ft p50/p95="},
      {" perf: fps= ... verdict=", "verdict=%s"},
      {"governor: capped at", "\"governor: capped at %.1f fps (%d Hz / %d)"},
      {"prefetch log: calls/s=<n> skipped=<x>%", "\"calls/s=%llu skipped=%.1f%% pointers="},
      {"prefetch log: local date and time first", "\"%04u-%02u-%02u %02u:%02u:%02u %s\\r\\n\""},
  };
  int missing = 0;
  for (const Phrase& p : phrases)
    if (src.find(p.source) == std::string::npos) {
      ++missing;
      printf("     log phrase missing: app \"%s\" <- source %s\n", p.app, p.source);
    }
  Check(missing == 0,
        "R107 log phrases: every phrase DCS Control parses from DcsQvCull.log / DcsQvCullPrefetch.log is still in the "
        "source byte-exact (loader, config dev mode, stats boost/timer, recorder device line, suite markers, perf, "
        "governor cap, prefetch statistics)");
  // The app also accepts "the driver lacks command lists" (an older wording of the device line). Our source has not
  // written it since before R106; the app matches either. Reported, not required.
  if (src.find("the driver lacks command lists") == std::string::npos)
    printf("     (log phrases: the legacy \"the driver lacks command lists\" is not written; the app also matches the "
           "current \"the D3D11 device lacks what the recorder needs\")\n");
}

}  // namespace lptest
