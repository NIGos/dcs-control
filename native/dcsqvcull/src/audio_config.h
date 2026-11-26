// Binaural audio: the [Audio] section of DcsQvCull.ini (names, defaults, ranges: native/binaural/include/
// binaural_config.h) and [Hotkeys] AudioToggle. Read by the audio module's control thread at start and at every change
// of the ini (Live: every key applies at the next device block; Hrtf after the new set is loaded). [Audio] is not an
// engine section (ini_diff.h): a Live write of it never touches the engine kill switch, and the engine kill switch
// never affects audio. Out-of-range values are clamped and named once in the log.
//
// The key names are spelled out below (not looped over binaural_config::kKeys) so the offline check of the keys the app
// writes (test/ini_keys_test.h) finds each one read; a test compares this reader with kKeys.
#pragma once

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

#include "binaural_config.h"
#include "cockpit_space.h"  // the cockpit space's families (native/binaural, or the module's stand-in until CORE is merged)

namespace audcfg {

using binaural_config::AudioConfig;

// Cockpit space (docs/COCKPIT_SPACE.md 7): [Audio] CockpitSpace 0..3 (Off, Subtle, Normal, Strong; default 1),
// CockpitSpaceFamily auto | a family key (default auto), CockpitSpaceCanopy 0/1 (default 1). All Live. The keys belong
// to binaural_config.h's table (CORE adds them there); until that is merged their ranges are spelt out here.
struct SpaceConfig {
  int level = 1;
  bool familyAuto = true;
  cockpit_space::Family family = cockpit_space::Family::Generic;  // with familyAuto false
  bool canopy = true;
};
constexpr int kSpaceLevelDefault = 1, kSpaceLevelMax = 3;
inline bool SameSpace(const SpaceConfig& a, const SpaceConfig& b) {
  return a.level == b.level && a.familyAuto == b.familyAuto && a.family == b.family && a.canopy == b.canopy;
}

struct Read {
  AudioConfig cfg;
  SpaceConfig space;
  int toggleVk = 0, toggleMods = 0;  // [Hotkeys] AudioToggle (default none)
  char notes[4][160] = {};           // clamped / invalid values (the control thread logs each once)
  int noteCount = 0;
};

inline void Note(Read& r, const char* fmt, ...) {
  if (r.noteCount >= 4) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(r.notes[r.noteCount++], sizeof(r.notes[0]), fmt, ap);
  va_end(ap);
}

inline const binaural_config::KeyInfo& Key(const wchar_t* name) {
  for (const binaural_config::KeyInfo& k : binaural_config::kKeys)
    if (!_wcsicmp(k.name, name)) return k;
  return binaural_config::kKeys[0];
}

// A number, its default when absent or not a number, clamped to the table's range (noted when it was out of range).
inline double Number(Read& r, const wchar_t* ini, const wchar_t* name) {
  const binaural_config::KeyInfo& k = Key(name);
  wchar_t buf[64] = {};
  GetPrivateProfileStringW(binaural_config::kSection, name, L"", buf, 64, ini);
  if (!buf[0]) return k.defaultValue;
  wchar_t* end = nullptr;
  const double v = wcstod(buf, &end);
  while (end && (*end == L' ' || *end == L'\t')) ++end;
  if (end == buf || (end && *end) || !std::isfinite(v)) {
    Note(r, "audio: config: %ls=%ls is not a number, %g", name, buf, k.defaultValue);
    return k.defaultValue;
  }
  const double c = binaural_config::Clamp(v, k.minValue, k.maxValue);
  if (c != v) Note(r, "audio: config: %ls=%ls out of range, %g", name, buf, c);
  return c;
}

inline bool ParseToggle(const wchar_t* text, int* vk, int* mods) {
  *vk = *mods = 0;
  wchar_t* end = nullptr;
  const long k = wcstol(text, &end, 10);
  if (end == text || *end != L':') return false;
  const wchar_t* m0 = end + 1;
  const long m = wcstol(m0, &end, 10);
  if (end == m0) return false;
  while (*end == L' ' || *end == L'\t') ++end;
  if (*end) return false;
  if (k == 0 && m == 0) return true;
  if (k < 1 || k > 254 || m < 0 || m > 7) return false;
  *vk = static_cast<int>(k);
  *mods = static_cast<int>(m);
  return true;
}

inline Read ReadIni(const wchar_t* ini) {
  Read r;
  AudioConfig& c = r.cfg;
  c.binaural = Number(r, ini, L"Binaural") != 0;
  {
    wchar_t buf[128] = {};
    GetPrivateProfileStringW(binaural_config::kSection, L"Hrtf", binaural_config::kDefaultHrtfName, buf, 128, ini);
    if (!buf[0]) wcscpy_s(buf, binaural_config::kDefaultHrtfName);
    if (binaural_config::IsValidHrtfName(buf)) {
      wcsncpy_s(c.hrtf, buf, _TRUNCATE);
    } else {
      // Never the value itself: the pilot names the sets, often after a person (the log reaches diagnostic packages).
      Note(r, "audio: config: Hrtf is not a valid name (%zu characters), default", wcslen(buf));
      wcscpy_s(c.hrtf, binaural_config::kDefaultHrtfName);
    }
  }
  c.headCircumferenceCm = static_cast<float>(Number(r, ini, L"HeadCircumferenceCm"));
  c.lateHeadTracking = Number(r, ini, L"LateHeadTracking") != 0;
  c.nearField = Number(r, ini, L"NearField") != 0;
  c.maxObjects = static_cast<int>(std::lround(Number(r, ini, L"MaxObjects")));
  c.levelTrimDb = static_cast<float>(Number(r, ini, L"LevelTrimDb"));
  c.cpuBudgetPercent = static_cast<int>(std::lround(Number(r, ini, L"CpuBudgetPercent")));
  {
    wchar_t buf[128] = {};
    GetPrivateProfileStringW(binaural_config::kSection, L"ExcludeContexts", L"", buf, 128, ini);
    uint32_t mask = 0;
    if (!binaural_config::ParseExcludeContexts(buf, &mask)) {
      Note(r, "audio: config: ExcludeContexts=%ls is not a list of context ids 0-7, none", buf);
      mask = 0;
    }
    c.excludeContextMask = mask;
  }
  c.logVoices = Number(r, ini, L"LogVoices") != 0;
  {  // cockpit space
    SpaceConfig& s = r.space;
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(binaural_config::kSection, L"CockpitSpace", L"", buf, 64, ini);
    if (buf[0]) {
      wchar_t* end = nullptr;
      const double v = wcstod(buf, &end);
      while (end && (*end == L' ' || *end == L'\t')) ++end;
      if (end == buf || (end && *end) || !std::isfinite(v)) {
        Note(r, "audio: config: CockpitSpace=%ls is not a number, %d", buf, kSpaceLevelDefault);
      } else {
        const long l = std::lround(binaural_config::Clamp(v, 0, kSpaceLevelMax));
        if (static_cast<double>(l) != v) Note(r, "audio: config: CockpitSpace=%ls out of range, %ld", buf, l);
        s.level = static_cast<int>(l);
      }
    }
    GetPrivateProfileStringW(binaural_config::kSection, L"CockpitSpaceFamily", L"auto", buf, 64, ini);
    bool isAuto = true;
    cockpit_space::Family f = cockpit_space::Family::Generic;
    if (!buf[0]) {
      s.familyAuto = true;
    } else if (cockpit_space::ParseFamily(buf, &f, &isAuto)) {
      s.familyAuto = isAuto;
      s.family = isAuto ? cockpit_space::Family::Generic : f;
    } else {
      Note(r, "audio: config: CockpitSpaceFamily is not a cockpit type (%zu characters), auto", wcslen(buf));
      s.familyAuto = true;
    }
    GetPrivateProfileStringW(binaural_config::kSection, L"CockpitSpaceCanopy", L"", buf, 64, ini);
    if (buf[0]) {
      wchar_t* end = nullptr;
      const double v = wcstod(buf, &end);
      if (end == buf || !std::isfinite(v)) Note(r, "audio: config: CockpitSpaceCanopy=%ls is not a number, 1", buf);
      else s.canopy = v != 0;
    }
  }
  {
    wchar_t hk[64] = {};
    GetPrivateProfileStringW(binaural_config::kHotkeySection, L"AudioToggle", L"0:0", hk, 64, ini);
    if (!ParseToggle(hk, &r.toggleVk, &r.toggleMods)) Note(r, "audio: config: [Hotkeys] AudioToggle=%ls is invalid; key disabled", hk);
  }
  return r;
}

inline bool Same(const AudioConfig& a, const AudioConfig& b) {
  return a.binaural == b.binaural && !wcscmp(a.hrtf, b.hrtf) && a.headCircumferenceCm == b.headCircumferenceCm &&
         a.lateHeadTracking == b.lateHeadTracking && a.nearField == b.nearField && a.maxObjects == b.maxObjects &&
         a.levelTrimDb == b.levelTrimDb && a.cpuBudgetPercent == b.cpuBudgetPercent &&
         a.excludeContextMask == b.excludeContextMask && a.logVoices == b.logVoices;
}

}  // namespace audcfg
