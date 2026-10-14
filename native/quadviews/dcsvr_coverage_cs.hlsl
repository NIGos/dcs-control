// DCS Control: Far depth's overlay coverage (CoverageMain), the mask FieldMain (dcsvr_depthwarp_cs.hlsl) reads at t2;
// constants in dcsvr_depthwarp.h (CoverageConstants), tested by tests/depthwarp.
//
// Once per eye at xrEndFrame before FieldMain, when the engine pushed the focus view's colour as it was before the
// overlay passes (HAS_PRE_OVERLAY): marks every field texel whose colour pixels the overlays changed (the image DCS
// submitted against that copy, sRGB-encoded: above 2 display steps; for an HDR pair, a relative change) and dilates the marks by DCSVR_COVERAGE_DILATE
// texels, more than the 0.9-texel shift clamp, so no warped colour can land on or read from symbology. The overlays are
// drawn after DLSS at the output resolution and unjittered: the field's colour uv grid is theirs, no jitter to undo.
// Its own file: the layer's project builds one entry point per shader file.

#ifndef DCSVR_COVERAGE_DILATE
#define DCSVR_COVERAGE_DILATE 2
#endif
#define CTILE 16
#define CSPAN (CTILE + 2 * DCSVR_COVERAGE_DILATE)

cbuffer covcb : register(b2) {
    float4 covField;  // field w, h, output pixels per field texel x, y
    float4 covImage;  // the submitted image's offset x, y (its subImage), the compared size w, h (output pixels)
    float4 covMisc;   // x threshold (difference of sRGB-encoded channels); y 1 for HDR copies (linear, unbounded): a change
                      // above z + w * the larger value instead
};

Texture2D<float4> PreOverlay : register(t3);  // the engine's copy, (0, 0) = the view's top left
Texture2D<float4> Submitted : register(t4);   // the focus swapchain image DCS submitted
RWTexture2D<float> CoverageOut : register(u1);
RWByteAddressBuffer CoverageCount : register(u2);  // +0 changed texels before the dilation (diagnostics only)

groupshared uint gsChanged[CSPAN * CSPAN];
groupshared uint gsCount;

// sRGB encoding of linear colour (the views of 8-bit targets decode it; float targets hold linear too), so the
// threshold is in display steps: an 8-bit step is 1 / 255 at every level.
float3 DcsvrSrgbEncode(float3 v) {
    v = saturate(v);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

// Whether the overlays changed any output pixel of field texel f.
bool OverlayChanged(int2 f) {
    if (any(f < 0) || any(f >= int2(covField.xy))) return false;
    const int2 size = int2(covImage.zw);
    const int2 p0 = clamp(int2(floor(float2(f) * covField.zw)), int2(0, 0), size - 1);
    // The pixels whose left/top edge falls in the texel: a partition of the image (each pixel read once; the dilation
    // takes care of the neighbours).
    const int2 p1 = clamp(int2(floor(float2(f + 1) * covField.zw)) - 1, p0, min(p0 + 7, size - 1));
    bool changed = false;
    [loop] for (int y = p0.y; y <= p1.y; ++y) {
        [loop] for (int x = p0.x; x <= p1.x; ++x) {
            const float3 a = PreOverlay.Load(int3(x, y, 0)).rgb;
            const float3 b = Submitted.Load(int3(int2(x, y) + int2(covImage.xy), 0)).rgb;
            if (covMisc.y > 0.5) {
                // HDR (the scene before the tonemap): relative, so a stroke over a bright sky counts as much as one over
                // the ground. Not finite (garbage) counts as unchanged.
                const float3 d = abs(a - b);
                const bool3 over = d > covMisc.z + covMisc.w * max(abs(a), abs(b));
                changed = changed || (any(over) && all(d == d) && all(d < 1e30));
            } else {
                const float3 d = abs(DcsvrSrgbEncode(a) - DcsvrSrgbEncode(b));
                changed = changed || max(d.r, max(d.g, d.b)) > covMisc.x;
            }
        }
    }
    return changed;
}

[numthreads(CTILE, CTILE, 1)]
void CoverageMain(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID, uint index : SV_GroupIndex) {
    const int2 origin = int2(group.xy) * CTILE - DCSVR_COVERAGE_DILATE;
    if (index == 0) gsCount = 0;
    for (uint i = index; i < CSPAN * CSPAN; i += CTILE * CTILE) {
        gsChanged[i] = OverlayChanged(origin + int2(i % CSPAN, i / CSPAN)) ? 1u : 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    const int2 q = int2(group.xy) * CTILE + int2(local.xy);
    const bool inside = all(q < int2(covField.xy));
    uint covered = 0;
    [unroll] for (int j = 0; j < (2 * DCSVR_COVERAGE_DILATE + 1) * (2 * DCSVR_COVERAGE_DILATE + 1); ++j) {
        const int2 o = int2(j % (2 * DCSVR_COVERAGE_DILATE + 1), j / (2 * DCSVR_COVERAGE_DILATE + 1));
        covered |= gsChanged[uint(local.y + o.y) * CSPAN + uint(local.x + o.x)];
    }
    const uint self = gsChanged[uint(local.y + DCSVR_COVERAGE_DILATE) * CSPAN + uint(local.x + DCSVR_COVERAGE_DILATE)];
    if (inside && self) InterlockedAdd(gsCount, 1u);
    if (inside) CoverageOut[q] = covered ? 1.0 : 0.0;
    GroupMemoryBarrierWithGroupSync();
    if (index == 0 && gsCount) CoverageCount.InterlockedAdd(0, gsCount);
}
