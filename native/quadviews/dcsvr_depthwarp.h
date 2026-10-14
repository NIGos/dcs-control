// DCS Control: Far depth, the depth-selective, infinity-anchored disparity warp of the bundled Quad Views compositor
// (design: docs/FAR_DEPTH.md; tested by tests/depthwarp). Built into the layer by scripts/build-quadviews.ps1
// (dcsvr_far_depth=1); it runs only with the engine's depth export (dcsvr_depth_export.h), which DcsQvCull does not
// push yet.
//
// One header for C++ and HLSL, as dcsvr_perception.h (which it includes for DCSVR_FN and the shared helpers). The
// first part is the maths the field shader runs (dcsvr_depthwarp_cs.hlsl) and the CPU tests check: the disparity
// curve, depth to range, the weights and the epipolar shift. The second part (C++ only) is what the layer keeps per
// frame: the projection parsed from DCS's gProj, the jitter centre, the settings and presets, the shader constants
// and the gain controller.
//
// Why: Depth boost scales the rendered IPD, which changes the near field's disparity a lot (the cockpit "breathes")
// and a far aircraft's almost not at all (+16 mm at 300 m adds 0.18 arcmin, under a pixel). Far depth leaves the near
// field (< Z0) and infinity untouched and adds disparity only in a far band, so a distant aircraft separates from the
// terrain behind it: total disparity I/R + g F (Dt - I/R), Dt = (I/Z0)(Z0/R)^gamma, still strictly decreasing in R
// (depth order kept) because g F <= 1. Each eye's image moves half of it along its epipolar direction, the left eye's
// content toward the right eye and the right eye's toward the left (crossed: nearer), so the cyclopean direction and
// HUD aiming are unchanged.
#ifndef DCSVR_DEPTHWARP_H
#define DCSVR_DEPTHWARP_H

#include "dcsvr_perception.h"

#ifdef __cplusplus
#include <array>
#include <cstring>
#include <limits>
namespace dcsvr {
#define DCSVR_OUT(T) T&
using uint = uint32_t;
using std::cos;
using std::floor;
using std::sin;
#else
#define DCSVR_OUT(T) out T
#endif

// Target disparity at range R (radians, small angles): what a viewer with IPD ipd would see at Z0 (Z0/R)^gamma.
// The ratio is positive wherever it is used (range > Z0 > 0); abs tells the HLSL compiler so (no X3571 warning).
DCSVR_FN float DcsvrTargetDisparityRad(float range, float ipd, float z0, float gamma) {
    return ipd / z0 * pow(abs(z0 / range), gamma);
}

// Extra disparity (both eyes together, radians) added at range R: zero up to Z0 (where the curve meets the natural
// disparity, so the near field and the cockpit are untouched), zero inside the near guard, and fading to zero from
// fadeStart to fadeEnd (infinity, the sky and the far plane stay where they are). Not a finite range above the
// guards (the sky's -1 or +inf, NaN): zero.
//
// The world-IPD curve (the Near preset; gamma < 0 selects it, -gamma = k - 1): the extra disparity of rendering the world
// with k times the eye distance, (k - 1) ipd / R, faded in from nearGuard to 3 nearGuard and out from fadeStart to
// fadeEnd. The cockpit keeps its true disparity (the field pins it by its stencil), so returning the gaze to it changes
// nothing. The total disparity (1 + (k - 1) fade(R)) ipd / R keeps falling with R for k - 1 <= 1 (the fade-in's slope
// is at most 0.75 / nearGuard at 2 nearGuard): depth order is kept.
DCSVR_FN float DcsvrExtraDisparityRad(float range, float ipd, float z0, float gamma, float fadeStart, float fadeEnd,
                                      float nearGuard) {
    const float fade = 1.0f - DcsvrSmoothstep(fadeStart, max(fadeEnd, fadeStart + 1.0f), range);
    if (gamma < 0.0f) {
        if (!(range > nearGuard) || !(range < 1e30f)) return 0.0f;
        return min(-gamma, 1.0f) * ipd / range * DcsvrSmoothstep(nearGuard, 3.0f * nearGuard, range) * fade;
    }
    if (!(range > max(z0, nearGuard)) || !(range < 1e30f)) return 0.0f;
    return max(0.0f, DcsvrTargetDisparityRad(range, ipd, z0, gamma) - ipd / range) * fade;
}

// Forward distance (clip w, metres) of an NDC depth through gProj's z column (the engine's ForwardDist, fixation_depth.h:
// clip = [x y z 1] * P, w = z P23 + P33): -1 for the sky / infinite far plane (reverse-Z depth <= 1e-7) and for
// anything that is not a finite positive distance.
DCSVR_FN float DcsvrForwardFromDepth(float ndcDepth, float p22, float p23, float p32, float p33) {
    if (!(ndcDepth > 1e-7f) || !(ndcDepth <= 1.0001f)) return -1.0f;
    const float den = ndcDepth * p23 - p22;
    if (!(abs(den) > 1e-30f)) return -1.0f;
    const float z = (p32 - ndcDepth * p33) / den;
    const float w = z * p23 + p33;
    return w > 0.0f && w < 1e30f ? w : -1.0f;
}

// Range along the ray (metres) of a forward distance at the view tangents (tx, ty); keeps the -1 of the sky.
DCSVR_FN float DcsvrRangeFromForward(float forward, float tx, float ty) {
    return forward > 0.0f ? forward * sqrt(1.0f + tx * tx + ty * ty) : -1.0f;
}

// DCS's stencil composition type (Bazar/shaders/common/stencil.hlsl, bits 3-5; the engine decodes it the same way):
// 5 is the cockpit. The cockpit never moves, whatever its depth says (canopy frame, panel, the pilot's body).
DCSVR_FN bool DcsvrStencilIsCockpit(uint stencil) {
    return ((stencil >> 3) & 7u) == 5u;
}

// Gaze weight: 1 inside innerDeg of the gaze, 0 beyond outerDeg (the effect is for the fovea; outside it the eye
// cannot use sub-arcminute disparity and a warp would only cost).
DCSVR_FN float DcsvrFoveaWeight(float eccentricityDeg, float innerDeg, float outerDeg) {
    return 1.0f - DcsvrSmoothstep(innerDeg, max(outerDeg, innerDeg + 0.01f), eccentricityDeg);
}

// The focus shape metric of a focus NDC point: max(|x|, |y|) for the rectangle (shape < 1), the superellipse's p-norm
// otherwise; 1 on the focus edge ProjectionPS draws.
DCSVR_FN float DcsvrFocusShapeMetric(float nx, float ny, float shape) {
    const float ax = abs(nx), ay = abs(ny);
    return shape >= 1.0f ? pow(pow(ax, shape) + pow(ay, shape), 1.0f / shape) : max(ax, ay);
}

// Edge window of the field: 1 inside lo, 0 from hi (of the shape metric, scaled by rhoScale). The field is applied to
// the focus and the periphery sample alike, so it only has to reach 0 at the focus edge (rhoScale 1). The compute
// fallback warps the focus image alone and must be 0 wherever the composition blends it with the periphery
// (DcsvrFocusFullAlphaScale), or the two would show the same object at two places.
DCSVR_FN float DcsvrFocusEdgeWindow(float nx, float ny, float shape, float lo, float hi, float rhoScale) {
    const float rho = DcsvrFocusShapeMetric(nx, ny, shape) / max(rhoScale, 1e-3f);
    return 1.0f - DcsvrSmoothstep(lo, max(hi, lo + 1e-4f), rho);
}

// The focus image's alpha in the composition at focus NDC (nx, ny): an exact port of ProjectionPS's (the rounded
// shape's radial fade, the rectangle's max(0.5, s.x s.y) smoothstep in uv, or 1), 0 outside the focus view.
DCSVR_FN float DcsvrFocusBlendWindow(float nx, float ny, float shape, float smoothing) {
    const float isInside = abs(nx) < 1.0f && abs(ny) < 1.0f ? 1.0f : 0.0f;
    if (shape >= 1.0f) {
        const float r = DcsvrFocusShapeMetric(nx, ny, shape);
        const float width = min(max(2.0f * smoothing, 0.01f), 0.95f);
        return isInside * (1.0f - DcsvrSmoothstep(1.0f - width, 1.0f, r));
    }
    if (smoothing > 0.0f) {
        const float u = nx * 0.5f + 0.5f, v = ny * -0.5f + 0.5f;
        const float sx = DcsvrSmoothstep(0.0f, smoothing, u) - DcsvrSmoothstep(1.0f - smoothing, 1.0f, u);
        const float sy = DcsvrSmoothstep(0.0f, smoothing, v) - DcsvrSmoothstep(1.0f - smoothing, 1.0f, v);
        return isInside * max(0.5f, sx * sy);
    }
    return isInside;
}

// The share of the focus shape metric over which the composition's alpha is exactly 1 (the compute fallback's
// rhoScale): the rectangle's |ndc| <= 1 - 2 smoothing, the rounded shape's r <= 1 - width.
DCSVR_FN float DcsvrFocusFullAlphaScale(float shape, float smoothing) {
    if (shape >= 1.0f) return 1.0f - min(max(2.0f * smoothing, 0.01f), 0.95f);
    return max(1.0f - 2.0f * max(smoothing, 0.0f), 0.0f);
}

// The ray through the tangents (tx, ty) (looking down -Z, +Y up) turned by angle a (radians) toward the unit
// direction (bx, by, bz) in the same frame: a rotation in the plane of the ray and the baseline, so the point moves
// along its epipolar line (no vertical disparity, also with canted displays). Output: the turned ray's tangents.
DCSVR_FN void DcsvrEpipolarShift(float tx, float ty, float a, float bx, float by, float bz, DCSVR_OUT(float) ox,
                                 DCSVR_OUT(float) oy) {
    ox = tx;
    oy = ty;
    const float il = 1.0f / sqrt(tx * tx + ty * ty + 1.0f);
    const float rx = tx * il, ry = ty * il, rz = -il;
    const float d = bx * rx + by * ry + bz * rz;
    float px = bx - d * rx, py = by - d * ry, pz = bz - d * rz;
    const float pl = sqrt(px * px + py * py + pz * pz);
    if (!(pl > 1e-6f)) return;  // the ray along the baseline: no epipolar direction
    px /= pl;
    py /= pl;
    pz /= pl;
    const float c = cos(a), s = sin(a);
    const float qx = c * rx + s * px, qy = c * ry + s * py, qz = c * rz + s * pz;
    if (!(qz < -1e-6f)) return;
    ox = qx / -qz;
    oy = qy / -qz;
}

// Per-eye turn angle (radians, >= 0): half the extra disparity, weighted by the gain and the window, clamped.
DCSVR_FN float DcsvrEyeAngle(float extraRad, float gain, float weight, float maxRad) {
    return min(maxRad, 0.5f * gain * weight * extraRad);
}

#ifdef __cplusplus

// ---- C++ only ------------------------------------------------------------------------------------------------------

constexpr float ArcminRad = 0.000290888209f;

// DCS's gProj (cPerView byte 256, logical P[r][c] at m[r * 4 + c], row-vector convention clip = [x y z 1] * P),
// parsed as the engine's ParseProj: NDC = s * tan + c, reverse-Z, infinite far.
struct ProjectionParams {
    bool ok{false}, reverse{false}, infinite{false};
    float sx{1.f}, sy{1.f};  // P00, P11 (NDC per tangent)
    float cx{0.f}, cy{0.f};  // P20 / P23, P21 / P23: NDC of the view axis (DLSS's jitter lands here)
    float p22{0.f}, p23{1.f}, p32{0.f}, p33{0.f};
    float nearM{0.f};        // reverse-Z infinite: P32 / P23

    static ProjectionParams Parse(const float m[16]) {
        ProjectionParams p;
        for (int i = 0; i < 16; ++i)
            if (!std::isfinite(m[i])) return p;
        const double p23 = m[11];
        if (std::fabs(p23) < 1e-6 || std::fabs(m[15]) > 1e-4 * std::fabs(p23)) return p;  // not perspective
        if (std::fabs(m[0]) < 1e-6 || std::fabs(m[5]) < 1e-6) return p;
        if (std::fabs(m[3]) > 1e-4 || std::fabs(m[7]) > 1e-4) return p;
        // The shift maths assumes an axis-aligned frustum (no skew): DCS's is.
        if (std::fabs(m[1]) > 1e-5 * std::fabs(m[0]) || std::fabs(m[4]) > 1e-5 * std::fabs(m[5])) return p;
        p.sx = m[0];
        p.sy = m[5];
        p.cx = float(m[8] / p23);
        p.cy = float(m[9] / p23);
        p.p22 = m[10];
        p.p23 = m[11];
        p.p32 = m[14];
        p.p33 = m[15];
        const auto depthAtW = [&](double w) {
            const double z = (w - p.p33) / p.p23;
            return (z * p.p22 + p.p32) / w;
        };
        const double d1 = depthAtW(1.0), d2 = depthAtW(1000.0);
        if (!std::isfinite(d1) || !std::isfinite(d2) || d1 == d2) return p;
        p.reverse = d1 > d2;
        const double farLimit = p.p22 / p23;
        p.infinite = p.reverse ? std::fabs(farLimit) < 1e-6 : std::fabs(farLimit - 1.0) < 1e-6;
        p.nearM = float(p.p32 / p23);
        p.ok = true;
        return p;
    }

    // Reverse-Z infinite perspective as DCS builds it, for tests: tangents, near plane, the axis' NDC (jitter).
    static std::array<float, 16> Make(float left, float right, float up, float down, float nearM, float cxJitter = 0.f,
                                      float cyJitter = 0.f) {
        std::array<float, 16> m{};
        m[0] = 2.f / (right - left);
        m[5] = 2.f / (up - down);
        m[8] = -(right + left) / (right - left) + cxJitter;
        m[9] = -(up + down) / (up - down) + cyJitter;
        m[10] = 0.f;
        m[11] = 1.f;
        m[14] = nearM;
        return m;
    }
};

// The projection centre offset (NDC) of a jitter of (jx, jy) internal pixels (+x right, +y down: the sample taken at
// the texel centre is the scene point at centre + j) in a w x h viewport: c = c0 + JitterNdc. The sign convention
// of the engine's jitterPx is to be confirmed live (docs/FAR_DEPTH.md, measurement request i).
inline void JitterNdc(float jx, float jy, float w, float h, float& dx, float& dy) {
    dx = -2.f * jx / w;
    dy = 2.f * jy / h;
}

// The unjittered centre c0 of DLSS's output (DLSS output is de-jittered; gProj's c = c0 + the jitter). Quad Views'
// focus frustum follows the gaze, so c0 moves every frame and no average of c can stand for it: an average lags a
// smooth pursuit by half its window (tens of depth texels) and a saccade by its whole amplitude. With the frustum Quad
// Views handed DCS for this frame (the reference: DCS renders exactly that FOV), only the residual c - reference is
// averaged over 32 frames: a low-discrepancy jitter averages to 0 within a few hundredths of a texel, and an offset
// DCS might add would be kept. Without a reference, the mean of c itself, restarted whenever c leaves it by more than
// 1.25 depth texel (more than DLSS's +-0.5 texel jitter can explain: the frustum moved), so the centre lags by about a
// texel at most and a frustum that keeps moving degrades to c itself (no re-alignment; the 3x3 footprint absorbs the
// jitter). A reference whose scale is not gProj's or whose residual exceeds 2 texels belongs to another frame and is
// ignored. A change of the projection's scale starts over. The engine's jitterPx, when it knows it, overrides both.
class JitterCentreEstimator {
  public:
    static constexpr int Window = 32;

    // The unjittered projection of a frustum (Quad Views' focus FOV for the frame, XrFovf angles through tan).
    struct Reference {
        bool valid{false};
        float sx{1.f}, sy{1.f}, cx{0.f}, cy{0.f};
        static Reference FromTangents(float left, float right, float up, float down) {
            Reference r;
            if (!(right - left > 1e-6f) || !(up - down > 1e-6f)) return r;
            r.sx = 2.f / (right - left);
            r.sy = 2.f / (up - down);
            r.cx = -(right + left) / (right - left);
            r.cy = -(up + down) / (up - down);
            r.valid = std::isfinite(r.sx) && std::isfinite(r.sy) && std::isfinite(r.cx) && std::isfinite(r.cy);
            return r;
        }
    };

    void Reset() {
        m_count = m_next = 0;
        m_sumX = m_sumY = 0.0;
    }

    // One frame's gProj with this frame's reference (or none) and one depth texel in NDC (2 / viewport w, 2 / h).
    void Add(const ProjectionParams& p, const Reference& ref, float texelX, float texelY) {
        if (m_count && (std::fabs(p.sx - m_sx) > 1e-4f * std::fabs(m_sx) || std::fabs(p.sy - m_sy) > 1e-4f * std::fabs(m_sy))) Reset();
        const bool useRef = ref.valid && std::fabs(ref.sx - p.sx) <= 1e-3f * std::fabs(p.sx) &&
                            std::fabs(ref.sy - p.sy) <= 1e-3f * std::fabs(p.sy) && std::fabs(p.cx - ref.cx) <= 2.f * texelX &&
                            std::fabs(p.cy - ref.cy) <= 2.f * texelY;
        if (m_count && useRef != m_useRef) Reset();
        const float ox = useRef ? ref.cx : 0.f, oy = useRef ? ref.cy : 0.f;
        if (m_count && !useRef &&
            (std::fabs(p.cx - m_sumX / m_count) > 1.25 * texelX || std::fabs(p.cy - m_sumY / m_count) > 1.25 * texelY))
            Reset();
        m_sx = p.sx;
        m_sy = p.sy;
        m_useRef = useRef;
        m_refX = ox;
        m_refY = oy;
        if (m_count == Window) {
            m_sumX -= m_x[m_next];
            m_sumY -= m_y[m_next];
        } else {
            ++m_count;
        }
        m_x[m_next] = p.cx - ox;
        m_y[m_next] = p.cy - oy;
        m_sumX += m_x[m_next];
        m_sumY += m_y[m_next];
        m_next = (m_next + 1) % Window;
        m_lastX = p.cx;
        m_lastY = p.cy;
    }

    // The centre for this frame's colour: the reference plus the mean residual (or the mean of c), or this frame's c
    // when the jitter is not in gProj.
    void Centre(float& cx, float& cy) const {
        if (!m_count || JitterAbsent()) {
            cx = m_lastX;
            cy = m_lastY;
            return;
        }
        cx = m_refX + float(m_sumX / m_count);
        cy = m_refY + float(m_sumY / m_count);
    }

    // A full window whose c (or residual) did not move (standard deviation under 1e-6 NDC).
    bool JitterAbsent() const {
        if (m_count < Window) return false;
        const double mx = m_sumX / m_count, my = m_sumY / m_count;
        double v = 0.0;
        for (int i = 0; i < m_count; ++i) v += (m_x[i] - mx) * (m_x[i] - mx) + (m_y[i] - my) * (m_y[i] - my);
        return std::sqrt(v / m_count) < 1e-6;
    }
    int Count() const { return m_count; }
    bool UsesReference() const { return m_count && m_useRef; }

  private:
    float m_x[Window]{}, m_y[Window]{};
    double m_sumX{0.0}, m_sumY{0.0};
    float m_sx{0.f}, m_sy{0.f}, m_lastX{0.f}, m_lastY{0.f}, m_refX{0.f}, m_refY{0.f};
    int m_count{0}, m_next{0};
    bool m_useRef{false};
};

enum class FarDepthPreset : int { Subtle = 0, Default = 1, Strong = 2, Near = 3 };
enum class FarDepthMode : int { Fused = 0, Compute = 1 };

// QV settings dcsvr_far_depth* (docs/FAR_DEPTH.md). A preset sets gamma and Z0 (never the gain, which stays a blend
// in [0, 1], so depth order is kept).
struct DisparitySettings {
    bool enabled{false};
    FarDepthPreset preset{FarDepthPreset::Default};
    float z0M{40.f};
    float gamma{0.35f};
    float nearGuardM{30.f};
    // The Near preset (world-IPD curve, DcsvrExtraDisparityRad): the world as if seen with worldIpdScale times the eye
    // distance, from nearGuardM out, the cockpit untouched; 0 selects the infinity-anchored curve (gamma, Z0).
    float worldIpdScale{0.f};
    float fadeStartM{5000.f};
    float fadeEndM{20000.f};
    float maxArcmin{3.f};      // per eye
    float innerDeg{10.f};      // fovea weight 1 ...
    float outerDeg{25.f};      // ... to 0
    float edgeLo{0.85f};       // edge window over the focus shape metric
    float edgeHi{0.97f};
    float clampTexels{0.9f};   // |shift| per field texel: keeps x -> q - s(x) a contraction (no holes, no fold-over)
    float saccadeStep{0.25f};  // gain change allowed per saccade onset
    float driftPerS{0.05f};    // and per second without one
    bool gated{true};          // false: the gain jumps to its target (A/B)
    int holdFrames{2};         // frames without depth bridged by the last field, reprojected (ReuseHomography)
    // Engine rule: HUD symbology, gunsight and GUI, drawn after the depth capture without depth, must not move. Without
    // the overlay coverage (CoverageMain: the engine's pre-overlay copy against the submitted image) they would take
    // the shift of whatever lies behind them, so depth without that copy counts as no depth unless this is turned off
    // (dcsvr_far_depth_allow_unmasked, live A/B only).
    bool requireCoverage{true};
    // CoverageMain: a pixel counts as drawn by the overlays above this difference of sRGB-encoded channels: 2 8-bit
    // steps. One step of rounding (a copy through a shader) reads up to 1.9 after the GPU's sRGB conversions (measured);
    // 3 steps and more always count, far below the faintest glow worth pinning.
    float overlayThreshold{2.f / 255.f};
    // A frame whose overlays changed more of the focus view than this share is logged: the submitted image is then not
    // the pre-overlay copy's pixel grid (a scaled or tone-mapped CopyToHMDPass), and the coverage pins nearly everything.
    float overlaySuspectShare{0.4f};
    FarDepthMode mode{FarDepthMode::Fused};
    int debug{0};              // 3: exaggerate (debugScale x the angle and its clamp) for sign and seam checks
    float debugScale{4.f};

    void ApplyPreset(FarDepthPreset p) {
        preset = p;
        worldIpdScale = 0.f;
        nearGuardM = 30.f;
        switch (p) {
        case FarDepthPreset::Subtle: gamma = 0.5f; z0M = 40.f; break;
        case FarDepthPreset::Strong: gamma = 0.25f; z0M = 35.f; break;
        case FarDepthPreset::Near: worldIpdScale = 1.4f; nearGuardM = 4.f; break;  // x1.4 from 4-12 m out
        default: gamma = 0.35f; z0M = 40.f; break;
        }
    }
};

// The constant buffer of dcsvr_depthwarp_cs.hlsl, in its order (rows of four floats).
struct DisparityConstants {
    float proj[4];      // sx, sy, c0x, c0y: the colour's unjittered projection (NDC = s * tan + c0)
    float depthMap[4];  // xy: c - c0 (the depth's jitter in NDC), z MinDepth, w 1 / (MaxDepth - MinDepth)
    float lin[4];       // P22, P23, P32, P33
    float viewport[4];  // the view's depth viewport in its texture: x, y, w, h (texels)
    float field[4];     // field width, height, 1 / width, 1 / height (= the viewport's size)
    float curve[4];     // rendered IPD (m), Z0 (m), gamma, near guard (m)
    float fade[4];      // fade start, fade end (m), max angle per eye (rad), clamp (field texels)
    float gaze[4];      // xyz unit gaze in this eye's frame (-Z forward); without a tracker the focus centre
    float baseline[4];  // xyz unit direction this eye's content moves toward (to the other eye), w unused
    float window[4];    // fovea inner, outer (deg), edge lo, hi (shape metric)
    float misc[4];      // x gain, y focus shape, z rho scale (1 fused, full-alpha share for the compute fallback), w FieldFlag bits
    float bounds[4];    // depth Loads clamp: first and last texel of the viewport, x0, y0, x1, y1
};
static_assert(sizeof(DisparityConstants) == 12 * 16, "DisparityConstants must match the shader's cbuffer");

// misc.w of DisparityConstants: which companions of the depth texture the field reads.
enum FieldFlag : uint32_t { FieldStencil = 1u, FieldCoverage = 2u };

// The constant buffer of CoverageMain (dcsvr_depthwarp_cs.hlsl, b2).
struct CoverageConstants {
    float field[4];  // field w, h, output pixels per field texel x, y
    float image[4];  // the submitted image's subImage offset x, y; the compared size w, h (output pixels)
    float misc[4];   // x threshold (sRGB steps / 255); y 1 for an HDR pair, z its absolute and w its relative threshold
};
static_assert(sizeof(CoverageConstants) == 3 * 16, "CoverageConstants must match the shader's cbuffer");

// CoverageMain's constants for a field of fieldW x fieldH over the focus image: the pre-overlay copy of w x h pixels
// (preOverlaySize) against the submitted subImage at (x, y) of extent subW x subH. False when they are not one pixel grid
// (the sizes differ): no coverage for this frame.
inline bool MakeCoverageConstants(uint32_t fieldW, uint32_t fieldH, uint32_t w, uint32_t h, int32_t x, int32_t y, int32_t subW,
                                  int32_t subH, float threshold, CoverageConstants& out) {
    out = {};
    if (!fieldW || !fieldH || !w || !h || int32_t(w) != subW || int32_t(h) != subH || x < 0 || y < 0) return false;
    out.field[0] = float(fieldW);
    out.field[1] = float(fieldH);
    out.field[2] = float(w) / float(fieldW);
    out.field[3] = float(h) / float(fieldH);
    // At most 8 output pixels per field texel along an axis (CoverageMain's loop bound).
    if (out.field[2] > 8.f || out.field[3] > 8.f) return false;
    out.image[0] = float(x);
    out.image[1] = float(y);
    out.image[2] = float(w);
    out.image[3] = float(h);
    out.misc[0] = threshold;
    return true;
}

// The pair's copies are HDR (a float target: the scene before the tonemap, where DCS draws HUD and MFD symbology after
// DLSS): compared linearly, a change counts above 1e-4 + 1 % of the larger value. Outside the symbology the two copies
// are of one frame and one target, so bit-identical: any threshold above rounding would do.
inline bool CoverageFormatIsHdr(uint32_t viewFormat) {
    return viewFormat == 10u /* DXGI_FORMAT_R16G16B16A16_FLOAT */ || viewFormat == 26u /* DXGI_FORMAT_R11G11B10_FLOAT */ ||
           viewFormat == 2u /* DXGI_FORMAT_R32G32B32A32_FLOAT */;
}
inline void SetCoverageHdr(CoverageConstants& k) {
    k.misc[1] = 1.f;
    k.misc[2] = 1e-4f;
    k.misc[3] = 0.01f;
}

// One eye's inputs to the field (FieldConstants): this frame's projection and the centre to align it to, the depth
// viewport (x, y, w, h, MinDepth, MaxDepth), the rendered IPD and the baseline in the eye's frame (toward the right
// eye), the gaze in the eye's frame (or none), the gain, the composition's focus shape and edge smoothing.
struct FieldInputs {
    ProjectionParams proj{};
    float c0x{0.f}, c0y{0.f};        // the unjittered centre: JitterCentreEstimator (Quad Views' focus FOV) or jitterPx
    float viewport[6]{0.f, 0.f, 1.f, 1.f, 0.f, 1.f};
    bool hasStencil{true};
    bool hasCoverage{false};         // CoverageMain's output on the field grid, bound at t2
    int eye{0};                      // 0 left, 1 right
    float ipdM{0.064f};
    float baseline[3]{1.f, 0.f, 0.f};
    bool gazeValid{false};
    float gaze[3]{0.f, 0.f, -1.f};
    float gain{1.f};
    float focusShape{0.f};
    float smoothing{0.2f};
};

inline void FieldConstants(const FieldInputs& in, const DisparitySettings& s, DisparityConstants& out) {
    out = {};
    out.proj[0] = in.proj.sx;
    out.proj[1] = in.proj.sy;
    out.proj[2] = in.c0x;
    out.proj[3] = in.c0y;
    out.depthMap[0] = in.proj.cx - in.c0x;
    out.depthMap[1] = in.proj.cy - in.c0y;
    const float range = in.viewport[5] - in.viewport[4];
    out.depthMap[2] = in.viewport[4];
    out.depthMap[3] = std::fabs(range) > 1e-6f ? 1.f / range : 1.f;
    out.lin[0] = in.proj.p22;
    out.lin[1] = in.proj.p23;
    out.lin[2] = in.proj.p32;
    out.lin[3] = in.proj.p33;
    for (int i = 0; i < 4; ++i) out.viewport[i] = in.viewport[i];
    const float fw = std::max(std::round(in.viewport[2]), 1.f), fh = std::max(std::round(in.viewport[3]), 1.f);
    out.field[0] = fw;
    out.field[1] = fh;
    out.field[2] = 1.f / fw;
    out.field[3] = 1.f / fh;
    out.curve[0] = in.ipdM;
    out.curve[1] = s.z0M;
    // The Near preset's world-IPD curve travels as a negative gamma (-(k - 1)).
    out.curve[2] = s.worldIpdScale > 1.f ? -std::min(s.worldIpdScale - 1.f, 1.f) : s.gamma;
    out.curve[3] = s.nearGuardM;
    const float scale = s.debug == 3 ? std::max(s.debugScale, 1.f) : 1.f;
    out.fade[0] = s.fadeStartM;
    out.fade[1] = s.fadeEndM;
    out.fade[2] = s.maxArcmin * ArcminRad * scale;
    // The exaggeration clamps at 0.6 texel: its silhouettes then converge in the backward solve like the real scale's
    // (a step of c between texels needs about log(0.01) / log(c) iterations).
    out.fade[3] = s.debug == 3 ? std::min(s.clampTexels, 0.6f) : s.clampTexels;
    if (in.gazeValid) {
        const float l = std::sqrt(in.gaze[0] * in.gaze[0] + in.gaze[1] * in.gaze[1] + in.gaze[2] * in.gaze[2]);
        for (int i = 0; i < 3; ++i) out.gaze[i] = l > 0.f ? in.gaze[i] / l : (i == 2 ? -1.f : 0.f);
    } else {
        // No tracker: the focus view's centre (NDC 0, 0 of the colour) stands for the gaze.
        const float tx = -in.c0x / in.proj.sx, ty = -in.c0y / in.proj.sy;
        const float l = std::sqrt(tx * tx + ty * ty + 1.f);
        out.gaze[0] = tx / l;
        out.gaze[1] = ty / l;
        out.gaze[2] = -1.f / l;
    }
    out.gaze[3] = in.gazeValid ? 1.f : 0.f;
    const float sign = in.eye == 0 ? 1.f : -1.f;  // the left eye's content toward the right eye, the right's toward the left
    const float bl = std::sqrt(in.baseline[0] * in.baseline[0] + in.baseline[1] * in.baseline[1] + in.baseline[2] * in.baseline[2]);
    for (int i = 0; i < 3; ++i) out.baseline[i] = bl > 0.f ? sign * in.baseline[i] / bl : (i == 0 ? sign : 0.f);
    out.window[0] = s.innerDeg;
    out.window[1] = s.outerDeg;
    out.window[2] = s.edgeLo;
    out.window[3] = s.edgeHi;
    out.misc[0] = std::clamp(in.gain, 0.f, 1.f) * scale;
    out.misc[1] = in.focusShape;
    out.misc[2] = s.mode == FarDepthMode::Compute ? DcsvrFocusFullAlphaScale(in.focusShape, in.smoothing) : 1.f;
    out.misc[3] = float((in.hasStencil ? FieldStencil : 0u) | (in.hasCoverage ? FieldCoverage : 0u));
    out.bounds[0] = std::floor(in.viewport[0]);
    out.bounds[1] = std::floor(in.viewport[1]);
    out.bounds[2] = std::floor(in.viewport[0]) + fw - 1.f;
    out.bounds[3] = std::floor(in.viewport[1]) + fh - 1.f;
}

// The forward shift (focus uv) the field shader computes at a colour position, before its texel clamp: the CPU
// reference of the tests and of the documentation's tables. range: metres along the ray, <= 0 for the sky.
inline void ReferenceShiftUv(const DisparityConstants& k, float u, float v, float range, bool cockpit, float& su, float& sv) {
    su = sv = 0.f;
    const float nx = 2.f * u - 1.f, ny = 1.f - 2.f * v;
    const float tx = (nx - k.proj[2]) / k.proj[0], ty = (ny - k.proj[3]) / k.proj[1];
    const float ecc = DcsvrEccentricityDeg(tx, ty, k.gaze[0], k.gaze[1], k.gaze[2]);
    const float w = DcsvrFoveaWeight(ecc, k.window[0], k.window[1]) *
                    DcsvrFocusEdgeWindow(nx, ny, k.misc[1], k.window[2], k.window[3], k.misc[2]);
    if (cockpit || w <= 0.f || k.misc[0] <= 0.f) return;
    const float dd = DcsvrExtraDisparityRad(range, k.curve[0], k.curve[1], k.curve[2], k.fade[0], k.fade[1], k.curve[3]);
    const float a = DcsvrEyeAngle(dd, k.misc[0], w, k.fade[2]);
    if (a <= 0.f) return;
    float ox, oy;
    DcsvrEpipolarShift(tx, ty, a, k.baseline[0], k.baseline[1], k.baseline[2], ox, oy);
    su = (ox - tx) * k.proj[0] * 0.5f;
    sv = -(oy - ty) * k.proj[1] * 0.5f;
}

// ---- Events from the pupil shift layer and the rendered eyes ------------------------------------------------------

// Saccade onsets and blinks from the pupil shift layer's 500 Hz PVR feed (DcsVr_QuadViewsGazeEvent): sticky counters
// the controller compares frame to frame, so an event between two frames is never lost.
struct GazeEventCounters {
    std::atomic<uint32_t> saccades{0}, blinks{0}, any{0};
};
inline GazeEventCounters& GazeEvents() {
    static GazeEventCounters counters;
    return counters;
}
enum GazeEventKind : uint32_t { GazeEventSaccade = 1u, GazeEventBlink = 2u };
inline void RecordGazeEvent(uint32_t kind) {
    if (kind == GazeEventSaccade) GazeEvents().saccades.fetch_add(1u, std::memory_order_relaxed);
    else if (kind == GazeEventBlink) GazeEvents().blinks.fetch_add(1u, std::memory_order_relaxed);
    else return;
    GazeEvents().any.store(1u, std::memory_order_relaxed);
}

// What the pupil shift layer rendered (DcsVr_QuadViewsRenderedEyes, at its xrLocateViews): the IPD DCS rendered with
// (Depth boost included) and each eye's unit baseline (toward the right eye) in that eye's own frame. Quad Views'
// own focus FOV is never used for the shift's size: pupil shift relabels FOVs and poses before Quad Views sees them.
struct RenderedEyes {
    int64_t displayTime{0};
    float ipdM{0.f};
    float baseline[2][3]{{1.f, 0.f, 0.f}, {1.f, 0.f, 0.f}};
};
class RenderedEyesStore {
  public:
    void Store(const RenderedEyes& e) {
        std::lock_guard lock(m_mutex);
        m_eyes = e;
    }
    RenderedEyes Load() const {
        std::lock_guard lock(m_mutex);
        return m_eyes;
    }

  private:
    mutable std::mutex m_mutex;
    RenderedEyes m_eyes{};
};
inline RenderedEyesStore& SharedRenderedEyes() {
    static RenderedEyesStore store;
    return store;
}

// The rendered IPD in priority order: pupil shift's push, else the engine's camera separation (when it is a plausible
// 50-90 mm times the Depth boost scale), else Quad Views' located views.
inline float ChooseRenderedIpd(float pushedM, float cameraSeparationM, float boostScale, float locatedM) {
    if (pushedM > 0.03f && pushedM < 0.2f) return pushedM;
    const float lo = 0.05f * std::max(boostScale, 0.5f), hi = 0.09f * std::max(boostScale, 0.5f);
    if (cameraSeparationM >= lo && cameraSeparationM <= hi) return cameraSeparationM;
    return locatedM > 0.03f && locatedM < 0.2f ? locatedM : 0.064f;
}

// ---- A held field across a short depth gap ------------------------------------------------------------------------

// The composition's reprojection of a field built for frame A to frame B (dcsvr_depthwarp_ps.hlsli, dcsvrReuse): rows
// 0-2 (xyz) map B's focus NDC to A's homogeneously, through both eyes' orientations (the head turns; the eye's travel
// over a frame or two is nothing at the far band's ranges) and both focus frustums (the gaze moves them); w of rows 0
// and 1 scales A's focus uv displacement into B's. Only for small changes: false when the spans differ by more than
// 1% or B's focus centre is not inside A's focus view, and the gap is then not bridged.
inline bool ReuseHomography(const Quat& eyeA, const TanFov& focusA, const Quat& eyeB, const TanFov& focusB, float h[3][4]) {
    const float sAx = 2.f / (focusA.right - focusA.left), sAy = 2.f / (focusA.up - focusA.down);
    const float sBx = 2.f / (focusB.right - focusB.left), sBy = 2.f / (focusB.up - focusB.down);
    const float cAx = -(focusA.right + focusA.left) * sAx / 2.f, cAy = -(focusA.up + focusA.down) * sAy / 2.f;
    const float cBx = -(focusB.right + focusB.left) * sBx / 2.f, cBy = -(focusB.up + focusB.down) * sBy / 2.f;
    if (!(std::fabs(sBx / sAx - 1.f) <= 0.01f) || !(std::fabs(sBy / sAy - 1.f) <= 0.01f)) return false;
    // M: B's eye frame into A's (columns: B's axes seen from A).
    const Quat rel = Multiply(Conjugate(eyeA), eyeB);
    float m[3][3];
    for (int c = 0; c < 3; ++c) {
        const float axis[3] = {c == 0 ? 1.f : 0.f, c == 1 ? 1.f : 0.f, c == 2 ? 1.f : 0.f};
        float v[3];
        Rotate(rel, axis, v);
        for (int r = 0; r < 3; ++r) m[r][c] = v[r];
    }
    // B's NDC -> B's ray: (n - c) / s, -1. A's ray -> A's NDC (homogeneous): (sA x - cA z, sA y - cA z, -z).
    const float kb[3][3] = {{1.f / sBx, 0.f, -cBx / sBx}, {0.f, 1.f / sBy, -cBy / sBy}, {0.f, 0.f, -1.f}};
    const float ka[3][3] = {{sAx, 0.f, -cAx}, {0.f, sAy, -cAy}, {0.f, 0.f, -1.f}};
    float mk[3][3]{}, out[3][3]{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k) mk[r][c] += m[r][k] * kb[k][c];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k) out[r][c] += ka[r][k] * mk[k][c];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            if (!std::isfinite(out[r][c])) return false;
    // B's focus centre must land inside A's focus view, in front of the eye.
    if (!(out[2][2] > 0.f)) return false;
    const float ax = out[0][2] / out[2][2], ay = out[1][2] / out[2][2];
    if (!(std::fabs(ax) < 1.f && std::fabs(ay) < 1.f)) return false;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) h[r][c] = out[r][c];
    h[0][3] = sBx / sAx;
    h[1][3] = sBy / sAy;
    h[2][3] = 0.f;
    return true;
}

// ---- Gain controller -----------------------------------------------------------------------------------------------

// Why skipped (per frame, both eyes alike): the reasons the statistics count.
enum class DisparitySkip : uint8_t { None, Off, Depth, Gain, Repeat };

struct DisparityFrameInput {
    int64_t displayTime{0};
    bool enabled{false};     // the setting, an opaque projection layer, not the debug focus view
    bool depthValid{false};  // DepthImport validated both focus views of this frame (with coverage when required)
    bool fieldReusable{false};  // both eyes' fields of the last warped frame are held and ReuseHomography accepts them
    bool tracker{false};     // an eye tracker supplies the gaze (saccades and blinks can hide a change)
    bool gazeValid{false};
    bool inSaccade{false};   // Quad Views' own detector, used until the pupil shift layer sends events
    uint32_t saccadeEvents{0}, blinkEvents{0};
    bool externalEvents{false};
    uint32_t sceneCuts{0};
};

struct DisparityDecision {
    bool warp{false};
    bool reuse{false};  // warp with the held fields, reprojected (no FieldMain this frame)
    float gain{0.f};
    float target{0.f};
    DisparitySkip skip{DisparitySkip::Off};
};

struct DisparityStats {
    uint64_t frames{0}, warped{0}, reused{0}, skipDepth{0}, skipGain{0}, saccadeSteps{0}, blinkSteps{0}, cutSteps{0};
};

// Far depth's state for the in-headset panel (DcsVr_QuadViewsDisparityStatus): bit 0 on, bit 1 warped last frame, bit 2
// depth valid last frame, bit 3 a held field bridged the frame, bits 8-15 gain x 255, bits 16-23 the last skip reason,
// bits 24-31 an update counter.
inline std::atomic<uint32_t>& DisparityStatus() {
    static std::atomic<uint32_t> status{0};
    return status;
}

// The extended panel status (DcsVr_QuadViewsDisparityStatus2), 64 bits: bit 0 on (enabled and not switched off by the
// key), bit 1 enabled in the settings, bit 2 the engine's depth valid this frame, bit 3 a held field bridged it, bit 4
// switched off by the key, bit 5 the overlay coverage ran this frame, bits 8-15 the gain x 100, bits 16-31 frames warped
// in the last second, bits 32-39 the coverage share in % (255 unknown), bits 40-47 frames held in the last second,
// bits 48-55 an update counter.
struct DisparityStatusWord {
    bool on{false}, enabled{false}, depthValid{false}, held{false}, offByKey{false}, coverage{false};
    float gain{0.f};
    uint32_t warpedPerSecond{0}, heldPerSecond{0}, coveragePercent{255};
};
inline uint64_t PackDisparityStatus2(const DisparityStatusWord& w, uint32_t counter) {
    return uint64_t(w.on) | uint64_t(w.enabled) << 1 | uint64_t(w.depthValid) << 2 | uint64_t(w.held) << 3 | uint64_t(w.offByKey) << 4 |
           uint64_t(w.coverage) << 5 | uint64_t(std::lround(std::clamp(w.gain, 0.f, 1.f) * 100.f)) << 8 |
           uint64_t(std::min(w.warpedPerSecond, 0xFFFFu)) << 16 | uint64_t(std::min(w.coveragePercent, 255u)) << 32 |
           uint64_t(std::min(w.heldPerSecond, 255u)) << 40 | uint64_t(counter & 0xFFu) << 48;
}
inline std::atomic<uint64_t>& DisparityStatus2() {
    static std::atomic<uint64_t> status{0};
    return status;
}
inline void PublishDisparityStatus2(const DisparityStatusWord& w) {
    static uint32_t counter = 0;
    DisparityStatus2().store(PackDisparityStatus2(w, ++counter), std::memory_order_relaxed);
}

// Once per submitted frame. The gain g (a blend between the natural and the target disparity) is what the viewer sees
// and moves toward its target (1 when enabled) only when a change cannot be seen: up to saccadeStep per saccade onset
// (saccadic suppression), all the way on a blink (80-300 ms without a gaze) or a scene cut, and at most driftPerS
// otherwise (20 s for the whole way without an eye tracker). The engine skips pushes it is unsure of, so a frame
// without depth must not show as a pop: up to holdFrames of them reuse the last frame's fields, reprojected, at the
// held gain (never across a scene cut). A longer gap shows the unwarped image, so g is 0 from then on and climbs again
// under the same rules once depth is back; a scene cut during the gap (a mission load stops the pushes around it) is
// kept for that first frame.
class DisparityController {
  public:
    DisparityDecision BeginFrame(const DisparityFrameInput& in, const DisparitySettings& s) {
        if (m_started && in.displayTime == m_time) return m_decision;  // a second projection layer: the same decision
        const float dt = m_started ? std::clamp((in.displayTime - m_time) / 1e9f, 0.f, 0.1f) : 0.f;
        m_invalidFrames = in.depthValid ? 0 : m_invalidFrames + 1;
        const float target = in.enabled ? 1.f : 0.f;

        // Events since the last frame.
        uint32_t saccades = 0, blinks = 0;
        if (in.tracker) {
            if (in.externalEvents) {
                saccades = m_started ? in.saccadeEvents - m_saccadeEvents : 0u;
                blinks = m_started ? in.blinkEvents - m_blinkEvents : 0u;
            } else {
                saccades = in.inSaccade && !m_inSaccade ? 1u : 0u;
                // A blink: the gaze lost for 80-300 ms, then back.
                if (!in.gazeValid && m_gazeValid) m_lostAt = in.displayTime;
                if (in.gazeValid && !m_gazeValid && m_lostAt) {
                    const int64_t lost = in.displayTime - m_lostAt;
                    if (lost >= 80'000'000 && lost <= 300'000'000) blinks = 1u;
                    m_lostAt = 0;
                }
            }
        }
        const bool cut = m_started && in.sceneCuts != m_sceneCuts;
        const bool reuse = in.enabled && !in.depthValid && m_haveField && in.fieldReusable && !cut && m_invalidFrames <= s.holdFrames;
        const bool shown = in.enabled && (in.depthValid || reuse);

        if (!shown) {
            // Unwarped this frame: g is what is seen, 0. A cut while disabled is no reason to jump when enabled.
            m_gain = 0.f;
            m_pendingCut = in.enabled && (m_pendingCut || cut);
        } else if (reuse) {
            // The held fields were built at this gain.
        } else if (!s.gated) {
            m_gain = target;
            m_pendingCut = false;
        } else {
            const bool fullCut = cut || m_pendingCut;
            float allowance = s.driftPerS * dt;
            if (fullCut || blinks) allowance = 1.f;
            else allowance += s.saccadeStep * std::min(saccades, 4u);
            if (fullCut) m_stats.cutSteps++;
            else if (blinks) m_stats.blinkSteps++;
            else if (saccades) m_stats.saccadeSteps++;
            m_gain = m_gain < target ? std::min(target, m_gain + allowance) : std::max(target, m_gain - allowance);
            m_pendingCut = false;
        }

        DisparityDecision d;
        d.gain = m_gain;
        d.target = target;
        if (!in.enabled) d.skip = DisparitySkip::Off;
        else if (!shown) d.skip = DisparitySkip::Depth;
        else if (!(m_gain > 0.f)) d.skip = DisparitySkip::Gain;
        else d.skip = DisparitySkip::None;
        d.warp = d.skip == DisparitySkip::None;
        d.reuse = d.warp && reuse;
        m_haveField = d.warp;  // fields built this frame, or the held ones still standing

        if (in.enabled) {
            m_stats.frames++;
            if (d.warp) m_stats.warped++;
            if (d.reuse) m_stats.reused++;
            else if (d.skip == DisparitySkip::Depth) m_stats.skipDepth++;
            else if (d.skip == DisparitySkip::Gain) m_stats.skipGain++;
        }
        m_saccadeEvents = in.saccadeEvents;
        m_blinkEvents = in.blinkEvents;
        m_inSaccade = in.inSaccade;
        m_gazeValid = in.gazeValid;
        m_sceneCuts = in.sceneCuts;
        m_time = in.displayTime;
        m_started = true;
        m_decision = d;
        ++m_updates;
        DisparityStatus().store((in.enabled ? 1u : 0u) | (d.warp ? 2u : 0u) | (in.depthValid ? 4u : 0u) | (d.reuse ? 8u : 0u) |
                                    (uint32_t)std::lround(std::clamp(m_gain, 0.f, 1.f) * 255.f) << 8 | (uint32_t)d.skip << 16 |
                                    (m_updates & 0xFFu) << 24,
                                std::memory_order_relaxed);
        return d;
    }

    float Gain() const { return m_gain; }
    const DisparityStats& Stats() const { return m_stats; }
    void ResetStats() { m_stats = {}; }

  private:
    DisparityDecision m_decision{};
    DisparityStats m_stats{};
    float m_gain{0.f};
    int m_invalidFrames{0};
    int64_t m_time{0}, m_lostAt{0};
    uint32_t m_saccadeEvents{0}, m_blinkEvents{0}, m_sceneCuts{0}, m_updates{0};
    bool m_inSaccade{false}, m_gazeValid{false}, m_started{false}, m_haveField{false}, m_pendingCut{false};
};

} // namespace dcsvr
#endif
#endif // DCSVR_DEPTHWARP_H
