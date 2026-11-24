// DCS Control binaural audio: the [Audio] section of DcsQvCull.ini, its defaults, ranges and Live rules.
//
// The app (task D) writes these keys; the audio module (task B) reads them at start and at every ini reload (Live).
// [Audio] is not an engine section (native/dcsqvcull/src/ini_diff.h): a Live write of it never resets the engine kill
// switch, and the engine kill switch never affects audio. Every key below is Live: the module applies a change at the
// next device block (Hrtf: after the control thread has loaded and resampled the new set; the swap is a per-source
// crossfade). Values out of range are clamped and logged once ("audio: config: <Key>=<value> out of range, <used>").
//
// Kill switch: toggle_audio.flag next to the payload (or the dev ini), polled by the same worker as toggle_engine.flag:
// its appearance flips binaural on/off for the session (the file is deleted); off = every owned voice fades back to
// DCS's own panning within one block, the hooks stay in place in pass-through. Optional hotkey [Hotkeys] AudioToggle
// (same syntax as the other hotkeys, default none). Back to stock (the app) removes the [Audio] section, the hrtf
// folder and PupilShift.ini [HeadPose].
//
// Pure, header-only, C++17: the module's config loader and the offline tests use the same table and clamps.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace binaural_config {

constexpr const wchar_t* kSection = L"Audio";
constexpr const wchar_t* kKillFlagFile = L"toggle_audio.flag";
constexpr const wchar_t* kHotkeySection = L"Hotkeys";
constexpr const wchar_t* kHotkeyKey = L"AudioToggle";
// Personal HRTF files converted by the app: <Saved Games>\DCS\Scripts\DcsQvCull\hrtf\<name>.dcshrtf
constexpr const wchar_t* kHrtfFolder = L"hrtf";
constexpr const wchar_t* kDefaultHrtfName = L"default";   // the set embedded in the payload (SADIE II KU100)
constexpr const wchar_t* kEmbeddedResourceName = L"HRTF_DEFAULT";  // RCDATA resource in DcsQvCullPayload.dll
// The pupil shift layer publishes Local\DcsVrHeadPose_v1 only with this key = 1 in PupilShift.ini (default 0). The app
// writes 1 when Binaural=1 and LateHeadTracking=1, else removes it.
constexpr const wchar_t* kPosePublishSection = L"HeadPose";
constexpr const wchar_t* kPosePublishKey = L"Publish";

enum class KeyType { Bool, Int, Float, Text };
struct KeyInfo {
    const wchar_t* name;
    KeyType type;
    double defaultValue;     // numbers; Text keys: see the comment
    double minValue, maxValue;
    const char* ui;          // where the app shows it ("main", "advanced", "diagnostics")
};

// The [Audio] keys. All Live.
constexpr KeyInfo kKeys[] = {
    {L"Binaural", KeyType::Bool, 0, 0, 1, "main"},                       // "3D headphone audio"
    {L"Hrtf", KeyType::Text, 0, 0, 0, "main"},                           // "default" or a file name (no path, no extension), <= 64 chars
    {L"HeadCircumferenceCm", KeyType::Float, 57.0, 48.0, 66.0, "main"},  // "Head size"
    {L"LateHeadTracking", KeyType::Bool, 1, 0, 1, "main"},               // "Follow the head between frames"
    {L"NearField", KeyType::Bool, 1, 0, 1, "advanced"},                  // close sources (cockpit) per ear
    {L"MaxObjects", KeyType::Int, 48, 8, 64, "advanced"},                // most voices rendered binaurally
    {L"LevelTrimDb", KeyType::Float, 0.0, -12.0, 6.0, "advanced"},       // binaural voices only
    {L"CpuBudgetPercent", KeyType::Int, 25, 5, 50, "advanced"},          // of each audio block
    {L"ExcludeContexts", KeyType::Text, 0, 0, 0, "advanced"},            // comma list of context ids 0..7, default empty
    {L"LogVoices", KeyType::Bool, 0, 0, 1, "diagnostics"},               // per-voice CSV next to the log
};
constexpr int kKeyCount = static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0]));

struct AudioConfig {
    bool binaural = false;
    wchar_t hrtf[65] = L"default";
    float headCircumferenceCm = 57.0f;
    bool lateHeadTracking = true;
    bool nearField = true;
    int maxObjects = 48;
    float levelTrimDb = 0.0f;
    int cpuBudgetPercent = 25;
    std::uint32_t excludeContextMask = 0;  // bit i = context id i excluded
    bool logVoices = false;
};

inline double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Head radius from the head circumference: r = C / (2 pi), clamped to 0.06 .. 0.12 m (57 cm -> 9.07 cm). The geometric
// size, for display; the ITD radius the renderer gets is PilotItdRadiusM below.
inline float HeadRadiusFromCircumferenceCm(float cm) {
    const double c = Clamp(std::isfinite(cm) ? cm : 57.0, 48.0, 66.0);
    return static_cast<float>(Clamp(c / 100.0 / (2.0 * 3.14159265358979323846), 0.06, 0.12));
}

// The circumference an HRTF set is taken to be measured at (the average adult head, HeadCircumferenceCm's default).
constexpr double kReferenceCircumferenceCm = 57.0;
// ITD scale of a head circumference against that reference: 1 at 57 cm (the set as measured), clamped like the key.
inline double HeadScaleFromCircumferenceCm(float cm) {
    return Clamp(std::isfinite(cm) ? cm : kReferenceCircumferenceCm, 48.0, 66.0) / kReferenceCircumferenceCm;
}
// The head radius the renderer is given (binaural::Config::headRadiusM, Renderer::SetHeadRadius): the set's own
// Woodworth-fitted radius scaled by the head size, so the renderer's ITD scale (radius / the set's fitted radius) is
// exactly the circumference ratio: 57 cm plays the set's own timing, whatever head it was measured on. (Comparing
// C / 2 pi with a Woodworth fit would scale even the reference head's ITDs by a few percent.) Clamped to the renderer's
// 0.06 .. 0.12 m.
inline float PilotItdRadiusM(float datasetHeadRadiusM, float cm) {
    const double fit = std::isfinite(datasetHeadRadiusM) && datasetHeadRadiusM > 0.0f ? datasetHeadRadiusM : 0.0875;
    return static_cast<float>(Clamp(fit * HeadScaleFromCircumferenceCm(cm), 0.06, 0.12));
}

// "1, 3,5" -> bits 1, 3, 5. Ignores spaces; any token that is not an integer 0..7 makes the result invalid (false).
inline bool ParseExcludeContexts(const wchar_t* text, std::uint32_t* mask) {
    std::uint32_t m = 0;
    if (!text) return *mask = 0, true;
    const wchar_t* p = text;
    while (*p) {
        while (*p == L' ' || *p == L'\t') ++p;
        if (!*p) break;
        if (*p < L'0' || *p > L'9') return false;
        int v = 0;
        while (*p >= L'0' && *p <= L'9') {
            v = v * 10 + (*p - L'0');
            if (v > 7) return false;
            ++p;
        }
        m |= 1u << v;
        while (*p == L' ' || *p == L'\t') ++p;
        if (*p == L',') ++p;
        else if (*p) return false;
    }
    *mask = m;
    return true;
}

// A valid Hrtf value: "default", or 1..64 characters of letters, digits, space, '-', '_', '.', not starting with '.',
// no path separators. The file is <hrtf folder>\<value>.dcshrtf.
inline bool IsValidHrtfName(const wchar_t* s) {
    if (!s || !s[0]) return false;
    const std::size_t n = std::wcslen(s);
    if (n > 64 || s[0] == L'.') return false;
    for (std::size_t i = 0; i < n; ++i) {
        const wchar_t c = s[i];
        const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L' ' ||
                        c == L'-' || c == L'_' || c == L'.';
        if (!ok) return false;
    }
    return true;
}

// Linear gain of LevelTrimDb.
inline float TrimGain(float db) { return static_cast<float>(std::pow(10.0, Clamp(db, -12.0, 6.0) / 20.0)); }

}  // namespace binaural_config
