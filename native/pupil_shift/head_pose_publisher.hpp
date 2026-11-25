// The writer side of Local\DcsVrHeadPose_v1 (native/binaural/include/head_pose_shm.h) for the pupil shift layer: the
// head pose of each display time DCS locates, for binaural audio's late head rotation (docs/BINAURAL_AUDIO.md).
//
// Pure and header-only (no OpenXR types, no Windows calls) so the unit tests use exactly what the layer runs:
//   - Publisher::claim: once per display time, across threads (DCS may locate the same display time again);
//   - from_views: the head from the two primary eye views, when the runtime cannot locate the VIEW space;
//   - Publisher::publish: checks and normalises the pose, fills a missing angular / linear velocity from the previous
//     entry (same base space, at most 100 ms before), and writes the entry under the block's per-entry seqlock. One
//     writer at a time (its own mutex), entries strictly in display-time order.
// Publishing never changes the views the layer hands to DCS: it only reads them.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>

#include "head_pose_shm.h"

namespace head_pose {

namespace hp = dcsvr_headpose;

// A located head pose in the base space of xrLocateViews (OpenXR axes, metres, rad/s, m/s).
struct Sample {
    float orientation[4]{0, 0, 0, 1};  // x, y, z, w
    float position[3]{};
    float angular_velocity[3]{};
    float linear_velocity[3]{};
    std::uint32_t flags{};             // hp::entry_flags
};

// Normalises q in place; false when it is not finite or too far from unit length to be a rotation (|q| outside 0.5..2).
inline bool normalize_orientation(float q[4]) {
    const double n = std::sqrt(static_cast<double>(q[0]) * q[0] + static_cast<double>(q[1]) * q[1] +
                               static_cast<double>(q[2]) * q[2] + static_cast<double>(q[3]) * q[3]);
    if (!std::isfinite(n) || n < 0.5 || n > 2.0) return false;
    for (int i = 0; i < 4; ++i) q[i] = static_cast<float>(q[i] / n);
    return true;
}

inline bool finite3(const float v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// The head from the two primary views (left, right): their midpoint and the rotation half-way between them (the views
// may be canted outwards symmetrically, as Pimax's are). Flags from the view state; no velocity (publish derives it).
// False when the two orientations are not usable.
inline bool from_views(const float q_left[4], const float p_left[3], const float q_right[4], const float p_right[3],
                       bool orientation_valid, bool position_valid, bool orientation_tracked, bool position_tracked, Sample& out) {
    float l[4]{q_left[0], q_left[1], q_left[2], q_left[3]}, r[4]{q_right[0], q_right[1], q_right[2], q_right[3]};
    if (!orientation_valid || !normalize_orientation(l) || !normalize_orientation(r)) return false;
    // q and -q are the same rotation: average on the same hemisphere.
    const float sign = l[0] * r[0] + l[1] * r[1] + l[2] * r[2] + l[3] * r[3] < 0 ? -1.0f : 1.0f;
    Sample s;
    for (int i = 0; i < 4; ++i) s.orientation[i] = l[i] + sign * r[i];
    if (!normalize_orientation(s.orientation)) return false;
    s.flags = hp::entry_flags::orientation_valid | hp::entry_flags::from_views |
              (orientation_tracked ? hp::entry_flags::orientation_tracked : 0u);
    if (position_valid && finite3(p_left) && finite3(p_right)) {
        for (int i = 0; i < 3; ++i) s.position[i] = 0.5f * (p_left[i] + p_right[i]);
        s.flags |= hp::entry_flags::position_valid | (position_tracked ? hp::entry_flags::position_tracked : 0u);
    }
    out = s;
    return true;
}

// The longest gap over which a missing velocity is derived from the previous entry.
constexpr double kDeriveMaxS = 0.100;

class Publisher {
public:
    // True for the first caller with a display time newer than every earlier claim (lock-free).
    bool claim(std::int64_t display_time) {
        if (display_time <= 0) return false;
        std::int64_t seen = claimed_.load(std::memory_order_relaxed);
        while (display_time > seen)
            if (claimed_.compare_exchange_weak(seen, display_time, std::memory_order_relaxed)) return true;
        return false;
    }

    // A new instance: XrTime is comparable only within one, so the times start over (the entry numbers go on).
    void reset() {
        std::lock_guard lock(mutex_);
        claimed_.store(0, std::memory_order_relaxed);
        last_time_ = 0;
        have_last_ = false;
    }

    // Publishes s as the entry of display_time (seqlock per entry, head_pose_shm.h). Refused (false): no block, a display
    // time not newer than the last published one (a later claim published first), an orientation that is not a
    // rotation, or no orientation at all. A position or velocity that is not finite is dropped with its flag.
    bool publish(hp::Block* b, std::int64_t display_time, std::int64_t qpc_locate, std::int64_t qpc_display,
                 std::uint64_t base_space, Sample s) {
        if (!b || !(s.flags & hp::entry_flags::orientation_valid) || !normalize_orientation(s.orientation)) return false;
        if (!finite3(s.position)) s.flags &= ~(hp::entry_flags::position_valid | hp::entry_flags::position_tracked);
        if (!finite3(s.angular_velocity)) s.flags &= ~hp::entry_flags::angular_velocity_valid;
        if (!finite3(s.linear_velocity)) s.flags &= ~hp::entry_flags::linear_velocity_valid;
        std::lock_guard lock(mutex_);
        if (display_time <= last_time_) return false;
        derive_velocity(display_time, base_space, s);
        hp::Entry e{};
        e.displayTime = display_time;
        e.qpcLocate = qpc_locate;
        e.qpcDisplay = qpc_display;
        e.baseSpace = base_space;
        e.flags = s.flags;
        e.frame = frame_.fetch_add(1, std::memory_order_relaxed) + 1;
        for (int i = 0; i < 4; ++i) e.orientation[i] = s.orientation[i];
        for (int i = 0; i < 3; ++i) {
            e.position[i] = (s.flags & hp::entry_flags::position_valid) ? s.position[i] : 0.0f;
            e.angularVelocity[i] = (s.flags & hp::entry_flags::angular_velocity_valid) ? s.angular_velocity[i] : 0.0f;
            e.linearVelocity[i] = (s.flags & hp::entry_flags::linear_velocity_valid) ? s.linear_velocity[i] : 0.0f;
        }
        hp::Publish(b, e);
        last_time_ = display_time;
        last_base_ = base_space;
        last_flags_ = s.flags;
        for (int i = 0; i < 4; ++i) last_q_[i] = s.orientation[i];
        for (int i = 0; i < 3; ++i) last_p_[i] = s.position[i];
        have_last_ = true;
        return true;
    }

    std::uint32_t frames() const { return frame_.load(std::memory_order_relaxed); }

private:
    // A velocity the runtime did not give, from the previous entry of the same base space (display times, at most
    // kDeriveMaxS apart). Angular: q = dq * q_last in base axes, so dq = q * conj(q_last).
    void derive_velocity(std::int64_t display_time, std::uint64_t base_space, Sample& s) const {
        if (!have_last_ || base_space != last_base_) return;
        const double dt = static_cast<double>(display_time - last_time_) * 1e-9;
        if (!(dt > 0) || dt > kDeriveMaxS) return;
        if (!(s.flags & hp::entry_flags::angular_velocity_valid)) {
            const hp::Quat q{s.orientation[0], s.orientation[1], s.orientation[2], s.orientation[3]};
            const hp::Quat ql{last_q_[0], last_q_[1], last_q_[2], last_q_[3]};
            hp::Quat dq = hp::Normalize(hp::Mul(q, hp::Conj(ql)));
            if (dq.w < 0) dq = {-dq.x, -dq.y, -dq.z, -dq.w};
            const double sn = std::sqrt(dq.x * dq.x + dq.y * dq.y + dq.z * dq.z);
            const double angle = 2.0 * std::atan2(sn, dq.w);
            const double k = sn > 1e-12 ? angle / (sn * dt) : 2.0 / dt;  // small angles: 2 v / dt
            s.angular_velocity[0] = static_cast<float>(dq.x * k);
            s.angular_velocity[1] = static_cast<float>(dq.y * k);
            s.angular_velocity[2] = static_cast<float>(dq.z * k);
            if (finite3(s.angular_velocity)) s.flags |= hp::entry_flags::angular_velocity_valid;
        }
        if (!(s.flags & hp::entry_flags::linear_velocity_valid) && (s.flags & hp::entry_flags::position_valid) &&
            (last_flags_ & hp::entry_flags::position_valid)) {
            for (int i = 0; i < 3; ++i) s.linear_velocity[i] = static_cast<float>((s.position[i] - last_p_[i]) / dt);
            if (finite3(s.linear_velocity)) s.flags |= hp::entry_flags::linear_velocity_valid;
        }
    }

    std::atomic<std::int64_t> claimed_{0};
    std::mutex mutex_;
    std::int64_t last_time_{};
    std::uint64_t last_base_{};
    std::uint32_t last_flags_{};
    float last_q_[4]{0, 0, 0, 1};
    float last_p_[3]{};
    bool have_last_{};
    std::atomic<std::uint32_t> frame_{0};
};

}  // namespace head_pose
