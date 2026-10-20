// XR_APILAYER_DCSVR_pupil_shift: renders each eye from where its pupil actually is (Pupil shift), and scales the
// render IPD up while the eyes fixate far (Depth boost). The two are independent: either can run alone.
//
// xrLocateViews: the gaze comes from the bundled Quad Views' DcsVr_QuadViewsGaze export when it is loaded (it also
// says when there is no real gaze: its fixed focus area is not one), else from the runtime's eye gaze through
// XR_EXT_eye_gaze_interaction, which the layer enables and reads with its own action (Pimax's native quad views; the
// runtime says when the gaze is not tracked), and only when neither exists from the focus views of a foveated
// quad-view configuration (their FOV is centred on the gaze, or on a fixed spot without one). Each eye's pose is moved
// to the pupil position for that gaze, and its FOV is shifted so the lens's virtual image plane stays where it was.
// xrEndFrame: the original poses and FOVs are put back on the submitted projection views, so the
// runtime places the image exactly where it would have without the layer; only the content
// changes. Settings live in PupilShift.ini next to the DLL and are re-read while running.
// Depth boost (docs/FIXATION_DEPTH.md): the fixation depth comes from the engine module's scene depth at the gaze,
// exchanged in shared memory (perception_shm.h: the layer writes its gaze, reads the summaries), and from the vergence
// of the two eyes' gazes (FixationEstimator; Pimax PVR's per-eye gaze when the runtime's session is found).
// FixationDecider applies the overlap rules; DepthBoostController turns the result into an IPD scale that DCS renders
// natively, applied with the same eye move as DCS's custom IPD. xrEndFrame restores the runtime's poses as for the
// pupil shift. The predictive transition (predictive_depth.hpp, [DepthBoost] Transition=predictive or the in-flight
// key) polls PVR's gaze on a small thread at ~500 Hz, predicts a saccade's landing at its velocity peak and takes one
// capped step towards the landing's scale (the engine's depth grid) while the eyes are in flight.
// PupilShift_Status() reports the state to the in-headset diagnostic panel (OFXR).
#define XR_USE_PLATFORM_WIN32
#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "perception_shm.h"
#include "dcsvr_pupil_fov.h"
#include "pupil_math.hpp"
#include "predictive_depth.hpp"
#include "pvr_gaze.hpp"

namespace {

using namespace pupil_shift;
constexpr const char* layer_name = "XR_APILAYER_DCSVR_pupil_shift";

PFN_xrGetInstanceProcAddr next_gipa{};
PFN_xrLocateViews next_locate_views{};
PFN_xrEndFrame next_end_frame{};
// The calls below this layer for the OpenXR eye gaze (the hooked ones are the application's own, merged).
struct EyeGazeDispatch {
    PFN_xrCreateActionSet create_action_set{};
    PFN_xrDestroyActionSet destroy_action_set{};
    PFN_xrCreateAction create_action{};
    PFN_xrStringToPath string_to_path{};
    PFN_xrGetSystemProperties get_system_properties{};
    PFN_xrCreateActionSpace create_action_space{};
    PFN_xrLocateSpace locate_space{};
    PFN_xrSuggestInteractionProfileBindings suggest_bindings{};
    PFN_xrAttachSessionActionSets attach_action_sets{};
    PFN_xrSyncActions sync_actions{};
    PFN_xrCreateSession create_session{};
    PFN_xrDestroySession destroy_session{};
    PFN_xrDestroyInstance destroy_instance{};
} xrd;
// The bundled Quad Views' gaze export (pupil_math.hpp, qv_gaze), resolved when the instance is created (the loader has
// loaded every layer by then) and once more at the first xrLocateViews; null with Pimax's native quad views.
std::atomic<QuadViewsGazeFn> qv_gaze_fn{};
std::atomic<int> live_instances{};          // instances created through the layer and not destroyed yet
std::atomic<XrTime> pvr_sampled_time{};     // the latest display time PVR was sampled for (reset with each instance)
bool qv_checked_late{};

std::mutex mutex;
std::wstring module_dir;
Settings settings;
DepthBoostSettings depth_settings;
FILETIME ini_time{};
unsigned reload_counter{};
// An in-flight switch, Toggle=virtual-key:modifiers (1 Ctrl, 2 Alt, 4 Shift; 0:0 off), the same format as DCS
// Control's other in-flight keys. [PupilShift] Toggle (default Ctrl+Shift+F10) switches the pupil correction,
// [DepthBoost] Toggle (default Alt+Shift+F10) Depth boost: separate keys, so each can be compared alone.
struct Hotkey {
    const char* name;
    int default_vk, default_mods;
    int vk, mods;
    unsigned off_tone, on_tone;
    bool off{}, down{};
    ULONGLONG at{};  // GetTickCount64 of the last switch
};
// A switch is the key's own press with exactly its modifiers held at that moment, at most once per 300 ms. Modifier
// changes while the key is held never count: Windows' Alt+Shift (input language) flickers the modifier state, which
// read as a second press and switched straight back.
bool hotkey_pressed(Hotkey& key, bool held, int mods) {
    const bool pressed = held && !key.down && mods == key.mods && GetTickCount64() - key.at >= 300;
    if (pressed) key.at = GetTickCount64();
    key.down = held;
    return pressed;
}
Hotkey pupil_key{"correction", VK_F10, 1 | 4, VK_F10, 1 | 4, 600, 1200};
Hotkey depth_key{"depth boost", VK_F10, 2 | 4, VK_F10, 2 | 4, 400, 800};
// [DepthBoost] TransitionToggle (default Alt+Shift+F9): switches Depth boost's transition between gradual and predictive
// in flight, for A/B comparison (one beep gradual, two beeps predictive).
Hotkey transition_key{"transition", VK_F9, 2 | 4, VK_F9, 2 | 4, 1000, 1000};
// [DepthBoost] Transition from the ini, and the in-flight key's choice over it (-1 none; a changed ini value clears it).
predictive::Transition ini_transition{predictive::Transition::gradual};
int transition_override{-1};
predictive::Settings predict_settings;
std::atomic<bool> predict_wanted{};  // Depth boost on, PimaxPvrGaze=1 and the predictive transition selected
std::atomic<unsigned> predict_poll_hz{500};
FILE* log_file{};
std::atomic<unsigned> restored_views{};
// PupilShift_Status(): the layout is in pupil_math.hpp (namespace status). Bits 0-4 and 8-15 are the pupil shift's
// (bit 1 correction on), the rest Depth boost's; an older panel that knows only the first ones keeps working.
std::atomic<std::uint64_t> status_word{};
// The eye distance and the pupil, per frame, for the panel (PupilShift_Ipd below): bits 0-15 the base IPD, 16-31 the
// rendered IPD now (measured between the views after the IPD move: base x Depth boost's scale when it applies), 32-47 the pupil offset (the larger eye's), all in 0.01 mm; bit 48
// the base is the runtime's, bit 49 it is Ipd=. 0 until the first frame.
std::atomic<std::uint64_t> ipd_word{};

// Depth boost state, under the mutex.
FixationEstimator fixation;
FixationDecider decider;
DepthMatcher matcher;
std::uint32_t last_depth_flags{};  // the engine module's latest depth_flags, for the log
DepthBoostController depth;
XrTime depth_time{};
std::uint64_t display_frames{};  // display times Depth boost has processed (the matcher's frame count)
// The two shared-memory blocks with the engine module (perception_shm.h), opened lazily; the suffix comes from
// DCSVR_PERCEPTION_SHM_SUFFIX (the harness's own names).
dcsvr_perception::SharedBlock<dcsvr_perception::GazeOut> gaze_block;
dcsvr_perception::SharedBlock<dcsvr_perception::DepthIn> depth_block;
wchar_t shm_suffix[64]{};
unsigned shm_retry{};
XrTime gaze_out_time{};
// The gaze Depth boost used for a display time (world space), kept for the GazeOut block.
struct DepthGaze { bool valid{}, per_eye{}, saccade{}; GazeSource source{GazeSource::none}; Vec3 world[2]{{0, 0, -1}, {0, 0, -1}}; Vec3 head_dir{0, 0, -1}; };
DepthGaze depth_gaze;
// The pupil correction's gaze filter, per eye (PupilGazeFilter), and the display time it last advanced.
PupilGazeFilter pupil_filter;
PupilLock pupil_lock;                       // when the pupil moves: during saccades only (PupilLock)
Vec3 pupil_filtered[2]{{0, 0, -1}, {0, 0, -1}}, pupil_locked[2]{{0, 0, -1}, {0, 0, -1}};
bool pupil_filtered_valid{};
XrTime pupil_time{};
std::uint64_t last_fresh_scene_frame{};  // the display frame of the last fresh scene depth summary (the panel's "no depth")
// The predictive transition (predictive_depth.hpp), under the mutex: the saccade detector on the high-rate stream, the
// gate that takes and holds a step, the engine's depth grid (its latest consistent copy, the frame it arrived) and the
// primary and focus views' orientations of recent display times (the grid is projected with its own frame's pose).
predictive::SaccadeDetector detector;
predictive::PredictiveGate gate;
dcsvr_perception::SharedBlock<dcsvr_perception::DepthGrid> grid_block;
// Local\DcsVrDepthBoost_v1: the eye distance of each display time, for the engine's dual camera (perception_shm.h).
dcsvr_perception::SharedBlock<dcsvr_perception::BoostOut> boost_block;
XrTime boost_out_time{};
unsigned boost_retry{};
float boost_last_base{};  // the base distance of the last record (for the final one at the session's end)
// Local\DcsVrCockpitStereo_v1: the engine's acknowledgement that its dual camera keeps the cockpit at the true eye
// distance (perception_shm.h). While fresh, Depth boost holds one world scale (cockpit_stereo_acked_locked).
dcsvr_perception::SharedBlock<dcsvr_perception::CockpitStereo> stereo_block;
unsigned stereo_retry{};
bool stereo_acked{};
std::int64_t stereo_last_ok{};  // QPC of the last active record
float dual_far_target{1.0f};    // the last far fixation's target while the dual camera is acknowledged
// [DepthBoost] Legacy=1 (diagnostics only): the gaze-driven Depth boost without the engine's dual camera, the cockpit
// returning to x1 in saccades (PERCEPTION.md "Stepped"). Default 0: Depth boost only with the dual camera.
bool depth_legacy{};
int depth_path_logged{-1};  // the path last logged: 0 waiting (x1), 1 dual camera, 2 legacy
unsigned dual_log_frames{};
float dual_camera_mm_per_s{20.0f};
std::uint32_t boost_out_frames{};
dcsvr_perception::DepthGrid grid{};
std::uint64_t grid_seq{}, grid_arrival{};
bool have_grid{};
unsigned grid_retry{};
struct PoseAt { XrTime time{}; std::uint64_t frame{}; std::uint32_t count{}; Quat q[4]{}; };
std::array<PoseAt, 16> pose_ring{};
unsigned pose_next{};
float frame_period_s{1.0f / 45.0f};
bool predict_effective{};
const char* predict_reason{};      // why predictive is selected but gradual in effect (string literals only)
bool predict_logged_effective{}, predict_logged_init{};
const char* predict_logged_reason{};
ULONGLONG last_stream_tick{};
// The predict: line's second: the gate's and detector's counters at its start, samples, grid frames.
struct PredictSecond {
    bool started{};
    predictive::PredictiveGate::Stats gate{};
    unsigned saccades{}, dropped{};
    unsigned samples{}, frames{}, effective_frames{}, grid_fresh{}, grid_stale{};
} predict_second;
bool pvr_wanted_setting{true};  // [DepthBoost] PimaxPvrGaze
bool depth_trace{};             // [DepthBoost] Trace=1: one "dtrace:" line per display time (diagnostics only)
std::atomic<bool> pvr_wanted{};  // Depth boost enabled and PimaxPvrGaze=1: attach to Pimax's PVR client
std::array<XrFovf, 2> last_focus_fov{};
bool have_last_focus{};
std::array<Vec3, 2> last_qv{};
bool have_last_qv{};

void log(const char* format, ...) {
    if (!log_file) return;
    SYSTEMTIME t; GetLocalTime(&t);
    std::fprintf(log_file, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args; va_start(args, format);
    std::vfprintf(log_file, format, args);
    va_end(args);
    std::fflush(log_file);
}

float ini_float(const wchar_t* key, float fallback, const std::wstring& path, const wchar_t* section = L"PupilShift") {
    wchar_t text[64]{};
    GetPrivateProfileStringW(section, key, L"", text, 64, path.c_str());
    if (!text[0]) return fallback;
    wchar_t* end{};
    const float value = std::wcstof(text, &end);
    return end != text && std::isfinite(value) ? value : fallback;
}

// "virtual-key:modifiers"; false (keeps the previous key) for anything else.
bool ini_hotkey(const std::wstring& path, const wchar_t* section, int& vk, int& mods, const wchar_t* name = L"Toggle") {
    wchar_t text[32]{};
    GetPrivateProfileStringW(section, name, L"", text, 32, path.c_str());
    int v{}, m{};
    if (!text[0] || swscanf_s(text, L"%d:%d", &v, &m) != 2 || v < 0 || v > 0xFE || m < 0 || m > 7) return false;
    vk = v; mods = m;
    return true;
}

// Re-reads PupilShift.ini when it changes (checked about once a second at 90 Hz).
void reload_settings_locked(bool force) {
    if (!force && ++reload_counter % 90 != 0) return;
    const std::wstring path = module_dir + L"PupilShift.ini";
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return;
    if (!force && CompareFileTime(&data.ftLastWriteTime, &ini_time) == 0) return;
    ini_time = data.ftLastWriteTime;
    for (auto [key, section] : {std::pair{&pupil_key, L"PupilShift"}, std::pair{&depth_key, L"DepthBoost"}})
        if (!ini_hotkey(path, section, key->vk, key->mods)) { key->vk = key->default_vk; key->mods = key->default_mods; }
    if (!ini_hotkey(path, L"DepthBoost", transition_key.vk, transition_key.mods, L"TransitionToggle")) {
        transition_key.vk = transition_key.default_vk; transition_key.mods = transition_key.default_mods;
    }
    Settings s;
    s.enabled = ini_float(L"Enabled", 1, path) != 0;
    s.eye_radius = std::clamp(ini_float(L"EyeRadiusMm", 10.5f, path), 0.0f, 30.0f) / 1000.0f;
    s.virtual_image = std::clamp(ini_float(L"VirtualImageM", 1.5f, path), 0.2f, 100.0f);
    s.max_gaze_deg = std::clamp(ini_float(L"MaxGazeDeg", 35.0f, path), 0.0f, 60.0f);
    s.smooth_s = std::clamp(ini_float(L"SmoothMs", 250.0f, path), 0.0f, 1000.0f) / 1000.0f;
    const float ipd_mm = ini_float(L"Ipd", 0, path);
    s.ipd = ipd_mm >= 40 && ipd_mm <= 90 ? ipd_mm / 1000.0f : 0;
    s.simulate_x_deg = std::clamp(ini_float(L"SimulateGazeXDeg", 0, path), -60.0f, 60.0f);
    s.simulate_y_deg = std::clamp(ini_float(L"SimulateGazeYDeg", 0, path), -60.0f, 60.0f);
    s.simulate = ini_float(L"SimulateGaze", 0, path) != 0;
    s.publish_gaze = ini_float(L"PublishGaze", 0, path) != 0;
    settings = s;
    log("settings: enabled=%d eye_radius=%.1fmm virtual_image=%.2fm max_gaze=%.0fdeg simulate=%d (%.1f, %.1f) toggle=%d:%d ipd=%.1fmm publish_gaze=%d\n",
        s.enabled, s.eye_radius * 1000, s.virtual_image, s.max_gaze_deg, s.simulate, s.simulate_x_deg, s.simulate_y_deg,
        pupil_key.vk, pupil_key.mods, s.ipd * 1000, s.publish_gaze ? 1 : 0);
    // [DepthBoost]: off unless Enabled=1. The ranges are the app's; the layer clamps them again.
    DepthBoostSettings d;
    d.enabled = ini_float(L"Enabled", 0, path, L"DepthBoost") != 0;
    // Mode: realistic (default) or enhanced; each has its own curve and MaxScale range (pupil_math.hpp, DepthMode).
    wchar_t mode[32]{};
    GetPrivateProfileStringW(L"DepthBoost", L"Mode", L"realistic", mode, 32, path.c_str());
    d.mode = _wcsicmp(mode, L"enhanced") == 0 ? DepthMode::enhanced : DepthMode::realistic;
    const DepthModeLimits limits = depth_mode_limits(d.mode);
    d.max_scale = std::clamp(ini_float(L"MaxScale", limits.max_default, path, L"DepthBoost"), limits.max_min, limits.max_max);
    d.far_start_m = std::clamp(ini_float(L"FarStartM", limits.far_start_m, path, L"DepthBoost"), 1.0f, 5000.0f);
    d.far_full_m = std::clamp(ini_float(L"FarFullM", limits.far_full_m, path, L"DepthBoost"), d.far_start_m * 1.1f, 20000.0f);
    pvr_wanted_setting = ini_float(L"PimaxPvrGaze", 1, path, L"DepthBoost") != 0;
    depth_trace = ini_float(L"Trace", 0, path, L"DepthBoost") != 0;
    d.near_m = std::clamp(ini_float(L"NearM", 1.5f, path, L"DepthBoost"), 0.5f, 5.0f);
    d.up_mm_per_s = std::clamp(ini_float(L"UpMmPerS", 5.0f, path, L"DepthBoost"), 0.5f, 20.0f);
    d.down_mm_per_s = std::clamp(ini_float(L"DownMmPerS", 40.0f, path, L"DepthBoost"), 5.0f, 100.0f);
    d.saccade_step_mm = std::clamp(ini_float(L"SaccadeStepMm", 4.0f, path, L"DepthBoost"), 0.0f, 10.0f);
    // Stepped (default): the scale changes in steps during saccades, at most DriftMmPerS while fixating
    // (DepthBoostController::step_stepped). Stepped=0 keeps the earlier ramps (UpMmPerS / DownMmPerS during fixations).
    d.stepped = ini_float(L"Stepped", 1, path, L"DepthBoost") != 0;
    d.fixation_drift_mm_per_s = std::clamp(ini_float(L"DriftMmPerS", 0.5f, path, L"DepthBoost"), 0.0f, 5.0f);
    d.step_down_mm = std::clamp(ini_float(L"StepDownMm", 8.0f, path, L"DepthBoost"), 0.0f, 40.0f);
    d.full_near_return = ini_float(L"FullNearReturn", 1, path, L"DepthBoost") != 0;
    // With the engine's dual camera acknowledged: the one world scale is approached at this rate, both ways.
    dual_camera_mm_per_s = std::clamp(ini_float(L"DualCameraMmPerS", 20.0f, path, L"DepthBoost"), 2.0f, 60.0f);
    depth_legacy = ini_float(L"Legacy", 0, path, L"DepthBoost") != 0;
    d.near_return_mm_per_s = std::clamp(ini_float(L"NearReturnMmPerS", 80.0f, path, L"DepthBoost"), 5.0f, 400.0f);
    d.recover_ms = std::clamp(ini_float(L"RecoverMs", 800.0f, path, L"DepthBoost"), 200.0f, 3000.0f);
    d.hold_ms = std::clamp(ini_float(L"HoldMs", 500.0f, path, L"DepthBoost"), 0.0f, 2000.0f);
    depth_settings = d;
    pvr_wanted.store(d.enabled && pvr_wanted_setting);
    // The transition (predictive_depth.hpp): gradual (default) or predictive; a changed value replaces the in-flight
    // key's choice.
    wchar_t transition[32]{};
    GetPrivateProfileStringW(L"DepthBoost", L"Transition", L"gradual", transition, 32, path.c_str());
    const auto t = _wcsicmp(transition, L"predictive") == 0 ? predictive::Transition::predictive : predictive::Transition::gradual;
    if (t != ini_transition) transition_override = -1;
    ini_transition = t;
    predictive::Settings ps;
    ps.min_amplitude_deg = std::clamp(ini_float(L"PredictMinDeg", 8.0f, path, L"DepthBoost"), 3.0f, 30.0f);
    ps.max_step_mm = std::clamp(ini_float(L"PredictMaxStepMm", 12.0f, path, L"DepthBoost"), 0.0f, 20.0f);
    ps.disparity_deg = std::clamp(ini_float(L"PredictDisparityDeg", 0.4f, path, L"DepthBoost"), 0.05f, 2.0f);
    ps.lead_frames = std::clamp(ini_float(L"PredictLeadFrames", 0.5f, path, L"DepthBoost"), 0.0f, 3.0f);
    ps.poll_hz = static_cast<unsigned>(std::clamp(ini_float(L"PredictPollHz", 500.0f, path, L"DepthBoost"), 100.0f, 1000.0f));
    predict_settings = ps;
    predict_poll_hz.store(ps.poll_hz);
    log("depth steps: stepped=%d step up %.1f mm, step down %.1f mm per saccade, %.2f mm/s while fixating; cockpit return %s\n",
        d.stepped ? 1 : 0, d.saccade_step_mm, d.step_down_mm, d.fixation_drift_mm_per_s,
        d.full_near_return ? (std::string("in one go (late: ") + std::to_string(static_cast<int>(d.near_return_mm_per_s)) + " mm/s)").c_str() : "in steps");
    log("depth settings: enabled=%d mode=%s max_scale=%.2f far=%.0f..%.0fm near=%.2fm up=%.1fmm/s down=%.1fmm/s saccade_step=%.1fmm recover=%.0fms hold=%.0fms pvr=%d key=%d:%d "
        "transition=%s transition_key=%d:%d predict min=%.1fdeg max_step=%.1fmm disparity=%.2fdeg lead=%.2fframes poll=%uHz\n",
        d.enabled, depth_mode_name(d.mode), d.max_scale, d.far_start_m, d.far_full_m, d.near_m, d.up_mm_per_s, d.down_mm_per_s,
        d.saccade_step_mm, d.recover_ms, d.hold_ms, pvr_wanted_setting ? 1 : 0, depth_key.vk, depth_key.mods,
        predictive::transition_name(transition_override >= 0 ? static_cast<predictive::Transition>(transition_override) : t), transition_key.vk, transition_key.mods, ps.min_amplitude_deg, ps.max_step_mm, ps.disparity_deg,
        ps.lead_frames, ps.poll_hz);
}

// The transition selected: the in-flight key's choice, else the ini's.
predictive::Transition selected_transition() {
    return transition_override >= 0 ? static_cast<predictive::Transition>(transition_override) : ini_transition;
}
// The high-rate sampler runs only while it can be used: Depth boost on (and not switched off), PVR wanted, predictive.
void update_predict_wanted_locked() {
    predict_wanted.store(depth_settings.enabled && !depth_key.off && pvr_wanted_setting && selected_transition() == predictive::Transition::predictive);
}

// The in-flight switches turn the correction (default Ctrl+Shift+F10) or Depth boost (default Alt+Shift+F10) off and
// on, for A/B comparison in the headset. The modifiers must match exactly, so Ctrl+Alt+Shift+F10 is a different key.
void beep(unsigned frequency, unsigned count = 1) {
    // Beep() blocks for its duration: off the render thread, so the switch itself causes no hitch. The thread holds
    // a reference to this DLL and drops it as it ends, so the loader unloading the layer can't remove its code first.
    HMODULE self{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&beep), &self)) return;
    if (HANDLE thread = CreateThread(nullptr, 0, [](LPVOID parameter) -> DWORD {
            // Frequency in the low 16 bits, the number of beeps above.
            const auto packed = reinterpret_cast<std::uintptr_t>(parameter);
            for (std::uintptr_t i = 0, n = std::max<std::uintptr_t>(1, packed >> 16); i < n; ++i) {
                if (i) Sleep(70);
                Beep(static_cast<DWORD>(packed & 0xFFFF), 80);
            }
            HMODULE module{};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&beep), &module);
            FreeLibraryAndExitThread(module, 0);
        }, reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>((frequency & 0xFFFF) | (std::min(count, 8u) << 16))), 0, nullptr))
        CloseHandle(thread);
    else
        FreeLibrary(self);
}

void poll_hotkey_locked(Hotkey& key, int mods) {
    if (key.vk == 0) { key.down = false; return; }
    if (hotkey_pressed(key, (GetAsyncKeyState(key.vk) & 0x8000) != 0, mods)) {
        key.off = !key.off;
        log("hotkey: %s %s\n", key.name, key.off ? "OFF" : "ON");
        beep(key.off ? key.off_tone : key.on_tone);
    }
}

void poll_hotkeys_locked() {
    const auto held = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    const int mods = (held(VK_CONTROL) ? 1 : 0) | (held(VK_MENU) ? 2 : 0) | (held(VK_SHIFT) ? 4 : 0);
    // A feature's keys only while it is on: a profile without it neither switches nor beeps on them (its keys are not
    // checked against DCS's bindings then).
    if (settings.enabled) poll_hotkey_locked(pupil_key, mods); else pupil_key.down = false;
    if (depth_settings.enabled) poll_hotkey_locked(depth_key, mods); else depth_key.down = false;
    // The transition key: gradual <-> predictive, one beep gradual, two beeps predictive.
    if (transition_key.vk == 0 || !depth_settings.enabled) { transition_key.down = false; update_predict_wanted_locked(); return; }
    if (hotkey_pressed(transition_key, held(transition_key.vk), mods)) {
        const auto next = selected_transition() == predictive::Transition::predictive ? predictive::Transition::gradual : predictive::Transition::predictive;
        transition_override = static_cast<int>(next);
        log("hotkey: transition %s\n", predictive::transition_name(next));
        beep(transition_key.on_tone, next == predictive::Transition::predictive ? 2 : 1);
    }
    update_predict_wanted_locked();
}

bool pupil_active() { return settings.enabled && !pupil_key.off; }

// Finds the bundled Quad Views' gaze export (absent: Pimax's native quad views, or a Quad Views without it). The late
// check at the first xrLocateViews logs only a find.
void resolve_quad_views_locked(bool late = false) {
    const HMODULE qv = GetModuleHandleW(qv_gaze::module_name);
    const auto fn = qv ? reinterpret_cast<QuadViewsGazeFn>(GetProcAddress(qv, qv_gaze::export_name)) : nullptr;
    qv_gaze_fn.store(fn);
    if (late && !fn) return;
    log("gaze: %s\n", fn ? "Quad Views gaze export found (source qv; its fixed focus is no gaze)"
                         : qv ? "Quad Views without the gaze export: OpenXR eye gaze, else the focus views"
                              : "no bundled Quad Views: OpenXR eye gaze, else the focus views");
}
bool depth_active() { return depth_settings.enabled && !depth_key.off; }

Vec3 to_vec(const XrVector3f& v) { return {v.x, v.y, v.z}; }
Quat to_quat(const XrQuaternionf& q) { return {q.x, q.y, q.z, q.w}; }
Fov to_fov(const XrFovf& f) { return {f.angleLeft, f.angleRight, f.angleUp, f.angleDown}; }

// What was handed to the application for one display time: original and shifted views.
struct Frame {
    XrTime time{};
    std::uint32_t count{};
    std::array<XrView, 4> original{};
    std::array<XrView, 4> shifted{};
};
std::deque<Frame> frames;

// Diagnostics, logged once a second: gaze range, and where the runtime itself puts each eye
// relative to the head (if that moved with the gaze, the runtime would already be doing this).
struct Stats {
    unsigned frames{}, shifted{};
    unsigned source_frames[5]{};       // per GazeSource: the gaze each frame had (the pupil correction's choice)
    unsigned qv_invalid{}, qv_saccade{}, xr_invalid{};
    // Quad Views' valid gazes this second, those plausible_gaze refused (looking more than 30 degrees up or 50 off the
    // eye's axis: a placeholder or a gaze in the wrong frame), and the last refused one's angles (left eye, degrees).
    unsigned qv_valid{}, qv_implausible{};
    float qv_refused_x{}, qv_refused_y{};
    GazeSource source{GazeSource::none};
    float gaze_min_x{1e9f}, gaze_max_x{-1e9f}, gaze_min_y{1e9f}, gaze_max_y{-1e9f};
    float eye_min_x{1e9f}, eye_max_x{-1e9f}, eye_min_z{1e9f}, eye_max_z{-1e9f};
    float shift_max_mm{};
    float cant_deg{}, runtime_ipd_mm{}, rendered_ipd_mm{};
    ULONGLONG start{};
} stats;

// Depth boost's once-a-second line.
struct DepthStats {
    unsigned frames{}, near_frames{}, far_frames{}, unknown{}, no_gaze{}, boosted{};
    float scale_min{1e9f}, scale_max{}, distance_min{1e9f}, distance_max{};
    unsigned saccades_start{}, rejected_start{};
    float base_mm{}, render_mm{};
    // The fixation depth (FixationDecider): frames per source, the scene depth summaries (fresh, stale, absent) and what
    // they said, edges, ambiguous cones, vetoes, state changes, landing steps.
    unsigned src_depth{}, src_vergence{}, src_none{};
    unsigned scene_fresh{}, scene_stale{}, scene_absent{}, scene_moved{};
    unsigned edge_frames{}, ambiguous{}, veto_frames{}, depth_over_vergence{}, landing_steps{};
    unsigned changes_start{}, vetoes_start{}, over_start{};
    double near_share{}, far_share{}, ring_near{}, ring_far{}, far_median{};
    unsigned far_median_n{};
    // The depth gaze's source: Pimax PVR per-eye gaze frames, frames it was queried but not valid.
    unsigned pvr_valid{}, pvr_invalid{}, pvr_eye_bad{};
    double pvr_convergence{};
    unsigned pvr_convergence_n{};
} depth_stats;

// Metres from an inverse distance, 999 for "infinity", -1 without an estimate (log only).
float metres(float w) { return !std::isfinite(w) ? -1.0f : w < 0.001f ? 999.0f : 1.0f / w; }
const char* source_name(DepthSource s) { return s == DepthSource::vergence ? "vergence" : s == DepthSource::constant ? "constant" : "probing"; }
const char* fixation_name(Fixation f) { return f == Fixation::near_ ? "near" : f == Fixation::far_ ? "far" : "unknown"; }

// The fixation distance shown and logged: the scene depth's median at the gaze while it is recent, else the vergence's.
float fixation_distance_m() {
    const float d = decider.depth_distance_m();
    if (std::isfinite(d) && d > 0) return decider.state() == Fixation::far_ && std::isfinite(decider.far_distance_m()) ? decider.far_distance_m() : d;
    return metres(fixation.inverse_distance());
}

void account_depth_locked(float base_mm, float render_mm, DepthMatcher::State scene, const pvr::Sample& pvr_sample, bool landing_step) {
    auto& d = depth_stats;
    if (!depth_settings.enabled) return;
    if (d.frames == 0) {
        d.saccades_start = fixation.saccades(); d.rejected_start = fixation.rejected();
        d.changes_start = decider.changes(); d.vetoes_start = decider.vetoes(); d.over_start = decider.depth_over_vergence();
    }
    ++d.frames;
    const float s = depth.scale();
    d.scale_min = std::min(d.scale_min, s); d.scale_max = std::max(d.scale_max, s);
    if (s > 1.0005f) ++d.boosted;
    if (!fixation.valid()) ++d.no_gaze;
    else if (decider.state() == Fixation::near_) ++d.near_frames;
    else if (decider.state() == Fixation::far_) ++d.far_frames;
    else ++d.unknown;
    const float m = fixation_distance_m();
    if (m >= 0) { d.distance_min = std::min(d.distance_min, m); d.distance_max = std::max(d.distance_max, m); }
    d.base_mm = base_mm; d.render_mm = render_mm;
    switch (decider.source()) {
    case FixSource::depth: ++d.src_depth; break;
    case FixSource::vergence: ++d.src_vergence; break;
    default: ++d.src_none; break;
    }
    if (scene == DepthMatcher::State::fresh) {
        ++d.scene_fresh;
        const DepthSummary& sum = matcher.summary();
        d.near_share += sum.near_share; d.far_share += sum.far_share; d.ring_near += sum.edge_near; d.ring_far += sum.edge_far;
        if (std::isfinite(sum.far_median_m) && sum.far_median_m > 0) { d.far_median += sum.far_median_m; ++d.far_median_n; }
    } else if (scene == DepthMatcher::State::stale) {
        ++d.scene_stale;
        if (matcher.reason() == DepthMatcher::Reason::moved) ++d.scene_moved;
    } else {
        ++d.scene_absent;
    }
    if (decider.edge()) ++d.edge_frames;
    if (decider.frame_class() == DepthClass::ambiguous) ++d.ambiguous;
    if (decider.veto()) ++d.veto_frames;
    if (landing_step) ++d.landing_steps;
    if (pvr_sample.queried) {
        if (pvr_sample.valid) ++d.pvr_valid; else ++d.pvr_invalid;
        if (!pvr_sample.eye_ok[0] || !pvr_sample.eye_ok[1]) ++d.pvr_eye_bad;
        if (pvr_sample.valid && pvr_sample.convergence_m > 0) { d.pvr_convergence += pvr_sample.convergence_m; ++d.pvr_convergence_n; }
    }
}

// "depth:" line. source: the fixation depth's source most frames had (depth: the scene depth from the engine module,
// vergence: per-eye vergence, none); vergence: whether per-eye vergence is proven (probing, vergence, constant).
// The "scene" segment: fresh, stale (moved: the gaze moved since) and absent summaries, the mean shares of fresh ones
// (cone near/far, ring near/far), their mean far median, edge and ambiguous frames, vetoes (vergence near over a far
// depth), depth over vergence (depth near trusted over a far vergence), state changes and landing steps.
void log_depth_locked() {
    auto& d = depth_stats;
    if (!depth_settings.enabled || d.frames == 0) { d = DepthStats{}; return; }
    if (d.distance_min > d.distance_max) d.distance_min = d.distance_max = -1;
    const char* src = d.src_depth >= d.src_vergence && d.src_depth >= d.src_none ? "depth" : d.src_vergence >= d.src_none ? "vergence" : "none";
    const double fresh = d.scene_fresh ? static_cast<double>(d.scene_fresh) : 1.0;
    char pvr_text[160] = "pvr off";
    if (d.pvr_valid + d.pvr_invalid)
        std::snprintf(pvr_text, sizeof pvr_text, "pvr valid=%u invalid=%u eye_invalid=%u convergence %.2f m", d.pvr_valid, d.pvr_invalid, d.pvr_eye_bad,
            d.pvr_convergence_n ? d.pvr_convergence / d.pvr_convergence_n : 0.0);
    else if (pvr_wanted.load())
        std::snprintf(pvr_text, sizeof pvr_text, "pvr none");
    log("depth: %s source=%s vergence=%s fixation=%s distance %.2f m [%.2f..%.2f] scale %.3f [%.3f..%.3f] target %.3f; "
        "frames=%u near=%u far=%u unknown=%u no_gaze=%u boosted=%u saccades=%u rejected=%u; "
        "scene mode=%s fresh=%u stale=%u moved=%u absent=%u shares near %.2f far %.2f ring %.2f/%.2f far_median %.1f m edge=%u ambiguous=%u "
        "vetoes=%u depth_over_vergence=%u changes=%u landing_steps=%u engine_flags=0x%02x; %s; IPD base %.1f mm, render %.1f mm\n",
        depth_active() ? "on" : "off (switch)", src, source_name(fixation.source()), fixation_name(decider.state()),
        fixation_distance_m(), d.distance_min, d.distance_max, depth.scale(), d.scale_min, d.scale_max, depth.target(),
        d.frames, d.near_frames, d.far_frames, d.unknown, d.no_gaze, d.boosted, fixation.saccades() - d.saccades_start,
        fixation.rejected() - d.rejected_start, depth_mode_name(depth_settings.mode), d.scene_fresh, d.scene_stale, d.scene_moved, d.scene_absent,
        d.near_share / fresh, d.far_share / fresh, d.ring_near / fresh, d.ring_far / fresh, d.far_median_n ? d.far_median / d.far_median_n : 0.0,
        d.edge_frames, d.ambiguous, decider.vetoes() - d.vetoes_start, decider.depth_over_vergence() - d.over_start,
        decider.changes() - d.changes_start, d.landing_steps, last_depth_flags & 0xffu, pvr_text, d.base_mm, d.render_mm);
    d = DepthStats{};
    // The transition's line, while predictive is selected: in effect or gradual (and why); the high-rate samples and
    // their rate, saccades seen, predicted, steps applied and their mean size, the landings that confirmed, contradicted
    // or let a step expire, the predictions without a step (short: predicted under PredictMinDeg; no_grid: no fresh
    // depth grid; edge, ambiguous, outside: the landing's cells; late: less than PredictLeadFrames of saccade left; same:
    // the scale already there), the detector's gain, and frames with a fresh / stale grid.
    auto& p = predict_second;
    if (selected_transition() == predictive::Transition::predictive && p.started && p.frames) {
        const auto& g = gate.stats();
        const auto& b = p.gate;
        const unsigned steps = g.steps - b.steps;
        log("predict: transition=predictive %s%s%s samples=%u rate %.0f Hz saccades=%u dropped=%u predicted=%u steps=%u mean_step %.2f mm "
            "confirmed=%u contradicted=%u expired=%u skipped short=%u no_grid=%u edge=%u ambiguous=%u outside=%u late=%u same=%u; gain %.2f; grid fresh=%u stale=%u\n",
            predict_effective ? "in effect" : "fallback gradual", predict_effective ? "" : ": ", predict_effective ? "" : (predict_reason ? predict_reason : "unknown"),
            p.samples, p.frames ? p.samples * (1.0 / (p.frames * std::max(1e-3f, frame_period_s))) : 0.0, detector.saccades() - p.saccades,
            detector.dropped_count() - p.dropped, g.predicted - b.predicted, steps, steps ? (g.step_mm_sum - b.step_mm_sum) / steps : 0.0,
            g.confirmed - b.confirmed, g.contradicted - b.contradicted, g.expired - b.expired, g.short_ - b.short_, g.no_grid - b.no_grid,
            g.edge - b.edge, g.ambiguous - b.ambiguous, g.outside - b.outside, g.late - b.late, g.same - b.same, detector.gain(), p.grid_fresh, p.grid_stale);
    }
    p = PredictSecond{};
}

// The Depth boost fields of the status word change every frame; the pupil shift's once a second.
std::uint64_t depth_bits_locked() {
    const float m = fixation_distance_m();
    // The panel's source: the scene depth while it was fresh within the last 90 frames, even between two summaries (the
    // panel showed "waiting" while the scene depth worked, 2026-10-13).
    const bool scene_recent = last_fresh_scene_frame && display_frames - last_fresh_scene_frame < 90;
    const FixSource shown = decider.source() == FixSource::none && scene_recent ? FixSource::depth : decider.source();
    return depth_status_bits(settings.enabled, depth_settings.enabled, depth_active(), fixation.source(), decider.state(),
        depth.scale(), m > 0 ? 1.0f / m : std::nanf(""), shown, decider.edge(), decider.veto(), predict_effective,
        depth_settings.enabled && selected_transition() == predictive::Transition::predictive && !predict_effective, scene_recent);
}

void account_locked(const XrView* views, std::uint32_t count, const Vec3* gaze, float shift_mm, float cant, float rendered_ipd_mm,
    GazeSource source, std::uint32_t qv_flags, const XrEyeGaze& xr) {
    if (!stats.start) stats.start = GetTickCount64();
    ++stats.frames;
    ++stats.source_frames[std::min<unsigned>(static_cast<unsigned>(source), 4)];
    stats.source = source;
    if ((qv_flags & qv_gaze::sample) && !(qv_flags & qv_gaze::valid)) ++stats.qv_invalid;
    else if (!(qv_flags & qv_gaze::sample) && xr.present && !xr.valid) ++stats.xr_invalid;
    if ((qv_flags & qv_gaze::valid) && (qv_flags & qv_gaze::saccade)) ++stats.qv_saccade;
    if (qv_flags & qv_gaze::valid) {
        ++stats.qv_valid;
        if (source == GazeSource::none) ++stats.qv_implausible;
    }
    if (gaze) {
        ++stats.shifted;
        const float gx = std::atan2(gaze->x, -gaze->z) * 57.2958f, gy = std::atan2(gaze->y, -gaze->z) * 57.2958f;
        stats.gaze_min_x = std::min(stats.gaze_min_x, gx); stats.gaze_max_x = std::max(stats.gaze_max_x, gx);
        stats.gaze_min_y = std::min(stats.gaze_min_y, gy); stats.gaze_max_y = std::max(stats.gaze_max_y, gy);
        stats.shift_max_mm = std::max(stats.shift_max_mm, shift_mm);
    }
    stats.cant_deg = cant;
    stats.rendered_ipd_mm = rendered_ipd_mm;
    if (count >= 2) {
        const Vec3 l = to_vec(views[0].pose.position), r = to_vec(views[1].pose.position);
        stats.runtime_ipd_mm = length(r - l) * 1000;
        const Vec3 local = rotate(conjugate(to_quat(views[0].pose.orientation)), l - (l + r) * 0.5f);
        stats.eye_min_x = std::min(stats.eye_min_x, local.x); stats.eye_max_x = std::max(stats.eye_max_x, local.x);
        stats.eye_min_z = std::min(stats.eye_min_z, local.z); stats.eye_max_z = std::max(stats.eye_max_z, local.z);
    }
    if (GetTickCount64() - stats.start >= 1000) {
        const unsigned restored = restored_views.exchange(0);
        // The gaze source: the last frame's, and how many frames of the second had each (qv: Quad Views' export; xr: the
        // OpenXR eye gaze; focus: the focus views' centres; none: no gaze, the eyes at rest). qv invalid: Quad Views said
        // its focus was the fixed one (no gaze); xr invalid: the runtime's eye gaze was not valid and tracked (no gaze);
        // saccade: frames Quad Views flagged as a saccade.
        char sources[320];
        std::snprintf(sources, sizeof sources,
            "gaze source=%s (qv %u, xr %u, focus %u, simulated %u, none %u; qv invalid %u, xr invalid %u, saccade %u; qv valid %u, "
            "refused as implausible %u, last refused x %.1f y %.1f deg)",
            gaze_source_name(stats.source), stats.source_frames[static_cast<unsigned>(GazeSource::qv)],
            stats.source_frames[static_cast<unsigned>(GazeSource::xr)],
            stats.source_frames[static_cast<unsigned>(GazeSource::focus)], stats.source_frames[static_cast<unsigned>(GazeSource::simulated)],
            stats.source_frames[static_cast<unsigned>(GazeSource::none)], stats.qv_invalid, stats.xr_invalid, stats.qv_saccade, stats.qv_valid,
            stats.qv_implausible, stats.qv_refused_x, stats.qv_refused_y);
        if (stats.shifted)
            log("stats: frames=%u shifted=%u restored=%u gaze x[%.1f..%.1f] y[%.1f..%.1f] deg, max shift %.2f mm; "
                "runtime left eye vs head x[%.2f..%.2f] z[%.2f..%.2f] mm; cant %.1f deg, runtime IPD %.1f mm, rendered IPD %.1f mm; %s\n",
                stats.frames, stats.shifted, restored, stats.gaze_min_x, stats.gaze_max_x, stats.gaze_min_y, stats.gaze_max_y,
                stats.shift_max_mm, stats.eye_min_x * 1000, stats.eye_max_x * 1000, stats.eye_min_z * 1000, stats.eye_max_z * 1000,
                stats.cant_deg, stats.runtime_ipd_mm, stats.rendered_ipd_mm, sources);
        if (stats.shifted) {
            // The pupil's gaze filter this second: saccades followed, single-frame spikes ignored (tracker noise).
            const auto f = pupil_filter.take_stats();
            const auto k = pupil_lock.take_stats();
            log("pupil filter: tracker noise %.2f deg%s, %u samples, %u held (blinks), %u saccades moved the pupil, smooth %.0f ms at rest\n",
                f.noise_deg, k.noisy ? " (too noisy: pupil back to rest)" : "", f.samples, f.held, k.saccades, settings.smooth_s * 1000);
        }
        if (!stats.shifted)
            log("stats: frames=%u shifted=0 (%s); runtime left eye vs head x[%.2f..%.2f] mm; cant %.1f deg, runtime IPD %.1f mm, rendered IPD %.1f mm; %s\n",
                stats.frames, !settings.enabled ? "pupil shift disabled" : pupil_key.off ? "correction off" : stats.qv_invalid || stats.xr_invalid ? "no valid gaze" : "no gaze source",
                stats.eye_min_x * 1000, stats.eye_max_x * 1000, stats.cant_deg, stats.runtime_ipd_mm, stats.rendered_ipd_mm, sources);
        log_depth_locked();
        const std::uint64_t tenths = static_cast<std::uint64_t>(std::clamp(stats.shift_max_mm * 10.0f + 0.5f, 0.0f, 255.0f));
        status_word.store(status::running | (pupil_active() ? status::pupil_on : 0) | (stats.shifted ? status::gaze : 0) |
            (settings.simulate ? status::simulated : 0) | (restored ? status::restored : 0) | (tenths << status::shift_mm10) |
            depth_bits_locked(), std::memory_order_release);
        stats = Stats{};
    } else {
        // Between the once-a-second updates, the switches and the depth fields show at once.
        auto word = (status_word.load(std::memory_order_relaxed) & ~depth_status_mask) | status::running;
        word = pupil_active() ? (word | status::pupil_on) : (word & ~status::pupil_on);
        status_word.store(word | depth_bits_locked(), std::memory_order_release);
    }
}

// ---- Pimax PVR per-eye gaze ----------------------------------------------------------------------------------------
// Pimax's OpenXR runtime runs on its PVR client (LibPVRClient64.dll, loaded in DCS's process). PVR's eye tracking gives
// each eye's own gaze (GazeTan[2]), which the OpenXR eye gaze and Quad Views combine into one: Depth boost's vergence
// needs it. The query needs the runtime's own HMD handle, which nothing exports. As Cheeky does (libovr_gaze.cpp, route
// 1), the layer patches four slots of PVR's shared interface table (end frame, submit frame, tracking state: the handle
// is noted as the runtime passes it; destroy: it is forgotten under an exclusive lock before PVR frees it), never any
// code. Cheeky's second route (inline hooks on the implementations, for a runtime that copied the table) is not used:
// it patches the runtime's code, which this layer does not do. Without the handle the layer keeps the other gaze
// sources and logs why. The layer never loads PVR (only a client the runtime loaded), never creates a PVR session, and
// queries only getEyeTrackingInfo and getTimeSeconds. Once patched, the slots stay patched for the life of the process
// and the layer's DLL is pinned, so a detour can never point at unloaded code. Putting a slot back at xrDestroyInstance
// and patching it again for a later instance could read another patcher's detour (chained over this layer's) as the
// original and call itself forever. xrDestroyInstance only forgets the session and stops the sampler; the detours then
// just pass through.
namespace pvr_hook {
struct Patch { void** slot{}; void* original{}; void* replacement{}; };
std::mutex mutex;                          // attach and restore
std::vector<Patch> patches;
std::uint32_t minor{};
bool attached{}, failed{};
ULONGLONG next_try{};
std::atomic<bool> ready{};
pvr::GetTimeSecondsFn time_fn{};
pvr::GetEyeTrackingInfoFn eye_fn{};
pvr::FrameFn end_frame_fn{}, submit_frame_fn{};
pvr::GetTrackingStateFn tracking_fn{};
pvr::DestroyHmdFn destroy_fn{};
SRWLOCK hmd_lock = SRWLOCK_INIT;           // sampling holds it shared; destroyHmd exclusive
std::atomic<pvr::HmdHandle> frame_hmd{}, tracking_hmd{};
std::atomic<unsigned> destroy_generation{};  // destroyHmd calls so far (incremented under hmd_lock exclusive)
std::atomic<bool> handle_logged{};
std::atomic<unsigned> frames_without_handle{};
bool no_handle_logged{};
std::mutex sample_mutex;                   // the filter
pvr::GazeFilter filter;
std::mutex poll_mutex;                     // the high-rate sampler's start and stop (below)
void poll_stop_locked();

// generation: destroy_generation when the frame call began. Under the shared lock, and only when no destroyHmd ran since,
// so a frame call racing destroyHmd on another thread never publishes the freed handle, while a later session that
// gets the same address back is still noted.
void note(std::atomic<pvr::HmdHandle>& slot, pvr::HmdHandle hmd, unsigned generation) {
    if (!hmd || slot.load(std::memory_order_relaxed) == hmd) return;
    AcquireSRWLockShared(&hmd_lock);
    const bool noted = destroy_generation.load() == generation;
    if (noted) slot.store(hmd);
    ReleaseSRWLockShared(&hmd_lock);
    if (!noted) return;
    if (!handle_logged.exchange(true)) log("pvr: the runtime's PVR session found (through the interface table)\n");
}
pvr::Result end_frame_hook(pvr::HmdHandle hmd, long long index, const void* const* layers, unsigned count) {
    note(frame_hmd, hmd, destroy_generation.load());
    return end_frame_fn(hmd, index, layers, count);
}
pvr::Result submit_frame_hook(pvr::HmdHandle hmd, long long index, const void* const* layers, unsigned count) {
    note(frame_hmd, hmd, destroy_generation.load());
    return submit_frame_fn(hmd, index, layers, count);
}
void destroy_hmd_hook(pvr::HmdHandle hmd) {
    AcquireSRWLockExclusive(&hmd_lock);
    for (auto* slot : {&frame_hmd, &tracking_hmd}) { pvr::HmdHandle expected = hmd; slot->compare_exchange_strong(expected, nullptr); }
    destroy_generation.fetch_add(1);
    ReleaseSRWLockExclusive(&hmd_lock);
    destroy_fn(hmd);
}

bool patch(void** entry, void* detour, std::vector<Patch>& out) {
    DWORD protection{};
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &protection)) return false;
    Patch p{entry, *entry, detour};
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(entry), detour);
    DWORD unused{};
    VirtualProtect(entry, sizeof(void*), protection, &unused);
    out.push_back(p);
    return true;
}

// Called from xrLocateViews (outside the main lock) while Depth boost wants PVR; tries at once, then every 2 s while the
// runtime has not loaded the client.
void try_attach() {
    std::lock_guard lock(mutex);
    if (failed) return;
    if (attached) {
        // Patched for an earlier instance: the detours are still in place, only the session has to be seen again.
        if (!ready.load()) { frames_without_handle = 0; ready.store(true); }
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now < next_try) return;
    next_try = now + 2000;
    HMODULE module{};
    // Only a client the runtime already loaded (the reference keeps it loaded while the slots point into it).
    if (!GetModuleHandleExW(0, pvr::client_module, &module)) return;
    const auto fail = [&](const char* reason) {
        FreeLibrary(module);
        failed = true;
        log("pvr: Pimax PVR per-eye gaze unavailable: %s\n", reason);
    };
    const auto get = reinterpret_cast<pvr::GetInterfaceFn>(GetProcAddress(module, pvr::interface_export));
    if (!get) return fail("getPvrInterface export missing");
    pvr::Interface* table{};
    for (auto m = pvr::newest_minor_version; m >= pvr::oldest_minor_version && !table; --m)
        if ((table = get(pvr::major_version, m))) minor = m;
    if (!table) return fail("no supported interface version (1.23-1.32)");
    for (const auto index : {pvr::slot_destroy_hmd, pvr::slot_get_time_seconds, pvr::slot_get_tracking_state, pvr::slot_end_frame,
             pvr::slot_submit_frame, pvr::slot_get_eye_tracking_info})
        if (!table->slots[index]) return fail("incomplete interface");
    time_fn = reinterpret_cast<pvr::GetTimeSecondsFn>(table->slots[pvr::slot_get_time_seconds]);
    eye_fn = reinterpret_cast<pvr::GetEyeTrackingInfoFn>(table->slots[pvr::slot_get_eye_tracking_info]);
    end_frame_fn = reinterpret_cast<pvr::FrameFn>(table->slots[pvr::slot_end_frame]);
    submit_frame_fn = reinterpret_cast<pvr::FrameFn>(table->slots[pvr::slot_submit_frame]);
    tracking_fn = reinterpret_cast<pvr::GetTrackingStateFn>(table->slots[pvr::slot_get_tracking_state]);
    destroy_fn = reinterpret_cast<pvr::DestroyHmdFn>(table->slots[pvr::slot_destroy_hmd]);
    // The runtime may have asked for another version: patch every table that forwards to the same implementations.
    std::vector<pvr::Interface*> tables{table};
    for (auto m = pvr::oldest_minor_version; m < 64; ++m) {
        auto* candidate = get(pvr::major_version, m);
        if (!candidate || std::find(tables.begin(), tables.end(), candidate) != tables.end()) continue;
        bool same = true;
        for (const auto index : {pvr::slot_destroy_hmd, pvr::slot_get_tracking_state, pvr::slot_end_frame, pvr::slot_submit_frame})
            same = same && candidate->slots[index] == table->slots[index];
        if (same) tables.push_back(candidate);
    }
    // The detours must outlive any unload of this layer.
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&end_frame_hook), &self);
    // Destroy first, and the frame calls only where it took: a handle noted without seeing its destruction would be
    // queried after PVR freed it.
    std::size_t destroy_hooked = 0;
    for (auto* t : tables) {
        if (!patch(&t->slots[pvr::slot_destroy_hmd], reinterpret_cast<void*>(&destroy_hmd_hook), patches)) continue;
        ++destroy_hooked;
        patch(&t->slots[pvr::slot_end_frame], reinterpret_cast<void*>(&end_frame_hook), patches);
        patch(&t->slots[pvr::slot_submit_frame], reinterpret_cast<void*>(&submit_frame_hook), patches);
        // get_tracking_state is deliberately left alone: observed live (2026-10-12), passing it through moved DCS's head
        // far from the seat, most likely a signature mismatch on the pose call. The frame calls alone give the session.
    }
    if (!destroy_hooked) {
        failed = true;
        log("pvr: Pimax PVR per-eye gaze unavailable: the interface table could not be patched\n");
        return;
    }
    attached = true;
    frames_without_handle = 0;
    ready.store(true);
    log("pvr: Pimax PVR client found (interface 1.%u, %zu table(s), %zu slot(s) observed); waiting for the runtime's session\n",
        minor, tables.size(), patches.size());
}

// Stops the sampler and forgets the session (the slots stay patched, see above); a later instance sees it again. No
// lock is taken on the handles: a sampler stuck inside PVR may still hold hmd_lock shared, and clearing the atomics
// frees nothing (destroyHmd's detour keeps guarding the real destruction).
void restore() {
    {
        std::lock_guard poll_lock(poll_mutex);
        poll_stop_locked();
    }
    std::lock_guard lock(mutex);
    ready.store(false);
    frame_hmd.store(nullptr); tracking_hmd.store(nullptr);
    if (attached) log("pvr: session forgotten (%zu interface slot(s) stay patched, passing through)\n", patches.size());
    failed = false;
    next_try = 0;
    handle_logged = false;
    no_handle_logged = false;
    std::lock_guard sample_lock(sample_mutex);
    filter.reset();
}

// ---- The high-rate sampler (Depth boost's predictive transition) ------------------------------------------------------
// A small thread queries getEyeTrackingInfo at PredictPollHz (500 by default) while the predictive transition is
// wanted and the runtime's session is known, under the same shared lock as the per-frame query (destroyHmd takes it
// exclusively and forgets the handle first, so PVR is never called with a destroyed handle). Each new sample time goes
// into a lock-free single-producer ring that xrLocateViews drains; while it runs it is the only caller of PVR (the
// per-frame query reads its latest answer instead). Stopped when no longer wanted and at xrDestroyInstance (joined).
struct RawSample { double time{}; float tan[2][2]{}; bool valid{}; };
constexpr std::uint64_t kRing = 1024;
RawSample ring[kRing];
std::atomic<std::uint64_t> ring_head{};
std::uint64_t ring_tail{};                 // the reader's, under sample_mutex
SRWLOCK latest_lock = SRWLOCK_INIT;
pvr::EyeTrackingInfo latest{};
bool latest_queried{};
HANDLE poll_thread{};
std::atomic<bool> poll_run{}, polling{};
std::atomic<unsigned> poll_hz{500};
std::atomic<std::uint64_t> poll_calls{};

DWORD WINAPI poll_main(LPVOID) {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    double last_time = -1;
    bool last_valid = false;
    while (poll_run.load(std::memory_order_acquire)) {
        pvr::EyeTrackingInfo info{};
        bool queried = false;
        if (ready.load()) {
            AcquireSRWLockShared(&hmd_lock);
            pvr::HmdHandle hmd = frame_hmd.load();
            if (!hmd) hmd = tracking_hmd.load();
            if (hmd && eye_fn && time_fn) queried = eye_fn(hmd, time_fn(), &info) == pvr::success;
            ReleaseSRWLockShared(&hmd_lock);
        }
        AcquireSRWLockExclusive(&latest_lock);
        latest = info; latest_queried = queried;
        ReleaseSRWLockExclusive(&latest_lock);
        if (queried) {
            const bool valid = std::isfinite(info.TimeInSeconds) && info.TimeInSeconds > 0 && pvr::eye_usable(info, 0) && pvr::eye_usable(info, 1);
            if ((valid && info.TimeInSeconds != last_time) || (!valid && last_valid)) {
                const std::uint64_t h = ring_head.load(std::memory_order_relaxed);
                RawSample& r = ring[h % kRing];
                r.valid = valid; r.time = valid ? info.TimeInSeconds : 0.0;
                for (int e = 0; e < 2; ++e) { r.tan[e][0] = info.GazeTan[e].x; r.tan[e][1] = info.GazeTan[e].y; }
                ring_head.store(h + 1, std::memory_order_release);
                if (valid) last_time = info.TimeInSeconds;
            }
            last_valid = valid;
        }
        poll_calls.fetch_add(1, std::memory_order_release);
        const unsigned hz = std::clamp(poll_hz.load(), 100u, 1000u);
        if (timer) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(10'000'000LL / hz);  // relative, 100 ns units
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 50);
            else Sleep(1);
        } else {
            Sleep(1);
        }
    }
    if (timer) CloseHandle(timer);
    return 0;
}

void poll_stop_locked() {
    if (!poll_thread) return;
    poll_run.store(false, std::memory_order_release);
    polling.store(false);
    if (WaitForSingleObject(poll_thread, 2000) != WAIT_OBJECT_0) {
        // Still inside PVR: the handle is kept and no new sampler starts until this one has ended (one ring writer).
        log("predict: gaze sampler did not stop within 2 s (inside PVR); it ends on its own, no new one meanwhile\n");
        return;
    }
    CloseHandle(poll_thread);
    poll_thread = nullptr;
    log("predict: gaze sampler stopped\n");
}

// Starts the sampler when wanted and the runtime's session is known; stops it when not wanted.
void poll_ensure(bool want, unsigned hz) {
    poll_hz.store(hz);
    if (want == polling.load()) return;
    std::lock_guard lock(poll_mutex);
    if (!want) { poll_stop_locked(); return; }
    if (poll_thread) {
        // A sampler that did not stop in time: wait until it has ended.
        if (poll_run.load() || WaitForSingleObject(poll_thread, 0) != WAIT_OBJECT_0) return;
        CloseHandle(poll_thread);
        poll_thread = nullptr;
    }
    if (!ready.load() || !(frame_hmd.load() || tracking_hmd.load())) return;
    {
        std::lock_guard sample_lock(sample_mutex);
        ring_tail = ring_head.load(std::memory_order_acquire);
    }
    poll_run.store(true, std::memory_order_release);
    poll_thread = CreateThread(nullptr, 0, poll_main, nullptr, 0, nullptr);
    if (!poll_thread) { poll_run.store(false); log("predict: the gaze sampler thread could not start (%lu)\n", GetLastError()); return; }
    polling.store(true);
    log("predict: gaze sampler started at %u Hz\n", hz);
}

// The samples since the last call, oldest first (a reader that fell more than the ring behind loses the oldest).
void drain(std::vector<RawSample>& out) {
    out.clear();
    std::lock_guard lock(sample_mutex);
    const std::uint64_t head = ring_head.load(std::memory_order_acquire);
    if (head - ring_tail > kRing) ring_tail = head - kRing;
    for (std::uint64_t i = ring_tail; i < head; ++i) out.push_back(ring[i % kRing]);
    // Entries the writer may have overwritten while they were copied are dropped, including the slot it may be writing
    // now (it writes entry `after` before publishing after + 1, and that slot aliases entry after - kRing).
    const std::uint64_t after = ring_head.load(std::memory_order_acquire);
    if (after + 1 - ring_tail > kRing) {
        const std::uint64_t lost = std::min<std::uint64_t>(out.size(), after + 1 - ring_tail - kRing);
        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(lost));
    }
    ring_tail = head;
}

// The tracker's clock now (seconds; 0 without the client).
double now_seconds() { return ready.load() && time_fn ? time_fn() : 0.0; }

// This frame's per-eye gaze (not queried without the client or the runtime's session). While the sampler runs, its
// latest answer (PVR is then called from one thread only).
pvr::Sample sample() {
    pvr::EyeTrackingInfo info{};
    bool queried = false;
    if (polling.load()) {
        AcquireSRWLockShared(&latest_lock);
        info = latest; queried = latest_queried;
        ReleaseSRWLockShared(&latest_lock);
    } else if (ready.load()) {
        AcquireSRWLockShared(&hmd_lock);
        pvr::HmdHandle hmd = frame_hmd.load();
        if (!hmd) hmd = tracking_hmd.load();
        if (hmd) queried = eye_fn(hmd, time_fn(), &info) == pvr::success;
        ReleaseSRWLockShared(&hmd_lock);
        if (!hmd && frames_without_handle.fetch_add(1) == 900) {
            std::lock_guard lock(mutex);
            if (!no_handle_logged) {
                no_handle_logged = true;
                log("pvr: no runtime session seen through the interface table after 900 frames: the runtime keeps its own copy of "
                    "the table. Per-eye gaze unavailable (the layer does not patch the runtime's code); vergence from the other sources\n");
            }
        }
    }
    std::lock_guard lock(sample_mutex);
    return filter.update(queried, info, GetTickCount64());
}
}  // namespace pvr_hook

// ---- Shared memory with the engine module (perception_shm.h) -------------------------------------------------------
// Opened lazily (Depth boost, SimulateGaze or PublishGaze on), retried about once a second when a mapping cannot be created.
bool open_blocks_locked() {
    if (gaze_block.get() && depth_block.get()) return true;
    if (shm_retry++ % 90 != 0) return false;
    const bool gaze = gaze_block.open(dcsvr_perception::gaze_block_name, shm_suffix);
    const bool depth_ok = depth_block.open(dcsvr_perception::depth_block_name, shm_suffix);
    log("perception: shared memory %ls%ls %s, %ls%ls %s\n", dcsvr_perception::gaze_block_name, shm_suffix, gaze ? "open" : "FAILED",
        dcsvr_perception::depth_block_name, shm_suffix, depth_ok ? "open" : "FAILED");
    return gaze && depth_ok;
}

DepthMatcher::Read read_depth_block_locked() {
    DepthMatcher::Read r;
    dcsvr_perception::DepthIn in{};
    if (!depth_block.get() || !dcsvr_perception::seqlock_read(depth_block.get(), in)) return r;
    r.ok = true;
    r.seq = in.seq;
    r.display_time = in.displayTime;
    r.valid = (in.flags & dcsvr_perception::depth_flags::valid) != 0;
    last_depth_flags = in.flags;
    r.summary = {in.nearShare, in.farShare, in.edgeNearShare, in.edgeFarShare, in.medianM, in.nearestM, in.farMedianM};
    return r;
}

// The engine's dual camera acknowledged (fresh and active): read once per frame, the mapping opened lazily (about once a
// second until it exists; the engine may start after the layer). Logged when it starts and stops.
bool cockpit_stereo_acked_locked() {
    if (!stereo_block.get() && stereo_retry++ % 90 == 0) stereo_block.open(dcsvr_perception::cockpit_stereo_block_name, shm_suffix);
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const auto seconds = [&](std::int64_t ticks) { return static_cast<double>(ticks) / static_cast<double>(frequency.QuadPart); };
    // Fresh: written less than 200 ms ago. Active and fresh enters (or keeps) the mode; an explicit stop (fresh, not
    // active, not observing: the key's pause or the option turned off) leaves it at once; anything else (no block, a
    // stale record, a dropped frame) leaves it only once nothing active came for 300 ms, so it never flickers.
    dcsvr_perception::CockpitStereo cs{};
    const bool read = stereo_block.get() && dcsvr_perception::seqlock_read(stereo_block.get(), cs);
    const double age = read ? seconds(now.QuadPart - cs.qpc) : 1e9;
    const bool fresh = age >= -0.05 && age < 0.2;
    const bool observing = (cs.flags & dcsvr_perception::cockpit_stereo_flags::observing) != 0;
    const bool active = fresh && (cs.flags & dcsvr_perception::cockpit_stereo_flags::active) && !observing;
    bool acked = stereo_acked;
    if (active) {
        acked = true;
        stereo_last_ok = now.QuadPart;
    } else if (fresh && !observing) {
        acked = false;
    } else if (stereo_acked && seconds(now.QuadPart - stereo_last_ok) > 0.3) {
        acked = false;
    } else if (observing) {
        acked = false;
    }
    if (acked != stereo_acked) {
        if (acked) log("depth: engine dual camera acknowledged: one world scale, changed at most %.0f mm/s\n", dual_camera_mm_per_s);
        else log("depth: engine dual camera gone (%s): the usual cockpit return\n", fresh && !observing ? "stopped" : observing ? "observing only" : "no fresh record for 300 ms");
        stereo_acked = acked;
    }
    return acked;
}

// BoostOut for a display time, once per new time, while Depth boost is enabled (perception_shm.h): the true and the
// rendered eye distance, for the engine's dual camera (the cockpit at the true distance).
void write_boost_out_locked(XrTime time, float base_ipd_m, float rendered_ipd_m) {
    // Once the block exists, a record for every display time, Depth boost enabled or not (then flags 0, scale 1): the
    // engine never keeps an earlier frame's distance. It is only created while Depth boost is enabled.
    if (time <= boost_out_time || (!depth_settings.enabled && !boost_block.get())) return;
    if (!boost_block.get()) {
        // Its own retry count (about once a second): open_blocks_locked's stops counting once its two blocks are open.
        if (boost_retry++ % 90 != 0) return;
        if (!boost_block.open(dcsvr_perception::boost_block_name, shm_suffix)) return;
        log("perception: shared memory %ls%ls created\n", dcsvr_perception::boost_block_name, shm_suffix);
    }
    boost_out_time = time;
    dcsvr_perception::BoostOut out{};
    out.displayTime = time;
    out.frame = ++boost_out_frames;
    out.baseIpdM = base_ipd_m;
    out.renderedIpdM = rendered_ipd_m;
    out.scale = base_ipd_m > 0.001f ? rendered_ipd_m / base_ipd_m : 1.0f;
    out.flags = (depth_active() ? dcsvr_perception::boost_flags::enabled : 0u) |
        (std::fabs(out.scale - 1.0f) > 1e-4f ? dcsvr_perception::boost_flags::applied : 0u);
    dcsvr_perception::seqlock_write(boost_block.get(), out);
    boost_last_base = base_ipd_m;
}

// The session or instance ends: a last record with nothing applied (flags 0, scale 1, rendered = base), for the display
// time after the last one written, so the engine stops correcting.
void write_boost_stop_locked() {
    if (!boost_block.get() || !boost_out_time) return;
    dcsvr_perception::BoostOut out{};
    out.displayTime = ++boost_out_time;
    out.frame = ++boost_out_frames;
    out.baseIpdM = out.renderedIpdM = boost_last_base;
    out.scale = 1.0f;
    dcsvr_perception::seqlock_write(boost_block.get(), out);
}

// ---- Local\DcsVrPupilFov_v1 (dcsvr_pupil_fov.h) ---------------------------------------------------------------------
// The FOV pairs of each display time: what DCS renders with and what Quad Views composes with. Created at instance
// creation, closed with the last instance. The writer keeps its own copy of the block (no allocation per frame) and
// publishes it whole under the seqlock, under the main mutex (no other lock).
dcsvr_perception::SharedBlock<dcsvr_pupil_fov::Block> fov_block;
dcsvr_pupil_fov::Block fov_mirror{};

// The layer may change the views: the pupil shift or Depth boost configured (their hotkeys only pause them: identity
// entries then).
bool fov_shift_configured() { return settings.enabled || depth_settings.enabled; }

void fov_publish_locked() {
    if (fov_block.get()) dcsvr_perception::seqlock_write(fov_block.get(), fov_mirror);
}

void fov_clear_entries_locked() {
    for (auto& e : fov_mirror.entries) e = dcsvr_pupil_fov::Entry{};
    fov_mirror.newestDisplayTime = 0;
}

void fov_open_locked() {
    if (!fov_block.get()) {
        const bool created = fov_block.open(dcsvr_pupil_fov::block_name, shm_suffix);
        log("perception: shared memory %ls%ls %s\n", dcsvr_pupil_fov::block_name, shm_suffix, created ? "created" : "FAILED");
        if (!created) return;
    }
    fov_mirror = dcsvr_pupil_fov::Block{};
    fov_mirror.magic = dcsvr_pupil_fov::magic_value;
    fov_mirror.version = dcsvr_pupil_fov::version_value;
    fov_mirror.size = sizeof(dcsvr_pupil_fov::Block);
    fov_mirror.writerPid = GetCurrentProcessId();
    fov_mirror.ringSize = dcsvr_pupil_fov::ring_size;
    fov_mirror.enabled = fov_shift_configured() ? 1u : 0u;
    fov_publish_locked();
}

// enabled follows the settings (re-read while running); turned off, the ring is cleared. Written only on a change.
void fov_update_enabled_locked() {
    const std::uint32_t on = fov_shift_configured() ? 1u : 0u;
    if (!fov_block.get() || fov_mirror.enabled == on) return;
    fov_mirror.enabled = on;
    if (!on) fov_clear_entries_locked();
    fov_publish_locked();
}

void fov_close_locked() {
    if (!fov_block.get()) return;
    fov_mirror.enabled = 0;
    fov_clear_entries_locked();
    fov_publish_locked();
    fov_block.close();
}

// The entry of a display time: the one already written for it, else the next slot of the ring.
dcsvr_pupil_fov::Entry* fov_entry_locked(XrTime time) {
    for (auto& e : fov_mirror.entries)
        if (e.displayTime == time) return &e;
    auto& e = fov_mirror.entries[fov_mirror.writeCount++ % dcsvr_pupil_fov::ring_size];
    e = dcsvr_pupil_fov::Entry{};
    e.displayTime = time;
    if (time > fov_mirror.newestDisplayTime) fov_mirror.newestDisplayTime = time;
    return &e;
}

void fov_fill(dcsvr_pupil_fov::View& out, const XrPosef& original_pose, const XrFovf& original_fov, const XrPosef& shifted_pose,
    const XrFovf& shifted_fov) {
    out = dcsvr_pupil_fov::View{};
    const XrFovf* f[2] = {&shifted_fov, &original_fov};
    float* dst[2] = {out.shiftedFov, out.originalFov};
    for (int k = 0; k < 2; ++k) { dst[k][0] = f[k]->angleLeft; dst[k][1] = f[k]->angleRight; dst[k][2] = f[k]->angleUp; dst[k][3] = f[k]->angleDown; }
    out.positionDelta[0] = shifted_pose.position.x - original_pose.position.x;
    out.positionDelta[1] = shifted_pose.position.y - original_pose.position.y;
    out.positionDelta[2] = shifted_pose.position.z - original_pose.position.z;
    // conjugate(a) * b (Hamilton product).
    const Quat a = conjugate(to_quat(original_pose.orientation)), b = to_quat(shifted_pose.orientation);
    out.orientationDelta[0] = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    out.orientationDelta[1] = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    out.orientationDelta[2] = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    out.orientationDelta[3] = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
}

// At xrLocateViews, after the views are final: the pairs handed to DCS for this display time (a second locate rewrites
// the same entry, keeping an xrEndFrame confirmation away until the next one).
void write_fov_located_locked(XrTime time, const XrView* original, const XrView* shifted, std::uint32_t n, std::uint32_t flags) {
    fov_update_enabled_locked();
    if (!fov_block.get() || !fov_mirror.enabled || n > 4) return;
    dcsvr_pupil_fov::Entry* e = fov_entry_locked(time);
    e->viewCount = n;
    e->flags = flags;
    for (std::uint32_t v = 0; v < n; ++v) fov_fill(e->views[v], original[v].pose, original[v].fov, shifted[v].pose, shifted[v].fov);
    for (std::uint32_t v = n; v < 4; ++v) e->views[v] = dcsvr_pupil_fov::View{};
    fov_publish_locked();
}

// At xrEndFrame: the pairs of the views DCS submitted (the first projection layer). matched[v]: the frame whose shift
// view v carried (its original was put back), or null for a view the layer did not change (identity, as submitted).
void write_fov_submitted_locked(XrTime time, const XrCompositionLayerProjectionView* submitted, std::uint32_t n,
    const Frame* const* matched) {
    if (!fov_block.get() || !fov_mirror.enabled || n < 2 || n > 4) return;
    dcsvr_pupil_fov::Entry* e = fov_entry_locked(time);
    const std::uint32_t flags = e->flags | dcsvr_pupil_fov::entry_flags::submitted;
    e->viewCount = n;
    for (std::uint32_t v = 0; v < n; ++v) {
        if (matched[v]) fov_fill(e->views[v], matched[v]->original[v].pose, matched[v]->original[v].fov, matched[v]->shifted[v].pose, matched[v]->shifted[v].fov);
        else fov_fill(e->views[v], submitted[v].pose, submitted[v].fov, submitted[v].pose, submitted[v].fov);
    }
    for (std::uint32_t v = n; v < 4; ++v) e->views[v] = dcsvr_pupil_fov::View{};
    e->flags = flags;
    fov_publish_locked();
}

// GazeOut for a display time, once per new time, after the views are final (fovTan is what DCS renders with). The gaze
// is Depth boost's (Pimax PVR per-eye, else the selected source), or SimulateGaze's direction in each eye's frame, or
// (PublishGaze without Depth boost) the frame's selected gaze, eye_gaze: Quad Views' export, the OpenXR eye gaze or the
// focus views, in each primary view's frame.
void write_gaze_out_locked(XrTime time, const XrView* views, std::uint32_t n, Vec3 simulated, const EyeGazes& eye_gaze) {
    if (!gaze_out_wanted(depth_settings.enabled, settings.simulate, settings.publish_gaze) || time <= gaze_out_time || !open_blocks_locked()) return;
    gaze_out_time = time;
    dcsvr_perception::GazeOut out{};
    out.displayTime = time;
    out.viewCount = n;
    Vec3 world[2]{};
    bool valid = false;
    if (settings.simulate) {
        valid = true;
        for (int e = 0; e < 2; ++e) world[e] = rotate(to_quat(views[e].pose.orientation), normalized(simulated));
    } else if (depth_settings.enabled && depth_gaze.valid) {
        valid = true;
        world[0] = depth_gaze.world[0]; world[1] = depth_gaze.world[1];
    } else if (!depth_settings.enabled && settings.publish_gaze && eye_gaze.source != GazeSource::none) {
        valid = true;
        for (int e = 0; e < 2; ++e) world[e] = rotate(to_quat(views[e].pose.orientation), eye_gaze.local[e]);
    }
    for (std::uint32_t v = 0; v < n && v < 4; ++v) {
        const XrFovf& f = views[v].fov;
        out.fovTan[v][0] = std::tan(f.angleLeft); out.fovTan[v][1] = std::tan(f.angleRight);
        out.fovTan[v][2] = std::tan(f.angleUp); out.fovTan[v][3] = std::tan(f.angleDown);
        out.gazeTan[v][0] = out.gazeTan[v][1] = 0;
        if (!valid) continue;
        const Vec3 local = rotate(conjugate(to_quat(views[v].pose.orientation)), world[v % 2]);
        if (!(local.z < -0.05f)) { valid = false; continue; }
        out.gazeTan[v][0] = local.x / -local.z; out.gazeTan[v][1] = local.y / -local.z;
    }
    if (!valid) for (auto& g : out.gazeTan) g[0] = g[1] = 0;
    out.flags = (valid ? dcsvr_perception::gaze_flags::valid : 0u) |
        (valid && !settings.simulate && (depth_settings.enabled ? fixation.saccade() : eye_gaze.saccade) ? dcsvr_perception::gaze_flags::saccade : 0u) |
        (valid && !settings.simulate && depth_gaze.per_eye ? dcsvr_perception::gaze_flags::per_eye : 0u);
    const float w = fixation.inverse_distance();
    out.vergenceM = depth_settings.enabled && fixation.source() == DepthSource::vergence && std::isfinite(w) ? (w < 0.001f ? 1000.0f : 1.0f / w) : 0.0f;
    dcsvr_perception::seqlock_write(gaze_block.get(), out);
    // The matcher pairs the engine's summaries with this gaze (its head-frame direction).
    matcher.note_gaze(time, display_frames, depth_settings.enabled && depth_gaze.valid, depth_gaze.head_dir, depth_gaze.saccade);
}

// Depth boost, once per display time. The gaze (Depth boost never uses SimulateGaze): Pimax's PVR per-eye gaze when it
// has a valid sample, else select_gaze()'s (Quad Views' export, the OpenXR eye gaze, the focus views). No gaze is no
// sample. A gaze identical to the previous frame's is not fresh either (the provider held or cached it, e.g. during a
// blink, or, read from the focus views, it is the fixed focus it falls back to): the estimator gets no sample and the
// scale holds. Quad Views' saccade flag counts as a saccade. Then the scene depth summary for this gaze (the engine
// module's, DepthMatcher), the fixation state (FixationDecider) and the scale (DepthBoostController).
// The engine's depth grid (perception_shm.h, DepthGrid), opened while the predictive transition is in effect; a new
// seq is a new grid, noted with the frame it arrived.
void read_grid_locked() {
    if (!grid_block.get()) {
        if (grid_retry++ % 90 != 0) return;
        const bool ok = grid_block.open(dcsvr_perception::grid_block_name, shm_suffix);
        log("perception: shared memory %ls%ls %s\n", dcsvr_perception::grid_block_name, shm_suffix, ok ? "open" : "FAILED");
        if (!ok) return;
    }
    static dcsvr_perception::DepthGrid copy{};
    if (!dcsvr_perception::seqlock_read(grid_block.get(), copy) || copy.seq == grid_seq) return;
    grid = copy; grid_seq = copy.seq; grid_arrival = display_frames; have_grid = true;
}

// The landing of a prediction in the grid: fresh (arrived within 4 frames, its display time one the layer located at
// most 4 frames ago), projected into the sampled view's frame at that display time. The landing direction is the eye's
// whose view was sampled (the left eye for view 0), from the head frame through this frame's head basis to world space.
predictive::GridLanding grid_landing_locked(const predictive::Prediction& p, const HeadBasis& head) {
    predictive::GridLanding none;
    // displayTime 0: the engine had no gaze at the capture (no pose to project with).
    if (!have_grid || display_frames - grid_arrival > 4 || grid.displayTime == 0) return none;
    const PoseAt* pose = nullptr;
    for (const auto& r : pose_ring)
        if (r.frame && r.time == grid.displayTime) { pose = &r; break; }
    if (!pose || display_frames - pose->frame > 4 || grid.view >= pose->count) return none;
    const Vec3 world = pvr::head_to_world(head, p.landing[grid.view % 2]);
    return predictive::classify_grid(grid, rotate(conjugate(pose->q[grid.view]), world));
}

// Why the predictive transition, selected, is not in effect (gradual instead).
const char* predict_fallback_reason() {
    if (!pvr_wanted_setting) return "PimaxPvrGaze=0";
    if (!pvr_hook::ready.load()) return "no usable Pimax PVR client (per-eye gaze)";
    if (!pvr_hook::polling.load()) return "the runtime's PVR session is not known";
    return "no samples from PVR eye tracking";
}

void update_depth_locked(const XrView* views, std::uint32_t n, const EyeGazes& gaze, XrTime time, float base_ipd, const pvr::Sample& pvr_sample,
    const std::vector<pvr_hook::RawSample>& stream, double now_s) {
    const bool configured = depth_settings.enabled;
    if (!configured) { depth = DepthBoostController{}; decider = FixationDecider{}; depth_gaze = DepthGaze{}; gate.update({}, depth_settings, predict_settings); predict_effective = false; return; }
    if (time <= depth_time) return;  // located again for the same (or an older) display time: keep the scale
    const float dt = depth_time ? static_cast<float>(static_cast<double>(time - depth_time) * 1e-9) : 0.0f;
    depth_time = time;
    ++display_frames;
    if (dt > 0.001f && dt < 0.1f) frame_period_s += (dt - frame_period_s) * 0.1f;
    open_blocks_locked();
    const Vec3 el = to_vec(views[0].pose.position), er = to_vec(views[1].pose.position);
    const HeadBasis head = head_basis(el, er, head_forward(to_quat(views[0].pose.orientation), to_quat(views[1].pose.orientation)));
    // The predictive transition: the high-rate samples since the last frame go through the saccade detector.
    auto& ps = predict_second;
    if (!ps.started) { ps.started = true; ps.gate = gate.stats(); ps.saccades = detector.saccades(); ps.dropped = detector.dropped_count(); }
    ++ps.frames;
    bool new_prediction = false;
    predictive::Prediction prediction{};
    bool stream_dropped = false;  // a gap (or an overlong flight) in the stream: a post-step hold expires
    bool stream_hint = false;     // a real saccade (3 degrees or more) in flight during this frame's samples
    for (const auto& r : stream) {
        predictive::GazeSample g;
        g.t = r.time; g.valid = r.valid;
        for (int e = 0; e < 2; ++e) g.eye[e] = normalized({r.tan[e][0], r.tan[e][1], -1.0f});
        const unsigned events = detector.push(g, predict_settings);
        if (events & predictive::SaccadeDetector::predicted) { new_prediction = true; prediction = detector.prediction(); }
        if (events & predictive::SaccadeDetector::dropped) stream_dropped = true;
        // The hint is a level (3 degrees travelled until the landing): latched per sample, as a short saccade's lies
        // between two frames at 2X and 3X.
        if (detector.saccade_hint() || ((events & predictive::SaccadeDetector::landed) && detector.last_amplitude() >= predictive::SaccadeDetector::kHintTravelDeg))
            stream_hint = true;
        ++ps.samples;
    }
    if (!stream.empty()) last_stream_tick = GetTickCount64();
    const bool predict_selected = selected_transition() == predictive::Transition::predictive;
    predict_effective = depth_active() && predict_selected && pvr_hook::polling.load() && GetTickCount64() - last_stream_tick < 300;
    predict_reason = predict_selected && !predict_effective ? (depth_active() ? predict_fallback_reason() : "Depth boost switched off") : nullptr;
    if (!predict_logged_init || predict_effective != predict_logged_effective || predict_reason != predict_logged_reason) {
        if (predict_selected || predict_logged_effective)
            log("predict: transition %s%s%s\n", predict_effective ? "predictive in effect" : predict_selected ? "predictive selected, gradual in effect: " : "gradual",
                predict_reason && predict_selected ? predict_reason : "", "");
        predict_logged_init = true; predict_logged_effective = predict_effective; predict_logged_reason = predict_reason;
    }
    if (predict_effective) ++ps.effective_frames;
    pose_ring[pose_next++ % pose_ring.size()] = [&] {
        PoseAt p; p.time = time; p.frame = display_frames; p.count = std::min<std::uint32_t>(n, 4);
        for (std::uint32_t v = 0; v < p.count; ++v) p.q[v] = to_quat(views[v].pose.orientation);
        return p;
    }();
    bool valid = false, fresh = false, per_eye = false;
    Vec3 gl{0, 0, -1}, gr{0, 0, -1};
    GazeSource source = gaze.source;
    if (pvr_sample.valid) {
        // Pimax PVR: each eye's own gaze (vergence even where the OpenXR gaze and Quad Views combine both eyes).
        source = GazeSource::pvr;
        per_eye = true;
        gl = pvr::head_to_world(head, pvr_sample.head[0]);
        gr = pvr::head_to_world(head, pvr_sample.head[1]);
        fresh = pvr_sample.fresh;
    } else {
        if (gaze.source == GazeSource::focus && n == 4) {
            const XrFovf* f[2] = {&views[2].fov, &views[3].fov};
            const auto same = [](const XrFovf& a, const XrFovf& b) {
                return a.angleLeft == b.angleLeft && a.angleRight == b.angleRight && a.angleUp == b.angleUp && a.angleDown == b.angleDown;
            };
            const auto finite = [](const XrFovf& a) {
                return std::isfinite(a.angleLeft) && std::isfinite(a.angleRight) && std::isfinite(a.angleUp) && std::isfinite(a.angleDown) &&
                    a.angleRight > a.angleLeft && a.angleUp > a.angleDown;
            };
            fresh = (!have_last_focus || !same(*f[0], last_focus_fov[0]) || !same(*f[1], last_focus_fov[1])) && finite(*f[0]) && finite(*f[1]);
        } else if (gaze.source == GazeSource::qv || gaze.source == GazeSource::xr) {
            const auto same = [](Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
            fresh = !have_last_qv || !same(gaze.local[0], last_qv[0]) || !same(gaze.local[1], last_qv[1]);
        }
        // Each eye's gaze in world space (from its primary view's frame).
        gl = rotate(to_quat(views[0].pose.orientation), gaze.local[0]);
        gr = rotate(to_quat(views[1].pose.orientation), gaze.local[1]);
    }
    if (settings.simulate) {
        // SimulateGaze drives Depth boost too (both eyes along the same direction, as GazeOut publishes it), so the
        // engine module's scene depth can be tested live without anyone in the headset. Parallel eyes read as far,
        // which never vetoes the scene depth.
        const Vec3 sim = normalized(Vec3{std::tan(settings.simulate_x_deg / 57.2958f), std::tan(settings.simulate_y_deg / 57.2958f), -1});
        gl = rotate(to_quat(views[0].pose.orientation), sim);
        gr = rotate(to_quat(views[1].pose.orientation), sim);
        source = GazeSource::simulated;
        per_eye = false;
        fresh = true;
    }
    if (n == 4) { last_focus_fov = {views[2].fov, views[3].fov}; have_last_focus = true; }
    if (gaze.source == GazeSource::qv || gaze.source == GazeSource::xr) { last_qv = {gaze.local[0], gaze.local[1]}; have_last_qv = true; }
    Vec3 cyclopean{};
    float w = std::nanf("");
    const Vec3 c = normalized(gl + gr);
    cyclopean = {dot(c, head.right), dot(c, head.up), -dot(c, head.forward)};
    if (fresh) {
        w = inverse_fixation_distance(el, er, gl, gr, head);
        valid = std::isfinite(w);
    }
    const bool have_source = settings.simulate || pvr_sample.valid || gaze.source != GazeSource::none;
    if (!fresh && have_source) {
        // A repeated gaze: no sample, the history and the state hold (a repeat longer than kHoldMaxS is a gap).
        fixation.hold(time);
    } else {
        // A saccade seen by Quad Views' detector, or (predictive) a real one in the high-rate stream, counts as one.
        fixation.update(time, valid, cyclopean, w, depth_settings.near_m,
            valid && ((gaze.saccade && source == GazeSource::qv) || (predict_effective && (stream_hint || detector.saccade_hint()))));
    }
    depth_gaze.valid = source != GazeSource::none && (settings.simulate || pvr_sample.valid || gaze.source != GazeSource::none);
    depth_gaze.per_eye = per_eye;
    depth_gaze.source = source;
    depth_gaze.world[0] = gl; depth_gaze.world[1] = gr;
    depth_gaze.head_dir = cyclopean;
    depth_gaze.saccade = fixation.saccade();

    // The scene depth for this gaze, from the engine module.
    const DepthMatcher::State scene = matcher.evaluate(read_depth_block_locked(), display_frames, cyclopean, fixation.valid());
    if (scene == DepthMatcher::State::fresh) last_fresh_scene_frame = display_frames;
    // The engine's dual camera acknowledged (perception_shm.h CockpitStereo): the cockpit stays at the true eyes, so no
    // fast return is ever needed and the cap is MaxScale itself (not limited by DownMmPerS x RecoverMs).
    const bool dual = cockpit_stereo_acked_locked();
    const float cap = dual ? std::clamp(std::isfinite(depth_settings.max_scale) ? depth_settings.max_scale : 1.0f, 1.0f, 2.0f)
                           : depth_boost_cap(depth_settings, base_ipd * 1000.0f);
    FixationDecider::Input fi;
    fi.dt = dt;
    fi.gaze_valid = fixation.valid();
    fi.saccade = fixation.saccade();
    fi.depth_fresh = scene == DepthMatcher::State::fresh;
    fi.depth = matcher.summary();
    fi.vergence_ok = fixation.source() == DepthSource::vergence && fixation.settled();
    fi.vergence = fixation.fixation();
    fi.vergence_w = fixation.inverse_distance();
    fi.vergence_pending = fixation.source() == DepthSource::vergence && fixation.valid() && !fixation.settled() && !fixation.saccade();
    decider.update(fi, depth_settings, cap);

    DepthBoostController::Input in;
    in.dt = dt;
    in.active = depth_active();
    in.gaze_valid = fixation.valid();
    in.saccade = fixation.saccade();
    in.saccade_started = fixation.saccade_started();
    in.settled = !decider.hold_up() && !fixation.saccade();
    in.fixation = decider.state();
    in.source = fixation.source();
    in.base_ipd_m = base_ipd;
    in.target = decider.target();
    in.far_rates = decider.state() == Fixation::far_;
    in.landing_window = decider.landing_window();
    in.landing_step = decider.landing_step();
    // The predictive transition: a new prediction's landing in the grid, the gate's step and hold.
    predictive::PredictiveGate::Input gi;
    gi.active = predict_effective;
    if (predict_effective) {
        read_grid_locked();
        const bool fresh_grid = have_grid && display_frames - grid_arrival <= 4;
        if (fresh_grid) ++ps.grid_fresh; else ++ps.grid_stale;
        gi.now_s = now_s;
        gi.frame_s = frame_period_s;
        gi.gaze_valid = fixation.valid() && !stream_dropped;
        gi.new_prediction = new_prediction;
        gi.prediction = prediction;
        if (new_prediction) gi.landing = grid_landing_locked(prediction, head);
        gi.landed_id = detector.landed_id();
        gi.scale = depth.scale();
        gi.base_mm = base_ipd * 1000.0f;
        gi.cap = cap;
        gi.decider_state = decider.state();
        gi.frame_class = decider.frame_class();
    }
    const auto po = gate.update(gi, depth_settings, predict_settings);
    in.jump_mm = po.jump_mm;
    in.hold = po.hold;
    in.no_saccade_step = po.no_saccade_step;
    if (po.no_saccade_step) { in.saccade_started = false; in.landing_step = false; }
    const float scale_before = depth.scale();
    if (dual) {
        // The engine keeps the cockpit at the true eye distance. The world's scale still follows the gaze (the usual far
        // target and curve), but a cockpit fixation keeps the last far target (the world does not breathe at every glance
        // at the instruments), and the scale moves only smoothly, both ways, at DualCameraMmPerS: no saccade or landing
        // steps, no predictive jumps, so a one-frame mis-pairing in the engine stays under 0.25 mm.
        if (decider.state() == Fixation::far_ && std::isfinite(decider.target())) dual_far_target = decider.target();
        DepthBoostSettings slow = depth_settings;
        slow.stepped = false;
        slow.full_near_return = false;
        slow.saccade_step_mm = 0;
        slow.up_mm_per_s = slow.down_mm_per_s = dual_camera_mm_per_s;
        slow.recover_ms = 1e6f;  // the cap is MaxScale (depth_boost_cap)
        slow.hold_ms = 1e6f;     // a lost gaze holds the scale
        // Equal steps on every frame: the mean frame period, not this display time's delta (it alternates with the
        // runtime's pacing, and the world's apparent speed then jitters into DLSS's history along cockpit edges).
        in.dt = std::clamp(frame_period_s, 0.002f, 0.05f);
        in.gaze_valid = true;
        in.settled = true;
        in.target = std::clamp(dual_far_target, 1.0f, cap);
        in.far_rates = true;
        in.saccade = false;
        in.saccade_started = false;
        in.landing_window = false;
        in.landing_step = false;
        in.jump_mm = std::nanf("");
        in.hold = 0;
        depth.step(in, slow);
        if (++dual_log_frames % 90 == 1)
            log("depth: dual camera: target %.3f (far %.3f, state %s) scale %.3f, cap %.2f\n", in.target, dual_far_target,
                decider.state() == Fixation::far_ ? "far" : decider.state() == Fixation::near_ ? "near" : "unknown", depth.scale(), cap);
    } else if (depth_legacy) {
        dual_far_target = depth.scale();  // entering the dual mode starts from where the scale is
        depth.step(in, depth_settings);
    } else {
        // Depth boost ships with the engine's dual camera only: without its fresh acknowledgement (the engine not
        // running it, paused, or gone) the eye distance holds or slides back to x1 at DualCameraMmPerS, equal steps.
        DepthBoostSettings slow = depth_settings;
        slow.stepped = false;
        slow.full_near_return = false;
        slow.saccade_step_mm = 0;
        slow.up_mm_per_s = slow.down_mm_per_s = dual_camera_mm_per_s;
        slow.recover_ms = 1e6f;
        slow.hold_ms = 1e6f;
        in.dt = std::clamp(frame_period_s, 0.002f, 0.05f);
        in.gaze_valid = true;
        in.settled = true;
        in.target = 1.0f;
        in.far_rates = true;
        in.saccade = false;
        in.saccade_started = false;
        in.landing_window = false;
        in.landing_step = false;
        in.jump_mm = std::nanf("");
        in.hold = 0;
        depth.step(in, slow);
        dual_far_target = depth.scale();
    }
    if (const int path = dual ? 1 : depth_legacy ? 2 : 0; path != depth_path_logged) {
        log("depth: path %s\n", path == 1 ? "dual camera (the engine keeps the cockpit at the true eye distance)" :
            path == 2 ? "legacy (Legacy=1: gaze-driven, the cockpit returns to x1 in saccades)" :
            "waiting for the engine's dual camera: x1 (slides back at DualCameraMmPerS)");
        depth_path_logged = path;
    }
    if (const auto& dd = gate.decision(); new_prediction && (dd.made || depth_trace)) {
        // One line per step (and per prediction with Trace=1): size and timing against the predicted saccade.
        const auto& p = dd.p;
        log("predict: %s %+.2f mm (scale %.4f -> %.4f, target %.3f) landing %s %s %.0f m cell %d,%d near %.2f far %.2f ring %.2f/%.2f; "
            "%.0f ms after onset, %.0f ms before the predicted end; amplitude %.1f deg (main sequence %.1f, travelled %.1f, peak %.0f deg/s, gain %.2f) at display time %lld\n",
            dd.made ? "step" : "no step", dd.jump_mm, scale_before, depth.scale(), dd.target, predictive::grid_status_name(dd.landing.status),
            depth_class_name(dd.landing.cls), dd.landing.distance_m, dd.landing.col, dd.landing.row, dd.landing.near_share, dd.landing.far_share,
            dd.landing.ring_near, dd.landing.ring_far, (now_s - p.onset_t) * 1000.0, dd.remaining_s * 1000.0f, p.amplitude_deg, p.raw_amplitude_deg,
            p.travelled_deg, p.peak_deg_s, detector.gain(), static_cast<long long>(time));
    }
    if (depth_trace) {
        // Per display time: gaze (v valid, s saccade), vergence (proven, settled, class), scene summary (state, reason,
        // class, edge, shares), the decider (state, source, landing candidate and agreement, pursuit), target and scale.
        const DepthSummary& ds = matcher.summary();
        log("dtrace: t=%lld v=%d s=%d verg=%s/%d/%s scene=%d/%d %s e=%d n=%.2f f=%.2f r=%.2f/%.2f | state=%s src=%s land=%d cand=%s %.3f purs=%s %.3f "
            "hold=%d step=%d | target %.4f scale %.4f\n", static_cast<long long>(time), fixation.valid() ? 1 : 0, fixation.saccade() ? 1 : 0,
            source_name(fixation.source()), fixation.settled() ? 1 : 0, fixation_name(fixation.fixation()), static_cast<int>(scene),
            static_cast<int>(matcher.reason()), depth_class_name(decider.depth_class()), decider.edge() ? 1 : 0, ds.near_share, ds.far_share,
            ds.edge_near, ds.edge_far, fixation_name(decider.state()), fix_source_name(decider.source()), decider.landing_pending() ? 1 : 0,
            fixation_name(decider.candidate()), decider.agree_s(), fixation_name(decider.pursuit()), decider.pursuit_s(), decider.hold_up() ? 1 : 0,
            in.landing_step ? 1 : 0, decider.target(), depth.scale());
    }
    account_depth_locked(base_ipd * 1000, base_ipd * depth.scale() * 1000, scene, pvr_sample, in.landing_step);
}

// ---- OpenXR eye gaze (XR_EXT_eye_gaze_interaction) ------------------------------------------------------------------
// Without the bundled Quad Views' export (Pimax's native quad views) the focus views cannot tell a real gaze from the
// fixed spot the runtime shows without one, so the layer reads the runtime's eye gaze itself. DCS does not enable the
// extension; Pimax's runtime offers it. The design is Cheeky's (external/cheeky/openxr_layer/openxr_layer.cpp), copied
// and adapted: the extension is added at instance creation when the runtime offers it (the instance is created as the
// application asked if the runtime refuses it); the layer's own action set with one pose action is bound to
// /user/eyes_ext/input/gaze_ext/pose, merged into the application's suggestion for the eye-gaze profile or suggested
// alone just before the application attaches; it is attached together with the application's action sets in the
// application's xrAttachSessionActionSets and made active in the application's own xrSyncActions. The layer never
// attaches or syncs on its own: attachment is once per session and belongs to the application, and an extra gaze-only
// sync could deactivate its controllers or consume input changes. When a merged call fails, the application's call is
// made again exactly as it was, so its input never depends on the layer's action. Another layer doing the same (Cheeky)
// merges its own binding and set the same way; an application that enables the extension itself keeps its own action.
enum class Availability { absent, present, unknown };

struct EyeGazeState {
    XrInstance instance{XR_NULL_HANDLE};
    bool extension{};                    // XR_EXT_eye_gaze_interaction is enabled on the instance
    XrActionSet action_set{XR_NULL_HANDLE};
    XrAction action{XR_NULL_HANDLE};
    XrPath path{XR_NULL_PATH}, profile{XR_NULL_PATH};
    bool binding_submitted{};
    bool app_profile_bound{};            // the application's own eye-gaze suggestion stands without the layer's binding
    XrSession session{XR_NULL_HANDLE};
    XrSpace space{XR_NULL_HANDLE};       // the gaze action's space; only when the system supports eye gaze
    bool attached{};                     // the layer's set was attached with the application's
    bool sync_off{};                     // merged syncs kept failing: the application syncs alone
    unsigned sync_failures{};            // consecutive merged syncs that failed
    const char* absent_reason{"no instance"};  // why the eye gaze is not available (string literals only)
};
EyeGazeState eye_state;
std::mutex eye_mutex;  // never nested with the main mutex
bool xr_was_present{}, focus_warned{};  // under the main mutex

// Whether the runtime (and the layers below) offer the extension, asked before the instance exists.
Availability probe_eye_gaze(PFN_xrGetInstanceProcAddr gipa) {
    PFN_xrVoidFunction f{};
    if (XR_FAILED(gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &f)) || f == nullptr) return Availability::unknown;
    const auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(f);
    std::uint32_t count{};
    if (XR_FAILED(enumerate(nullptr, 0, &count, nullptr))) return Availability::unknown;
    std::vector<XrExtensionProperties> properties(count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    if (count && XR_FAILED(enumerate(nullptr, count, &count, properties.data()))) return Availability::unknown;
    for (std::uint32_t i = 0; i < count && i < properties.size(); ++i)
        if (std::strcmp(properties[i].extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0) return Availability::present;
    return Availability::absent;
}

// After the instance exists: the calls below the layer, and the layer's action set and action when the extension is on.
void setup_eye_gaze(XrInstance instance, bool wanted, bool extension, const char* reason) {
    {
        std::lock_guard lock(mutex);
        xr_was_present = false;
        focus_warned = false;
    }
    std::lock_guard lock(eye_mutex);
    eye_state = EyeGazeState{};
    eye_state.instance = instance;
    eye_state.extension = extension;
    const auto load = [instance](const char* name, auto& fn) {
        PFN_xrVoidFunction f{};
        if (XR_SUCCEEDED(next_gipa(instance, name, &f)) && f) fn = reinterpret_cast<std::decay_t<decltype(fn)>>(f);
    };
    load("xrCreateActionSet", xrd.create_action_set);
    load("xrDestroyActionSet", xrd.destroy_action_set);
    load("xrCreateAction", xrd.create_action);
    load("xrStringToPath", xrd.string_to_path);
    load("xrGetSystemProperties", xrd.get_system_properties);
    load("xrCreateActionSpace", xrd.create_action_space);
    load("xrLocateSpace", xrd.locate_space);
    load("xrSuggestInteractionProfileBindings", xrd.suggest_bindings);
    load("xrAttachSessionActionSets", xrd.attach_action_sets);
    load("xrSyncActions", xrd.sync_actions);
    load("xrCreateSession", xrd.create_session);
    load("xrDestroySession", xrd.destroy_session);
    load("xrDestroyInstance", xrd.destroy_instance);
    if (!wanted || !extension) {
        eye_state.absent_reason = reason ? reason : "XR_EXT_eye_gaze_interaction is not enabled";
        log("eye gaze: not used (%s)\n", eye_state.absent_reason);
        return;
    }
    if (!xrd.create_action_set || !xrd.create_action || !xrd.string_to_path || !xrd.locate_space || !xrd.create_action_space) {
        eye_state.absent_reason = "the runtime lacks the action functions";
        log("eye gaze: not used (%s)\n", eye_state.absent_reason);
        return;
    }
    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(set_info.actionSetName, "dcsvr_pupil_gaze");
    strcpy_s(set_info.localizedActionSetName, "DCS Control pupil gaze");
    XrResult r = xrd.create_action_set(instance, &set_info, &eye_state.action_set);
    if (XR_SUCCEEDED(r)) {
        XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
        action_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
        strcpy_s(action_info.actionName, "gaze_pose");
        strcpy_s(action_info.localizedActionName, "Gaze pose");
        r = xrd.create_action(eye_state.action_set, &action_info, &eye_state.action);
        if (XR_SUCCEEDED(r)) r = xrd.string_to_path(instance, "/user/eyes_ext/input/gaze_ext/pose", &eye_state.path);
        if (XR_SUCCEEDED(r)) r = xrd.string_to_path(instance, "/interaction_profiles/ext/eye_gaze_interaction", &eye_state.profile);
        if (XR_FAILED(r) && xrd.destroy_action_set) xrd.destroy_action_set(eye_state.action_set);  // destroys the action too
    }
    if (XR_FAILED(r)) {
        eye_state.action_set = XR_NULL_HANDLE;
        eye_state.action = XR_NULL_HANDLE;
        eye_state.absent_reason = "the gaze action could not be created";
        log("eye gaze: not used (%s: %d)\n", eye_state.absent_reason, r);
        return;
    }
    eye_state.absent_reason = "no session yet";
    log("eye gaze: action set dcsvr_pupil_gaze created\n");
}

// The layer's own suggestion for the eye-gaze profile, when the application has not made one the layer merged into.
// Suggestions replace earlier ones for the same profile, so it is made only once, just before the attachment.
void ensure_eye_binding_locked() {
    if (eye_state.binding_submitted || eye_state.action == XR_NULL_HANDLE || !xrd.suggest_bindings) return;
    // The application's own bindings for the profile took without the layer's: a suggestion of the layer's alone would
    // replace them, so the layer goes without the OpenXR eye gaze instead.
    if (eye_state.app_profile_bound) return;
    const XrActionSuggestedBinding binding{eye_state.action, eye_state.path};
    XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    info.interactionProfile = eye_state.profile;
    info.countSuggestedBindings = 1;
    info.suggestedBindings = &binding;
    const XrResult r = xrd.suggest_bindings(eye_state.instance, &info);
    eye_state.binding_submitted = XR_SUCCEEDED(r);
    log("eye gaze: binding suggested (%d)\n", r);
}

XRAPI_ATTR XrResult XRAPI_CALL hook_SuggestInteractionProfileBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* suggested) {
    XrAction action{XR_NULL_HANDLE};
    XrPath path{XR_NULL_PATH};
    {
        std::lock_guard lock(eye_mutex);
        if (suggested && instance == eye_state.instance && eye_state.action != XR_NULL_HANDLE && suggested->interactionProfile == eye_state.profile) {
            action = eye_state.action;
            path = eye_state.path;
        }
    }
    if (action == XR_NULL_HANDLE) return xrd.suggest_bindings(instance, suggested);
    // The application suggests for the eye-gaze profile itself: the layer's binding joins its own.
    std::vector<XrActionSuggestedBinding> bindings;
    if (suggested->countSuggestedBindings && suggested->suggestedBindings)
        bindings.assign(suggested->suggestedBindings, suggested->suggestedBindings + suggested->countSuggestedBindings);
    if (std::none_of(bindings.begin(), bindings.end(), [&](const XrActionSuggestedBinding& b) { return b.action == action && b.binding == path; }))
        bindings.push_back({action, path});
    XrInteractionProfileSuggestedBinding merged = *suggested;
    merged.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size());
    merged.suggestedBindings = bindings.data();
    XrResult result = xrd.suggest_bindings(instance, &merged);
    const bool ours = XR_SUCCEEDED(result);
    if (!ours) {
        log("eye gaze: the application's eye-gaze suggestion failed with the gaze binding (%d); made without it\n", result);
        result = xrd.suggest_bindings(instance, suggested);
    }
    {
        std::lock_guard lock(eye_mutex);
        // A successful suggestion without the layer's binding replaced it; a failed one left the previous in place.
        if (instance == eye_state.instance) {
            eye_state.binding_submitted = ours || (eye_state.binding_submitted && XR_FAILED(result));
            // A failed suggestion leaves the application's earlier bindings in place.
            eye_state.app_profile_bound = (!ours && XR_SUCCEEDED(result)) || (eye_state.app_profile_bound && XR_FAILED(result));
        }
    }
    log("eye gaze: the application suggested eye-gaze bindings, the layer's merged: %s (%d)\n", ours ? "yes" : "no", result);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_CreateSession(XrInstance instance, const XrSessionCreateInfo* info, XrSession* session) {
    const XrResult result = xrd.create_session(instance, info, session);
    if (XR_FAILED(result) || info == nullptr || session == nullptr) return result;
    std::lock_guard lock(eye_mutex);
    if (instance != eye_state.instance || eye_state.action == XR_NULL_HANDLE) return result;
    if (eye_state.session != XR_NULL_HANDLE) {
        log("eye gaze: another session while the first exists; the gaze stays with the first\n");
        return result;
    }
    eye_state.session = *session;
    eye_state.space = XR_NULL_HANDLE;
    eye_state.attached = eye_state.sync_off = false;
    eye_state.sync_failures = 0;
    XrSystemEyeGazeInteractionPropertiesEXT gaze_properties{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    properties.next = &gaze_properties;
    const bool supported = xrd.get_system_properties && XR_SUCCEEDED(xrd.get_system_properties(instance, info->systemId, &properties)) &&
        gaze_properties.supportsEyeGazeInteraction == XR_TRUE;
    XrResult space_result = XR_ERROR_FEATURE_UNSUPPORTED;
    if (supported) {
        XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        space_info.action = eye_state.action;
        space_info.poseInActionSpace.orientation.w = 1.0f;
        space_result = xrd.create_action_space(*session, &space_info, &eye_state.space);
        if (XR_FAILED(space_result)) eye_state.space = XR_NULL_HANDLE;
    }
    eye_state.absent_reason = !supported ? "the system does not support eye gaze interaction" :
        eye_state.space == XR_NULL_HANDLE ? "the gaze action space could not be created" : "the application has not attached its action sets";
    log("eye gaze: session created; system support %d, gaze space %d\n", supported ? 1 : 0, space_result);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_DestroySession(XrSession session) {
    {
        std::lock_guard lock(mutex);
        write_boost_stop_locked();
    }
    {
        std::lock_guard lock(eye_mutex);
        if (session == eye_state.session) {  // the space goes with the session; the binding stays with the instance
            eye_state.session = XR_NULL_HANDLE;
            eye_state.space = XR_NULL_HANDLE;
            eye_state.attached = eye_state.sync_off = false;
            eye_state.sync_failures = 0;
            eye_state.absent_reason = "no session";
        }
    }
    return xrd.destroy_session(session);
}

XRAPI_ATTR XrResult XRAPI_CALL hook_DestroyInstance(XrInstance instance) {
    // The PVR session, the sampler and the Quad Views export serve every live instance (DCS has one): they go only with
    // the last one. The loader may unload Quad Views with it (GetModuleHandle took no reference): resolved again later.
    if (live_instances.fetch_sub(1) == 1) {
        qv_gaze_fn.store(nullptr);
        {
            std::lock_guard lock(mutex);
            qv_checked_late = false;
            write_boost_stop_locked();
            fov_close_locked();
        }
        pvr_hook::restore();
    }
    {
        std::lock_guard lock(eye_mutex);
        if (instance == eye_state.instance) eye_state = EyeGazeState{};  // the action set goes with the instance
    }
    return xrd.destroy_instance(instance);
}

XRAPI_ATTR XrResult XRAPI_CALL hook_AttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* info) {
    XrActionSet ours{XR_NULL_HANDLE};
    {
        std::lock_guard lock(eye_mutex);
        if (info && session == eye_state.session && eye_state.space != XR_NULL_HANDLE && !eye_state.attached) {
            ensure_eye_binding_locked();
            if (eye_state.binding_submitted) ours = eye_state.action_set;
            else eye_state.absent_reason = eye_state.app_profile_bound ? "the application's eye-gaze bindings would be replaced"
                                                                        : "the gaze binding was refused";
        }
    }
    if (ours == XR_NULL_HANDLE) return xrd.attach_action_sets(session, info);
    std::vector<XrActionSet> sets;
    if (info->countActionSets && info->actionSets) sets.assign(info->actionSets, info->actionSets + info->countActionSets);
    if (std::find(sets.begin(), sets.end(), ours) == sets.end()) sets.push_back(ours);
    XrSessionActionSetsAttachInfo merged = *info;
    merged.countActionSets = static_cast<std::uint32_t>(sets.size());
    merged.actionSets = sets.data();
    XrResult result = xrd.attach_action_sets(session, &merged);
    const bool attached = XR_SUCCEEDED(result);
    if (!attached && result != XR_ERROR_ACTIONSETS_ALREADY_ATTACHED) {
        log("eye gaze: the application's xrAttachSessionActionSets failed with the gaze action set (%d); made without it\n", result);
        result = xrd.attach_action_sets(session, info);
    }
    {
        std::lock_guard lock(eye_mutex);
        if (session == eye_state.session) {
            eye_state.attached = attached;
            eye_state.absent_reason = attached ? "attached" : "the gaze action set could not be attached";
        }
    }
    log("eye gaze: attached with the application's %u action set(s): %s (%d)\n", info->countActionSets, attached ? "yes" : "no", result);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_SyncActions(XrSession session, const XrActionsSyncInfo* info) {
    XrActionSet ours{XR_NULL_HANDLE};
    unsigned failures{};
    {
        std::lock_guard lock(eye_mutex);
        if (info && session == eye_state.session && eye_state.attached && !eye_state.sync_off) { ours = eye_state.action_set; failures = eye_state.sync_failures; }
    }
    if (ours == XR_NULL_HANDLE) return xrd.sync_actions(session, info);
    std::vector<XrActiveActionSet> active;
    if (info->countActiveActionSets && info->activeActionSets)
        active.assign(info->activeActionSets, info->activeActionSets + info->countActiveActionSets);
    if (std::any_of(active.begin(), active.end(), [ours](const XrActiveActionSet& a) { return a.actionSet == ours; }))
        return xrd.sync_actions(session, info);
    active.push_back({ours, XR_NULL_PATH});
    XrActionsSyncInfo merged = *info;
    merged.countActiveActionSets = static_cast<std::uint32_t>(active.size());
    merged.activeActionSets = active.data();
    const XrResult result = xrd.sync_actions(session, &merged);
    if (XR_SUCCEEDED(result)) {
        if (failures) { std::lock_guard lock(eye_mutex); eye_state.sync_failures = 0; }
        return result;
    }
    // The application's own sync, as it was: its input never depends on the gaze action.
    const XrResult own = xrd.sync_actions(session, info);
    std::lock_guard lock(eye_mutex);
    if (session == eye_state.session && ++eye_state.sync_failures >= 3 && !eye_state.sync_off) {
        eye_state.sync_off = true;
        eye_state.absent_reason = "the application's xrSyncActions kept failing with the gaze action set";
    }
    if (eye_state.sync_failures <= 3) log("eye gaze: xrSyncActions failed with the gaze action set (%d); made without it (%d)\n", result, own);
    return own;
}

// The OpenXR eye gaze of this display time: located in the base space of xrLocateViews (info->space, at its display
// time), then turned into each primary view's frame. present: the source exists (the runtime then says when there is no
// gaze); valid: ORIENTATION_VALID and ORIENTATION_TRACKED. absent: why there is no source.
XrEyeGaze locate_eye_gaze(XrSession session, const XrViewLocateInfo* info, const XrView* views, const char*& absent) {
    XrEyeGaze gaze;
    XrSpace space{XR_NULL_HANDLE};
    {
        std::lock_guard lock(eye_mutex);
        if (session == eye_state.session && eye_state.space != XR_NULL_HANDLE && eye_state.attached && !eye_state.sync_off) space = eye_state.space;
        absent = eye_state.session == XR_NULL_HANDLE || session == eye_state.session ? eye_state.absent_reason : "another session has the gaze";
    }
    if (space == XR_NULL_HANDLE || !xrd.locate_space) return gaze;
    gaze.present = true;
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    const XrResult r = xrd.locate_space(space, info->space, info->displayTime, &location);
    if (XR_FAILED(r)) {
        static std::atomic<unsigned> logged{};
        if (logged.fetch_add(1) < 3) log("eye gaze: xrLocateSpace failed (%d)\n", r);
        return gaze;
    }
    constexpr XrSpaceLocationFlags tracked = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    if ((location.locationFlags & tracked) != tracked) return gaze;
    Quat q = to_quat(location.pose.orientation);
    const float norm = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!std::isfinite(norm) || norm < 1e-4f) return gaze;
    q = {q.x / norm, q.y / norm, q.z / norm, q.w / norm};
    const Vec3 dir = rotate(q, {0, 0, -1});  // the gaze ray looks down -Z of its pose
    for (int e = 0; e < 2; ++e) gaze.local[e] = normalized(rotate(conjugate(to_quat(views[e].pose.orientation)), dir));
    gaze.valid = true;
    return gaze;
}

// Logs when the OpenXR eye gaze becomes available or goes away, and once a warning when the focus views are read
// because there is neither the Quad Views export nor the OpenXR eye gaze.
void log_gaze_source_locked(GazeSource source, bool xr_present, const char* xr_absent) {
    if (xr_present != xr_was_present) {
        xr_was_present = xr_present;
        if (xr_present) {
            log("gaze: OpenXR eye gaze in use (source xr; a gaze that is not valid and tracked is no gaze)\n");
            focus_warned = false;
        } else {
            log("gaze: OpenXR eye gaze no longer available (%s)\n", xr_absent ? xr_absent : "unknown");
        }
    }
    if (source == GazeSource::focus && !focus_warned) {
        focus_warned = true;
        log("gaze: WARNING reading the gaze from the focus views' centres: no Quad Views export and no OpenXR eye gaze (%s). "
            "Without a real gaze the runtime's fixed focus reads as one (Pimax: about 2.4 deg outwards per eye)\n",
            xr_absent ? xr_absent : "unknown");
    }
}

XRAPI_ATTR XrResult XRAPI_CALL hook_LocateViews(XrSession session, const XrViewLocateInfo* info, XrViewState* state,
    uint32_t capacity, uint32_t* count, XrView* views) {
    const XrResult result = next_locate_views(session, info, state, capacity, count, views);
    if (XR_FAILED(result) || views == nullptr || capacity == 0 || count == nullptr || *count < 2 || *count > 4 || info == nullptr) return result;
    if (!(state->viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) || !(state->viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        // No eye distance is moved on this frame (tracking lost): the engine is told so (scale 1), instead of carrying the
        // previous frame's shift onto a cockpit DCS now renders at the runtime's own eye distance.
        std::lock_guard lock(mutex);
        if (boost_last_base > 0.001f) write_boost_out_locked(info->displayTime, boost_last_base, boost_last_base);
        return result;
    }

    // The OpenXR eye gaze for this display time, in the views' base space (a runtime call: outside the main lock).
    const char* xr_absent{};
    const XrEyeGaze xr_gaze = locate_eye_gaze(session, info, views, xr_absent);
    // Pimax PVR per-eye gaze for Depth boost (runtime calls, outside the main lock).
    pvr::Sample pvr_sample;
    // The predictive transition's high-rate stream: the sampler runs while wanted; its samples since the last frame, and
    // the tracker's clock now (the samples' time base).
    thread_local std::vector<pvr_hook::RawSample> stream;
    stream.clear();
    double now_s = 0;
    if (pvr_wanted.load()) pvr_hook::try_attach();
    pvr_hook::poll_ensure(pvr_wanted.load() && predict_wanted.load(), predict_poll_hz.load());
    // Once per display time: DCS may locate the same one again, and a second call would take this frame's samples and
    // the sample's freshness away from the next display time (Depth boost keeps the first call's scale anyway).
    XrTime sampled = pvr_sampled_time.load();
    const bool new_time = info->displayTime > sampled && pvr_sampled_time.compare_exchange_strong(sampled, info->displayTime);
    if (new_time && pvr_hook::polling.load()) { pvr_hook::drain(stream); now_s = pvr_hook::now_seconds(); }
    if (new_time && pvr_wanted.load()) pvr_sample = pvr_hook::sample();

    std::lock_guard lock(mutex);
    reload_settings_locked(false);
    poll_hotkeys_locked();
    const std::uint32_t n = *count;
    const bool quad = info->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO && n == 4;
    if (!qv_checked_late) { qv_checked_late = true; if (!qv_gaze_fn.load()) resolve_quad_views_locked(true); }

    Frame frame{};
    frame.time = info->displayTime;
    frame.count = n;
    for (std::uint32_t i = 0; i < n; ++i) frame.original[i] = views[i];

    Vec3 gaze_log{};
    bool have_gaze{};
    float shift_mm{};
    const Quat q_left = to_quat(views[0].pose.orientation), q_right = to_quat(views[1].pose.orientation);
    const Vec3 rest_world = head_forward(q_left, q_right);
    // The gaze, before anything moves: Quad Views' export for this display time (it has just located it, below this
    // layer), else the focus views' centres in each primary view's frame.
    std::uint32_t qv_flags{};
    float qv_left[3]{0, 0, -1}, qv_right[3]{0, 0, -1};
    if (const QuadViewsGazeFn qv = qv_gaze_fn.load()) qv_flags = qv(info->displayTime, qv_left, qv_right);
    if ((qv_flags & qv_gaze::valid) && std::isfinite(qv_left[0] + qv_left[1] + qv_left[2]) && qv_left[2] != 0.0f) {
        // For the statistics: where Quad Views says the left eye looks (refused ones are logged).
        const float x = std::atan2(qv_left[0], -qv_left[2]) * 57.29578f, y = std::atan2(qv_left[1], -qv_left[2]) * 57.29578f;
        const Vec3 l{qv_left[0], qv_left[1], qv_left[2]}, r{qv_right[0], qv_right[1], qv_right[2]};
        EyeGazes probe;
        probe.source = GazeSource::qv;
        probe.local[0] = l;
        probe.local[1] = r;
        if (plausible_gaze(probe).source == GazeSource::none) { stats.qv_refused_x = x; stats.qv_refused_y = y; }
    }
    Vec3 focus_local[2]{};
    if (quad)
        for (std::uint32_t eye = 0; eye < 2; ++eye)
            focus_local[eye] = rotate(conjugate(to_quat(views[eye].pose.orientation)),
                rotate(to_quat(views[eye + 2].pose.orientation), gaze_from_focus_fov(to_fov(views[eye + 2].fov))));
    const Vec3 simulated{std::tan(settings.simulate_x_deg / 57.2958f), std::tan(settings.simulate_y_deg / 57.2958f), -1};
    const EyeGazes eye_gaze = plausible_gaze(select_gaze(settings.simulate, simulated, qv_flags, qv_left, qv_right, quad, focus_local, xr_gaze));
    log_gaze_source_locked(eye_gaze.source, xr_gaze.present, xr_absent);
    // The eye distance: DCS's custom IPD (Ipd=; DCS replaces the eyes' sideways position after this layer, so its
    // option is off and the layer applies the same value) or the runtime's own, times Depth boost's scale. Applied
    // whatever the pupil shift's switch says: that switch compares the pupil shift only, at the same IPD.
    const float runtime_ipd = length(to_vec(views[1].pose.position) - to_vec(views[0].pose.position));
    const float base_ipd = settings.ipd > 0 ? settings.ipd : runtime_ipd;
    update_depth_locked(views, n, plausible_gaze(select_gaze(false, {}, qv_flags, qv_left, qv_right, quad, focus_local, xr_gaze)), info->displayTime, base_ipd, pvr_sample,
        stream, now_s);
    const float scale = depth.scale();
    bool moved_ipd{};
    if ((settings.enabled || depth_settings.enabled) && (settings.ipd > 0 || scale != 1.0f)) {
        Vec3 move[2];
        ipd_moves(to_vec(views[0].pose.position), to_vec(views[1].pose.position), base_ipd * scale, move[0], move[1]);
        for (std::uint32_t v = 0; v < n; ++v) {
            views[v].pose.position.x += move[v % 2].x;
            views[v].pose.position.y += move[v % 2].y;
            views[v].pose.position.z += move[v % 2].z;
        }
        moved_ipd = true;
    }
    // The eye distance DCS renders with now (before the pupil's own sideways shift), for the panel.
    const float rendered_ipd_now = length(to_vec(views[1].pose.position) - to_vec(views[0].pose.position));
    // No gaze (Quad Views' fixed focus, a focus FOV that is not finite, no quad views): the eyes stay at rest, after the
    // filter's short hold (a blink).
    float pupil_max_mm = 0;
    const float pupil_dt = pupil_time && info->displayTime > pupil_time ? static_cast<float>(static_cast<double>(info->displayTime - pupil_time) * 1e-9) : 0.0f;
    if (info->displayTime > pupil_time) pupil_time = info->displayTime;
    const bool simulated_gaze = eye_gaze.source == GazeSource::simulated;
    const Vec3* pupil_gaze = nullptr;
    if (!pupil_active() || simulated_gaze) {
        // Off, or a simulated gaze (exact): no filter; it starts over when a real gaze comes.
        pupil_filter.reset();
        pupil_lock = PupilLock{};
        pupil_filtered_valid = false;
        if (pupil_active()) pupil_gaze = eye_gaze.local;
    } else {
        // The eyes' resting gaze, not the tracker's noise (PupilGazeFilter), and the pupil moved only during saccades
        // (PupilLock); once per display time.
        if (pupil_dt > 0 || !pupil_filtered_valid) {
            const Vec3* raw = eye_gaze.source != GazeSource::none ? eye_gaze.local : nullptr;
            const bool filtered = pupil_filter.update(raw, std::min(pupil_dt, 0.1f), settings.smooth_s, pupil_filtered);
            Vec3 rest_eye[2];
            for (std::uint32_t eye = 0; eye < 2; ++eye) rest_eye[eye] = rotate(conjugate(to_quat(views[eye].pose.orientation)), rest_world);
            pupil_filtered_valid = pupil_lock.update(raw, filtered, pupil_filtered, rest_eye, pupil_filter.noise_deg(), std::min(pupil_dt, 0.1f), pupil_locked);
        }
        if (pupil_filtered_valid) pupil_gaze = pupil_locked;
    }
    if (pupil_gaze) {
        for (std::uint32_t eye = 0; eye < 2; ++eye) {
            const Quat qp = to_quat(views[eye].pose.orientation);
            // Straight ahead in this eye's frame: the head's forward, not the (possibly canted) view axis.
            const Vec3 rest = rotate(conjugate(qp), rest_world);
            const Vec3 gaze = clamp_gaze(pupil_gaze[eye], rest, settings.max_gaze_deg);
            const Vec3 offset = pupil_offset(gaze, rest, settings.eye_radius);
            const Vec3 world = rotate(qp, offset);
            for (std::uint32_t v = eye; v < n; v += 2) {
                const Quat qv = to_quat(views[v].pose.orientation);
                const Fov f = shifted_fov(to_fov(views[v].fov), rotate(conjugate(qv), world), settings.virtual_image);
                views[v].pose.position.x += world.x;
                views[v].pose.position.y += world.y;
                views[v].pose.position.z += world.z;
                views[v].fov = {f.left, f.right, f.up, f.down};
            }
            if (eye == 0) {
                // The stats line's gaze range is the tracker's raw gaze (its noise stays visible in the log).
                gaze_log = eye_gaze.source != GazeSource::none ? clamp_gaze(eye_gaze.local[0], rest, settings.max_gaze_deg) : gaze;
                shift_mm = length(offset) * 1000;
            }
            pupil_max_mm = std::max(pupil_max_mm, length(offset) * 1000);
        }
        have_gaze = true;
    }
    {
        const auto hundredths = [](float mm) { return std::isfinite(mm) ? static_cast<std::uint64_t>(std::clamp(mm * 100.0f + 0.5f, 0.0f, 65535.0f)) : 0ULL; };
        ipd_word.store(hundredths(base_ipd * 1000) | hundredths(rendered_ipd_now * 1000) << 16 | hundredths(pupil_max_mm) << 32 |
            (settings.ipd > 0 ? 1ULL << 49 : 1ULL << 48), std::memory_order_release);
    }
    for (std::uint32_t i = 0; i < n; ++i) frame.shifted[i] = views[i];
    account_locked(frame.original.data(), n, have_gaze ? &gaze_log : nullptr, shift_mm, cant_deg(q_left, q_right),
        length(to_vec(views[1].pose.position) - to_vec(views[0].pose.position)) * 1000, eye_gaze.source, qv_flags, xr_gaze);
    if (have_gaze || moved_ipd) {
        frames.push_back(frame);
        while (frames.size() > 8) frames.pop_front();
    }
    write_gaze_out_locked(info->displayTime, views, n, simulated, eye_gaze);
    write_boost_out_locked(info->displayTime, base_ipd, rendered_ipd_now);
    write_fov_located_locked(info->displayTime, frame.original.data(), views, n,
        (have_gaze ? dcsvr_pupil_fov::entry_flags::pupil : 0u) | (moved_ipd ? dcsvr_pupil_fov::entry_flags::ipd : 0u));
    return result;
}

bool matches(const XrView& a, const XrPosef& pose, const XrFovf& fov) {
    // NaN equals NaN here: a view the runtime gave with a NaN FOV is still recognised (its pose may carry the IPD move).
    auto within = [](float x, float y, float tol) { return std::fabs(x - y) <= tol || (std::isnan(x) && std::isnan(y)); };
    return within(a.pose.position.x, pose.position.x, 1e-4f) && within(a.pose.position.y, pose.position.y, 1e-4f) &&
        within(a.pose.position.z, pose.position.z, 1e-4f) && within(a.fov.angleLeft, fov.angleLeft, 1e-4f) &&
        within(a.fov.angleRight, fov.angleRight, 1e-4f) && within(a.fov.angleUp, fov.angleUp, 1e-4f) &&
        within(a.fov.angleDown, fov.angleDown, 1e-4f);
}

XRAPI_ATTR XrResult XRAPI_CALL hook_EndFrame(XrSession session, const XrFrameEndInfo* info) {
    if (info == nullptr || info->layerCount == 0) return next_end_frame(session, info);
    std::vector<XrCompositionLayerProjection> projections;
    std::vector<std::vector<XrCompositionLayerProjectionView>> view_copies;
    std::vector<const XrCompositionLayerBaseHeader*> layers(info->layers, info->layers + info->layerCount);
    unsigned restored{};
    bool passthrough{};
    {
        std::lock_guard lock(mutex);
        // DCS may locate views more than once for one display time: every shift handed out for it is a candidate.
        std::vector<const Frame*> candidates;
        for (auto it = frames.rbegin(); it != frames.rend(); ++it)
            if (it->time == info->displayTime) candidates.push_back(&*it);
        passthrough = candidates.empty();
        if (!passthrough) {
        projections.reserve(layers.size());
        view_copies.reserve(layers.size());
        bool fov_written{};
        for (auto& layer : layers) {
            if (layer == nullptr || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
            const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            view_copies.emplace_back(projection->views, projection->views + projection->viewCount);
            const Frame* matched[4]{};
            for (std::uint32_t v = 0; v < projection->viewCount; ++v) {
                auto& view = view_copies.back()[v];
                // Only views that still carry what this layer handed out are put back.
                for (const Frame* frame : candidates) {
                    if (v >= frame->count || !matches(frame->shifted[v], view.pose, view.fov)) continue;
                    view.pose = frame->original[v].pose;
                    view.fov = frame->original[v].fov;
                    if (v < 4) matched[v] = frame;
                    ++restored;
                    break;
                }
            }
            // Local\DcsVrPupilFov_v1: the pairs of the first projection layer, as DCS submitted it.
            if (!fov_written && projection->views) {
                fov_written = true;
                write_fov_submitted_locked(info->displayTime, projection->views, projection->viewCount, matched);
            }
            projections.push_back(*projection);
            projections.back().views = view_copies.back().data();
            layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections.back());
        }
        }
    }
    // The runtime's xrEndFrame (and every layer below) runs outside the lock, so xrLocateViews never waits on it.
    if (passthrough) return next_end_frame(session, info);
    static unsigned logged{};
    if (logged < 3) { ++logged; log("end_frame: restored %u projection views\n", restored); }
    restored_views.fetch_add(restored, std::memory_order_relaxed);
    XrFrameEndInfo forwarded = *info;
    forwarded.layers = layers.data();
    return next_end_frame(session, &forwarded);
}

XRAPI_ATTR XrResult XRAPI_CALL hook_GetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (next_gipa == nullptr) return XR_ERROR_HANDLE_INVALID;
    const XrResult result = next_gipa(instance, name, function);
    if (XR_FAILED(result) || name == nullptr) return result;
    if (*function == nullptr) return result;
#define DCSVR_HOOK(fn, next, hook) \
    else if (std::strcmp(name, fn) == 0) { next = reinterpret_cast<decltype(next)>(*function); *function = reinterpret_cast<PFN_xrVoidFunction>(hook); }
    if (std::strcmp(name, "xrLocateViews") == 0) {
        next_locate_views = reinterpret_cast<PFN_xrLocateViews>(*function);
        *function = reinterpret_cast<PFN_xrVoidFunction>(hook_LocateViews);
    }
    DCSVR_HOOK("xrEndFrame", next_end_frame, hook_EndFrame)
    // The OpenXR eye gaze: the application's own calls, with the layer's action merged in.
    DCSVR_HOOK("xrSuggestInteractionProfileBindings", xrd.suggest_bindings, hook_SuggestInteractionProfileBindings)
    DCSVR_HOOK("xrAttachSessionActionSets", xrd.attach_action_sets, hook_AttachSessionActionSets)
    DCSVR_HOOK("xrSyncActions", xrd.sync_actions, hook_SyncActions)
    DCSVR_HOOK("xrCreateSession", xrd.create_session, hook_CreateSession)
    DCSVR_HOOK("xrDestroySession", xrd.destroy_session, hook_DestroySession)
    DCSVR_HOOK("xrDestroyInstance", xrd.destroy_instance, hook_DestroyInstance)
#undef DCSVR_HOOK
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL hook_CreateApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layer_info,
    XrInstance* instance) {
    if (info == nullptr || layer_info == nullptr || layer_info->nextInfo == nullptr ||
        layer_info->nextInfo->nextGetInstanceProcAddr == nullptr || layer_info->nextInfo->nextCreateApiLayerInstance == nullptr)
        return XR_ERROR_INITIALIZATION_FAILED;
    next_gipa = layer_info->nextInfo->nextGetInstanceProcAddr;
    XrApiLayerCreateInfo next_info = *layer_info;
    next_info.nextInfo = layer_info->nextInfo->next;
    // XR_EXT_eye_gaze_interaction: wanted unless the bundled Quad Views' export is loaded (it has the gaze and enables
    // the extension itself, below this layer) or PupilShift.ini says OpenXrEyeGaze=0. Added when the runtime offers it
    // (or cannot say); if the runtime refuses it, the instance is created as the application asked.
    const HMODULE qv_module = GetModuleHandleW(qv_gaze::module_name);
    const bool qv_export = qv_module && GetProcAddress(qv_module, qv_gaze::export_name);
    const bool wanted = !qv_export && ini_float(L"OpenXrEyeGaze", 1, module_dir + L"PupilShift.ini") != 0;
    bool app_enabled{};
    for (std::uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        app_enabled |= std::strcmp(info->enabledExtensionNames[i], XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0;
    const Availability availability = app_enabled ? Availability::present : wanted ? probe_eye_gaze(next_gipa) : Availability::absent;
    bool inject = wanted && !app_enabled && availability != Availability::absent;
    std::vector<const char*> extensions;
    if (info->enabledExtensionCount && info->enabledExtensionNames)
        extensions.assign(info->enabledExtensionNames, info->enabledExtensionNames + info->enabledExtensionCount);
    if (inject) extensions.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    XrInstanceCreateInfo forwarded = *info;
    forwarded.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    forwarded.enabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    XrResult result = layer_info->nextInfo->nextCreateApiLayerInstance(&forwarded, &next_info, instance);
    bool refused{};
    if (inject && result == XR_ERROR_EXTENSION_NOT_PRESENT) {
        inject = false; refused = true;
        result = layer_info->nextInfo->nextCreateApiLayerInstance(info, &next_info, instance);
    }
    if (XR_SUCCEEDED(result)) {
        PFN_xrVoidFunction f{};
        if (XR_SUCCEEDED(next_gipa(*instance, "xrLocateViews", &f))) next_locate_views = reinterpret_cast<PFN_xrLocateViews>(f);
        if (XR_SUCCEEDED(next_gipa(*instance, "xrEndFrame", &f))) next_end_frame = reinterpret_cast<PFN_xrEndFrame>(f);
        {
            std::lock_guard lock(mutex);
            log("instance created for %s\n", info->applicationInfo.applicationName);
            live_instances.fetch_add(1);
            pvr_sampled_time.store(0);  // XrTime is comparable only within one instance
            reload_settings_locked(true);
            resolve_quad_views_locked();
            qv_checked_late = false;
            // Depth boost starts afresh with each instance; the shared-memory names may carry the harness's suffix.
            fixation = FixationEstimator{}; decider = FixationDecider{}; matcher = DepthMatcher{}; depth = DepthBoostController{};
            depth_time = gaze_out_time = boost_out_time = 0; display_frames = 0; depth_gaze = DepthGaze{};
            pupil_filter = PupilGazeFilter{};
            pupil_lock = PupilLock{};
            pupil_filtered_valid = false;
            pupil_time = 0;
            last_fresh_scene_frame = 0;
            detector = predictive::SaccadeDetector{}; gate = predictive::PredictiveGate{}; have_grid = false; grid_seq = grid_arrival = 0;
            pose_ring = {}; pose_next = 0; predict_effective = false; predict_logged_init = false; predict_second = PredictSecond{};
            // Blocks still open from the previous instance hold that session's last records: not new for this one.
            {
                dcsvr_perception::DepthIn in{};
                if (depth_block.get() && dcsvr_perception::seqlock_read(depth_block.get(), in)) matcher.seed(in.seq);
                static dcsvr_perception::DepthGrid old_grid{};
                if (grid_block.get() && dcsvr_perception::seqlock_read(grid_block.get(), old_grid)) grid_seq = old_grid.seq;
            }
            update_predict_wanted_locked();
            if (!GetEnvironmentVariableW(L"DCSVR_PERCEPTION_SHM_SUFFIX", shm_suffix, 64) || shm_suffix[0] == 0) shm_suffix[0] = 0;
            // The engine's dual camera opens Local\DcsVrDepthBoost_v1 to read: created here, at instance creation, whenever
            // Depth boost is enabled (write_boost_out_locked retries about once a second if this fails).
            if (depth_settings.enabled && !boost_block.get()) {
                const bool created = boost_block.open(dcsvr_perception::boost_block_name, shm_suffix);
                log("perception: shared memory %ls%ls %s\n", dcsvr_perception::boost_block_name, shm_suffix, created ? "created" : "FAILED");
            }
            // Local\DcsVrPupilFov_v1: whenever the layer runs (enabled says whether it shifts views).
            fov_open_locked();
        }
        log("eye gaze: XR_EXT_eye_gaze_interaction %s (runtime %s)\n",
            app_enabled ? "enabled by the application" : inject ? "added by the layer" : refused ? "refused by the runtime" : "not enabled",
            availability == Availability::present ? "offers it" : availability == Availability::unknown ? "did not say" : "does not offer it, or not asked");
        setup_eye_gaze(*instance, wanted, app_enabled || inject, !wanted ? (qv_export ? "the bundled Quad Views' export has the gaze" :
            "OpenXrEyeGaze=0 in PupilShift.ini") : refused ? "the runtime refused XR_EXT_eye_gaze_interaction" :
            !(app_enabled || inject) ? "the runtime does not offer XR_EXT_eye_gaze_interaction" : nullptr);
    }
    return result;
}

}  // namespace

// For the in-headset diagnostic panel (OFXR), which finds it with GetModuleHandle/GetProcAddress.
extern "C" __declspec(dllexport) unsigned long long PupilShift_Status() { return status_word.load(std::memory_order_acquire); }
// The eye distance and pupil for the panel's lines ("Depth boost x1.18 · IPD 77.4 mm", "Pupil shift ON 1.2 mm"): see
// ipd_word.
extern "C" __declspec(dllexport) unsigned long long PupilShift_Ipd() noexcept { return ipd_word.load(std::memory_order_acquire); }

extern "C" __declspec(dllexport) XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loader_info, const char* requested_layer_name, XrNegotiateApiLayerRequest* request) {
    if (loader_info == nullptr || request == nullptr || loader_info->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        loader_info->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader_info->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION ||
        requested_layer_name == nullptr || std::strcmp(requested_layer_name, layer_name) != 0)
        return XR_ERROR_INITIALIZATION_FAILED;
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = (std::min)(loader_info->maxApiVersion, XR_CURRENT_API_VERSION);
    request->getInstanceProcAddr = hook_GetInstanceProcAddr;
    request->createApiLayerInstance = hook_CreateApiLayerInstance;
    return XR_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(module, path, MAX_PATH);
        module_dir = path;
        module_dir = module_dir.substr(0, module_dir.find_last_of(L"\\/") + 1);
        // Shared for reading: the app shows the layer's status from this log while DCS runs.
        log_file = _wfsopen((module_dir + L"PupilShift.log").c_str(), L"w", _SH_DENYWR);
        log("%s loaded\n", layer_name);
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {  // at process exit the CRT is going away too
        gaze_block.close();
        boost_block.close();
        fov_block.close();
        stereo_block.close();
        depth_block.close();
        grid_block.close();
        if (log_file) std::fclose(log_file);
    }
    return TRUE;
}
