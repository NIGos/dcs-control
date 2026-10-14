// DCS Control: Far depth's displacement field (FieldMain) and its compute fallback (WarpMain); maths and constants in
// dcsvr_depthwarp.h, tested by tests/depthwarp.
//
// FieldMain, once per eye at xrEndFrame: for each texel of the focus view's colour uv grid (at the depth viewport's
// density) the backward displacement b, in focus uv, that the composition (dcsvr_depthwarp_ps.hlsli) subtracts from
// both its focus and its periphery sample position: one field for both, so the two layers of an eye move alike and no
// seam can show inside the focus rectangle. The field reaches 0 at the focus edge (edge window).
//
//  1. Forward shift s(x) per texel, over the tile and a 2-texel apron: the depth of the colour position (re-aligned
//     to the jittered depth frustum), nearest of a 3x3 footprint (a sub-texel far aircraft keeps its own range and
//     its whole colour moves with it; any cockpit stencil in it pins the texel: the cockpit and canopy glass never
//     move, and the colour beside them never reads them; the overlay coverage built by CoverageMain, already dilated,
//     pins it the same way for HUD symbology, gunsight and GUI), the extra disparity of that
//     range, the fovea and edge weights, the turn of the ray along its epipolar line, clamped to c = 0.9 field texel.
//     All shifts of an eye point one way and lie in [0, c], so s is c-Lipschitz (bilinear between texels whose shifts
//     differ by at most c): x -> x + s(x) is monotonic with a positive Jacobian, no holes, no fold-over, and the
//     backward map below is a contraction with a unique solution.
//  2. Backward solve q = x + s(x) by fixed-point iterations x <- q - s(x) (s bilinear from groupshared); b = q - x.
//     Each step shrinks the error by L <= c, so after k steps it is at most L^k |s| and the k-th step at most c^k
//     texel. Inside a surface L is ~0 and 2-3 steps settle; at the real scale (|s| ~0.3 texel) a silhouette has
//     L ~0.3 and needs about 5, but one whose shift sits at the 0.9 clamp against the sky has L = 0.9, where 3 steps
//     stop up to 0.34 texel short. The loop therefore runs until a step is under 1/128 texel, at most
//     DCSVR_FIELD_ITERATIONS: 16 leave at most 0.9^17 = 0.17 texel there (0.09 measured; the debug exaggeration
//     clamps at 0.6 texel, where 16 steps converge). If the last step still moved more than 0.25 texel (at most
//     0.9^16 = 0.19 under the 0.9 clamp: a guard for other settings) the farther candidate wins: a disocclusion is
//     filled from the background, never by stretching the foreground.
//
// The overlay coverage FieldMain reads at t2 is CoverageMain's (dcsvr_coverage_cs.hlsl).
//
// WarpMain (setting dcsvr_far_depth_mode=compute, only if the fused route is rejected): warps the focus image itself
// with a Catmull-Rom fetch clamped to the bilinear footprint's range (no ringing), passing pixels that move less than
// 1/64 px through untouched. Its field is built with the full-alpha window (misc.z), so it is 0 wherever the
// composition blends the focus with the (unwarped) periphery.

#include "dcsvr_depthwarp.h"

cbuffer cb : register(b0) {
    float4 proj;      // sx, sy, c0x, c0y: colour NDC = s * tan + c0 (c0 unjittered, this frame's frustum)
    float4 depthMap;  // xy depth's jitter (c - c0, NDC), z MinDepth, w 1 / (MaxDepth - MinDepth)
    float4 lin;       // P22, P23, P32, P33
    float4 viewport;  // depth viewport x, y, w, h (texels)
    float4 field;     // field w, h, 1 / w, 1 / h
    float4 curve;     // IPD (m), Z0 (m), gamma, near guard (m)
    float4 fade;      // fade start, end (m), max angle per eye (rad), clamp (field texels)
    float4 gaze;      // xyz unit gaze in this eye (-Z forward)
    float4 baseline;  // xyz unit direction this eye's content moves toward
    float4 window;    // fovea inner, outer (deg), edge lo, hi
    float4 misc;      // x gain, y focus shape, z rho scale, w flags (1 stencil, 2 overlay coverage)
    float4 bounds;    // first and last depth texel of the viewport
};

cbuffer warp : register(b1) {
    float4 colourSize;  // WarpMain: focus image width, height, 1 / width, 1 / height
};

Texture2D<float> Depth : register(t0);
Texture2D<uint2> Stencil : register(t1);
Texture2D<float> Coverage : register(t2);  // CoverageMain's output on the field grid: non-zero pins
RWTexture2D<float2> Field : register(u0);

#define TILE 16
#define APRON 2
#define SPAN (TILE + 2 * APRON)

groupshared float2 gsShift[SPAN * SPAN];  // forward shift, field texels
groupshared float gsDepth[SPAN * SPAN];   // the footprint's nearest NDC depth, for the farther-candidate rule

// By its bits: the compiler may fold a NaN comparison away.
bool DcsvrFinite(float x) {
    return (asuint(x) & 0x7f800000u) != 0x7f800000u;
}

// The depth footprint of a field texel: 3 (default) the texel holding the depth position and its 8 neighbours, a
// symmetric one-texel dilation of every nearer object, so an aircraft's colour edges (and DLSS's re-aligned jitter)
// always fall inside its own shift; 2 the 2x2 texels around the position (half the loads, a one-sided dilation).
#ifndef DCSVR_FOOTPRINT
#define DCSVR_FOOTPRINT 3
#endif
#ifndef DCSVR_FIELD_ITERATIONS
#define DCSVR_FIELD_ITERATIONS 16
#endif

// Nearest NDC depth (reverse-Z: the largest) of the footprint around the depth NDC position nd; pinned when any of
// its texels carries the cockpit composition. Garbage (not finite) counts as the sky: nothing
// moves for it.
float FootprintDepth(float2 nd, out bool pinned) {
    const float2 p = viewport.xy + (nd * float2(0.5, -0.5) + 0.5) * viewport.zw;
#if DCSVR_FOOTPRINT == 3
    const int2 i0 = int2(floor(p)) - 1;
#else
    const int2 i0 = int2(floor(p - 0.5));
#endif
    const uint flags = uint(misc.w);
    float nearest = 0;
    pinned = false;
    [unroll] for (int j = 0; j < DCSVR_FOOTPRINT * DCSVR_FOOTPRINT; ++j) {
        const int2 t = clamp(i0 + int2(j % DCSVR_FOOTPRINT, j / DCSVR_FOOTPRINT), int2(bounds.xy), int2(bounds.zw));
        float d = (Depth.Load(int3(t, 0)) - depthMap.z) * depthMap.w;
        if (!DcsvrFinite(d)) d = 0;
        nearest = max(nearest, d);
        if (flags & 1u) pinned = pinned || DcsvrStencilIsCockpit(Stencil.Load(int3(t, 0)).y);
    }
    return nearest;
}

// Forward shift (field texels) of the content at field texel x; 0 outside the field.
float2 ForwardShift(int2 x, out float ndcDepth) {
    ndcDepth = 0;
    if (any(x < 0) || any(x >= int2(field.xy))) return 0;
    const float2 uv = (float2(x) + 0.5) * field.zw;
    const float2 n = float2(2 * uv.x - 1, 1 - 2 * uv.y);
    const float2 t = (n - proj.zw) / proj.xy;
    const float ecc = DcsvrEccentricityDeg(t.x, t.y, gaze.x, gaze.y, gaze.z);
    const float w = DcsvrFoveaWeight(ecc, window.x, window.y) * DcsvrFocusEdgeWindow(n.x, n.y, misc.y, window.z, window.w, misc.z);
    [branch] if (!(w * misc.x > 0)) return 0;
    bool pinned;
    ndcDepth = FootprintDepth(n + depthMap.xy, pinned);
    if ((uint(misc.w) & 2u) && Coverage.Load(int3(x, 0)) > 0) pinned = true;
    if (pinned) return 0;
    const float range = DcsvrRangeFromForward(DcsvrForwardFromDepth(ndcDepth, lin.x, lin.y, lin.z, lin.w), t.x, t.y);
    const float a = DcsvrEyeAngle(DcsvrExtraDisparityRad(range, curve.x, curve.y, curve.z, fade.x, fade.y, curve.w), misc.x, w, fade.z);
    [branch] if (!(a > 0)) return 0;
    float ox, oy;
    DcsvrEpipolarShift(t.x, t.y, a, baseline.x, baseline.y, baseline.z, ox, oy);
    float2 s = float2((ox - t.x) * proj.x * 0.5, -(oy - t.y) * proj.y * 0.5) * field.xy;
    const float l = length(s);
    if (l > fade.w) s *= fade.w / l;
    return DcsvrFinite(s.x) && DcsvrFinite(s.y) ? s : 0;
}

// s bilinear at x (field texel units, centres at i + 0.5) from the tile's groupshared copy.
float2 SampleShift(float2 x, int2 origin) {
    const float2 l = x - 0.5 - float2(origin);
    const int2 i = clamp(int2(floor(l)), int2(0, 0), int2(SPAN - 2, SPAN - 2));
    const float2 f = saturate(l - float2(i));
    const uint k = uint(i.y) * SPAN + uint(i.x);
    return lerp(lerp(gsShift[k], gsShift[k + 1], f.x), lerp(gsShift[k + SPAN], gsShift[k + SPAN + 1], f.x), f.y);
}

float NearestDepth(float2 x, int2 origin) {
    const int2 i = clamp(int2(floor(x - float2(origin))), int2(0, 0), int2(SPAN - 1, SPAN - 1));
    return gsDepth[uint(i.y) * SPAN + uint(i.x)];
}

[numthreads(TILE, TILE, 1)]
void FieldMain(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID, uint index : SV_GroupIndex) {
    const int2 origin = int2(group.xy) * TILE - APRON;
    for (uint i = index; i < SPAN * SPAN; i += TILE * TILE) {
        float d;
        gsShift[i] = ForwardShift(origin + int2(i % SPAN, i / SPAN), d);
        gsDepth[i] = d;
    }
    GroupMemoryBarrierWithGroupSync();
    const int2 q = int2(group.xy) * TILE + int2(local.xy);
    if (any(q >= int2(field.xy))) return;
    const float2 qc = float2(q) + 0.5;
    float2 x = qc, previous = qc;
    [loop] for (int k = 0; k < DCSVR_FIELD_ITERATIONS; ++k) {
        previous = x;
        x = qc - SampleShift(x, origin);
        if (dot(x - previous, x - previous) < 1.0 / (128.0 * 128.0)) break;
    }
    if (dot(x - previous, x - previous) > 0.0625 && NearestDepth(previous, origin) < NearestDepth(x, origin)) x = previous;
    Field[q] = (qc - x) * field.zw;
}

// ---- Compute fallback --------------------------------------------------------------------------------------------

Texture2D<float4> Colour : register(t0);
Texture2D<float2> FieldIn : register(t1);
RWTexture2D<float4> Warped : register(u0);
SamplerState LinearClamp : register(s0);

[numthreads(8, 8, 1)]
void WarpMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= uint2(colourSize.xy))) return;
    const float2 uv = (float2(id.xy) + 0.5) * colourSize.zw;
    const float2 b = FieldIn.SampleLevel(LinearClamp, uv, 0);
    [branch] if (all(abs(b * colourSize.xy) < 1.0 / 64.0)) {
        Warped[id.xy] = Colour.Load(int3(id.xy, 0));
        return;
    }
    // Catmull-Rom in five bilinear taps at uv - b, clamped to the range of the four texels around it.
    const float2 position = (uv - b) * colourSize.xy;
    const float2 center = floor(position - 0.5) + 0.5;
    const float2 f = position - center;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 t0 = (center - 1.0) * colourSize.zw;
    const float2 t3 = (center + 2.0) * colourSize.zw;
    const float2 t12 = (center + w2 / w12) * colourSize.zw;
    float4 c = Colour.SampleLevel(LinearClamp, float2(t12.x, t0.y), 0) * (w12.x * w0.y);
    c += Colour.SampleLevel(LinearClamp, float2(t0.x, t12.y), 0) * (w0.x * w12.y);
    c += Colour.SampleLevel(LinearClamp, float2(t12.x, t12.y), 0) * (w12.x * w12.y);
    c += Colour.SampleLevel(LinearClamp, float2(t3.x, t12.y), 0) * (w3.x * w12.y);
    c += Colour.SampleLevel(LinearClamp, float2(t12.x, t3.y), 0) * (w12.x * w3.y);
    c /= w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    const int2 i0 = clamp(int2(center - 0.5), int2(0, 0), int2(colourSize.xy) - 2);
    const float4 a = Colour.Load(int3(i0, 0)), b1 = Colour.Load(int3(i0 + int2(1, 0), 0));
    const float4 c2 = Colour.Load(int3(i0 + int2(0, 1), 0)), d = Colour.Load(int3(i0 + int2(1, 1), 0));
    Warped[id.xy] = clamp(c, min(min(a, b1), min(c2, d)), max(max(a, b1), max(c2, d)));
}
