// BinauralCore: SSE2 kernels (the fallback for CPUs without AVX2/FMA) and the CPU feature check.
#include <emmintrin.h>
#include <intrin.h>

#include <cmath>

#include "internal.h"

namespace binaural::kernels {
namespace {

void FirSse(const float* h, std::uint32_t taps, const float* x, float* y, std::uint32_t n) {
    const float* xc = x + taps - 1;
    std::uint32_t i = 0;
    for (; i + 32 <= n; i += 32) {
        __m128 a[8];
        for (auto& v : a) v = _mm_setzero_ps();
        const float* p = xc + i;
        for (std::uint32_t k = 0; k < taps; ++k, --p) {
            const __m128 hk = _mm_set1_ps(h[k]);
            for (int j = 0; j < 8; ++j) a[j] = _mm_add_ps(a[j], _mm_mul_ps(hk, _mm_loadu_ps(p + 4 * j)));
        }
        for (int j = 0; j < 8; ++j) _mm_storeu_ps(y + i + 4 * j, a[j]);
    }
    for (; i + 16 <= n; i += 16) {
        __m128 a0 = _mm_setzero_ps(), a1 = _mm_setzero_ps(), a2 = _mm_setzero_ps(), a3 = _mm_setzero_ps();
        const float* p = xc + i;
        for (std::uint32_t k = 0; k < taps; ++k, --p) {
            const __m128 hk = _mm_set1_ps(h[k]);
            a0 = _mm_add_ps(a0, _mm_mul_ps(hk, _mm_loadu_ps(p)));
            a1 = _mm_add_ps(a1, _mm_mul_ps(hk, _mm_loadu_ps(p + 4)));
            a2 = _mm_add_ps(a2, _mm_mul_ps(hk, _mm_loadu_ps(p + 8)));
            a3 = _mm_add_ps(a3, _mm_mul_ps(hk, _mm_loadu_ps(p + 12)));
        }
        _mm_storeu_ps(y + i, a0);
        _mm_storeu_ps(y + i + 4, a1);
        _mm_storeu_ps(y + i + 8, a2);
        _mm_storeu_ps(y + i + 12, a3);
    }
    for (; i + 4 <= n; i += 4) {
        __m128 a0 = _mm_setzero_ps();
        const float* p = xc + i;
        for (std::uint32_t k = 0; k < taps; ++k, --p) a0 = _mm_add_ps(a0, _mm_mul_ps(_mm_set1_ps(h[k]), _mm_loadu_ps(p)));
        _mm_storeu_ps(y + i, a0);
    }
    for (; i < n; ++i) {
        float acc = 0.0f;
        const float* p = xc + i;
        for (std::uint32_t k = 0; k < taps; ++k, --p) acc += h[k] * *p;
        y[i] = acc;
    }
}

void FdSse(const float* table, const float* x, std::int32_t base, float d0, float step, float* out, std::uint32_t n) {
    if (step == 0.0f) {
        const float p = static_cast<float>(base) - d0;
        const float fl = std::floor(p);
        const float ph = (p - fl) * kFdPhases;
        int r = static_cast<int>(ph);
        if (r >= kFdPhases) r = kFdPhases - 1;
        const float t = ph - static_cast<float>(r);
        alignas(16) float h[kFdTaps];
        const float* r0 = table + r * kFdTaps;
        for (int j = 0; j < kFdTaps; ++j) h[kFdTaps - 1 - j] = r0[j] + t * (r0[j + kFdTaps] - r0[j]);
        FirSse(h, kFdTaps, x + static_cast<std::int32_t>(fl) - (kFdHalf - 1), out, n);
        return;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        const float p = static_cast<float>(base + static_cast<std::int32_t>(i)) - (d0 + step * static_cast<float>(i));
        const float fl = std::floor(p);
        const float ph = (p - fl) * kFdPhases;
        int r = static_cast<int>(ph);
        if (r >= kFdPhases) r = kFdPhases - 1;
        const __m128 t = _mm_set1_ps(ph - static_cast<float>(r));
        const float* r0 = table + r * kFdTaps;
        const float* xs = x + static_cast<std::int32_t>(fl) - (kFdHalf - 1);
        __m128 acc = _mm_setzero_ps();
        for (int j = 0; j < kFdTaps; j += 4) {
            const __m128 lo = _mm_loadu_ps(r0 + j);
            const __m128 c = _mm_add_ps(lo, _mm_mul_ps(t, _mm_sub_ps(_mm_loadu_ps(r0 + kFdTaps + j), lo)));
            acc = _mm_add_ps(acc, _mm_mul_ps(c, _mm_loadu_ps(xs + j)));
        }
        acc = _mm_add_ps(acc, _mm_movehl_ps(acc, acc));
        acc = _mm_add_ss(acc, _mm_shuffle_ps(acc, acc, 1));
        out[i] = _mm_cvtss_f32(acc);
    }
}

}  // namespace

void FirNSse(const float* const* h, int count, std::uint32_t taps, const float* x, float* const* y, std::uint32_t n) {
    for (int c = 0; c < count; ++c) FirSse(h[c], taps, x, y[c], n);
}
Set Sse2() { return {&FirSse, &FirNSse, &FdSse, "SSE2"}; }

bool CpuHasAvx2Fma() {
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuid(r, 1);
    const bool fma = (r[2] & (1 << 12)) != 0, osxsave = (r[2] & (1 << 27)) != 0, avx = (r[2] & (1 << 28)) != 0;
    if (!(fma && osxsave && avx)) return false;
    if ((_xgetbv(0) & 6u) != 6u) return false;  // the OS saves the YMM state
    __cpuidex(r, 7, 0);
    return (r[1] & (1 << 5)) != 0;
}

}  // namespace binaural::kernels
