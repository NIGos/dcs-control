// DCSVR opaque focus (dcsvr_two_layers_alpha=3 with dcsvr_two_layers, patches/quadviews/0024-opaque-focus.patch).
//
// The focus layer goes to the runtime opaque (no blend flags), so the runtime never alpha-blends it. It is cut down to
// the rectangle of the focus image where the fade of the composition (ProjectionPS.hlsl, dcsvr_twolayer_ps.hlsl) is
// exactly 1, corners included, and the periphery layer's own output pass draws everything else of the fade: there it
// blends the focus image over the periphery with the composition's weights. Shown together, the two layers give the
// composed image, the area inside the rectangle at the focus resolution.
//
// Plain C++ (no OpenXR or D3D types) so tests/quadviews-composition can check it against the shader's fade.
#pragma once

#include <algorithm>
#include <cmath>

namespace dcsvr {

    // Pixel edges of the opaque rectangle in a width x height focus image: [x0, x1) x [y0, y1).
    struct OpaqueFocusRect {
        int x0{0}, y0{0}, x1{0}, y1{0};
    };

    // The largest centred rectangle, on whole pixels and rounded inward, inside which the fade is exactly 1.
    //  - Rounded shape (shape >= 1): the fade is 1 where |x|^p + |y|^p <= (1 - band)^p in the focus view's NDC,
    //    band = clamp(2 * smoothing, 0.01, 0.95); the square with its corners on that curve is the largest.
    //  - Rectangle with a smoothing area s: 1 where both uv lie in [s, 1 - s].
    //  - Rectangle without one: the whole image.
    // Never empty: at least the centre 2 x 2 pixels (only reached with a rectangle and s >= 0.5).
    inline OpaqueFocusRect OpaqueFocusCrop(float shape, float smoothing, int width, int height) {
        float half = 1.f;  // half the side, in NDC
        if (shape >= 1.f) {
            const float band = std::clamp(2.f * smoothing, 0.01f, 0.95f);
            half = (1.f - band) * std::pow(0.5f, 1.f / shape);
        } else if (smoothing > 0.f) {
            half = 1.f - 2.f * smoothing;
        }
        const auto edge = [&](int size) {
            // NDC -half is the pixel edge (1 - half) / 2 * size; inward = up.
            const int e = (int)std::ceil((1.f - half) * 0.5f * (float)size - 1e-4f);
            return std::clamp(e, 0, (std::max)(size / 2 - 1, 0));
        };
        OpaqueFocusRect rect;
        rect.x0 = edge(width);
        rect.y0 = edge(height);
        rect.x1 = width - rect.x0;
        rect.y1 = height - rect.y0;
        return rect;
    }

    // The field of view of the rectangle: the focus view's tangents at its pixel edges (left, right, up, down angles
    // in and out, as XrFovf orders them; y grows downward in the image).
    inline void OpaqueFocusFov(const float fov[4], const OpaqueFocusRect& rect, int width, int height, float out[4]) {
        const float left = std::tan(fov[0]), right = std::tan(fov[1]), up = std::tan(fov[2]), down = std::tan(fov[3]);
        const float w = (float)(std::max)(width, 1), h = (float)(std::max)(height, 1);
        out[0] = std::atan(left + (float)rect.x0 / w * (right - left));
        out[1] = std::atan(left + (float)rect.x1 / w * (right - left));
        out[2] = std::atan(up - (float)rect.y0 / h * (up - down));
        out[3] = std::atan(up - (float)rect.y1 / h * (up - down));
    }

}  // namespace dcsvr
