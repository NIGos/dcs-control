// DCSVR two-layer submission (dcsvr_two_layers=1): the output pass of one layer of one eye.
//
// Instead of composing the periphery and the focus into one image of the runtime's size, Quad Views hands the
// compositor two projection layers per eye and the compositor does the enlargement and the cross-fade. This shader
// writes each of them at the view's own size. It runs with ProjectionVS: texcoord is the uv over the whole target and,
// in the periphery pass (the target covers the eye's whole field of view, as the composed image does),
// projectedFocusCoord is the focus view's projection of the pixel, exactly as the composition gets it.
//
//  focusLayer = 0, the periphery: the colour as it is, alpha 1. The target has the application's format, so the
//                  render target view does any sRGB encoding.
//  focusLayer = 1, the focus: the colour NOT premultiplied, alpha = the fade of the composition exactly
//                  (ProjectionPS.hlsl: the rounded shape fading to exactly 0 at r = 1, or upstream's rectangle). The
//                  target is R8G8B8A8_UNORM, not sRGB: the Pimax compositor blends the encoded values, and an sRGB
//                  focus layer shows as a ring. The bytes must still hold sRGB-encoded colour, and the source is read
//                  linear when the application's swapchain is sRGB (and the sharpened texture is linear float then):
//                  encodeSrgb = 1 applies the transfer function here. With a UNORM application swapchain the source
//                  already holds encoded values: encodeSrgb = 0.
//
// Without Far depth every texel is a Load at the pixel (1:1, no filtering).
//
// No edge match here (dcsvr_edge_match, patches/quadviews/0019-edge-match.patch): it needs the periphery's resolution
// beside the focus image's, and in this mode the compositor enlarges the periphery after this pass. The focus layer
// keeps its full detail up to the fade, as the composition does with edge match off.
//
// Far depth (dcsvrWarp.x = 1, the field at t2): the composition moves its two samples of an output pixel, the focus
// image's and the periphery's, by one backward displacement b read from the field at the pixel's focus position
// (dcsvr_depthwarp_ps.hlsli, the same function and the same constants here), and it takes the fade at the moved focus
// position. The two passes do the same, each on its own pixel grid: the focus pass reads the field at its pixel,
// samples the focus image at the moved position and takes the fade there; the periphery pass, inside the focus
// rectangle only, reads the field at the pixel's focus position and samples the periphery b * dcsvrWarp.yz away. Both
// layers so carry the same displaced scene where the compositor cross-fades them. A pixel the field does not move
// stays a Load.
//
// dcsvrDebug.x = 1 (debug_focus_view): the periphery is black, so only the focus shows, as the composition's debug
// output. dcsvrDebug.y = 1: where the fade is 0 the focus colour comes from the unsharpened focus image at t1 instead
// of the sharpened one, whose tiles outside the rounded shape the sharpening skips (black, or stale after a change of
// shape). The compositor never shows those texels (alpha 0, and a bilinear tap never mixes one into a visible texel:
// the skip keeps a 2 texel margin), but anything that moves colour without its alpha (frame generation) may.

cbuffer ConstantBuffer : register(b0) {
    float smoothingArea;
    float focusViewShape;
    float focusLayer;
    float encodeSrgb;
    float4 dcsvrWarp; // as ProjectionPS.hlsl: x on, yz focus uv -> periphery uv, w a held field
    float4 dcsvrReuse[3]; // a held field's reprojection
    float4 dcsvrDebug; // x the periphery black, y the focus colour outside the fade from t1
};

SamplerState sourceSampler : register(s0);
Texture2D source : register(t0);
Texture2D plainFocus : register(t1);
Texture2D<float2> dcsvrDisparityField : register(t2);
#include "dcsvr_depthwarp_ps.hlsli"

float3 DcsvrLinearToSrgb(float3 c) {
    c = saturate(c);
    float3 low = c * 12.92;
    float3 high = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return float3(c.r <= 0.0031308 ? low.r : high.r, c.g <= 0.0031308 ? low.g : high.g, c.b <= 0.0031308 ? low.b : high.b);
}

float4 main(in float4 position : SV_POSITION, in float2 texcoord : PROJ_COORD0, in float3 projectedFocusCoord : PROJ_COORD1) : SV_TARGET {
    if (focusLayer == 0) {
        if (dcsvrDebug.x != 0) {
            return float4(0, 0, 0, 1);
        }
        float2 peripheryUv = texcoord;
        [branch] if (dcsvrWarp.x > 0) {
            float2 focusNdc = projectedFocusCoord.xy / projectedFocusCoord.z;
            DcsvrApplyDisparity(dcsvrWarp, dcsvrReuse, dcsvrDisparityField, sourceSampler, focusNdc, peripheryUv);
        }
        float4 periphery;
        [branch] if (any(peripheryUv != texcoord)) {
            periphery = source.SampleLevel(sourceSampler, peripheryUv, 0);
        } else {
            periphery = source.Load(int3(position.xy, 0));
        }
        return float4(periphery.rgb, 1);
    }

    // The fade, as ProjectionPS.hlsl computes it from the focus view's NDC and uv of the output pixel (after Far
    // depth's displacement, as there).
    float2 layer1ProjectedCoordNdc = texcoord * float2(2, -2) + float2(-1, 1);
    float2 layer1TexCoord = texcoord;
    bool moved = false;
    [branch] if (dcsvrWarp.x > 0) {
        const float2 unmoved = layer1ProjectedCoordNdc;
        float2 unusedPeripheryUv = 0;
        DcsvrApplyDisparity(dcsvrWarp, dcsvrReuse, dcsvrDisparityField, sourceSampler, layer1ProjectedCoordNdc, unusedPeripheryUv);
        moved = any(layer1ProjectedCoordNdc != unmoved);
        if (moved) {
            layer1TexCoord = layer1ProjectedCoordNdc * float2(0.5f, -0.5f) + 0.5f;
        }
    }
    float alpha;
    float isInside = all(abs(layer1ProjectedCoordNdc) < 1);
    if (focusViewShape >= 1) {
        float2 d = abs(layer1ProjectedCoordNdc);
        float r = pow(pow(d.x, focusViewShape) + pow(d.y, focusViewShape), 1 / focusViewShape);
        float width = clamp(2 * smoothingArea, 0.01, 0.95);
        alpha = isInside * (1 - smoothstep(1 - width, 1, r));
    } else if (smoothingArea) {
        float2 s = smoothstep(float2(0, 0), float2(smoothingArea, smoothingArea), layer1TexCoord) -
                   smoothstep(float2(1, 1) - float2(smoothingArea, smoothingArea), float2(1, 1), layer1TexCoord);
        alpha = isInside * max(0.5, s.x * s.y);
    } else {
        alpha = isInside;
    }

    float4 color;
    [branch] if (dcsvrDebug.y != 0 && alpha <= 0) {
        [branch] if (moved) {
            color = plainFocus.SampleLevel(sourceSampler, layer1TexCoord, 0);
        } else {
            color = plainFocus.Load(int3(position.xy, 0));
        }
    } else {
        [branch] if (moved) {
            color = source.SampleLevel(sourceSampler, layer1TexCoord, 0);
        } else {
            color = source.Load(int3(position.xy, 0));
        }
    }

    float3 rgb = encodeSrgb != 0 ? DcsvrLinearToSrgb(color.rgb) : saturate(color.rgb);
    return float4(rgb, alpha);
}
