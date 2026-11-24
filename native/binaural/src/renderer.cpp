// BinauralCore: the Renderer (include/binaural_core.h). Per source and call, every kSubBlockFrames:
//   mono input -> minimum-phase FIR of each ear (barycentric interpolation; both ears, and the old and new filters
//   of a crossfade, in one pass over the shared input) -> per-ear fractional delay line (onset + scaled ITD, changing
//   per sample) -> near-field low shelf -> gain and ownership ramps -> out += .
// The FIR and the delay are both linear and time-invariant between parameter changes, so their order does not change
// the result; putting the FIR first lets both ears share one input and one pass.
// The whole call is rendered into scratch first and added only when every value is finite.
#include <immintrin.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include "dsp.h"
#include "internal.h"

namespace binaural {
namespace {

std::atomic<int> g_forceKernels{0};
constexpr std::uint32_t kHistMax = kMaxDeviceTaps - 1;  // FIR input history kept per slot
constexpr int kBufs = 3;                                // coefficient buffers per ear (current, next, swap)

double BesselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

// kFdPhases + 1 rows of kFdTaps weights: row r is the fractional position r / kFdPhases; weight j is for the sample at
// floor(p) - (kFdHalf - 1) + j. Kaiser-windowed sinc (beta 6), each row normalised to unit DC gain.
const float* FdTable() {
    static const std::vector<float> table = [] {
        std::vector<float> t(std::size_t(kFdPhases + 1) * kFdTaps);
        const double beta = 6.0, half = kFdTaps / 2.0, i0b = BesselI0(beta);
        for (int r = 0; r <= kFdPhases; ++r) {
            const double f = static_cast<double>(r) / kFdPhases;
            double sum = 0.0;
            double w[kFdTaps];
            for (int j = 0; j < kFdTaps; ++j) {
                const double x = j - (kFdTaps / 2 - 1) - f;
                const double u = x / half;
                const double win = std::fabs(u) >= 1.0 ? 0.0 : BesselI0(beta * std::sqrt(1.0 - u * u)) / i0b;
                const double s = std::fabs(x) < 1e-12 ? 1.0 : std::sin(dsp::kPi * x) / (dsp::kPi * x);
                w[j] = s * win;
                sum += w[j];
            }
            for (int j = 0; j < kFdTaps; ++j) t[std::size_t(r) * kFdTaps + j] = static_cast<float>(w[j] / sum);
        }
        return t;
    }();
    return table.data();
}

struct Slot {
    bool hasState = false;
    float dir[3] = {1, 0, 0};
    float r = 1.0f;
    float gain = 0.0f;
    float delay[2] = {kFdLatency, kFdLatency};
    float shelf[2] = {1, 1};
    float lp[2] = {0, 0};
    std::int32_t triCache[2] = {-1, -1};
    float fdir[2][3] = {};
    std::int32_t ftri[2] = {-1, -1};
    int cur[2] = {0, 0};             // coefficient buffer in use per ear
    std::uint32_t taps = 0;          // taps of the current filters (their set's)
    std::uint64_t serial = 0;        // HRTF set of the current filters
    float* coef = nullptr;           // [ear][kBufs][kMaxDeviceTaps], zero beyond each filter's taps
    float* xh = nullptr;             // FIR input history, kHistMax samples (the last taps - 1 are valid)
    float* dh = nullptr;             // [ear][H] delay-line history (FIR output)
    float* Coef(int ear, int buf) const { return coef + (std::size_t(ear) * kBufs + std::size_t(buf)) * kMaxDeviceTaps; }
};

struct EarTarget {
    float dir[3];
    std::uint32_t idx[3];
    float w[3];
    std::int32_t tri;
    float delay;
    float shelf;
};

inline bool Finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// Largest |x| as raw bits (an integer max, which vectorises; a float sum would be a serial dependency chain).
// Some value is NaN or Inf exactly when the result is >= 0x7f800000.
inline std::uint32_t MaxAbsBits(const float* x, std::uint32_t n) {
    std::uint32_t m = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t b;
        std::memcpy(&b, &x[i], 4);
        b &= 0x7fffffffu;
        m = b > m ? b : m;
    }
    return m;
}
constexpr std::uint32_t kNonFiniteBits = 0x7f800000u;

// y += (z - y) * (i + 1) / n: a linear crossfade from y to z over n samples.
inline void FadeTo(float* y, const float* z, std::uint32_t n, float a0, float da) {
    for (std::uint32_t i = 0; i < n; ++i) y[i] += (z[i] - y[i]) * (a0 + da * static_cast<float>(i));
}

}  // namespace

struct Renderer::Impl {
    Config cfg{};
    kernels::Set k{};
    const float* fd = nullptr;
    std::uint32_t H = 0;            // delay-line history per ear
    float maxDelay = 0.0f;
    float shelfCoef = 0.0f;
    std::uint32_t maxTaps = 0;      // most taps of any set adopted (coefficients are zero-filled up to it)
    Slot slots[kMaxSources];
    void* block = nullptr;          // every per-slot and scratch buffer
    float* xb = nullptr;            // kHistMax + kSubBlockFrames: FIR input
    float* zb[2] = {};              // H + kSubBlockFrames: delay-line input (FIR output) per ear
    float* y[8] = {};               // kSubBlockFrames each: FIR outputs and delay outputs
    float* mono = nullptr;          // kMaxFramesPerCall
    float* callOut = nullptr;       // 2 * kMaxFramesPerCall

    // control -> audio
    std::atomic<const HrtfSet*> pending{nullptr};
    std::atomic<const HrtfSet*> inUse{nullptr};
    std::atomic<const HrtfSet*> trying{nullptr};
    std::atomic<float> headRadius{kDefaultHeadRadiusM};
    std::atomic<bool> nearField{true};
    std::vector<const HrtfSet*> owned;  // control thread only

    // audio thread
    const HrtfSet* set = nullptr;
    bool inBlock = false;
    unsigned int csr = 0;
    LARGE_INTEGER t0{};
    float radius = kDefaultHeadRadiusM;
    bool nearOnBlock = true;

    // stats (written by the audio thread, read anywhere)
    std::atomic<std::uint64_t> blocks{0}, renders{0}, filterCrossfades{0}, hrtfSwaps{0}, nonFinite{0}, rejected{0},
        lastBlockNs{0}, worstBlockNs{0};
    std::atomic<std::uint32_t> hist[16] = {};
    double qpcToNs = 0.0;

    ~Impl() { _aligned_free(block); }

    void ResetSlot(Slot& s) {
        s.hasState = false;
        s.triCache[0] = s.triCache[1] = -1;
        s.ftri[0] = s.ftri[1] = -1;
        s.lp[0] = s.lp[1] = 0.0f;
    }

    void Targets(Slot& s, const float dir[3], float r, bool nearOn, EarTarget t[2]) const {
        const float pw = nearOn ? ParallaxWeight(r) : 0.0f;
        const float scale = radius / set->datasetHeadRadiusM;
        if (pw > 0.0f) {
            for (int e = 0; e < 2; ++e) {
                const float ez = (e == 0 ? -radius : radius) * pw;
                float v[3] = {dir[0] * r, dir[1] * r, dir[2] * r - ez};
                const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (len > 1e-6f) {
                    v[0] /= len, v[1] /= len, v[2] /= len;
                } else {
                    v[0] = dir[0], v[1] = dir[1], v[2] = dir[2];
                }
                std::memcpy(t[e].dir, v, sizeof(v));
                Locate(*set, v, &s.triCache[e], t[e].idx, t[e].w);
                t[e].tri = s.triCache[e];
                float d[2];
                InterpolatedDelays(*set, t[e].idx, t[e].w, scale, d);
                t[e].delay = d[e];
            }
        } else {
            std::memcpy(t[0].dir, dir, sizeof(float) * 3);
            Locate(*set, dir, &s.triCache[0], t[0].idx, t[0].w);
            t[0].tri = s.triCache[0];
            s.triCache[1] = s.triCache[0];
            float d[2];
            InterpolatedDelays(*set, t[0].idx, t[0].w, scale, d);
            t[1] = t[0];
            t[0].delay = d[0];
            t[1].delay = d[1];
        }
        for (int e = 0; e < 2; ++e) {
            t[e].delay = std::clamp(t[e].delay + kFdLatency, kFdLatency, maxDelay);
            const float cosEar = e == 0 ? -dir[2] : dir[2];
            t[e].shelf = (nearOn && r < kNearFieldM) ? std::pow(10.0f, NearFieldShelfDb(cosEar, r) / 20.0f) : 1.0f;
        }
    }

    void BuildFilter(const EarTarget& t, int ear, float* out) const {
        const std::uint32_t T = set->taps;
        const float* a = set->Ir(t.idx[0], ear);
        const float* b = set->Ir(t.idx[1], ear);
        const float* c = set->Ir(t.idx[2], ear);
        const float wa = t.w[0], wb = t.w[1], wc = t.w[2];
        for (std::uint32_t i = 0; i < T; ++i) out[i] = wa * a[i] + wb * b[i] + wc * c[i];
        for (std::uint32_t i = T; i < maxTaps; ++i) out[i] = 0.0f;
    }

    // The constant-delay interpolator as FIR taps (reversed row) for delay d at output 0 of a buffer whose current
    // sample is at `base`: out[i] = sum_k h[k] x[start + kFdTaps - 1 + i - k]; returns `start`.
    std::int32_t FdRow(std::int32_t base, float d, float* h) const {
        const float p = static_cast<float>(base) - d;
        const float fl = std::floor(p);
        const float ph = (p - fl) * kFdPhases;
        int r = static_cast<int>(ph);
        if (r >= kFdPhases) r = kFdPhases - 1;
        const float t = ph - static_cast<float>(r);
        const float* r0 = fd + r * kFdTaps;
        for (int j = 0; j < kFdTaps; ++j) h[kFdTaps - 1 - j] = r0[j] + t * (r0[j + kFdTaps] - r0[j]);
        return static_cast<std::int32_t>(fl) - (kFdHalf - 1);
    }

    // Delays one ear's FIR output z (L samples, after the slot's history) from s.delay[e] towards `target`.
    void Delay(Slot& s, int e, float target, std::uint32_t L, float* out) {
        float* zb_ = zb[e];
        const float diff = target - s.delay[e];
        float step = 0.0f;
        bool reached = true;
        if (std::fabs(diff) > 1e-6f) {
            step = diff / static_cast<float>(L);
            if (step > kMaxDelayStep) step = kMaxDelayStep, reached = false;
            if (step < -kMaxDelayStep) step = -kMaxDelayStep, reached = false;
        }
        const float dEnd = reached ? target : s.delay[e] + step * static_cast<float>(L);
        const auto base = static_cast<std::int32_t>(H);
        if (step != 0.0f && std::fabs(dEnd - s.delay[e]) <= kFdBlendMax && L > 1) {
            // A slow change (the usual head motion): the two constant-delay outputs at the sub-block's ends, blended per
            // sample (within 0.05 dB of the per-sample interpolator at 16 kHz, at a fraction of its cost).
            float* tmp = y[7];
            alignas(32) float rows[2][kFdTaps];
            const std::int32_t i0 = FdRow(base, s.delay[e] + step, rows[0]), i1 = FdRow(base, dEnd, rows[1]);
            if (i0 == i1) {  // the same integer delay (the usual case): one pass for both ends
                const float* hh[2] = {rows[0], rows[1]};
                float* yy[2] = {out, tmp};
                k.firN(hh, 2, kFdTaps, zb_ + i0, yy, L);
            } else {
                k.fir(rows[0], kFdTaps, zb_ + i0, out, L);
                k.fir(rows[1], kFdTaps, zb_ + i1, tmp, L);
            }
            const float inv = 1.0f / static_cast<float>(L - 1);
            for (std::uint32_t i = 0; i < L; ++i) out[i] += (tmp[i] - out[i]) * (static_cast<float>(i) * inv);
        } else {
            k.fd(fd, zb_, base, s.delay[e] + step, step, out, L);
        }
        s.delay[e] = dEnd;
    }
};

Renderer::Renderer() : impl_(new Impl()) {}
Renderer::~Renderer() { delete impl_; }

bool Renderer::Init(const Config& config, char* error, std::size_t errorCapacity) {
    auto fail = [&](const char* why) {
        if (error && errorCapacity) std::snprintf(error, errorCapacity, "%s", why);
        return false;
    };
    Impl& m = *impl_;
    if (m.block) return fail("already initialised");
    if (config.sampleRate < kMinSampleRate || config.sampleRate > kMaxSampleRate) return fail("sample rate out of range");
    if (config.maxSources < 1 || config.maxSources > kMaxSources) return fail("maxSources out of range");
    m.cfg = config;
    float radius = config.headRadiusM == 0.0f ? kDefaultHeadRadiusM : config.headRadiusM;
    if (!std::isfinite(radius)) radius = kDefaultHeadRadiusM;
    radius = std::clamp(radius, 0.06f, 0.12f);
    m.headRadius.store(radius);
    m.radius = radius;
    m.nearField.store(config.nearField);
    m.nearOnBlock = config.nearField;

    const int force = g_forceKernels.load();
    const bool avx = kernels::CpuHasAvx2Fma();
    m.k = (force == 1 || !avx) ? kernels::Sse2() : kernels::Avx2();
    m.fd = FdTable();
    m.H = static_cast<std::uint32_t>(std::ceil(kMaxDelayMs * 1e-3 * config.sampleRate)) + 2u * kFdTaps;
    m.maxDelay = static_cast<float>(m.H - kFdTaps);
    m.shelfCoef = static_cast<float>(1.0 - std::exp(-2.0 * dsp::kPi * kShelfCornerHz / config.sampleRate));

    auto up = [](std::size_t n) { return (n + 15u) & ~std::size_t(15); };  // floats, 64-byte multiples
    const std::size_t perSlot = up(2u * kBufs * kMaxDeviceTaps) + up(kHistMax) + 2 * up(m.H);
    const std::size_t scratch = up(kHistMax + kSubBlockFrames + 16) + 2 * up(m.H + kSubBlockFrames + 16) +
                                8 * up(kSubBlockFrames) + up(kMaxFramesPerCall) + up(2u * kMaxFramesPerCall);
    const std::size_t total = perSlot * config.maxSources + scratch;
    m.block = _aligned_malloc(total * sizeof(float), 64);
    if (!m.block) return fail("out of memory");
    std::memset(m.block, 0, total * sizeof(float));
    float* p = static_cast<float*>(m.block);
    for (std::uint32_t i = 0; i < config.maxSources; ++i) {
        Slot& s = m.slots[i];
        s.coef = p, p += up(2u * kBufs * kMaxDeviceTaps);
        s.xh = p, p += up(kHistMax);
        s.dh = p, p += 2 * up(m.H);
    }
    m.xb = p, p += up(kHistMax + kSubBlockFrames + 16);
    m.zb[0] = p, p += up(m.H + kSubBlockFrames + 16);
    m.zb[1] = p, p += up(m.H + kSubBlockFrames + 16);
    for (auto& y : m.y) y = p, p += up(kSubBlockFrames);
    m.mono = p, p += up(kMaxFramesPerCall);
    m.callOut = p, p += up(2u * kMaxFramesPerCall);
    m.owned.reserve(16);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    m.qpcToNs = 1e9 / static_cast<double>(f.QuadPart);
    if (error && errorCapacity) error[0] = 0;
    return true;
}

void Renderer::SetHrtf(const HrtfSet* set) {
    Impl& m = *impl_;
    if (!set) return;
    if (std::find(m.owned.begin(), m.owned.end(), set) == m.owned.end()) m.owned.push_back(set);
    m.pending.store(set);
}

const HrtfSet* Renderer::TakeRetiredHrtf() {
    Impl& m = *impl_;
    for (auto it = m.owned.begin(); it != m.owned.end(); ++it) {
        const HrtfSet* s = *it;
        if (s != m.pending.load() && s != m.inUse.load() && s != m.trying.load()) {
            m.owned.erase(it);
            return s;
        }
    }
    return nullptr;
}

void Renderer::SetHeadRadius(float metres) {
    if (!std::isfinite(metres)) return;
    impl_->headRadius.store(std::clamp(metres, 0.06f, 0.12f));
}
void Renderer::SetNearField(bool on) { impl_->nearField.store(on); }

void Renderer::BeginBlock(std::uint32_t frames) {
    (void)frames;
    Impl& m = *impl_;
    if (!m.block) return;
    if (!m.inBlock) {
        m.csr = _mm_getcsr();
        _mm_setcsr(m.csr | 0x8040u);  // FTZ | DAZ
    }
    m.inBlock = true;
    QueryPerformanceCounter(&m.t0);
    // Adopt the newest set (hazard-pointer handshake with TakeRetiredHrtf, bounded). A set built for another rate is
    // never used.
    const HrtfSet* p = m.pending.load();
    for (int tries = 0; tries < 4 && p && p != m.set; ++tries) {
        m.trying.store(p);
        const HrtfSet* again = m.pending.load();
        if (again == p) {
            if (p->sampleRate == m.cfg.sampleRate) {
                m.set = p;
                m.inUse.store(p);
                m.maxTaps = std::max(m.maxTaps, p->taps);
            }
            break;
        }
        p = again;
    }
    m.trying.store(nullptr);
    m.radius = m.headRadius.load(std::memory_order_relaxed);
    m.nearOnBlock = m.nearField.load(std::memory_order_relaxed);
}

void Renderer::EndBlock() {
    Impl& m = *impl_;
    if (!m.inBlock) return;
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    const auto ns = static_cast<std::uint64_t>(static_cast<double>(t1.QuadPart - m.t0.QuadPart) * m.qpcToNs);
    m.lastBlockNs.store(ns, std::memory_order_relaxed);
    if (ns > m.worstBlockNs.load(std::memory_order_relaxed)) m.worstBlockNs.store(ns, std::memory_order_relaxed);
    const std::uint64_t us = ns / 1000;
    int b = 0;
    if (us >= 2) {
        unsigned long idx;
        _BitScanReverse64(&idx, us);
        b = std::min(static_cast<int>(idx), 15);
    }
    m.hist[b].fetch_add(1, std::memory_order_relaxed);
    m.blocks.fetch_add(1, std::memory_order_relaxed);
    _mm_setcsr(m.csr);
    m.inBlock = false;
}

void Renderer::ResetSource(std::uint32_t slot) {
    Impl& m = *impl_;
    if (slot < m.cfg.maxSources && m.block) m.ResetSlot(m.slots[slot]);
}

bool Renderer::Render(std::uint32_t slot, const SourceUpdate& update, const MonoInput& input, std::uint32_t frames,
                      float w0, float w1, float* outLR) {
    Impl& m = *impl_;
    if (!m.inBlock || slot >= m.cfg.maxSources || !m.set || frames > kMaxFramesPerCall || !outLR || !input.a ||
        input.stride == 0) {
        m.rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (frames == 0) return true;
    Slot& s = m.slots[slot];
    const HrtfSet& set = *m.set;

    // Input and parameters must be finite.
    {
        const float* a = input.a;
        const float* b = input.b;
        const std::size_t st = input.stride;
        const float sc = input.scale;
        if (b) {
            for (std::uint32_t i = 0; i < frames; ++i) m.mono[i] = sc * (a[i * st] + b[i * st]);
        } else {
            for (std::uint32_t i = 0; i < frames; ++i) m.mono[i] = sc * a[i * st];
        }
    }
    if (MaxAbsBits(m.mono, frames) >= kNonFiniteBits || !Finite3(update.position) || !std::isfinite(update.gain) ||
        !std::isfinite(w0) || !std::isfinite(w1)) {
        m.ResetSlot(s);
        m.nonFinite.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Target position (clamped to kMinDistanceM).
    float newDir[3] = {1, 0, 0};
    float newR = std::sqrt(update.position[0] * update.position[0] + update.position[1] * update.position[1] +
                           update.position[2] * update.position[2]);
    if (newR > 1e-9f) {
        for (int i = 0; i < 3; ++i) newDir[i] = update.position[i] / newR;
    }
    newR = std::max(newR, kMinDistanceM);
    const float newGain = update.gain;
    const bool nearOn = m.nearOnBlock && !(update.flags & source_flags::kNoNearField);
    const bool reset = (update.flags & source_flags::kReset) || !s.hasState;
    const bool swap = !reset && s.serial != set.serial;
    const std::uint32_t T = set.taps;

    EarTarget tg[2];
    if (reset) {
        m.ResetSlot(s);
        std::memset(s.xh, 0, kHistMax * sizeof(float));
        std::memset(s.dh, 0, 2u * m.H * sizeof(float));
        std::memcpy(s.dir, newDir, sizeof(newDir));
        s.r = newR;
        s.gain = newGain;
        m.Targets(s, newDir, newR, nearOn, tg);
        for (int e = 0; e < 2; ++e) {
            s.delay[e] = tg[e].delay;
            s.shelf[e] = tg[e].shelf;
            s.cur[e] = 0;
            m.BuildFilter(tg[e], e, s.Coef(e, 0));
            std::memcpy(s.fdir[e], tg[e].dir, sizeof(tg[e].dir));
            s.ftri[e] = tg[e].tri;
        }
        s.serial = set.serial;
        s.taps = T;
        s.hasState = true;
    }
    const std::uint32_t oldTaps = s.taps;
    if (swap) {
        s.triCache[0] = s.triCache[1] = -1;
        s.ftri[0] = s.ftri[1] = -1;
        if (T > oldTaps) std::memset(s.xh, 0, (kHistMax - (oldTaps - 1)) * sizeof(float));  // older input not kept
    }
    const std::uint32_t Tf = std::max(T, oldTaps);  // filters are zero beyond their own taps
    const std::uint32_t need = Tf - 1;

    const float prevDir[3] = {s.dir[0], s.dir[1], s.dir[2]};
    const float prevR = s.r, prevGain = s.gain;
    const float dot = prevDir[0] * newDir[0] + prevDir[1] * newDir[1] + prevDir[2] * newDir[2];
    const int oldBuf[2] = {s.cur[0], s.cur[1]};
    int newBuf[2] = {-1, -1};  // swap call: the new set's current filter
    bool crossfaded = false;
    const float invFrames = 1.0f / static_cast<float>(frames);

    for (std::uint32_t o = 0; o < frames; o += kSubBlockFrames) {
        const std::uint32_t L = std::min(kSubBlockFrames, frames - o);
        const float t = static_cast<float>(o + L) * invFrames;
        float dir[3];
        if (dot > -0.999f) {
            float len2 = 0.0f;
            for (int i = 0; i < 3; ++i) {
                dir[i] = prevDir[i] + (newDir[i] - prevDir[i]) * t;
                len2 += dir[i] * dir[i];
            }
            const float inv = 1.0f / std::sqrt(std::max(len2, 1e-12f));
            for (float& c : dir) c *= inv;
        } else {
            std::memcpy(dir, newDir, sizeof(dir));
        }
        const float r = prevR + (newR - prevR) * t;
        m.Targets(s, dir, r, nearOn, tg);

        // FIR input: the kept history and this sub-block.
        std::memcpy(m.xb + kHistMax - need, s.xh + kHistMax - need, need * sizeof(float));
        std::memcpy(m.xb + kHistMax, m.mono + o, L * sizeof(float));
        const float* xe = m.xb + kHistMax - need;
        const float step = 1.0f / static_cast<float>(L);

        // Filters to run, in one pass over the input: z[e] gets each ear's output.
        const float* h[6];
        float* yo[6];
        int count = 0;
        float* z[2] = {m.zb[0] + m.H, m.zb[1] + m.H};
        if (!swap) {
            // Each ear's newest filter over the whole sub-block; the previous one only over the crossfade window at
            // its start (the same click-free linear fade, at a fraction of the cost of running both throughout).
            int nb[2] = {-1, -1};
            for (int e = 0; e < 2; ++e) {
                const float c = tg[e].dir[0] * s.fdir[e][0] + tg[e].dir[1] * s.fdir[e][1] + tg[e].dir[2] * s.fdir[e][2];
                if (c < kFilterUpdateCos) {
                    nb[e] = (s.cur[e] + 1) % kBufs;
                    m.BuildFilter(tg[e], e, s.Coef(e, nb[e]));
                }
                h[count] = s.Coef(e, nb[e] >= 0 ? nb[e] : s.cur[e]), yo[count++] = z[e];
            }
            m.k.firN(h, count, Tf, xe, yo, L);
            const std::uint32_t W = std::min(L, kFilterFadeFrames);
            count = 0;
            for (int e = 0; e < 2; ++e)
                if (nb[e] >= 0) h[count] = s.Coef(e, s.cur[e]), yo[count++] = m.y[e];
            if (count) m.k.firN(h, count, Tf, xe, yo, W);
            const float fstep = 1.0f / static_cast<float>(W);
            for (int e = 0; e < 2; ++e) {
                if (nb[e] < 0) continue;
                float* yOld = m.y[e];
                FadeTo(yOld, z[e], W, fstep, fstep);  // old -> new over the window
                std::memcpy(z[e], yOld, W * sizeof(float));
                s.cur[e] = nb[e];
                std::memcpy(s.fdir[e], tg[e].dir, sizeof(tg[e].dir));
                s.ftri[e] = tg[e].tri;
                crossfaded = true;
            }
        } else {
            // The old set's filter frozen for the whole call, the new set's tracking the direction; full-call fade.
            int next[2] = {-1, -1};
            for (int e = 0; e < 2; ++e) {
                h[count] = s.Coef(e, oldBuf[e]), yo[count++] = z[e];
                if (newBuf[e] < 0) {
                    newBuf[e] = (oldBuf[e] + 1) % kBufs;
                    m.BuildFilter(tg[e], e, s.Coef(e, newBuf[e]));
                    std::memcpy(s.fdir[e], tg[e].dir, sizeof(tg[e].dir));
                    s.ftri[e] = tg[e].tri;
                } else {
                    const float c = tg[e].dir[0] * s.fdir[e][0] + tg[e].dir[1] * s.fdir[e][1] + tg[e].dir[2] * s.fdir[e][2];
                    if (c < kFilterUpdateCos) {
                        next[e] = (newBuf[e] + 1) % kBufs;
                        if (next[e] == oldBuf[e]) next[e] = (next[e] + 1) % kBufs;
                        m.BuildFilter(tg[e], e, s.Coef(e, next[e]));
                    }
                }
                h[count] = s.Coef(e, newBuf[e]), yo[count++] = m.y[2 * e];
                if (next[e] >= 0) h[count] = s.Coef(e, next[e]), yo[count++] = m.y[2 * e + 1];
            }
            if (count <= 4) {
                m.k.firN(h, count, Tf, xe, yo, L);
            } else {
                m.k.firN(h, 4, Tf, xe, yo, L);
                m.k.firN(h + 4, count - 4, Tf, xe, yo + 4, L);
            }
            for (int e = 0; e < 2; ++e) {
                float* yn = m.y[2 * e];
                if (next[e] >= 0) {
                    FadeTo(yn, m.y[2 * e + 1], L, step, step);
                    newBuf[e] = next[e];
                    std::memcpy(s.fdir[e], tg[e].dir, sizeof(tg[e].dir));
                    s.ftri[e] = tg[e].tri;
                }
                FadeTo(z[e], yn, L, static_cast<float>(o + 1) * invFrames, invFrames);
            }
        }
        std::memcpy(s.xh + kHistMax - need, m.xb + kHistMax + L - need, need * sizeof(float));

        // Per ear: delay line, near-field shelf (y + (G - 1) * lowpass(y)), gain and ownership ramps.
        const float gA = prevGain + (newGain - prevGain) * static_cast<float>(o + 1) * invFrames;
        const float wA = w0 + (w1 - w0) * static_cast<float>(o) * invFrames;
        const float gD = (newGain - prevGain) * invFrames, wD = (w1 - w0) * invFrames;
        for (int e = 0; e < 2; ++e) {
            float* zb = m.zb[e];
            float* dh = s.dh + std::size_t(e) * m.H;
            std::memcpy(zb, dh, m.H * sizeof(float));
            float* y0 = m.y[6];
            m.Delay(s, e, tg[e].delay, L, y0);
            std::memcpy(dh, zb + L, m.H * sizeof(float));

            const float g0 = s.shelf[e], gStep = (tg[e].shelf - g0) * step;
            float* out = m.callOut + 2u * o + e;
            if (g0 == 1.0f && tg[e].shelf == 1.0f) {
                // No near-field shelf in this sub-block (far source or near field off): the shelf's low-pass state is
                // not needed; it restarts from 0, where (G - 1) is still 0 when a shelf ramps in.
                for (std::uint32_t i = 0; i < L; ++i) {
                    const float fi = static_cast<float>(i);
                    out[2u * i] = y0[i] * (gA + gD * fi) * (wA + wD * fi);
                }
                s.lp[e] = 0.0f;
            } else {
                float lp = s.lp[e];
                const float lc = m.shelfCoef;
                for (std::uint32_t i = 0; i < L; ++i) {
                    const float v = y0[i];
                    lp += lc * (v - lp);
                    const float G = g0 + gStep * static_cast<float>(i + 1);
                    const float fi = static_cast<float>(i);
                    out[2u * i] = (v + (G - 1.0f) * lp) * (gA + gD * fi) * (wA + wD * fi);
                }
                s.lp[e] = lp;
            }
            s.shelf[e] = tg[e].shelf;
        }
    }
    if (swap) {
        s.cur[0] = newBuf[0];
        s.cur[1] = newBuf[1];
        s.serial = set.serial;
        m.hrtfSwaps.fetch_add(1, std::memory_order_relaxed);
    }
    s.taps = T;
    std::memcpy(s.dir, newDir, sizeof(newDir));
    s.r = newR;
    s.gain = newGain;

    // Output only when everything is finite.
    const float* co = m.callOut;
    const float state[4] = {s.lp[0], s.lp[1], s.delay[0], s.delay[1]};
    if (MaxAbsBits(co, 2u * frames) >= kNonFiniteBits || MaxAbsBits(state, 4) >= kNonFiniteBits ||
        MaxAbsBits(s.dh, 2u * m.H) >= kNonFiniteBits) {
        m.ResetSlot(s);
        m.nonFinite.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    for (std::uint32_t i = 0; i < 2u * frames; ++i) outLR[i] += co[i];
    m.renders.fetch_add(1, std::memory_order_relaxed);
    if (crossfaded) m.filterCrossfades.fetch_add(1, std::memory_order_relaxed);
    return true;
}

Stats Renderer::GetStats() const {
    const Impl& m = *impl_;
    Stats s{};
    s.blocks = m.blocks.load(std::memory_order_relaxed);
    s.renders = m.renders.load(std::memory_order_relaxed);
    s.filterCrossfades = m.filterCrossfades.load(std::memory_order_relaxed);
    s.hrtfSwaps = m.hrtfSwaps.load(std::memory_order_relaxed);
    s.nonFinite = m.nonFinite.load(std::memory_order_relaxed);
    s.rejected = m.rejected.load(std::memory_order_relaxed);
    s.lastBlockNs = m.lastBlockNs.load(std::memory_order_relaxed);
    s.worstBlockNs = m.worstBlockNs.load(std::memory_order_relaxed);
    for (int i = 0; i < 16; ++i) s.blockNsHistogram[i] = m.hist[i].load(std::memory_order_relaxed);
    return s;
}

void Renderer::ResetStats() {
    Impl& m = *impl_;
    for (auto* a : {&m.blocks, &m.renders, &m.filterCrossfades, &m.hrtfSwaps, &m.nonFinite, &m.rejected, &m.lastBlockNs,
                    &m.worstBlockNs})
        a->store(0, std::memory_order_relaxed);
    for (auto& h : m.hist) h.store(0, std::memory_order_relaxed);
}

std::uint32_t Renderer::SampleRate() const { return impl_->cfg.sampleRate; }

struct RendererAccess {
    static const Renderer::Impl& Of(const Renderer& r) { return *r.impl_; }
};

namespace detail {
void ForceKernels(int which) { g_forceKernels.store(which); }
const char* KernelName(const Renderer& r) { return RendererAccess::Of(r).k.name; }
float SourceDelay(const Renderer& r, std::uint32_t slot, int ear) { return RendererAccess::Of(r).slots[slot].delay[ear]; }
void SourceFilterDirection(const Renderer& r, std::uint32_t slot, int ear, float out[3]) {
    std::memcpy(out, RendererAccess::Of(r).slots[slot].fdir[ear], sizeof(float) * 3);
}
}  // namespace detail

}  // namespace binaural
