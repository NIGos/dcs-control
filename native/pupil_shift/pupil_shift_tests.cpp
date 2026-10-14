// Offline tests of the pupil shift geometry. A planar virtual image at distance D stands in for
// the lens: the runtime shows a pixel at tangent t (in the original FOV) at the virtual point
// (t * D, -D). The test checks that, seen from the displaced pupil, every object lands where it
// really is.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "perception_shm.h"
#include "predictive_depth.hpp"
#include "pupil_math.hpp"
#include "pvr_gaze.hpp"

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

// ---- Depth boost ----------------------------------------------------------------------------------------------------
// A small deterministic generator, so the noisy traces are the same on every run.
struct Random {
    std::uint64_t state{0x9E3779B97F4A7C15ULL};
    float uniform() { state = state * 6364136223846793005ULL + 1442695040888963407ULL; return static_cast<float>((state >> 40) & 0xFFFFFF) / 16777216.0f; }
    float gauss() { const float u = std::max(uniform(), 1e-7f), v = uniform(); return std::sqrt(-2.0f * std::log(u)) * std::cos(6.2831853f * v); }
};

// Per-eye gaze towards a point (head frame: eyes on the X axis), with optional noise in degrees per eye and axis.
static void eye_gazes(Vec3 target, float ipd, float noise_deg, Random& rng, Vec3& gl, Vec3& gr) {
    const Vec3 el{-ipd / 2, 0, 0}, er{ipd / 2, 0, 0};
    gl = normalized(target - el); gr = normalized(target - er);
    if (noise_deg > 0) {
        const float k = noise_deg / 57.29578f;
        gl = normalized(gl + Vec3{rng.gauss() * k, rng.gauss() * k, 0});
        gr = normalized(gr + Vec3{rng.gauss() * k, rng.gauss() * k, 0});
    }
}

static void depth_boost_tests() {
    const float ipd = 0.0655f;
    const HeadBasis head = head_basis({-ipd / 2, 0, 0}, {ipd / 2, 0, 0}, {0, 0, -1});
    Random rng;
    // Vergence: exact for rays that meet, anywhere in the field; infinity for parallel or diverging rays.
    for (float distance : {0.4f, 0.7f, 1.5f, 10.0f, 300.0f})
        for (float yaw : {-30.0f, 0.0f, 20.0f})
            for (float pitch : {-20.0f, 0.0f, 15.0f}) {
                const Vec3 dir = normalized({tan_deg(yaw), tan_deg(pitch), -1});
                Vec3 gl, gr;
                eye_gazes(dir * distance, ipd, 0, rng, gl, gr);
                const float w = inverse_fixation_distance({-ipd / 2, 0, 0}, {ipd / 2, 0, 0}, gl, gr, head);
                check(std::fabs(w - 1.0f / distance) < 2e-4f * (1.0f / distance) + 1e-4f, "vergence distance", 1.0f / w, distance);
            }
    check(inverse_fixation_distance({-ipd / 2, 0, 0}, {ipd / 2, 0, 0}, {0, 0, -1}, {0, 0, -1}, head) == 0, "parallel is infinity", 0, 0);
    check(inverse_fixation_distance({-ipd / 2, 0, 0}, {ipd / 2, 0, 0}, normalized({-0.05f, 0, -1}), normalized({0.05f, 0, -1}), head) == 0, "diverging is infinity", 0, 0);
    check(std::isnan(inverse_fixation_distance({-ipd / 2, 0, 0}, {ipd / 2, 0, 0}, {0, 0, 1}, {0, 0, -1}, head)), "backwards gaze is no estimate", 0, 0);
    // The same in a turned head with canted views: the basis comes from the eye line and the views' mean forward.
    {
        const float a = 0.6f;  // head turned 34 deg about Y
        const Quat q{0, std::sin(a / 2), 0, std::cos(a / 2)};
        const Vec3 el = rotate(q, {-ipd / 2, 0, 0}), er = rotate(q, {ipd / 2, 0, 0});
        const float c = 3 * 3.14159265f / 180 / 2;
        const Quat ql = Quat{0, std::sin(a / 2 + c), 0, std::cos(a / 2 + c)}, qr = Quat{0, std::sin(a / 2 - c), 0, std::cos(a / 2 - c)};
        const HeadBasis turned = head_basis(el, er, head_forward(ql, qr));
        const Vec3 target = rotate(q, Vec3{0.1f, -0.2f, -0.7f});
        const float w = inverse_fixation_distance(el, er, normalized(target - el), normalized(target - er), turned);
        check(std::fabs(1.0f / w - length(target - (el + er) * 0.5f)) < 1e-3f, "vergence in a turned head", 1.0f / w, length(target - (el + er) * 0.5f));
    }
    // Status word: every field decodes back.
    {
        const std::uint64_t word = depth_status_bits(false, true, true, DepthSource::vergence, Fixation::far_, 1.3217f, 1.0f / 12.34f);
        check(((word >> status::scale_percent) & 0xFF) == 32, "status scale percent", static_cast<double>((word >> 16) & 0xFF), 32);
        check(((word >> status::scale_fine) & 0xFFFF) == 3217, "status scale fine", static_cast<double>((word >> 32) & 0xFFFF), 3217);
        check(((word >> status::fixation) & 3) == 2, "status fixation", 0, 0);
        check(((word >> status::distance_cm) & 0xFFF) == 1234, "status distance", static_cast<double>((word >> 48) & 0xFFF), 1234);
        check((word & status::depth_on) && (word & status::depth_source) && (word & status::pupil_disabled) && (word & status::depth_enabled) && !(word & status::depth_constant), "status flags", 0, 0);
        check((word & ~depth_status_mask) == 0, "depth fields stay out of the pupil shift's bits", 0, 0);
        // "No depth" only when nothing gives a distance: a combined gaze (no vergence) with the scene depth driving is fine.
        check((depth_status_bits(true, true, true, DepthSource::constant, Fixation::far_, 1.03f, 0.0f, FixSource::none) & status::depth_constant) &&
              !(depth_status_bits(true, true, true, DepthSource::constant, Fixation::far_, 1.03f, 0.0f, FixSource::depth) & status::depth_constant),
            "status: no depth only without the scene depth", 0, 0);
        check(((depth_status_bits(true, false, false, DepthSource::probing, Fixation::unknown, 1.0f, 0.0f) >> status::distance_cm) & 0xFFF) == 4095, "infinity is 4095 cm", 0, 0);
        check(depth_status_bits(true, false, false, DepthSource::probing, Fixation::unknown, std::nanf(""), std::nanf("")) == 0, "NaN-safe status", 0, 0);
    }
    // Estimator: a per-eye trace with 0.7 deg noise per eye, cockpit (0.7 m) then far, with a saccade between.
    const DepthBoostSettings settings;  // defaults
    {
        FixationEstimator e;
        std::int64_t t = 1'000'000'000;
        const std::int64_t frame = 11'111'111;
        Vec3 gl, gr;
        auto feed = [&](Vec3 target, float noise) {
            eye_gazes(target, ipd, noise, rng, gl, gr);
            const float w = inverse_fixation_distance({-ipd / 2, 0, 0}, {ipd / 2, 0, 0}, gl, gr, head);
            const Vec3 c = normalized(gl + gr);
            e.update(t, true, c, w, settings.near_m);
            t += frame;
        };
        for (int i = 0; i < 60; ++i) feed({0.1f, -0.25f, -0.65f}, 0.7f);
        check(e.fixation() == Fixation::near_, "cockpit reads near", e.inverse_distance(), 1 / 0.7);
        check(e.source() == DepthSource::probing, "one class seen: still probing", 0, 0);
        // A 25 deg saccade over 40 ms (600 deg/s): rejected, the class does not change during it.
        const Vec3 far_target = normalized({tan_deg(15), tan_deg(5), -1}) * 2000.0f;
        const unsigned rejected = e.rejected();
        for (int i = 1; i <= 4; ++i) {
            const float k = i / 4.0f;
            const Vec3 from = normalized({0.1f, -0.25f, -0.65f}), to = normalized(far_target);
            feed(normalized(from * (1 - k) + to * k) * (0.7f + k * 1999.3f), 0.0f);
            check(e.fixation() == Fixation::near_, "saccade freezes the class", i, 0);
        }
        check(e.saccade(), "saccade detected by its end", 0, 0);
        check(e.saccades() >= 1 && e.rejected() > rejected, "saccade samples rejected", e.rejected(), rejected);
        for (int i = 0; i < 60; ++i) feed(far_target, 0.7f);
        check(e.fixation() == Fixation::far_, "far reads far", e.inverse_distance(), 0);
        check(e.source() == DepthSource::vergence, "both classes seen: vergence source", 0, 0);
        // A missing gaze: no sample, invalid.
        e.update(t, false, {0, 0, -1}, std::nanf(""), settings.near_m); t += frame;
        check(!e.valid() && e.fixation() == Fixation::far_, "invalid gaze keeps the class", 0, 0);
        // The same display time again changes nothing.
        e.update(t - frame, true, {0, 0, -1}, 2.0f, settings.near_m);
        check(!e.valid(), "repeated display time ignored", 0, 0);
        // A repeated gaze (held by the provider, a tracker slower than the frames) holds the estimate and the state for up
        // to kHoldMaxS; longer is a gap.
        for (int i = 0; i < 20; ++i) feed(far_target, 0.0f);
        const float held = e.inverse_distance();
        for (int i = 0; i < 5; ++i) { e.hold(t); t += frame; }  // 55 ms
        check(e.valid() && e.inverse_distance() == held && e.fixation() == Fixation::far_, "a repeated gaze holds the estimate", e.inverse_distance(), held);
        feed(far_target, 0.0f);
        check(e.valid() && e.fixation() == Fixation::far_, "the next fresh sample continues the history", 0, 0);
        for (int i = 0; i < 7; ++i) { e.hold(t); t += frame; }  // 77 ms
        check(!e.valid(), "a repeat longer than 60 ms is a gap", 0, 0);
    }
    // Combined gaze (one gaze projected through both eyes at 1 m, as the bundled Quad Views does): the vergence never
    // changes, so the source never proves itself and the scale stays 1. Projected at infinity (parallel): flagged.
    for (float projected : {1.0f, 1e9f}) {
        FixationEstimator e;
        std::int64_t t = 1'000'000'000;
        for (int i = 0; i < 900; ++i, t += 11'111'111) {
            const float yaw = 20.0f * std::sin(i * 0.02f) + rng.gauss() * 0.5f, pitch = 10.0f * std::sin(i * 0.013f);
            const Vec3 dir = normalized({tan_deg(yaw), tan_deg(pitch), -1});
            Vec3 gl, gr;
            if (projected > 1e6f) gl = gr = dir;
            else eye_gazes(dir * projected, ipd, 0, rng, gl, gr);
            e.update(t, true, normalized(gl + gr), inverse_fixation_distance({-ipd / 2, 0, 0}, {ipd / 2, 0, 0}, gl, gr, head), settings.near_m);
            check(e.source() != DepthSource::vergence, "combined gaze never proves vergence", projected, 0);
        }
        check(projected > 1e6f ? e.source() == DepthSource::constant : e.source() != DepthSource::vergence, "constant vergence detected", projected, 0);
    }
    // Stepped controller (the layer's default): while fixating at most DriftMmPerS; up only in saccade steps of
    // SaccadeStepMm towards the median of the last fixations' targets; back to 1 in the saccade that lands in the cockpit.
    {
        DepthBoostSettings st = settings;
        st.stepped = true; st.mode = DepthMode::realistic; st.max_scale = 1.25f;
        DepthBoostController c;
        DepthBoostController::Input in;
        // A combined gaze (no vergence): the scene depth gives the target.
        in.active = true; in.gaze_valid = true; in.source = DepthSource::constant; in.base_ipd_m = 0.064f; in.dt = 1.0f / 45;
        const auto fixate = [&](Fixation f, float target, float seconds) {
            in.saccade = false; in.landing_window = false; in.settled = true; in.fixation = f; in.target = target;
            float worst = 0, last = c.scale();
            for (float t = 0; t < seconds; t += in.dt) { c.step(in, st); worst = std::max(worst, std::fabs(c.scale() - last) * 64.0f / in.dt); last = c.scale(); }
            return worst;  // mm/s
        };
        const auto saccade = [&](Fixation f, float target) {
            in.saccade = true; in.settled = false; in.fixation = f; in.target = target;
            const float before = c.scale();
            for (int i = 0; i < 2; ++i) c.step(in, st);
            in.saccade = false; in.landing_window = true; c.step(in, st);
            return (c.scale() - before) * 64.0f;  // mm
        };
        // One far glance from the cockpit: no climb (one fixation of three is not a confirmation).
        fixate(Fixation::near_, 1.0f, 1.0f);
        saccade(Fixation::far_, 1.25f);
        const float glance = fixate(Fixation::far_, 1.25f, 0.5f);
        check(c.scale() - 1.0f < 0.5f * 0.5f / 64.0f + 1e-4f && glance <= 0.5f + 1e-3f, "stepped: one far glance hardly moves it (drift only)", (c.scale() - 1) * 64, glance);
        // Several far fixations: climbs only in saccade steps of at most 4 mm, and while fixating at most 0.5 mm/s.
        float worst_fix = 0, worst_step = 0;
        int saccades = 0;
        while (c.scale() < 1.25f - 1e-4f && saccades < 20) {
            worst_step = std::max(worst_step, saccade(Fixation::far_, 1.25f));
            worst_fix = std::max(worst_fix, fixate(Fixation::far_, 1.25f, 0.4f));
            ++saccades;
        }
        check(c.scale() > 1.25f - 1e-4f && saccades <= 6, "stepped: reaches x1.25 within 6 saccades", saccades, 6);
        check(worst_step <= 4.0f + 0.5f + 1e-3f && worst_fix <= 0.5f + 1e-3f, "stepped: at most 4 mm per saccade, 0.5 mm/s while fixating", worst_step, worst_fix);
        // Terrain nearer than full strength for a while: down only in saccade steps (8 mm) and drift, never a ramp.
        const float far_drift = fixate(Fixation::far_, 1.10f, 1.0f);
        check(far_drift <= 0.5f + 1e-3f, "stepped: a lower far target is not a ramp while fixating", far_drift, 0.5);
        // Back to the cockpit (FullNearReturn, the default): exactly 1 within the saccade that lands there, steady after.
        while (c.scale() < 1.25f - 1e-4f) { saccade(Fixation::far_, 1.25f); fixate(Fixation::far_, 1.25f, 0.4f); }
        const float down = -saccade(Fixation::near_, 1.0f);
        const float after = fixate(Fixation::near_, 1.0f, 0.3f);
        check(c.scale() == 1.0f && down > 15.0f && after == 0.0f, "stepped: back to 1 in the one saccade that lands in the cockpit", down, after);
        // The cockpit confirmed only after the landing window: a quick return (80 mm/s), not a miniature cockpit until
        // the next saccade.
        while (c.scale() < 1.25f - 1e-4f) { saccade(Fixation::far_, 1.25f); fixate(Fixation::far_, 1.25f, 0.4f); }
        saccade(Fixation::far_, 1.25f);  // the eyes land in the cockpit, the estimate still says far
        const float late = fixate(Fixation::near_, 1.0f, 0.5f);
        check(c.scale() == 1.0f && late <= 80.0f + 1e-2f && late > 40.0f, "stepped: a late cockpit confirmation returns at 80 mm/s", late, 80);
        // The live case (2026-10-13): the saccade ends, the landing window opens, the scene depth at the landing says near
        // (landing_step) while the state still says far for another 100 ms: x1 at once in that window, no rise after.
        while (c.scale() < 1.25f - 1e-4f) { saccade(Fixation::far_, 1.25f); fixate(Fixation::far_, 1.25f, 0.4f); }
        in.saccade = true; in.settled = false; in.fixation = Fixation::far_; in.target = 1.25f;
        c.step(in, st);
        in.saccade = false; in.landing_window = true; c.step(in, st);
        const float before_landing = c.scale();
        in.landing_step = true;
        for (int i = 0; i < 5; ++i) c.step(in, st);
        const float landed = c.scale();
        in.landing_step = false; in.landing_window = false; in.settled = true; in.fixation = Fixation::near_; in.target = 1.0f;
        for (int i = 0; i < 20; ++i) c.step(in, st);
        check(before_landing > 1.2f && landed == 1.0f && c.scale() == 1.0f, "stepped: landed near, state still far: x1 in the landing window", before_landing, landed);
        // FullNearReturn=0: the earlier steps of at most 8 mm per saccade, nothing in between; 1 after two saccades.
        st.full_near_return = false;
        while (c.scale() < 1.25f - 1e-4f) { saccade(Fixation::far_, 1.25f); fixate(Fixation::far_, 1.25f, 0.4f); }
        float worst_down = 0, between = 0;
        for (int i = 0; i < 2; ++i) {
            worst_down = std::max(worst_down, -saccade(Fixation::near_, 1.0f));
            between = std::max(between, fixate(Fixation::near_, 1.0f, 0.3f));
        }
        check(c.scale() == 1.0f && worst_down <= 8.0f + 0.5f + 1e-3f && between <= 0.5f + 1e-3f,
            "stepped, FullNearReturn=0: back to 1 in two cockpit saccades of at most 8 mm, steady in between", worst_down, between);
    }
    // Controller: far ramps up at most UpMmPerS, near returns at most DownMmPerS, saccades add at most SaccadeStepMm.
    {
        DepthBoostController c;
        DepthBoostController::Input in;
        in.active = true; in.gaze_valid = true; in.source = DepthSource::vergence; in.base_ipd_m = 0.064f; in.dt = 1.0f / 90;
        in.fixation = Fixation::far_;
        float last = c.scale(), seconds = 0;
        while (c.scale() < 1.4f - 1e-6f && seconds < 30) {
            c.step(in, settings); seconds += in.dt;
            check((c.scale() - last) * 64.0f <= settings.up_mm_per_s * in.dt + 1e-4f, "ramp-up rate", (c.scale() - last) * 64.0f, settings.up_mm_per_s * in.dt);
            last = c.scale();
        }
        check(std::fabs(c.scale() - 1.4f) < 1e-5f && std::fabs(seconds - 25.6f / 5.0f) < 0.05f, "reaches the maximum in (max-1)*IPD/rate", seconds, 25.6 / 5);
        // Recovery: from the maximum back to exactly 1 within RecoverMs.
        in.fixation = Fixation::near_;
        seconds = 0; last = c.scale();
        while (c.scale() > 1.0f && seconds < 5) {
            c.step(in, settings); seconds += in.dt;
            check((last - c.scale()) * 64.0f <= settings.down_mm_per_s * in.dt + 1e-4f, "return rate", (last - c.scale()) * 64.0f, 0);
            last = c.scale();
        }
        check(c.scale() == 1.0f && seconds <= settings.recover_ms / 1000 + 1e-3f, "recovery within RecoverMs, to exactly 1", seconds, settings.recover_ms / 1000);
        // The quick-recovery cap: a slow return rate lowers the reachable maximum.
        DepthBoostSettings slow = settings; slow.down_mm_per_s = 10;
        check(std::fabs(depth_boost_cap(slow, 64) - 1.125f) < 1e-6f, "recovery cap", depth_boost_cap(slow, 64), 1.125);
        // A saccade adds at most SaccadeStepMm, once per saccade.
        in.fixation = Fixation::near_;
        DepthBoostController d; in.fixation = Fixation::far_;
        for (int i = 0; i < 300; ++i) d.step(in, settings);
        const float before = d.scale();
        in.fixation = Fixation::near_; in.saccade = in.saccade_started = true;
        d.step(in, settings);
        in.saccade_started = false;
        d.step(in, settings); d.step(in, settings);
        const float saccade_mm = (before - d.scale()) * 64.0f;
        check(saccade_mm <= settings.saccade_step_mm + 3 * settings.down_mm_per_s * in.dt + 1e-3f && saccade_mm > 3 * settings.down_mm_per_s * in.dt, "saccade step bounded", saccade_mm, settings.saccade_step_mm);
        in.saccade = false;
        // A stale estimate (a saccade, or no fresh fixation yet) never pushes towards the boost, only back to 1.
        {
            DepthBoostController st; DepthBoostController::Input stale = in;
            stale.fixation = Fixation::far_; stale.settled = false;
            for (int i = 0; i < 100; ++i) st.step(stale, settings);
            check(st.scale() == 1.0f, "no boost on a stale estimate", st.scale(), 1);
        }
        // No gaze: holds through a blink (HoldMs), then returns to 1.
        DepthBoostController h; in.fixation = Fixation::far_;
        for (int i = 0; i < 400; ++i) h.step(in, settings);
        const float held = h.scale();
        in.gaze_valid = false;
        for (int i = 0; i < 40; ++i) { h.step(in, settings); check(h.scale() == held, "blink: the scale holds", h.scale(), held); }
        for (int i = 0; i < 120; ++i) h.step(in, settings);
        check(h.scale() == 1.0f, "a lost gaze returns to 1", h.scale(), 1);
        // Unknown source or near: 1; switched off: 1 at once; NaN inputs: finite.
        in.gaze_valid = true; in.source = DepthSource::constant; in.fixation = Fixation::far_;
        DepthBoostController u; for (int i = 0; i < 300; ++i) u.step(in, settings);
        check(u.scale() == 1.0f, "no proven source: 1", u.scale(), 1);
        in.source = DepthSource::vergence; for (int i = 0; i < 300; ++i) u.step(in, settings);
        in.active = false; u.step(in, settings);
        check(u.scale() == 1.0f, "switched off: 1 at once", u.scale(), 1);
        in.active = true; in.dt = std::nanf(""); in.base_ipd_m = std::nanf(""); u.step(in, settings);
        check(u.scale() == 1.0f, "NaN-safe", u.scale(), 1);
        in.dt = 5.0f; in.base_ipd_m = 0.064f; u.step(in, settings);
        check((u.scale() - 1.0f) * 64.0f <= settings.up_mm_per_s * 0.05f + 1e-4f, "a long frame gap moves at most 50 ms worth", u.scale(), 1);
    }
    // The IPD move with a scaled base: exact distance, centre kept, focus views follow their eye.
    {
        const Vec3 l{-0.03275f, 0.012f, -0.004f}, r{0.03275f, 0.012f, -0.004f};
        for (float s : {1.0f, 1.25f, 1.4f, 2.0f}) {
            Vec3 ml, mr;
            ipd_moves(l, r, 0.064f * s, ml, mr);
            check(std::fabs(length((r + mr) - (l + ml)) - 0.064f * s) < 1e-6f, "scaled ipd distance", length((r + mr) - (l + ml)), 0.064 * s);
            check(length((l + ml + r + mr) * 0.5f - (l + r) * 0.5f) < 1e-7f, "scaled ipd keeps the centre", 0, 0);
        }
    }
}

// ---- Gaze source --------------------------------------------------------------------------------------------------
// Quad Views' fixed focus without a gaze: the resting gaze plus a per-eye horizontal offset (outwards), read from the
// focus views as about 2.4 degrees outwards per eye. With Quad Views' export the frame has no gaze at all.
static void gaze_source_tests() {
    const float out = 2.4f;
    const Vec3 fixed_focus[2] = {normalized({-tan_deg(out), 0, -1}), normalized({tan_deg(out), 0, -1})};
    const float qv_l[3] = {std::sin(0.2f), 0.0f, -std::cos(0.2f)}, qv_r[3] = {std::sin(0.15f), 0.0f, -std::cos(0.15f)};
    const Vec3 sim{tan_deg(10), 0, -1};
    // No export (Pimax native quad views): the focus centres, as before.
    EyeGazes g = select_gaze(false, sim, 0, qv_l, qv_r, true, fixed_focus);
    check(g.source == GazeSource::focus && std::fabs(g.local[0].x + std::sin(out * 3.14159265f / 180)) < 1e-6f, "no export: focus centre", g.local[0].x, 0);
    check(select_gaze(false, sim, 0, qv_l, qv_r, false, fixed_focus).source == GazeSource::none, "no export, stereo: no gaze", 0, 0);
    // Export, invalid: the fixed focus is no gaze, whatever the focus views say.
    g = select_gaze(false, sim, qv_gaze::sample, qv_l, qv_r, true, fixed_focus);
    check(g.source == GazeSource::none && !g.saccade, "qv invalid: no gaze (the fixed focus is not read)", static_cast<int>(g.source), 0);
    // Export, valid: its own gaze per eye, preferred to the (stabilised) focus centres.
    g = select_gaze(false, sim, qv_gaze::sample | qv_gaze::valid, qv_l, qv_r, true, fixed_focus);
    check(g.source == GazeSource::qv && std::fabs(g.local[0].x - std::sin(0.2f)) < 1e-6f && std::fabs(g.local[1].x - std::sin(0.15f)) < 1e-6f && !g.saccade,
        "qv valid: its gaze per eye", g.local[0].x, std::sin(0.2f));
    g = select_gaze(false, sim, qv_gaze::sample | qv_gaze::valid | qv_gaze::saccade, qv_l, qv_r, true, fixed_focus);
    check(g.source == GazeSource::qv && g.saccade, "qv saccade flag", g.saccade, 1);
    // A valid flag without the sample flag is not a sample (nothing recorded for this time): the focus centres.
    check(select_gaze(false, sim, qv_gaze::valid, qv_l, qv_r, true, fixed_focus).source == GazeSource::focus, "qv no sample: focus", 0, 0);
    // A non-finite export is no gaze; SimulateGaze wins over everything.
    const float bad[3] = {std::nanf(""), 0, -1};
    check(select_gaze(false, sim, qv_gaze::sample | qv_gaze::valid, bad, qv_r, true, fixed_focus).source == GazeSource::none, "qv NaN: no gaze", 0, 0);
    g = select_gaze(true, sim, qv_gaze::sample, qv_l, qv_r, false, fixed_focus);
    check(g.source == GazeSource::simulated && std::fabs(g.local[1].x - std::sin(10 * 3.14159265f / 180)) < 1e-6f, "simulated", g.local[1].x, 0);
    const Vec3 nan_focus[2] = {{std::nanf(""), 0, -1}, {0, 0, -1}};
    check(select_gaze(false, sim, 0, qv_l, qv_r, true, nan_focus).source == GazeSource::none, "NaN focus: no gaze", 0, 0);
    check(std::strcmp(gaze_source_name(GazeSource::qv), "qv") == 0 && std::strcmp(gaze_source_name(GazeSource::none), "none") == 0 &&
        std::strcmp(gaze_source_name(GazeSource::focus), "focus") == 0 && std::strcmp(gaze_source_name(GazeSource::simulated), "simulated") == 0 &&
        std::strcmp(gaze_source_name(GazeSource::xr), "xr") == 0, "source names", 0, 0);
    // OpenXR eye gaze (Pimax native quad views): present and valid, its gaze; present but not valid (nobody wearing the
    // headset), no gaze, although the focus views sit at the fixed spot; the export's sample wins over it; absent, the
    // focus centres as before.
    XrEyeGaze xr;
    xr.present = true; xr.valid = true;
    xr.local[0] = normalized({tan_deg(5), tan_deg(-3), -1}); xr.local[1] = normalized({tan_deg(4), tan_deg(-3), -1});
    g = select_gaze(false, sim, 0, qv_l, qv_r, true, fixed_focus, xr);
    check(g.source == GazeSource::xr && std::fabs(g.local[0].x - xr.local[0].x) < 1e-6f && std::fabs(g.local[1].x - xr.local[1].x) < 1e-6f,
        "xr valid: its gaze per eye", g.local[0].x, xr.local[0].x);
    check(select_gaze(false, sim, 0, qv_l, qv_r, false, fixed_focus, xr).source == GazeSource::xr, "xr valid, stereo: its gaze", 0, 0);
    check(select_gaze(false, sim, qv_gaze::sample | qv_gaze::valid, qv_l, qv_r, true, fixed_focus, xr).source == GazeSource::qv, "qv sample wins over xr", 0, 0);
    check(select_gaze(false, sim, qv_gaze::sample, qv_l, qv_r, true, fixed_focus, xr).source == GazeSource::none, "qv invalid wins over xr", 0, 0);
    check(select_gaze(true, sim, 0, qv_l, qv_r, true, fixed_focus, xr).source == GazeSource::simulated, "simulated wins over xr", 0, 0);
    xr.valid = false;
    g = select_gaze(false, sim, 0, qv_l, qv_r, true, fixed_focus, xr);
    check(g.source == GazeSource::none, "xr invalid: no gaze (the fixed focus is not read)", static_cast<int>(g.source), 0);
    xr.valid = true; xr.local[0] = {std::nanf(""), 0, -1};
    check(select_gaze(false, sim, 0, qv_l, qv_r, true, fixed_focus, xr).source == GazeSource::none, "xr NaN: no gaze", 0, 0);
    xr.present = false;
    check(select_gaze(false, sim, 0, qv_l, qv_r, true, fixed_focus, xr).source == GazeSource::focus, "xr absent: focus centre", 0, 0);
    // The invalid frame moves no pupil: rendered from rest, the IPD is exactly the configured one. The old reading of
    // the fixed focus moved each pupil about 0.44 mm outwards (64.9 mm rendered with Ipd=64, seen live).
    {
        const float r = 0.0105f;
        const Vec3 old_l = pupil_offset(fixed_focus[0], {0, 0, -1}, r), old_r = pupil_offset(fixed_focus[1], {0, 0, -1}, r);
        check(std::fabs((old_r.x - old_l.x) * 1000 - 0.88f) < 0.02f, "old reading: 0.88 mm wider", (old_r.x - old_l.x) * 1000, 0.88);
    }
    // Quad Views' saccade flag is a saccade for the fixation estimator too: the sample is rejected.
    {
        FixationEstimator e;
        std::int64_t t = 1'000'000'000;
        for (int i = 0; i < 20; ++i) e.update(t += 11'111'111, true, {0, 0, -1}, 0.0f, 1.5f);
        check(e.settled() && !e.saccade(), "steady fixation settles", e.settled(), 1);
        const unsigned rejected = e.rejected(), saccades = e.saccades();
        e.update(t += 11'111'111, true, {0, 0, -1}, 0.0f, 1.5f, true);
        check(e.saccade() && !e.settled() && e.rejected() == rejected + 1 && e.saccades() == saccades + 1, "saccade hint rejects the sample", e.rejected(), rejected + 1);
        e.update(t += 11'111'111, true, {0, 0, -1}, 0.0f, 1.5f, true);
        check(e.saccades() == saccades + 1, "one saccade while the hint lasts", e.saccades(), saccades + 1);
        for (int i = 0; i < 12; ++i) e.update(t += 11'111'111, true, {0, 0, -1}, 0.0f, 1.5f);
        check(!e.saccade() && e.settled(), "settles again after the hint", e.settled(), 1);
    }
}

// ---- Fixation depth (docs/FIXATION_DEPTH.md): one test per rule ---------------------------------------------------
static DepthSummary summary(float near_share, float far_share, float ring_near = 0, float ring_far = 0, float far_median = 80) {
    DepthSummary d;
    d.near_share = near_share; d.far_share = far_share; d.edge_near = ring_near; d.edge_far = ring_far;
    d.median_m = near_share >= far_share ? 0.7f : far_median; d.nearest_m = 0.6f; d.far_median_m = far_median;
    return d;
}
// A decider fed at 90 Hz. Inputs that stay the same between calls are kept in `in`.
struct DeciderRun {
    FixationDecider d;
    FixationDecider::Input in;
    DepthBoostSettings s;
    float cap{1.4f};
    DeciderRun() { in.dt = 1.0f / 90; in.gaze_valid = true; }
    // Frames until the state becomes `want` (at most `limit`); -1 if it never does.
    int until(Fixation want, int limit) {
        for (int i = 1; i <= limit; ++i) { d.update(in, s, cap); if (d.state() == want) return i; }
        return -1;
    }
    void frames(int n) { for (int i = 0; i < n; ++i) d.update(in, s, cap); }
    void saccade() { in.saccade = true; frames(4); in.saccade = false; }
    void depth(const DepthSummary& sum) { in.depth_fresh = true; in.depth = sum; }
};

static void fixation_depth_tests() {
    // Classes: a 70 % majority, a dead band, nonsense is ambiguous; an edge needs 30 % of each in the ring.
    check(classify_depth(summary(0.70f, 0.10f)) == DepthClass::near_ && classify_depth(summary(0.69f, 0.31f)) == DepthClass::ambiguous &&
          classify_depth(summary(0.10f, 0.70f)) == DepthClass::far_ && classify_depth(summary(0.5f, 0.2f)) == DepthClass::ambiguous &&
          classify_depth(summary(std::nanf(""), 0.9f)) == DepthClass::ambiguous && classify_depth(summary(1.5f, 0)) == DepthClass::ambiguous,
        "depth classes: 70 % majority, else ambiguous");
    check(depth_edge(summary(0, 1, 0.30f, 0.30f)) && !depth_edge(summary(0, 1, 0.29f, 0.71f)) && !depth_edge(summary(0, 1, std::nanf(""), 0.5f)),
        "edge: 30 % near and 30 % far in the ring");

    // The far curves. Realistic: exactly 1 to 150 m (a wingman at 80 m), log ramp to the cap at 400 m and beyond.
    const DepthModeLimits real = depth_mode_limits(DepthMode::realistic), enh = depth_mode_limits(DepthMode::enhanced);
    check(real.max_default == 1.25f && real.max_max == 1.5f && real.far_start_m == 150 && real.far_full_m == 400 &&
          enh.max_default == 1.4f && enh.max_max == 2.0f && enh.far_start_m == 3 && enh.far_full_m == 50, "mode limits");
    check(far_scale(80, 150, 400, 1.25f) == 1.0f && far_scale(150, 150, 400, 1.25f) == 1.0f && far_scale(400, 150, 400, 1.25f) == 1.25f &&
          far_scale(5000, 150, 400, 1.25f) == 1.25f, "realistic curve: 1 to 150 m, the cap from 400 m", far_scale(80, 150, 400, 1.25f), 1);
    check(std::fabs(far_scale(std::sqrt(150.0f * 400.0f), 150, 400, 1.25f) - 1.125f) < 1e-5f, "log ramp: half way at the geometric mean",
        far_scale(std::sqrt(150.0f * 400.0f), 150, 400, 1.25f), 1.125);
    check(far_scale(3, 3, 50, 1.4f) == 1.0f && far_scale(50, 3, 50, 1.4f) == 1.4f && far_scale(std::nanf(""), 3, 50, 1.4f) == 1.0f &&
          far_scale(100, 3, 50, 1.0f) == 1.0f, "enhanced curve, NaN and cap 1");

    // The matcher: absent before any summary; fresh on arrival for at most 3 frames; the gaze it was taken for must be
    // valid, not in a saccade, at most 8 frames old and within 2 degrees of the current gaze; a time the layer never
    // wrote is accepted on arrival; an invalid summary or a failed read never refreshes.
    {
        DepthMatcher m;
        const Vec3 ahead{0, 0, -1};
        DepthMatcher::Read r;
        check(m.evaluate(r, 1, ahead, true) == DepthMatcher::State::absent, "matcher: absent");
        for (std::uint64_t f = 1; f <= 10; ++f) m.note_gaze(static_cast<std::int64_t>(f) * 100, f, f != 4, ahead, f == 5);
        r.ok = true; r.seq = 2; r.display_time = 900; r.valid = true; r.summary = summary(0, 1);
        check(m.evaluate(r, 10, ahead, true) == DepthMatcher::State::fresh && m.matched(), "matcher: fresh on arrival, matched");
        check(m.evaluate(r, 13, ahead, true) == DepthMatcher::State::fresh && m.evaluate(r, 14, ahead, true) == DepthMatcher::State::stale &&
              m.reason() == DepthMatcher::Reason::old, "matcher: used for 3 frames after arrival");
        r.seq = 4; r.display_time = 400;
        check(m.evaluate(r, 14, ahead, true) == DepthMatcher::State::stale && m.reason() == DepthMatcher::Reason::no_gaze, "matcher: its gaze was not valid");
        r.seq = 6; r.display_time = 500;
        check(m.evaluate(r, 14, ahead, true) == DepthMatcher::State::stale && m.reason() == DepthMatcher::Reason::saccade, "matcher: its gaze was in a saccade");
        r.seq = 8; r.display_time = 300;
        check(m.evaluate(r, 14, ahead, true) == DepthMatcher::State::stale && m.reason() == DepthMatcher::Reason::old, "matcher: its gaze 11 frames old");
        r.seq = 10; r.display_time = 1000;
        check(m.evaluate(r, 12, normalized({std::tan(2.5f / 57.29578f), 0, -1}), true) == DepthMatcher::State::stale && m.reason() == DepthMatcher::Reason::moved,
            "matcher: the gaze moved 2.5 degrees since");
        r.seq = 12;
        check(m.evaluate(r, 12, normalized({std::tan(1.5f / 57.29578f), 0, -1}), true) == DepthMatcher::State::fresh, "matcher: 1.5 degrees is the same fixation");
        r.seq = 14; r.display_time = 123456;
        check(m.evaluate(r, 12, ahead, true) == DepthMatcher::State::fresh && !m.matched(), "matcher: an unknown time is accepted on arrival");
        r.seq = 16; r.valid = false;
        check(m.evaluate(r, 12, ahead, true) == DepthMatcher::State::stale && m.reason() == DepthMatcher::Reason::invalid, "matcher: invalid summary");
        // A display time older than every gaze in the ring (16 frames) was published long ago: not accepted on arrival.
        for (std::uint64_t f = 11; f <= 30; ++f) m.note_gaze(static_cast<std::int64_t>(f) * 100, f, true, ahead);
        r.seq = 20; r.valid = true; r.display_time = 1200;
        check(m.evaluate(r, 30, ahead, true) == DepthMatcher::State::stale && m.reason() == DepthMatcher::Reason::old, "matcher: a summary older than the ring is old");
        r.seq = 22; r.display_time = 123456;
        check(m.evaluate(r, 30, ahead, true) == DepthMatcher::State::fresh, "matcher: a newer unknown time is still accepted");
        {
            DepthMatcher seeded;
            seeded.seed(22);
            check(seeded.evaluate(r, 1, ahead, true) == DepthMatcher::State::absent, "matcher: a new instance ignores the previous session's last record");
        }
        DepthMatcher::Read torn; torn.ok = false;
        check(m.evaluate(torn, 13, ahead, true) == DepthMatcher::State::stale && m.received() == 10, "matcher: a failed read changes nothing");
    }

    // Without a saccade a different class must hold 300 ms (pursuit); after a saccade's end, 120 ms.
    {
        DeciderRun t;
        check(t.d.state() == Fixation::near_ && t.d.target() == 1.0f, "decider starts near");
        t.depth(summary(0, 1));
        const int pursuit = t.until(Fixation::far_, 90);
        check(pursuit >= 27 && pursuit <= 28, "pursuit: far after 300 ms of agreement", pursuit, 27);
        DeciderRun u;
        u.depth(summary(0, 1));
        u.saccade();
        const int landing = u.until(Fixation::far_, 90);
        check(landing >= 11 && landing <= 12, "landing: far 120 ms after the saccade", landing, 11);
        check(u.d.target() == 1.4f, "enhanced: 80 m is beyond 50 m: the cap", u.d.target(), 1.4);
    }
    // A landing that starts ambiguous counts as near (state far -> near after 120 ms), with the saccade step allowed
    // in the landing window and no move towards the boost meanwhile.
    {
        DeciderRun t;
        t.depth(summary(0, 1)); t.saccade(); t.frames(20);
        t.depth(summary(0.5f, 0.4f));
        t.saccade();
        t.d.update(t.in, t.s, t.cap);
        check(t.d.pending_near() && t.d.landing_step() && t.d.hold_up(), "ambiguous landing: near pending, step allowed");
        const int n = t.until(Fixation::near_, 30);
        check(n >= 10 && n <= 11, "ambiguous landing becomes near after 120 ms", n, 11);
    }
    // Ambiguous without a landing holds the state indefinitely.
    {
        DeciderRun t;
        t.depth(summary(0, 1)); t.saccade(); t.frames(20);
        t.depth(summary(0.5f, 0.4f));
        check(t.until(Fixation::near_, 450) == -1 && t.d.state() == Fixation::far_, "ambiguous holds (5 s)");
    }
    // An edge freezes: a far state on a near cone with a mixed ring stays far for 5 s; leaving the edge onto the near
    // side needs the full confirmation afterwards (the edge reset it).
    {
        DeciderRun t;
        t.depth(summary(0, 1)); t.saccade(); t.frames(20);
        t.depth(summary(0.8f, 0.2f, 0.5f, 0.5f));
        check(t.until(Fixation::near_, 450) == -1 && t.d.edge() && t.d.hold_up(), "edge: no change for 5 s, the scale held");
        t.depth(summary(1, 0, 1, 0));
        const int n = t.until(Fixation::near_, 60);
        check(n >= 27, "off the edge: the pursuit rule (300 ms)", n, 27);
        // Alternating edge and near frames never confirm: each edge frame starts over.
        DeciderRun a;
        a.depth(summary(0, 1)); a.saccade(); a.frames(20);
        for (int i = 0; i < 450; ++i) { a.depth(i % 5 == 4 ? summary(0.8f, 0.2f, 0.5f, 0.5f) : summary(1, 0)); a.frames(1); }
        check(a.d.state() == Fixation::far_, "edge frames interrupt the confirmation", static_cast<int>(a.d.state()), 2);
    }
    // A landing expires after 0.4 s: a landing on an edge, then 120 ms of near is not enough.
    {
        DeciderRun t;
        t.depth(summary(0, 1)); t.saccade(); t.frames(20);
        t.depth(summary(0.5f, 0.5f, 0.5f, 0.5f)); t.saccade(); t.frames(60);
        // While pending the landing's evidence counts; after expiry the pursuit rule: no change within 120 ms.
        t.depth(summary(1, 0)); t.frames(15);
        check(t.d.state() == Fixation::far_ && !t.d.landing_pending(), "expired landing: 120 ms of near is not enough");
        check(t.until(Fixation::near_, 30) > 0, "and 300 ms is");
    }
    // Vergence veto: near (nearer than 1 m) over a far depth makes it near; not at 1.25 m; a far vergence never vetoes
    // a near depth (depth is trusted).
    {
        DeciderRun t;
        t.depth(summary(0, 1)); t.saccade(); t.frames(20);
        t.in.vergence_ok = true; t.in.vergence = Fixation::near_; t.in.vergence_w = 1.0f / 1.25f;
        t.frames(60);
        check(t.d.state() == Fixation::far_ && t.d.vetoes() == 0, "no veto from a vergence at 1.25 m");
        t.in.vergence_w = 1.0f / 0.7f;
        t.d.update(t.in, t.s, t.cap);
        check(t.d.veto(), "veto at 0.7 m");
        check(t.until(Fixation::near_, 40) > 0 && t.d.vetoes() > 0, "the veto makes it near");
        DeciderRun u;
        u.depth(summary(1, 0)); u.in.vergence_ok = true; u.in.vergence = Fixation::far_; u.in.vergence_w = 0;
        u.frames(90);
        check(u.d.state() == Fixation::near_ && u.d.depth_over_vergence() > 0 && !u.d.veto(), "depth near is trusted over a far vergence");
    }
    // A landing on a far depth waits while the proven vergence has not settled; a near landing does not.
    {
        DeciderRun t;
        t.depth(summary(0, 1)); t.saccade();
        t.in.vergence_pending = true;
        check(t.until(Fixation::far_, 20) == -1, "far landing waits for the vergence");
        t.in.vergence_pending = false;
        check(t.until(Fixation::far_, 15) > 0, "then confirms");
        t.depth(summary(1, 0)); t.saccade(); t.in.vergence_pending = true;
        check(t.until(Fixation::near_, 15) > 0, "a near landing does not wait");
    }
    // A frame without a summary between scene-depth frames: a lagging far vergence does not re-arm a near landing.
    {
        DeciderRun t;
        t.depth(summary(0, 1)); t.saccade(); t.frames(20);
        t.depth(summary(1, 0)); t.saccade(); t.frames(5);
        t.in.depth_fresh = false; t.in.vergence_ok = true; t.in.vergence = Fixation::far_; t.in.vergence_w = 0;
        t.frames(1);
        check(t.d.candidate() == Fixation::near_ && t.d.agree_s() > 0.04f, "a vergence gap frame pauses, does not re-arm", t.d.agree_s(), 0.05);
        t.depth(summary(1, 0));
        check(t.until(Fixation::near_, 10) > 0, "the near landing completes");
    }
    // No source, or no gaze, for HoldMs: back to near. The end of a gap arms a landing (120 ms, not 300).
    {
        DeciderRun t;
        t.depth(summary(0, 1)); t.saccade(); t.frames(20);
        t.in.depth_fresh = false;
        t.frames(44);
        check(t.d.state() == Fixation::far_, "no source: held for HoldMs");
        t.frames(2);
        check(t.d.state() == Fixation::near_, "no source: near after HoldMs");
        DeciderRun g;
        g.depth(summary(0, 1)); g.saccade(); g.frames(20);
        g.in.gaze_valid = false; g.frames(46);
        check(g.d.state() == Fixation::near_, "no gaze for HoldMs: near");
        DeciderRun b;
        b.depth(summary(0, 1)); b.saccade(); b.frames(20);
        b.in.gaze_valid = false; b.frames(14); b.in.gaze_valid = true;
        b.depth(summary(1, 0));
        const int n = b.until(Fixation::near_, 30);
        check(n >= 11 && n <= 12 && !b.d.landing_window(), "after a blink: a landing (120 ms), no saccade step", n, 11);
    }
    // The far target: the median far distance through the curve, moved only by 0.02 or more. Realistic: 80 m stays 1,
    // 500 m the cap. Without the scene depth: Enhanced the cap on vergence, Realistic 1 (after 2 s of memory).
    {
        DeciderRun t;
        t.s.mode = DepthMode::realistic; t.s.far_start_m = 150; t.s.far_full_m = 400; t.cap = 1.25f;
        t.depth(summary(0, 1, 0, 0, 80)); t.saccade(); t.frames(30);
        check(t.d.state() == Fixation::far_ && t.d.target() == 1.0f, "realistic: a far wingman at 80 m, target exactly 1");
        t.depth(summary(0, 1, 0, 0, 500)); t.saccade(); t.frames(30);
        check(t.d.target() == 1.25f, "realistic: 500 m, the cap", t.d.target(), 1.25);
        t.depth(summary(0, 1, 0, 0, 250)); t.saccade(); t.frames(30);
        const float at250 = t.d.target();
        t.depth(summary(0, 1, 0, 0, 255)); t.frames(30);
        check(t.d.target() == at250 && at250 > 1.0f && at250 < 1.25f, "deadband: 250 m -> 255 m does not move the target", t.d.target(), at250);
        // Depth gone, vergence far: the target is held for 2 s, then Realistic returns to 1.
        t.in.depth_fresh = false; t.in.vergence_ok = true; t.in.vergence = Fixation::far_; t.in.vergence_w = 0;
        t.frames(170);
        check(t.d.target() == at250, "depth memory: target held", t.d.target(), at250);
        t.frames(20);
        check(t.d.target() == 1.0f && t.d.state() == Fixation::far_, "realistic on vergence alone: 1", t.d.target(), 1);
        DeciderRun e;
        e.in.vergence_ok = true; e.in.vergence = Fixation::far_; e.in.vergence_w = 0; e.saccade(); e.frames(20);
        check(e.d.state() == Fixation::far_ && e.d.target() == 1.4f && e.d.source() == FixSource::vergence, "enhanced on vergence alone: the cap (v1)");
        // The median ignores a single outlier.
        DeciderRun m;
        m.depth(summary(0, 1, 0, 0, 80)); m.saccade(); m.frames(20);
        m.depth(summary(0, 1, 0, 0, 2.0f)); m.frames(1);
        check(m.d.far_distance_m() == 80.0f, "far median ignores one outlier", m.d.far_distance_m(), 80);
    }
    // The controller with the decider's inputs: a lower far target moves at UpMmPerS (far_rates); the landing step is
    // spent at once, once, only in the window; a gap clears it.
    {
        const DepthBoostSettings s;
        DepthBoostController c;
        DepthBoostController::Input in;
        in.active = in.gaze_valid = true; in.base_ipd_m = 0.064f; in.dt = 1.0f / 90; in.target = 1.4f;
        for (int i = 0; i < 600; ++i) c.step(in, s);
        check(c.scale() == 1.4f, "target reached", c.scale(), 1.4);
        in.target = 1.2f; in.far_rates = true;
        float last = c.scale();
        c.step(in, s);
        check(std::fabs((last - c.scale()) * 64.0f - s.up_mm_per_s / 90) < 1e-3f, "far state: a lower target at UpMmPerS", (last - c.scale()) * 64.0f, s.up_mm_per_s / 90);
        in.far_rates = false; in.target = 1.4f;
        for (int i = 0; i < 600; ++i) c.step(in, s);
        in.saccade = in.saccade_started = true; c.step(in, s);
        in.saccade = in.saccade_started = false; in.landing_window = true; in.landing_step = true; in.settled = false;
        last = c.scale();
        c.step(in, s);
        check(std::fabs((last - c.scale()) * 64.0f - s.saccade_step_mm) < 1e-3f, "landing step: SaccadeStepMm at once", (last - c.scale()) * 64.0f, s.saccade_step_mm);
        last = c.scale();
        c.step(in, s);
        check(c.scale() == last, "the step is spent once; no move up while confirming", c.scale(), last);
        // A gap clears the budget.
        DepthBoostController g;
        DepthBoostController::Input gi = in;
        gi.landing_step = false; gi.settled = true; gi.landing_window = false;
        for (int i = 0; i < 600; ++i) g.step(gi, s);
        gi.saccade = gi.saccade_started = true; g.step(gi, s);
        gi.saccade = gi.saccade_started = false; gi.gaze_valid = false; g.step(gi, s);
        gi.gaze_valid = true; gi.landing_window = gi.landing_step = true; gi.settled = false;
        last = g.scale();
        g.step(gi, s);
        check(g.scale() == last, "no saccade step after a gap", g.scale(), last);
    }
    // Status bits of the fixation depth.
    {
        const std::uint64_t w = depth_status_bits(true, true, true, DepthSource::vergence, Fixation::far_, 1.2f, 0.01f, FixSource::depth, true, true);
        check(((w >> status::fix_source) & 3) == 2 && (w & status::depth_edge) && (w & status::depth_veto) && (w & ~depth_status_mask) == 0,
            "status: source, edge, veto inside the Depth boost mask");
    }
    // Pimax PVR: both eyes must be valid; time 0 is invalid; a repeated time is valid but not fresh; a time that stops
    // advancing for 200 ms is invalid. Head-frame tangents to world.
    {
        pvr::GazeFilter f;
        pvr::EyeTrackingInfo info{};
        info.GazeTan[0] = {0.1f, 0.0f}; info.GazeTan[1] = {0.05f, 0.0f}; info.TimeInSeconds = 0;
        check(!f.update(true, info, 1000).valid, "pvr: time 0 is invalid");
        info.TimeInSeconds = 5.0;
        pvr::Sample s = f.update(true, info, 1000);
        check(s.valid && s.fresh && std::fabs(s.head[0].x / -s.head[0].z - 0.1f) < 1e-6f, "pvr: valid, fresh, tangents kept");
        s = f.update(true, info, 1011);
        check(s.valid && !s.fresh, "pvr: the same time again is not fresh");
        check(!f.update(true, info, 1250).valid, "pvr: stalled 250 ms is invalid");
        info.TimeInSeconds = 5.3; info.GazeTan[1] = {std::nanf(""), 0};
        s = f.update(true, info, 1260);
        check(!s.valid && s.eye_ok[0] && !s.eye_ok[1], "pvr: one bad eye invalidates the sample");
        info.GazeTan[1] = {20.0f, 0};
        check(!f.update(true, info, 1270).valid, "pvr: an eye beyond 84 degrees is invalid");
        check(!f.update(false, info, 1280).queried, "pvr: not queried");
        info.GazeTan[1] = {0.05f, 0}; info.TimeInSeconds = 5.4; info.ConvergenceDistance = 0.8f;
        check(f.update(true, info, 1290).convergence_m == 0.8f, "pvr: ConvergenceDistance carried for the log");
        // An eye reported closed, or both eyes at exactly (0, 0) (the tracker lost them), is no gaze.
        info.TimeInSeconds = 5.5; info.blink[1] = 1.0f;
        s = f.update(true, info, 1300);
        check(!s.valid && s.eye_ok[0] && !s.eye_ok[1], "pvr: a blinking eye invalidates the sample");
        info.TimeInSeconds = 5.6; info.blink[1] = 0.0f; info.GazeTan[0] = info.GazeTan[1] = {0.0f, 0.0f};
        check(!f.update(true, info, 1310).valid, "pvr: both eyes at exactly (0, 0) is no gaze");
        info.TimeInSeconds = 5.7; info.GazeTan[0] = {0.1f, 0.0f}; info.GazeTan[1] = {0.05f, 0.0f};
        check(f.update(true, info, 1320).valid, "pvr: valid again with the eyes open");
        const HeadBasis b = head_basis({-0.032f, 0, 0}, {0.032f, 0, 0}, {0, 0, -1});
        const Vec3 wv = pvr::head_to_world(b, normalized({0.1f, 0.05f, -1}));
        check(std::fabs(wv.x / -wv.z - 0.1f) < 1e-5f && std::fabs(wv.y / -wv.z - 0.05f) < 1e-5f, "pvr: head frame to world");
        // Vergence from PVR tangents: both eyes towards a point 0.7 m ahead.
        const float ipd = 0.064f;
        const Vec3 gl = pvr::head_to_world(b, normalized({ipd / 2 / 0.7f, 0, -1})), gr = pvr::head_to_world(b, normalized({-ipd / 2 / 0.7f, 0, -1}));
        const float iw = inverse_fixation_distance({-ipd / 2, 0, 0}, {ipd / 2, 0, 0}, gl, gr, b);
        check(std::fabs(1.0f / iw - 0.7f) < 1e-3f, "pvr: vergence from per-eye tangents", 1.0f / iw, 0.7);
    }
    // The shared-memory blocks: the seqlock round-trips, a block never written or being written reads as nothing.
    {
        dcsvr_perception::DepthIn block{}, out{}, value{};
        check(!dcsvr_perception::seqlock_read(&block, out), "seqlock: never written");
        value.displayTime = 42; value.flags = dcsvr_perception::depth_flags::valid; value.nearShare = 0.25f; value.histogram[3] = 7;
        dcsvr_perception::seqlock_write(&block, value);
        check(dcsvr_perception::seqlock_read(&block, out) && out.seq == 2 && out.displayTime == 42 && out.nearShare == 0.25f && out.histogram[3] == 7,
            "seqlock: round trip");
        dcsvr_perception::seqlock_write(&block, value);
        check(dcsvr_perception::seqlock_read(&block, out) && out.seq == 4, "seqlock: the sequence advances by 2");
        block.seq = 5;
        check(!dcsvr_perception::seqlock_read(&block, out), "seqlock: odd (being written) is not read");
        dcsvr_perception::seqlock_write(&block, value);
        check(block.seq == 6, "seqlock: a writer that died mid-write is repaired", static_cast<double>(block.seq), 6);
    }
}

// ---- The predictive transition (predictive_depth.hpp) ------------------------------------------------------------------
namespace pd = pupil_shift::predictive;

// A synthetic high-rate trace: a fixation, then a saccade of `amplitude` degrees in direction `dir_deg` (0 = right, 90 =
// up) from `from_yaw`/`from_pitch`, whose peak velocity is the main sequence's times `peak_scale` (smoothstep profile,
// so its duration is 1.5 * A / Vp), then a fixation. Per-sample white noise (degrees per axis, per eye). Both eyes look
// at a point 50 m away. Returns the samples; the true onset, end and landing direction (head frame, cyclopean).
struct Trace { std::vector<pd::GazeSample> samples; double onset{}, end{}; Vec3 landing{}; float duration{}; };
static Vec3 dir_of(float yaw, float pitch) { return normalized({tan_deg(yaw), tan_deg(pitch), -1}); }
static Trace saccade_trace(float hz, float amplitude, float dir_deg, float from_yaw, float from_pitch, float noise_deg, Random& rng,
    float peak_scale = 1.0f, double before = 0.3, double after = 0.3) {
    Trace tr;
    const float vp = pd::main_sequence::peak_velocity(amplitude) * peak_scale;
    tr.duration = 1.5f * amplitude / vp;
    tr.onset = 1.0 + before; tr.end = tr.onset + tr.duration;
    const float to_yaw = from_yaw + amplitude * std::cos(dir_deg / 57.29578f), to_pitch = from_pitch + amplitude * std::sin(dir_deg / 57.29578f);
    tr.landing = dir_of(to_yaw, to_pitch);
    const double total = before + tr.duration + after;
    for (double t = 0; t < total; t += 1.0 / hz) {
        const double tt = 1.0 + t;
        float k = static_cast<float>((tt - tr.onset) / tr.duration);
        k = std::clamp(k, 0.0f, 1.0f);
        const float s = k * k * (3 - 2 * k);
        // Along the great circle from the start to the landing.
        const Vec3 a = dir_of(from_yaw, from_pitch), b = tr.landing;
        const float angle = std::acos(std::clamp(dot(a, b), -1.0f, 1.0f));
        const Vec3 axis = normalized(cross(a, b));
        const Vec3 c = pd::rotate_about(a, axis, angle * s);
        pd::GazeSample g; g.t = tt; g.valid = true;
        const Vec3 point = c * 50.0f;
        for (int e = 0; e < 2; ++e) {
            const Vec3 eye{e == 0 ? -0.032f : 0.032f, 0, 0};
            Vec3 d = normalized(point - eye);
            if (noise_deg > 0) d = normalized(d + Vec3{rng.gauss() * noise_deg / 57.29578f, rng.gauss() * noise_deg / 57.29578f, 0});
            g.eye[e] = d;
        }
        tr.samples.push_back(g);
    }
    return tr;
}
struct Run { unsigned started{}, predicted{}, landed{}, dropped{}; pd::Prediction p{}; double decided{-1}; };
static Run run_detector(pd::SaccadeDetector& d, const std::vector<pd::GazeSample>& samples, const pd::Settings& set = {}) {
    Run r;
    for (const auto& g : samples) {
        const unsigned e = d.push(g, set);
        if (e & pd::SaccadeDetector::started) ++r.started;
        if (e & pd::SaccadeDetector::predicted) { ++r.predicted; r.p = d.prediction(); r.decided = g.t; }
        if (e & pd::SaccadeDetector::landed) ++r.landed;
        if (e & pd::SaccadeDetector::dropped) ++r.dropped;
    }
    return r;
}
static float landing_error_deg(const Run& r, const Trace& t) { return pd::angle_deg(normalized(r.p.landing[0] + r.p.landing[1]), t.landing); }

// A 32 x 32 grid over tangents -1..1 (left, right), 1..-1 (up, down): what classify_grid reads.
static dcsvr_perception::DepthGrid make_grid(float (*scene)(float tx, float ty), bool log_mean = false) {
    dcsvr_perception::DepthGrid g{};
    g.flags = dcsvr_perception::grid_flags::valid; g.w = g.h = 32;
    g.fovTan[0] = -1; g.fovTan[1] = 1; g.fovTan[2] = 1; g.fovTan[3] = -1;
    for (int row = 0; row < 32; ++row)
        for (int col = 0; col < 32; ++col) {
            float nearest = 1e9f, sum_log = 0; int near_n = 0; float v[16]; int n = 0;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) {
                    const float tx = -1 + (col + (i + 0.5f) / 4) * 2.0f / 32, ty = 1 - (row + (j + 0.5f) / 4) * 2.0f / 32;
                    const float m = scene(tx, ty);
                    v[n++] = m; nearest = std::min(nearest, m); sum_log += std::log(m); if (m < 1.5f) ++near_n;
                }
            std::sort(v, v + n);
            const int k = row * 32 + col;
            g.nearestM[k] = nearest; g.typicalM[k] = log_mean ? std::exp(sum_log / n) : v[n / 2];
            g.nearShare[k] = static_cast<std::uint8_t>(std::lround(255.0f * near_n / n));
        }
    return g;
}
// Sky above ty = -0.3, the panel (0.7 m) below; a canopy strut (0.6 m) at tx 0.4..0.5 above the panel; a 2 m object
// (the dead band) at tx -0.8..-0.6, ty 0.4..0.6.
static float grid_scene(float tx, float ty) {
    if (ty < -0.3f) return 0.7f;
    if (tx >= 0.4f && tx <= 0.5f) return 0.6f;
    if (tx >= -0.8f && tx <= -0.6f && ty >= 0.4f && ty <= 0.6f) return 2.0f;
    return 20000.0f;
}

static void predictive_tests() {
    namespace ms = pd::main_sequence;
    // The main sequence and its inverse.
    for (float a : {2.0f, 5.0f, 10.0f, 20.0f, 30.0f})
        check(std::fabs(ms::amplitude(ms::peak_velocity(a)) - a) < 0.01f * a + 0.01f, "main sequence: amplitude(peak_velocity(A)) = A", ms::amplitude(ms::peak_velocity(a)), a);
    check(std::fabs(ms::peak_velocity(20) - 600 * (1 - std::exp(-20.0f / 15))) < 1e-3f && ms::peak_velocity(20) > 440 && ms::peak_velocity(20) < 445, "main sequence: 20 deg peaks at about 442 deg/s", ms::peak_velocity(20), 442);
    check(ms::amplitude(600) == ms::max_amplitude_deg && ms::amplitude(0) == 0, "main sequence: saturated and zero", ms::amplitude(600), 45);
    check(std::fabs(ms::duration_s(20) - 0.065f) < 1e-6f, "Carpenter's duration: 21 + 2.2 ms/deg", ms::duration_s(20), 0.065);
    Random rng;
    // A clean 20 degree saccade at 500 Hz: one onset, one prediction before the landing, amplitude and landing right.
    {
        pd::SaccadeDetector d;
        const Trace t = saccade_trace(500, 20, 90, 0, -18, 0, rng);
        const Run r = run_detector(d, t.samples);
        check(r.started == 1 && r.predicted == 1 && r.landed == 1 && r.dropped == 0, "20 deg: one saccade, predicted, landed", r.started * 100 + r.predicted * 10 + r.landed, 111);
        check(std::fabs(r.p.amplitude_deg - 20) < 2.0f, "20 deg: amplitude from the peak velocity", r.p.amplitude_deg, 20);
        check(landing_error_deg(r, t) < 2.0f, "20 deg: landing direction within 2 deg", landing_error_deg(r, t), 2);
        check(r.decided > t.onset && r.decided < t.end - 0.015, "20 deg: decided at least 15 ms before the eyes land", (r.decided - t.onset) * 1000, t.duration * 1000);
        check(std::fabs(r.p.onset_t - t.onset) < 0.008, "20 deg: onset time within 8 ms", (r.p.onset_t - t.onset) * 1000, 0);
        check(std::fabs(r.p.end_t - t.end) < 0.012, "20 deg: predicted end within 12 ms", (r.p.end_t - t.end) * 1000, 0);
        std::printf("predictive: 20 deg clean: amplitude %.2f deg, landing error %.2f deg, decided %.1f ms after onset of %.1f ms, end error %+.1f ms\n",
            r.p.amplitude_deg, landing_error_deg(r, t), (r.decided - t.onset) * 1000, t.duration * 1000, (r.p.end_t - t.end) * 1000);
    }
    // Tracker noise: 0.15 deg per sample and axis at 500 Hz never starts a saccade in 5 s of fixation; a 20 degree
    // saccade in that noise is still predicted within 15 % and 3 degrees.
    {
        pd::SaccadeDetector d;
        Trace fix = saccade_trace(500, 20, 0, 0, 0, 0.15f, rng, 1.0f, 5.0, 0.0);
        fix.samples.resize(static_cast<std::size_t>(5.0 * 500));
        const Run r = run_detector(d, fix.samples);
        check(r.started == 0, "noise 0.15 deg at 500 Hz: no false saccade in 5 s", r.started, 0);
        int good = 0, runs = 0; float worst = 0, worst_amp = 0;
        for (int k = 0; k < 20; ++k) {
            pd::SaccadeDetector dn;
            const Trace t = saccade_trace(500, 20, 15.0f * k, 0, 0, 0.15f, rng);
            const Run rn = run_detector(dn, t.samples);
            ++runs;
            if (rn.predicted == 1) {
                worst = std::max(worst, landing_error_deg(rn, t)); worst_amp = std::max(worst_amp, std::fabs(rn.p.amplitude_deg - 20));
                if (landing_error_deg(rn, t) < 3.0f && std::fabs(rn.p.amplitude_deg - 20) < 3.0f) ++good;
            }
        }
        check(good >= 18, "noise 0.15 deg: 20 deg saccades predicted within 3 deg (18 of 20)", good, runs);
        std::printf("predictive: noise 0.15 deg: %d/%d within 3 deg, worst landing error %.2f deg, worst amplitude error %.2f deg\n", good, runs, worst, worst_amp);
    }
    // Tracker noise of 0.5 deg at 120 Hz starts short false flights; none of them travels 3 degrees or is predicted, so
    // none reaches the fixation filters as a saccade.
    {
        pd::SaccadeDetector d;
        Trace fix = saccade_trace(120, 20, 0, 0, 0, 0.5f, rng, 1.0f, 30.0, 0.0);
        fix.samples.resize(static_cast<std::size_t>(30.0 * 120));
        const pd::Settings set;
        unsigned flights = 0, hints = 0;
        bool was = false;
        for (const auto& g : fix.samples) {
            d.push(g, set);
            if (d.in_flight() && !was) ++flights;
            was = d.in_flight();
            if (d.saccade_hint()) ++hints;
        }
        std::printf("predictive: noise 0.5 deg at 120 Hz, 30 s: %u false flights, %u samples with a saccade hint\n", flights, hints);
        check(hints == 0, "noise 0.5 deg at 120 Hz: no saccade hint for the fixation filters", hints, flights);
        pd::SaccadeDetector real;
        const Trace t = saccade_trace(120, 20, 0, -10, 0, 0.1f, rng);
        bool hinted = false;
        for (const auto& g : t.samples) { real.push(g, set); hinted = hinted || real.saccade_hint(); }
        check(hinted, "a real 20 deg saccade at 120 Hz gives the hint", 0, 0);
    }
    // A 120 Hz tracker: still one saccade, predicted.
    {
        pd::SaccadeDetector d;
        const Trace t = saccade_trace(120, 20, 0, -10, 0, 0.1f, rng);
        const Run r = run_detector(d, t.samples);
        check(r.started == 1 && r.predicted == 1, "120 Hz: the 20 deg saccade is seen and predicted", r.started, r.predicted);
        std::printf("predictive: 120 Hz: amplitude %.2f deg, landing error %.2f deg, decided %.1f ms after onset of %.1f ms\n", r.p.amplitude_deg,
            landing_error_deg(r, t), (r.decided - t.onset) * 1000, t.duration * 1000);
    }
    // Timing at the tracker's real rate: the predicted end (onset + the main sequence's duration) against the true one,
    // 10-30 degree saccades at 120 and 500 Hz. A late predicted end lets the gate step with no flight left.
    for (const float hz : {120.0f, 500.0f}) {
        float worst_late = -1e9f, worst_early = 1e9f;
        int predicted = 0;
        for (const float a : {10.0f, 15.0f, 20.0f, 25.0f, 30.0f}) {
            for (int phase = 0; phase < 4; ++phase) {
                pd::SaccadeDetector d;
                Trace t = saccade_trace(hz, a, 30.0f * phase, -12, 0, 0.0f, rng, 1.0f, 0.3 + phase * 0.25 / hz, 0.3);
                const Run r = run_detector(d, t.samples);
                if (r.predicted != 1) continue;
                ++predicted;
                const float err = static_cast<float>((r.p.end_t - t.end) * 1000);
                worst_late = std::max(worst_late, err); worst_early = std::min(worst_early, err);
            }
        }
        std::printf("predictive: %.0f Hz predicted end error %+.1f to %+.1f ms (10-30 deg)\n", hz, worst_early, worst_late);
        check(predicted >= 18, "predicted end: 18 of the 20 saccades predicted", predicted, 20);
        check(worst_late < 6.0f && worst_early > -10.0f, "predicted end: at most 6 ms late, 10 ms early, at the tracker's rate", worst_late, worst_early);
    }
    // A 4 degree saccade is predicted short (under PredictMinDeg).
    {
        pd::SaccadeDetector d;
        const Trace t = saccade_trace(500, 4, 90, 0, -14, 0.1f, rng);
        const Run r = run_detector(d, t.samples);
        check(r.predicted <= 1 && (r.predicted == 0 || r.p.amplitude_deg < 8.0f), "4 deg: predicted under 8 deg", r.p.amplitude_deg, 8);
    }
    // A saccade faster than the main sequence (x1.3 peak) predicts too far, bounded by three times the path at the peak.
    {
        pd::SaccadeDetector d;
        const Trace t = saccade_trace(500, 18, 90, 0, -34, 0, rng, 1.3f);
        const Run r = run_detector(d, t.samples);
        check(r.predicted == 1 && r.p.amplitude_deg > 22 && r.p.amplitude_deg <= 3.0f * r.p.travelled_deg + 1e-3f, "fast saccade: over-predicted, bounded by the path", r.p.amplitude_deg, 3 * r.p.travelled_deg);
    }
    // The online gain: a tracker whose filtering lowers every peak by 15 % under-predicts; the gain learns it.
    {
        pd::SaccadeDetector d;
        float first_error = 0, last_error = 0;
        for (int k = 0; k < 16; ++k) {
            const Trace t = saccade_trace(500, 20, 90.0f + 180.0f * (k % 2), 0, k % 2 ? 10.0f : -10.0f, 0, rng, 0.85f);
            Trace shifted = t;
            for (auto& g : shifted.samples) g.t += 2.0 * k;
            const Run r = run_detector(d, shifted.samples);
            const float err = std::fabs(r.p.amplitude_deg - 20);
            if (k == 0) first_error = err;
            last_error = err;
        }
        check(d.gain() > 1.1f && last_error < first_error && last_error < 1.5f, "online gain learns a 15 % low peak", d.gain(), last_error);
        std::printf("predictive: online gain %.3f, amplitude error first %.2f deg, after 16 saccades %.2f deg\n", d.gain(), first_error, last_error);
    }
    // A blink in flight drops the saccade (no landing, no gain update).
    {
        pd::SaccadeDetector d;
        Trace t = saccade_trace(500, 20, 0, 0, 0, 0, rng);
        for (auto& g : t.samples) if (g.t > t.onset + 0.03 && g.t < t.onset + 0.05) g.valid = false;
        const Run r = run_detector(d, t.samples);
        check(r.dropped >= 1 && d.gain() == 1.0f, "a blink in flight drops the saccade", r.dropped, d.gain());
    }
    // The grid: sky far, panel near, the strut's edge, the dead band, outside, invalid; the log mean reads the same.
    for (bool log_mean : {false, true}) {
        const auto g = make_grid(grid_scene, log_mean);
        const auto at = [&](float tx, float ty) { return pd::classify_grid(g, normalized({tx, ty, -1})); };
        const auto sky = at(0.0f, 0.5f), panel = at(0.0f, -0.7f), strut = at(0.45f, 0.2f), band = at(-0.7f, 0.5f);
        check(sky.status == pd::GridLanding::Status::ok && sky.cls == DepthClass::far_ && sky.distance_m > 1000, "grid: sky is far", sky.distance_m, log_mean);
        check(panel.status == pd::GridLanding::Status::ok && panel.cls == DepthClass::near_ && panel.distance_m < 0.75f, "grid: panel is near", panel.distance_m, log_mean);
        check(strut.status == pd::GridLanding::Status::edge, "grid: the strut is an edge", static_cast<int>(strut.status), log_mean);
        check(band.status == pd::GridLanding::Status::ambiguous || band.status == pd::GridLanding::Status::edge, "grid: 2 m is the dead band", static_cast<int>(band.status), log_mean);
        check(at(0.0f, -0.3f).status == pd::GridLanding::Status::edge, "grid: the panel's top is an edge", static_cast<int>(at(0.0f, -0.3f).status), log_mean);
        check(at(1.5f, 0.0f).status == pd::GridLanding::Status::outside && pd::classify_grid(g, {0, 0, 1}).status == pd::GridLanding::Status::outside, "grid: outside", 0, 0);
        auto bad = g; bad.flags = 0;
        check(pd::classify_grid(bad, {0, 0, -1}).status == pd::GridLanding::Status::no_grid, "grid: not valid", 0, 0);
        bad = g; bad.w = 40;
        check(pd::classify_grid(bad, {0, 0, -1}).status == pd::GridLanding::Status::no_grid, "grid: nonsense size", 0, 0);
    }
    // The live grid's asymmetric FOV (view 0 of a Pimax: left -1.53, right 1.03, up 1.27, down -1.26) and an empty cell
    // (typical distance 0: not valid).
    {
        dcsvr_perception::DepthGrid g{};
        g.flags = dcsvr_perception::grid_flags::valid; g.w = g.h = 32;
        g.fovTan[0] = -1.53f; g.fovTan[1] = 1.03f; g.fovTan[2] = 1.27f; g.fovTan[3] = -1.26f;
        for (int k = 0; k < 1024; ++k) { g.nearestM[k] = g.typicalM[k] = 20000; g.nearShare[k] = 0; }
        const auto r = pd::classify_grid(g, normalized({-1.2f, 1.0f, -1}));
        check(r.status == pd::GridLanding::Status::ok && r.col == 4 && r.row == 3, "grid: asymmetric FOV, cell (4, 3)", r.col * 100 + r.row, 403);
        g.typicalM[3 * 32 + 4] = 0;
        check(pd::classify_grid(g, normalized({-1.2f, 1.0f, -1})).status == pd::GridLanding::Status::ambiguous, "grid: an empty cell is no evidence", 0, 0);
        // The ring's shares over its valid cells only: 4 empty, 2 near and 2 far neighbours are an edge (50 / 50).
        g.typicalM[3 * 32 + 4] = 20000;
        const int ring_cells[8][2] = {{3, 2}, {4, 2}, {5, 2}, {3, 3}, {5, 3}, {3, 4}, {4, 4}, {5, 4}};
        for (int k = 0; k < 8; ++k) {
            const int i = ring_cells[k][1] * 32 + ring_cells[k][0];
            if (k < 4) g.typicalM[i] = 0;
            else if (k < 6) { g.nearestM[i] = g.typicalM[i] = 0.7f; g.nearShare[i] = 255; }
        }
        check(pd::classify_grid(g, normalized({-1.2f, 1.0f, -1})).status == pd::GridLanding::Status::edge, "grid: empty neighbours do not dilute an edge", 0, 0);
    }
    // Step sizes: up to the sky capped at 12 mm; to the panel at 0.7 m by 0.4 deg of disparity (4.89 mm); none needed.
    {
        const pd::Settings set;
        check(std::fabs(pd::step_mm(1.0f, 1.25f, 64, 20000, set) - 12.0f) < 1e-4f, "step: the absolute cap", pd::step_mm(1.0f, 1.25f, 64, 20000, set), 12);
        check(std::fabs(pd::step_mm(1.25f, 1.0f, 64, 0.7f, set) + 0.4f / 57.29578f * 700) < 1e-3f, "step: the disparity cap at 0.7 m", pd::step_mm(1.25f, 1.0f, 64, 0.7f, set), -4.887);
        check(std::fabs(pd::step_mm(1.1f, 1.12f, 64, 500, set) - 1.28f) < 1e-3f, "step: the rest when it is small", pd::step_mm(1.1f, 1.12f, 64, 500, set), 1.28);
    }
    // The gate: a step, its hold, confirmation; contradiction without a second step; timing; the skips.
    {
        DepthBoostSettings s; s.mode = DepthMode::realistic; s.far_start_m = 150; s.far_full_m = 400; s.max_scale = 1.25f;
        const pd::Settings set;
        pd::Prediction p; p.id = 1; p.amplitude_deg = 20; p.onset_t = 10.0; p.end_t = 10.065;
        pd::GridLanding far_; far_.status = pd::GridLanding::Status::ok; far_.cls = DepthClass::far_; far_.distance_m = 20000;
        const auto input = [&](double now, bool fresh, Fixation state, DepthClass frame, unsigned landed, float scale) {
            pd::PredictiveGate::Input in;
            in.now_s = now; in.frame_s = 1.0f / 45; in.active = true; in.new_prediction = fresh; in.prediction = p; in.landing = far_;
            in.landed_id = landed; in.scale = scale; in.base_mm = 64; in.cap = 1.25f; in.decider_state = state; in.frame_class = frame;
            return in;
        };
        pd::PredictiveGate g;
        auto o = g.update(input(10.035, true, Fixation::near_, DepthClass::none, 0, 1.0f), s, set);
        check(std::fabs(o.jump_mm - 12.0f) < 1e-4f && o.hold == 1 && o.no_saccade_step, "gate: a 12 mm step with 30 ms left, held", o.jump_mm, o.hold);
        o = g.update(input(10.06, false, Fixation::near_, DepthClass::near_, 0, 1.1875f), s, set);
        check(o.jump_mm == 0 && o.hold == 1, "gate: a near frame before the landing is no contradiction", o.jump_mm, o.hold);
        o = g.update(input(10.2, false, Fixation::far_, DepthClass::far_, 1, 1.1875f), s, set);
        check(o.hold == 0 && g.stats().confirmed == 1 && o.no_saccade_step, "gate: confirmed by the decider's far state", g.stats().confirmed, o.hold);
        // Contradiction.
        pd::PredictiveGate c;
        c.update(input(10.035, true, Fixation::near_, DepthClass::none, 0, 1.0f), s, set);
        o = c.update(input(10.1, false, Fixation::near_, DepthClass::near_, 1, 1.1875f), s, set);
        check(o.hold == 0 && o.jump_mm == 0 && c.stats().contradicted == 1 && o.no_saccade_step, "gate: contradicted by a near frame after the landing, no step back", c.stats().contradicted, o.jump_mm);
        // Expiry on an ambiguous landing.
        pd::PredictiveGate e;
        e.update(input(10.035, true, Fixation::near_, DepthClass::none, 0, 1.0f), s, set);
        o = e.update(input(10.5, false, Fixation::near_, DepthClass::ambiguous, 1, 1.1875f), s, set);
        check(o.hold == 1, "gate: an ambiguous landing holds", o.hold, 1);
        o = e.update(input(10.7, false, Fixation::near_, DepthClass::ambiguous, 1, 1.1875f), s, set);
        check(o.hold == 0 && e.stats().expired == 1, "gate: and expires after hold_s", e.stats().expired, 1);
        // Timing: 15 ms left at 45 fps (22 ms per frame) is late; at 90 fps it is not.
        pd::PredictiveGate t;
        pd::Settings one = set; one.lead_frames = 1.0f;
        o = t.update(input(10.05, true, Fixation::near_, DepthClass::none, 0, 1.0f), s, one);
        check(o.jump_mm == 0 && t.stats().late == 1, "gate: lead 1 frame: 15 ms at 45 fps is late", o.jump_mm, t.stats().late);
        auto fast = input(10.05, true, Fixation::near_, DepthClass::none, 0, 1.0f); fast.frame_s = 1.0f / 90;
        o = t.update(fast, s, one);
        check(o.jump_mm > 11.9f, "gate: at 90 fps 15 ms is enough", o.jump_mm, 12);
        pd::PredictiveGate h;
        o = h.update(input(10.05, true, Fixation::near_, DepthClass::none, 0, 1.0f), s, set);
        check(o.jump_mm > 11.9f, "gate: the default lead (half a frame): 15 ms at 45 fps is enough", o.jump_mm, 12);
        // Skips: short, no grid, edge; and nothing to do (already at the target).
        pd::PredictiveGate k;
        auto in = input(10.0, true, Fixation::near_, DepthClass::none, 0, 1.0f);
        in.prediction.amplitude_deg = 5; k.update(in, s, set);
        in = input(10.0, true, Fixation::near_, DepthClass::none, 0, 1.0f); in.landing.status = pd::GridLanding::Status::no_grid; k.update(in, s, set);
        in = input(10.0, true, Fixation::near_, DepthClass::none, 0, 1.0f); in.landing.status = pd::GridLanding::Status::edge; k.update(in, s, set);
        in = input(10.0, true, Fixation::far_, DepthClass::none, 0, 1.25f); k.update(in, s, set);
        check(k.stats().short_ == 1 && k.stats().no_grid == 1 && k.stats().edge == 1 && k.stats().same == 1 && k.stats().steps == 0, "gate: short, no grid, edge, same",
            k.stats().short_ * 1000 + k.stats().no_grid * 100 + k.stats().edge * 10 + k.stats().same, 1111);
        // Inactive: nothing, and the hold is gone.
        pd::PredictiveGate z;
        z.update(input(10.035, true, Fixation::near_, DepthClass::none, 0, 1.0f), s, set);
        auto off = input(10.04, false, Fixation::near_, DepthClass::none, 0, 1.1875f); off.active = false;
        check(z.update(off, s, set).hold == 0 && z.hold() == 0, "gate: off releases", z.hold(), 0);
    }
    // The controller with a step: applied at once, held against its direction, then the normal rates on release.
    {
        DepthBoostSettings s; s.mode = DepthMode::realistic; s.far_start_m = 150; s.far_full_m = 400; s.max_scale = 1.25f;
        DepthBoostController c;
        DepthBoostController::Input in;
        in.dt = 1.0f / 45; in.active = true; in.gaze_valid = true; in.base_ipd_m = 0.064f; in.target = 1.0f; in.fixation = Fixation::near_;
        in.jump_mm = 12; in.hold = 1; in.no_saccade_step = true; in.saccade = true; in.saccade_started = true;
        c.step(in, s);
        check(std::fabs(c.scale() - 1.1875f) < 1e-5f, "controller: the step at once", c.scale(), 1.1875);
        in.jump_mm = 0; in.saccade_started = false;
        for (int k = 0; k < 10; ++k) c.step(in, s);
        check(std::fabs(c.scale() - 1.1875f) < 1e-5f, "controller: held against the step", c.scale(), 1.1875);
        in.hold = 0; in.saccade = false; in.landing_window = true; in.landing_step = true;
        float prev = c.scale(), largest = 0;
        for (int k = 0; k < 45; ++k) { c.step(in, s); largest = std::max(largest, (prev - c.scale()) * 64); prev = c.scale(); }
        check(largest <= 40.0f / 45 + 1e-3f && c.scale() == 1.0f, "controller: released, back to 1 at DownMmPerS with no saccade step", largest, 40.0 / 45);
    }
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
    // The pupil's gaze filter at 45 FPS (median of 5, noise-adaptive One Euro): checks independent of how it works.
    {
        const float dt = 1.0f / 45.0f, r = 0.0105f;
        const Vec3 rest{0, 0, -1};
        const auto dir = [](float x, float y) { return normalized({tan_deg(x), tan_deg(y), -1}); };
        const auto deg = [](Vec3 a, Vec3 b) { return std::acos(std::clamp(dot(normalized(a), normalized(b)), -1.0f, 1.0f)) * 57.3f; };
        const auto step = [&](PupilGazeFilter& f, Vec3 g, Vec3 out[2]) { const Vec3 in[2] = {g, g}; return f.update(in, dt, 0.25f, out); };
        // Fixation with 3 degrees of noise per axis: the pupil moves under 0.3 mm from frame to frame (unfiltered: over 2 mm).
        {
            Random rng;
            PupilGazeFilter f;
            Vec3 out[2], last{};
            float worst = 0, raw_worst = 0;
            Vec3 last_raw = dir(-5, -15);
            for (int i = 0; i < 450; ++i) {
                const Vec3 g = dir(-5 + rng.gauss() * 3, -15 + rng.gauss() * 3);
                step(f, g, out);
                if (i > 45) {
                    worst = std::max(worst, length(pupil_offset(out[0], rest, r) - pupil_offset(last, rest, r)));
                    raw_worst = std::max(raw_worst, length(pupil_offset(g, rest, r) - pupil_offset(last_raw, rest, r)));
                }
                last = out[0]; last_raw = g;
            }
            check(worst < 0.0003f && raw_worst > 0.002f, "pupil filter: 3 deg noise moves the pupil under 0.3 mm per frame", worst * 1000, raw_worst * 1000);
            check(f.take_stats().noise_deg > 2.0f, "pupil filter: the tracker's noise is measured", 0, 0);
        }
        // Two bad samples in a row (35 degrees off) during a clean fixation: never followed.
        {
            Random rng;
            PupilGazeFilter f;
            Vec3 out[2];
            float worst = 0;
            for (int i = 0; i < 200; ++i) {
                const Vec3 g = (i == 100 || i == 101) ? dir(0, -35) : dir(rng.gauss() * 0.5f, rng.gauss() * 0.5f);
                step(f, g, out);
                if (i >= 100) worst = std::max(worst, deg(out[0], dir(0, 0)));
            }
            check(worst < 1.5f, "pupil filter: a pair of bad samples is not followed", worst, 1.5);
        }
        // Smooth pursuit at 50 deg/s: no steps (under 0.4 mm per frame), lag under 8 degrees.
        {
            PupilGazeFilter f;
            Vec3 out[2], last{};
            float worst_step = 0, worst_lag = 0;
            for (int i = 0; i < 50; ++i) {
                const float x = -25 + 50.0f * i * dt;
                step(f, dir(x, 0), out);
                if (i > 10) { worst_step = std::max(worst_step, length(pupil_offset(out[0], rest, r) - pupil_offset(last, rest, r))); worst_lag = std::max(worst_lag, deg(out[0], dir(x, 0))); }
                last = out[0];
            }
            check(worst_step < 0.0004f && worst_lag < 8.0f, "pupil filter: smooth pursuit is followed without steps", worst_step * 1000, worst_lag);
        }
        // An 18-degree saccade with 1 degree of noise: within 3 degrees of the landing in 10 frames or less.
        {
            Random rng;
            PupilGazeFilter f;
            Vec3 out[2];
            int frames = 999;
            for (int i = 0; i < 160; ++i) {
                const bool after = i >= 100;
                step(f, after ? dir(15 + rng.gauss(), -10 + rng.gauss()) : dir(rng.gauss(), rng.gauss()), out);
                if (after && frames == 999 && deg(out[0], dir(15, -10)) < 3.0f) frames = i - 100 + 1;
            }
            check(frames <= 10, "pupil filter: a saccade is followed within 10 frames", frames, 10);
        }
        // When the pupil moves (PupilLock): a clean fixation keeps it still; a saccade moves it within the two frames after
        // the landing (post-saccadic suppression); a tracker noisier than 1.5 degrees takes it slowly back to rest.
        {
            PupilGazeFilter f;
            PupilLock lock;
            Vec3 filtered[2], out[2];
            const Vec3 rests[2] = {rest, rest};
            const auto frame = [&](Vec3 g) {
                const Vec3 in[2] = {g, g};
                const bool v = f.update(in, dt, 0.25f, filtered);
                return lock.update(in, v, filtered, rests, f.noise_deg(), dt, out);
            };
            for (int i = 0; i < 90; ++i) frame(dir(0, -20));
            const Vec3 before = out[0];
            float worst = 0;
            for (int i = 0; i < 45; ++i) { frame(dir(0, -20)); worst = std::max(worst, length(pupil_offset(out[0], rest, r) - pupil_offset(before, rest, r))); }
            check(worst < 0.0001f, "pupil lock: a clean fixation keeps the pupil still", worst * 1000, 0.1);
            // An 18-degree saccade over 3 frames, then the fixation: on the landing within 2 frames after it.
            frame(dir(5, -17)); frame(dir(12, -13));
            int frames = 99;
            for (int i = 0; i < 6; ++i) { frame(dir(15, -10)); if (frames == 99 && deg(out[0], dir(15, -10)) < 0.5f) frames = i + 1; }
            check(frames <= 2, "pupil lock: a saccade moves the pupil onto the landing within 2 frames", frames, 2);
            const Vec3 landed = out[0];
            for (int i = 0; i < 45; ++i) frame(dir(15, -10));
            check(deg(out[0], landed) < 0.5f, "pupil lock: it stays after the landing", deg(out[0], landed), 0.5);
            check(lock.take_stats().saccades >= 1, "pupil lock: the saccade counted", 0, 0);
        }
        {
            Random rng;
            PupilGazeFilter f;
            PupilLock lock;
            Vec3 filtered[2], out[2];
            const Vec3 rests[2] = {rest, rest};
            for (int i = 0; i < 1500; ++i) {  // 33 s: back to rest at 1 deg/s from 22 degrees away
                const Vec3 g = dir(10 + rng.gauss() * 4, -20 + rng.gauss() * 4);
                const Vec3 in[2] = {g, g};
                const bool v = f.update(in, dt, 0.25f, filtered);
                lock.update(in, v, filtered, rests, f.noise_deg(), dt, out);
            }
            check(lock.take_stats().noisy && deg(out[0], rest) < 1.0f, "pupil lock: a tracker with 4 deg noise takes the pupil back to rest", deg(out[0], rest), 1.0);
        }
        // A blink: the last gaze is held for 0.3 s, then nothing (the pupil goes to rest).
        {
            PupilGazeFilter f;
            Vec3 out[2];
            for (int i = 0; i < 30; ++i) step(f, dir(10, -10), out);
            bool held = true;
            for (int i = 0; i < 12; ++i) held = held && f.update(nullptr, dt, 0.25f, out) && deg(out[0], dir(10, -10)) < 1.0f;  // 0.27 s
            for (int i = 0; i < 3; ++i) f.update(nullptr, dt, 0.25f, out);
            check(held && !f.update(nullptr, dt, 0.25f, out), "pupil filter: a blink holds the gaze 0.3 s, then the pupil rests", 0, 0);
        }
    }
    // A gaze no eye can hold is no gaze: the (5.9, 34.7) degree placeholder, or 55 degrees off; 25 degrees down is kept.
    {
        EyeGazes g; g.source = GazeSource::qv;
        const auto at = [](float x, float y) { return normalized({tan_deg(x), tan_deg(y), -1}); };
        g.local[0] = g.local[1] = at(5.9f, 34.7f);
        check(plausible_gaze(g).source == GazeSource::none, "gaze: the upward placeholder is no gaze", 0, 0);
        g.local[0] = g.local[1] = at(55, 0);
        check(plausible_gaze(g).source == GazeSource::none, "gaze: 55 degrees off is no gaze", 0, 0);
        g.local[0] = g.local[1] = at(10, -25);
        check(plausible_gaze(g).source == GazeSource::qv, "gaze: 25 degrees down (the panel) is kept", 0, 0);
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
    depth_boost_tests();
    fixation_depth_tests();
    gaze_source_tests();
    predictive_tests();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
