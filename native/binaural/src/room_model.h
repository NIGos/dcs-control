// BinauralCore room internals: the pure parametric model of the cockpit space (docs/COCKPIT_SPACE.md 4.2, 6.2): image
// sources of the box, bus panning (VBAP on the 14 aircraft-fixed buses), filter coefficients and the energies the
// direct gain compensates. Header-only and allocation-free: room.cpp runs it on the audio thread per voice per block, the
// harness checks it against closed forms. Not part of the interface (the module uses include/binaural_room.h).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "binaural_room.h"
#include "cockpit_space.h"

namespace binaural::room_model {

using cockpit_space::Family;
using cockpit_space::RoomParams;
using cockpit_space::SurfaceParams;
namespace cs = cockpit_space;

constexpr float kPiF = 3.14159265358979f;
// The model's sample rate for the pure energy estimates (RoomDirectGain has no rate): white-noise power gains of the
// first-order filters are taken at 48 kHz; at 44.1 or 96 kHz they differ by under 0.1 dB of the wet energy.
constexpr float kModelRate = 48000.0f;
constexpr float kMinSourceM = 0.05f;   // as binaural_core.h kMinDistanceM
// FDN line lengths at 48 kHz (docs/COCKPIT_SPACE.md 4.3), primes, 3.1 to 10 ms.
constexpr std::uint32_t kFdnLengths48[kRoomFdnLines] = {149, 211, 263, 293, 337, 389, 431, 479};

// ---- buses ----------------------------------------------------------------------------------------------------------
// 0..5: +x, -x, +y, -y, +z, -z; 6..13: cube corners (sx, sy, sz), index 6 + 4 (sx < 0) + 2 (sy < 0) + (sz < 0).
inline void BusDirection(std::uint32_t b, float out[3]) {
    if (b < 6) {
        out[0] = out[1] = out[2] = 0.0f;
        out[b / 2] = (b & 1) ? -1.0f : 1.0f;
        return;
    }
    const std::uint32_t c = b - 6;
    const float k = 0.57735026919f;
    out[0] = (c & 4) ? -k : k;
    out[1] = (c & 2) ? -k : k;
    out[2] = (c & 1) ? -k : k;
}
inline std::uint32_t AxisBus(int axis, float sign) { return static_cast<std::uint32_t>(axis * 2 + (sign < 0.0f ? 1 : 0)); }
inline std::uint32_t CornerBus(float sx, float sy, float sz) {
    return 6u + (sx < 0.0f ? 4u : 0u) + (sy < 0.0f ? 2u : 0u) + (sz < 0.0f ? 1u : 0u);
}

// Energy-preserving VBAP: the hull of the 14 buses is a tetrakis hexahedron (24 triangles: each cube face's centre with
// one of its edges). The triangle holding direction `u` (unit, aircraft frame) is the face of the largest |component|
// and the edge on the larger of the other two; the three gains solve u = g0 A + g1 C1 + g2 C2 (all >= 0 inside the
// cone), normalised to sum of squares 1. A direction on a bus gives that bus alone.
inline void Pan(const float u[3], std::uint32_t idx[3], float g[3]) {
    const float ax[3] = {std::fabs(u[0]), std::fabs(u[1]), std::fabs(u[2])};
    int a = 0;
    if (ax[1] > ax[a]) a = 1;
    if (ax[2] > ax[a]) a = 2;
    const int b0 = (a + 1) % 3, c0 = (a + 2) % 3;
    const int b = ax[b0] >= ax[c0] ? b0 : c0, c = b == b0 ? c0 : b0;
    float s[3];
    s[a] = u[a] < 0.0f ? -1.0f : 1.0f;
    s[b] = u[b] < 0.0f ? -1.0f : 1.0f;
    float A[3] = {0, 0, 0}, C1[3], C2[3];
    A[a] = s[a];
    const float k = 0.57735026919f;
    C1[a] = s[a] * k, C1[b] = s[b] * k, C1[c] = k;
    C2[a] = s[a] * k, C2[b] = s[b] * k, C2[c] = -k;
    auto cross = [](const float* p, const float* q, float* r) {
        r[0] = p[1] * q[2] - p[2] * q[1], r[1] = p[2] * q[0] - p[0] * q[2], r[2] = p[0] * q[1] - p[1] * q[0];
    };
    auto dot = [](const float* p, const float* q) { return p[0] * q[0] + p[1] * q[1] + p[2] * q[2]; };
    float x12[3], x20[3], x01[3];
    cross(C1, C2, x12), cross(C2, A, x20), cross(A, C1, x01);
    const float det = dot(A, x12);  // > 0 by construction (A, C1, C2 counter-clockwise or not: take the sign)
    float g0 = dot(u, x12) / det, g1 = dot(u, x20) / det, g2 = dot(u, x01) / det;
    g0 = std::max(g0, 0.0f), g1 = std::max(g1, 0.0f), g2 = std::max(g2, 0.0f);
    const float n = std::sqrt(g0 * g0 + g1 * g1 + g2 * g2);
    const float inv = n > 1e-12f ? 1.0f / n : 0.0f;
    idx[0] = AxisBus(a, s[a]);
    float sc1[3], sc2[3];
    sc1[a] = s[a], sc1[b] = s[b], sc1[c] = 1.0f;
    sc2[a] = s[a], sc2[b] = s[b], sc2[c] = -1.0f;
    idx[1] = CornerBus(sc1[0], sc1[1], sc1[2]);
    idx[2] = CornerBus(sc2[0], sc2[1], sc2[2]);
    g[0] = g0 * inv, g[1] = g1 * inv, g[2] = g2 * inv;
    if (inv == 0.0f) g[0] = 1.0f;  // degenerate input: the face's centre
}

// ---- filters --------------------------------------------------------------------------------------------------------
// One-pole coefficient: y += a (x - y), a = 1 - exp(-2 pi fc / fs). 1 (no filtering) for fc <= 0 or near Nyquist.
inline float OnePoleLowpassCoef(float fc, float fs) {
    if (!(fc > 0.0f) || fc >= 0.45f * fs) return 1.0f;
    return 1.0f - std::exp(-2.0f * kPiF * fc / fs);
}
// High-pass as x - lowpass(x): its lowpass coefficient; 0 (no filtering) for fc <= 5 Hz.
inline float OnePoleHighpassCoef(float fc, float fs) {
    if (!(fc > 5.0f)) return 0.0f;
    return 1.0f - std::exp(-2.0f * kPiF * std::min(fc, 0.45f * fs) / fs);
}
// White-noise power gains of those filters (closed forms; product of the two for a cascade with corners far apart).
inline float LowpassPower(float a) { return a >= 1.0f ? 1.0f : a / (2.0f - a); }
inline float HighpassPower(float b) { return b <= 0.0f ? 1.0f : (1.0f - b) * (1.0f - b) * 2.0f / (2.0f - b); }
// Fourth-order Butterworth high-pass (the send high-pass): the share of white-noise power it keeps,
// 1 - (pi / 8) / sin(pi / 8) x fc / (fs / 2).
inline float SendHighpassPower(float fc, float fs) { return std::max(0.0f, 1.0f - 1.0262472f * fc / (0.5f * fs)); }

// ---- the box and its image sources ----------------------------------------------------------------------------------
struct Tap {
    float delayS;      // relative to the direct sound (seconds, 0 .. kRoomMaxTapDelayMs)
    float gain;        // amplitude re the direct sound, before the early normalisation (geometry x reflection(s) x canopy)
    float u[3];        // direction of the image source, aircraft frame, unit
    float lowpassHz;   // the wall's (min of both for the scatter taps), 0 = none
    float highpassHz;  // the wall's (max of both), 0 = none
};
// Tap order: 0 left, 1 right, 2 front, 3 back, 4 up, 5 down, 6 up then left, 7 up then right.
constexpr int kTapSurface[kRoomTaps][2] = {{cs::kLeft, -1}, {cs::kRight, -1}, {cs::kFront, -1}, {cs::kBack, -1},
                                           {cs::kUp, -1},   {cs::kDown, -1},  {cs::kUp, cs::kLeft}, {cs::kUp, cs::kRight}};

// A fixed extra delay per tap (seconds): a real cockpit is neither a perfect box nor symmetric, and a mirror-symmetric
// box gives a median source pairs of reflections (left/right walls, the two canopy scatter taps) at exactly the same
// delay, which add coherently at both ears: up to 3 dB more early energy than the model's incoherent sum (measured), a
// deeper comb, and early reflections that are identical at both ears (no decorrelation, R1). 0.05-0.3 ms apart
// decorrelates the pairs above about 2 kHz and leaves the geometry's timing within a few centimetres of path.
constexpr float kTapJitterS[kRoomTaps] = {0.00000f, 0.00021f, 0.00007f, 0.00013f, 0.00003f, 0.00017f, 0.00029f, 0.00011f};
// Harness hook (C5's symmetric-room check needs the box exactly symmetric): true turns the offsets off. Never set by the
// module.
inline bool& TapJitterOffForTests() {
    static bool off = false;
    return off;
}
inline float TapJitter(std::uint32_t k) { return TapJitterOffForTests() ? 0.0f : kTapJitterS[k]; }

// The wall on surface s: axis (0 x, 1 y, 2 z) and signed coordinate.
inline void Wall(const RoomParams& p, int s, int* axis, float* coord) {
    const float d = p.surface[s].distanceM;
    switch (s) {
        case cs::kLeft: *axis = 2, *coord = -d; break;
        case cs::kRight: *axis = 2, *coord = d; break;
        case cs::kFront: *axis = 0, *coord = d; break;
        case cs::kBack: *axis = 0, *coord = -d; break;
        case cs::kUp: *axis = 1, *coord = d; break;
        default: *axis = 1, *coord = -d; break;
    }
}

// The point the source excites (section 4.2): the source itself inside the box; outside it (or flagged through the
// canopy), the point where its direction from the head crosses the box. Positions shorter than kMinSourceM are pushed
// out to it along their direction (straight ahead for a zero vector). Returns true when the point was moved onto the box.
inline bool ExcitationPoint(const RoomParams& p, const float pos[3], std::uint32_t flags, float out[3]) {
    float v[3] = {pos[0], pos[1], pos[2]};
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) v[0] = kRoomRefDistanceM, v[1] = v[2] = 0.0f;
    float r = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (r < 1e-6f) v[0] = kMinSourceM, v[1] = v[2] = 0.0f, r = kMinSourceM;
    if (r < kMinSourceM) {
        const float k = kMinSourceM / r;
        v[0] *= k, v[1] *= k, v[2] *= k;
    }
    // The largest t <= 1 that keeps t v inside the box.
    float t = 1.0f;
    const float lim[3][2] = {{-p.surface[cs::kBack].distanceM, p.surface[cs::kFront].distanceM},
                             {-p.surface[cs::kDown].distanceM, p.surface[cs::kUp].distanceM},
                             {-p.surface[cs::kLeft].distanceM, p.surface[cs::kRight].distanceM}};
    for (int a = 0; a < 3; ++a) {
        if (v[a] > lim[a][1]) t = std::min(t, lim[a][1] / v[a]);
        if (v[a] < lim[a][0]) t = std::min(t, lim[a][0] / v[a]);
    }
    const bool through = (flags & room_flags::kThroughCanopy) != 0;
    if (through && t >= 1.0f) {
        // Flagged through the canopy but inside the box (a world voice close to the head): its own direction onto the box.
        float tb = 1e9f;
        for (int a = 0; a < 3; ++a) {
            if (v[a] > 1e-9f) tb = std::min(tb, lim[a][1] / v[a]);
            if (v[a] < -1e-9f) tb = std::min(tb, lim[a][0] / v[a]);
        }
        t = tb;
    }
    out[0] = v[0] * t, out[1] = v[1] * t, out[2] = v[2] * t;
    return t < 1.0f || through;
}

inline bool InsideBox(const RoomParams& p, const float pos[3]) {
    float e[3];
    return !ExcitationPoint(p, pos, 0, e);
}

// The 8 taps for a source at `pos` (aircraft frame), before the early normalisation. Canopy `c` (0..1) scales the up
// wall and the two scatter taps by (1 - c), except for the helicopter family (no glass above to open). kLateOnly: all
// gains 0. A tap whose image coincides with the excitation point (the wall it lies on) gets gain 0.
inline void ComputeTaps(const RoomParams& p, float canopy, const float pos[3], std::uint32_t flags, Tap out[kRoomTaps]) {
    float e[3];
    ExcitationPoint(p, pos, flags, e);
    const float r = std::max(std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]), 1e-6f);
    const float c = std::clamp(std::isfinite(canopy) ? canopy : 0.0f, 0.0f, 1.0f);
    const float open = p.family == Family::Helicopter ? 1.0f : 1.0f - c;
    const float maxDelay = kRoomMaxTapDelayMs * 1e-3f;
    for (std::uint32_t k = 0; k < kRoomTaps; ++k) {
        Tap& t = out[k];
        float q[3] = {e[0], e[1], e[2]};
        float refl = 1.0f, lp = 0.0f, hp = 0.0f;
        for (int j = 0; j < 2; ++j) {
            const int s = kTapSurface[k][j];
            if (s < 0) continue;
            int axis;
            float w;
            Wall(p, s, &axis, &w);
            q[axis] = 2.0f * w - q[axis];
            const SurfaceParams& sp = p.surface[s];
            refl *= sp.reflection;
            if (sp.lowpassHz > 0.0f) lp = lp > 0.0f ? std::min(lp, sp.lowpassHz) : sp.lowpassHz;
            hp = std::max(hp, sp.highpassHz);
        }
        const float rq = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
        const float extra = rq - r;
        float g = (extra > 1e-4f && rq > 1e-6f) ? (r / rq) * refl : 0.0f;
        if (k == 4 || k >= 6) g *= open;
        if (flags & room_flags::kLateOnly) g = 0.0f;
        t.gain = g;
        t.delayS = std::clamp(extra / kRoomSpeedOfSound + TapJitter(k), 0.0f, maxDelay);
        const float inv = rq > 1e-6f ? 1.0f / rq : 0.0f;
        t.u[0] = q[0] * inv, t.u[1] = q[1] * inv, t.u[2] = q[2] * inv;
        if (inv == 0.0f) t.u[0] = 1.0f;
        t.lowpassHz = lp;
        t.highpassHz = hp;
    }
}

// The amplitude factor that makes the tap energy (sum of gains squared, canopy closed) of a source kRoomRefDistanceM
// straight ahead equal earlyDb.
inline float EarlyNorm(const RoomParams& p) {
    Tap t[kRoomTaps];
    const float ref[3] = {kRoomRefDistanceM, 0.0f, 0.0f};
    ComputeTaps(p, 0.0f, ref, 0, t);
    float e = 0.0f;
    for (const Tap& x : t) e += x.gain * x.gain;
    return e > 1e-12f ? std::sqrt(std::pow(10.0f, p.earlyDb / 10.0f) / e) : 0.0f;
}

// The largest single reflection, re the direct sound (after the early normalisation and the send's distance law): 3 dB
// under the preset's whole early energy (Normal -15 dB, Subtle -19 dB, Strong -12 dB). A source close to a wall (the
// canopy 5 cm above the helmet) would otherwise get one reflection a few dB under the direct sound a fraction of a
// millisecond later: a deep comb filter, heard as "tin can" (docs/COCKPIT_SPACE.md section 1; R9 measured 5 dB of
// ripple at Normal without it).
inline float TapCap(const RoomParams& p) { return std::pow(10.0f, (p.earlyDb - 3.0f) / 20.0f); }
// A tap's gain on the voice's send after the cap: k x raw gain, limited so that k x raw x distance gain <= cap.
inline float CappedTapGain(float normalised, float distanceGain, float cap) {
    return distanceGain > 1e-6f ? std::min(normalised * distanceGain, cap) / distanceGain : normalised;
}

// ---- the late tail's energy (section 4.3) ---------------------------------------------------------------------------
inline float LineGain(std::uint32_t lengthSamples, float rt60S, float fs) {
    return std::pow(10.0f, -3.0f * static_cast<float>(lengthSamples) / (std::max(rt60S, 0.01f) * fs));
}
// The RT60s with the canopy open by c (section 4.5: x (1 - 0.4 c), not for helicopters).
inline float CanopyRt60(const RoomParams& p, float rt60, float c) {
    return p.family == Family::Helicopter ? rt60 : rt60 * (1.0f - 0.4f * std::clamp(c, 0.0f, 1.0f));
}
inline float CanopyLate(float c) { return 1.0f - 0.7f * std::clamp(c, 0.0f, 1.0f); }
// Energy of the FDN's output for a unit input (up to the constant the Room calibrates): sum over the two bands of the
// white-noise share of that band / (1 - mean line gain squared), lengths at 48 kHz.
inline float FdnEnergy(float rtLow, float rtHigh, float crossoverHz) {
    float lo = 0.0f, hi = 0.0f;
    for (std::uint32_t l : kFdnLengths48) {
        const float gl = LineGain(l, rtLow, kModelRate), gh = LineGain(l, rtHigh, kModelRate);
        lo += gl * gl, hi += gh * gh;
    }
    lo /= kRoomFdnLines, hi /= kRoomFdnLines;
    const float wLo = std::clamp(crossoverHz / (0.5f * kModelRate), 0.0f, 1.0f);
    return wLo / (1.0f - lo) + (1.0f - wLo) / (1.0f - hi);
}

// Per-voice amplitude factors of the sends: through the canopy (throughDb) and diffuse beds (-3 dB on the late send).
inline float ThroughGain(const RoomParams& p, std::uint32_t flags) {
    return (flags & room_flags::kThroughCanopy) ? std::pow(10.0f, p.throughDb / 20.0f) : 1.0f;
}
constexpr float kLateOnlyGain = 0.70794578f;  // -3 dB

inline float SendDistanceGain(const RoomParams& p, const float pos[3], std::uint32_t flags) {
    if (flags & room_flags::kThroughCanopy) return 1.0f;
    if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) return 1.0f;
    if (!InsideBox(p, pos)) return 1.0f;
    const float r = std::sqrt(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]);
    return std::clamp(r / kRoomRefDistanceM, 0.5f, 2.0f);
}

// The wet energies of a voice relative to its direct sound (white noise, the model rate), wet weight not applied.
struct WetEnergy {
    float early, late;
};
// What VoiceWetEnergy needs that depends on the parameters and the canopy only (not on the voice): computed once and
// reused while they stay the same (RoomDirectGainMix keeps the last one per thread).
struct WetConsts {
    float K = 0.0f, cap = 0.0f, hp = 1.0f, late = 0.0f, through = 1.0f;
    float tapPower[kRoomTaps] = {};  // each tap's wall filters' white-noise power gain
};
inline WetConsts ComputeWetConsts(const RoomParams& p, float canopy) {
    WetConsts w;
    const float c = std::clamp(std::isfinite(canopy) ? canopy : 0.0f, 0.0f, 1.0f);
    w.K = EarlyNorm(p), w.cap = TapCap(p), w.hp = SendHighpassPower(p.sendHpHz, kModelRate);
    w.through = std::pow(10.0f, p.throughDb / 20.0f);
    for (std::uint32_t k = 0; k < kRoomTaps; ++k) {
        float lp = 0.0f, hpz = 0.0f;
        for (int j = 0; j < 2; ++j) {
            const int sf = kTapSurface[k][j];
            if (sf < 0) continue;
            const SurfaceParams& sp = p.surface[sf];
            if (sp.lowpassHz > 0.0f) lp = lp > 0.0f ? std::min(lp, sp.lowpassHz) : sp.lowpassHz;
            hpz = std::max(hpz, sp.highpassHz);
        }
        w.tapPower[k] = LowpassPower(OnePoleLowpassCoef(lp, kModelRate)) * HighpassPower(OnePoleHighpassCoef(hpz, kModelRate));
    }
    float late = std::pow(10.0f, p.lateDb / 10.0f);
    if (c > 0.0f) {
        const float cl = CanopyLate(c);
        late *= cl * cl;
        if (p.family != Family::Helicopter)
            late *= FdnEnergy(CanopyRt60(p, p.rt60LowS, c), CanopyRt60(p, p.rt60HighS, c), p.crossoverHz) /
                    FdnEnergy(p.rt60LowS, p.rt60HighS, p.crossoverHz);
    }
    w.late = late;
    return w;
}
inline WetEnergy VoiceWetEnergy(const RoomParams& p, const WetConsts& wc, float canopy, bool early, const float pos[3],
                                std::uint32_t flags) {
    const float sd = SendDistanceGain(p, pos, flags);
    const float s = sd * ((flags & room_flags::kThroughCanopy) ? wc.through : 1.0f);
    WetEnergy w{0.0f, 0.0f};
    const float c = std::clamp(std::isfinite(canopy) ? canopy : 0.0f, 0.0f, 1.0f);
    if (early && !(flags & room_flags::kLateOnly)) {
        Tap t[kRoomTaps];
        ComputeTaps(p, c, pos, flags, t);
        float e = 0.0f;
        for (std::uint32_t k = 0; k < kRoomTaps; ++k) {
            const float g = CappedTapGain(t[k].gain * wc.K, sd, wc.cap);
            e += g * g * wc.tapPower[k];
        }
        w.early = e * s * s * wc.hp;
    }
    float late = wc.late * s * s * wc.hp;
    if (flags & room_flags::kLateOnly) late *= kLateOnlyGain * kLateOnlyGain;
    w.late = late;
    return w;
}
inline WetEnergy VoiceWetEnergy(const RoomParams& p, float canopy, bool early, const float pos[3], std::uint32_t flags) {
    return VoiceWetEnergy(p, ComputeWetConsts(p, canopy), canopy, early, pos, flags);
}

}  // namespace binaural::room_model

// The offline harness's read-only probes of a Room (implemented in room.cpp); not an API.
namespace binaural::room_probe {
const Renderer& Bus(const Room& r);                          // the bus Renderer (filter directions per bus slot)
void BusHead(const Room& r, std::uint32_t b, float out[3]);  // bus b's head-frame direction of the last block
float Kappa(const Room& r);                                  // late calibration factor
void ForceRoomKernels(int which);                            // 0: by the CPU (AVX2/FMA when present), 1: SSE2; read at Init
void ForceBusTaps(int taps48);                               // 0: kRoomBusTaps48; read at SetHrtf
void ForceTapMinShare(float share);                          // < 0: kRoomTapMinShare; read per AddSource
void ForceOwnTaps(int voices);                               // < 0: kRoomOwnTaps (0: every voice its own); per block
void ForceNoZeroSkip(bool off);                              // true: taps run over a silent line too (C19)
void ForceMatchDb(const float* db);                          // the level match held at db[3]; null: measured
float HNorm(const Room& r);                                  // early (bus) calibration gain
float EFront(const Room& r);
float EDiff(const Room& r);
void Eq(const Room& r, int ear, void* out3);                 // 3 room_fdn::Biquad of the current diffuse EQ
float EarlyMix(const Room& r);
bool BusActive(const Room& r);
bool LateActive(const Room& r);
std::uint32_t FdnLength(const Room& r, std::uint32_t l);
void Profile(Room& r, std::uint64_t out[6]);                 // TSC cycles per Render section since the last call
double BusKept(const Room& r);                               // the smallest energy share the buses' cut filters keep
}  // namespace binaural::room_probe
