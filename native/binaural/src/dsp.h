// BinauralCore: control-thread DSP helpers (FFT, minimum phase, IR resampling). Never used on the audio thread.
// The offline harness uses them too (synthetic HRTF sets, band levels), so they are header-only.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace binaural::dsp {

constexpr double kPi = 3.14159265358979323846;

inline std::size_t NextPow2(std::size_t n) {
    std::size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

// In-place iterative radix-2 FFT. `inverse` divides by n.
inline void Fft(std::vector<std::complex<double>>& a, bool inverse) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * kPi / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto& x : a) x /= static_cast<double>(n);
}

// Magnitude spectrum (bins 0 .. n/2) of `ir` zero-padded to n (a power of two).
inline std::vector<double> Magnitude(const float* ir, std::size_t taps, std::size_t n) {
    std::vector<std::complex<double>> a(n);
    for (std::size_t i = 0; i < taps && i < n; ++i) a[i] = ir[i];
    Fft(a, false);
    std::vector<double> m(n / 2 + 1);
    for (std::size_t k = 0; k <= n / 2; ++k) m[k] = std::abs(a[k]);
    return m;
}

// Minimum-phase impulse response (real cepstrum) from a magnitude spectrum given at bins 0 .. n/2 of an n-point FFT.
// Magnitudes are floored at -140 dB of the peak. The result is truncated to `taps` with a half-Hann window over the
// last quarter (the hrtf_format.h rule).
inline std::vector<float> MinimumPhase(const std::vector<double>& mag, std::size_t n, std::size_t taps) {
    double peak = 0.0;
    for (double m : mag) peak = std::max(peak, m);
    const double floor = std::max(peak, 1e-30) * 1e-7;
    std::vector<std::complex<double>> c(n);
    for (std::size_t k = 0; k <= n / 2; ++k) {
        const double v = std::log(std::max(mag[k], floor));
        c[k] = v;
        if (k > 0 && k < n / 2) c[n - k] = v;
    }
    Fft(c, true);  // real cepstrum
    for (std::size_t i = 1; i < n / 2; ++i) {
        c[i] = 2.0 * c[i].real();
        c[n - i] = 0.0;
    }
    c[0] = c[0].real();
    c[n / 2] = c[n / 2].real();
    Fft(c, false);
    for (auto& x : c) x = std::exp(x);
    Fft(c, true);
    std::vector<float> out(taps, 0.0f);
    const std::size_t fade = std::max<std::size_t>(taps / 4, 1);
    for (std::size_t i = 0; i < taps && i < n; ++i) {
        double w = 1.0;
        if (i >= taps - fade) {
            const double t = static_cast<double>(i - (taps - fade) + 1) / static_cast<double>(fade + 1);
            w = 0.5 * (1.0 + std::cos(kPi * t));
        }
        out[i] = static_cast<float>(c[i].real() * w);
    }
    return out;
}

// Resamples a minimum-phase IR from rate `from` to rate `to` in the magnitude domain and rebuilds its minimum phase:
// the magnitude is read at the new bins (linear interpolation); above the old Nyquist (upsampling) the last value
// rolls off with a half-cosine to -40 dB at 1.25 x the old Nyquist. The gain of each frequency is kept (the IR samples
// are not scaled by the rate ratio: a filter's frequency response, not its sample values, is what is kept).
inline std::vector<float> ResampleMinimumPhase(const float* ir, std::size_t taps, double from, double to,
                                               std::size_t outTaps) {
    const std::size_t n1 = std::max<std::size_t>(NextPow2(taps * 4), 256);
    const std::vector<double> m1 = Magnitude(ir, taps, n1);
    const std::size_t n2 = std::max<std::size_t>(NextPow2(outTaps * 4), 256);
    std::vector<double> m2(n2 / 2 + 1);
    const double nyq1 = from * 0.5;
    const std::size_t top = n1 / 2;
    auto at = [&](double f) {
        const double pos = f / from * static_cast<double>(n1);
        const std::size_t i = std::min<std::size_t>(static_cast<std::size_t>(pos), top - 1);
        const double t = std::min(pos - static_cast<double>(i), 1.0);
        return m1[i] * (1.0 - t) + m1[i + 1] * t;
    };
    const double edge = at(nyq1 * 0.98);
    for (std::size_t k = 0; k <= n2 / 2; ++k) {
        const double f = static_cast<double>(k) * to / static_cast<double>(n2);
        if (f <= nyq1 * 0.98) {
            m2[k] = at(f);
        } else {
            const double t = std::min((f - nyq1 * 0.98) / (nyq1 * 1.25 - nyq1 * 0.98), 1.0);
            m2[k] = edge * (0.01 + 0.99 * 0.5 * (1.0 + std::cos(kPi * t)));
        }
    }
    return MinimumPhase(m2, n2, outTaps);
}

// Energy of `x` in the band [lo, hi) Hz (FFT of n points, rate `fs`).
inline double BandEnergy(const std::vector<double>& mag, std::size_t n, double fs, double lo, double hi) {
    double e = 0.0;
    for (std::size_t k = 1; k <= n / 2; ++k) {
        const double f = static_cast<double>(k) * fs / static_cast<double>(n);
        if (f >= lo && f < hi) e += mag[k] * mag[k];
    }
    return e;
}

}  // namespace binaural::dsp
