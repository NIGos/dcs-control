DWORD WINAPI ProfileThread(void*) {
  char label[64];
  snprintf(label, sizeof(label), "optimisation %s, keep %.2f", g_enabled.load() ? "ON" : "OFF",
           EffectiveKeep());
  Chime(1000, 60);
  prof::Run(std::max(2, g_cfg.profileSeconds), std::max(1, g_cfg.profileThreads),
            std::max(1, g_cfg.profilePeriodMs), label);
  Chime(1000, 60);
  Chime(1000, 60);
  return 0;
}

// Last occurrence of `marker` in a text file (read with full sharing), with
// the rest of that line.
std::string LastLineWith(const std::wstring& path, const char* marker) {
  FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
  if (!f) return {};
  std::string text;
  char buf[65536];
  size_t r;
  if (_fseeki64(f, 0, SEEK_END) == 0) {
    long long size = _ftelli64(f);
    _fseeki64(f, size > 4 * 1024 * 1024 ? size - 4 * 1024 * 1024 : 0, SEEK_SET);
  }
  while ((r = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, r);
  fclose(f);
  size_t p = text.rfind(marker);
  if (p == std::string::npos) return {};
  size_t b = text.rfind('\n', p);
  size_t e = text.find('\n', p);
  return text.substr(b == std::string::npos ? 0 : b + 1, (e == std::string::npos ? text.size() : e) - (b == std::string::npos ? 0 : b + 1));
}

// The state a session-state line of dcs.log names ("... -> XR_SESSION_STATE_X ..."); empty when none.
std::string XrStateOfLine(const std::string& st) {
  size_t a = st.rfind("->");
  if (a == std::string::npos) return {};
  size_t e = st.find_first_of(" \r", a + 2);
  return st.substr(a + 2, (e == std::string::npos ? st.size() : e) - a - 2);
}

// The last OpenXR session state DCS logged. Each poll reads only what the log
// gained since the previous one (all of it at the first poll, again after it
// shrank: a new log), and a state found is kept until a newer line names
// another. Before, every poll (each 2 s, the whole session) re-read the last
// 4 MB of dcs.log, and once more than 4 MB followed the last state change
// (which DCS logs only on transitions) the state read as "unknown", so a
// worn, FOCUSED headset counted as a focus loss (suite waits, bench blocks
// discarded).
struct XrStateTail {
  long long offset = 0;
  std::string partial;  // the last line read, not finished yet
  std::string state = "unknown";
  std::string Poll(const std::wstring& path) {
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return state;
    if (_fseeki64(f, 0, SEEK_END) == 0) {
      const long long size = _ftelli64(f);
      if (size < offset) {  // truncated or replaced
        offset = 0;
        partial.clear();
        state = "unknown";
      }
    }
    _fseeki64(f, offset, SEEK_SET);
    static const char kMarker[] = "XrEventDataSessionStateChanged";
    char buf[65536];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
      offset += static_cast<long long>(r);
      size_t from = 0;
      for (size_t i = 0; i < r; ++i) {
        if (buf[i] != '\n') continue;
        partial.append(buf + from, i - from);
        if (partial.find(kMarker) != std::string::npos) {
          std::string s = XrStateOfLine(partial);
          if (!s.empty()) state = s;
        }
        partial.clear();
        from = i + 1;
      }
      if (partial.size() < 4096) partial.append(buf + from, std::min<size_t>(r - from, 4096));  // no line is longer
    }
    fclose(f);
    return state;
  }
};

std::string XrSessionState() {
  static std::mutex m;  // the worker and the suite's report both ask
  static XrStateTail tail;
  std::lock_guard<std::mutex> lock(m);
  return tail.Poll(g_dir + L"..\\..\\Logs\\dcs.log");
}

// Blocks until the session is FOCUSED (headset awake), up to 60 minutes.
bool WaitForFocus() {
  if (g_xrFocused.load()) return true;
  Log("  session not FOCUSED (headset asleep?): move the headset, the test waits");
  for (int i = 0; i < 1800 && !g_benchAbort.load() && !g_stop.load(); ++i) {
    if (i % 15 == 0) {
      Chime(400, 150);
      Chime(400, 150);
    }
    Sleep(2000);
    if (g_xrFocused.load()) {
      Log("  session FOCUSED again, continuing in 3 s");
      Sleep(3000);
      return true;
    }
  }
  return false;
}

// Folder of the process image (DCS.exe in DCS's bin folder), with trailing
// backslash; empty when it cannot be read. Long paths are handled (the
// buffer grows while GetModuleFileNameW truncates).
std::wstring ProcessImageDir() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, &path[0], static_cast<DWORD>(path.size()));
    if (n == 0) return std::wstring();
    if (n < path.size()) {
      path.resize(n);
      break;
    }
    if (path.size() >= 32768) return std::wstring();
    path.resize(path.size() * 2);
  }
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
}

// One-key test suite: configuration check, CPU profile, exclusion A/B and
// collect-threads A/B, summarised in one report file.
void CheckConfiguration() {
  struct Mod {
    const wchar_t* file;
    const char* what;
    bool wanted;
  };
  const Mod mods[] = {
      {L"XR_APILAYER_MBUCCHIA_quad_views_foveated.dll", "Quad-Views-Foveated layer", true},
      // The old loader-chain prefetch fix; replaced by [Prefetch] (prefetch_fix.h), which takes over its slots.
      {L"dxgi2.dll", "old prefetch fix (bin/dxgi2.dll, now in the payload)", false},
      {L"PiOpenXR_64.dll", "Pimax OpenXR runtime", true},
      {L"CheekyOpenXRLayer.dll", "Cheeky OpenXR layer (absent in base)", false},
      {L"XR_APILAYER_XRFrameBridge_diagnostic.dll", "OFXR frame bridge / frame-gen (absent in base)", false},
  };
  Log("-- configuration --");
  // R30 L8: name the runtime; Pimax and QVFR are only expected on a Pimax runtime (no false warnings for others).
  const std::string runtime = envinfo::XrRuntime();
  const bool pimax = runtime.find("Pimax") != std::string::npos || runtime.find("PiOpenXR") != std::string::npos ||
                     GetModuleHandleW(L"PiOpenXR_64.dll") != nullptr;
  Log("  OpenXR runtime: %s; view layout: %d views per frame, %d focus/peripheral pairs", runtime.c_str(),
      perfgov::g_views.load(), perfgov::g_pairs.load());
  for (const Mod& m : mods) {
    bool present = GetModuleHandleW(m.file) != nullptr;
    const bool pimaxOnly = wcscmp(m.file, L"PiOpenXR_64.dll") == 0 ||
                           wcscmp(m.file, L"XR_APILAYER_MBUCCHIA_quad_views_foveated.dll") == 0;
    const bool check = pimaxOnly && !pimax ? false : present != m.wanted;
    Log("  %-50s %s%s", m.what, present ? "loaded" : "not loaded", check ? "   <-- check" : "");
  }
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  // The prefetch fix logs next to DCS.exe: DCS's bin folder from the process
  // image (was a hard-coded install path of one machine).
  const std::wstring binDir = ProcessImageDir();
  if (!binDir.empty() &&
      GetFileAttributesExW((binDir + L"DcsVrPrefetchFix.log").c_str(), GetFileExInfoStandard, &fad)) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = now.dwLowDateTime;
    a.HighPart = now.dwHighDateTime;
    b.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    b.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    double age = (a.QuadPart - b.QuadPart) / 1e7;
    Log("  prefetch fix log last written %.0f s ago%s", age, age > 60 ? "   <-- fix not active?" : "");
  }
  Log("  payload prefetch fix: %s, mode %s, pointers=%u imports=%u takeovers=%llu",
      pffix::g_state.load() == 1 ? "installed" : pffix::g_state.load() < 0 ? "unavailable" : "not installed",
      pffix::g_mode.load() == pffix::kSkip ? "skip" : pffix::g_mode.load() == pffix::kObserve ? "observe" : "off",
      pffix::g_livePointers.load(), pffix::g_liveImports.load(),
      static_cast<unsigned long long>(pffix::g_takeovers.load()));
  Log("  keep fraction %.2f, QVFR edge smoothing %.2f", EffectiveKeep(), g_qvSmoothing.load());
  // OpenXR session state (DCS log) and headset screen state (Pimax runtime log):
  // with the headset still for longer than Pimax's "Headset Sleep" time the
  // screen turns off, the session drops to VISIBLE and DCS slows down.
  {
    std::string state = XrSessionState();
    bool focused = state == "XR_SESSION_STATE_FOCUSED";
    Log("  OpenXR session state: %s%s", state.c_str(), focused ? "" : "   <-- not FOCUSED: DCS throttles, results invalid");
    wchar_t lad[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH)) {
      std::wstring dir = std::wstring(lad) + L"\\Pimax\\runtime\\";
      WIN32_FIND_DATAW fd;
      HANDLE h = FindFirstFileW((dir + L"pvr_srv_log_*.txt").c_str(), &fd);
      std::wstring newest;
      FILETIME best{};
      if (h != INVALID_HANDLE_VALUE) {
        do {
          if (CompareFileTime(&fd.ftLastWriteTime, &best) > 0) {
            best = fd.ftLastWriteTime;
            newest = fd.cFileName;
          }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
      }
      if (!newest.empty()) {
        std::string scr = LastLineWith(dir + newest, "setScreenState: ");
        bool on = !scr.empty() && scr.back() != '0' && scr.find("setScreenState: 0") == std::string::npos;
        Log("  headset screen: %s%s", scr.empty() ? "unknown" : (on ? "on" : "OFF"),
            on || scr.empty() ? "" : "   <-- screen off (Pimax Headset Sleep): move the headset");
      }
    }
  }
  Log("  GPU utilisation now: %d%%", gpu::Init() ? gpu::Utilization() : -1);
  if (void* sc = g_scene.load()) {
    uint32_t tmax = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(sc) + kSceneCollectThreadsMax);
    uint32_t f = g_lastCollectFlags.load();
    uint32_t m = std::min(tmax, f);
    uint32_t tasks = std::min<uint32_t>((m + 1) * (tmax > 1 ? 2 : 4), 16);
    Log("  culling partition: threadsMax %u, collect arg %u -> up to %u parse tasks per collect", tmax, f, tasks);
  }
}

