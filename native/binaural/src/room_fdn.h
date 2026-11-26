// BinauralCore room internals: the late tail (docs/COCKPIT_SPACE.md 4.3). An 8-line feedback delay network with a
// Hadamard feedback matrix and a two-band absorption filter per line, the interaural coherence mixer and the biquads of
// the diffuse-field EQ. Header-only; Init allocates (control thread), everything else is bounded and allocation-free
// (audio thread). Not part of the interface.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

#include "binaural_room.h"
#include "room_model.h"

namespace binaural::room_fdn {

// ---- biquads (RBJ cookbook), transposed direct form II -------------------------------------------------------------
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};
struct BiquadState {
    float z1 = 0, z2 = 0;
};
inline float Tick(const Biquad& q, BiquadState& s, float x) {
    const float y = q.b0 * x + s.z1;
    s.z1 = q.b1 * x - q.a1 * y + s.z2;
    s.z2 = q.b2 * x - q.a2 * y;
    return y;
}
inline Biquad Normalised(double b0, double b1, double b2, double a0, double a1, double a2) {
    Biquad q;
    q.b0 = static_cast<float>(b0 / a0), q.b1 = static_cast<float>(b1 / a0), q.b2 = static_cast<float>(b2 / a0);
    q.a1 = static_cast<float>(a1 / a0), q.a2 = static_cast<float>(a2 / a0);
    return q;
}
constexpr double kPi = 3.14159265358979323846;
inline Biquad Highpass(double fc, double q, double fs) {
    fc = std::clamp(fc, 5.0, 0.45 * fs);
    const double w = 2 * kPi * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
    return Normalised((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}
inline Biquad LowShelf(double fc, double db, double fs) {
    const double A = std::pow(10.0, db / 40.0), w = 2 * kPi * fc / fs, c = std::cos(w), s = std::sin(w);
    const double al = s / 2 * std::sqrt(2.0), sa = 2 * std::sqrt(A) * al;
    return Normalised(A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
                      (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
}
inline Biquad HighShelf(double fc, double db, double fs) {
    const double A = std::pow(10.0, db / 40.0), w = 2 * kPi * fc / fs, c = std::cos(w), s = std::sin(w);
    const double al = s / 2 * std::sqrt(2.0), sa = 2 * std::sqrt(A) * al;
    return Normalised(A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
                      (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
}
inline Biquad Peak(double fc, double db, double q, double fs) {
    const double A = std::pow(10.0, db / 40.0), w = 2 * kPi * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
    return Normalised(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}
// |H(f)| of a biquad.
inline double Magnitude(const Biquad& q, double f, double fs) {
    const double w = 2 * kPi * f / fs;
    const double cr = std::cos(w), ci = -std::sin(w), c2r = std::cos(2 * w), c2i = -std::sin(2 * w);
    const double nr = q.b0 + q.b1 * cr + q.b2 * c2r, ni = q.b1 * ci + q.b2 * c2i;
    const double dr = 1 + q.a1 * cr + q.a2 * c2r, di = q.a1 * ci + q.a2 * c2i;
    return std::sqrt((nr * nr + ni * ni) / std::max(dr * dr + di * di, 1e-30));
}

// ---- the feedback delay network -------------------------------------------------------------------------------------
// Line lengths: kFdnLengths48 scaled to the rate and moved to the nearest prime (mutually prime, no common echoes).
inline bool IsPrime(std::uint32_t n) {
    if (n < 2) return false;
    for (std::uint32_t d = 2; d * d <= n; ++d)
        if (n % d == 0) return false;
    return true;
}
inline std::uint32_t ScaledPrime(std::uint32_t len48, std::uint32_t fs) {
    const std::uint32_t n = std::max<std::uint32_t>(2u, static_cast<std::uint32_t>(std::lround(double(len48) * fs / 48000.0)));
    for (std::uint32_t d = 0; d < n; ++d) {
        if (IsPrime(n + d)) return n + d;
        if (d < n && IsPrime(n - d)) return n - d;
    }
    return n;
}

class Fdn {
public:
    static constexpr std::uint32_t kLines = kRoomFdnLines;
    // Input distribution and the two output vectors: +-1 / sqrt(8), mutually orthogonal, over all eight lines (each
    // output sees every line, so both have the same power in every band; outputs from disjoint line subsets differed by
    // up to 1.5 dB per band, which the coherence mixer turns into interaural correlation). The lag structure the
    // lines leave between the outputs is broken up by the Mixer's allpasses.
    static constexpr float kIn[kLines] = {1, -1, -1, 1, 1, 1, -1, -1};
    static constexpr float kOut1[kLines] = {1, 1, -1, -1, 1, -1, 1, -1};
    static constexpr float kOut2[kLines] = {1, -1, 1, -1, -1, -1, 1, 1};

    bool Init(std::uint32_t fs) {
        std::uint32_t longest = 0;
        for (std::uint32_t l = 0; l < kLines; ++l) {
            len_[l] = ScaledPrime(room_model::kFdnLengths48[l], fs);
            longest = std::max(longest, len_[l]);
        }
        size_ = 1;
        while (size_ < longest + 1) size_ <<= 1;
        buf_.reset(new (std::nothrow) float[std::size_t(size_) * kLines]);
        if (!buf_) return false;
        Reset();
        return true;
    }
    void Reset() {
        if (buf_) std::memset(buf_.get(), 0, sizeof(float) * size_ * kLines);
        std::memset(lp_, 0, sizeof(lp_));
        w_ = 0;
    }
    std::uint32_t Length(std::uint32_t l) const { return len_[l]; }
    // n samples of `in` (may be null: silence) -> o1, o2. Per-line gains low (DC) and high (Nyquist) ramp linearly from
    // g*0 to g*1 across the call; xA is the absorption crossover's one-pole coefficient. Each line's filter is
    // g_hi + (g_lo - g_hi) LP(x): a convex mix of the two band gains, so every line gain stays below 1 (stable).
    void Process(const float* in, float* o1, float* o2, std::uint32_t n, const float* gLo0, const float* gLo1,
                 const float* gHi0, const float* gHi1, float xA) {
        const float scale = 0.35355339f;  // 1 / sqrt(8)
        const std::uint32_t mask = size_ - 1;
        float* b = buf_.get();
        float dLo[kLines], dHi[kLines], lo[kLines], hi[kLines];
        const float inv = n ? 1.0f / static_cast<float>(n) : 0.0f;
        for (std::uint32_t l = 0; l < kLines; ++l) {
            dLo[l] = (gLo1[l] - gLo0[l]) * inv, dHi[l] = (gHi1[l] - gHi0[l]) * inv;
            lo[l] = gLo0[l], hi[l] = gHi0[l];
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            float f[kLines];
            float a1 = 0.0f, a2 = 0.0f;
            for (std::uint32_t l = 0; l < kLines; ++l) {
                lo[l] += dLo[l], hi[l] += dHi[l];
                const float y = b[l * size_ + ((w_ - len_[l]) & mask)];
                lp_[l] += xA * (y - lp_[l]);
                f[l] = hi[l] * y + (lo[l] - hi[l]) * lp_[l];
                a1 += kOut1[l] * f[l], a2 += kOut2[l] * f[l];
            }
            o1[i] = a1 * scale, o2[i] = a2 * scale;
            // Fast Walsh-Hadamard transform (Sylvester order), 24 additions.
            for (std::uint32_t h = 1; h < kLines; h <<= 1)
                for (std::uint32_t j = 0; j < kLines; j += h << 1)
                    for (std::uint32_t k = j; k < j + h; ++k) {
                        const float x = f[k], y = f[k + h];
                        f[k] = x + y, f[k + h] = x - y;
                    }
            const float x = in ? in[i] * scale : 0.0f;
            for (std::uint32_t l = 0; l < kLines; ++l) b[l * size_ + (w_ & mask)] = f[l] * scale + kIn[l] * x;
            ++w_;
        }
    }
    // The energy held in the lines (sum of squares), for the silence check.
    float StoredEnergy() const {
        double e = 0.0;
        const float* b = buf_.get();
        for (std::uint32_t i = 0; i < size_ * kLines; ++i) e += double(b[i]) * b[i];
        return static_cast<float>(e);
    }
    bool Finite() const {
        for (std::uint32_t l = 0; l < kLines; ++l)
            if (!std::isfinite(lp_[l])) return false;
        return true;
    }

private:
    std::uint32_t len_[kLines] = {};
    std::uint32_t size_ = 0, w_ = 0;
    std::unique_ptr<float[]> buf_;
    float lp_[kLines] = {};
};

// The per-line gains of the two bands for RT60s (seconds) at rate fs.
inline void LineGains(const Fdn& fdn, float rtLow, float rtHigh, float fs, float* gLo, float* gHi) {
    for (std::uint32_t l = 0; l < Fdn::kLines; ++l) {
        gLo[l] = room_model::LineGain(fdn.Length(l), rtLow, fs);
        gHi[l] = room_model::LineGain(fdn.Length(l), rtHigh, fs);
    }
}

// ---- interaural coherence -------------------------------------------------------------------------------------------
// L = (x1 + H x2) / sqrt 2, R = (x1 - H x2) / sqrt 2 with H = eps + (1 - eps) HP2(650 Hz): coherence
// (1 - |H|^2) / (1 + |H|^2): 0.9 at DC (eps = tan 0.23), above 0.9 up to 300 Hz, about 0 from 1 kHz up (a head in a
// diffuse field, in two bands).
constexpr float kMixEps = 0.23414f;     // tan(0.23 rad)
constexpr double kMixCornerHz = 650.0;
// A Schroeder allpass (flat magnitude): y[n] = -g x[n] + v[n - D], v[n] = x[n] + g v[n - D].
struct Allpass {
    static constexpr std::uint32_t kMax = 512;
    float buf[kMax] = {};
    std::uint32_t d = 1, w = 0;
    float g = 0.5f;
    void Init(std::uint32_t delay, float gain) { d = std::clamp<std::uint32_t>(delay, 1, kMax - 1), g = gain, Reset(); }
    void Reset() { std::memset(buf, 0, sizeof(buf)), w = 0; }
    float Tick(float x) {
        const float vd = buf[(w + kMax - d) % kMax];
        const float v = x + g * vd;
        buf[w] = v;
        w = (w + 1) % kMax;
        return vd - g * v;
    }
};
// Before the mixer each FDN output runs through two allpasses of its own (delays 0.77/2.35 ms and 1.10/2.02 ms at any
// rate, g 0.5): the outputs share the lines' recirculation pattern, whose copies a few hundred microseconds apart
// would otherwise correlate the ears within the +-1 ms of an IACC (C7 measured 0.42 at 1-2 kHz).
struct Mixer {
    Biquad hp;
    BiquadState s;
    Allpass ap[4];
    void Init(double fs) {
        hp = Highpass(kMixCornerHz, 0.70710678, fs), s = BiquadState{};
        const double ms[4] = {0.77, 2.35, 1.10, 2.02};
        for (int i = 0; i < 4; ++i) ap[i].Init(static_cast<std::uint32_t>(std::lround(ms[i] * 1e-3 * fs)), 0.5f);
    }
    void Reset() {
        s = BiquadState{};
        for (Allpass& a : ap) a.Reset();
    }
    void TickPair(float x1, float x2, float* l, float* r) {
        x1 = ap[1].Tick(ap[0].Tick(x1));
        x2 = ap[3].Tick(ap[2].Tick(x2));
        const float h = kMixEps * x2 + (1.0f - kMixEps) * Tick(hp, s, x2);
        *l = (x1 + h) * 0.70710678f, *r = (x1 - h) * 0.70710678f;
    }
};

}  // namespace binaural::room_fdn
