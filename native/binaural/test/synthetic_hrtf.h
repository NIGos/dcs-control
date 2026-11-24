// Offline harness: a synthetic HRTF set in the .dcshrtf format, so every renderer test runs without downloaded data.
//   - Grid: an icosphere (subdivided icosahedron), rotated so a vertex is straight ahead (and, by central symmetry,
//     straight behind); the triangulation is known by construction.
//   - Ears: Brown & Duda's spherical-head shadow per ear, H(w) = (1 + j a(t) w / 2w0) / (1 + j w / 2w0), w0 = c / r,
//     a(t) = 1.05 + 0.95 cos(pi t / 180 deg) with t the angle between the source and that ear's axis (monotonic, no
//     bright spot, so level is monotonic in the lateral angle); sources behind (x < 0) get a high shelf down to
//     -8 dB x (-x) above ~3 kHz (a pinna-like front/back cue).
//   - Delays: Woodworth per ear (ITD = r/c (phi + sin phi)), at 48 kHz.
//   - IRs: minimum phase from those magnitudes (dsp::MinimumPhase), front-normalised (front IR energy 1 per ear).
#pragma once

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "dsp.h"
#include "hrtf_format.h"

namespace synth {

struct Mesh {
    std::vector<std::array<double, 3>> v;
    std::vector<std::array<std::uint32_t, 3>> t;
};

inline std::array<double, 3> Norm(std::array<double, 3> a) {
    const double l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    return {a[0] / l, a[1] / l, a[2] / l};
}

inline Mesh Icosphere(int levels) {
    const double p = (1.0 + std::sqrt(5.0)) / 2.0;
    Mesh m;
    const double raw[12][3] = {{-1, p, 0}, {1, p, 0}, {-1, -p, 0}, {1, -p, 0}, {0, -1, p}, {0, 1, p},
                               {0, -1, -p}, {0, 1, -p}, {p, 0, -1}, {p, 0, 1}, {-p, 0, -1}, {-p, 0, 1}};
    for (auto& r : raw) m.v.push_back(Norm({r[0], r[1], r[2]}));
    m.t = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
           {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    for (int l = 0; l < levels; ++l) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> mid;
        auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
            const auto key = std::make_pair(std::min(a, b), std::max(a, b));
            const auto it = mid.find(key);
            if (it != mid.end()) return it->second;
            const auto& A = m.v[a];
            const auto& B = m.v[b];
            m.v.push_back(Norm({A[0] + B[0], A[1] + B[1], A[2] + B[2]}));
            const auto i = static_cast<std::uint32_t>(m.v.size() - 1);
            mid.emplace(key, i);
            return i;
        };
        std::vector<std::array<std::uint32_t, 3>> next;
        for (const auto& f : m.t) {
            const std::uint32_t a = midpoint(f[0], f[1]), b = midpoint(f[1], f[2]), c = midpoint(f[2], f[0]);
            next.push_back({f[0], a, c});
            next.push_back({f[1], b, a});
            next.push_back({f[2], c, b});
            next.push_back({a, b, c});
        }
        m.t.swap(next);
    }
    // Rotate vertex 0 onto +x (Rodrigues), then orient every triangle counter-clockwise from outside.
    const auto a = m.v[0];
    const std::array<double, 3> x{1, 0, 0};
    std::array<double, 3> k{a[1] * x[2] - a[2] * x[1], a[2] * x[0] - a[0] * x[2], a[0] * x[1] - a[1] * x[0]};
    const double s = std::sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]), c = a[0];
    if (s > 1e-12) {
        k = {k[0] / s, k[1] / s, k[2] / s};
        for (auto& v : m.v) {
            const double kv = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
            const std::array<double, 3> kxv{k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0]};
            for (int i = 0; i < 3; ++i) v[i] = v[i] * c + kxv[i] * s + k[i] * kv * (1 - c);
            v = Norm(v);
        }
    }
    m.v[0] = {1, 0, 0};
    for (auto& f : m.t) {
        const auto& A = m.v[f[0]];
        const auto& B = m.v[f[1]];
        const auto& C = m.v[f[2]];
        const double det = A[0] * (B[1] * C[2] - B[2] * C[1]) - A[1] * (B[0] * C[2] - B[2] * C[0]) + A[2] * (B[0] * C[1] - B[1] * C[0]);
        if (det < 0) std::swap(f[1], f[2]);
    }
    return m;
}

struct Options {
    int levels = 4;               // 2562 directions
    std::uint32_t rate = 48000;
    std::uint32_t taps = 128;
    double headRadius = 0.0875;
    double backShelfDb = -8.0;
    double alphaMin = 0.1;
    const char* name = "Synthetic sphere";
};

// Magnitude of one ear at frequency f for a unit direction d.
inline double EarMagnitude(const Options& o, const std::array<double, 3>& d, int ear, double f) {
    const double c = 343.0, w0 = c / o.headRadius, w = 2.0 * binaural::dsp::kPi * f;
    const double axis = ear == 0 ? -d[2] : d[2];
    const double t = std::acos(std::max(-1.0, std::min(1.0, axis))) * 180.0 / binaural::dsp::kPi;
    const double alpha = (1.0 + o.alphaMin / 2.0) + (1.0 - o.alphaMin / 2.0) * std::cos(binaural::dsp::kPi * t / 180.0);
    const double num = std::hypot(1.0, alpha * w / (2.0 * w0)), den = std::hypot(1.0, w / (2.0 * w0));
    double mag = num / den;
    if (d[0] < 0) {
        const double g = std::pow(10.0, o.backShelfDb * (-d[0]) / 20.0);
        const double s = (f * f) / (f * f + 3000.0 * 3000.0);
        mag *= 1.0 + (g - 1.0) * s;
    }
    return mag;
}

// Woodworth onset delay (seconds) of one ear: t = angle from that ear's axis.
inline double EarDelaySeconds(const Options& o, const std::array<double, 3>& d, int ear) {
    const double axis = ear == 0 ? -d[2] : d[2];
    const double t = std::acos(std::max(-1.0, std::min(1.0, axis)));
    const double rc = o.headRadius / 343.0;
    const double tau = t < binaural::dsp::kPi / 2 ? -rc * std::cos(t) : rc * (t - binaural::dsp::kPi / 2);
    return tau + rc;  // >= 0
}

inline void SetString(char* dst, std::size_t cap, const char* s) {
    std::memset(dst, 0, cap);
    for (std::size_t i = 0; i + 1 < cap && s[i]; ++i) dst[i] = s[i];
}

// Assembles a .dcshrtf image (4-byte aligned storage: std::vector<std::uint32_t>).
inline std::vector<std::uint32_t> Assemble(const std::vector<std::array<double, 3>>& dirs, const std::vector<float>& delays,
                                           const std::vector<std::array<std::uint32_t, 3>>& tris, const std::vector<float>& ir,
                                           std::uint32_t rate, std::uint32_t taps, float headRadius, float distance,
                                           float frontGain, std::uint32_t flags, const char* name, const char* source) {
    const std::uint32_t n = static_cast<std::uint32_t>(dirs.size()), t = static_cast<std::uint32_t>(tris.size());
    const std::uint64_t payload = hrtf_format::PayloadBytes(n, t, taps);
    std::vector<std::uint32_t> words((sizeof(hrtf_format::Header) + payload + 3) / 4, 0u);
    auto* base = reinterpret_cast<unsigned char*>(words.data());
    hrtf_format::Header h{};
    h.magic = hrtf_format::kMagic;
    h.version = hrtf_format::kVersion;
    h.headerBytes = hrtf_format::kHeaderBytes;
    h.flags = flags;
    h.sampleRate = rate;
    h.directionCount = n;
    h.triangleCount = t;
    h.taps = static_cast<std::uint16_t>(taps);
    h.datasetHeadRadiusM = headRadius;
    h.measurementDistanceM = distance;
    h.frontGain = frontGain;
    h.payloadBytes = payload;
    SetString(h.name, sizeof(h.name), name);
    SetString(h.source, sizeof(h.source), source);
    unsigned char* p = base + sizeof(h);
    for (std::uint32_t i = 0; i < n; ++i) {
        const hrtf_format::Direction d{static_cast<float>(dirs[i][0]), static_cast<float>(dirs[i][1]), static_cast<float>(dirs[i][2]), 0.0f};
        std::memcpy(p, &d, sizeof(d));
        p += sizeof(d);
    }
    std::memcpy(p, delays.data(), n * 2 * sizeof(float));
    p += n * 2 * sizeof(float);
    for (std::uint32_t i = 0; i < t; ++i) {
        const hrtf_format::Triangle tr{tris[i][0], tris[i][1], tris[i][2]};
        std::memcpy(p, &tr, sizeof(tr));
        p += sizeof(tr);
    }
    std::memcpy(p, ir.data(), ir.size() * sizeof(float));
    h.payloadFnv1a = hrtf_format::Fnv1a(base + sizeof(h), static_cast<std::size_t>(payload));
    h.headerFnv1a = hrtf_format::HeaderChecksum(h);
    std::memcpy(base, &h, sizeof(h));
    words.resize((sizeof(h) + payload) / 4);  // sizes are multiples of 4
    return words;
}

inline std::vector<std::uint32_t> Build(const Options& o) {
    const Mesh m = Icosphere(o.levels);
    const std::uint32_t n = static_cast<std::uint32_t>(m.v.size());
    const std::size_t N = 1024;
    std::vector<float> ir(std::size_t(n) * 2 * o.taps);
    std::vector<float> delays(std::size_t(n) * 2);
    for (std::uint32_t i = 0; i < n; ++i) {
        for (int e = 0; e < 2; ++e) {
            std::vector<double> mag(N / 2 + 1);
            for (std::size_t k = 0; k <= N / 2; ++k) mag[k] = EarMagnitude(o, m.v[i], e, static_cast<double>(k) * o.rate / N);
            const std::vector<float> h = binaural::dsp::MinimumPhase(mag, N, o.taps);
            std::memcpy(&ir[(std::size_t(i) * 2 + e) * o.taps], h.data(), o.taps * sizeof(float));
            delays[std::size_t(i) * 2 + e] = static_cast<float>(EarDelaySeconds(o, m.v[i], e) * o.rate);
        }
    }
    // Front normalisation: vertex 0 is straight ahead.
    double e = 0.0;
    for (int ear = 0; ear < 2; ++ear)
        for (std::uint32_t k = 0; k < o.taps; ++k) e += 0.5 * double(ir[ear * o.taps + k]) * ir[ear * o.taps + k];
    const float g = static_cast<float>(1.0 / std::sqrt(e));
    for (float& v : ir) v *= g;
    using namespace hrtf_format::flags;
    return Assemble(m.v, delays, m.t, ir, o.rate, o.taps, static_cast<float>(o.headRadius), 1.5f, g,
                    kMinimumPhase | kFrontNormalised, o.name, "synthetic test set");
}

}  // namespace synth
