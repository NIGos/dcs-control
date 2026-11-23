// DCS Control: band match for the bundled Quad Views compositor (dcsvr_band_match, dcsvr_band_match_color,
// dcsvr_band_stats; patches/quadviews/0035-band-match.patch; copied in by scripts/build-quadviews.ps1, tested by
// tests/quadviews-composition).
//
// Why. Whatever makes the focus image a little brighter, darker or tinted against the periphery around it (DLSS 5's
// look on the focus views only, which Cheeky's Match brightness evens out on average over the whole DLSS 5 area; a
// per-view difference of DCS's own; another DLSS preset) is invisible while the eyes rest, because the composition
// fades one into the other over the edge band. When the eyes move, the band moves over the scene and every point it
// crosses changes from one look to the other: a step of D in the band becomes a temporal change of up to D at the
// focus border, the faint "flash" around where one looks. The band is the only place where the two images meet, so
// it is where they must agree.
//
// What. Once per eye and frame a 16 x 16-thread group samples the focus view on a 32 x 32 grid. At every sample in
// the edge band it reads the focus colour at the periphery's resolution (the edge match's tent with its full width:
// k = one periphery texel in focus texels) and the periphery colour in the same direction, weighted by 4 a (1 - a), a
// the composition's fade there (the band's middle counts most: that is where a moving band changes the image most).
// The weighted mean log ratio periphery / focus (of the luminance, or per colour channel) is the target gain, clamped
// to 1 +- kDcsvrBandLimit; the gain moves toward it by kDcsvrBandRate per frame (snapped on the first measurement and
// after a scene cut). A frame with too little band (no weight) or too dark a periphery keeps the gain.
// The focus pass multiplies the focus colour by gain^strength (dcsvr_band_match, 0..1) before it composes: one gain
// for the whole focus view. Inside the band's inner edge nothing compares the focus with anything, so a uniform gain
// shows only where the two meet. dcsvr_band_stats=1 measures and logs without changing the image.
//
// One header for C++ and HLSL (as dcsvr_perception.h): the gain step the shader runs and the CPU tests check.
#ifndef DCSVR_BANDMATCH_H
#define DCSVR_BANDMATCH_H

#include "dcsvr_perception.h"

#ifdef __cplusplus
#include <cstdio>
#include <string>
namespace dcsvr {
using std::exp;
using std::log;
using std::max;
using std::min;
#endif

// The grid: kDcsvrBandGrid x kDcsvrBandGrid samples over the focus view, 4 per thread of one 16 x 16 group.
static const int kDcsvrBandGrid = 32;
// The gain stays within 1 +- this (Cheeky's Match brightness uses the same limit).
static const float kDcsvrBandLimit = 0.1f;
// Per frame toward the target: about 0.5 s to 90 % at 45 rendered frames per second. Slow enough that a gaze moving
// over different scenery (whose ratio differs a little) does not make the focus pump; fast enough to follow a scene.
static const float kDcsvrBandRate = 0.05f;
// Below this total weight (about 4 samples in the band's middle) the frame is not measured.
static const float kDcsvrBandMinWeight = 4.0f;
// Below this mean periphery luminance (source values) the frame is not measured: in a black frame the ratio is noise.
static const float kDcsvrBandMinLevel = 0.002f;
// Added to both sides of the ratio (source values): a black texel does not weigh as a huge ratio.
static const float kDcsvrBandEpsilon = 0.001f;

// A sample's weight from the composition's fade a there: 1 in the band's middle, 0 where the focus or the periphery
// shows alone.
DCSVR_FN float DcsvrBandWeight(float alpha) {
    return 4.0f * DcsvrSaturate(alpha) * (1.0f - DcsvrSaturate(alpha));
}

// The target gain of a weighted mean log ratio periphery / focus, within 1 +- limit.
DCSVR_FN float DcsvrBandTarget(float meanLogRatio, float limit) {
    return min(max(exp(meanLogRatio), 1.0f - limit), 1.0f + limit);
}

// One frame's step of the gain toward its target (snap: the first measurement, a scene cut).
DCSVR_FN float DcsvrBandStep(float previous, float target, float rate, bool snap) {
    return snap ? target : previous + (target - previous) * rate;
}

// The factor the focus pass applies: gain^strength (strength 0: 1, the image as without the match).
DCSVR_FN float DcsvrBandApplied(float gain, float strength) {
    return gain > 0.0f ? exp(log(gain) * DcsvrSaturate(strength)) : 1.0f;
}

#ifdef __cplusplus

// The shader's state per eye (a RWByteAddressBuffer, kBandStateStride bytes per eye; eye 0 at 0, eye 1 at the stride):
//  0  gain r, g, b (float), valid (uint 1 once measured)
//  16 the last measured ratio periphery / focus r, g, b and of the luminance (exp of the weighted mean log ratio)
//  32 the last measured weight sum, mean periphery luminance (exp of the weighted mean log), measured frames (uint),
//     skipped frames (uint: too little band or too dark)
//  48 the last measured local mismatch: the weighted standard deviation of ln(periphery Y / focus Y) over the band's
//     samples (what one gain cannot remove: local tone, texture, a different reconstruction); 52-63 unused
inline constexpr uint32_t kBandStateStride = 64u;

struct BandState {
    float gain[3]{1.f, 1.f, 1.f};
    uint32_t valid{0};
    float ratio[3]{1.f, 1.f, 1.f};
    float ratioY{1.f};
    float weight{0.f};
    float level{0.f};
    uint32_t measured{0};
    uint32_t skipped{0};
    float spreadY{0.f};
    float unused[3]{};
};
static_assert(sizeof(BandState) == kBandStateStride, "BandState must match dcsvr_bandmatch_cs.hlsl's layout");

// The constant buffer of dcsvr_bandmatch_cs.hlsl.
struct BandMatchConstants {
    float shape[4];   // x the edge smoothing (smoothen_focus_view_edges), y focus_view_shape, z k (one periphery texel
                      // in focus texels; <= 1: no tent), w 1 per colour channel, 0 luminance
    float band[4];    // focus uv -> periphery uv: xy scale, zw offset (as the opaque focus's dcsvrBand)
    float params[4];  // x rate, y limit, z 1: snap this frame, w unused
    uint32_t slot[4]; // x this eye's byte offset in the state
};
static_assert(sizeof(BandMatchConstants) == 64, "BandMatchConstants must match dcsvr_bandmatch_cs.hlsl's cbuffer");

// What the layer accumulates per eye from the read-back states between two log lines.
struct BandStats {
    double logRatioY{0.0};   // sum of ln(ratio Y) over measured read-backs
    double logRatio[3]{};    // per channel
    double logRatioYSq{0.0}; // for the spread over time
    double localSpread{0.0}; // sum of the shader's local mismatch
    uint64_t samples{0};     // read-backs with a new measurement
    uint32_t lastMeasured{0};
    uint32_t lastSkipped{0};
    uint64_t skippedFrames{0};
    float gain[3]{1.f, 1.f, 1.f};
    bool any{false};

    // One read-back of the state (a new measurement only when the shader's counter moved).
    void Add(const BandState& s) {
        if (s.skipped > lastSkipped) skippedFrames += s.skipped - lastSkipped;
        lastSkipped = s.skipped;
        if (s.valid == 1u) {
            for (int c = 0; c < 3; c++) gain[c] = s.gain[c];
            any = true;
        }
        if (s.measured == lastMeasured || !(s.ratioY > 0.f)) return;
        lastMeasured = s.measured;
        const double y = std::log((double)s.ratioY);
        logRatioY += y;
        logRatioYSq += y * y;
        localSpread += s.spreadY;
        for (int c = 0; c < 3; c++) logRatio[c] += std::log(std::max((double)s.ratio[c], 1e-6));
        ++samples;
    }
    void Clear() {
        const uint32_t measured = lastMeasured, skipped = lastSkipped;
        *this = BandStats{};
        lastMeasured = measured;
        lastSkipped = skipped;
    }
};

// "L: band periphery/focus Y 0.982 (+-0.6 % over time) R 0.990 G 0.981 B 0.968, local mismatch 4.1 %, gain
// 1.000/1.000/1.000, 312 measured, 4 skipped"
// for one eye; no '%' formats from outside (the layer's Log takes it as a format: written as "%%").
inline std::string BandStatsText(const char* eye, const BandStats& s) {
    char text[320];
    if (s.samples == 0) {
        std::snprintf(text, sizeof(text), "%s: no band measured (%llu frames skipped: too little band or too dark)", eye,
                      (unsigned long long)s.skippedFrames);
        return text;
    }
    const double n = (double)s.samples;
    const double mean = s.logRatioY / n;
    const double spread = std::sqrt(std::max(0.0, s.logRatioYSq / n - mean * mean));
    std::snprintf(text, sizeof(text),
                  "%s: band periphery/focus Y %.3f (+-%.1f %%%% over time) R %.3f G %.3f B %.3f, local mismatch %.1f %%%%, "
                  "gain %.3f/%.3f/%.3f, %llu measured, %llu skipped",
                  eye, std::exp(mean), spread * 100.0, std::exp(s.logRatio[0] / n), std::exp(s.logRatio[1] / n),
                  std::exp(s.logRatio[2] / n), s.localSpread / n * 100.0, s.gain[0], s.gain[1], s.gain[2], (unsigned long long)s.samples,
                  (unsigned long long)s.skippedFrames);
    return text;
}

}  // namespace dcsvr
#endif

#endif
