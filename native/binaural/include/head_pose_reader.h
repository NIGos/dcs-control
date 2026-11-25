// DCS Control: the reader side of Local\DcsVrHeadPose_v1 (head_pose_shm.h), for the binaural audio module.
//
// Real-time safe: no allocation, locks, waits, system calls or logging; bounded work (a few entry copies). The caller
// passes the clocks (QueryPerformanceCounter) and opens the mapping outside the audio callback (head_pose_shm.h
// SharedBlock, or dcsvr_perception::SharedBlock<Block>).
//
//   ReadNewest       the newest complete entry, tear-free (per-entry seqlock), at most `attempts` tries while the writer
//                    laps the reader; then kBusy, never a torn copy.
//   ReadPredicted    the newest entry, checked (orientation a rotation, the writer the expected process, not stale), its
//                    orientation extrapolated by its angular velocity and its position by its linear velocity to
//                    qpcTarget, by at most maxPredictionS either way (<= kMaxPredictionS); a velocity above the speed
//                    limit is taken as a glitch: no extrapolation for that part.
//   ReadUsed         the entry of one display time (the pose DCS rendered and set the listener with).
//   LateCorrectionNow  the rotation / translation from the pose of a used display time to the predicted pose now
//                    (head_pose_shm.h ComputeLateCorrection), both from the same base space.
//
// Staleness: an entry is stale when it was written more than staleS before qpcNow (the layer writes once per DCS frame:
// a paused, loading or closed DCS view, or Publish switched off). Stale means "no late correction": render with DCS's
// own listener.
//
// Header-only, C++17.
#pragma once

#include <cmath>
#include <cstdint>

#include "head_pose_shm.h"

namespace dcsvr_headpose {

enum class PoseStatus : std::uint32_t {
    kOk = 0,
    kNoBlock,     // no mapping
    kNotReady,    // never initialised by a writer (magic / version)
    kWrongWriter, // written by another process than expectedPid
    kEmpty,       // no entry yet
    kBusy,        // the writer lapped every attempt (try again next block)
    kInvalid,     // the entry is not a usable pose (no orientation, not a rotation, clock in the future)
    kStale,       // the newest entry is older than staleS
    kNotFound,    // ReadUsed: no entry for that display time in the ring
    kOtherSpace,  // LateCorrectionNow: the two poses are in different base spaces
};

inline const char* PoseStatusName(PoseStatus s) {
    switch (s) {
    case PoseStatus::kOk: return "ok";
    case PoseStatus::kNoBlock: return "no block";
    case PoseStatus::kNotReady: return "not ready";
    case PoseStatus::kWrongWriter: return "wrong writer";
    case PoseStatus::kEmpty: return "empty";
    case PoseStatus::kBusy: return "busy";
    case PoseStatus::kInvalid: return "invalid";
    case PoseStatus::kStale: return "stale";
    case PoseStatus::kNotFound: return "not found";
    case PoseStatus::kOtherSpace: return "other space";
    }
    return "?";
}

struct ReaderLimits {
    double staleS = 0.150;                  // newest entry older than this: stale
    double maxPredictionS = kMaxPredictionS;  // extrapolation horizon either way (clamped to kMaxPredictionS)
    double maxAngularSpeed = 17.5;          // rad/s (about 1000 deg/s; fast head turns peak at 600-800 deg/s)
    double maxLinearSpeed = 5.0;            // m/s
    std::uint32_t expectedPid = 0;          // 0: any writer; else the writer must be this process
    int attempts = 4;                       // ReadNewest tries
};

struct PredictedPose {
    Entry entry{};           // the entry it was predicted from
    Quat orientation{0, 0, 0, 1};  // OpenXR, base space
    Vec3 position{0, 0, 0};
    double ageS = 0;         // qpcNow - entry.qpcLocate
    double aheadS = 0;       // qpcTarget - entry time, after the horizon clamp (negative: back in time)
    bool rotated = false;    // the orientation was extrapolated
    bool translated = false; // the position was extrapolated
};

// An entry the readers can use: an orientation that is a finite unit quaternion (the writer normalises it).
inline bool EntryUsable(const Entry& e) {
    if (!(e.flags & entry_flags::orientation_valid)) return false;
    const double n2 = static_cast<double>(e.orientation[0]) * e.orientation[0] + static_cast<double>(e.orientation[1]) * e.orientation[1] +
                      static_cast<double>(e.orientation[2]) * e.orientation[2] + static_cast<double>(e.orientation[3]) * e.orientation[3];
    return std::isfinite(n2) && n2 > 0.98 && n2 < 1.02;
}

inline PoseStatus CheckBlock(const Block* b, const ReaderLimits& limits) {
    if (!b) return PoseStatus::kNoBlock;
    if (!Ready(b)) return PoseStatus::kNotReady;
    if (limits.expectedPid && b->writerPid != limits.expectedPid) return PoseStatus::kWrongWriter;
    return PoseStatus::kOk;
}

// The newest complete entry, tear-free; kBusy after `attempts` lapped tries.
inline PoseStatus ReadNewest(const Block* b, const ReaderLimits& limits, Entry& out) {
    const PoseStatus s = CheckBlock(b, limits);
    if (s != PoseStatus::kOk) return s;
    const int attempts = limits.attempts < 1 ? 1 : (limits.attempts > 64 ? 64 : limits.attempts);
    for (int a = 0; a < attempts; ++a) {
        const std::uint64_t n = AtomicU64(&b->writeCount).load(std::memory_order_acquire);
        if (n == 0) return PoseStatus::kEmpty;
        if (ReadIndex(b, n, out) || (n > 1 && ReadIndex(b, n - 1, out))) return PoseStatus::kOk;
    }
    return PoseStatus::kBusy;
}

inline double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Seconds from the entry's time (QpcOf: display time when known, else locate time) to qpcTarget, clamped to the
// horizon. 0 without a QPC frequency.
inline double AheadSeconds(const Entry& e, std::int64_t qpcTarget, std::int64_t qpcFrequency, const ReaderLimits& limits) {
    if (qpcFrequency <= 0) return 0;
    const double horizon = Clamp(limits.maxPredictionS, 0.0, kMaxPredictionS);
    const double dt = static_cast<double>(qpcTarget - QpcOf(e)) / static_cast<double>(qpcFrequency);
    return std::isfinite(dt) ? Clamp(dt, -horizon, horizon) : 0.0;
}

// The orientation `dt` seconds after the entry (dt already clamped), by its angular velocity (base-space axes, so the
// increment multiplies on the left). rotated = false: no valid velocity, or above the speed limit.
inline Quat ExtrapolateOrientation(const Entry& e, double dt, const ReaderLimits& limits, bool& rotated) {
    rotated = false;
    const Quat q = OrientationOf(e);
    if (!(e.flags & entry_flags::angular_velocity_valid) || dt == 0) return q;
    const Vec3 w{e.angularVelocity[0], e.angularVelocity[1], e.angularVelocity[2]};
    const double speed = std::sqrt(w.x * w.x + w.y * w.y + w.z * w.z);
    if (!std::isfinite(speed) || speed > limits.maxAngularSpeed) return q;
    if (speed < 1e-9) { rotated = true; return q; }
    rotated = true;
    return Normalize(Mul(AxisAngle({w.x / speed, w.y / speed, w.z / speed}, speed * dt), q));
}

inline Vec3 ExtrapolatePosition(const Entry& e, double dt, const ReaderLimits& limits, bool& translated) {
    translated = false;
    const Vec3 p = PositionOf(e);
    if (!(e.flags & entry_flags::position_valid)) return {0, 0, 0};
    if (!(e.flags & entry_flags::linear_velocity_valid) || dt == 0) return p;
    const Vec3 v{e.linearVelocity[0], e.linearVelocity[1], e.linearVelocity[2]};
    const double speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!std::isfinite(speed) || speed > limits.maxLinearSpeed) return p;
    translated = true;
    return {p.x + v.x * dt, p.y + v.y * dt, p.z + v.z * dt};
}

// The newest pose, predicted to qpcTarget (e.g. the audio block's output time). qpcNow is the reader's clock now.
inline PoseStatus ReadPredicted(const Block* b, std::int64_t qpcNow, std::int64_t qpcTarget, const ReaderLimits& limits, PredictedPose& out) {
    Entry e;
    const PoseStatus s = ReadNewest(b, limits, e);
    if (s != PoseStatus::kOk) return s;
    if (!EntryUsable(e)) return PoseStatus::kInvalid;
    const std::int64_t f = b->qpcFrequency;
    if (f <= 0) return PoseStatus::kInvalid;
    const double age = static_cast<double>(qpcNow - e.qpcLocate) / static_cast<double>(f);
    if (age < -0.010) return PoseStatus::kInvalid;  // written "in the future": another clock
    if (age > limits.staleS) return PoseStatus::kStale;
    out.entry = e;
    out.ageS = age;
    out.aheadS = AheadSeconds(e, qpcTarget, f, limits);
    out.orientation = ExtrapolateOrientation(e, out.aheadS, limits, out.rotated);
    out.position = ExtrapolatePosition(e, out.aheadS, limits, out.translated);
    return PoseStatus::kOk;
}

// The entry of `displayTime` (exact), checked.
inline PoseStatus ReadUsed(const Block* b, std::int64_t displayTime, const ReaderLimits& limits, Entry& out) {
    const PoseStatus s = CheckBlock(b, limits);
    if (s != PoseStatus::kOk) return s;
    if (AtomicU64(&b->writeCount).load(std::memory_order_acquire) == 0) return PoseStatus::kEmpty;
    if (!ReadForDisplayTime(b, displayTime, out)) return PoseStatus::kNotFound;
    return EntryUsable(out) ? PoseStatus::kOk : PoseStatus::kInvalid;
}

// The late correction (DCS listener frame) from the pose DCS used (its display time) to the pose predicted for
// qpcTarget. Both poses from one base space; positions only when both have one (else rotation only).
inline PoseStatus LateCorrectionNow(const Block* b, std::int64_t usedDisplayTime, std::int64_t qpcNow, std::int64_t qpcTarget,
                                    const ReaderLimits& limits, LateCorrection& out, PredictedPose* now = nullptr) {
    PredictedPose p;
    PoseStatus s = ReadPredicted(b, qpcNow, qpcTarget, limits, p);
    if (s != PoseStatus::kOk) return s;
    Entry used;
    s = ReadUsed(b, usedDisplayTime, limits, used);
    if (s != PoseStatus::kOk) return s;
    if (used.baseSpace != p.entry.baseSpace) return PoseStatus::kOtherSpace;
    const bool positions = (used.flags & entry_flags::position_valid) && (p.entry.flags & entry_flags::position_valid);
    const Vec3 zero{0, 0, 0};
    out = ComputeLateCorrection(OrientationOf(used), positions ? PositionOf(used) : zero, p.orientation, positions ? p.position : zero);
    if (now) *now = p;
    return PoseStatus::kOk;
}

}  // namespace dcsvr_headpose
