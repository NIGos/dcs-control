// A stand-in for the bundled Quad Views' gaze export, for pupil_shift_harness: built as
// XR_APILAYER_MBUCCHIA_quad_views_foveated.dll so the layer finds it with GetModuleHandle, it exports
// DcsVr_QuadViewsGaze (the signature of native/quadviews/dcsvr_perception.h's export) and forwards each call to the
// harness's fake runtime, which knows the gaze it scripted for every display time.
#include <cstdint>

namespace {
using Provider = std::uint32_t (*)(std::int64_t display_time, float left[3], float right[3]);
Provider provider{};
}  // namespace

extern "C" __declspec(dllexport) void FakeQv_SetProvider(Provider p) { provider = p; }

extern "C" __declspec(dllexport) std::uint32_t DcsVr_QuadViewsGaze(std::int64_t display_time, float left[3], float right[3]) {
    return provider ? provider(display_time, left, right) : 0;
}
