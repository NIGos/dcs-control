// R62 gbuffer-quad-filter-false-warning: the recorder's fallback lines against the app's "turned itself off" scan.
#pragma once

namespace r62gbl {

// EngineOptimizations.cs Read(): the "<name> turned itself off" condition for one line of a prefix (app copy,
// DCS VR Control 2026-10; keep in step with the app).
bool AppWouldWarn(const std::string& l, const char* prefix) {
  auto has = [&](const char* t) { return l.find(t) != std::string::npos; };
  auto ends = [&](const char* t) {
    const size_t n = strlen(t);
    return l.size() >= n && l.compare(l.size() - n, n, t) == 0;
  };
  std::string lower = l;
  for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  if (!has(prefix) || has("not recordable") || has("texture classes the table refused") ||
      has("the driver lacks command lists") || has("the D3D11 device lacks what the recorder needs"))
    return false;
  return has("does not match") || has("unexpected bytes") || has("unknown patch") || has("could not") ||
         has("FAILED") || has("disabled for this session") || has("off for this session") || has("NOT SAFE") ||
         has("latched") || (lower.find("exception") != std::string::npos && has("contained")) || has("DISABLED") ||
         ends("; unavailable") || has("; jobs start at RenderGraph::render entry") || ends("; off") ||
         ends("; skipped") || (has("not installed") && !has("counters not installed"));
}

void GbRecLines() {
  // gbuffer-quad-filter-false-warning: the recorder keeps running after either fallback; the app must not read
  // "turned itself off" from them.
  char id[512];
  snprintf(id, sizeof(id), gbrec::kIdentityDroppedFmt, "slot 2 (viewport tag 0x10, rank 1) saw no execution for 5 s");
  Check(!AppWouldWarn(gbrec::kQuadFilterDroppedLine, "gbuffer recorder") && !AppWouldWarn(id, "gbuffer recorder") &&
            AppWouldWarn("gbuffer recorder: disabled for this session (x)", "gbuffer recorder") &&
            AppWouldWarn("gbuffer recorder: quad-view filter off for this session (30 frames)", "gbuffer recorder"),
        "R62 gbuffer recorder: the quad-view filter / execution identity fallback lines are not read as "
        "'turned itself off' by the app (the old wording was)");
}

void Run() {
  GbRecLines();
}

}  // namespace r62gbl
