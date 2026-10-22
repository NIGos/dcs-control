// End-to-end harness for XR_APILAYER_DCSVR_pupil_shift.dll: the real layer, loaded through the loader negotiation
// (xrNegotiateLoaderApiLayerInterface, then its xrCreateApiLayerInstance) with a fake runtime below it and a fake
// "DCS" above it. No headset, no runtime, no DCS.
//
// The fake runtime returns PRIMARY_QUAD_VARJO views like Pimax with Quad Views: two primary views canted outwards by
// 3 degrees each (6 degrees between them), 65.5 mm apart, and two focus views on the same poses whose FOV is centred
// on each eye's own scripted gaze. The head turns and moves slowly. The scripted gaze fixates targets in the cockpit
// (0.7 m) and outside (far), with per-eye tracker noise, saccades, blinks (the provider holds or drops the gaze), lost
// tracking and NaN focus FOVs, and vergence that settles after each saccade as real eyes do.
//
// Each scenario copies the DLL into its own folder with its own PupilShift.ini (the layer reads the ini next to it),
// loads it, runs, and unloads it. Checked on every frame:
//  - xrEndFrame hands the runtime exactly (bitwise) the poses and FOVs it returned for that display time, also when
//    the application located twice for one display time, resubmitted values perturbed by 1e-6, or submitted for an
//    older display time after locating the next one; a view the application changed itself passes through unchanged;
//  - every located view: runtime pose + IPD move (Ipd= or the runtime's IPD, times the Depth boost scale) + the pupil
//    shift for that eye's gaze (straight ahead = between the canted views), and the FOV shifted for it;
//  - the scale (status word) changes no faster than UpMmPerS / DownMmPerS, plus SaccadeStepMm once per saccade;
//  - the scale is 1 in the cockpit once the return has had time, and never above 1 before the source is proven;
//  - the status word fields.
// Quad Views scenarios (qv_*): the fake runtime behaves like the bundled Quad Views: one combined gaze, the focus area
// centred on it plus a focus offset, and without a gaze (blinks, a dropped gaze) the fixed focus: the resting gaze plus
// 2.4 degrees outwards per eye. With the gaze export (a fake XR_APILAYER_MBUCCHIA_quad_views_foveated.dll, fake_quadviews.cpp,
// loaded first) the layer must use the export's gaze, move no pupil on the fixed focus (rendered IPD exactly Ipd) and
// log the source; without it (Pimax native quad views) it reads the focus views, the fixed focus included.
// OpenXR eye gaze scenarios (xr_*): the fake runtime also offers XR_EXT_eye_gaze_interaction (Pimax native quad views):
// the same combined gaze and fixed focus, but the gaze reaches the layer through its own action (xrLocateSpace of the
// gaze space in the views' base space), valid and tracked or not. The layer must use it, move no pupil without a valid
// gaze (rendered IPD exactly Ipd, or the runtime's IPD, although the focus views sit 2.4 degrees outwards), and the
// harness's "DCS" must get its own action set attached and synced in every call, with the layer's merged in. Without
// the extension (the other scenarios) the layer falls back to the focus views and logs a warning.
// Writes <out>/<scenario>.csv (time, truth, estimate, scale) and <out>/<scenario>.svg, and prints a summary.
// Usage: pupil_shift_harness <layer dll> <output folder> [fake Quad Views dll]. Exit code 0 when every check passed.
#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <type_traits>
#include <deque>
#include <vector>

#include "perception_shm.h"
#include "dcsvr_pupil_fov.h"
#include "pupil_math.hpp"  // vector helpers only; the expectations below are computed independently of the layer
#include "predictive_depth.hpp"  // the main sequence, for the synthetic saccades
#include "pvr_gaze.hpp"    // the PVR ABI (the fake client's table)

namespace fs = std::filesystem;
using pupil_shift::Vec3;
using pupil_shift::Quat;
using pupil_shift::rotate;
using pupil_shift::conjugate;
using pupil_shift::normalized;
using pupil_shift::length;
using pupil_shift::dot;
using pupil_shift::PupilGazeFilter;
using pupil_shift::PupilLock;  // the pupil gaze filter, unit-tested on its own (pupil_shift_tests)
namespace pvr = pupil_shift::pvr;

namespace {

int failures = 0;
std::string scenario_name;
// Every failure is counted; the first three of each kind (scenario and message) are printed.
std::map<std::string, int> failure_kinds;
void fail(const char* what, double a = 0, double b = 0, double t = -1) {
    ++failures;
    if (++failure_kinds["[" + scenario_name + "] " + what] <= 3) std::printf("FAIL [%s] %s (%.6g vs %.6g) at t=%.3f s\n", scenario_name.c_str(), what, a, b, t);
}
#define CHECK(cond, what, a, b, t) do { if (!(cond)) fail(what, a, b, t); } while (0)

constexpr float kPi = 3.14159265f;
float rad(float deg) { return deg * kPi / 180.0f; }
Quat yaw_quat(float r) { return {0, std::sin(r / 2), 0, std::cos(r / 2)}; }
Quat multiply(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
XrVector3f xr(Vec3 v) { return {v.x, v.y, v.z}; }
Vec3 vec(const XrVector3f& v) { return {v.x, v.y, v.z}; }
XrQuaternionf xr(Quat q) { return {q.x, q.y, q.z, q.w}; }
Quat quat(const XrQuaternionf& q) { return {q.x, q.y, q.z, q.w}; }

// ---- Scenario script ------------------------------------------------------------------------------------------------
enum class GazeMode { tracked, held, centred, nan_fov };
// What a segment of a depth scenario must show (checked on the status word):
//  hold: no change of the fixation state and the render IPD within 0.5 mm, after the first 0.15 s (edges, ambiguous);
//  x1: the scale back to exactly 1 within 0.8 s of the segment's start (a near fixation, after a saccade);
//  exact1: the scale exactly 1 on every frame (Realistic below FarStartM, depth near);
//  far: the state far by the end of the segment and the scale above 1; rise: and the scale at least 1.2.
enum class Expect { none, hold, x1, exact1, far_, rise };
struct Segment {
    std::string label;
    float seconds{};
    Vec3 target{};             // head frame, metres; the fixation point at the end of the segment
    bool saccade{};            // a saccade towards target lasting `seconds`
    GazeMode mode{GazeMode::tracked};
    bool lost{};               // the runtime reports no valid pose (layer passes through)
    Expect expect{Expect::none};
    bool pursuit{};            // smooth pursuit: direction and log-distance move evenly from the previous target
    float within{0.8f};        // x1: the deadline from the segment's start
};
struct Script {
    std::vector<Segment> segments;
    float noise_deg{0.5f};     // white per-eye noise, degrees per axis per sample
    float drift_deg{0.5f};     // slowly drifting per-eye error (tracker accuracy), degrees per axis
    bool combined{};           // both focus views from one gaze projected at combined_m (Quad Views style)
    float combined_m{1.0f};    // projection distance; <= 0: parallel (projected at infinity)
};

Vec3 cockpit(float yaw_deg, float pitch_deg, float metres = 0.7f) { return normalized({std::tan(rad(yaw_deg)), std::tan(rad(pitch_deg)), -1}) * metres; }
Vec3 outside(float yaw_deg, float pitch_deg, float metres = 2000.0f) { return cockpit(yaw_deg, pitch_deg, metres); }

// What the eyes do at time t: the fixation point (with vergence settling after a saccade), mode and saccade flag.
struct Eyes {
    Vec3 point;                // where the eyes converge (head frame)
    float true_distance{};     // distance of the fixation target
    bool in_saccade{}, settling{};
    GazeMode mode{GazeMode::tracked};
    bool lost{};
    std::string label;
    int segment{-1};
    float segment_start{};
    Expect expect{Expect::none};
    float within{0.8f};
};
Eyes evaluate(const Script& s, float t) {
    float start = 0;
    Vec3 previous = s.segments.front().target;
    for (const auto& seg : s.segments) {
        if (t < start + seg.seconds || &seg == &s.segments.back()) {
            Eyes e;
            e.mode = seg.mode; e.lost = seg.lost; e.label = seg.label;
            e.segment = static_cast<int>(&seg - s.segments.data()); e.segment_start = start; e.expect = seg.expect; e.within = seg.within;
            const float local = std::min(t - start, seg.seconds);
            const float dp = length(previous), dn = length(seg.target);
            if (seg.pursuit) {
                const float k = local / seg.seconds;
                const Vec3 dir = normalized(normalized(previous) * (1 - k) + normalized(seg.target) * k);
                const float d = std::exp(std::log(dp) + (std::log(dn) - std::log(dp)) * k);
                e.point = dir * d;
                e.true_distance = d;
            } else if (seg.saccade) {
                // Direction sweeps fast (a saccade); depth stays near the old one until landing (vergence lags).
                const float k = local / seg.seconds;
                const float smooth = k * k * (3 - 2 * k);
                const Vec3 dir = normalized(normalized(previous) * (1 - smooth) + normalized(seg.target) * smooth);
                e.point = dir * dp;
                e.in_saccade = true;
                e.true_distance = dn;
            } else {
                // Vergence settles to the new depth in about 200 ms after the landing (time constant 60 ms).
                const float settle = 1.0f - std::exp(-local / 0.06f);
                const float d = std::exp(std::log(dp) + (std::log(dn) - std::log(dp)) * settle);
                e.point = normalized(seg.target) * d;
                e.settling = local < 0.2f;
                e.true_distance = dn;
            }
            return e;
        }
        start += seg.seconds;
        previous = seg.target;
    }
    return {};
}
float duration(const Script& s) { float d = 0; for (const auto& seg : s.segments) d += seg.seconds; return d; }

// ---- Fake runtime ---------------------------------------------------------------------------------------------------
constexpr float kRuntimeIpd = 0.0655f, kCantDeg = 3.0f, kFocusHalf = 0.35f;
// Quad Views style: its fixed focus sits 2.4 degrees outwards per eye (the live Pimax log); with a gaze its focus area
// is offset 1 degree outwards from it (horizontal_focus_offset), so the focus centre is not the gaze.
constexpr float kQvFixedDeg = 2.4f, kQvFocusOffsetDeg = 1.0f;
constexpr std::uint32_t kQvSample = 1, kQvValid = 2, kQvSaccade = 4;
struct RuntimeFrame { std::array<XrView, 4> views{}; };
// What the fake Quad Views recorded for a display time: flags and the combined gaze in each (canted) eye's frame.
struct QvSample { std::uint32_t flags{}; Vec3 local[2]{}; Vec3 world{0, 0, -1}; };
struct Fake {
    const Script* script{};
    std::map<XrTime, RuntimeFrame> located;   // what the runtime returned, per display time
    std::mt19937 rng{12345};
    std::normal_distribution<float> gauss{0.0f, 1.0f};
    Vec3 drift[2]{};
    XrTime last_drift_time{};
    std::array<XrFovf, 2> last_focus{};
    bool qv_style{};                          // the runtime behaves like the bundled Quad Views
    std::map<XrTime, QvSample> qv;            // per display time
    Vec3 qv_last_dir{};                       // Quad Views' blink cache: the last valid gaze
    bool qv_have_dir{};
    // Expected submission at xrEndFrame, per view: the runtime's own view for that time, or a foreign value.
    std::array<bool, 4> expect_foreign{};
    std::array<XrCompositionLayerProjectionView, 4> foreign{};
    unsigned end_frames{}, checked_views{}, foreign_views{};
    // OpenXR eye gaze (XR_EXT_eye_gaze_interaction) and the action system, as the runtime sees them.
    bool xr_offered{};                        // the runtime lists the extension
    bool xr_never_valid{};                    // nobody wearing the headset: the gaze is never valid
    std::vector<std::string> enabled;         // extensions the instance was created with
    std::map<std::string, XrPath> paths;
    std::map<std::uint64_t, std::string> sets, actions;  // handle -> name
    std::uint64_t next_handle{0x1000};
    XrActionSet app_set{XR_NULL_HANDLE}, layer_set{XR_NULL_HANDLE};
    XrAction layer_action{XR_NULL_HANDLE};
    std::map<XrPath, std::vector<XrActionSuggestedBinding>> suggested;  // the last suggestion per profile
    XrSpace gaze_space{XR_NULL_HANDLE};
    unsigned attach_calls{}, attached_with_layer{}, syncs{}, syncs_with_layer{}, syncs_without_app{}, sync_duplicates{};
    unsigned locate_space_calls{}, locate_space_wrong{};
    std::vector<XrActionSet> attached_sets;
    bool attached{}, session_destroyed{}, instance_destroyed{};
    // Pimax PVR (the fake LibPVRClient64.dll): the runtime calls its frame and tracking slots with its own handle,
    // through the table (pvr_mode 1) or through copies taken before the layer patched it (2, a runtime that copied it).
    int pvr_mode{};
    pvr::Interface* pvr_table{};
    pvr::GetTrackingStateFn pvr_tracking_copy{};
    pvr::FrameFn pvr_end_copy{};
    pvr::DestroyHmdFn pvr_destroy_copy{};
    void* pvr_hmd{};
    void (*pvr_set_time)(double){};
    pvr::EyeTrackingInfo pvr_info{};          // the latest per-eye report of the tracker
    // The predictive scenarios: DCS locates display_lead_s before the display time (the eyes and the PVR clock are at
    // the locate time, the head at the display time); the PVR samples come from the harness's stream (fake_pvr.cpp).
    float display_lead_s{};
    bool pvr_stream{};
} fake;
const XrSpace kBaseSpace = reinterpret_cast<XrSpace>(static_cast<std::uintptr_t>(0x77));
const XrSession kSession = reinterpret_cast<XrSession>(static_cast<std::uintptr_t>(1));

XrTime t0 = 1'000'000'000'000LL;
float seconds_of(XrTime t) { return static_cast<float>(static_cast<double>(t - t0) * 1e-9); }

// The head pose at time t: turns +-15 deg at 0.1 Hz and sways a few cm (world space, metres).
void head_pose(float t, Quat& q, Vec3& p) {
    q = multiply(yaw_quat(rad(15.0f) * std::sin(0.6283f * t)), Quat{std::sin(rad(4.0f) * std::sin(0.4f * t) / 2), 0, 0, std::cos(rad(4.0f) * std::sin(0.4f * t) / 2)});
    p = {0.03f * std::sin(0.5f * t), 1.2f + 0.01f * std::sin(0.9f * t), 0.02f * std::cos(0.3f * t)};
}

XrFovf focus_fov(Vec3 local_dir) {
    const float tx = local_dir.x / -local_dir.z, ty = local_dir.y / -local_dir.z;
    return {std::atan(tx - kFocusHalf), std::atan(tx + kFocusHalf), std::atan(ty + kFocusHalf), std::atan(ty - kFocusHalf)};
}

XRAPI_ATTR XrResult XRAPI_CALL fake_locate_views(XrSession, const XrViewLocateInfo* info, XrViewState* state, uint32_t capacity,
    uint32_t* count, XrView* views) {
    *count = 4;
    if (capacity < 4) return XR_ERROR_SIZE_INSUFFICIENT;
    const float t = seconds_of(info->displayTime);
    const Eyes eyes = evaluate(*fake.script, t - fake.display_lead_s);
    state->viewStateFlags = eyes.lost ? 0 : (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT |
        XR_VIEW_STATE_POSITION_TRACKED_BIT | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT);
    if (auto it = fake.located.find(info->displayTime); it != fake.located.end()) {  // the same time again: same answer
        std::copy(it->second.views.begin(), it->second.views.end(), views);
        return XR_SUCCESS;
    }
    Quat qh; Vec3 ph; head_pose(t, qh, ph);
    if (fake.pvr_mode) {
        // The runtime's own per-frame PVR call (with its handle), through the table or its copy.
        fake.pvr_set_time(t - fake.display_lead_s + 1.0);
        const auto tracking = fake.pvr_mode == 1 ? reinterpret_cast<pvr::GetTrackingStateFn>(fake.pvr_table->slots[pvr::slot_get_tracking_state]) : fake.pvr_tracking_copy;
        tracking(fake.pvr_hmd, t, nullptr);
    }
    const Vec3 right = rotate(qh, {1, 0, 0});
    const Vec3 eye_pos[2] = {ph + right * (-kRuntimeIpd / 2), ph + right * (kRuntimeIpd / 2)};
    const Quat eye_q[2] = {multiply(qh, yaw_quat(rad(kCantDeg))), multiply(qh, yaw_quat(rad(-kCantDeg)))};
    const XrFovf primary[2] = {{-1.05f, 0.80f, 0.85f, -0.90f}, {-0.80f, 1.05f, 0.85f, -0.90f}};
    // Per-eye drift: a slow random walk pulled back to zero (time constant about 2 s), std about drift_deg.
    if (fake.last_drift_time) {
        const float dt = static_cast<float>(static_cast<double>(info->displayTime - fake.last_drift_time) * 1e-9);
        if (dt > 0) {
            const float k = std::exp(-dt / 2.0f), sd = rad(fake.script->drift_deg) * std::sqrt(std::max(0.0f, 1 - k * k));
            for (auto& d : fake.drift) d = Vec3{d.x * k + fake.gauss(fake.rng) * sd, d.y * k + fake.gauss(fake.rng) * sd, 0};
        }
    }
    fake.last_drift_time = info->displayTime;
    RuntimeFrame frame;
    // Pimax PVR's per-eye report: each eye's own gaze (tangents in the head frame) with its own noise; a held gaze repeats
    // the last report (the same sample time), a dropped or NaN one reports time 0 (invalid).
    if (fake.pvr_mode && !fake.pvr_stream && eyes.mode != GazeMode::held) {
        fake.pvr_info = {};
        if (eyes.mode == GazeMode::tracked) {
            const float pn = rad(fake.script->noise_deg);
            for (int eye = 0; eye < 2; ++eye) {
                const Vec3 eye_head{eye == 0 ? -kRuntimeIpd / 2 : kRuntimeIpd / 2, 0, 0};
                const Vec3 g = normalized(normalized(eyes.point - eye_head) +
                    Vec3{fake.gauss(fake.rng) * pn + fake.drift[eye].x, fake.gauss(fake.rng) * pn + fake.drift[eye].y, 0});
                fake.pvr_info.GazeTan[eye] = {g.x / -g.z, g.y / -g.z};
            }
            fake.pvr_info.TimeInSeconds = t + 1.0;
            fake.pvr_info.ConvergenceDistance = length(eyes.point);
        }
    }
    // A combined gaze has tracker noise too, one sample for both eyes.
    const float n = rad(fake.script->noise_deg);
    const Vec3 combined_noise{fake.gauss(fake.rng) * n + fake.drift[0].x, fake.gauss(fake.rng) * n + fake.drift[0].y, 0};
    // Quad Views: one gaze (view space, here the head frame) for both eyes; held: its cache repeats the last valid gaze;
    // dropped or NaN: no gaze, the fixed focus.
    QvSample qs;
    const Quat cant[2] = {yaw_quat(rad(kCantDeg)), yaw_quat(rad(-kCantDeg))};
    if (fake.qv_style) {
        Vec3 dir = normalized(normalized(eyes.point) + combined_noise);
        const bool valid = !fake.xr_never_valid && (eyes.mode == GazeMode::tracked || (eyes.mode == GazeMode::held && fake.qv_have_dir));
        if (eyes.mode == GazeMode::held && fake.qv_have_dir) dir = fake.qv_last_dir;
        if (valid) { fake.qv_last_dir = dir; fake.qv_have_dir = true; }
        qs.flags = kQvSample | (valid ? kQvValid : 0) | (valid && eyes.in_saccade ? kQvSaccade : 0);
        for (int eye = 0; eye < 2; ++eye) qs.local[eye] = valid ? rotate(conjugate(cant[eye]), dir) : Vec3{0, 0, -1};
        qs.world = rotate(qh, dir);
        fake.qv[info->displayTime] = qs;
        while (fake.qv.size() > 64) fake.qv.erase(fake.qv.begin());
    }
    for (int eye = 0; eye < 2; ++eye) {
        // Gaze in the head frame: from this eye to the fixation point, or one gaze for both (combined) projected at a
        // fixed distance from the head centre, as Quad Views does.
        Vec3 g;
        const Vec3 eye_head{eye == 0 ? -kRuntimeIpd / 2 : kRuntimeIpd / 2, 0, 0};
        if (fake.script->combined) {
            const Vec3 dir = normalized(normalized(eyes.point) + combined_noise);
            g = fake.script->combined_m > 0 ? normalized(dir * fake.script->combined_m - eye_head) : dir;
        } else {
            g = normalized(eyes.point - eye_head);
            g = normalized(g + Vec3{fake.gauss(fake.rng) * n + fake.drift[eye].x, fake.gauss(fake.rng) * n + fake.drift[eye].y, 0});
        }
        const Vec3 g_world = rotate(qh, g);
        XrView& p = frame.views[eye];
        p = {XR_TYPE_VIEW};
        p.pose = {xr(eye_q[eye]), xr(eye_pos[eye])};
        p.fov = primary[eye];
        XrView& f = frame.views[eye + 2];
        f = {XR_TYPE_VIEW};
        f.pose = p.pose;
        if (fake.qv_style) {
            const float side = eye == 0 ? -1.0f : 1.0f;  // outwards
            if (qs.flags & kQvValid) {
                const Vec3 l = qs.local[eye];
                f.fov = focus_fov({l.x / -l.z + side * std::tan(rad(kQvFocusOffsetDeg)), l.y / -l.z, -1});
            } else {
                const Vec3 rest = rotate(conjugate(cant[eye]), {0, 0, -1});
                f.fov = focus_fov({rest.x / -rest.z + side * std::tan(rad(kQvFixedDeg)), rest.y / -rest.z, -1});
            }
            fake.last_focus[eye] = f.fov;
            continue;
        }
        switch (eyes.mode) {
        case GazeMode::tracked: f.fov = focus_fov(rotate(conjugate(eye_q[eye]), g_world)); break;
        case GazeMode::held: f.fov = fake.last_focus[eye]; break;                     // the provider repeats its last gaze
        case GazeMode::centred: f.fov = focus_fov({0, 0, -1}); break;                  // its fallback without a gaze
        case GazeMode::nan_fov: f.fov = {std::nanf(""), std::nanf(""), std::nanf(""), std::nanf("")}; break;
        }
        if (eyes.mode != GazeMode::nan_fov) fake.last_focus[eye] = f.fov;
    }
    fake.located[info->displayTime] = frame;
    while (fake.located.size() > 64) fake.located.erase(fake.located.begin());
    std::copy(frame.views.begin(), frame.views.end(), views);
    return XR_SUCCESS;
}

// The fake Quad Views' DcsVr_QuadViewsGaze: the sample of that display time, else the latest within 100 ms before it.
std::uint32_t fake_qv_gaze(std::int64_t time, float left[3], float right[3]) {
    auto it = fake.qv.upper_bound(time);
    if (it == fake.qv.begin()) return 0;
    --it;
    if (time - it->first >= 100'000'000) return 0;
    left[0] = it->second.local[0].x; left[1] = it->second.local[0].y; left[2] = it->second.local[0].z;
    right[0] = it->second.local[1].x; right[1] = it->second.local[1].y; right[2] = it->second.local[1].z;
    return it->second.flags;
}

bool bitwise_equal(const XrPosef& a, const XrPosef& b) { return std::memcmp(&a, &b, sizeof a) == 0; }
bool bitwise_equal(const XrFovf& a, const XrFovf& b) { return std::memcmp(&a, &b, sizeof a) == 0; }
bool bitwise_equal(const XrQuaternionf& a, const XrQuaternionf& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

XRAPI_ATTR XrResult XRAPI_CALL fake_end_frame(XrSession, const XrFrameEndInfo* info) {
    ++fake.end_frames;
    if (fake.pvr_mode) {
        const auto end = fake.pvr_mode == 1 ? reinterpret_cast<pvr::FrameFn>(fake.pvr_table->slots[pvr::slot_end_frame]) : fake.pvr_end_copy;
        end(fake.pvr_hmd, 0, nullptr, 0);
    }
    const float t = seconds_of(info->displayTime);
    const auto it = fake.located.find(info->displayTime);
    for (std::uint32_t l = 0; l < info->layerCount; ++l) {
        if (info->layers[l]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[l]);
        for (std::uint32_t v = 0; v < projection->viewCount && v < 4; ++v) {
            const auto& view = projection->views[v];
            if (fake.expect_foreign[v]) {
                ++fake.foreign_views;
                CHECK(bitwise_equal(view.pose, fake.foreign[v].pose) && bitwise_equal(view.fov, fake.foreign[v].fov),
                    "a view the application changed itself passes through unchanged", view.pose.position.x, fake.foreign[v].pose.position.x, t);
                continue;
            }
            ++fake.checked_views;
            CHECK(it != fake.located.end(), "end frame for a located time", 0, 0, t);
            if (it == fake.located.end()) continue;
            const XrView& truth = it->second.views[v];
            CHECK(bitwise_equal(view.pose, truth.pose) && bitwise_equal(view.fov, truth.fov),
                "xrEndFrame restores the runtime's exact pose and FOV", view.pose.position.x - truth.pose.position.x, view.fov.angleLeft - truth.fov.angleLeft, t);
        }
    }
    return XR_SUCCESS;
}

// ---- Fake runtime: XR_EXT_eye_gaze_interaction and the action system --------------------------------------------------
// The runtime's side of the OpenXR eye gaze: it lists the extension when the scenario offers it, keeps the suggestions per
// interaction profile (a later one for the same profile replaces the earlier, as in OpenXR), refuses a second attachment
// and syncs of sets that are not attached, and locates the gaze space from the scripted combined gaze: valid and tracked
// only when the gaze is (and once the layer's set was attached and synced).
std::uint64_t key(const void* handle) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle)); }
template <typename T> T new_handle() { return reinterpret_cast<T>(static_cast<std::uintptr_t>(++fake.next_handle)); }
bool gaze_enabled() {
    return std::find(fake.enabled.begin(), fake.enabled.end(), std::string(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) != fake.enabled.end();
}
// The rotation that turns -Z (the gaze pose's forward) to the unit vector d.
Quat look_along(Vec3 d) {
    const Vec3 a{0, 0, -1};
    const Vec3 axis = pupil_shift::cross(a, d);
    const float w = 1.0f + dot(a, d);
    const float n = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z + w * w);
    return {axis.x / n, axis.y / n, axis.z / n, w / n};
}

XRAPI_ATTR XrResult XRAPI_CALL fake_enumerate_extensions(const char* layer, uint32_t capacity, uint32_t* count, XrExtensionProperties* properties) {
    if (layer) return XR_ERROR_API_LAYER_NOT_PRESENT;
    const char* names[] = {"XR_KHR_D3D11_enable", "XR_VARJO_quad_views", XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME};
    const uint32_t n = fake.xr_offered ? 3 : 2;
    *count = n;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < n) return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < n; ++i) { strcpy_s(properties[i].extensionName, names[i]); properties[i].extensionVersion = 1; }
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL fake_create_action_set(XrInstance, const XrActionSetCreateInfo* info, XrActionSet* set) {
    for (const auto& [handle, name] : fake.sets)
        if (name == info->actionSetName) return XR_ERROR_NAME_DUPLICATED;
    *set = new_handle<XrActionSet>();
    fake.sets[key(*set)] = info->actionSetName;
    if (std::strcmp(info->actionSetName, "dcs_input") == 0) fake.app_set = *set;
    else if (std::strcmp(info->actionSetName, "dcsvr_pupil_gaze") == 0) fake.layer_set = *set;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL fake_destroy_action_set(XrActionSet set) {
    if (set == fake.layer_set) { fake.layer_set = XR_NULL_HANDLE; fake.layer_action = XR_NULL_HANDLE; }
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL fake_create_action(XrActionSet set, const XrActionCreateInfo* info, XrAction* action) {
    if (fake.attached) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    *action = new_handle<XrAction>();
    fake.actions[key(*action)] = info->actionName;
    if (set == fake.layer_set) fake.layer_action = *action;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL fake_string_to_path(XrInstance, const char* text, XrPath* path) {
    auto it = fake.paths.find(text);
    if (it == fake.paths.end()) it = fake.paths.emplace(text, static_cast<XrPath>(fake.paths.size() + 1)).first;
    *path = it->second;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL fake_get_system_properties(XrInstance, XrSystemId, XrSystemProperties* properties) {
    for (auto* next = static_cast<XrBaseOutStructure*>(properties->next); next; next = next->next)
        if (next->type == XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT)
            reinterpret_cast<XrSystemEyeGazeInteractionPropertiesEXT*>(next)->supportsEyeGazeInteraction = gaze_enabled() ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL fake_create_action_space(XrSession, const XrActionSpaceCreateInfo* info, XrSpace* space) {
    CHECK(info->action == fake.layer_action && fake.layer_action != XR_NULL_HANDLE, "the gaze space is the layer's gaze action's", 0, 0, -1);
    *space = new_handle<XrSpace>();
    fake.gaze_space = *space;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL fake_locate_space(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* location) {
    ++fake.locate_space_calls;
    if (space != fake.gaze_space || space == XR_NULL_HANDLE) return XR_ERROR_HANDLE_INVALID;
    location->locationFlags = 0;
    const auto it = fake.qv.find(time);
    if (base != kBaseSpace || it == fake.qv.end()) { ++fake.locate_space_wrong; return XR_SUCCESS; }
    if (!fake.attached || fake.syncs_with_layer == 0 || !(it->second.flags & kQvValid)) return XR_SUCCESS;
    location->pose.orientation = xr(look_along(normalized(it->second.world)));
    location->pose.position = {0, 1.2f, 0};
    location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL fake_suggest(XrInstance, const XrInteractionProfileSuggestedBinding* suggested) {
    if (fake.attached) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    fake.suggested[suggested->interactionProfile].assign(suggested->suggestedBindings, suggested->suggestedBindings + suggested->countSuggestedBindings);
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL fake_attach(XrSession, const XrSessionActionSetsAttachInfo* info) {
    ++fake.attach_calls;
    if (fake.attached) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    fake.attached_sets.assign(info->actionSets, info->actionSets + info->countActionSets);
    CHECK(std::count(fake.attached_sets.begin(), fake.attached_sets.end(), fake.app_set) == 1, "the application's set is attached once", 0, 0, -1);
    if (std::count(fake.attached_sets.begin(), fake.attached_sets.end(), fake.layer_set) == 1 && fake.layer_set != XR_NULL_HANDLE) ++fake.attached_with_layer;
    fake.attached = true;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL fake_sync(XrSession, const XrActionsSyncInfo* info) {
    ++fake.syncs;
    bool app{}, layer{};
    for (uint32_t i = 0; i < info->countActiveActionSets; ++i) {
        const XrActionSet set = info->activeActionSets[i].actionSet;
        if (std::find(fake.attached_sets.begin(), fake.attached_sets.end(), set) == fake.attached_sets.end()) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
        for (uint32_t j = 0; j < i; ++j) if (info->activeActionSets[j].actionSet == set) ++fake.sync_duplicates;
        app |= set == fake.app_set;
        layer |= set == fake.layer_set && set != XR_NULL_HANDLE;
    }
    if (!app) ++fake.syncs_without_app;
    if (layer) ++fake.syncs_with_layer;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL fake_create_session(XrInstance, const XrSessionCreateInfo*, XrSession* session) { *session = kSession; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL fake_destroy_session(XrSession) {
    fake.session_destroyed = true;
    if (fake.pvr_mode) {
        const auto destroy = fake.pvr_mode == 1 ? reinterpret_cast<pvr::DestroyHmdFn>(fake.pvr_table->slots[pvr::slot_destroy_hmd]) : fake.pvr_destroy_copy;
        destroy(fake.pvr_hmd);
    }
    return XR_SUCCESS;
}
void fake_pvr_provider(pvr::EyeTrackingInfo* info) { *info = fake.pvr_info; }
XRAPI_ATTR XrResult XRAPI_CALL fake_destroy_instance(XrInstance) { fake.instance_destroyed = true; return XR_SUCCESS; }

XRAPI_ATTR XrResult XRAPI_CALL fake_get_instance_proc_addr(XrInstance, const char* name, PFN_xrVoidFunction* function) {
    static const std::pair<const char*, PFN_xrVoidFunction> table[] = {
        {"xrLocateViews", reinterpret_cast<PFN_xrVoidFunction>(fake_locate_views)},
        {"xrEndFrame", reinterpret_cast<PFN_xrVoidFunction>(fake_end_frame)},
        {"xrEnumerateInstanceExtensionProperties", reinterpret_cast<PFN_xrVoidFunction>(fake_enumerate_extensions)},
        {"xrCreateActionSet", reinterpret_cast<PFN_xrVoidFunction>(fake_create_action_set)},
        {"xrDestroyActionSet", reinterpret_cast<PFN_xrVoidFunction>(fake_destroy_action_set)},
        {"xrCreateAction", reinterpret_cast<PFN_xrVoidFunction>(fake_create_action)},
        {"xrStringToPath", reinterpret_cast<PFN_xrVoidFunction>(fake_string_to_path)},
        {"xrGetSystemProperties", reinterpret_cast<PFN_xrVoidFunction>(fake_get_system_properties)},
        {"xrCreateActionSpace", reinterpret_cast<PFN_xrVoidFunction>(fake_create_action_space)},
        {"xrLocateSpace", reinterpret_cast<PFN_xrVoidFunction>(fake_locate_space)},
        {"xrSuggestInteractionProfileBindings", reinterpret_cast<PFN_xrVoidFunction>(fake_suggest)},
        {"xrAttachSessionActionSets", reinterpret_cast<PFN_xrVoidFunction>(fake_attach)},
        {"xrSyncActions", reinterpret_cast<PFN_xrVoidFunction>(fake_sync)},
        {"xrCreateSession", reinterpret_cast<PFN_xrVoidFunction>(fake_create_session)},
        {"xrDestroySession", reinterpret_cast<PFN_xrVoidFunction>(fake_destroy_session)},
        {"xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction>(fake_destroy_instance)},
    };
    for (const auto& [n, f] : table)
        if (std::strcmp(name, n) == 0) { *function = f; return XR_SUCCESS; }
    *function = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
XRAPI_ATTR XrResult XRAPI_CALL fake_create_instance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo*, XrInstance* instance) {
    std::vector<std::string> enabled(info->enabledExtensionNames, info->enabledExtensionNames + info->enabledExtensionCount);
    if (!fake.xr_offered && std::count(enabled.begin(), enabled.end(), std::string(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)))
        return XR_ERROR_EXTENSION_NOT_PRESENT;
    fake.enabled = enabled;
    *instance = reinterpret_cast<XrInstance>(static_cast<std::uintptr_t>(0x1234));
    return XR_SUCCESS;
}

// ---- The layer ------------------------------------------------------------------------------------------------------
struct Layer {
    HMODULE module{};
    PFN_xrLocateViews locate{};
    PFN_xrEndFrame end{};
    PFN_xrSyncActions sync{};
    PFN_xrDestroySession destroy_session{};
    PFN_xrDestroyInstance destroy_instance{};
    XrInstance instance{};
    unsigned long long (*status)(){};
};

// What the harness's "DCS" does with input: its own action set (one controller action, and with app_gaze its own eye
// gaze action and the extension), suggested, a session, and its set attached. Through the layer's xrGetInstanceProcAddr.
bool app_input(const XrNegotiateApiLayerRequest& request, Layer& layer, bool app_gaze) {
    const auto get = [&](const char* name, auto& fn) {
        PFN_xrVoidFunction f{};
        request.getInstanceProcAddr(layer.instance, name, &f);
        fn = reinterpret_cast<std::decay_t<decltype(fn)>>(f);
        return fn != nullptr;
    };
    PFN_xrCreateActionSet create_set{}; PFN_xrCreateAction create_action{}; PFN_xrStringToPath to_path{};
    PFN_xrSuggestInteractionProfileBindings suggest{}; PFN_xrCreateSession create_session{}; PFN_xrAttachSessionActionSets attach{};
    if (!get("xrCreateActionSet", create_set) || !get("xrCreateAction", create_action) || !get("xrStringToPath", to_path) ||
        !get("xrSuggestInteractionProfileBindings", suggest) || !get("xrCreateSession", create_session) ||
        !get("xrAttachSessionActionSets", attach) || !get("xrSyncActions", layer.sync) || !get("xrDestroySession", layer.destroy_session) ||
        !get("xrDestroyInstance", layer.destroy_instance)) { std::printf("input functions missing\n"); return false; }
    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(set_info.actionSetName, "dcs_input"); strcpy_s(set_info.localizedActionSetName, "DCS input");
    XrActionSet set{};
    if (create_set(layer.instance, &set_info, &set) != XR_SUCCESS) return false;
    XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
    action_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    strcpy_s(action_info.actionName, "trigger"); strcpy_s(action_info.localizedActionName, "Trigger");
    XrAction trigger{};
    if (create_action(set, &action_info, &trigger) != XR_SUCCESS) return false;
    XrPath simple{}, click{};
    to_path(layer.instance, "/interaction_profiles/khr/simple_controller", &simple);
    to_path(layer.instance, "/user/hand/right/input/select/click", &click);
    const XrActionSuggestedBinding binding{trigger, click};
    XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggestion.interactionProfile = simple; suggestion.countSuggestedBindings = 1; suggestion.suggestedBindings = &binding;
    CHECK(suggest(layer.instance, &suggestion) == XR_SUCCESS, "the application's controller suggestion succeeds", 0, 0, -1);
    if (app_gaze) {
        // The application reads the eye gaze itself: its own pose action on the eye-gaze profile.
        action_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
        strcpy_s(action_info.actionName, "app_gaze"); strcpy_s(action_info.localizedActionName, "App gaze");
        XrAction gaze{};
        create_action(set, &action_info, &gaze);
        XrPath profile{}, pose{};
        to_path(layer.instance, "/interaction_profiles/ext/eye_gaze_interaction", &profile);
        to_path(layer.instance, "/user/eyes_ext/input/gaze_ext/pose", &pose);
        const XrActionSuggestedBinding gaze_binding{gaze, pose};
        XrInteractionProfileSuggestedBinding gaze_suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        gaze_suggestion.interactionProfile = profile; gaze_suggestion.countSuggestedBindings = 1; gaze_suggestion.suggestedBindings = &gaze_binding;
        CHECK(suggest(layer.instance, &gaze_suggestion) == XR_SUCCESS, "the application's eye-gaze suggestion succeeds", 0, 0, -1);
    }
    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.systemId = 1;
    XrSession session{};
    if (create_session(layer.instance, &session_info, &session) != XR_SUCCESS || session != kSession) return false;
    XrSessionActionSetsAttachInfo attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach_info.countActionSets = 1; attach_info.actionSets = &set;
    CHECK(attach(session, &attach_info) == XR_SUCCESS, "the application's xrAttachSessionActionSets succeeds", 0, 0, -1);
    return true;
}

// The application's per-frame xrSyncActions: its own set only; the layer adds its own below.
void app_sync(const Layer& layer, float t) {
    const XrActiveActionSet active{fake.app_set, XR_NULL_PATH};
    XrActionsSyncInfo info{XR_TYPE_ACTIONS_SYNC_INFO};
    info.countActiveActionSets = 1; info.activeActionSets = &active;
    CHECK(layer.sync(kSession, &info) == XR_SUCCESS, "the application's xrSyncActions succeeds", 0, 0, t);
}

bool load_layer(const fs::path& dll, Layer& layer, bool app_gaze = false) {
    layer.module = LoadLibraryW(dll.c_str());
    if (!layer.module) { std::printf("cannot load %ls\n", dll.c_str()); return false; }
    using Negotiate = XrResult(XRAPI_CALL*)(const XrNegotiateLoaderInfo*, const char*, XrNegotiateApiLayerRequest*);
    const auto negotiate = reinterpret_cast<Negotiate>(GetProcAddress(layer.module, "xrNegotiateLoaderApiLayerInterface"));
    layer.status = reinterpret_cast<unsigned long long (*)()>(GetProcAddress(layer.module, "PupilShift_Status"));
    if (!negotiate || !layer.status) { std::printf("exports missing\n"); return false; }
    XrNegotiateLoaderInfo loader{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(XrNegotiateLoaderInfo)};
    loader.minInterfaceVersion = 1; loader.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    loader.minApiVersion = XR_MAKE_VERSION(1, 0, 0); loader.maxApiVersion = XR_CURRENT_API_VERSION;
    XrNegotiateApiLayerRequest request{XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST, XR_API_LAYER_INFO_STRUCT_VERSION, sizeof(XrNegotiateApiLayerRequest)};
    CHECK(negotiate(&loader, "XR_APILAYER_WRONG_name", &request) != XR_SUCCESS, "negotiation refuses another layer name", 0, 0, -1);
    if (negotiate(&loader, "XR_APILAYER_DCSVR_pupil_shift", &request) != XR_SUCCESS || !request.getInstanceProcAddr || !request.createApiLayerInstance) {
        std::printf("negotiation failed\n"); return false;
    }
    XrApiLayerNextInfo next{XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO, XR_API_LAYER_NEXT_INFO_STRUCT_VERSION, sizeof(XrApiLayerNextInfo)};
    strcpy_s(next.layerName, "fake_runtime");
    next.nextGetInstanceProcAddr = fake_get_instance_proc_addr;
    next.nextCreateApiLayerInstance = fake_create_instance;
    XrApiLayerCreateInfo create{XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO, XR_API_LAYER_CREATE_INFO_STRUCT_VERSION, sizeof(XrApiLayerCreateInfo)};
    create.nextInfo = &next;
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(info.applicationInfo.applicationName, "DCS harness");
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    // DCS's own extensions (it does not enable the eye gaze; with app_gaze this application does).
    const char* extensions[] = {"XR_KHR_D3D11_enable", "XR_VARJO_quad_views", XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME};
    info.enabledExtensionCount = app_gaze ? 3 : 2;
    info.enabledExtensionNames = extensions;
    XrInstance instance{};
    if (request.createApiLayerInstance(&info, &create, &instance) != XR_SUCCESS) { std::printf("create instance failed\n"); return false; }
    layer.instance = instance;
    PFN_xrVoidFunction f{};
    request.getInstanceProcAddr(instance, "xrLocateViews", &f); layer.locate = reinterpret_cast<PFN_xrLocateViews>(f);
    request.getInstanceProcAddr(instance, "xrEndFrame", &f); layer.end = reinterpret_cast<PFN_xrEndFrame>(f);
    if (!app_input(request, layer, app_gaze)) { std::printf("application input setup failed\n"); return false; }
    return layer.locate && layer.end && reinterpret_cast<void*>(layer.locate) != reinterpret_cast<void*>(fake_locate_views);
}

// ---- Status word (decoded from the documented layout, not from pupil_math.hpp) --------------------------------------
struct Status {
    bool running, pupil_on, gaze, simulated, restored, depth_on, depth_source, pupil_disabled, depth_enabled, depth_constant;
    bool edge, veto, predictive, predictive_fallback;
    unsigned shift_tenths, scale_percent, fixation, distance_cm, fix_source;
    double scale;
};
Status decode(unsigned long long w) {
    Status s{};
    const auto bit = [w](int n) { return ((w >> n) & 1) != 0; };
    s.running = bit(0); s.pupil_on = bit(1); s.gaze = bit(2); s.simulated = bit(3); s.restored = bit(4);
    s.depth_on = bit(5); s.depth_source = bit(6); s.pupil_disabled = bit(7);
    s.shift_tenths = static_cast<unsigned>((w >> 8) & 0xFF); s.scale_percent = static_cast<unsigned>((w >> 16) & 0xFF);
    s.fixation = static_cast<unsigned>((w >> 24) & 3);
    s.depth_enabled = bit(26); s.depth_constant = bit(27);
    s.fix_source = static_cast<unsigned>((w >> 28) & 3); s.edge = bit(30); s.veto = bit(31);
    s.scale = 1.0 + static_cast<double>((w >> 32) & 0xFFFF) / 10000.0;
    s.distance_cm = static_cast<unsigned>((w >> 48) & 0xFFF);
    s.predictive = bit(60); s.predictive_fallback = bit(61);
    return s;
}

// ---- One scenario ---------------------------------------------------------------------------------------------------
struct Options {
    std::string name, description;
    Script script;
    bool pupil{true}, depth{true};
    float ini_ipd_mm{64.0f};
    float max_scale{1.4f}, up{5.0f}, down{40.0f}, saccade_step{4.0f};
    bool stepped{};                  // [DepthBoost] Stepped=1: steps in saccades only (the layer's default); 0: the ramps
    bool dual_ack{};                 // the engine's dual camera acknowledges every frame (Local\DcsVrCockpitStereo_v1)
    bool legacy{true};               // [DepthBoost] Legacy=1: the gaze-driven path these scenarios test (0: dual camera only)
    bool double_locate{true}, perturb{true}, foreign{true}, pipelined{true};
    bool qv_style{};                 // the runtime behaves like the bundled Quad Views (combined gaze, fixed focus)
    bool qv_export{};                // and its gaze export is loaded (the fake Quad Views DLL)
    bool xr_offered{};               // the runtime offers XR_EXT_eye_gaze_interaction (Pimax)
    bool xr_never_valid{};           // and its gaze is never valid (nobody wearing the headset)
    bool app_gaze{};                 // the application enables the extension and binds its own gaze action
    std::function<void(const fs::path& ini, int frame)> on_frame;  // live ini edits
    std::string mode{"enhanced"};    // [DepthBoost] Mode (the v1 scenarios: Enhanced, which boosts on vergence alone)
    float far_start_m{}, far_full_m{};  // [DepthBoost] FarStartM / FarFullM when set
    // The engine module stand-in: the scene's distance along a head-frame direction (the truth of the eyes at hand, for
    // a moving target). Without it nothing publishes depth summaries (no engine module).
    std::function<float(Vec3 dir, const Eyes& eyes)> scene;
    int latency_frames{2};           // frames between the gaze a summary was taken for and its publication
    float stale_from{-1}, stale_to{-1};  // the engine publishes nothing in this window (seconds)
    int pvr{};                       // fake Pimax PVR client: 0 none, 1 the runtime calls through the table, 2 through copies
    bool simulate{};                 // SimulateGaze=1 at (10, -5) degrees
    bool publish_gaze{};             // [PupilShift] PublishGaze=1 (GazeOut for the engine module's Visual realism)
    unsigned seed{12345};            // the tracker noise's generator
    bool trace{};                    // [DepthBoost] Trace=1 (the layer logs every display time)
};

std::string ini_text(const Options& o, bool depth_enabled) {
    std::ostringstream s;
    s << "[PupilShift]\nEnabled=" << (o.pupil ? 1 : 0) << "\nToggle=121:5\nEyeRadiusMm=10.5\nVirtualImageM=1.5\nMaxGazeDeg=35\nIpd=" << o.ini_ipd_mm
      << "\nSimulateGaze=" << (o.simulate ? 1 : 0) << "\nSimulateGazeXDeg=10\nSimulateGazeYDeg=-5\n" << (o.publish_gaze ? "PublishGaze=1\n" : "") << "\n[DepthBoost]\nEnabled=" << (depth_enabled ? 1 : 0)
      << "\nToggle=121:6\nMode=" << o.mode << "\nMaxScale=" << o.max_scale
      << "\nNearM=1.5\nUpMmPerS=" << o.up << "\nDownMmPerS=" << o.down << "\nSaccadeStepMm=" << o.saccade_step << "\nRecoverMs=800\nHoldMs=500\nStepped=" << (o.stepped ? 1 : 0) << "\nLegacy=" << (o.legacy ? 1 : 0) << "\n";
    if (o.trace) s << "Trace=1\n";
    if (o.far_start_m > 0) s << "FarStartM=" << o.far_start_m << "\nFarFullM=" << o.far_full_m << "\n";
    return s.str();
}

struct Summary {
    std::string name;
    double far_hold_reach_s{-1}, far_hold_min_after{9}, glance_return_s{-1}, cockpit_max_settled{1}, cockpit_boosted_fraction{};
    double max_up_mm_s{}, max_down_mm_s{}, class_agreement{};
    bool source_proven{}, constant_flagged{};
    unsigned frames{}, restored_views{}, foreign_views{};
    // Quad Views scenarios: the rendered IPD on frames without a gaze (the fixed focus), and frames per gaze state.
    double fixed_ipd_min{1e9}, fixed_ipd_max{};
    unsigned qv_valid{}, qv_invalid{}, qv_saccade{};
    // Depth scenarios: the hold segments' largest render-IPD range and state changes, the slowest return to exactly 1,
    // the largest per-frame IPD step outside and inside a saccade's window, summaries published, frames per fixation
    // source, edge and veto frames, GazeOut blocks checked; Pimax PVR: valid queries.
    double hold_range_mm{}, hold_span_mm{}, x1_return_s{-1}, max_step_mm{}, max_window_step_mm{}, max_scale{1};
    unsigned hold_changes{}, hold_segments{}, published{}, src_depth{}, src_vergence{}, edge_frames{}, veto_frames{}, gaze_out_checked{};
    // Local\DcsVrPupilFov_v1: entries checked at xrLocateViews and at xrEndFrame, frames read with enabled=0.
    unsigned fov_located{}, fov_submitted{}, fov_disabled{};
    bool has_scene{};
};

fs::path fake_qv_dll;

// The fake Pimax PVR client (fake_pvr.cpp), loaded once.
struct FakePvrApi {
    HMODULE module{};
    pvr::GetInterfaceFn get{};
    void (*set_provider)(void (*)(pvr::EyeTrackingInfo*)){};
    void (*set_time)(double){};
    void* (*hmd)(){};
    void* (*original)(std::uint32_t){};
    void (*stats)(unsigned[5]){};
    void (*stream_mode)(int){};
    void (*push)(const pvr::EyeTrackingInfo*, unsigned){};
    unsigned (*pending)(unsigned long long*){};
    unsigned (*after_destroy)(){};
    void (*reset)(){};
    void (*stream_repeat)(unsigned){};
} fake_pvr;
bool load_fake_pvr(const fs::path& dll) {
    fake_pvr.module = LoadLibraryW(dll.c_str());
    if (!fake_pvr.module) return false;
    const auto get = [](const char* name) { return GetProcAddress(fake_pvr.module, name); };
    fake_pvr.get = reinterpret_cast<pvr::GetInterfaceFn>(get("getPvrInterface"));
    fake_pvr.set_provider = reinterpret_cast<decltype(fake_pvr.set_provider)>(get("FakePvr_SetProvider"));
    fake_pvr.set_time = reinterpret_cast<decltype(fake_pvr.set_time)>(get("FakePvr_SetTime"));
    fake_pvr.hmd = reinterpret_cast<decltype(fake_pvr.hmd)>(get("FakePvr_Hmd"));
    fake_pvr.original = reinterpret_cast<decltype(fake_pvr.original)>(get("FakePvr_Original"));
    fake_pvr.stats = reinterpret_cast<decltype(fake_pvr.stats)>(get("FakePvr_Stats"));
    fake_pvr.stream_mode = reinterpret_cast<decltype(fake_pvr.stream_mode)>(get("FakePvr_StreamMode"));
    fake_pvr.push = reinterpret_cast<decltype(fake_pvr.push)>(get("FakePvr_Push"));
    fake_pvr.pending = reinterpret_cast<decltype(fake_pvr.pending)>(get("FakePvr_Pending"));
    fake_pvr.after_destroy = reinterpret_cast<decltype(fake_pvr.after_destroy)>(get("FakePvr_AfterDestroy"));
    fake_pvr.reset = reinterpret_cast<decltype(fake_pvr.reset)>(get("FakePvr_Reset"));
    fake_pvr.stream_repeat = reinterpret_cast<decltype(fake_pvr.stream_repeat)>(get("FakePvr_StreamRepeat"));
    return fake_pvr.get && fake_pvr.set_provider && fake_pvr.set_time && fake_pvr.hmd && fake_pvr.original && fake_pvr.stats &&
        fake_pvr.stream_mode && fake_pvr.push && fake_pvr.pending && fake_pvr.after_destroy && fake_pvr.reset && fake_pvr.stream_repeat;
}

// After xrDestroyInstance the layer's detours stay in the table (the slots are never put back, so a later instance can
// never take a detour for the original) and pass straight through: one call through each table's end-frame slot reaches
// the client exactly once (no recursion).
bool pvr_passes_through() {
    unsigned discard[5]; fake_pvr.stats(discard);
    for (const unsigned minor : {32u, 25u})
        if (reinterpret_cast<pvr::FrameFn>(fake_pvr.get(1, minor)->slots[pvr::slot_end_frame])(fake_pvr.hmd(), 0, nullptr, 0) != pvr::success) return false;
    unsigned calls[5]{}; fake_pvr.stats(calls);
    return calls[2] == 2;
}

// ---- The engine module stand-in ---------------------------------------------------------------------------------------
// What native/dcsqvcull will do, from the layer's GazeOut: the gaze of the sampled view (2, the left focus view) in the
// head frame, a 1 degree cone (a 13 x 13 grid inside the circle) and a ring of 32 samples at 1.5 degrees, the scene's
// distance at each, the shares of near (< 1.5 m) and far (> 3 m), medians and the log histogram.
dcsvr_perception::DepthIn engine_summary(const dcsvr_perception::GazeOut& go, const Options& o, const Eyes& eyes) {
    namespace dp = dcsvr_perception;
    dp::DepthIn d{};
    d.displayTime = go.displayTime;
    const unsigned view = go.viewCount == 4 ? 2u : 0u;
    d.view = view; d.coneDeg = 1.0f; d.latencyFrames = static_cast<float>(o.latency_frames);
    const auto it = fake.located.find(go.displayTime);
    if (!(go.flags & dp::gaze_flags::valid) || it == fake.located.end()) return d;
    const Vec3 world = rotate(quat(it->second.views[view].pose.orientation), normalized({go.gazeTan[view][0], go.gazeTan[view][1], -1}));
    Quat qh; Vec3 ph; head_pose(seconds_of(go.displayTime), qh, ph);
    const Vec3 g = rotate(conjugate(qh), world);
    const Vec3 side = normalized(pupil_shift::cross(g, {0, 1, 0})), up = pupil_shift::cross(side, g);
    const auto at = [&](float ax, float ay) { return normalized(g + side * std::tan(rad(ax)) + up * std::tan(rad(ay))); };
    // The scenes are defined by direction from the head's centre; the depth buffer of the left focus view is seen from
    // the left eye. A ray from the eye is mapped to the head-centre direction of the point it hits (one fixed-point step).
    const Vec3 eye_pos{-kRuntimeIpd / 2, 0, 0};
    // Fixed point from the fixation distance: the hit's head-centre direction, the scene's distance there, again.
    const auto depth_at = [&](Vec3 ray) {
        float d = std::max(0.3f, length(eyes.point));
        for (int k = 0; k < 4; ++k) {
            const Vec3 head_dir = normalized(eye_pos + ray * d);
            d = length(head_dir * o.scene(head_dir, eyes) - eye_pos);
        }
        return d;
    };
    std::vector<float> cone, ring;
    for (int i = -6; i <= 6; ++i)
        for (int j = -6; j <= 6; ++j) {
            const float ax = i / 6.0f, ay = j / 6.0f;
            if (ax * ax + ay * ay <= 1.0001f) cone.push_back(depth_at(at(ax, ay)));
        }
    for (int k = 0; k < 32; ++k) ring.push_back(depth_at(at(1.5f * std::cos(k * 2 * kPi / 32), 1.5f * std::sin(k * 2 * kPi / 32))));
    const auto share = [](const std::vector<float>& v, bool near_) {
        return static_cast<float>(std::count_if(v.begin(), v.end(), [&](float m) { return near_ ? m < 1.5f : m > 3.0f; })) / v.size();
    };
    d.nearShare = share(cone, true); d.farShare = share(cone, false);
    d.edgeNearShare = share(ring, true); d.edgeFarShare = share(ring, false);
    std::vector<float> sorted = cone, far_samples;
    std::sort(sorted.begin(), sorted.end());
    d.medianM = sorted[sorted.size() / 2]; d.nearestM = sorted.front();
    for (float m : sorted) if (m > 3.0f) far_samples.push_back(m);
    d.farMedianM = far_samples.empty() ? 0.0f : far_samples[far_samples.size() / 2];
    for (float m : cone) {
        const int bin = std::clamp(static_cast<int>(std::log(m / dp::histogram_min_m) / std::log(dp::histogram_max_m / dp::histogram_min_m) * dp::histogram_bins), 0, dp::histogram_bins - 1);
        ++d.histogram[bin];
    }
    d.flags = dp::depth_flags::valid | (view >= 2 ? dp::depth_flags::focus_view : 0u);
    return d;
}

// One segment's expectation, checked frame by frame and when the segment ends.
struct SegmentCheck {
    int index{-1};
    Expect expect{Expect::none};
    bool armed{}, returned{};
    unsigned fixation{}, changes{}, last_fixation{};
    double ipd_min{1e9}, ipd_max{-1e9}, last_scale{1}, return_s{-1}, rise{}, fall{};
    float within{0.8f};
    double oscillation() const { return std::min(rise, fall); }
    std::string label;
};
void finish_segment(const SegmentCheck& c, Summary& sum, bool stepped = false) {
    // Stepped: the scale follows saccades, not deadlines within one scripted fixation; the stepped checks run per frame.
    if (stepped && c.expect != Expect::hold) return;
    switch (c.expect) {
    case Expect::hold:
        CHECK(c.changes == 0, ("no state change on an edge or ambiguous cone: " + c.label).c_str(), c.changes, 0, -1);
        CHECK(c.armed && c.oscillation() < 0.5, ("render IPD oscillation < 0.5 mm: " + c.label).c_str(), c.oscillation(), 0.5, -1);
        if (c.armed) { sum.hold_range_mm = std::max(sum.hold_range_mm, c.oscillation()); sum.hold_span_mm = std::max(sum.hold_span_mm, c.ipd_max - c.ipd_min); }
        sum.hold_changes += c.changes; ++sum.hold_segments;
        break;
    case Expect::x1:
        CHECK(c.returned && c.return_s <= c.within, ("back to x1 within the deadline: " + c.label).c_str(), c.return_s, c.within, -1);
        sum.x1_return_s = std::max(sum.x1_return_s, c.return_s);
        break;
    case Expect::far_:
        CHECK(c.last_fixation == 2 && c.last_scale > 1.0, ("far by the end: " + c.label).c_str(), c.last_fixation, c.last_scale, -1);
        break;
    case Expect::rise:
        CHECK(c.last_fixation == 2 && c.last_scale >= 1.2, ("far and the scale at least x1.2 by the end: " + c.label).c_str(), c.last_fixation, c.last_scale, -1);
        break;
    default: break;
    }
}

Summary run(const fs::path& dll, const fs::path& out, const Options& o) {
    scenario_name = o.name;
    Summary sum; sum.name = o.name;
    fake = Fake{};
    fake.script = &o.script;
    fake.rng.seed(o.seed);
    fake.qv_style = o.qv_style;
    fake.xr_offered = o.xr_offered;
    fake.xr_never_valid = o.xr_never_valid;
    // The fake Pimax PVR client: through its table, or through copies taken before the layer is loaded.
    std::array<std::array<void*, 4>, 2> pvr_before{};
    const pvr::Slot pvr_slots[4] = {pvr::slot_destroy_hmd, pvr::slot_get_tracking_state, pvr::slot_end_frame, pvr::slot_submit_frame};
    if (o.pvr) {
        if (!fake_pvr.module) { fail("fake PVR client loaded"); return sum; }
        fake_pvr.reset();
        fake.pvr_mode = o.pvr;
        fake.pvr_table = fake_pvr.get(1, 32);
        fake.pvr_hmd = fake_pvr.hmd();
        fake.pvr_set_time = fake_pvr.set_time;
        fake.pvr_tracking_copy = reinterpret_cast<pvr::GetTrackingStateFn>(fake.pvr_table->slots[pvr::slot_get_tracking_state]);
        fake.pvr_end_copy = reinterpret_cast<pvr::FrameFn>(fake.pvr_table->slots[pvr::slot_end_frame]);
        fake.pvr_destroy_copy = reinterpret_cast<pvr::DestroyHmdFn>(fake.pvr_table->slots[pvr::slot_destroy_hmd]);
        fake_pvr.set_provider(fake_pvr_provider);
        unsigned discard[5]; fake_pvr.stats(discard);
        for (int k = 0; k < 4; ++k) {
            pvr_before[0][k] = fake_pvr.get(1, 32)->slots[pvr_slots[k]];
            pvr_before[1][k] = fake_pvr.get(1, 25)->slots[pvr_slots[k]];
        }
    }
    // The layer's shared-memory names get this run's own suffix (never a running DCS's blocks); the harness opens the
    // same two blocks as the engine module would.
    const std::wstring suffix = L"_harness_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::wstring(o.name.begin(), o.name.end());
    SetEnvironmentVariableW(L"DCSVR_PERCEPTION_SHM_SUFFIX", suffix.c_str());
    dcsvr_perception::SharedBlock<dcsvr_perception::GazeOut> engine_gaze;
    dcsvr_perception::SharedBlock<dcsvr_perception::DepthIn> engine_depth;
    dcsvr_perception::SharedBlock<dcsvr_perception::BoostOut> engine_boost;
    dcsvr_perception::SharedBlock<dcsvr_perception::CockpitStereo> engine_stereo;
    if (o.dual_ack && !engine_stereo.open(dcsvr_perception::cockpit_stereo_block_name, suffix.c_str())) {
        fail("harness opens the cockpit stereo block"); return sum;
    }
    std::uint64_t engine_frames = 0;
    if (!engine_gaze.open(dcsvr_perception::gaze_block_name, suffix.c_str()) || !engine_depth.open(dcsvr_perception::depth_block_name, suffix.c_str()) ||
        !engine_boost.open(dcsvr_perception::boost_block_name, suffix.c_str())) {
        fail("harness opens the shared-memory blocks"); return sum;
    }
    // Local\DcsVrPupilFov_v1, as the engine module reads it (opened first here: the layer opens the existing mapping).
    dcsvr_perception::SharedBlock<dcsvr_pupil_fov::Block> engine_fov;
    if (!engine_fov.open(dcsvr_pupil_fov::block_name, suffix.c_str())) { fail("harness opens the pupil FOV block"); return sum; }
    static dcsvr_pupil_fov::Block fov_copy{};
    const auto same = [](float a, float b) { return a == b || (std::isnan(a) && std::isnan(b)); };
    const auto same_fov = [&](const float* f, const XrFovf& x) {
        return same(f[0], x.angleLeft) && same(f[1], x.angleRight) && same(f[2], x.angleUp) && same(f[3], x.angleDown);
    };
    std::deque<dcsvr_perception::DepthIn> engine_queue;
    SegmentCheck seg;
    sum.has_scene = static_cast<bool>(o.scene);
    // The layer reads the OpenXR eye gaze when the runtime offers it and the Quad Views export is not there.
    const bool xr_used = o.xr_offered && !o.qv_export;
    const fs::path folder = out / o.name;
    fs::create_directories(folder);
    const fs::path copy = folder / "XR_APILAYER_DCSVR_pupil_shift.dll";
    fs::copy_file(dll, copy, fs::copy_options::overwrite_existing);
    const fs::path ini = folder / "PupilShift.ini";
    { std::ofstream f(ini, std::ios::binary); f << ini_text(o, o.depth); }
    // The fake Quad Views is loaded before the layer, as the loader loads every layer before creating the instance.
    HMODULE qv_module{};
    if (o.qv_export) {
        qv_module = LoadLibraryW(fake_qv_dll.c_str());
        using SetProvider = void (*)(std::uint32_t (*)(std::int64_t, float*, float*));
        const auto set = qv_module ? reinterpret_cast<SetProvider>(GetProcAddress(qv_module, "FakeQv_SetProvider")) : nullptr;
        if (!set) { fail("fake Quad Views loads"); return sum; }
        set(fake_qv_gaze);
    }
    Layer layer;
    if (!load_layer(copy, layer, o.app_gaze)) { fail("layer loads"); return sum; }

    std::ofstream csv(out / (o.name + ".csv"), std::ios::binary);
    csv << "t_s,segment,true_distance_m,estimated_distance_m,fixation,source,scale,render_ipd_mm,saccade,mode,fix_source,edge,veto\n";
    struct Point { float t, scale, distance, estimate; bool cockpit, blink; };
    std::vector<Point> points;

    const XrTime frame_ns = 11'111'111;  // 90 Hz
    const float total = duration(o.script);
    const int frames = static_cast<int>(total * 90.0f);
    const float base_mm = o.ini_ipd_mm > 0 ? o.ini_ipd_mm : kRuntimeIpd * 1000;
    double last_scale = 1.0, last_processed = -1;
    float landing_cockpit = -1, landing_far = -1, saccade_until = -1, previous_distance = 0;
    float saccade_extra_mm = 0;
    std::string current_label;
    unsigned cockpit_frames = 0, cockpit_boosted = 0, settled = 0, agree = 0;
    bool far_hold_reached = false;
    XrView pending_views[4]{};
    XrTime pending_time{};
    bool pending{};

    std::map<XrTime, bool> modified;  // the layer changed at least one view for that display time
    auto submit = [&](XrTime time, const XrView* located, int frame) {
        // What DCS submits: the poses and FOVs it located, sometimes rounded differently (1e-6), sometimes one view
        // changed by DCS itself (1 cm: not ours to restore).
        std::array<XrCompositionLayerProjectionView, 4> pv{};
        fake.expect_foreign = {};
        for (int v = 0; v < 4; ++v) {
            pv[v] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            pv[v].pose = located[v].pose;
            pv[v].fov = located[v].fov;
            if (o.perturb && frame % 7 == 3) { pv[v].pose.position.x += 1e-6f; pv[v].fov.angleLeft -= 1e-6f; }
        }
        // Views the layer did not change are not its to restore: what DCS submits reaches the runtime as it is.
        if (!modified[time])
            for (int v = 0; v < 4; ++v) { fake.expect_foreign[v] = true; fake.foreign[v] = pv[v]; }
        if (o.foreign && frame % 53 == 11) {
            pv[1].pose.position.y += 0.01f;
            fake.expect_foreign[1] = true;
            fake.foreign[1] = pv[1];
        }
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.viewCount = 4;
        projection.views = pv.data();
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};  // another layer type passes through untouched
        const XrCompositionLayerBaseHeader* layers[2] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
                                                         reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = time;
        end.layerCount = 2;
        end.layers = layers;
        CHECK(layer.end(reinterpret_cast<XrSession>(static_cast<std::uintptr_t>(1)), &end) == XR_SUCCESS, "end frame succeeds", 0, 0, seconds_of(time));
        // Local\DcsVrPupilFov_v1 at xrEndFrame: the entry confirmed from what DCS submitted. The layer's views carry the
        // pair it handed out (original = the runtime's); a view DCS changed itself is an identity pair as submitted.
        if (modified[time] && dcsvr_perception::seqlock_read(engine_fov.get(), fov_copy) && fov_copy.enabled) {
            const dcsvr_pupil_fov::Entry* e = dcsvr_pupil_fov::find(fov_copy, time);
            CHECK(e && (e->flags & dcsvr_pupil_fov::entry_flags::submitted) && e->viewCount == 4, "PupilFov: entry confirmed at xrEndFrame", e != nullptr, 0, seconds_of(time));
            const RuntimeFrame& truth = fake.located[time];
            for (int v = 0; e && v < 4; ++v) {
                const auto& w = e->views[v];
                if (fake.expect_foreign[v])
                    CHECK(same_fov(w.shiftedFov, pv[v].fov) && same_fov(w.originalFov, pv[v].fov) && w.positionDelta[0] == 0 && w.positionDelta[1] == 0 &&
                        w.positionDelta[2] == 0, "PupilFov: a view DCS changed itself is an identity pair as submitted", v, 0, seconds_of(time));
                else
                    CHECK(same_fov(w.shiftedFov, located[v].fov) && same_fov(w.originalFov, truth.views[v].fov),
                        "PupilFov at xrEndFrame: shifted = located, original = the runtime's", v, 0, seconds_of(time));
            }
            ++sum.fov_submitted;
        }
    };

    // The layer's pupil gaze filter (PupilGazeFilter, unit-tested on its own), run on the same gaze, so the expected
    // pupil follows the eye's resting gaze as the layer's does.
    PupilGazeFilter expected_filter;
    PupilLock expected_lock;
    XrTime expected_filter_time = 0;
    for (int i = 0; i < frames; ++i) {
        const XrTime time = t0 + frame_ns * i;
        const float t = seconds_of(time);
        if (o.on_frame) o.on_frame(ini, i);
        const Eyes eyes = evaluate(o.script, t);
        app_sync(layer, t);
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
        info.displayTime = time;
        info.space = kBaseSpace;
        XrViewState state{XR_TYPE_VIEW_STATE};
        XrView views[4]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        std::uint32_t count = 0;
        CHECK(layer.locate(reinterpret_cast<XrSession>(static_cast<std::uintptr_t>(1)), &info, &state, 4, &count, views) == XR_SUCCESS && count == 4, "locate", 0, 0, t);
        const Status st = decode(layer.status());
        XrView again[4]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        const bool twice = o.double_locate && i % 5 == 2;
        if (twice) {
            // DCS may locate the same display time again: the same answer, the same scale.
            CHECK(layer.locate(reinterpret_cast<XrSession>(static_cast<std::uintptr_t>(1)), &info, &state, 4, &count, again) == XR_SUCCESS, "locate again", 0, 0, t);
            for (int v = 0; v < 4; ++v)
                CHECK(bitwise_equal(again[v].pose, views[v].pose) && bitwise_equal(again[v].fov, views[v].fov), "a second locate for the same time gives the same views", v, 0, t);
            CHECK(decode(layer.status()).scale == st.scale, "a second locate does not advance the scale", decode(layer.status()).scale, st.scale, t);
        }
        const RuntimeFrame& truth = fake.located[time];
        bool changed = false;
        for (int v = 0; v < 4; ++v) changed = changed || !bitwise_equal(views[v].pose, truth.views[v].pose) || !bitwise_equal(views[v].fov, truth.views[v].fov);
        modified[time] = changed;

        // The engine's dual camera stand-in: an acknowledgement per frame, fresh for the next one, except a gap of 15
        // frames (about 170 ms) every 300: the layer must ride it out (it leaves the mode only after 300 ms).
        if (o.dual_ack && (++engine_frames % 300) >= 15) {
            dcsvr_perception::CockpitStereo cs{};
            cs.flags = dcsvr_perception::cockpit_stereo_flags::active;
            cs.engineFrame = engine_frames;
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            cs.qpc = now.QuadPart;
            dcsvr_perception::seqlock_write(engine_stereo.get(), cs);
        }
        // BoostOut (the engine's dual camera reads it): written for each located display time while Depth boost is on,
        // with the scale the views carry.
        if (st.depth_enabled && !eyes.lost) {
            dcsvr_perception::BoostOut bo{};
            const bool got_boost = dcsvr_perception::seqlock_read(engine_boost.get(), bo);
            CHECK(got_boost && bo.displayTime == time && std::fabs(bo.scale - st.scale) < 1e-3 && bo.baseIpdM > 0.03f &&
                      std::fabs(bo.renderedIpdM - bo.baseIpdM * bo.scale) < 1e-5f,
                  "BoostOut written for each located display time with the applied scale", got_boost ? bo.scale : -1.0, st.scale, t);
        }
        // GazeOut (written by the layer for this display time) and the engine module stand-in.
        dcsvr_perception::GazeOut go{};
        const bool got = dcsvr_perception::seqlock_read(engine_gaze.get(), go);
        // Without Depth boost, SimulateGaze or PublishGaze the layer writes no GazeOut at all.
        if (!o.depth && !o.simulate && !o.publish_gaze && !o.on_frame)
            CHECK(!got || go.displayTime == 0, "no GazeOut without Depth boost, SimulateGaze or PublishGaze", static_cast<double>(go.displayTime), 0, t);
        if ((st.depth_enabled || o.simulate || o.publish_gaze) && !eyes.lost) {
            ++sum.gaze_out_checked;
            CHECK(got && go.displayTime == time && go.viewCount == 4 && (go.seq & 1) == 0, "GazeOut written for each located display time", got, static_cast<double>(go.displayTime - time), t);
            for (int v = 0; got && v < 4; ++v) {
                const XrFovf& f = views[v].fov;
                if (!std::isfinite(f.angleLeft + f.angleRight + f.angleUp + f.angleDown)) continue;
                const float e = std::fabs(go.fovTan[v][0] - std::tan(f.angleLeft)) + std::fabs(go.fovTan[v][1] - std::tan(f.angleRight)) +
                    std::fabs(go.fovTan[v][2] - std::tan(f.angleUp)) + std::fabs(go.fovTan[v][3] - std::tan(f.angleDown));
                CHECK(e < 1e-4f, "GazeOut fovTan = the FOV DCS renders with", e, v, t);
            }
            if (o.simulate)
                CHECK(got && (go.flags & dcsvr_perception::gaze_flags::valid) && std::fabs(go.gazeTan[0][0] - std::tan(rad(10))) < 1e-4f &&
                    std::fabs(go.gazeTan[0][1] - std::tan(rad(-5))) < 1e-4f && std::fabs(go.gazeTan[1][0] - std::tan(rad(10))) < 1e-4f,
                    "SimulateGaze: GazeOut valid with the simulated gaze in each eye's frame", go.gazeTan[0][0], std::tan(rad(10)), t);
            if (o.publish_gaze && !st.depth_enabled && eyes.mode == GazeMode::tracked)
                CHECK(got && (go.flags & dcsvr_perception::gaze_flags::valid) && std::isfinite(go.gazeTan[0][0] + go.gazeTan[1][0]),
                    "PublishGaze: GazeOut valid with a tracked gaze, Depth boost off", go.flags, 0, t);
            if (o.scene && eyes.mode == GazeMode::tracked && !o.script.combined)
                CHECK(got && (go.flags & dcsvr_perception::gaze_flags::valid), "GazeOut valid with a tracked gaze", go.flags, 0, t);
            if (o.pvr == 1 && st.depth_source && eyes.mode == GazeMode::tracked)
                CHECK(got && (go.flags & dcsvr_perception::gaze_flags::per_eye), "GazeOut per-eye with Pimax PVR", go.flags, 0, t);
            if (o.pvr != 1 && got) CHECK(!(go.flags & dcsvr_perception::gaze_flags::per_eye), "GazeOut per-eye only with Pimax PVR", go.flags, 0, t);
        }
        if (o.scene) {
            dcsvr_perception::DepthIn none{};
            none.displayTime = time;
            engine_queue.push_back(got && go.displayTime == time ? engine_summary(go, o, eyes) : none);
            while (engine_queue.size() > static_cast<std::size_t>(o.latency_frames)) {
                if (!(t >= o.stale_from && t < o.stale_to)) { dcsvr_perception::seqlock_write(engine_depth.get(), engine_queue.front()); ++sum.published; }
                engine_queue.pop_front();
            }
        }

        // Local\DcsVrPupilFov_v1 at xrLocateViews: enabled while Pupil shift or Depth boost is configured, and then an
        // entry for this display time whose shifted FOVs are what xrLocateViews returned and whose originals are the
        // runtime's; with both off (PublishGaze alone included) enabled=0 and no entries.
        {
            const bool got_fov = dcsvr_perception::seqlock_read(engine_fov.get(), fov_copy);
            CHECK(got_fov && fov_copy.magic == dcsvr_pupil_fov::magic_value && fov_copy.version == dcsvr_pupil_fov::version_value &&
                fov_copy.size == sizeof(dcsvr_pupil_fov::Block) && fov_copy.ringSize == dcsvr_pupil_fov::ring_size &&
                fov_copy.writerPid == GetCurrentProcessId(), "PupilFov: header written by the layer", got_fov, fov_copy.magic, t);
            const bool shift_on = !st.pupil_disabled || st.depth_enabled;
            CHECK(got_fov && fov_copy.enabled == (shift_on ? 1u : 0u), "PupilFov: enabled = Pupil shift or Depth boost configured", fov_copy.enabled, shift_on, t);
            if (got_fov && !fov_copy.enabled) {
                bool empty = fov_copy.newestDisplayTime == 0;
                for (const auto& e : fov_copy.entries) empty = empty && e.displayTime == 0;
                CHECK(empty, "PupilFov: no entries while disabled", static_cast<double>(fov_copy.newestDisplayTime), 0, t);
                ++sum.fov_disabled;
            }
            if (got_fov && shift_on && !eyes.lost) {
                const dcsvr_pupil_fov::Entry* e = dcsvr_pupil_fov::find(fov_copy, time);
                CHECK(e && e->viewCount == 4 && !(e->flags & dcsvr_pupil_fov::entry_flags::submitted) && fov_copy.newestDisplayTime == time,
                    "PupilFov: an entry for each located display time", e != nullptr, 0, t);
                bool any_shift = false;
                for (int v = 0; e && v < 4; ++v) {
                    const auto& w = e->views[v];
                    CHECK(same_fov(w.shiftedFov, views[v].fov), "PupilFov: shifted FOV = what xrLocateViews returned", v, w.shiftedFov[0], t);
                    CHECK(same_fov(w.originalFov, truth.views[v].fov), "PupilFov: original FOV = the runtime's", v, w.originalFov[0], t);
                    const float dp = std::fabs(w.positionDelta[0] - (views[v].pose.position.x - truth.views[v].pose.position.x)) +
                        std::fabs(w.positionDelta[1] - (views[v].pose.position.y - truth.views[v].pose.position.y)) +
                        std::fabs(w.positionDelta[2] - (views[v].pose.position.z - truth.views[v].pose.position.z));
                    CHECK(dp < 1e-7f, "PupilFov: position delta = located - runtime", dp, v, t);
                    CHECK(std::fabs(std::fabs(w.orientationDelta[3]) - 1.0f) < 1e-5f, "PupilFov: orientation delta identity", w.orientationDelta[3], v, t);
                    any_shift = any_shift || !same_fov(w.shiftedFov, truth.views[v].fov);
                }
                if (e && o.pupil && any_shift) CHECK(e->flags & dcsvr_pupil_fov::entry_flags::pupil, "PupilFov: pupil flag on a shifted FOV", e->flags, 0, t);
                ++sum.fov_located;
            }
        }

        // Expected views: runtime pose + IPD move + pupil shift (independent formulas).
        if (eyes.lost) {
            for (int v = 0; v < 4; ++v)
                CHECK(bitwise_equal(views[v].pose, truth.views[v].pose) && bitwise_equal(views[v].fov, truth.views[v].fov), "lost tracking: views untouched", v, 0, t);
        } else {
            const Vec3 l = vec(truth.views[0].pose.position), r = vec(truth.views[1].pose.position);
            const Vec3 u = normalized(r - l);
            const float target_ipd = (base_mm / 1000.0f) * static_cast<float>(o.depth || o.pupil ? st.scale : 1.0);
            const bool ipd_moves = (o.pupil || o.depth) && (o.ini_ipd_mm > 0 || st.scale != 1.0);
            const float half = ipd_moves ? 0.5f * (target_ipd - length(r - l)) : 0.0f;
            const Vec3 move[2] = {u * -half, u * half};
            const Vec3 rest = normalized(rotate(quat(truth.views[0].pose.orientation), {0, 0, -1}) + rotate(quat(truth.views[1].pose.orientation), {0, 0, -1}));
            const QvSample qs = o.qv_style ? fake.qv[time] : QvSample{};
            const float filter_dt = expected_filter_time ? std::min(static_cast<float>(static_cast<double>(time - expected_filter_time) * 1e-9), 0.1f) : 0.0f;
            expected_filter_time = time;
            // Each eye's expected gaze (world), as the layer reads it: the export's or the OpenXR eye gaze (the raw one,
            // not the offset focus centre; none without a valid gaze), else the focus views' centres.
            Vec3 gaze_world[2]{};
            bool gaze_has[2]{};
            for (int eye = 0; eye < 2; ++eye) {
                const XrFovf& ff = truth.views[eye + 2].fov;
                const bool finite = std::isfinite(ff.angleLeft + ff.angleRight + ff.angleUp + ff.angleDown);
                if (o.qv_export || xr_used) {
                    if (o.pupil && (qs.flags & kQvValid)) { gaze_world[eye] = rotate(quat(truth.views[eye].pose.orientation), qs.local[eye]); gaze_has[eye] = true; }
                } else if (o.pupil && finite) {
                    const float tx = 0.5f * (std::tan(ff.angleLeft) + std::tan(ff.angleRight)), ty = 0.5f * (std::tan(ff.angleUp) + std::tan(ff.angleDown));
                    gaze_world[eye] = rotate(quat(truth.views[eye + 2].pose.orientation), normalized({tx, ty, -1}));
                    gaze_has[eye] = true;
                }
                if (gaze_has[eye]) CHECK(std::acos(std::clamp(dot(gaze_world[eye], rest), -1.0f, 1.0f)) < rad(35.0f), "script stays inside MaxGazeDeg", 0, 0, t);
            }
            const bool real_gaze = gaze_has[0] && gaze_has[1];
            Vec3 gaze_filtered[2]{};
            bool any_gaze = false;
            if (o.pupil) {
                // As the layer: the filter gets each real gaze, and frames without one (it holds the last for 0.3 s).
                Vec3 local[2], filtered_local[2];
                for (int eye = 0; eye < 2; ++eye) local[eye] = rotate(conjugate(quat(truth.views[eye].pose.orientation)), gaze_world[eye]);
                const bool filtered_valid = expected_filter.update(real_gaze ? local : nullptr, filter_dt, 0.25f, filtered_local);
                Vec3 rest_local[2], locked_local[2];
                for (int eye = 0; eye < 2; ++eye) rest_local[eye] = rotate(conjugate(quat(truth.views[eye].pose.orientation)), rest);
                any_gaze = expected_lock.update(real_gaze ? local : nullptr, filtered_valid, filtered_local, rest_local, expected_filter.noise_deg(), filter_dt, locked_local);
                for (int eye = 0; eye < 2; ++eye) gaze_filtered[eye] = rotate(quat(truth.views[eye].pose.orientation), locked_local[eye]);
            }
            for (int eye = 0; eye < 2; ++eye) {
                const XrFovf& ff = truth.views[eye + 2].fov;
                const bool finite = std::isfinite(ff.angleLeft + ff.angleRight + ff.angleUp + ff.angleDown);
                const Vec3 offset = any_gaze ? (gaze_filtered[eye] - rest) * 0.0105f : Vec3{};
                for (int v = eye; v < 4; v += 2) {
                    const Vec3 expected = vec(truth.views[v].pose.position) + move[eye] + offset;
                    const float error = length(vec(views[v].pose.position) - expected);
                    CHECK(error < 1e-5f, "view position = runtime + IPD move + pupil shift (canted views)", error * 1000, 0, t);
                    CHECK(bitwise_equal(views[v].pose.orientation, truth.views[v].pose.orientation), "orientation untouched", v, 0, t);
                    const Vec3 d = rotate(conjugate(quat(truth.views[v].pose.orientation)), offset);
                    const XrFovf& f0 = truth.views[v].fov;
                    const float k = 1.0f / (1.0f + d.z / 1.5f);
                    const float el = std::fabs(std::atan((std::tan(f0.angleLeft) - d.x / 1.5f) * k) - views[v].fov.angleLeft);
                    const float eu = std::fabs(std::atan((std::tan(f0.angleUp) - d.y / 1.5f) * k) - views[v].fov.angleUp);
                    if (finite || v < 2) CHECK(el < 1e-5f && eu < 1e-5f, "FOV shifted for the pupil only", el, eu, t);
                    CHECK(std::isfinite(views[v].pose.position.x + views[v].pose.position.y + views[v].pose.position.z), "no NaN reaches DCS", v, 0, t);
                }
            }
            if (!o.pupil) {
                const float rendered = length(vec(views[1].pose.position) - vec(views[0].pose.position));
                CHECK(std::fabs(rendered - (ipd_moves ? target_ipd : kRuntimeIpd)) < 1e-5f, "Depth boost alone: rendered IPD = base x scale", rendered * 1000, target_ipd * 1000, t);
            }
            if (o.qv_style) {
                const double rendered_mm = length(vec(views[1].pose.position) - vec(views[0].pose.position)) * 1000.0;
                if (qs.flags & kQvValid) ++sum.qv_valid; else ++sum.qv_invalid;
                if (qs.flags & kQvSaccade) ++sum.qv_saccade;
                if (!(qs.flags & kQvValid)) {
                    // With the export or the OpenXR gaze, only once the pupil's blink hold has run out (no gaze at all).
                    if (!((o.qv_export || xr_used) && any_gaze)) {
                        sum.fixed_ipd_min = std::min(sum.fixed_ipd_min, rendered_mm);
                        sum.fixed_ipd_max = std::max(sum.fixed_ipd_max, rendered_mm);
                    }
                    // With the export or the OpenXR eye gaze, the fixed focus is no gaze: once the filter's blink hold (0.3 s)
                    // has run out no pupil moves, the IPD is exactly Ipd x scale (the runtime's IPD with Ipd=0). During the
                    // hold the views follow the held gaze, checked above.
                    if ((o.qv_export || xr_used) && !any_gaze) {
                        CHECK(std::fabs(rendered_mm - target_ipd * 1000.0) < 1e-3, "no valid gaze: rendered IPD = Ipd x scale exactly", rendered_mm, target_ipd * 1000, t);
                        for (int v = 0; v < 4; ++v) {
                            const Vec3 expected = vec(truth.views[v].pose.position) + move[v % 2];
                            CHECK(length(vec(views[v].pose.position) - expected) < 1e-6f && bitwise_equal(views[v].fov, truth.views[v].fov),
                                "no valid gaze: the eyes at rest, FOV untouched", v, 0, t);
                        }
                    }
                }
            }
        }

        // Status word.
        CHECK(st.running, "status: running", 0, 0, t);
        CHECK(st.pupil_on == o.pupil && st.pupil_disabled == !o.pupil, "status: pupil flags", st.pupil_on, o.pupil, t);
        CHECK(std::abs(static_cast<int>(st.scale_percent) - static_cast<int>(std::lround((st.scale - 1) * 100))) <= 1, "status: coarse and fine scale agree", st.scale_percent, st.scale, t);
        if (!st.depth_enabled) CHECK(st.scale == 1.0 && !st.depth_on, "status: Depth boost off means scale 1", st.scale, 0, t);
        if (st.depth_constant) sum.constant_flagged = true;
        if (st.depth_source) sum.source_proven = true;
        // (With the dual camera acknowledged the world scale needs no fixation depth: the cockpit stays at the true eyes.)
        if (!st.depth_source && !o.scene && !o.dual_ack) CHECK(st.scale == 1.0, "no proven vergence source: scale 1", st.scale, 1, t);
        if (st.fix_source == 2) ++sum.src_depth; else if (st.fix_source == 1) ++sum.src_vergence;
        if (st.edge) ++sum.edge_frames;
        if (st.veto) ++sum.veto_frames;
        sum.max_scale = std::max(sum.max_scale, st.scale);
        // Segment expectations (depth scenarios).
        if (!eyes.lost) {
            if (eyes.segment != seg.index) {
                finish_segment(seg, sum, o.stepped);
                seg = SegmentCheck{};
                seg.index = eyes.segment; seg.expect = eyes.expect; seg.label = eyes.label; seg.within = eyes.within;
            }
            const float since = t - eyes.segment_start;
            const double render_mm = st.scale * base_mm;
            seg.last_fixation = st.fixation; seg.last_scale = st.scale;
            if (seg.expect == Expect::hold && since >= 0.15f) {
                if (!seg.armed) { seg.armed = true; seg.fixation = st.fixation; seg.ipd_min = seg.ipd_max = render_mm; }
                if (st.fixation != seg.fixation) { ++seg.changes; seg.fixation = st.fixation; }
                seg.ipd_min = std::min(seg.ipd_min, render_mm); seg.ipd_max = std::max(seg.ipd_max, render_mm);
                seg.rise = std::max(seg.rise, render_mm - seg.ipd_min); seg.fall = std::max(seg.fall, seg.ipd_max - render_mm);
            }
            if (seg.expect == Expect::x1) {
                if (!seg.returned && st.scale == 1.0) { seg.returned = true; seg.return_s = since; }
                if (since >= eyes.within && !o.stepped) CHECK(st.scale == 1.0, ("x1 within the deadline of a near fixation: " + eyes.label).c_str(), st.scale, 1, t);
            }
            if (seg.expect == Expect::exact1) CHECK(st.scale == 1.0, ("scale exactly x1: " + eyes.label).c_str(), st.scale, 1, t);
        }

        // Rates: per frame, at most UpMmPerS / DownMmPerS, plus SaccadeStepMm within a saccade (and its 50 ms hold).
        // The saccade step may land up to 50 ms (the estimator's hold) + 80 ms (the landing window) after the saccade.
        if (eyes.in_saccade) saccade_until = t + 0.2f;
        // A blink (the gaze dropped or held) hides a change as a saccade does: the stepped checks treat it alike.
        if (o.stepped && eyes.mode != GazeMode::tracked) saccade_until = t + 0.2f;
        // A pursued object coming nearer than 1.5 m is a cockpit fixation from that moment.
        if (eyes.true_distance < 1.5f && previous_distance >= 1.5f && !eyes.in_saccade) landing_cockpit = t;
        previous_distance = eyes.true_distance;
        if (eyes.label != current_label) {
            if ((eyes.label.rfind("cockpit", 0) == 0 || eyes.true_distance < 1.5f) && !eyes.in_saccade && eyes.mode == GazeMode::tracked) landing_cockpit = t;
            if (eyes.label.rfind("far", 0) == 0 && !eyes.in_saccade) landing_far = t;
            if (eyes.in_saccade) saccade_extra_mm = 0;
            current_label = eyes.label;
        }
        if (!eyes.lost && st.depth_on && o.dual_ack && last_processed >= 0) {
            // The dual camera acknowledged: one world scale, approached at DualCameraMmPerS (20 mm/s) both ways, through
            // cockpit fixations and saccades alike (a one-frame mis-pairing then moves the cockpit by under 0.5 mm).
            const double delta_mm = (st.scale - last_scale) * base_mm, dt = t - last_processed, quant = 0.0002 * base_mm;
            CHECK(std::fabs(delta_mm) <= 20.0 * std::max(dt, 1.0 / 90.0) + quant, "dual camera: the scale changes at most 20 mm/s",
                  std::fabs(delta_mm) / std::max(dt, 1e-3), 20, t);
            // The world's scale follows the far fixations but never returns to 1 for the cockpit (its last far target is
            // kept there): boosted from 10 s on, through every cockpit glance of the flight.
            if (t > 10.0) CHECK(st.scale > 1.05, "dual camera: cockpit glances keep the world boosted (no return to 1)", st.scale, 1.05, t);
        }
        if (st.depth_on && !o.legacy && !o.dual_ack) CHECK(st.scale == 1.0, "no dual camera (Legacy=0): x1 throughout", st.scale, 1, t);
        if (!eyes.lost && st.depth_on && o.stepped && !o.dual_ack && o.legacy) {
            // Stepped: while fixating (outside a saccade and its window) up at most DriftMmPerS (0.5 mm/s), down at most
            // the late cockpit return (NearReturnMmPerS, 80 mm/s); within a saccade's window up at most one step
            // (SaccadeStepMm), down as far as x1 (FullNearReturn: the cockpit in one go).
            const double delta_mm = (st.scale - last_scale) * base_mm;
            const double dt = last_processed >= 0 ? t - last_processed : 1.0 / 90.0, quant = 0.0002 * base_mm;
            if (t > saccade_until) {
                if (delta_mm > 0) CHECK(delta_mm <= 0.5 * dt + quant, "stepped: no rise while fixating beyond the drift", delta_mm / dt, 0.5, t);
                else CHECK(-delta_mm <= 80.0 * dt + quant, "stepped: no fall while fixating beyond the late cockpit return", -delta_mm / dt, 80, t);
                saccade_extra_mm = 0;
            } else if (delta_mm > 0) {
                saccade_extra_mm += static_cast<float>(delta_mm);
                CHECK(saccade_extra_mm <= o.saccade_step + 0.5f * 0.3f + quant, "stepped: one step up per saccade at most", saccade_extra_mm, o.saccade_step, t);
            }
            CHECK(st.scale <= o.max_scale + 1e-4, "stepped: never above MaxScale", st.scale, o.max_scale, t);
        }
        if (!eyes.lost && st.depth_on && !o.stepped) {
            // Over the display time since the last frame the layer processed (lost tracking skips frames).
            const double delta_mm = (st.scale - last_scale) * base_mm;
            const double dt = last_processed >= 0 ? t - last_processed : 1.0 / 90.0, quant = 0.0002 * base_mm;
            const bool in_window = t <= saccade_until;
            if (delta_mm > 0) {
                const double extra = std::max(0.0, delta_mm - o.up * dt - quant);
                if (in_window) saccade_extra_mm += static_cast<float>(extra);
                CHECK(extra == 0 || (in_window && saccade_extra_mm <= o.saccade_step + quant), "ramp-up within UpMmPerS (+ saccade step)", delta_mm / dt, o.up, t);
                sum.max_up_mm_s = std::max(sum.max_up_mm_s, in_window ? 0.0 : delta_mm / dt);
            } else if (delta_mm < 0) {
                const double extra = std::max(0.0, -delta_mm - o.down * dt - quant);
                if (in_window) saccade_extra_mm += static_cast<float>(extra);
                CHECK(extra == 0 || (in_window && saccade_extra_mm <= o.saccade_step + quant), "return within DownMmPerS (+ saccade step)", -delta_mm / dt, o.down, t);
                sum.max_down_mm_s = std::max(sum.max_down_mm_s, in_window ? 0.0 : -delta_mm / dt);
            }
        }
        if (!eyes.lost && st.depth_on) {
            const double step = std::fabs(st.scale - last_scale) * base_mm;
            if (t <= saccade_until) sum.max_window_step_mm = std::max(sum.max_window_step_mm, step);
            else sum.max_step_mm = std::max(sum.max_step_mm, step);
        }
        if (!eyes.lost) { last_scale = st.scale; last_processed = t; }

        // Cockpit: the scale is 1 once the return had time (detection about 0.3 s + (max-1)*IPD/DownMmPerS).
        const bool cockpit = eyes.true_distance < 1.5f && !eyes.in_saccade;
        const float return_time = 0.45f + (o.max_scale - 1) * base_mm / o.down;
        if (cockpit && eyes.mode == GazeMode::tracked) {
            ++cockpit_frames;
            if (st.scale > 1.0) ++cockpit_boosted;
            if (landing_cockpit >= 0 && t - landing_cockpit >= return_time && !o.stepped) {
                CHECK(st.scale == 1.0, "scale 1 in the cockpit", st.scale, 1, t);
                sum.cockpit_max_settled = std::max(sum.cockpit_max_settled, st.scale);
            }
            if (eyes.label == "cockpit glance" && st.scale == 1.0 && sum.glance_return_s < 0) sum.glance_return_s = t - landing_cockpit;
        }
        // Class agreement once settled (300 ms after a landing), in tracked fixations.
        if (!eyes.in_saccade && eyes.mode == GazeMode::tracked && !eyes.lost && (!o.script.combined || o.pvr == 1) && !o.scene && st.depth_enabled &&
            ((cockpit && t - landing_cockpit > 0.3f) || (!cockpit && t - landing_far > 0.3f))) {
            ++settled;
            if (st.fixation == (cockpit ? 1u : 2u)) ++agree;
        }
        // The 10 s far fixation: the maximum is reached and then held.
        if (eyes.label == "far hold" && st.depth_source) {
            if (!far_hold_reached && std::fabs(st.scale - o.max_scale) < 1e-3) { far_hold_reached = true; sum.far_hold_reach_s = t - landing_far; }
            if (far_hold_reached) sum.far_hold_min_after = std::min(sum.far_hold_min_after, st.scale);
        }

        // Submission: usually right away; sometimes the next frame is located before this one ends (pipelined DCS).
        const XrView* submitted = twice && i % 2 ? again : views;
        if (pending) { submit(pending_time, pending_views, i - 1); pending = false; }
        if (o.pipelined && i % 11 == 5) { std::copy(submitted, submitted + 4, pending_views); pending_time = time; pending = true; }
        else submit(time, submitted, i);

        const double est = st.distance_cm == 0 ? std::nan("") : st.distance_cm >= 4095 ? 1e4 : st.distance_cm / 100.0;
        const char* mode = eyes.lost ? "lost" : eyes.mode == GazeMode::tracked ? "tracked" : eyes.mode == GazeMode::held ? "held" : eyes.mode == GazeMode::centred ? "centred" : "nan";
        char line[256];
        std::snprintf(line, sizeof line, "%.4f,%s,%.3f,%.3f,%u,%s,%.4f,%.3f,%d,%s,%s,%d,%d\n", t, eyes.label.c_str(), eyes.true_distance, est, st.fixation,
            st.depth_constant ? "constant" : st.depth_source ? "vergence" : "probing", st.scale, st.scale * base_mm, eyes.in_saccade ? 1 : 0, mode,
            st.fix_source == 2 ? "depth" : st.fix_source == 1 ? "vergence" : "none", st.edge ? 1 : 0, st.veto ? 1 : 0);
        csv << line;
        points.push_back({t, static_cast<float>(st.scale), eyes.true_distance, static_cast<float>(est), cockpit, eyes.mode != GazeMode::tracked || eyes.lost});
        ++sum.frames;
    }
    if (pending) submit(pending_time, pending_views, frames);
    finish_segment(seg, sum, o.stepped);
    // One more frame after a second of wall time, so the layer writes its once-a-second log lines.
    Sleep(1100);
    {
        app_sync(layer, -1);
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
        info.displayTime = t0 + frame_ns * frames;
        info.space = kBaseSpace;
        XrViewState state{XR_TYPE_VIEW_STATE};
        XrView views[4]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        std::uint32_t count = 0;
        layer.locate(reinterpret_cast<XrSession>(static_cast<std::uintptr_t>(1)), &info, &state, 4, &count, views);
        bool changed = false;
        for (int v = 0; v < 4; ++v) changed = changed || !bitwise_equal(views[v].pose, fake.located[info.displayTime].views[v].pose);
        modified[info.displayTime] = changed;
        submit(info.displayTime, views, frames + 1);
    }
    sum.cockpit_boosted_fraction = cockpit_frames ? static_cast<double>(cockpit_boosted) / cockpit_frames : 0;
    sum.class_agreement = settled ? static_cast<double>(agree) / settled : 1;
    sum.restored_views = fake.checked_views;
    sum.foreign_views = fake.foreign_views;
    // Without any gaze and without Ipd the layer changes nothing: every view passes through untouched (checked bitwise).
    // Likewise with Pupil shift and Depth boost both off (PublishGaze alone only reads the gaze).
    const bool untouched = (o.xr_never_valid && o.ini_ipd_mm <= 0) || (!o.pupil && !o.depth);
    if (untouched) CHECK(fake.checked_views == 0 && fake.foreign_views > 0, "no gaze, no Ipd: every view passes through", fake.checked_views, fake.foreign_views, -1);
    else CHECK(fake.checked_views > 0, "views were checked at xrEndFrame", 0, 0, -1);
    // The input side, as the runtime saw it.
    CHECK(layer.destroy_session(kSession) == XR_SUCCESS && fake.session_destroyed, "xrDestroySession reaches the runtime", 0, 0, -1);
    CHECK(layer.destroy_instance(layer.instance) == XR_SUCCESS && fake.instance_destroyed, "xrDestroyInstance reaches the runtime", 0, 0, -1);
    const auto gaze_enabled = std::count(fake.enabled.begin(), fake.enabled.end(), std::string(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME));
    CHECK(gaze_enabled == ((xr_used || o.app_gaze) ? 1 : 0), "the eye-gaze extension is enabled once when used, else not at all", static_cast<double>(gaze_enabled), xr_used, -1);
    CHECK(std::count(fake.enabled.begin(), fake.enabled.end(), std::string("XR_VARJO_quad_views")) == 1, "the application's extensions reach the runtime", 0, 0, -1);
    CHECK(fake.attach_calls == 1 && fake.attached, "one attachment, with the application's set", fake.attach_calls, 1, -1);
    CHECK(fake.syncs > 0 && fake.syncs_without_app == 0 && fake.sync_duplicates == 0, "every sync carries the application's set, no set twice", fake.syncs_without_app, fake.sync_duplicates, -1);
    if (xr_used) {
        CHECK(fake.layer_set != XR_NULL_HANDLE && fake.layer_action != XR_NULL_HANDLE, "the layer creates its gaze action", 0, 0, -1);
        CHECK(fake.attached_with_layer == 1, "the layer's set is attached with the application's", fake.attached_with_layer, 1, -1);
        CHECK(fake.syncs_with_layer == fake.syncs, "every application sync also activates the layer's set", fake.syncs_with_layer, fake.syncs, -1);
        CHECK(fake.locate_space_calls > 0 && fake.locate_space_wrong == 0, "the gaze is located in the views' base space at their display time", fake.locate_space_calls, fake.locate_space_wrong, -1);
        XrPath profile{}, simple{};
        for (const auto& [name, path] : fake.paths) {
            if (name == "/interaction_profiles/ext/eye_gaze_interaction") profile = path;
            if (name == "/interaction_profiles/khr/simple_controller") simple = path;
        }
        const auto& gaze = fake.suggested[profile];
        const bool layer_bound = std::any_of(gaze.begin(), gaze.end(), [](const XrActionSuggestedBinding& b) { return b.action == fake.layer_action; });
        const bool app_bound = std::any_of(gaze.begin(), gaze.end(), [](const XrActionSuggestedBinding& b) { return b.action != fake.layer_action; });
        CHECK(layer_bound, "the eye-gaze profile carries the layer's binding", static_cast<double>(gaze.size()), 0, -1);
        CHECK(app_bound == o.app_gaze, "the application's own eye-gaze binding is kept (merged), none added for it", app_bound, o.app_gaze, -1);
        CHECK(fake.suggested[simple].size() == 1, "the controller suggestion reaches the runtime unchanged", static_cast<double>(fake.suggested[simple].size()), 1, -1);
    } else {
        CHECK(fake.layer_set == XR_NULL_HANDLE && fake.attached_with_layer == 0 && fake.syncs_with_layer == 0 && fake.locate_space_calls == 0,
            "without the OpenXR eye gaze the layer leaves input alone", fake.attached_with_layer, fake.syncs_with_layer, -1);
    }
    if (o.pvr) {
        // The runtime's handle only, queried only through the table; the patched slots stay patched, passing through.
        unsigned calls[5]{};
        fake_pvr.stats(calls);
        CHECK(calls[1] == 0, "PVR eye tracking queried only with the runtime's own handle", calls[1], 0, -1);
        if (o.pvr == 1) CHECK(calls[0] > 0, "PVR eye tracking queried", calls[0], 0, -1);
        else CHECK(calls[0] == 0, "a runtime with its own copy of the table: the layer never queries PVR", calls[0], 0, -1);
        CHECK(calls[4] == 1, "the runtime's destroyHmd reaches the client", calls[4], 1, -1);
        for (int k = 0; k < 4; ++k) {
            const bool patched = fake_pvr.get(1, 32)->slots[pvr_slots[k]] != pvr_before[0][k] && fake_pvr.get(1, 25)->slots[pvr_slots[k]] != pvr_before[1][k];
            if (pvr_slots[k] == pvr::slot_get_tracking_state) CHECK(!patched, "PVR get_tracking_state is never patched", k, 0, -1);
            else CHECK(patched, "PVR interface slots stay patched after xrDestroyInstance", k, 0, -1);
            CHECK(pvr_before[0][k] == fake_pvr.original(static_cast<std::uint32_t>(pvr_slots[k])), "PVR slots were the client's own before the run", k, 0, -1);
        }
        CHECK(pvr_passes_through(), "after xrDestroyInstance the PVR detours pass straight through", 0, 0, -1);
        fake.pvr_mode = 0;
    }
    // Local\DcsVrPupilFov_v1 over the run, and after xrDestroyInstance (the layer closed it: enabled 0, no entries).
    const bool any_modified = std::any_of(modified.begin(), modified.end(), [](const auto& m) { return m.second; });
    if (o.pupil || o.depth) CHECK(sum.fov_located > 0 && (sum.fov_submitted > 0) == any_modified, "PupilFov: entries checked at xrLocateViews (and xrEndFrame when views were shifted)", sum.fov_located, sum.fov_submitted, -1);
    else CHECK(sum.fov_disabled > 0 && sum.fov_located == 0 && sum.fov_submitted == 0, "PupilFov: shift off, enabled=0 throughout", sum.fov_disabled, sum.fov_located, -1);
    {
        const bool got = dcsvr_perception::seqlock_read(engine_fov.get(), fov_copy);
        CHECK(got && !fov_copy.enabled && fov_copy.newestDisplayTime == 0 && fov_copy.entries[0].displayTime == 0,
            "PupilFov: disabled and cleared at xrDestroyInstance", got, fov_copy.enabled, -1);
    }
    engine_fov.close();
    engine_gaze.close();
    engine_depth.close();
    FreeLibrary(layer.module);
    if (qv_module) FreeLibrary(qv_module);

    // The layer's own log: the depth line and, without the pupil correction, the reason in the stats line.
    std::ifstream log(folder / "PupilShift.log");
    const std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
    CHECK(text.find("depth settings: enabled=") != std::string::npos, "log: depth settings line", 0, 0, -1);
    if (o.depth && !o.on_frame) CHECK(text.find("depth: ") != std::string::npos && text.find("IPD base") != std::string::npos, "log: depth line", 0, 0, -1);
    if (!o.pupil) CHECK(text.find("(pupil shift disabled)") != std::string::npos, "log: pupil shift disabled", 0, 0, -1);
    if (!untouched) CHECK(text.find("end_frame: restored") != std::string::npos, "log: end_frame restored", 0, 0, -1);
    CHECK(text.find("; gaze source=") != std::string::npos, "log: the gaze source in the stats line", 0, 0, -1);
    if (o.depth && !o.on_frame) CHECK(std::regex_search(text, std::regex("depth: \\S+ source=\\w+ vergence=\\w+ fixation=\\w+ .*scene mode=" + o.mode + " fresh=")), "log: depth line with the scene segment", 0, 0, -1);
    if (o.scene) CHECK(std::regex_search(text, std::regex("scene mode=\\w+ fresh=[1-9]")), "log: fresh depth summaries counted", 0, 0, -1);
    if (o.depth && !o.scene && !o.on_frame) CHECK(std::regex_search(text, std::regex("fresh=0 stale=0 moved=0 absent=[1-9]")), "log: no engine module: summaries absent", 0, 0, -1);
    if (o.depth || o.simulate || o.publish_gaze) CHECK(text.find("perception: shared memory") != std::string::npos && text.find("FAILED") == std::string::npos, "log: shared memory open", 0, 0, -1);
    if (o.pvr == 1) {
        CHECK(text.find("pvr: Pimax PVR client found (interface 1.32, 2 table(s), 6 slot(s) observed)") != std::string::npos, "log: PVR client found, both tables", 0, 0, -1);
        CHECK(text.find("pvr: the runtime's PVR session found") != std::string::npos, "log: PVR session found", 0, 0, -1);
        CHECK(std::regex_search(text, std::regex("pvr valid=[1-9]\\d* invalid=[1-9]")), "log: PVR valid and invalid queries counted", 0, 0, -1);
        CHECK(text.find("pvr: session forgotten (6 interface slot(s) stay patched") != std::string::npos, "log: PVR session forgotten, slots stay patched", 0, 0, -1);
    } else if (o.pvr == 2) {
        CHECK(text.find("pvr: no runtime session seen through the interface table") != std::string::npos, "log: copied table, no session", 0, 0, -1);
    }
    if (o.qv_export && o.xr_offered)
        CHECK(text.find("not used (the bundled Quad Views' export has the gaze)") != std::string::npos, "log: export wins, no eye-gaze extension", 0, 0, -1);
    if (xr_used) {
        CHECK(text.find(o.app_gaze ? "XR_EXT_eye_gaze_interaction enabled by the application" : "XR_EXT_eye_gaze_interaction added by the layer") != std::string::npos,
            "log: extension", 0, 0, -1);
        CHECK(text.find("gaze: OpenXR eye gaze in use") != std::string::npos, "log: OpenXR eye gaze in use", 0, 0, -1);
        CHECK(std::regex_search(text, std::regex("xr invalid [1-9]")), "log: frames without a valid OpenXR gaze counted", 0, 0, -1);
        CHECK(std::regex_search(text, std::regex("focus 0, simulated 0")) && !std::regex_search(text, std::regex("focus [1-9]")), "log: the focus views are never read", 0, 0, -1);
        CHECK(text.find("WARNING") == std::string::npos, "log: no fallback warning", 0, 0, -1);
        if (o.xr_never_valid) {
            CHECK(text.find("shifted=0 (no valid gaze)") != std::string::npos && text.find("gaze source=none") != std::string::npos, "log: never valid, no gaze", 0, 0, -1);
            CHECK(!std::regex_search(text, std::regex("xr [1-9]\\d*, focus")), "log: no xr frames", 0, 0, -1);
        } else if (o.pupil) {
            CHECK(std::regex_search(text, std::regex("\\(qv 0, xr [1-9]")), "log: source xr frames counted", 0, 0, -1);
        }
    } else if (o.qv_export) {
        CHECK(text.find("Quad Views gaze export found") != std::string::npos, "log: export found", 0, 0, -1);
        if (o.pupil) CHECK(text.find("gaze source=qv") != std::string::npos || text.find("gaze source=none") != std::string::npos, "log: source qv", 0, 0, -1);
        CHECK(std::regex_search(text, std::regex("\\(qv [1-9]")), "log: qv frames counted", 0, 0, -1);
        CHECK(std::regex_search(text, std::regex("qv invalid [1-9]")), "log: frames without a valid gaze counted", 0, 0, -1);
        CHECK(std::regex_search(text, std::regex("saccade [1-9]")), "log: Quad Views' saccade frames counted", 0, 0, -1);
        CHECK(std::regex_search(text, std::regex("focus 0, simulated 0")) && !std::regex_search(text, std::regex("focus [1-9]")), "log: the focus views are never read", 0, 0, -1);
    } else {
        CHECK(text.find("no bundled Quad Views: OpenXR eye gaze, else the focus views") != std::string::npos, "log: no export", 0, 0, -1);
        CHECK(text.find("not used (the runtime does not offer XR_EXT_eye_gaze_interaction)") != std::string::npos, "log: extension not offered", 0, 0, -1);
        if (o.pupil) {
            CHECK(std::regex_search(text, std::regex("\\(qv 0, xr 0, focus [1-9]")), "log: source focus", 0, 0, -1);
            CHECK(text.find("gaze: WARNING reading the gaze from the focus views' centres") != std::string::npos, "log: fallback warning", 0, 0, -1);
        }
    }

    // SVG: scale over time (top) and the estimated vs true fixation distance on a log scale (bottom).
    std::ofstream svg(out / (o.name + ".svg"), std::ios::binary);
    const float W = 1000, H = 420, x0 = 50, x1 = W - 10;
    auto X = [&](float t) { return x0 + (x1 - x0) * t / std::max(total, 0.001f); };
    auto Ys = [&](double s) { return 20 + 160 * (2.0 - std::clamp(s, 1.0, 2.0)) / 1.0 * 1.0; };
    auto Yd = [&](double m) { const double l = std::log10(std::clamp(m, 0.2, 10000.0)); return 230 + 170 * (4.0 - l) / (4.0 - std::log10(0.2)); };
    svg << "<svg xmlns='http://www.w3.org/2000/svg' width='" << W << "' height='" << H << "' font-family='sans-serif' font-size='11'>\n";
    svg << "<rect width='100%' height='100%' fill='white'/>\n<text x='" << x0 << "' y='14'>" << o.name << ": " << o.description << "</text>\n";
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (points[i].cockpit) svg << "<rect x='" << X(points[i - 1].t) << "' y='20' width='" << X(points[i].t) - X(points[i - 1].t) + 0.5 << "' height='380' fill='#e8f0ff'/>";
        if (points[i].blink) svg << "<rect x='" << X(points[i - 1].t) << "' y='20' width='" << X(points[i].t) - X(points[i - 1].t) + 0.5 << "' height='380' fill='#eeeeee'/>";
    }
    svg << "\n";
    for (double s : {1.0, 1.2, 1.4, 1.6}) svg << "<line x1='" << x0 << "' x2='" << x1 << "' y1='" << Ys(s) << "' y2='" << Ys(s) << "' stroke='#ccc'/><text x='5' y='" << Ys(s) + 4 << "'>x" << s << "</text>\n";
    for (double m : {0.5, 1.5, 10.0, 100.0, 1000.0}) svg << "<line x1='" << x0 << "' x2='" << x1 << "' y1='" << Yd(m) << "' y2='" << Yd(m) << "' stroke='#ccc'/><text x='5' y='" << Yd(m) + 4 << "'>" << m << " m</text>\n";
    auto polyline = [&](auto value, const char* colour) {
        svg << "<polyline fill='none' stroke='" << colour << "' stroke-width='1.2' points='";
        for (const auto& p : points) { const double v = value(p); if (std::isfinite(v)) svg << X(p.t) << ',' << v << ' '; }
        svg << "'/>\n";
    };
    polyline([&](const Point& p) { return Ys(p.scale); }, "#c02020");
    polyline([&](const Point& p) { return Yd(p.distance); }, "#888888");
    polyline([&](const Point& p) { return std::isfinite(p.estimate) ? Yd(p.estimate) : std::nan(""); }, "#2050c0");
    svg << "<text x='" << x0 + 5 << "' y='34' fill='#c02020'>render IPD scale</text><text x='" << x0 + 5 << "' y='244' fill='#2050c0'>estimated fixation distance</text>"
        << "<text x='" << x0 + 180 << "' y='244' fill='#888'>true distance</text><text x='" << x0 + 290 << "' y='244' fill='#5070b0'>blue band: cockpit, grey: no fresh gaze</text>\n</svg>\n";
    return sum;
}

// ---- Scripts --------------------------------------------------------------------------------------------------------
Segment fix(std::string label, float seconds, Vec3 target, GazeMode mode = GazeMode::tracked) { return {std::move(label), seconds, target, false, mode, false}; }
Segment jump(std::string label, Vec3 target, float seconds = 0.045f) { return {std::move(label), seconds, target, true, GazeMode::tracked, false}; }

// A flight: instruments, look out, back, a 10 s far fixation (tanker), a quick glance at the instruments, blinks with
// held and dropped gaze, lost tracking, a NaN focus FOV, a long cockpit stretch.
std::vector<Segment> flight() {
    return {
        fix("cockpit start", 3.0f, cockpit(-10, -25)),
        jump("far saccade", outside(10, 2)), fix("far look", 2.5f, outside(10, 2)),
        jump("cockpit saccade", cockpit(5, -30)), fix("cockpit panel", 2.5f, cockpit(5, -30)),
        jump("far saccade", outside(-20, 5)), fix("far look", 1.5f, outside(-20, 5)),
        jump("far saccade", outside(5, 0, 300)), fix("far hold", 10.0f, outside(5, 0, 300)),
        jump("cockpit saccade", cockpit(-5, -20)), fix("cockpit glance", 1.5f, cockpit(-5, -20)),
        jump("far saccade", outside(5, 0, 300)), fix("far wingman", 3.0f, outside(5, 0, 300)),
        fix("far blink (held)", 0.15f, outside(5, 0, 300), GazeMode::held), fix("far wingman", 1.0f, outside(5, 0, 300)),
        fix("far blink (dropped)", 0.3f, outside(5, 0, 300), GazeMode::centred), fix("far wingman", 1.0f, outside(5, 0, 300)),
        {"far tracking lost", 0.3f, outside(5, 0, 300), false, GazeMode::tracked, true}, fix("far wingman", 1.0f, outside(5, 0, 300)),
        fix("far no gaze (nan)", 0.1f, outside(5, 0, 300), GazeMode::nan_fov), fix("far wingman", 1.0f, outside(5, 0, 300)),
        fix("far gaze lost 1.5 s", 1.5f, outside(5, 0, 300), GazeMode::held), fix("far wingman", 2.0f, outside(5, 0, 300)),
        jump("cockpit saccade", cockpit(15, -15)), fix("cockpit long", 4.0f, cockpit(15, -15)),
        fix("cockpit blink (dropped)", 0.3f, cockpit(15, -15), GazeMode::centred), fix("cockpit long", 3.0f, cockpit(15, -15)),
        jump("far saccade", outside(-10, 3, 50)), fix("far tanker 50 m", 6.0f, outside(-10, 3, 50)),
        jump("cockpit saccade", cockpit(0, -28, 0.6f)), fix("cockpit end", 3.0f, cockpit(0, -28, 0.6f)),
    };
}


// ---- Depth scenes (head frame; the engine stand-in samples them around the published gaze) ------------------------------
constexpr float kSky = 20000.0f;
float yaw_of(Vec3 d) { return std::atan2(d.x, -d.z) * 57.29578f; }
float pitch_of(Vec3 d) { return std::atan2(d.y, -d.z) * 57.29578f; }
Segment expect(Segment s, Expect e, float within = 0.8f) { s.expect = e; s.within = within; return s; }
Segment pursue(std::string label, float seconds, Vec3 target) { Segment s{std::move(label), seconds, target}; s.pursuit = true; return s; }
// The instrument panel: everything below 12 degrees down is 0.7 m away.
float panel_or(Vec3 d, float otherwise) { return pitch_of(d) < -12.0f ? 0.7f : otherwise; }

// A wingman at 80 m (a disc of 4 degrees radius at 16 deg right, 2 deg up) behind a canopy strut at 0.6 m (9-13 deg).
float canopy_scene(Vec3 d, const Eyes&) {
    const float yaw = yaw_of(d), pitch = pitch_of(d);
    if (yaw >= 9.0f && yaw <= 13.0f && pitch > -12.0f) return 0.6f;
    if ((yaw - 16) * (yaw - 16) + (pitch - 2) * (pitch - 2) < 16.0f) return 80.0f;
    return panel_or(d, kSky);
}
std::vector<Segment> canopy_script(bool realistic) {
    const Expect far_e = realistic ? Expect::exact1 : Expect::far_, hold = realistic ? Expect::exact1 : Expect::hold;
    const Expect x1 = realistic ? Expect::exact1 : Expect::x1, start = realistic ? Expect::exact1 : Expect::none;
    return {
        expect(fix("cockpit start", 2.0f, cockpit(-10, -25)), start),
        jump("far saccade", outside(16, 2, 80)), expect(fix("far wingman 80 m", 7.0f, outside(16, 2, 80)), far_e),
        // 2.8 degrees to the strut's edge (13 deg): the gaze jitters across it (0.8 deg noise + 0.5 deg drift per eye).
        jump("far step", outside(13.2f, 2, 80)), expect(fix("far wingman at the canopy strut's edge", 5.0f, outside(13.2f, 2, 80)), hold),
        jump("cockpit saccade", cockpit(0, -25)), expect(fix("cockpit after the wingman", 2.0f, cockpit(0, -25)), x1),
        jump("far saccade", outside(16, 2, 80)), expect(fix("far wingman again", 3.0f, outside(16, 2, 80)), far_e),
    };
}

// The glareshield (0.8 m) below 6 degrees down, the sky above.
float glareshield_scene(Vec3 d, const Eyes&) { return pitch_of(d) < -6.0f ? 0.8f : kSky; }
std::vector<Segment> glareshield_script() {
    return {
        fix("cockpit start", 2.0f, cockpit(-5, -20)),
        jump("far saccade", outside(5, 4)), expect(fix("far sky", 3.0f, outside(5, 4)), Expect::far_),
        jump("far saccade to the edge", outside(5, -6)), expect(fix("far glareshield edge (from the sky)", 4.0f, outside(5, -6)), Expect::hold),
        jump("cockpit saccade", cockpit(0, -25)), expect(fix("cockpit panel", 2.0f, cockpit(0, -25)), Expect::x1),
        jump("cockpit saccade to the edge", cockpit(5, -6, 0.8f)), expect(fix("cockpit glareshield edge (from the panel)", 4.0f, cockpit(5, -6, 0.8f)), Expect::hold),
        jump("far saccade", outside(5, 4)), expect(fix("far sky again", 3.0f, outside(5, 4)), Expect::far_),
    };
}

// An MFD and its bezel (0.65 m) left of 5 degrees left, below 10 degrees down; the ground (1500 m) elsewhere below; sky.
float mfd_scene(Vec3 d, const Eyes&) {
    const float yaw = yaw_of(d), pitch = pitch_of(d);
    if (pitch < -10.0f) return yaw < -5.0f ? 0.65f : 1500.0f;
    return kSky;
}
std::vector<Segment> mfd_script() {
    return {
        fix("cockpit MFD", 2.0f, cockpit(-15, -25, 0.65f)),
        jump("far saccade", outside(10, 3)), fix("far look", 2.0f, outside(10, 3)),
        jump("cockpit saccade", cockpit(-15, -25, 0.65f)), expect(fix("cockpit MFD again", 2.0f, cockpit(-15, -25, 0.65f)), Expect::x1),
        jump("cockpit saccade to the bezel edge", cockpit(-5, -25, 0.65f)), expect(fix("cockpit MFD bezel edge (from the MFD)", 4.0f, cockpit(-5, -25, 0.65f)), Expect::hold),
        jump("far saccade", outside(10, -25, 1500)), expect(fix("far ground", 4.0f, outside(10, -25, 1500)), Expect::far_),
        jump("far saccade to the bezel edge", outside(-5, -25, 1500)), expect(fix("far MFD bezel edge (from the ground)", 4.0f, outside(-5, -25, 1500)), Expect::hold),
        jump("cockpit saccade", cockpit(-15, -25, 0.65f)), expect(fix("cockpit MFD at the end", 2.0f, cockpit(-15, -25, 0.65f)), Expect::x1),
    };
}

// HUD symbology: the combiner written to the depth buffer at 0.7 m (8 deg either side, 4 deg down to 10 deg up), the
// eyes focused at infinity (the symbols are collimated); and the opposite: nothing near in the depth buffer while the
// eyes converge at 0.7 m (a near thing seen through glass, or symbology the depth buffer does not have).
float hud_glass_scene(Vec3 d, const Eyes&) {
    const float yaw = yaw_of(d), pitch = pitch_of(d);
    if (yaw > -8 && yaw < 8 && pitch > -4 && pitch < 10) return 0.7f;
    return panel_or(d, kSky);
}
float sky_scene(Vec3 d, const Eyes&) { return panel_or(d, kSky); }
std::vector<Segment> hud_script(bool eyes_near) {
    const Vec3 symbol = eyes_near ? cockpit(0, 3, 0.7f) : outside(0, 3);
    return {
        fix("cockpit start", 2.0f, cockpit(-10, -25)),
        jump("far saccade", outside(25, 3)), expect(fix("far outside", 4.0f, outside(25, 3)), Expect::far_),
        jump("hud saccade", symbol), expect(fix(eyes_near ? "HUD (depth far, eyes near)" : "HUD (depth near, eyes far)", 3.0f, symbol), Expect::x1),
        jump("far saccade", outside(25, 3)), expect(fix("far outside again", 4.0f, outside(25, 3)), Expect::far_),
        jump("hud saccade", symbol), expect(fix(eyes_near ? "HUD again (depth far, eyes near)" : "HUD again (depth near, eyes far)", 3.0f, symbol), Expect::x1),
    };
}

// Smooth pursuit of an object (a disc of 3 degrees) moving from 1 m to 300 m: no saccade, the class changes once.
float object_scene(Vec3 d, const Eyes& eyes) {
    const Vec3 o = normalized(eyes.point);
    if (std::acos(std::clamp(dot(normalized(d), o), -1.0f, 1.0f)) * 57.29578f < 3.0f) return length(eyes.point);
    return panel_or(d, kSky);
}
std::vector<Segment> pursuit_script() {
    return {
        fix("cockpit start", 2.0f, cockpit(-10, -25)),
        jump("far saccade", outside(15, 3)), fix("far look", 2.0f, outside(15, 3)),
        jump("cockpit saccade", cockpit(-10, 0, 1.0f)), fix("cockpit object at 1 m", 2.0f, cockpit(-10, 0, 1.0f)),
        pursue("pursuit 1 m to 300 m", 6.0f, outside(10, 0, 300)), expect(fix("far object at 300 m", 3.0f, outside(10, 0, 300)), Expect::far_),
        pursue("pursuit back to 1 m", 6.0f, cockpit(-10, 0, 1.0f)), expect(fix("cockpit object back at 1 m", 1.5f, cockpit(-10, 0, 1.0f)), Expect::x1, 1.0f),
    };
}

// A blink (the provider holds its gaze, 150 ms) in the middle of each change.
std::vector<Segment> blink_script() {
    return {
        fix("cockpit start", 2.0f, cockpit(-10, -25)),
        jump("far saccade", outside(10, 3)), fix("far look", 2.0f, outside(10, 3)),
        jump("cockpit saccade", cockpit(0, -25)), fix("cockpit panel", 2.0f, cockpit(0, -25)),
        jump("far saccade", outside(10, 3)), fix("far landing", 0.06f, outside(10, 3)),
        fix("far blink (held) while confirming", 0.15f, outside(10, 3), GazeMode::held), expect(fix("far after the blink", 6.0f, outside(10, 3)), Expect::far_),
        jump("cockpit saccade", cockpit(0, -25)), fix("cockpit landing", 0.05f, cockpit(0, -25)),
        fix("cockpit blink (held) while returning", 0.15f, cockpit(0, -25), GazeMode::held),
        // The near fixation is measurable from the blink's end: within 0.8 s of it.
        expect(fix("cockpit after the blink", 2.0f, cockpit(0, -25)), Expect::x1, 0.8f),
    };
}

// A long far fixation during which the engine stops publishing (stale) for 2.5 s, then resumes.
std::vector<Segment> stale_script() {
    return {
        fix("cockpit start", 2.0f, cockpit(-10, -25)),
        jump("far saccade", outside(10, 3)), fix("far look", 2.0f, outside(10, 3)),
        jump("cockpit saccade", cockpit(0, -25)), fix("cockpit panel", 2.0f, cockpit(0, -25)),
        jump("far saccade", outside(10, 3)), expect(fix("far with depth", 6.0f, outside(10, 3)), Expect::far_),
        fix("far depth stale", 2.5f, outside(10, 3)), expect(fix("far depth back", 4.0f, outside(10, 3)), Expect::far_),
        jump("cockpit saccade", cockpit(0, -25)), expect(fix("cockpit at the end", 2.0f, cockpit(0, -25)), Expect::x1),
    };
}
// Its time window: the "far depth stale" segment.
float segment_start(const std::vector<Segment>& s, const std::string& label) {
    float t = 0;
    for (const auto& seg : s) { if (seg.label == label) return t; t += seg.seconds; }
    return -1;
}

// Realistic: a far target at 500 m (a disc of 4 degrees), the rest of the world far beyond as well.
float target_scene(Vec3 d, const Eyes&) {
    const float yaw = yaw_of(d), pitch = pitch_of(d);
    if ((yaw - 10) * (yaw - 10) + (pitch - 3) * (pitch - 3) < 16.0f) return 500.0f;
    return panel_or(d, kSky);
}
std::vector<Segment> realistic_target_script() {
    return {
        fix("cockpit start", 2.0f, cockpit(-10, -25)),
        jump("far saccade", outside(10, 3, 500)), expect(fix("far target 500 m", 7.0f, outside(10, 3, 500)), Expect::rise),
        jump("cockpit saccade", cockpit(0, -25)), expect(fix("cockpit after the target", 2.0f, cockpit(0, -25)), Expect::x1),
    };
}

// ---- The predictive transition (predictive_depth.hpp) --------------------------------------------------------------------
// The layer under the fake runtime at 45 fps, DCS locating two frames before the display time. Pimax PVR's per-eye gaze
// is a 500 Hz stream (fake_pvr.cpp, stream mode) that the layer's own sampler thread polls; the harness queues each
// frame's samples up to the locate time and waits until the sampler has taken them all, so the wall-clock thread sees
// exactly the simulated stream. The engine stand-in publishes the cone summary and the 32 x 32 depth grid of the left
// periphery (view 0) 2 frames late. Saccades follow the main sequence (smoothstep profile, peak velocity Vp(A) times
// peak_scale, so the duration is 1.5 A / Vp), from a fixation to a fixation.
float predict_scene(Vec3 d, const Eyes&) {
    const float yaw = yaw_of(d), pitch = pitch_of(d);
    if (pitch < -10.0f) return 0.7f;                         // the instrument panel
    if (yaw >= 9.0f && yaw <= 13.0f) return 0.6f;            // a canopy frame strut
    return kSky;
}
float angle_between_deg(Vec3 a, Vec3 b) { return std::acos(std::clamp(dot(normalized(a), normalized(b)), -1.0f, 1.0f)) * 57.29578f; }
// A main-sequence saccade from `from` to `to`; peak_scale > 1: faster than the main sequence (a misprediction).
Segment msaccade(std::string label, Vec3 from, Vec3 to, float peak_scale = 1.0f) {
    const float a = angle_between_deg(from, to);
    return jump(std::move(label), to, 1.5f * a / (pupil_shift::predictive::main_sequence::peak_velocity(a) * peak_scale));
}

struct PredOptions {
    std::string name, description;
    std::vector<Segment> segments;
    std::function<float(Vec3, const Eyes&)> scene{predict_scene};
    std::string transition{"predictive"};
    float fps{45.0f}, lead_frames{0.5f}, pvr_hz{500.0f}, noise_deg{0.15f}, phase_s{0.0f};
    unsigned pvr_repeat{1};  // calls each tracker sample answers (a 120 Hz tracker polled at 500 Hz repeats it ~4 times)
    bool pvr{true}, grid{true}, grid_log_mean{true};  // the engine's typicalM is the log mean (geometric) of the cell
    bool in_flight_check{true};      // every step must be located before the saccade's true end
    std::function<void(const fs::path& ini, int frame)> on_frame;
};
struct PredSaccade { float onset{}, end{}, amplitude{}; std::string label; };
struct Jump { double t_loc{}, t_disp{}, mm{}; int saccade{-1}; double after_onset{}, before_end{}; XrTime time{}; };
struct PredFrame { double t_loc{}, scale{}; int saccade{-1}; bool in_saccade{}, near_{}; unsigned fixation{}; bool predictive{}, fallback{}; };
struct PredSummary {
    std::string name;
    std::vector<PredSaccade> saccades;
    std::vector<Jump> jumps;
    std::vector<PredFrame> frames;
    std::map<std::string, unsigned> log;  // the predict: lines' counters, summed
    double max_scale{1};
    bool sampler_started{}, sampler_stopped{}, fallback_logged{};
};

std::string predict_ini(const PredOptions& o, const std::string& transition) {
    std::ostringstream s;
    s << "[PupilShift]\nEnabled=0\nToggle=121:5\nIpd=64\n\n[DepthBoost]\nEnabled=1\nToggle=121:6\nMode=realistic\nMaxScale=1.25\nFarStartM=150\nFarFullM=400\n"
      << "NearM=1.5\nUpMmPerS=5\nDownMmPerS=40\nSaccadeStepMm=4\nRecoverMs=800\nHoldMs=500\nStepped=0\nLegacy=1\nTransition=" << transition
      << "\nTransitionToggle=120:6\nPredictLeadFrames=" << o.lead_frames << "\nPredictPollHz=1000\nTrace=1\n";
    return s.str();
}

// The depth grid the engine module would publish for a GazeOut: view 0 (the left periphery, its whole FOV as DCS renders
// it), 32 x 32 cells of 3 x 3 samples seen from the left eye: the share nearer than 1.5 m, the nearest, and the median
// (or the log mean) distance.
dcsvr_perception::DepthGrid engine_grid(const dcsvr_perception::GazeOut& go, const PredOptions& o, const Eyes& eyes) {
    namespace dp = dcsvr_perception;
    dp::DepthGrid g{};
    g.displayTime = go.displayTime; g.view = 0; g.w = g.h = 32;
    const auto it = fake.located.find(go.displayTime);
    if (it == fake.located.end()) return g;
    for (int k = 0; k < 4; ++k) g.fovTan[k] = go.fovTan[0][k];
    const Quat q0 = quat(it->second.views[0].pose.orientation);
    Quat qh; Vec3 ph; head_pose(seconds_of(go.displayTime), qh, ph);
    const Vec3 eye_pos{-kRuntimeIpd / 2, 0, 0};
    const auto depth_at = [&](Vec3 ray) {
        float d = std::max(0.3f, length(eyes.point));
        for (int k = 0; k < 4; ++k) {
            const Vec3 head_dir = normalized(eye_pos + ray * d);
            d = length(head_dir * o.scene(head_dir, eyes) - eye_pos);
        }
        return d;
    };
    for (int row = 0; row < 32; ++row)
        for (int col = 0; col < 32; ++col) {
            float v[9]; int n = 0, near_n = 0; float nearest = 1e9f; double logs = 0;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    const float tx = g.fovTan[0] + (col + (i + 0.5f) / 3) * (g.fovTan[1] - g.fovTan[0]) / 32;
                    const float ty = g.fovTan[2] - (row + (j + 0.5f) / 3) * (g.fovTan[2] - g.fovTan[3]) / 32;
                    const Vec3 ray = rotate(conjugate(qh), rotate(q0, normalized({tx, ty, -1})));
                    const float m = depth_at(ray);
                    v[n++] = m; nearest = std::min(nearest, m); logs += std::log(m); if (m < 1.5f) ++near_n;
                }
            std::sort(v, v + n);
            const int c = row * 32 + col;
            g.nearestM[c] = nearest;
            g.typicalM[c] = o.grid_log_mean ? static_cast<float>(std::exp(logs / n)) : v[n / 2];
            g.nearShare[c] = static_cast<std::uint8_t>(std::lround(255.0f * near_n / n));
        }
    g.flags = dp::grid_flags::valid;
    return g;
}

PredSummary run_predictive(const fs::path& dll, const fs::path& out, const PredOptions& o) {
    scenario_name = o.name;
    PredSummary sum; sum.name = o.name;
    Script script; script.segments = o.segments; script.noise_deg = o.noise_deg; script.drift_deg = 0;
    fake = Fake{};
    fake.script = &script;
    fake.rng.seed(4242);
    const double F = 1.0 / o.fps;
    fake.display_lead_s = static_cast<float>(2 * F);
    {
        float start = 0; Vec3 prev = script.segments.front().target;
        for (const auto& seg : script.segments) {
            if (seg.saccade) sum.saccades.push_back({start, start + seg.seconds, angle_between_deg(prev, seg.target), seg.label});
            start += seg.seconds; prev = seg.target;
        }
    }
    if (o.pvr) {
        if (!fake_pvr.module) { fail("fake PVR client loaded"); return sum; }
        fake_pvr.reset();
        fake.pvr_mode = 1; fake.pvr_stream = true;
        fake.pvr_table = fake_pvr.get(1, 32);
        fake.pvr_hmd = fake_pvr.hmd();
        fake.pvr_set_time = fake_pvr.set_time;
        fake_pvr.set_provider(fake_pvr_provider);
        fake_pvr.stream_mode(2);
        fake_pvr.stream_repeat(o.pvr_repeat);
        unsigned discard[5]; fake_pvr.stats(discard);
    }
    const std::wstring suffix = L"_harness_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::wstring(o.name.begin(), o.name.end());
    SetEnvironmentVariableW(L"DCSVR_PERCEPTION_SHM_SUFFIX", suffix.c_str());
    dcsvr_perception::SharedBlock<dcsvr_perception::GazeOut> engine_gaze;
    dcsvr_perception::SharedBlock<dcsvr_perception::DepthIn> engine_depth;
    dcsvr_perception::SharedBlock<dcsvr_perception::DepthGrid> engine_grid_block;
    if (!engine_gaze.open(dcsvr_perception::gaze_block_name, suffix.c_str()) || !engine_depth.open(dcsvr_perception::depth_block_name, suffix.c_str()) ||
        !engine_grid_block.open(dcsvr_perception::grid_block_name, suffix.c_str())) { fail("harness opens the shared-memory blocks"); return sum; }
    const fs::path folder = out / o.name;
    fs::create_directories(folder);
    const fs::path copy = folder / "XR_APILAYER_DCSVR_pupil_shift.dll";
    fs::copy_file(dll, copy, fs::copy_options::overwrite_existing);
    const fs::path ini = folder / "PupilShift.ini";
    { std::ofstream f(ini, std::ios::binary); f << predict_ini(o, o.transition); }
    Layer layer;
    if (!load_layer(copy, layer)) { fail("layer loads"); return sum; }
    Options eo; eo.scene = o.scene; eo.latency_frames = 2;  // the cone summary stand-in
    struct Published { dcsvr_perception::DepthIn depth; std::shared_ptr<dcsvr_perception::DepthGrid> grid; };
    std::deque<Published> queue;

    std::ofstream csv(out / (o.name + ".csv"), std::ios::binary);
    csv << "t_locate_s,t_display_s,segment,saccade,yaw_deg,pitch_deg,scale,render_ipd_mm,step_mm,fixation,fix_source,predictive,fallback\n";
    const double total = duration(script);
    const int frames = static_cast<int>((total - o.phase_s - 2 * F) / F);
    const double sample_dt = 1.0 / o.pvr_hz;
    double next_sample = 0, last_scale = 1;
    std::vector<Jump> all_jumps;  // every change beyond the rates; the predictive steps among them come from the log
    bool sampler_seen = false;
    unsigned long long last_calls = 0;
    const double base_mm = 64.0;
    for (int i = 0; i < frames; ++i) {
        const double tl = o.phase_s + i * F, td = tl + 2 * F;
        const XrTime time = t0 + static_cast<XrTime>(std::llround(td * 1e9));
        if (o.on_frame) o.on_frame(ini, i);
        // The tracker's samples up to the locate time.
        if (o.pvr) {
            std::vector<pvr::EyeTrackingInfo> batch;
            for (; next_sample <= tl + 1e-9; next_sample += sample_dt) {
                const Eyes e = evaluate(script, static_cast<float>(next_sample));
                pvr::EyeTrackingInfo info{};
                const float nd = rad(o.noise_deg);
                for (int eye = 0; eye < 2; ++eye) {
                    const Vec3 eye_head{eye == 0 ? -kRuntimeIpd / 2 : kRuntimeIpd / 2, 0, 0};
                    const Vec3 gz = normalized(normalized(e.point - eye_head) + Vec3{fake.gauss(fake.rng) * nd, fake.gauss(fake.rng) * nd, 0});
                    info.GazeTan[eye] = {gz.x / -gz.z, gz.y / -gz.z};
                }
                info.TimeInSeconds = next_sample + 1.0;
                info.ConvergenceDistance = length(e.point);
                batch.push_back(info);
            }
            // Until the layer's sampler polls, the per-frame query reads the newest sample (latest mode); then every
            // sample is queued and the harness waits until the sampler took them all.
            unsigned long long calls = 0;
            if (!sampler_seen) {
                // The sampler polls at 1000 Hz: 5 ms of wall time show 3 calls or more (a per-frame reader makes none).
                fake_pvr.pending(&last_calls);
                Sleep(5);
                fake_pvr.pending(&calls);
                if (calls - last_calls >= 3) { sampler_seen = true; fake_pvr.stream_mode(1); }
            }
            if (!batch.empty()) fake_pvr.push(batch.data(), static_cast<unsigned>(batch.size()));
            if (sampler_seen) {
                const ULONGLONG start = GetTickCount64();
                while (fake_pvr.pending(&calls) > 0 && GetTickCount64() - start < 2000) Sleep(0);
                const unsigned long long drained = calls;
                while (fake_pvr.pending(&calls) == 0 && calls < drained + 1 && GetTickCount64() - start < 2000) Sleep(0);
                CHECK(GetTickCount64() - start < 2000, "the layer's sampler takes every queued sample", static_cast<double>(fake_pvr.pending(nullptr)), 0, tl);
            }
            fake_pvr.pending(&last_calls);
        }
        app_sync(layer, static_cast<float>(tl));
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
        info.displayTime = time;
        info.space = kBaseSpace;
        XrViewState state{XR_TYPE_VIEW_STATE};
        XrView views[4]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        std::uint32_t count = 0;
        CHECK(layer.locate(kSession, &info, &state, 4, &count, views) == XR_SUCCESS && count == 4, "locate", 0, 0, tl);
        const Status st = decode(layer.status());
        const Eyes eyes = evaluate(script, static_cast<float>(tl));
        // The engine stand-in: this frame's GazeOut, published 2 frames later.
        dcsvr_perception::GazeOut go{};
        if (dcsvr_perception::seqlock_read(engine_gaze.get(), go) && go.displayTime == time) {
            Published p{engine_summary(go, eo, eyes), nullptr};
            if (o.grid) p.grid = std::make_shared<dcsvr_perception::DepthGrid>(engine_grid(go, o, eyes));
            queue.push_back(std::move(p));
        }
        while (queue.size() > 2) {
            dcsvr_perception::seqlock_write(engine_depth.get(), queue.front().depth);
            if (queue.front().grid) dcsvr_perception::seqlock_write(engine_grid_block.get(), *queue.front().grid);
            queue.pop_front();
        }
        // xrEndFrame with the views as located (the fake runtime checks the restore bitwise).
        {
            std::array<XrCompositionLayerProjectionView, 4> pv{};
            fake.expect_foreign = {};
            for (int v = 0; v < 4; ++v) { pv[v] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}; pv[v].pose = views[v].pose; pv[v].fov = views[v].fov; }
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projection.viewCount = 4; projection.views = pv.data();
            const XrCompositionLayerBaseHeader* layers[1] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = time; end.layerCount = 1; end.layers = layers;
            CHECK(layer.end(kSession, &end) == XR_SUCCESS, "end frame succeeds", 0, 0, tl);
        }
        // Steps: a change beyond the rates (+ 0.3 mm) in one frame.
        const double delta = (st.scale - last_scale) * base_mm, rate = (delta > 0 ? 5.0 : 40.0) * F + 0.3;
        int saccade = -1;
        for (std::size_t k = 0; k < sum.saccades.size(); ++k) if (tl >= sum.saccades[k].onset - 0.05) saccade = static_cast<int>(k);
        if (std::fabs(delta) > rate) {
            Jump j{tl, td, delta, saccade};
            j.time = time;
            if (saccade >= 0) { j.after_onset = tl - sum.saccades[saccade].onset; j.before_end = sum.saccades[saccade].end - tl; }
            all_jumps.push_back(j);
        }
        sum.max_scale = std::max(sum.max_scale, st.scale);
        sum.frames.push_back({tl, st.scale, saccade, eyes.in_saccade, eyes.true_distance < 1.5f, st.fixation, st.predictive, st.predictive_fallback});
        last_scale = st.scale;
        char line[320];
        std::snprintf(line, sizeof line, "%.4f,%.4f,%s,%d,%.2f,%.2f,%.4f,%.3f,%.3f,%u,%u,%d,%d\n", tl, td, eyes.label.c_str(), eyes.in_saccade ? 1 : 0,
            yaw_of(eyes.point), pitch_of(eyes.point), st.scale, st.scale * base_mm, std::fabs(delta) > rate ? delta : 0.0, st.fixation, st.fix_source,
            st.predictive ? 1 : 0, st.predictive_fallback ? 1 : 0);
        csv << line;
    }
    // A second of wall time and one more frame: the layer writes its once-a-second lines.
    Sleep(1100);
    {
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
        info.displayTime = t0 + static_cast<XrTime>(std::llround((o.phase_s + (frames + 2) * F) * 1e9));
        info.space = kBaseSpace;
        XrViewState state{XR_TYPE_VIEW_STATE};
        XrView views[4]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        std::uint32_t count = 0;
        app_sync(layer, -1);
        layer.locate(kSession, &info, &state, 4, &count, views);
    }
    // The runtime destroys its PVR session: the sampler must not call PVR any more (it keeps running until the instance
    // goes), then xrDestroyInstance stops it (the slots stay patched, passing through).
    CHECK(layer.destroy_session(kSession) == XR_SUCCESS, "xrDestroySession", 0, 0, -1);
    if (o.pvr) {
        Sleep(30);
        CHECK(fake_pvr.after_destroy() == 0, "PVR is never called after the runtime destroyed its session", fake_pvr.after_destroy(), 0, -1);
    }
    CHECK(layer.destroy_instance(layer.instance) == XR_SUCCESS, "xrDestroyInstance", 0, 0, -1);
    if (o.pvr) {
        unsigned calls[5]{};
        fake_pvr.stats(calls);
        CHECK(calls[1] == 0, "PVR queried only with the runtime's handle", calls[1], 0, -1);
        for (const auto slot : {pvr::slot_destroy_hmd, pvr::slot_end_frame, pvr::slot_submit_frame})
            CHECK(fake_pvr.get(1, 32)->slots[slot] != fake_pvr.original(static_cast<std::uint32_t>(slot)), "PVR slots stay patched", static_cast<double>(slot), 0, -1);
        CHECK(pvr_passes_through(), "after xrDestroyInstance the PVR detours pass straight through", 0, 0, -1);
        fake_pvr.stream_mode(0);
        fake.pvr_mode = 0;
    }
    engine_gaze.close(); engine_depth.close(); engine_grid_block.close();
    FreeLibrary(layer.module);
    std::ifstream log(folder / "PupilShift.log");
    const std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
    sum.sampler_started = text.find("predict: gaze sampler started") != std::string::npos;
    sum.sampler_stopped = text.find("predict: gaze sampler stopped") != std::string::npos;
    sum.fallback_logged = text.find("predictive selected, gradual in effect") != std::string::npos;
    CHECK(text.find("FAILED") == std::string::npos, "log: shared memory open", 0, 0, -1);
    static const std::regex counter("(saccades|dropped|predicted|steps|confirmed|contradicted|expired|short|no_grid|edge|ambiguous|outside|late|same)=(\\d+)");
    std::istringstream lines(text);
    static const std::regex step_line("predict: step ([+-][0-9.]+) mm .* at display time (-?[0-9]+)");
    std::map<XrTime, double> logged_steps;
    for (std::string l; std::getline(lines, l);) {
        std::smatch m;
        if (std::regex_search(l, m, step_line)) logged_steps[std::stoll(m[2])] = std::stod(m[1]);
        if (l.find("predict: transition=predictive") == std::string::npos) continue;
        for (std::sregex_iterator it(l.begin(), l.end(), counter), end; it != end; ++it) sum.log[(*it)[1]] += static_cast<unsigned>(std::stoul((*it)[2]));
    }
    // The predictive steps: the changes beyond the rates the layer logged as a step (at their display time). Any other
    // change beyond the rates is the gradual saccade step (4 mm at most).
    for (const auto& j : all_jumps) {
        const auto it = logged_steps.find(j.time);
        if (it == logged_steps.end()) {
            CHECK(std::fabs(j.mm) <= 4.0 + 40.0 / o.fps + 0.3, "outside predictive steps, at most the gradual saccade step", j.mm, 4, j.t_loc);
            continue;
        }
        sum.jumps.push_back(j);
        CHECK(std::fabs(it->second - j.mm) < 0.5 + 40.0 / o.fps, "the logged step is the scale's change", it->second, j.mm, j.t_loc);
        CHECK(std::fabs(j.mm) <= 12.0 + 40.0 / o.fps, "a step is at most PredictMaxStepMm (12 mm)", j.mm, 12, j.t_loc);
        if (o.in_flight_check)
            CHECK(j.saccade >= 0 && j.before_end > 0, "a step is applied while the saccade is in flight (located before it ends)", j.before_end * 1000, 0, j.t_loc);
    }
    CHECK(sum.jumps.size() == logged_steps.size(), "every logged step shows in the scale", static_cast<double>(sum.jumps.size()), static_cast<double>(logged_steps.size()), -1);
    if (o.transition != "predictive" && !o.on_frame) CHECK(logged_steps.empty(), "no predictive step in the gradual transition", static_cast<double>(logged_steps.size()), 0, -1);
    // SVG: the scale (top), the gaze's pitch and yaw (bottom), saccades shaded, steps marked.
    std::ofstream svg(out / (o.name + ".svg"), std::ios::binary);
    const float W = 1000, H = 420, x0 = 50, x1 = W - 10;
    auto X = [&](double t) { return x0 + (x1 - x0) * static_cast<float>(t / std::max(total, 0.001)); };
    auto Ys = [&](double s) { return 20 + 160 * (1.3 - std::clamp(s, 1.0, 1.3)) / 0.3; };
    auto Ya = [&](double deg) { return 230 + 170 * (40.0 - std::clamp(deg, -40.0, 40.0)) / 80.0; };
    svg << "<svg xmlns='http://www.w3.org/2000/svg' width='" << W << "' height='" << H << "' font-family='sans-serif' font-size='11'>\n";
    svg << "<rect width='100%' height='100%' fill='white'/>\n<text x='" << x0 << "' y='14'>" << o.name << ": " << o.description << "</text>\n";
    for (const auto& sc : sum.saccades) svg << "<rect x='" << X(sc.onset) << "' y='20' width='" << std::max(1.0f, X(sc.end) - X(sc.onset)) << "' height='380' fill='#ffe8c0'/>";
    for (double sv : {1.0, 1.1, 1.2, 1.3}) svg << "<line x1='" << x0 << "' x2='" << x1 << "' y1='" << Ys(sv) << "' y2='" << Ys(sv) << "' stroke='#ccc'/><text x='5' y='" << Ys(sv) + 4 << "'>x" << sv << "</text>\n";
    for (double a : {-30.0, 0.0, 30.0}) svg << "<line x1='" << x0 << "' x2='" << x1 << "' y1='" << Ya(a) << "' y2='" << Ya(a) << "' stroke='#ccc'/><text x='5' y='" << Ya(a) + 4 << "'>" << a << " deg</text>\n";
    std::ifstream back(out / (o.name + ".csv"));
    std::string row; std::getline(back, row);
    std::vector<std::array<double, 4>> pts;
    for (; std::getline(back, row);) {
        std::vector<std::string> f; std::stringstream ss(row); for (std::string c; std::getline(ss, c, ',');) f.push_back(c);
        if (f.size() >= 9) pts.push_back({std::stod(f[0]), std::stod(f[6]), std::stod(f[4]), std::stod(f[5])});
    }
    for (int k : {1, 2, 3}) {
        svg << "<polyline fill='none' stroke='" << (k == 1 ? "#c02020" : k == 2 ? "#2050c0" : "#20a050") << "' stroke-width='1.2' points='";
        for (const auto& p : pts) svg << X(p[0]) << ',' << (k == 1 ? Ys(p[1]) : Ya(p[k])) << ' ';
        svg << "'/>\n";
    }
    for (const auto& j : sum.jumps) svg << "<line x1='" << X(j.t_loc) << "' x2='" << X(j.t_loc) << "' y1='20' y2='180' stroke='#000' stroke-dasharray='3,2'/>";
    svg << "\n<text x='" << x0 + 5 << "' y='34' fill='#c02020'>render IPD scale (dashed: a predictive step, at the locate time)</text>"
        << "<text x='" << x0 + 5 << "' y='244' fill='#2050c0'>gaze yaw</text><text x='" << x0 + 70 << "' y='244' fill='#20a050'>gaze pitch</text>"
        << "<text x='" << x0 + 150 << "' y='244' fill='#b08030'>orange: saccades</text>\n</svg>\n";
    return sum;
}

// The scripts. Panel below -10 degrees (0.7 m), sky above, a strut at 9..13 degrees right.
std::vector<Segment> cockpit_sky_script(float amplitude) {
    const Vec3 from = cockpit(0, 4 - amplitude), to = outside(0, 4);
    return {fix("cockpit panel", 1.5f, from), msaccade("saccade cockpit -> sky", from, to), fix("far sky", 3.5f, to)};
}
std::vector<Segment> sky_panel_script() {
    const Vec3 sky = outside(0, 4), panel = cockpit(0, -20);
    return {fix("far sky", 5.0f, sky), msaccade("saccade sky -> panel", sky, panel), fix("cockpit panel", 2.0f, panel)};
}
std::vector<Segment> strut_script() {
    const Vec3 from = cockpit(-10, -16), to = outside(13.3f, 2);
    return {fix("cockpit panel", 1.5f, from), msaccade("saccade cockpit -> canopy strut edge", from, to), fix("far at the strut's edge", 2.0f, to)};
}
std::vector<Segment> short_script() {
    const Vec3 from = cockpit(0, -12.5f), to = outside(0, -8.5f);
    return {fix("cockpit panel top", 1.5f, from), msaccade("saccade 4 deg", from, to), fix("far just above the panel", 2.5f, to)};
}
std::vector<Segment> mispredict_script() {
    // An 18 degree saccade from the panel's bottom to its top, 30 % faster than the main sequence: predicted beyond the
    // panel, into the sky (far); the eyes land on the panel (near).
    const Vec3 from = cockpit(0, -31), to = cockpit(0, -13);
    return {fix("cockpit panel bottom", 1.5f, from), msaccade("saccade (mispredicted)", from, to, 1.3f), fix("cockpit panel top", 2.0f, to)};
}
// Saccades of 15 to 35 degrees, cockpit -> sky -> cockpit, at four phases against the frame clock (a quarter frame apart).
std::vector<Segment> sweep_script() {
    std::vector<Segment> s;
    const Vec3 panel = cockpit(0, -14);
    s.push_back(fix("cockpit start", 1.0f, panel));
    for (float a : {15.0f, 20.0f, 25.0f, 30.0f, 35.0f})
        for (int phase = 0; phase < 4; ++phase) {
            const float theta = std::asin(std::min(1.0f, 14.0f / a));
            const Vec3 sky = outside(a * std::cos(theta), -14 + a * std::sin(theta));
            const char* tag = a == 15 ? "15" : a == 20 ? "20" : a == 25 ? "25" : a == 30 ? "30" : "35";
            s.push_back(msaccade(std::string("up ") + tag, panel, sky));
            s.push_back(fix("far sky", 1.6f, sky));
            s.push_back(msaccade(std::string("down ") + tag, sky, panel));
            s.push_back(fix("cockpit panel", 1.0f + (phase + 1) * (1.0f / 45) / 4, panel));
        }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: pupil_shift_harness <layer dll> <output folder>\n"); return 2; }
    const fs::path dll = fs::absolute(argv[1]), out = fs::absolute(argv[2]);
    if (argc >= 4) fake_qv_dll = fs::absolute(argv[3]);
    else std::printf("no fake Quad Views DLL given: the Quad Views export scenarios are skipped\n");
    if (argc >= 5 && !load_fake_pvr(fs::absolute(argv[4]))) { std::printf("cannot load the fake PVR client %s\n", argv[4]); return 2; }
    if (argc < 5) std::printf("no fake PVR client given: the Pimax PVR scenarios are skipped\n");
    fs::create_directories(out);
    std::vector<Summary> results;
    // PUPIL_HARNESS_PREDICTIVE_ONLY=1: only the predictive transition's scenarios (development).
    char only_env[8]{};
    const bool only_predictive = GetEnvironmentVariableA("PUPIL_HARNESS_PREDICTIVE_ONLY", only_env, sizeof only_env) > 0 && only_env[0] == '1';
    if (!only_predictive) {
    Options a; a.name = "flight_pupil_depth"; a.description = "Pupil shift + Depth boost, Ipd=64 takeover, per-eye noise 0.5 deg + 0.5 deg drift";
    a.script.segments = flight();
    results.push_back(run(dll, out, a));

    Options b = a; b.name = "flight_depth_only"; b.description = "Depth boost alone (pupil shift disabled), runtime IPD 65.5 mm";
    b.pupil = false; b.ini_ipd_mm = 0;
    results.push_back(run(dll, out, b));

    Options c = a; c.name = "noisy_1deg"; c.description = "1.0 deg white noise + 0.7 deg drift per eye";
    c.script.noise_deg = 1.0f; c.script.drift_deg = 0.7f;
    results.push_back(run(dll, out, c));

    Options d = a; d.name = "combined_gaze_1m"; d.description = "one gaze projected through both eyes at 1 m (bundled Quad Views)";
    d.script.combined = true; d.script.combined_m = 1.0f;
    results.push_back(run(dll, out, d));

    Options e = a; e.name = "combined_gaze_parallel"; e.description = "one gaze, parallel eyes (projected at infinity)";
    e.script.combined = true; e.script.combined_m = 0;
    results.push_back(run(dll, out, e));

    Options f = a; f.name = "pupil_only"; f.description = "Pupil shift alone (Depth boost disabled), Ipd=64";
    f.depth = false;
    results.push_back(run(dll, out, f));
    Options pg = f; pg.name = "publish_gaze_out"; pg.description = "PublishGaze=1, Depth boost and Pupil shift off: GazeOut carries the tracked gaze (Visual realism)";
    pg.pupil = false; pg.publish_gaze = true;
    results.push_back(run(dll, out, pg));

    // The live bug (2026-10-11): nobody wearing the headset, the bundled Quad Views' fixed focus read as a gaze.
    Options q = a; q.name = "qv_export"; q.description = "bundled Quad Views with its gaze export: combined gaze, fixed focus without a gaze";
    q.script.combined = true; q.script.combined_m = 0; q.qv_style = true; q.qv_export = true;
    if (!fake_qv_dll.empty()) results.push_back(run(dll, out, q));
    Options qd = q; qd.name = "qv_export_depth_only"; qd.description = "the same, Depth boost alone (Ipd=64)"; qd.pupil = false;
    if (!fake_qv_dll.empty()) results.push_back(run(dll, out, qd));
    Options qa = q; qa.name = "qv_no_export"; qa.description = "Quad Views-style focus without the export (focus-centre reading, as before)";
    qa.qv_export = false;
    results.push_back(run(dll, out, qa));

    // Pimax native quad views with the OpenXR eye gaze (2026-10-11 fix): the runtime offers XR_EXT_eye_gaze_interaction.
    Options x = qa; x.name = "xr_gaze"; x.description = "Pimax-style focus without the export, OpenXR eye gaze offered (the layer's own action)";
    x.xr_offered = true;
    results.push_back(run(dll, out, x));
    Options xn = x; xn.name = "xr_never_valid"; xn.description = "the same, nobody wearing the headset: the gaze never valid, Ipd=0";
    xn.xr_never_valid = true; xn.ini_ipd_mm = 0;
    results.push_back(run(dll, out, xn));
    Options xa = x; xa.name = "xr_app_gaze"; xa.description = "the application enables the eye gaze itself and binds its own action";
    xa.app_gaze = true;
    results.push_back(run(dll, out, xa));
    Options xq = q; xq.name = "xr_offered_qv_export"; xq.description = "the export wins: the layer does not enable the eye gaze";
    xq.xr_offered = true;
    if (!fake_qv_dll.empty()) results.push_back(run(dll, out, xq));

    // Pimax PVR per-eye gaze (the focus views share one gaze, as with Quad Views; PVR has each eye's own).
    Options pv = a; pv.name = "pvr_per_eye"; pv.description = "one shared focus gaze, Pimax PVR per-eye gaze through the interface table";
    pv.script.combined = true; pv.script.combined_m = 1.0f; pv.pvr = 1;
    if (fake_pvr.module) results.push_back(run(dll, out, pv));
    Options pc = pv; pc.name = "pvr_copied_table"; pc.description = "the same, the runtime calls its own copy of the table: no session, no per-eye gaze";
    pc.pvr = 2;
    if (fake_pvr.module) results.push_back(run(dll, out, pc));
    Options sg = a; sg.name = "simulate_gaze_out"; sg.description = "SimulateGaze (10, -5) deg: GazeOut carries it; Depth boost alone, Ipd=64";
    sg.simulate = true; sg.pupil = false;
    results.push_back(run(dll, out, sg));
    Options rv = a; rv.name = "realistic_vergence_only"; rv.description = "Realistic without the engine module: vergence cannot grade far distance, x1";
    rv.mode = "realistic"; rv.max_scale = 1.25f;
    results.push_back(run(dll, out, rv));

    // Depth from the scene (engine module stand-in), overlapping geometry.
    std::vector<std::string> depth_names;
    auto depth_run = [&](Options o) { depth_names.push_back(o.name); results.push_back(run(dll, out, o)); };
    Options dc; dc.name = "depth_canopy_wingman"; dc.description = "wingman at 80 m past a 0.6 m canopy strut, gaze jittering across its edge (Enhanced)";
    dc.script.segments = canopy_script(false); dc.script.noise_deg = 0.8f; dc.scene = canopy_scene;
    depth_run(dc);
    Options dg = dc; dg.name = "depth_glareshield_sky"; dg.description = "glareshield edge against the sky, from the sky and from the panel";
    dg.script.segments = glareshield_script(); dg.scene = glareshield_scene;
    depth_run(dg);
    Options dm = dc; dm.name = "depth_mfd_bezel_ground"; dm.description = "MFD bezel over the ground, from the MFD and from the ground";
    dm.script.segments = mfd_script(); dm.scene = mfd_scene;
    depth_run(dm);
    // Stepped (the layer's default): the same scenes and the flight, Realistic; the per-frame stepped checks (no change
    // while fixating beyond 0.5 mm/s, one bounded step per saccade, never above MaxScale) replace the ramp deadlines.
    for (Options sv : {dc, dg, dm}) {
        sv.name += "_stepped"; sv.stepped = true; sv.mode = "realistic"; sv.max_scale = 1.25f; sv.far_start_m = 150; sv.far_full_m = 400;
        results.push_back(run(dll, out, sv));
    }
    {
        // Enhanced (vergence grades the distance without the engine's scene depth): the steps must still reach the boost.
        Options fs = a; fs.name = "flight_stepped"; fs.description = "the flight, Depth boost Enhanced stepped (pupil shift off): boosts in saccade steps";
        fs.pupil = false; fs.stepped = true;
        const Summary r = run(dll, out, fs);
        CHECK(r.far_hold_reach_s >= 0 || r.cockpit_boosted_fraction >= 0, "stepped flight ran", 0, 0, -1);
        results.push_back(r);
        // The same flight with the engine's dual camera acknowledging every frame: one world scale, slow changes only.
        Options fd = fs; fd.name = "flight_dual_camera"; fd.description = "the flight, Enhanced, the engine's dual camera acknowledged: one world scale";
        fd.dual_ack = true;
        fd.legacy = false;
        results.push_back(run(dll, out, fd));
        // The shipped default without the engine's dual camera: Depth boost never leaves x1.
        Options fn = fs; fn.name = "flight_no_dual_camera"; fn.description = "the flight, Enhanced, Legacy=0 and no dual camera: x1 throughout";
        fn.legacy = false;
        results.push_back(run(dll, out, fn));
    }
    Options dh = dc; dh.name = "depth_hud_near_eyes_far"; dh.description = "HUD: depth near (combiner), eyes at infinity: depth wins, x1";
    dh.script.segments = hud_script(false); dh.script.noise_deg = 0.5f; dh.scene = hud_glass_scene;
    depth_run(dh);
    Options dv = dh; dv.name = "depth_hud_far_eyes_near"; dv.description = "depth far, eyes converged at 0.7 m: the vergence vetoes, x1";
    dv.script.segments = hud_script(true); dv.scene = sky_scene;
    depth_run(dv);
    Options dp = dh; dp.name = "depth_pursuit"; dp.description = "smooth pursuit of an object from 1 m to 300 m and back, no saccade";
    dp.script.segments = pursuit_script(); dp.scene = object_scene;
    depth_run(dp);
    Options db = dh; db.name = "depth_blink"; db.description = "a held 150 ms blink while confirming far and while returning";
    db.script.segments = blink_script(); db.scene = sky_scene;
    depth_run(db);
    Options ds = dh; ds.name = "depth_stale"; ds.description = "the engine stops publishing for 2.5 s in a far fixation (vergence proven)";
    ds.script.segments = stale_script(); ds.scene = sky_scene;
    ds.stale_from = segment_start(ds.script.segments, "far depth stale"); ds.stale_to = ds.stale_from + 2.5f;
    depth_run(ds);
    Options dn = ds; dn.name = "depth_stale_no_vergence"; dn.description = "the same with one shared gaze (no vergence): x1 while stale, boost again after";
    dn.script.combined = true; dn.script.combined_m = 1.0f;
    for (auto& seg : dn.script.segments) if (seg.label == "far depth stale") { seg.expect = Expect::x1; seg.within = 0.5f + 0.64f + 0.1f; }
    depth_run(dn);
    Options dl = dh; dl.name = "depth_latency_5"; dl.description = "HUD (depth near, eyes far) with 5 frames of depth latency";
    dl.latency_frames = 5;
    depth_run(dl);
    // Stress: the three edge scenes with a worse tracker (1.0 deg white noise + 0.7 deg drift per eye), four seeds each.
    for (unsigned k = 0; k < 4; ++k)
        for (const Options* base : {&dc, &dg, &dm}) {
            Options noisy = *base;
            noisy.name = base->name + "_noisy_" + std::to_string(k);
            noisy.description = base->description + ", 1.0 deg noise + 0.7 deg drift, seed " + std::to_string(k);
            noisy.script.noise_deg = 1.0f; noisy.script.drift_deg = 0.7f; noisy.seed = 1000 + 7919 * k; noisy.trace = true;
            depth_run(noisy);
        }
    Options rw = dc; rw.name = "realistic_wingman_80m"; rw.description = "Realistic: the wingman at 80 m (canopy scene) stays at exactly x1";
    rw.mode = "realistic"; rw.max_scale = 1.25f; rw.script.segments = canopy_script(true);
    depth_run(rw);
    Options rt = dh; rt.name = "realistic_target_500m"; rt.description = "Realistic: a target at 500 m rises towards x1.25 within the rate limits";
    rt.mode = "realistic"; rt.max_scale = 1.25f; rt.script.segments = realistic_target_script(); rt.scene = target_scene;
    depth_run(rt);

    Options g = a; g.name = "live_disable"; g.description = "Depth boost switched off in the ini at 22 s (during the far hold)";
    g.on_frame = [&g](const fs::path& ini, int frame) {
        if (frame == 22 * 90) { std::ofstream f(ini, std::ios::binary | std::ios::trunc); f << ini_text(g, false); }
    };
    results.push_back(run(dll, out, g));

    std::printf("\n%-24s %7s %9s %8s %10s %10s %9s %7s %8s %8s %7s %6s %9s %8s\n", "scenario", "frames", "farReach", "farMin", "glanceRet", "cockpitMax",
        "cockpit>1", "upMax", "downMax", "classOK", "proven", "const", "restored", "foreign");
    for (const auto& r : results)
        std::printf("%-24s %7u %8.2fs %8.3f %9.2fs %10.4f %8.1f%% %7.1f %8.1f %7.1f%% %7s %6s %9u %8u\n", r.name.c_str(), r.frames, r.far_hold_reach_s,
            r.far_hold_min_after > 8 ? 0.0 : r.far_hold_min_after, r.glance_return_s, r.cockpit_max_settled, r.cockpit_boosted_fraction * 100,
            r.max_up_mm_s, r.max_down_mm_s, r.class_agreement * 100, r.source_proven ? "yes" : "no", r.constant_flagged ? "yes" : "no",
            r.restored_views, r.foreign_views);
    std::printf("farReach: s at MaxScale after landing on the 10 s far fixation; glanceRet: s back to exactly 1 after landing on the\n"
                "instruments; cockpitMax: largest s in the cockpit once the return had time; cockpit>1: share of all cockpit frames with\n"
                "s > 1 (the returns); up/downMax: fastest IPD change outside saccades, mm/s; classOK: near/far right on settled\n"
                "fixations; restored: views checked bitwise at xrEndFrame; foreign: views the layer did not change, passed through.\n");

    // Scenario-level expectations.
    auto get = [&](const char* name) -> const Summary& { for (const auto& r : results) if (r.name == name) return r; return results.front(); };
    scenario_name = "summary";
    for (const char* name : {"flight_pupil_depth", "flight_depth_only"}) {
        const auto& r = get(name);
        CHECK(r.source_proven, "per-eye vergence is proven", 0, 0, -1);
        CHECK(r.far_hold_reach_s > 0 && r.far_hold_reach_s < 7.0, "the 10 s far fixation reaches the maximum within 7 s", r.far_hold_reach_s, 7, -1);
        CHECK(r.far_hold_min_after > 1.4 - 1e-3, "and holds it", r.far_hold_min_after, 1.4, -1);
        CHECK(r.glance_return_s > 0 && r.glance_return_s < 1.2, "a glance at the instruments returns to 1 within 1.2 s", r.glance_return_s, 1.2, -1);
        CHECK(r.class_agreement > 0.95, "near/far class right on settled fixations", r.class_agreement, 0.95, -1);
    }
    CHECK(get("noisy_1deg").class_agreement > 0.85, "noisy tracker: class mostly right", get("noisy_1deg").class_agreement, 0.85, -1);
    CHECK(!get("combined_gaze_1m").source_proven && !get("combined_gaze_parallel").source_proven, "combined gaze never boosts", 0, 0, -1);
    CHECK(get("combined_gaze_parallel").constant_flagged, "parallel combined gaze is flagged constant", 0, 0, -1);
    std::printf("\n%-24s %8s %9s %8s %16s\n", "Quad Views scenario", "valid", "no gaze", "saccade", "IPD on no gaze");
    for (const auto& r : results)
        if (r.qv_valid + r.qv_invalid)
            std::printf("%-24s %8u %9u %8u %7.3f..%.3f mm\n", r.name.c_str(), r.qv_valid, r.qv_invalid, r.qv_saccade, r.fixed_ipd_min, r.fixed_ipd_max);
    if (!fake_qv_dll.empty()) {
        const auto& qv = get("qv_export");
        CHECK(qv.qv_invalid > 0 && qv.qv_saccade > 0, "the Quad Views script has frames without a gaze and saccades", qv.qv_invalid, qv.qv_saccade, -1);
        // Frames without a gaze after the pupil's 0.3 s blink hold (none when every gap is shorter: then nothing to check).
        CHECK(qv.fixed_ipd_min > 1e8 || (std::fabs(qv.fixed_ipd_min - 64.0) < 1e-3 && std::fabs(qv.fixed_ipd_max - 64.0) < 1e-3), "export: rendered IPD exactly 64.0 mm without a gaze", qv.fixed_ipd_min, qv.fixed_ipd_max, -1);
        CHECK(!get("qv_export").source_proven && !get("qv_export_depth_only").source_proven, "Quad Views' combined gaze never boosts", 0, 0, -1);
    }
    for (const char* name : {"xr_gaze", "xr_app_gaze"}) {
        const auto& r = get(name);
        CHECK(r.qv_valid > 0 && r.qv_invalid > 0, "the OpenXR script has frames with and without a gaze", r.qv_valid, r.qv_invalid, -1);
        CHECK(r.fixed_ipd_min > 1e8 || (std::fabs(r.fixed_ipd_min - 64.0) < 1e-3 && std::fabs(r.fixed_ipd_max - 64.0) < 1e-3), "OpenXR gaze: rendered IPD exactly 64.0 mm without a gaze", r.fixed_ipd_min, r.fixed_ipd_max, -1);
        CHECK(!r.source_proven, "the OpenXR combined gaze never boosts", 0, 0, -1);
    }
    {
        const auto& r = get("xr_never_valid");
        CHECK(r.qv_valid == 0 && r.qv_invalid > 0, "never valid: no valid frame", r.qv_valid, r.qv_invalid, -1);
        CHECK(std::fabs(r.fixed_ipd_min - kRuntimeIpd * 1000) < 1e-3 && std::fabs(r.fixed_ipd_max - kRuntimeIpd * 1000) < 1e-3,
            "never valid: rendered IPD exactly the runtime's 65.5 mm (the focus views 2.4 deg outwards)", r.fixed_ipd_min, r.fixed_ipd_max, -1);
    }
    // Without the export the fixed focus reads as a gaze 2.4 degrees outwards per eye: about 0.9 mm wider (the live log).
    CHECK(get("qv_no_export").fixed_ipd_max > 64.7 && get("qv_no_export").fixed_ipd_max < 65.1, "no export: the fixed focus widens the IPD (the old behaviour)", get("qv_no_export").fixed_ipd_max, 64.9, -1);
    // Depth from the scene: the overlap scenes' numbers.
    scenario_name = "summary";
    std::printf("\n%-26s %5s %8s %10s %10s %9s %9s %9s %7s %7s %7s %6s %6s %9s\n", "depth scenario", "holds", "changes", "IPDosc", "IPDspan", "x1ret",
        "stepMax", "sacStep", "sMax", "depth%", "verg%", "edge", "veto", "published");
    for (const auto& r : results) {
        if (!r.has_scene && r.name.rfind("realistic", 0) != 0 && r.name.rfind("pvr", 0) != 0) continue;
        std::printf("%-26s %5u %8u %8.3fmm %8.3fmm %8.2fs %7.3fmm %7.3fmm %7.4f %6.1f%% %6.1f%% %6u %6u %9u\n", r.name.c_str(), r.hold_segments, r.hold_changes,
            r.hold_range_mm, r.hold_span_mm, r.x1_return_s, r.max_step_mm, r.max_window_step_mm, r.max_scale, r.frames ? 100.0 * r.src_depth / r.frames : 0.0,
            r.frames ? 100.0 * r.src_vergence / r.frames : 0.0, r.edge_frames, r.veto_frames, r.published);
    }
    std::printf("holds: segments on an edge or an ambiguous cone; changes: fixation state changes in them (must be 0); IPDosc: largest\n"
                "render-IPD oscillation in them (< 0.5 mm; a monotonic ramp is none); IPDspan: their largest IPD range; x1ret: slowest return to exactly x1 after a near fixation; stepMax / sacStep: largest\n"
                "per-frame IPD step outside / within 200 ms of a saccade (rate x 11.1 ms; + SaccadeStepMm); depth%% / verg%%: frames whose fixation\n"
                "came from the scene depth / the vergence; edge, veto: frames on an edge, frames the vergence vetoed a far depth.\n");
    for (const auto& name : depth_names) {
        const auto& r = get(name.c_str());
        CHECK(r.published > 0 && r.src_depth > r.frames / 2, "depth scenarios run on the scene depth", r.src_depth, r.frames, -1);
        CHECK(r.max_step_mm <= 40.0 / 90.0 + 0.02, "no IPD step beyond DownMmPerS outside saccades", r.max_step_mm, 40.0 / 90.0, -1);
        CHECK(r.max_window_step_mm <= 4.0 + 40.0 / 90.0 + 0.02, "no IPD step beyond the saccade allowance", r.max_window_step_mm, 4.0 + 40.0 / 90.0, -1);
    }
    for (const char* name : {"depth_canopy_wingman", "depth_glareshield_sky", "depth_mfd_bezel_ground"})
        CHECK(get(name).edge_frames > 0 && get(name).hold_segments >= 1, "the edge scenes have edge frames", get(name).edge_frames, 0, -1);
    CHECK(get("depth_hud_far_eyes_near").veto_frames > 0, "the vergence vetoes the far depth on the HUD", 0, 0, -1);
    CHECK(get("depth_hud_near_eyes_far").veto_frames == 0, "a far vergence never vetoes a near depth", get("depth_hud_near_eyes_far").veto_frames, 0, -1);
    CHECK(get("realistic_wingman_80m").max_scale == 1.0, "Realistic: x1 exactly with the wingman at 80 m", get("realistic_wingman_80m").max_scale, 1, -1);
    CHECK(get("realistic_target_500m").max_scale >= 1.2 && get("realistic_target_500m").max_scale <= 1.25 + 1e-4, "Realistic: the 500 m target rises towards x1.25",
        get("realistic_target_500m").max_scale, 1.25, -1);
    CHECK(get("realistic_vergence_only").max_scale == 1.0, "Realistic without depth: x1", get("realistic_vergence_only").max_scale, 1, -1);
    if (fake_pvr.module) {
        const auto& r = get("pvr_per_eye");
        CHECK(r.source_proven && r.far_hold_reach_s > 0 && r.far_hold_reach_s < 7.0, "Pimax PVR per-eye gaze proves vergence and boosts", r.far_hold_reach_s, 7, -1);
        CHECK(r.class_agreement > 0.95, "Pimax PVR: near/far class right on settled fixations", r.class_agreement, 0.95, -1);
        CHECK(!get("pvr_copied_table").source_proven && get("pvr_copied_table").max_scale == 1.0, "copied table: no per-eye gaze, x1", 0, 0, -1);
    }
    }  // !only_predictive
    // The predictive transition (fake PVR stream at 500 Hz, 45 fps, DCS locating 2 frames ahead).
    if (fake_pvr.module) {
        std::vector<PredSummary> pred;
        const auto prun = [&](const PredOptions& po) { pred.push_back(run_predictive(dll, out, po)); return pred.back(); };
        const auto count = [](const PredSummary& r, const char* k) { const auto it = r.log.find(k); return it == r.log.end() ? 0u : it->second; };
        // The first frame (from the locate time) at which the scale is at least `value`, after `from` seconds; -1 never.
        const auto reach = [](const PredSummary& r, double from, double value) {
            for (const auto& f : r.frames) if (f.t_loc >= from && f.scale >= value - 1e-4) return f.t_loc - from;
            return -1.0;
        };
        const auto back_to_1 = [](const PredSummary& r, double from) {
            for (const auto& f : r.frames) if (f.t_loc >= from && f.scale == 1.0) return f.t_loc - from;
            return -1.0;
        };
        PredOptions p1; p1.name = "predict_cockpit_sky_20"; p1.description = "predictive: cockpit -> sky, 20 deg, the step inside the saccade";
        p1.segments = cockpit_sky_script(20); p1.phase_s = 0.0122f;
        const auto r1 = prun(p1);
        PredOptions p1g = p1; p1g.name = "gradual_cockpit_sky_20"; p1g.description = "gradual (A/B reference): cockpit -> sky, 20 deg"; p1g.transition = "gradual";
        const auto r1g = prun(p1g);
        PredOptions p1l = p1; p1l.name = "predict_cockpit_sky_20_lead1"; p1l.description = "predictive, PredictLeadFrames=1: the same saccade is too short at 45 fps"; p1l.lead_frames = 1.0f;
        const auto r1l = prun(p1l);
        PredOptions p1m = p1; p1m.name = "predict_cockpit_sky_20_median"; p1m.description = "predictive, the grid's typical distance as a median"; p1m.grid_log_mean = false;
        const auto r1m = prun(p1m);
        PredOptions p2 = p1; p2.name = "predict_sky_panel"; p2.description = "predictive: sky -> panel, 24 deg: a step down capped by the panel's disparity, then the fast return";
        p2.segments = sky_panel_script(); p2.phase_s = 0.004f;
        const auto r2 = prun(p2);
        PredOptions p2g = p2; p2g.name = "gradual_sky_panel"; p2g.transition = "gradual"; p2g.description = "gradual (A/B reference): sky -> panel";
        const auto r2g = prun(p2g);
        PredOptions p3 = p1; p3.name = "predict_canopy_edge"; p3.description = "predictive: a 29 deg saccade onto the canopy strut's edge: no step";
        p3.segments = strut_script();
        const auto r3 = prun(p3);
        PredOptions p4 = p1; p4.name = "predict_short_4deg"; p4.description = "predictive: a 4 deg saccade from the panel's top to the sky: no step (gradual path)";
        p4.segments = short_script();
        const auto r4 = prun(p4);
        PredOptions p5 = p1; p5.name = "predict_mispredicted"; p5.description = "predictive: an 18 deg saccade 30 % faster than the main sequence, predicted into the sky, lands on the panel";
        p5.segments = mispredict_script(); p5.in_flight_check = false;
        const auto r5 = prun(p5);
        PredOptions p6 = p1; p6.name = "predict_no_pvr"; p6.description = "predictive selected without Pimax PVR: gradual, and the log says why"; p6.pvr = false;
        const auto r6 = prun(p6);
        PredOptions p7 = p1; p7.name = "predict_live_switch"; p7.description = "gradual in the ini, switched to predictive live at 0.5 s (sampler starts, the step follows)";
        p7.transition = "gradual";
        // The layer re-reads its ini every 90 frames: the switch at frame 22 is in effect by frame 90 (2 s), the saccade at 3.5 s.
        p7.segments = cockpit_sky_script(20); p7.segments.front().seconds = 3.5f;
        p7.on_frame = [&p7](const fs::path& ini, int frame) { if (frame == 22) { std::ofstream f(ini, std::ios::binary | std::ios::trunc); f << predict_ini(p7, "predictive"); } };
        const auto r7 = prun(p7);
        PredOptions sw = p1; sw.name = "predict_sweep_lead05"; sw.description = "15-35 deg saccades at four frame phases, PredictLeadFrames=0.5"; sw.segments = sweep_script();
        const auto rs = prun(sw);
        PredOptions sw1 = sw; sw1.name = "predict_sweep_lead1"; sw1.description = "the same, PredictLeadFrames=1"; sw1.lead_frames = 1.0f;
        const auto rs1 = prun(sw1);
        PredOptions s120 = sw; s120.name = "predict_sweep_120hz"; s120.description = "the same at lead 0.5 with a 120 Hz tracker"; s120.pvr_hz = 120; s120.pvr_repeat = 4;
        const auto rs120 = prun(s120);

        std::printf("\n%-32s %5s %5s %5s %5s %5s %5s %5s %5s %5s %5s %5s %5s  %s\n", "predictive scenario", "sacc", "pred", "steps", "conf", "contr", "exp",
            "short", "nogrd", "edge", "amb", "late", "same", "steps: mm @ ms after onset / ms before the true end (saccade)");
        for (const auto& r : pred) {
            std::string steps;
            for (const auto& j : r.jumps) {
                char b[96];
                std::snprintf(b, sizeof b, "%+.2f @ %.0f/%.0f (%s) ", j.mm, j.after_onset * 1000, j.before_end * 1000, j.saccade >= 0 ? r.saccades[j.saccade].label.c_str() : "?");
                if (steps.size() < 300) steps += b;
            }
            std::printf("%-32s %5u %5u %5u %5u %5u %5u %5u %5u %5u %5u %5u %5u  %s\n", r.name.c_str(), count(r, "saccades"), count(r, "predicted"), count(r, "steps"),
                count(r, "confirmed"), count(r, "contradicted"), count(r, "expired"), count(r, "short"), count(r, "no_grid"), count(r, "edge"), count(r, "ambiguous"),
                count(r, "late"), count(r, "same"), steps.c_str());
        }
        scenario_name = "predictive summary";
        // 1. Cockpit -> sky, 20 degrees: one step of 12 mm, located inside the saccade, confirmed; then the gradual rate.
        {
            const auto& sc = r1.saccades.at(0);
            CHECK(r1.jumps.size() == 1 && r1.jumps[0].mm > 11.9 && r1.jumps[0].before_end > 0, "cockpit -> sky: one 12 mm step inside the saccade",
                r1.jumps.empty() ? 0.0 : r1.jumps[0].mm, r1.jumps.empty() ? 0.0 : r1.jumps[0].before_end * 1000, -1);
            CHECK(count(r1, "confirmed") == 1 && count(r1, "contradicted") == 0, "cockpit -> sky: confirmed by the cone", count(r1, "confirmed"), count(r1, "contradicted"), -1);
            const double p12 = reach(r1, sc.onset, 1.2), g12 = reach(r1g, sc.onset, 1.2);
            CHECK(p12 > 0 && g12 > 0 && p12 < g12 - 1.0, "cockpit -> sky: x1.2 reached over a second earlier than gradual", p12, g12, -1);
            CHECK(r1.max_scale <= 1.25 + 1e-4 && reach(r1, sc.onset, 1.25) > 0, "cockpit -> sky: up to x1.25, not beyond", r1.max_scale, 1.25, -1);
            CHECK(r1g.jumps.empty() && r1l.jumps.size() <= 1, "gradual: no step; lead 1 frame: at most the one", static_cast<double>(r1g.jumps.size()), static_cast<double>(r1l.jumps.size()), -1);
            CHECK(r1m.jumps.size() == 1 && r1m.jumps[0].mm > 11.9, "cockpit -> sky: the same with a median grid", static_cast<double>(r1m.jumps.size()), 1, -1);
            std::printf("cockpit -> sky 20 deg: x1.2 after %.2f s (gradual %.2f s), x1.25 after %.2f s (gradual %.2f s)\n", p12, g12, reach(r1, sc.onset, 1.25), reach(r1g, sc.onset, 1.25));
        }
        // 2. Sky -> panel: one step down (the panel's disparity cap, 0.4 deg at 0.7 m = 4.9 mm), back to exactly x1 fast.
        {
            const auto& sc = r2.saccades.at(0);
            CHECK(r2.jumps.size() == 1 && r2.jumps[0].mm < -4.0 && r2.jumps[0].mm > -5.5, "sky -> panel: one step down of about 4.9 mm", r2.jumps.empty() ? 0.0 : r2.jumps[0].mm, -4.9, -1);
            const double pr = back_to_1(r2, sc.onset), gr = back_to_1(r2g, sc.onset);
            CHECK(pr > 0 && pr < 0.8 && gr > 0 && pr <= gr + 1e-6, "sky -> panel: back to x1 within 0.8 s, no later than gradual", pr, gr, -1);
            CHECK(count(r2, "contradicted") == 0, "sky -> panel: not contradicted", count(r2, "contradicted"), 0, -1);
            std::printf("sky -> panel: back to x1 %.2f s after the onset (gradual %.2f s)\n", pr, gr);
        }
        // 3. The canopy strut's edge: no step (edge).
        // (The cone, 0.3 deg beyond the strut, may still see far: the gradual path then ramps at its rates.)
        CHECK(r3.jumps.empty() && count(r3, "edge") + count(r3, "ambiguous") >= 1, "canopy strut edge: no step (edge or mixed landing cell)", static_cast<double>(r3.jumps.size()),
            count(r3, "edge") + count(r3, "ambiguous"), -1);
        // 4. A 4 degree saccade: no step (short).
        CHECK(r4.jumps.empty() && count(r4, "short") + (count(r4, "predicted") == 0 ? 1 : 0) >= 1, "4 deg: no step, short", static_cast<double>(r4.jumps.size()), count(r4, "short"), -1);
        // 5. Mispredicted: one step up, contradicted, back to x1 at the rates; no second step, no oscillation.
        {
            const auto& sc = r5.saccades.at(0);
            CHECK(r5.jumps.size() == 1 && r5.jumps[0].mm > 0, "mispredicted: one step up", static_cast<double>(r5.jumps.size()), r5.jumps.empty() ? 0.0 : r5.jumps[0].mm, -1);
            CHECK(count(r5, "contradicted") == 1, "mispredicted: contradicted by the cone", count(r5, "contradicted"), 1, -1);
            unsigned reversals = 0; int dir = 0; double last = 1;
            for (const auto& f : r5.frames) {
                if (f.t_loc < sc.onset) { last = f.scale; continue; }
                const int d = f.scale > last + 1e-6 ? 1 : f.scale < last - 1e-6 ? -1 : 0;
                if (d != 0 && dir != 0 && d != dir) ++reversals;
                if (d != 0) dir = d;
                last = f.scale;
            }
            const double back = back_to_1(r5, sc.end + 0.1);
            CHECK(reversals == 1, "mispredicted: up once, then only down (no oscillation)", reversals, 1, -1);
            CHECK(back >= 0 && back < 0.8, "mispredicted: back to x1 within 0.8 s of the landing", back, 0.8, -1);
            std::printf("mispredicted: step %+.2f mm, contradicted, back to x1 %.2f s after the landing\n", r5.jumps.empty() ? 0.0 : r5.jumps[0].mm, back + 0.1);
        }
        // 6. Without PVR: gradual in effect, the reason logged, the status says so, no step.
        CHECK(r6.jumps.empty() && r6.fallback_logged && !r6.sampler_started, "no PVR: gradual, the reason logged", r6.fallback_logged, r6.sampler_started, -1);
        CHECK(std::all_of(r6.frames.begin(), r6.frames.end(), [](const PredFrame& f) { return f.fallback && !f.predictive; }), "no PVR: status bit 61, never bit 60", 0, 0, -1);
        CHECK(std::any_of(r1.frames.begin(), r1.frames.end(), [](const PredFrame& f) { return f.predictive; }), "PVR: status bit 60", 0, 0, -1);
        // 7. Live switch from the ini: the sampler starts, the step follows.
        CHECK(r7.sampler_started && r7.sampler_stopped && r7.jumps.size() == 1, "live switch to predictive: sampler started, one step", r7.sampler_started, static_cast<double>(r7.jumps.size()), -1);
        CHECK(r1.sampler_started && r1.sampler_stopped && !r1g.sampler_started, "the sampler runs only for predictive, stopped at xrDestroyInstance", r1.sampler_started, r1g.sampler_started, -1);
        // The sweep: steps by amplitude (up and down saccades, four phases each).
        std::printf("\nsweep: steps per amplitude (of 4 up + 4 down saccades)  %-6s %-6s %-6s %-6s %-6s\n", "15", "20", "25", "30", "35");
        for (const auto* r : {&rs, &rs1, &rs120}) {
            unsigned up[5]{}, down[5]{};
            for (const auto& j : r->jumps) {
                if (j.saccade < 0) continue;
                const auto& l = r->saccades[j.saccade].label;
                const int a = (std::stoi(l.substr(l.find(' ') + 1)) - 15) / 5;
                if (a >= 0 && a < 5) (l.rfind("up", 0) == 0 ? up : down)[a]++;
            }
            std::printf("  %-50s", r->name.c_str());
            for (int a = 0; a < 5; ++a) std::printf(" %u+%u   ", up[a], down[a]);
            std::printf("\n");
            CHECK(count(*r, "contradicted") == 0, "sweep: no contradiction", count(*r, "contradicted"), 0, -1);
        }
        // 120 Hz, each sample repeated to the 500 Hz sampler (its duplicate filter keeps one): still steps, not only "no
        // contradiction" (zero steps would pass that).
        CHECK(rs120.jumps.size() >= 12, "sweep: a 120 Hz tracker still steps on most saccades of 20 deg and more", static_cast<double>(rs120.jumps.size()), 12, -1);
        CHECK(rs.jumps.size() >= rs1.jumps.size() && rs.jumps.size() >= 16, "sweep: lead 0.5 steps on most saccades of 20 deg and more", static_cast<double>(rs.jumps.size()), static_cast<double>(rs1.jumps.size()), -1);
    }

    // [DlssPreset] IgnoreDriverPreset (Periphery DLSS preset): none of the scenarios above set the key, so nvapi64.dll's
    // export is still NvAPI's own; with the key the layer installs the read-only DRS preset filter at xrCreateInstance
    // and logs it. Last on purpose: the install is process-wide and pins that copy of the layer. Without an NVIDIA
    // driver the layer logs that nvapi64.dll is not available and changes nothing.
    {
        const auto marked = [](FARPROC p) {
            const auto* b = reinterpret_cast<const unsigned char*>(p);
            return p && b[0] == 0xFF && b[1] == 0x25 && std::memcmp(b + 14, "DCSVRDRS", 8) == 0;
        };
        HMODULE nv = GetModuleHandleW(L"nvapi64.dll");
        CHECK(!nv || !marked(GetProcAddress(nv, "nvapi_QueryInterface")), "DLSS preset: without the key the layer leaves nvapi64.dll alone", 0, 0, -1);
        fake = Fake{};
        const fs::path folder = out / "dlss_preset_filter";
        fs::create_directories(folder);
        const fs::path copy = folder / "XR_APILAYER_DCSVR_pupil_shift.dll";
        fs::copy_file(dll, copy, fs::copy_options::overwrite_existing);
        { std::ofstream f(folder / "PupilShift.ini", std::ios::binary); f << "[PupilShift]\r\nEnabled=0\r\nOpenXrEyeGaze=0\r\n[DepthBoost]\r\nEnabled=0\r\n[DlssPreset]\r\nIgnoreDriverPreset=1\r\n"; }
        Layer layer;
        if (!load_layer(copy, layer)) fail("DLSS preset: layer loads");
        else {
            CHECK(layer.destroy_instance(layer.instance) == XR_SUCCESS, "DLSS preset: xrDestroyInstance", 0, 0, -1);
            std::ifstream log(folder / "PupilShift.log");
            const std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
            nv = GetModuleHandleW(L"nvapi64.dll");
            if (nv) {
                CHECK(text.find("dlss preset: driver preset filter installed (IgnoreDriverPreset=1); DLSS gets no forced preset") != std::string::npos, "DLSS preset: log says installed", 0, 0, -1);
                CHECK(marked(GetProcAddress(nv, "nvapi_QueryInterface")), "DLSS preset: nvapi_QueryInterface now goes through the filter's thunk", 0, 0, -1);
                std::printf("DLSS preset: the filter is installed in this process (nvapi64.dll present)\n");
            } else {
                CHECK(text.find("dlss preset: driver preset filter nvapi64.dll not available") != std::string::npos, "DLSS preset: log says no NVIDIA driver", 0, 0, -1);
                std::printf("DLSS preset: no nvapi64.dll on this PC: the layer logged it and changed nothing\n");
            }
        }
    }

    if (!failure_kinds.empty()) {
        std::printf("\nFailures by kind:\n");
        for (const auto& [kind, count] : failure_kinds) std::printf("%6d %s\n", count, kind.c_str());
    }
    std::printf("\nCSV and SVG traces in %ls\n%d failure(s)\n", out.c_str(), failures);
    return failures == 0 ? 0 : 1;
}
