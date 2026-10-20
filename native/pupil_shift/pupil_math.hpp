// Pupil shift math, kept free of OpenXR calls so the tests can exercise it offline.
//
// When the eye turns, its pupil moves on a sphere around the eye's rotation centre. The runtime
// renders every frame from one fixed eye point, so near objects show a perspective error and the
// whole image shifts against the lens's virtual image (at a finite distance). Rendering from the
// real pupil position and shifting the frustum so that the virtual image plane stays put removes
// both errors. OpenXR view space: +X right, +Y up, -Z forward; lengths in metres.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pupil_shift {

struct Vec3 { float x{}, y{}, z{}; };
struct Quat { float x{}, y{}, z{}, w{1}; };
struct Fov { float left{}, right{}, up{}, down{}; };  // angles in radians, as XrFovf

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }

inline Vec3 rotate(Quat q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}
inline Quat conjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }

// Gaze in a view's local frame from a gaze-centred (focus) FOV: the centre of the focus
// rectangle in tangent space.
inline Vec3 gaze_from_focus_fov(const Fov& f) {
    const float tx = 0.5f * (std::tan(f.left) + std::tan(f.right));
    const float ty = 0.5f * (std::tan(f.down) + std::tan(f.up));
    const Vec3 g{tx, ty, -1.0f};
    return g * (1.0f / length(g));
}

inline Vec3 normalized(Vec3 a) { const float l = length(a); return l > 1e-9f ? a * (1.0f / l) : Vec3{0, 0, -1}; }

// Limits the gaze to max_deg off rest (both unit vectors): the direction the eye looks when it looks straight ahead.
inline Vec3 clamp_gaze(Vec3 g, Vec3 rest, float max_deg) {
    const float max_rad = max_deg * 3.14159265f / 180.0f;
    const float cosine = std::clamp(dot(g, rest), -1.0f, 1.0f);
    if (std::acos(cosine) <= max_rad) return g;
    const Vec3 side = g - rest * cosine;
    if (length(side) < 1e-9f) return rest;
    return rest * std::cos(max_rad) + normalized(side) * std::sin(max_rad);
}

// Pupil position relative to where it is when the eye looks along rest, for an eye of rotation radius r.
inline Vec3 pupil_offset(Vec3 gaze, Vec3 rest, float radius) { return (gaze - rest) * radius; }

// Straight ahead for both eyes: the mean of the two primary views' forward axes (world space). Headsets whose views
// are canted outwards (Pimax: a few degrees) would otherwise read a straight gaze as turned towards the nose.
inline Vec3 head_forward(Quat left, Quat right) { return normalized(rotate(left, {0, 0, -1}) + rotate(right, {0, 0, -1})); }
// Angle between the two primary views' forward axes, degrees (the views' cant, logged).
inline float cant_deg(Quat left, Quat right) {
    return std::acos(std::clamp(dot(rotate(left, {0, 0, -1}), rotate(right, {0, 0, -1})), -1.0f, 1.0f)) * 57.29578f;
}

// Moves the two eye positions symmetrically along the line between them so they are ipd apart (world space);
// returns the move of each eye. DCS's own custom IPD replaces the eyes' sideways position after this layer, so the
// layer applies that IPD itself and DCS's option is turned off: the pupil's sideways shift then reaches DCS.
inline void ipd_moves(Vec3 left, Vec3 right, float ipd, Vec3& move_left, Vec3& move_right) {
    const Vec3 between = right - left;
    const float distance = length(between);
    if (ipd <= 0 || distance < 1e-6f) { move_left = move_right = {}; return; }
    const Vec3 u = between * (1.0f / distance);
    const float half = 0.5f * (ipd - distance);
    move_left = u * -half; move_right = u * half;
}

// Frustum for rendering from a pupil displaced by d (view-local), such that, once the image is
// submitted with the original FOV, a point at infinity lands where the displaced pupil sees it
// through a virtual image at distance vid. Tangents shift by -d/vid, and scale because a pupil
// that moved back (d.z > 0) is farther from the virtual image.
inline Fov shifted_fov(const Fov& f, Vec3 d, float vid) {
    const float sx = d.x / vid, sy = d.y / vid, k = 1.0f / (1.0f + d.z / vid);
    return {std::atan((std::tan(f.left) - sx) * k), std::atan((std::tan(f.right) - sx) * k),
            std::atan((std::tan(f.up) - sy) * k), std::atan((std::tan(f.down) - sy) * k)};
}

// ---- Gaze source -------------------------------------------------------------------------------------------------
// The bundled Quad Views exports DcsVr_QuadViewsGaze (native/quadviews/dcsvr_perception.h): for a display time it has
// located, flags and its gaze in each eye's view frame. Without a gaze it shows the focus views at a fixed spot (the
// resting gaze plus a per-eye horizontal offset), which the focus-centre reading takes for a gaze about 2.4 degrees
// outwards per eye; the export says when the gaze is not real. Its gaze is also the raw one (the focus area carries
// Quad Views' focus offsets, saccade lead and deadzone). Pimax's native quad views has no export: the layer reads the
// runtime's eye gaze through XR_EXT_eye_gaze_interaction (XrEyeGaze below), which carries its own validity; only when
// neither exists does it read the focus centre (Pimax, too, puts the focus at a fixed spot about 2.4 degrees outwards
// per eye without a gaze).
namespace qv_gaze {
constexpr std::uint32_t sample = 1, valid = 2, saccade = 4;
constexpr const wchar_t* module_name = L"XR_APILAYER_MBUCCHIA_quad_views_foveated.dll";
constexpr const char* export_name = "DcsVr_QuadViewsGaze";
}  // namespace qv_gaze
using QuadViewsGazeFn = std::uint32_t (*)(std::int64_t display_time, float left[3], float right[3]);

// pvr: Pimax PVR's per-eye gaze, used by Depth boost only (pvr_gaze.hpp).
enum class GazeSource : std::uint8_t { none = 0, focus = 1, qv = 2, simulated = 3, xr = 4, pvr = 5 };
inline const char* gaze_source_name(GazeSource s) {
    return s == GazeSource::focus ? "focus" : s == GazeSource::qv ? "qv" : s == GazeSource::simulated ? "simulated" :
        s == GazeSource::xr ? "xr" : s == GazeSource::pvr ? "pvr" : "none";
}

// The OpenXR eye gaze of a frame (XR_EXT_eye_gaze_interaction, the layer's own action). present: the extension, the
// system's support, the action space and the action set's attachment all worked, so the runtime says when there is no
// gaze; valid: this frame's location had ORIENTATION_VALID and ORIENTATION_TRACKED. local: the one (combined) gaze
// direction in each primary view's frame.
struct XrEyeGaze {
    bool present{}, valid{};
    Vec3 local[2]{{0, 0, -1}, {0, 0, -1}};
};

// One frame's gaze per eye, in each primary view's frame (unit vectors).
struct EyeGazes {
    GazeSource source{GazeSource::none};
    bool saccade{};          // Quad Views reported a saccade
    Vec3 local[2]{{0, 0, -1}, {0, 0, -1}};
};

// The gaze of a frame. simulate: SimulateGaze's direction for both eyes. Otherwise Quad Views' export when it has a
// sample for this display time (qv_flags from DcsVr_QuadViewsGaze, 0 without the export): its gaze when valid, none
// (the eyes at rest) when not, whatever the focus views show. Then the OpenXR eye gaze when it is present: its gaze when
// valid, none when not. Only without either, the focus views' centres (focus_local, already in the primary views'
// frames) in a quad-view configuration. A gaze that is not finite is none.
// A gaze no eye can hold is no gaze: more than kMaxUpDeg upwards (eyes rarely turn up past 25-30 degrees; the bundled
// Quad Views reported a fixed (5.9, 34.7) degrees, flagged valid, before the tracker had the eyes, 2026-10-12) or more
// than kMaxGazeDeg from straight ahead. Each eye in its own frame (+Y up, -Z forward). A simulated gaze is kept.
inline EyeGazes plausible_gaze(EyeGazes g) {
    constexpr float kMaxUpDeg = 30.0f, kMaxGazeDeg = 50.0f;
    if (g.source == GazeSource::none || g.source == GazeSource::simulated) return g;
    for (const Vec3& d : g.local) {
        const Vec3 n = normalized(d);
        const float up = std::atan2(n.y, -n.z) * 57.29578f;
        const float off = std::acos(std::clamp(-n.z, -1.0f, 1.0f)) * 57.29578f;
        if (!std::isfinite(up + off) || up > kMaxUpDeg || off > kMaxGazeDeg) { g.source = GazeSource::none; g.saccade = false; return g; }
    }
    return g;
}

inline EyeGazes select_gaze(bool simulate, Vec3 simulated, std::uint32_t qv_flags, const float qv_left[3], const float qv_right[3],
    bool quad, const Vec3 focus_local[2], const XrEyeGaze& xr = XrEyeGaze{}) {
    EyeGazes g;
    const auto finite = [](Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && length(v) > 1e-6f; };
    if (simulate) {
        g.source = GazeSource::simulated;
        g.local[0] = g.local[1] = normalized(simulated);
    } else if (qv_flags & qv_gaze::sample) {
        if (!(qv_flags & qv_gaze::valid)) return g;
        const Vec3 l{qv_left[0], qv_left[1], qv_left[2]}, r{qv_right[0], qv_right[1], qv_right[2]};
        if (!finite(l) || !finite(r)) return g;
        g.source = GazeSource::qv;
        g.saccade = (qv_flags & qv_gaze::saccade) != 0;
        g.local[0] = normalized(l); g.local[1] = normalized(r);
    } else if (xr.present) {
        if (!xr.valid || !finite(xr.local[0]) || !finite(xr.local[1])) return g;
        g.source = GazeSource::xr;
        g.local[0] = normalized(xr.local[0]); g.local[1] = normalized(xr.local[1]);
    } else if (quad) {
        if (!finite(focus_local[0]) || !finite(focus_local[1])) return g;
        g.source = GazeSource::focus;
        g.local[0] = normalized(focus_local[0]); g.local[1] = normalized(focus_local[1]);
    }
    return g;
}

// The gaze the pupil correction follows. An eye tracker's gaze is noisy (the bundled Quad Views passes it on raw: in
// the first in-headset test, 2026-10-12, single frames jumped to the 35-degree limit and one second's vertical gaze
// spanned 40-50 degrees), and a pupil that follows the noise moves the cockpit by millimetres: it shows as the cockpit
// jittering. Per axis (each eye's horizontal and vertical angle): the median of the last 5 samples (single spikes and
// pairs of bad samples never pass), then a One Euro filter (Casiez, Roussel, Vogel, CHI 2012): steady while the eye
// rests, following quickly while it moves (a saccade, smooth pursuit, a head turn's counter-rotation). Its resting
// cut-off falls with the tracker's measured noise (the median absolute deviation from the median, over the last
// second), so a noisy tracker is smoothed more and a clean one stays responsive. Short gaps (a blink, up to HoldS) hold
// the last gaze instead of snapping the pupil back to rest.
class PupilGazeFilter {
public:
    static constexpr int kMedian = 5, kNoise = 45;
    static constexpr float kBeta = 0.02f, kDerivativeHz = 1.0f, kHoldS = 0.3f;
    struct Stats { unsigned samples{}, held{}; float noise_deg{}; };

    // gaze: both eyes' gaze in their own frames, or nullptr for a frame without one. smooth_s: the resting time
    // constant for a clean tracker. Returns whether out holds a gaze (false after a gap longer than kHoldS).
    bool update(const Vec3* gaze, float dt, float smooth_s, Vec3 out[2]) {
        if (!gaze) {
            gap_s_ += dt;
            if (gap_s_ > kHoldS) reset();
            if (!have_) return false;
            ++stats_.held;
            out[0] = out_[0]; out[1] = out_[1];
            return true;
        }
        gap_s_ = 0;
        float p[4];
        for (int e = 0; e < 2; ++e) {
            const Vec3 g = normalized(gaze[e]);
            p[2 * e] = std::atan2(g.x, -g.z) * 57.29578f;
            p[2 * e + 1] = std::atan2(g.y, -g.z) * 57.29578f;
        }
        for (int i = 0; i < 4; ++i) window_[i][n_ % kMedian] = p[i];
        ++n_;
        const int m = std::min(n_, kMedian);
        float med[4];
        for (int i = 0; i < 4; ++i) {
            float v[kMedian];
            std::copy(window_[i], window_[i] + m, v);
            std::nth_element(v, v + m / 2, v + m);
            med[i] = v[m / 2];
            residual_[(r_ * 4 + i) % (kNoise * 4)] = std::fabs(p[i] - med[i]);
        }
        ++r_;
        // The tracker's noise: 1.4826 x the median absolute residual (a standard deviation for Gaussian noise).
        float sigma = 0;
        if (r_ > 8) {
            const int count = std::min(r_, kNoise) * 4;
            float v[kNoise * 4];
            std::copy(residual_, residual_ + count, v);
            std::nth_element(v, v + count / 2, v + count);
            sigma = 1.4826f * v[count / 2];
        }
        stats_.noise_deg = sigma;
        const float fc_rest = (smooth_s > 0 ? 1.0f / (6.2831853f * smooth_s) : 1000.0f) / std::max(1.0f, sigma);
        float o[4];
        for (int i = 0; i < 4; ++i) o[i] = euro_[i].update(med[i], dt, fc_rest);
        for (int e = 0; e < 2; ++e)
            out_[e] = normalized({std::tan(o[2 * e] / 57.29578f), std::tan(o[2 * e + 1] / 57.29578f), -1.0f});
        have_ = true;
        ++stats_.samples;
        out[0] = out_[0]; out[1] = out_[1];
        return true;
    }
    void reset() { *this = PupilGazeFilter{}; }
    Stats take_stats() { const Stats s = stats_; stats_.samples = stats_.held = 0; return s; }
    float noise_deg() const { return stats_.noise_deg; }

private:
    struct OneEuro {
        bool have{};
        float x{}, dx{};
        static float alpha(float fc, float dt) { const float r = 6.2831853f * fc * dt; return r / (r + 1.0f); }
        float update(float v, float dt, float fc_rest) {
            if (!have || !(dt > 0)) { if (!have) { x = v; dx = 0; have = true; } return x; }
            dx += alpha(kDerivativeHz, dt) * ((v - x) / dt - dx);
            x += alpha(fc_rest + kBeta * std::fabs(dx), dt) * (v - x);
            return x;
        }
    };
    float window_[4][kMedian]{};
    float residual_[kNoise * 4]{};
    int n_{}, r_{};
    OneEuro euro_[4];
    Vec3 out_[2]{{0, 0, -1}, {0, 0, -1}};
    bool have_{};
    float gap_s_{};
    Stats stats_{};
};

// When the pupil moves. The correction shifts the viewpoint by up to ~6 mm as the eyes turn; in reality that happens
// during the eye's own movement, where vision is suppressed (saccadic suppression), so it is never seen. Following the
// filtered gaze moves it ~150 ms after the eyes have landed, in plain sight: the cockpit was seen sliding up and down
// in the second in-headset test (2026-10-12, a clean tracker, noise 0.0x deg). So the pupil moves only during a
// saccade (a frame-to-frame gaze change above max(3 deg, 5 x the tracker's noise)) and the two frames after it, still
// inside post-saccadic suppression, onto the mean of the last two raw samples; while the eyes fixate it drifts at most
// kDriftDegS towards the filtered gaze (far below what is seen), which also follows smooth pursuit slowly. A tracker
// noisier than kNoisyDeg cannot place it reliably: it then drifts back to rest (Stats::noisy, logged).
class PupilLock {
public:
    static constexpr float kSaccadeDeg = 3.0f, kSaccadeSigmas = 5.0f, kDriftDegS = 1.0f, kNoisyDeg = 1.5f;
    static constexpr int kPostFrames = 2;
    struct Stats { unsigned saccades{}; bool noisy{}; };

    // raw: this frame's gaze (both eyes, their frames) or nullptr; filtered: PupilGazeFilter's output, valid or not;
    // rest: each eye's straight ahead; noise_deg: the filter's measured tracker noise. Returns whether out is set.
    bool update(const Vec3* raw, bool filtered_valid, const Vec3 filtered[2], const Vec3 rest[2], float noise_deg, float dt, Vec3 out[2]) {
        if (!filtered_valid) { const Stats keep = stats_; *this = PupilLock{}; stats_ = keep; return false; }
        if (!have_) { committed_[0] = filtered[0]; committed_[1] = filtered[1]; have_ = true; }
        bool moved = false;
        stats_.noisy = noise_deg > kNoisyDeg;
        if (raw) {
            if (have_prev_ && !stats_.noisy) {
                const float speed = std::max(angle_deg(raw[0], prev_raw_[0]), angle_deg(raw[1], prev_raw_[1]));
                if (speed > std::max(kSaccadeDeg, kSaccadeSigmas * noise_deg)) {
                    if (window_ == 0) ++stats_.saccades;
                    window_ = kPostFrames + 1;
                }
            }
            if (window_ > 0) {
                --window_;
                for (int e = 0; e < 2; ++e) committed_[e] = have_prev_ ? normalized(prev_raw_[e] + raw[e]) : normalized(raw[e]);
                moved = true;
            }
            prev_raw_[0] = normalized(raw[0]); prev_raw_[1] = normalized(raw[1]);
            have_prev_ = true;
        } else {
            have_prev_ = false;  // a gap: the next sample is not a speed
        }
        if (stats_.noisy) window_ = 0;  // a noisy tracker never moves it: back to rest, slowly
        if (!moved) {
            const float step = kDriftDegS * std::max(dt, 0.0f);
            for (int e = 0; e < 2; ++e) {
                const Vec3 target = stats_.noisy ? normalized(rest[e]) : normalized(filtered[e]);
                const float d = angle_deg(committed_[e], target);
                if (d > 1e-4f) committed_[e] = normalized(committed_[e] + (target - committed_[e]) * std::min(1.0f, step / d));
            }
        }
        out[0] = committed_[0]; out[1] = committed_[1];
        return true;
    }
    Stats take_stats() { const Stats s = stats_; stats_.saccades = 0; return s; }

private:
    static float angle_deg(Vec3 a, Vec3 b) { return std::acos(std::clamp(dot(normalized(a), normalized(b)), -1.0f, 1.0f)) * 57.29578f; }
    Vec3 committed_[2]{{0, 0, -1}, {0, 0, -1}}, prev_raw_[2]{{0, 0, -1}, {0, 0, -1}};
    bool have_{}, have_prev_{};
    int window_{};
    Stats stats_{};
};

struct Settings {
    bool enabled{true};              // [PupilShift] Enabled: the pupil correction (Depth boost has its own switch)
    float eye_radius{0.0105f};       // pupil to rotation centre, metres
    float virtual_image{1.5f};       // lens virtual image distance, metres
    float max_gaze_deg{35.0f};
    float smooth_s{0.25f};           // [PupilShift] SmoothMs: the resting time constant for a clean tracker (PupilGazeFilter)
    float ipd{};                     // metres; 0 = the runtime's own eye distance
    float simulate_x_deg{0}, simulate_y_deg{0};
    bool simulate{};
    bool publish_gaze{};             // [PupilShift] PublishGaze: GazeOut for the engine module's gaze effects (Visual realism)
};

// GazeOut (perception_shm.h) is written while Depth boost, SimulateGaze or PublishGaze asks for it. PublishGaze serves
// the engine module's eye-focus blur and G vision without Depth boost; with all three off the layer opens no block.
inline bool gaze_out_wanted(bool depth_boost, bool simulate, bool publish_gaze) { return depth_boost || simulate || publish_gaze; }

// ---- Depth boost -------------------------------------------------------------------------------------------------
// A camera-based simplification of GazeStereo3D (Kellnhofer et al., SIGGRAPH 2016): while the eyes fixate far, the
// render IPD (interaxial) is scaled up by s, so distant objects get s times more disparity; while they fixate the
// cockpit, s returns to 1 (orthostereo). Only the IPD scale changes, never a convergence offset: points at infinity
// keep zero disparity. Changes are rate-limited below the perceptual thresholds measured for disparity changes
// (docs/PERCEPTION.md derives the defaults).

// Two curves from the far fixation's distance to the scale (docs/FIXATION_DEPTH.md, "Modes"). Realistic (default): x1
// up to FarStartM (150 m), where a headset's stereo is already about as good as real stereo (1 px of disparity at
// ~46 px/deg falls near 190 m), then a log ramp to MaxScale (1.25) at FarFullM (400 m) and beyond, where real stereo
// still works and the headset's pixels no longer resolve it. Enhanced: x1 at 3 m, MaxScale (1.4) from 50 m: more depth,
// not realistic (distant objects look nearer and the world smaller).
enum class DepthMode : std::uint8_t { realistic = 0, enhanced = 1 };
inline const char* depth_mode_name(DepthMode m) { return m == DepthMode::enhanced ? "enhanced" : "realistic"; }
struct DepthModeLimits { float max_default, max_min, max_max, far_start_m, far_full_m; };
inline DepthModeLimits depth_mode_limits(DepthMode m) {
    return m == DepthMode::enhanced ? DepthModeLimits{1.4f, 1.0f, 2.0f, 3.0f, 50.0f} : DepthModeLimits{1.25f, 1.0f, 1.5f, 150.0f, 400.0f};
}

struct DepthBoostSettings {
    bool enabled{};                  // [DepthBoost] Enabled
    // [DepthBoost] Mode. The layer's ini default is realistic; the struct keeps Enhanced's values so the controller
    // tests keep their historical numbers.
    DepthMode mode{DepthMode::enhanced};
    float far_start_m{3.0f};         // the scale is 1 up to this far-fixation distance
    float far_full_m{50.0f};         // and the maximum from this one (log ramp between)
    float max_scale{1.4f};           // render IPD / base IPD while fixating far, 1..2
    float near_m{1.5f};              // a fixation nearer than this is the cockpit: scale 1
    float up_mm_per_s{5.0f};         // ramp towards max_scale
    float down_mm_per_s{40.0f};      // return towards 1
    float saccade_step_mm{4.0f};     // extra change allowed per saccade (vision is suppressed); 0 = none
    float recover_ms{800.0f};        // max_scale is capped so the return to 1 takes at most this long
    float hold_ms{500.0f};           // without a fresh gaze the scale holds this long, then returns to 1
    // [DepthBoost] Stepped (the layer's default; the struct keeps the ramps for the controller's historical tests): the
    // scale changes in steps during saccades only (DepthBoostController::step_stepped), at most fixation_drift_mm_per_s
    // while the eyes fixate.
    bool stepped{false};
    float fixation_drift_mm_per_s{0.5f};  // [DepthBoost] DriftMmPerS: the change allowed while fixating, both ways
    float step_down_mm{8.0f};             // [DepthBoost] StepDownMm: per saccade towards a lower far scale
    // [DepthBoost] FullNearReturn (default 1): back to x1 for the cockpit in one go, inside the saccade or landing window
    // that brings the eyes there (a miniature cockpit is the worst case, and saccadic suppression hides the change);
    // when the cockpit is only confirmed after that window, at NearReturnMmPerS until the next saccade finishes it.
    bool full_near_return{true};
    float near_return_mm_per_s{80.0f};
};

// The view-space basis of the head from the two eye positions and the head's forward direction (world space):
// +X right (left eye to right eye), +Y up, -Z forward.
struct HeadBasis { Vec3 right{1, 0, 0}, up{0, 1, 0}, forward{0, 0, -1}; };
inline HeadBasis head_basis(Vec3 left_eye, Vec3 right_eye, Vec3 forward) {
    HeadBasis b;
    b.right = normalized(right_eye - left_eye);
    if (length(right_eye - left_eye) < 1e-6f) b.right = {1, 0, 0};
    b.up = normalized(cross(b.right, normalized(forward)));
    b.forward = cross(b.up, b.right);
    return b;
}

// Inverse fixation distance (1/m, 0 = infinity) from the two eyes' gaze directions (world space, from the eyes at
// left_eye and right_eye), by binocular vergence. In the head's frame each eye's gaze has horizontal tangent tx; the
// two rays meet at depth z = ipd / (tx_left - tx_right) and the fixation's distance from the point between the eyes
// is z * sqrt(1 + tx^2 + ty^2) with the mean tangents. Exact for rays that meet; for rays that do not (tracker noise,
// vertical misalignment) it uses their horizontal projection. Diverging or parallel rays read as infinity (0), a
// fixation nearer than 0.2 m as 0.2 m. NaN (no estimate) when a gaze does not point forward.
inline float inverse_fixation_distance(Vec3 left_eye, Vec3 right_eye, Vec3 gaze_left, Vec3 gaze_right, const HeadBasis& head) {
    const float ipd = length(right_eye - left_eye);
    const float fl = dot(gaze_left, head.forward), fr = dot(gaze_right, head.forward);
    if (!(ipd > 0.01f) || !(fl > 0.1f) || !(fr > 0.1f)) return std::nanf("");
    const float txl = dot(gaze_left, head.right) / fl, txr = dot(gaze_right, head.right) / fr;
    const float tyl = dot(gaze_left, head.up) / fl, tyr = dot(gaze_right, head.up) / fr;
    const float tx = 0.5f * (txl + txr), ty = 0.5f * (tyl + tyr);
    const float w = (txl - txr) / (ipd * std::sqrt(1.0f + tx * tx + ty * ty));
    if (!std::isfinite(w)) return std::nanf("");
    return std::clamp(w, 0.0f, 5.0f);
}

enum class Fixation : std::uint8_t { unknown = 0, near_ = 1, far_ = 2 };
// probing: no proof yet that the gaze carries vergence; vergence: the two eyes' gazes have shown both a cockpit and a
// far fixation (a real per-eye source); constant: the vergence stays constant while the gaze moves, i.e. both focus
// views come from one combined gaze (the bundled Quad Views projects one gaze through both eyes), so depth is unknown.
enum class DepthSource : std::uint8_t { probing = 0, vergence = 1, constant = 2 };

// Fixation depth v1: binocular vergence from the per-eye gaze, filtered. Samples during a saccade (and 50 ms after it)
// are rejected: vergence is meaningless while the eyes jump. A 9-sample median then a 80 ms IIR remove tracker noise;
// a hysteresis band (near below near_m, far beyond 2 x near_m) keeps the class from flickering.
//
// It is the secondary source: FixationDecider (below) combines it with the scene depth at the gaze (the primary one,
// docs/FIXATION_DEPTH.md) and uses its class for vetoes and when no scene depth arrives.
class FixationEstimator {
public:
    static constexpr int kMedian = 9, kMinSamples = 5, kBlock = 45;
    static constexpr float kSaccadeDegPerS = 120.0f, kSaccadeHoldS = 0.05f, kIirS = 0.08f;
    static constexpr float kHoldMaxS = 0.06f;
    static constexpr float kConstantStd = 0.002f, kVaryingStd = 0.01f, kConstantSpanDeg = 2.0f, kSaccadeStepDeg = 4.0f;

    // Once per display time (ns). valid: a fresh gaze for this time (not repeated from an earlier frame).
    // cyclopean: the mean gaze in the head's frame, for the saccade speed. w: inverse_fixation_distance().
    // saccade_hint: the gaze provider reports a saccade (Quad Views' detector): the sample counts as one too.
    void update(std::int64_t time, bool valid, Vec3 cyclopean, float w, float near_m, bool saccade_hint = false) {
        if (time <= last_time_) return;  // the same display time located again, or an older one: nothing new
        const float dt = last_time_ ? static_cast<float>(static_cast<double>(time - last_time_) * 1e-9) : 0.0f;
        last_time_ = time;
        saccade_started_ = false;
        if (!valid || !std::isfinite(w) || !std::isfinite(cyclopean.x + cyclopean.y + cyclopean.z)) {
            // No gaze (a blink, lost tracking): the history is gone, and so is a saccade in progress.
            valid_ = false; count_ = 0; have_direction_ = false; history_n_ = 0; saccade_ = false; saccade_until_ = 0;
            return;
        }
        valid_ = true;
        last_fresh_ = time;
        const Vec3 c = normalized(cyclopean);
        // Saccade: the cyclopean gaze turns faster than 120 deg/s (the speed rule of native/quadviews/dcsvr_gaze.h),
        // measured between the mean of the last three samples and the mean of the three before. That divides the per-eye
        // tracker noise's apparent speed by about five (single 90 Hz samples with 1 degree of noise read as 100+ deg/s),
        // while a saccade (100 deg/s or more for 30 ms or more) still stands out, a frame or two later.
        if (have_direction_ && dt >= 0.2f) { history_n_ = 0; saccade_ = false; }
        if (have_direction_ && dt < 0.004f) return;  // too close to the last sample for a speed: not a sample either
        if (saccade_hint) {
            if (!saccade_) { ++saccades_; saccade_from_ = have_direction_ ? direction_ : c; budget_given_ = false; }
            saccade_ = true;
            saccade_until_ = std::max(saccade_until_, time + static_cast<std::int64_t>(kSaccadeHoldS * 1e9f));
        }
        history_[history_n_ % 6] = c; history_t_[history_n_ % 6] = time; ++history_n_;
        if (history_n_ >= 6) {
            Vec3 recent{}, older{};
            double recent_t = 0, older_t = 0;
            for (unsigned k = 0; k < 3; ++k) {
                const unsigned i = (history_n_ - 1 - k) % 6, j = (history_n_ - 4 - k) % 6;
                recent = recent + history_[i]; recent_t += static_cast<double>(history_t_[i]);
                older = older + history_[j]; older_t += static_cast<double>(history_t_[j]);
            }
            const float span = static_cast<float>((recent_t - older_t) / 3 * 1e-9);
            const float speed = span > 0 ? std::acos(std::clamp(dot(normalized(recent), normalized(older)), -1.0f, 1.0f)) * 57.29578f / span : 0.0f;
            if (speed >= kSaccadeDegPerS) {
                if (!saccade_) { ++saccades_; saccade_from_ = normalized(older); budget_given_ = false; }
                saccade_ = true;
                saccade_until_ = time + static_cast<std::int64_t>(kSaccadeHoldS * 1e9f);
                // The controller's saccade step is for real saccades: once the gaze has travelled 4 degrees.
                if (!budget_given_ && std::acos(std::clamp(dot(c, saccade_from_), -1.0f, 1.0f)) * 57.29578f >= kSaccadeStepDeg) {
                    saccade_started_ = true; budget_given_ = true;
                }
            } else if (time >= saccade_until_) {
                saccade_ = false;
            }
        }
        direction_ = c; have_direction_ = true;
        if (saccade_) { count_ = 0; ++rejected_; return; }
        // Median of the last samples since the saccade or the gap.
        ring_[count_ % kMedian] = w;
        ++count_;
        const int n = std::min(count_, kMedian);
        if (n < kMinSamples) return;
        float sorted[kMedian];
        std::copy(ring_, ring_ + n, sorted);
        std::sort(sorted, sorted + n);
        const float median = sorted[n / 2];
        const float a = dt > 0 ? 1.0f - std::exp(-dt / kIirS) : 1.0f;
        // The IIR restarts with each fixation (the first estimate after a saccade or a gap): it never blends the
        // vergence of the previous fixation into the new one.
        filtered_ = std::isfinite(filtered_) && count_ > kMinSamples ? filtered_ + (median - filtered_) * a : median;
        if (filtered_ > 1.0f / near_m) fixation_ = Fixation::near_;
        else if (filtered_ < 0.5f / near_m) fixation_ = Fixation::far_;
        if (fixation_ == Fixation::near_) seen_near_ = true;
        if (fixation_ == Fixation::far_) seen_far_ = true;
        if (source_ == DepthSource::probing && seen_near_ && seen_far_) source_ = DepthSource::vergence;
        account_block(w, c);
    }

    // Once per display time whose gaze only repeats an earlier one (the provider held it, or the tracker updates more
    // slowly than the frames): no sample, so the history, the estimate and the state hold. A repeat lasting longer than
    // kHoldMaxS since the last fresh sample is a gap, as update(valid = false) treats one.
    void hold(std::int64_t time) {
        if (time <= last_time_) return;
        if (!valid_ || time - last_fresh_ > static_cast<std::int64_t>(kHoldMaxS * 1e9f)) {
            update(time, false, Vec3{}, std::nanf(""), 1.0f);
            return;
        }
        last_time_ = time;
        saccade_started_ = false;
        if (saccade_ && time >= saccade_until_) saccade_ = false;  // no sample to prolong it
    }

    // A depth source other than vergence (not used yet): its class and inverse distance replace the estimate.
    void set_external(Fixation fixation, float inverse_distance) {
        fixation_ = fixation; filtered_ = inverse_distance; source_ = DepthSource::vergence; valid_ = true;
    }

    Fixation fixation() const { return fixation_; }
    DepthSource source() const { return source_; }
    float inverse_distance() const { return filtered_; }  // NaN before the first estimate
    bool valid() const { return valid_; }
    bool saccade() const { return saccade_; }
    // The class comes from fixation samples measured since the last saccade or gap.
    bool settled() const { return valid_ && !saccade_ && count_ >= kMinSamples; }
    // True on the frame a saccade has travelled 4 degrees (once per saccade): the controller may add its saccade step.
    bool saccade_started() const { return saccade_started_; }
    unsigned saccades() const { return saccades_; }
    unsigned rejected() const { return rejected_; }

private:
    // Blocks of 45 accepted samples (half a second at 90 Hz): a vergence that stays constant to 0.002/m while the gaze
    // sweeps 2 degrees or more, twice in a row, is not a per-eye measurement. Real per-eye trackers show far more
    // noise than that. A block that varies again sends the source back to probing (both classes must be seen again).
    void account_block(float w, Vec3 c) {
        const float yaw = std::atan2(c.x, -c.z) * 57.29578f, pitch = std::atan2(c.y, -c.z) * 57.29578f;
        if (block_n_ == 0) { yaw_min_ = yaw_max_ = yaw; pitch_min_ = pitch_max_ = pitch; sum_ = sum2_ = 0; first_ = w; }
        yaw_min_ = std::min(yaw_min_, yaw); yaw_max_ = std::max(yaw_max_, yaw);
        pitch_min_ = std::min(pitch_min_, pitch); pitch_max_ = std::max(pitch_max_, pitch);
        const double d = static_cast<double>(w) - first_;  // shifted sums: no cancellation for tiny spreads
        sum_ += d; sum2_ += d * d;
        if (++block_n_ < kBlock) return;
        const double mean = sum_ / block_n_;
        const float spread = static_cast<float>(std::sqrt(std::max(0.0, sum2_ / block_n_ - mean * mean)));
        const float span = std::max(yaw_max_ - yaw_min_, pitch_max_ - pitch_min_);
        block_n_ = 0;
        if (spread < kConstantStd && span >= kConstantSpanDeg) {
            if (++constant_blocks_ >= 2) { source_ = DepthSource::constant; seen_near_ = seen_far_ = false; }
        } else {
            constant_blocks_ = 0;
            if (source_ == DepthSource::constant && spread >= kVaryingStd) source_ = DepthSource::probing;
        }
    }

    std::int64_t last_time_{}, saccade_until_{}, last_fresh_{};
    Vec3 direction_{}, saccade_from_{}, history_[6]{};
    std::int64_t history_t_[6]{};
    unsigned history_n_{};
    bool have_direction_{}, valid_{}, saccade_{}, saccade_started_{}, seen_near_{}, seen_far_{}, budget_given_{};
    float ring_[kMedian]{};
    int count_{};
    float filtered_{std::nanf("")};
    Fixation fixation_{Fixation::unknown};
    DepthSource source_{DepthSource::probing};
    unsigned saccades_{}, rejected_{};
    int block_n_{}, constant_blocks_{};
    double sum_{}, sum2_{}, first_{};
    float yaw_min_{}, yaw_max_{}, pitch_min_{}, pitch_max_{};
};

// ---- Fixation depth from the scene (docs/FIXATION_DEPTH.md) -----------------------------------------------------------
// The engine module samples DCS's depth buffer in a 1 degree cone around the gaze and in a ring at 1.5 degrees, and
// publishes shares of near (< 1.5 m) and far (> 3 m) samples (perception_shm.h, DepthIn). The rules below turn those
// summaries (primary) and the binocular vergence (secondary) into a calm fixation state.
namespace fixation_rules {
constexpr float majority = 0.70f;            // near or far needs this share of the cone
constexpr float edge_share = 0.30f;          // the ring mixes near and far by at least this much each: an edge
constexpr float landing_confirm_s = 0.12f;   // after a saccade lands, the new state needs this long of agreement
constexpr float pursuit_confirm_s = 0.30f;   // without a saccade (smooth pursuit, drift), this long
constexpr float landing_expiry_s = 0.40f;    // a landing not confirmed by then (it rested on an edge) is a fixation:
                                             // a change then needs the pursuit rule
constexpr float landing_window_s = 0.08f;    // the saccade step may still be used this long after the saccade ended
constexpr float target_deadband = 0.02f;     // the far target moves only by at least this much of scale
constexpr float depth_memory_s = 2.0f;       // a far target from the scene depth outlives a stale summary this long
constexpr float veto_max_m = 1.0f;           // the vergence vetoes a far depth only nearer than this (HUD, glass: within
                                             // reach; a far fixation misread this near needs 4 degrees of vergence error)
constexpr float depth_gap_s = 0.25f;         // a single frame without a summary: the last depth class still counts against
                                             // a far vergence
constexpr int far_median = 9;                // far distances in the median of the far target
}  // namespace fixation_rules

// What the layer takes from a DepthIn summary.
struct DepthSummary {
    float near_share{}, far_share{}, edge_near{}, edge_far{};
    float median_m{}, nearest_m{}, far_median_m{};
};
enum class DepthClass : std::uint8_t { none = 0, near_ = 1, far_ = 2, ambiguous = 3 };
inline const char* depth_class_name(DepthClass c) {
    return c == DepthClass::near_ ? "near" : c == DepthClass::far_ ? "far" : c == DepthClass::ambiguous ? "ambiguous" : "none";
}
inline bool share_ok(float x) { return std::isfinite(x) && x >= 0.0f && x <= 1.0f; }
// Near or far with a clear (70 %) majority of the cone; anything else (or nonsense) is ambiguous.
inline DepthClass classify_depth(const DepthSummary& d) {
    if (!share_ok(d.near_share) || !share_ok(d.far_share)) return DepthClass::ambiguous;
    if (d.near_share >= fixation_rules::majority) return DepthClass::near_;
    if (d.far_share >= fixation_rules::majority) return DepthClass::far_;
    return DepthClass::ambiguous;
}
// The 1.5 degree ring mixes near and far: the gaze is on an edge (canopy frame, glareshield, a bezel).
inline bool depth_edge(const DepthSummary& d) {
    return share_ok(d.edge_near) && share_ok(d.edge_far) && d.edge_near >= fixation_rules::edge_share && d.edge_far >= fixation_rules::edge_share;
}
// The far curve: 1 up to start_m, cap from full_m, linear in log distance between.
inline float far_scale(float distance_m, float start_m, float full_m, float cap) {
    if (!std::isfinite(cap) || cap <= 1.0f) return 1.0f;
    if (!(distance_m > start_m) || !(full_m > start_m) || !(start_m > 0)) return 1.0f;
    if (distance_m >= full_m) return cap;
    const float u = std::log(distance_m / start_m) / std::log(full_m / start_m);
    return 1.0f + (cap - 1.0f) * std::clamp(u, 0.0f, 1.0f);
}

// Pairs the engine's summaries with the gazes the layer published. A summary is used for at most 3 frames after it
// arrived (a new seq). When its displayTime is one the layer wrote, that gaze must have been valid, not in a saccade
// (depth sampled mid-flight describes no fixation), at most 8 frames old, and within 2 degrees of the current gaze (else
// the summary describes what the eyes looked at before). A displayTime the layer never wrote (the engine had none for
// its frame) is accepted on arrival alone.
class DepthMatcher {
public:
    static constexpr int kRing = 16;
    static constexpr std::uint64_t kMaxArrivalFrames = 3, kMaxGazeAgeFrames = 8;
    static constexpr float kMovedDeg = 2.0f;
    enum class State : std::uint8_t { absent = 0, stale = 1, fresh = 2 };
    enum class Reason : std::uint8_t { none = 0, invalid = 1, old = 2, no_gaze = 3, moved = 4, saccade = 5 };
    struct Read { bool ok{}; std::uint64_t seq{}; std::int64_t display_time{}; bool valid{}; DepthSummary summary{}; };

    // The gaze published for a display time (frame: the layer's own count of display times, from 1).
    void note_gaze(std::int64_t time, std::uint64_t frame, bool valid, Vec3 direction, bool saccade = false) {
        ring_[next_ % kRing] = {time, frame, valid, saccade, normalized(direction)};
        ++next_;
    }

    State evaluate(const Read& r, std::uint64_t frame, Vec3 current, bool current_valid) {
        reason_ = Reason::none; matched_ = false;
        if (r.ok && r.seq != last_seq_) { last_seq_ = r.seq; arrival_ = frame; have_ = true; last_ = r; ++received_; }
        if (!have_) return State::absent;
        if (!last_.valid) { reason_ = Reason::invalid; return State::stale; }
        if (frame - arrival_ > kMaxArrivalFrames) { reason_ = Reason::old; return State::stale; }
        for (const auto& g : ring_) {
            if (g.frame == 0 || g.time != last_.display_time) continue;
            matched_ = true;
            if (!g.valid) { reason_ = Reason::no_gaze; return State::stale; }
            if (g.saccade) { reason_ = Reason::saccade; return State::stale; }
            if (frame - g.frame > kMaxGazeAgeFrames) { reason_ = Reason::old; return State::stale; }
            if (current_valid && std::acos(std::clamp(dot(g.direction, normalized(current)), -1.0f, 1.0f)) * 57.29578f > kMovedDeg) {
                reason_ = Reason::moved; return State::stale;
            }
            break;
        }
        if (!matched_ && last_.display_time != 0 && next_ >= static_cast<unsigned>(kRing)) {
            // A display time older than every gaze still in the ring was published long ago (the engine's readback
            // stalled): not a summary for a time the layer never wrote.
            std::int64_t oldest = INT64_MAX;
            for (const auto& g : ring_) if (g.frame) oldest = std::min(oldest, g.time);
            if (last_.display_time < oldest) { reason_ = Reason::old; return State::stale; }
        }
        return State::fresh;
    }

    // At a new instance: the block's current record belongs to the previous session, so only a later one is new.
    void seed(std::uint64_t seq) { last_seq_ = seq; }

    const DepthSummary& summary() const { return last_.summary; }
    bool matched() const { return matched_; }
    Reason reason() const { return reason_; }
    unsigned received() const { return received_; }

private:
    struct Gaze { std::int64_t time{}; std::uint64_t frame{}; bool valid{}, saccade{}; Vec3 direction{0, 0, -1}; };
    Gaze ring_[kRing]{};
    unsigned next_{};
    std::uint64_t last_seq_{}, arrival_{};
    bool have_{}, matched_{};
    Read last_{};
    Reason reason_{Reason::none};
    unsigned received_{};
};

// Where the fixation depth came from on a frame.
enum class FixSource : std::uint8_t { none = 0, vergence = 1, depth = 2 };
inline const char* fix_source_name(FixSource s) { return s == FixSource::depth ? "depth" : s == FixSource::vergence ? "vergence" : "none"; }

// The fixation state, once per display time. Pure.
//  - Class per frame: the depth summary when fresh (near or far with a 70 % majority, else ambiguous); a vergence that
//    says near (and nearer than 1 m) while the depth says far makes it near (veto: HUD symbols, near things seen
//    through glass); the reverse
//    (depth near, vergence far) is left to depth. Without a fresh summary, the vergence class (when proven and
//    settled). Neither: no source, nothing changes; after HoldMs of that (or of no gaze) the state returns to near.
//  - On an edge (the ring mixes near and far) nothing changes until the gaze moves off it; a change being confirmed
//    starts over.
//  - A saccade's end arms a landing: the first class after it is the candidate (ambiguous counts as near, the safe
//    side) and becomes the state after 120 ms of agreement (ambiguous pauses a far candidate and agrees with a near
//    one; the opposite class re-arms). With per-eye vergence proven, a landing on a far depth waits until the vergence
//    has settled (about 55 ms) so a veto applies at once. Without a saccade a different class must hold 300 ms (ambiguous pauses).
//  - The end of a gap in the gaze (a blink) arms a landing too, without the saccade step. A landing not confirmed
//    within 0.4 s of evidence (it rested on an edge) expires: the eyes are fixating, and the pursuit rule applies.
//  - Ambiguous never changes the state by itself.
//  - The far target: the median of the last far distances through the mode's curve, moved only by 0.02 or more.
//    Without a scene depth for 2 s, Enhanced uses MaxScale on vergence alone (as Depth boost v1); Realistic stays at 1
//    (vergence cannot tell 150 m from 50 m).
class FixationDecider {
public:
    struct Input {
        float dt{};                      // seconds since the last update
        bool gaze_valid{};               // a fresh gaze this display time
        bool saccade{};                  // the estimator's saccade flag (and its 50 ms hold)
        bool depth_fresh{};              // a usable depth summary (DepthMatcher::State::fresh)
        DepthSummary depth{};
        bool vergence_ok{};              // per-eye vergence proven and settled this frame
        Fixation vergence{Fixation::unknown};
        bool vergence_pending{};         // per-eye vergence proven but not settled since the saccade: a landing waits for it
        float vergence_w{std::nanf("")}; // the vergence's inverse distance, 1/m (for the veto)
    };

    void update(const Input& in, const DepthBoostSettings& s, float cap) {
        const float dt = std::isfinite(in.dt) ? std::clamp(in.dt, 0.0f, 0.05f) : 0.0f;
        const float hold = std::max(0.0f, s.hold_ms) * 0.001f;
        landing_window_s_ = std::max(0.0f, landing_window_s_ - dt);
        depth_age_s_ += dt;
        veto_ = edge_ = false;
        depth_class_ = frame_class_ = DepthClass::none;
        if (!in.gaze_valid) {
            invalid_s_ += dt;
            source_ = FixSource::none;
            if (invalid_s_ >= hold) to_near();
            update_target(s, cap);
            return;
        }
        if (invalid_s_ > 0) {
            // After a blink (or any gap) the eyes may rest on something else: a new fixation, confirmed as after a
            // saccade (no saccade step: the step belongs to the saccade).
            landing_pending_ = true; candidate_ = Fixation::unknown; agree_s_ = 0; landing_age_s_ = 0;
            ++landings_;
        }
        invalid_s_ = 0;
        if (in.saccade) {
            in_saccade_ = true; landing_pending_ = false; candidate_ = Fixation::unknown; agree_s_ = 0;
            source_ = in.depth_fresh ? FixSource::depth : in.vergence_ok ? FixSource::vergence : FixSource::none;
            update_target(s, cap);
            return;
        }
        if (in_saccade_) {
            // The eyes landed: a new fixation, whose far distance starts afresh.
            in_saccade_ = false; landing_pending_ = true; candidate_ = Fixation::unknown; agree_s_ = 0; landing_age_s_ = 0;
            landing_window_s_ = fixation_rules::landing_window_s;
            far_n_ = 0;
            ++landings_;
        }
        DepthClass c = DepthClass::none;
        if (in.depth_fresh) {
            source_ = FixSource::depth;
            depth_age_s_ = 0;
            depth_class_ = c = classify_depth(in.depth);
            last_depth_class_ = depth_class_;
            edge_ = depth_edge(in.depth);
            last_median_m_ = in.depth.median_m;
            if (in.vergence_ok && in.vergence == Fixation::near_ && in.vergence_w > 1.0f / fixation_rules::veto_max_m && c == DepthClass::far_) {
                c = DepthClass::near_; veto_ = true; ++vetoes_;
            }
            else if (in.vergence_ok && in.vergence == Fixation::far_ && c == DepthClass::near_) ++depth_over_vergence_;
        } else if (in.vergence_ok) {
            source_ = FixSource::vergence;
            c = in.vergence == Fixation::near_ ? DepthClass::near_ : in.vergence == Fixation::far_ ? DepthClass::far_ : DepthClass::ambiguous;
            // A frame without a fresh summary in the middle of scene-depth frames: a far vergence does not contradict a
            // near depth seen within the last 0.25 s (depth near is trusted over vergence far); a near vergence still
            // vetoes a far one.
            if (depth_age_s_ < fixation_rules::depth_gap_s && last_depth_class_ == DepthClass::near_ && c == DepthClass::far_) {
                frame_class_ = DepthClass::ambiguous;
                none_s_ = 0;
                update_target(s, cap);
                return;  // no evidence either way: pauses
            }
        } else {
            source_ = FixSource::none;
        }
        frame_class_ = c;
        if (c == DepthClass::none) {
            none_s_ += dt;
            if (none_s_ >= hold) to_near();
            update_target(s, cap);
            return;
        }
        none_s_ = 0;
        if (landing_pending_ && (landing_age_s_ += dt) > fixation_rules::landing_expiry_s) { landing_pending_ = false; candidate_ = Fixation::unknown; agree_s_ = 0; }
        if (c == DepthClass::ambiguous) ++ambiguous_frames_;
        // Frozen on an edge: no change, and a change being confirmed starts over (the fixation is not stable on it).
        if (edge_) { ++edge_frames_; agree_s_ = pursuit_s_ = 0; update_target(s, cap); return; }
        // A landing on a far depth waits for the vergence to settle when it is proven, so its veto can act on the new
        // fixation (a veto only ever turns far into near: a near or ambiguous landing does not wait).
        if (landing_pending_ && in.vergence_pending && c == DepthClass::far_) { update_target(s, cap); return; }
        const Fixation cls = c == DepthClass::near_ ? Fixation::near_ : c == DepthClass::far_ ? Fixation::far_ : Fixation::unknown;
        if (landing_pending_) {
            if (candidate_ == Fixation::unknown) { candidate_ = cls == Fixation::unknown ? Fixation::near_ : cls; agree_s_ = dt; }
            else if (cls == candidate_ || (cls == Fixation::unknown && candidate_ == Fixation::near_)) agree_s_ += dt;
            else if (cls != Fixation::unknown) { candidate_ = cls; agree_s_ = dt; }
            if (agree_s_ >= fixation_rules::landing_confirm_s - 1e-4f) { commit(candidate_); landing_pending_ = false; candidate_ = Fixation::unknown; }
        } else if (cls != Fixation::unknown) {
            if (cls == state_) { pursuit_ = Fixation::unknown; pursuit_s_ = 0; }
            else {
                if (pursuit_ == cls) pursuit_s_ += dt; else { pursuit_ = cls; pursuit_s_ = dt; }
                if (pursuit_s_ >= fixation_rules::pursuit_confirm_s - 1e-4f) { commit(cls); pursuit_ = Fixation::unknown; pursuit_s_ = 0; }
            }
        }
        if (source_ == FixSource::depth && depth_class_ == DepthClass::far_ && !veto_ && std::isfinite(in.depth.far_median_m) && in.depth.far_median_m > 0) {
            far_[far_n_ % fixation_rules::far_median] = in.depth.far_median_m;
            ++far_n_;
        }
        update_target(s, cap);
    }

    Fixation state() const { return state_; }
    FixSource source() const { return source_; }
    float target() const { return target_; }
    bool edge() const { return edge_; }
    bool veto() const { return veto_; }
    DepthClass depth_class() const { return depth_class_; }
    DepthClass frame_class() const { return frame_class_; }
    // A landing on near is pending while the state is far: no move towards the boost; the saccade step may be used
    // while the landing window lasts.
    bool pending_near() const { return landing_pending_ && candidate_ == Fixation::near_ && state_ == Fixation::far_; }
    bool landing_window() const { return landing_window_s_ > 0; }
    bool landing_step() const { return pending_near() && landing_window(); }
    // No move towards the boost while a change is being confirmed or the gaze is on an edge (the scale holds there).
    bool hold_up() const { return landing_pending_ || in_saccade_ || pursuit_ == Fixation::near_ || edge_; }
    float far_distance_m() const { return far_n_ ? median_far() : std::nanf(""); }
    float depth_distance_m() const { return depth_recent() ? last_median_m_ : std::nanf(""); }
    bool depth_recent() const { return depth_age_s_ < fixation_rules::depth_memory_s; }
    unsigned changes() const { return changes_; }
    unsigned vetoes() const { return vetoes_; }
    unsigned depth_over_vergence() const { return depth_over_vergence_; }
    unsigned edge_frames() const { return edge_frames_; }
    unsigned ambiguous_frames() const { return ambiguous_frames_; }
    unsigned landings() const { return landings_; }
    // For the per-frame trace: the landing being confirmed (candidate, agreement so far) and the pursuit's.
    bool landing_pending() const { return landing_pending_; }
    Fixation candidate() const { return candidate_; }
    float agree_s() const { return agree_s_; }
    Fixation pursuit() const { return pursuit_; }
    float pursuit_s() const { return pursuit_s_; }

private:
    void commit(Fixation f) { if (f != state_) { state_ = f; ++changes_; } }
    void to_near() {
        commit(Fixation::near_);
        landing_pending_ = false; candidate_ = pursuit_ = Fixation::unknown; pursuit_s_ = agree_s_ = 0;
        far_n_ = 0;
    }
    float median_far() const {
        const int n = std::min(far_n_, fixation_rules::far_median);
        float v[fixation_rules::far_median];
        std::copy(far_, far_ + n, v);
        std::sort(v, v + n);
        return v[n / 2];
    }
    void update_target(const DepthBoostSettings& s, float cap) {
        if (state_ != Fixation::far_) { target_ = 1.0f; return; }
        float wanted;
        if (depth_recent()) {
            if (!far_n_) return;  // the scene depth is there but has no far distance for this fixation yet: hold
            wanted = far_scale(median_far(), s.far_start_m, s.far_full_m, cap);
        } else {
            wanted = s.mode == DepthMode::enhanced ? cap : 1.0f;
        }
        if (std::fabs(wanted - target_) >= fixation_rules::target_deadband || wanted == 1.0f || wanted == cap) target_ = wanted;
    }

    Fixation state_{Fixation::near_}, candidate_{Fixation::unknown}, pursuit_{Fixation::unknown};
    FixSource source_{FixSource::none};
    DepthClass depth_class_{DepthClass::none}, frame_class_{DepthClass::none}, last_depth_class_{DepthClass::none};
    bool in_saccade_{}, landing_pending_{}, veto_{}, edge_{};
    float agree_s_{}, pursuit_s_{}, none_s_{}, invalid_s_{}, landing_window_s_{}, landing_age_s_{}, depth_age_s_{1e9f};
    float target_{1.0f}, last_median_m_{std::nanf("")};
    float far_[fixation_rules::far_median]{};
    int far_n_{};
    unsigned changes_{}, vetoes_{}, depth_over_vergence_{}, edge_frames_{}, ambiguous_frames_{}, landings_{};
};

// The largest scale the settings allow: max_scale (1..2), capped so that the return to 1 at down_mm_per_s takes at
// most recover_ms (GazeStereo3D's quick-recovery guarantee).
inline float depth_boost_cap(const DepthBoostSettings& s, float base_mm) {
    const float max_scale = std::clamp(std::isfinite(s.max_scale) ? s.max_scale : 1.0f, 1.0f, 2.0f);
    if (!(base_mm > 1.0f) || !(s.down_mm_per_s > 0) || !(s.recover_ms > 0)) return 1.0f;
    return std::min(max_scale, 1.0f + s.down_mm_per_s * s.recover_ms * 0.001f / base_mm);
}

// The render IPD scale s, rate-limited. Pure: step() takes everything it needs.
class DepthBoostController {
public:
    struct Input {
        float dt{};                          // seconds since the last step (clamped to 50 ms)
        bool active{};                       // Depth boost enabled and not switched off in flight
        bool gaze_valid{};                   // a fresh gaze this frame
        bool saccade{}, saccade_started{};
        bool settled{true};                  // the class comes from samples since the last saccade or gap
        Fixation fixation{Fixation::unknown};
        DepthSource source{DepthSource::probing};
        float base_ipd_m{};                  // the IPD s multiplies (Ipd= or the runtime's)
        // From FixationDecider. NaN target: the v1 rule (cap while the source is vergence and the fixation far).
        float target{std::nanf("")};         // the wanted scale, clamped to 1..cap
        bool far_rates{};                    // the state is far: a lower target (a nearer far object) moves at UpMmPerS too
        bool landing_window{};               // the saccade step stays usable a little after the saccade
        bool landing_step{};                 // spend it now towards 1: the eyes landed on something near
        // The predictive transition (predictive_depth.hpp, PredictiveGate): a step applied at once (already capped), the
        // hold against its direction (+1 no move down, -1 no move up) and the gradual saccade step withheld.
        float jump_mm{};
        int hold{};
        bool no_saccade_step{};
    };

    float step(const Input& in, const DepthBoostSettings& s) {
        const float base_mm = in.base_ipd_m * 1000.0f;
        if (!in.active || !(base_mm >= 40.0f && base_mm <= 90.0f)) {
            scale_ = 1.0f; budget_mm_ = 0; invalid_s_ = 0; target_ = 1.0f; fixation_targets_n_ = 0; in_saccade_ = false; return scale_;
        }
        if (s.stepped) return step_stepped(in, s, base_mm);
        const float dt = std::isfinite(in.dt) ? std::clamp(in.dt, 0.0f, 0.05f) : 0.0f;
        const float cap = depth_boost_cap(s, base_mm);
        if (in.gaze_valid) {
            invalid_s_ = 0;
            target_ = std::isfinite(in.target) ? std::clamp(in.target, 1.0f, std::max(1.0f, cap)) :
                in.source == DepthSource::vergence && in.fixation == Fixation::far_ ? cap : 1.0f;
        } else {
            invalid_s_ += dt;
            // No gaze: the scale never changes on its own (a blink keeps it) until the hold time, then it returns to 1.
            // The saccade step does not outlive the saccade through a gap.
            budget_mm_ = 0;
            if (invalid_s_ * 1000.0f < s.hold_ms) { scale_ = std::min(scale_, cap); return scale_; }
            target_ = 1.0f;
        }
        if (in.saccade_started) budget_mm_ = std::max(0.0f, s.saccade_step_mm);
        if ((!in.saccade && !in.landing_window) || in.no_saccade_step) budget_mm_ = 0;
        // A predictive step: during the saccade, at once.
        if (in.gaze_valid && std::isfinite(in.jump_mm) && in.jump_mm != 0.0f) scale_ = std::clamp(scale_ + in.jump_mm / base_mm, 1.0f, std::max(1.0f, cap));
        // The eyes landed on something near while the state is still far (being confirmed): the saccade step goes
        // towards 1 now, at the tail of saccadic suppression, ahead of the confirmation.
        if (in.landing_step && budget_mm_ > 0 && scale_ > 1.0f) {
            const float step = std::min(budget_mm_, (scale_ - 1.0f) * base_mm);
            budget_mm_ -= step;
            scale_ = std::max(1.0f, scale_ - step / base_mm);
        }
        float delta_mm = (target_ - scale_) * base_mm;
        // Never towards the boost on a stale estimate: during a saccade and until the new fixation has been measured (the
        // eyes may be landing in the cockpit). The return towards 1 is always allowed.
        if (delta_mm > 0 && !in.settled) delta_mm = 0;
        // After a predictive step the scale holds against it until the landing confirms or contradicts it.
        if ((in.hold > 0 && delta_mm < 0) || (in.hold < 0 && delta_mm > 0)) delta_mm = 0;
        const float rate = std::max(0.0f, delta_mm > 0 || in.far_rates ? s.up_mm_per_s : s.down_mm_per_s) * dt;
        const float allowed = rate + (in.saccade ? budget_mm_ : 0.0f);
        const float move = std::clamp(delta_mm, -allowed, allowed);
        budget_mm_ = std::max(0.0f, budget_mm_ - std::max(0.0f, std::fabs(move) - rate));
        scale_ = std::clamp(scale_ + move / base_mm, 1.0f, std::max(1.0f, cap));
        if (!std::isfinite(scale_)) scale_ = 1.0f;
        return scale_;
    }

    float scale() const { return scale_; }
    float target() const { return target_; }

private:
    // Stepped: the first in-headset tests (2026-10-12/13) showed the gradual ramps changing the IPD during 71 % of the far
    // fixations, about 18 reversals a minute as the far target followed each glance across terrain; a changing IPD
    // moves the near field's disparity by dI/Z (5 mm/s is 25-34 arcmin/s at the 0.5-0.7 m canopy, against GazeStereo3D's
    // ~6 arcmin/s for depth scaling), seen as the foreground "breathing". So:
    // - while the eyes fixate, at most fixation_drift_mm_per_s either way (0.5 mm/s: ~3 arcmin/s at 0.5 m);
    // - during a saccade (and the short landing window after it, still suppressed), a step: up to saccade_step_mm
    //   upwards, up to step_down_mm downwards (back to the cockpit too: two saccades from x1.25);
    // - upwards only towards the median of the last three fixations' targets (one far glance does not start it; two
    //   of three do), downwards towards the current one.
    float step_stepped(const Input& in, const DepthBoostSettings& s, float base_mm) {
        const float dt = std::isfinite(in.dt) ? std::clamp(in.dt, 0.0f, 0.05f) : 0.0f;
        const float cap = std::max(1.0f, depth_boost_cap(s, base_mm));
        if (in.gaze_valid) {
            invalid_s_ = 0;
            target_ = std::isfinite(in.target) ? std::clamp(in.target, 1.0f, cap) :
                in.source == DepthSource::vergence && in.fixation == Fixation::far_ ? cap : 1.0f;
        } else {
            invalid_s_ += dt;
            if (invalid_s_ * 1000.0f < s.hold_ms) { scale_ = std::min(scale_, cap); return scale_; }
            target_ = 1.0f;
        }
        // A new saccade: the fixation that ends gives its target to the history, and a fresh step budget.
        if (in.saccade && !in_saccade_) {
            fixation_targets_[fixation_targets_n_ % 3] = last_fixation_target_;
            ++fixation_targets_n_;
            up_budget_mm_ = std::max(0.0f, s.saccade_step_mm);
            down_budget_mm_ = std::max(0.0f, s.step_down_mm);
        }
        in_saccade_ = in.saccade;
        if (!in.saccade && in.settled) last_fixation_target_ = target_;
        const bool suppressed = (in.saccade || in.landing_window) && !in.no_saccade_step;
        if (!suppressed) up_budget_mm_ = down_budget_mm_ = 0;
        // A predictive step: during the saccade, at once, no larger than a saccade step upwards.
        if (in.gaze_valid && std::isfinite(in.jump_mm) && in.jump_mm != 0.0f)
            scale_ = std::clamp(scale_ + std::min(in.jump_mm, std::max(0.0f, s.saccade_step_mm)) / base_mm, 1.0f, cap);
        // The upward target: the median of the last three fixations' targets and this one's (never above this one's).
        float up_target = target_;
        if (fixation_targets_n_ >= 2) {
            float v[3];
            const int n = std::min(fixation_targets_n_, 3);
            for (int i = 0; i < n; ++i) v[i] = fixation_targets_[i];
            std::sort(v, v + n);
            up_target = std::min(target_, v[n / 2]);
        } else {
            up_target = 1.0f;  // no history yet: nothing to confirm a far fixation with
        }
        const float drift = std::max(0.0f, s.fixation_drift_mm_per_s) * dt;
        // The eyes landed on something near (the scene depth at the landing) while the state still says far: with
        // FullNearReturn, x1 now, inside the landing window (the tail of saccadic suppression), ahead of the confirmation
        // that would come after it. No step up in this window any more: the confirmation keeps it at 1.
        if (s.full_near_return && in.landing_step && suppressed && scale_ > 1.0f) {
            scale_ = 1.0f;
            up_budget_mm_ = down_budget_mm_ = 0;
            return scale_;
        }
        float delta_mm = 0;
        if (scale_ < up_target - 1e-5f) delta_mm = (up_target - scale_) * base_mm;
        else if (scale_ > target_ + 1e-5f) delta_mm = (target_ - scale_) * base_mm;
        if (delta_mm > 0 && !in.settled && !suppressed) delta_mm = 0;
        if (delta_mm > 0 && s.full_near_return && in.landing_step) delta_mm = 0;  // landed near: no rise before it is settled
        if ((in.hold > 0 && delta_mm < 0) || (in.hold < 0 && delta_mm > 0)) delta_mm = 0;
        float move = 0;
        if (delta_mm > 0) {
            move = std::min(delta_mm, drift + up_budget_mm_);
            up_budget_mm_ = std::max(0.0f, up_budget_mm_ - std::max(0.0f, move - drift));
        } else if (delta_mm < 0 && s.full_near_return && target_ <= 1.0f + 1e-5f) {
            // Back to the cockpit (target x1): the whole way inside the saccade or landing window that brings the eyes
            // there, under saccadic suppression; confirmed only after it, a quick return at near_return_mm_per_s rather
            // than a miniature cockpit until the next saccade (which finishes it at once).
            move = suppressed ? delta_mm : -std::min(-delta_mm, std::max(drift, std::max(0.0f, s.near_return_mm_per_s) * dt));
        } else if (delta_mm < 0) {
            // Towards a lower far scale (nearer terrain), and back to the cockpit with FullNearReturn=0: steps of at most
            // step_down_mm per saccade, drift in between (no flicker between near and far: changes only in saccades).
            move = -std::min(-delta_mm, drift + down_budget_mm_);
            down_budget_mm_ = std::max(0.0f, down_budget_mm_ - std::max(0.0f, -move - drift));
        }
        scale_ = std::clamp(scale_ + move / base_mm, 1.0f, cap);
        if (!std::isfinite(scale_)) scale_ = 1.0f;
        return scale_;
    }

    float scale_{1.0f}, target_{1.0f}, budget_mm_{}, invalid_s_{};
    float up_budget_mm_{}, down_budget_mm_{}, last_fixation_target_{1.0f};
    float fixation_targets_[3]{1.0f, 1.0f, 1.0f};
    int fixation_targets_n_{};
    bool in_saccade_{};
};

// PupilShift_Status() layout (64 bits). Bits 0-15 are the pupil shift fields the OFXR panel patch 0015 reads; the
// rest is Depth boost (patch 0017), so an older panel keeps working.
namespace status {
constexpr std::uint64_t running = 1ULL << 0;          // DCS asked for view poses
constexpr std::uint64_t pupil_on = 1ULL << 1;         // pupil correction enabled and not switched off in flight
constexpr std::uint64_t gaze = 1ULL << 2;             // a gaze was applied in the last second
constexpr std::uint64_t simulated = 1ULL << 3;        // SimulateGaze
constexpr std::uint64_t restored = 1ULL << 4;         // runtime projection views put back in the last second
constexpr std::uint64_t depth_on = 1ULL << 5;         // Depth boost enabled and not switched off in flight
constexpr std::uint64_t depth_source = 1ULL << 6;     // the gaze carries per-eye vergence (DepthSource::vergence)
constexpr std::uint64_t pupil_disabled = 1ULL << 7;   // [PupilShift] Enabled=0: the layer runs for Depth boost only
// bits 8-15: the largest pupil shift of the last second, 0.1 mm (saturating)
// bits 16-23: the current scale, round((s - 1) * 100), 0-100
// bits 24-25: the fixation, 0 unknown, 1 near (cockpit), 2 far
constexpr std::uint64_t depth_enabled = 1ULL << 26;   // [DepthBoost] Enabled=1 (whatever the in-flight switch says)
constexpr std::uint64_t depth_constant = 1ULL << 27;  // DepthSource::constant: the focus views share one gaze
// bits 28-29: the fixation depth's source this frame, 0 none, 1 vergence, 2 scene depth (FixSource)
constexpr std::uint64_t depth_edge = 1ULL << 30;      // the gaze is on an edge: the state is frozen
constexpr std::uint64_t depth_veto = 1ULL << 31;      // the vergence vetoed a far scene depth (near)
// bits 32-47: the current scale, round((s - 1) * 10000)
// bits 48-59: the fixation distance in cm, 4095 = 40.95 m or farther, 0 = no estimate
constexpr std::uint64_t predictive = 1ULL << 60;          // the predictive transition is in effect
constexpr std::uint64_t predictive_fallback = 1ULL << 61; // predictive selected, gradual in effect (no high-rate gaze)
constexpr int shift_mm10 = 8, scale_percent = 16, fixation = 24, fix_source = 28, scale_fine = 32, distance_cm = 48;
}  // namespace status

// The Depth boost fields of the status word (bits 5-7, 16-31, 32-59).
inline std::uint64_t depth_status_bits(bool pupil_enabled, bool depth_enabled, bool depth_on, DepthSource source,
    Fixation fixation, float scale, float inverse_distance, FixSource fix_source = FixSource::none, bool edge = false, bool veto = false,
    bool predictive = false, bool predictive_fallback = false, bool scene_depth = false) {
    std::uint64_t word = (predictive ? status::predictive : 0) | (predictive_fallback ? status::predictive_fallback : 0) | (depth_on ? status::depth_on : 0) | (source == DepthSource::vergence ? status::depth_source : 0) |
        (pupil_enabled ? 0 : status::pupil_disabled) | (depth_enabled ? status::depth_enabled : 0) |
        // "No depth": the gaze carries no vergence (one combined gaze) AND nothing else gives the distance. With the
        // engine's scene depth driving the fixation, or fresh within the last 2 s (scene_depth), it is not shown (the panel showed "no depth" in the first in-headset
        // test while the scene depth worked, 2026-10-12).
        (source == DepthSource::constant && fix_source != FixSource::depth && !scene_depth ? status::depth_constant : 0) | (edge ? status::depth_edge : 0) | (veto ? status::depth_veto : 0);
    word |= static_cast<std::uint64_t>(static_cast<unsigned>(fix_source) & 3U) << status::fix_source;
    const float s = std::isfinite(scale) ? std::clamp(scale, 1.0f, 2.0f) : 1.0f;
    word |= static_cast<std::uint64_t>(std::lround((s - 1.0f) * 100.0f)) << status::scale_percent;
    word |= static_cast<std::uint64_t>(static_cast<unsigned>(fixation) & 3U) << status::fixation;
    word |= static_cast<std::uint64_t>(std::lround((s - 1.0f) * 10000.0f)) << status::scale_fine;
    std::uint64_t cm = 0;
    if (std::isfinite(inverse_distance)) cm = inverse_distance <= 1.0f / 40.95f ? 4095 : std::min<std::uint64_t>(4095, static_cast<std::uint64_t>(std::lround(100.0f / inverse_distance)));
    word |= cm << status::distance_cm;
    return word;
}
constexpr std::uint64_t depth_status_mask = 0x3FFF'FFFF'FFFF'00E0ULL;

}  // namespace pupil_shift
