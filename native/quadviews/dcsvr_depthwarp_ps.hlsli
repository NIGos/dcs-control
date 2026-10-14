// DCS Control: Far depth in the composition (included by ProjectionPS.hlsl; field from dcsvr_depthwarp_cs.hlsl).
// Both sample positions of a composited pixel, the focus image's and the periphery's, move by the same backward
// displacement b (focus uv, scaled into periphery uv by the ratio of the two fields of view), so the eye's two layers
// stay aligned through the composition's blend and no seam can appear: the warp costs no extra resample and no extra
// target. Outside the focus rectangle the field is never fetched (and is 0 at its edge anyway). With warp.x == 0 the
// arithmetic is the upstream one, bit for bit.
//
// A frame without the engine's depth (a skipped push) reuses the last frame's field instead of popping back unwarped:
// warp.w == 1, and reuse (rows of a 3x3 map, xyz; dcsvr::ReuseHomography) takes this frame's focus NDC to the field's,
// across the head's turn and the focus frustum's move since; reuse[0].w, reuse[1].w scale the field's displacement
// into this frame's focus uv. Outside the field's view the clamp sampler reads its edge, which is 0.

// warp: x 1 when a field is valid this frame, yz focus uv -> periphery uv (focus tan span / periphery tan span), w 1
// when the field is held from an earlier frame.
void DcsvrApplyDisparity(float4 warp, float4 reuse[3], Texture2D<float2> field, SamplerState fieldSampler, inout float2 focusNdc,
                         inout float2 peripheryUv) {
    [branch] if (warp.x > 0 && all(abs(focusNdc) < 1)) {
        float2 fieldNdc = focusNdc;
        float2 scale = 1;
        [branch] if (warp.w > 0) {
            const float3 p = float3(focusNdc, 1);
            const float3 h = float3(dot(reuse[0].xyz, p), dot(reuse[1].xyz, p), dot(reuse[2].xyz, p));
            fieldNdc = h.xy / h.z;
            scale = float2(reuse[0].w, reuse[1].w);
        }
        const float2 b = field.SampleLevel(fieldSampler, fieldNdc * float2(0.5, -0.5) + 0.5, 0) * scale;
        focusNdc -= b * float2(2, -2);
        peripheryUv -= b * warp.yz;
    }
}
