// BinauralCore: AVX2/FMA kernels (this file is compiled with /arch:AVX2; called only when the CPU has AVX2 and FMA).
#include <immintrin.h>

#include "internal.h"

// Only intrinsics and functions with internal linkage here: an inline function from a header (std::floor, std::fma,
// std::min ...) compiled with /arch:AVX2 is a COMDAT the linker may pick over the SSE2 copy of another object, which
// would run VEX code on a CPU without AVX2. The ctest binaural_avx2_object (test/check_avx2_object.ps1) checks
// that this object defines no external symbol outside binaural::kernels.
namespace binaural::kernels {
namespace {

inline float FmaSs(float a, float b, float c) {
    return _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c)));
}
inline float FloorSs(float x) {
    const __m128 v = _mm_set_ss(x);
    return _mm_cvtss_f32(_mm_floor_ss(v, v));
}

// y[i] = sum_k h[k] x[taps-1+i-k]: each tap broadcast across 8 outputs; 64 outputs per pass (8 independent FMA chains
// keep both FMA ports busy), then 32, 8 and a scalar tail.
void FirAvx2(const float* h, std::uint32_t taps, const float* x, float* y, std::uint32_t n) {
    const float* xc = x + taps - 1;
    std::uint32_t i = 0;
    for (; i + 64 <= n; i += 64) {
        __m256 a[8];
        for (auto& v : a) v = _mm256_setzero_ps();
        const float* p = xc + i;
        for (std::uint32_t k = 0; k < taps; ++k, --p) {
            const __m256 hk = _mm256_broadcast_ss(h + k);
            for (int j = 0; j < 8; ++j) a[j] = _mm256_fmadd_ps(hk, _mm256_loadu_ps(p + 8 * j), a[j]);
        }
        for (int j = 0; j < 8; ++j) _mm256_storeu_ps(y + i + 8 * j, a[j]);
    }
    for (; i + 32 <= n; i += 32) {
        __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps(), a2 = _mm256_setzero_ps(), a3 = _mm256_setzero_ps();
        const float* p = xc + i;
        for (std::uint32_t k = 0; k < taps; ++k, --p) {
            const __m256 hk = _mm256_broadcast_ss(h + k);
            a0 = _mm256_fmadd_ps(hk, _mm256_loadu_ps(p), a0);
            a1 = _mm256_fmadd_ps(hk, _mm256_loadu_ps(p + 8), a1);
            a2 = _mm256_fmadd_ps(hk, _mm256_loadu_ps(p + 16), a2);
            a3 = _mm256_fmadd_ps(hk, _mm256_loadu_ps(p + 24), a3);
        }
        _mm256_storeu_ps(y + i, a0);
        _mm256_storeu_ps(y + i + 8, a1);
        _mm256_storeu_ps(y + i + 16, a2);
        _mm256_storeu_ps(y + i + 24, a3);
    }
    for (; i + 8 <= n; i += 8) {
        __m256 a0 = _mm256_setzero_ps();
        const float* p = xc + i;
        for (std::uint32_t k = 0; k < taps; ++k, --p) a0 = _mm256_fmadd_ps(_mm256_broadcast_ss(h + k), _mm256_loadu_ps(p), a0);
        _mm256_storeu_ps(y + i, a0);
    }
    for (; i < n; ++i) {
        float acc = 0.0f;
        const float* p = xc + i;
        for (std::uint32_t k = 0; k < taps; ++k, --p) acc = FmaSs(h[k], *p, acc);
        y[i] = acc;
    }
}

// C filters x V vectors of 8 outputs, from output i.
template <int C, int V>
inline void FirBlock(const float* const* h, std::uint32_t taps, const float* xc, float* const* y, std::uint32_t i) {
    __m256 a[C][V];
    for (int c = 0; c < C; ++c)
        for (int v = 0; v < V; ++v) a[c][v] = _mm256_setzero_ps();
    const float* p = xc + i;
    for (std::uint32_t k = 0; k < taps; ++k, --p) {
        __m256 xv[V];
        for (int v = 0; v < V; ++v) xv[v] = _mm256_loadu_ps(p + 8 * v);
        for (int c = 0; c < C; ++c) {
            const __m256 hk = _mm256_broadcast_ss(h[c] + k);
            for (int v = 0; v < V; ++v) a[c][v] = _mm256_fmadd_ps(hk, xv[v], a[c][v]);
        }
    }
    for (int c = 0; c < C; ++c)
        for (int v = 0; v < V; ++v) _mm256_storeu_ps(y[c] + i + 8 * v, a[c][v]);
}

template <int C, int V>
void FirNT(const float* const* h, std::uint32_t taps, const float* x, float* const* y, std::uint32_t n) {
    const float* xc = x + taps - 1;
    std::uint32_t i = 0;
    for (; i + 8 * V <= n; i += 8 * V) FirBlock<C, V>(h, taps, xc, y, i);
    for (; i + 8 <= n; i += 8) FirBlock<C, 1>(h, taps, xc, y, i);
    for (; i < n; ++i)
        for (int c = 0; c < C; ++c) {
            float acc = 0.0f;
            const float* p = xc + i;
            for (std::uint32_t k = 0; k < taps; ++k, --p) acc = FmaSs(h[c][k], *p, acc);
            y[c][i] = acc;
        }
}

void FirNAvx2(const float* const* h, int count, std::uint32_t taps, const float* x, float* const* y, std::uint32_t n) {
    switch (count) {
        case 1: FirAvx2(h[0], taps, x, y[0], n); break;
        case 2: FirNT<2, 4>(h, taps, x, y, n); break;
        case 3: FirNT<3, 2>(h, taps, x, y, n); break;
        default: FirNT<4, 2>(h, taps, x, y, n); break;
    }
}

inline float HSum(__m256 v) {
    __m128 s = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    s = _mm_add_ps(s, _mm_movehl_ps(s, s));
    s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
    return _mm_cvtss_f32(s);
}

void FdAvx2(const float* table, const float* x, std::int32_t base, float d0, float step, float* out, std::uint32_t n) {
    if (step == 0.0f) {
        // Constant delay: one interpolated row, run as a 16-tap FIR across outputs.
        const float p = static_cast<float>(base) - d0;
        const float fl = FloorSs(p);
        const float ph = (p - fl) * kFdPhases;
        int r = static_cast<int>(ph);
        if (r >= kFdPhases) r = kFdPhases - 1;
        const float t = ph - static_cast<float>(r);
        alignas(32) float h[kFdTaps];
        const float* r0 = table + r * kFdTaps;
        for (int j = 0; j < kFdTaps; ++j) h[kFdTaps - 1 - j] = r0[j] + t * (r0[j + kFdTaps] - r0[j]);
        // out[i] = sum_j c[j] x[fl-(kFdHalf-1)+i+j] = sum_k h[k] x[(fl-(kFdHalf-1)) + kFdTaps-1 + i - k]
        FirAvx2(h, kFdTaps, x + static_cast<std::int32_t>(fl) - (kFdHalf - 1), out, n);
        return;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        const float p = static_cast<float>(base + static_cast<std::int32_t>(i)) - (d0 + step * static_cast<float>(i));
        const float fl = FloorSs(p);
        const float ph = (p - fl) * kFdPhases;
        int r = static_cast<int>(ph);
        if (r >= kFdPhases) r = kFdPhases - 1;
        const __m256 t = _mm256_set1_ps(ph - static_cast<float>(r));
        const float* r0 = table + r * kFdTaps;
        const float* xs = x + static_cast<std::int32_t>(fl) - (kFdHalf - 1);
        __m256 acc = _mm256_setzero_ps();
        for (int j = 0; j < kFdTaps; j += 8) {
            const __m256 lo = _mm256_loadu_ps(r0 + j);
            const __m256 c = _mm256_fmadd_ps(t, _mm256_sub_ps(_mm256_loadu_ps(r0 + kFdTaps + j), lo), lo);
            acc = _mm256_fmadd_ps(c, _mm256_loadu_ps(xs + j), acc);
        }
        out[i] = HSum(acc);
    }
}

}  // namespace

Set Avx2() { return {&FirAvx2, &FirNAvx2, &FdAvx2, "AVX2/FMA"}; }

// The cockpit space's tap mix (room.cpp): acc[q][i] += (gs[q] + dg[q] i) src[i], i < n, q < nu (<= 6), eight frames at a
// time, the source loaded once for all the tap's buses.
void RoomMixAvx2(const float* src, float* const* acc, const float* gs, const float* dg, int nu, std::uint32_t n) {
    __m256 g[6], st[6];
    const __m256 ramp = _mm256_setr_ps(0, 1, 2, 3, 4, 5, 6, 7);
    for (int q = 0; q < nu; ++q) {
        g[q] = _mm256_fmadd_ps(_mm256_set1_ps(dg[q]), ramp, _mm256_set1_ps(gs[q]));
        st[q] = _mm256_set1_ps(8.0f * dg[q]);
    }
    std::uint32_t i = 0;
    if (nu == 3) {
        __m256 ga = g[0], gb = g[1], gc = g[2];
        float* pa = acc[0];
        float* pb = acc[1];
        float* pc = acc[2];
        for (; i + 8 <= n; i += 8) {
            const __m256 x = _mm256_loadu_ps(src + i);
            _mm256_storeu_ps(pa + i, _mm256_fmadd_ps(ga, x, _mm256_loadu_ps(pa + i)));
            _mm256_storeu_ps(pb + i, _mm256_fmadd_ps(gb, x, _mm256_loadu_ps(pb + i)));
            _mm256_storeu_ps(pc + i, _mm256_fmadd_ps(gc, x, _mm256_loadu_ps(pc + i)));
            ga = _mm256_add_ps(ga, st[0]), gb = _mm256_add_ps(gb, st[1]), gc = _mm256_add_ps(gc, st[2]);
        }
    } else {
        for (; i + 8 <= n; i += 8) {
            const __m256 x = _mm256_loadu_ps(src + i);
            for (int q = 0; q < nu; ++q) {
                _mm256_storeu_ps(acc[q] + i, _mm256_fmadd_ps(g[q], x, _mm256_loadu_ps(acc[q] + i)));
                g[q] = _mm256_add_ps(g[q], st[q]);
            }
        }
    }
    for (; i < n; ++i)
        for (int q = 0; q < nu; ++q) acc[q][i] += (gs[q] + dg[q] * static_cast<float>(i)) * src[i];
}

}  // namespace binaural::kernels
