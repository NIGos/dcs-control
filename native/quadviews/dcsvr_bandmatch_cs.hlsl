// DCS Control: band match, one eye's measurement and gain step (dcsvr_bandmatch.h says what and why; copied into
// Quad-Views-Foveated by scripts/build-quadviews.ps1, compiled with the layer's other shaders; tested by
// tests/quadviews-composition). One group of 16 x 16 threads per eye and frame, before the focus pass of that eye.

#include "dcsvr_depthwarp.h"  // DcsvrFocusBlendWindow: the composition's fade, exactly
#include "dcsvr_bandmatch.h"

cbuffer cb : register(b0) {
    float4 shape;   // x edge smoothing, y focus_view_shape, z k (periphery texel in focus texels), w 1 per channel
    float4 band;    // focus uv -> periphery uv: xy scale, zw offset
    float4 params;  // x rate, y limit, z 1: snap
    uint4 slot;     // x this eye's byte offset in the state
};

Texture2D<float4> Focus : register(t0);      // the focus image the focus pass reads (sharpened)
Texture2D<float4> Periphery : register(t1);  // the periphery image the periphery layer shows
SamplerState LinearClamp : register(s0);
RWByteAddressBuffer State : register(u0);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

groupshared float4 gFocus[256];      // weighted ln(focus + e): rgb, luminance
groupshared float4 gPeriphery[256];  // weighted ln(periphery + e): rgb, luminance
groupshared float2 gWeight[256];     // x weight, y weighted periphery luminance level

// The focus colour at the periphery's resolution: the edge match's [1 2 1] tent of bilinear taps at its full width.
float3 FocusAtPeripheryResolution(float2 uv, float2 size) {
    if (shape.z <= 1) {
        return Focus.SampleLevel(LinearClamp, uv, 0).rgb;
    }
    const float2 stepUv = shape.z * 0.57735 / size;
    const float2 lo = 0.5 / size, hi = 1 - lo;
    float3 sum = 0;
    [unroll] for (int j = -1; j <= 1; ++j) {
        [unroll] for (int i = -1; i <= 1; ++i) {
            sum += (2 - abs(i)) * (2 - abs(j)) * Focus.SampleLevel(LinearClamp, clamp(uv + float2(i, j) * stepUv, lo, hi), 0).rgb;
        }
    }
    return sum / 16;
}

[numthreads(16, 16, 1)]
void main(uint3 thread : SV_GroupThreadID) {
    float2 size;
    Focus.GetDimensions(size.x, size.y);
    float4 f = 0, p = 0;
    float2 w = 0;
    [loop] for (uint j = 0; j < (uint)kDcsvrBandGrid / 16; ++j) {
        [loop] for (uint i = 0; i < (uint)kDcsvrBandGrid / 16; ++i) {
            const float2 uv = (float2(thread.xy + uint2(i, j) * 16) + 0.5) / kDcsvrBandGrid;
            const float alpha = DcsvrFocusBlendWindow(uv.x * 2 - 1, 1 - uv.y * 2, shape.y, shape.x);
            const float weight = DcsvrBandWeight(alpha);
            if (weight <= 0) {
                continue;
            }
            const float3 focus = max(FocusAtPeripheryResolution(uv, size), 0);
            const float3 periphery = max(Periphery.SampleLevel(LinearClamp, uv * band.xy + band.zw, 0).rgb, 0);
            const float yf = dot(focus, kLuma), yp = dot(periphery, kLuma);
            f += weight * log(float4(focus, yf) + kDcsvrBandEpsilon);
            p += weight * log(float4(periphery, yp) + kDcsvrBandEpsilon);
            w += float2(weight, weight * yp);
        }
    }
    const uint index = thread.y * 16 + thread.x;
    gFocus[index] = f;
    gPeriphery[index] = p;
    gWeight[index] = w;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 128; stride > 0; stride >>= 1) {
        if (index < stride) {
            gFocus[index] += gFocus[index + stride];
            gPeriphery[index] += gPeriphery[index + stride];
            gWeight[index] += gWeight[index + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (index != 0) {
        return;
    }
    const uint at = slot.x;
    const float total = gWeight[0].x;
    // Too little band, or too dark a periphery: the gain stays, the frame is counted as skipped.
    if (total < kDcsvrBandMinWeight || gWeight[0].y < kDcsvrBandMinLevel * total) {
        State.Store(at + 44, State.Load(at + 44) + 1);
        return;
    }
    const float4 meanLogRatio = (gPeriphery[0] - gFocus[0]) / total;
    const float4 ratio = exp(meanLogRatio);
    const bool perChannel = shape.w > 0;
    const float limit = params.y;
    const float3 target = perChannel ? float3(DcsvrBandTarget(meanLogRatio.x, limit), DcsvrBandTarget(meanLogRatio.y, limit),
                                              DcsvrBandTarget(meanLogRatio.z, limit))
                                     : DcsvrBandTarget(meanLogRatio.w, limit).xxx;
    const uint4 state = State.Load4(at);
    const bool snap = state.w != 1 || params.z > 0;
    const float3 previous = asfloat(state.xyz);
    const float3 gain = float3(DcsvrBandStep(previous.x, target.x, params.x, snap), DcsvrBandStep(previous.y, target.y, params.x, snap),
                               DcsvrBandStep(previous.z, target.z, params.x, snap));
    State.Store4(at, uint4(asuint(gain), 1));
    State.Store4(at + 16, asuint(ratio));
    State.Store3(at + 32, uint3(asuint(total), asuint(exp(gPeriphery[0].w / total)), State.Load(at + 40) + 1));
}
