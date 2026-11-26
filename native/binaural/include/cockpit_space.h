// DCS Control binaural audio: cockpit space families, presets and the unit type table (docs/COCKPIT_SPACE.md
// sections 5 and 8.1).
//
// Pure, header-only, C++17: used by the core's Room (binaural_room.h), the audio module (family resolution, Live
// changes), the offline harness and the contract test. No allocation, no I/O.
//
// Unit type names (section 5.1). Checked statically on 2026-11-25 against the DCS 2.9.31 install, read-only:
// `make_flyable('<name>', ...)` and `self_ID`/`Name` lines in <DCS>\Mods\aircraft\*\entry.lua and the aircraft Lua the
// install keeps readable. Names found there are marked [V] in the table below, names not found in that install (modules
// not installed, or defined in an encrypted file) are marked [A]: they come from the design list and fall back to the
// prefix rules (and then to Generic) if DCS spells them differently, never failing.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>

namespace cockpit_space {

enum class Family : std::uint8_t { Generic = 0, Bubble, Framed, Helicopter, Warbird, Transport, Count };
// Walls of the box around the head, aircraft frame: x forward, y up, z right; head at the origin.
enum Surface : std::uint8_t { kLeft, kRight, kFront, kBack, kUp, kDown, kSurfaces };

// One wall: distance from the head (metres, > 0), broadband amplitude reflection coefficient (0..0.95), and the
// first-order filters of its reflection: low-pass corner (panels, seat, body) and high-pass corner (glass); 0 Hz = no
// filter.
struct SurfaceParams {
    float distanceM, reflection, lowpassHz, highpassHz;
};

struct RoomParams {
    Family family;
    int strength;                       // 1..3 (0 is "no room": never published)
    SurfaceParams surface[kSurfaces];
    float earlyDb, lateDb;              // energies re direct at kRoomRefDistanceM (0.6 m ahead), after the send high-pass
    float rt60LowS, rt60HighS, crossoverHz, preDelayMs;
    float sendHpHz;                     // 200
    float throughDb;                    // 0
    float neutralShare;                 // kNeutralShare of section 6.2 (0..1)
};

// The share of the wet energy the direct gain takes back (docs/COCKPIT_SPACE.md 6.2). Chosen by R4 (level neutrality of
// K-weighted pink noise and of a DCS-like mix, binaural_room_test C8): the sends are high-passed at 200 Hz, so the full
// broadband energy over-compensates low-heavy sounds. Measured (2026-11-26, generic family, five cockpit sources, mean):
// 0.6 left pink noise +0.45 dB at Strong on the KU100 set (worst source +1.01 dB), the DCS-like mix -0.15 dB; 0.7 centres
// the two.
constexpr float kNeutralShare = 0.7f;

constexpr int kStrengthOff = 0, kStrengthSubtle = 1, kStrengthNormal = 2, kStrengthStrong = 3;

inline const wchar_t* FamilyKey(Family f) {
    switch (f) {
        case Family::Bubble: return L"bubble";
        case Family::Framed: return L"framed";
        case Family::Helicopter: return L"helicopter";
        case Family::Warbird: return L"warbird";
        case Family::Transport: return L"transport";
        default: return L"generic";
    }
}
// The UI name (docs/COCKPIT_SPACE.md 5.1), for log lines.
inline const char* FamilyName(Family f) {
    switch (f) {
        case Family::Bubble: return "bubble canopy";
        case Family::Framed: return "framed canopy";
        case Family::Helicopter: return "helicopter cabin";
        case Family::Warbird: return "warbird";
        case Family::Transport: return "transport flight deck";
        default: return "generic";
    }
}

namespace detail {
inline wchar_t LowerW(wchar_t c) { return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c; }
inline char LowerA(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
inline bool EqualsIW(const wchar_t* a, const wchar_t* b) {
    for (; *a && *b; ++a, ++b)
        if (LowerW(*a) != LowerW(*b)) return false;
    return *a == *b;
}
// Length of `prefix` when `s` starts with it (case-insensitive), else 0.
inline std::size_t PrefixI(const char* s, const char* prefix) {
    std::size_t n = 0;
    for (; prefix[n]; ++n)
        if (!s[n] || LowerA(s[n]) != LowerA(prefix[n])) return 0;
    return n;
}
inline bool Equals(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (*a != *b) return false;
    return *a == *b;
}
}  // namespace detail

// "auto" (isAuto true, *f Generic) or a family key, case-insensitive, surrounding spaces ignored. False (outputs
// untouched) for anything else or null. A long value is rejected, not truncated.
inline bool ParseFamily(const wchar_t* s, Family* f, bool* isAuto) {
    if (!s || !f || !isAuto) return false;
    while (*s == L' ' || *s == L'\t') ++s;
    wchar_t buf[16];
    std::size_t n = 0;
    for (; s[n] && n < 15; ++n) buf[n] = s[n];
    if (s[n]) return false;
    while (n > 0 && (buf[n - 1] == L' ' || buf[n - 1] == L'\t')) --n;
    buf[n] = 0;
    if (detail::EqualsIW(buf, L"auto")) return *f = Family::Generic, *isAuto = true, true;
    for (int i = 0; i < static_cast<int>(Family::Count); ++i) {
        const Family c = static_cast<Family>(i);
        if (detail::EqualsIW(buf, FamilyKey(c))) return *f = c, *isAuto = false, true;
    }
    return false;
}

struct TypeEntry {
    const char* name;
    Family family;
};
// Exact unit type names (case-sensitive). [V] found in the 2.9.31 install read on 2026-11-25; [A] from the design list.
constexpr TypeEntry kTypes[] = {
    // Bubble canopy
    {"F-16C_50", Family::Bubble},           // [V] also in the hrtf live runs' dcs.log
    {"FA-18C_hornet", Family::Bubble},      // [V]
    {"F-15C", Family::Bubble},              // [V]
    {"F-15ESE", Family::Bubble},            // [V]
    {"AV8BNA", Family::Bubble},             // [V]
    {"MiG-29A", Family::Bubble},            // [V]
    {"MiG-29G", Family::Bubble},            // [V]
    {"MiG-29S", Family::Bubble},            // [V]
    {"MiG-29 Fulcrum", Family::Bubble},     // [A] the full-fidelity module's flyable_ID (Mods\aircraft\MiG-29-Fulcrum)
    {"Su-27", Family::Bubble},              // [V]
    {"J-11A", Family::Bubble},              // [V]
    {"Su-33", Family::Bubble},              // [V]
    {"JF-17", Family::Bubble},              // [V]
    {"M-2000C", Family::Bubble},            // [V]
    {"A-10A", Family::Bubble},              // [V]
    {"A-10C", Family::Bubble},              // [V]
    {"A-10C_2", Family::Bubble},            // [V]
    {"F-86F Sabre", Family::Bubble},        // [V]
    {"F-86F_FC", Family::Bubble},           // [V]
    {"Christen Eagle II", Family::Bubble},  // [V]
    // Framed canopy (and tandem armoured cockpits)
    {"F-14A-135-GR", Family::Framed},       // [V]
    {"F-14A-95-GR", Family::Framed},        // [V]
    {"F-14A-135-GR-Early", Family::Framed}, // [V]
    {"F-14B", Family::Framed},              // [V]
    {"F-14BU", Family::Framed},             // [V]
    {"F-4E-45MC", Family::Framed},          // [V]
    {"F-5E-3", Family::Framed},             // [V]
    {"F-5E-3_FC", Family::Framed},          // [V]
    {"MiG-21Bis", Family::Framed},          // [V]
    {"MiG-19P", Family::Framed},            // [V]
    {"MiG-15bis", Family::Framed},          // [V]
    {"MiG-15bis_FC", Family::Framed},       // [V]
    {"Mirage-F1CE", Family::Framed},        // [V]
    {"Mirage-F1EE", Family::Framed},        // [V]
    {"Mirage-F1BE", Family::Framed},        // [V]
    {"Mirage-F1M-EE", Family::Framed},      // [A] not in this install's entry.lua
    {"AJS37", Family::Framed},              // [V]
    {"Su-25", Family::Framed},              // [V]
    {"Su-25T", Family::Framed},             // [V]
    {"F-100D", Family::Framed},             // [V]
    {"L-39C", Family::Framed},              // [V]
    {"L-39ZA", Family::Framed},             // [V]
    {"C-101EB", Family::Framed},            // [V]
    {"C-101CC", Family::Framed},            // [V]
    {"MB-339A", Family::Framed},            // [V]
    {"MB-339APAN", Family::Framed},         // [V]
    {"Yak-52", Family::Framed},             // [V]
    {"AH-64D_BLK_II", Family::Framed},      // [V]
    {"Ka-50", Family::Framed},              // [V]
    {"Ka-50_3", Family::Framed},            // [V]
    {"Mi-24P", Family::Framed},             // [V]
    // Helicopter cabin
    {"UH-1H", Family::Helicopter},          // [V]
    {"Mi-8MT", Family::Helicopter},         // [V]
    {"CH-47Fbl1", Family::Helicopter},      // [V]
    {"SA342L", Family::Helicopter},         // [V]
    {"SA342M", Family::Helicopter},         // [V]
    {"SA342Mistral", Family::Helicopter},   // [V]
    {"SA342Minigun", Family::Helicopter},   // [V]
    {"OH58D", Family::Helicopter},          // [V]
    // Warbird
    {"P-51D", Family::Warbird},             // [V]
    {"P-51D-30-NA", Family::Warbird},       // [V]
    {"TF-51D", Family::Warbird},            // [V]
    {"P-47D-30", Family::Warbird},          // [V]
    {"P-47D-30bl1", Family::Warbird},       // [V]
    {"P-47D-40", Family::Warbird},          // [V]
    {"Bf-109K-4", Family::Warbird},         // [V]
    {"FW-190A8", Family::Warbird},          // [V]
    {"FW-190D9", Family::Warbird},          // [V]
    {"SpitfireLFMkIX", Family::Warbird},    // [V]
    {"SpitfireLFMkIXCW", Family::Warbird},  // [V]
    {"MosquitoFBMkVI", Family::Warbird},    // [V]
    {"F4U-1D", Family::Warbird},            // [V]
    {"F4U-1D_CW", Family::Warbird},         // [V]
    {"I-16", Family::Warbird},              // [V]
    // Transport flight deck
    {"C-130J-30", Family::Transport},       // [V]
};
constexpr int kTypeCount = static_cast<int>(sizeof(kTypes) / sizeof(kTypes[0]));

// Case-insensitive prefixes, used when no exact name matched; the longest matching prefix wins (so "F-14" beats "F-1",
// "Su-25" beats "Su-2").
constexpr TypeEntry kPrefixes[] = {
    {"F-16", Family::Bubble},   {"FA-18", Family::Bubble},   {"F/A-18", Family::Bubble},  {"F-15", Family::Bubble},
    {"AV8B", Family::Bubble},   {"AV-8B", Family::Bubble},   {"MiG-29", Family::Bubble},  {"Su-27", Family::Bubble},
    {"J-11", Family::Bubble},   {"Su-33", Family::Bubble},   {"JF-17", Family::Bubble},   {"M-2000", Family::Bubble},
    {"A-10", Family::Bubble},   {"F-86", Family::Bubble},    {"Christen Eagle", Family::Bubble},
    {"F-14", Family::Framed},   {"F-4E", Family::Framed},    {"F-5", Family::Framed},     {"MiG-21", Family::Framed},
    {"MiG-19", Family::Framed}, {"MiG-15", Family::Framed},  {"Mirage-F1", Family::Framed}, {"Mirage F1", Family::Framed},
    {"AJS37", Family::Framed},  {"AJS-37", Family::Framed},  {"Su-25", Family::Framed},   {"F-100", Family::Framed},
    {"L-39", Family::Framed},   {"C-101", Family::Framed},   {"MB-339", Family::Framed},  {"Hawk", Family::Framed},
    {"Yak-52", Family::Framed}, {"A-4", Family::Framed},     {"A-6", Family::Framed},     {"AH-64", Family::Framed},
    {"Ka-50", Family::Framed},  {"Mi-24", Family::Framed},
    {"UH-1", Family::Helicopter}, {"Mi-8", Family::Helicopter}, {"CH-47", Family::Helicopter}, {"SA342", Family::Helicopter},
    {"OH58", Family::Helicopter}, {"OH-58", Family::Helicopter},
    {"P-51", Family::Warbird},  {"TF-51", Family::Warbird},  {"P-47", Family::Warbird},   {"Bf-109", Family::Warbird},
    {"FW-190", Family::Warbird}, {"Fw 190", Family::Warbird}, {"Spitfire", Family::Warbird}, {"Mosquito", Family::Warbird},
    {"F4U", Family::Warbird},   {"I-16", Family::Warbird},
    {"C-130", Family::Transport},
};
constexpr int kPrefixCount = static_cast<int>(sizeof(kPrefixes) / sizeof(kPrefixes[0]));

// The family for a unit type name: exact names first, then the longest case-insensitive prefix, else Generic (also for
// null or empty).
inline Family FamilyForType(const char* unitType) {
    if (!unitType || !unitType[0]) return Family::Generic;
    for (const TypeEntry& e : kTypes)
        if (detail::Equals(unitType, e.name)) return e.family;
    std::size_t best = 0;
    Family f = Family::Generic;
    for (const TypeEntry& e : kPrefixes) {
        const std::size_t n = detail::PrefixI(unitType, e.name);
        if (n > best) best = n, f = e.family;
    }
    return f;
}

// Section 5.2 (Normal) x section 5.3 (strength 1..3). False (out untouched) for strength outside 1..3, a family out of
// range or null out.
inline bool Preset(Family f, int strength, RoomParams* out) {
    if (!out || strength < kStrengthSubtle || strength > kStrengthStrong || static_cast<int>(f) < 0 ||
        static_cast<int>(f) >= static_cast<int>(Family::Count))
        return false;
    struct Row {
        float left, right, front, back, up, down;
        SurfaceParams upS, sideS, frontS, backS, downS;  // distance filled from above
        float earlyDb, lateDb, rtLow, rtHigh, preDelayMs;
    };
    // R, LP, HP per wall (docs/COCKPIT_SPACE.md 5.2).
    static const Row kRows[] = {
        // Generic
        {0.40f, 0.40f, 0.80f, 0.35f, 0.28f, 0.95f, {0, 0.80f, 0, 250}, {0, 0.70f, 6000, 0}, {0, 0.70f, 5000, 0},
         {0, 0.45f, 2000, 0}, {0, 0.50f, 3000, 0}, -12, -17, 0.12f, 0.09f, 2},
        // Bubble
        {0.38f, 0.38f, 0.75f, 0.30f, 0.25f, 0.95f, {0, 0.85f, 0, 300}, {0, 0.75f, 8000, 250}, {0, 0.70f, 5000, 0},
         {0, 0.45f, 2000, 0}, {0, 0.50f, 3000, 0}, -12, -17, 0.10f, 0.08f, 2},
        // Framed
        {0.35f, 0.35f, 0.70f, 0.30f, 0.28f, 0.95f, {0, 0.75f, 7000, 250}, {0, 0.70f, 6000, 0}, {0, 0.70f, 5000, 0},
         {0, 0.45f, 2000, 0}, {0, 0.50f, 3000, 0}, -12, -17, 0.12f, 0.09f, 2},
        // Helicopter
        {0.70f, 0.90f, 1.00f, 1.20f, 0.60f, 1.00f, {0, 0.75f, 7000, 200}, {0, 0.70f, 6000, 0}, {0, 0.75f, 0, 200},
         {0, 0.60f, 4000, 0}, {0, 0.60f, 4000, 0}, -11, -14, 0.25f, 0.18f, 5},
        // Warbird
        {0.35f, 0.35f, 0.80f, 0.50f, 0.25f, 0.90f, {0, 0.80f, 8000, 300}, {0, 0.75f, 6000, 0}, {0, 0.70f, 5000, 0},
         {0, 0.80f, 4000, 0}, {0, 0.50f, 3000, 0}, -12, -16, 0.14f, 0.10f, 3},
        // Transport
        {0.80f, 0.80f, 1.00f, 1.50f, 0.70f, 1.10f, {0, 0.60f, 6000, 200}, {0, 0.55f, 4000, 0}, {0, 0.70f, 0, 200},
         {0, 0.50f, 3000, 0}, {0, 0.45f, 2500, 0}, -13, -15, 0.25f, 0.20f, 6},
    };
    const Row& r = kRows[static_cast<int>(f)];
    RoomParams p{};
    p.family = f;
    p.strength = strength;
    p.surface[kLeft] = r.sideS, p.surface[kLeft].distanceM = r.left;
    p.surface[kRight] = r.sideS, p.surface[kRight].distanceM = r.right;
    p.surface[kFront] = r.frontS, p.surface[kFront].distanceM = r.front;
    p.surface[kBack] = r.backS, p.surface[kBack].distanceM = r.back;
    p.surface[kUp] = r.upS, p.surface[kUp].distanceM = r.up;
    p.surface[kDown] = r.downS, p.surface[kDown].distanceM = r.down;
    // Strength (section 5.3): Subtle early -4 dB, late -5 dB, RT60 x 0.9; Strong +3, +4, x 1.2.
    const float earlyStep[] = {0, -4.0f, 0.0f, 3.0f}, lateStep[] = {0, -5.0f, 0.0f, 4.0f}, rtScale[] = {0, 0.9f, 1.0f, 1.2f};
    p.earlyDb = r.earlyDb + earlyStep[strength];
    p.lateDb = r.lateDb + lateStep[strength];
    p.rt60LowS = r.rtLow * rtScale[strength];
    p.rt60HighS = r.rtHigh * rtScale[strength];
    p.crossoverHz = 1000.0f;
    p.preDelayMs = r.preDelayMs;
    p.sendHpHz = 200.0f;
    p.throughDb = 0.0f;
    p.neutralShare = kNeutralShare;
    *out = p;
    return true;
}

// Clamps every field to the range the Room accepts (non-finite values take the generic Normal value). The Room applies
// it to every SetParams; exposed for the module's logging and the tests.
inline RoomParams Sanitize(const RoomParams& in) {
    RoomParams def{};
    Preset(Family::Generic, kStrengthNormal, &def);
    RoomParams p = in;
    auto clampf = [](float v, float lo, float hi, float fallback) {
        if (!std::isfinite(v)) return fallback;
        return v < lo ? lo : (v > hi ? hi : v);
    };
    if (static_cast<int>(p.family) < 0 || static_cast<int>(p.family) >= static_cast<int>(Family::Count)) p.family = Family::Generic;
    p.strength = p.strength < kStrengthSubtle ? kStrengthSubtle : (p.strength > kStrengthStrong ? kStrengthStrong : p.strength);
    for (int s = 0; s < kSurfaces; ++s) {
        SurfaceParams& w = p.surface[s];
        const SurfaceParams& d = def.surface[s];
        w.distanceM = clampf(w.distanceM, 0.1f, 3.0f, d.distanceM);
        w.reflection = clampf(w.reflection, 0.0f, 0.95f, d.reflection);
        w.lowpassHz = clampf(w.lowpassHz, 0.0f, 20000.0f, 0.0f);
        w.highpassHz = clampf(w.highpassHz, 0.0f, 2000.0f, 0.0f);
    }
    p.earlyDb = clampf(p.earlyDb, -40.0f, 0.0f, def.earlyDb);
    p.lateDb = clampf(p.lateDb, -40.0f, 0.0f, def.lateDb);
    p.rt60LowS = clampf(p.rt60LowS, 0.03f, 1.0f, def.rt60LowS);
    p.rt60HighS = clampf(p.rt60HighS, 0.03f, 1.0f, def.rt60HighS);
    p.crossoverHz = clampf(p.crossoverHz, 200.0f, 4000.0f, def.crossoverHz);
    p.preDelayMs = clampf(p.preDelayMs, 0.0f, 10.0f, def.preDelayMs);
    p.sendHpHz = clampf(p.sendHpHz, 20.0f, 1000.0f, def.sendHpHz);
    p.throughDb = clampf(p.throughDb, -40.0f, 6.0f, 0.0f);
    p.neutralShare = clampf(p.neutralShare, 0.0f, 1.0f, kNeutralShare);
    return p;
}

}  // namespace cockpit_space
