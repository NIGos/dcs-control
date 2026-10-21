// R107 (R106 F12): the ini readers that were copied around LoadConfig (seven "GetPrivateProfileString + wcstoul"
// bit masks) and the two double readers that disagreed on a bad value (main.cpp's ReadDouble: _wtof, so "abc" or an
// empty value read as 0; color_grade.h's IniD: the default). One rule now:
//   Double  missing, empty or unparsable -> the default (*bad set when the text was there but not a number; the
//           caller logs it). A number followed by junk ("1.5x") reads its number, as wcstod / _wtof did.
//   Int     a Double truncated toward zero (1.9 -> 1, -0.5 -> 0, as static_cast<int>); not finite or outside int's
//           range (nan, inf, 1e10) -> the default (R115 G4: the plain cast of such a value is undefined).
//   Mask    decimal or 0x hex, ANDed with the mask. Missing -> the default; empty or junk -> 0, as before (a key
//           written empty switches every bit off; that is how these keys have always behaved).
// Pure: <windows.h> only, no logging (main.cpp's Log comes later in the unity build).
#pragma once

#include <windows.h>

#include <cstdint>
#include <cwchar>

namespace qvini {

inline double ParseDouble(const wchar_t* text, double dflt, bool* bad = nullptr) {
  if (bad) *bad = false;
  if (!text || !text[0]) return dflt;
  wchar_t* end = nullptr;
  const double v = wcstod(text, &end);
  if (end == text) {
    if (bad) *bad = true;
    return dflt;
  }
  return v;
}

inline double Double(const wchar_t* sec, const wchar_t* key, double dflt, const wchar_t* ini, bool* bad = nullptr) {
  wchar_t b[64] = {};
  GetPrivateProfileStringW(sec, key, L"", b, 64, ini);
  return ParseDouble(b, dflt, bad);
}

// R115 G4: v as an int, or dflt when v is not finite or does not fit (NaN fails both comparisons).
inline int ToInt(double v, int dflt) {
  return v > -2147483649.0 && v < 2147483648.0 ? static_cast<int>(v) : dflt;
}

inline uint32_t Mask(const wchar_t* sec, const wchar_t* key, uint32_t dflt, uint32_t mask, const wchar_t* ini) {
  wchar_t d[16];
  swprintf_s(d, L"0x%x", dflt);
  wchar_t b[32] = {};
  GetPrivateProfileStringW(sec, key, d, b, 32, ini);
  return static_cast<uint32_t>(wcstoul(b, nullptr, 0)) & mask;
}

}  // namespace qvini
