// BinauralCore internals shared by the implementation files and the offline harness (native/binaural/test). Not part
// of the interface: the audio module (task B) uses only include/binaural_core.h.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "binaural_core.h"

namespace binaural {

// The fractional delay interpolator: a 24-tap Kaiser-windowed sinc (beta 6, full band), 256 phases with linear
// interpolation between them. Flat within 0.02 dB up to 0.42 x the rate (20 kHz at 48 kHz). It needs kFdHalf samples
// of look-ahead, so every delay carries kFdLatency samples (0.25 ms at 48 kHz): the renderer's whole added latency.
constexpr int kFdTaps = 24;
constexpr int kFdHalf = kFdTaps / 2;
constexpr int kFdPhases = 256;
constexpr float kFdLatency = static_cast<float>(kFdHalf);
constexpr std::uint32_t kMaxDeviceTaps = 512;
constexpr float kMaxDelayStep = 1.0f / 16.0f;      // delay change per output sample (binaural_core.h)
constexpr float kFdBlendMax = 0.1f;                // sub-block delay change rendered as a blend of its two ends
constexpr float kFilterUpdateCos = 0.99996192f;     // cos(0.5 degree): a filter direction change past it crossfades
constexpr std::uint32_t kFilterFadeFrames = 64;     // that crossfade's length (from the start of the sub-block)
constexpr float kShelfCornerHz = 1000.0f;           // near-field ILD low shelf corner
// Near field switched on or off: the parallax and the shelf fade in or out over this long, so the per-ear delays of a
// close source glide instead of jumping within one call (on the KU100 set at 0.3 m a jump within a 480-frame call
// overshot A10's -60 dBFS step limit).
constexpr float kNearFieldFadeS = 0.02f;
constexpr float kNearFieldMaxDb = 12.0f;            // added ILD at 0.25 m, 90 degrees: +8 dB near ear, -4 dB far ear
constexpr float kNearFieldRefM = 0.25f;

struct HrtfSet {
    std::uint32_t sampleRate = 0;       // device rate
    std::uint32_t fileRate = 0;
    std::uint32_t directions = 0;
    std::uint32_t triangles = 0;
    std::uint32_t taps = 0;             // device taps (multiple of 8)
    float datasetHeadRadiusM = 0.0f;
    float delayBase = 0.0f;             // samples added to every delay so that the shortest (largest head) is >= 0
    char name[64] = {};
    std::vector<float> dir;             // n * 3
    std::vector<float> delay;           // n * 2, device samples, common minimum removed
    std::vector<std::uint32_t> tri;     // t * 3
    std::vector<float> triInv;          // t * 9: rows of the inverse of [A B C] (columns), w = Inv * d
    std::vector<std::int32_t> adj;      // t * 3: neighbour across the edge opposite vertex i
    std::vector<std::int32_t> seed;     // kSeedAz * kSeedEl: a triangle containing each cell's centre
    float* ir = nullptr;                // n * 2 * taps, 32-byte aligned
    std::unique_ptr<float[]> irStorage;
    std::uint64_t serial = 0;           // unique per set (crossfade bookkeeping)
    const float* Ir(std::uint32_t d, int ear) const { return ir + (std::size_t(d) * 2u + std::size_t(ear)) * taps; }
};
constexpr int kSeedAz = 64, kSeedEl = 32;

// Point location: the triangle whose cone contains `d` (unit), its barycentric weights (>= 0, sum 1). `cache` is a
// triangle to start the walk from (-1: none) and receives the result. Bounded work: a walk of at most 128 steps from
// the cache or the seed grid, then a full scan.
void Locate(const HrtfSet& set, const float d[3], std::int32_t* cache, std::uint32_t idx[3], float w[3]);

// The onset delay (samples, device rate, before kFdLatency) of each ear for weights `w` on vertices `idx`, scaled
// about the pair's mean by `scale` = headRadius / datasetHeadRadius, plus the set's delayBase.
void InterpolatedDelays(const HrtfSet& set, const std::uint32_t idx[3], const float w[3], float scale, float out[2]);

// Near-field low-shelf gain (dB) of one ear: `cosEar` is the cosine between the source direction (from the head
// centre) and that ear's axis (+1 = straight at the ear), `r` the distance in metres. G(r) = kNearFieldMaxDb x
// clamp((1/r - 1/kNearFieldM) / (1/kNearFieldRefM - 1/kNearFieldM), 0, 1); near ear +2/3 G cosEar, far ear
// +1/3 G cosEar (a cut), so the ILD grows by G at 90 degrees; 0 at and beyond kNearFieldM.
float NearFieldShelfDb(float cosEar, float r);
// Parallax weight: 1 below 1.5 m, fading linearly to 0 at kParallaxM.
float ParallaxWeight(float r);

namespace kernels {
// y[i] = sum_{k<taps} h[k] * x[taps - 1 + i - k], i < n.
using FirFn = void (*)(const float* h, std::uint32_t taps, const float* x, float* y, std::uint32_t n);
// out[i] = sum_j table[row(i)][j] * x[i0(i) + j], with the delay d(i) = d0 + i * step:
// position p = base + i - d(i), i0 = floor(p) - (kFdHalf - 1), frac = p - floor(p) (row interpolated).
using FdFn = void (*)(const float* table, const float* x, std::int32_t base, float d0, float step, float* out,
                      std::uint32_t n);
// `count` (1..4) filters over the same input: y[c][i] = sum_{k<taps} h[c][k] * x[taps - 1 + i - k]. The input is loaded
// once per tap for all of them (both ears, or the two filters of a crossfade).
using FirNFn = void (*)(const float* const* h, int count, std::uint32_t taps, const float* x, float* const* y, std::uint32_t n);
struct Set {
    FirFn fir;
    FirNFn firN;
    FdFn fd;
    const char* name;
};
Set Avx2();
Set Sse2();
bool CpuHasAvx2Fma();
}  // namespace kernels

namespace detail {
// Harness hooks (never used by the audio module).
// 0: choose at Init (AVX2/FMA when present), 1: force SSE2, 2: force AVX2 (ignored without the CPU feature).
void ForceKernels(int which);
const char* KernelName(const Renderer& r);
// The renderer's current total delay (samples, incl. kFdLatency) of a slot's ear, and its filter weights' triangle.
float SourceDelay(const Renderer& r, std::uint32_t slot, int ear);
// The direction (head frame, unit) of the filter a slot's ear currently uses (per-ear parallax).
void SourceFilterDirection(const Renderer& r, std::uint32_t slot, int ear, float out[3]);
// The delay the renderer converges to for a far source in direction `d` with head radius `radiusM`.
void ExpectedDelays(const HrtfSet& set, const float d[3], float radiusM, float out[2]);
// Point locations that fell back to the full scan (the walk did not converge), since the process started.
std::uint64_t LocateScans();
}  // namespace detail

}  // namespace binaural
