// A stand-in for Pimax's PVR client, for pupil_shift_harness: built as LibPVRClient64.dll (in its own folder) so the
// layer finds it with GetModuleHandle, it exports getPvrInterface with two interface tables (1.32 and 1.25) that share
// their implementations, as a real client's versions do. The harness's fake runtime calls the frame and tracking slots
// with its HMD handle, through a table (as a runtime that keeps the table) or through copies taken before the layer
// patched it (as one that copied it). getEyeTrackingInfo answers with what the harness scripted, and counts calls made
// with any other handle.
// Stream mode (the predictive transition's high-rate sampler): the harness queues samples (FakePvr_Push); each
// getEyeTrackingInfo call takes the next one (the last one again when the queue is empty), so a sampler thread polling
// in wall time sees every sample of the simulated stream; the harness waits until the queue is drained. Calls made after
// the runtime destroyed its handle are counted.
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>

#include "pupil_math.hpp"
#include "pvr_gaze.hpp"

namespace {
using namespace pupil_shift;
using Provider = void (*)(pvr::EyeTrackingInfo* info);
Provider provider{};
double now_seconds{};
void* const kHmd = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x5150));
unsigned wrong_handle{}, eye_calls{}, frame_calls{}, tracking_calls{}, destroyed{}, after_destroy{};
std::mutex stream_mutex;
std::deque<pvr::EyeTrackingInfo> stream;
pvr::EyeTrackingInfo stream_last{};
int stream_mode{};  // 0 off, 1 queue (every sample, in order), 2 latest (a push replaces the queue)
unsigned stream_repeat{1}, stream_served{};  // queue mode: each sample answers this many calls (a tracker slower than the poll)
std::uint64_t stream_calls{};

void opaque() {}
void destroy_hmd(pvr::HmdHandle hmd) { if (hmd == kHmd) { std::lock_guard lock(stream_mutex); ++destroyed; } }
double time_seconds() { return now_seconds; }
pvr::Result tracking_state(pvr::HmdHandle hmd, double, void*) { ++tracking_calls; return hmd == kHmd ? pvr::success : -1; }
pvr::Result end_frame(pvr::HmdHandle hmd, long long, const void* const*, unsigned) { ++frame_calls; return hmd == kHmd ? pvr::success : -1; }
pvr::Result submit_frame(pvr::HmdHandle hmd, long long, const void* const*, unsigned) { ++frame_calls; return hmd == kHmd ? pvr::success : -1; }
pvr::Result eye_tracking(pvr::HmdHandle hmd, double, pvr::EyeTrackingInfo* info) {
    std::lock_guard lock(stream_mutex);
    ++eye_calls;
    if (destroyed) ++after_destroy;
    if (hmd != kHmd || !info) { ++wrong_handle; return -1; }
    std::memset(info, 0, sizeof *info);
    if (stream_mode) {
        if (!stream.empty() && (stream_mode == 2 || stream_served >= stream_repeat || stream_last.TimeInSeconds == 0)) {
            stream_last = stream.front(); stream.pop_front(); stream_served = 0;
        }
        ++stream_served;
        *info = stream_last;
        ++stream_calls;
        return pvr::success;
    }
    if (provider) provider(info);
    return pvr::success;
}

pvr::Interface make_table() {
    pvr::Interface t{};
    for (auto& s : t.slots) s = reinterpret_cast<void*>(&opaque);
    t.slots[pvr::slot_destroy_hmd] = reinterpret_cast<void*>(&destroy_hmd);
    t.slots[pvr::slot_get_time_seconds] = reinterpret_cast<void*>(&time_seconds);
    t.slots[pvr::slot_get_tracking_state] = reinterpret_cast<void*>(&tracking_state);
    t.slots[pvr::slot_end_frame] = reinterpret_cast<void*>(&end_frame);
    t.slots[pvr::slot_submit_frame] = reinterpret_cast<void*>(&submit_frame);
    t.slots[pvr::slot_get_eye_tracking_info] = reinterpret_cast<void*>(&eye_tracking);
    return t;
}
pvr::Interface table32 = make_table(), table25 = make_table();
}  // namespace

extern "C" __declspec(dllexport) pvr::Interface* getPvrInterface(std::uint32_t major, std::uint32_t minor) {
    if (major != 1) return nullptr;
    if (minor == 32) return &table32;
    if (minor == 25) return &table25;
    return nullptr;
}
// A fresh client's tables (each harness run loads its own copy of the layer, which keeps its slots patched for the life
// of the process: the next run starts from the client's own slots, as a new DCS process would).
extern "C" __declspec(dllexport) void FakePvr_Reset() { table32 = make_table(); table25 = make_table(); }
extern "C" __declspec(dllexport) void FakePvr_SetProvider(Provider p) { provider = p; }
extern "C" __declspec(dllexport) void FakePvr_SetTime(double seconds) { now_seconds = seconds; }
extern "C" __declspec(dllexport) void* FakePvr_Hmd() { return kHmd; }
// The original implementations (to check that the layer put every slot back).
extern "C" __declspec(dllexport) void* FakePvr_Original(std::uint32_t slot) { return slot < pvr::slot_count ? make_table().slots[slot] : nullptr; }
extern "C" __declspec(dllexport) void FakePvr_Stats(unsigned out[5]) {
    std::lock_guard lock(stream_mutex);
    out[0] = eye_calls; out[1] = wrong_handle; out[2] = frame_calls; out[3] = tracking_calls; out[4] = destroyed;
    eye_calls = wrong_handle = frame_calls = tracking_calls = destroyed = after_destroy = 0;
}
// Stream mode: 0 off (clears everything), 1 queue, 2 latest (a once-per-frame reader sees the newest sample).
extern "C" __declspec(dllexport) void FakePvr_StreamMode(int mode) {
    std::lock_guard lock(stream_mutex);
    if (mode == 0 || stream_mode == 0) { stream.clear(); stream_last = {}; stream_calls = 0; stream_served = 0; }
    if (mode == 0) stream_repeat = 1;
    stream_mode = mode;
}
// Queue mode: each sample answers `count` calls with the same TimeInSeconds, as a 120 Hz tracker polled at 500 Hz does
// (the layer's sampler must keep only the first).
extern "C" __declspec(dllexport) void FakePvr_StreamRepeat(unsigned count) { std::lock_guard lock(stream_mutex); stream_repeat = count ? count : 1; }
extern "C" __declspec(dllexport) void FakePvr_Push(const pvr::EyeTrackingInfo* samples, unsigned count) {
    std::lock_guard lock(stream_mutex);
    if (stream_mode == 2 && count) { stream.clear(); stream.push_back(samples[count - 1]); return; }
    for (unsigned i = 0; i < count; ++i) stream.push_back(samples[i]);
}
// Samples still queued, and the calls so far (stream mode).
extern "C" __declspec(dllexport) unsigned FakePvr_Pending(unsigned long long* calls) {
    std::lock_guard lock(stream_mutex);
    if (calls) *calls = stream_calls;
    return static_cast<unsigned>(stream.size());
}
extern "C" __declspec(dllexport) unsigned FakePvr_AfterDestroy() { std::lock_guard lock(stream_mutex); return after_destroy; }
