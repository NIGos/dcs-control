// R59 xr-focus-4mb-tail / xr-state-reads-4mb-log-every-2s: the OpenXR session
// state is tracked from what dcs.log gains between polls; a state line older
// than 4 MB of log is still the state (it was "unknown" = not FOCUSED).
#pragma once

namespace r59xr {

void Run() {
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring path = std::wstring(tmp) + L"qv_r59_dcs_" + std::to_wstring(GetCurrentProcessId()) + L".log";
  auto append = [&](const std::string& s, const wchar_t* mode = L"ab") {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), mode) || !f) return;
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
  };
  // DCS's format (dcs.log 2026-10-17): "... XrEventDataSessionStateChanged: state A->B session=1 time=..."
  const std::string line = "2026-10-17 10:00:00.000 INFO    VISUALIZER (Main): [10:00:00.000][Info   ] OpenXR: "
                           "XrEventDataSessionStateChanged: state XR_SESSION_STATE_READY->";
  append("2026-10-17 09:59:59.000 INFO    EDCORE (Main): start\n", L"wb");
  append(line + "XR_SESSION_STATE_SYNCHRONIZED session=1 time=1\n" + line + "XR_SESSION_STATE_FOCUSED\r\n");
  XrStateTail t;
  const std::string s0 = t.Poll(path);
  // A long, log-heavy session: 5 MB more without a state change.
  std::string spam;
  for (int i = 0; i < 1000; ++i) spam += "2026-10-17 10:00:01.000 WARNING SCRIPTING (Main): spam spam spam spam spam spam\n";
  bool kept = true;
  for (int k = 0; k < 64 && kept; ++k) {
    append(spam);
    kept = t.Poll(path) == "XR_SESSION_STATE_FOCUSED";
  }
  // A fresh tracker (a payload reloaded mid-session) reads the whole log once.
  XrStateTail fresh;
  const std::string sFresh = fresh.Poll(path);
  // A change written in two pieces (a poll between them) is seen once complete.
  append(line + "XR_SESSION_STA");
  const std::string sHalf = t.Poll(path);
  append("TE_VISIBLE session=1 time=2\n");
  const std::string sVis = t.Poll(path);
  // A new (shorter) log: unknown until it names a state.
  append("2026-10-17 11:00:00.000 INFO    EDCORE (Main): start\n", L"wb");
  const std::string sNew = t.Poll(path);
  DeleteFileW(path.c_str());
  Check(s0 == "XR_SESSION_STATE_FOCUSED" && kept && sFresh == "XR_SESSION_STATE_FOCUSED" &&
            sHalf == "XR_SESSION_STATE_FOCUSED" && sVis == "XR_SESSION_STATE_VISIBLE" && sNew == "unknown" &&
            XrStateOfLine("x A->XR_SESSION_STATE_FOCUSED\r") == "XR_SESSION_STATE_FOCUSED" &&
            XrStateOfLine("x A->XR_SESSION_STATE_FOCUSED session=1") == "XR_SESSION_STATE_FOCUSED",
        "R59 xr state: FOCUSED stays FOCUSED after 5 MB of log with no state change; reads only the new bytes");
}

}  // namespace r59xr
