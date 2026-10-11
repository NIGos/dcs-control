// DCS Control: eye-tracked focus area with frame generation (copied into Quad-Views-Foveated by
// scripts/build-quadviews.ps1; tested by tests/quadviews-gaze).
//
// DCS places the focus area once per rendered frame, and with frame generation that frame reaches the headset 55-88 ms
// later. After a large saccade the eye lands outside the sharp area for a frame or two, and the tracker's noise while
// the eye rests makes the focus edge shimmer. Both fixes are free on the GPU: the focus view keeps its pixel count, so
// a wider focus field only lowers its pixel density for the frames of a saccade (when the eye does not see).
//  - Saccade lead: while the gaze turns faster than `speed` (deg/s), and for `holdMs` after, the focus box reaches
//    ahead along the eye's motion by its velocity times `leadMs` (the display latency), at most `maxExtend` (NDC).
//  - Fixation deadzone: outside saccades the focus stays put while the gaze moves less than `deadzoneDeg`.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dcsvr {

struct GazeVec2 {
    float x{}, y{};
};

struct GazeSettings {
    bool widening{false};
    float speed{120.f};     // deg/s that start a saccade
    float leadMs{50.f};     // how far ahead the focus box reaches, as display latency
    float maxExtend{0.35f}; // NDC, per axis
    float holdMs{40.f};     // the lead fades out over this time after the saccade
    float deadzoneDeg{0.f}; // 0: off
};

struct GazeStats {
    uint64_t eyeFrames{}, widened{}, held{}, saccades{};
    float peakSpeed{};
};

class GazeFilter {
  public:
    static constexpr int Eyes = 2;
    static constexpr float MinSampleSeconds = 0.004f;

    // Once per located frame (time in ns, a display time): the gaze unit vector's angular speed since the previous one.
    // A frame located again for the same time, a non-finite vector or a gap of 200 ms or more changes nothing. Display
    // times closer than 4 ms (DCS may locate views for nearly the same time twice) are not a speed sample: dividing
    // tracker noise by microseconds read as thousands of degrees per second; the earlier sample stays the reference.
    void UpdateSpeed(int64_t time, float x, float y, float z, const GazeSettings& s) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || time == m_lastTime) return;
        if (m_lastTime != 0 && time < m_lastTime) return;  // an older display time: neither a sample nor the reference
        const float length = std::sqrt(x * x + y * y + z * z);
        if (length <= 0.f) return;
        if (m_lastTime != 0 && time > m_lastTime) {
            const float dt = (time - m_lastTime) / 1e9f;
            if (dt < MinSampleSeconds) return;
            if (dt < 0.2f) {
                const float cosine = std::clamp((m_last[0] * x + m_last[1] * y + m_last[2] * z) / length, -1.f, 1.f);
                m_speed = std::acos(cosine) * 57.2957795f / dt;
                const bool saccade = m_speed >= s.speed;
                if (saccade && !m_inSaccade) m_stats.saccades++;
                m_inSaccade = saccade;
                if (saccade) m_until = time + static_cast<int64_t>(s.holdMs * 1e6f);
                m_stats.peakSpeed = std::max(m_stats.peakSpeed, m_speed);
            } else {
                m_inSaccade = false;
            }
        }
        m_last[0] = x / length; m_last[1] = y / length; m_last[2] = z / length;
        m_lastTime = time;
    }

    // Per eye and located frame: records the eye's velocity in this view (the lead during a saccade) and applies the
    // fixation deadzone. ndcPerDeg: NDC units per degree near the view centre, per axis. Returns the gaze to use.
    GazeVec2 Eye(int eye, int64_t time, GazeVec2 projected, float ndcPerDegX, float ndcPerDegY, const GazeSettings& s) {
        if (eye < 0 || eye >= Eyes) return projected;
        EyeState& e = m_eyes[eye];
        // A display time within 4 ms of this eye's last frame counts as that frame again (see UpdateSpeed).
        if (time != e.time && (e.time == 0 || time < e.time || (time - e.time) / 1e9f >= MinSampleSeconds)) {
            m_stats.eyeFrames++;
            const float dt = (time - e.time) / 1e9f;
            if (m_inSaccade && e.time != 0 && dt > 0.f && dt < 0.2f) {
                const float lead = s.leadMs / 1000.f / dt;
                e.lead = {std::clamp((projected.x - e.previous.x) * lead, -s.maxExtend, s.maxExtend),
                          std::clamp((projected.y - e.previous.y) * lead, -s.maxExtend, s.maxExtend)};
            } else if (m_inSaccade) {
                e.lead = {};  // a saccade right after a gap: no lead yet, never the previous saccade's
            }
            e.previous = projected;
            e.time = time;
        }
        if (s.deadzoneDeg > 0.f && !m_inSaccade && e.stableValid && ndcPerDegX > 0.f && ndcPerDegY > 0.f) {
            const float dx = (projected.x - e.stable.x) / ndcPerDegX, dy = (projected.y - e.stable.y) / ndcPerDegY;
            if (dx * dx + dy * dy < s.deadzoneDeg * s.deadzoneDeg) {
                if (e.heldTime != time) m_stats.held++;
                e.heldTime = time;
                return e.stable;
            }
        }
        e.stable = projected;
        e.stableValid = true;
        return projected;
    }

    // Per eye: extends the focus box [min, max] (NDC, clamped to -1..1) toward the lead while a saccade lasts and fades
    // it out over the hold time. Returns true when the box changed.
    bool Extend(int eye, int64_t time, GazeVec2& min, GazeVec2& max, const GazeSettings& s) {
        if (!s.widening || eye < 0 || eye >= Eyes || m_until == 0 || time > m_until) return false;
        const float fade = m_inSaccade || s.holdMs <= 0.f ? 1.f : std::clamp((m_until - time) / (s.holdMs * 1e6f), 0.f, 1.f);
        const GazeVec2 lead{m_eyes[eye].lead.x * fade, m_eyes[eye].lead.y * fade};
        if (lead.x == 0.f && lead.y == 0.f) return false;
        if (lead.x > 0.f) max.x = std::min(1.f, max.x + lead.x); else min.x = std::max(-1.f, min.x + lead.x);
        if (lead.y > 0.f) max.y = std::min(1.f, max.y + lead.y); else min.y = std::max(-1.f, min.y + lead.y);
        m_stats.widened++;
        return true;
    }

    // NDC units per degree near the centre of a view with these half-angle tangents.
    static float NdcPerDegree(float tanNear, float tanFar) {
        const float span = tanFar - tanNear;
        return span > 0.f ? 2.f / span * 0.0174532925f : 0.f;
    }

    bool InSaccade() const { return m_inSaccade; }
    float Speed() const { return m_speed; }
    const GazeStats& Stats() const { return m_stats; }
    void ResetStats() { m_stats = {}; }

  private:
    struct EyeState {
        GazeVec2 previous{}, lead{}, stable{};
        int64_t time{0};
        int64_t heldTime{0};
        bool stableValid{false};
    };
    float m_last[3]{};
    int64_t m_lastTime{0};
    float m_speed{0.f};
    bool m_inSaccade{false};
    int64_t m_until{0};
    EyeState m_eyes[Eyes]{};
    GazeStats m_stats{};
};

} // namespace dcsvr
