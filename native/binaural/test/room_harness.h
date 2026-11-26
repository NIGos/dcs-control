// Offline harness of the cockpit space (docs/COCKPIT_SPACE.md 10, 13): a rig with the voices' Renderer and a Room on
// one HRTF set, scene rendering as the audio module does it (direct path with RoomDirectGain, room sends with the
// distance law), test signals, and the metrics of section 10 (IACC, DRR, EDT/T20, K-weighted loudness, colouration).
// Shared by binaural_room_test (C2-C14, R1-R10) and binaural_room_render (the metrics/WAV tool of the listening kit).
#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "binaural_core.h"
#include "binaural_room.h"
#include "cockpit_space.h"
#include "dsp.h"
#include "internal.h"
#include "room_model.h"

namespace room_harness {

using namespace binaural;
using Vec = std::vector<float>;
using cockpit_space::Family;
using cockpit_space::RoomParams;
constexpr double kPi = dsp::kPi;

// ---- signals ----------------------------------------------------------------------------------------------------------
inline Vec White(std::size_t n, unsigned seed, float amp = 0.1f) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> nd(0.0f, amp);
    Vec v(n);
    for (auto& x : v) x = nd(rng);
    return v;
}
// Pink noise (Kellet's filter on white noise), RMS about `amp`.
inline Vec Pink(std::size_t n, unsigned seed, float amp = 0.1f) {
    Vec w = White(n, seed, 1.0f), v(n);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, e = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = w[i];
        b0 = 0.99886 * b0 + x * 0.0555179;
        b1 = 0.99332 * b1 + x * 0.0750759;
        b2 = 0.96900 * b2 + x * 0.1538520;
        b3 = 0.86650 * b3 + x * 0.3104856;
        b4 = 0.55000 * b4 + x * 0.5329522;
        b5 = -0.7616 * b5 - x * 0.0168980;
        const double y = b0 + b1 + b2 + b3 + b4 + b5 + b6 + x * 0.5362;
        b6 = x * 0.115926;
        v[i] = static_cast<float>(y);
        e += y * y;
    }
    const double s = amp / std::sqrt(std::max(e / n, 1e-30));
    for (auto& x : v) x = static_cast<float>(x * s);
    return v;
}
inline Vec Sine(std::size_t n, double f, double fs, float amp = 1.0f) {
    Vec v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<float>(amp * std::sin(2.0 * kPi * f * double(i) / fs));
    return v;
}
// A DCS-like mix: low-heavy engine noise (brown noise low-passed at 300 Hz plus an 80 Hz harmonic series), broadband
// wind (white noise high-passed at 500 Hz) and switch clicks (decaying 2-6 kHz bursts every 0.25 s).
inline Vec DcsLike(std::size_t n, double fs, unsigned seed) {
    Vec w = White(n, seed, 1.0f), w2 = White(n, seed + 1, 1.0f), v(n);
    double brown = 0, lp = 0, hpl = 0;
    const double aLp = 1 - std::exp(-2 * kPi * 300 / fs), aHp = 1 - std::exp(-2 * kPi * 500 / fs);
    for (std::size_t i = 0; i < n; ++i) {
        brown = 0.995 * brown + 0.05 * w[i];
        lp += aLp * (brown - lp);
        double eng = 2.0 * lp;
        for (int h = 1; h <= 6; ++h) eng += 0.08 / h * std::sin(2 * kPi * 80.0 * h * double(i) / fs + h);
        hpl += aHp * (w2[i] - hpl);
        const double wind = 0.03 * (w2[i] - hpl);
        const double tc = std::fmod(double(i) / fs, 0.25);
        const double click = tc < 0.004 ? 0.3 * std::exp(-tc / 0.0008) * std::sin(2 * kPi * 3500.0 * tc) : 0.0;
        v[i] = static_cast<float>(eng + wind + click);
    }
    return v;
}
// Pink-noise bursts (150 ms on, 100 ms off, 5 ms raised-cosine edges) and a "switch click + hum" complex (a 3.5 kHz
// click every 0.3 s over a 400 Hz hum with harmonics) for the listening-test stimuli.
inline Vec PinkBursts(std::size_t n, double fs, unsigned seed) {
    Vec p = Pink(n, seed, 0.1f);
    const double period = 0.25, on = 0.15, edge = 0.005;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = std::fmod(double(i) / fs, period);
        double g = t < on ? 1.0 : 0.0;
        if (t < edge) g = 0.5 - 0.5 * std::cos(kPi * t / edge);
        else if (t < on && t > on - edge) g = 0.5 - 0.5 * std::cos(kPi * (on - t) / edge);
        p[i] = static_cast<float>(p[i] * g);
    }
    return p;
}
inline Vec ClickHum(std::size_t n, double fs) {
    Vec v(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = double(i) / fs;
        double hum = 0.0;
        for (int h = 1; h <= 4; ++h) hum += 0.03 / h * std::sin(2 * kPi * 400.0 * h * t);
        const double tc = std::fmod(t, 0.3);
        const double click = tc < 0.006 ? 0.4 * std::exp(-tc / 0.001) * std::sin(2 * kPi * 3500.0 * tc) : 0.0;
        v[i] = static_cast<float>(hum + click);
    }
    return v;
}

// ---- geometry --------------------------------------------------------------------------------------------------------
// Rotation by `deg` about the y (up) axis, as a quaternion (x, y, z, w); positive turns +x (forward) towards -z (left).
inline void YawQuat(double deg, float q[4]) {
    const double h = deg * kPi / 360.0;
    q[0] = 0, q[1] = static_cast<float>(std::sin(h)), q[2] = 0, q[3] = static_cast<float>(std::cos(h));
}
inline void Rotate(const float q[4], const float v[3], float out[3]) {
    const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]), ty = 2.0f * (q[2] * v[0] - q[0] * v[2]), tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}
inline double AngleDeg(const float a[3], const float b[3]) {
    const double d = (double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2]) /
                     std::sqrt((double(a[0]) * a[0] + double(a[1]) * a[1] + double(a[2]) * a[2]) *
                               (double(b[0]) * b[0] + double(b[1]) * b[1] + double(b[2]) * b[2]));
    return std::acos(std::clamp(d, -1.0, 1.0)) * 180.0 / kPi;
}

// The cockpit sources of section 10 (aircraft frame: x forward, y up, z right).
struct NamedPos {
    const char* name;
    float p[3];
};
inline const std::vector<NamedPos>& CockpitSources() {
    static const std::vector<NamedPos> v = {
        {"above-front 0.45 m", {0.39f, 0.225f, 0.0f}},
        {"left console 0.5 m", {0.15f, -0.25f, -0.41f}},
        {"right console 0.5 m", {0.15f, -0.25f, 0.41f}},
        {"front panel 0.8 m", {0.75f, -0.28f, 0.0f}},
        {"behind 0.35 m", {-0.30f, 0.18f, 0.0f}},
    };
    return v;
}

// ---- rig -------------------------------------------------------------------------------------------------------------
struct RoomRig {
    HrtfSet* set = nullptr;
    Renderer direct;
    Room room;
    std::uint32_t rate = 48000;
    bool ok = false;
    RoomRig(const std::vector<std::uint32_t>& file, std::uint32_t deviceRate = 48000, std::uint32_t maxSources = 64)
        : rate(deviceRate) {
        char err[128];
        set = LoadHrtf(file.data(), file.size() * 4, deviceRate, err, sizeof(err));
        if (!set) {
            std::printf("LoadHrtf failed: %s\n", err);
            return;
        }
        Config c;
        c.sampleRate = deviceRate;
        c.maxSources = maxSources;
        if (!direct.Init(c, err, sizeof(err)) || !room.Init(deviceRate, maxSources, err, sizeof(err))) {
            std::printf("Init failed: %s\n", err);
            return;
        }
        direct.SetHrtf(set);
        room.SetHrtf(set);
        ok = true;
    }
    ~RoomRig() {
        if (set) ReleaseHrtf(set);
    }
};

struct SceneSource {
    float pos[3];          // aircraft frame
    const float* signal;   // at least the scene's frames
    std::uint32_t flags = 0;
    float gain = 1.0f;
    float w0 = 1.0f, w1 = 1.0f;  // this block's HRTF weight ramp (the hook may set it)
    bool active = true;
};
struct SceneState {
    RoomParams params{};
    float q[4] = {0, 0, 0, 1};
    bool frameValid = true, early = true;
    float wet = 1.0f, canopy = 0.0f;
    bool directGain = true;   // multiply the direct path by RoomDirectGain (false: the Off render)
    bool renderDirect = true, renderRoom = true;
    float roomGainShare = -1.0f;  // >= 0: overrides params.neutralShare for the direct gain only
    // The module's way (Room::Render with the direct sound): the direct path goes to the room, which adds the
    // level-matched direct + room; SceneOut::room then holds the whole output, SceneOut::direct stays zero.
    bool levelMatch = false;
};
struct SceneOut {
    Vec direct, room;
    Vec Total() const {
        Vec t(direct.size());
        for (std::size_t i = 0; i < t.size(); ++i) t[i] = direct[i] + (i < room.size() ? room[i] : 0.0f);
        return t;
    }
};
using BlockHook = std::function<void(std::uint32_t block, std::uint32_t offset, SceneState&, std::vector<SceneSource>&)>;
// Renders `frames` frames in blocks (fixed `block`, or the sizes in `sizes` cycled). The room gets SetParams(st.params)
// before the first block; the hook may change anything (call rig.room.SetParams itself for a live change).
inline SceneOut RenderScene(RoomRig& rig, SceneState st, std::vector<SceneSource> src, std::uint32_t frames, std::uint32_t block,
                            const BlockHook& hook = {}, const std::vector<std::uint32_t>* sizes = nullptr) {
    SceneOut out;
    out.direct.assign(std::size_t(frames) * 2, 0.0f);
    out.room.assign(std::size_t(frames) * 2, 0.0f);
    rig.room.SetParams(st.params);
    std::vector<bool> started(src.size(), false);
    std::vector<float> blockDirect;
    std::uint32_t o = 0, b = 0;
    while (o < frames) {
        std::uint32_t n = sizes ? (*sizes)[b % sizes->size()] : block;
        n = std::min(n, frames - o);
        if (hook) hook(b, o, st, src);
        RoomFrame f{};
        std::memcpy(f.aircraftToHead, st.q, sizeof(st.q));
        f.frameValid = st.frameValid, f.wet = st.wet, f.canopyOpen = st.canopy, f.early = st.early;
        if (st.renderRoom) rig.room.BeginBlock(n, f);
        rig.direct.BeginBlock(n);
        const bool match = st.levelMatch && st.renderRoom && st.renderDirect;
        if (match) blockDirect.assign(std::size_t(n) * 2, 0.0f);
        float* dOut = match ? blockDirect.data() : out.direct.data() + 2 * std::size_t(o);
        float dLast = 1.0f;
        for (std::size_t i = 0; i < src.size(); ++i) {
            SceneSource& s = src[i];
            if (!s.active) {
                if (started[i]) rig.direct.ResetSource(static_cast<std::uint32_t>(i)), rig.room.ResetSource(static_cast<std::uint32_t>(i));
                started[i] = false;
                continue;
            }
            const std::uint32_t reset = started[i] ? 0u : 1u;
            MonoInput mi{s.signal + o, nullptr, 1, 1.0f};
            if (st.renderDirect) {
                float hp[3];
                Rotate(st.q, s.pos, hp);
                RoomParams pd = st.params;
                if (st.roomGainShare >= 0.0f) pd.neutralShare = st.roomGainShare;
                const float d = st.directGain ? RoomDirectGain(pd, st.wet, st.canopy, st.early && st.frameValid, s.pos, s.flags) : 1.0f;
                dLast = d;
                SourceUpdate u{{hp[0], hp[1], hp[2]}, s.gain * d, reset ? source_flags::kReset : 0u};
                rig.direct.Render(static_cast<std::uint32_t>(i), u, mi, n, s.w0, s.w1, dOut);
            }
            if (st.renderRoom) {
                RoomSource rs{{s.pos[0], s.pos[1], s.pos[2]}, s.gain * RoomSendDistanceGain(st.params, s.pos, s.flags),
                              s.flags | (reset ? room_flags::kReset : 0u), dLast};
                rig.room.AddSource(static_cast<std::uint32_t>(i), rs, mi, n, s.w0, s.w1);
            }
            started[i] = true;
            s.w0 = s.w1;
        }
        rig.direct.EndBlock();
        if (st.renderRoom) {
            if (match) rig.room.Render(out.room.data() + 2 * std::size_t(o), n, blockDirect.data());
            else rig.room.Render(out.room.data() + 2 * std::size_t(o), n);
            rig.room.EndBlock();
        }
        o += n, ++b;
    }
    return out;
}

// Impulse responses of one source: after `warmS` of silence (the wet and early fades settle), a unit impulse; the
// outputs are cut to start at the impulse. `off`: the direct path alone without the direct gain (the Off render).
struct Irs {
    Vec off, direct, room;  // interleaved stereo
};
inline Irs ImpulseResponses(RoomRig& rig, const RoomParams& p, const float pos[3], std::uint32_t flags, const float q[4],
                            double lengthS = 0.6, double warmS = 0.6, std::uint32_t block = 480, float canopy = 0.0f) {
    const std::uint32_t warm = static_cast<std::uint32_t>(warmS * rig.rate), len = static_cast<std::uint32_t>(lengthS * rig.rate);
    Vec sig(std::size_t(warm) + len, 0.0f);
    sig[warm] = 1.0f;
    SceneState st;
    st.params = p;
    std::memcpy(st.q, q, sizeof(st.q));
    st.canopy = canopy;
    std::vector<SceneSource> src(1);
    std::memcpy(src[0].pos, pos, sizeof(src[0].pos));
    src[0].signal = sig.data(), src[0].flags = flags;
    SceneOut a = RenderScene(rig, st, src, warm + len, block);
    st.directGain = false, st.renderRoom = false;
    SceneOut b = RenderScene(rig, st, src, warm + len, block);
    Irs r;
    r.direct.assign(a.direct.begin() + 2 * std::size_t(warm), a.direct.end());
    r.room.assign(a.room.begin() + 2 * std::size_t(warm), a.room.end());
    r.off.assign(b.direct.begin() + 2 * std::size_t(warm), b.direct.end());
    return r;
}

// ---- metrics ---------------------------------------------------------------------------------------------------------
inline Vec Channel(const Vec& lr, int ear) {
    Vec v(lr.size() / 2);
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = lr[2 * i + ear];
    return v;
}
inline Vec Add(const Vec& a, const Vec& b) {
    Vec v(std::max(a.size(), b.size()), 0.0f);
    for (std::size_t i = 0; i < a.size(); ++i) v[i] += a[i];
    for (std::size_t i = 0; i < b.size(); ++i) v[i] += b[i];
    return v;
}
inline double Energy(const Vec& x, std::size_t from = 0, std::size_t to = ~std::size_t(0)) {
    double e = 0;
    for (std::size_t i = from; i < std::min(to, x.size()); ++i) e += double(x[i]) * x[i];
    return e;
}
inline double Db(double e) { return 10.0 * std::log10(std::max(e, 1e-30)); }

// Zero-phase band-pass by FFT with raised-cosine edges (1/6 octave wide) around [lo, hi).
inline Vec BandPass(const Vec& x, double fs, double lo, double hi) {
    const std::size_t n = dsp::NextPow2(x.size() * 2);
    std::vector<std::complex<double>> a(n);
    for (std::size_t i = 0; i < x.size(); ++i) a[i] = x[i];
    dsp::Fft(a, false);
    const double e = std::pow(2.0, 1.0 / 12.0);
    for (std::size_t k = 0; k <= n / 2; ++k) {
        const double f = double(k) * fs / double(n);
        double g;
        if (f < lo / e || f > hi * e) g = 0.0;
        else if (f < lo * e) g = 0.5 - 0.5 * std::cos(kPi * std::log(f / (lo / e)) / std::log(e * e));
        else if (f > hi / e) g = 0.5 - 0.5 * std::cos(kPi * std::log((hi * e) / f) / std::log(e * e));
        else g = 1.0;
        a[k] *= g;
        if (k > 0 && k < n / 2) a[n - k] *= g;
    }
    dsp::Fft(a, true);
    Vec y(x.size());
    for (std::size_t i = 0; i < y.size(); ++i) y[i] = static_cast<float>(a[i].real());
    return y;
}
inline Vec Octave(const Vec& x, double fs, double centre) { return BandPass(x, fs, centre / std::sqrt(2.0), centre * std::sqrt(2.0)); }

// Interaural cross-correlation: max |normalised xcorr| over +-maxLag samples in [from, to).
inline double Iacc(const Vec& l, const Vec& r, std::size_t from, std::size_t to, int maxLag) {
    to = std::min({to, l.size(), r.size()});
    double el = 0, er = 0;
    for (std::size_t i = from; i < to; ++i) el += double(l[i]) * l[i], er += double(r[i]) * r[i];
    if (el <= 0 || er <= 0) return 0.0;
    double best = 0;
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        double c = 0;
        for (std::size_t i = from; i < to; ++i) {
            const long long j = static_cast<long long>(i) + lag;
            if (j < static_cast<long long>(from) || j >= static_cast<long long>(to)) continue;
            c += double(l[i]) * r[static_cast<std::size_t>(j)];
        }
        best = std::max(best, std::fabs(c));
    }
    return best / std::sqrt(el * er);
}

// Schroeder backward integration (both ears' energy summed) and the decay times from linear fits: EDT (0 to -10 dB),
// T20 (-5 to -25 dB); seconds, from the curve's start at sample `from`. -1 when the range is not reached.
struct Decay {
    double edt, t20;
};
inline Decay DecayTimes(const Vec& l, const Vec& r, std::size_t from, double fs) {
    const std::size_t n = std::min(l.size(), r.size());
    std::vector<double> e(n + 1, 0.0);
    for (std::size_t i = n; i-- > from;) e[i] = e[i + 1] + double(l[i]) * l[i] + double(r[i]) * r[i];
    const double e0 = e[from];
    auto fit = [&](double hiDb, double loDb) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int c = 0;
        for (std::size_t i = from; i < n; ++i) {
            const double db = 10.0 * std::log10(std::max(e[i] / e0, 1e-30));
            if (db > hiDb) continue;
            if (db < loDb) break;
            const double t = double(i - from) / fs;
            sx += t, sy += db, sxx += t * t, sxy += t * db, ++c;
        }
        if (c < 8) return -1.0;
        const double slope = (c * sxy - sx * sy) / (c * sxx - sx * sx);
        return slope < 0 ? -60.0 / slope : -1.0;
    };
    return {fit(0.0, -10.0), fit(-5.0, -25.0)};
}
inline std::size_t PeakIndex(const Vec& l, const Vec& r) {
    std::size_t k = 0;
    double m = -1;
    for (std::size_t i = 0; i < std::min(l.size(), r.size()); ++i) {
        const double v = double(l[i]) * l[i] + double(r[i]) * r[i];
        if (v > m) m = v, k = i;
    }
    return k;
}
// Onset: the first sample within -40 dB of the peak (both ears).
inline std::size_t Onset(const Vec& l, const Vec& r) {
    const std::size_t pk = PeakIndex(l, r);
    const double m = double(l[pk]) * l[pk] + double(r[pk]) * r[pk];
    for (std::size_t i = 0; i < pk; ++i)
        if (double(l[i]) * l[i] + double(r[i]) * r[i] > m * 1e-4) return i;
    return pk;
}

// ITU-R BS.1770 K-weighted loudness (LKFS) of a stereo signal at 48 kHz, both ears' power summed.
inline double Loudness(const Vec& lr, std::size_t from = 0) {
    const double b1[3] = {1.53512485958697, -2.69169618940638, 1.19839281085285}, a1[3] = {1, -1.69065929318241, 0.73248077421585};
    const double b2[3] = {1, -2, 1}, a2[3] = {1, -1.99004745483398, 0.99007225036621};
    double sum = 0;
    std::size_t count = 0;
    for (int ch = 0; ch < 2; ++ch) {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0, u1 = 0, u2 = 0, v1 = 0, v2 = 0;
        for (std::size_t i = 0; i < lr.size() / 2; ++i) {
            const double x = lr[2 * i + ch];
            const double y = b1[0] * x + b1[1] * x1 + b1[2] * x2 - a1[1] * y1 - a1[2] * y2;
            x2 = x1, x1 = x, y2 = y1, y1 = y;
            const double v = b2[0] * y + b2[1] * u1 + b2[2] * u2 - a2[1] * v1 - a2[2] * v2;
            u2 = u1, u1 = y, v2 = v1, v1 = v;
            if (i >= from) sum += v * v, ++count;
        }
    }
    return -0.691 + 10.0 * std::log10(std::max(sum / std::max<std::size_t>(count / 2, 1), 1e-30));
}

// Third-octave band energies (both ears) of an IR, centres 200 Hz .. 12.5 kHz.
inline std::vector<double> ThirdOctaves(const Vec& lr, double fs, std::vector<double>* centres = nullptr) {
    const Vec l = Channel(lr, 0), r = Channel(lr, 1);
    const std::size_t n = std::max<std::size_t>(dsp::NextPow2(l.size()), 4096);
    const auto ml = dsp::Magnitude(l.data(), l.size(), n), mr = dsp::Magnitude(r.data(), r.size(), n);
    std::vector<double> out;
    for (int k = -7; k <= 11; ++k) {
        const double c = 1000.0 * std::pow(2.0, k / 3.0);
        if (c < 190 || c > 13000) continue;
        if (centres) centres->push_back(c);
        const double lo = c / std::pow(2.0, 1.0 / 6.0), hi = c * std::pow(2.0, 1.0 / 6.0);
        out.push_back(dsp::BandEnergy(ml, n, fs, lo, hi) + dsp::BandEnergy(mr, n, fs, lo, hi));
    }
    return out;
}
// Colouration (R9): the largest |dB difference| between the third-octave spectra of a and b after removing the mean.
inline double RippleDb(const Vec& a, const Vec& b, double fs) {
    const auto ea = ThirdOctaves(a, fs), eb = ThirdOctaves(b, fs);
    std::vector<double> d(ea.size());
    double mean = 0;
    for (std::size_t i = 0; i < d.size(); ++i) d[i] = Db(ea[i]) - Db(eb[i]), mean += d[i];
    mean /= d.size();
    double worst = 0;
    for (double x : d) worst = std::max(worst, std::fabs(x - mean));
    return worst;
}

// max |second difference| per sample (as the core harness's A10): a step of e adds e.
inline double MaxD2(const Vec& x, std::size_t from, std::size_t to) {
    double m = 0.0;
    for (std::size_t i = std::max<std::size_t>(from, 2); i < to && i < x.size(); ++i)
        m = std::max(m, double(std::fabs(x[i] - 2 * x[i - 1] + x[i - 2])));
    return m;
}

// 16-bit PCM stereo WAV (plays with Windows' SoundPlayer). Returns the number of clipped samples.
inline std::size_t WriteWav16(const std::string& path, const Vec& lr, std::uint32_t rate, float gain = 1.0f) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return ~std::size_t(0);
    const std::uint32_t data = static_cast<std::uint32_t>(lr.size() * 2), riff = 36 + data;
    const std::uint16_t fmt = 1, ch = 2, bits = 16, align = 4;
    const std::uint32_t fmtLen = 16, bps = rate * 4;
    std::fwrite("RIFF", 1, 4, f), std::fwrite(&riff, 4, 1, f), std::fwrite("WAVEfmt ", 1, 8, f), std::fwrite(&fmtLen, 4, 1, f);
    std::fwrite(&fmt, 2, 1, f), std::fwrite(&ch, 2, 1, f), std::fwrite(&rate, 4, 1, f), std::fwrite(&bps, 4, 1, f);
    std::fwrite(&align, 2, 1, f), std::fwrite(&bits, 2, 1, f), std::fwrite("data", 1, 4, f), std::fwrite(&data, 4, 1, f);
    std::size_t clipped = 0;
    std::vector<std::int16_t> pcm(lr.size());
    for (std::size_t i = 0; i < lr.size(); ++i) {
        double v = double(lr[i]) * gain * 32767.0;
        if (v > 32767.0) v = 32767.0, ++clipped;
        if (v < -32768.0) v = -32768.0, ++clipped;
        pcm[i] = static_cast<std::int16_t>(std::lround(v));
    }
    std::fwrite(pcm.data(), 2, pcm.size(), f);
    std::fclose(f);
    return clipped;
}

// ---- the metrics of section 10 for one source (R1, R2, R3, R9) -------------------------------------------------------
struct SourceMetrics {
    double iaccEarlyOff[4], iaccEarly[4], iaccLate[4];  // 500, 1k, 2k, 4k
    double drr;                                         // dB, 1-4 kHz, Off direct vs room
    double edt[4], t20[4];                              // room IR
    double ripple;                                      // R9, direct+early vs direct
};
constexpr double kMetricOctaves[4] = {500, 1000, 2000, 4000};
inline SourceMetrics MeasureSource(RoomRig& rig, const RoomParams& p, const float pos[3], const float q[4]) {
    SourceMetrics m{};
    const double fs = rig.rate;
    const Irs ir = ImpulseResponses(rig, p, pos, 0, q);
    const Vec full = Add(ir.direct, ir.room);
    const Vec ol = Channel(ir.off, 0), orr = Channel(ir.off, 1), fl = Channel(full, 0), fr = Channel(full, 1);
    const Vec rl = Channel(ir.room, 0), rr = Channel(ir.room, 1);
    const std::size_t on = Onset(ol, orr), e5 = on + static_cast<std::size_t>(0.005 * fs);
    const int lag = static_cast<int>(0.001 * fs);
    for (int b = 0; b < 4; ++b) {
        const double c = kMetricOctaves[b];
        const Vec bol = Octave(ol, fs, c), bor = Octave(orr, fs, c), bfl = Octave(fl, fs, c), bfr = Octave(fr, fs, c);
        const std::size_t pre = static_cast<std::size_t>(0.001 * fs);
        const std::size_t a = on > pre ? on - pre : 0;
        m.iaccEarlyOff[b] = Iacc(bol, bor, a, e5, lag);
        m.iaccEarly[b] = Iacc(bfl, bfr, a, e5, lag);
        m.iaccLate[b] = Iacc(bfl, bfr, e5, bfl.size(), lag);
        const Vec brl = Octave(rl, fs, c), brr = Octave(rr, fs, c);
        const Decay d = DecayTimes(brl, brr, PeakIndex(brl, brr), fs);
        m.edt[b] = d.edt, m.t20[b] = d.t20;
    }
    {
        const Vec dl = BandPass(ol, fs, 1000, 4000), dr = BandPass(orr, fs, 1000, 4000);
        const Vec wl = BandPass(rl, fs, 1000, 4000), wr = BandPass(rr, fs, 1000, 4000);
        m.drr = Db(Energy(dl) + Energy(dr)) - Db(Energy(wl) + Energy(wr));
    }
    {
        // Direct + early: the room rendered with the tail at -60 dB.
        RoomParams pe = p;
        pe.lateDb = -40.0f;
        const Irs e = ImpulseResponses(rig, pe, pos, 0, q, 0.1);
        const Vec de = Add(e.direct, e.room);
        m.ripple = RippleDb(de, e.off, fs);
    }
    return m;
}

}  // namespace room_harness
