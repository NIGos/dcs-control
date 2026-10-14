// DCS Control: offline checks of periphery stability (native/quadviews/dcsvr_perception.h and dcsvr_temporal_cs.hlsl).
// Built and run by scripts/build-quadviews.ps1. Exit code 0 when every check passes.
//
//   perception_test.exe <native/quadviews dir> [--warp] [--quick]
//
// Part 1 checks the shared maths on the CPU: eccentricity, the weight, the time-based blend, the flicker fusion
// model against the published values, the reset decisions and the reprojection.
// Part 2 runs the real compute shader on a D3D11 device (the GPU, or WARP with --warp or without one) over synthetic
// periphery sequences rendered from a scene defined in world directions, so every frame has an exact ground truth:
//   (a) a still scene with per-frame sub-pixel jitter (aliasing shimmer of thin lines and a fine checker),
//   (b) a steady head turn the reprojection explains,
//   (c) a horizon roll it cannot explain (no motion vectors),
//   (d) the saccade, gap and size resets,
//   (e) the same step at 2X, 3X and Auto frame times,
// and times the pass at the real periphery size (814 x 804 per eye).
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <execution>
#include <filesystem>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "dcsvr_perception.h"

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;
float g_clip = 0.f, g_confidence = 0.f, g_tauMs = 0.f;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}
bool Near(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}

constexpr float Rad = 0.0174532925f;
constexpr int64_t Ms = 1'000'000;

dcsvr::Quat Yaw(float deg) {
    return {0.f, std::sin(deg * Rad / 2.f), 0.f, std::cos(deg * Rad / 2.f)};
}
dcsvr::Quat Roll(float deg) {  // about the view axis
    return {0.f, 0.f, std::sin(deg * Rad / 2.f), std::cos(deg * Rad / 2.f)};
}

// ---- Part 1: the maths ---------------------------------------------------------------------------------------------

void MathChecks() {
    using namespace dcsvr;
    Check(Near(DcsvrEccentricityDeg(0.f, 0.f, 0.f, 0.f, -1.f), 0.f, 0.05f), "eccentricity: the view centre at a centred gaze is 0 degrees");
    Check(Near(DcsvrEccentricityDeg(std::tan(30.f * Rad), 0.f, 0.f, 0.f, -1.f), 30.f, 0.01f), "eccentricity: tan 30 degrees right is 30 degrees");
    {
        const float g[3] = {std::sin(10.f * Rad), 0.f, -std::cos(10.f * Rad)};
        Check(Near(DcsvrEccentricityDeg(std::tan(-20.f * Rad), 0.f, g[0], g[1], g[2]), 30.f, 0.01f), "eccentricity: 20 degrees left of a gaze 10 degrees right is 30 degrees");
        Check(Near(DcsvrEccentricityDeg(0.f, std::tan(40.f * Rad), 0.f, 0.f, -1.f), 40.f, 0.01f), "eccentricity: vertical axis");
    }
    // Weight: zero inside the focus radius, full after the ramp, fading out between the far eccentricities.
    Check(DcsvrStabilityWeight(19.9f, 20.f, 8.f, 19.9f, 50.f, 60.f) == 0.f, "weight: zero inside the focus area");
    Check(Near(DcsvrStabilityWeight(24.f, 20.f, 8.f, 24.f, 50.f, 60.f), 0.5f, 0.001f), "weight: half way up the ramp");
    Check(DcsvrStabilityWeight(28.f, 20.f, 8.f, 28.f, 50.f, 60.f) == 1.f, "weight: full from focus radius + ramp");
    Check(DcsvrStabilityWeight(40.f, 20.f, 8.f, 61.f, 50.f, 60.f) == 0.f, "weight: zero beyond the far end");
    {
        float previous = -1.f;
        bool monotonic = true;
        for (float e = 0.f; e < 50.f; e += 0.25f) {
            const float w = DcsvrStabilityWeight(e, 20.f, 8.f, e, 50.f, 60.f);
            monotonic &= w >= previous;
            previous = w;
        }
        Check(monotonic, "weight: rises monotonically from the focus edge");
    }
    // Focus edge as the composition draws it: an elongated focus view (22 x 18 degrees half-widths) ramps up from its
    // own edge on both axes, the round shape from the superellipse, the rectangle (shape 0) from its sides.
    {
        const float l = -std::tan(22.f * Rad), r = std::tan(22.f * Rad), u = std::tan(18.f * Rad), d = -std::tan(18.f * Rad);
        const float rx = 22.f, ry = 18.f;
        Check(DcsvrFocusEdgeDeg(0.f, 0.f, l, r, u, d, rx, ry, 2.f) < 0.f, "focus edge: the centre is inside");
        Check(Near(DcsvrFocusEdgeDeg(r, 0.f, l, r, u, d, rx, ry, 2.f), 0.f, 1e-4f) && Near(DcsvrFocusEdgeDeg(0.f, u, l, r, u, d, rx, ry, 2.f), 0.f, 1e-4f), "focus edge: zero at the edge on the long and the short axis");
        Check(DcsvrFocusEdgeDeg(std::tan(20.f * Rad), 0.f, l, r, u, d, rx, ry, 2.f) < 0.f, "focus edge: 20 degrees along the 22-degree axis is still inside (the smaller radius said outside)");
        Check(DcsvrFocusEdgeDeg(0.7f * r, 0.7f * u, l, r, u, d, rx, ry, 2.f) < 0.f && DcsvrFocusEdgeDeg(0.75f * r, 0.75f * u, l, r, u, d, rx, ry, 2.f) > 0.f, "focus edge: the round shape's diagonal edge at 1/sqrt(2)");
        Check(DcsvrFocusEdgeDeg(0.95f * r, 0.95f * u, l, r, u, d, rx, ry, 0.f) < 0.f, "focus edge: the rectangle's corner is inside for shape 0");
        Check(DcsvrStabilityWeight(20.f + DcsvrFocusEdgeDeg(std::tan(20.f * Rad), 0.f, l, r, u, d, rx, ry, 2.f), 20.f, 8.f, 20.f, 50.f, 60.f) == 0.f, "focus edge: no weight inside the long axis");
    }
    // Time-based blend: after 100 ms the history keeps the same share at 45 FPS (2X), 30 FPS (3X) and mixed (Auto).
    {
        const float tau = 0.04f;
        float keep45 = 1.f, keep30 = 1.f, keepAuto = 1.f, t = 0.f;
        for (int i = 0; i < 9; ++i) keep45 *= DcsvrHistoryBlend(1.f / 90.f, tau, 1.f);   // 9 x 11.1 ms = 100 ms
        for (int i = 0; i < 6; ++i) keep30 *= DcsvrHistoryBlend(1.f / 60.f, tau, 1.f);   // 6 x 16.7 ms
        for (int i = 0; t < 0.0999f; ++i) {
            const float dt = i % 2 ? 1.f / 45.f : 1.f / 30.f;
            const float step = std::min(dt, 0.1f - t);
            keepAuto *= DcsvrHistoryBlend(step, tau, 1.f);
            t += step;
        }
        Check(Near(keep45, std::exp(-2.5f), 1e-4f) && Near(keep30, keep45, 1e-4f) && Near(keepAuto, keep45, 1e-4f), "blend: the same history share after 100 ms in 2X, 3X and Auto");
        Check(DcsvrHistoryBlend(0.0222f, tau, 0.f) == 0.f && DcsvrHistoryBlend(0.0222f, 0.f, 1.f) == 0.f, "blend: no history at zero weight or zero time constant");
        Check(Near(DcsvrHistoryBlend(0.0222f, 0.04f, 1.f), 0.574f, 0.002f), "blend: 0.574 at 45 FPS with 40 ms");
    }
    Check(Near(DcsvrClipConfidence(0.f, 0.1f, 1.f), 1.f, 1e-6f) && DcsvrClipConfidence(0.2f, 0.1f, 1.f) == 0.f, "confidence: full inside the box, none far outside");
    // The flicker fusion model against the study's evaluated table (380 cd/m2, Hz, rounded).
    {
        const float table[3][6] = {{59, 68, 73, 73, 70, 61}, {51, 57, 60, 58, 53, 43}, {42, 42, 38, 31, 19, 4}};
        const float fs[3] = {0.5f, 1.f, 2.f};
        bool ok = true;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 6; ++j) ok &= Near(CriticalFlickerHz(10.f * j, fs[i]), table[i][j], 1.f);
        Check(ok, "flicker fusion: Krajancich 2021 values at 0.5, 1 and 2 cpd, 0 to 50 degrees");
        Check(Near(CriticalFlickerHz(20.f, 1.f, 30.f), 52.f, 1.5f) && Near(CriticalFlickerHz(40.f, 2.f, 30.f), 14.f, 1.5f), "flicker fusion: luminance scaling at 30 cd/m2");
        Check(Near(AcuityCpd(10.f), 7.3f, 0.1f) && Near(AcuityCpd(20.f), 4.1f, 0.1f) && Near(AcuityCpd(30.f), 2.8f, 0.1f), "acuity: Geisler-Perry values");
    }
    {
        const FarFalloff f45 = ComputeFarFalloff(45.f), f30 = ComputeFarFalloff(30.f);
        std::printf("     far falloff: 2X (45 FPS) %.1f-%.1f deg, 3X (30 FPS) %.1f-%.1f deg\n", f45.startDeg, f45.endDeg, f30.startDeg, f30.endDeg);
        Check(f45.startDeg >= 20.f && f45.endDeg > f45.startDeg && f30.startDeg >= f45.startDeg, "far falloff: starts beyond 20 degrees, later at a lower frame rate");
    }
    // Controller: reset reasons, both eyes alike.
    {
        TemporalSettings s;
        s.enabled = true;
        TemporalController c;
        TemporalEye eyes[2];
        for (auto& e : eyes) {
            e.width = 814;
            e.height = 804;
            e.fov = {-1.2f, 1.2f, 1.2f, -1.2f};
        }
        GazeSample gaze{};
        int64_t t = 1'000'000'000;
        Check(c.BeginFrame(t, eyes, 1, gaze, true, s) == TemporalReset::First, "reset: the first frame");
        Check(c.BeginFrame(t += 22 * Ms, eyes, 1, gaze, true, s) == TemporalReset::None, "reset: none on the next 2X frame");
        TemporalConstants k{};
        c.Constants(0, {-0.36f, 0.36f, 0.36f, -0.36f}, s, k);
        Check(Near(k.params[0], 0.022f, 1e-4f) && Near(k.params[1], 0.04f, 1e-6f) && Near(k.focus[3], 19.8f, 0.1f), "constants: frame interval, time constant, focus radius");
        Check(c.BeginFrame(t, eyes, 1, gaze, true, s) == TemporalReset::None, "reset: a second layer of the same frame repeats the decision");
        Check(c.Active() && c.Time() == t && c.PreviousTime() == t - 22 * Ms, "frame times: this frame and the one before, a second layer leaves them");
        {
            const uint64_t ps = PeripheryStatus().load();
            Check((ps & 1) && (ps & 2) && (ps & 4) && ((ps >> 8) & 0xFF) == 50 && ((ps >> 48) & 0xFF) != 0, "panel status: on, filtered, valid, strength 0.50, updating");
        }
        {
            TemporalSettings zero = s;
            zero.tauMs = 0.f;
            TemporalController z;
            z.BeginFrame(t, eyes, 1, gaze, true, zero);
            Check(!z.Active(), "strength 0: the pass does not run");
        }
        {
            TemporalSettings shaped = s;
            shaped.focusShape = 2.f;
            c.Constants(0, {-0.4f, 0.36f, 0.3f, -0.36f}, shaped, k);
            Check(k.focusTan[0] == -0.4f && k.focusTan[2] == 0.3f && k.focusShape[2] == 2.f && Near(k.focusShape[0], 0.5f * (std::atan(0.36f) + std::atan(0.4f)) / Rad, 1e-3f), "constants: focus edge field, half-widths and shape");
        }
        Check(c.BeginFrame(t += 150 * Ms, eyes, 1, gaze, true, s) == TemporalReset::Gap, "reset: a 150 ms gap");
        c.BeginFrame(t += 22 * Ms, eyes, 1, gaze, true, s);
        gaze.inSaccade = true;
        Check(c.BeginFrame(t += 22 * Ms, eyes, 1, gaze, true, s) == TemporalReset::Saccade, "reset: a saccade");
        c.Constants(1, {-0.36f, 0.36f, 0.36f, -0.36f}, s, k);
        Check(k.params[1] == 0.f, "reset: no history for the other eye either");
        gaze.inSaccade = false;
        eyes[1].width = 700;
        Check(c.BeginFrame(t += 22 * Ms, eyes, 1, gaze, true, s) == TemporalReset::Size, "reset: one eye's size changes");
        Check(c.BeginFrame(t += 22 * Ms, eyes, 2, gaze, true, s) == TemporalReset::Space, "reset: another layer space");
        SceneCuts().fetch_add(1u);
        Check(c.BeginFrame(t += 22 * Ms, eyes, 2, gaze, true, s) == TemporalReset::Scene && c.BeginFrame(t += 22 * Ms, eyes, 2, gaze, true, s) == TemporalReset::None,
              "reset: a scene cut from the export, once");
        eyes[0].pose = eyes[1].pose = Yaw(12.f);
        Check(c.BeginFrame(t += 22 * Ms, eyes, 2, gaze, true, s) == TemporalReset::Turn, "reset: a 12-degree turn in one frame");
        eyes[0].pose = eyes[1].pose = Yaw(16.f);
        Check(c.BeginFrame(t += 22 * Ms, eyes, 2, gaze, true, s) == TemporalReset::None, "reset: none for a 4-degree turn");
        Check(c.BeginFrame(t += 22 * Ms, eyes, 2, gaze, false, s) == TemporalReset::Inactive && c.BeginFrame(t += 22 * Ms, eyes, 2, gaze, true, s) == TemporalReset::First, "reset: after an inactive frame it starts over");
        const auto& st = c.Stats();
        Check(st.gap == 1 && st.saccade == 1 && st.size == 1 && st.turn == 1 && st.other == 2, "statistics count each reason");
        // Reprojection: after a 4-degree turn to the left (positive yaw), a ray straight ahead was 4 degrees left of
        // straight ahead in the previous frame's eye.
        eyes[0].pose = eyes[1].pose = Yaw(20.f);
        c.BeginFrame(t += 22 * Ms, eyes, 2, gaze, true, s);
        c.Constants(0, {-0.36f, 0.36f, 0.36f, -0.36f}, s, k);
        const float ray[3] = {0.f, 0.f, -1.f};
        float prev[3];
        for (int r = 0; r < 3; ++r) prev[r] = k.reproject[r][0] * ray[0] + k.reproject[r][1] * ray[1] + k.reproject[r][2] * ray[2];
        Check(Near(std::atan2(prev[0], -prev[2]) / Rad, -4.f, 0.01f), "reprojection: a 4-degree yaw moves the ray 4 degrees");
    }
    // Gaze into an eye's frame: the cyclopean gaze is the same world direction for both eyes.
    {
        TemporalSettings s;
        s.enabled = true;
        TemporalController c;
        TemporalEye eyes[2];
        eyes[0].eyeInView = Yaw(5.f);    // canted displays: the left eye turned 5 degrees outward
        eyes[1].eyeInView = Yaw(-5.f);
        for (auto& e : eyes) e.width = e.height = 100;
        GazeSample gaze{1, true, false, {0.f, 0.f, -1.f}};
        c.BeginFrame(1, eyes, 1, gaze, true, s);
        TemporalConstants l{}, r{};
        c.Constants(0, {-0.3f, 0.3f, 0.3f, -0.3f}, s, l);
        c.Constants(1, {-0.3f, 0.3f, 0.3f, -0.3f}, s, r);
        Check(Near(std::atan2(l.gaze[0], -l.gaze[2]) / Rad, 5.f, 0.01f) && Near(std::atan2(r.gaze[0], -r.gaze[2]) / Rad, -5.f, 0.01f) && l.gaze[3] == 1.f, "gaze: one view-space gaze, rotated into each canted eye");
        GazeSample none{};
        c.BeginFrame(2, eyes, 1, none, true, s);
        c.Constants(0, {-0.1f, 0.5f, 0.3f, -0.3f}, s, l);
        Check(l.gaze[3] == 0.f && Near(l.gaze[0], l.focus[0], 1e-6f), "gaze: without a gaze the focus centre stands for it");
    }
    {
        dcsvr::GazeHistory h;
        h.Record({100 * Ms, true, false, {0.f, 0.f, -1.f}});
        h.Record({122 * Ms, true, true, {0.f, 0.f, -1.f}});
        Check(h.At(122 * Ms).inSaccade && !h.At(121 * Ms).inSaccade && h.At(130 * Ms).time == 122 * Ms && !h.At(400 * Ms).valid, "gaze history: exact, latest before, nothing stale");
    }
    {
        // DcsVr_QuadViewsGaze: flags and the one combined gaze in each canted eye's frame.
        dcsvr::GazeHistory h;
        const Quat eyeInView[2] = {Yaw(3.f), Yaw(-3.f)};  // Pimax-like: each eye turned 3 degrees outwards
        GazeSample valid{100 * Ms, true, false, {std::sin(10.f * Rad), 0.f, -std::cos(10.f * Rad)}};
        valid.SetEyes(eyeInView);
        h.Record(valid);
        GazeSample saccade{111 * Ms, true, true, {0.f, 0.f, -1.f}};
        saccade.SetEyes(eyeInView);
        h.Record(saccade);
        h.Record({122 * Ms, false, false, {0.f, 0.f, -1.f}});  // no gaze: Quad Views shows its fixed focus
        float l[3], r[3];
        const uint32_t fv = dcsvr::ExportGaze(h.At(100 * Ms), l, r);
        Check(fv == (dcsvr::GazeExportSample | dcsvr::GazeExportValid) && Near(std::atan2(l[0], -l[2]) / Rad, 13.f, 0.01f) &&
                  Near(std::atan2(r[0], -r[2]) / Rad, 7.f, 0.01f) && Near(l[0] * l[0] + l[1] * l[1] + l[2] * l[2], 1.f, 1e-5f),
              "gaze export: a valid gaze, 10 degrees right in view space, 13 (left eye) and 7 (right eye) degrees in the canted eyes");
        Check(dcsvr::ExportGaze(h.At(111 * Ms), l, r) == (dcsvr::GazeExportSample | dcsvr::GazeExportValid | dcsvr::GazeExportSaccade),
              "gaze export: the saccade flag");
        const uint32_t fi = dcsvr::ExportGaze(h.At(122 * Ms), l, r);
        Check(fi == dcsvr::GazeExportSample && l[0] == 0.f && l[2] == -1.f && r[0] == 0.f,
              "gaze export: no gaze is a sample without the valid flag (the fixed focus is not a gaze), straight ahead");
        Check(dcsvr::ExportGaze(h.At(125 * Ms), l, r) == dcsvr::GazeExportSample, "gaze export: a display time just after a sample gets that sample");
        Check(dcsvr::ExportGaze(h.At(400 * Ms), l, r) == 0u && dcsvr::ExportGaze(h.At(50 * Ms), nullptr, nullptr) == 0u,
              "gaze export: nothing recorded within 100 ms, no flags");
        Check(&dcsvr::SharedGazeHistory() == &dcsvr::SharedGazeHistory(), "gaze export: one shared history");
    }
}

// ---- Part 2: the shader on synthetic sequences -----------------------------------------------------------------------

// The scenes, in world directions, grey. Lines (the worst case): thin bright lines (canopy frame, power lines) every
// degree or so along azimuth and elevation and a fine checker above the periphery's Nyquist frequency (aliasing), on
// a dim background. Horizon: sky over ground with a road every 6 degrees and a few thin wires, mostly flat areas.
enum class SceneKind { Lines, Horizon };
SceneKind g_scene = SceneKind::Lines;

float Scene(const float d[3]) {
    const float az = std::atan2(d[0], -d[2]) / Rad, el = std::asin(std::clamp(d[1], -1.f, 1.f)) / Rad;
    const auto frac = [](float x) { return x - std::floor(x); };
    if (g_scene == SceneKind::Horizon) {
        if (el > 2.f) return (frac(el / 15.f) < 0.02f ? 0.15f : 0.65f + 0.002f * el);  // sky, a wire every 15 degrees
        return frac(az / 6.f) < 0.05f ? 0.55f : 0.22f;                                  // ground, roads every 6 degrees
    }
    const float lines = std::max(frac(az * 0.9f) < 0.12f ? 1.f : 0.f, frac(el * 0.6f + 0.3f) < 0.1f ? 0.9f : 0.f);
    const float checker = (int(std::floor(az * 3.f)) + int(std::floor(el * 3.f))) & 1 ? 0.12f : 0.f;
    return 0.08f + 0.8f * lines + checker;
}

struct Frame {
    int w, h;
    std::vector<float> v;
};

// Renders the periphery for an eye orientation (pose) and an extra world rotation (content moving without the head):
// one point sample per pixel at a sub-pixel jitter (DCS's residual shimmer), or a 4x4 supersampled ground truth.
Frame Render(int w, int h, const dcsvr::TanFov& fov, const dcsvr::Quat& pose, const dcsvr::Quat& world, float jx, float jy, bool truth) {
    Frame f{w, h, std::vector<float>(size_t(w) * h)};
    const dcsvr::Quat toScene = dcsvr::Multiply(dcsvr::Conjugate(world), pose);
    std::vector<int> rows(h);
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int y) {
        for (int x = 0; x < w; ++x) {
            float sum = 0.f;
            const int n = truth ? 4 : 1;
            for (int sy = 0; sy < n; ++sy)
                for (int sx = 0; sx < n; ++sx) {
                    const float px = truth ? x + (sx + 0.5f) / n : x + 0.5f + jx;
                    const float py = truth ? y + (sy + 0.5f) / n : y + 0.5f + jy;
                    const float ray[3] = {fov.left + (fov.right - fov.left) * px / w, fov.up + (fov.down - fov.up) * py / h, -1.f};
                    float d[3];
                    dcsvr::Rotate(toScene, ray, d);
                    const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    for (float& c : d) c /= l;
                    sum += Scene(d);
                }
            f.v[size_t(y) * w + x] = sum / (n * n);
        }
    });
    return f;
}

void HrCheck(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        std::printf("FAIL %s (0x%08lx)\n", what, (unsigned long)hr);
        std::exit(2);
    }
}

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Texture2D> input, history[2], staging;
    ComPtr<ID3D11ShaderResourceView> inputSrv, historySrv[2];
    ComPtr<ID3D11UnorderedAccessView> historyUav[2];
    int w = 0, h = 0, next = 0;
    std::string adapter;
    bool warp = false;
    bool srgb = false, inputSrgb = false;  // the input as an 8-bit sRGB texture (the next Resize recreates it)

    Gpu(const std::filesystem::path& dir, bool forceWarp) {
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        if (forceWarp || FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context))) {
            HrCheck(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context), "WARP device");
            warp = true;
        }
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> a;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&a)) && SUCCEEDED(a->GetDesc(&desc))) {
            char name[128]{};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
            adapter = name;
        }
        ComPtr<ID3DBlob> blob, errors;
        const auto source = (dir / "dcsvr_temporal_cs.hlsl").wstring();
        if (FAILED(D3DCompileFromFile(source.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors))) {
            std::printf("FAIL shader compile: %s\n", errors ? (const char*)errors->GetBufferPointer() : "?");
            std::exit(2);
        }
        HrCheck(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader), "compute shader");
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(dcsvr::TemporalConstants);
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HrCheck(device->CreateBuffer(&cb, nullptr, &constants), "constants");
        D3D11_SAMPLER_DESC s{};
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;  // the layer's linear clamp sampler
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.MaxAnisotropy = 1;
        s.MinLOD = D3D11_MIP_LOD_BIAS_MIN;
        s.MaxLOD = D3D11_MIP_LOD_BIAS_MAX;
        HrCheck(device->CreateSamplerState(&s, &sampler), "sampler");
    }

    // As the layer: two RGBA16F textures per eye, written in turn; a new size starts over.
    void Resize(int width, int height) {
        if (width == w && height == h && srgb == inputSrgb) return;
        w = width;
        h = height;
        inputSrgb = srgb;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        // DCS's swapchain may be 8-bit sRGB: its typed view reads linear values, as the float input gives them.
        d.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HrCheck(device->CreateTexture2D(&d, nullptr, input.ReleaseAndGetAddressOf()), "input");
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        HrCheck(device->CreateShaderResourceView(input.Get(), nullptr, inputSrv.ReleaseAndGetAddressOf()), "input view");
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        for (int i = 0; i < 2; ++i) {
            HrCheck(device->CreateTexture2D(&d, nullptr, history[i].ReleaseAndGetAddressOf()), "history");
            HrCheck(device->CreateShaderResourceView(history[i].Get(), nullptr, historySrv[i].ReleaseAndGetAddressOf()), "history view");
            HrCheck(device->CreateUnorderedAccessView(history[i].Get(), nullptr, historyUav[i].ReleaseAndGetAddressOf()), "history uav");
        }
        d.BindFlags = 0;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HrCheck(device->CreateTexture2D(&d, nullptr, staging.ReleaseAndGetAddressOf()), "staging");
    }

    void Upload(const Frame& f) {
        if (srgb) {
            std::vector<uint8_t> bytes(size_t(f.w) * f.h * 4);
            for (size_t i = 0; i < f.v.size(); ++i) {
                const float x = std::clamp(f.v[i], 0.f, 1.f);
                const float e = x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.f / 2.4f) - 0.055f;
                bytes[i * 4] = bytes[i * 4 + 1] = bytes[i * 4 + 2] = uint8_t(std::lround(e * 255.f));
                bytes[i * 4 + 3] = 255;
            }
            context->UpdateSubresource(input.Get(), 0, nullptr, bytes.data(), f.w * 4, 0);
            return;
        }
        std::vector<float> rgba(size_t(f.w) * f.h * 4);
        for (size_t i = 0; i < f.v.size(); ++i) rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = f.v[i], rgba[i * 4 + 3] = 1.f;
        context->UpdateSubresource(input.Get(), 0, nullptr, rgba.data(), f.w * 16, 0);
    }

    void Dispatch(const dcsvr::TemporalConstants& k) {
        D3D11_MAPPED_SUBRESOURCE m;
        HrCheck(context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "map");
        memcpy(m.pData, &k, sizeof(k));
        context->Unmap(constants.Get(), 0);
        const int write = next, read = 1 - next;
        ID3D11ShaderResourceView* srvs[] = {inputSrv.Get(), historySrv[read].Get()};
        context->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->CSSetShaderResources(0, 2, srvs);
        context->CSSetUnorderedAccessViews(0, 1, historyUav[write].GetAddressOf(), nullptr);
        context->CSSetSamplers(0, 1, sampler.GetAddressOf());
        context->CSSetShader(shader.Get(), nullptr, 0);
        context->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        ID3D11ShaderResourceView* nullSrv[2] = {};
        ID3D11UnorderedAccessView* nullUav[1] = {};
        context->CSSetShaderResources(0, 2, nullSrv);
        context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
        next = read;
    }

    Frame Run(const Frame& f, const dcsvr::TemporalConstants& k) {
        Resize(f.w, f.h);
        Upload(f);
        Dispatch(k);
        context->CopyResource(staging.Get(), history[1 - next].Get());
        D3D11_MAPPED_SUBRESOURCE m;
        HrCheck(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m), "read back");
        Frame out{f.w, f.h, std::vector<float>(f.v.size())};
        for (int y = 0; y < f.h; ++y) {
            const auto* row = reinterpret_cast<const DirectX::PackedVector::HALF*>(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch);
            for (int x = 0; x < f.w; ++x) out.v[size_t(y) * f.w + x] = DirectX::PackedVector::XMConvertHalfToFloat(row[x * 4 + 1]);
        }
        context->Unmap(staging.Get(), 0);
        return out;
    }

    // GPU time of one dispatch at this size, median of runs (timestamp queries).
    double TimeMs(int width, int height, const dcsvr::TemporalConstants& k, int runs) {
        Resize(width, height);
        Frame f{width, height, std::vector<float>(size_t(width) * height, 0.5f)};
        Upload(f);
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        ComPtr<ID3D11Query> disjoint, begin, end;
        HrCheck(device->CreateQuery(&qd, &disjoint), "query");
        qd.Query = D3D11_QUERY_TIMESTAMP;
        HrCheck(device->CreateQuery(&qd, &begin), "query");
        HrCheck(device->CreateQuery(&qd, &end), "query");
        std::vector<double> ms;
        for (int i = 0; i < runs; ++i) {
            context->Begin(disjoint.Get());
            context->End(begin.Get());
            Dispatch(k);
            context->End(end.Get());
            context->End(disjoint.Get());
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            while (context->GetData(disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE) {}
            UINT64 t0 = 0, t1 = 0;
            while (context->GetData(begin.Get(), &t0, sizeof(t0), 0) == S_FALSE) {}
            while (context->GetData(end.Get(), &t1, sizeof(t1), 0) == S_FALSE) {}
            if (!dj.Disjoint && dj.Frequency) ms.push_back(double(t1 - t0) * 1000.0 / double(dj.Frequency));
        }
        if (ms.empty()) return -1.0;
        std::sort(ms.begin(), ms.end());
        return ms[ms.size() / 2];
    }
};

struct Setup {
    int w, h;
    dcsvr::TanFov fov{-1.19f, 1.19f, 1.19f, -1.19f};      // about 50 degrees each way
    dcsvr::TanFov focus{-0.364f, 0.364f, 0.364f, -0.364f};  // a centred focus area of 20 degrees radius
    std::vector<float> ecc;                                 // eccentricity of each pixel (gaze at the centre)
    Setup(int width, int height) : w(width), h(height), ecc(size_t(width) * height) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                ecc[size_t(y) * w + x] = dcsvr::DcsvrEccentricityDeg(fov.left + (fov.right - fov.left) * (x + 0.5f) / w,
                                                                     fov.up + (fov.down - fov.up) * (y + 0.5f) / h, 0.f, 0.f, -1.f);
    }
};

const float Bands[][2] = {{0, 20}, {20, 28}, {28, 35}, {35, 45}, {45, 70}};
constexpr int BandCount = 5;

// One run of the filter over a sequence: frame times (s), head and world orientation per frame, optional resets.
struct Sequence {
    std::vector<double> times;
    std::vector<dcsvr::Quat> pose, world;
    bool jitter = true;
    bool antialiased = false;  // the input is the supersampled truth itself: any error is what the filter adds
};

struct Result {
    std::vector<Frame> in, out, truth;
};

Result RunSequence(Gpu& gpu, const Setup& su, const Sequence& seq, const dcsvr::TemporalSettings& s, bool keepTruth, std::mt19937& rng) {
    dcsvr::TemporalController c;
    std::uniform_real_distribution<float> j(-0.5f, 0.5f);
    Result r;
    for (size_t i = 0; i < seq.times.size(); ++i) {
        dcsvr::TemporalEye eyes[2];
        for (auto& e : eyes) {
            e.pose = seq.pose[i];
            e.fov = su.fov;
            e.width = su.w;
            e.height = su.h;
        }
        const dcsvr::GazeSample gaze{int64_t(seq.times[i] * 1e9), true, false, {0.f, 0.f, -1.f}};
        c.BeginFrame(gaze.time, eyes, 1, gaze, true, s);
        dcsvr::TemporalConstants k{};
        c.Constants(0, su.focus, s, k);
        const float jx = seq.jitter ? j(rng) : 0.f, jy = seq.jitter ? j(rng) : 0.f;
        Frame in = Render(su.w, su.h, su.fov, seq.pose[i], seq.world[i], jx, jy, seq.antialiased);
        r.out.push_back(gpu.Run(in, k));
        if (keepTruth) r.truth.push_back(seq.antialiased ? in : Render(su.w, su.h, su.fov, seq.pose[i], seq.world[i], 0, 0, true));
        r.in.push_back(std::move(in));
    }
    return r;
}

// Mean temporal standard deviation per band over frames [from, end).
std::vector<double> Flicker(const std::vector<Frame>& frames, const Setup& su, size_t from) {
    std::vector<double> sum(BandCount, 0.0), count(BandCount, 0.0);
    const size_t n = frames.size() - from;
    for (size_t p = 0; p < frames[0].v.size(); ++p) {
        double m = 0, m2 = 0;
        for (size_t i = from; i < frames.size(); ++i) m += frames[i].v[p], m2 += double(frames[i].v[p]) * frames[i].v[p];
        m /= n;
        const double sd = std::sqrt(std::max(m2 / n - m * m, 0.0));
        for (int b = 0; b < BandCount; ++b)
            if (su.ecc[p] >= Bands[b][0] && su.ecc[p] < Bands[b][1]) sum[b] += sd, count[b] += 1;
    }
    for (int b = 0; b < BandCount; ++b) sum[b] = count[b] ? sum[b] / count[b] : 0.0;
    return sum;
}

// RMS error against the ground truth outside the focus area plus ramp (eccentricity >= minEcc), frames [from, end).
double Rmse(const std::vector<Frame>& a, const std::vector<Frame>& truth, const Setup& su, size_t from, float minEcc, int lag = 0) {
    double e = 0, n = 0;
    for (size_t i = from; i < a.size(); ++i)
        for (size_t p = 0; p < a[i].v.size(); ++p)
            if (su.ecc[p] >= minEcc && su.ecc[p] < 45.f) {
                const double d = a[i - lag].v[p] - truth[i].v[p];
                e += d * d;
                n += 1;
            }
    return n ? std::sqrt(e / n) : 0.0;
}

double MaxAbsDiff(const Frame& a, const Frame& b) {
    double m = 0;
    for (size_t p = 0; p < a.v.size(); ++p) m = std::max(m, std::fabs(double(a.v[p]) - b.v[p]));
    return m;
}

Sequence Steady(int frames, double dt, float yawDegPerS = 0.f, float rollDegPerS = 0.f) {
    Sequence s;
    for (int i = 0; i < frames; ++i) {
        const double t = 1.0 + i * dt;
        s.times.push_back(t);
        s.pose.push_back(Yaw(float(yawDegPerS * i * dt)));
        s.world.push_back(Roll(float(rollDegPerS * i * dt)));
    }
    return s;
}

void ShaderChecks(const std::filesystem::path& dir, bool forceWarp, bool quick) {
    Gpu gpu(dir, forceWarp);
    std::printf("\nDevice: %s%s\n", gpu.adapter.c_str(), gpu.warp ? " (WARP)" : "");
    const Setup su(quick ? 204 : 407, quick ? 201 : 402);  // half the real periphery per axis, same field
    std::printf("Synthetic periphery %dx%d over +-50 degrees (half the real 814x804, same field), focus radius 20 degrees, gaze centred\n", su.w, su.h);
    dcsvr::TemporalSettings s;
    s.enabled = true;  // the defaults: 40 ms (strength 0.5), ramp 8 degrees, clip 1, confidence 1
    if (g_clip > 0.f) s.clip = g_clip;
    if (g_confidence > 0.f) s.confidence = g_confidence;
    if (g_tauMs > 0.f) s.tauMs = g_tauMs;
    std::printf("Settings: time constant %.0f ms, clip %.2f, confidence %.2f, ramp %.0f degrees\n", s.tauMs, s.clip, s.confidence, s.rampDeg);
    std::mt19937 rng(7);
    const int frames = quick ? 30 : 60;
    const size_t warm = 15;

    // (a) Still scene with per-frame jitter, 2X and 3X.
    std::printf("\n(a) Still scene, per-frame sub-pixel jitter: temporal flicker (mean per-pixel standard deviation of luma)\n");
    std::printf("| Frame rate | Band (deg) | Off | On | On / Off |\n|---|---|---|---|---|\n");
    double ratioPeriphery45 = 1, inside45 = 0, truthOff = 0, truthOn = 0;
    for (const double dt : {1.0 / 45.0, 1.0 / 30.0}) {
        Sequence seq = Steady(frames, dt);
        Result r = RunSequence(gpu, su, seq, s, dt < 0.03, rng);
        const auto off = Flicker(r.in, su, warm), on = Flicker(r.out, su, warm);
        for (int b = 0; b < BandCount; ++b)
            std::printf("| %s | %.0f-%.0f | %.4f | %.4f | %.2f |\n", dt < 0.03 ? "45 FPS (2X)" : "30 FPS (3X)", Bands[b][0], Bands[b][1], off[b], on[b], off[b] > 0 ? on[b] / off[b] : 0.0);
        if (dt < 0.03) {
            ratioPeriphery45 = on[2] / off[2];
            inside45 = std::fabs(on[0] - off[0]);
            truthOff = Rmse(r.in, r.truth, su, warm, 28.f);
            truthOn = Rmse(r.out, r.truth, su, warm, 28.f);
        }
    }
    std::printf("Error against the 4x4 supersampled truth, 28-45 degrees, 2X: off %.4f, on %.4f\n", truthOff, truthOn);
    Check(ratioPeriphery45 < 0.75, "(a) flicker 28-35 degrees falls by more than a quarter at 2X");
    Check(inside45 < 1e-3, "(a) inside the focus area the output is the input (no change in flicker)");
    Check(truthOn < truthOff, "(a) the stabilised periphery is closer to the supersampled truth than the input");
    // The same at 2X from an 8-bit sRGB swapchain (the statistics and the clamp run on the linear values its view gives).
    {
        gpu.srgb = true;
        Sequence seq = Steady(frames, 1.0 / 45.0);
        Result r = RunSequence(gpu, su, seq, s, true, rng);
        gpu.srgb = false;
        const auto off = Flicker(r.in, su, warm), on = Flicker(r.out, su, warm);
        const double tOff = Rmse(r.in, r.truth, su, warm, 28.f), tOn = Rmse(r.out, r.truth, su, warm, 28.f);
        std::printf("8-bit sRGB input, 2X: flicker 28-35 degrees %.4f -> %.4f, error against the truth %.4f -> %.4f\n", off[2], on[2], tOff, tOn);
        Check(on[2] / off[2] < 0.75 && tOn < tOff, "(a) an 8-bit sRGB swapchain is stabilised alike");
    }

    // (b) Steady head turn, explained by the reprojection. The input is anti-aliased (the supersampled truth itself),
    // so any error is what the filter adds: blur from resampling the history, or a trail.
    std::printf("\n(b) Steady head turn, anti-aliased input (error = what the filter adds): RMS error against the truth, 28-45 degrees\n");
    std::printf("| Scene | Turn | Off | On | On, not reprojected | Previous frame shown (1-frame ghost) |\n|---|---|---|---|---|---|\n");
    bool turnOk = true;
    // Speeds that do not move the line pattern by whole periods per frame.
    for (const auto scene : {SceneKind::Horizon, SceneKind::Lines})
    for (const float speed : {37.f, 113.f, 211.f}) {
        g_scene = scene;
        Sequence seq = Steady(quick ? 20 : 40, 1.0 / 45.0, speed);
        seq.jitter = false;
        seq.antialiased = true;
        const Result r = RunSequence(gpu, su, seq, s, true, rng);
        // The same frames, the filter told the head did not move: what it would do without the reprojection.
        std::vector<Frame> notReprojectedOut;
        {
            dcsvr::TemporalController c;
            for (size_t i = 0; i < seq.times.size(); ++i) {
                dcsvr::TemporalEye eyes[2];
                for (auto& e : eyes) e.fov = su.fov, e.width = su.w, e.height = su.h;
                const dcsvr::GazeSample g{int64_t(seq.times[i] * 1e9), true, false, {0.f, 0.f, -1.f}};
                c.BeginFrame(g.time, eyes, 1, g, true, s);
                dcsvr::TemporalConstants k{};
                c.Constants(0, su.focus, s, k);
                notReprojectedOut.push_back(gpu.Run(r.in[i], k));
            }
        }
        const double on = Rmse(r.out, r.truth, su, 5, 28.f), notReprojected = Rmse(notReprojectedOut, r.truth, su, 5, 28.f), ghost = Rmse(r.in, r.truth, su, 5, 28.f, 1);
        std::printf("| %s | %.0f deg/s | 0 | %.4f | %.4f | %.4f |\n", scene == SceneKind::Horizon ? "Horizon" : "Lines", speed, on, notReprojected, ghost);
        // What remains is the resampling of content finer than a periphery pixel (the lines), not a trail.
        turnOk &= on < 0.4 * ghost && on < notReprojected;
    }
    g_scene = SceneKind::Lines;
    Check(turnOk, "(b) a reprojected head turn: under 0.4 of a one-frame ghost, and less than without the reprojection");
    {
        // The reprojection itself, with the clamp out of the way: a turn of 113 deg/s over the horizon scene.
        g_scene = SceneKind::Horizon;
        Sequence seq = Steady(quick ? 20 : 40, 1.0 / 45.0, 113.f);
        seq.jitter = false;
        seq.antialiased = true;
        dcsvr::TemporalSettings open = s;
        open.clip = 1000.f;
        open.confidence = 1000.f;
        const Result r = RunSequence(gpu, su, seq, open, true, rng);
        Sequence held = seq;
        held.pose.assign(held.pose.size(), dcsvr::Quat{});
        held.world.clear();
        for (const auto& q : seq.pose) held.world.push_back(dcsvr::Conjugate(q));  // the same images, head "still"
        const Result n = RunSequence(gpu, su, held, open, true, rng);
        // A negative control: images turning the other way than the filter is told.
        Sequence wrong = seq;
        wrong.world.clear();
        for (const auto& q : seq.pose) wrong.world.push_back(dcsvr::Multiply(q, q));
        const Result w = RunSequence(gpu, su, wrong, open, true, rng);
        const double reprojected = Rmse(r.out, r.truth, su, 5, 28.f), notReprojected = Rmse(n.out, n.truth, su, 5, 28.f), opposite = Rmse(w.out, w.truth, su, 5, 28.f);
        std::printf("Clamp disabled, 113 deg/s over the horizon: error reprojected %.4f, not reprojected %.4f, reprojected the wrong way %.4f\n", reprojected, notReprojected, opposite);
        Check(reprojected < 0.7 * notReprojected && notReprojected < opposite, "(b) the reprojection follows the head (clamp disabled: less error than none, the wrong way more)");
        g_scene = SceneKind::Lines;
    }
    {
        // The same turn with DCS-like jitter on aliased content: the stabilised periphery is closer to the truth.
        const Sequence seq = Steady(quick ? 20 : 40, 1.0 / 45.0, 100.f);
        const Result r = RunSequence(gpu, su, seq, s, true, rng);
        const double off = Rmse(r.in, r.truth, su, 5, 28.f), on = Rmse(r.out, r.truth, su, 5, 28.f);
        std::printf("Jittered aliased input during the 100 deg/s turn, error against the truth off / on: %.4f / %.4f\n", off, on);
        Check(on < off, "(b) with jitter during a head turn the stabilised periphery is closer to the truth");
    }

    // (c) Horizon roll the head does not explain (no motion vectors): anti-aliased input, so the error is the lag.
    std::printf("\n(c) Horizon roll with the head still (not explained by the reprojection), anti-aliased input: RMS error against the truth, 28-45 degrees\n");
    std::printf("| Scene | Roll | Off | On | On, clamp disabled | Previous frame shown (1-frame ghost) |\n|---|---|---|---|---|---|\n");
    bool rollOk = true;
    for (const auto scene : {SceneKind::Horizon, SceneKind::Lines})
    for (const float speed : {10.f, 30.f, 90.f}) {
        g_scene = scene;
        Sequence seq = Steady(quick ? 20 : 40, 1.0 / 45.0, 0.f, speed);
        seq.jitter = false;
        seq.antialiased = true;
        const Result r = RunSequence(gpu, su, seq, s, true, rng);
        dcsvr::TemporalSettings open = s;
        open.clip = 1000.f;
        open.confidence = 1000.f;
        const Result u = RunSequence(gpu, su, seq, open, false, rng);
        const double on = Rmse(r.out, r.truth, su, 5, 28.f), ghost = Rmse(r.in, r.truth, su, 5, 28.f, 1), unclamped = Rmse(u.out, r.truth, su, 5, 28.f);
        std::printf("| %s | %.0f deg/s | 0 | %.4f | %.4f | %.4f |\n", scene == SceneKind::Horizon ? "Horizon" : "Lines", speed, on, unclamped, ghost);
        // The clamp can only act where the history leaves the neighbourhood's range: on the horizon and its sparse
        // features. Dense line patterns keep the history inside the range (reported, the documented worst case).
        if (scene == SceneKind::Horizon) rollOk &= on < unclamped && (speed < 50.f || on < 0.75 * unclamped);
        rollOk &= on < 0.7 * ghost;
    }
    g_scene = SceneKind::Lines;
    Check(rollOk, "(c) unexplained motion: under 0.7 of a one-frame ghost; on the horizon the clamp cuts the lag (by a quarter at 90 deg/s)");

    // (d) Resets: after a saccade, a gap or a new size the output is exactly the input.
    std::printf("\n(d) Resets\n");
    {
        dcsvr::TemporalController c;
        std::uniform_real_distribution<float> j(-0.5f, 0.5f);
        // One frame; returns the reset and the largest |output - input|.
        const auto frame = [&](int64_t t, bool saccade, int w, int h) {
            dcsvr::TemporalEye eyes[2];
            for (auto& e : eyes) e.fov = su.fov, e.width = w, e.height = h;
            const dcsvr::GazeSample g{t, true, saccade, {0.f, 0.f, -1.f}};
            const auto reset = c.BeginFrame(t, eyes, 1, g, true, s);
            dcsvr::TemporalConstants k{};
            c.Constants(0, su.focus, s, k);
            const Frame in = Render(w, h, su.fov, {}, {}, j(rng), j(rng), false);
            return std::make_pair(reset, MaxAbsDiff(gpu.Run(in, k), in));
        };
        int64_t t = 1'000'000'000;
        for (int i = 0; i < 10; ++i) frame(t += 22 * Ms, false, su.w, su.h);
        const auto filtering = frame(t += 22 * Ms, false, su.w, su.h);
        const auto saccade = frame(t += 22 * Ms, true, su.w, su.h);
        frame(t += 22 * Ms, false, su.w, su.h);
        const auto gap = frame(t += 150 * Ms, false, su.w, su.h);
        frame(t += 22 * Ms, false, su.w, su.h);
        const auto size = frame(t += 22 * Ms, false, su.w - 10, su.h - 6);
        const auto resumed = frame(t += 22 * Ms, false, su.w - 10, su.h - 6);
        std::printf("max |out - in|: while filtering %.3f; on the saccade frame %.5f, after a 150 ms gap %.5f, on a new size %.5f; the frame after %.3f\n",
                    filtering.second, saccade.second, gap.second, size.second, resumed.second);
        Check(filtering.first == dcsvr::TemporalReset::None && filtering.second > 0.05, "(d) the filter changes the jittered periphery before the reset (the check is sensitive)");
        Check(saccade.first == dcsvr::TemporalReset::Saccade && saccade.second < 1e-3, "(d) a saccade: output equals input");
        Check(gap.first == dcsvr::TemporalReset::Gap && gap.second < 1e-3, "(d) a 150 ms gap: output equals input");
        Check(size.first == dcsvr::TemporalReset::Size && size.second < 1e-3, "(d) a new periphery size: output equals input");
        Check(resumed.first == dcsvr::TemporalReset::None && resumed.second > 0.05, "(d) filtering resumes on the next frame");
    }

    // (e) The same small step (inside the clamp box, on a textured background) at 2X, 3X and Auto frame times.
    std::printf("\n(e) A +0.03 step on a textured still scene, 28-45 degrees: share of the step still missing\n");
    std::printf("| Time after step | 2X (45 FPS) | 3X (30 FPS) | Auto (45/30 alternating) | exp(-t/40 ms) |\n|---|---|---|---|---|\n");
    {
        std::vector<float> texture(size_t(su.w) * su.h);
        std::mt19937 tr(3);
        std::uniform_real_distribution<float> u(0.3f, 0.7f);
        for (auto& v : texture) v = u(tr);
        const auto residual = [&](const std::vector<double>& dts) {
            dcsvr::TemporalController c;
            std::vector<std::pair<double, double>> out;  // time after the step, share of it still missing
            double t = 1.0, stepAt = 0.0;
            const int before = 10;
            for (size_t i = 0; i < dts.size(); ++i) {
                t += dts[i];
                dcsvr::TemporalEye eyes[2];
                for (auto& e : eyes) e.fov = su.fov, e.width = su.w, e.height = su.h;
                const dcsvr::GazeSample g{int64_t(t * 1e9), true, false, {0.f, 0.f, -1.f}};
                c.BeginFrame(g.time, eyes, 1, g, true, s);
                dcsvr::TemporalConstants k{};
                c.Constants(0, su.focus, s, k);
                Frame in{su.w, su.h, texture};
                const bool stepped = int(i) >= before;
                if (stepped)
                    for (auto& v : in.v) v += 0.03f;
                const Frame o = gpu.Run(in, k);
                if (stepped) {
                    if (int(i) == before) stepAt = t;
                    double missing = 0, n = 0;
                    for (size_t p = 0; p < o.v.size(); ++p)
                        if (su.ecc[p] >= 28.f && su.ecc[p] < 45.f) missing += (in.v[p] - o.v[p]) / 0.03, n += 1;
                    out.push_back({t - stepAt, missing / n});
                }
            }
            return out;
        };
        const auto interpolate = [](const std::vector<std::pair<double, double>>& v, double at) {
            for (size_t i = 1; i < v.size(); ++i)
                if (v[i].first >= at) {
                    const double f = (at - v[i - 1].first) / (v[i].first - v[i - 1].first);
                    return v[i - 1].second + f * (v[i].second - v[i - 1].second);
                }
            return v.back().second;
        };
        std::vector<double> d45(24, 1.0 / 45.0), d30(18, 1.0 / 30.0), dAuto;
        for (int i = 0; i < 22; ++i) dAuto.push_back(i % 2 ? 1.0 / 30.0 : 1.0 / 45.0);
        const auto r45 = residual(d45), r30 = residual(d30), rAuto = residual(dAuto);
        // Each list starts at the first stepped frame, time 0: the step is shown with exp(-0) history.
        double worst = 0;
        for (const double at : {0.0667, 0.1, 0.1333, 0.2}) {
            const double a = interpolate(r45, at), b = interpolate(r30, at), c = interpolate(rAuto, at);
            std::printf("| %.0f ms | %.3f | %.3f | %.3f | %.3f |\n", at * 1000, a, b, c, std::exp(-at / 0.04));
            worst = std::max({worst, std::fabs(a - b), std::fabs(a - c), std::fabs(b - c)});
        }
        Check(worst < 0.05, "(e) 2X, 3X and Auto leave the same share of the step at the same time (within 0.05)");
    }

    // Cost at the real periphery size.
    {
        dcsvr::TemporalController c;
        dcsvr::TemporalEye eyes[2];
        for (auto& e : eyes) e.fov = su.fov, e.width = 814, e.height = 804;
        dcsvr::GazeSample g{1, true, false, {0.f, 0.f, -1.f}};
        c.BeginFrame(1'000'000'000, eyes, 1, g, true, s);
        c.BeginFrame(1'022'000'000, eyes, 1, g, true, s);
        dcsvr::TemporalConstants k{};
        c.Constants(0, su.focus, s, k);
        const double ms = gpu.TimeMs(814, 804, k, quick ? 20 : 200);
        std::printf("\nGPU time of the pass at 814x804 (one eye), median: %.3f ms%s; both eyes about %.3f ms\n", ms, gpu.warp ? " (WARP: CPU, not representative)" : "", 2 * ms);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: perception_test.exe <native/quadviews dir> [--warp] [--quick]\n");
        return 2;
    }
    bool warp = false, quick = false;
    for (int i = 2; i < argc; ++i) {
        warp |= std::string(argv[i]) == "--warp";
        quick |= std::string(argv[i]) == "--quick";
        // Tuning only: --clip=, --confidence=, --tau= override the defaults.
        const std::string a = argv[i];
        if (a.rfind("--clip=", 0) == 0) g_clip = std::stof(a.substr(7));
        if (a.rfind("--confidence=", 0) == 0) g_confidence = std::stof(a.substr(13));
        if (a.rfind("--tau=", 0) == 0) g_tauMs = std::stof(a.substr(6));
    }
    MathChecks();
    ShaderChecks(argv[1], warp, quick);
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
