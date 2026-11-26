// STAND-IN for native/binaural/include/cockpit_space.h (CORE, branch feat/cspace-core; docs/COCKPIT_SPACE.md 8.1).
// The audio module builds against this copy only while the core's header does not exist (CMakeLists.txt adds this
// folder to the include path after native/binaural/include, and only then); once CORE is merged the core's header is
// used and this folder goes. Same API as the design; the type table and the presets are the design's starting values
// (sections 5.1-5.3), not checked against Mods\aircraft here (CORE does that).
#pragma once

#include <cstdint>
#include <cstring>
#include <cwchar>

namespace cockpit_space {

enum class Family : std::uint8_t { Generic = 0, Bubble, Framed, Helicopter, Warbird, Transport, Count };
enum Surface : std::uint8_t { kLeft, kRight, kFront, kBack, kUp, kDown, kSurfaces };
struct SurfaceParams {
  float distanceM, reflection, lowpassHz, highpassHz;  // 0 Hz: no filter
};
struct RoomParams {
  Family family;
  int strength;  // 1..3 (0 is "no room": never published)
  SurfaceParams surface[kSurfaces];
  float earlyDb, lateDb;  // energies re direct at kRoomRefDistanceM, after the send high-pass
  float rt60LowS, rt60HighS, crossoverHz, preDelayMs;
  float sendHpHz;      // 200
  float throughDb;     // 0
  float neutralShare;  // kNeutralShare of section 6.2 (0..1)
};

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

// "auto" (*isAuto) or a family key, case-insensitive. False for anything else (the caller keeps auto).
inline bool ParseFamily(const wchar_t* s, Family* f, bool* isAuto) {
  *isAuto = false;
  if (!s) return false;
  if (!_wcsicmp(s, L"auto")) {
    *isAuto = true;
    *f = Family::Generic;
    return true;
  }
  for (int i = 0; i < static_cast<int>(Family::Count); ++i)
    if (!_wcsicmp(s, FamilyKey(static_cast<Family>(i)))) {
      *f = static_cast<Family>(i);
      return true;
    }
  return false;
}

// Section 5.1: exact unit type names first, then case-insensitive prefixes, else Generic. Never fails.
inline Family FamilyForType(const char* t) {
  if (!t || !*t) return Family::Generic;
  struct E {
    const char* name;
    Family f;
  };
  static const E kExact[] = {
      {"F-16C_50", Family::Bubble}, {"FA-18C_hornet", Family::Bubble}, {"F-15C", Family::Bubble},
      {"F-15ESE", Family::Bubble}, {"AV8BNA", Family::Bubble}, {"MiG-29A", Family::Bubble}, {"MiG-29G", Family::Bubble},
      {"MiG-29S", Family::Bubble}, {"Su-27", Family::Bubble}, {"J-11A", Family::Bubble}, {"Su-33", Family::Bubble},
      {"JF-17", Family::Bubble}, {"M-2000C", Family::Bubble}, {"A-10A", Family::Bubble}, {"A-10C", Family::Bubble},
      {"A-10C_2", Family::Bubble}, {"F-86F Sabre", Family::Bubble}, {"Christen Eagle II", Family::Bubble},
      {"F-14A-135-GR", Family::Framed}, {"F-14B", Family::Framed}, {"F-4E-45MC", Family::Framed},
      {"F-5E-3", Family::Framed}, {"MiG-21Bis", Family::Framed}, {"MiG-19P", Family::Framed},
      {"MiG-15bis", Family::Framed}, {"AJS37", Family::Framed}, {"Su-25", Family::Framed}, {"Su-25T", Family::Framed},
      {"F-100D", Family::Framed}, {"L-39C", Family::Framed}, {"L-39ZA", Family::Framed}, {"C-101EB", Family::Framed},
      {"C-101CC", Family::Framed}, {"MB-339A", Family::Framed}, {"MB-339APAN", Family::Framed},
      {"Yak-52", Family::Framed}, {"AH-64D_BLK_II", Family::Framed}, {"Ka-50", Family::Framed},
      {"Ka-50_3", Family::Framed}, {"Mi-24P", Family::Framed}, {"UH-1H", Family::Helicopter},
      {"Mi-8MT", Family::Helicopter}, {"CH-47Fbl1", Family::Helicopter}, {"SA342L", Family::Helicopter},
      {"SA342M", Family::Helicopter}, {"SA342Mistral", Family::Helicopter}, {"SA342Minigun", Family::Helicopter},
      {"OH58D", Family::Helicopter}, {"P-51D", Family::Warbird}, {"P-51D-30-NA", Family::Warbird},
      {"TF-51D", Family::Warbird}, {"P-47D-30", Family::Warbird}, {"P-47D-30bl1", Family::Warbird},
      {"P-47D-40", Family::Warbird}, {"Bf-109K-4", Family::Warbird}, {"FW-190A8", Family::Warbird},
      {"FW-190D9", Family::Warbird}, {"SpitfireLFMkIX", Family::Warbird}, {"SpitfireLFMkIXCW", Family::Warbird},
      {"MosquitoFBMkVI", Family::Warbird}, {"F4U-1D", Family::Warbird}, {"F4U-1D_CW", Family::Warbird},
      {"I-16", Family::Warbird}, {"C-130J-30", Family::Transport}};
  for (const E& e : kExact)
    if (!strcmp(t, e.name)) return e.f;
  static const E kPrefix[] = {{"Mirage-F1", Family::Framed}, {"F-16", Family::Bubble},  {"FA-18", Family::Bubble},
                              {"F-15", Family::Bubble},      {"MiG-29", Family::Bubble}, {"Su-2", Family::Bubble},
                              {"Mi-8", Family::Helicopter},  {"SA342", Family::Helicopter}, {"P-51", Family::Warbird},
                              {"FW-190", Family::Warbird},   {"Spitfire", Family::Warbird}, {"C-130", Family::Transport}};
  for (const E& e : kPrefix)
    if (!_strnicmp(t, e.name, strlen(e.name))) return e.f;
  return Family::Generic;
}

// Section 5.2 (Normal) x 5.3 (strength). False for strength outside 1..3 or an unknown family.
inline bool Preset(Family f, int strength, RoomParams* out) {
  if (strength < 1 || strength > 3 || static_cast<int>(f) >= static_cast<int>(Family::Count)) return false;
  // left, right, front, back, up, down: {distance, R, LP, HP}
  struct Row {
    SurfaceParams s[kSurfaces];
    float early, late, rtLow, rtHigh, pre;
  };
  static const Row kRows[] = {
      // Generic
      {{{0.40f, 0.70f, 6000, 0}, {0.40f, 0.70f, 6000, 0}, {0.80f, 0.70f, 5000, 0}, {0.35f, 0.45f, 2000, 0},
        {0.28f, 0.80f, 0, 250}, {0.95f, 0.50f, 3000, 0}}, -12, -17, 0.12f, 0.09f, 2},
      // Bubble
      {{{0.38f, 0.75f, 8000, 250}, {0.38f, 0.75f, 8000, 250}, {0.75f, 0.70f, 5000, 0}, {0.30f, 0.45f, 2000, 0},
        {0.25f, 0.85f, 0, 300}, {0.95f, 0.50f, 3000, 0}}, -12, -17, 0.10f, 0.08f, 2},
      // Framed
      {{{0.35f, 0.70f, 6000, 0}, {0.35f, 0.70f, 6000, 0}, {0.70f, 0.70f, 5000, 0}, {0.30f, 0.45f, 2000, 0},
        {0.28f, 0.75f, 7000, 250}, {0.95f, 0.50f, 3000, 0}}, -12, -17, 0.12f, 0.09f, 2},
      // Helicopter
      {{{0.70f, 0.70f, 6000, 0}, {0.90f, 0.70f, 6000, 0}, {1.00f, 0.75f, 0, 200}, {1.20f, 0.60f, 4000, 0},
        {0.60f, 0.75f, 7000, 200}, {1.00f, 0.60f, 4000, 0}}, -11, -14, 0.25f, 0.18f, 5},
      // Warbird
      {{{0.35f, 0.75f, 6000, 0}, {0.35f, 0.75f, 6000, 0}, {0.80f, 0.70f, 5000, 0}, {0.50f, 0.80f, 4000, 0},
        {0.25f, 0.80f, 8000, 300}, {0.90f, 0.50f, 3000, 0}}, -12, -16, 0.14f, 0.10f, 3},
      // Transport
      {{{0.80f, 0.55f, 4000, 0}, {0.80f, 0.55f, 4000, 0}, {1.00f, 0.70f, 0, 200}, {1.50f, 0.50f, 3000, 0},
        {0.70f, 0.60f, 6000, 200}, {1.10f, 0.45f, 2500, 0}}, -13, -15, 0.25f, 0.20f, 6},
  };
  const Row& r = kRows[static_cast<int>(f)];
  RoomParams p{};
  p.family = f;
  p.strength = strength;
  for (int i = 0; i < kSurfaces; ++i) p.surface[i] = r.s[i];
  const float earlyAdd[4] = {0, -4, 0, 3}, lateAdd[4] = {0, -5, 0, 4}, rtMul[4] = {1, 0.9f, 1, 1.2f};
  p.earlyDb = r.early + earlyAdd[strength];
  p.lateDb = r.late + lateAdd[strength];
  p.rt60LowS = r.rtLow * rtMul[strength];
  p.rt60HighS = r.rtHigh * rtMul[strength];
  p.crossoverHz = 1000;
  p.preDelayMs = r.pre;
  p.sendHpHz = 200;
  p.throughDb = 0;
  p.neutralShare = 0.6f;
  *out = p;
  return true;
}

}  // namespace cockpit_space
