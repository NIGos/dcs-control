// BinauralCore: the cockpit space Room (include/binaural_room.h, docs/COCKPIT_SPACE.md 4).
//
// Per block: BeginBlock adopts new parameters / HRTF data (seqlocks, bounded), advances the smoothed values (wet, early,
// canopy, the 100 ms parameter interpolation) and computes the block's derived values; AddSource writes each voice's
// weighted send into its slot's delay line and the shared late input, reads its 8 taps (whole-sample delays) and adds
// them into per-(tap, bus) accumulators with ramped VBAP gains; Render runs each accumulator through its wall's filters
// into its bus, high-passes the buses, renders them
// through the bus Renderer (one far-field HRTF source each, aircraft-fixed directions rotated into the head frame) and
// runs the late tail (pre-delay, send high-pass, FDN, coherence mixer, diffuse EQ, gain), adding both to the output.
#include <immintrin.h>
#include <intrin.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include "binaural_room.h"
#include "dsp.h"
#include "internal.h"
#include "room_fdn.h"
#include "room_model.h"

namespace binaural {

namespace rm = room_model;
namespace rf = room_fdn;

namespace {

constexpr std::uint32_t kChunk = kSubBlockFrames;     // per-voice processing step (delay line sized for it)
constexpr float kPreDelayMaxMs = 10.0f;
constexpr float kSilence = 1e-14f;                     // -140 dB mean power per sample
constexpr float kRefRt60Low = 0.12f, kRefRt60High = 0.09f;  // the late calibration's reference decay
constexpr std::uint32_t kDrainMargin = 64;             // frames past the longest tap for the wall filters to settle
// The level match's floor: under this mean power per frame (both ears, the direct sound) the measure holds (-110 dB).
constexpr double kMatchFloor = 1e-11;
// The buses' HRTF filters are cut to this many taps at 48 kHz (scaled with the rate, a multiple of 8): the reflections
// are 15-25 dB under the direct sound and high-passed at 200 Hz; the minimum-phase filters keep at least 98.3 % of their
// energy in the first 1.3 ms (KU100, worst direction and ear; the early calibration takes the rest). Half the buses'
// FIR cost for the KU100's 128 taps (docs/COCKPIT_SPACE.md 9).
constexpr std::uint32_t kBusTaps48 = kRoomBusTaps48;
// A voice within this of the position its taps were computed for keeps them (0.5 mm: 1.5 us of path, 0.07 samples).
constexpr float kTapReuseM = 0.0005f;

// Per-block buffers are packed at this stride (frames rounded up to 16, plus 16): never a multiple of 4 KB, so the
// accumulators, buses and lines touched together do not share cache sets (a stride of kMaxFramesPerCall, 32 KB, put
// every accumulator of a frame in the same L1 and L2 sets).
inline std::uint32_t PackedStride(std::uint32_t frames) { return ((frames + 15u) & ~15u) + 16u; }

// True when any of n floats is not finite: (x * 0) is +-0 for a finite x and NaN otherwise; OR-ing the bit patterns
// (no dependency chain through a float add) and testing the exponent bits.
inline bool AnyNonFinite(const float* p, std::size_t n) {
    const __m128 zero = _mm_setzero_ps();
    __m128 acc = zero;
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) acc = _mm_or_ps(acc, _mm_mul_ps(_mm_loadu_ps(p + i), zero));
    float t[4];
    _mm_storeu_ps(t, acc);
    std::uint32_t bits = 0;
    for (float v : t) {
        std::uint32_t u;
        std::memcpy(&u, &v, 4);
        bits |= u;
    }
    for (; i < n; ++i) {
        const float v = p[i] * 0.0f;
        std::uint32_t u;
        std::memcpy(&u, &v, 4);
        bits |= u;
    }
    return (bits & 0x7fffffffu) != 0;
}

// The tap mix, SSE2 (the AVX2/FMA one is kernels::RoomMixAvx2): acc[q][i] += (gs[q] + dg[q] i) src[i], i < n.
void RoomMixSse2(const float* src, float* const* acc, const float* gs, const float* dg, int nu, std::uint32_t n) {
    __m128 gv[6], step[6];
    for (int q = 0; q < nu; ++q) {
        gv[q] = _mm_setr_ps(gs[q], gs[q] + dg[q], gs[q] + 2.0f * dg[q], gs[q] + 3.0f * dg[q]);
        step[q] = _mm_set1_ps(4.0f * dg[q]);
    }
    std::uint32_t i = 0;
    if (nu == 3) {
        __m128 ga = gv[0], gb = gv[1], gc = gv[2];
        float* pa = acc[0];
        float* pb = acc[1];
        float* pc = acc[2];
        for (; i + 4 <= n; i += 4) {
            const __m128 tv = _mm_loadu_ps(src + i);
            _mm_storeu_ps(pa + i, _mm_add_ps(_mm_loadu_ps(pa + i), _mm_mul_ps(ga, tv)));
            _mm_storeu_ps(pb + i, _mm_add_ps(_mm_loadu_ps(pb + i), _mm_mul_ps(gb, tv)));
            _mm_storeu_ps(pc + i, _mm_add_ps(_mm_loadu_ps(pc + i), _mm_mul_ps(gc, tv)));
            ga = _mm_add_ps(ga, step[0]), gb = _mm_add_ps(gb, step[1]), gc = _mm_add_ps(gc, step[2]);
        }
    } else {
        for (; i + 4 <= n; i += 4) {
            const __m128 tv = _mm_loadu_ps(src + i);
            for (int q = 0; q < nu; ++q) {
                _mm_storeu_ps(acc[q] + i, _mm_add_ps(_mm_loadu_ps(acc[q] + i), _mm_mul_ps(gv[q], tv)));
                gv[q] = _mm_add_ps(gv[q], step[q]);
            }
        }
    }
    for (; i < n; ++i)
        for (int q = 0; q < nu; ++q) acc[q][i] += (gs[q] + dg[q] * static_cast<float>(i)) * src[i];
}
std::atomic<int> g_forceRoomKernels{0};  // harness: 0 by the CPU, 1 SSE2
// Harness: the buses' filter length at 48 kHz (0: kRoomBusTaps48), read at SetHrtf; the per-voice tap threshold
// (negative: kRoomTapMinShare), read per AddSource. For the trade-off measures (C17); never set by the module.
std::atomic<int> g_forceBusTaps{0};
std::atomic<float> g_forceTapMinShare{-1.0f};
std::atomic<int> g_forceOwnTaps{-1};
std::atomic<bool> g_forceNoZeroSkip{false};  // harness: the silent-tap skip off (C19's bit-exactness reference)
// Harness: the level match's gains held at these (dB) instead of measured; NaN in [0]: measured (C18).
std::atomic<float> g_forceMatchDb[3] = {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};

// A seqlock mailbox: the control thread writes (serialised by a mutex), the audio thread reads a consistent copy in a
// bounded number of tries (and keeps its old copy when it cannot).
template <class T>
class Mailbox {
public:
    void Write(const T& v) {
        std::lock_guard<std::mutex> g(m_);
        seq_.fetch_add(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        data_ = v;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.fetch_add(1, std::memory_order_release);
    }
    // True when a value newer than *seen was copied to *out.
    bool ReadIfNew(T* out, std::uint32_t* seen) const {
        for (int tries = 0; tries < 4; ++tries) {
            const std::uint32_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 == *seen) return false;
            if (s1 & 1u) continue;
            T tmp = data_;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s1) {
                *out = tmp;
                *seen = s1;
                return true;
            }
        }
        return false;
    }

private:
    std::mutex m_;
    std::atomic<std::uint32_t> seq_{0};
    T data_{};
};

struct HrtfDerived {
    bool valid = false;
    float eFront = 1.0f;   // both ears' energy of the set's front filter
    float eDiff = 1.0f;    // both ears' energy averaged over the set's directions
    float kappa = 1.0f;    // the late chain's measured energy / room_model::FdnEnergy at the reference decay
    // The same ratio at RT60 (high band; low band x 1.33, as the presets) 0.05, 0.1, 0.2, 0.4, 0.8 s: the closed form
    // misses the decay's effect on the energy by up to 2 dB between 0.1 and 0.25 s (measured).
    float kappaT[5] = {1, 1, 1, 1, 1};
    float hNorm = 1.0f;    // bus gain that makes the rendered early energy the model's (CalibrateEarly)
    rf::Biquad eq[2][3];   // diffuse-field EQ per ear: low shelf, high shelf, peak; 0 dB at 1 kHz
};

// Values derived from the effective parameters, the canopy and the HRTF data, once per block.
struct Derived {
    float K = 0.0f;                         // early normalisation
    float lpA[kRoomTaps] = {}, hpB[kRoomTaps] = {};
    float gLo[kRoomFdnLines] = {}, gHi[kRoomFdnLines] = {};
    float xA = 1.0f;
    float lateGain = 0.0f;
    std::int32_t preDelay = 0;
    rf::Biquad sendHp[2];  // fourth-order Butterworth high-pass at sendHpHz (two sections)
    float hNorm = 1.0f;
    float cap = 0.0f;      // room_model::TapCap
    float through = 1.0f;  // throughDb as a gain
};

// The level match (docs/COCKPIT_SPACE.md 6.2): band split, smoothed band energies of the direct sound and of direct +
// room, the gains (dB) at full wet, and the voices' energy-weighted d^2 over the same window.
struct LevelMatch {
    // The measure's band filters, Butterworth: [0] low band (fourth-order low-pass at kRoomMatchLowHz, two sections),
    // [1] middle band (second-order high-pass at kRoomMatchLowHz, then second-order low-pass at kRoomMatchHighHz),
    // [2] high band (fourth-order high-pass at kRoomMatchHighHz, two sections). Lanes: direct L, R, room L, R.
    rf::Biquad band[kRoomMatchBands][2];
    float z[kRoomMatchBands][2][2][4] = {};  // [band][section][z1, z2][lane]
    // The correction, on direct + room (lanes L, R): the middle band's gain times a low shelf at kRoomMatchLowHz and a
    // high shelf at kRoomMatchHighHz (the other two bands relative to it), redesigned per block (the gains move slowly).
    float eqZ[2][2][4] = {};                 // [shelf][z1, z2][lane]
    double sD[kRoomMatchBands] = {}, sT[kRoomMatchBands] = {};
    double sW = 0.0, sWd = 0.0;
    bool primed = false;
    float gainDb[kRoomMatchBands] = {};    // at full wet
    float appliedPrev[kRoomMatchBands] = {1.0f, 1.0f, 1.0f};  // linear, as applied at the end of the last block
    std::uint64_t lastBlock = 0;  // the block MatchMix last ran in: a gap (the room idled) restarts the filters at bypass
};
// The match's shelves: designed for the block's start gains and its end gains, the coefficients interpolated per
// sample between the two (no step at a block's edge while the gains or the wet weight move).
// RBJ low / high shelf (S = 1) for a gain in dB, with the corner's cos and sin given (they do not change).
inline rf::Biquad ShelfFast(bool high, float db, float c, float sn) {
    const float A = std::pow(10.0f, db / 40.0f), al = sn * 0.70710678f, sa = 2.0f * std::sqrt(A) * al;
    rf::Biquad q;
    float a0;
    if (!high) {
        q.b0 = A * ((A + 1) - (A - 1) * c + sa), q.b1 = 2 * A * ((A - 1) - (A + 1) * c), q.b2 = A * ((A + 1) - (A - 1) * c - sa);
        a0 = (A + 1) + (A - 1) * c + sa, q.a1 = -2 * ((A - 1) + (A + 1) * c), q.a2 = (A + 1) + (A - 1) * c - sa;
    } else {
        q.b0 = A * ((A + 1) + (A - 1) * c + sa), q.b1 = -2 * A * ((A - 1) + (A + 1) * c), q.b2 = A * ((A + 1) + (A - 1) * c - sa);
        a0 = (A + 1) - (A - 1) * c + sa, q.a1 = 2 * ((A - 1) - (A + 1) * c), q.a2 = (A + 1) - (A - 1) * c - sa;
    }
    const float inv = 1.0f / a0;
    q.b0 *= inv, q.b1 *= inv, q.b2 *= inv, q.a1 *= inv, q.a2 *= inv;
    return q;
}

struct Slot {
    float* line = nullptr;
    std::uint32_t w = 0;
    bool active = false;
    std::uint64_t lastBlock = 0;
    float lp[kRoomTaps] = {}, hp[kRoomTaps] = {};
    std::int32_t delay[kRoomTaps] = {};
    float bus[kRoomTaps][kRoomBuses] = {};
    float send = 0.0f, late = 0.0f;
    std::int32_t drain = 0;
    // The last targets' inputs: a voice that did not move (in a block where nothing else that shapes the taps changed)
    // keeps its taps without computing them again.
    // The shared tap line (kRoomOwnTaps): this slot's send energy in the last block it was added, its rank among the
    // block's voices, and whether it went to the shared line (its send ramps then, as for its own line).
    double lastEnergy = 0.0;
    std::uint64_t energyBlock = 0;
    std::uint32_t rank = ~0u;
    bool shared = false;
    std::uint64_t sharedBlock = 0;
    float shSend = 0.0f, shLate = 0.0f;
    bool tapsValid = false;
    float tapPos[3] = {};
    std::uint32_t tapFlags = 0;
    float tapSd = 1.0f;  // frames of silence still to run after the voice ended (its reflections play out)
    // The newest samples of the line that are all zero (saturating at the line's size): once they reach past the
    // longest tap, every tap reads zeros and its work (adding 0 x gain) is skipped, the same output to the bit.
    std::uint32_t zeroRun = 0;
};

inline void RotateQ(const float q[4], const float v[3], float out[3]) {
    // v' = v + 2 w (q x v) + 2 q x (q x v)
    const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
    const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
    const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

float LerpLogHz(float a, float b, float x, float none) {
    // 0 Hz means "no filter": interpolated as `none` (1 Hz for a high-pass, 24 kHz for a low-pass).
    const float la = std::log(a > 0.0f ? a : none), lb = std::log(b > 0.0f ? b : none);
    return std::exp(la + (lb - la) * x);
}

RoomParams LerpParams(const RoomParams& a, const RoomParams& b, float x) {
    if (x >= 1.0f) return b;
    if (x <= 0.0f) return a;
    RoomParams p = b;
    auto L = [x](float u, float v) { return u + (v - u) * x; };
    p.family = x < 0.5f ? a.family : b.family;
    p.strength = x < 0.5f ? a.strength : b.strength;
    for (int s = 0; s < cockpit_space::kSurfaces; ++s) {
        p.surface[s].distanceM = L(a.surface[s].distanceM, b.surface[s].distanceM);
        p.surface[s].reflection = L(a.surface[s].reflection, b.surface[s].reflection);
        const float lp = LerpLogHz(a.surface[s].lowpassHz, b.surface[s].lowpassHz, x, 24000.0f);
        p.surface[s].lowpassHz = lp >= 20000.0f ? 0.0f : lp;
        const float hp = LerpLogHz(a.surface[s].highpassHz, b.surface[s].highpassHz, x, 1.0f);
        p.surface[s].highpassHz = hp <= 1.5f ? 0.0f : hp;
    }
    p.earlyDb = L(a.earlyDb, b.earlyDb);
    p.lateDb = L(a.lateDb, b.lateDb);
    p.rt60LowS = L(a.rt60LowS, b.rt60LowS);
    p.rt60HighS = L(a.rt60HighS, b.rt60HighS);
    p.crossoverHz = L(a.crossoverHz, b.crossoverHz);
    p.preDelayMs = L(a.preDelayMs, b.preDelayMs);
    p.sendHpHz = LerpLogHz(a.sendHpHz, b.sendHpHz, x, 20.0f);
    p.throughDb = L(a.throughDb, b.throughDb);
    p.neutralShare = L(a.neutralShare, b.neutralShare);
    return p;
}

// ---- control thread: what the room takes from an HRTF set ---------------------------------------------------------
double SmoothDb(const std::vector<double>& pw, std::size_t n, double fs, double f, double ref) {
    const double lo = f / std::pow(2.0, 1.0 / 6.0), hi = f * std::pow(2.0, 1.0 / 6.0);
    double s = 0.0;
    int c = 0;
    for (std::size_t k = 1; k <= n / 2; ++k) {
        const double fk = double(k) * fs / double(n);
        if (fk >= lo && fk < hi) s += pw[k], ++c;
    }
    if (c == 0) {
        const std::size_t k = std::min<std::size_t>(n / 2, static_cast<std::size_t>(std::lround(f * n / fs)));
        s = pw[std::max<std::size_t>(k, 1)], c = 1;
    }
    return 10.0 * std::log10(std::max(s / c, 1e-30) / std::max(ref, 1e-30));
}

void FitEq(const std::vector<double>& pw, std::size_t n, double fs, rf::Biquad out[3]) {
    double ref = 0.0;
    int rc = 0;
    for (std::size_t k = 1; k <= n / 2; ++k) {
        const double fk = double(k) * fs / double(n);
        if (fk >= 900.0 && fk <= 1100.0) ref += pw[k], ++rc;
    }
    ref = rc ? ref / rc : pw[std::max<std::size_t>(1, static_cast<std::size_t>(1000.0 * n / fs))];
    const double nyq = 0.45 * fs;
    double low = 0.0;
    int lc = 0;
    for (double f : {100.0, 125.0, 160.0, 200.0}) low += SmoothDb(pw, n, fs, f, ref), ++lc;
    low = std::clamp(low / lc, -12.0, 12.0);
    double high = 0.0;
    int hc = 0;
    for (double f : {9000.0, 10000.0, 11200.0, 12500.0})
        if (f * std::pow(2.0, 1.0 / 6.0) < nyq) high += SmoothDb(pw, n, fs, f, ref), ++hc;
    high = hc ? std::clamp(high / hc, -12.0, 12.0) : 0.0;
    out[0] = rf::LowShelf(250.0, low, fs);
    out[1] = rf::HighShelf(std::min(7000.0, 0.3 * fs), high, fs);
    double bestF = 0.0, bestR = 0.0;
    for (double f = 1500.0; f <= std::min(7000.0, nyq); f *= std::pow(2.0, 1.0 / 12.0)) {
        const double shelves = 20.0 * std::log10(rf::Magnitude(out[0], f, fs) * rf::Magnitude(out[1], f, fs));
        const double r = SmoothDb(pw, n, fs, f, ref) - shelves;
        if (std::fabs(r) > std::fabs(bestR)) bestR = r, bestF = f;
    }
    out[2] = std::fabs(bestR) > 0.5 ? rf::Peak(bestF, std::clamp(bestR, -10.0, 10.0), 1.2, fs) : rf::Biquad{};
    // 0 dB at 1 kHz.
    const double g = rf::Magnitude(out[0], 1000.0, fs) * rf::Magnitude(out[1], 1000.0, fs) * rf::Magnitude(out[2], 1000.0, fs);
    const float s = static_cast<float>(1.0 / std::max(g, 1e-6));
    out[0].b0 *= s, out[0].b1 *= s, out[0].b2 *= s;
}

// The late chain's energy (both ears, after the mixer and the EQ) for a unit impulse into the FDN at the reference decay,
// divided by the model's FdnEnergy at that decay.
float MeasureKappa(std::uint32_t fs, const rf::Biquad eq[2][3], float rtLow = kRefRt60Low, float rtHigh = kRefRt60High) {
    rf::Fdn fdn;
    if (!fdn.Init(fs)) return 1.0f;
    rf::Mixer mx;
    mx.Init(fs);
    float gLo[kRoomFdnLines], gHi[kRoomFdnLines];
    rf::LineGains(fdn, rtLow, rtHigh, static_cast<float>(fs), gLo, gHi);
    const float xA = rm::OnePoleLowpassCoef(1000.0f, static_cast<float>(fs));
    const std::uint32_t n = 1024, total = static_cast<std::uint32_t>(fs * std::max(1.5f, 5.0f * std::max(rtLow, rtHigh)));
    std::vector<float> in(n, 0.0f), o1(n), o2(n);
    rf::BiquadState st[2][3];
    double e = 0.0;
    for (std::uint32_t done = 0; done < total; done += n) {
        in[0] = done == 0 ? 1.0f : 0.0f;
        fdn.Process(in.data(), o1.data(), o2.data(), n, gLo, gLo, gHi, gHi, xA);
        for (std::uint32_t i = 0; i < n; ++i) {
            float l, r;
            mx.TickPair(o1[i], o2[i], &l, &r);
            for (int k = 0; k < 3; ++k) l = rf::Tick(eq[0][k], st[0][k], l), r = rf::Tick(eq[1][k], st[1][k], r);
            e += double(l) * l + double(r) * r;
        }
    }
    const double model = rm::FdnEnergy(rtLow, rtHigh, 1000.0f);
    return static_cast<float>(e / std::max(model, 1e-9));
}

// The early reflections' rendered energy against the model's (control thread). The model adds the taps' energies
// (each tap's VBAP gains have a sum of squares of 1, so each tap carries its own energy) relative to the set's
// diffuse-field energy; rendered, the three bus filters of a tap are correlated below a few kHz and taps of similar
// delay add partly in phase, so the real energy is higher (+1 to +3 dB for median sources on the synthetic set). The
// taps of the generic preset for the reference source and the five cockpit sources of the harness are rendered here
// through a bus Renderer on this set, unfiltered, and the bus gain is set so that their summed energy equals the
// model's: RoomDirectGain's incoherent model then holds on average for this set.
float CalibrateEarly(const HrtfSet& set, float eDiff) {
    RoomParams p{};
    cockpit_space::Preset(cockpit_space::Family::Generic, cockpit_space::kStrengthNormal, &p);
    const float positions[6][3] = {{kRoomRefDistanceM, 0, 0}, {0.39f, 0.225f, 0}, {0.15f, -0.25f, -0.41f},
                                   {0.15f, -0.25f, 0.41f},     {0.75f, -0.28f, 0}, {-0.30f, 0.18f, 0}};
    Renderer r;
    Config c;
    c.sampleRate = set.sampleRate;
    c.maxSources = kRoomBuses;
    c.nearField = false;
    char err[64];
    if (!r.Init(c, err, sizeof(err))) return 1.0f;
    r.SetHrtf(&set);
    const std::uint32_t n = 4096;
    std::vector<float> bus(std::size_t(kRoomBuses) * n), out(std::size_t(n) * 2);
    const float k = rm::EarlyNorm(p), cap = rm::TapCap(p);
    double model = 0.0, actual = 0.0;
    for (const auto& pos : positions) {
        std::fill(bus.begin(), bus.end(), 0.0f);
        std::fill(out.begin(), out.end(), 0.0f);
        rm::Tap taps[kRoomTaps];
        rm::ComputeTaps(p, 0.0f, pos, 0, taps);
        const float sd = rm::SendDistanceGain(p, pos, 0);
        for (const rm::Tap& t : taps) {
            const float g = rm::CappedTapGain(t.gain * k, sd, cap);
            if (g <= 0.0f) continue;
            std::uint32_t idx[3];
            float pg[3];
            rm::Pan(t.u, idx, pg);
            const std::uint32_t at = static_cast<std::uint32_t>(std::lround(t.delayS * set.sampleRate));
            for (int j = 0; j < 3; ++j) bus[std::size_t(idx[j]) * n + at] += pg[j] * g;
            model += double(g) * g * eDiff;
        }
        r.BeginBlock(n);
        for (std::uint32_t b = 0; b < kRoomBuses; ++b) {
            float d[3];
            rm::BusDirection(b, d);
            SourceUpdate u{{d[0] * kRoomBusDistanceM, d[1] * kRoomBusDistanceM, d[2] * kRoomBusDistanceM}, 1.0f,
                           source_flags::kNoNearField | source_flags::kReset};
            MonoInput mi{bus.data() + std::size_t(b) * n, nullptr, 1, 1.0f};
            r.Render(b, u, mi, n, 1.0f, 1.0f, out.data());
        }
        r.EndBlock();
        for (float v : out) actual += double(v) * v;
    }
    return actual > 0.0 && model > 0.0 ? static_cast<float>(std::sqrt(model / actual)) : 1.0f;
}

HrtfDerived ComputeHrtfDerived(const HrtfSet& set, const HrtfSet& busSet) {
    HrtfDerived d;
    const std::uint32_t taps = set.taps;
    // Front: the interpolated filter straight ahead, as the voices' renderer builds it.
    {
        const float f[3] = {1.0f, 0.0f, 0.0f};
        std::int32_t cache = -1;
        std::uint32_t idx[3];
        float w[3];
        Locate(set, f, &cache, idx, w);
        double e = 0.0;
        for (int ear = 0; ear < 2; ++ear)
            for (std::uint32_t t = 0; t < taps; ++t) {
                double v = 0.0;
                for (int j = 0; j < 3; ++j) v += double(w[j]) * set.Ir(idx[j], ear)[t];
                e += v * v;
            }
        d.eFront = static_cast<float>(e);
    }
    const std::size_t n = std::max<std::size_t>(512, dsp::NextPow2(std::size_t(taps) * 2));
    std::vector<double> pw[2] = {std::vector<double>(n / 2 + 1, 0.0), std::vector<double>(n / 2 + 1, 0.0)};
    const std::uint32_t stride = std::max<std::uint32_t>(1, set.directions / 1500);
    double eSum = 0.0;
    std::uint32_t count = 0;
    for (std::uint32_t dir = 0; dir < set.directions; dir += stride, ++count) {
        for (int ear = 0; ear < 2; ++ear) {
            const float* ir = set.Ir(dir, ear);
            for (std::uint32_t t = 0; t < taps; ++t) eSum += double(ir[t]) * ir[t];
            const std::vector<double> m = dsp::Magnitude(ir, taps, n);
            for (std::size_t k = 0; k < m.size(); ++k) pw[ear][k] += m[k] * m[k];
        }
    }
    d.eDiff = count ? static_cast<float>(eSum / count) : d.eFront;
    for (int ear = 0; ear < 2; ++ear) FitEq(pw[ear], n, set.sampleRate, d.eq[ear]);
    d.kappa = MeasureKappa(set.sampleRate, d.eq);
    for (int i = 0; i < 5; ++i) {
        const float t = 0.05f * static_cast<float>(1 << i);
        d.kappaT[i] = MeasureKappa(set.sampleRate, d.eq, 1.33f * t, t);
    }
    d.hNorm = CalibrateEarly(busSet, d.eDiff);  // the buses' own (shortened) filters
    d.valid = d.eFront > 0.0f && d.eDiff > 0.0f && std::isfinite(d.kappa) && d.kappa > 0.0f;
    return d;
}

}  // namespace

// ---- Impl ------------------------------------------------------------------------------------------------------------
struct Room::Impl {
    std::uint32_t fs = 0, maxSources = 0;
    std::uint32_t lineSize = 0, lineMask = 0;
    std::int32_t maxTapDelay = 0;
    std::uint32_t preSize = 0, preMask = 0, preW = 0;
    std::uint32_t busTail = 0;
    bool inited = false;
    double qpcToNs = 0.0;
    kernels::RoomMixFn mix = &RoomMixSse2;
    kernels::RoomPairsFn pairs = nullptr;  // AVX2/FMA pair filters, null: the SSE2 ones here

    // The buses' shortened copies of the voices' sets (control thread; declared before `bus`, freed by ~Room after it).
    struct BusSet {
        const HrtfSet* orig;
        HrtfSet* copy;  // null: the bus renderer got `orig` itself (no memory for a copy)
    };
    std::vector<BusSet> busSets;
    std::vector<const HrtfSet*> retiredOrig;
    double busKeptWorst = 1.0;

    Renderer bus;
    std::unique_ptr<float[]> lines, busBuf, late, x1, x2, tmp, pre, wetBuf, earlyBuf;
    // Early reflections are mixed per (tap, bus) pair over all voices; each pair runs through its wall's filters once in
    // Render (the filters are linear and the same for every voice). The pairs written in a block get accumulators packed
    // in the order of first use (accIx: the pair's index in this block, -1 none; accList: the pairs in that order).
    std::unique_ptr<float[]> accBuf, accZero, accSink, vin;
    std::int16_t accIx[kRoomTaps][kRoomBuses];
    std::uint16_t accList[kRoomTaps * kRoomBuses] = {};
    std::uint32_t accUsed = 0;
    std::uint32_t pstride = 0;  // PackedStride(frames) of this block: accumulators and buses
    float pairHp[kRoomTaps][kRoomBuses] = {}, pairLp[kRoomTaps][kRoomBuses] = {};
    std::vector<Slot> slots;
    rf::Fdn fdn;
    rf::Mixer mixer;
    rf::BiquadState earlyHp[2][2], lateHp[2];  // the send high-pass on the buses' binaural output [ear][section], the tail's
    LevelMatch match;
    double wSum = 0.0, wdSum = 0.0;
    // The shared tap line of the quieter voices: its slot (index maxSources), this block's summed input, and the
    // energy-weighted position of what went into it (this block), its through-canopy share.
    std::unique_ptr<float[]> sharedIn;
    bool sharedThisBlock = false;
    double shPos[3] = {}, shE = 0.0, shThroughE = 0.0;
    std::uint32_t ownTaps = kRoomOwnTaps;
    double ownEnergy = 0.0;  // the last block's send energy of the kRoomOwnTaps-th loudest voice
    void TapTargets(const RoomParams& p, const float pos[3], std::uint32_t flags, float sd, std::int32_t* delayT,
                    float (*busT)[kRoomBuses]);
    // Time per Render section (TSC cycles, summed since Init; RoomStats::sectionUs, room_probe::Profile): drain, wall
    // filters, bus HRTF renders, early high-pass, tail, output / level match. tscPerUs measured at Init.
    std::atomic<std::uint64_t> prof[kRoomSections] = {};
    std::atomic<std::uint64_t> profMax[kRoomSections] = {};  // the largest one block took, since ResetSectionMax
    double tscPerUs = 0.0;

    float* AccFor(std::uint32_t k, std::uint32_t j) {
        std::int16_t& ix = accIx[k][j];
        if (ix < 0) {
            ix = static_cast<std::int16_t>(accUsed);
            accList[accUsed++] = static_cast<std::uint16_t>(k * kRoomBuses + j);
        }
        return accBuf.get() + std::size_t(ix) * pstride;
    }
    void ClearAccs(std::uint32_t nf) {
        for (std::uint32_t u = 0; u < accUsed; ++u) {
            std::memset(accBuf.get() + std::size_t(u) * pstride, 0, sizeof(float) * nf);
            accIx[accList[u] / kRoomBuses][accList[u] % kRoomBuses] = -1;
        }
        accUsed = 0;
    }
    void MatchMix(float* outLR, const float* directLR, std::uint32_t nfr);
    rf::Biquad eqCoef[2][2][3];      // [set][ear][stage]
    rf::BiquadState eqState[2][2][3];
    int eqCur = 0;
    bool eqFade = false;

    Mailbox<RoomParams> paramsBox;
    Mailbox<HrtfDerived> hrtfBox;
    std::uint32_t paramsSeen = 0, hrtfSeen = 0;

    // Audio-thread state.
    bool haveParams = false, haveHrtf = false;
    RoomParams from{}, to{}, eff{};
    float xfade = 1.0f;
    bool xfadeWasDone = false;
    HrtfDerived hd;
    Derived dPrev, dCur;
    float wetPrev = 0.0f, wetCur = 0.0f, wetTarget = 0.0f;
    float earlyPrev = 0.0f, earlyCur = 0.0f;
    float canPrev = 0.0f, canCur = 0.0f;
    float C[4] = {0, 0, 0, 1};
    bool haveRotation = false;
    float busHead[kRoomBuses][3] = {};
    bool inBlock = false, blockOk = false;
    std::uint32_t frames = 0;
    std::uint64_t blockIndex = 0;
    unsigned int csr = 0;
    LARGE_INTEGER t0{};
    bool inputThisBlock = false, busInputThisBlock = false, lateInputThisBlock = false;
    bool tapsStable = false;  // this block's taps depend on the voice's position only as in the last block
    std::uint64_t framesSinceInput = ~0ull >> 1, framesSinceBus = ~0ull >> 1, framesSinceLate = ~0ull >> 1;
    bool busActive = false, lateActive = false, busReset = true;
    // The set the bus Renderer was last given (control thread) and the one it last opened a block with (audio thread):
    // BeginBlock opens an empty bus block when they differ, so the old set is retired even while the buses do not run.
    std::atomic<const HrtfSet*> busGiven{nullptr};
    const HrtfSet* busSeen = nullptr;

    std::atomic<std::uint64_t> sBlocks{0}, sAdded{0}, sEarlyOff{0}, sFrameInvalid{0}, sNonFinite{0}, sRejected{0}, sIdle{0};
    std::atomic<std::uint64_t> sLastNs{0}, sWorstNs{0};
    std::atomic<float> sWet{0.0f}, sCanopy{0.0f};
    std::atomic<float> sMatch[kRoomMatchBands] = {};

    bool Ready() const { return inited && haveParams && haveHrtf; }

    bool Run(Slot& s, const float* a, const float* b, std::uint32_t stride, float sc, std::uint32_t nf, float w0, float w1,
             const std::int32_t* delayT, const float (*busT)[kRoomBuses], float sendT, float lateT, double* inEnergy = nullptr,
             float* sumTo = nullptr, bool noWet = false);

    void ResetSlot(Slot& s) {
        if (s.line) std::memset(s.line, 0, sizeof(float) * lineSize);
        s.zeroRun = s.line ? lineSize : 0;
        std::memset(s.lp, 0, sizeof(s.lp));
        std::memset(s.hp, 0, sizeof(s.hp));
        std::memset(s.bus, 0, sizeof(s.bus));
        std::memset(s.delay, 0, sizeof(s.delay));
        s.send = s.late = 0.0f;
        s.drain = 0;
        s.active = false;
        s.tapsValid = false;
    }
    void ResetLate() {
        fdn.Reset();
        mixer.Reset();
        lateHp[0] = lateHp[1] = rf::BiquadState{};
        std::memset(eqState, 0, sizeof(eqState));
        if (pre) std::memset(pre.get(), 0, sizeof(float) * preSize);
    }

    Derived Derive(const RoomParams& p, float canopy) const {
        Derived d;
        const float f = static_cast<float>(fs);
        d.K = rm::EarlyNorm(p);
        for (std::uint32_t k = 0; k < kRoomTaps; ++k) {
            float lp = 0.0f, hp = 0.0f;
            for (int j = 0; j < 2; ++j) {
                const int s = rm::kTapSurface[k][j];
                if (s < 0) continue;
                const auto& sp = p.surface[s];
                if (sp.lowpassHz > 0.0f) lp = lp > 0.0f ? std::min(lp, sp.lowpassHz) : sp.lowpassHz;
                hp = std::max(hp, sp.highpassHz);
            }
            d.lpA[k] = rm::OnePoleLowpassCoef(lp, f);
            d.hpB[k] = rm::OnePoleHighpassCoef(hp, f);
        }
        const float rtLo = rm::CanopyRt60(p, p.rt60LowS, canopy), rtHi = rm::CanopyRt60(p, p.rt60HighS, canopy);
        rf::LineGains(fdn, rtLo, rtHi, f, d.gLo, d.gHi);
        d.xA = rm::OnePoleLowpassCoef(p.crossoverHz, f);
        // The tail's energy for a unit send at the closed-canopy decay is lateDb re the front direct sound; opening
        // the canopy lowers it by the shorter decay and by (1 - 0.7 c) (docs/COCKPIT_SPACE.md 4.5).
        const float eModel = rm::FdnEnergy(p.rt60LowS, p.rt60HighS, p.crossoverHz);
        const float lateLin = std::pow(10.0f, p.lateDb / 10.0f);
        // Relative to the direct sound of the average direction (the set's diffuse-field energy): the room's energy does not
        // depend on the voice's direction, its direct sound does, so over all directions the level stays the same.
        float kappa = hd.kappa;
        if (hd.valid) {
            // kappaT interpolated on log2(RT60 / 0.05 s), clamped to the table.
            const float x = std::clamp(std::log2(std::max(p.rt60HighS, 1e-3f) / 0.05f), 0.0f, 4.0f);
            const int i0 = std::min(static_cast<int>(x), 3);
            kappa = hd.kappaT[i0] + (hd.kappaT[i0 + 1] - hd.kappaT[i0]) * (x - static_cast<float>(i0));
        }
        d.lateGain = hd.valid ? std::sqrt(lateLin * hd.eDiff / (kappa * eModel)) * rm::CanopyLate(canopy) : 0.0f;
        d.preDelay = std::clamp(static_cast<std::int32_t>(std::lround(p.preDelayMs * 1e-3f * f)), 0,
                                static_cast<std::int32_t>(preSize) - 2);
        // Fourth order: the early reflections arrive within a few milliseconds, so below 200 Hz they add to the direct
        // sound in phase (amplitudes, not energies); a second-order send high-pass left the DCS-like mix 1.3 dB louder.
        d.sendHp[0] = rf::Highpass(p.sendHpHz, 0.54119610, fs);
        d.sendHp[1] = rf::Highpass(p.sendHpHz, 1.30656296, fs);
        d.hNorm = hd.valid ? hd.hNorm : 1.0f;
        d.cap = rm::TapCap(p);
        d.through = std::pow(10.0f, p.throughDb / 20.0f);
        return d;
    }
};

Room::Room() : impl_(new Impl) {}
Room::~Room() {
    std::vector<Impl::BusSet> sets;
    sets.swap(impl_->busSets);
    delete impl_;  // the bus Renderer first: then nothing references the copies
    for (const Impl::BusSet& b : sets)
        if (b.copy) ReleaseHrtf(b.copy);
}

bool Room::Init(std::uint32_t sampleRate, std::uint32_t maxSources, char* error, std::size_t errorCapacity) {
    auto fail = [&](const char* why) {
        if (error && errorCapacity) {
            strncpy_s(error, errorCapacity, why, _TRUNCATE);
        }
        return false;
    };
    Impl& m = *impl_;
    if (m.inited) return fail("already initialised");
    if (sampleRate < kMinSampleRate || sampleRate > kMaxSampleRate) return fail("sample rate out of range");
    if (maxSources < 1 || maxSources > kMaxSources) return fail("maxSources out of range");
    Config c;
    c.sampleRate = sampleRate;
    c.maxSources = kRoomBuses;
    c.nearField = false;
    char err[96] = {};
    if (!m.bus.Init(c, err, sizeof(err))) return fail(err);
    m.fs = sampleRate;
    m.maxSources = maxSources;
    m.maxTapDelay = static_cast<std::int32_t>(std::ceil(kRoomMaxTapDelayMs * 1e-3 * sampleRate));
    m.lineSize = 1;
    while (m.lineSize < static_cast<std::uint32_t>(m.maxTapDelay) + kChunk + 2) m.lineSize <<= 1;
    m.lineMask = m.lineSize - 1;
    m.preSize = 1;
    while (m.preSize < static_cast<std::uint32_t>(std::ceil(kPreDelayMaxMs * 1e-3 * sampleRate)) + 2) m.preSize <<= 1;
    m.preMask = m.preSize - 1;
    m.busTail = kMaxDeviceTaps * (sampleRate / 48000 + 1) + static_cast<std::uint32_t>(0.03 * sampleRate);
    m.lines.reset(new (std::nothrow) float[std::size_t(maxSources + 1) * m.lineSize]());
    m.sharedIn.reset(new (std::nothrow) float[kMaxFramesPerCall]());
    if (!m.sharedIn) return fail("out of memory");
    const std::size_t maxStride = PackedStride(kMaxFramesPerCall);
    m.busBuf.reset(new (std::nothrow) float[std::size_t(kRoomBuses) * maxStride]());
    m.wetBuf.reset(new (std::nothrow) float[std::size_t(2) * kMaxFramesPerCall]());
    m.earlyBuf.reset(new (std::nothrow) float[std::size_t(2) * kMaxFramesPerCall]());
    m.late.reset(new (std::nothrow) float[kMaxFramesPerCall]());
    m.x1.reset(new (std::nothrow) float[kMaxFramesPerCall]());
    m.x2.reset(new (std::nothrow) float[kMaxFramesPerCall]());
    m.tmp.reset(new (std::nothrow) float[kChunk]());
    m.pre.reset(new (std::nothrow) float[m.preSize]());
    m.accBuf.reset(new (std::nothrow) float[std::size_t(kRoomTaps) * kRoomBuses * maxStride]());
    std::memset(m.accIx, 0xff, sizeof(m.accIx));
    m.accZero.reset(new (std::nothrow) float[kMaxFramesPerCall]());
    m.accSink.reset(new (std::nothrow) float[kMaxFramesPerCall]());
    m.vin.reset(new (std::nothrow) float[kChunk]());
    if (!m.accBuf || !m.accZero || !m.accSink || !m.vin) return fail("out of memory");
    if (!m.lines || !m.busBuf || !m.late || !m.x1 || !m.x2 || !m.tmp || !m.pre || !m.wetBuf || !m.earlyBuf) return fail("out of memory");
    m.slots.resize(maxSources + 1);  // the last one: the shared tap line of the quieter voices
    for (std::uint32_t i = 0; i <= maxSources; ++i) m.slots[i].line = m.lines.get() + std::size_t(i) * m.lineSize;
    if (!m.fdn.Init(sampleRate)) return fail("out of memory");
    m.mixer.Init(sampleRate);
    const bool avx2 = g_forceRoomKernels.load() != 1 && kernels::CpuHasAvx2Fma();
    m.mix = avx2 ? &kernels::RoomMixAvx2 : &RoomMixSse2;
    m.pairs = avx2 ? &kernels::RoomPairsAvx2 : nullptr;
    m.match.band[0][0] = rf::Lowpass(kRoomMatchLowHz, 0.54119610, sampleRate);
    m.match.band[0][1] = rf::Lowpass(kRoomMatchLowHz, 1.30656296, sampleRate);
    m.match.band[1][0] = rf::Highpass(kRoomMatchLowHz, 0.70710678, sampleRate);
    m.match.band[1][1] = rf::Lowpass(kRoomMatchHighHz, 0.70710678, sampleRate);
    m.match.band[2][0] = rf::Highpass(kRoomMatchHighHz, 0.54119610, sampleRate);
    m.match.band[2][1] = rf::Highpass(kRoomMatchHighHz, 1.30656296, sampleRate);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    m.qpcToNs = 1e9 / static_cast<double>(f.QuadPart);
    {
        // The TSC's rate against QPC over about 2 ms (control thread), for RoomStats::sectionUs.
        LARGE_INTEGER q0, q1;
        QueryPerformanceCounter(&q0);
        const std::uint64_t t0 = __rdtsc();
        do QueryPerformanceCounter(&q1);
        while ((q1.QuadPart - q0.QuadPart) * 500 < f.QuadPart);
        const double us = static_cast<double>(q1.QuadPart - q0.QuadPart) * 1e6 / static_cast<double>(f.QuadPart);
        m.tscPerUs = us > 0 ? static_cast<double>(__rdtsc() - t0) / us : 0.0;
    }
    for (std::uint32_t b = 0; b < kRoomBuses; ++b) rm::BusDirection(b, m.busHead[b]);
    m.inited = true;
    return true;
}

void Room::SetHrtf(const HrtfSet* set) {
    Impl& m = *impl_;
    if (!m.inited || !set) return;
    if (!m.busSets.empty() && m.busSets.back().orig == set) return;  // already the buses' set
    // The buses render a copy of the set with shorter filters (kBusTaps48); the room never reads `set` on the audio
    // thread, so it is retired as soon as the buses let go of its copy.
    double kept = 1.0;
    const int forced = g_forceBusTaps.load();
    const std::uint32_t taps = static_cast<std::uint32_t>(
        std::lround(double(forced > 0 ? static_cast<std::uint32_t>(forced) : kBusTaps48) * set->sampleRate / 48000.0));
    HrtfSet* copy = taps < set->taps ? CopyHrtfShortened(*set, taps, &kept) : nullptr;
    const HrtfSet* forBus = copy ? copy : set;
    m.busSets.push_back({set, copy});
    if (copy) m.busKeptWorst = kept;
    m.bus.SetHrtf(forBus);
    m.busGiven.store(forBus, std::memory_order_release);
    if (set->sampleRate != m.fs) return;
    m.hrtfBox.Write(ComputeHrtfDerived(*set, *forBus));
}
const HrtfSet* Room::TakeRetiredHrtf() {
    Impl& m = *impl_;
    if (!m.inited) return nullptr;
    while (const HrtfSet* r = m.bus.TakeRetiredHrtf()) {
        bool found = false;
        for (std::size_t i = 0; i < m.busSets.size(); ++i) {
            const Impl::BusSet b = m.busSets[i];
            if (b.copy == r || (!b.copy && b.orig == r)) {
                m.retiredOrig.push_back(b.orig);
                if (b.copy) ReleaseHrtf(b.copy);
                m.busSets.erase(m.busSets.begin() + static_cast<std::ptrdiff_t>(i));
                found = true;
                break;
            }
        }
        if (!found) m.retiredOrig.push_back(r);
    }
    if (m.retiredOrig.empty()) return nullptr;
    const HrtfSet* r = m.retiredOrig.front();
    m.retiredOrig.erase(m.retiredOrig.begin());
    return r;
}
void Room::SetHeadRadius(float metres) {
    if (impl_->inited) impl_->bus.SetHeadRadius(metres);
}
void Room::SetParams(const RoomParams& p) {
    if (!impl_->inited || p.strength < cockpit_space::kStrengthSubtle) return;
    impl_->paramsBox.Write(cockpit_space::Sanitize(p));
}

// ---- audio thread ----------------------------------------------------------------------------------------------------
void Room::BeginBlock(std::uint32_t frames, const RoomFrame& f) {
    Impl& m = *impl_;
    if (!m.inited) return;
    if (!m.inBlock) {
        m.csr = _mm_getcsr();
        _mm_setcsr(m.csr | 0x8040u);  // FTZ | DAZ
    }
    m.inBlock = true;
    QueryPerformanceCounter(&m.t0);
    ++m.blockIndex;
    if (m.accUsed) m.ClearAccs(m.frames);  // a block that was not rendered left its accumulators (at its own stride)
    m.frames = frames;
    m.pstride = PackedStride(std::min(frames, kMaxFramesPerCall));
    m.wSum = m.wdSum = 0.0;
    // The voices' ranks by the last block's send energy (the shared tap line's choice, kRoomOwnTaps).
    {
        const int forced = g_forceOwnTaps.load(std::memory_order_relaxed);
        m.ownTaps = forced >= 0 ? (forced == 0 ? kMaxSources : static_cast<std::uint32_t>(forced)) : kRoomOwnTaps;
        float e[kMaxSources];
        std::uint32_t ix[kMaxSources], n = 0;
        for (std::uint32_t i = 0; i < m.maxSources; ++i) {
            Slot& s = m.slots[i];
            s.rank = ~0u;
            if (s.energyBlock == m.blockIndex - 1) e[n] = static_cast<float>(s.lastEnergy), ix[n++] = i;
        }
        if (n > m.ownTaps) {
            // insertion sort, largest first (at most kMaxSources entries, no allocation)
            for (std::uint32_t a = 1; a < n; ++a)
                for (std::uint32_t b = a; b > 0 && e[b] > e[b - 1]; --b) std::swap(e[b], e[b - 1]), std::swap(ix[b], ix[b - 1]);
            for (std::uint32_t r = 0; r < n; ++r) m.slots[ix[r]].rank = r;
            m.ownEnergy = e[m.ownTaps - 1];
        } else {
            for (std::uint32_t r = 0; r < n; ++r) m.slots[ix[r]].rank = 0;
            m.ownEnergy = 0.0;
        }
        m.sharedThisBlock = false;
        m.shPos[0] = m.shPos[1] = m.shPos[2] = m.shE = m.shThroughE = 0.0;
    }
    m.inputThisBlock = m.busInputThisBlock = m.lateInputThisBlock = false;
    m.blockOk = frames > 0 && frames <= kMaxFramesPerCall;
    if (!m.blockOk) {
        m.sRejected.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // A new set for the buses while they may not run (early reflections off, no early input): an empty bus block adopts
    // it, so the old one can be retired and freed (bounded: one BeginBlock / EndBlock per change, no source rendered).
    if (const HrtfSet* given = m.busGiven.load(std::memory_order_acquire); given != m.busSeen) {
        m.bus.BeginBlock(frames);
        m.bus.EndBlock();
        m.busSeen = given;
    }

    // New HRTF data: the diffuse EQ crossfades over this block (both EQ sets run), the gains ramp.
    HrtfDerived h;
    if (m.hrtfBox.ReadIfNew(&h, &m.hrtfSeen) && h.valid) {
        if (m.haveHrtf) {
            const int next = 1 - m.eqCur;
            std::memcpy(m.eqCoef[next], h.eq, sizeof(h.eq));
            std::memcpy(m.eqState[next], m.eqState[m.eqCur], sizeof(m.eqState[0]));
            m.eqFade = true;
        } else {
            std::memcpy(m.eqCoef[m.eqCur], h.eq, sizeof(h.eq));
        }
        m.hd = h;
    }
    // New parameters: interpolate from the current values over kRoomParamsCrossfadeMs.
    RoomParams p{};
    bool freshParams = false;
    if (m.paramsBox.ReadIfNew(&p, &m.paramsSeen)) {
        if (!m.haveParams) {
            m.from = m.to = m.eff = p;
            m.xfade = 1.0f;
            freshParams = true;
        } else {
            m.from = m.eff;
            m.to = p;
            m.xfade = 0.0f;
        }
        m.haveParams = true;
    }
    const bool firstReady = !m.haveHrtf && m.hd.valid && m.haveParams;
    if (m.hd.valid) m.haveHrtf = true;
    const float fsF = static_cast<float>(m.fs);
    if (m.xfade < 1.0f) {
        m.xfade = std::min(1.0f, m.xfade + static_cast<float>(frames) / (kRoomParamsCrossfadeMs * 1e-3f * fsF));
        m.eff = LerpParams(m.from, m.to, m.xfade);
    }

    // Smoothed values.
    const float wt = std::isfinite(f.wet) ? std::clamp(f.wet, 0.0f, 1.0f) : 0.0f;
    m.wetTarget = wt;
    m.wetPrev = m.wetCur;
    const float wetStep = static_cast<float>(frames) / (kRoomWetSlewS * fsF);
    m.wetCur = wt > m.wetCur ? std::min(wt, m.wetCur + wetStep) : std::max(wt, m.wetCur - wetStep);

    float q[4] = {f.aircraftToHead[0], f.aircraftToHead[1], f.aircraftToHead[2], f.aircraftToHead[3]};
    const float qn = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    const bool qOk = std::isfinite(qn) && qn > 0.5f && qn < 1.5f;
    const bool valid = f.frameValid && qOk;
    if (valid) {
        for (float& v : q) v /= qn;
        std::memcpy(m.C, q, sizeof(q));
        m.haveRotation = true;
        for (std::uint32_t b = 0; b < kRoomBuses; ++b) {
            float d[3];
            rm::BusDirection(b, d);
            RotateQ(m.C, d, m.busHead[b]);
        }
    }
    if (!f.frameValid) m.sFrameInvalid.fetch_add(1, std::memory_order_relaxed);
    if (!f.early) m.sEarlyOff.fetch_add(1, std::memory_order_relaxed);
    const float earlyTarget = (valid && f.early && m.haveRotation) ? 1.0f : 0.0f;
    m.earlyPrev = m.earlyCur;
    const float earlyStep = static_cast<float>(frames) / (kRoomEarlyFadeMs * 1e-3f * fsF);
    m.earlyCur = earlyTarget > m.earlyCur ? std::min(earlyTarget, m.earlyCur + earlyStep) : std::max(earlyTarget, m.earlyCur - earlyStep);

    const float ct = std::isfinite(f.canopyOpen) ? std::clamp(f.canopyOpen, 0.0f, 1.0f) : 0.0f;
    m.canPrev = m.canCur;
    m.canCur += (ct - m.canCur) * (1.0f - std::exp(-static_cast<float>(frames) / (kRoomCanopyTauS * fsF)));
    if (std::fabs(ct - m.canCur) < 1e-4f) m.canCur = ct;

    if (m.Ready()) {
        m.dPrev = m.dCur;
        m.dCur = m.Derive(m.eff, m.canCur);
        if (freshParams || firstReady) m.dPrev = m.dCur;
    }
    m.tapsStable = m.Ready() && !freshParams && !firstReady && m.xfade >= 1.0f && m.xfadeWasDone && m.earlyCur == m.earlyPrev &&
                   m.canCur == m.canPrev && m.dCur.K == m.dPrev.K;
    m.xfadeWasDone = m.xfade >= 1.0f;
    m.sWet.store(m.wetCur, std::memory_order_relaxed);
    m.sCanopy.store(m.canCur, std::memory_order_relaxed);
    std::memset(m.busBuf.get(), 0, sizeof(float) * std::size_t(kRoomBuses) * m.pstride);
    std::memset(m.late.get(), 0, sizeof(float) * frames);
}

// One slot's block: its weighted input (a, b, stride, scale; stride 0 reads a[0] for every frame) into its delay line
// and the late input, its taps through the wall filters onto the buses. Gains and delays ramp from the slot's previous
// values (s.send, s.late, s.delay, s.bus) to the targets across `frames`; the targets become the slot's values. False
// when a filter state went non-finite (the caller resets the slot).
bool Room::Impl::Run(Slot& s, const float* a, const float* b, std::uint32_t stride, float sc, std::uint32_t nf, float w0,
                     float w1, const std::int32_t* delayT, const float (*busT)[kRoomBuses], float sendT, float lateT,
                     double* inEnergy, float* sumTo, bool noWet) {
    // sumTo: the weighted input is added there (the shared tap line's input) instead of this slot's line, no taps.
    // noWet: the wet weight is not applied (the shared line's input carries it already).
    bool early = false;
    for (std::uint32_t k = 0; k < kRoomTaps && !early; ++k)
        for (std::uint32_t j = 0; j < kRoomBuses; ++j)
            if (s.bus[k][j] != 0.0f || busT[k][j] != 0.0f) {
                early = true;
                break;
            }
    const float invF = 1.0f / static_cast<float>(nf);
    const float s0 = s.send, ds = sendT - s.send, l0 = s.late, dl = lateT - s.late;
    const float we0 = noWet ? 1.0f : wetPrev, dwe = noWet ? 0.0f : wetCur - wetPrev, dw = w1 - w0;
    if (sumTo) early = false;
    const std::uint32_t mask = lineMask;
    float* line = s.line;
    float* lateIn = late.get();
    float* t = tmp.get();
    const std::uint32_t base = s.w;
    const bool zeroSkip = !g_forceNoZeroSkip.load(std::memory_order_relaxed);
    bool any = false;
    __m128 vEn = _mm_setzero_ps();  // the raw input's energy (the level match's weight)
    float enTail = 0.0f;
    for (std::uint32_t o = 0; o < nf; o += kChunk) {
        const std::uint32_t n = std::min(kChunk, nf - o);
        {
            // The weighted input: gather (any stride), then the gain ramps four frames at a time (SSE2).
            float* v = vin.get();
            if (b && b == a + 1 && stride == 2) {
                // The module's stereo pair, interleaved: L + R four frames at a time.
                const float* p = a + 2 * std::size_t(o);
                std::uint32_t i = 0;
                for (; i + 4 <= n; i += 4) {
                    const __m128 q0 = _mm_loadu_ps(p + 2 * i), q1 = _mm_loadu_ps(p + 2 * i + 4);
                    _mm_storeu_ps(v + i, _mm_add_ps(_mm_shuffle_ps(q0, q1, 0x88), _mm_shuffle_ps(q0, q1, 0xdd)));
                }
                for (; i < n; ++i) v[i] = p[2 * i] + p[2 * i + 1];
            } else if (b) {
                for (std::uint32_t i = 0; i < n; ++i) v[i] = a[std::size_t(o + i) * stride] + b[std::size_t(o + i) * stride];
            } else {
                for (std::uint32_t i = 0; i < n; ++i) v[i] = a[std::size_t(o + i) * stride];
            }
            const __m128 vInv = _mm_set1_ps(invF), vOne = _mm_set1_ps(1.0f), vSc = _mm_set1_ps(sc);
            const __m128 vS0 = _mm_set1_ps(s0), vDs = _mm_set1_ps(ds), vW0 = _mm_set1_ps(w0), vDw = _mm_set1_ps(dw);
            const __m128 vE0 = _mm_set1_ps(we0), vDe = _mm_set1_ps(dwe), vL0 = _mm_set1_ps(l0), vDl = _mm_set1_ps(dl);
            __m128 idx = _mm_setr_ps(static_cast<float>(o + 1), static_cast<float>(o + 2), static_cast<float>(o + 3), static_cast<float>(o + 4));
            const __m128 four = _mm_set1_ps(4.0f), zero = _mm_setzero_ps();
            int nz = 0;
            std::uint32_t i = 0;
            if (ds == 0.0f && dw == 0.0f && dwe == 0.0f && dl == 0.0f) {
                // Every gain steady (the usual block): one product.
                const __m128 g = _mm_set1_ps(sc * s0 * w0 * we0);
                for (; i + 4 <= n; i += 4) {
                    const __m128 raw = _mm_loadu_ps(v + i);
                    vEn = _mm_add_ps(vEn, _mm_mul_ps(raw, raw));
                    const __m128 x = _mm_mul_ps(raw, g);
                    _mm_storeu_ps(v + i, x);
                    _mm_storeu_ps(lateIn + o + i, _mm_add_ps(_mm_loadu_ps(lateIn + o + i), _mm_mul_ps(x, vL0)));
                    nz |= _mm_movemask_ps(_mm_cmpneq_ps(x, zero));
                }
            }
            for (; i + 4 <= n; i += 4) {
                const __m128 tg = _mm_mul_ps(idx, vInv), tw = _mm_mul_ps(_mm_sub_ps(idx, vOne), vInv);
                __m128 g = _mm_mul_ps(vSc, _mm_add_ps(vS0, _mm_mul_ps(vDs, tg)));
                g = _mm_mul_ps(g, _mm_add_ps(vW0, _mm_mul_ps(vDw, tw)));
                g = _mm_mul_ps(g, _mm_add_ps(vE0, _mm_mul_ps(vDe, tg)));
                const __m128 raw = _mm_loadu_ps(v + i);
                vEn = _mm_add_ps(vEn, _mm_mul_ps(raw, raw));
                const __m128 x = _mm_mul_ps(raw, g);
                _mm_storeu_ps(v + i, x);
                const __m128 lg = _mm_add_ps(vL0, _mm_mul_ps(vDl, tg));
                _mm_storeu_ps(lateIn + o + i, _mm_add_ps(_mm_loadu_ps(lateIn + o + i), _mm_mul_ps(x, lg)));
                nz |= _mm_movemask_ps(_mm_cmpneq_ps(x, zero));
                idx = _mm_add_ps(idx, four);
            }
            for (; i < n; ++i) {
                const std::uint32_t j = o + i;
                const float tg = static_cast<float>(j + 1) * invF, tw = static_cast<float>(j) * invF;
                const float x = v[i] * sc * (s0 + ds * tg) * (w0 + dw * tw) * (we0 + dwe * tg);
                enTail += v[i] * v[i];
                v[i] = x;
                lateIn[j] += x * (l0 + dl * tg);
                nz |= x != 0.0f ? 1 : 0;
            }
            any = any || nz != 0;
            // Every sample a tap reads in this chunk (from maxTapDelay before it to its end) zero: skip the taps.
            const bool silentReads = zeroSkip && !sumTo && nz == 0 && s.zeroRun >= static_cast<std::uint32_t>(maxTapDelay);
            s.zeroRun = sumTo ? 0u : nz == 0 ? std::min(s.zeroRun + n, lineSize) : 0u;
            if (silentReads) {
                const std::uint32_t wpos = (base + o) & mask, first = std::min(n, lineSize - wpos);
                std::memcpy(line + wpos, v, sizeof(float) * first);
                if (first < n) std::memcpy(line, v + first, sizeof(float) * (n - first));
                // The accumulators are still claimed, in the same order: Render runs their wall filters (whose state
                // still rings) and sums them in claim order, so the output stays the same to the bit.
                if (early)
                    for (std::uint32_t k = 0; k < kRoomTaps; ++k) {
                        int nu = 0;
                        for (std::uint32_t j = 0; j < kRoomBuses && nu < 6; ++j)
                            if (s.bus[k][j] != 0.0f || busT[k][j] != 0.0f) AccFor(k, j), ++nu;
                    }
                continue;
            }
            if (sumTo) {
                std::uint32_t q = 0;
                for (; q + 4 <= n; q += 4) _mm_storeu_ps(sumTo + o + q, _mm_add_ps(_mm_loadu_ps(sumTo + o + q), _mm_loadu_ps(v + q)));
                for (; q < n; ++q) sumTo[o + q] += v[q];
                continue;
            }
            const std::uint32_t wpos = (base + o) & mask, first = std::min(n, lineSize - wpos);
            std::memcpy(line + wpos, v, sizeof(float) * first);
            if (first < n) std::memcpy(line, v + first, sizeof(float) * (n - first));
        }
        if (!early) continue;
        for (std::uint32_t k = 0; k < kRoomTaps; ++k) {
            // The buses this tap feeds in this block (its previous and its new triangle: at most 6).
            std::uint32_t used[6];
            int nu = 0;
            for (std::uint32_t j = 0; j < kRoomBuses && nu < 6; ++j)
                if (s.bus[k][j] != 0.0f || busT[k][j] != 0.0f) used[nu++] = j;
            if (nu == 0) continue;
            const std::int32_t d0 = s.delay[k], d1 = delayT[k];
            const std::uint32_t r1 = (base + o - static_cast<std::uint32_t>(d1)) & mask;
            const float* src = t;
            if (d0 == d1) {
                if (r1 + n <= lineSize) {
                    src = line + r1;  // contiguous: read the line in place
                } else {
                    const std::uint32_t first = lineSize - r1;
                    std::memcpy(t, line + r1, sizeof(float) * first);
                    std::memcpy(t + first, line, sizeof(float) * (n - first));
                }
            } else {
                const std::uint32_t r0 = base + o - static_cast<std::uint32_t>(d0);
                for (std::uint32_t i = 0; i < n; ++i) {
                    const float u = static_cast<float>(o + i + 1) * invF;
                    const float a0 = line[(r0 + i) & mask], a1 = line[(r1 + i) & mask];
                    t[i] = a0 + (a1 - a0) * u;
                }
            }
            // Into the (tap, bus) accumulators (one pass over the tap's samples for all its buses: AVX2/FMA when the CPU
            // has it, else SSE2); Render runs each accumulator through its wall's filters once for all voices.
            float* acc[6];
            float gs[6], dg[6];
            for (int q = 0; q < nu; ++q) {
                const std::uint32_t j = used[q];
                const float g0 = s.bus[k][j], g1 = busT[k][j];
                acc[q] = AccFor(k, j) + o;
                dg[q] = (g1 - g0) * invF, gs[q] = g0 + dg[q] * static_cast<float>(o + 1);
            }
            mix(src, acc, gs, dg, nu, n);
        }
    }
    if (inEnergy) {
        float t4[4];
        _mm_storeu_ps(t4, vEn);
        *inEnergy = double(t4[0]) + t4[1] + t4[2] + t4[3] + enTail;
    }
    s.w = (base + nf) & mask;
    if (delayT != s.delay) std::memcpy(s.delay, delayT, sizeof(s.delay));
    if (busT != s.bus) std::memcpy(s.bus, busT, sizeof(s.bus));
    s.send = sendT;
    s.late = lateT;
    s.active = true;
    s.lastBlock = blockIndex;
    // A voice that fell silent leaves taps reading older samples for up to kRoomMaxTapDelayMs: Render's bus tail counter
    // (busTail, longer than that) keeps the buses running for them.
    if (any) {
        inputThisBlock = true;
        lateInputThisBlock = true;
        if (early) busInputThisBlock = true;
    }
    return true;
}

bool Room::AddSource(std::uint32_t slot, const RoomSource& src, const MonoInput& in, std::uint32_t frames, float w0, float w1) {
    Impl& m = *impl_;
    if (!m.inited || !m.inBlock || !m.blockOk || !m.Ready() || slot >= m.maxSources || frames == 0 || frames > m.frames ||
        !in.a || in.stride == 0) {
        m.sRejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    Slot& s = m.slots[slot];
    if (s.active && s.lastBlock == m.blockIndex) {  // twice in one block
        m.sRejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const float* pos = src.posAircraft;
    if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2]) || !std::isfinite(src.send) ||
        !std::isfinite(w0) || !std::isfinite(w1) || !std::isfinite(in.scale)) {
        m.ResetSlot(s);
        m.sNonFinite.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // A non-finite input sample resets the slot and adds nothing. The module's stereo input (a, a + 1, stride 2) is
    // checked as one contiguous run.
    {
        const std::uint32_t st = in.stride;
        bool bad;
        if (in.b == in.a + 1 && st == 2) {
            bad = AnyNonFinite(in.a, std::size_t(frames) * 2);
        } else if (st == 1) {
            bad = AnyNonFinite(in.a, frames) || (in.b && AnyNonFinite(in.b, frames));
        } else {
            std::uint32_t bits = 0;
            for (std::uint32_t i = 0; i < frames; ++i) {
                float v = in.a[std::size_t(i) * st] * 0.0f;
                if (in.b) v += in.b[std::size_t(i) * st] * 0.0f;
                std::uint32_t u;
                std::memcpy(&u, &v, 4);
                bits |= u;
            }
            bad = (bits & 0x7fffffffu) != 0;
        }
        if (bad) {
            m.ResetSlot(s);
            m.sNonFinite.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    // The quieter voices (kRoomOwnTaps) send to the shared tap line: their weighted input (the same ramps as on their
    // own line) is summed there, their position into its energy-weighted mean; their own line plays out what it holds.
    {
        const bool early = m.earlyCur > 0.0f || m.earlyPrev > 0.0f;
        const bool known = !(src.flags & room_flags::kReset) && s.energyBlock + 1 == m.blockIndex;
        const bool wasShared = s.shared && s.sharedBlock + 1 == m.blockIndex;
        // Past the kRoomOwnTaps loudest, and clearly quieter than the last of them (6 dB to go to the shared line, 3 dB
        // back): voices of about the same level never swap lines from block to block.
        const bool share = early && known && !(src.flags & room_flags::kLateOnly) && s.rank != ~0u && s.rank >= m.ownTaps &&
                           s.lastEnergy < m.ownEnergy * (wasShared ? 0.5 : 0.25);
        if (share) {
            const float sendT = std::max(src.send, 0.0f) * ((src.flags & room_flags::kThroughCanopy) ? m.dCur.through : 1.0f);
            Slot t;  // the ramps' state for the shared line's input (no line, no taps)
            t.send = wasShared ? s.shSend : sendT;
            t.late = wasShared ? s.shLate : 1.0f;
            t.line = nullptr;
            double inEnergy = 0.0;
            m.Run(t, in.a, in.b, in.stride, in.scale, frames, w0, w1, t.delay, t.bus, sendT, 1.0f, &inEnergy, m.sharedIn.get());
            s.shSend = sendT, s.shLate = 1.0f, s.shared = true, s.sharedBlock = m.blockIndex;
            const double e = inEnergy * double(in.scale) * in.scale * double(sendT) * sendT;
            if (std::isfinite(e)) {
                s.lastEnergy = e, s.energyBlock = m.blockIndex;
                for (int k = 0; k < 3; ++k) m.shPos[k] += e * pos[k];
                m.shE += e;
                if (src.flags & room_flags::kThroughCanopy) m.shThroughE += e;
            }
            m.sharedThisBlock = true;
            const float sd = rm::SendDistanceGain(m.eff, pos, src.flags);
            const float hrtfGain = sd > 1e-6f ? std::max(src.send, 0.0f) / sd : 0.0f;
            const double w = 0.5 * (double(w0) + w1), ed = inEnergy * double(in.scale) * in.scale * hrtfGain * hrtfGain * w * w;
            const float dd = (src.direct > 0.0f && std::isfinite(src.direct)) ? std::min(src.direct, 4.0f) : 1.0f;
            if (std::isfinite(ed)) m.wSum += ed, m.wdSum += ed * dd * dd;
            m.sAdded.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        s.shared = false;
    }
    // A new voice, or one that skipped a block (its line holds stale samples): start from a clean slot, no ramps.
    const bool fresh = (src.flags & room_flags::kReset) || !s.active || s.lastBlock + 1 != m.blockIndex;
    if (fresh) m.ResetSlot(s);

    // Targets at the block's end.
    const RoomParams& p = m.eff;
    const Derived& d = m.dCur;
    rm::Tap taps[kRoomTaps];
    const std::uint32_t tapFlags = src.flags & (room_flags::kThroughCanopy | room_flags::kLateOnly);
    // "Did not move": within kTapReuseM on each axis of the position its taps were computed for (an aircraft-fixed
    // voice seen from a turning head comes back through two rotations with float noise of a few micrometres).
    const bool reuse = !fresh && m.tapsStable && s.tapsValid && s.tapFlags == tapFlags &&
                       std::fabs(s.tapPos[0] - pos[0]) <= kTapReuseM && std::fabs(s.tapPos[1] - pos[1]) <= kTapReuseM &&
                       std::fabs(s.tapPos[2] - pos[2]) <= kTapReuseM;
    // Early reflections off (and faded out in the previous block too): no taps to compute, the late send only.
    const bool earlyNow = m.earlyCur > 0.0f || m.earlyPrev > 0.0f;
    if (reuse) {
    } else if (earlyNow) {
        rm::ComputeTaps(p, m.canCur, pos, src.flags, taps);
    } else {
        for (std::uint32_t k = 0; k < kRoomTaps; ++k) taps[k] = rm::Tap{static_cast<float>(s.delay[k]) / static_cast<float>(m.fs), 0.0f, {1, 0, 0}, 0, 0};
    }
    const float sendT = std::max(src.send, 0.0f) * ((src.flags & room_flags::kThroughCanopy) ? d.through : 1.0f);
    const float lateT = (src.flags & room_flags::kLateOnly) ? rm::kLateOnlyGain : 1.0f;
    std::int32_t delayBuf[kRoomTaps];
    float busBufT[kRoomTaps][kRoomBuses];
    const std::int32_t* delayT = s.delay;
    const float(*busT)[kRoomBuses] = s.bus;
    const float fsF = static_cast<float>(m.fs);
    const float sd = reuse ? s.tapSd : rm::SendDistanceGain(p, pos, src.flags), cap = d.cap;
    if (!reuse) {
        std::memset(busBufT, 0, sizeof(busBufT));
        delayT = delayBuf, busT = busBufT;
        // The voice's weak taps (under kRoomTapMinShare of its early energy: usually the floor's and the far front's)
        // are left out and the others scaled to keep the energy: each tap costs a pass over the block.
        float g[kRoomTaps], eAll = 0.0f;
        for (std::uint32_t k = 0; k < kRoomTaps; ++k) {
            delayBuf[k] = std::clamp(static_cast<std::int32_t>(std::lround(taps[k].delayS * fsF)), 0, m.maxTapDelay);
            g[k] = rm::CappedTapGain(taps[k].gain * d.K, sd, cap) * m.earlyCur;
            eAll += g[k] * g[k];
        }
        const float forcedShare = g_forceTapMinShare.load(std::memory_order_relaxed);
        const float minShare = forcedShare >= 0.0f ? forcedShare : kRoomTapMinShare;
        float eKept = 0.0f;
        for (std::uint32_t k = 0; k < kRoomTaps; ++k) {
            if (g[k] * g[k] < minShare * eAll) g[k] = 0.0f;
            eKept += g[k] * g[k];
        }
        const float keep = eKept > 0.0f ? std::sqrt(eAll / eKept) : 0.0f;
        for (std::uint32_t k = 0; k < kRoomTaps; ++k) {
            if (g[k] > 0.0f) {
                std::uint32_t idx[3];
                float pg[3];
                rm::Pan(taps[k].u, idx, pg);
                for (int j = 0; j < 3; ++j) busBufT[k][idx[j]] += pg[j] * g[k] * keep;
            }
        }
    }
    if (fresh) {
        std::memcpy(s.delay, delayT, sizeof(s.delay));
        std::memcpy(s.bus, busT, sizeof(s.bus));
        s.send = sendT;
        s.late = lateT;
    }
    double inEnergy = 0.0;
    if (!m.Run(s, in.a, in.b, in.stride, in.scale, frames, w0, w1, delayT, busT, sendT, lateT, &inEnergy)) {
        m.ResetSlot(s);
        m.sNonFinite.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // The level match's weight: this voice's direct energy (input x its HRTF gain, the send without the distance law, x
    // its weight), and d^2 x that.
    {
        const float hrtfGain = sd > 1e-6f ? std::max(src.send, 0.0f) / sd : 0.0f;
        const double w = 0.5 * (double(w0) + w1), e = inEnergy * double(in.scale) * in.scale * hrtfGain * hrtfGain * w * w;
        const float dd = (src.direct > 0.0f && std::isfinite(src.direct)) ? std::min(src.direct, 4.0f) : 1.0f;
        if (std::isfinite(e)) m.wSum += e, m.wdSum += e * dd * dd;
        const double es = inEnergy * double(in.scale) * in.scale * double(sendT) * sendT;
        if (std::isfinite(es)) s.lastEnergy = es, s.energyBlock = m.blockIndex;
    }
    s.drain = 0;
    if (!reuse) {  // kept from the computation, so a slow drift still recomputes once it passes kTapReuseM
        std::memcpy(s.tapPos, pos, sizeof(s.tapPos));
        s.tapSd = sd;
    }
    s.tapFlags = tapFlags, s.tapsValid = true;
    m.sAdded.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// A tap set's targets for a position: whole-sample delays and the VBAP bus gains (the early normalisation, the cap,
// the early reflections' fade), as AddSource computes them for a voice (the shared tap line's).
void Room::Impl::TapTargets(const RoomParams& p, const float pos[3], std::uint32_t flags, float sd, std::int32_t* delayT,
                            float (*busT)[kRoomBuses]) {
    rm::Tap taps[kRoomTaps];
    rm::ComputeTaps(p, canCur, pos, flags, taps);
    std::memset(busT, 0, sizeof(float) * kRoomTaps * kRoomBuses);
    const float fsF = static_cast<float>(fs);
    for (std::uint32_t k = 0; k < kRoomTaps; ++k) {
        delayT[k] = std::clamp(static_cast<std::int32_t>(std::lround(taps[k].delayS * fsF)), 0, maxTapDelay);
        const float g = rm::CappedTapGain(taps[k].gain * dCur.K, sd, dCur.cap) * earlyCur;
        if (g > 0.0f) {
            std::uint32_t idx[3];
            float pg[3];
            rm::Pan(taps[k].u, idx, pg);
            for (int j = 0; j < 3; ++j) busT[k][idx[j]] += pg[j] * g;
        }
    }
}

void Room::ResetSource(std::uint32_t slot) {
    Impl& m = *impl_;
    if (!m.inited || slot >= m.maxSources) return;
    Slot& s = m.slots[slot];
    // The voice ended: its last reflections (up to kRoomMaxTapDelayMs of its line) still play out, Render drains the slot
    // with silence, instead of being cut mid-reflection; a new voice in the slot comes with kReset and clears it.
    if (s.active && s.lastBlock + 1 >= m.blockIndex) s.drain = m.maxTapDelay + static_cast<std::int32_t>(kDrainMargin);
    else m.ResetSlot(s);
}

// The wall filters of (tap, bus) pairs, SSE2: two groups of four lanes side by side (two independent recursions per
// frame), four frames at a time through a 4x4 transpose. Per pair, x = in[p][i]: hs = ch hs + hb x (the high-pass's
// low-pass state), ls = cl ls + la (x - hs) (the low-pass of the high-passed signal), out[p][i] += ls. count: a multiple
// of 4 (idle lanes read zeros into a sink).
static void RoomPairsSse2(const float* const* in, float* const* out, const float* hb, const float* la, float* hs, float* ls,
                          int count, std::uint32_t n) {
    for (int g0 = 0; g0 < count; g0 += 8) {
        const int ng = count - g0 > 4 ? 2 : 1;
        __m128 vhb[2], vla[2], vch[2], vcl[2], vhs[2], vls[2];
        for (int g = 0; g < ng; ++g) {
            const int b = g0 + 4 * g;
            vhb[g] = _mm_loadu_ps(hb + b), vla[g] = _mm_loadu_ps(la + b);
            vch[g] = _mm_sub_ps(_mm_set1_ps(1.0f), vhb[g]), vcl[g] = _mm_sub_ps(_mm_set1_ps(1.0f), vla[g]);
            vhs[g] = _mm_loadu_ps(hs + b), vls[g] = _mm_loadu_ps(ls + b);
        }
        std::uint32_t i = 0;
        for (; i + 4 <= n; i += 4) {
            __m128 r[2][4];
            for (int g = 0; g < ng; ++g) {
                const int b = g0 + 4 * g;
                r[g][0] = _mm_loadu_ps(in[b] + i), r[g][1] = _mm_loadu_ps(in[b + 1] + i);
                r[g][2] = _mm_loadu_ps(in[b + 2] + i), r[g][3] = _mm_loadu_ps(in[b + 3] + i);
                _MM_TRANSPOSE4_PS(r[g][0], r[g][1], r[g][2], r[g][3]);
            }
            for (int f = 0; f < 4; ++f)
                for (int g = 0; g < ng; ++g) {
                    const __m128 x = r[g][f];
                    vhs[g] = _mm_add_ps(_mm_mul_ps(vch[g], vhs[g]), _mm_mul_ps(vhb[g], x));
                    vls[g] = _mm_add_ps(_mm_mul_ps(vcl[g], vls[g]), _mm_mul_ps(vla[g], _mm_sub_ps(x, vhs[g])));
                    r[g][f] = vls[g];
                }
            for (int g = 0; g < ng; ++g) {
                const int b = g0 + 4 * g;
                _MM_TRANSPOSE4_PS(r[g][0], r[g][1], r[g][2], r[g][3]);
                for (int l = 0; l < 4; ++l) _mm_storeu_ps(out[b + l] + i, _mm_add_ps(_mm_loadu_ps(out[b + l] + i), r[g][l]));
            }
        }
        for (int g = 0; g < ng; ++g) {
            const int b = g0 + 4 * g;
            float h4[4], l4[4];
            _mm_storeu_ps(h4, vhs[g]), _mm_storeu_ps(l4, vls[g]);
            for (std::uint32_t k = i; k < n; ++k)
                for (int l = 0; l < 4; ++l) {
                    const float x = in[b + l][k];
                    h4[l] = (1.0f - hb[b + l]) * h4[l] + hb[b + l] * x;
                    l4[l] = (1.0f - la[b + l]) * l4[l] + la[b + l] * (x - h4[l]);
                    out[b + l][k] += l4[l];
                }
            for (int l = 0; l < 4; ++l) hs[b + l] = h4[l], ls[b + l] = l4[l];
        }
    }
}

void Room::Render(float* outLR, std::uint32_t frames) { Render(outLR, frames, nullptr); }

void Room::Render(float* outLR, std::uint32_t frames, const float* directLR) {
    Impl& m = *impl_;
    if (!m.inited || !m.inBlock || !m.blockOk || !m.Ready() || !outLR || frames != m.frames) {
        if (m.inited && m.inBlock) m.sRejected.fetch_add(1, std::memory_order_relaxed);
        // The direct sound is the caller's: it plays whatever the room can do.
        if (outLR && directLR && frames <= kMaxFramesPerCall)
            for (std::uint32_t i = 0; i < 2 * frames; ++i) outLR[i] += directLR[i];
        return;
    }
    const float invF = 1.0f / static_cast<float>(frames);
    const Derived& d0 = m.dPrev;
    const Derived& d1 = m.dCur;
    float* wet = m.wetBuf.get();
    std::memset(wet, 0, sizeof(float) * 2 * frames);
    std::uint64_t tsc = __rdtsc();
    std::uint64_t secBlock[kRoomSections] = {};
    auto lap = [&](int k) {
        const std::uint64_t t = __rdtsc();
        m.prof[k].store(m.prof[k].load(std::memory_order_relaxed) + (t - tsc), std::memory_order_relaxed);
        secBlock[k] += t - tsc;
        tsc = t;
    };

    // ---- the shared tap line of the quieter voices: their summed input, taps at their energy-weighted position ----
    if (m.sharedThisBlock && m.shE > 0.0) {
        Slot& s = m.slots[m.maxSources];
        const float pos[3] = {static_cast<float>(m.shPos[0] / m.shE), static_cast<float>(m.shPos[1] / m.shE),
                              static_cast<float>(m.shPos[2] / m.shE)};
        const std::uint32_t flags = m.shThroughE > 0.5 * m.shE ? room_flags::kThroughCanopy : 0u;
        const bool fresh = !s.active || s.lastBlock + 1 != m.blockIndex;
        if (fresh) m.ResetSlot(s);
        std::int32_t delayT[kRoomTaps];
        float busT[kRoomTaps][kRoomBuses];
        m.TapTargets(m.eff, pos, flags, rm::SendDistanceGain(m.eff, pos, flags), delayT, busT);
        if (fresh) std::memcpy(s.delay, delayT, sizeof(s.delay)), std::memcpy(s.bus, busT, sizeof(s.bus)), s.send = 1.0f, s.late = 0.0f;
        if (!m.Run(s, m.sharedIn.get(), nullptr, 1, 1.0f, frames, 1.0f, 1.0f, delayT, busT, 1.0f, 0.0f, nullptr, nullptr, true))
            m.ResetSlot(s);
        s.drain = 0;
    }
    std::memset(m.sharedIn.get(), 0, sizeof(float) * frames);

    // ---- voices that ended (ResetSource) or were not added this block: their reflections play out with silence ----
    for (Slot& s : m.slots) {
        if (!s.active || s.lastBlock == m.blockIndex) continue;
        if (s.lastBlock + 1 != m.blockIndex) {  // missed more than one block: what its line holds is stale
            m.ResetSlot(s);
            continue;
        }
        if (s.drain <= 0) s.drain = m.maxTapDelay + static_cast<std::int32_t>(kDrainMargin);
        const float zero = 0.0f;
        if (!m.Run(s, &zero, nullptr, 0, 0.0f, frames, 0.0f, 0.0f, s.delay, s.bus, s.send, s.late)) {
            m.ResetSlot(s);
            continue;
        }
        s.drain -= static_cast<std::int32_t>(frames);
        if (s.drain <= 0) m.ResetSlot(s);
    }

    lap(0);
    // ---- early reflections: the buses ----
    if (m.busInputThisBlock) m.framesSinceBus = 0;
    const bool busNow = m.busInputThisBlock || m.framesSinceBus < m.busTail;
    if (!busNow) m.ClearAccs(frames);
    if (busNow) {
        // Each live (tap, bus) pair (written this block, or its filters still ringing) through its wall's high-pass and
        // low-pass into its bus (the send high-pass follows on the buses' binaural output); the accumulators are left
        // zeroed.
        constexpr std::uint32_t kPairs = kRoomTaps * kRoomBuses;
        const float* in[kPairs + 8];
        float* out[kPairs + 8];
        float hb[kPairs + 8], la[kPairs + 8], hs[kPairs + 8], ls[kPairs + 8];
        std::uint16_t pr[kPairs + 8];
        std::uint32_t nLive = 0;
        auto addPair = [&](std::uint32_t p, const float* src) {
            const std::uint32_t k = p / kRoomBuses, j = p % kRoomBuses;
            pr[nLive] = static_cast<std::uint16_t>(p);
            in[nLive] = src;
            out[nLive] = m.busBuf.get() + std::size_t(j) * m.pstride;
            hb[nLive] = d1.hpB[k], la[nLive] = d1.lpA[k], hs[nLive] = m.pairHp[k][j], ls[nLive] = m.pairLp[k][j];
            ++nLive;
        };
        for (std::uint32_t u = 0; u < m.accUsed; ++u) addPair(m.accList[u], m.accBuf.get() + std::size_t(u) * m.pstride);
        for (std::uint32_t k = 0; k < kRoomTaps; ++k)
            for (std::uint32_t j = 0; j < kRoomBuses; ++j)
                if (m.accIx[k][j] < 0 && std::fabs(m.pairHp[k][j]) + std::fabs(m.pairLp[k][j]) > 1e-12f)
                    addPair(k * kRoomBuses + j, m.accZero.get());
        const std::uint32_t nPad = (nLive + 7u) & ~7u;
        for (std::uint32_t l = nLive; l < nPad; ++l) {
            in[l] = m.accZero.get(), out[l] = m.accSink.get();
            hb[l] = 0.0f, la[l] = 1.0f, hs[l] = 0.0f, ls[l] = 0.0f;
        }
        if (nPad) {
            if (m.pairs) m.pairs(in, out, hb, la, hs, ls, static_cast<int>(nPad), frames);
            else RoomPairsSse2(in, out, hb, la, hs, ls, static_cast<int>(nPad), frames);
        }
        for (std::uint32_t l = 0; l < nLive; ++l) {
            const std::uint32_t k = pr[l] / kRoomBuses, j = pr[l] % kRoomBuses;
            const bool ok = std::isfinite(hs[l]) && std::isfinite(ls[l]);
            if (!ok) m.sNonFinite.fetch_add(1, std::memory_order_relaxed);
            m.pairHp[k][j] = ok ? hs[l] : 0.0f, m.pairLp[k][j] = ok ? ls[l] : 0.0f;
        }
        m.ClearAccs(frames);
        lap(1);
        // The buses through the bus Renderer (one far-field HRTF source each, the shortened filters) into the early
        // output, then the send high-pass (fourth order) and the bus gain on that binaural pair: both are linear and the
        // same for every bus, so they run on two channels instead of 14.
        float* early = m.earlyBuf.get();
        std::memset(early, 0, sizeof(float) * 2 * frames);
        m.bus.BeginBlock(frames);
        for (std::uint32_t b = 0; b < kRoomBuses; ++b) {
            SourceUpdate u{{m.busHead[b][0] * kRoomBusDistanceM, m.busHead[b][1] * kRoomBusDistanceM,
                            m.busHead[b][2] * kRoomBusDistanceM},
                           1.0f,
                           source_flags::kNoNearField | (m.busReset ? source_flags::kReset : 0u)};
            MonoInput mi{m.busBuf.get() + std::size_t(b) * m.pstride, nullptr, 1, 1.0f};
            m.bus.Render(b, u, mi, frames, 1.0f, 1.0f, early);
        }
        m.bus.EndBlock();
        m.busReset = false;
        lap(2);
        {
            const float h0 = d0.hNorm, dh = d1.hNorm - d0.hNorm;
            const rf::Biquad q0 = d1.sendHp[0], q1 = d1.sendHp[1];
            // Lanes: left, right (two idle lanes); the two sections in cascade.
            const __m128 b00 = _mm_set1_ps(q0.b0), b01 = _mm_set1_ps(q0.b1), b02 = _mm_set1_ps(q0.b2), a01 = _mm_set1_ps(q0.a1), a02 = _mm_set1_ps(q0.a2);
            const __m128 b10 = _mm_set1_ps(q1.b0), b11 = _mm_set1_ps(q1.b1), b12 = _mm_set1_ps(q1.b2), a11 = _mm_set1_ps(q1.a1), a12 = _mm_set1_ps(q1.a2);
            __m128 s01 = _mm_setr_ps(m.earlyHp[0][0].z1, m.earlyHp[1][0].z1, 0, 0), s02 = _mm_setr_ps(m.earlyHp[0][0].z2, m.earlyHp[1][0].z2, 0, 0);
            __m128 s11 = _mm_setr_ps(m.earlyHp[0][1].z1, m.earlyHp[1][1].z1, 0, 0), s12 = _mm_setr_ps(m.earlyHp[0][1].z2, m.earlyHp[1][1].z2, 0, 0);
            for (std::uint32_t i = 0; i < frames; ++i) {
                const __m128 x = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<const double*>(early + 2 * i)));
                const __m128 y0 = _mm_add_ps(_mm_mul_ps(b00, x), s01);
                s01 = _mm_add_ps(_mm_sub_ps(_mm_mul_ps(b01, x), _mm_mul_ps(a01, y0)), s02);
                s02 = _mm_sub_ps(_mm_mul_ps(b02, x), _mm_mul_ps(a02, y0));
                const __m128 y1 = _mm_add_ps(_mm_mul_ps(b10, y0), s11);
                s11 = _mm_add_ps(_mm_sub_ps(_mm_mul_ps(b11, y0), _mm_mul_ps(a11, y1)), s12);
                s12 = _mm_sub_ps(_mm_mul_ps(b12, y0), _mm_mul_ps(a12, y1));
                const __m128 g = _mm_set1_ps(h0 + dh * static_cast<float>(i + 1) * invF);
                const __m128 w = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<const double*>(wet + 2 * i)));
                _mm_store_sd(reinterpret_cast<double*>(wet + 2 * i), _mm_castps_pd(_mm_add_ps(w, _mm_mul_ps(g, y1))));
            }
            float z[4][4];
            _mm_storeu_ps(z[0], s01), _mm_storeu_ps(z[1], s02), _mm_storeu_ps(z[2], s11), _mm_storeu_ps(z[3], s12);
            bool fin = true;
            for (int ear = 0; ear < 2; ++ear) {
                m.earlyHp[ear][0].z1 = z[0][ear], m.earlyHp[ear][0].z2 = z[1][ear];
                m.earlyHp[ear][1].z1 = z[2][ear], m.earlyHp[ear][1].z2 = z[3][ear];
                for (int q = 0; q < 4; ++q) fin = fin && std::isfinite(z[q][ear]);
            }
            if (!fin) {
                std::memset(m.earlyHp, 0, sizeof(m.earlyHp));
                std::memset(wet, 0, sizeof(float) * 2 * frames);
                m.sNonFinite.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!m.busInputThisBlock) m.framesSinceBus += frames;
        m.busActive = true;
    } else if (m.busActive) {
        // The buses' tails are done: stop rendering them (no cost while the early reflections are off).
        std::memset(m.pairHp, 0, sizeof(m.pairHp));
        std::memset(m.pairLp, 0, sizeof(m.pairLp));
        std::memset(m.earlyHp, 0, sizeof(m.earlyHp));
        m.busActive = false;
        m.busReset = true;
    }

    lap(3);
    // ---- late tail ----
    if (m.lateInputThisBlock) m.framesSinceLate = 0, m.lateActive = true;
    if (m.lateActive) {
        float* late = m.late.get();
        float* pre = m.pre.get();
        const std::int32_t pd0 = d0.preDelay, pd1 = d1.preDelay;
        for (std::uint32_t i = 0; i < frames; ++i) {
            pre[m.preW & m.preMask] = late[i];
            float y = pre[(m.preW - static_cast<std::uint32_t>(pd1)) & m.preMask];
            if (pd0 != pd1) {
                const float a = pre[(m.preW - static_cast<std::uint32_t>(pd0)) & m.preMask];
                y = a + (y - a) * static_cast<float>(i + 1) * invF;
            }
            late[i] = rf::Tick(d1.sendHp[1], m.lateHp[1], rf::Tick(d1.sendHp[0], m.lateHp[0], y));
            ++m.preW;
        }
        m.fdn.Process(late, m.x1.get(), m.x2.get(), frames, d0.gLo, d1.gLo, d0.gHi, d1.gHi, d1.xA);
        const float g0 = d0.lateGain, dg = d1.lateGain - d0.lateGain;
        const int cur = m.eqCur, nxt = 1 - m.eqCur;
        double energy = 0.0;
        // The diffuse EQ of both ears side by side (lanes left, right): three biquads in cascade.
        __m128 qb0[3], qb1[3], qb2[3], qa1[3], qa2[3], qz1[3], qz2[3];
        for (int k = 0; k < 3; ++k) {
            const rf::Biquad& cl = m.eqCoef[cur][0][k];
            const rf::Biquad& cr = m.eqCoef[cur][1][k];
            qb0[k] = _mm_setr_ps(cl.b0, cr.b0, 0, 0), qb1[k] = _mm_setr_ps(cl.b1, cr.b1, 0, 0), qb2[k] = _mm_setr_ps(cl.b2, cr.b2, 0, 0);
            qa1[k] = _mm_setr_ps(cl.a1, cr.a1, 0, 0), qa2[k] = _mm_setr_ps(cl.a2, cr.a2, 0, 0);
            qz1[k] = _mm_setr_ps(m.eqState[cur][0][k].z1, m.eqState[cur][1][k].z1, 0, 0);
            qz2[k] = _mm_setr_ps(m.eqState[cur][0][k].z2, m.eqState[cur][1][k].z2, 0, 0);
        }
        for (std::uint32_t i = 0; i < frames; ++i) {
            float l, r;
            m.mixer.TickPair(m.x1[i], m.x2[i], &l, &r);
            float el, er;
            {
                __m128 v = _mm_setr_ps(l, r, 0, 0);
                for (int k = 0; k < 3; ++k) {
                    const __m128 y = _mm_add_ps(_mm_mul_ps(qb0[k], v), qz1[k]);
                    qz1[k] = _mm_add_ps(_mm_sub_ps(_mm_mul_ps(qb1[k], v), _mm_mul_ps(qa1[k], y)), qz2[k]);
                    qz2[k] = _mm_sub_ps(_mm_mul_ps(qb2[k], v), _mm_mul_ps(qa2[k], y));
                    v = y;
                }
                el = _mm_cvtss_f32(v), er = _mm_cvtss_f32(_mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1)));
            }
            const float t = static_cast<float>(i + 1) * invF;
            if (m.eqFade) {
                float nl = l, nr = r;
                for (int k = 0; k < 3; ++k)
                    nl = rf::Tick(m.eqCoef[nxt][0][k], m.eqState[nxt][0][k], nl), nr = rf::Tick(m.eqCoef[nxt][1][k], m.eqState[nxt][1][k], nr);
                el += (nl - el) * t, er += (nr - er) * t;
            }
            const float g = g0 + dg * t;
            wet[2 * i] += g * el;
            wet[2 * i + 1] += g * er;
            energy += double(l) * l + double(r) * r;
        }
        for (int k = 0; k < 3; ++k) {
            float z1[4], z2[4];
            _mm_storeu_ps(z1, qz1[k]), _mm_storeu_ps(z2, qz2[k]);
            m.eqState[cur][0][k].z1 = z1[0], m.eqState[cur][1][k].z1 = z1[1];
            m.eqState[cur][0][k].z2 = z2[0], m.eqState[cur][1][k].z2 = z2[1];
        }
        if (m.eqFade) m.eqCur = nxt, m.eqFade = false;
        if (!m.lateInputThisBlock) m.framesSinceLate += frames;
        if (!std::isfinite(energy) || !m.fdn.Finite()) {
            m.ResetLate();
            m.sNonFinite.fetch_add(1, std::memory_order_relaxed);
            m.lateActive = false;
            std::memset(wet, 0, sizeof(float) * 2 * frames);
        } else {
            const float rtMax = std::max(m.eff.rt60LowS, m.eff.rt60HighS);
            const std::uint64_t hold = static_cast<std::uint64_t>(3.0f * rtMax * static_cast<float>(m.fs)) + m.preSize;
            if (m.framesSinceLate > hold && energy / frames < kSilence) {
                m.ResetLate();
                m.lateActive = false;
            }
        }
    } else if (m.eqFade) {
        m.eqCur = 1 - m.eqCur, m.eqFade = false;
    }
    if (!busNow && !m.lateActive) m.sIdle.fetch_add(1, std::memory_order_relaxed);

    lap(4);
    // ---- the output: the room alone, or direct + room through the level match ----
    if (directLR) {
        m.MatchMix(outLR, directLR, frames);
    } else {
        for (std::uint32_t i = 0; i < 2 * frames; i += 4) {
            if (i + 4 <= 2 * frames) {
                _mm_storeu_ps(outLR + i, _mm_add_ps(_mm_loadu_ps(outLR + i), _mm_loadu_ps(wet + i)));
            } else {
                for (std::uint32_t k = i; k < 2 * frames; ++k) outLR[k] += wet[k];
            }
        }
    }
    lap(5);
    for (std::uint32_t k = 0; k < kRoomSections; ++k) {
        std::uint64_t cur = m.profMax[k].load(std::memory_order_relaxed);
        while (secBlock[k] > cur && !m.profMax[k].compare_exchange_weak(cur, secBlock[k], std::memory_order_relaxed)) {
        }
    }
}

// The level match (docs/COCKPIT_SPACE.md 6.2). Measured per band (LevelMatch::band, lanes direct L, R, room L, R): the
// direct sound's energy and direct + room's. Output: direct + room through the correction (the middle band's gain, ramped
// across the block, times a low and a high shelf for the other two bands). Then, at full wet, the smoothed energies give
// each band's target, 10 log10(E_direct / (mean d^2 x E_total)): what Off would have played over what plays; the gain
// moves to it at most kRoomMatchSlewDbPerS, within +-kRoomMatchMaxDb.
void Room::Impl::MatchMix(float* outLR, const float* directLR, std::uint32_t nfr) {
    LevelMatch& lm = match;
    const float* w = wetBuf.get();
    const bool forced = !std::isnan(g_forceMatchDb[0].load(std::memory_order_relaxed));
    if (forced)
        for (std::uint32_t b = 0; b < kRoomMatchBands; ++b) lm.gainDb[b] = g_forceMatchDb[b].load(std::memory_order_relaxed);
    float g1[kRoomMatchBands];
    for (std::uint32_t b = 0; b < kRoomMatchBands; ++b) g1[b] = std::pow(10.0f, lm.gainDb[b] * wetCur / 20.0f);
    const float invF = 1.0f / static_cast<float>(nfr);
    if (lm.lastBlock + 1 != blockIndex) {
        // Back after an idle stretch (or the first block): the filters start empty at bypass, so nothing of an old state
        // rings into the first block (with the wet weight rising from 0 the gains are near bypass anyway).
        std::memset(lm.z, 0, sizeof(lm.z)), std::memset(lm.eqZ, 0, sizeof(lm.eqZ));
        for (float& g : lm.appliedPrev) g = 1.0f;
    }
    lm.lastBlock = blockIndex;
    // The shelves (the low and the high band relative to the middle one), from the block's start gains to its end gains.
    const float kPi2 = 6.28318530718f;
    const float cl = std::cos(kPi2 * kRoomMatchLowHz / static_cast<float>(fs)), sl = std::sin(kPi2 * kRoomMatchLowHz / static_cast<float>(fs));
    const float ch = std::cos(kPi2 * kRoomMatchHighHz / static_cast<float>(fs)), shn = std::sin(kPi2 * kRoomMatchHighHz / static_cast<float>(fs));
    const float lo0 = 20.0f * std::log10(lm.appliedPrev[0] / lm.appliedPrev[1]), lo1 = 20.0f * std::log10(g1[0] / g1[1]);
    const float hi0 = 20.0f * std::log10(lm.appliedPrev[2] / lm.appliedPrev[1]), hi1 = 20.0f * std::log10(g1[2] / g1[1]);
    __m128 eb0[2], eb1[2], eb2[2], ea1[2], ea2[2], ez1[2], ez2[2];
    __m128 db0[2], db1[2], db2[2], da1[2], da2[2];  // per-sample steps of the coefficients
    {
        const rf::Biquad s0[2] = {ShelfFast(false, lo0, cl, sl), ShelfFast(true, hi0, ch, shn)};
        const rf::Biquad s1[2] = {ShelfFast(false, lo1, cl, sl), ShelfFast(true, hi1, ch, shn)};
        for (int k = 0; k < 2; ++k) {
            eb0[k] = _mm_set1_ps(s0[k].b0), eb1[k] = _mm_set1_ps(s0[k].b1), eb2[k] = _mm_set1_ps(s0[k].b2);
            ea1[k] = _mm_set1_ps(s0[k].a1), ea2[k] = _mm_set1_ps(s0[k].a2);
            db0[k] = _mm_set1_ps((s1[k].b0 - s0[k].b0) * invF), db1[k] = _mm_set1_ps((s1[k].b1 - s0[k].b1) * invF);
            db2[k] = _mm_set1_ps((s1[k].b2 - s0[k].b2) * invF), da1[k] = _mm_set1_ps((s1[k].a1 - s0[k].a1) * invF);
            da2[k] = _mm_set1_ps((s1[k].a2 - s0[k].a2) * invF);
        }
    }
    for (int k = 0; k < 2; ++k) ez1[k] = _mm_loadu_ps(lm.eqZ[k][0]), ez2[k] = _mm_loadu_ps(lm.eqZ[k][1]);
    __m128 bb0[3][2], bb1[3][2], bb2[3][2], ba1[3][2], ba2[3][2], bz1[3][2], bz2[3][2];
    for (int b = 0; b < 3; ++b)
        for (int k = 0; k < 2; ++k) {
            const rf::Biquad& q = lm.band[b][k];
            bb0[b][k] = _mm_set1_ps(q.b0), bb1[b][k] = _mm_set1_ps(q.b1), bb2[b][k] = _mm_set1_ps(q.b2);
            ba1[b][k] = _mm_set1_ps(q.a1), ba2[b][k] = _mm_set1_ps(q.a2);
            bz1[b][k] = _mm_loadu_ps(lm.z[b][k][0]), bz2[b][k] = _mm_loadu_ps(lm.z[b][k][1]);
        }
    auto tick = [](__m128 x, const __m128& b0, const __m128& b1, const __m128& b2, const __m128& a1, const __m128& a2,
                   __m128& z1, __m128& z2) {
        const __m128 y = _mm_add_ps(_mm_mul_ps(b0, x), z1);
        z1 = _mm_add_ps(_mm_sub_ps(_mm_mul_ps(b1, x), _mm_mul_ps(a1, y)), z2);
        z2 = _mm_sub_ps(_mm_mul_ps(b2, x), _mm_mul_ps(a2, y));
        return y;
    };
    __m128 eD[3] = {_mm_setzero_ps(), _mm_setzero_ps(), _mm_setzero_ps()}, eT[3] = {_mm_setzero_ps(), _mm_setzero_ps(), _mm_setzero_ps()};
    const float m0 = lm.appliedPrev[1], dm = g1[1] - lm.appliedPrev[1];
    for (std::uint32_t i = 0; i < nfr; ++i) {
        for (int k = 0; k < 2; ++k) {
            eb0[k] = _mm_add_ps(eb0[k], db0[k]), eb1[k] = _mm_add_ps(eb1[k], db1[k]), eb2[k] = _mm_add_ps(eb2[k], db2[k]);
            ea1[k] = _mm_add_ps(ea1[k], da1[k]), ea2[k] = _mm_add_ps(ea2[k], da2[k]);
        }
        const __m128 x = _mm_setr_ps(directLR[2 * i], directLR[2 * i + 1], w[2 * i], w[2 * i + 1]);
        for (int b = 0; b < 3; ++b) {
            const __m128 y0 = tick(x, bb0[b][0], bb1[b][0], bb2[b][0], ba1[b][0], ba2[b][0], bz1[b][0], bz2[b][0]);
            const __m128 y = tick(y0, bb0[b][1], bb1[b][1], bb2[b][1], ba1[b][1], ba2[b][1], bz1[b][1], bz2[b][1]);
            const __m128 t = _mm_add_ps(y, _mm_movehl_ps(y, y));  // lanes 0, 1: direct + room
            eD[b] = _mm_add_ps(eD[b], _mm_mul_ps(y, y));
            eT[b] = _mm_add_ps(eT[b], _mm_mul_ps(t, t));
        }
        // Direct + room, lanes 0, 1, through the two shelves and the middle band's gain.
        __m128 v = _mm_add_ps(x, _mm_movehl_ps(x, x));
        v = tick(v, eb0[0], eb1[0], eb2[0], ea1[0], ea2[0], ez1[0], ez2[0]);
        v = tick(v, eb0[1], eb1[1], eb2[1], ea1[1], ea2[1], ez1[1], ez2[1]);
        const __m128 g = _mm_set1_ps(m0 + dm * static_cast<float>(i + 1) * invF);
        const __m128 o = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<const double*>(outLR + 2 * i)));
        _mm_store_sd(reinterpret_cast<double*>(outLR + 2 * i), _mm_castps_pd(_mm_add_ps(o, _mm_mul_ps(g, v))));
    }
    for (int b = 0; b < 3; ++b)
        for (int k = 0; k < 2; ++k) _mm_storeu_ps(lm.z[b][k][0], bz1[b][k]), _mm_storeu_ps(lm.z[b][k][1], bz2[b][k]);
    for (int k = 0; k < 2; ++k) _mm_storeu_ps(lm.eqZ[k][0], ez1[k]), _mm_storeu_ps(lm.eqZ[k][1], ez2[k]);
    bool fin = true;
    for (int b = 0; b < 3; ++b)
        for (int k = 0; k < 2; ++k)
            for (int q = 0; q < 4; ++q) fin = fin && std::isfinite(lm.z[b][k][0][q]) && std::isfinite(lm.z[b][k][1][q]);
    for (int k = 0; k < 2; ++k)
        for (int q = 0; q < 4; ++q) fin = fin && std::isfinite(lm.eqZ[k][0][q]) && std::isfinite(lm.eqZ[k][1][q]);
    if (!fin) {
        std::memset(lm.z, 0, sizeof(lm.z)), std::memset(lm.eqZ, 0, sizeof(lm.eqZ));
        sNonFinite.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (std::uint32_t b = 0; b < kRoomMatchBands; ++b) lm.appliedPrev[b] = g1[b];
    double ed[kRoomMatchBands], et[kRoomMatchBands], edSum = 0.0;
    for (std::uint32_t b = 0; b < kRoomMatchBands; ++b) {
        float a[4], c[4];
        _mm_storeu_ps(a, eD[b]), _mm_storeu_ps(c, eT[b]);
        ed[b] = double(a[0]) + a[1], et[b] = double(c[0]) + c[1];
        edSum += ed[b];
    }
    // Measured only at full wet (the gain is applied as gain^wet while the wet weight moves), with voices sent, above
    // the floor; otherwise it holds.
    const bool steady = wetPrev >= 0.999f && wetCur >= 0.999f;
    if (forced || !steady || !(wSum > 0.0) || !(edSum / nfr > kMatchFloor) || !std::isfinite(edSum)) return;
    const double a = std::exp(-double(nfr) / (double(kRoomMatchTauS) * fs));
    if (!lm.primed) {
        for (std::uint32_t b = 0; b < kRoomMatchBands; ++b) lm.sD[b] = ed[b], lm.sT[b] = et[b];
        lm.sW = wSum, lm.sWd = wdSum, lm.primed = true;
    } else {
        for (std::uint32_t b = 0; b < kRoomMatchBands; ++b) lm.sD[b] = a * lm.sD[b] + (1 - a) * ed[b], lm.sT[b] = a * lm.sT[b] + (1 - a) * et[b];
        lm.sW = a * lm.sW + (1 - a) * wSum, lm.sWd = a * lm.sWd + (1 - a) * wdSum;
    }
    const double m2 = lm.sW > 0.0 ? std::clamp(lm.sWd / lm.sW, 0.25, 1.0) : 1.0;
    double sDsum = 0.0;
    for (double v : lm.sD) sDsum += v;
    const float step = kRoomMatchSlewDbPerS * static_cast<float>(nfr) / static_cast<float>(fs);
    for (std::uint32_t b = 0; b < kRoomMatchBands; ++b) {
        // A band with almost nothing in it (under -50 dB of the total) holds its gain.
        if (!(lm.sD[b] > 1e-5 * sDsum) || !(lm.sT[b] > 0.0)) continue;
        const float target = std::clamp(static_cast<float>(10.0 * std::log10(lm.sD[b] / (m2 * lm.sT[b]))), -kRoomMatchMaxDb, kRoomMatchMaxDb);
        lm.gainDb[b] += std::clamp(target - lm.gainDb[b], -step, step);
    }
    for (std::uint32_t b = 0; b < kRoomMatchBands; ++b) sMatch[b].store(lm.gainDb[b], std::memory_order_relaxed);
}

void Room::EndBlock() {
    Impl& m = *impl_;
    if (!m.inited || !m.inBlock) return;
    if (m.blockOk) {
        if (m.inputThisBlock) m.framesSinceInput = 0;
        else m.framesSinceInput += m.frames;
    }
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    const auto ns = static_cast<std::uint64_t>(static_cast<double>(t1.QuadPart - m.t0.QuadPart) * m.qpcToNs);
    m.sLastNs.store(ns, std::memory_order_relaxed);
    if (ns > m.sWorstNs.load(std::memory_order_relaxed)) m.sWorstNs.store(ns, std::memory_order_relaxed);
    m.sBlocks.fetch_add(1, std::memory_order_relaxed);
    _mm_setcsr(m.csr);
    m.inBlock = false;
}

RoomDirectState Room::DirectState() const {
    const Impl& m = *impl_;
    return RoomDirectState{m.haveParams ? &m.eff : nullptr, m.wetCur, m.canCur, m.earlyCur};
}

bool Room::Ready() const { return impl_->Ready(); }
void Room::ResetSectionMax() {
    for (auto& a : impl_->profMax) a.store(0, std::memory_order_relaxed);
}

bool Room::Idle() const {
    const Impl& m = *impl_;
    if (!m.Ready()) return true;
    return m.wetCur == 0.0f && m.wetTarget == 0.0f && !m.busActive && !m.lateActive &&
           m.framesSinceInput >= static_cast<std::uint64_t>(m.maxTapDelay);
}

RoomStats Room::GetStats() const {
    const Impl& m = *impl_;
    RoomStats s{};
    s.blocks = m.sBlocks.load(std::memory_order_relaxed);
    s.sourcesAdded = m.sAdded.load(std::memory_order_relaxed);
    s.earlyOffBlocks = m.sEarlyOff.load(std::memory_order_relaxed);
    s.frameInvalidBlocks = m.sFrameInvalid.load(std::memory_order_relaxed);
    s.nonFinite = m.sNonFinite.load(std::memory_order_relaxed);
    s.rejected = m.sRejected.load(std::memory_order_relaxed);
    s.idleBlocks = m.sIdle.load(std::memory_order_relaxed);
    s.lastBlockNs = m.sLastNs.load(std::memory_order_relaxed);
    s.worstBlockNs = m.sWorstNs.load(std::memory_order_relaxed);
    s.lastWet = m.sWet.load(std::memory_order_relaxed);
    s.lastCanopy = m.sCanopy.load(std::memory_order_relaxed);
    for (std::uint32_t b = 0; b < kRoomMatchBands; ++b) s.matchDb[b] = m.sMatch[b].load(std::memory_order_relaxed);
    for (std::uint32_t k = 0; k < kRoomSections; ++k)
        s.sectionUs[k] = m.tscPerUs > 0 ? static_cast<double>(m.prof[k].load(std::memory_order_relaxed)) / m.tscPerUs : 0.0,
        s.sectionMaxUs[k] = m.tscPerUs > 0 ? static_cast<double>(m.profMax[k].load(std::memory_order_relaxed)) / m.tscPerUs : 0.0;
    return s;
}
void Room::ResetStats() {
    Impl& m = *impl_;
    for (auto* a : {&m.sBlocks, &m.sAdded, &m.sEarlyOff, &m.sFrameInvalid, &m.sNonFinite, &m.sRejected, &m.sIdle, &m.sLastNs, &m.sWorstNs})
        a->store(0, std::memory_order_relaxed);
}

// ---- pure ------------------------------------------------------------------------------------------------------------
namespace {
bool SameParams(const RoomParams& a, const RoomParams& b) {
    if (a.family != b.family || a.strength != b.strength) return false;
    for (int k = 0; k < cockpit_space::kSurfaces; ++k)
        if (a.surface[k].distanceM != b.surface[k].distanceM || a.surface[k].reflection != b.surface[k].reflection ||
            a.surface[k].lowpassHz != b.surface[k].lowpassHz || a.surface[k].highpassHz != b.surface[k].highpassHz)
            return false;
    return a.earlyDb == b.earlyDb && a.lateDb == b.lateDb && a.rt60LowS == b.rt60LowS && a.rt60HighS == b.rt60HighS &&
           a.crossoverHz == b.crossoverHz && a.preDelayMs == b.preDelayMs && a.sendHpHz == b.sendHpHz &&
           a.throughDb == b.throughDb && a.neutralShare == b.neutralShare;
}
}  // namespace
float RoomDirectGain(const RoomParams& p, float wet, float canopyOpen, bool early, const float posAircraft[3], std::uint32_t flags) {
    return RoomDirectGainMix(p, wet, canopyOpen, early ? 1.0f : 0.0f, posAircraft, flags);
}
float RoomDirectGainMix(const RoomParams& p, float wet, float canopyOpen, float earlyMix, const float posAircraft[3],
                        std::uint32_t flags) {
    if (!(wet > 0.0f) || !posAircraft) return 1.0f;
    const float w = std::min(wet, 1.0f);
    const float em = std::isfinite(earlyMix) ? std::clamp(earlyMix, 0.0f, 1.0f) : 0.0f;
    // The voice-independent part, kept per thread while the parameters and the canopy stay the same (the module calls
    // this for every voice of a block with the same ones).
    struct Cache {
        bool valid = false;
        RoomParams p{};
        float canopy = 0.0f;
        rm::WetConsts c;
    };
    thread_local Cache cache;
    if (!cache.valid || cache.canopy != canopyOpen || !SameParams(cache.p, p)) {
        cache.c = rm::ComputeWetConsts(p, canopyOpen);
        cache.p = p, cache.canopy = canopyOpen, cache.valid = true;
    }
    rm::WetEnergy e = rm::VoiceWetEnergy(p, cache.c, canopyOpen, em > 0.0f, posAircraft, flags);
    e.early *= em * em;  // the taps' gain is scaled by the mix (Run: CappedTapGain x earlyCur)
    const float share = std::clamp(std::isfinite(p.neutralShare) ? p.neutralShare : cockpit_space::kNeutralShare, 0.0f, 1.0f);
    const float total = share * w * w * (e.early + e.late);
    return std::isfinite(total) && total > 0.0f ? 1.0f / std::sqrt(1.0f + total) : 1.0f;
}
float RoomSendDistanceGain(const RoomParams& p, const float posAircraft[3], std::uint32_t flags) {
    return posAircraft ? rm::SendDistanceGain(p, posAircraft, flags) : 1.0f;
}

// ---- harness probes (src/room_model.h declares RoomAccess) -----------------------------------------------------------
struct RoomAccess {
    static const Renderer& Bus(const Room& r) { return r.impl_->bus; }
    static void BusHead(const Room& r, std::uint32_t b, float out[3]) { std::memcpy(out, r.impl_->busHead[b], sizeof(float) * 3); }
    static float Kappa(const Room& r) { return r.impl_->hd.kappa; }
    static float HNorm(const Room& r) { return r.impl_->hd.hNorm; }
    static float EFront(const Room& r) { return r.impl_->hd.eFront; }
    static float EDiff(const Room& r) { return r.impl_->hd.eDiff; }
    static void Eq(const Room& r, int ear, void* out3) { std::memcpy(out3, r.impl_->eqCoef[r.impl_->eqCur][ear], sizeof(rf::Biquad) * 3); }
    static float EarlyMix(const Room& r) { return r.impl_->earlyCur; }
    static bool BusActive(const Room& r) { return r.impl_->busActive; }
    static bool LateActive(const Room& r) { return r.impl_->lateActive; }
    static std::uint32_t FdnLength(const Room& r, std::uint32_t l) { return r.impl_->fdn.Length(l); }
    static double BusKept(const Room& r) { return r.impl_->busKeptWorst; }
    static void Profile(Room& r, std::uint64_t out[6]) {
        for (std::uint32_t k = 0; k < kRoomSections; ++k) out[k] = r.impl_->prof[k].load(std::memory_order_relaxed);
    }
};
namespace room_probe {
const Renderer& Bus(const Room& r) { return RoomAccess::Bus(r); }
void BusHead(const Room& r, std::uint32_t b, float out[3]) { RoomAccess::BusHead(r, b, out); }
float Kappa(const Room& r) { return RoomAccess::Kappa(r); }
void ForceRoomKernels(int which) { g_forceRoomKernels.store(which); }
void ForceBusTaps(int taps48) { g_forceBusTaps.store(taps48); }
void ForceTapMinShare(float share) { g_forceTapMinShare.store(share); }
void ForceOwnTaps(int voices) { g_forceOwnTaps.store(voices); }
void ForceNoZeroSkip(bool off) { g_forceNoZeroSkip.store(off); }
void ForceMatchDb(const float* db) {
    g_forceMatchDb[1].store(db ? db[1] : 0.0f), g_forceMatchDb[2].store(db ? db[2] : 0.0f);
    g_forceMatchDb[0].store(db ? db[0] : std::numeric_limits<float>::quiet_NaN());
}
float HNorm(const Room& r) { return RoomAccess::HNorm(r); }
float EFront(const Room& r) { return RoomAccess::EFront(r); }
float EDiff(const Room& r) { return RoomAccess::EDiff(r); }
void Eq(const Room& r, int ear, void* out3) { RoomAccess::Eq(r, ear, out3); }
float EarlyMix(const Room& r) { return RoomAccess::EarlyMix(r); }
bool BusActive(const Room& r) { return RoomAccess::BusActive(r); }
bool LateActive(const Room& r) { return RoomAccess::LateActive(r); }
std::uint32_t FdnLength(const Room& r, std::uint32_t l) { return RoomAccess::FdnLength(r, l); }
void Profile(Room& r, std::uint64_t out[6]) { RoomAccess::Profile(r, out); }
double BusKept(const Room& r) { return RoomAccess::BusKept(r); }
}  // namespace room_probe

}  // namespace binaural
