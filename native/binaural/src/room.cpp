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
#include <windows.h>

#include <algorithm>
#include <atomic>
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
};

struct Slot {
    float* line = nullptr;
    std::uint32_t w = 0;
    bool active = false;
    std::uint64_t lastBlock = 0;
    float lp[kRoomTaps] = {}, hp[kRoomTaps] = {};
    std::int32_t delay[kRoomTaps] = {};
    float bus[kRoomTaps][kRoomBuses] = {};
    float send = 0.0f, late = 0.0f;
    std::int32_t drain = 0;  // frames of silence still to run after the voice ended (its reflections play out)
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

HrtfDerived ComputeHrtfDerived(const HrtfSet& set) {
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
    d.hNorm = CalibrateEarly(set, d.eDiff);
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

    Renderer bus;
    std::unique_ptr<float[]> lines, busBuf, late, x1, x2, tmp, pre;
    // Early reflections are mixed per (tap, bus) pair over all voices; each pair runs through its wall's filters once in
    // Render (the filters are linear and the same for every voice). accDirty: pairs written this block.
    std::unique_ptr<float[]> accBuf, accZero, accSink, vin;
    std::uint16_t accDirty[kRoomTaps] = {};
    float pairHp[kRoomTaps][kRoomBuses] = {}, pairLp[kRoomTaps][kRoomBuses] = {};
    std::vector<Slot> slots;
    rf::Fdn fdn;
    rf::Mixer mixer;
    rf::BiquadState busHp[kRoomBuses][2], lateHp[2];
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
    std::uint64_t framesSinceInput = ~0ull >> 1, framesSinceBus = ~0ull >> 1, framesSinceLate = ~0ull >> 1;
    bool busActive = false, lateActive = false, busReset = true;
    // The set the bus Renderer was last given (control thread) and the one it last opened a block with (audio thread):
    // BeginBlock opens an empty bus block when they differ, so the old set is retired even while the buses do not run.
    std::atomic<const HrtfSet*> busGiven{nullptr};
    const HrtfSet* busSeen = nullptr;

    std::atomic<std::uint64_t> sBlocks{0}, sAdded{0}, sEarlyOff{0}, sFrameInvalid{0}, sNonFinite{0}, sRejected{0}, sIdle{0};
    std::atomic<std::uint64_t> sLastNs{0}, sWorstNs{0};
    std::atomic<float> sWet{0.0f}, sCanopy{0.0f};

    bool Ready() const { return inited && haveParams && haveHrtf; }

    bool Run(Slot& s, const float* a, const float* b, std::uint32_t stride, float sc, std::uint32_t nf, float w0, float w1,
             const std::int32_t* delayT, const float (*busT)[kRoomBuses], float sendT, float lateT);

    void ResetSlot(Slot& s) {
        if (s.line) std::memset(s.line, 0, sizeof(float) * lineSize);
        std::memset(s.lp, 0, sizeof(s.lp));
        std::memset(s.hp, 0, sizeof(s.hp));
        std::memset(s.bus, 0, sizeof(s.bus));
        std::memset(s.delay, 0, sizeof(s.delay));
        s.send = s.late = 0.0f;
        s.drain = 0;
        s.active = false;
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
        return d;
    }
};

Room::Room() : impl_(new Impl) {}
Room::~Room() { delete impl_; }

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
    m.lines.reset(new (std::nothrow) float[std::size_t(maxSources) * m.lineSize]());
    m.busBuf.reset(new (std::nothrow) float[std::size_t(kRoomBuses) * kMaxFramesPerCall]());
    m.late.reset(new (std::nothrow) float[kMaxFramesPerCall]());
    m.x1.reset(new (std::nothrow) float[kMaxFramesPerCall]());
    m.x2.reset(new (std::nothrow) float[kMaxFramesPerCall]());
    m.tmp.reset(new (std::nothrow) float[kChunk]());
    m.pre.reset(new (std::nothrow) float[m.preSize]());
    m.accBuf.reset(new (std::nothrow) float[std::size_t(kRoomTaps) * kRoomBuses * kMaxFramesPerCall]());
    m.accZero.reset(new (std::nothrow) float[kMaxFramesPerCall]());
    m.accSink.reset(new (std::nothrow) float[kMaxFramesPerCall]());
    m.vin.reset(new (std::nothrow) float[kChunk]());
    if (!m.accBuf || !m.accZero || !m.accSink || !m.vin) return fail("out of memory");
    if (!m.lines || !m.busBuf || !m.late || !m.x1 || !m.x2 || !m.tmp || !m.pre) return fail("out of memory");
    m.slots.resize(maxSources);
    for (std::uint32_t i = 0; i < maxSources; ++i) m.slots[i].line = m.lines.get() + std::size_t(i) * m.lineSize;
    if (!m.fdn.Init(sampleRate)) return fail("out of memory");
    m.mixer.Init(sampleRate);
    m.mix = (g_forceRoomKernels.load() != 1 && kernels::CpuHasAvx2Fma()) ? &kernels::RoomMixAvx2 : &RoomMixSse2;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    m.qpcToNs = 1e9 / static_cast<double>(f.QuadPart);
    for (std::uint32_t b = 0; b < kRoomBuses; ++b) rm::BusDirection(b, m.busHead[b]);
    m.inited = true;
    return true;
}

void Room::SetHrtf(const HrtfSet* set) {
    Impl& m = *impl_;
    if (!m.inited || !set) return;
    m.bus.SetHrtf(set);
    m.busGiven.store(set, std::memory_order_release);
    if (set->sampleRate != m.fs) return;
    m.hrtfBox.Write(ComputeHrtfDerived(*set));
}
const HrtfSet* Room::TakeRetiredHrtf() { return impl_->inited ? impl_->bus.TakeRetiredHrtf() : nullptr; }
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
    m.frames = frames;
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
    m.sWet.store(m.wetCur, std::memory_order_relaxed);
    m.sCanopy.store(m.canCur, std::memory_order_relaxed);
    for (std::uint32_t b = 0; b < kRoomBuses; ++b) std::memset(m.busBuf.get() + std::size_t(b) * kMaxFramesPerCall, 0, sizeof(float) * frames);
    std::memset(m.late.get(), 0, sizeof(float) * frames);
}

// One slot's block: its weighted input (a, b, stride, scale; stride 0 reads a[0] for every frame) into its delay line
// and the late input, its taps through the wall filters onto the buses. Gains and delays ramp from the slot's previous
// values (s.send, s.late, s.delay, s.bus) to the targets across `frames`; the targets become the slot's values. False
// when a filter state went non-finite (the caller resets the slot).
bool Room::Impl::Run(Slot& s, const float* a, const float* b, std::uint32_t stride, float sc, std::uint32_t nf, float w0,
                     float w1, const std::int32_t* delayT, const float (*busT)[kRoomBuses], float sendT, float lateT) {
    bool early = false;
    for (std::uint32_t k = 0; k < kRoomTaps && !early; ++k)
        for (std::uint32_t j = 0; j < kRoomBuses; ++j)
            if (s.bus[k][j] != 0.0f || busT[k][j] != 0.0f) {
                early = true;
                break;
            }
    const float invF = 1.0f / static_cast<float>(nf);
    const float s0 = s.send, ds = sendT - s.send, l0 = s.late, dl = lateT - s.late;
    const float we0 = wetPrev, dwe = wetCur - wetPrev, dw = w1 - w0;
    const std::uint32_t mask = lineMask;
    float* line = s.line;
    float* lateIn = late.get();
    float* t = tmp.get();
    const std::uint32_t base = s.w;
    bool any = false;
    for (std::uint32_t o = 0; o < nf; o += kChunk) {
        const std::uint32_t n = std::min(kChunk, nf - o);
        {
            // The weighted input: gather (any stride), then the gain ramps four frames at a time (SSE2).
            float* v = vin.get();
            if (b) {
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
            for (; i + 4 <= n; i += 4) {
                const __m128 tg = _mm_mul_ps(idx, vInv), tw = _mm_mul_ps(_mm_sub_ps(idx, vOne), vInv);
                __m128 g = _mm_mul_ps(vSc, _mm_add_ps(vS0, _mm_mul_ps(vDs, tg)));
                g = _mm_mul_ps(g, _mm_add_ps(vW0, _mm_mul_ps(vDw, tw)));
                g = _mm_mul_ps(g, _mm_add_ps(vE0, _mm_mul_ps(vDe, tg)));
                const __m128 x = _mm_mul_ps(_mm_loadu_ps(v + i), g);
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
                v[i] = x;
                lateIn[j] += x * (l0 + dl * tg);
                nz |= x != 0.0f ? 1 : 0;
            }
            any = any || nz != 0;
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
                acc[q] = accBuf.get() + (std::size_t(k) * kRoomBuses + j) * kMaxFramesPerCall + o;
                dg[q] = (g1 - g0) * invF, gs[q] = g0 + dg[q] * static_cast<float>(o + 1);
                accDirty[k] = static_cast<std::uint16_t>(accDirty[k] | (1u << j));
            }
            mix(src, acc, gs, dg, nu, n);
        }
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
    // A non-finite input sample resets the slot and adds nothing.
    {
        float z = 0.0f;
        const std::uint32_t st = in.stride;
        for (std::uint32_t i = 0; i < frames; ++i) z += in.a[std::size_t(i) * st] * 0.0f;
        if (in.b)
            for (std::uint32_t i = 0; i < frames; ++i) z += in.b[std::size_t(i) * st] * 0.0f;
        if (z != 0.0f || !std::isfinite(z)) {
            m.ResetSlot(s);
            m.sNonFinite.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    // A new voice, or one that skipped a block (its line holds stale samples): start from a clean slot, no ramps.
    const bool fresh = (src.flags & room_flags::kReset) || !s.active || s.lastBlock + 1 != m.blockIndex;
    if (fresh) m.ResetSlot(s);

    // Targets at the block's end.
    const RoomParams& p = m.eff;
    const Derived& d = m.dCur;
    rm::Tap taps[kRoomTaps];
    // Early reflections off (and faded out in the previous block too): no taps to compute, the late send only.
    const bool earlyNow = m.earlyCur > 0.0f || m.earlyPrev > 0.0f;
    if (earlyNow) {
        rm::ComputeTaps(p, m.canCur, pos, src.flags, taps);
    } else {
        for (std::uint32_t k = 0; k < kRoomTaps; ++k) taps[k] = rm::Tap{static_cast<float>(s.delay[k]) / static_cast<float>(m.fs), 0.0f, {1, 0, 0}, 0, 0};
    }
    const float sendT = std::max(src.send, 0.0f) * rm::ThroughGain(p, src.flags);
    const float lateT = (src.flags & room_flags::kLateOnly) ? rm::kLateOnlyGain : 1.0f;
    std::int32_t delayT[kRoomTaps];
    float busT[kRoomTaps][kRoomBuses] = {};
    const float fsF = static_cast<float>(m.fs);
    const float sd = rm::SendDistanceGain(p, pos, src.flags), cap = rm::TapCap(p);
    for (std::uint32_t k = 0; k < kRoomTaps; ++k) {
        delayT[k] = std::clamp(static_cast<std::int32_t>(std::lround(taps[k].delayS * fsF)), 0, m.maxTapDelay);
        const float g = rm::CappedTapGain(taps[k].gain * d.K, sd, cap) * m.earlyCur;
        if (g > 0.0f) {
            std::uint32_t idx[3];
            float pg[3];
            rm::Pan(taps[k].u, idx, pg);
            for (int j = 0; j < 3; ++j) busT[k][idx[j]] += pg[j] * g;
        }
    }
    if (fresh) {
        std::memcpy(s.delay, delayT, sizeof(delayT));
        std::memcpy(s.bus, busT, sizeof(busT));
        s.send = sendT;
        s.late = lateT;
    }
    if (!m.Run(s, in.a, in.b, in.stride, in.scale, frames, w0, w1, delayT, busT, sendT, lateT)) {
        m.ResetSlot(s);
        m.sNonFinite.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    s.drain = 0;
    m.sAdded.fetch_add(1, std::memory_order_relaxed);
    return true;
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

void Room::Render(float* outLR, std::uint32_t frames) {
    Impl& m = *impl_;
    if (!m.inited || !m.inBlock || !m.blockOk || !m.Ready() || !outLR || frames != m.frames) {
        if (m.inited && m.inBlock) m.sRejected.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const float invF = 1.0f / static_cast<float>(frames);
    const Derived& d0 = m.dPrev;
    const Derived& d1 = m.dCur;

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

    // ---- early reflections: the buses ----
    if (m.busInputThisBlock) m.framesSinceBus = 0;
    const bool busNow = m.busInputThisBlock || m.framesSinceBus < m.busTail;
    if (!busNow) {
        for (std::uint32_t k = 0; k < kRoomTaps; ++k)
            for (std::uint32_t j = 0; j < kRoomBuses; ++j)
                if ((m.accDirty[k] >> j) & 1u)
                    std::memset(m.accBuf.get() + (std::size_t(k) * kRoomBuses + j) * kMaxFramesPerCall, 0, sizeof(float) * frames);
        std::memset(m.accDirty, 0, sizeof(m.accDirty));
    }
    if (busNow) {
        // Each live (tap, bus) pair through its wall's high-pass (folded with the send high-pass) and low-pass, four
        // pairs per pass (independent recursions side by side), into its bus; the accumulators are left zeroed.
        std::uint32_t live[kRoomTaps * kRoomBuses];
        std::uint32_t nLive = 0;
        for (std::uint32_t k = 0; k < kRoomTaps; ++k)
            for (std::uint32_t j = 0; j < kRoomBuses; ++j)
                if ((m.accDirty[k] >> j) & 1u || std::fabs(m.pairHp[k][j]) + std::fabs(m.pairLp[k][j]) > 1e-12f)
                    live[nLive++] = k * kRoomBuses + j;
        for (std::uint32_t g = 0; g < nLive; g += 4) {
            float* in[4];
            float* out[4];
            float hb[4], la[4], hs[4], ls[4];
            for (std::uint32_t l = 0; l < 4; ++l) {
                if (g + l < nLive) {
                    const std::uint32_t pr = live[g + l], k = pr / kRoomBuses, j = pr % kRoomBuses;
                    in[l] = m.accBuf.get() + std::size_t(pr) * kMaxFramesPerCall;
                    out[l] = m.busBuf.get() + std::size_t(j) * kMaxFramesPerCall;
                    hb[l] = d1.hpB[k], la[l] = d1.lpA[k], hs[l] = m.pairHp[k][j], ls[l] = m.pairLp[k][j];
                } else {
                    in[l] = m.accZero.get(), out[l] = m.accSink.get();
                    hb[l] = 0.0f, la[l] = 1.0f, hs[l] = 0.0f, ls[l] = 0.0f;
                }
            }
            {
                // Four pairs in the four lanes of an SSE vector, four frames at a time (a 4x4 transpose in and out).
                __m128 vhb = _mm_loadu_ps(hb), vla = _mm_loadu_ps(la), vhs = _mm_loadu_ps(hs), vls = _mm_loadu_ps(ls);
                std::uint32_t i = 0;
                for (; i + 4 <= frames; i += 4) {
                    __m128 r0 = _mm_loadu_ps(in[0] + i), r1 = _mm_loadu_ps(in[1] + i), r2 = _mm_loadu_ps(in[2] + i), r3 = _mm_loadu_ps(in[3] + i);
                    _MM_TRANSPOSE4_PS(r0, r1, r2, r3);  // r0 = frame i of the four lanes, ...
                    __m128* rs[4] = {&r0, &r1, &r2, &r3};
                    for (int f = 0; f < 4; ++f) {
                        const __m128 x = *rs[f];
                        vhs = _mm_add_ps(vhs, _mm_mul_ps(vhb, _mm_sub_ps(x, vhs)));
                        vls = _mm_add_ps(vls, _mm_mul_ps(vla, _mm_sub_ps(_mm_sub_ps(x, vhs), vls)));
                        *rs[f] = vls;
                    }
                    _MM_TRANSPOSE4_PS(r0, r1, r2, r3);  // back: r0 = lane 0's four frames, ...
                    _mm_storeu_ps(out[0] + i, _mm_add_ps(_mm_loadu_ps(out[0] + i), r0));
                    _mm_storeu_ps(out[1] + i, _mm_add_ps(_mm_loadu_ps(out[1] + i), r1));
                    _mm_storeu_ps(out[2] + i, _mm_add_ps(_mm_loadu_ps(out[2] + i), r2));
                    _mm_storeu_ps(out[3] + i, _mm_add_ps(_mm_loadu_ps(out[3] + i), r3));
                }
                _mm_storeu_ps(hs, vhs), _mm_storeu_ps(ls, vls);
                for (; i < frames; ++i) {
                    for (std::uint32_t q = 0; q < 4; ++q) {
                        const float x = in[q][i];
                        hs[q] += hb[q] * (x - hs[q]);
                        ls[q] += la[q] * ((x - hs[q]) - ls[q]);
                    }
                    for (std::uint32_t q = 0; q < 4; ++q) out[q][i] += ls[q];
                }
            }
            for (std::uint32_t l = 0; l < 4 && g + l < nLive; ++l) {
                const std::uint32_t pr = live[g + l], k = pr / kRoomBuses, j = pr % kRoomBuses;
                const bool ok = std::isfinite(hs[l]) && std::isfinite(ls[l]);
                if (!ok) m.sNonFinite.fetch_add(1, std::memory_order_relaxed);
                m.pairHp[k][j] = ok ? hs[l] : 0.0f, m.pairLp[k][j] = ok ? ls[l] : 0.0f;
                std::memset(in[l], 0, sizeof(float) * frames);
            }
        }
        std::memset(m.accDirty, 0, sizeof(m.accDirty));
        // The send high-pass (fourth order) on each bus, and the bus gain.
        const float h0 = d0.hNorm, dh = d1.hNorm - d0.hNorm;
        // Four buses in the four lanes of an SSE vector (the 14 buses and two idle lanes), four frames at a time.
        static_assert(kRoomBuses <= 16, "four groups of four lanes");
        const rf::Biquad q0 = d1.sendHp[0], q1 = d1.sendHp[1];
        const __m128 b00 = _mm_set1_ps(q0.b0), b01 = _mm_set1_ps(q0.b1), b02 = _mm_set1_ps(q0.b2), a01 = _mm_set1_ps(q0.a1), a02 = _mm_set1_ps(q0.a2);
        const __m128 b10 = _mm_set1_ps(q1.b0), b11 = _mm_set1_ps(q1.b1), b12 = _mm_set1_ps(q1.b2), a11 = _mm_set1_ps(q1.a1), a12 = _mm_set1_ps(q1.a2);
        for (std::uint32_t b = 0; b < kRoomBuses; b += 4) {
            float* bb[4];
            float z[4][4];  // [lane][section 0 z1, z2, section 1 z1, z2]
            for (std::uint32_t q = 0; q < 4; ++q) {
                const bool real = b + q < kRoomBuses;
                bb[q] = real ? m.busBuf.get() + std::size_t(b + q) * kMaxFramesPerCall : m.accSink.get();
                if (!real) std::memset(bb[q], 0, sizeof(float) * frames);
                z[q][0] = real ? m.busHp[b + q][0].z1 : 0.0f, z[q][1] = real ? m.busHp[b + q][0].z2 : 0.0f;
                z[q][2] = real ? m.busHp[b + q][1].z1 : 0.0f, z[q][3] = real ? m.busHp[b + q][1].z2 : 0.0f;
            }
            __m128 s01 = _mm_setr_ps(z[0][0], z[1][0], z[2][0], z[3][0]), s02 = _mm_setr_ps(z[0][1], z[1][1], z[2][1], z[3][1]);
            __m128 s11 = _mm_setr_ps(z[0][2], z[1][2], z[2][2], z[3][2]), s12 = _mm_setr_ps(z[0][3], z[1][3], z[2][3], z[3][3]);
            auto step = [&](__m128 x) {
                const __m128 y0 = _mm_add_ps(_mm_mul_ps(b00, x), s01);
                s01 = _mm_add_ps(_mm_sub_ps(_mm_mul_ps(b01, x), _mm_mul_ps(a01, y0)), s02);
                s02 = _mm_sub_ps(_mm_mul_ps(b02, x), _mm_mul_ps(a02, y0));
                const __m128 y1 = _mm_add_ps(_mm_mul_ps(b10, y0), s11);
                s11 = _mm_add_ps(_mm_sub_ps(_mm_mul_ps(b11, y0), _mm_mul_ps(a11, y1)), s12);
                s12 = _mm_sub_ps(_mm_mul_ps(b12, y0), _mm_mul_ps(a12, y1));
                return y1;
            };
            std::uint32_t i = 0;
            for (; i + 4 <= frames; i += 4) {
                __m128 r0 = _mm_loadu_ps(bb[0] + i), r1 = _mm_loadu_ps(bb[1] + i), r2 = _mm_loadu_ps(bb[2] + i), r3 = _mm_loadu_ps(bb[3] + i);
                _MM_TRANSPOSE4_PS(r0, r1, r2, r3);
                r0 = step(r0), r1 = step(r1), r2 = step(r2), r3 = step(r3);
                _MM_TRANSPOSE4_PS(r0, r1, r2, r3);
                const __m128 g = _mm_add_ps(_mm_set1_ps(h0), _mm_mul_ps(_mm_set1_ps(dh * invF),
                                                                        _mm_setr_ps(float(i + 1), float(i + 2), float(i + 3), float(i + 4))));
                _mm_storeu_ps(bb[0] + i, _mm_mul_ps(r0, g)), _mm_storeu_ps(bb[1] + i, _mm_mul_ps(r1, g));
                _mm_storeu_ps(bb[2] + i, _mm_mul_ps(r2, g)), _mm_storeu_ps(bb[3] + i, _mm_mul_ps(r3, g));
            }
            for (; i < frames; ++i) {
                const __m128 y = step(_mm_setr_ps(bb[0][i], bb[1][i], bb[2][i], bb[3][i]));
                float yl[4];
                _mm_storeu_ps(yl, y);
                const float g = h0 + dh * static_cast<float>(i + 1) * invF;
                for (std::uint32_t q = 0; q < 4; ++q) bb[q][i] = yl[q] * g;
            }
            _mm_storeu_ps(z[0], s01), _mm_storeu_ps(z[1], s02), _mm_storeu_ps(z[2], s11), _mm_storeu_ps(z[3], s12);
            for (std::uint32_t q = 0; q < 4 && b + q < kRoomBuses; ++q) {
                rf::BiquadState& e0 = m.busHp[b + q][0];
                rf::BiquadState& e1 = m.busHp[b + q][1];
                e0.z1 = z[0][q], e0.z2 = z[1][q], e1.z1 = z[2][q], e1.z2 = z[3][q];
                if (!std::isfinite(e0.z1) || !std::isfinite(e0.z2) || !std::isfinite(e1.z1) || !std::isfinite(e1.z2)) {
                    std::memset(bb[q], 0, sizeof(float) * frames);
                    e0 = e1 = rf::BiquadState{};
                }
            }
        }
        m.bus.BeginBlock(frames);
        for (std::uint32_t b = 0; b < kRoomBuses; ++b) {
            SourceUpdate u{{m.busHead[b][0] * kRoomBusDistanceM, m.busHead[b][1] * kRoomBusDistanceM,
                            m.busHead[b][2] * kRoomBusDistanceM},
                           1.0f,
                           source_flags::kNoNearField | (m.busReset ? source_flags::kReset : 0u)};
            MonoInput mi{m.busBuf.get() + std::size_t(b) * kMaxFramesPerCall, nullptr, 1, 1.0f};
            m.bus.Render(b, u, mi, frames, 1.0f, 1.0f, outLR);
        }
        m.bus.EndBlock();
        m.busReset = false;
        if (!m.busInputThisBlock) m.framesSinceBus += frames;
        m.busActive = true;
    } else if (m.busActive) {
        // The buses' tails are done: stop rendering them (no cost while the early reflections are off).
        std::memset(m.pairHp, 0, sizeof(m.pairHp));
        std::memset(m.pairLp, 0, sizeof(m.pairLp));
        std::memset(m.busHp, 0, sizeof(m.busHp));
        m.busActive = false;
        m.busReset = true;
    }

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
        for (std::uint32_t i = 0; i < frames; ++i) {
            float l, r;
            m.mixer.TickPair(m.x1[i], m.x2[i], &l, &r);
            float el = l, er = r;
            for (int k = 0; k < 3; ++k)
                el = rf::Tick(m.eqCoef[cur][0][k], m.eqState[cur][0][k], el), er = rf::Tick(m.eqCoef[cur][1][k], m.eqState[cur][1][k], er);
            const float t = static_cast<float>(i + 1) * invF;
            if (m.eqFade) {
                float nl = l, nr = r;
                for (int k = 0; k < 3; ++k)
                    nl = rf::Tick(m.eqCoef[nxt][0][k], m.eqState[nxt][0][k], nl), nr = rf::Tick(m.eqCoef[nxt][1][k], m.eqState[nxt][1][k], nr);
                el += (nl - el) * t, er += (nr - er) * t;
            }
            const float g = g0 + dg * t;
            outLR[2 * i] += g * el;
            outLR[2 * i + 1] += g * er;
            energy += double(l) * l + double(r) * r;
        }
        if (m.eqFade) m.eqCur = nxt, m.eqFade = false;
        if (!m.lateInputThisBlock) m.framesSinceLate += frames;
        if (!std::isfinite(energy) || !m.fdn.Finite()) {
            m.ResetLate();
            m.sNonFinite.fetch_add(1, std::memory_order_relaxed);
            m.lateActive = false;
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
    return s;
}
void Room::ResetStats() {
    Impl& m = *impl_;
    for (auto* a : {&m.sBlocks, &m.sAdded, &m.sEarlyOff, &m.sFrameInvalid, &m.sNonFinite, &m.sRejected, &m.sIdle, &m.sLastNs, &m.sWorstNs})
        a->store(0, std::memory_order_relaxed);
}

// ---- pure ------------------------------------------------------------------------------------------------------------
float RoomDirectGain(const RoomParams& p, float wet, float canopyOpen, bool early, const float posAircraft[3], std::uint32_t flags) {
    return RoomDirectGainMix(p, wet, canopyOpen, early ? 1.0f : 0.0f, posAircraft, flags);
}
float RoomDirectGainMix(const RoomParams& p, float wet, float canopyOpen, float earlyMix, const float posAircraft[3],
                        std::uint32_t flags) {
    if (!(wet > 0.0f) || !posAircraft) return 1.0f;
    const float w = std::min(wet, 1.0f);
    const float em = std::isfinite(earlyMix) ? std::clamp(earlyMix, 0.0f, 1.0f) : 0.0f;
    rm::WetEnergy e = rm::VoiceWetEnergy(p, canopyOpen, em > 0.0f, posAircraft, flags);
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
};
namespace room_probe {
const Renderer& Bus(const Room& r) { return RoomAccess::Bus(r); }
void BusHead(const Room& r, std::uint32_t b, float out[3]) { RoomAccess::BusHead(r, b, out); }
float Kappa(const Room& r) { return RoomAccess::Kappa(r); }
void ForceRoomKernels(int which) { g_forceRoomKernels.store(which); }
float HNorm(const Room& r) { return RoomAccess::HNorm(r); }
float EFront(const Room& r) { return RoomAccess::EFront(r); }
float EDiff(const Room& r) { return RoomAccess::EDiff(r); }
void Eq(const Room& r, int ear, void* out3) { RoomAccess::Eq(r, ear, out3); }
float EarlyMix(const Room& r) { return RoomAccess::EarlyMix(r); }
bool BusActive(const Room& r) { return RoomAccess::BusActive(r); }
bool LateActive(const Room& r) { return RoomAccess::LateActive(r); }
std::uint32_t FdnLength(const Room& r, std::uint32_t l) { return RoomAccess::FdnLength(r, l); }
}  // namespace room_probe

}  // namespace binaural
