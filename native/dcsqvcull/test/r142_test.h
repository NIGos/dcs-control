// R142 (docs/research/R142_live_reload.md): the ini reload diff (ini_diff.h), the engine key set, and the frame-time
// capture around a reload ([Log] ReloadFrameTimes). The kill-switch behaviour through LoadConfig itself is checked in
// test_main.cpp next to the hotkey tests ("R142 reload: ..."), where the timer cache and partition boost are live.
#pragma once

namespace r142test {

std::wstring TempIni(const wchar_t* tag) {
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  return std::wstring(tmp) + L"qv_r142_" + tag + L"_" + std::to_wstring(GetCurrentProcessId()) + L".ini";
}

void Parse() {
  const inidiff::Snapshot s = inidiff::ParseText(
      L"orphan=1\r\n; comment\r\n# also a comment\r\n[Color]\r\n  Mode = 2 \r\nmode=9\r\n[colorfix]\r\nExposureEV=\"0.5\"\r\n"
      L"[COLOR]\r\nPanelGamut=1\r\n[VrFx]\r\nnovalue\r\nGazeDof=\r\n");
  auto get = [&](const wchar_t* sec, const wchar_t* key, const wchar_t* want) {
    const auto it = s.keys.find(inidiff::MapKey(sec, key));
    return it != s.keys.end() && it->second.val == want;
  };
  Check(s.ok && s.keys.size() == 3 && get(L"color", L"MODE", L"2") && get(L"ColorFix", L"ExposureEV", L"0.5") &&
            get(L"VrFx", L"GazeDof", L"") && s.keys.find(inidiff::MapKey(L"Color", L"PanelGamut")) == s.keys.end(),
        "R142 ini parse: as GetPrivateProfile* reads it (case-insensitive, first key and first section of a name win, "
        "values trimmed and unquoted, comments and keys before any section ignored, an empty value kept)");
  // The file as DCS Control writes it (UTF-16 LE with a BOM), and through the profile API itself.
  const std::wstring ini = TempIni(L"parse");
  DeleteFileW(ini.c_str());
  {
    FILE* f = nullptr;
    _wfopen_s(&f, ini.c_str(), L"wb");
    if (f) {
      const wchar_t text[] = L"\xFEFF[ColorEnhanced]\r\nGlare=1\r\nGlareStrength=0.7\r\n[Timing]\r\nCacheUs=900\r\n";
      fwrite(text, sizeof(wchar_t), wcslen(text), f);
      fclose(f);
    }
  }
  const inidiff::Snapshot u = inidiff::ReadFile(ini.c_str());
  const bool apiAgrees = GetPrivateProfileIntW(L"Timing", L"CacheUs", 0, ini.c_str()) == 900;
  WritePrivateProfileStringW(L"VrFx", L"GazeDof", L"1", ini.c_str());
  const inidiff::Snapshot u2 = inidiff::ReadFile(ini.c_str());
  DeleteFileW(ini.c_str());
  const inidiff::Snapshot missing = inidiff::ReadFile(ini.c_str());
  Check(u.ok && u.keys.size() == 3 && apiAgrees && u2.ok && u2.keys.size() == 4 && missing.ok && missing.keys.empty(),
        "R142 ini read: a UTF-16 LE file with a BOM (DCS Control's encoding) parsed, a key added by "
        "WritePrivateProfileStringW seen, a missing file = an empty snapshot (every key at its default)");
}

void Diffs() {
  const inidiff::Snapshot a = inidiff::ParseText(L"[Color]\nMode=1\n[Timing]\nCacheUs=1000\n[Prefetch]\nMode=Skip\n"
                                                 L"[VrFx]\nGVision=1\n");
  const inidiff::Snapshot same = inidiff::ParseText(L"; rewritten\n[timing]\ncacheus=1000\n[Color]\nMode=1\n"
                                                    L"[Prefetch]\nMode=skip\n[VrFx]\nGVision=1\n");
  const std::vector<inidiff::Change> none = inidiff::Diff(a, same);
  const inidiff::Snapshot vis = inidiff::ParseText(L"[Color]\nMode=2\n[Timing]\nCacheUs=1000\n[Prefetch]\nMode=skip\n"
                                                   L"[ColorEnhanced]\nGlare=1\n");
  const std::vector<inidiff::Change> v = inidiff::Diff(a, vis);
  const inidiff::Snapshot eng = inidiff::ParseText(L"[Color]\nMode=1\n[Timing]\nCacheUs=900\n[Prefetch]\nMode=skip\n"
                                                   L"[VrFx]\nGVision=1\n");
  const std::vector<inidiff::Change> e = inidiff::Diff(a, eng);
  const std::vector<std::string> fv = inidiff::FormatChanges(v), fn = inidiff::FormatChanges(none);
  Check(none.empty() && fn.size() == 1 && fn[0] == "none" && v.size() == 3 && !inidiff::AnyEngine(v) && e.size() == 1 &&
            inidiff::AnyEngine(e) && fv.size() == 1 &&
            fv[0] == "Color.Mode 1->2, ColorEnhanced.Glare (none)->1, VrFx.GVision 1->(none)",
        "R142 ini diff: a reordered / re-cased rewrite with the same values changes nothing; a visual change lists "
        "changed, added and removed keys with no engine key; [Timing] CacheUs 1000->900 is one engine change");
  std::vector<inidiff::Change> many;
  for (int i = 0; i < 200; ++i) {
    inidiff::Change c;
    c.sec = L"Suite";
    c.key = L"Key" + std::to_wstring(i);
    c.newVal = L"1";
    c.hasNew = true;
    many.push_back(c);
  }
  const std::vector<std::string> fm = inidiff::FormatChanges(many, 1500);
  bool fits = fm.size() >= 2;
  for (const std::string& l : fm) fits = fits && l.size() <= 1500;
  Check(fits, "R142 ini diff: a long change list is split into lines of at most 1500 characters (the log line is 2 KB)");
}

// Every key DCS Control's template writes (iktest::kAppKeys) has a class, the visual and perception sections are not
// engine keys, and every section the payload reads is classified.
void Classes() {
  int unknown = 0, wrongVisual = 0, wrongEngine = 0, engine = 0, other = 0;
  for (const iktest::AppKey& k : iktest::kAppKeys) {
    const inidiff::Class c = inidiff::Classify(k.sec, k.key);
    if (c == inidiff::kUnknown) {
      ++unknown;
      printf("     app key [%ls] %ls has no class\n", k.sec, k.key);
      continue;
    }
    (c == inidiff::kEngine ? engine : other) += 1;
    const std::wstring s = inidiff::Lower(k.sec);
    const bool visual = inidiff::In(s, {L"color", L"colorfix", L"colorenhanced", L"night", L"vrfx", L"dlss",
                                        L"perception", L"hotkeys", L"dev", L"log"}) ||
                        (s == L"general" && (inidiff::Lower(k.key) == L"perceptiononly" ||
                                             inidiff::Lower(k.key) == L"diagnostics" ||
                                             inidiff::Lower(k.key) == L"beeps" || inidiff::Lower(k.key) == L"sigscan"));
    if (visual && c == inidiff::kEngine) {
      ++wrongVisual;
      printf("     app key [%ls] %ls is visual / perception but classed as an engine key\n", k.sec, k.key);
    }
    const bool eng = inidiff::In(s, {L"timing", L"scene", L"model", L"texture", L"effects", L"d3d", L"shadows",
                                     L"prefetch", L"suite", L"bench", L"cull", L"governor", L"threads", L"shadow"});
    if (eng && c != inidiff::kEngine) {
      ++wrongEngine;
      printf("     app key [%ls] %ls is an optimization key but not classed as an engine key\n", k.sec, k.key);
    }
  }
  printf("     app template keys: %d engine, %d not engine\n", engine, other);
  Check(unknown == 0 && wrongVisual == 0 && wrongEngine == 0,
        "R142 engine key set: every key DCS Control's template writes is classified; [Color] [ColorFix] "
        "[ColorEnhanced] [Night] [VrFx] [DLSS] [Perception] [Hotkeys] [Dev] [Log] and [General] PerceptionOnly / "
        "Diagnostics / Beeps / SigScan are not engine keys; the optimization sections are");
  // Every section literal the payload reads (GetPrivateProfile*W / ReadDouble / qvini::Mask / clampInt) is classified.
  const std::string src = lptest::ReadAllSources();
  std::set<std::string> sections;
  for (const char* pat : {"ProfileIntW(L\"", "ProfileStringW(L\"", "ReadDouble(L\"", "qvini::Mask(L\"", "clampInt(L\""}) {
    for (size_t p = src.find(pat); p != std::string::npos; p = src.find(pat, p + 1)) {
      const size_t a = p + strlen(pat), e = src.find('"', a);
      if (e != std::string::npos && e - a < 40) sections.insert(src.substr(a, e - a));
    }
  }
  int unclassified = 0;
  for (const std::string& s : sections) {
    const std::wstring w(s.begin(), s.end());
    if (inidiff::Classify(w, L"x") == inidiff::kUnknown) {
      ++unclassified;
      printf("     section [%s] read by the payload has no class in ini_diff.h\n", s.c_str());
    }
  }
  printf("     sections read by the payload: %zu\n", sections.size());
  Check(src.size() > 100000 && sections.size() >= 20 && unclassified == 0,
        "R142 engine key set: every ini section the payload reads is classified (engine or not)");
}

void FrameTimes() {
  // Pure windows: 90 fps (11.1 ms) for 20 s before; after the reload one 40 ms frame, then 11.1 ms again.
  std::vector<float> before(1800, 11.1f), after(300, 11.1f);
  after[5] = 40.0f;
  before[1795] = 25.0f;  // inside the 2 s before
  const reloadft::Result r = reloadft::Compute(before, after);
  Check(r.base.n >= 895 && r.base.n <= 905 && std::fabs(r.base.p99 - 11.1) < 1e-3 && r.pre.n >= 175 &&
            r.pre.n <= 181 && std::fabs(r.pre.max - 25.0) < 1e-3 && r.post.n >= 175 && r.post.n <= 181 &&
            std::fabs(r.post.max - 40.0) < 1e-3 && std::fabs(r.post.p50 - 11.1) < 1e-3,
        "R142 reload frame times: baseline 12..2 s before, the 2 s before and the 2 s after a reload by summed frame "
        "time (p50 / p99 / max; a 40 ms frame after the reload is its max)");
  // Through the payload: a capture armed at a reload, written when forced (the CSV next to the log).
  const std::wstring dir0 = g_dir;
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  g_dir = std::wstring(tmp) + L"qv_r142_ft_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
  CreateDirectoryW(g_dir.c_str(), nullptr);
  auto push = [](float ms) {
    const uint64_t n = perfgov::g_frames.load();
    perfgov::g_ring[n % perfgov::kRing] = ms;
    perfgov::g_frames.store(n + 1);
  };
  for (int i = 0; i < 1200; ++i) push(11.1f);
  inidiff::Change c;
  c.sec = L"Color";
  c.key = L"Mode";
  c.oldVal = L"0";
  c.newVal = L"1";
  c.hadOld = c.hasNew = true;
  const uint64_t w0 = g_reloadFtWritten.load();
  ArmReloadFrameTimes("12:00:00.000", {c});
  TickReloadFrameTimes();  // not yet: 2.5 s have not passed
  const bool notYet = g_reloadFtWritten.load() == w0;
  for (int i = 0; i < 200; ++i) push(i == 3 ? 33.0f : 11.1f);
  TickReloadFrameTimes(true);
  int csv = 0, rows = 0;
  {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_dir + L"reload_frametimes_*.csv").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        ++csv;
        FILE* f = nullptr;
        if (_wfopen_s(&f, (g_dir + fd.cFileName).c_str(), L"rb") == 0 && f) {
          char line[128];
          while (fgets(line, sizeof(line), f)) ++rows;
          fclose(f);
        }
        DeleteFileW((g_dir + fd.cFileName).c_str());
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
  }
  RemoveDirectoryW(g_dir.c_str());
  g_dir = dir0;
  // 12 s before at 11.1 ms (1081 frames, the 1200 pushed cover 13.3 s) + 2 s after (181 frames) + the header.
  Check(notYet && g_reloadFtWritten.load() == w0 + 1 && csv == 1 && rows > 1200 && rows < 1300,
        "R142 reload frame times: armed by a reload, written after the 2 s after it (one line, one CSV of the frames "
        "12 s before to 2 s after)");
}

void Run() {
  Parse();
  Diffs();
  Classes();
  FrameTimes();
}

}  // namespace r142test
