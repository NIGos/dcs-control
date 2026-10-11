// Offline tests of the pupil shift geometry. A planar virtual image at distance D stands in for
// the lens: the runtime shows a pixel at tangent t (in the original FOV) at the virtual point
// (t * D, -D). The test checks that, seen from the displaced pupil, every object lands where it
// really is.
#include <cmath>
#include <cstdio>

#include "pupil_math.hpp"

using namespace pupil_shift;

static int failures = 0;
static void check(bool ok, const char* what, double a = 0, double b = 0) {
    if (!ok) { ++failures; std::printf("FAIL %s (%.6g vs %.6g)\n", what, a, b); }
}
static float tan_deg(float d) { return std::tan(d * 3.14159265f / 180.0f); }

// Tangent at which the runtime shows a world point X when the frame is rendered from pupil P
// with frustum fr and submitted with the original frustum fo.
static void displayed_tangent(const Fov& fr, const Fov& fo, Vec3 P, Vec3 X, float& tx, float& ty) {
    const Vec3 d = X - P;
    const float wx = d.x / -d.z, wy = d.y / -d.z;
    const float fx = (wx - std::tan(fr.left)) / (std::tan(fr.right) - std::tan(fr.left));
    const float fy = (wy - std::tan(fr.down)) / (std::tan(fr.up) - std::tan(fr.down));
    tx = std::tan(fo.left) + fx * (std::tan(fo.right) - std::tan(fo.left));
    ty = std::tan(fo.down) + fy * (std::tan(fo.up) - std::tan(fo.down));
}

int main() {
    // Focus FOV centred on 20 deg right, 10 deg up gives that gaze.
    {
        const float cx = tan_deg(20), cy = tan_deg(10), h = 0.3f;
        const Fov focus{std::atan(cx - h), std::atan(cx + h), std::atan(cy + h), std::atan(cy - h)};
        const Vec3 g = gaze_from_focus_fov(focus);
        check(std::fabs(g.x / -g.z - cx) < 1e-5f, "gaze x", g.x / -g.z, cx);
        check(std::fabs(g.y / -g.z - cy) < 1e-5f, "gaze y", g.y / -g.z, cy);
    }
    // Pupil offset: straight gaze is zero; 25 deg right moves r*sin sideways and r*(1-cos) back.
    {
        const float r = 0.0105f;
        const Vec3 z = pupil_offset({0, 0, -1}, {0, 0, -1}, r);
        check(length(z) < 1e-9f, "straight gaze offset", length(z), 0);
        const float s = std::sin(25 * 3.14159265f / 180), c = std::cos(25 * 3.14159265f / 180);
        const Vec3 o = pupil_offset({s, 0, -c}, {0, 0, -1}, r);
        check(std::fabs(o.x - r * s) < 1e-7f, "offset x", o.x, r * s);
        check(std::fabs(o.z - r * (1 - c)) < 1e-7f, "offset z", o.z, r * (1 - c));
    }
    // Clamp keeps the direction and limits the angle.
    {
        const Vec3 g = clamp_gaze({std::sin(0.8f), 0, -std::cos(0.8f)}, {0, 0, -1}, 30);
        check(std::fabs(std::acos(-g.z) - 30 * 3.14159265f / 180) < 1e-5f, "clamp angle", std::acos(-g.z), 0.5236);
        check(g.x > 0 && std::fabs(g.y) < 1e-7f, "clamp direction", g.x, g.y);
    }
    // Rotation: 90 deg about Y turns -Z into -X.
    {
        const float h = std::sqrt(0.5f);
        const Vec3 v = rotate({0, h, 0, h}, {0, 0, -1});
        check(std::fabs(v.x + 1) < 1e-6f && std::fabs(v.z) < 1e-6f, "rotate", v.x, v.z);
    }
    // The geometry: objects far and near are seen in their true direction from the moved pupil.
    {
        const Fov fo{std::atan(-1.1f), std::atan(1.1f), std::atan(0.95f), std::atan(-0.95f)};
        const float D = 1.5f, r = 0.0105f;
        for (float gaze_deg : {0.0f, 15.0f, 25.0f, 35.0f}) {
            const float s = std::sin(gaze_deg * 3.14159265f / 180), c = std::cos(gaze_deg * 3.14159265f / 180);
            const Vec3 P = pupil_offset({s, 0.3f * s, -c}, {0, 0, -1}, r);
            const Fov fr = shifted_fov(fo, P, D);
            for (float dist : {1000.0f, 5.0f, 0.7f, 0.4f})
                for (float ang : {-30.0f, 0.0f, 10.0f, 30.0f}) {
                    const Vec3 X{dist * tan_deg(ang), dist * 0.2f, -dist};
                    float tx, ty;
                    displayed_tangent(fr, fo, P, X, tx, ty);
                    const Vec3 V{tx * D, ty * D, -D};           // where the lens shows that pixel
                    const Vec3 seen = V - P, truth = X - P;
                    const float ex = std::atan(seen.x / -seen.z) - std::atan(truth.x / -truth.z);
                    const float ey = std::atan(seen.y / -seen.z) - std::atan(truth.y / -truth.z);
                    check(std::fabs(ex) < 2e-5f && std::fabs(ey) < 2e-5f, "seen direction", ex * 57.3, ey * 57.3);
                    // Without the correction the error is the full pupil swim (sanity: it is not tiny).
                    if (gaze_deg == 25.0f && dist == 0.7f && ang == 0.0f) {
                        float ux, uy;
                        displayed_tangent(fo, fo, {0, 0, 0}, X, ux, uy);
                        const Vec3 Vu{ux * D, uy * D, -D};
                        const float swim = std::atan((Vu - P).x / -(Vu - P).z) - std::atan(truth.x / -truth.z);
                        std::printf("uncorrected error at 25 deg gaze, object at 0.7 m: %.3f deg\n", swim * 57.3);
                        check(std::fabs(swim) > 0.002f, "uncorrected swim visible", swim, 0);
                    }
                }
        }
    }
    // Canted views: straight ahead is the head's forward, so a straight gaze moves no pupil.
    {
        const float a = 6 * 3.14159265f / 180 / 2;  // each view turned 3 deg outwards about +Y
        const Quat left{0, std::sin(a / 2), 0, std::cos(a / 2)}, right{0, -std::sin(a / 2), 0, std::cos(a / 2)};
        check(std::fabs(cant_deg(left, right) - 6) < 1e-3f, "cant", cant_deg(left, right), 6);
        const Vec3 head = head_forward(left, right);
        check(std::fabs(head.x) < 1e-6f && std::fabs(head.z + 1) < 1e-6f, "head forward", head.x, head.z);
        const Vec3 rest = rotate(conjugate(left), head);
        const Vec3 straight = pupil_offset(rest, rest, 0.0105f);
        check(length(straight) < 1e-9f, "straight gaze on canted view", length(straight), 0);
        // The old view-axis reference would have moved it about 0.55 mm (3 deg on a 10.5 mm radius).
        check(length(pupil_offset(rest, {0, 0, -1}, 0.0105f)) > 0.0005f, "view-axis reference moves it", length(pupil_offset(rest, {0, 0, -1}, 0.0105f)), 0);
        const Vec3 limited = clamp_gaze(normalized(Vec3{1, 0, -1}), rest, 30);
        check(std::fabs(std::acos(std::clamp(dot(limited, rest), -1.0f, 1.0f)) * 57.29578f - 30) < 1e-3f, "clamp about rest", 0, 30);
    }
    // DCS's custom IPD applied by the layer: both eyes move by the same amount along the line between them.
    {
        Vec3 ml, mr;
        const Vec3 l{-0.03277f, 0.01f, 0.002f}, r{0.03277f, 0.01f, 0.002f};  // 65.54 mm
        ipd_moves(l, r, 0.064f, ml, mr);
        check(std::fabs(length((r + mr) - (l + ml)) - 0.064f) < 1e-6f, "ipd distance", length((r + mr) - (l + ml)), 0.064);
        check(length((l + ml + r + mr) * 0.5f - (l + r) * 0.5f) < 1e-7f, "ipd keeps the centre", 0, 0);
        ipd_moves(l, r, 0, ml, mr);
        check(length(ml) == 0 && length(mr) == 0, "no ipd, no move", 0, 0);
    }
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
