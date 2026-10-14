// DCS Control: shared perception helpers for the bundled Quad Views compositor (copied into Quad-Views-Foveated by
// scripts/build-quadviews.ps1; tested by tests/perception).
//
// One header for C++ and HLSL. The first part (eccentricity, the periphery stability weight and blend) compiles in
// both, so the offline tests check the very maths the shader runs. The second part (C++ only) holds what the layer
// keeps per frame: the gaze and saccade state of a display time, shared by both eyes, and the periphery stability
// controller (resets, reprojection, statistics).
//
// Periphery stability (docs/PERCEPTION.md): the periphery reaches the headset enlarged several times, so DCS's
// residual shimmer on thin high-contrast lines (canopy frame, power lines, horizon) pumps over many output pixels.
// A temporal low-pass on the periphery at its own resolution steadies it: the previous result, reprojected by the
// head's rotation, clipped to the current 3x3 neighbourhood (against ghosting), blended with a time constant that
// does not depend on the frame rate, zero inside the focus area and ramping up outside it.
#ifndef DCSVR_PERCEPTION_H
#define DCSVR_PERCEPTION_H

#ifdef __cplusplus
#if defined(min) || defined(max)
#error "dcsvr_perception.h needs NOMINMAX before windows.h"
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
namespace dcsvr {
#define DCSVR_FN inline
using std::acos;
using std::exp;
using std::max;
using std::abs;
using std::min;
using std::pow;
using std::sqrt;
#else
#define DCSVR_FN
#endif

DCSVR_FN float DcsvrSaturate(float x) {
    return min(max(x, 0.0f), 1.0f);
}

// As HLSL's smoothstep, written out so C++ and HLSL give the same value.
DCSVR_FN float DcsvrSmoothstep(float edge0, float edge1, float x) {
    const float t = DcsvrSaturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

// Angle in degrees between the ray through the tangent-plane point (tx, ty) of a view (looking down -Z, +Y up; tx and
// ty are tan of the angles, as XrFovf's) and the unit direction (dx, dy, dz) in the same view.
DCSVR_FN float DcsvrEccentricityDeg(float tx, float ty, float dx, float dy, float dz) {
    const float c = (tx * dx + ty * dy - dz) / sqrt(tx * tx + ty * ty + 1.0f);
    return acos(min(max(c, -1.0f), 1.0f)) * 57.2957795f;
}

// Angle in degrees from the focus area's edge to the periphery ray (tx, ty): negative inside, positive outside. The
// edge is the one the composition draws (ProjectionPS): the superellipse of exponent shape inscribed in the focus
// view's field (tan left, right, up, down), or that field's rectangle for shape < 1. radiusXDeg and radiusYDeg are
// the focus view's angular half-widths; along a direction the centre-to-edge angle interpolates between them, so an
// elongated focus view ramps up from its real edge on both axes.
DCSVR_FN float DcsvrFocusEdgeDeg(float tx, float ty, float focusLeft, float focusRight, float focusUp, float focusDown,
                                 float radiusXDeg, float radiusYDeg, float shape) {
    const float x = (2.0f * tx - (focusLeft + focusRight)) / max(focusRight - focusLeft, 1e-6f);
    const float y = (2.0f * ty - (focusUp + focusDown)) / max(focusUp - focusDown, 1e-6f);
    const float ax = abs(x), ay = abs(y);
    const float r = shape >= 1.0f ? pow(pow(ax, shape) + pow(ay, shape), 1.0f / shape) : max(ax, ay);
    if (!(r > 1e-6f)) return -min(radiusXDeg, radiusYDeg);
    const float along = sqrt(x * x * radiusXDeg * radiusXDeg + y * y * radiusYDeg * radiusYDeg) / r;
    return (r - 1.0f) * along;
}

// Weight of the periphery stability at a periphery pixel, 0..1. focusDeg: angle from the focus area's centre, zero up
// to the focus radius (where the sharp focus view covers the periphery or fades into it, so no lag shows at its edge)
// and ramping up over rampDeg; eccentricityDeg: angle from the gaze, fading out from farStartDeg to farEndDeg where
// flicker at the rendered frame rate is fused anyway (FarFalloff below).
DCSVR_FN float DcsvrStabilityWeight(float focusDeg, float focusRadiusDeg, float rampDeg, float eccentricityDeg,
                                    float farStartDeg, float farEndDeg) {
    const float inner = DcsvrSmoothstep(focusRadiusDeg, focusRadiusDeg + max(rampDeg, 0.01f), focusDeg);
    const float outer = 1.0f - DcsvrSmoothstep(farStartDeg, max(farEndDeg, farStartDeg + 0.01f), eccentricityDeg);
    return inner * outer;
}

// Share of the history in the blend for a frame interval dtSeconds: a first-order low-pass with the time constant
// tauSeconds x weight. Being a function of time, not of frames, it keeps the same cut-off in 2X (45 FPS), 3X (30 FPS)
// and Auto: after T seconds a change has the same weight exp(-T / tau) at any frame rate.
DCSVR_FN float DcsvrHistoryBlend(float dtSeconds, float tauSeconds, float weight) {
    const float tau = tauSeconds * weight;
    return tau > 0.00001f ? exp(-dtSeconds / tau) : 0.0f;
}

// How much of the history to keep after the neighbourhood clamp moved it by clipDistance (luma) in a neighbourhood
// of standard deviation sigma: 1 when it was inside the box, 0 from k standard deviations outside. A history that had
// to be clamped far belongs to content that moved without the head (no motion vectors here); the filter backs off.
DCSVR_FN float DcsvrClipConfidence(float clipDistance, float sigma, float k) {
    return 1.0f - DcsvrSmoothstep(0.0f, max(k, 0.001f), clipDistance / (sigma + 0.002f));
}

#ifdef __cplusplus

// ---- C++ only ------------------------------------------------------------------------------------------------------

// Krajancich, Kellnhofer, Wetzstein 2021 (ACM TOG 40(4) 47): critical flicker fusion frequency in Hz at eccentricity
// e (degrees) for spatial frequency fs (cycles per degree), full fit, 380 cd/m2.
inline float CriticalFlickerHz(float e, float fs) {
    static constexpr float p[10] = {-4.06f, -10.1f, 94.3f, 0.0440f, -0.281f, 0.435f, -0.00111f, 0.00613f, -0.00877f, 1.58f};
    const float t = std::max(std::log10(std::max(fs, 1e-6f)) - std::log10(0.0055f), 0.f);
    const float z = std::exp(p[9] * t) - 1.f;
    const float v = p[0] * t * t + p[1] * t + p[2] + (p[3] * t * t + p[4] * t + p[5]) * z * e +
                    (p[6] * t * t + p[7] * t + p[8]) * z * e * e;
    return std::max(0.f, v);
}

// The same at luminance L (cd/m2), with the paper's retinal-illuminance scaling (pupil after Watson and Yellott).
inline float CriticalFlickerHz(float e, float fs, float luminance) {
    static constexpr float q[3] = {5.71e-6f, -1.78e-4f, 0.204f};
    const float t = std::max(std::log10(std::max(fs, 1e-6f)) - std::log10(0.0055f), 0.f);
    const float z = std::exp(1.58f * t) - 1.f;
    const float s = z * (q[0] * e * e + q[1] * e) + q[2];
    const float f = std::pow(std::max(luminance, 0.001f) * 3600.f / 846.f, 0.41f);
    const float pupil = 7.75f - 5.75f * (f / (f + 2.f));
    const float troland = 3.14159265f * pupil * pupil / 4.f * luminance;
    return std::max(0.f, (s * std::log10(troland / 1488.f) + 1.f) * CriticalFlickerHz(e, fs));
}

// Geisler and Perry 1998: the highest visible spatial frequency (cycles per degree) at eccentricity e.
inline float AcuityCpd(float e) {
    return 4.15888308f * 2.3f / (0.106f * (std::max(e, 0.f) + 2.3f));
}

struct FarFalloff {
    float startDeg{180.f}, endDeg{180.f};
};

// Where shimmer at the rendered rate is fused anyway: the eccentricities at which the flicker fusion limit of
// shimmer-band content (shimmerCpd, or less where acuity ends) falls to 10 Hz above, and then to, the rendered
// frame rate's Nyquist frequency. Beyond them the stability fades out (DcsvrStabilityWeight). Never before 20 degrees.
inline FarFalloff ComputeFarFalloff(float renderedHz, float shimmerCpd = 1.5f, float luminance = 100.f) {
    FarFalloff out{};
    const float nyquist = renderedHz * 0.5f;
    bool started = false;
    for (float e = 20.f; e <= 90.f; e += 0.5f) {
        const float cff = CriticalFlickerHz(e, std::min(shimmerCpd, AcuityCpd(e)), luminance);
        if (!started && cff < nyquist + 10.f) {
            out.startDeg = e;
            started = true;
        }
        if (cff < nyquist) {
            out.endDeg = e;
            if (!started) out.startDeg = e;
            return out;
        }
    }
    if (started) out.endDeg = std::max(out.startDeg + 10.f, 90.f);
    return out;
}

struct Quat {
    float x{0.f}, y{0.f}, z{0.f}, w{1.f};
};

inline Quat Conjugate(const Quat& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

inline Quat Multiply(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline void Rotate(const Quat& q, const float v[3], float out[3]) {
    // v + 2 w (u x v) + 2 u x (u x v)
    const float u[3] = {q.x, q.y, q.z};
    const float c[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    const float cc[3] = {u[1] * c[2] - u[2] * c[1], u[2] * c[0] - u[0] * c[2], u[0] * c[1] - u[1] * c[0]};
    for (int i = 0; i < 3; ++i) out[i] = v[i] + 2.f * (q.w * c[i] + cc[i]);
}

// Rotation angle of a unit quaternion, degrees.
inline float AngleDeg(const Quat& q) {
    return 2.f * std::acos(std::min(std::fabs(q.w), 1.f)) * 57.2957795f;
}

// The view's tangents (XrFovf angles through tan): left and down negative.
struct TanFov {
    float left{-1.f}, right{1.f}, up{1.f}, down{-1.f};
};

// Gaze and saccade state of one display time, the same for both eyes: the cyclopean gaze in view space (unit,
// looking down -Z), whether it was valid and whether the eyes were in a saccade (dcsvr_gaze.h's detector).
// located: recorded by the layer for a located frame (a default sample stands for "nothing recorded"). eye: the same
// gaze in each eye's own view frame (SetEyes), for DcsVr_QuadViewsGaze.
struct GazeSample {
    int64_t time{0};
    bool valid{false};
    bool inSaccade{false};
    float dir[3]{0.f, 0.f, -1.f};
    bool located{false};
    float eye[2][3]{{0.f, 0.f, -1.f}, {0.f, 0.f, -1.f}};

    // Fills eye[] from dir and each eye's orientation in view space (canted displays turn each eye's frame).
    void SetEyes(const Quat eyeInView[2]) {
        for (int e = 0; e < 2; ++e) {
            float v[3];
            Rotate(Conjugate(eyeInView[e]), dir, v);
            const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            for (int i = 0; i < 3; ++i) eye[e][i] = length > 0.f ? v[i] / length : (i == 2 ? -1.f : 0.f);
        }
    }
};

// Written when views are located, read when the frame is submitted (another thread, later): a few frames of samples.
class GazeHistory {
  public:
    void Record(const GazeSample& sample) {
        std::lock_guard lock(m_mutex);
        for (auto& s : m_samples)
            if (s.time == sample.time) {
                s = sample;
                s.located = true;
                return;
            }
        m_samples[m_next] = sample;
        m_samples[m_next].located = true;
        m_next = (m_next + 1) % Count;
    }
    // The sample of this display time, else the latest one before it (DCS may submit a time it located a little
    // differently); none (invalid, no saccade) when there is nothing within 100 ms.
    GazeSample At(int64_t time) const {
        std::lock_guard lock(m_mutex);
        const GazeSample* best = nullptr;
        for (const auto& s : m_samples)
            if (s.time != 0 && s.time <= time && time - s.time < 100'000'000 && (!best || s.time > best->time)) best = &s;
        return best ? *best : GazeSample{time};
    }

  private:
    static constexpr int Count = 16;
    mutable std::mutex m_mutex;
    GazeSample m_samples[Count]{};
    int m_next{0};
};

// ---- Gaze export ---------------------------------------------------------------------------------------------------
// DcsVr_QuadViewsGaze(displayTime, left[3], right[3]), exported by the bundled Quad Views (scripts/build-quadviews.ps1)
// for DCS Control's pupil shift layer, which sits above it and calls it from its own xrLocateViews, after Quad Views
// has located that display time. Without a gaze Quad Views places the focus views at a fixed spot (the resting gaze
// plus a per-eye horizontal offset), which a reader of the focus views would take for a real gaze; this says which it
// is. Returns GazeExport flags; left and right receive the gaze as a unit vector in each eye's view frame (-Z forward).
// Quad Views has one combined gaze, so both are the same direction, each expressed in its own (canted) eye's frame.
// Without a valid gaze they hold straight ahead (0, 0, -1): only the flags decide.
enum GazeExport : uint32_t {
    GazeExportSample = 1u,   // a sample of this display time, or the latest one within 100 ms before it
    GazeExportValid = 2u,    // a real eye-tracker gaze (Quad Views' own short blink cache included), not the fixed focus
    GazeExportSaccade = 4u,  // the eyes were in a saccade (dcsvr_gaze.h's detector)
};

// The periphery stability's state for the in-headset panel (DcsVr_QuadViewsPeripheryStatus): bit 0 the pass runs
// (enabled, strength above 0, an opaque layer, not the focus debug view), bit 1 the last frame was filtered, bit 2 the
// status is valid (written at least once), bits 8-15 the strength x 100 (time constant / 80 ms), bits 16-31 frames
// filtered and bits 32-47 resets in the last full second of display time, bits 48-55 a counter of updates (the panel
// sees a status that stopped changing, e.g. loading screens without a projection layer). 0 until the first frame:
// "starting", not "off".
inline std::atomic<uint64_t>& PeripheryStatus() {
    static std::atomic<uint64_t> status{0};
    return status;
}

// One history for the layer and the export (the layer is a single instance per process).
inline GazeHistory& SharedGazeHistory() {
    static GazeHistory history;
    return history;
}

inline uint32_t ExportGaze(const GazeSample& sample, float left[3], float right[3]) {
    for (int i = 0; i < 3; ++i) {
        if (left) left[i] = sample.eye[0][i];
        if (right) right[i] = sample.eye[1][i];
    }
    if (!sample.located) return 0u;
    return GazeExportSample | (sample.valid ? GazeExportValid : 0u) | (sample.valid && sample.inSaccade ? GazeExportSaccade : 0u);
}

struct TemporalSettings {
    bool enabled{false};
    float tauMs{40.f};      // history time constant at full weight; 0: off
    float rampDeg{8.f};     // from the focus edge to full weight
    float clip{1.f};        // neighbourhood box half-width, in standard deviations
    float confidence{1.f};  // history clamped this many standard deviations is dropped
    bool debug{false};      // shows the weight as a red tint instead of filtering
    float maxGapMs{100.f};  // a longer interval between submitted frames starts over
    float maxTurnDeg{10.f}; // so does a larger head turn between two frames
    float focusShape{0.f};  // the composition's focus_view_shape (its focus edge; < 1: the rectangle)
};

enum class TemporalReset { None, First, Gap, Saccade, Turn, Size, Space, Inactive, Scene };

// Scene cuts the head pose does not show (DCS switching to another view or camera, a respawn): another module in the
// process (DCS Control's engine module) calls the layer's DcsVr_QuadViewsTemporalReset export, which counts here; the
// next frame starts over instead of clamping the old scene's history into the new one for a few frames.
inline std::atomic<uint32_t>& SceneCuts() {
    static std::atomic<uint32_t> cuts{0};
    return cuts;
}

// One eye of a submitted frame: the eye's orientation in the layer's space as DCS rendered it, the periphery image's
// field and size, and the eye's orientation in view space (to bring the cyclopean gaze into the eye's frame).
struct TemporalEye {
    Quat pose{};
    Quat eyeInView{};
    TanFov fov{};
    uint32_t width{0}, height{0};
};

// The constant buffer of dcsvr_temporal_cs.hlsl, in its order (rows of four floats).
struct TemporalConstants {
    float reproject[3][4]; // rows: a ray of this frame's eye -> the same direction in the previous frame's eye
    float fov[4];          // tan left, right, up, down of this frame's periphery
    float prevFov[4];      // the same for the history
    float gaze[4];         // xyz unit gaze in this eye's frame, w 1 when valid
    float focus[4];        // xyz unit centre of the focus area, w its radius in degrees
    float params[4];       // x frame interval (s), y time constant (s; 0: no history this frame), z ramp (deg), w clip
    float params2[4];      // x far start (deg), y far end (deg), z confidence (standard deviations), w debug
    float size[4];         // width, height, 1 / width, 1 / height
    float focusTan[4];     // tan left, right, up, down of the focus view (its edge: DcsvrFocusEdgeDeg)
    float focusShape[4];   // x, y the focus view's angular half-widths (deg), z focus_view_shape, w unused
};

struct TemporalStats {
    uint64_t frames{0}, filtered{0}, resets{0}, saccade{0}, gap{0}, turn{0}, size{0}, other{0};
    double dtSum{0.0};
    FarFalloff falloff{};
};

// Once per submitted frame (BeginFrame, the same decision for both eyes: a reset in one eye resets both, so one eye
// is never filtered while the other is not), then per eye (Constants).
class TemporalController {
  public:
    TemporalReset BeginFrame(int64_t displayTime, const TemporalEye eyes[2], uint64_t space, const GazeSample& gaze,
                             bool active, const TemporalSettings& s) {
        if (displayTime == m_time && m_valid) return m_reset;  // a second projection layer of the same frame
        TemporalReset reset = TemporalReset::None;
        const float dt = m_time ? (displayTime - m_time) / 1e9f : 0.f;
        if (!active || !s.enabled || s.tauMs <= 0.f) reset = TemporalReset::Inactive;
        else if (!m_valid) reset = TemporalReset::First;
        else if (dt <= 0.f || dt * 1000.f > s.maxGapMs) reset = TemporalReset::Gap;
        else if (space != m_space) reset = TemporalReset::Space;
        else if (SceneCuts().load(std::memory_order_relaxed) != m_sceneCuts) reset = TemporalReset::Scene;
        else if (gaze.inSaccade) reset = TemporalReset::Saccade;
        else if (eyes[0].width != m_eyes[0].width || eyes[0].height != m_eyes[0].height ||
                 eyes[1].width != m_eyes[1].width || eyes[1].height != m_eyes[1].height)
            reset = TemporalReset::Size;
        else if (AngleDeg(Multiply(Conjugate(m_eyes[0].pose), eyes[0].pose)) > s.maxTurnDeg)
            reset = TemporalReset::Turn;
        if (reset == TemporalReset::None) {
            // Frame rate for the far falloff, smoothed (2X and 3X differ, Auto switches between them).
            m_dtSmoothed = m_dtSmoothed > 0.f ? m_dtSmoothed + 0.1f * (dt - m_dtSmoothed) : dt;
            const float hz = 1.f / m_dtSmoothed;
            if (std::fabs(hz - m_farHz) > 1.f) {
                m_far = ComputeFarFalloff(hz);
                m_farHz = hz;
            }
            m_dt = dt;
            m_stats.filtered++;
            m_stats.dtSum += dt;
        }
        if (reset != TemporalReset::Inactive) {
            m_stats.frames++;
            if (reset != TemporalReset::None && reset != TemporalReset::First) {
                m_stats.resets++;
                if (reset == TemporalReset::Saccade) m_stats.saccade++;
                else if (reset == TemporalReset::Gap) m_stats.gap++;
                else if (reset == TemporalReset::Turn) m_stats.turn++;
                else if (reset == TemporalReset::Size) m_stats.size++;
                else m_stats.other++;
            }
        }
        for (int eye = 0; eye < 2; ++eye) {
            m_previous[eye] = m_eyes[eye];
            m_eyes[eye] = eyes[eye];
        }
        m_gaze = gaze;
        m_space = space;
        m_sceneCuts = SceneCuts().load(std::memory_order_relaxed);
        m_previousTime = m_time;
        m_time = displayTime;
        m_reset = reset;
        m_valid = reset != TemporalReset::Inactive;
        m_stats.falloff = m_far;
        // The panel's status: per second of display time, the last full second's counts.
        if (!m_secondStart || displayTime - m_secondStart >= 1'000'000'000 || displayTime < m_secondStart) {
            m_lastSecondFiltered = m_secondFiltered;
            m_lastSecondResets = m_secondResets;
            m_secondFiltered = m_secondResets = 0;
            m_secondStart = displayTime;
        }
        if (reset == TemporalReset::None) ++m_secondFiltered;
        else if (reset != TemporalReset::Inactive && reset != TemporalReset::First) ++m_secondResets;
        const bool on = active && s.enabled && s.tauMs > 0.f;
        const uint64_t strength = (uint64_t)std::clamp((int)std::lround(s.tauMs / 80.f * 100.f), 0, 255);
        ++m_statusUpdates;
        PeripheryStatus().store((on ? 1ull : 0ull) | (reset == TemporalReset::None ? 2ull : 0ull) | 4ull | strength << 8 |
                                    (uint64_t)std::min(m_lastSecondFiltered, 65535u) << 16 | (uint64_t)std::min(m_lastSecondResets, 65535u) << 32 |
                                    (uint64_t)(m_statusUpdates & 0xFF) << 48,
                                std::memory_order_relaxed);
        return reset;
    }

    // Whether the pass runs this frame (strength 0, a blended layer and the focus debug view do not).
    bool Active() const { return m_reset != TemporalReset::Inactive; }
    // Display time of this frame and of the one before (a history the pass did not write then is not continuous).
    int64_t Time() const { return m_time; }
    int64_t PreviousTime() const { return m_previousTime; }

    // The pass did not run after all (its resources failed, a debug view): the next frame starts over.
    void Invalidate() {
        m_valid = false;
    }

    // Per eye of the frame BeginFrame decided: focus is the focus view's field this frame (same eye frame).
    void Constants(int eye, const TanFov& focus, const TemporalSettings& s, TemporalConstants& out) const {
        out = {};
        const TemporalEye& cur = m_eyes[eye];
        const TemporalEye& prev = m_reset == TemporalReset::None ? m_previous[eye] : cur;
        // A ray of this frame's eye, into the layer's space, into the previous frame's eye.
        const Quat relative = Multiply(Conjugate(prev.pose), cur.pose);
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                float axis[3] = {0.f, 0.f, 0.f}, image[3];
                axis[column] = 1.f;
                Rotate(relative, axis, image);
                out.reproject[row][column] = image[row];
            }
        }
        const float tan[4] = {cur.fov.left, cur.fov.right, cur.fov.up, cur.fov.down};
        const float prevTan[4] = {prev.fov.left, prev.fov.right, prev.fov.up, prev.fov.down};
        std::copy(tan, tan + 4, out.fov);
        std::copy(prevTan, prevTan + 4, out.prevFov);
        // The focus area: its centre and smaller radius (the eccentricity without a gaze), and its edge as the
        // composition draws it (focus view field, half-widths and shape) for the ramp.
        const float cx = 0.5f * (focus.left + focus.right), cy = 0.5f * (focus.up + focus.down);
        const float centre[3] = {cx, cy, -1.f};
        Normalize(centre, out.focus);
        const float radiusX = 0.5f * (std::atan(focus.right) - std::atan(focus.left)) * 57.2957795f;
        const float radiusY = 0.5f * (std::atan(focus.up) - std::atan(focus.down)) * 57.2957795f;
        out.focus[3] = std::min(radiusX, radiusY);
        out.focusTan[0] = focus.left;
        out.focusTan[1] = focus.right;
        out.focusTan[2] = focus.up;
        out.focusTan[3] = focus.down;
        out.focusShape[0] = radiusX;
        out.focusShape[1] = radiusY;
        out.focusShape[2] = s.focusShape;
        if (m_gaze.valid) {
            float inEye[3];
            Rotate(Conjugate(cur.eyeInView), m_gaze.dir, inEye);
            Normalize(inEye, out.gaze);
            out.gaze[3] = 1.f;
        } else {
            std::copy(out.focus, out.focus + 3, out.gaze);  // a fixed focus: its centre stands for the gaze
            out.gaze[3] = 0.f;
        }
        out.params[0] = m_dt;
        out.params[1] = m_reset == TemporalReset::None && !s.debug ? s.tauMs / 1000.f : 0.f;
        out.params[2] = s.rampDeg;
        out.params[3] = s.clip;
        out.params2[0] = m_far.startDeg;
        out.params2[1] = m_far.endDeg;
        out.params2[2] = s.confidence;
        out.params2[3] = s.debug ? 1.f : 0.f;
        out.size[0] = (float)cur.width;
        out.size[1] = (float)cur.height;
        out.size[2] = cur.width ? 1.f / cur.width : 0.f;
        out.size[3] = cur.height ? 1.f / cur.height : 0.f;
    }

    const TemporalStats& Stats() const { return m_stats; }
    void ResetStats() { m_stats = {}; }

  private:
    static void Normalize(const float v[3], float out[3]) {
        const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int i = 0; i < 3; ++i) out[i] = length > 0.f ? v[i] / length : (i == 2 ? -1.f : 0.f);
    }

    TemporalEye m_eyes[2]{}, m_previous[2]{};
    GazeSample m_gaze{};
    uint64_t m_space{0};
    uint32_t m_sceneCuts{0};
    int64_t m_secondStart{0};
    uint32_t m_statusUpdates{0};
    unsigned m_secondFiltered{0}, m_secondResets{0}, m_lastSecondFiltered{0}, m_lastSecondResets{0};
    int64_t m_time{0}, m_previousTime{0};
    float m_dt{0.f}, m_dtSmoothed{0.f}, m_farHz{0.f};
    FarFalloff m_far{};
    TemporalReset m_reset{TemporalReset::Inactive};
    bool m_valid{false};
    TemporalStats m_stats{};
};

} // namespace dcsvr
#endif
#endif // DCSVR_PERCEPTION_H
