// BinauralCore: loading an HRTF set (validate, resample to the device rate, build the lookup structures), point
// location on the measurement sphere, the delay and near-field models. Control thread, except Locate,
// InterpolatedDelays, NearFieldShelfDb and ParallaxWeight, which the audio thread calls (bounded, no allocation).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <unordered_map>

#include "dsp.h"
#include "hrtf_format.h"
#include "internal.h"

namespace binaural {
namespace {

std::atomic<std::uint64_t> g_serial{1};
std::atomic<std::uint64_t> g_locateScans{0};

void SetError(char* error, std::size_t capacity, const char* text) {
    if (!error || capacity == 0) return;
    std::snprintf(error, capacity, "%s", text);
}

int SeedCell(const float d[3]) {
    const double az = std::atan2(-static_cast<double>(d[2]), static_cast<double>(d[0]));  // -pi .. pi, left positive
    const double el = std::asin(std::clamp(static_cast<double>(d[1]), -1.0, 1.0));       // -pi/2 .. pi/2
    int ia = static_cast<int>((az + dsp::kPi) / (2.0 * dsp::kPi) * kSeedAz);
    int ie = static_cast<int>((el + dsp::kPi / 2) / dsp::kPi * kSeedEl);
    ia = std::clamp(ia, 0, kSeedAz - 1);
    ie = std::clamp(ie, 0, kSeedEl - 1);
    return ie * kSeedAz + ia;
}

inline void Weights(const HrtfSet& set, std::int32_t t, const float d[3], float w[3]) {
    const float* m = &set.triInv[std::size_t(t) * 9];
    w[0] = m[0] * d[0] + m[1] * d[1] + m[2] * d[2];
    w[1] = m[3] * d[0] + m[4] * d[1] + m[5] * d[2];
    w[2] = m[6] * d[0] + m[7] * d[1] + m[8] * d[2];
}

inline int ArgMin3(const float w[3]) { return w[0] <= w[1] ? (w[0] <= w[2] ? 0 : 2) : (w[1] <= w[2] ? 1 : 2); }

}  // namespace

void Locate(const HrtfSet& set, const float d[3], std::int32_t* cache, std::uint32_t idx[3], float w[3]) {
    constexpr float kInside = -1e-6f;
    std::int32_t t = (cache && *cache >= 0 && std::uint32_t(*cache) < set.triangles) ? *cache : set.seed[SeedCell(d)];
    bool found = false;
    for (int step = 0; step < 128; ++step) {
        Weights(set, t, d, w);
        const int k = ArgMin3(w);
        if (w[k] >= kInside) {
            found = true;
            break;
        }
        t = set.adj[std::size_t(t) * 3 + k];
    }
    if (!found) {  // bounded fallback: the triangle whose smallest weight is largest
        g_locateScans.fetch_add(1, std::memory_order_relaxed);
        float best = -1e30f;
        std::int32_t bestT = 0;
        for (std::uint32_t i = 0; i < set.triangles; ++i) {
            float wi[3];
            Weights(set, static_cast<std::int32_t>(i), d, wi);
            const float m = wi[ArgMin3(wi)];
            if (m > best) {
                best = m;
                bestT = static_cast<std::int32_t>(i);
            }
        }
        t = bestT;
        Weights(set, t, d, w);
    }
    float sum = 0.0f;
    for (int i = 0; i < 3; ++i) {
        w[i] = std::max(w[i], 0.0f);
        sum += w[i];
    }
    if (!(sum > 1e-12f)) {
        w[0] = 1.0f;
        w[1] = w[2] = 0.0f;
        sum = 1.0f;
    }
    for (int i = 0; i < 3; ++i) w[i] /= sum;
    const std::uint32_t* v = &set.tri[std::size_t(t) * 3];
    idx[0] = v[0];
    idx[1] = v[1];
    idx[2] = v[2];
    if (cache) *cache = t;
}

void InterpolatedDelays(const HrtfSet& set, const std::uint32_t idx[3], const float w[3], float scale, float out[2]) {
    float l = 0.0f, r = 0.0f;
    for (int i = 0; i < 3; ++i) {
        l += w[i] * set.delay[std::size_t(idx[i]) * 2];
        r += w[i] * set.delay[std::size_t(idx[i]) * 2 + 1];
    }
    const float m = 0.5f * (l + r);
    out[0] = set.delayBase + m + (l - m) * scale;
    out[1] = set.delayBase + m + (r - m) * scale;
}

float NearFieldShelfDb(float cosEar, float r, float fromM) {
    if (!(r < fromM)) return 0.0f;
    auto k = [](float x) {
        const float xx = std::max(x, kMinDistanceM);
        return std::clamp((1.0f / xx - 1.0f / kNearFieldM) / (1.0f / kNearFieldRefM - 1.0f / kNearFieldM), 0.0f, 1.0f);
    };
    // k(kNearFieldM) is exactly 0: a far-field set gets the model unchanged.
    const float g = kNearFieldMaxDb * std::max(0.0f, k(r) - k(fromM));
    return cosEar >= 0.0f ? (2.0f / 3.0f) * g * cosEar : (1.0f / 3.0f) * g * cosEar;
}

float ParallaxWeight(float r) {
    if (r <= 1.5f) return 1.0f;
    if (r >= kParallaxM) return 0.0f;
    return (kParallaxM - r) / (kParallaxM - 1.5f);
}

HrtfSet* LoadHrtf(const void* data, std::size_t bytes, std::uint32_t deviceRate, char* error, std::size_t errorCapacity) {
    if (deviceRate < kMinSampleRate || deviceRate > kMaxSampleRate) return SetError(error, errorCapacity, "device sample rate out of range"), nullptr;
    hrtf_format::View v;
    const char* why = nullptr;
    if (!hrtf_format::Validate(data, bytes, &v, &why)) return SetError(error, errorCapacity, why ? why : "invalid file"), nullptr;
    const hrtf_format::Header& h = *v.header;

    std::unique_ptr<HrtfSet> set(new (std::nothrow) HrtfSet());
    if (!set) return SetError(error, errorCapacity, "out of memory"), nullptr;
    try {
        HrtfSet& s = *set;
        s.sampleRate = deviceRate;
        s.fileRate = h.sampleRate;
        s.directions = h.directionCount;
        s.triangles = h.triangleCount;
        s.datasetHeadRadiusM = h.datasetHeadRadiusM;
        s.nearFromM = h.measurementDistanceM < kNearFieldM ? std::clamp(h.measurementDistanceM, kNearFieldFloorM, kNearFieldM) : kNearFieldM;
        std::memcpy(s.name, h.name, sizeof(s.name));
        s.name[sizeof(s.name) - 1] = 0;
        s.serial = g_serial.fetch_add(1);
        const std::uint32_t n = s.directions, t = s.triangles;

        s.dir.resize(std::size_t(n) * 3);
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto& d = v.directions[i];
            const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            s.dir[i * 3 + 0] = d.x / len;
            s.dir[i * 3 + 1] = d.y / len;
            s.dir[i * 3 + 2] = d.z / len;
        }

        // Triangulation: inverse matrices (the origin must be inside every triangle's cone: positive determinant for a
        // counter-clockwise triangle seen from outside) and a closed, consistently oriented adjacency.
        s.tri.resize(std::size_t(t) * 3);
        s.triInv.resize(std::size_t(t) * 9);
        s.adj.assign(std::size_t(t) * 3, -1);
        std::unordered_map<std::uint64_t, std::uint32_t> edges;  // directed edge (u,v) -> triangle * 3 + opposite vertex slot
        edges.reserve(std::size_t(t) * 3);
        for (std::uint32_t i = 0; i < t; ++i) {
            const std::uint32_t a = v.triangles[i].a, b = v.triangles[i].b, c = v.triangles[i].c;
            s.tri[i * 3 + 0] = a;
            s.tri[i * 3 + 1] = b;
            s.tri[i * 3 + 2] = c;
            const double A[3] = {s.dir[a * 3], s.dir[a * 3 + 1], s.dir[a * 3 + 2]};
            const double B[3] = {s.dir[b * 3], s.dir[b * 3 + 1], s.dir[b * 3 + 2]};
            const double C[3] = {s.dir[c * 3], s.dir[c * 3 + 1], s.dir[c * 3 + 2]};
            // M = [A B C] (columns); inverse rows = (B x C, C x A, A x B) / det.
            const double bc[3] = {B[1] * C[2] - B[2] * C[1], B[2] * C[0] - B[0] * C[2], B[0] * C[1] - B[1] * C[0]};
            const double ca[3] = {C[1] * A[2] - C[2] * A[1], C[2] * A[0] - C[0] * A[2], C[0] * A[1] - C[1] * A[0]};
            const double ab[3] = {A[1] * B[2] - A[2] * B[1], A[2] * B[0] - A[0] * B[2], A[0] * B[1] - A[1] * B[0]};
            const double det = A[0] * bc[0] + A[1] * bc[1] + A[2] * bc[2];
            if (!(det > 1e-9)) return SetError(error, errorCapacity, "triangulation does not surround the head"), nullptr;
            for (int k = 0; k < 3; ++k) {
                s.triInv[i * 9 + 0 + k] = static_cast<float>(bc[k] / det);
                s.triInv[i * 9 + 3 + k] = static_cast<float>(ca[k] / det);
                s.triInv[i * 9 + 6 + k] = static_cast<float>(ab[k] / det);
            }
            const std::uint32_t verts[3] = {a, b, c};
            for (int k = 0; k < 3; ++k) {  // edge opposite vertex k: (verts[k+1], verts[k+2])
                const std::uint64_t key = (std::uint64_t(verts[(k + 1) % 3]) << 32) | verts[(k + 2) % 3];
                if (!edges.emplace(key, i * 3 + k).second) return SetError(error, errorCapacity, "triangulation not a closed surface"), nullptr;
            }
        }
        for (const auto& e : edges) {
            const std::uint64_t twin = (e.first << 32) | (e.first >> 32);
            const auto it = edges.find(twin);
            if (it == edges.end()) return SetError(error, errorCapacity, "triangulation not a closed surface"), nullptr;
            s.adj[e.second] = static_cast<std::int32_t>(it->second / 3);
        }

        // Delays: to the device rate, common minimum removed; the base keeps the largest head's shortest delay >= 0.
        const double ratio = static_cast<double>(deviceRate) / static_cast<double>(h.sampleRate);
        s.delay.resize(std::size_t(n) * 2);
        float minDelay = 1e30f;
        for (std::uint32_t i = 0; i < n; ++i) minDelay = std::min({minDelay, v.delays[i].left, v.delays[i].right});
        for (std::uint32_t i = 0; i < n; ++i) {
            s.delay[i * 2 + 0] = static_cast<float>((v.delays[i].left - minDelay) * ratio);
            s.delay[i * 2 + 1] = static_cast<float>((v.delays[i].right - minDelay) * ratio);
        }
        // The largest ITD scale the base keeps non-negative: a 0.12 m head (the geometric radius of the largest head
        // size), and at least kMaxHeadSizeScale, the head-size range's own maximum (66 / 57 cm, PilotItdRadiusM), which
        // decides for a set fitted above 0.104 m (a personal set; SADIE II's KU100 fits 0.094 m, so its base covers a
        // 0.12 m head, scale 1.27). A larger scale only clamps the shortest delays at zero (Targets).
        const float sMax = std::max(0.12f / s.datasetHeadRadiusM, kMaxHeadSizeScale);
        float lowest = 0.0f;
        for (std::uint32_t i = 0; i < n; ++i) {
            const float l = s.delay[i * 2], r = s.delay[i * 2 + 1], m = 0.5f * (l + r);
            lowest = std::min({lowest, m + (l - m) * sMax, m + (r - m) * sMax});
        }
        s.delayBase = -lowest;
        const float maxTotal = kMaxDelayMs * 1e-3f * static_cast<float>(deviceRate);
        for (std::uint32_t i = 0; i < n; ++i) {
            const float l = s.delay[i * 2], r = s.delay[i * 2 + 1], m = 0.5f * (l + r);
            const float top = s.delayBase + m + std::max(std::fabs(l - m), std::fabs(r - m)) * sMax + kFdLatency;
            if (top > maxTotal) return SetError(error, errorCapacity, "onset delays too long"), nullptr;
        }

        // IRs at the device rate, padded to a multiple of 8 taps.
        std::uint32_t taps = static_cast<std::uint32_t>(std::ceil(h.taps * ratio));
        taps = std::clamp<std::uint32_t>((taps + 7u) & ~7u, 16u, kMaxDeviceTaps);
        s.taps = taps;
        const std::size_t count = std::size_t(n) * 2u * taps;
        s.irStorage.reset(new float[count + 8]);
        s.ir = reinterpret_cast<float*>((reinterpret_cast<std::uintptr_t>(s.irStorage.get()) + 31u) & ~std::uintptr_t(31));
        std::memset(s.ir, 0, count * sizeof(float));
        for (std::uint32_t d = 0; d < n; ++d) {
            for (int ear = 0; ear < 2; ++ear) {
                float* dst = s.ir + (std::size_t(d) * 2u + ear) * taps;
                const float* src = v.Ir(d, ear);
                if (deviceRate == h.sampleRate) {
                    std::memcpy(dst, src, std::min<std::uint32_t>(h.taps, taps) * sizeof(float));
                } else {
                    const std::vector<float> res = dsp::ResampleMinimumPhase(src, h.taps, h.sampleRate, deviceRate, taps);
                    std::memcpy(dst, res.data(), taps * sizeof(float));
                }
            }
        }

        // Seed grid for point location.
        s.seed.assign(std::size_t(kSeedAz) * kSeedEl, 0);
        std::int32_t cache = 0;
        for (int ie = 0; ie < kSeedEl; ++ie) {
            for (int ia = 0; ia < kSeedAz; ++ia) {
                const double az = (ia + 0.5) / kSeedAz * 2.0 * dsp::kPi - dsp::kPi;
                const double el = (ie + 0.5) / kSeedEl * dsp::kPi - dsp::kPi / 2;
                const float d[3] = {static_cast<float>(std::cos(el) * std::cos(az)), static_cast<float>(std::sin(el)),
                                    static_cast<float>(-std::cos(el) * std::sin(az))};
                std::uint32_t idx[3];
                float w[3];
                Locate(s, d, &cache, idx, w);
                s.seed[std::size_t(ie) * kSeedAz + ia] = cache;
            }
        }
    } catch (const std::bad_alloc&) {
        return SetError(error, errorCapacity, "out of memory"), nullptr;
    }
    if (error && errorCapacity) error[0] = 0;
    return set.release();
}

void ReleaseHrtf(HrtfSet* set) { delete set; }

HrtfInfo DescribeHrtf(const HrtfSet* set) {
    HrtfInfo info{};
    if (!set) return info;
    info.directions = set->directions;
    info.taps = set->taps;
    info.sampleRate = set->sampleRate;
    info.datasetHeadRadiusM = set->datasetHeadRadiusM;
    std::memcpy(info.name, set->name, sizeof(info.name));
    return info;
}

namespace detail {
void ExpectedDelays(const HrtfSet& set, const float d[3], float radiusM, float out[2]) {
    std::int32_t cache = -1;
    std::uint32_t idx[3];
    float w[3];
    Locate(set, d, &cache, idx, w);
    InterpolatedDelays(set, idx, w, radiusM / set.datasetHeadRadiusM, out);
    out[0] += kFdLatency;
    out[1] += kFdLatency;
}
std::uint64_t LocateScans() { return g_locateScans.load(); }
}  // namespace detail

}  // namespace binaural
