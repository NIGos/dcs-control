// Pimax PVR per-eye gaze for Depth boost's vergence, kept free of Windows calls so the tests can exercise it offline.
//
// The ABI mirror is Cheeky's (external/cheeky/src/pvr_abi.hpp): the PVR client (LibPVRClient64.dll, export
// getPvrInterface) hands out a table of functions; only the prefix shared by SDK 1.23 to 1.32 is used, and every entry
// up to getEyeTrackingInfo keeps its position in those versions. How the layer finds the runtime's own HMD handle (the
// interface table's frame and tracking slots, never the runtime's code) is in pupil_shift_layer.cpp.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "pupil_math.hpp"

namespace pupil_shift::pvr {

using Result = std::int32_t;
using HmdHandle = void*;
constexpr Result success = 0;
constexpr std::uint32_t major_version = 1;
constexpr std::uint32_t newest_minor_version = 32;
constexpr std::uint32_t oldest_minor_version = 23;
constexpr const wchar_t* client_module = L"LibPVRClient64.dll";
constexpr const char* interface_export = "getPvrInterface";

struct Vector2f { float x, y; };

// GazeTan: x right-positive, y up-positive (confirmed by Cheeky on a Pimax Crystal), one per eye (0 left, 1 right).
// TimeInSeconds == 0 marks an invalid sample. Clients before 1.30 write only GazeTan and TimeInSeconds.
struct alignas(8) EyeTrackingInfo {
    Vector2f GazeTan[2];
    double TimeInSeconds;
    float ConvergenceDistance;
    float blink[2];
};
static_assert(offsetof(EyeTrackingInfo, TimeInSeconds) == 16 && sizeof(EyeTrackingInfo) == 40, "EyeTrackingInfo");

// Positions in pvrInterfaceV23..V32; all other entries are opaque.
enum Slot : std::size_t {
    slot_destroy_hmd = 3,
    slot_get_time_seconds = 5,
    slot_get_tracking_state = 13,
    slot_end_frame = 31,
    slot_submit_frame = 34,
    slot_get_eye_tracking_info = 65,
    slot_count = 66,
};
struct Interface { void* slots[slot_count]; };

using GetInterfaceFn = Interface* (*)(std::uint32_t major, std::uint32_t minor);
using DestroyHmdFn = void (*)(HmdHandle);
using GetTimeSecondsFn = double (*)();
using GetTrackingStateFn = Result (*)(HmdHandle, double, void*);
using FrameFn = Result (*)(HmdHandle, long long, const void* const*, unsigned);
using GetEyeTrackingInfoFn = Result (*)(HmdHandle, double, EyeTrackingInfo*);

// One eye's tangents are usable: finite and within 84 degrees.
inline bool eye_valid(Vector2f t) { return std::isfinite(t.x) && std::isfinite(t.y) && std::fabs(t.x) <= 10.0f && std::fabs(t.y) <= 10.0f; }

// One eye of a query is a gaze: its tangents valid, the eye not reported closed (blink, clients 1.30+; older ones leave
// 0), and not both eyes at exactly (0, 0), which a tracker that lost the eyes writes and a real gaze never lands on.
inline bool eye_usable(const EyeTrackingInfo& i, int e) {
    const bool zeroed = i.GazeTan[0].x == 0 && i.GazeTan[0].y == 0 && i.GazeTan[1].x == 0 && i.GazeTan[1].y == 0;
    return eye_valid(i.GazeTan[e]) && !zeroed && !(std::isfinite(i.blink[e]) && i.blink[e] > 0.5f);
}

// One query's result, filtered. valid: both eyes usable (the per-eye flags gate everything: one bad eye is no sample),
// the sample time set and advancing within stall_ms; fresh: a new sample time (a repeat is the tracker's own hold, not
// a new measurement). head[e]: each eye's gaze in the head's frame (+X right, +Y up, -Z forward; PVR's tangents are
// taken as the headset's frame, not each canted display's: docs/FIXATION_DEPTH.md). convergence_m: the runtime's own
// ConvergenceDistance when it is set (clients 1.30+), else 0; logged for comparison, not used.
struct Sample {
    bool queried{}, valid{}, fresh{};
    bool eye_ok[2]{};
    Vec3 head[2]{{0, 0, -1}, {0, 0, -1}};
    float convergence_m{};
    double time{};
};

class GazeFilter {
public:
    static constexpr std::uint64_t stall_ms = 200;

    Sample update(bool queried, const EyeTrackingInfo& info, std::uint64_t now_ms) {
        Sample s;
        s.queried = queried;
        if (!queried) return s;
        for (int e = 0; e < 2; ++e) s.eye_ok[e] = eye_usable(info, e);
        const bool timed = std::isfinite(info.TimeInSeconds) && info.TimeInSeconds > 0;
        if (timed && info.TimeInSeconds != last_time_) { last_time_ = info.TimeInSeconds; last_change_ms_ = now_ms; seen_ = true; s.fresh = true; }
        const bool stalled = !seen_ || now_ms - last_change_ms_ > stall_ms;
        s.valid = timed && !stalled && s.eye_ok[0] && s.eye_ok[1];
        if (!s.valid) { s.fresh = false; return s; }
        s.time = info.TimeInSeconds;
        for (int e = 0; e < 2; ++e) s.head[e] = normalized({info.GazeTan[e].x, info.GazeTan[e].y, -1.0f});
        s.convergence_m = std::isfinite(info.ConvergenceDistance) && info.ConvergenceDistance > 0 ? info.ConvergenceDistance : 0.0f;
        return s;
    }
    void reset() { last_time_ = 0; last_change_ms_ = 0; seen_ = false; }

private:
    double last_time_{};
    std::uint64_t last_change_ms_{};
    bool seen_{};
};

// A head-frame direction in world space, from the head's basis.
inline Vec3 head_to_world(const HeadBasis& b, Vec3 d) { return normalized(b.right * d.x + b.up * d.y + b.forward * -d.z); }

}  // namespace pupil_shift::pvr
