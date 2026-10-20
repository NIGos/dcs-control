// R62 governor-cap-line-locale-comma: log numbers keep '.' on comma-decimal regions (log_utf8.h).
#pragma once

namespace r62log {

void LogNumbers() {
  // governor-cap-line-locale-comma: the log locale converts %ls as UTF-8 but keeps '.' whatever the region.
  _locale_t de = qvlog::CreateLogLocale("de-DE.UTF-8");
  _locale_t deAll = _create_locale(LC_ALL, "de-DE.UTF-8");
  if (!de || !deAll) {
    printf("     (de-DE.UTF-8 locale not available: R62 log number check skipped)\n");
  } else {
    char a[128] = {}, b[128] = {};
#pragma warning(push)
#pragma warning(disable : 4996)
    _snprintf_l(a, sizeof(a) - 1, "governor: capped at %.1f fps (%ls)", de, 36.0, L"72 Hz / 2 \x00e9");
    _snprintf_l(b, sizeof(b) - 1, "%.1f", deAll, 36.0);
#pragma warning(pop)
    Check(strcmp(a, "governor: capped at 36.0 fps (72 Hz / 2 \xc3\xa9)") == 0 && strcmp(b, "36,0") == 0,
          "R62 log: a comma-decimal region (de-DE) still logs '36.0' (was '36,0'), %ls still UTF-8");
  }
  if (de) _free_locale(de);
  if (deAll) _free_locale(deAll);
  char c[64] = {};
  qvlog::Format(c, sizeof(c), "capped at %.1f fps", 36.0);
  Check(strcmp(c, "capped at 36.0 fps") == 0, "R62 log: the log's own locale formats 36.0 with '.'");
}

void Run() {
  LogNumbers();
}

}  // namespace r62log
