// DCS Control: periphery stability (copied into Quad-Views-Foveated by scripts/build-quadviews.ps1, compiled with
// the layer's other shaders; tested by tests/perception). One eye's periphery at its own resolution, after the
// flatten and before the periphery contrast pass: the previous result, reprojected by the head's rotation and clipped
// to this frame's 3x3 neighbourhood, blended in with a time-based weight that is zero inside the focus area and
// ramps up outside it (dcsvr_perception.h). The output is the next frame's history; the contrast pass reads it and
// writes its own texture, so sharpening never feeds back into the history.

#include "dcsvr_perception.h"

cbuffer cb : register(b0) {
    float4 reproject0; // rows: a ray of this frame's eye -> the previous frame's eye
    float4 reproject1;
    float4 reproject2;
    float4 fov;        // tan left, right, up, down of this frame's periphery
    float4 prevFov;    // the same for the history
    float4 gaze;       // xyz unit gaze in this eye, w 1 when valid
    float4 focus;      // xyz unit centre of the focus area, w its radius (deg)
    float4 params;     // x frame interval (s), y time constant (s; 0: no history), z ramp (deg), w clip (sd)
    float4 params2;    // x far start (deg), y far end (deg), z confidence (sd), w debug
    float4 size;       // width, height, 1 / width, 1 / height
    float4 focusTan;   // tan left, right, up, down of the focus view
    float4 focusShape; // x, y its angular half-widths (deg), z focus_view_shape
};

Texture2D<float4> Current : register(t0);
Texture2D<float4> History : register(t1);
RWTexture2D<float4> Output : register(u0);
SamplerState LinearClamp : register(s0);

float3 ToYCoCg(float3 c) {
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 FromYCoCg(float3 c) {
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// Catmull-Rom in five bilinear taps: a moving head resamples the history every frame, and a bilinear resample would
// soften it a little more each time.
float3 SampleHistory(float2 uv) {
    const float2 position = uv * size.xy;
    const float2 center = floor(position - 0.5) + 0.5;
    const float2 f = position - center;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 t0 = (center - 1.0) * size.zw;
    const float2 t3 = (center + 2.0) * size.zw;
    const float2 t12 = (center + w2 / w12) * size.zw;
    float3 c = History.SampleLevel(LinearClamp, float2(t12.x, t0.y), 0).rgb * (w12.x * w0.y);
    c += History.SampleLevel(LinearClamp, float2(t0.x, t12.y), 0).rgb * (w0.x * w12.y);
    c += History.SampleLevel(LinearClamp, float2(t12.x, t12.y), 0).rgb * (w12.x * w12.y);
    c += History.SampleLevel(LinearClamp, float2(t3.x, t12.y), 0).rgb * (w3.x * w12.y);
    c += History.SampleLevel(LinearClamp, float2(t12.x, t3.y), 0).rgb * (w12.x * w3.y);
    const float sum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(c / sum, 0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)size.x || id.y >= (uint)size.y) {
        return;
    }
    const int2 p = int2(id.xy);
    const float4 current = Current.Load(int3(p, 0));
    const float2 uv = (float2(p) + 0.5) * size.zw;
    const float tx = lerp(fov.x, fov.y, uv.x);
    const float ty = lerp(fov.z, fov.w, uv.y);
    const float focusDeg = DcsvrEccentricityDeg(tx, ty, focus.x, focus.y, focus.z);
    const float eccentricityDeg = gaze.w > 0 ? DcsvrEccentricityDeg(tx, ty, gaze.x, gaze.y, gaze.z) : focusDeg;
    // Zero up to the focus edge the composition draws, ramping up beyond it.
    const float edgeDeg = DcsvrFocusEdgeDeg(tx, ty, focusTan.x, focusTan.y, focusTan.z, focusTan.w, focusShape.x, focusShape.y, focusShape.z);
    const float weight = DcsvrStabilityWeight(focus.w + edgeDeg, focus.w, params.z, eccentricityDeg, params2.x, params2.y);
    float4 result = current;
    if (params2.w > 0) {
        // Debug view: where and how strongly it would act, no filtering.
        result.rgb = lerp(current.rgb, float3(1, 0, 0), 0.5 * weight);
        Output[p] = result;
        return;
    }
    float blend = DcsvrHistoryBlend(params.x, params.y, weight);
    [branch] if (blend > 1.0 / 512.0) {
        const float3 ray = float3(tx, ty, -1.0);
        const float3 previous = float3(dot(reproject0.xyz, ray), dot(reproject1.xyz, ray), dot(reproject2.xyz, ray));
        const float2 tanPrevious = previous.xy / max(-previous.z, 1e-6);
        const float2 uvPrevious = float2((tanPrevious.x - prevFov.x) / (prevFov.y - prevFov.x),
                                         (tanPrevious.y - prevFov.z) / (prevFov.w - prevFov.z));
        [branch] if (previous.z < 0 && all(uvPrevious >= 0.0) && all(uvPrevious <= 1.0)) {
            const int2 last = int2(size.xy) - 1;
            float3 m1 = 0, m2 = 0;
            [unroll] for (int y = -1; y <= 1; ++y) {
                [unroll] for (int x = -1; x <= 1; ++x) {
                    const float3 c = ToYCoCg(Current.Load(int3(clamp(p + int2(x, y), int2(0, 0), last), 0)).rgb);
                    m1 += c;
                    m2 += c * c;
                }
            }
            const float3 mean = m1 / 9.0;
            const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
            // The box: mean +- clip standard deviations, widened to hold this pixel itself, so a still pixel keeps
            // its own history (a thin bright line is an outlier in its neighbourhood, and must not be dimmed).
            const float3 own = ToYCoCg(current.rgb);
            const float3 history = ToYCoCg(SampleHistory(uvPrevious));
            const float3 clipped = clamp(history, min(own, mean - params.w * sigma), max(own, mean + params.w * sigma));
            blend *= DcsvrClipConfidence(abs(history.x - clipped.x), sigma.x, params2.z);
            if (all(isfinite(clipped))) {
                result.rgb = max(FromYCoCg(lerp(own, clipped, blend)), 0.0);
            }
        }
    }
    Output[p] = result;
}
