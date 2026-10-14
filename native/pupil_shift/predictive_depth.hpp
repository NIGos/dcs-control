// Depth boost's predictive transition (docs/FIXATION_DEPTH.md, "Predictive transition"), kept free of Windows and
// OpenXR calls so the tests can exercise it offline.
//
// Gradual (the default): the scale follows the confirmed fixation at UpMmPerS / DownMmPerS, plus SaccadeStepMm at a
// landing. Predictive: a high-rate gaze stream (Pimax PVR, polled at ~500 Hz) is watched for saccades; at the saccade's
// velocity peak its amplitude is predicted from the main sequence, the landing point is looked up in the engine's
// coarse depth grid, and one capped step towards the landing's scale is taken while the eyes are still in flight
// (saccadic suppression). The rest of the change follows the gradual rates; the landing's scene depth (the 1 degree
// cone, FixationDecider) confirms the step or reverts it at the normal rates, never with a second step.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "perception_shm.h"
#include "pupil_math.hpp"

namespace pupil_shift::predictive {

enum class Transition : std::uint8_t { gradual = 0, predictive = 1 };
inline const char* transition_name(Transition t) { return t == Transition::predictive ? "predictive" : "gradual"; }

struct Settings {
    float onset_deg_s{100.0f};       // a saccade starts at this speed (two samples in a row)...
    float offset_deg_s{70.0f};       // ...and ends below this one (two samples in a row): the hysteresis band
    float min_amplitude_deg{8.0f};   // [DepthBoost] PredictMinDeg: shorter predicted saccades use the gradual path
    float max_step_mm{12.0f};        // [DepthBoost] PredictMaxStepMm: the absolute cap of one step, mm of IPD
    float disparity_deg{0.4f};       // [DepthBoost] PredictDisparityDeg: the landing target's disparity changes by at most this
    float lead_frames{0.5f};         // [DepthBoost] PredictLeadFrames: a step needs this many frame periods of saccade left
    float hold_s{0.6f};              // after the predicted end, the cone has this long to confirm the step
    unsigned poll_hz{500};           // [DepthBoost] PredictPollHz (100..1000)
};

// ---- The main sequence --------------------------------------------------------------------------------------------
// Peak velocity against amplitude, the saturating exponential of Baloh et al. (1975, Neurology 25:1065) and Bahill,
// Clark & Stark (1975, Math. Biosci. 24:191): Vp = Vmax * (1 - exp(-A / C)). Vmax = 600 deg/s and C = 15 deg lie in the
// range those and later fits report (the plateau at 500-700 deg/s); they are not one study's constants, and the online
// gain below absorbs a systematic error (an individual, or a tracker whose filtering lowers the peaks). Duration from
// Carpenter (1988, Movements of the Eyes): D = 21 ms + 2.2 ms/deg * A. Inverted: A = -C * ln(1 - Vp / Vmax), which
// saturates: above about 20 degrees a small error in Vp is a large error in A, so the prediction is bounded by how far
// the eyes have travelled at the peak (1.4 to 3 times that: the peak comes at 35-60 % of a saccade's path).
namespace main_sequence {
constexpr float vmax_deg_s = 600.0f, c_deg = 15.0f, d0_ms = 21.0f, d_per_deg_ms = 2.2f, max_amplitude_deg = 45.0f;
constexpr float min_travel_factor = 1.4f, max_travel_factor = 3.0f;
inline float peak_velocity(float amplitude_deg) { return vmax_deg_s * (1.0f - std::exp(-std::max(0.0f, amplitude_deg) / c_deg)); }
inline float amplitude(float peak_deg_s) {
    if (!(peak_deg_s > 0)) return 0.0f;
    const float r = peak_deg_s / vmax_deg_s;
    if (r >= 0.98f) return max_amplitude_deg;
    return std::min(max_amplitude_deg, -c_deg * std::log(1.0f - r));
}
inline float duration_s(float amplitude_deg) { return (d0_ms + d_per_deg_ms * std::max(0.0f, amplitude_deg)) * 0.001f; }
}  // namespace main_sequence

// Rotation of v about the unit axis k by angle (radians), Rodrigues.
inline Vec3 rotate_about(Vec3 v, Vec3 k, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1.0f - c));
}
inline float angle_deg(Vec3 a, Vec3 b) { return std::acos(std::clamp(dot(normalized(a), normalized(b)), -1.0f, 1.0f)) * 57.29578f; }

// One fresh sample of the high-rate stream: time (the tracker's own clock, seconds), both eyes' gaze in the head frame
// (+X right, +Y up, -Z forward). valid false: a blink or lost tracking.
struct GazeSample {
    double t{};
    bool valid{};
    Vec3 eye[2]{{0, 0, -1}, {0, 0, -1}};
};

struct Prediction {
    unsigned id{};                   // the saccade's number
    double onset_t{}, peak_t{}, decided_t{}, end_t{};  // end_t: onset + the main sequence's duration of the prediction
    float peak_deg_s{}, raw_amplitude_deg{}, amplitude_deg{}, travelled_deg{};
    Vec3 axis{0, 1, 0};              // rotation axis of the saccade (head frame)
    Vec3 onset[2]{{0, 0, -1}, {0, 0, -1}}, landing[2]{{0, 0, -1}, {0, 0, -1}};  // per eye, head frame
};

// Saccades in the high-rate stream. Speed: the slope of a least-squares line through the cyclopean gaze's yaw and pitch
// over the last 8 ms (at least 2 samples), which divides white tracker noise down without the lag of a long filter.
// Onset: speed >= onset_deg_s on two samples in a row; the onset position is the fitted position before them. The peak
// is taken once the speed (the mean of the last three) has fallen below 93 % of its maximum (the first sign the eyes
// decelerate); the prediction is made there. End: speed below offset_deg_s on two samples in a row. A saccade still in
// flight after 250 ms, or a gap in the stream, is dropped; after a gap a saccade may start only once the stream has run
// 30 ms again (not from the middle of one).
class SaccadeDetector {
public:
    static constexpr int kHistory = 64;
    static constexpr double kWindowS = 0.008, kMaxFlightS = 0.25, kMaxGapS = 0.05, kSettleS = 0.03;
    static constexpr float kPeakDrop = 0.93f;
    enum Event : unsigned { none = 0, started = 1, predicted = 2, landed = 4, dropped = 8 };

    unsigned push(const GazeSample& s, const Settings& set) {
        unsigned events = none;
        if (!s.valid || !std::isfinite(s.t) || (n_ && (s.t <= hist_[(n_ - 1) % kHistory].t || s.t - hist_[(n_ - 1) % kHistory].t > kMaxGapS))) {
            if (flight_) { flight_ = false; ++dropped_; events |= dropped; }
            n_ = 0; above_ = below_ = 0; speeds_n_ = 0; prev_speed_t_ = 0;
            if (!s.valid || !std::isfinite(s.t)) return events;
        }
        const Vec3 c = normalized(s.eye[0] + s.eye[1]);
        H& h = hist_[n_ % kHistory];
        h.t = s.t; h.yaw = std::atan2(c.x, -c.z) * 57.29578f; h.pitch = std::atan2(c.y, -c.z) * 57.29578f;
        h.eye[0] = normalized(s.eye[0]); h.eye[1] = normalized(s.eye[1]);
        ++n_;
        if (n_ < 2) { fit_ = {h.yaw, h.pitch}; last_below_ = fit_; last_below_eye_[0] = h.eye[0]; last_below_eye_[1] = h.eye[1]; return events; }
        float vy{}, vp{};
        const double spacing = s.t - hist_[(n_ - 2) % kHistory].t;
        fit(s.t, vy, vp);
        speed_ = std::sqrt(vy * vy + vp * vp);
        // The peak is judged on the mean of the last speeds (noise), the thresholds on each: three at 500 Hz (6 ms), fewer
        // for a slower tracker, whose single speeds already span 8 ms (a 3-sample mean at 120 Hz lags the peak by a
        // sample and moves its position on).
        speeds_[speeds_n_++ % 3] = speed_;
        const unsigned smooth_n = std::min(speeds_n_, spacing <= 0.003 ? 3u : spacing <= 0.006 ? 2u : 1u);
        float smooth = 0;
        for (unsigned k = 0; k < smooth_n; ++k) smooth += speeds_[(speeds_n_ - 1 - k) % 3];
        smooth /= static_cast<float>(smooth_n);
        smooth_ = smooth;
        if (n_ == 2) start_t_ = hist_[0].t;
        if (!flight_) {
            // A saccade starts only on a stream that has run 30 ms since a gap (not in the middle of one).
            if (speed_ >= set.onset_deg_s && s.t - start_t_ >= kSettleS) {
                if (above_++ == 0) {
                    // When the speed crossed the threshold (between the last sample below and this one), less the fit's
                    // lag (half its span): the onset at any tracker rate, not only at 500 Hz.
                    first_above_t_ = s.t;
                    if (prev_speed_t_ > 0 && prev_speed_ < set.onset_deg_s && speed_ > prev_speed_)
                        first_above_t_ = prev_speed_t_ + (set.onset_deg_s - prev_speed_) / (speed_ - prev_speed_) * (s.t - prev_speed_t_);
                    first_above_lag_ = 0.5 * fit_span_;
                }
                if (above_ >= 2) {
                    flight_ = true; predicted_ = false; have_prediction_ = false; below_ = 0;
                    ++saccades_; events |= started;
                    p_ = Prediction{};
                    p_.id = saccades_;
                    p_.onset_t = first_above_t_ - first_above_lag_;
                    onset_dir_ = dir(last_below_);
                    p_.onset[0] = last_below_eye_[0]; p_.onset[1] = last_below_eye_[1];
                    peak_ = 0; peak_t_ = s.t; peak_pos_ = fit_; fn_ = 0;
                }
            } else {
                above_ = 0;
                last_below_ = fit_; last_below_eye_[0] = h.eye[0]; last_below_eye_[1] = h.eye[1];
            }
        }
        prev_speed_ = speed_; prev_speed_t_ = s.t;
        if (flight_) {
            if (smooth > peak_) { peak_ = smooth; peak_t_ = s.t; peak_pos_ = fit_; }
            F& f = flight_hist_[fn_++ % kFlight];
            f.t = s.t; f.v = speed_; f.pos = fit_;
            if (!predicted_ && (vertex_peak(s.t) || smooth < kPeakDrop * peak_) && predict(s.t)) {
                predicted_ = true;
                events |= predicted; ++predictions_;
            }
            if (speed_ < set.offset_deg_s) {
                if (++below_ >= 2) {
                    flight_ = false; above_ = 0; events |= landed;
                    last_amplitude_ = angle_deg(onset_dir_, dir(fit_));
                    landed_id_ = p_.id;
                    if (have_prediction_ && p_.raw_amplitude_deg > 0 && p_.peak_deg_s < 0.95f * main_sequence::vmax_deg_s && last_amplitude_ >= 3.0f) {
                        // The online gain: the log ratio of the actual to the main-sequence amplitude, averaged (EWMA 0.2).
                        log_gain_ += 0.2f * (std::log(last_amplitude_ / p_.raw_amplitude_deg) - log_gain_);
                        log_gain_ = std::clamp(log_gain_, std::log(0.7f), std::log(1.4f));
                    }
                    last_below_ = fit_; last_below_eye_[0] = h.eye[0]; last_below_eye_[1] = h.eye[1];
                }
            } else {
                below_ = 0;
            }
            if (flight_ && s.t - p_.onset_t > kMaxFlightS) { flight_ = false; ++dropped_; events |= dropped; above_ = 0; }
        }
        return events;
    }

    bool in_flight() const { return flight_; }
    // A flight that is a saccade for the fixation filters too: once the gaze has travelled kHintTravelDeg from the onset.
    // Tracker noise at 0.3-0.5 degrees starts short false flights (two speeds over the onset threshold; some even reach
    // a prediction), which must not reset the vergence and the fixation decision.
    static constexpr float kHintTravelDeg = 3.0f;
    bool saccade_hint() const { return flight_ && angle_deg(onset_dir_, dir(fit_)) >= kHintTravelDeg; }
    const Prediction& prediction() const { return p_; }
    float speed() const { return speed_; }
    float gain() const { return std::exp(log_gain_); }
    unsigned saccades() const { return saccades_; }
    unsigned predictions() const { return predictions_; }
    unsigned dropped_count() const { return dropped_; }
    unsigned landed_id() const { return landed_id_; }
    float last_amplitude() const { return last_amplitude_; }
    // The fitted cyclopean direction of the latest sample (head frame).
    Vec3 direction() const { return dir(fit_); }

private:
    struct H { double t{}; float yaw{}, pitch{}; Vec3 eye[2]; };
    struct YP { float yaw{}, pitch{}; };
    static Vec3 dir(YP a) { return normalized({std::tan(a.yaw / 57.29578f), std::tan(a.pitch / 57.29578f), -1.0f}); }

    // Least-squares line through the samples of the last 8 ms (at least the last 2): slopes (deg/s) and the fitted
    // position at t.
    void fit(double t, float& vy, float& vp) {
        int count = 0;
        double st = 0, sy = 0, sp = 0;
        const int avail = static_cast<int>(std::min<unsigned>(n_, kHistory));
        for (int k = 0; k < avail; ++k) {
            const H& h = hist_[(n_ - 1 - k) % kHistory];
            if (k >= 2 && t - h.t > kWindowS) break;
            st += h.t - t; sy += h.yaw; sp += h.pitch; ++count;
            fit_span_ = t - h.t;
        }
        const double mt = st / count, my = sy / count, mp = sp / count;
        double stt = 0, sty = 0, stp = 0;
        for (int k = 0; k < count; ++k) {
            const H& h = hist_[(n_ - 1 - k) % kHistory];
            const double dt = (h.t - t) - mt;
            stt += dt * dt; sty += dt * (h.yaw - my); stp += dt * (h.pitch - mp);
        }
        const double by = stt > 0 ? sty / stt : 0, bp = stt > 0 ? stp / stt : 0;
        vy = static_cast<float>(by); vp = static_cast<float>(bp);
        fit_ = {static_cast<float>(my - by * mt), static_cast<float>(mp - bp * mt)};
    }

    // The peak as the vertex of a parabola fitted to the speeds of the last 12 ms (at least 6 samples): once the vertex
    // lies 3 ms or more in the past, within 5 % of the highest speed so far, and the speed is 3 % under it, the peak is its value (less noisy and 5-8 ms earlier
    // than waiting for a 7 % drop; a slow tracker never has the samples and uses the drop).
    bool vertex_peak(double now) {
        const int avail = static_cast<int>(std::min<unsigned>(fn_, kFlight));
        int count = 0;
        double s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, y0 = 0, y1 = 0, y2 = 0;
        for (int k = 0; k < avail; ++k) {
            const F& f = flight_hist_[(fn_ - 1 - k) % kFlight];
            const double x = f.t - now;
            if (k >= 5 && -x > 0.012) break;
            const double x2 = x * x;
            s0 += 1; s1 += x; s2 += x2; s3 += x2 * x; s4 += x2 * x2; y0 += f.v; y1 += f.v * x; y2 += f.v * x2;
            ++count;
        }
        if (count < 6) return false;
        // Normal equations for v = a + b x + c x^2 (Cramer's rule).
        const auto det3 = [](double a, double b, double c, double d, double e, double f, double g, double h, double i) {
            return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        };
        const double det = det3(s0, s1, s2, s1, s2, s3, s2, s3, s4);
        if (std::fabs(det) < 1e-30) return false;
        const double a = det3(y0, s1, s2, y1, s2, s3, y2, s3, s4) / det;
        const double b = det3(s0, y0, s2, s1, y1, s3, s2, y2, s4) / det;
        const double c = det3(s0, s1, y0, s1, s2, y1, s2, s3, y2) / det;
        if (!(c < 0)) return false;
        const double xv = -b / (2 * c);
        if (xv > -0.003 || xv < -0.012) return false;
        const float vertex = static_cast<float>(a + b * xv + c * xv * xv);
        if (!(vertex > 0) || smooth_ > 0.97f * vertex || vertex < 0.95f * peak_ || vertex > 1.1f * peak_) return false;
        peak_ = vertex; peak_t_ = now + xv;
        // The position at the sample nearest the vertex.
        double best = 1e9;
        for (int k = 0; k < avail; ++k) {
            const F& f = flight_hist_[(fn_ - 1 - k) % kFlight];
            if (std::fabs(f.t - peak_t_) < best) { best = std::fabs(f.t - peak_t_); peak_pos_ = f.pos; }
        }
        return true;
    }

    // At the peak: the amplitude from the main sequence (times the online gain), bounded by the path travelled; the
    // direction from the onset to the peak position; each eye's landing is its onset rotated by that amplitude. False
    // (try again on a later sample) when the peak is not plausible yet.
    bool predict(double now) {
        have_prediction_ = false;
        const Vec3 at_peak = dir(peak_pos_);
        const float travelled = angle_deg(onset_dir_, at_peak);
        if (travelled < 0.3f) return false;
        // A peak is plausible only where the eyes have done 30 % of the path it predicts (a noise bump on the rising
        // flank is not the peak; 25 degrees at most, where the main sequence saturates): otherwise wait for the next one.
        if (travelled < 0.3f * std::min(25.0f, main_sequence::amplitude(peak_) * gain())) return false;
        const Vec3 axis = cross(onset_dir_, at_peak);
        if (length(axis) < 1e-6f) return false;
        p_.peak_t = peak_t_; p_.decided_t = now; p_.peak_deg_s = peak_;
        p_.raw_amplitude_deg = main_sequence::amplitude(peak_);
        p_.travelled_deg = travelled;
        p_.amplitude_deg = std::clamp(p_.raw_amplitude_deg * gain(), travelled * main_sequence::min_travel_factor,
            std::min(main_sequence::max_amplitude_deg, travelled * main_sequence::max_travel_factor));
        p_.axis = normalized(axis);
        p_.end_t = p_.onset_t + main_sequence::duration_s(p_.amplitude_deg);
        for (int e = 0; e < 2; ++e) p_.landing[e] = normalized(rotate_about(p_.onset[e], p_.axis, p_.amplitude_deg / 57.29578f));
        have_prediction_ = true;
        return true;
    }

    static constexpr int kFlight = 32;
    struct F { double t{}; float v{}; YP pos{}; };
    F flight_hist_[kFlight]{};
    unsigned fn_{};
    H hist_[kHistory]{};
    unsigned n_{};
    YP fit_{}, last_below_{}, peak_pos_{};
    Vec3 last_below_eye_[2]{{0, 0, -1}, {0, 0, -1}}, onset_dir_{0, 0, -1};
    int above_{}, below_{};
    double first_above_t_{}, first_above_lag_{}, peak_t_{}, fit_span_{}, prev_speed_t_{};
    float prev_speed_{};
    float speed_{}, smooth_{}, peak_{}, last_amplitude_{}, log_gain_{}, speeds_[3]{};
    unsigned speeds_n_{};
    double start_t_{};
    bool flight_{}, predicted_{}, have_prediction_{};
    Prediction p_{};
    unsigned saccades_{}, predictions_{}, dropped_{}, landed_id_{};
};

// ---- The landing's depth from the grid ----------------------------------------------------------------------------
// The cell the landing direction falls in is the cone, its 8 neighbours the ring, with the cone's rules
// (fixation_rules): near or far with a 70 % majority of the centre cell, an edge when the neighbours hold at least 30 %
// near and 30 % far, between 1.5 m and 3 m (the dead band) neither. Per cell: near = nearShare / 255; far = 1 when even
// the nearest point is beyond 3 m, else the part that is not near when the typical distance (the cell's log mean) is
// beyond 3 m (the far part dominates then), else 0 (between, or unknown). Empty neighbours are no evidence: the ring's
// shares are over the valid ones.
struct GridLanding {
    enum class Status : std::uint8_t { ok = 0, no_grid = 1, outside = 2, edge = 3, ambiguous = 4 };
    Status status{Status::no_grid};
    DepthClass cls{DepthClass::none};
    float distance_m{};              // far: the median typical distance of the far cells; near: the centre's nearest
    int col{-1}, row{-1};
    float near_share{}, far_share{}, ring_near{}, ring_far{};
};
inline const char* grid_status_name(GridLanding::Status s) {
    switch (s) {
    case GridLanding::Status::ok: return "ok";
    case GridLanding::Status::outside: return "outside";
    case GridLanding::Status::edge: return "edge";
    case GridLanding::Status::ambiguous: return "ambiguous";
    default: return "no_grid";
    }
}

inline bool grid_usable(const dcsvr_perception::DepthGrid& g) {
    const float* f = g.fovTan;
    return (g.flags & dcsvr_perception::grid_flags::valid) && g.w >= 1 && g.h >= 1 && g.w <= 32 && g.h <= 32 &&
        g.w * g.h <= static_cast<std::uint32_t>(dcsvr_perception::grid_cells) && std::isfinite(f[0] + f[1] + f[2] + f[3]) && f[1] > f[0] && f[2] > f[3];
}

// local: the landing direction in the sampled view's own frame.
inline GridLanding classify_grid(const dcsvr_perception::DepthGrid& g, Vec3 local) {
    GridLanding r;
    if (!grid_usable(g)) return r;
    r.status = GridLanding::Status::outside;
    if (!(local.z < -0.05f) || !std::isfinite(local.x + local.y)) return r;
    const float tx = local.x / -local.z, ty = local.y / -local.z;
    const float u = (tx - g.fovTan[0]) / (g.fovTan[1] - g.fovTan[0]), v = (g.fovTan[2] - ty) / (g.fovTan[2] - g.fovTan[3]);
    if (!(u >= 0 && u < 1 && v >= 0 && v < 1)) return r;
    const int w = static_cast<int>(g.w), h = static_cast<int>(g.h);
    r.col = std::min(w - 1, static_cast<int>(u * w)); r.row = std::min(h - 1, static_cast<int>(v * h));
    const auto shares = [&](int c, int rr, float& near_, float& far_) {
        const int i = rr * w + c;
        const float nearest = g.nearestM[i], typical = g.typicalM[i];
        near_ = std::clamp(g.nearShare[i] / 255.0f, 0.0f, 1.0f);
        if (!(std::isfinite(nearest) && nearest > 0 && std::isfinite(typical) && typical > 0)) { near_ = 0; far_ = 0; return false; }
        far_ = nearest > 3.0f ? 1.0f : typical > 3.0f ? 1.0f - near_ : 0.0f;
        return true;
    };
    float cn{}, cf{};
    shares(r.col, r.row, cn, cf);
    r.near_share = cn; r.far_share = cf;
    float rn = 0, rf = 0;
    int ring = 0;
    float far_d[9]; int far_n = 0;
    for (int dr = -1; dr <= 1; ++dr)
        for (int dc = -1; dc <= 1; ++dc) {
            const int c = r.col + dc, rr = r.row + dr;
            if (c < 0 || c >= w || rr < 0 || rr >= h) continue;
            float n_{}, f_{};
            const bool ok = shares(c, rr, n_, f_);
            if (ok && f_ >= fixation_rules::majority) far_d[far_n++] = g.typicalM[rr * w + c];
            if (dc == 0 && dr == 0) continue;
            if (!ok) continue;  // an empty cell is no evidence either way
            rn += n_; rf += f_; ++ring;
        }
    r.ring_near = ring ? rn / ring : 0; r.ring_far = ring ? rf / ring : 0;
    if (r.ring_near >= fixation_rules::edge_share && r.ring_far >= fixation_rules::edge_share) { r.status = GridLanding::Status::edge; return r; }
    if (cn >= fixation_rules::majority) {
        r.cls = DepthClass::near_;
        r.distance_m = std::max(0.3f, g.nearestM[r.row * w + r.col]);
    } else if (cf >= fixation_rules::majority && far_n > 0) {
        r.cls = DepthClass::far_;
        std::sort(far_d, far_d + far_n);
        r.distance_m = far_d[far_n / 2];
    } else {
        r.status = GridLanding::Status::ambiguous;
        return r;
    }
    r.status = GridLanding::Status::ok;
    return r;
}

// One step, signed mm of IPD, towards the target scale: at most max_step_mm, and at most what changes the disparity of
// the landing target at distance_m by disparity_deg (an IPD change d moves a point at Z by d / Z radians).
inline float step_mm(float scale, float target, float base_mm, float distance_m, const Settings& set) {
    const float want = (target - scale) * base_mm;
    float cap = std::max(0.0f, set.max_step_mm);
    if (std::isfinite(distance_m) && distance_m > 0) cap = std::min(cap, set.disparity_deg / 57.29578f * distance_m * 1000.0f);
    return std::clamp(want, -cap, cap);
}

// The step and what follows it, once per display time. Pure.
//  - A new prediction: no step when the saccade is predicted shorter than min_amplitude_deg (short), there is no fresh
//    grid (no_grid), the landing is outside the grid, on an edge or ambiguous, the predicted saccade ends within
//    lead_frames frame periods (late: the frame would not be rendered in flight), or the step would be under 0.5 mm.
//    Otherwise one step towards the landing's scale (1 near, the mode's far curve far), capped (step_mm).
//  - After a step the scale holds against the step's direction (hold +1 after a step up: no move down; -1 after a step
//    down: no move up) until the eyes have landed and the decider's state is the predicted class (confirmed), or its
//    frame class is the opposite one (contradicted), or hold_s passes after the predicted end (expired), or the next
//    saccade's prediction or a gap in the gaze arrives (expired). Released, the controller moves at the normal rates:
//    the step is never taken back with a jump, and the gradual saccade step is withheld for that saccade.
class PredictiveGate {
public:
    struct Input {
        double now_s{};                  // the tracker's clock at this xrLocateViews
        float frame_s{1.0f / 45.0f};     // the frame period
        bool active{};                   // Depth boost on and the predictive transition in effect
        bool gaze_valid{true};
        bool new_prediction{};
        Prediction prediction{};
        GridLanding landing{};           // for the new prediction
        unsigned landed_id{};            // the detector's last landed saccade
        float scale{1.0f}, base_mm{64.0f}, cap{1.0f};
        Fixation decider_state{Fixation::near_};
        DepthClass frame_class{DepthClass::none};
    };
    struct Output { float jump_mm{}; int hold{}; bool no_saccade_step{}; };
    struct Stats {
        unsigned predicted{}, steps{}, confirmed{}, contradicted{}, expired{};
        unsigned short_{}, no_grid{}, outside{}, edge{}, ambiguous{}, late{}, same{};
        double step_mm_sum{};
    };
    // What the last decision was (for the layer's per-step log line).
    struct Decision { bool made{}; float jump_mm{}, target{}, remaining_s{}; GridLanding landing{}; Prediction p{}; };

    Output update(const Input& in, const DepthBoostSettings& s, const Settings& set) {
        Output out;
        decision_ = Decision{};
        if (!in.active) { hold_ = 0; return out; }
        if (hold_ != 0) {
            if (!in.gaze_valid || in.new_prediction) { hold_ = 0; ++stats_.expired; }
            else if (in.landed_id >= hold_id_ || in.now_s > hold_end_ + 0.03) {
                const DepthClass opposite = hold_cls_ == Fixation::far_ ? DepthClass::near_ : DepthClass::far_;
                if (in.decider_state == hold_cls_) { hold_ = 0; ++stats_.confirmed; }
                else if (in.frame_class == opposite) { hold_ = 0; ++stats_.contradicted; }
                else if (in.now_s > hold_end_ + set.hold_s) { hold_ = 0; ++stats_.expired; }
            }
        }
        if (in.new_prediction) {
            ++stats_.predicted;
            const Prediction& p = in.prediction;
            decision_.p = p; decision_.landing = in.landing; decision_.remaining_s = static_cast<float>(p.end_t - in.now_s);
            if (p.amplitude_deg < set.min_amplitude_deg) ++stats_.short_;
            else if (in.landing.status == GridLanding::Status::no_grid) ++stats_.no_grid;
            else if (in.landing.status == GridLanding::Status::outside) ++stats_.outside;
            else if (in.landing.status == GridLanding::Status::edge) ++stats_.edge;
            else if (in.landing.status != GridLanding::Status::ok) ++stats_.ambiguous;
            else if (decision_.remaining_s < set.lead_frames * in.frame_s) ++stats_.late;
            else {
                const bool far_ = in.landing.cls == DepthClass::far_;
                const float target = far_ ? far_scale(in.landing.distance_m, s.far_start_m, s.far_full_m, in.cap) : 1.0f;
                const float mm = step_mm(in.scale, target, in.base_mm, in.landing.distance_m, set);
                decision_.target = target;
                if (std::fabs(mm) < 0.5f) ++stats_.same;
                else {
                    out.jump_mm = mm;
                    hold_ = mm > 0 ? 1 : -1;
                    hold_cls_ = far_ ? Fixation::far_ : Fixation::near_;
                    hold_id_ = p.id; hold_end_ = p.end_t;
                    last_step_end_ = p.end_t; have_step_ = true;
                    ++stats_.steps; stats_.step_mm_sum += std::fabs(mm);
                    decision_.made = true; decision_.jump_mm = mm;
                }
            }
        }
        out.hold = hold_;
        // The gradual saccade step is withheld while a step holds and until well after it (a contradiction is reverted
        // at the rates, not with the 4 mm landing step).
        out.no_saccade_step = hold_ != 0 || (have_step_ && in.now_s < last_step_end_ + set.hold_s + 0.3);
        return out;
    }

    int hold() const { return hold_; }
    const Stats& stats() const { return stats_; }
    const Decision& decision() const { return decision_; }

private:
    int hold_{};
    Fixation hold_cls_{Fixation::unknown};
    unsigned hold_id_{};
    double hold_end_{}, last_step_end_{};
    bool have_step_{};
    Stats stats_{};
    Decision decision_{};
};

}  // namespace pupil_shift::predictive
