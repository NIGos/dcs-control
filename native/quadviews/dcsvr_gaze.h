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
// The saccade tail (`tail`, GitHub PR #26) changes how the lead's saccade is followed. It has its own state: the
// saccade other code reads (InSaccade: periphery stability, the gaze export for Pupil shift, Far depth) stays the plain
// one, every sample at or above `speed`. With `tail` off the lead uses that same saccade, as before the tail.
//  - Tail: a saccade starts above `speed` but only ends once the gaze turns slower than `endSpeed`. Saccades
//    decelerate sharply before they land, so with a single threshold the last frames before the landing (the ones that
//    reach the headset as the eye arrives) had no lead at all.
//  - Latched reach: the leading edge of the focus box never retreats during one saccade. The lead is velocity times
//    latency, and the velocity falls as the eye decelerates, so the reach shrank just before the landing; the farthest
//    point reached so far is kept instead, until the eye passes it or turns back.
//  - Repeated samples: Quad Views hands out its last gaze again while the tracker has nothing new (no new sample yet,
//    a blink, up to its 600 ms cache). Those frames are not speed samples: a repeat in the middle of a saccade read as
//    the eye stopping (0 deg/s) and ended the lead early. After a repeat or two the next real sample measures the speed
//    over the time since the last real one. After a longer run (more than RepeatStretchSeconds of repeats) it measures
//    over the last frame alone, as without repeats: the first frame of a new saccade must still read as a saccade (a
//    false start only widens the box for a moment). A repeat lasting 100 ms (longer than any saccade) ends the saccade.
//  - Landing check (statistics): when a lead saccade ends, the gaze of the next 100 ms is compared with the focus box
//    that was on screen at that moment (the box located for the display time `leadMs` earlier, since a gaze sample is
//    that much older than the frame it is located for). A landing outside the box's inner 80 % is counted as missed.
// Independent of the tail:
//  - Lost gaze: when the tracker loses the eyes for good (past Quad Views' cache), the focus eases from the last gaze to
//    the fixed focus over `lostEaseMs` instead of jumping there (0: off). When the gaze returns the focus jumps
//    straight to it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dcsvr {

struct GazeVec2 {
    float x{}, y{};
};

// The defaults are the filter without the tail and without the lost-gaze ease.
struct GazeSettings {
    bool widening{false};
    float speed{120.f};     // deg/s that start a saccade
    float leadMs{50.f};     // how far ahead the focus box reaches, as display latency
    float maxExtend{0.35f}; // NDC, per axis
    float holdMs{40.f};     // the lead fades out over this time after the saccade
    float deadzoneDeg{0.f}; // 0: off
    bool tail{false};       // the saccade tail: endSpeed, the latched reach, repeated samples skipped
    float endSpeed{50.f};   // with `tail`: deg/s below which a lead saccade ends (at most `speed`)
    float lostEaseMs{0.f};  // 0: the focus jumps to the fixed one when the gaze is lost
};

struct GazeStats {
    uint64_t eyeFrames{}, widened{}, held{}, saccades{};
    uint64_t repeated{}, landings{}, missed{}, eased{};
    float peakSpeed{};
};

class GazeFilter {
  public:
    static constexpr int Eyes = 2;
    static constexpr float MinSampleSeconds = 0.004f;
    // With the tail: the longest run of repeated samples the next speed measurement spans. A repeat or two in a saccade
    // (a tracker slower than the frames) are one or two frame times (11-14 ms each at 72-90 Hz) and are spanned; after a
    // longer run (a blink) the speed is measured over the last frame alone, so the next saccade's start is not hidden.
    static constexpr float RepeatStretchSeconds = 0.03f;

    // Once per located frame (time in ns, a display time): the gaze unit vector's angular speed since the previous one.
    // A frame located again for the same time, a non-finite vector or a gap of 200 ms or more changes nothing. Display
    // times closer than 4 ms (DCS may locate views for nearly the same time twice) are not a speed sample: dividing
    // tracker noise by microseconds read as thousands of degrees per second; the earlier sample stays the reference.
    void UpdateSpeed(int64_t time, float x, float y, float z, const GazeSettings& s) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || time == m_lastTime) return;
        if (m_lastTime != 0 && time < m_lastTime) return;  // an older display time: neither a sample nor the reference
        const float length = std::sqrt(x * x + y * y + z * z);
        if (length <= 0.f) return;
        const float unit[3]{x / length, y / length, z / length};
        if (m_lastTime != 0 && time > m_lastTime) {
            const float dt = (time - m_lastTime) / 1e9f;
            if (dt < MinSampleSeconds) return;
            // The plain saccade (InSaccade): every sample, one threshold.
            if (dt < 0.2f) {
                m_speed = std::acos(std::clamp((m_last[0] * x + m_last[1] * y + m_last[2] * z) / length, -1.f, 1.f)) * 57.2957795f / dt;
                m_inSaccade = m_speed >= s.speed;
            } else {
                m_inSaccade = false;
            }
            if (s.tail) {
                UpdateTail(time, x, y, z, unit, s);
            } else {
                // Without the tail the lead follows the plain saccade, as before the tail.
                SetLeadSaccade(m_inSaccade, time, s);
                if (dt < 0.2f) m_stats.peakSpeed = std::max(m_stats.peakSpeed, m_speed);
                SetLeadReference(time, x, y, z, unit);
            }
        } else {
            SetLeadReference(time, x, y, z, unit);
        }
        m_last[0] = unit[0]; m_last[1] = unit[1]; m_last[2] = unit[2];
        m_lastTime = time;
    }

    // Per eye and located frame: records the eye's velocity in this view (the lead during a saccade) and applies the
    // fixation deadzone. ndcPerDeg: NDC units per degree near the view centre, per axis. Returns the gaze to use.
    GazeVec2 Eye(int eye, int64_t time, GazeVec2 projected, float ndcPerDegX, float ndcPerDegY, const GazeSettings& s) {
        if (eye < 0 || eye >= Eyes) return projected;
        EyeState& e = m_eyes[eye];
        e.hadGaze = true;
        e.lostAt = 0;
        CheckLanding(e, time, projected, s);
        // A display time within 4 ms of this eye's last frame counts as that frame again (see UpdateSpeed), and with the
        // tail so does a repeated tracker sample: the lead keeps the last real velocity, the next real sample measures
        // over both.
        const bool newFrame = time != e.time && (e.time == 0 || time < e.time || (time - e.time) / 1e9f >= MinSampleSeconds);
        if (newFrame && time != e.counted) {
            m_stats.eyeFrames++;
            e.counted = time;
        }
        if (newFrame && !(s.tail && time == m_repeatTime)) {
            const float dt = (time - e.time) / 1e9f;
            if (m_leadSaccade && e.time != 0 && dt > 0.f && dt < 0.2f) {
                const float lead = s.leadMs / 1000.f / dt;
                const GazeVec2 now{std::clamp((projected.x - e.previous.x) * lead, -s.maxExtend, s.maxExtend),
                                   std::clamp((projected.y - e.previous.y) * lead, -s.maxExtend, s.maxExtend)};
                const bool same = s.tail && e.saccadeId == m_saccadeId;
                e.lead = {Latch(same, projected.x, now.x, e.reach.x, s.maxExtend),
                          Latch(same, projected.y, now.y, e.reach.y, s.maxExtend)};
                e.saccadeId = m_saccadeId;
            } else if (m_leadSaccade) {
                e.lead = {};  // a saccade right after a gap: no lead yet, never the previous saccade's
                e.reach = projected;
                e.saccadeId = m_saccadeId;
            }
            e.previous = projected;
            e.time = time;
        }
        if (s.deadzoneDeg > 0.f && !m_leadSaccade && e.stableValid && ndcPerDegX > 0.f && ndcPerDegY > 0.f) {
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
        const float fade = m_leadSaccade || s.holdMs <= 0.f ? 1.f : std::clamp((m_until - time) / (s.holdMs * 1e6f), 0.f, 1.f);
        const GazeVec2 lead{m_eyes[eye].lead.x * fade, m_eyes[eye].lead.y * fade};
        if (lead.x == 0.f && lead.y == 0.f) return false;
        if (lead.x > 0.f) max.x = std::min(1.f, max.x + lead.x); else min.x = std::max(-1.f, min.x + lead.x);
        if (lead.y > 0.f) max.y = std::min(1.f, max.y + lead.y); else min.y = std::max(-1.f, min.y + lead.y);
        m_stats.widened++;
        return true;
    }

    // Per eye and located frame, after Extend: the focus box this display time shows (NDC), for the landing check.
    void Record(int eye, int64_t time, GazeVec2 min, GazeVec2 max) {
        if (eye < 0 || eye >= Eyes) return;
        EyeState& e = m_eyes[eye];
        Box& last = e.boxes[(e.boxNext + BoxHistory - 1) % BoxHistory];
        if (last.time == time) { last.min = min; last.max = max; return; }
        e.boxes[e.boxNext] = {time, min, max};
        e.boxNext = (e.boxNext + 1) % BoxHistory;
    }

    // Per eye, on a frame without a gaze: while it lasts, the focus centre (NDC) easing from `from` (the last gaze's)
    // to `to` (the fixed focus's) since the gaze was lost. False: no ease (off, no gaze seen yet, or finished), the
    // caller shows the fixed focus.
    // progress (optional): the ease's share done, 0 .. 1, for EaseSection: the fixed focus is bigger than the tracked
    // one (Quad Views' fixed and foveated sections), so the box's size eases with its centre, and the hand-over to the
    // fixed focus at the end is not a step from the tracked size to the fixed size.
    bool Ease(int eye, int64_t time, GazeVec2 from, GazeVec2 to, const GazeSettings& s, GazeVec2& out, float* progress = nullptr) {
        if (s.lostEaseMs <= 0.f || eye < 0 || eye >= Eyes) return false;
        EyeState& e = m_eyes[eye];
        if (!e.hadGaze) return false;
        if (e.lostAt == 0) {
            e.lostAt = time;
            e.lostFrom = from;
            if (eye == 0) m_stats.eased++;
        }
        const float t = std::clamp((time - e.lostAt) / (s.lostEaseMs * 1e6f), 0.f, 1.f);
        if (t >= 1.f) return false;
        const float k = t * t * (3.f - 2.f * t);
        out = {e.lostFrom.x + (to.x - e.lostFrom.x) * k, e.lostFrom.y + (to.y - e.lostFrom.y) * k};
        if (progress) *progress = k;
        return true;
    }

    // The focus box's half size (NDC) while the focus eases: the tracked size (with its widening) at progress 0, the
    // fixed focus's at 1.
    static float EaseSection(float tracked, float fixed, float progress) {
        const float k = std::clamp(progress, 0.f, 1.f);
        return tracked + (fixed - tracked) * k;
    }

    // NDC units per degree near the centre of a view with these half-angle tangents.
    static float NdcPerDegree(float tanNear, float tanFar) {
        const float span = tanFar - tanNear;
        return span > 0.f ? 2.f / span * 0.0174532925f : 0.f;
    }

    // The plain saccade: the last sample at or above `speed`, whatever the tail. Read by the periphery stability, the
    // gaze export (Pupil shift) and Far depth.
    bool InSaccade() const { return m_inSaccade; }
    // The saccade the lead follows: InSaccade without the tail, with it the tail's.
    bool InLeadSaccade() const { return m_leadSaccade; }
    float Speed() const { return m_speed; }
    float LeadSpeed() const { return m_leadSpeed; }
    const GazeStats& Stats() const { return m_stats; }
    void ResetStats() { m_stats = {}; }

  private:
    static float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    void SetLeadReference(int64_t time, float x, float y, float z, const float unit[3]) {
        m_leadLast[0] = unit[0]; m_leadLast[1] = unit[1]; m_leadLast[2] = unit[2];
        m_lastRaw[0] = x; m_lastRaw[1] = y; m_lastRaw[2] = z;
        m_leadTime = time;
    }

    // The lead's saccade after a speed sample: counts starts and landings, and the hold runs from its last frame.
    void SetLeadSaccade(bool saccade, int64_t time, const GazeSettings& s) {
        if (saccade && !m_leadSaccade) { m_stats.saccades++; m_saccadeId++; }
        if (!saccade && m_leadSaccade) {
            // The landing: checked against the boxes on screen for the next 100 ms (see "Landing check").
            m_stats.landings++;
            m_landingUntil = time + 100'000'000;
            m_landingMissed = false;
        }
        m_leadSaccade = saccade;
        if (saccade) m_until = time + static_cast<int64_t>(s.holdMs * 1e6f);
    }

    // With the tail: the lead's saccade from real samples only (see "Repeated samples"), ending below endSpeed.
    void UpdateTail(int64_t time, float x, float y, float z, const float unit[3], const GazeSettings& s) {
        if (x == m_lastRaw[0] && y == m_lastRaw[1] && z == m_lastRaw[2]) {
            if (m_repeatTime != time) m_stats.repeated++;
            m_repeatTime = time;
            if (m_leadSaccade && (time - m_leadTime) / 1e9f >= 0.1f) SetLeadSaccade(false, time, s);
            return;
        }
        float dt = (time - m_leadTime) / 1e9f;
        if (m_repeatTime > m_leadTime && (m_repeatTime - m_leadTime) / 1e9f > RepeatStretchSeconds) dt = (time - m_repeatTime) / 1e9f;
        if (dt < 0.2f) {
            m_leadSpeed = std::acos(std::clamp(Dot(m_leadLast, unit), -1.f, 1.f)) * 57.2957795f / dt;
            // Hysteresis: once started, the saccade lasts through its deceleration down to endSpeed.
            const float threshold = m_leadSaccade ? std::min(s.endSpeed, s.speed) : s.speed;
            SetLeadSaccade(m_leadSpeed >= threshold, time, s);
            m_stats.peakSpeed = std::max(m_stats.peakSpeed, m_leadSpeed);
        } else {
            SetLeadSaccade(false, time, s);
        }
        SetLeadReference(time, x, y, z, unit);
    }

    // One axis of the lead. `now` is this frame's lead (velocity times latency) and `reach` the farthest point the
    // leading edge reached so far in this saccade (NDC). Within the same saccade, while the eye still moves the same
    // way and has not passed that point, the lead keeps reaching it; otherwise this frame's lead starts a new reach.
    static float Latch(bool sameSaccade, float position, float now, float& reach, float maxExtend) {
        if (sameSaccade && now != 0.f) {
            const float kept = std::clamp(reach - position, -maxExtend, maxExtend);
            if ((now > 0.f && kept > now) || (now < 0.f && kept < now)) return kept;
        }
        reach = position + now;
        return now;
    }

    static constexpr int BoxHistory = 24;  // 260 ms at 90 Hz: more than any lead
    struct Box {
        int64_t time{0};
        GazeVec2 min{}, max{};
    };
    struct EyeState {
        GazeVec2 previous{}, lead{}, stable{}, reach{}, lostFrom{};
        int64_t time{0};
        int64_t heldTime{0};
        int64_t lostAt{0};
        int64_t counted{0};  // the display time last counted in eyeFrames
        uint64_t saccadeId{0};
        bool stableValid{false};
        bool hadGaze{false};
        Box boxes[BoxHistory]{};
        int boxNext{0};
    };

    // While a landing is checked: the gaze sampled for display time `time` is where the eye was about leadMs before it,
    // so it is compared with the box located for that earlier display time. Outside the inner 80 % of the box's
    // inscribed ellipse counts as a miss, once per landing.
    void CheckLanding(const EyeState& e, int64_t time, GazeVec2 gaze, const GazeSettings& s) {
        if (m_landingUntil == 0 || time > m_landingUntil || m_leadSaccade || m_landingMissed) return;
        const int64_t shown = time - static_cast<int64_t>(s.leadMs * 1e6f);
        const Box* best = nullptr;
        for (const Box& b : e.boxes)
            if (b.time != 0 && b.time <= shown && (!best || b.time > best->time)) best = &b;
        if (!best) return;
        const float hx = (best->max.x - best->min.x) * 0.5f, hy = (best->max.y - best->min.y) * 0.5f;
        if (hx <= 0.f || hy <= 0.f) return;
        const float dx = (gaze.x - (best->min.x + hx)) / hx, dy = (gaze.y - (best->min.y + hy)) / hy;
        if (dx * dx + dy * dy > 0.8f * 0.8f) {
            m_landingMissed = true;
            m_stats.missed++;
        }
    }

    // The plain saccade's reference: the previous sample, repeats included.
    float m_last[3]{};
    int64_t m_lastTime{0};
    float m_speed{0.f};
    bool m_inSaccade{false};
    // The lead's: without the tail the same samples; with it the last real (not repeated) sample.
    float m_leadLast[3]{}, m_lastRaw[3]{};
    int64_t m_leadTime{0};
    int64_t m_repeatTime{0};
    float m_leadSpeed{0.f};
    bool m_leadSaccade{false};
    uint64_t m_saccadeId{0};  // not reset with the statistics: tells one saccade from the next
    int64_t m_until{0};
    int64_t m_landingUntil{0};
    bool m_landingMissed{false};
    EyeState m_eyes[Eyes]{};
    GazeStats m_stats{};
};

} // namespace dcsvr
