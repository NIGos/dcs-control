// DCS Control: offline checks of Far depth (native/quadviews/dcsvr_depthwarp.h, dcsvr_depthwarp_cs.hlsl,
// dcsvr_depthwarp_ps.hlsli, dcsvr_depth_import.h). Built and run by scripts/build-depthwarp-test.ps1. Exit code 0 when
// every check passes.
//
//   depthwarp_test.exe <native/quadviews dir> <quad views openxr-api-layer dir> [--warp] [--quick] [--perf] [--only=<section>]
//
// Part 1 checks the shared maths and the C++ state on the CPU: the disparity curve's values and monotonicity, the
// clamps, gProj parsing and depth linearisation, the jitter centre, the edge and blend windows, the epipolar shift
// with canted displays, the gain controller and the engine-export import (reference counts and rejections).
// Part 2 runs the real shaders on a D3D11 device (the GPU, or WARP with --warp or without one): a fake engine renders
// synthetic world-space scenes into a reverse-Z, infinite-far D32S8 depth at DLSS's internal size with Halton jitter,
// and the colour, anti-aliased and de-jittered, at the output size; FieldMain builds the field and the composition
// runs through the shipping ProjectionPS with dcsvr_depthwarp_ps.hlsli injected at the anchors the build will use.
// Every expectation has an exact ground truth (the scene is analytic), so the checks are numbers.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <execution>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dcsvr_depthwarp.h"
#include "dcsvr_depth_import.h"

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;
bool g_perf = false;  // --perf: assert the GPU cost limits (else they are reported only: a busy GPU, DCS running, skews them)
std::string g_only;  // --only=<tag>: one section of the shader checks (identity, s7, fovea, canopy, overlay, s3, s9, s9move, s8, solve, reuse, robust, formats, fallback, cost)
bool Want(const char* tag) {
    return g_only.empty() || g_only == tag;
}
void Check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}
template <class... A>
std::string Fmt(const char* f, A... a) {
    char b[1024];
    std::snprintf(b, sizeof(b), f, a...);
    return b;
}
bool Near(double a, double b, double tolerance) {
    return std::fabs(a - b) <= tolerance;
}

constexpr float Rad = 0.0174532925f;
constexpr int64_t Ms = 1'000'000;
float ToArcmin(float rad) {
    return rad / dcsvr::ArcminRad;
}

dcsvr::Quat Yaw(float deg) {
    return {0.f, std::sin(deg * Rad / 2.f), 0.f, std::cos(deg * Rad / 2.f)};
}

float Halton(int i, int b) {
    float f = 1.f, r = 0.f;
    for (int n = i; n > 0; n /= b) {
        f /= b;
        r += f * (n % b);
    }
    return r;
}

// ---- Part 1: the maths and the CPU state ----------------------------------------------------------------------------

void CurveChecks() {
    using namespace dcsvr;
    const float I = 0.064f;
    DisparitySettings s;
    s.ApplyPreset(FarDepthPreset::Default);
    const auto dd = [&](float r, const DisparitySettings& x) {
        return DcsvrExtraDisparityRad(r, I, x.z0M, x.gamma, x.fadeStartM, x.fadeEndM, x.nearGuardM);
    };
    std::printf("\nM1 Default preset (gamma %.2f, Z0 %.0f m), IPD 64 mm: extra disparity, both eyes\n", s.gamma, s.z0M);
    std::printf("| Range | Natural | Extra | Extra per eye (px at 29 px/deg) |\n|---|---|---|---|\n");
    for (float r : {30.f, 40.f, 60.f, 100.f, 200.f, 300.f, 1000.f, 3000.f, 5000.f, 10000.f, 20000.f}) {
        const float e = dd(r, s);
        std::printf("| %.0f m | %.2f' | %.2f' | %.2f |\n", r, ToArcmin(I / r), ToArcmin(e), ToArcmin(e) / 2.f / 60.f * 29.f);
    }
    Check(Near(ToArcmin(dd(100.f, s)), 1.79, 0.03) && Near(ToArcmin(dd(200.f, s)), 2.03, 0.03) && Near(ToArcmin(dd(1000.f, s)), 1.56, 0.03) &&
              Near(ToArcmin(dd(3000.f, s)), 1.14, 0.03),
          "M1 Default: 1.79' at 100 m, 2.03' at 200 m (peak), 1.56' at 1 km, 1.14' at 3 km");
    {
        float best = 0.f, bestR = 0.f;
        for (float r = 41.f; r < 4000.f; r *= 1.002f)
            if (dd(r, s) > best) best = dd(r, s), bestR = r;
        Check(Near(bestR, 201.f, 3.f), Fmt("M1 Default peak at Z0 gamma^(-1/(1-gamma)) = 201 m (found %.0f m)", bestR));
        DisparitySettings h = s;
        h.gamma = 0.5f;
        float best5 = 0.f, bestR5 = 0.f;
        for (float r = 41.f; r < 4000.f; r *= 1.002f)
            if (dd(r, h) > best5) best5 = dd(r, h), bestR5 = r;
        Check(Near(bestR5, 160.f, 2.f) && Near(best5, I / 160.f, 2e-6f), Fmt("M1 gamma 0.5: peak I / (4 Z0) at 4 Z0 = 160 m (%.0f m, %.3f')", bestR5, ToArcmin(best5)));
    }
    Check(dd(40.f, s) == 0.f && dd(25.f, s) == 0.f && dd(-1.f, s) == 0.f && dd(INFINITY, s) == 0.f && dd(NAN, s) == 0.f && dd(1e31f, s) == 0.f && dd(20000.f, s) == 0.f &&
              dd(25000.f, s) == 0.f,
          "M1 zero at and inside Z0, in the near guard, for the sky (-1), +inf, NaN, beyond the far fade");
    {
        DisparitySettings g = s;
        g.z0M = 20.f;  // a Z0 inside the near guard: the guard still holds the near field
        Check(DcsvrExtraDisparityRad(29.f, I, g.z0M, g.gamma, g.fadeStartM, g.fadeEndM, g.nearGuardM) == 0.f &&
                  DcsvrExtraDisparityRad(31.f, I, g.z0M, g.gamma, g.fadeStartM, g.fadeEndM, g.nearGuardM) > 0.f,
              "M1 the 30 m near guard holds when Z0 is set below it");
    }
    {
        const float unfaded = DcsvrTargetDisparityRad(12500.f, I, s.z0M, s.gamma) - I / 12500.f;
        Check(Near(dd(12500.f, s), 0.5f * unfaded, 1e-9f), "M1 half way through the 5-20 km fade: half the extra disparity");
    }
    // M2: total disparity strictly decreasing over 0.2 m - 100 km for every preset and gain (depth order kept), and a
    // gain above 1 / (1 - gamma) would reverse it (why g is a blend, never a multiplier above 1).
    {
        bool ok = true;
        for (auto p : {FarDepthPreset::Subtle, FarDepthPreset::Default, FarDepthPreset::Strong}) {
            DisparitySettings x;
            x.ApplyPreset(p);
            for (float g : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
                double previous = INFINITY;
                for (double r = 0.2; r < 1e5; r *= 1.001) {
                    const double total = I / r + g * dd(float(r), x);
                    if (!(total < previous)) {
                        ok = false;
                        std::printf("     not decreasing: preset %d g %.2f at %.2f m\n", int(p), g, r);
                        break;
                    }
                    previous = total;
                }
            }
        }
        Check(ok, "M2 total disparity strictly decreasing in range: all presets, gain 0..1, fades included");
        DisparitySettings x;
        x.ApplyPreset(FarDepthPreset::Subtle);
        const float g = 1.f / (1.f - x.gamma) + 0.5f;
        bool reversed = false;
        double previous = INFINITY;
        for (double r = 41.0; r < 4000.0; r *= 1.001) {
            const double total = I / r + g * dd(float(r), x);
            reversed |= total > previous;
            previous = total;
        }
        Check(reversed, "M2 negative control: a multiplier above 1 / (1 - gamma) reverses depth order");
    }
    // The Near preset's world-IPD curve: (k - 1) ipd / R from 3 near guards out, nothing inside the guard, depth order
    // kept for k up to 2, and the constants carry it as a negative gamma.
    {
        DisparitySettings n;
        n.ApplyPreset(FarDepthPreset::Near);
        const float k1 = n.worldIpdScale - 1.f, ipd = 0.064f;
        DisparityConstants kc;
        FieldInputs fin;
        FieldConstants(fin, n, kc);
        const auto extra = [&](float r, float g) { return DcsvrExtraDisparityRad(r, ipd, n.z0M, g, n.fadeStartM, n.fadeEndM, n.nearGuardM); };
        const float at50 = extra(50.f, -k1), at500 = extra(500.f, -k1);
        Check(n.worldIpdScale == 1.4f && n.nearGuardM == 4.f && Near(kc.curve[2], -0.4f, 1e-6f) && extra(3.9f, -k1) == 0.f &&
                  Near(at50, k1 * ipd / 50.f, 1e-7f) && Near(at500, k1 * ipd / 500.f, 1e-8f) && extra(30000.f, -k1) == 0.f,
              Fmt("M1 Near: x1.4 world IPD, %.2f' at 50 m and %.2f' at 500 m (both eyes), 0 inside 4 m and beyond the far fade",
                  at50 / ArcminRad, at500 / ArcminRad));
        bool ordered = true;
        for (float kk : {1.2f, 1.4f, 2.0f}) {
            float previous = 1e9f;
            for (float r = 1.f; r < 30000.f; r *= 1.01f) {
                const float total = ipd / r + extra(r, -(kk - 1.f));
                ordered &= total < previous;
                previous = total;
            }
        }
        Check(ordered, "M2 Near: total disparity strictly decreasing from 1 m to 30 km for x1.2, x1.4 and x2 (fade-in included)");
        // Per eye, clamped at 3': x1.4 reaches the clamp nearer than about 15 m.
        const float eye10 = DcsvrEyeAngle(extra(10.f, -k1), 1.f, 1.f, n.maxArcmin * ArcminRad) / ArcminRad;
        const float eye30 = DcsvrEyeAngle(extra(30.f, -k1), 1.f, 1.f, n.maxArcmin * ArcminRad) / ArcminRad;
        Check(Near(eye10, 3.f, 1e-3f) && eye30 < 3.f && eye30 > 1.4f,
              Fmt("M3 Near per eye: %.2f' at 10 m (the 3' clamp), %.2f' at 30 m", eye10, eye30));
        n.ApplyPreset(FarDepthPreset::Default);
        FieldConstants(fin, n, kc);
        Check(n.worldIpdScale == 0.f && n.nearGuardM == 30.f && kc.curve[2] == 0.35f, "M1 another preset after Near: the infinity-anchored curve again");
    }
    // M3: the per-eye angle clamp.
    {
        const float maxRad = 3.f * ArcminRad;
        Check(DcsvrEyeAngle(10.f * ArcminRad, 1.f, 1.f, maxRad) == maxRad && Near(DcsvrEyeAngle(2.f * ArcminRad, 1.f, 1.f, maxRad), ArcminRad, 1e-9) &&
                  DcsvrEyeAngle(2.f * ArcminRad, 0.f, 1.f, maxRad) == 0.f && Near(DcsvrEyeAngle(2.f * ArcminRad, 1.f, 0.5f, maxRad), 0.5f * ArcminRad, 1e-9),
              "M3 per-eye angle: half the extra disparity, times gain and window, clamped to 3'");
    }
}

void ProjectionChecks() {
    using namespace dcsvr;
    // M4: gProj as DCS builds it (reverse-Z, infinite far, near 0.02 m), with jitter in P20 / P21.
    const auto m = ProjectionParams::Make(-0.62f, 0.58f, 0.6f, -0.585f, 0.02f, 0.0004f, -0.0003f);
    const auto p = ProjectionParams::Parse(m.data());
    Check(p.ok && p.reverse && p.infinite && Near(p.nearM, 0.02f, 1e-7f) && Near(p.sx, 2.f / 1.2f, 1e-6f) && Near(p.sy, 2.f / 1.185f, 1e-6f) &&
              Near(p.cx, 0.04f / 1.2f + 0.0004f, 1e-6f) && Near(p.cy, -0.015f / 1.185f - 0.0003f, 1e-6f),
          "M4 gProj parse: reverse-Z, infinite, near 0.02 m, scale and centre with the jitter in P20/P21");
    {
        bool ok = true;
        double worst = 0;
        for (float w : {0.03f, 0.5f, 1.f, 40.f, 300.f, 1000.f, 20000.f, 99000.f}) {
            const float d = p.nearM / w;  // what the depth buffer holds at forward distance w
            const float back = DcsvrForwardFromDepth(d, p.p22, p.p23, p.p32, p.p33);
            worst = std::max(worst, std::fabs(back / w - 1.0));
            ok &= std::fabs(back / w - 1.f) < 2e-6f;
        }
        Check(ok, Fmt("M4 forward distance round trip 3 cm - 99 km (worst relative error %.1e)", worst));
        Check(DcsvrForwardFromDepth(0.f, p.p22, p.p23, p.p32, p.p33) < 0.f && DcsvrForwardFromDepth(1e-8f, p.p22, p.p23, p.p32, p.p33) < 0.f &&
                  DcsvrForwardFromDepth(NAN, p.p22, p.p23, p.p32, p.p33) < 0.f && DcsvrForwardFromDepth(-0.5f, p.p22, p.p23, p.p32, p.p33) < 0.f &&
                  DcsvrForwardFromDepth(1.5f, p.p22, p.p23, p.p32, p.p33) < 0.f,
              "M4 the sky (0), below 1e-7, NaN, negative and > 1 give no distance");
        Check(Near(DcsvrRangeFromForward(1000.f, 0.75f, 0.f), 1250.f, 0.01f) && DcsvrRangeFromForward(-1.f, 0.5f, 0.5f) < 0.f,
              "M4 range along the ray: forward x sqrt(1 + tx^2 + ty^2), the sky stays the sky");
    }
    {
        auto bad = m;
        bad[15] = 1.f;  // orthographic-like
        auto skew = m;
        skew[1] = 0.01f;
        auto nan = m;
        nan[5] = NAN;
        Check(!ProjectionParams::Parse(bad.data()).ok && !ProjectionParams::Parse(skew.data()).ok && !ProjectionParams::Parse(nan.data()).ok,
              "M4 rejects a non-perspective, a skewed and a NaN gProj");
        // A forward-Z projection parses but is not reverse (the import rejects it).
        std::array<float, 16> f{};
        f[0] = 1.f, f[5] = 1.f, f[10] = 1.f, f[11] = 1.f, f[14] = -0.1f;
        const auto fp = ProjectionParams::Parse(f.data());
        Check(fp.ok && !fp.reverse, "M4 a forward-Z projection is detected as such");
    }
    // M5: jitter centre.
    {
        using Ref = JitterCentreEstimator::Reference;
        const float w = 1176.f, h = 1161.f, tx = 2.f / w, ty = 2.f / h;
        // Static frustum, with and without Quad Views' reference.
        for (int withRef = 0; withRef < 2; ++withRef) {
            JitterCentreEstimator e;
            float worst = 0.f;
            for (int i = 1; i <= 96; ++i) {
                float dx, dy;
                JitterNdc(Halton(i, 2) - 0.5f, Halton(i, 3) - 0.5f, w, h, dx, dy);
                auto mm = ProjectionParams::Make(-0.62f, 0.58f, 0.6f, -0.585f, 0.02f, dx, dy);
                e.Add(ProjectionParams::Parse(mm.data()), withRef ? Ref::FromTangents(-0.62f, 0.58f, 0.6f, -0.585f) : Ref{}, tx, ty);
                if (i >= 32) {
                    float cx, cy;
                    e.Centre(cx, cy);
                    const float pp = ProjectionParams::Parse(mm.data()).cx - dx;  // the exact unjittered centre
                    const float ppy = ProjectionParams::Parse(mm.data()).cy - dy;
                    worst = std::max({worst, std::fabs(cx - pp) * w / 2.f, std::fabs(cy - ppy) * h / 2.f});
                }
            }
            Check(worst < 0.04f && !e.JitterAbsent() && e.UsesReference() == (withRef == 1),
                  Fmt("M5 Halton jitter in gProj, fixed frustum, %s: the centre within %.3f internal px (< 0.04)", withRef ? "Quad Views' FOV as reference" : "no reference", worst));
        }
        // The focus frustum follows the gaze (Quad Views' ComputeBoundingFov around the gaze): constant span, its
        // centre moving every frame. A 10 deg/s smooth pursuit at 45 FPS for 60 frames, an 8 degree saccade, 30
        // still frames, a 4 deg/s pursuit back. The error in depth texels against the exact unjittered centre.
        {
            const float span = 1.2f, spanY = 1.185f;
            std::vector<float> centre;  // the frustum's horizontal centre tangent per frame
            float cxTan = -0.02f;
            for (int i = 0; i < 60; ++i) centre.push_back(cxTan += std::tan(10.f / 45.f * Rad));
            cxTan += std::tan(8.f * Rad);
            for (int i = 0; i < 30; ++i) centre.push_back(cxTan);
            for (int i = 0; i < 60; ++i) centre.push_back(cxTan -= std::tan(4.f / 45.f * Rad));
            float worstRef = 0.f, worstRefLate = 0.f, worstNoRef = 0.f, worstMean = 0.f;
            JitterCentreEstimator withRef, noRef;
            double meanX[32]{};
            for (size_t i = 0; i < centre.size(); ++i) {
                float dx, dy;
                JitterNdc(Halton(int(i) + 1, 2) - 0.5f, Halton(int(i) + 1, 3) - 0.5f, w, h, dx, dy);
                const float l = centre[i] - span / 2.f, r = centre[i] + span / 2.f, u = 0.6f, d = u - spanY;
                const auto mm = ProjectionParams::Make(l, r, u, d, 0.02f, dx, dy);
                const ProjectionParams pp = ProjectionParams::Parse(mm.data());
                withRef.Add(pp, Ref::FromTangents(l, r, u, d), tx, ty);
                noRef.Add(pp, Ref{}, tx, ty);
                const float exact = pp.cx - dx;
                float cx, cy;
                withRef.Centre(cx, cy);
                worstRef = std::max(worstRef, std::fabs(cx - exact) / tx);
                if (i >= 32) worstRefLate = std::max(worstRefLate, std::fabs(cx - exact) / tx);
                noRef.Centre(cx, cy);
                worstNoRef = std::max(worstNoRef, std::fabs(cx - exact) / tx);
                // Negative control: the plain 32-frame mean of c (the estimator before the fix).
                meanX[i % 32] = pp.cx;
                const size_t n = std::min<size_t>(i + 1, 32);
                const double mean = std::accumulate(meanX, meanX + n, 0.0) / double(n);
                worstMean = std::max(worstMean, float(std::fabs(mean - exact) / tx));
            }
            Check(worstRefLate < 0.04f && worstRef < 0.6f,
                  Fmt("M5 moving focus frustum (pursuit, saccade, pursuit), Quad Views' FOV as reference: within %.3f texel from frame 32, %.3f from the first (DLSS's jitter alone)", worstRefLate, worstRef));
            Check(worstNoRef <= 1.8f, Fmt("M5 moving focus frustum without a reference: within %.2f texel (restarts when c leaves the mean; <= 1.8)", worstNoRef));
            Check(worstMean > 20.f, Fmt("M5 negative control: a plain 32-frame mean of gProj's centre is %.0f texels off on the same sequence", worstMean));
        }
        // A reference of another frame (5 texels off) or another scale is ignored; a constant offset DCS might add is kept.
        {
            JitterCentreEstimator e;
            float worst = 0.f;
            for (int i = 1; i <= 64; ++i) {
                float dx, dy;
                JitterNdc(Halton(i, 2) - 0.5f, Halton(i, 3) - 0.5f, w, h, dx, dy);
                const auto mm = ProjectionParams::Make(-0.62f, 0.58f, 0.6f, -0.585f, 0.02f, dx + 0.3f * tx, dy);
                Ref ref = Ref::FromTangents(-0.62f, 0.58f, 0.6f, -0.585f);
                e.Add(ProjectionParams::Parse(mm.data()), ref, tx, ty);
                float cx, cy;
                e.Centre(cx, cy);
                if (i >= 32) worst = std::max(worst, std::fabs(cx - (ProjectionParams::Parse(mm.data()).cx - dx)) / tx);
            }
            JitterCentreEstimator f;
            const auto mm = ProjectionParams::Make(-0.62f, 0.58f, 0.6f, -0.585f, 0.02f);
            Ref wrong = Ref::FromTangents(-0.62f, 0.58f, 0.6f, -0.585f);
            wrong.cx += 5.f * tx;
            f.Add(ProjectionParams::Parse(mm.data()), wrong, tx, ty);
            const bool farIgnored = !f.UsesReference();
            Ref scaled = Ref::FromTangents(-0.7f, 0.58f, 0.6f, -0.585f);
            f.Add(ProjectionParams::Parse(mm.data()), scaled, tx, ty);
            Check(worst < 0.04f && farIgnored && !f.UsesReference(),
                  Fmt("M5 a constant 0.3 texel offset of gProj against the reference is kept (%.3f texel); a reference 5 texels off or of another scale is ignored", worst));
        }
        JitterCentreEstimator still;
        const auto mm = ProjectionParams::Make(-0.62f, 0.58f, 0.6f, -0.585f, 0.02f);
        for (int i = 0; i < 32; ++i) still.Add(ProjectionParams::Parse(mm.data()), Ref::FromTangents(-0.62f, 0.58f, 0.6f, -0.585f), tx, ty);
        float cx, cy;
        still.Centre(cx, cy);
        Check(still.JitterAbsent() && cx == ProjectionParams::Parse(mm.data()).cx, "M5 a gProj without jitter over 32 frames: detected, its own centre used");
        auto wider = ProjectionParams::Make(-0.7f, 0.7f, 0.6f, -0.585f, 0.02f);
        still.Add(ProjectionParams::Parse(wider.data()), Ref{}, tx, ty);
        Check(still.Count() == 1, "M5 a new projection scale starts over");
    }
}

void WindowChecks() {
    using namespace dcsvr;
    // M6: the field's edge window is 0 at the focus edge; the compute fallback's is 0 wherever the composition's
    // alpha is below 1 (so a focus-only warp can never show beside the unwarped periphery).
    Check(DcsvrFocusEdgeWindow(0.97f, 0.f, 0.f, 0.85f, 0.97f, 1.f) == 0.f && DcsvrFocusEdgeWindow(0.f, -0.99f, 0.f, 0.85f, 0.97f, 1.f) == 0.f &&
              DcsvrFocusEdgeWindow(0.84f, 0.84f, 0.f, 0.85f, 0.97f, 1.f) == 1.f && DcsvrFocusEdgeWindow(0.6f, 0.6f, 2.f, 0.85f, 0.97f, 1.f) == 1.f &&
              DcsvrFocusEdgeWindow(0.69f, 0.69f, 2.f, 0.85f, 0.97f, 1.f) == 0.f,
          "M6 edge window: 1 inside 0.85, 0 from 0.97 of the shape metric (rectangle and round)");
    bool ok = true;
    int blended = 0;
    for (float shape : {0.f, 2.f, 4.f})
        for (float smoothing : {0.f, 0.05f, 0.2f, 0.3f})
            for (float y = -1.f; y <= 1.f; y += 0.01f)
                for (float x = -1.f; x <= 1.f; x += 0.01f) {
                    const float a = DcsvrFocusBlendWindow(x, y, shape, smoothing);
                    const float w = DcsvrFocusEdgeWindow(x, y, shape, 0.85f, 0.97f, DcsvrFocusFullAlphaScale(shape, smoothing));
                    if (a < 1.f) {
                        ++blended;
                        ok &= w == 0.f;
                    }
                }
    Check(ok && blended > 0, Fmt("M6 compute fallback: window 0 at all %d sampled points where alpha < 1 (3 shapes x 4 smoothings)", blended));
    Check(Near(DcsvrFocusBlendWindow(0.7f, 0.f, 0.f, 0.2f), 0.84375f, 1e-5f) && DcsvrFocusBlendWindow(0.95f, 0.95f, 0.f, 0.2f) == 0.5f &&
              DcsvrFocusBlendWindow(1.01f, 0.f, 0.f, 0.2f) == 0.f,
          "M6 blend window port: 0.844 at ndc 0.7 with smoothing 0.2, the 0.5 floor in the corner, 0 outside");
    Check(Near(DcsvrFoveaWeight(10.f, 10.f, 25.f), 1.f, 1e-6) && Near(DcsvrFoveaWeight(17.5f, 10.f, 25.f), 0.5f, 1e-6) && DcsvrFoveaWeight(25.f, 10.f, 25.f) == 0.f,
          "M6 fovea weight: 1 to 10 degrees, 0.5 at 17.5, 0 from 25");
}

void EpipolarChecks() {
    using namespace dcsvr;
    // A small turn along the baseline moves a central ray horizontally by tan(a), not vertically.
    {
        float ox, oy;
        DcsvrEpipolarShift(0.f, 0.f, 0.001f, 1.f, 0.f, 0.f, ox, oy);
        Check(Near(ox, std::tan(0.001f), 1e-7) && oy == 0.f, "S10 a central ray turned 1 mrad toward +x moves tan(1 mrad) right, not up");
    }
    // S10 canted displays: each eye's frame turned 10 degrees outward. The world baseline expressed in each eye's frame
    // keeps every shifted ray in the epipolar plane of its point, so no vertical disparity appears.
    {
        const Quat eyeInView[2] = {Yaw(10.f), Yaw(-10.f)};
        const float worldBaseline[3] = {1.f, 0.f, 0.f};
        float worst = 0.f;
        for (float el : {-15.f, -5.f, 0.f, 8.f, 20.f})
            for (float az : {-20.f, -5.f, 0.f, 12.f, 25.f}) {
                // A point 500 m away in that direction, seen from both eyes (32 mm left and right).
                const float P[3] = {500.f * std::cos(el * Rad) * std::sin(az * Rad), 500.f * std::sin(el * Rad), -500.f * std::cos(el * Rad) * std::cos(az * Rad)};
                float shifted[2][3];
                for (int e = 0; e < 2; ++e) {
                    const float ex = e ? 0.032f : -0.032f;
                    const float r[3] = {P[0] - ex, P[1], P[2]};
                    float inEye[3], b[3];
                    Rotate(Conjugate(eyeInView[e]), r, inEye);
                    Rotate(Conjugate(eyeInView[e]), worldBaseline, b);
                    const float sign = e ? -1.f : 1.f;
                    float ox, oy;
                    DcsvrEpipolarShift(inEye[0] / -inEye[2], inEye[1] / -inEye[2], 3.f * ArcminRad, sign * b[0], sign * b[1], sign * b[2], ox, oy);
                    const float ray[3] = {ox, oy, -1.f};
                    Rotate(eyeInView[e], ray, shifted[e]);
                }
                // The epipolar plane of P: spanned by the baseline and P (from the baseline's midpoint).
                float n[3] = {worldBaseline[1] * P[2] - worldBaseline[2] * P[1], worldBaseline[2] * P[0] - worldBaseline[0] * P[2],
                              worldBaseline[0] * P[1] - worldBaseline[1] * P[0]};
                const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                for (float& c : n) c /= nl;
                for (int e = 0; e < 2; ++e) {
                    const float l = std::sqrt(shifted[e][0] * shifted[e][0] + shifted[e][1] * shifted[e][1] + shifted[e][2] * shifted[e][2]);
                    const float off = std::fabs(n[0] * shifted[e][0] + n[1] * shifted[e][1] + n[2] * shifted[e][2]) / l;  // rad out of the plane
                    worst = std::max(worst, off / Rad * 29.f);                                                              // px at 29 px/deg
                }
            }
        Check(worst <= 0.01f, Fmt("S10 canted eyes +-10 degrees, 3' shift: out of the epipolar plane at most %.4f px (<= 0.01)", worst));
    }
}

void ControllerChecks() {
    using namespace dcsvr;
    DisparitySettings s;
    s.enabled = true;
    // M7: gated gain with the pupil shift layer's events.
    {
        DisparityController c;
        DisparityFrameInput in;
        in.enabled = in.depthValid = in.tracker = in.gazeValid = in.externalEvents = true;
        int64_t t = 1'000'000'000;
        auto frame = [&]() {
            in.displayTime = (t += 22 * Ms);
            return c.BeginFrame(in, s);
        };
        DisparityDecision d = frame();
        Check(d.gain == 0.f && !d.warp && d.skip == DisparitySkip::Gain, "M7 starts at gain 0 (nothing to warp yet)");
        for (int i = 0; i < 45; ++i) d = frame();  // about 1 s of drift
        Check(Near(d.gain, 0.05f * 45 * 0.022f, 1e-4f) && d.warp, Fmt("M7 drift only: %.3f after 0.99 s (0.05/s)", d.gain));
        const float before = d.gain;
        in.saccadeEvents += 1;
        d = frame();
        Check(Near(d.gain - before, 0.25f + 0.05f * 0.022f, 1e-4f), "M7 one saccade onset: +0.25");
        const float repeated = c.BeginFrame(in, s).gain;
        Check(repeated == d.gain, "M7 a second layer of the same display time repeats the decision (no second step)");
        in.blinkEvents += 1;
        d = frame();
        Check(d.gain == 1.f && c.Stats().blinkSteps == 1, "M7 a blink: the full way");
        // A skipped push: the held fields bridge it (reprojected), at the same gain: nothing pops.
        in.depthValid = false;
        in.fieldReusable = true;
        d = frame();
        Check(d.warp && d.reuse && d.gain == 1.f && d.skip == DisparitySkip::None, "M7 one frame without depth: warped with the held fields, gain held (no pop)");
        const uint32_t gapStatus = DisparityStatus().load();
        Check((gapStatus & 2u) && !(gapStatus & 4u) && (gapStatus & 8u), "M7 panel status on that frame: warped, depth not valid, a held field");
        d = frame();
        Check(d.warp && d.reuse, "M7 a second frame without depth (holdFrames 2): still bridged");
        d = frame();
        Check(!d.warp && !d.reuse && d.skip == DisparitySkip::Depth && d.gain == 0.f,
              "M7 a third: unwarped, so the gain is 0 (what is seen) instead of jumping back later");
        const uint32_t offStatus = DisparityStatus().load();
        Check(!(offStatus & 2u) && !(offStatus & 4u) && ((offStatus >> 16) & 0xFFu) == (uint32_t)DisparitySkip::Depth,
              "M7 panel status without depth: not warped, depth not valid, skip reason depth");
        in.depthValid = true;
        d = frame();
        Check(Near(d.gain, 0.05f * 0.022f, 1e-5f) && !d.reuse, "M7 depth back after a long gap: the gain climbs from 0 under the gated rules (drift)");
        in.blinkEvents += 1;
        d = frame();
        Check(d.gain == 1.f, "M7 ... and a blink takes it the whole way");
        // A gap that cannot be bridged (no reprojection, or a scene cut) is not.
        in.depthValid = false;
        in.fieldReusable = false;
        d = frame();
        Check(!d.warp && d.gain == 0.f, "M7 a gap whose held fields cannot be reprojected: unwarped, gain 0");
        in.depthValid = true;
        in.blinkEvents += 1;
        frame();
        in.depthValid = false;
        in.fieldReusable = true;
        in.sceneCuts += 1;
        d = frame();
        Check(!d.warp && !d.reuse, "M7 a scene cut during a gap: the held (old scene's) fields are not reused");
        in.depthValid = true;
        d = frame();
        Check(d.gain == 1.f && d.warp, "M7 ... and the cut is applied (the full way) on the next frame with depth");
        // A mission load: the engine stops pushing for 2 s, the cut arrives meanwhile; depth returns afterwards.
        in.depthValid = false;
        for (int i = 0; i < 90; ++i) d = frame();
        Check(d.gain == 0.f, "M7 a 2 s gap: unwarped, gain 0");
        in.sceneCuts += 1;
        for (int i = 0; i < 10; ++i) d = frame();
        in.depthValid = true;
        d = frame();
        Check(d.gain == 1.f && d.warp, "M7 a scene cut during a long gap (mission load) is kept: the full way on the first frame with depth");
        in.enabled = false;
        d = frame();
        Check(!d.warp && d.skip == DisparitySkip::Off && d.gain == 0.f, "M7 disabled: no warp, the gain is 0");
        const uint32_t status = DisparityStatus().load();
        Check((status & 1u) == 0 && (status & 4u) && ((status >> 16) & 0xFFu) == (uint32_t)DisparitySkip::Off, "M7 panel status: off, depth valid, skip reason");
        in.sceneCuts += 1;  // a cut while disabled
        frame();
        in.enabled = true;
        d = frame();
        Check(Near(d.gain, 0.05f * 0.022f, 1e-5f), "M7 enabled again (after a cut while disabled): from 0, gated");
        // The A/B key: off is in.enabled false (gain 0 at once); on adds one to the scene cuts on the frame it is enabled
        // again, so the full effect is back at once even with the gate.
        in.enabled = false;
        frame();
        in.enabled = true;
        in.sceneCuts += 1;
        d = frame();
        Check(d.gain == 1.f && d.warp, "M7 the A/B key back on (counted as a cut): the full effect on that frame, gated or not");
        DisparityStatusWord w;
        w.on = w.enabled = w.depthValid = w.coverage = true;
        w.gain = 0.35f;
        w.warpedPerSecond = 88;
        w.heldPerSecond = 3;
        w.coveragePercent = 12;
        const uint64_t packed = PackDisparityStatus2(w, 0x1A5);
        Check(packed == (0x1ull | 0x2ull | 0x4ull | 0x20ull | (35ull << 8) | (88ull << 16) | (12ull << 32) | (3ull << 40) | (0xA5ull << 48)),
              "M7 extended panel status: on, enabled, depth, coverage, gain 35, 88 warped/s, 12 %, 3 held/s, counter (8 bits)");
        w.on = false, w.offByKey = true, w.coveragePercent = 999, w.warpedPerSecond = 70000;
        const uint64_t off = PackDisparityStatus2(w, 1);
        Check((off & 1ull) == 0 && (off & 0x10ull) && ((off >> 32) & 0xFF) == 255 && ((off >> 16) & 0xFFFF) == 0xFFFF,
              "M7 extended panel status: off by the key, saturated fields");
    }
    // Without an eye tracker: drift only (20 s for the whole way), saccade events ignored.
    {
        DisparityController c;
        DisparityFrameInput in;
        in.enabled = in.depthValid = in.externalEvents = true;
        in.tracker = false;
        int64_t t = 1'000'000'000;
        DisparityDecision d;
        for (int i = 0; i < 451; ++i) {  // the first frame has no interval
            in.displayTime = (t += 22 * Ms);
            if (i == 100) in.saccadeEvents += 3, in.blinkEvents += 1;
            d = c.BeginFrame(in, s);
        }
        Check(Near(d.gain, 0.05f * 450 * 0.022f, 1e-3f), Fmt("M7 no tracker: drift only, %.3f after 9.9 s, events ignored", d.gain));
    }
    // Quad Views' own detector (no pupil shift events): the saccade's rising edge, a blink from an 80-300 ms gap.
    {
        DisparityController c;
        DisparityFrameInput in;
        in.enabled = in.depthValid = in.tracker = in.gazeValid = true;
        int64_t t = 1'000'000'000;
        auto frame = [&]() {
            in.displayTime = (t += 22 * Ms);
            return c.BeginFrame(in, s);
        };
        frame();
        in.inSaccade = true;
        float g = frame().gain;
        const float g2 = frame().gain;  // still in the same saccade: no second step
        Check(Near(g, 0.25f + 0.05f * 0.022f, 1e-4f) && Near(g2 - g, 0.05f * 0.022f, 1e-5f), "M7 Quad Views' saccade flag: one step per rising edge");
        in.inSaccade = false;
        in.gazeValid = false;
        for (int i = 0; i < 2; ++i) frame();  // 44 ms: too short for a blink
        in.gazeValid = true;
        g = frame().gain;
        in.gazeValid = false;
        for (int i = 0; i < 7; ++i) frame();  // 154 ms
        in.gazeValid = true;
        const float blink = frame().gain;
        in.gazeValid = false;
        for (int i = 0; i < 25; ++i) frame();  // 550 ms: tracking lost, not a blink
        Check(g < 0.5f && blink == 1.f, "M7 a 154 ms gaze gap is a blink (full step), a 44 ms one is not");
    }
    {
        DisparitySettings imm = s;
        imm.gated = false;
        DisparityController c;
        DisparityFrameInput in;
        in.enabled = in.depthValid = true;
        in.displayTime = 1;
        Check(c.BeginFrame(in, imm).gain == 1.f, "M7 immediate mode (A/B): the gain jumps to its target");
    }
    Check(Near(ChooseRenderedIpd(0.0712f, 0.064f, 1.f, 0.063f), 0.0712f, 1e-7) && Near(ChooseRenderedIpd(0.f, 0.071f, 1.1f, 0.063f), 0.071f, 1e-7) &&
              Near(ChooseRenderedIpd(0.f, 0.3f, 1.f, 0.063f), 0.063f, 1e-7),
          "M7 rendered IPD: pupil shift's push, else a plausible camera separation, else the located views");
}

// ---- D3D11 -------------------------------------------------------------------------------------------------------

void Hr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        std::printf("FAIL %s (0x%08lx)\n", what, (unsigned long)hr);
        std::exit(2);
    }
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    std::string s = text.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

void ReplaceOnce(std::string& text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    if (at == std::string::npos || text.find(from, at + 1) != std::string::npos) throw std::runtime_error("anchor not found once: " + from);
    text.replace(at, from.size(), to);
}

// The composition with Far depth, as scripts/build-quadviews.ps1 injects it (same anchors, exact-match once; the test
// checks the built shader equals this).
std::string InjectProjectionPS(std::string ps) {
    ReplaceOnce(ps, "    float focusViewShape;\n};",
                "    float focusViewShape;\n    float4 dcsvrWarp; // DCSVR Far depth: x on, yz focus uv -> periphery uv, w a held field\n"
                "    float4 dcsvrReuse[3]; // DCSVR Far depth: a held field's reprojection\n};");
    ReplaceOnce(ps, "Texture2D sourceFocusTexture : register(t1);",
                "Texture2D sourceFocusTexture : register(t1);\nTexture2D<float2> dcsvrDisparityField : register(t2);\n#include \"dcsvr_depthwarp_ps.hlsli\"");
    ReplaceOnce(ps,
                "    float4 color0 = sourceStereoTexture.Sample(sourceSampler, texcoord);\n\n"
                "    float2 layer1ProjectedCoordNdc = projectedFocusCoord.xy / projectedFocusCoord.z;",
                "    float2 layer1ProjectedCoordNdc = projectedFocusCoord.xy / projectedFocusCoord.z;\n"
                "    // DCSVR Far depth: the focus and the periphery sample move by one displacement field.\n"
                "    float2 dcsvrTexcoord = texcoord;\n"
                "    DcsvrApplyDisparity(dcsvrWarp, dcsvrReuse, dcsvrDisparityField, sourceSampler, layer1ProjectedCoordNdc, dcsvrTexcoord);\n"
                "    float4 color0 = sourceStereoTexture.Sample(sourceSampler, dcsvrTexcoord);");
    return ps;
}

class Includes final : public ID3DInclude {
  public:
    explicit Includes(std::vector<std::filesystem::path> d) : m_dirs(std::move(d)) {}
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override {
        for (const auto& dir : m_dirs) {
            const auto path = dir / name;
            if (std::filesystem::exists(path)) {
                m_held.push_back(ReadText(path));
                *data = m_held.back().data();
                *bytes = (UINT)m_held.back().size();
                return S_OK;
            }
        }
        return E_FAIL;
    }
    HRESULT __stdcall Close(LPCVOID) override { return S_OK; }

  private:
    std::vector<std::filesystem::path> m_dirs;
    std::vector<std::string> m_held;  // reserve()d: Open hands out pointers into it
  public:
    void Reserve() { m_held.reserve(64); }
};

ComPtr<ID3DBlob> Compile(const std::string& source, const char* name, const char* entry, const char* target, Includes& inc,
                         std::vector<D3D_SHADER_MACRO> macros = {}) {
    macros.push_back({nullptr, nullptr});
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source.data(), source.size(), name, macros.data(), &inc, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
        std::printf("FAIL compile %s %s: %s\n", name, entry, errors ? (const char*)errors->GetBufferPointer() : "?");
        std::exit(2);
    }
    return code;
}

struct Fov {
    float l, r, u, d;
    float Sx() const { return 2.f / (r - l); }
    float Sy() const { return 2.f / (u - d); }
    float Cx() const { return -(r + l) / (r - l); }
    float Cy() const { return -(u + d) / (u - d); }
};

// The geometry of one eye: DCS's focus frustum (colour at the output size, depth at DLSS's internal size inside a
// larger texture), the periphery and the composition target over the periphery at the focus density.
struct Rig {
    Fov focus{-0.62f, 0.58f, 0.6f, -0.585f};
    int cw = 2028, ch = 2003;
    int dw = 1176, dh = 1161;
    int vpX = 8, vpY = 6, texW = 1176 + 16, texH = 1161 + 12;
    Fov periph{-0.75f, 0.75f, 0.75f, -0.75f};
    int pw = 1040, ph = 1040;
    int compW = 2535, compH = 2535;
    float nearM = 0.02f;
    float ipd = 0.064f;
    dcsvr::Quat orient{};  // the eye's orientation in the world (the head's turn); identity: looking down -Z
    // Output pixels per tangent in the composition (the measurements' unit) and composition coordinates of a tangent.
    float CompPxPerTan() const { return compW / (periph.r - periph.l); }
    float CompX(float tx) const { return (tx - periph.l) / (periph.r - periph.l) * compW; }
    float CompY(float ty) const { return (periph.u - ty) / (periph.u - periph.d) * compH; }
};

// ---- The synthetic world ------------------------------------------------------------------------------------------
// Looking down -Z from the eye at (ex, 0, 0). Fronto-parallel quads at a forward distance (a wall is a large one),
// with an optional sinusoidal texture, and colour-only overlays drawn after the opaque scene without depth: a cloud
// (alpha-blended, in the world) and a HUD glyph (additive, collimated: fixed tangents in both eyes).
struct Quad {
    float z, x0, x1, y0, y1, value;
    float amp = 0.f, period = 1.f;
    uint8_t stencil = 0;
};
struct Overlay {
    bool collimated = false;
    float z = 0.f;
    float cx = 0.f, cy = 0.f, radius = 1.f;  // world metres at z, or tangents when collimated
    float value = 1.f, alpha = 1.f;
    bool additive = false;
    bool covered = false;  // drawn by the overlay passes (HUD symbology): absent from the engine's pre-overlay copy
};
struct Scene {
    std::vector<Quad> quads;
    std::vector<Overlay> overlays;
    float sky = 0.6f;
};
constexpr uint8_t CockpitStencil = 5 << 3;

struct Hit {
    float fwd = -1.f;  // forward distance, -1 for the sky
    float value = 0.f;
    uint8_t stencil = 0;
};

Hit Cast(const Scene& s, float ex, float tx, float ty) {
    Hit h;
    h.value = s.sky;
    for (const auto& q : s.quads) {
        if (h.fwd > 0.f && q.z >= h.fwd) continue;
        const float x = ex + tx * q.z, y = ty * q.z;
        if (x < q.x0 || x >= q.x1 || y < q.y0 || y >= q.y1) continue;
        h.fwd = q.z;
        h.value = q.value + (q.amp != 0.f ? q.amp * std::sin(6.2831853f * x / q.period) * std::sin(6.2831853f * y / q.period + 0.7f) : 0.f);
        h.stencil = q.stencil;
    }
    return h;
}

float OverlayAlpha(const Overlay& o, float ex, float tx, float ty) {
    float dx, dy;
    if (o.collimated) dx = tx - o.cx, dy = ty - o.cy;
    else dx = ex + tx * o.z - o.cx, dy = ty * o.z - o.cy;
    return o.alpha * std::exp(-(dx * dx + dy * dy) / (2.f * o.radius * o.radius));
}

float Shade(const Scene& s, float ex, float tx, float ty) {
    float v = Cast(s, ex, tx, ty).value;
    for (const auto& o : s.overlays) {
        const float a = OverlayAlpha(o, ex, tx, ty);
        v = o.additive ? v + o.value * a : v + (o.value - v) * a;
    }
    return v;
}

// The eye's tangents (tx, ty) as world tangents for an eye turned by q, and the factor from the world's forward
// distance (-Z) to the eye's. Identity: unchanged, factor 1, bit for bit.
bool ToWorld(const dcsvr::Quat& q, float& tx, float& ty, float& fwdScale) {
    fwdScale = 1.f;
    if (q.x == 0.f && q.y == 0.f && q.z == 0.f) return true;
    const float v[3] = {tx, ty, -1.f};
    float w[3];
    dcsvr::Rotate(q, v, w);
    if (!(w[2] < -1e-6f)) return false;
    tx = w[0] / -w[2];
    ty = w[1] / -w[2];
    fwdScale = 1.f / -w[2];
    return true;
}

// The anti-aliased colour of a frustum (DLSS's de-jittered output): 4 x 4 samples per pixel.
std::vector<float> RenderColour(const Scene& s, float ex, const Fov& f, int w, int h, int n = 4, const dcsvr::Quat& q = {}) {
    std::vector<float> out(size_t(w) * h);
    std::vector<int> rows(h);
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int y) {
        for (int x = 0; x < w; ++x) {
            float sum = 0.f;
            for (int sy = 0; sy < n; ++sy)
                for (int sx = 0; sx < n; ++sx) {
                    const float u = (x + (sx + 0.5f) / n) / w, v = (y + (sy + 0.5f) / n) / h;
                    float tx = f.l + (f.r - f.l) * u, ty = f.u + (f.d - f.u) * v, k;
                    sum += ToWorld(q, tx, ty, k) ? Shade(s, ex, tx, ty) : s.sky;
                }
            out[size_t(y) * w + x] = sum / (n * n);
        }
    });
    return out;
}

enum class DepthFormat { D32S8, D24S8, D32 };

// The engine's copy of DCS's depth for one view: one point sample per texel centre through the jittered projection
// (centre offset jx, jy NDC), reverse-Z infinite (d = near / forward, the sky 0), the cockpit's stencil. Outside the
// viewport the texture holds a near cockpit value, so any read past the viewport shows as a wrong shift.
std::vector<uint8_t> RenderDepth(const Scene& s, float ex, const Rig& rig, float cdx, float cdy, DepthFormat fmt,
                                 const std::function<void(int, int, float&)>& tamper = {}) {
    const int stride = fmt == DepthFormat::D32S8 ? 8 : 4;
    std::vector<uint8_t> out(size_t(rig.texW) * rig.texH * stride, 0);
    const float sx = rig.focus.Sx(), sy = rig.focus.Sy(), cx = rig.focus.Cx() + cdx, cy = rig.focus.Cy() + cdy;
    std::vector<int> rows(rig.texH);
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int ty) {
        for (int tx = 0; tx < rig.texW; ++tx) {
            const int i = tx - rig.vpX, j = ty - rig.vpY;
            float d = 1.f;
            uint8_t st = CockpitStencil;
            if (i >= 0 && j >= 0 && i < rig.dw && j < rig.dh) {
                const float nx = (i + 0.5f) / rig.dw * 2.f - 1.f, ny = 1.f - (j + 0.5f) / rig.dh * 2.f;
                float wx = (nx - cx) / sx, wy = (ny - cy) / sy, k;
                const Hit h = ToWorld(rig.orient, wx, wy, k) ? Cast(s, ex, wx, wy) : Hit{};
                d = h.fwd > 0.f ? rig.nearM / (h.fwd * k) : 0.f;
                st = h.stencil;
                if (tamper) tamper(i, j, d);
            }
            uint8_t* p = out.data() + (size_t(ty) * rig.texW + tx) * stride;
            if (fmt == DepthFormat::D32S8) {
                std::memcpy(p, &d, 4);
                p[4] = st;
            } else if (fmt == DepthFormat::D24S8) {
                const uint32_t v = (uint32_t)std::lround(std::clamp(d, 0.f, 1.f) * 16777215.f) | uint32_t(st) << 24;
                std::memcpy(p, &v, 4);
            } else {
                std::memcpy(p, &d, 4);
            }
        }
    });
    return out;
}

// 8-bit sRGB of a linear value (the swapchain's and the pre-overlay copy's storage).
uint8_t SrgbByte(float v) {
    v = std::clamp(v, 0.f, 1.f);
    const float e = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
    return uint8_t(std::lround(e * 255.f));
}

struct DepthTex {
    ComPtr<ID3D11Texture2D> tex, coverageTex;
    ComPtr<ID3D11ShaderResourceView> depth, stencil, coverage;
    bool hasStencil = true;
};

struct FieldTex {
    ComPtr<ID3D11Texture2D> tex, staging;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    int w = 0, h = 0;
};

struct Field {
    int w = 0, h = 0;
    std::vector<float> u, v;  // backward displacement, focus uv
    float U(int x, int y) const { return u[size_t(y) * w + x]; }
};

struct Image {
    int w = 0, h = 0;
    std::vector<float> r;  // red (all channels are equal)
    uint64_t hash = 0;     // FNV-1a of every byte of every channel
    bool finite = true;
    float At(int x, int y) const { return r[size_t(y) * w + x]; }
};

struct PsConstants {
    float smoothingArea;
    uint32_t ignoreAlpha, unpremultiplied, debugFocusView;
    float focusViewShape;
    float pad[3];
    float warp[4];
    float reuse[3][4];
};
static_assert(sizeof(PsConstants) == 96);

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> fieldCs, field2Cs, field3itCs, field64itCs, warpCs, coverageCs;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> psShipping, psFarDepth;
    ComPtr<ID3D11Buffer> cb, cbWarp, cbCoverage, psCb, vsCb;
    ComPtr<ID3D11SamplerState> linearClamp;
    ComPtr<ID3D11RasterizerState> raster;
    std::string adapter;
    bool warpDevice = false;

    Gpu(const std::filesystem::path& native, const std::filesystem::path& layer, bool forceWarp) {
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        if (forceWarp || FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context))) {
            Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context), "WARP device");
            warpDevice = true;
        }
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> a;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&a)) && SUCCEEDED(a->GetDesc(&desc))) {
            char name[128]{};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
            adapter = name;
        }
        Includes inc({native, layer});
        inc.Reserve();
        const std::string cs = ReadText(native / "dcsvr_depthwarp_cs.hlsl");
        auto b = Compile(cs, "dcsvr_depthwarp_cs.hlsl", "FieldMain", "cs_5_0", inc);
        Hr(device->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &fieldCs), "FieldMain");
        b = Compile(cs, "dcsvr_depthwarp_cs.hlsl", "FieldMain", "cs_5_0", inc, {{"DCSVR_FOOTPRINT", "2"}});
        Hr(device->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &field2Cs), "FieldMain 2x2");
        b = Compile(cs, "dcsvr_depthwarp_cs.hlsl", "FieldMain", "cs_5_0", inc, {{"DCSVR_FIELD_ITERATIONS", "3"}});
        Hr(device->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &field3itCs), "FieldMain 3 iterations");
        b = Compile(cs, "dcsvr_depthwarp_cs.hlsl", "FieldMain", "cs_5_0", inc, {{"DCSVR_FIELD_ITERATIONS", "64"}});
        Hr(device->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &field64itCs), "FieldMain 64 iterations");
        b = Compile(ReadText(native / "dcsvr_coverage_cs.hlsl"), "dcsvr_coverage_cs.hlsl", "CoverageMain", "cs_5_0", inc);
        Hr(device->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &coverageCs), "CoverageMain");
        b = Compile(cs, "dcsvr_depthwarp_cs.hlsl", "WarpMain", "cs_5_0", inc);
        Hr(device->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &warpCs), "WarpMain");
        b = Compile(ReadText(layer / "ProjectionVS.hlsl"), "ProjectionVS.hlsl", "main", "vs_5_0", inc);
        Hr(device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs), "ProjectionVS");
        // scripts/build-quadviews.ps1 builds Far depth into the layer's ProjectionPS and keeps the shader from before
        // (ProjectionPS.nofardepth.hlsl): that is "the shipping composition without Far depth" here, and the layer's
        // own shader must be exactly this test's injection into it (same anchors, same text). Before the build has
        // injected it, the layer's shader is the one without Far depth and the test injects it itself.
        const std::string built = ReadText(layer / "ProjectionPS.hlsl");
        const bool builtIn = built.find("dcsvrDisparityField") != std::string::npos;
        const std::string shipping = builtIn ? ReadText(layer / "ProjectionPS.nofardepth.hlsl") : built;
        if (shipping.empty()) throw std::runtime_error("ProjectionPS.nofardepth.hlsl missing: run scripts/build-quadviews.ps1");
        const std::string farDepth = InjectProjectionPS(shipping);
        if (builtIn) Check(farDepth == built, "the layer's ProjectionPS is exactly this test's injection (the build's anchors and text)");
        b = Compile(shipping, "ProjectionPS.hlsl", "main", "ps_5_0", inc);
        Hr(device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psShipping), "ProjectionPS");
        b = Compile(builtIn ? built : farDepth, "ProjectionPS.hlsl (Far depth)", "main", "ps_5_0", inc);
        Hr(device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psFarDepth), "ProjectionPS Far depth");
        cb = Buffer(sizeof(dcsvr::DisparityConstants));
        cbWarp = Buffer(16);
        psCb = Buffer(sizeof(PsConstants));
        cbCoverage = Buffer(sizeof(dcsvr::CoverageConstants));
        vsCb = Buffer(64);
        D3D11_SAMPLER_DESC s{};
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;  // the layer's m_linearClampSampler
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.MaxAnisotropy = 1;
        s.MinLOD = D3D11_MIP_LOD_BIAS_MIN;
        s.MaxLOD = D3D11_MIP_LOD_BIAS_MAX;
        Hr(device->CreateSamplerState(&s, &linearClamp), "sampler");
        D3D11_RASTERIZER_DESC r{};
        r.FillMode = D3D11_FILL_SOLID;
        r.CullMode = D3D11_CULL_NONE;
        Hr(device->CreateRasterizerState(&r, &raster), "raster");
    }

    ComPtr<ID3D11Buffer> Buffer(UINT size) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = (size + 15) / 16 * 16;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> b;
        Hr(device->CreateBuffer(&d, nullptr, &b), "buffer");
        return b;
    }
    void Upload(ID3D11Buffer* b, const void* data, size_t size) {
        D3D11_MAPPED_SUBRESOURCE m;
        Hr(context->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "map");
        std::memcpy(m.pData, data, size);
        context->Unmap(b, 0);
    }

    DepthTex MakeDepth(const std::vector<uint8_t>& bytes, const Rig& rig, DepthFormat fmt) {
        DepthTex t;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = rig.texW;
        d.Height = rig.texH;
        d.MipLevels = d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;  // the engine's copy: no depth-stencil binding
        d.Format = fmt == DepthFormat::D32S8 ? DXGI_FORMAT_R32G8X24_TYPELESS : fmt == DepthFormat::D24S8 ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
        const D3D11_SUBRESOURCE_DATA init{bytes.data(), UINT(rig.texW * (fmt == DepthFormat::D32S8 ? 8 : 4)), 0};
        Hr(device->CreateTexture2D(&d, &init, &t.tex), "depth texture");
        DXGI_FORMAT depth, stencil;
        dcsvr::DepthSrvFormats(d.Format, depth, stencil);
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
        v.Format = depth;
        Hr(device->CreateShaderResourceView(t.tex.Get(), &v, &t.depth), "depth view");
        t.hasStencil = stencil != DXGI_FORMAT_UNKNOWN;
        if (t.hasStencil) {
            v.Format = stencil;
            Hr(device->CreateShaderResourceView(t.tex.Get(), &v, &t.stencil), "stencil view");
        }
        return t;
    }
    // A grey image as 8-bit sRGB (R8G8B8A8_TYPELESS viewed as R8G8B8A8_UNORM_SRGB, as DCS's targets), w x h at (x, y)
    // of a texture of tw x th (the rest black): the engine's pre-overlay copy or the swapchain image DCS submits.
    ComPtr<ID3D11ShaderResourceView> MakeSrgb(const std::vector<float>& v, int w, int h, int x = 0, int y = 0, int tw = 0, int th = 0) {
        tw = std::max(tw, x + w), th = std::max(th, y + h);
        std::vector<uint8_t> rgba(size_t(tw) * th * 4, 0);
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i) {
                uint8_t* p = &rgba[(size_t(j + y) * tw + i + x) * 4];
                p[0] = p[1] = p[2] = SrgbByte(v[size_t(j) * w + i]);
                p[3] = 255;
            }
        D3D11_TEXTURE2D_DESC d{};
        d.Width = tw;
        d.Height = th;
        d.MipLevels = d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA init{rgba.data(), UINT(tw * 4), 0};
        ComPtr<ID3D11Texture2D> t;
        Hr(device->CreateTexture2D(&d, &init, &t), "sRGB image");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> srv;
        Hr(device->CreateShaderResourceView(t.Get(), &sv, &srv), "sRGB view");
        return srv;
    }

    // CoverageMain into t's coverage (R8_UNORM, the field's size); returns the changed texels before the dilation, and
    // the coverage itself when asked.
    uint32_t BuildCoverage(DepthTex& t, const dcsvr::CoverageConstants& k, ID3D11ShaderResourceView* pre, ID3D11ShaderResourceView* submitted,
                           std::vector<uint8_t>* readback = nullptr, double* timeMs = nullptr) {
        const UINT fw = UINT(k.field[0]), fh = UINT(k.field[1]);
        D3D11_TEXTURE2D_DESC d{};
        d.Width = fw;
        d.Height = fh;
        d.MipLevels = d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R8_UNORM;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        Hr(device->CreateTexture2D(&d, nullptr, &t.coverageTex), "coverage texture");
        Hr(device->CreateShaderResourceView(t.coverageTex.Get(), nullptr, &t.coverage), "coverage view");
        ComPtr<ID3D11UnorderedAccessView> uav;
        Hr(device->CreateUnorderedAccessView(t.coverageTex.Get(), nullptr, &uav), "coverage uav");
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        const uint32_t zero[4] = {};
        const D3D11_SUBRESOURCE_DATA bi{zero, 0, 0};
        ComPtr<ID3D11Buffer> count, countStaging;
        Hr(device->CreateBuffer(&bd, &bi, &count), "count buffer");
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 4;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        ComPtr<ID3D11UnorderedAccessView> countUav;
        Hr(device->CreateUnorderedAccessView(count.Get(), &ud, &countUav), "count uav");
        bd.BindFlags = 0;
        bd.MiscFlags = 0;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Hr(device->CreateBuffer(&bd, nullptr, &countStaging), "count staging");
        Upload(cbCoverage.Get(), &k, sizeof(k));
        ID3D11ShaderResourceView* srvs[2] = {pre, submitted};
        ID3D11UnorderedAccessView* uavs[2] = {uav.Get(), countUav.Get()};
        context->CSSetShader(coverageCs.Get(), nullptr, 0);
        context->CSSetConstantBuffers(2, 1, cbCoverage.GetAddressOf());
        context->CSSetShaderResources(3, 2, srvs);
        context->CSSetUnorderedAccessViews(1, 2, uavs, nullptr);
        context->Dispatch((fw + 15) / 16, (fh + 15) / 16, 1);
        if (timeMs) *timeMs = TimeMs([&] { context->Dispatch((fw + 15) / 16, (fh + 15) / 16, 1); }, 50);
        ID3D11ShaderResourceView* none[2] = {};
        ID3D11UnorderedAccessView* noUav[2] = {};
        context->CSSetShaderResources(3, 2, none);
        context->CSSetUnorderedAccessViews(1, 2, noUav, nullptr);
        context->CopyResource(countStaging.Get(), count.Get());
        D3D11_MAPPED_SUBRESOURCE m;
        Hr(context->Map(countStaging.Get(), 0, D3D11_MAP_READ, 0, &m), "count read");
        const uint32_t changed = *static_cast<const uint32_t*>(m.pData);
        context->Unmap(countStaging.Get(), 0);
        if (readback) {
            d.BindFlags = 0;
            d.Usage = D3D11_USAGE_STAGING;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> st;
            Hr(device->CreateTexture2D(&d, nullptr, &st), "coverage staging");
            context->CopyResource(st.Get(), t.coverageTex.Get());
            Hr(context->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m), "coverage read");
            readback->assign(size_t(fw) * fh, 0);
            for (UINT y = 0; y < fh; ++y)
                std::memcpy(&(*readback)[size_t(y) * fw], static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, fw);
            context->Unmap(st.Get(), 0);
        }
        return changed;
    }

    ComPtr<ID3D11ShaderResourceView> MakeColour(const std::vector<float>& v, int w, int h, ComPtr<ID3D11Texture2D>* keep = nullptr) {
        std::vector<float> rgba(v.size() * 4);
        for (size_t i = 0; i < v.size(); ++i) rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = v[i], rgba[i * 4 + 3] = 1.f;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA init{rgba.data(), UINT(w * 16), 0};
        ComPtr<ID3D11Texture2D> t;
        Hr(device->CreateTexture2D(&d, &init, &t), "colour");
        ComPtr<ID3D11ShaderResourceView> srv;
        Hr(device->CreateShaderResourceView(t.Get(), nullptr, &srv), "colour view");
        if (keep) *keep = t;
        return srv;
    }

    FieldTex MakeField(int w, int h) {
        FieldTex f;
        f.w = w;
        f.h = h;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R16G16_FLOAT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        Hr(device->CreateTexture2D(&d, nullptr, &f.tex), "field");
        Hr(device->CreateShaderResourceView(f.tex.Get(), nullptr, &f.srv), "field view");
        Hr(device->CreateUnorderedAccessView(f.tex.Get(), nullptr, &f.uav), "field uav");
        d.BindFlags = 0;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Hr(device->CreateTexture2D(&d, nullptr, &f.staging), "field staging");
        return f;
    }

    void DispatchField(const dcsvr::DisparityConstants& k, const DepthTex& depth, FieldTex& f, bool footprint2 = false, ID3D11ComputeShader* variant = nullptr) {
        Upload(cb.Get(), &k, sizeof(k));
        ID3D11ShaderResourceView* srvs[3] = {depth.depth.Get(), depth.stencil.Get(), depth.coverage.Get()};
        context->CSSetShader(variant ? variant : footprint2 ? field2Cs.Get() : fieldCs.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
        context->CSSetShaderResources(0, 3, srvs);
        context->CSSetUnorderedAccessViews(0, 1, f.uav.GetAddressOf(), nullptr);
        context->Dispatch((f.w + 15) / 16, (f.h + 15) / 16, 1);
        ID3D11ShaderResourceView* none[3] = {};
        ID3D11UnorderedAccessView* noUav = nullptr;
        context->CSSetShaderResources(0, 3, none);
        context->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    }

    Field ReadField(FieldTex& f) {
        context->CopyResource(f.staging.Get(), f.tex.Get());
        D3D11_MAPPED_SUBRESOURCE m;
        Hr(context->Map(f.staging.Get(), 0, D3D11_MAP_READ, 0, &m), "field read");
        Field out{f.w, f.h, std::vector<float>(size_t(f.w) * f.h), std::vector<float>(size_t(f.w) * f.h)};
        for (int y = 0; y < f.h; ++y) {
            const auto* row = reinterpret_cast<const DirectX::PackedVector::HALF*>(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch);
            for (int x = 0; x < f.w; ++x) {
                out.u[size_t(y) * f.w + x] = DirectX::PackedVector::XMConvertHalfToFloat(row[x * 2]);
                out.v[size_t(y) * f.w + x] = DirectX::PackedVector::XMConvertHalfToFloat(row[x * 2 + 1]);
            }
        }
        context->Unmap(f.staging.Get(), 0);
        return out;
    }

    // The compute fallback into an RGBA32F target of the colour's size.
    ComPtr<ID3D11ShaderResourceView> Warp(ID3D11ShaderResourceView* colour, const FieldTex& f, int w, int h, ComPtr<ID3D11Texture2D>* keep = nullptr) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> t;
        Hr(device->CreateTexture2D(&d, nullptr, &t), "warped");
        ComPtr<ID3D11UnorderedAccessView> uav;
        ComPtr<ID3D11ShaderResourceView> srv;
        Hr(device->CreateUnorderedAccessView(t.Get(), nullptr, &uav), "warped uav");
        Hr(device->CreateShaderResourceView(t.Get(), nullptr, &srv), "warped view");
        DispatchWarp(colour, f.srv.Get(), uav.Get(), w, h);
        if (keep) *keep = t;
        return srv;
    }
    void DispatchWarp(ID3D11ShaderResourceView* colour, ID3D11ShaderResourceView* field, ID3D11UnorderedAccessView* out, int w, int h) {
        const float size[4] = {float(w), float(h), 1.f / w, 1.f / h};
        Upload(cbWarp.Get(), size, sizeof(size));
        ID3D11ShaderResourceView* srvs[2] = {colour, field};
        context->CSSetShader(warpCs.Get(), nullptr, 0);
        context->CSSetConstantBuffers(1, 1, cbWarp.GetAddressOf());
        context->CSSetShaderResources(0, 2, srvs);
        context->CSSetSamplers(0, 1, linearClamp.GetAddressOf());
        context->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
        context->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        ID3D11ShaderResourceView* none[2] = {};
        ID3D11UnorderedAccessView* noUav = nullptr;
        context->CSSetShaderResources(0, 2, none);
        context->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    }

    // ProjectionVS's focusProjection for a focus frustum inside the periphery frustum (same orientation): target NDC
    // -> focus NDC, affine; stored transposed as the layer stores it (HLSL's column-major packing).
    void SetComposition(const Rig& rig, const Fov& focus) {
        const float ax = focus.Sx() * (rig.periph.r - rig.periph.l) / 2.f, bx = focus.Sx() * (rig.periph.l + (rig.periph.r - rig.periph.l) / 2.f) + focus.Cx();
        const float ay = focus.Sy() * (rig.periph.u - rig.periph.d) / 2.f, by = focus.Sy() * (rig.periph.d + (rig.periph.u - rig.periph.d) / 2.f) + focus.Cy();
        float m[4][4] = {{ax, 0, 0, 0}, {0, ay, 0, 0}, {0, 0, 0, 0}, {bx, by, 0, 1}};
        float t[16];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) t[c * 4 + r] = m[r][c];
        Upload(vsCb.Get(), t, sizeof(t));
    }

    // One composition draw (as the layer's: triangle strip of 3, its rasterizer, the full target as the viewport).
    void Draw(bool farDepth, const PsConstants& k, ID3D11ShaderResourceView* periph, ID3D11ShaderResourceView* focus, ID3D11ShaderResourceView* field,
              ID3D11RenderTargetView* rtv, int w, int h) {
        Upload(psCb.Get(), &k, sizeof(k));
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->IASetInputLayout(nullptr);
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->RSSetState(raster.Get());
        D3D11_VIEWPORT vp{};
        vp.Width = float(w);
        vp.Height = float(h);
        vp.MaxDepth = 1.f;
        context->RSSetViewports(1, &vp);
        context->VSSetConstantBuffers(0, 1, vsCb.GetAddressOf());
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, psCb.GetAddressOf());
        context->PSSetSamplers(0, 1, linearClamp.GetAddressOf());
        ID3D11ShaderResourceView* srvs[3] = {periph, focus, field};
        context->PSSetShaderResources(0, 3, srvs);
        context->PSSetShader(farDepth ? psFarDepth.Get() : psShipping.Get(), nullptr, 0);
        context->Draw(3, 0);
        ID3D11ShaderResourceView* none[3] = {};
        context->PSSetShaderResources(0, 3, none);
        ID3D11RenderTargetView* noRtv = nullptr;
        context->OMSetRenderTargets(1, &noRtv, nullptr);
    }

    ComPtr<ID3D11Texture2D> compTarget, compStaging;
    ComPtr<ID3D11RenderTargetView> compRtv;
    int compW = 0, compH = 0;
    Image Composite(const Rig& rig, bool farDepth, const PsConstants& k, ID3D11ShaderResourceView* periph, ID3D11ShaderResourceView* focus,
                    ID3D11ShaderResourceView* field) {
        if (compW != rig.compW || compH != rig.compH) {
            compW = rig.compW;
            compH = rig.compH;
            D3D11_TEXTURE2D_DESC d{};
            d.Width = compW;
            d.Height = compH;
            d.MipLevels = d.ArraySize = 1;
            d.SampleDesc.Count = 1;
            d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET;
            Hr(device->CreateTexture2D(&d, nullptr, compTarget.ReleaseAndGetAddressOf()), "composition target");
            Hr(device->CreateRenderTargetView(compTarget.Get(), nullptr, compRtv.ReleaseAndGetAddressOf()), "composition rtv");
            d.BindFlags = 0;
            d.Usage = D3D11_USAGE_STAGING;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            Hr(device->CreateTexture2D(&d, nullptr, compStaging.ReleaseAndGetAddressOf()), "composition staging");
        }
        const float clear[4] = {-1.f, -1.f, -1.f, -1.f};
        context->ClearRenderTargetView(compRtv.Get(), clear);
        Draw(farDepth, k, periph, focus, field, compRtv.Get(), compW, compH);
        context->CopyResource(compStaging.Get(), compTarget.Get());
        D3D11_MAPPED_SUBRESOURCE m;
        Hr(context->Map(compStaging.Get(), 0, D3D11_MAP_READ, 0, &m), "composition read");
        Image img{compW, compH, std::vector<float>(size_t(compW) * compH)};
        uint64_t hash = 1469598103934665603ull;
        for (int y = 0; y < compH; ++y) {
            const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
            for (int i = 0; i < compW * 16; ++i) hash = (hash ^ row[i]) * 1099511628211ull;
            const float* f = reinterpret_cast<const float*>(row);
            for (int x = 0; x < compW; ++x) {
                img.r[size_t(y) * compW + x] = f[x * 4];
                img.finite &= std::isfinite(f[x * 4]) && std::isfinite(f[x * 4 + 3]);
            }
        }
        context->Unmap(compStaging.Get(), 0);
        img.hash = hash;
        return img;
    }

    // GPU time of a piece of work, median of runs (timestamp queries).
    double TimeMs(const std::function<void()>& work, int runs) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        ComPtr<ID3D11Query> disjoint, begin, end;
        Hr(device->CreateQuery(&qd, &disjoint), "query");
        qd.Query = D3D11_QUERY_TIMESTAMP;
        Hr(device->CreateQuery(&qd, &begin), "query");
        Hr(device->CreateQuery(&qd, &end), "query");
        std::vector<double> ms;
        for (int i = 0; i < runs + 3; ++i) {
            context->Begin(disjoint.Get());
            context->End(begin.Get());
            work();
            context->End(end.Get());
            context->End(disjoint.Get());
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            while (context->GetData(disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE) {}
            UINT64 t0 = 0, t1 = 0;
            while (context->GetData(begin.Get(), &t0, sizeof(t0), 0) == S_FALSE) {}
            while (context->GetData(end.Get(), &t1, sizeof(t1), 0) == S_FALSE) {}
            if (i >= 3 && !dj.Disjoint && dj.Frequency) ms.push_back(double(t1 - t0) * 1000.0 / double(dj.Frequency));
        }
        if (ms.empty()) return -1.0;
        std::sort(ms.begin(), ms.end());
        return ms[ms.size() / 2];
    }
};

// ---- One eye through the pipeline -------------------------------------------------------------------------------

struct EyeSetup {
    int eye = 0;               // 0 left, 1 right
    float gx = 0.03f, gy = 0.02f;  // gaze tangents (focus centre when !gazeValid)
    bool gazeValid = true;
    float gain = 1.f;
    float jitterX = 0.f, jitterY = 0.f;  // internal px
    bool jitterInProj = true;            // DCS puts the jitter into gProj (else the depth is jittered, gProj is not)
    bool jitterKnown = false;            // the engine's jitterPx
    DepthFormat format = DepthFormat::D32S8;
    float minDepth = 0.f, maxDepth = 1.f;
    float shape = 0.f, smoothing = 0.2f;
    bool footprint2 = false;
    bool c0Override = false;  // the layer's estimate of the unjittered centre
    float c0x = 0.f, c0y = 0.f;
    bool coverage = false;    // the engine pushes its pre-overlay copy: CoverageMain pins what the overlays drew
};

struct EyeRun {
    dcsvr::DisparityConstants k{};
    Field field;
    Image off, on;
};

float EyeX(const Rig& rig, int eye) {
    return eye ? rig.ipd / 2.f : -rig.ipd / 2.f;
}

dcsvr::DisparityConstants MakeConstants(const Rig& rig, const EyeSetup& e, const dcsvr::DisparitySettings& s) {
    float cdx, cdy;
    dcsvr::JitterNdc(e.jitterX, e.jitterY, float(rig.dw), float(rig.dh), cdx, cdy);
    const auto m = dcsvr::ProjectionParams::Make(rig.focus.l, rig.focus.r, rig.focus.u, rig.focus.d, rig.nearM, e.jitterInProj ? cdx : 0.f, e.jitterInProj ? cdy : 0.f);
    dcsvr::FieldInputs in;
    in.proj = dcsvr::ProjectionParams::Parse(m.data());
    in.c0x = rig.focus.Cx();
    in.c0y = rig.focus.Cy();
    if (e.c0Override) {
        in.c0x = e.c0x;
        in.c0y = e.c0y;
    } else if (e.jitterKnown) {
        // c0 from this frame's c and the engine's jitterPx.
        in.c0x = in.proj.cx - cdx;
        in.c0y = in.proj.cy - cdy;
    }
    const float vp[6] = {float(rig.vpX), float(rig.vpY), float(rig.dw), float(rig.dh), e.minDepth, e.maxDepth};
    std::copy(vp, vp + 6, in.viewport);
    in.hasStencil = e.format != DepthFormat::D32;
    in.hasCoverage = e.coverage;
    in.eye = e.eye;
    in.ipdM = rig.ipd;
    in.gazeValid = e.gazeValid;
    const float l = std::sqrt(e.gx * e.gx + e.gy * e.gy + 1.f);
    in.gaze[0] = e.gx / l;
    in.gaze[1] = e.gy / l;
    in.gaze[2] = -1.f / l;
    in.gain = e.gain;
    in.focusShape = e.shape;
    in.smoothing = e.smoothing;
    dcsvr::DisparityConstants k;
    dcsvr::FieldConstants(in, s, k);
    return k;
}

PsConstants Ps(const Rig& rig, const EyeSetup& e, bool warpOn, bool debugFocus = false) {
    PsConstants p{};
    p.smoothingArea = e.smoothing;
    p.ignoreAlpha = 1;
    p.debugFocusView = debugFocus ? 1 : 0;
    p.focusViewShape = e.shape;
    p.warp[0] = warpOn ? 1.f : 0.f;
    p.warp[1] = (rig.focus.r - rig.focus.l) / (rig.periph.r - rig.periph.l);
    p.warp[2] = (rig.focus.u - rig.focus.d) / (rig.periph.u - rig.periph.d);
    return p;
}

struct Images {
    std::vector<float> focus, periph;
    ComPtr<ID3D11ShaderResourceView> focusSrv, periphSrv;
};

Images RenderImages(Gpu& gpu, const Scene& scene, const Rig& rig, int eye) {
    Images im;
    const float ex = EyeX(rig, eye);
    im.focus = RenderColour(scene, ex, rig.focus, rig.cw, rig.ch, 4, rig.orient);
    im.periph = RenderColour(scene, ex, rig.periph, rig.pw, rig.ph, 4, rig.orient);
    im.focusSrv = gpu.MakeColour(im.focus, rig.cw, rig.ch);
    im.periphSrv = gpu.MakeColour(im.periph, rig.pw, rig.ph);
    return im;
}

DepthTex RenderDepthTex(Gpu& gpu, const Scene& scene, const Rig& rig, const EyeSetup& e,
                        const std::function<void(int, int, float&)>& tamper = {}) {
    float cdx, cdy;
    dcsvr::JitterNdc(e.jitterX, e.jitterY, float(rig.dw), float(rig.dh), cdx, cdy);
    auto bytes = RenderDepth(scene, EyeX(rig, e.eye), rig, cdx, cdy, e.format, tamper);
    if (e.minDepth != 0.f || e.maxDepth != 1.f) {
        // The viewport's depth range as D3D applies it: stored = Min + d (Max - Min).
        for (size_t i = 0; i < bytes.size(); i += 8) {
            float d;
            std::memcpy(&d, &bytes[i], 4);
            d = e.minDepth + d * (e.maxDepth - e.minDepth);
            std::memcpy(&bytes[i], &d, 4);
        }
    }
    DepthTex t = gpu.MakeDepth(bytes, rig, e.format);
    if (e.coverage) {
        // The engine's copy before the overlay passes (the scene without the covered overlays) against the image DCS
        // submits (with them), both 8-bit sRGB at the output resolution; the submitted one inside a larger swapchain
        // image at an offset, as a subImage.
        Scene pre = scene;
        pre.overlays.erase(std::remove_if(pre.overlays.begin(), pre.overlays.end(), [](const Overlay& o) { return o.covered; }),
                           pre.overlays.end());
        const float ex = EyeX(rig, e.eye);
        const auto before = RenderColour(pre, ex, rig.focus, rig.cw, rig.ch, 2, rig.orient);
        const auto after = RenderColour(scene, ex, rig.focus, rig.cw, rig.ch, 2, rig.orient);
        const auto preSrv = gpu.MakeSrgb(before, rig.cw, rig.ch);
        const auto subSrv = gpu.MakeSrgb(after, rig.cw, rig.ch, 12, 4, rig.cw + 20, rig.ch + 10);
        dcsvr::CoverageConstants ck;
        const dcsvr::DisparitySettings defaults;
        if (!dcsvr::MakeCoverageConstants(rig.dw, rig.dh, rig.cw, rig.ch, 12, 4, rig.cw, rig.ch, defaults.overlayThreshold, ck))
            throw std::runtime_error("coverage constants");
        gpu.BuildCoverage(t, ck, preSrv.Get(), subSrv.Get());
    }
    return t;
}

EyeRun RunEye(Gpu& gpu, const Scene& scene, const Rig& rig, const EyeSetup& e, const dcsvr::DisparitySettings& s, const Images& im,
              bool compose = true, const DepthTex* depthOverride = nullptr) {
    EyeRun run;
    run.k = MakeConstants(rig, e, s);
    DepthTex depth = depthOverride ? *depthOverride : RenderDepthTex(gpu, scene, rig, e);
    FieldTex f = gpu.MakeField(rig.dw, rig.dh);
    gpu.DispatchField(run.k, depth, f, e.footprint2);
    run.field = gpu.ReadField(f);
    if (compose) {
        gpu.SetComposition(rig, rig.focus);
        run.off = gpu.Composite(rig, false, Ps(rig, e, false), im.periphSrv.Get(), im.focusSrv.Get(), nullptr);
        run.on = gpu.Composite(rig, true, Ps(rig, e, true), im.periphSrv.Get(), im.focusSrv.Get(), f.srv.Get());
    }
    return run;
}

// Contrast-weighted centroid of an object inside a window (composition px), against the median of the window's border
// in the reference image (the same background for both images of a pair).
struct Centroid {
    double x = 0, y = 0, mass = 0;
};
float BorderMedian(const Image& img, int x0, int y0, int x1, int y1) {
    std::vector<float> b;
    for (int x = x0; x <= x1; ++x) b.push_back(img.At(x, y0)), b.push_back(img.At(x, y1));
    for (int y = y0; y <= y1; ++y) b.push_back(img.At(x0, y)), b.push_back(img.At(x1, y));
    std::nth_element(b.begin(), b.begin() + b.size() / 2, b.end());
    return b[b.size() / 2];
}
Centroid Measure(const Image& img, double cx, double cy, int half, float bg) {
    Centroid c;
    const int x0 = int(cx) - half, x1 = int(cx) + half, y0 = int(cy) - half, y1 = int(cy) + half;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const double w = std::fabs(img.At(x, y) - bg);
            c.x += w * (x + 0.5);
            c.y += w * (y + 0.5);
            c.mass += w;
        }
    if (c.mass > 0) c.x /= c.mass, c.y /= c.mass;
    return c;
}
// Shift (on - off) of an object around (cx, cy), composition px.
std::pair<double, double> Shift(const Image& off, const Image& on, double cx, double cy, int half) {
    const float bg = BorderMedian(off, int(cx) - half, int(cy) - half, int(cx) + half, int(cy) + half);
    const Centroid a = Measure(off, cx, cy, half, bg), b = Measure(on, cx, cy, half, bg);
    return {b.x - a.x, b.y - a.y};
}

// The expected shift (composition px) of content at the eye's tangents (tx, ty) and range: the CPU reference of the
// field's forward shift through the colour's frustum.
std::pair<double, double> Expected(const Rig& rig, const dcsvr::DisparityConstants& k, float tx, float ty, float range, bool cockpit = false) {
    const float nx = rig.focus.Sx() * tx + rig.focus.Cx(), ny = rig.focus.Sy() * ty + rig.focus.Cy();
    float su, sv;
    dcsvr::ReferenceShiftUv(k, nx * 0.5f + 0.5f, 0.5f - ny * 0.5f, range, cockpit, su, sv);
    const double dtx = su * (rig.focus.r - rig.focus.l), dty = -sv * (rig.focus.u - rig.focus.d);
    return {dtx * rig.CompPxPerTan(), -dty * rig.CompPxPerTan()};
}

double FieldPx(const Rig& rig, const Field& f, int x, int y) {
    return f.U(x, y) * rig.cw;  // focus output px
}

double MaxAbsField(const Field& f) {
    double m = 0;
    for (size_t i = 0; i < f.u.size(); ++i) m = std::max({m, (double)std::fabs(f.u[i]), (double)std::fabs(f.v[i])});
    return m;
}
bool FieldFinite(const Field& f) {
    for (size_t i = 0; i < f.u.size(); ++i)
        if (!std::isfinite(f.u[i]) || !std::isfinite(f.v[i])) return false;
    return true;
}

Quad Wall(float z, float value, float amp = 0.f, float period = 1.f) {
    return {z, -1e7f, 1e7f, -1e7f, 1e7f, value, amp, period};
}
// A fronto-parallel object of w x h px at the centre of tangents (tx, ty) and forward distance z (sizes at the focus
// colour's density, g_pxPerTan), so its size on screen does not depend on its distance.
float g_pxPerTan = 1690.f;
Quad Object(float tx, float ty, float z, float wPx, float hPx, float value) {
    const float hw = wPx / g_pxPerTan * z / 2.f, hh = hPx / g_pxPerTan * z / 2.f;
    return {z, tx * z - hw, tx * z + hw, ty * z - hh, ty * z + hh, value};
}

// Field texels whose whole 7x7 depth neighbourhood (unjittered) is one surface: "away from edges" (the footprint's
// dilation, the backward solve and the jitter each reach one texel or less).
std::vector<uint8_t> FlatMask(const Scene& sc, const Rig& rig, int eye) {
    std::vector<float> fwd(size_t(rig.dw) * rig.dh);
    for (int y = 0; y < rig.dh; ++y)
        for (int x = 0; x < rig.dw; ++x) {
            const float nx = (x + 0.5f) / rig.dw * 2 - 1, ny = 1 - (y + 0.5f) / rig.dh * 2;
            float tx = (nx - rig.focus.Cx()) / rig.focus.Sx(), ty = (ny - rig.focus.Cy()) / rig.focus.Sy(), k;
            fwd[size_t(y) * rig.dw + x] = ToWorld(rig.orient, tx, ty, k) ? Cast(sc, EyeX(rig, eye), tx, ty).fwd : -1.f;
        }
    std::vector<uint8_t> flat(size_t(rig.dw) * rig.dh, 0);
    for (int y = 3; y < rig.dh - 3; ++y)
        for (int x = 3; x < rig.dw - 3; ++x) {
            const float f0 = fwd[size_t(y) * rig.dw + x];
            bool same = true;
            for (int j = -3; j <= 3 && same; ++j)
                for (int i = -3; i <= 3 && same; ++i) same = fwd[size_t(y + j) * rig.dw + x + i] == f0;
            flat[size_t(y) * rig.dw + x] = same;
        }
    return flat;
}

// ---- Part 2 checks --------------------------------------------------------------------------------------------------

void ShaderChecks(const std::filesystem::path& native, const std::filesystem::path& layer, bool forceWarp, bool quick) {
    Gpu gpu(native, layer, forceWarp);
    std::printf("\nDevice: %s%s\n", gpu.adapter.c_str(), gpu.warpDevice ? " (WARP)" : "");
    Rig rig;
    if (quick) {
        rig.cw = 1014, rig.ch = 1001, rig.dw = 588, rig.dh = 580, rig.texW = 604, rig.texH = 592, rig.pw = rig.ph = 520, rig.compW = rig.compH = 1267;
    }
    const float pxPerTan = rig.CompPxPerTan();
    g_pxPerTan = rig.cw / (rig.focus.r - rig.focus.l);
    std::printf("Rig: focus colour %dx%d over tan [%.3f, %.3f] x [%.3f, %.3f] (%.0f px per tangent, %.1f px/deg), depth %dx%d at (%d, %d) in a %dx%d texture,\n"
                "     periphery %dx%d over +-%.2f, composition %dx%d; IPD %.0f mm, near %.2f m\n",
                rig.cw, rig.ch, rig.focus.l, rig.focus.r, rig.focus.d, rig.focus.u, pxPerTan, pxPerTan * Rad, rig.dw, rig.dh, rig.vpX, rig.vpY, rig.texW, rig.texH,
                rig.pw, rig.ph, rig.periph.r, rig.compW, rig.compH, rig.ipd * 1000.f, rig.nearM);
    dcsvr::DisparitySettings s;
    s.enabled = true;
    s.ApplyPreset(dcsvr::FarDepthPreset::Default);
    const double fieldTexelPx = double(rig.cw) / rig.dw;

    // ---- Near field, sky, gain 0: nothing may change, bit for bit ----
    std::printf("\nIdentity: the composited image with the warp must equal the shipping composition bit for bit\n");
    if (Want("identity")) {
        Scene nearScene;
        nearScene.quads.push_back(Wall(25.f, 0.35f, 0.2f, 2.f));                                     // terrain 25 m away (all ranges < 40 m)
        nearScene.quads.push_back({0.8f, -0.08f, -0.04f, -1.f, 1.f, 0.05f, 0.f, 1.f, CockpitStencil});  // canopy frame
        nearScene.quads.push_back({0.7f, -1.f, 1.f, -1.f, -0.25f, 0.2f, 0.05f, 0.03f, CockpitStencil}); // panel
        nearScene.quads.push_back({9.f, 0.5f, 3.f, -0.4f, 1.0f, 0.8f});                                   // a tanker's boom at 9 m, no stencil
        const Images im = RenderImages(gpu, nearScene, rig, 0);
        EyeSetup e;
        const EyeRun r = RunEye(gpu, nearScene, rig, e, s, im);
        Check(MaxAbsField(r.field) == 0.0 && r.on.hash == r.off.hash,
              Fmt("near field (< 40 m, cockpit stencil and a 9 m object without): field 0 everywhere, composition bit-identical (max |b| %.2g)", MaxAbsField(r.field)));
    }
    if (Want("identity")) {
        Scene sky;
        sky.quads.push_back(Wall(25000.f, 0.4f, 0.2f, 300.f));  // terrain beyond the 20 km fade, below the horizon
        sky.quads.back().y1 = 0.f;                               // above it the sky (depth 0)
        sky.quads.push_back({0.8f, 0.02f, 0.06f, -1.f, 1.f, 0.05f, 0.f, 1.f, CockpitStencil});
        const Images im = RenderImages(gpu, sky, rig, 1);
        EyeSetup e;
        e.eye = 1;
        const EyeRun r = RunEye(gpu, sky, rig, e, s, im);
        Check(MaxAbsField(r.field) == 0.0 && r.on.hash == r.off.hash, "sky (depth 0) and terrain beyond 20 km: field 0, composition bit-identical");
    }
    Scene world;  // the rich scene: textured terrain at 3 km, aircraft at 300 m and 1 km, a ship at 2 km, the canopy frame
    world.quads.push_back(Wall(3000.f, 0.4f, 0.15f, 60.f));
    world.quads.back().y1 = 3000.f * 0.15f;  // a horizon: the sky above
    world.quads.push_back(Object(0.03f, 0.02f, 300.f, 6.f, 3.f, 0.95f));
    world.quads.push_back(Object(0.10f, -0.05f, 1000.f, 5.f, 3.f, 0.95f));
    world.quads.push_back(Object(-0.08f, 0.0f, 2000.f, 9.f, 2.f, 0.1f));
    world.quads.push_back({0.8f, -0.12f, -0.09f, -1.f, 1.f, 0.05f, 0.f, 1.f, CockpitStencil});
    {
        const Images im = RenderImages(gpu, world, rig, 0);
        EyeSetup e;
        e.gain = 0.f;
        const EyeRun r = RunEye(gpu, world, rig, e, s, im);
        Check(MaxAbsField(r.field) == 0.0 && r.on.hash == r.off.hash, "gain 0 on the full scene: field 0, composition bit-identical");
        e.gain = 1.f;
        const EyeRun on = RunEye(gpu, world, rig, e, s, im, false);
        gpu.SetComposition(rig, rig.focus);
        FieldTex f = gpu.MakeField(rig.dw, rig.dh);
        gpu.DispatchField(on.k, RenderDepthTex(gpu, world, rig, e), f);
        const Image disabled = gpu.Composite(rig, true, Ps(rig, e, false), im.periphSrv.Get(), im.focusSrv.Get(), f.srv.Get());
        const Image shipping = gpu.Composite(rig, false, Ps(rig, e, false), im.periphSrv.Get(), im.focusSrv.Get(), nullptr);
        Check(disabled.hash == shipping.hash && MaxAbsField(on.field) > 0.0,
              "warp.x = 0 with a non-zero field bound: bit-identical to the shipping composition (the injection changes nothing when off)");
        for (const float shape : {2.f}) {
            EyeSetup round = e;
            round.shape = shape;
            round.smoothing = 0.1f;
            const Image a = gpu.Composite(rig, true, Ps(rig, round, false), im.periphSrv.Get(), im.focusSrv.Get(), f.srv.Get());
            const Image b = gpu.Composite(rig, false, Ps(rig, round, false), im.periphSrv.Get(), im.focusSrv.Get(), nullptr);
            Check(a.hash == b.hash, "warp.x = 0, round focus shape: bit-identical too");
        }
        Check(r.on.finite && on.field.w == rig.dw && FieldFinite(on.field), "no NaN or Inf in the field or the composition");
    }

    // ---- S7: a far aircraft gets its intended extra disparity, measured between the two eyes' images ----
    std::printf("\nS7 Far aircraft (6x3 px, over a uniform background at 3x its range, at the gaze): left/right image offset\n");
    std::printf("| Range | Natural disparity (px, analytic) | Added, measured (px) | Added, expected (px) | Error (px) | Added (arcmin) | Per eye L / R (px) |\n|---|---|---|---|---|---|---|\n");
    if (Want("s7")) {
        bool ok = true, signs = true;
        double worst = 0;
        for (float z : {300.f, 600.f, 1000.f}) {
            Scene sc;
            sc.quads.push_back(Wall(3.f * z, 0.3f));
            sc.quads.push_back(Object(0.03f, 0.02f, z, 6.f, 3.f, 0.95f));
            double pos[2][2], added[2], expected[2];
            for (int eye = 0; eye < 2; ++eye) {
                const Images im = RenderImages(gpu, sc, rig, eye);
                EyeSetup e;
                e.eye = eye;
                const EyeRun r = RunEye(gpu, sc, rig, e, s, im);
                const float ex = EyeX(rig, eye);
                const float tx = (0.03f * z - ex) / z, ty = 0.02f;
                const double cx = rig.CompX(tx), cy = rig.CompY(ty);
                const float bg = BorderMedian(r.off, int(cx) - 14, int(cy) - 14, int(cx) + 14, int(cy) + 14);
                pos[eye][0] = Measure(r.off, cx, cy, 14, bg).x;
                pos[eye][1] = Measure(r.on, cx, cy, 14, bg).x;
                added[eye] = pos[eye][1] - pos[eye][0];
                expected[eye] = Expected(rig, r.k, tx, ty, z * std::sqrt(1.f + tx * tx + ty * ty)).first;
                worst = std::max(worst, std::fabs(added[eye] - expected[eye]));
                ok &= r.on.finite;
            }
            // The natural offset (pos off) is measured too, but the 4x4 supersampling quantises edges to 1/4 px:
            // the added offset compares the same image warped and unwarped, so it is exact.
            const double natural = rig.ipd / z * pxPerTan, measuredNatural = pos[0][0] - pos[1][0];
            const double add = (pos[0][1] - pos[1][1]) - measuredNatural, exp = expected[0] - expected[1];
            const double err = add - exp;
            const double dd = dcsvr::DcsvrExtraDisparityRad(z, rig.ipd, s.z0M, s.gamma, s.fadeStartM, s.fadeEndM, s.nearGuardM);
            std::printf("| %.0f m | %.3f | %.3f | %.3f | %+.4f | %.2f' (curve %.2f') | %+.3f / %+.3f |\n", z, natural, add, exp, err,
                        ToArcmin(float(add / pxPerTan)), ToArcmin(float(dd)), added[0], added[1]);
            ok &= std::fabs(err) <= 0.03;
            signs &= added[0] > 0 && added[1] < 0 && add > 0;
        }
        Check(ok, Fmt("S7 added left/right offset within 0.03 px of the curve at 300 m, 600 m and 1 km (worst per eye %.4f px)", worst));
        Check(signs, "S7 crossed: the left eye's image moves right, the right eye's left (nearer), the cyclopean position stays");
    }

    // ---- S7 calibration on a plane: field against the CPU reference, and the fovea falloff ----
    std::printf("\nS7 / fovea: a uniform plane at 1 km, field along the row through the gaze (left eye)\n");
    if (Want("fovea")) {
        Scene sc;
        sc.quads.push_back(Wall(1000.f, 0.5f, 0.1f, 25.f));
        EyeSetup e;
        e.gx = 0.0f;
        e.gy = 0.0f;
        const auto k = MakeConstants(rig, e, s);
        FieldTex f = gpu.MakeField(rig.dw, rig.dh);
        gpu.DispatchField(k, RenderDepthTex(gpu, sc, rig, e), f);
        const Field fd = gpu.ReadField(f);
        // The field texel row through the gaze (tangent 0).
        const float ny = rig.focus.Cy();
        const int row = int((0.5f - ny * 0.5f) * rig.dh);
        std::printf("| Eccentricity | Field (px) | Reference (px) | Weight |\n|---|---|---|---|\n");
        double worst = 0, centre = 0;
        bool monotone = true;
        double previous = 1e9;
        const int c0 = int((rig.focus.Cx() * 0.5f + 0.5f) * rig.dw);
        for (int x = c0; x < rig.dw; ++x) {
            const float u = (x + 0.5f) / rig.dw, v = (row + 0.5f) / rig.dh;
            const float nx = 2.f * u - 1.f, nyy = 1.f - 2.f * v;
            const float tx = (nx - rig.focus.Cx()) / rig.focus.Sx(), ty = (nyy - rig.focus.Cy()) / rig.focus.Sy();
            float su, sv;
            dcsvr::ReferenceShiftUv(k, u, v, 1000.f * std::sqrt(1.f + tx * tx + ty * ty), false, su, sv);
            const double ref = su * rig.cw, got = FieldPx(rig, fd, x, row);
            worst = std::max(worst, std::fabs(ref - got));
            if (x == c0) centre = got;
            const float ecc = std::atan(tx) / Rad;
            // Inside 10 degrees the angle is constant and its px grow off axis (1 + t^2); the falloff must not rise.
            if (ecc >= 12.f) monotone &= got <= previous + 5e-4;  // half-float storage: 2.4e-4 px steps here
            previous = got;
            static const float marks[] = {0.f, 5.f, 10.f, 15.f, 17.5f, 20.f, 25.f, 28.f};
            for (float m : marks)
                if (std::fabs(ecc - m) < 0.5f * std::atan(1.f / (rig.dw * k.proj[0] * 0.5f)) / Rad + 1e-3f)
                    std::printf("| %.1f deg | %.4f | %.4f | %.3f |\n", ecc, got, ref, centre > 0 ? got / centre : 0.0);
        }
        const double perEye = 0.5 * dcsvr::DcsvrExtraDisparityRad(1000.f, rig.ipd, s.z0M, s.gamma, s.fadeStartM, s.fadeEndM, s.nearGuardM);
        Check(worst <= 0.005, Fmt("S7 the field equals the CPU reference along the row within %.4f px (<= 0.005; half-float storage)", worst));
        Check(Near(centre, std::tan(perEye) * rig.cw / (rig.focus.r - rig.focus.l), 0.01),
              Fmt("S7 at the gaze: %.4f px = half the curve's 1.56' (%.4f px)", centre, std::tan(perEye) * rig.cw / (rig.focus.r - rig.focus.l)));
        Check(monotone, "fovea: the shift falls monotonically from 12 degrees out");
        // Field 0 beyond 25 degrees from the gaze and at the focus edge (rho >= 0.97).
        double beyond = 0, edge = 0;
        for (int y = 0; y < rig.dh; ++y)
            for (int x = 0; x < rig.dw; ++x) {
                const float u = (x + 0.5f) / rig.dw, v = (y + 0.5f) / rig.dh, nx = 2 * u - 1, nyy = 1 - 2 * v;
                const float tx = (nx - rig.focus.Cx()) / rig.focus.Sx(), ty = (nyy - rig.focus.Cy()) / rig.focus.Sy();
                const float ecc = dcsvr::DcsvrEccentricityDeg(tx, ty, 0.f, 0.f, -1.f);
                // the field at x may hold the shift of a neighbour up to one texel away
                if (ecc > 25.5f) beyond = std::max(beyond, (double)std::fabs(fd.U(x, y)) * rig.cw);
                if (std::max(std::fabs(nx), std::fabs(nyy)) >= 0.975f) edge = std::max(edge, (double)std::fabs(fd.U(x, y)) * rig.cw);
            }
        Check(beyond == 0.0 && edge == 0.0, "fovea: field exactly 0 beyond 25 degrees from the gaze and at the focus edge");
        // The fovea follows the gaze: 8 degrees right, the peak moves there.
        EyeSetup g = e;
        g.gx = std::tan(8.f * Rad);
        gpu.DispatchField(MakeConstants(rig, g, s), RenderDepthTex(gpu, sc, rig, g), f);
        const Field fg = gpu.ReadField(f);
        const int xg = int(((rig.focus.Sx() * g.gx + rig.focus.Cx()) * 0.5f + 0.5f) * rig.dw);
        const int xl = int(((rig.focus.Sx() * std::tan(-12.f * Rad) + rig.focus.Cx()) * 0.5f + 0.5f) * rig.dw);
        Check(fg.U(xg, row) > 0.98f * fd.U(c0, row) && fg.U(xl, row) < 0.6f * fd.U(c0, row) && fd.U(xl, row) > 0.95f * fd.U(c0, row),
              "fovea: with the gaze 8 degrees right, full shift there, falling 20 degrees to its left (12 left of centre)");
        // Without a tracker the focus centre stands for the gaze.
        EyeSetup nt = e;
        nt.gazeValid = false;
        nt.gx = 0.3f;  // ignored
        gpu.DispatchField(MakeConstants(rig, nt, s), RenderDepthTex(gpu, sc, rig, nt), f);
        const Field fn = gpu.ReadField(f);
        const int xc = int(rig.dw / 2);
        const float tcx = (0.f - rig.focus.Cx()) / rig.focus.Sx();
        (void)tcx;
        Check(fn.U(xc, rig.dh / 2) > 0.98f * fd.U(c0, row), "no tracker: the focus view's centre gets the full shift");
    }

    // ---- Canopy edge: disocclusion over terrain, nothing against the sky ----
    std::printf("\nCanopy frame (0.8 m, cockpit stencil) over textured terrain at 2 km and over the sky (left eye, gaze at the frame)\n");
    if (Want("canopy")) {
        Scene sc;
        sc.quads.push_back(Wall(2000.f, 0.6f, 0.25f, 20.f));
        sc.quads.back().y1 = 0.f;  // the sky above the horizon
        const float fx0 = 0.04f * 0.8f, fx1 = 0.08f * 0.8f;
        sc.quads.push_back({0.8f, fx0, fx1, -1.f, 1.f, 0.02f, 0.f, 1.f, CockpitStencil});
        const Images im = RenderImages(gpu, sc, rig, 0);
        EyeSetup e;
        e.gx = 0.06f;
        e.gy = 0.f;
        const EyeRun r = RunEye(gpu, sc, rig, e, s, im);
        const float ex = EyeX(rig, 0);
        // Rows below the horizon (terrain) and above (sky), 2-5 degrees from it.
        int moved = 0, frameChanged = 0, worstEdge = 0, rowsChecked = 0;
        double terrainShift = 0;
        bool skyIdentical = true;
        const int left = int(rig.CompX((fx0 - ex) / 0.8f)), right = int(rig.CompX((fx1 - ex) / 0.8f));
        for (int dy = 2; dy <= 5; ++dy) {
            // Terrain rows.
            const int y = int(rig.CompY(-std::tan(dy * Rad)));
            ++rowsChecked;
            // Frame pixels: deep inside the frame identical; its edges (first/last pixel under 0.2) moved at most 1 px.
            auto edges = [&](const Image& img) {
                int a = -1, b = -1;
                for (int x = left - 20; x <= right + 20; ++x)
                    if (img.At(x, y) < 0.2f) {
                        if (a < 0) a = x;
                        b = x;
                    }
                return std::make_pair(a, b);
            };
            const auto eo = edges(r.off), en = edges(r.on);
            worstEdge = std::max({worstEdge, std::abs(eo.first - en.first), std::abs(eo.second - en.second)});
            for (int x = left + 3; x <= right - 3; ++x) frameChanged += r.off.At(x, y) != r.on.At(x, y);
            // The terrain's shift 30-60 px right of the frame (cross-correlation of the row, sub-pixel by parabola).
            double best = -1e9, bestS = 0, scores[41];
            for (int k = -20; k <= 20; ++k) {
                const double sh = k * 0.05;
                double sum = 0;
                for (int x = right + 30; x < right + 200; ++x) {
                    const double p = x - sh;
                    const int i = int(std::floor(p));
                    const double t = p - i;
                    const double v = r.off.At(i, y) * (1 - t) + r.off.At(i + 1, y) * t;
                    sum -= (v - r.on.At(x, y)) * (v - r.on.At(x, y));
                }
                scores[k + 20] = sum;
                if (sum > best) best = sum, bestS = sh;
            }
            // Sub-step: the parabola through the best score and its neighbours.
            const int kb = int(std::lround(bestS / 0.05)) + 20;
            if (kb > 0 && kb < 40) {
                const double a = scores[kb - 1], b = scores[kb], c = scores[kb + 1], den = a - 2 * b + c;
                if (den < 0) bestS += 0.05 * 0.5 * (a - c) / den;
            }
            terrainShift += bestS;
            for (int x = left; x <= right; ++x) moved += r.off.At(x, y) != r.on.At(x, y);
            // Sky rows above.
            const int ys = int(rig.CompY(std::tan(dy * Rad)));
            for (int x = left - 60; x <= right + 60; ++x) skyIdentical &= r.off.At(x, ys) == r.on.At(x, ys);
        }
        terrainShift /= rowsChecked;
        const float ttx = 0.18f, tty = -std::tan(3.5f * Rad);  // the middle of the correlation window
        const auto expect = Expected(rig, r.k, ttx, tty, 2000.f * std::sqrt(1.f + ttx * ttx + tty * tty));
        std::printf("frame edges moved at most %d px; frame interior pixels changed: %d; terrain shift right of the frame %.3f px (expected %.3f)\n",
                    worstEdge, frameChanged, terrainShift, expect.first);
        Check(frameChanged == 0 && worstEdge <= 1, "canopy over terrain: the frame's interior is untouched and its edges move at most 1 px");
        Check(Near(terrainShift, expect.first, 0.03), "canopy over terrain: the terrain beside it moves by its own shift (disocclusion filled from the terrain)");
        Check(skyIdentical, "canopy against the sky: no pixel changes (no sky stretched into the frame or frame into the sky)");
        Check(r.on.finite, "canopy: no NaN");
        // The stretch on the disoccluded side: the field ramps from 0 (frame) to the terrain's shift within 2 field texels.
        const Field& fd = r.field;
        const int fy = int((0.5f - (rig.focus.Sy() * -std::tan(3.5f * Rad) + rig.focus.Cy()) * 0.5f) * rig.dh);
        const int fxEdge = int(((rig.focus.Sx() * ((fx1 - ex) / 0.8f) + rig.focus.Cx()) * 0.5f + 0.5f) * rig.dw);
        const double full = FieldPx(rig, fd, fxEdge + 8, fy);
        int ramp = 0;
        for (int x = fxEdge - 2; x < fxEdge + 8; ++x)
            if (FieldPx(rig, fd, x, fy) > 0.02 * full && FieldPx(rig, fd, x, fy) < 0.98 * full) ++ramp;
        std::printf("field across the frame's right edge (px):");
        for (int x = fxEdge - 2; x < fxEdge + 5; ++x) std::printf(" %.3f", FieldPx(rig, fd, x, fy));
        std::printf("; ramp %d texel(s), terrain stretched by %.3f px over it\n", ramp, full);
        Check(ramp <= 2 && full <= 0.9 * fieldTexelPx, "canopy: the terrain is stretched over at most 2 field texels, by under one texel");
    }

    // ---- Clouds and HUD glyphs without depth ----
    std::printf("\nColour without depth: a cloud (4 km, alpha 0.6) and an additive HUD glyph (collimated), over terrain at 3 km and over the sky\n");
    if (Want("overlay")) {
        Scene sc;
        sc.quads.push_back(Wall(3000.f, 0.3f));
        sc.quads.back().y1 = 0.f;
        Overlay cloudLow{false, 4000.f, 0.02f * 4000.f, -0.06f * 4000.f, 25.f, 0.95f, 0.6f, false};
        Overlay cloudHigh{false, 4000.f, -0.12f * 4000.f, 0.08f * 4000.f, 25.f, 0.95f, 0.6f, false};
        Overlay hudLow{true, 0.f, 0.10f, -0.05f, 0.002f, 0.5f, 1.f, true, true};
        Overlay hudHigh{true, 0.f, -0.05f, 0.07f, 0.002f, 0.5f, 1.f, true, true};
        sc.overlays = {cloudLow, cloudHigh, hudLow, hudHigh};
        // Without the engine's pre-overlay copy (dcsvr_far_depth_allow_unmasked only), then with it (the default
        // requirement): CoverageMain finds the glyphs by the difference.
        for (int masked = 0; masked < 2; ++masked) {
            double cloud[2][2], hud[2][2], expectT[2][2];
            for (int eye = 0; eye < 2; ++eye) {
                const Images im = RenderImages(gpu, sc, rig, eye);
                EyeSetup e;
                e.eye = eye;
                e.gx = 0.f, e.gy = 0.f;
                e.coverage = masked == 1;
                const EyeRun r = RunEye(gpu, sc, rig, e, s, im);
                const float ex = EyeX(rig, eye);
                const float ct[2][2] = {{(0.02f * 4000.f - ex) / 4000.f, -0.06f}, {(-0.12f * 4000.f - ex) / 4000.f, 0.08f}};
                for (int i = 0; i < 2; ++i) cloud[eye][i] = Shift(r.off, r.on, rig.CompX(ct[i][0]), rig.CompY(ct[i][1]), 30).first;
                hud[eye][0] = Shift(r.off, r.on, rig.CompX(0.10f), rig.CompY(-0.05f), 14).first;
                hud[eye][1] = Shift(r.off, r.on, rig.CompX(-0.05f), rig.CompY(0.07f), 14).first;
                expectT[eye][0] = Expected(rig, r.k, ct[0][0], ct[0][1], 3000.f * std::sqrt(1.f + ct[0][0] * ct[0][0] + ct[0][1] * ct[0][1])).first;
                expectT[eye][1] = Expected(rig, r.k, 0.10f, -0.05f, 3000.f * std::sqrt(1.f + 0.01f + 0.0025f)).first;
            }
            std::printf("%s\n| Content | Left eye (px) | Right eye (px) | Terrain's shift L / R (px) |\n|---|---|---|---|\n",
                        masked ? "With the pre-overlay copy (CoverageMain finds the HUD glyphs):" : "Without the pre-overlay copy (allow_unmasked):");
            std::printf("| Cloud over terrain | %+.4f | %+.4f | %+.4f / %+.4f |\n", cloud[0][0], cloud[1][0], expectT[0][0], expectT[1][0]);
            std::printf("| Cloud over sky | %+.4f | %+.4f | 0 |\n", cloud[0][1], cloud[1][1]);
            std::printf("| HUD glyph over terrain | %+.4f | %+.4f | %+.4f / %+.4f |\n", hud[0][0], hud[1][0], expectT[0][1], expectT[1][1]);
            std::printf("| HUD glyph over sky | %+.4f | %+.4f | 0 |\n", hud[0][1], hud[1][1]);
            Check(Near(cloud[0][0], expectT[0][0], 0.03) && Near(cloud[1][0], expectT[1][0], 0.03) && cloud[0][1] == 0.0 && cloud[1][1] == 0.0,
                  Fmt("cloud without depth%s: moves with the terrain behind it (its range, 3 km), not at all over the sky (as specified)", masked ? " (coverage on)" : ""));
            if (!masked) {
                // The violation of the engine rule that makes coverage a requirement: collimated symbology takes the
                // disparity of whatever lies behind it, here a step of twice this at the horizon.
                Check(std::fabs(hud[0][0]) > 0.1 && Near(hud[0][0], -hud[1][0], 0.02) && hud[0][1] == 0.0 && hud[1][1] == 0.0,
                      Fmt("without coverage a HUD glyph over far terrain moves (%.3f / %.3f px) and over the sky not: why Far depth requires coverage", hud[0][0], hud[1][0]));
            } else {
                const double worst = std::max({std::fabs(hud[0][0]), std::fabs(hud[1][0]), std::fabs(hud[0][1]), std::fabs(hud[1][1])});
                // What is left is the glyph's faint glow below CoverageMain's 2-step threshold (its contribution there is
                // under 2 display steps, so its movement cannot show), against 0.28 px unmasked.
                Check(worst <= 0.005, Fmt("with coverage the HUD glyphs stay at infinity: at most %.4f px per eye over terrain and over the sky (<= 0.005)", worst));
            }
        }
    }

    // ---- The overlay coverage (CoverageMain) ----
    std::printf("\nOverlay coverage: the engine's pre-overlay copy against the submitted image (8-bit sRGB, %dx%d over a %dx%d field)\n", rig.cw, rig.ch,
                rig.dw, rig.dh);
    if (Want("coverage")) {
        const dcsvr::DisparitySettings defaults;
        // A busy grey pattern down to the darkest codes.
        std::vector<float> base(size_t(rig.cw) * rig.ch);
        for (int y = 0; y < rig.ch; ++y)
            for (int x = 0; x < rig.cw; ++x) {
                const float t = 0.5f + 0.5f * std::sin(x * 0.37f) * std::cos(y * 0.23f);
                base[size_t(y) * rig.cw + x] = t * t * t;  // 0..1 linear, many values near black
            }
        dcsvr::CoverageConstants ck;
        const bool made = dcsvr::MakeCoverageConstants(rig.dw, rig.dh, rig.cw, rig.ch, 12, 4, rig.cw, rig.ch, defaults.overlayThreshold, ck);
        dcsvr::CoverageConstants unused;
        Check(made && !dcsvr::MakeCoverageConstants(rig.dw, rig.dh, rig.cw, rig.ch, 0, 0, rig.cw + 1, rig.ch, 0.02f, unused) &&
                  !dcsvr::MakeCoverageConstants(rig.dw, rig.dh, rig.cw, rig.ch, -1, 0, rig.cw, rig.ch, 0.02f, unused) &&
                  !dcsvr::MakeCoverageConstants(100, 100, 2000, 2000, 0, 0, 2000, 2000, 0.02f, unused) &&
                  !dcsvr::MakeCoverageConstants(0, rig.dh, rig.cw, rig.ch, 0, 0, rig.cw, rig.ch, 0.02f, unused),
              "coverage constants: the copy and the subImage must be one pixel grid (same size, inside the image), at most 8 pixels per field texel");
        const auto preSrv = gpu.MakeSrgb(base, rig.cw, rig.ch);
        // 1. The same image submitted: nothing covered.
        {
            DepthTex t;
            std::vector<uint8_t> cov;
            const uint32_t changed = gpu.BuildCoverage(t, ck, preSrv.Get(), gpu.MakeSrgb(base, rig.cw, rig.ch, 12, 4, rig.cw + 20, rig.ch + 10).Get(), &cov);
            Check(changed == 0 && std::count(cov.begin(), cov.end(), 0) == ptrdiff_t(cov.size()), "coverage: an unchanged image covers nothing");
        }
        // 2. One 8-bit step of rounding on every pixel (a CopyToHMDPass through a shader): still nothing.
        {
            std::vector<float> noisy(base);
            for (size_t i = 0; i < noisy.size(); ++i) {
                const uint8_t c = SrgbByte(base[i]);
                const int n = std::clamp(int(c) + ((i * 2654435761u >> 7) % 3 == 0 ? 1 : -1), 0, 255);
                const float e = n / 255.f;
                noisy[i] = e <= 0.04045f ? e / 12.92f : std::pow((e + 0.055f) / 1.055f, 2.4f);
            }
            DepthTex t;
            const uint32_t changed = gpu.BuildCoverage(t, ck, preSrv.Get(), gpu.MakeSrgb(noisy, rig.cw, rig.ch, 12, 4, rig.cw + 20, rig.ch + 10).Get());
            Check(changed == 0, Fmt("coverage: one 8-bit step of rounding everywhere covers nothing (%u texels)", changed));
        }
        // 3. Single overlay pixels (dim and bright, at the image's corners and inside): exactly the field texels whose
        //    footprint holds them, dilated by 2 texels, nothing else.
        {
            struct Dot {
                int x, y;
                float add;
            };
            const Dot dots[] = {{rig.cw / 2, rig.ch / 3, 0.05f}, {0, 0, 0.5f}, {rig.cw - 1, rig.ch - 1, 0.5f}, {rig.cw / 4 + 10, rig.ch * 4 / 5, 0.02f}};
            std::vector<float> after(base);
            std::vector<uint8_t> expect(size_t(rig.dw) * rig.dh, 0);
            uint32_t expectChanged = 0;
            std::vector<uint8_t> raw(size_t(rig.dw) * rig.dh, 0);
            for (const Dot& d : dots) after[size_t(d.y) * rig.cw + d.x] = std::min(1.f, after[size_t(d.y) * rig.cw + d.x] + d.add);
            for (int fy = 0; fy < rig.dh; ++fy)
                for (int fx = 0; fx < rig.dw; ++fx) {
                    const int x0 = std::clamp(int(std::floor(fx * ck.field[2])), 0, rig.cw - 1);
                    const int x1 = std::clamp(int(std::floor((fx + 1) * ck.field[2])) - 1, x0, std::min(x0 + 7, rig.cw - 1));
                    const int y0 = std::clamp(int(std::floor(fy * ck.field[3])), 0, rig.ch - 1);
                    const int y1 = std::clamp(int(std::floor((fy + 1) * ck.field[3])) - 1, y0, std::min(y0 + 7, rig.ch - 1));
                    for (const Dot& d : dots)
                        if (d.x >= x0 && d.x <= x1 && d.y >= y0 && d.y <= y1 && std::abs(int(SrgbByte(after[size_t(d.y) * rig.cw + d.x])) - int(SrgbByte(base[size_t(d.y) * rig.cw + d.x]))) >= 3)
                            raw[size_t(fy) * rig.dw + fx] = 1;
                }
            for (int fy = 0; fy < rig.dh; ++fy)
                for (int fx = 0; fx < rig.dw; ++fx) {
                    expectChanged += raw[size_t(fy) * rig.dw + fx];
                    for (int oy = -2; oy <= 2; ++oy)
                        for (int ox = -2; ox <= 2; ++ox) {
                            const int x = fx + ox, y = fy + oy;
                            if (x >= 0 && y >= 0 && x < rig.dw && y < rig.dh && raw[size_t(y) * rig.dw + x]) expect[size_t(fy) * rig.dw + fx] = 255;
                        }
                }
            DepthTex t;
            std::vector<uint8_t> cov;
            const uint32_t changed = gpu.BuildCoverage(t, ck, preSrv.Get(), gpu.MakeSrgb(after, rig.cw, rig.ch, 12, 4, rig.cw + 20, rig.ch + 10).Get(), &cov);
            size_t mismatch = 0, covered = 0;
            for (size_t i = 0; i < cov.size(); ++i) mismatch += cov[i] != expect[i], covered += cov[i] != 0;
            Check(mismatch == 0 && changed == expectChanged && expectChanged >= 4,
                  Fmt("coverage: single overlay pixels (one as dim as +0.02) pin exactly their field texels dilated by 2: %zu covered, %zu wrong, %u / %u changed",
                      covered, mismatch, changed, expectChanged));
        }
        // 4. A HUD-sized change (a box of sparse strokes, 15% x 10% of the view) covers nothing outside its dilated rectangle.
        {
            std::vector<float> after(base);
            const int bx0 = rig.cw * 2 / 5, bx1 = bx0 + rig.cw * 3 / 20, by0 = rig.ch * 9 / 20, by1 = by0 + rig.ch / 10;
            for (int y = by0; y < by1; ++y)
                for (int x = bx0; x < bx1; x += 3) after[size_t(y) * rig.cw + x] = 0.8f;  // sparse vertical strokes
            DepthTex t;
            std::vector<uint8_t> cov;
            gpu.BuildCoverage(t, ck, preSrv.Get(), gpu.MakeSrgb(after, rig.cw, rig.ch, 12, 4, rig.cw + 20, rig.ch + 10).Get(), &cov);
            // A pixel lies in the footprint of its own texel and possibly a neighbour (the footprints overlap by up to
            // one pixel), plus the dilation of 2: 3 texels around the box bound everything that may be covered.
            const int fx0 = int(bx0 / ck.field[2]) - 3, fx1 = int((bx1 - 1) / ck.field[2]) + 3, fy0 = int(by0 / ck.field[3]) - 3,
                      fy1 = int((by1 - 1) / ck.field[3]) + 3;
            size_t inside = 0, outside = 0, holes = 0;
            for (int fy = 0; fy < rig.dh; ++fy)
                for (int fx = 0; fx < rig.dw; ++fx) {
                    const bool in = fx >= fx0 && fx <= fx1 && fy >= fy0 && fy <= fy1;
                    const bool c = cov[size_t(fy) * rig.dw + fx] != 0;
                    inside += in && c, outside += !in && c;
                    holes += fx >= fx0 + 3 && fx <= fx1 - 3 && fy >= fy0 + 3 && fy <= fy1 - 3 && !c;
                }
            Check(outside == 0 && holes == 0 && inside > 0,
                  Fmt("coverage: a HUD box of sparse strokes is covered without holes (%zu texels) and nothing outside its dilated rectangle", inside));
        }
        // 5. The engine's pair (PRE_POST_OVERLAY): before at (0, 0) and after at (w, 0) of one 2w-wide texture, compared
        //    through one view: the same mask as against a separate submitted image.
        {
            std::vector<float> after(base);
            for (int y = rig.ch / 3; y < rig.ch / 3 + 40; ++y)
                for (int x = rig.cw / 2; x < rig.cw / 2 + 60; x += 2) after[size_t(y) * rig.cw + x] = 0.9f;
            std::vector<float> pair(size_t(rig.cw) * 2 * rig.ch);
            for (int y = 0; y < rig.ch; ++y)
                for (int x = 0; x < rig.cw; ++x) {
                    pair[size_t(y) * rig.cw * 2 + x] = base[size_t(y) * rig.cw + x];
                    pair[size_t(y) * rig.cw * 2 + rig.cw + x] = after[size_t(y) * rig.cw + x];
                }
            const auto pairSrv = gpu.MakeSrgb(pair, rig.cw * 2, rig.ch);
            dcsvr::CoverageConstants pk;
            const bool pairMade = dcsvr::MakeCoverageConstants(rig.dw, rig.dh, rig.cw, rig.ch, rig.cw, 0, rig.cw, rig.ch, defaults.overlayThreshold, pk);
            DepthTex tp, ts;
            std::vector<uint8_t> covPair, covSeparate;
            const uint32_t nPair = gpu.BuildCoverage(tp, pk, pairSrv.Get(), pairSrv.Get(), &covPair);
            const uint32_t nSeparate =
                gpu.BuildCoverage(ts, ck, preSrv.Get(), gpu.MakeSrgb(after, rig.cw, rig.ch, 12, 4, rig.cw + 20, rig.ch + 10).Get(), &covSeparate);
            Check(pairMade && nPair > 0 && nPair == nSeparate && covPair == covSeparate,
                  Fmt("coverage: the engine's before/after pair in one texture gives the same mask as the submitted image (%u texels)", nPair));
        }
        // 6. An HDR pair (the scene before the tonemap, where DCS draws HUD and MFD after DLSS): linear values far above 1
        //    (a sky at 6), strokes added on the bright sky and on dark ground, compared relatively; the sRGB path would
        //    clip the sky and miss them.
        {
            const int w = rig.cw, h = rig.ch;
            std::vector<float> pairHdr(size_t(w) * 2 * h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const float v = y < h / 2 ? 6.f + 0.5f * std::sin(x * 0.05f) : 0.02f + 0.01f * std::cos(y * 0.1f);
                    pairHdr[size_t(y) * w * 2 + x] = pairHdr[size_t(y) * w * 2 + w + x] = v;
                }
            const int sx = w / 2, skyY = h / 4, groundY = h * 3 / 4;
            pairHdr[size_t(skyY) * w * 2 + w + sx] += 0.3f;      // +5 % on the sky
            pairHdr[size_t(groundY) * w * 2 + w + sx] += 0.005f;  // +20 % on the ground
            const auto hdrSrv = gpu.MakeColour(pairHdr, w * 2, h);
            dcsvr::CoverageConstants hk;
            dcsvr::MakeCoverageConstants(rig.dw, rig.dh, w, h, w, 0, w, h, defaults.overlayThreshold, hk);
            DepthTex tl;
            const uint32_t sRgbPath = gpu.BuildCoverage(tl, hk, hdrSrv.Get(), hdrSrv.Get());
            dcsvr::SetCoverageHdr(hk);
            DepthTex th;
            std::vector<uint8_t> cov;
            const uint32_t hdrPath = gpu.BuildCoverage(th, hk, hdrSrv.Get(), hdrSrv.Get(), &cov);
            const auto at = [&](int px, int py) { return cov[size_t(int(py / hk.field[3])) * rig.dw + int(px / hk.field[2])] != 0; };
            Check(dcsvr::CoverageFormatIsHdr(10) && dcsvr::CoverageFormatIsHdr(26) && !dcsvr::CoverageFormatIsHdr(28) && sRgbPath == 1 &&
                      hdrPath == 2 && at(sx, skyY) && at(sx, groundY),
                  Fmt("coverage: an HDR pair is compared relatively: strokes on a sky at 6 and on dark ground both found (%u texels; the sRGB path %u)",
                      hdrPath, sRgbPath));
        }
    }

    // ---- Thin far aircraft and the footprint ----
    std::printf("\nS3 thin aircraft at 3 km over ground at 10 km: share of its own shift (field at its centre / the reference)\n");
    if (Want("s3")) {
        std::printf("| Width | 3x3 footprint | 2x2 footprint |\n|---|---|---|\n");
        bool ok = true;
        for (float wpx : {1.0f, 1.72f, 2.f, 4.f}) {
            Scene sc;
            sc.quads.push_back(Wall(10000.f, 0.3f));
            sc.quads.push_back(Object(0.013f, 0.007f, 3000.f, wpx, 1.72f, 0.9f));
            double share[2];
            for (int fp = 0; fp < 2; ++fp) {
                EyeSetup e;
                e.footprint2 = fp == 1;
                const auto k = MakeConstants(rig, e, s);
                FieldTex f = gpu.MakeField(rig.dw, rig.dh);
                gpu.DispatchField(k, RenderDepthTex(gpu, sc, rig, e), f, e.footprint2);
                const Field fd = gpu.ReadField(f);
                const float tx = (0.013f * 3000.f - EyeX(rig, 0)) / 3000.f, ty = 0.007f;
                const int x = int(((rig.focus.Sx() * tx + rig.focus.Cx()) * 0.5f + 0.5f) * rig.dw);
                const int y = int((0.5f - (rig.focus.Sy() * ty + rig.focus.Cy()) * 0.5f) * rig.dh);
                // Best over the texels the aircraft's colour covers (+-1 texel).
                double best = 0;
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i) best = std::max(best, (double)fd.U(x + i, y + j));
                const double ref = Expected(rig, k, tx, ty, 3000.f).first / rig.CompPxPerTan() * rig.cw / (rig.focus.r - rig.focus.l);
                share[fp] = best * rig.cw / ref;
            }
            std::printf("| %.2f px | %.0f%% | %.0f%% |\n", wpx, share[0] * 100, share[1] * 100);
            if (wpx >= 1.72f) ok &= share[0] >= 0.8;
        }
        Check(ok, "S3 an aircraft of one depth texel (1.72 px) or more keeps >= 80% of its own shift (3x3 nearest footprint)");
    }

    // ---- Jitter stability ----
    std::printf("\nS9 Jitter (Halton 2,3 over 16 frames, internal px): change of the field and of the aircraft's position\n");
    if (Want("s9")) {
        Scene sc = world;
        const Images im = RenderImages(gpu, sc, rig, 0);
        gpu.SetComposition(rig, rig.focus);
        EyeSetup base;
        const EyeRun ref = RunEye(gpu, sc, rig, base, s, im);
        // Texels whose whole 7x7 depth neighbourhood (unjittered) is one surface: "away from edges" (the footprint's
        // dilation, the backward solve and the jitter each reach one texel or less).
        std::vector<float> fwd(size_t(rig.dw) * rig.dh);
        for (int y = 0; y < rig.dh; ++y)
            for (int x = 0; x < rig.dw; ++x) {
                const float nx = (x + 0.5f) / rig.dw * 2 - 1, ny = 1 - (y + 0.5f) / rig.dh * 2;
                fwd[size_t(y) * rig.dw + x] = Cast(sc, EyeX(rig, 0), (nx - rig.focus.Cx()) / rig.focus.Sx(), (ny - rig.focus.Cy()) / rig.focus.Sy()).fwd;
            }
        std::vector<uint8_t> flat(size_t(rig.dw) * rig.dh, 0);
        for (int y = 3; y < rig.dh - 3; ++y)
            for (int x = 3; x < rig.dw - 3; ++x) {
                const float f0 = fwd[size_t(y) * rig.dw + x];
                bool same = true;
                for (int j = -3; j <= 3 && same; ++j)
                    for (int i = -3; i <= 3 && same; ++i) same = fwd[size_t(y + j) * rig.dw + x + i] == f0;
                flat[size_t(y) * rig.dw + x] = same;
            }
        const float atx = (0.03f * 300.f - EyeX(rig, 0)) / 300.f;
        const double acx = rig.CompX(atx), acy = rig.CompY(0.02f);
        const float bg = BorderMedian(ref.off, int(acx) - 14, int(acy) - 14, int(acx) + 14, int(acy) + 14);
        const double refPos = Measure(ref.on, acx, acy, 14, bg).x;
        std::printf("| Case | Field change, flat (px) | Field change, anywhere (px) | Aircraft position spread (px) | Frame pixels changed |\n|---|---|---|---|---|\n");
        bool ok = true;
        // Cases 0-2 with the default 3x3 footprint, 3-4 with the 2x2 one (reported, not checked).
        for (int c = 0; c < 5; ++c) {
            double flatMax = 0, anyMax = 0, lo = 1e9, hi = -1e9;
            int frameChanged = 0;
            const bool inProj = c != 1 && c != 4;
            // The layer's estimator, warmed up on the 32 frames before (DLSS's sequence is longer than the window).
            dcsvr::JitterCentreEstimator est;
            const auto projOf = [&](int i) {
                float cdx, cdy;
                dcsvr::JitterNdc(Halton(i, 2) - 0.5f, Halton(i, 3) - 0.5f, float(rig.dw), float(rig.dh), cdx, cdy);
                const auto m = dcsvr::ProjectionParams::Make(rig.focus.l, rig.focus.r, rig.focus.u, rig.focus.d, rig.nearM, inProj ? cdx : 0.f, inProj ? cdy : 0.f);
                return dcsvr::ProjectionParams::Parse(m.data());
            };
            const auto fovRef = dcsvr::JitterCentreEstimator::Reference::FromTangents(rig.focus.l, rig.focus.r, rig.focus.u, rig.focus.d);
            for (int i = 100; i < 132; ++i) est.Add(projOf(i), fovRef, 2.f / rig.dw, 2.f / rig.dh);
            for (int i = 1; i <= 16; ++i) {
                EyeSetup e;
                e.jitterX = Halton(i, 2) - 0.5f;
                e.jitterY = Halton(i, 3) - 0.5f;
                e.jitterInProj = inProj;
                e.jitterKnown = c == 2;
                e.footprint2 = c >= 3;
                est.Add(projOf(i), fovRef, 2.f / rig.dw, 2.f / rig.dh);
                if (c != 2) {
                    e.c0Override = true;
                    est.Centre(e.c0x, e.c0y);
                }
                const EyeRun r = RunEye(gpu, sc, rig, e, s, im);
                for (size_t p = 0; p < r.field.u.size(); ++p) {
                    const double d = std::fabs(r.field.u[p] - ref.field.u[p]) * rig.cw;
                    anyMax = std::max(anyMax, d);
                    if (flat[p] && d > flatMax) {
                        flatMax = d;
                        if (GetEnvironmentVariableA("DW_DEBUG", nullptr, 0)) std::printf("     flat max %.4f at %zu,%zu frame %d ref %.4f got %.4f\n", d, p % rig.dw, p / rig.dw, i, ref.field.u[p] * rig.cw, r.field.u[p] * rig.cw);
                    }
                }
                const double pos = Measure(r.on, acx, acy, 14, bg).x;
                lo = std::min(lo, pos), hi = std::max(hi, pos);
                // The frame (deep inside: value < 0.05 off) never changes.
                for (int y = 200; y < rig.compH - 200; y += 7)
                    for (int x = 0; x < rig.compW; ++x)
                        if (ref.off.At(x, y) < 0.05f) frameChanged += r.on.At(x, y) != ref.off.At(x, y);
            }
            const char* names[] = {"jitter in gProj (c0 = QV's FOV + mean residual)", "jitter not in gProj", "jitterPx known", "2x2 footprint, jitter in gProj",
                                   "2x2 footprint, jitter not in gProj"};
            std::printf("| %s | %.4f | %.3f | %.4f (mean offset %+.4f) | %d |\n", names[c], flatMax, anyMax, hi - lo, 0.5 * (hi + lo) - refPos, frameChanged);
            if (c < 3) ok &= flatMax <= 0.05 && hi - lo <= 0.05 && frameChanged == 0;
        }
        Check(ok, "S9 all three jitter cases: field change <= 0.05 px away from edges, the aircraft's position within 0.05 px, the frame untouched");
    }

    // ---- Jitter with the focus frustum following the gaze ----
    std::printf("\nS9 moving focus frustum (gaze pursuit at 10 deg/s, a 4 degree saccade half way; Halton jitter in gProj): field against the\n"
                "unjittered field of the same frustum, the layer's centre from Quad Views' FOV for the frame\n");
    if (Want("s9move")) {
        const Scene sc = world;
        const int frames = quick ? 6 : 10;
        dcsvr::JitterCentreEstimator est;
        double meanC[32]{};
        std::vector<float> centre;
        float offset = 0.f;
        for (int i = 0; i < 32 + frames; ++i) {
            if (i == 32 + frames / 2) offset += std::tan(4.f * Rad);
            centre.push_back(offset += std::tan(10.f / 45.f * Rad));
        }
        double worstFlat = 0, worstAny = 0, worstMeanFlat = 0;
        for (int i = 0; i < 32 + frames; ++i) {
            Rig r2 = rig;
            const float dx = centre[i] - centre[32];  // the measured frames start at the default frustum
            r2.focus.l += dx, r2.focus.r += dx;
            EyeSetup e;
            e.jitterX = Halton(i + 1, 2) - 0.5f;
            e.jitterY = Halton(i + 1, 3) - 0.5f;
            e.gx += dx;
            float cdx, cdy;
            dcsvr::JitterNdc(e.jitterX, e.jitterY, float(r2.dw), float(r2.dh), cdx, cdy);
            const auto m = dcsvr::ProjectionParams::Make(r2.focus.l, r2.focus.r, r2.focus.u, r2.focus.d, r2.nearM, cdx, cdy);
            const auto p = dcsvr::ProjectionParams::Parse(m.data());
            est.Add(p, dcsvr::JitterCentreEstimator::Reference::FromTangents(r2.focus.l, r2.focus.r, r2.focus.u, r2.focus.d), 2.f / r2.dw, 2.f / r2.dh);
            meanC[i % 32] = p.cx;
            if (i < 32) continue;
            e.c0Override = true;
            est.Centre(e.c0x, e.c0y);
            FieldTex f = gpu.MakeField(r2.dw, r2.dh);
            const DepthTex depth = RenderDepthTex(gpu, sc, r2, e);
            gpu.DispatchField(MakeConstants(r2, e, s), depth, f);
            const Field got = gpu.ReadField(f);
            EyeSetup e0 = e;
            e0.jitterX = e0.jitterY = 0.f;
            e0.c0Override = false;
            gpu.DispatchField(MakeConstants(r2, e0, s), RenderDepthTex(gpu, sc, r2, e0), f);
            const Field ref = gpu.ReadField(f);
            // Negative control: the centre as a plain 32-frame mean of gProj's (the estimator before the fix).
            EyeSetup en = e;
            en.c0x = float(std::accumulate(meanC, meanC + 32, 0.0) / 32.0);
            en.c0y = p.cy - cdy;
            gpu.DispatchField(MakeConstants(r2, en, s), depth, f);
            const Field bad = gpu.ReadField(f);
            const std::vector<uint8_t> flat = FlatMask(sc, r2, 0);
            for (size_t q = 0; q < got.u.size(); ++q) {
                const double d = std::fabs(got.u[q] - ref.u[q]) * r2.cw, dn = std::fabs(bad.u[q] - ref.u[q]) * r2.cw;
                worstAny = std::max(worstAny, d);
                if (flat[q]) worstFlat = std::max(worstFlat, d), worstMeanFlat = std::max(worstMeanFlat, dn);
            }
        }
        std::printf("field change away from edges %.4f px (anywhere %.3f px); with a plain 32-frame mean as the centre %.3f px away from edges\n", worstFlat,
                    worstAny, worstMeanFlat);
        Check(worstFlat <= 0.05, "S9 moving frustum: the field matches the unjittered one of the same frustum within 0.05 px away from edges");
        Check(worstMeanFlat > 0.1, Fmt("S9 negative control: a plain 32-frame mean of gProj's centre misplaces the field by %.2f px away from edges", worstMeanFlat));
    }

    // ---- The backward solve where the shift steps from 0 to the clamp ----
    std::printf("\nBackward solve at a silhouette at the clamp (a 40x20 px aircraft at 200 m against the sky): against a 64-step solve, field texels\n");
    if (Want("solve")) {
        Scene sc;
        sc.quads.push_back(Wall(2000.f, 0.4f, 0.1f, 30.f));
        sc.quads.back().y1 = -0.05f * 2000.f;  // the horizon well below the aircraft
        sc.quads.push_back(Object(0.03f, 0.02f, 200.f, 40.f, 20.f, 0.9f));
        EyeSetup e;
        dcsvr::DisparitySettings dbg = s;
        dbg.debug = 3;  // x4: clamped at 0.6 texel
        dcsvr::DisparitySettings extreme = s;  // a flat 6.4 mrad target (gamma 0, Z0 10 m), 12' allowed: the 0.9 clamp
        extreme.gamma = 0.f, extreme.z0M = 10.f, extreme.nearGuardM = 5.f, extreme.maxArcmin = 12.f;
        bool ok = true;
        std::printf("| Setting | Clamp | Max shift | Default solve vs 64 steps | 3 steps vs 64 steps |\n|---|---|---|---|---|\n");
        for (int c = 0; c < 2; ++c) {
            const dcsvr::DisparitySettings& set = c == 0 ? dbg : extreme;
            const auto k = MakeConstants(rig, e, set);
            const DepthTex depth = RenderDepthTex(gpu, sc, rig, e);
            FieldTex f = gpu.MakeField(rig.dw, rig.dh);
            gpu.DispatchField(k, depth, f);
            const Field def = gpu.ReadField(f);
            gpu.DispatchField(k, depth, f, false, gpu.field3itCs.Get());
            const Field three = gpu.ReadField(f);
            gpu.DispatchField(k, depth, f, false, gpu.field64itCs.Get());
            const Field ref = gpu.ReadField(f);
            double dDef = 0, dThree = 0, maxB = 0;
            for (size_t q = 0; q < def.u.size(); ++q) {
                dDef = std::max(dDef, (double)std::fabs(def.u[q] - ref.u[q]) * rig.dw);
                dThree = std::max(dThree, (double)std::fabs(three.u[q] - ref.u[q]) * rig.dw);
                maxB = std::max(maxB, (double)std::fabs(ref.u[q]) * rig.dw);
            }
            std::printf("| %s | %.1f | %.3f | %.4f | %.4f |\n", c == 0 ? "debug x4" : "extreme curve", k.fade[3], maxB, dDef, dThree);
            if (c == 0) ok &= k.fade[3] <= 0.6f && maxB <= 0.6 + 0.002 && maxB > 0.5 && dDef <= 0.01;
            else ok &= maxB > 0.85 && dDef <= 0.17 && dThree > 0.2;  // the a-priori bound at L = 0.9: 0.9^17 = 0.17 texel
        }
        Check(ok, "solve: debug x4 clamps at 0.6 texel and converges within 0.01 texel; at the 0.9 clamp within the 0.17 texel bound of 16 steps, where 3 steps (before) were 0.2+ short");
    }

    // ---- A frame without depth, bridged by the last frame's field ----
    std::printf("\nA frame without depth bridged by the held field: the head turned 1 degree and the focus frustum moved 2 degrees since it was built\n");
    if (Want("reuse")) {
        // Over a uniform background, so that a misplaced field shows only where it matters: the aircraft and the frame.
        Scene sc;
        sc.quads.push_back(Wall(3000.f, 0.3f));
        sc.quads.push_back(Object(0.03f, 0.02f, 300.f, 6.f, 3.f, 0.95f));
        const float fx0 = -0.12f, fx1 = -0.09f;  // the canopy frame at 0.8 m (cockpit stencil)
        sc.quads.push_back({0.8f, fx0, fx1, -1.f, 1.f, 0.05f, 0.f, 1.f, CockpitStencil});
        Rig a = rig, b = rig;
        b.orient = Yaw(1.f);
        const float move = std::tan(2.f * Rad);
        b.focus.l += move, b.focus.r += move;
        EyeSetup ea, eb;
        eb.gx = ea.gx + move;
        FieldTex fa = gpu.MakeField(rig.dw, rig.dh), fb = gpu.MakeField(rig.dw, rig.dh);
        gpu.DispatchField(MakeConstants(a, ea, s), RenderDepthTex(gpu, sc, a, ea), fa);
        gpu.DispatchField(MakeConstants(b, eb, s), RenderDepthTex(gpu, sc, b, eb), fb);
        const Images im = RenderImages(gpu, sc, b, 0);
        gpu.SetComposition(b, b.focus);
        const Image off = gpu.Composite(b, false, Ps(b, eb, false), im.periphSrv.Get(), im.focusSrv.Get(), nullptr);
        const Image fresh = gpu.Composite(b, true, Ps(b, eb, true), im.periphSrv.Get(), im.focusSrv.Get(), fb.srv.Get());
        PsConstants pr = Ps(b, eb, true);
        const bool accepted = dcsvr::ReuseHomography(a.orient, {a.focus.l, a.focus.r, a.focus.u, a.focus.d}, b.orient, {b.focus.l, b.focus.r, b.focus.u, b.focus.d}, pr.reuse);
        pr.warp[3] = 1.f;
        const Image reused = gpu.Composite(b, true, pr, im.periphSrv.Get(), im.focusSrv.Get(), fa.srv.Get());
        PsConstants pn = Ps(b, eb, true);  // negative control: the held field read where it lies (no reprojection)
        pn.warp[3] = 1.f;
        const float identity[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 0}};
        std::memcpy(pn.reuse, identity, sizeof(identity));
        const Image naive = gpu.Composite(b, true, pn, im.periphSrv.Get(), im.focusSrv.Get(), fa.srv.Get());
        // World tangents seen from B's eye, in composition px.
        const auto inB = [&](float wx, float wy, double& px, double& py) {
            const float wv[3] = {wx, wy, -1.f};
            float ev[3];
            dcsvr::Rotate(dcsvr::Conjugate(b.orient), wv, ev);
            px = b.CompX(ev[0] / -ev[2]), py = b.CompY(ev[1] / -ev[2]);
        };
        double cx, cy;
        inB((0.03f * 300.f - EyeX(b, 0)) / 300.f, 0.02f, cx, cy);
        const double shFresh = Shift(off, fresh, cx, cy, 14).first, shReused = Shift(off, reused, cx, cy, 14).first, shNaive = Shift(off, naive, cx, cy, 14).first;
        // Around the frame's edges (pinned, the terrain beside it moving): pixels off the fresh field's image.
        int movedReused = 0, movedNaive = 0, changedFresh = 0;
        for (const float ty : {-0.1f, 0.f, 0.1f}) {
            double x0, x1, y;
            inB((fx0 - EyeX(b, 0)) / 0.8f, ty, x0, y);
            inB((fx1 - EyeX(b, 0)) / 0.8f, ty, x1, y);
            for (int x = int(x0) - 4; x <= int(x1) + 4; ++x) {
                const int yy = int(y);
                changedFresh += fresh.At(x, yy) != off.At(x, yy);
                movedReused += std::fabs(reused.At(x, yy) - fresh.At(x, yy)) > 0.0025f;
                movedNaive += std::fabs(naive.At(x, yy) - fresh.At(x, yy)) > 0.0025f;
            }
        }
        std::printf("aircraft shift: fresh field %.4f px, held field reprojected %.4f px, held field not reprojected %.4f px (unwarped: 0)\n", shFresh, shReused, shNaive);
        std::printf("canopy frame: pixels around its edges the fresh field changes %d; off the fresh field's image by > 0.0025: reprojected %d, not reprojected %d\n",
                    changedFresh, movedReused, movedNaive);
        Check(accepted && shFresh > 0.4 * g_pxPerTan / 1690.f && std::fabs(shReused - shFresh) <= 0.03,
              Fmt("reuse: the held field, reprojected through the head turn and the frustum move, moves the aircraft as the fresh field does (%.4f / %.4f px)", shReused, shFresh));
        Check(movedReused == 0, "reuse: around the pinned canopy frame the reprojected field gives the fresh field's image");
        Check(std::fabs(shNaive - shFresh) > 0.1 && movedNaive > 0,
              Fmt("reuse negative control: the held field read without reprojection moves the aircraft by the terrain's shift (%.3f px) and the frame's edges", shNaive));
        // Too large a change is not bridged.
        float h[3][4];
        Fov wide = b.focus;
        wide.r += 0.1f;
        Check(!dcsvr::ReuseHomography(a.orient, {a.focus.l, a.focus.r, a.focus.u, a.focus.d}, Yaw(40.f), {b.focus.l, b.focus.r, b.focus.u, b.focus.d}, h) &&
                  !dcsvr::ReuseHomography(a.orient, {a.focus.l, a.focus.r, a.focus.u, a.focus.d}, b.orient, {wide.l, wide.r, wide.u, wide.d}, h),
              "reuse: a 40 degree turn (the focus centre leaves the held view) or another focus span is not bridged");
    }

    // ---- Seam: the focus and the periphery of an eye move alike in the composition's blend band ----
    std::printf("\nS8 Seam: aircraft in the blend band (alpha < 1), exaggerated x4; focus-only and periphery-only position change\n");
    if (Want("s8")) {
        bool ok = true;
        std::printf("| Shape | Smoothing | Alpha at the aircraft | Focus layer shift (px) | Periphery layer shift (px) | Difference |\n|---|---|---|---|---|---|\n");
        struct SeamCase {
            float shape, smoothing, nx;
        };
        for (const SeamCase sc0 : {SeamCase{0.f, 0.2f, 0.7f}, SeamCase{0.f, 0.3f, 0.6f}, SeamCase{2.f, 0.3f, 0.7f}, SeamCase{0.f, 0.05f, 0.93f}}) {
            const float tx = (sc0.nx - rig.focus.Cx()) / rig.focus.Sx(), ty = (0.f - rig.focus.Cy()) / rig.focus.Sy();
            Scene sc;
            sc.quads.push_back(Wall(1500.f, 0.3f));
            const float z = 500.f;
            sc.quads.push_back(Object((tx * z + EyeX(rig, 0)) / z, ty, z, 12.f, 8.f, 0.95f));
            const Images im = RenderImages(gpu, sc, rig, 0);
            dcsvr::DisparitySettings x4 = s;
            x4.debug = 3;
            EyeSetup e;
            e.gx = tx, e.gy = ty;
            e.shape = sc0.shape;
            e.smoothing = sc0.smoothing;
            const auto k = MakeConstants(rig, e, x4);
            FieldTex f = gpu.MakeField(rig.dw, rig.dh);
            gpu.DispatchField(k, RenderDepthTex(gpu, sc, rig, e), f);
            gpu.SetComposition(rig, rig.focus);
            // A white focus image shows the composition's alpha in the debug focus view.
            const auto white = gpu.MakeColour(std::vector<float>(size_t(rig.cw) * rig.ch, 1.f), rig.cw, rig.ch);
            const Image alpha = gpu.Composite(rig, true, Ps(rig, e, false, true), im.periphSrv.Get(), white.Get(), f.srv.Get());
            double shift[2];
            double alphaAt = 0;
            for (int on = 0; on < 2; ++on) {
                const Image A = gpu.Composite(rig, true, Ps(rig, e, on == 1), im.periphSrv.Get(), im.focusSrv.Get(), f.srv.Get());
                const Image D = gpu.Composite(rig, true, Ps(rig, e, on == 1, true), im.periphSrv.Get(), im.focusSrv.Get(), f.srv.Get());
                const Image W = gpu.Composite(rig, true, Ps(rig, e, on == 1, true), im.periphSrv.Get(), white.Get(), f.srv.Get());
                // Split the blend: focus = D / a, periphery = (A - D) / (1 - a).
                Image foc = D, per = D;
                for (size_t p = 0; p < A.r.size(); ++p) {
                    const float a = W.r[p];
                    foc.r[p] = a > 0.f ? D.r[p] / a : 0.f;
                    per.r[p] = a < 1.f ? (A.r[p] - D.r[p]) / (1.f - a) : 0.f;
                }
                const double cx = rig.CompX(tx), cy = rig.CompY(ty);
                alphaAt = alpha.At(int(cx), int(cy));
                static Image focOff, perOff;
                if (on == 0) {
                    focOff = foc, perOff = per;
                } else {
                    // A window inside the blend band (the split is undefined where alpha is 1).
                    const int half = int(22.f * g_pxPerTan / 1690.f);
                    const double fs = Shift(focOff, foc, cx, cy, half).first, ps = Shift(perOff, per, cx, cy, half).first;
                    shift[0] = fs, shift[1] = ps;
                }
            }
            std::printf("| %s | %.2f | %.3f | %+.4f | %+.4f | %.4f |\n", sc0.shape >= 1 ? "round" : "rectangle", sc0.smoothing, alphaAt, shift[0], shift[1], std::fabs(shift[0] - shift[1]));
            // Both layers read the same b at every pixel; what differs is how the coarser periphery (2.4x, 4.9x with
            // --quick) averages a field that falls steeply in the edge window (last case) under the centroid.
            ok &= alphaAt < 0.99 && alphaAt > 0.01 && std::fabs(shift[0] - shift[1]) <= (quick ? 0.08 : 0.05) && shift[0] > 0.2;
        }
        Check(ok, Fmt("S8 inside the blend band both layers of the eye move together (within %.2f px): no double image, no seam", quick ? 0.08 : 0.05));
    }

    // ---- Garbage depth, extreme settings: finite, clamped, no fold-over ----
    std::printf("\nRobustness: garbage depth and an extreme curve (about x40 the default's angle)\n");
    if (Want("robust")) {
        Scene sc = world;
        EyeSetup e;
        auto tamper = [&](int i, int j, float& d) {
            if (i > 500 && i < 540) d = NAN;
            else if (i > 560 && i < 580) d = INFINITY;
            else if (j > 300 && j < 320) d = -0.3f;
            else if (j > 340 && j < 350) d = 7.f;
        };
        const DepthTex bad = RenderDepthTex(gpu, sc, rig, e, tamper);
        dcsvr::DisparitySettings x = s;
        x.gamma = 0.f;  // a flat 64 mrad target (Z0 1 m), 300' allowed: about x40 the default's angle
        x.z0M = 1.f;
        x.maxArcmin = 300.f;
        const Images im = RenderImages(gpu, sc, rig, 0);
        const EyeRun r = RunEye(gpu, sc, rig, e, x, im, true, &bad);
        double maxTex = 0;
        bool monotone = true;
        for (int y = 0; y < rig.dh; ++y) {
            double previous = -1e9;
            for (int xx = 0; xx < rig.dw; ++xx) {
                const double b = r.field.u[size_t(y) * rig.dw + xx] * rig.dw, bv = r.field.v[size_t(y) * rig.dw + xx] * rig.dh;
                maxTex = std::max(maxTex, std::sqrt(b * b + bv * bv));
                const double source = xx + 0.5 - b;  // where the output texel reads from
                monotone &= source > previous;
                previous = source;
            }
        }
        std::printf("max |b| %.3f field texels; source positions strictly increasing along every row: %s\n", maxTex, monotone ? "yes" : "no");
        Check(FieldFinite(r.field) && r.on.finite, "NaN, Inf, negative and > 1 depth: the field and the composition stay finite");
        Check(maxTex <= 0.9 + 0.002 && maxTex > 0.85, "the 0.9 field texel clamp is reached and holds (half-float rounding aside)");
        Check(monotone, "no fold-over: the backward map stays monotonic along every row (no holes, no duplicated content)");
    }

    // ---- Formats, viewport depth range ----
    if (Want("formats")) {
        Scene sc = world;
        EyeSetup e;
        const auto k = MakeConstants(rig, e, s);
        FieldTex f = gpu.MakeField(rig.dw, rig.dh);
        gpu.DispatchField(k, RenderDepthTex(gpu, sc, rig, e), f);
        const Field a = gpu.ReadField(f);
        double worst24 = 0, worst32 = 0, worstRange = 0;
        {
            EyeSetup e24 = e;
            e24.format = DepthFormat::D24S8;
            gpu.DispatchField(MakeConstants(rig, e24, s), RenderDepthTex(gpu, sc, rig, e24), f);
            const Field b = gpu.ReadField(f);
            for (size_t p = 0; p < a.u.size(); ++p) worst24 = std::max(worst24, (double)std::fabs(a.u[p] - b.u[p]) * rig.cw);
        }
        {
            EyeSetup e32 = e;
            e32.format = DepthFormat::D32;
            gpu.DispatchField(MakeConstants(rig, e32, s), RenderDepthTex(gpu, sc, rig, e32), f);
            const Field b = gpu.ReadField(f);
            // Without stencil the cockpit is pinned by its range (< 40 m) alone: the same field.
            for (size_t p = 0; p < a.u.size(); ++p) worst32 = std::max(worst32, (double)std::fabs(a.u[p] - b.u[p]) * rig.cw);
        }
        {
            EyeSetup er = e;
            er.minDepth = 0.1f, er.maxDepth = 0.9f;
            gpu.DispatchField(MakeConstants(rig, er, s), RenderDepthTex(gpu, sc, rig, er), f);
            const Field b = gpu.ReadField(f);
            for (size_t p = 0; p < a.u.size(); ++p) worstRange = std::max(worstRange, (double)std::fabs(a.u[p] - b.u[p]) * rig.cw);
        }
        Check(worst24 <= 0.02 && worst32 <= 1e-6 && worstRange <= 0.02,
              Fmt("formats: D24S8 within %.4f px, R32 (no stencil) %.1g px, viewport depth range 0.1-0.9 within %.4f px of D32S8 0-1", worst24, worst32, worstRange));
    }

    // ---- Compute fallback ----
    std::printf("\nCompute fallback (WarpMain): the focus image warped alone, its field 0 wherever the composition blends\n");
    if (Want("fallback")) {
        Scene sc;
        sc.quads.push_back(Wall(3000.f, 0.3f));
        sc.quads.push_back(Object(0.03f, 0.02f, 500.f, 2.f, 2.f, 0.95f));
        const Images im = RenderImages(gpu, sc, rig, 0);
        dcsvr::DisparitySettings c = s;
        c.mode = dcsvr::FarDepthMode::Compute;
        EyeSetup e;
        const auto k = MakeConstants(rig, e, c);
        FieldTex f = gpu.MakeField(rig.dw, rig.dh);
        gpu.DispatchField(k, RenderDepthTex(gpu, sc, rig, e), f);
        const Field fd = gpu.ReadField(f);
        ComPtr<ID3D11Texture2D> warpedTex;
        const auto warped = gpu.Warp(im.focusSrv.Get(), f, rig.cw, rig.ch, &warpedTex);
        gpu.SetComposition(rig, rig.focus);
        const Image off = gpu.Composite(rig, false, Ps(rig, e, false), im.periphSrv.Get(), im.focusSrv.Get(), nullptr);
        const Image on = gpu.Composite(rig, false, Ps(rig, e, false), im.periphSrv.Get(), warped.Get(), nullptr);
        const float tx = (0.03f * 500.f - EyeX(rig, 0)) / 500.f;
        const double cx = rig.CompX(tx), cy = rig.CompY(0.02f);
        const auto sh = Shift(off, on, cx, cy, 10);
        const auto ex = Expected(rig, k, tx, 0.02f, 500.f * std::sqrt(1.f + tx * tx + 0.0004f));
        const float bg = BorderMedian(off, int(cx) - 10, int(cy) - 10, int(cx) + 10, int(cy) + 10);
        float peakOff = 0, peakOn = 0;
        for (int y = int(cy) - 4; y <= int(cy) + 4; ++y)
            for (int x = int(cx) - 4; x <= int(cx) + 4; ++x) peakOff = std::max(peakOff, off.At(x, y) - bg), peakOn = std::max(peakOn, on.At(x, y) - bg);
        // Passthrough: where the field is under 1/64 px the warped image is the input, bit for bit.
        D3D11_TEXTURE2D_DESC d{};
        warpedTex->GetDesc(&d);
        d.BindFlags = 0;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st;
        Hr(gpu.device->CreateTexture2D(&d, nullptr, &st), "staging");
        gpu.context->CopyResource(st.Get(), warpedTex.Get());
        D3D11_MAPPED_SUBRESOURCE m;
        Hr(gpu.context->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m), "map");
        size_t passed = 0, exact = 0;
        for (int y = 0; y < rig.ch; ++y)
            for (int x = 0; x < rig.cw; ++x) {
                const float u = (x + 0.5f) / rig.cw, v = (y + 0.5f) / rig.ch;
                const int fx = std::min(int(u * rig.dw), rig.dw - 1), fy = std::min(int(v * rig.dh), rig.dh - 1);
                bool still = true;
                for (int j = -1; j <= 1 && still; ++j)
                    for (int i = -1; i <= 1 && still; ++i) {
                        const int xx = std::clamp(fx + i, 0, rig.dw - 1), yy = std::clamp(fy + j, 0, rig.dh - 1);
                        still = fd.U(xx, yy) == 0.f && fd.v[size_t(yy) * rig.dw + xx] == 0.f;
                    }
                if (!still) continue;
                ++passed;
                exact += reinterpret_cast<const float*>(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch)[x * 4] == im.focus[size_t(y) * rig.cw + x];
            }
        gpu.context->Unmap(st.Get(), 0);
        // The window: 0 wherever alpha < 1 (rectangle, smoothing 0.2: |ndc| > 0.6).
        double blendMax = 0;
        for (int y = 0; y < rig.dh; ++y)
            for (int x = 0; x < rig.dw; ++x) {
                const float nx = (x + 0.5f) / rig.dw * 2 - 1, ny = 1 - (y + 0.5f) / rig.dh * 2;
                if (dcsvr::DcsvrFocusBlendWindow(nx, ny, 0.f, 0.2f) < 1.f) blendMax = std::max(blendMax, (double)std::fabs(fd.U(x, y)));
            }
        std::printf("2x2 px aircraft at 500 m: shift %.4f px (expected %.4f), contrast kept %.0f%%; passthrough pixels bit-exact %zu of %zu\n", sh.first, ex.first,
                    100.0 * peakOn / peakOff, exact, passed);
        // Catmull-Rom reads +-2 px, wider than the field's one-texel dilation around a 2 px aircraft: its outer taps take
        // the background's shift, so a tiny aircraft moves a little less than in the fused route (which reads +-1 px).
        Check(sh.first >= 0.75 * ex.first && sh.first <= 1.1 * ex.first && peakOn >= 0.8f * peakOff,
              "compute fallback: the aircraft moves by 75-110% of its shift and keeps >= 80% of its contrast");
        Check(passed > 0 && exact == passed, "compute fallback: unmoved pixels pass through bit for bit");
        Check(blendMax == 0.0, "compute fallback: field 0 wherever the composition blends the focus with the periphery");
    }

    // ---- Cost ----
    std::printf("\nGPU cost (median of %d runs)%s\n", quick ? 10 : 50, gpu.warpDevice ? " (WARP: CPU, not representative)" : "");
    if (Want("cost")) {
        const int runs = quick ? 10 : 50;
        Scene sc = world;
        struct Size {
            int w, h;
            const char* what;
        };
        std::printf("| Pass | Size | ms |\n|---|---|---|\n");
        double focusMs = 0;
        for (const Size sz : {Size{1176, 1164, "FieldMain, focus depth (DLSS internal)"}, Size{816, 804, "FieldMain, shared 816x804 target"},
                              Size{1040, 1028, "FieldMain at the periphery's size"}}) {
            Rig r2 = rig;
            r2.dw = sz.w, r2.dh = sz.h, r2.texW = sz.w + 16, r2.texH = sz.h + 12;
            EyeSetup e;
            const DepthTex depth = RenderDepthTex(gpu, sc, r2, e);
            FieldTex f = gpu.MakeField(sz.w, sz.h);
            const auto k = MakeConstants(r2, e, s);
            dcsvr::DisparitySettings wide = s;
            wide.outerDeg = 90.f, wide.innerDeg = 80.f;
            const auto kWide = MakeConstants(r2, e, wide);
            const double ms = gpu.TimeMs([&] { gpu.DispatchField(k, depth, f); }, runs);
            const double msWide = gpu.TimeMs([&] { gpu.DispatchField(kWide, depth, f); }, runs);
            const double ms2 = gpu.TimeMs([&] { gpu.DispatchField(k, depth, f, true); }, runs);
            std::printf("| %s | %dx%d | %.4f (fovea 25 deg), %.4f (whole view), %.4f (2x2 footprint) |\n", sz.what, sz.w, sz.h, ms, msWide, ms2);
            if (sz.w == 1176) focusMs = msWide;
        }
        {
            // CoverageMain at the live sizes: the 2028x2004 copy against the submitted image over a 1176x1164 field.
            std::vector<float> grey(size_t(2028) * 2004, 0.4f);
            const auto pre = gpu.MakeSrgb(grey, 2028, 2004);
            const auto sub = gpu.MakeSrgb(grey, 2028, 2004);
            dcsvr::CoverageConstants ck;
            dcsvr::MakeCoverageConstants(1176, 1164, 2028, 2004, 0, 0, 2028, 2004, s.overlayThreshold, ck);
            DepthTex t;
            double ms = 0;
            gpu.BuildCoverage(t, ck, pre.Get(), sub.Get(), nullptr, &ms);
            std::printf("| CoverageMain, 2028x2004 copy over a 1176x1164 field | 1176x1164 | %.4f |\n", ms);
        }
        double warpFocus = 0;
        for (const Size sz : {Size{2028, 2004, "WarpMain (fallback), focus output"}, Size{1040, 1028, "WarpMain at the periphery's size"}}) {
            std::vector<float> c(size_t(sz.w) * sz.h, 0.5f);
            const auto colour = gpu.MakeColour(c, sz.w, sz.h);
            FieldTex f = gpu.MakeField(1176, 1164);
            const float half = 0.3f / sz.w;  // a field that moves every pixel: no passthrough
            std::vector<DirectX::PackedVector::HALF> fv(size_t(1176) * 1164 * 2, DirectX::PackedVector::XMConvertFloatToHalf(half));
            gpu.context->UpdateSubresource(f.tex.Get(), 0, nullptr, fv.data(), 1176 * 4, 0);
            D3D11_TEXTURE2D_DESC d{};
            d.Width = sz.w, d.Height = sz.h, d.MipLevels = d.ArraySize = 1, d.SampleDesc.Count = 1;
            d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            ComPtr<ID3D11Texture2D> t;
            ComPtr<ID3D11UnorderedAccessView> uav;
            Hr(gpu.device->CreateTexture2D(&d, nullptr, &t), "warp target");
            Hr(gpu.device->CreateUnorderedAccessView(t.Get(), nullptr, &uav), "warp uav");
            const double ms = gpu.TimeMs([&] { gpu.DispatchWarp(colour.Get(), f.srv.Get(), uav.Get(), sz.w, sz.h); }, runs);
            std::printf("| %s | %dx%d | %.4f |\n", sz.what, sz.w, sz.h, ms);
            if (sz.w == 2028) warpFocus = ms;
        }
        {
            // The composition at a Crystal Super's output size into its 8-bit sRGB swapchain, with and without the warp.
            const int W = quick ? 2712 : 5424, H = quick ? 2678 : 5356;
            D3D11_TEXTURE2D_DESC d{};
            d.Width = W, d.Height = H, d.MipLevels = d.ArraySize = 1, d.SampleDesc.Count = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            d.BindFlags = D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> t;
            ComPtr<ID3D11RenderTargetView> rtv;
            Hr(gpu.device->CreateTexture2D(&d, nullptr, &t), "target");
            Hr(gpu.device->CreateRenderTargetView(t.Get(), nullptr, &rtv), "rtv");
            const Images im = RenderImages(gpu, sc, rig, 0);
            EyeSetup e;
            FieldTex f = gpu.MakeField(rig.dw, rig.dh);
            gpu.DispatchField(MakeConstants(rig, e, s), RenderDepthTex(gpu, sc, rig, e), f);
            gpu.SetComposition(rig, rig.focus);
            const double off = gpu.TimeMs([&] { gpu.Draw(false, Ps(rig, e, false), im.periphSrv.Get(), im.focusSrv.Get(), nullptr, rtv.Get(), W, H); }, runs);
            const double on = gpu.TimeMs([&] { gpu.Draw(true, Ps(rig, e, true), im.periphSrv.Get(), im.focusSrv.Get(), f.srv.Get(), rtv.Get(), W, H); }, runs);
            std::printf("| Composition %dx%d sRGB8, shipping / with Far depth | %dx%d | %.4f / %.4f (delta %+.4f) |\n", W, H, W, H, off, on, on - off);
            std::printf("Per frame, both eyes (fused route): about %.3f ms (2 fields, whole view, + 2 x composition delta); fallback route adds 2 x %.3f ms\n",
                        2 * focusMs + 2 * std::max(0.0, on - off), warpFocus);
            if (!gpu.warpDevice && g_perf) {
                Check(focusMs <= 0.06, Fmt("cost: FieldMain at 1176x1164 %.4f ms per eye (<= 0.06 ms, whole view in the fovea)", focusMs));
                Check(on - off <= 0.06, Fmt("cost: the composition's extra field fetch %.4f ms per eye (<= 0.06 ms)", on - off));
            } else {
                std::printf("cost limits (FieldMain <= 0.06 ms, composition delta <= 0.06 ms per eye) reported, not asserted: run with --perf on an idle GPU\n");
            }
        }
    }
}

// ---- M8: the engine export's import ------------------------------------------------------------------------------

ULONG RefCount(IUnknown* p) {
    p->AddRef();
    return p->Release();
}

// A device proxy as ReShade installs one: the application (and so Quad Views' xrCreateSession) holds the proxy, while
// the resources made through it are the inner device's and report it from GetDevice.
class ProxyDevice final : public ID3D11Device {
  public:
    explicit ProxyDevice(ID3D11Device* inner) : m_inner(inner) {}
    HRESULT __stdcall QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11Device)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        return m_inner->QueryInterface(riid, out);
    }
    ULONG __stdcall AddRef() override { return ++m_refs; }
    ULONG __stdcall Release() override { return --m_refs; }  // on the stack: never deleted
    HRESULT __stdcall CreateBuffer(const D3D11_BUFFER_DESC* a, const D3D11_SUBRESOURCE_DATA* b, ID3D11Buffer** c) override { return m_inner->CreateBuffer(a, b, c); }
    HRESULT __stdcall CreateTexture1D(const D3D11_TEXTURE1D_DESC* a, const D3D11_SUBRESOURCE_DATA* b, ID3D11Texture1D** c) override { return m_inner->CreateTexture1D(a, b, c); }
    HRESULT __stdcall CreateTexture2D(const D3D11_TEXTURE2D_DESC* a, const D3D11_SUBRESOURCE_DATA* b, ID3D11Texture2D** c) override { return m_inner->CreateTexture2D(a, b, c); }
    HRESULT __stdcall CreateTexture3D(const D3D11_TEXTURE3D_DESC* a, const D3D11_SUBRESOURCE_DATA* b, ID3D11Texture3D** c) override { return m_inner->CreateTexture3D(a, b, c); }
    HRESULT __stdcall CreateShaderResourceView(ID3D11Resource* a, const D3D11_SHADER_RESOURCE_VIEW_DESC* b, ID3D11ShaderResourceView** c) override { return m_inner->CreateShaderResourceView(a, b, c); }
    HRESULT __stdcall CreateUnorderedAccessView(ID3D11Resource* a, const D3D11_UNORDERED_ACCESS_VIEW_DESC* b, ID3D11UnorderedAccessView** c) override { return m_inner->CreateUnorderedAccessView(a, b, c); }
    HRESULT __stdcall CreateRenderTargetView(ID3D11Resource* a, const D3D11_RENDER_TARGET_VIEW_DESC* b, ID3D11RenderTargetView** c) override { return m_inner->CreateRenderTargetView(a, b, c); }
    HRESULT __stdcall CreateDepthStencilView(ID3D11Resource* a, const D3D11_DEPTH_STENCIL_VIEW_DESC* b, ID3D11DepthStencilView** c) override { return m_inner->CreateDepthStencilView(a, b, c); }
    HRESULT __stdcall CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* a, UINT b, const void* c, SIZE_T d, ID3D11InputLayout** e) override { return m_inner->CreateInputLayout(a, b, c, d, e); }
    HRESULT __stdcall CreateVertexShader(const void* a, SIZE_T b, ID3D11ClassLinkage* c, ID3D11VertexShader** d) override { return m_inner->CreateVertexShader(a, b, c, d); }
    HRESULT __stdcall CreateGeometryShader(const void* a, SIZE_T b, ID3D11ClassLinkage* c, ID3D11GeometryShader** d) override { return m_inner->CreateGeometryShader(a, b, c, d); }
    HRESULT __stdcall CreateGeometryShaderWithStreamOutput(const void* a, SIZE_T b, const D3D11_SO_DECLARATION_ENTRY* c, UINT d, const UINT* e, UINT f, UINT g, ID3D11ClassLinkage* h,
                                                           ID3D11GeometryShader** i) override {
        return m_inner->CreateGeometryShaderWithStreamOutput(a, b, c, d, e, f, g, h, i);
    }
    HRESULT __stdcall CreatePixelShader(const void* a, SIZE_T b, ID3D11ClassLinkage* c, ID3D11PixelShader** d) override { return m_inner->CreatePixelShader(a, b, c, d); }
    HRESULT __stdcall CreateHullShader(const void* a, SIZE_T b, ID3D11ClassLinkage* c, ID3D11HullShader** d) override { return m_inner->CreateHullShader(a, b, c, d); }
    HRESULT __stdcall CreateDomainShader(const void* a, SIZE_T b, ID3D11ClassLinkage* c, ID3D11DomainShader** d) override { return m_inner->CreateDomainShader(a, b, c, d); }
    HRESULT __stdcall CreateComputeShader(const void* a, SIZE_T b, ID3D11ClassLinkage* c, ID3D11ComputeShader** d) override { return m_inner->CreateComputeShader(a, b, c, d); }
    HRESULT __stdcall CreateClassLinkage(ID3D11ClassLinkage** a) override { return m_inner->CreateClassLinkage(a); }
    HRESULT __stdcall CreateBlendState(const D3D11_BLEND_DESC* a, ID3D11BlendState** b) override { return m_inner->CreateBlendState(a, b); }
    HRESULT __stdcall CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* a, ID3D11DepthStencilState** b) override { return m_inner->CreateDepthStencilState(a, b); }
    HRESULT __stdcall CreateRasterizerState(const D3D11_RASTERIZER_DESC* a, ID3D11RasterizerState** b) override { return m_inner->CreateRasterizerState(a, b); }
    HRESULT __stdcall CreateSamplerState(const D3D11_SAMPLER_DESC* a, ID3D11SamplerState** b) override { return m_inner->CreateSamplerState(a, b); }
    HRESULT __stdcall CreateQuery(const D3D11_QUERY_DESC* a, ID3D11Query** b) override { return m_inner->CreateQuery(a, b); }
    HRESULT __stdcall CreatePredicate(const D3D11_QUERY_DESC* a, ID3D11Predicate** b) override { return m_inner->CreatePredicate(a, b); }
    HRESULT __stdcall CreateCounter(const D3D11_COUNTER_DESC* a, ID3D11Counter** b) override { return m_inner->CreateCounter(a, b); }
    HRESULT __stdcall CreateDeferredContext(UINT a, ID3D11DeviceContext** b) override { return m_inner->CreateDeferredContext(a, b); }
    HRESULT __stdcall OpenSharedResource(HANDLE a, REFIID b, void** c) override { return m_inner->OpenSharedResource(a, b, c); }
    HRESULT __stdcall CheckFormatSupport(DXGI_FORMAT a, UINT* b) override { return m_inner->CheckFormatSupport(a, b); }
    HRESULT __stdcall CheckMultisampleQualityLevels(DXGI_FORMAT a, UINT b, UINT* c) override { return m_inner->CheckMultisampleQualityLevels(a, b, c); }
    void __stdcall CheckCounterInfo(D3D11_COUNTER_INFO* a) override { m_inner->CheckCounterInfo(a); }
    HRESULT __stdcall CheckCounter(const D3D11_COUNTER_DESC* a, D3D11_COUNTER_TYPE* b, UINT* c, LPSTR d, UINT* e, LPSTR f, UINT* g, LPSTR h, UINT* i) override {
        return m_inner->CheckCounter(a, b, c, d, e, f, g, h, i);
    }
    HRESULT __stdcall CheckFeatureSupport(D3D11_FEATURE a, void* b, UINT c) override { return m_inner->CheckFeatureSupport(a, b, c); }
    HRESULT __stdcall GetPrivateData(REFGUID a, UINT* b, void* c) override { return m_inner->GetPrivateData(a, b, c); }
    HRESULT __stdcall SetPrivateData(REFGUID a, UINT b, const void* c) override { return m_inner->SetPrivateData(a, b, c); }
    HRESULT __stdcall SetPrivateDataInterface(REFGUID a, const IUnknown* b) override { return m_inner->SetPrivateDataInterface(a, b); }
    D3D_FEATURE_LEVEL __stdcall GetFeatureLevel() override { return m_inner->GetFeatureLevel(); }
    UINT __stdcall GetCreationFlags() override { return m_inner->GetCreationFlags(); }
    HRESULT __stdcall GetDeviceRemovedReason() override { return m_inner->GetDeviceRemovedReason(); }
    void __stdcall GetImmediateContext(ID3D11DeviceContext** a) override { m_inner->GetImmediateContext(a); }
    HRESULT __stdcall SetExceptionMode(UINT a) override { return m_inner->SetExceptionMode(a); }
    UINT __stdcall GetExceptionMode() override { return m_inner->GetExceptionMode(); }

  private:
    ID3D11Device* m_inner;
    ULONG m_refs{1};
};

void ImportChecks(bool forceWarp) {
    using namespace dcsvr;
    std::printf("\nM8 Engine export import (DcsVr_QuadViewsDepth)\n");
    ComPtr<ID3D11Device> device, other;
    ComPtr<ID3D11DeviceContext> context, otherContext;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (forceWarp || FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context)))
        Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context), "device");
    Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &other, nullptr, &otherContext), "other device");
    auto tex = [](ID3D11Device* d, DXGI_FORMAT f, UINT w, UINT h, UINT bind = D3D11_BIND_SHADER_RESOURCE) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w, td.Height = h, td.MipLevels = td.ArraySize = 1, td.SampleDesc.Count = 1;
        td.Format = f;
        td.BindFlags = bind;
        ComPtr<ID3D11Texture2D> t;
        Hr(d->CreateTexture2D(&td, nullptr, &t), "texture");
        return t;
    };
    ComPtr<ID3D11Texture2D> engineL = tex(device.Get(), DXGI_FORMAT_R32G8X24_TYPELESS, 1192, 1173);
    ComPtr<ID3D11Texture2D> engineR = tex(device.Get(), DXGI_FORMAT_R32G8X24_TYPELESS, 1192, 1173);
    const auto proj = ProjectionParams::Make(-0.62f, 0.58f, 0.6f, -0.585f, 0.02f);
    auto view = [&](uint32_t hmd, ID3D11Texture2D* t, uint64_t frame, uint32_t gen = 1) {
        DcsVrDepthView v{};
        v.structSize = sizeof(DcsVrDepthView);
        v.version = 1;
        v.hmdView = hmd;
        v.flags = DCSVR_DEPTH_VALID | DCSVR_DEPTH_REVERSE_Z | DCSVR_DEPTH_INFINITE_FAR | DCSVR_DEPTH_AFTER_WATER | DCSVR_DEPTH_HAS_STENCIL | DCSVR_DEPTH_VIEWINV_VALID;
        v.engineFrame = frame;
        v.generation = gen;
        v.format = DXGI_FORMAT_R32G8X24_TYPELESS;
        v.texture = t;
        v.texWidth = 1192, v.texHeight = 1173, v.sampleCount = 1;
        const float vp[6] = {8, 6, 1176, 1161, 0, 1};
        std::copy(vp, vp + 6, v.viewport);
        std::copy(proj.begin(), proj.end(), v.proj);
        // gViewInv: identity rotation, camera at x = -/+ 32 mm.
        v.viewInv[0] = v.viewInv[5] = v.viewInv[10] = v.viewInv[15] = 1.f;
        v.viewInv[12] = hmd == 2 ? -0.032f : 0.032f;
        return v;
    };
    DepthImport imp;
    imp.SetDevice(device.Get());
    imp.SetRenderThread(GetCurrentThreadId());
    const float aspect = 2028.f / 2003.f;
    {
        auto v = view(2, engineL.Get(), 1);
        Check(imp.Push(&v) == DCSVR_DEPTH_PUSH_NOT_WANTED, "M8 nothing wanted (Far depth off): rejected, no reference taken");
    }
    imp.SetWanted(DepthImport::WantedFocus);
    const ULONG base = RefCount(engineL.Get());
    auto l1 = view(2, engineL.Get(), 1), r1 = view(3, engineR.Get(), 1);
    Check(imp.Push(&l1) == DCSVR_DEPTH_PUSH_ACCEPTED && imp.Push(&r1) == DCSVR_DEPTH_PUSH_ACCEPTED && RefCount(engineL.Get()) == base + 1,
          "M8 both focus views accepted; Quad Views holds one reference each");
    Check(imp.Push(&l1) == DCSVR_DEPTH_PUSH_ACCEPTED && RefCount(engineL.Get()) == base + 1, "M8 a repeated push replaces (releases) the held reference");
    DepthFrame f = imp.Validate(0, aspect);
    Check(f.reject == DepthReject::None && f.view[0] && f.view[1] && f.proj[0].ok, "M8 a complete frame validates");
    Check(imp.Validate(0, aspect).reject == DepthReject::Missing, "M8 the next frame without new pushes: missing (never stale depth)");
    {
        auto l = view(2, engineL.Get(), 2);
        imp.Push(&l);
        Check(imp.Validate(0, aspect).reject == DepthReject::Missing, "M8 only the left eye pushed: both skipped");
        auto r = view(3, engineR.Get(), 3);
        imp.Push(&l), imp.Push(&r);
        Check(imp.Validate(0, aspect).reject == DepthReject::Frame, "M8 the two eyes from different engine frames: skipped");
        auto lf = view(2, engineL.Get(), 4), rf = view(3, engineR.Get(), 4);
        rf.flags &= ~DCSVR_DEPTH_REVERSE_Z;
        imp.Push(&lf), imp.Push(&rf);
        Check(imp.Validate(0, aspect).reject == DepthReject::Flags, "M8 not flagged reverse-Z: skipped");
        auto lp = view(2, engineL.Get(), 5), rp = view(3, engineR.Get(), 5);
        rp.proj[11] = 0.f;
        imp.Push(&lp), imp.Push(&rp);
        Check(imp.Validate(0, aspect).reject == DepthReject::Projection, "M8 a gProj that is not a perspective: skipped");
        auto la = view(2, engineL.Get(), 6), ra = view(3, engineR.Get(), 6);
        la.viewport[2] = 1040;
        imp.Push(&la), imp.Push(&ra);
        Check(imp.Validate(0, aspect).reject == DepthReject::Aspect, "M8 a viewport of another aspect than the focus image: skipped");
        auto ld = view(2, engineL.Get(), 7), rd = view(3, engineR.Get(), 7);
        ld.displayTime = rd.displayTime = 1000;
        imp.Push(&ld), imp.Push(&rd);
        Check(imp.Validate(2000, aspect).reject == DepthReject::DisplayTime, "M8 the engine's display time is another frame's: skipped");
        auto ls = view(3, engineL.Get(), 8), rs = view(2, engineR.Get(), 8);
        ls.hmdView = 2, rs.hmdView = 3;
        ls.viewInv[12] = 0.032f, rs.viewInv[12] = -0.032f;
        imp.Push(&ls), imp.Push(&rs);
        Check(imp.Validate(0, aspect).reject == DepthReject::Swap, "M8 left and right exchanged (camera positions): skipped");
        auto lg = view(2, engineL.Get(), 9, 2), rg = view(3, engineR.Get(), 9, 1);
        imp.Push(&lg), imp.Push(&rg);
        Check(imp.Validate(0, aspect).reject == DepthReject::Generation, "M8 the two eyes of different generations (one from before a reload): skipped");
    }
    {
        auto v = view(2, engineL.Get(), 10);
        v.structSize = sizeof(DcsVrDepthView) - 4;
        const uint32_t size = imp.Push(&v);
        v = view(2, engineL.Get(), 10);
        v.sampleCount = 4;
        const uint32_t msaa = imp.Push(&v);
        ComPtr<ID3D11Texture2D> colour = tex(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64);
        v = view(2, colour.Get(), 10);
        v.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        const uint32_t format = imp.Push(&v);
        ComPtr<ID3D11Texture2D> foreign = tex(other.Get(), DXGI_FORMAT_R32G8X24_TYPELESS, 64, 64);
        v = view(2, foreign.Get(), 10);
        const uint32_t dev = imp.Push(&v);
        uint32_t thread = 99;
        std::thread([&] {
            auto t = view(2, engineL.Get(), 10);
            thread = imp.Push(&t);
        }).join();
        Check(size == DCSVR_DEPTH_PUSH_NULL_OR_SIZE && imp.Push(nullptr) == DCSVR_DEPTH_PUSH_NULL_OR_SIZE && msaa == DCSVR_DEPTH_PUSH_MSAA &&
                  format == DCSVR_DEPTH_PUSH_FORMAT && dev == DCSVR_DEPTH_PUSH_FOREIGN_DEVICE && thread == DCSVR_DEPTH_PUSH_NOT_RENDER_THREAD,
              "M8 rejection codes: size (0), MSAA (4), format (3), foreign device (2), another thread (6)");
    }
    {
        // The engine releases its texture (a hot reload) while Quad Views holds it: still alive, released by Quad Views.
        ComPtr<ID3D11Texture2D> t = tex(device.Get(), DXGI_FORMAT_R32G8X24_TYPELESS, 1192, 1173);
        ID3D11Texture2D* raw = t.Get();
        auto v = view(2, raw, 11, 3);
        imp.Push(&v);
        t.Reset();  // the engine's reference is gone
        D3D11_TEXTURE2D_DESC d{};
        raw->GetDesc(&d);
        Check(d.Width == 1192 && RefCount(raw) == 1, "M8 the engine drops its texture: Quad Views' reference keeps it alive");
        imp.Release(3);
        Check(imp.View(2).texture == nullptr, "M8 DcsVr_QuadViewsDepthRelease(generation) drops it");
    }
    imp.ReleaseAll();
    Check(RefCount(engineL.Get()) == base && RefCount(engineR.Get()) == base, "M8 session end: every reference released (counts back to the engine's own)");
    const auto counts = std::array<uint64_t, 4>{imp.Rejects(DepthReject::Missing), imp.Rejects(DepthReject::Frame), imp.Rejects(DepthReject::Flags), imp.Rejects(DepthReject::None)};
    Check(counts[0] == 2 && counts[1] == 1 && counts[2] == 1 && counts[3] == 1, "M8 one statistics counter per reason");

    // Generations are opaque ids: a reload whose counter restarts lower is accepted; Release drops only its own.
    {
        DepthImport g;
        g.SetDevice(device.Get());
        g.SetWanted(DepthImport::WantedFocus);
        auto l = view(2, engineL.Get(), 20, 3), r = view(3, engineR.Get(), 20, 3);
        g.Push(&l), g.Push(&r);
        const bool before = g.Validate(0, aspect).reject == DepthReject::None;
        l = view(2, engineL.Get(), 21, 1), r = view(3, engineR.Get(), 21, 1);  // the reloaded payload starts at 1
        g.Push(&l), g.Push(&r);
        const bool after = g.Validate(0, aspect).reject == DepthReject::None;
        g.Release(3);
        const bool kept = g.View(2).texture && g.View(3).texture;
        g.Release(1);
        Check(before && after && kept && !g.View(2).texture && !g.View(3).texture,
              "M8 a reload restarting the generation lower (3 -> 1): frames still validate; Release(3) keeps generation 1, Release(1) drops it");
    }
    // Formats and layout: no 16-bit depth, one slice and one mip.
    {
        DepthImport g;
        g.SetDevice(device.Get());
        g.SetWanted(DepthImport::WantedFocus);
        ComPtr<ID3D11Texture2D> d16 = tex(device.Get(), DXGI_FORMAT_R16_TYPELESS, 1192, 1173);
        auto v = view(2, d16.Get(), 30);
        v.format = DXGI_FORMAT_R16_TYPELESS;
        const uint32_t r16 = g.Push(&v);
        v = view(2, engineL.Get(), 30);
        v.arraySlice = 1;
        const uint32_t slice = g.Push(&v);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 1192, td.Height = 1173, td.MipLevels = 1, td.ArraySize = 2, td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> array;
        Hr(device->CreateTexture2D(&td, nullptr, &array), "array texture");
        v = view(2, array.Get(), 30);
        const uint32_t arrayed = g.Push(&v);
        Check(r16 == DCSVR_DEPTH_PUSH_FORMAT && slice == DCSVR_DEPTH_PUSH_LAYOUT && arrayed == DCSVR_DEPTH_PUSH_LAYOUT && !g.View(2).texture,
              "M8 rejected: R16 depth (format, 3), arraySlice 1 and a 2-slice texture (layout, 7)");
    }
    // The pre-overlay copy (HAS_PRE_OVERLAY): latched with its own reference, checked, required by default.
    {
        DepthImport g;
        g.SetDevice(device.Get());
        g.SetWanted(DepthImport::WantedFocus);
        ComPtr<ID3D11Texture2D> preL = tex(device.Get(), DXGI_FORMAT_R8G8B8A8_TYPELESS, 2028, 2004),
                                preR = tex(device.Get(), DXGI_FORMAT_R8G8B8A8_TYPELESS, 2028, 2004);
        auto l = view(2, engineL.Get(), 40), r = view(3, engineR.Get(), 40);
        const bool plain = g.Push(&l) == DCSVR_DEPTH_PUSH_ACCEPTED && g.Push(&r) == DCSVR_DEPTH_PUSH_ACCEPTED;
        const DepthReject unmasked = g.Validate(0, aspect, true).reject;
        g.Push(&l), g.Push(&r);
        const DepthReject allowed = g.Validate(0, aspect, false).reject;
        const ULONG preBase = RefCount(preL.Get());
        l = view(2, engineL.Get(), 41), r = view(3, engineR.Get(), 41);
        for (auto* v : {&l, &r}) {
            v->flags |= DCSVR_DEPTH_HAS_PRE_OVERLAY;
            v->preOverlayFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            v->preOverlaySize = 2028u | (2004u << 16);
        }
        l.preOverlay = preL.Get(), r.preOverlay = preR.Get();
        const bool masked = g.Push(&l) == DCSVR_DEPTH_PUSH_ACCEPTED && g.Push(&r) == DCSVR_DEPTH_PUSH_ACCEPTED && RefCount(preL.Get()) == preBase + 1;
        const DepthFrame mf = g.Validate(0, aspect, true);
        Check(plain && unmasked == DepthReject::Coverage && allowed == DepthReject::None && masked && mf.reject == DepthReject::None && mf.preOverlay &&
                  mf.view[0]->preOverlay.Get() == preL.Get() && mf.view[0]->desc.preOverlay == nullptr,
              "M8 pre-overlay: depth without it is rejected while required (Coverage), accepted when allowed; with it, accepted and held");
        // Accepted too: a larger texture than the copied box, the render target's own format without a typeless family.
        auto ok = l;
        ComPtr<ID3D11Texture2D> larger = tex(device.Get(), DXGI_FORMAT_R8G8B8A8_TYPELESS, 2048, 2048),
                                packed = tex(device.Get(), DXGI_FORMAT_R11G11B10_FLOAT, 2028, 2004);
        ok.preOverlay = larger.Get();
        const uint32_t largerOk = g.Push(&ok);
        ok.preOverlay = packed.Get(), ok.preOverlayFormat = DXGI_FORMAT_R11G11B10_FLOAT;
        const uint32_t packedOk = g.Push(&ok);
        Check(largerOk == DCSVR_DEPTH_PUSH_ACCEPTED && packedOk == DCSVR_DEPTH_PUSH_ACCEPTED,
              "M8 pre-overlay accepted: a texture larger than the box, R11G11B10_FLOAT viewed as itself");
        auto bad = l;
        ComPtr<ID3D11Texture2D> tiny = tex(device.Get(), DXGI_FORMAT_R8G8B8A8_TYPELESS, 1024, 2004),
                                family = tex(device.Get(), DXGI_FORMAT_R10G10B10A2_TYPELESS, 2028, 2004),
                                r8 = tex(device.Get(), DXGI_FORMAT_R8_UNORM, 2028, 2004),
                                foreignPre = tex(other.Get(), DXGI_FORMAT_R8G8B8A8_TYPELESS, 2028, 2004);
        bad.preOverlay = tiny.Get();
        const uint32_t size = g.Push(&bad);
        bad.preOverlay = preL.Get(), bad.preOverlaySize = 0;
        const uint32_t empty = g.Push(&bad);
        bad = l, bad.preOverlay = family.Get();
        const uint32_t format = g.Push(&bad);
        bad.preOverlay = r8.Get(), bad.preOverlayFormat = DXGI_FORMAT_R8_UNORM;
        const uint32_t unsupported = g.Push(&bad);
        bad = l, bad.preOverlay = foreignPre.Get();
        const uint32_t foreign = g.Push(&bad);
        bad.preOverlay = nullptr;
        const uint32_t null = g.Push(&bad);
        Check(size == DCSVR_DEPTH_PUSH_LAYOUT && empty == DCSVR_DEPTH_PUSH_LAYOUT && format == DCSVR_DEPTH_PUSH_FORMAT &&
                  unsupported == DCSVR_DEPTH_PUSH_FORMAT && foreign == DCSVR_DEPTH_PUSH_FOREIGN_DEVICE && null == DCSVR_DEPTH_PUSH_NULL_OR_SIZE,
              "M8 pre-overlay rejected: smaller than its box or no box (7), another family or an R8 view (3), another device (2), flagged but null (0)");
        // The pair (PRE_POST_OVERLAY): a texture of 2w, and only with the copy it belongs to.
        ComPtr<ID3D11Texture2D> pairTex = tex(device.Get(), DXGI_FORMAT_R8G8B8A8_TYPELESS, 4056, 2004);
        auto pairView = l;
        pairView.flags |= DCSVR_DEPTH_PRE_POST_OVERLAY;
        pairView.preOverlay = pairTex.Get();
        const uint32_t pairOk = g.Push(&pairView);
        pairView.preOverlay = preL.Get();
        const uint32_t pairNarrow = g.Push(&pairView);
        pairView.flags &= ~(uint32_t)DCSVR_DEPTH_HAS_PRE_OVERLAY;
        const uint32_t pairAlone = g.Push(&pairView);
        Check(pairOk == DCSVR_DEPTH_PUSH_ACCEPTED && pairNarrow == DCSVR_DEPTH_PUSH_LAYOUT && pairAlone == DCSVR_DEPTH_PUSH_LAYOUT,
              "M8 pre/post pair: accepted at 2w, rejected narrower or without HAS_PRE_OVERLAY (7)");
        g.ReleaseAll();
        Check(RefCount(preL.Get()) == preBase, "M8 session end releases the pre-overlay copy too");
    }
    // A second projection layer validates the same frame again: the same answer, nothing consumed or counted twice.
    {
        DepthImport g;
        g.SetDevice(device.Get());
        g.SetWanted(DepthImport::WantedFocus);
        auto l = view(2, engineL.Get(), 50), r = view(3, engineR.Get(), 50);
        g.Push(&l), g.Push(&r);
        const DepthFrame a = g.Validate(5000, aspect), b = g.Validate(5000, aspect);
        const DepthFrame next = g.Validate(5022, aspect);
        Check(a.reject == DepthReject::None && b.reject == DepthReject::None && b.view[0] == a.view[0] && b.view[1] && g.Rejects(DepthReject::None) == 1 &&
                  next.reject == DepthReject::Missing,
              "M8 Validate twice for one display time (two layers): the same frame, counted once; the next display time needs new pushes");
        // The two eyes' viewports must be one size (one field size), even when both have the focus image's aspect.
        l = view(2, engineL.Get(), 51), r = view(3, engineR.Get(), 51);
        r.viewport[2] = 1174, r.viewport[3] = 1159;
        g.Push(&l), g.Push(&r);
        Check(g.Validate(5044, aspect).reject == DepthReject::Viewport, "M8 the eyes' viewports of different sizes (1176x1161, 1174x1159): skipped");
    }
    // A device proxy (ReShade): Quad Views gets the proxy, the engine's textures report the inner device.
    {
        ProxyDevice proxy(device.Get());
        DepthImport g;
        g.SetDevice(&proxy);
        g.SetWanted(DepthImport::WantedFocus);
        auto l = view(2, engineL.Get(), 60), r = view(3, engineR.Get(), 60);
        const bool accepted = g.Push(&l) == DCSVR_DEPTH_PUSH_ACCEPTED && g.Push(&r) == DCSVR_DEPTH_PUSH_ACCEPTED;
        ComPtr<ID3D11Texture2D> foreign = tex(other.Get(), DXGI_FORMAT_R32G8X24_TYPELESS, 64, 64);
        auto f2 = view(2, foreign.Get(), 60);
        const uint32_t still = g.Push(&f2);
        g.ReleaseAll();
        g.SetDevice(nullptr);
        Check(accepted && still == DCSVR_DEPTH_PUSH_FOREIGN_DEVICE, "M8 behind a device proxy (ReShade): the engine's textures are accepted, another device's still rejected");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: depthwarp_test.exe <native/quadviews dir> <quad views openxr-api-layer dir> [--warp] [--quick] [--perf] [--only=<section>]\n");
        return 2;
    }
    bool warp = false, quick = false;
    for (int i = 3; i < argc; ++i) {
        warp |= std::string(argv[i]) == "--warp";
        quick |= std::string(argv[i]) == "--quick";
        g_perf |= std::string(argv[i]) == "--perf";
        if (std::string(argv[i]).rfind("--only=", 0) == 0) g_only = std::string(argv[i]).substr(7);
    }
    try {
        CurveChecks();
        ProjectionChecks();
        WindowChecks();
        EpipolarChecks();
        ControllerChecks();
        ImportChecks(warp);
        ShaderChecks(argv[1], argv[2], warp, quick);
    } catch (const std::exception& e) {
        std::printf("FAIL exception: %s\n", e.what());
        return 2;
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
