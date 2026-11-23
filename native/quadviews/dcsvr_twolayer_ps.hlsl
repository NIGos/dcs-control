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
// Focus alpha convention (dcsvr_two_layers_alpha, patches/quadviews/0023-two-layer-alpha.patch; dcsvrEdge.z):
//  0 the above: encoded colour not premultiplied, flags SOURCE_ALPHA | UNPREMULTIPLIED, UNORM target.
//  1 premultiplied on the encoded values (bytes = encoded colour x alpha), flags SOURCE_ALPHA alone, UNORM target.
//  2 the same bytes in an R8G8B8A8_UNORM_SRGB target: the pass returns the linear value whose sRGB encoding is the
//    encoded colour x alpha, so the render target view stores exactly mode 1's bytes in an sRGB swapchain.
// The runtime probe (tools/runtime-probe) measured the Pimax compositor blending every variant on the stored (encoded)
// values; 1 and 2 are the OpenXR default convention, which needs no UNPREMULTIPLIED handling in the compositor.
//
// Opaque focus (dcsvr_two_layers_alpha 3, patches/quadviews/0024-opaque-focus.patch, 0025-opaque-focus-rim.patch and
// 0031-opaque-focus-wide-ramp.patch; dcsvrEdge.w = 1, z 0, focus pass only): the focus pass decides the fade, and every
// texel's colour is chosen for the alpha it stores. It computes the composed result over the whole focus rectangle
// itself: the focus colour as in 0 (edge match tent, Far depth, fill) blended over the
// periphery image (t3, the source the periphery layer shows) with the composition's fade, focus * a + periphery * (1 - a).
// The periphery is read where the composition reads it for the same direction: the pixel's (moved) focus uv times
// dcsvrBand.xy plus dcsvrBand.zw (the focus view's span and offset in the periphery's uv), bilinear. Where the fade is 1
// the colour is the focus colour exactly; outside the shape (the corners of a round focus) it is the periphery.
// The layer goes out with mode 0's flags, and its alpha is the wide ramp below (0031; 0025 had a thin rim). Where alpha a
// is between 0 and 1 the colour is chosen so that the runtime's blend over the periphery layer (on the encoded values,
// as measured) gives the composed colour: P + (C - P) / a, P the periphery, C the composed colour, encoded, a as stored
// (8 bits). Where a is 0 the colour is the composed one: the periphery layer shows there (the composed colour is the
// periphery there), and the same colour shows if a display path ever ignores the alpha (the flickering rectangle of
// mode 0, whose transparent texels hold the focus image). Where a is 1 it is the composed colour too.
//
// Why the alpha must change slowly (0031, the thin warm line along all four edges of the focus rectangle in the Pimax
// screen mirror, absent from its undistorted mirror; flights of 2026-11-22). The runtime's distortion samples each
// colour channel at its own place (chromatic aberration correction: red a few texels towards the centre of the lens,
// blue a few texels outwards, green between them) but takes the factor of the layer below from one alpha only, green's
// place:  shown_c = F_c(x_c) a(x_c) + D_c(x_c) (1 - a(x_G)).  Wherever the alpha changes within those few texels, red
// and blue so get D (a(x_c) - a(x_G)) too much or too little, green nothing. The mirror says exactly that: green
// unchanged across the edge, red +35 % and blue -41 % of the background on every edge (sky 138/178/222 -> 193/178/131,
// a dark edge 18/30/52 -> 25/30/32), red's band one mirror pixel outside blue's; each band's integral is the same
// offset (0.8-1.0 mirror px, about 5 focus texels) everywhere. 0024's hard edge (alpha 1 up to the edge, nothing
// outside) gives blue 0 and red 255 by the same formula, which is what the rig saw then; 0025's rim ramped over 0.75 %
// of the side (15 texels of 1928), so 5 texels of offset made 0.4 of the alpha go missing: the line it left. The error
// of a ramp is the background times the offset times the alpha's slope, so the alpha now ramps as slowly as the
// composition allows: it is the composition's own fade (1 - smoothstep over the band) taken at the pixel's own place
// (not Far depth's moved one, whose steps would be edges of their own), but never over less than kDcsvrOpaqueRamp of
// the half size (the composition's band at the default edge blend 0.2): about 3 codes of red and 6 of blue spread over
// the band instead of a line. Where the band is narrower than that (edge blend under 0.2, Pimax Transition Mode off)
// the colour there is P + (C - P) / a with a below the fade: detail is amplified and, where that would leave 0..1, clamped,
// which softens the outer part of a hard edge slightly. A rectangular focus area (shape below 1) ramps the same way
// with the largest of |x| and |y| as its radius. The periphery layer stays as in 0. dcsvrEdge.w 0: the pass is unchanged.
//
// Without Far depth every texel is a Load at the pixel (1:1, no filtering).
//
// Edge match (dcsvr_edge_match, patches/quadviews/0019-edge-match.patch; dcsvrEdge.x the share of the fade band, 0 off):
// the focus pass blurs the focus image in the band with the composition's tent. Its width needs only k, one periphery
// texel in focus texels, which the composition takes from both views' derivatives; here the compositor enlarges the
// periphery after this pass, so the CPU computes k from the two source sizes and fields of view (dcsvrEdge.y,
// patches/quadviews/0021-two-layer-edge-match.patch). Off, the output is the pass as before (tests/quadviews-composition).
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
    float4 dcsvrEdge; // edge match: x the share of the fade band (0 off), y k (one periphery texel in focus texels);
                      // z the focus alpha convention (0 unpremultiplied, 1 premultiplied, 2 premultiplied, sRGB target)
                      // w 1: opaque focus (0024), the composed result written by the focus pass
    float4 dcsvrBand; // opaque focus: focus uv -> periphery uv, xy scale, zw offset
};

SamplerState sourceSampler : register(s0);
Texture2D source : register(t0);
Texture2D plainFocus : register(t1);
Texture2D<float2> dcsvrDisparityField : register(t2);
Texture2D bandPeriphery : register(t3); // opaque focus: the periphery image, for the focus pass
#include "dcsvr_depthwarp_ps.hlsli"

// Opaque focus (0031): the default alpha ramp, in the focus view's radius (1 at its edge): the outer 0.4 of it, 390
// texels of 1928 (the composition's band at edge blend 0.2). Since 0034 the layer sets it (dcsvrEdge.w = 1 + width,
// dcsvr_two_layers_opaque_ramp, live): the width as set, not widened to the composition's band.
//
// Why a narrower ramp (0034, user's flight 2026-11-23: the focus rectangle flickering in the right eye, "solid at the
// edges, fading toward the centre"): wherever the stored alpha a is below 1 the runtime shows (1 - a) of ITS periphery
// layer under the focus, and the colour there (P + (C - P) / a) is right only if that periphery is the one the focus pass
// composed against. At edge blend 0.2 the 0.4 ramp is the composition's own band, so the colour there is the focus
// colour unblended and the layer is mode 0 (Standard) in all but the corners: whatever makes the runtime's periphery
// under the focus differ on some frames (the Standard flicker that 0024, alpha 1 and no blending, removed) shows again,
// weighted by 1 - a, i.e. solid at the edge and fading inwards. A ramp of width w exposes only the outer w, where the
// composition already shows mostly periphery; the chromatic aberration residual grows as 1 / w (about 3 codes of red and
// 6 of blue at 0.4, so about 12 / 24 at 0.1, spread over the ramp instead of a line).
static const float kDcsvrOpaqueRamp = 0.4;

float3 DcsvrLinearToSrgb(float3 c) {
    c = saturate(c);
    float3 low = c * 12.92;
    float3 high = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return float3(c.r <= 0.0031308 ? low.r : high.r, c.g <= 0.0031308 ? low.g : high.g, c.b <= 0.0031308 ? low.b : high.b);
}

float3 DcsvrSrgbToLinear(float3 c) {
    c = saturate(c);
    float3 low = c / 12.92;
    float3 high = pow((c + 0.055) / 1.055, 2.4);
    return float3(c.r <= 0.04045 ? low.r : high.r, c.g <= 0.04045 ? low.g : high.g, c.b <= 0.04045 ? low.b : high.b);
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
        // Edge match: in the fade band the detail tapers to the periphery's resolution first (ProjectionPS.hlsl's tent).
        [branch] if (dcsvrEdge.x > 0 && focusViewShape >= 1 && smoothingArea > 0 && dcsvrEdge.y > 1) {
            const float2 m = abs(layer1ProjectedCoordNdc);
            const float rm = pow(pow(m.x, focusViewShape) + pow(m.y, focusViewShape), 1 / focusViewShape);
            const float b = saturate(smoothstep(1 - clamp(2 * smoothingArea, 0.01, 0.95), 1, rm) / dcsvrEdge.x);
            [branch] if (b > 0) {
                float2 focusSize;
                source.GetDimensions(focusSize.x, focusSize.y);
                // A [1 2 1] tent of bilinear taps with the variance of the periphery's reconstruction (k^2 / 6).
                const float2 stepUv = b * dcsvrEdge.y * 0.57735 / focusSize;
                const float2 lo = 0.5 / focusSize, hi = 1 - lo;
                float4 sum = 0;
                [unroll] for (int j = -1; j <= 1; ++j) {
                    [unroll] for (int i = -1; i <= 1; ++i) {
                        sum += (2 - abs(i)) * (2 - abs(j)) *
                               source.SampleLevel(sourceSampler, clamp(layer1TexCoord + float2(i, j) * stepUv, lo, hi), 0);
                    }
                }
                color = sum / 16;
            }
        }
    }

    [branch] if (dcsvrEdge.w != 0) {
        // Opaque focus: the composition's blend over the periphery (black for debug_focus_view, as it shows the focus
        // alone), in the sources' own values, then encoded as in 0.
        // The layer's alpha by the pixel's own place in the layer (not the moved one): the composition's fade, never
        // steeper than kDcsvrOpaqueRamp, 8 bits as stored.
        const float2 own = abs(texcoord * float2(2, -2) + float2(-1, 1));
        const float ownRadius = focusViewShape >= 1 ? pow(pow(own.x, focusViewShape) + pow(own.y, focusViewShape), 1 / focusViewShape)
                                                    : max(own.x, own.y);
        // 0034: the ramp's width is dcsvrEdge.w - 1 (dcsvr_two_layers_opaque_ramp, default kDcsvrOpaqueRamp), no longer
        // widened to the composition's band; 0: alpha 1 everywhere, and the layer goes out without blending (0024).
        const float ramp = dcsvrEdge.w - 1;
        const float layerAlpha = ramp <= 0 ? 1 : round((all(own < 1) ? 1 - smoothstep(1 - ramp, 1, ownRadius) : 0) * 255) / 255;
        float4 periphery = 0;
        [branch] if (dcsvrDebug.x == 0 && (alpha < 1 || layerAlpha < 1)) {
            periphery = bandPeriphery.SampleLevel(sourceSampler, layer1TexCoord * dcsvrBand.xy + dcsvrBand.zw, 0);
        }
        const float3 premultiplied = color.rgb * alpha;
        const float3 composed = premultiplied + periphery.rgb * (1 - alpha);
        const float3 composedEncoded = encodeSrgb != 0 ? DcsvrLinearToSrgb(composed) : saturate(composed);
        [branch] if (layerAlpha > 0 && layerAlpha < 1) {
            const float3 peripheryEncoded = encodeSrgb != 0 ? DcsvrLinearToSrgb(periphery.rgb) : saturate(periphery.rgb);
            return float4(saturate(peripheryEncoded + (composedEncoded - peripheryEncoded) / layerAlpha), layerAlpha);
        }
        return float4(composedEncoded, layerAlpha);
    }
    float3 rgb = encodeSrgb != 0 ? DcsvrLinearToSrgb(color.rgb) : saturate(color.rgb);
    [branch] if (dcsvrEdge.z >= 2) {
        return float4(DcsvrSrgbToLinear(rgb * alpha), alpha);
    } else if (dcsvrEdge.z >= 1) {
        return float4(rgb * alpha, alpha);
    }
    return float4(rgb, alpha);
}
