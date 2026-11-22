// regress-07 (artifacts\regress-07\FINDINGS.md item 9): misleading log lines.
//  - The recorders' 10 s lines divided by max(frames, 1): with no Quad Views frame (main menu, loading, F10 map) they
//    printed totals as per-frame values. Now one line saying there was no frame.
//  - "gbuffer inst: ... selection code differs" did not say which of the five checks failed.
// (The DLSS load line's wording is checked in r130_test.h.)
#pragma once

namespace regress07test {

bool AnyHas(const std::vector<std::string>& lines, const char* what) {
  for (const std::string& l : lines)
    if (l.find(what) != std::string::npos) return true;
  return false;
}

void Run() {
  printf("-- regress-07: log lines --\n");
  {
    r93::LogFile lf;
    if (!lf.Open()) {
      Check(false, "regress-07: temporary log file");
      return;
    }
    gbrec::LogSummary("10 s", 0);
    gbrec::LogCounters("10 s", 0);
    shrec::LogSummary("10 s", 0);
    shrec::LogCounters("10 s", 0);
    r93::WaitWritten();
    const std::vector<std::string> lines = lf.Close();
    int none = 0;
    bool perFrame = false;
    for (const std::string& l : lines) {
      if (l.find("no Quad Views frame in this interval") != std::string::npos) ++none;
      if (l.find("/frame") != std::string::npos) perFrame = true;
    }
    char what[200];
    snprintf(what, sizeof(what),
             "recorders: 0 frames (menu) -> one 'no Quad Views frame' line per call, no per-frame figures (%d lines, "
             "%d of them so)",
             static_cast<int>(lines.size()), none);
    Check(none == 4 && !perFrame && AnyHas(lines, "gbuffer recorder 10 s") && AnyHas(lines, "shadow recorder 10 s"),
          what);
  }
  Check(shadowinst::SelFailText(0) == "none" &&
            shadowinst::SelFailText(shadowinst::kSelCodeBits | shadowinst::kSelDrawTable) ==
                "selection code hash, draw table hash" &&
            shadowinst::SelFailText(shadowinst::kSelSlot).find("vtable slot 4") != std::string::npos,
        "gbuffer inst: the selection check line names the failed checks");
}

}  // namespace regress07test
