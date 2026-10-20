// Log-line formatting with %ls arguments written as UTF-8 (loader and payload).
//
// vsnprintf converts %ls through the CRT's "C" locale, which cannot represent
// characters above U+00FF: the call fails with -1 and the whole log line comes
// out empty. Paths with a Cyrillic or CJK user name (Saved Games, the payload
// copy, the dev ini) were lost from DcsQvCull.log that way, and U+0080-U+00FF
// came out as Latin-1 bytes. FormatV formats with a UTF-8 locale instead and
// always NUL-terminates (long lines are truncated, as before).
#pragma once

#include <locale.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>

namespace qvlog {

// Created once per module and never freed (process lifetime). Null where the
// CRT has no UTF-8 locale (before Windows 10 1803): the current locale is used.
// R62: LC_CTYPE only. A ".UTF-8" locale for LC_ALL takes the user's region for
// the other categories, so on a comma-decimal Windows region (de-DE, fr-FR,
// ru-RU) every %f in DcsQvCull.log came out as "36,0" and the app's parsers
// (governor "capped at N fps") missed the line. The numbers stay in the "C"
// locale ('.'), as the lines built with plain snprintf already were.
inline _locale_t CreateLogLocale(const char* name) { return _create_locale(LC_CTYPE, name); }
inline _locale_t Utf8Locale() {
  static const _locale_t loc = CreateLogLocale(".UTF-8");
  return loc;
}

inline void FormatV(char* buf, size_t n, const char* fmt, va_list ap) {
  if (!buf || n == 0) return;
  buf[0] = 0;
  // _vsnprintf_l does not terminate a truncated line and may leave a partial
  // one on a conversion error (an unpaired surrogate): terminate it here.
#pragma warning(push)
#pragma warning(disable : 4996)  // _vsnprintf_l: terminated below
  const int r = _vsnprintf_l(buf, n, fmt, Utf8Locale(), ap);
#pragma warning(pop)
  if (r < 0 || static_cast<size_t>(r) >= n) buf[n - 1] = 0;
}

inline void Format(char* buf, size_t n, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  FormatV(buf, n, fmt, ap);
  va_end(ap);
}

}  // namespace qvlog
