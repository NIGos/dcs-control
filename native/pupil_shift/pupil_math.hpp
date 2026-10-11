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

struct Settings {
    bool enabled{true};
    float eye_radius{0.0105f};       // pupil to rotation centre, metres
    float virtual_image{1.5f};       // lens virtual image distance, metres
    float max_gaze_deg{35.0f};
    float ipd{};                     // metres; 0 = the runtime's own eye distance
    float simulate_x_deg{0}, simulate_y_deg{0};
    bool simulate{};
};

}  // namespace pupil_shift
