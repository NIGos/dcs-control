// XR_APILAYER_DCSVR_pupil_shift: renders each eye from where its pupil actually is.
//
// xrLocateViews: the gaze is read from the focus views of a foveated quad-view configuration
// (their FOV is centred on the gaze), each eye's pose is moved to the pupil position for that
// gaze, and its FOV is shifted so the lens's virtual image plane stays where it was.
// xrEndFrame: the original poses and FOVs are put back on the submitted projection views, so the
// runtime places the image exactly where it would have without the layer; only the content
// changes. Settings live in PupilShift.ini next to the DLL and are re-read while running.
// PupilShift_Status() reports the state to the in-headset diagnostic panel (OFXR).
#define XR_USE_PLATFORM_WIN32
#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "pupil_math.hpp"

namespace {

using namespace pupil_shift;
constexpr const char* layer_name = "XR_APILAYER_DCSVR_pupil_shift";

PFN_xrGetInstanceProcAddr next_gipa{};
PFN_xrLocateViews next_locate_views{};
PFN_xrEndFrame next_end_frame{};

std::mutex mutex;
std::wstring module_dir;
Settings settings;
FILETIME ini_time{};
unsigned reload_counter{};
bool toggled_off{};
bool hotkey_down{};
// The in-flight switch, [PupilShift] Toggle=virtual-key:modifiers (1 Ctrl, 2 Alt, 4 Shift; 0:0 off),
// the same format as DCS Control's other in-flight keys. Default Ctrl+Shift+F10.
int toggle_vk{VK_F10};
int toggle_mods{1 | 4};
FILE* log_file{};
std::atomic<unsigned> restored_views{};
// PupilShift_Status(): bit 0 running (DCS asked for view poses), bit 1 correction on (enabled and not
// switched off in flight), bit 2 a gaze was applied during the last second, bit 3 the gaze is
// simulated (SimulateGaze), bit 4 the runtime's projection views were put back during the last
// second, bits 8-15 the largest pupil shift of the last second in 0.1 mm (saturating).
std::atomic<std::uint64_t> status_word{};

void log(const char* format, ...) {
    if (!log_file) return;
    SYSTEMTIME t; GetLocalTime(&t);
    std::fprintf(log_file, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args; va_start(args, format);
    std::vfprintf(log_file, format, args);
    va_end(args);
    std::fflush(log_file);
}

float ini_float(const wchar_t* key, float fallback, const std::wstring& path) {
    wchar_t text[64]{};
    GetPrivateProfileStringW(L"PupilShift", key, L"", text, 64, path.c_str());
    if (!text[0]) return fallback;
    wchar_t* end{};
    const float value = std::wcstof(text, &end);
    return end != text ? value : fallback;
}

// "virtual-key:modifiers"; false (keeps the previous key) for anything else.
bool ini_hotkey(const std::wstring& path, int& vk, int& mods) {
    wchar_t text[32]{};
    GetPrivateProfileStringW(L"PupilShift", L"Toggle", L"", text, 32, path.c_str());
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
    if (!ini_hotkey(path, toggle_vk, toggle_mods)) { toggle_vk = VK_F10; toggle_mods = 1 | 4; }
    Settings s;
    s.enabled = ini_float(L"Enabled", 1, path) != 0;
    s.eye_radius = std::clamp(ini_float(L"EyeRadiusMm", 10.5f, path), 0.0f, 30.0f) / 1000.0f;
    s.virtual_image = std::clamp(ini_float(L"VirtualImageM", 1.5f, path), 0.2f, 100.0f);
    s.max_gaze_deg = std::clamp(ini_float(L"MaxGazeDeg", 35.0f, path), 0.0f, 60.0f);
    s.simulate_x_deg = ini_float(L"SimulateGazeXDeg", 0, path);
    s.simulate_y_deg = ini_float(L"SimulateGazeYDeg", 0, path);
    s.simulate = ini_float(L"SimulateGaze", 0, path) != 0;
    settings = s;
    log("settings: enabled=%d eye_radius=%.1fmm virtual_image=%.2fm max_gaze=%.0fdeg simulate=%d (%.1f, %.1f) toggle=%d:%d\n",
        s.enabled, s.eye_radius * 1000, s.virtual_image, s.max_gaze_deg, s.simulate, s.simulate_x_deg, s.simulate_y_deg,
        toggle_vk, toggle_mods);
}

// The in-flight switch (default Ctrl+Shift+F10) turns the correction off and on, for A/B comparison
// in the headset. The modifiers must match exactly, so Ctrl+Alt+Shift+F10 is a different key.
void poll_hotkey_locked() {
    if (toggle_vk == 0) { hotkey_down = false; return; }
    const auto held = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    const int mods = (held(VK_CONTROL) ? 1 : 0) | (held(VK_MENU) ? 2 : 0) | (held(VK_SHIFT) ? 4 : 0);
    const bool down = held(toggle_vk) && mods == toggle_mods;
    if (down && !hotkey_down) {
        toggled_off = !toggled_off;
        log("hotkey: correction %s\n", toggled_off ? "OFF" : "ON");
        // Beep() blocks for its duration: off the render thread, so the switch itself causes no hitch.
        std::thread([off = toggled_off] { Beep(off ? 600 : 1200, 80); }).detach();
    }
    hotkey_down = down;
}

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
    float gaze_min_x{1e9f}, gaze_max_x{-1e9f}, gaze_min_y{1e9f}, gaze_max_y{-1e9f};
    float eye_min_x{1e9f}, eye_max_x{-1e9f}, eye_min_z{1e9f}, eye_max_z{-1e9f};
    float shift_max_mm{};
    ULONGLONG start{};
} stats;

void account_locked(const XrView* views, std::uint32_t count, const Vec3* gaze, float shift_mm) {
    if (!stats.start) stats.start = GetTickCount64();
    ++stats.frames;
    if (gaze) {
        ++stats.shifted;
        const float gx = std::atan2(gaze->x, -gaze->z) * 57.2958f, gy = std::atan2(gaze->y, -gaze->z) * 57.2958f;
        stats.gaze_min_x = std::min(stats.gaze_min_x, gx); stats.gaze_max_x = std::max(stats.gaze_max_x, gx);
        stats.gaze_min_y = std::min(stats.gaze_min_y, gy); stats.gaze_max_y = std::max(stats.gaze_max_y, gy);
        stats.shift_max_mm = std::max(stats.shift_max_mm, shift_mm);
    }
    if (count >= 2) {
        const Vec3 l = to_vec(views[0].pose.position), r = to_vec(views[1].pose.position);
        const Vec3 local = rotate(conjugate(to_quat(views[0].pose.orientation)), l - (l + r) * 0.5f);
        stats.eye_min_x = std::min(stats.eye_min_x, local.x); stats.eye_max_x = std::max(stats.eye_max_x, local.x);
        stats.eye_min_z = std::min(stats.eye_min_z, local.z); stats.eye_max_z = std::max(stats.eye_max_z, local.z);
    }
    if (GetTickCount64() - stats.start >= 1000) {
        const unsigned restored = restored_views.exchange(0);
        if (stats.shifted)
            log("stats: frames=%u shifted=%u restored=%u gaze x[%.1f..%.1f] y[%.1f..%.1f] deg, max shift %.2f mm; "
                "runtime left eye vs head x[%.2f..%.2f] z[%.2f..%.2f] mm\n",
                stats.frames, stats.shifted, restored, stats.gaze_min_x, stats.gaze_max_x, stats.gaze_min_y, stats.gaze_max_y,
                stats.shift_max_mm, stats.eye_min_x * 1000, stats.eye_max_x * 1000, stats.eye_min_z * 1000, stats.eye_max_z * 1000);
        else
            log("stats: frames=%u shifted=0 (%s); runtime left eye vs head x[%.2f..%.2f] mm\n",
                stats.frames, !settings.enabled || toggled_off ? "correction off" : "no gaze source", stats.eye_min_x * 1000, stats.eye_max_x * 1000);
        const std::uint64_t tenths = static_cast<std::uint64_t>(std::clamp(stats.shift_max_mm * 10.0f + 0.5f, 0.0f, 255.0f));
        status_word.store(1ULL | (settings.enabled && !toggled_off ? 2ULL : 0) | (stats.shifted ? 4ULL : 0) |
            (settings.simulate ? 8ULL : 0) | (restored ? 16ULL : 0) | (tenths << 8), std::memory_order_release);
        stats = Stats{};
    } else {
        // Between the once-a-second updates, the switch shows at once.
        auto word = status_word.load(std::memory_order_relaxed) | 1ULL;
        word = settings.enabled && !toggled_off ? (word | 2ULL) : (word & ~2ULL);
        status_word.store(word, std::memory_order_release);
    }
}

XRAPI_ATTR XrResult XRAPI_CALL hook_LocateViews(XrSession session, const XrViewLocateInfo* info, XrViewState* state,
    uint32_t capacity, uint32_t* count, XrView* views) {
    const XrResult result = next_locate_views(session, info, state, capacity, count, views);
    if (XR_FAILED(result) || views == nullptr || capacity == 0 || count == nullptr || *count < 2 || *count > 4 || info == nullptr) return result;
    if (!(state->viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) || !(state->viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return result;

    std::lock_guard lock(mutex);
    reload_settings_locked(false);
    poll_hotkey_locked();
    const std::uint32_t n = *count;
    const bool quad = info->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO && n == 4;

    Frame frame{};
    frame.time = info->displayTime;
    frame.count = n;
    for (std::uint32_t i = 0; i < n; ++i) frame.original[i] = views[i];

    Vec3 gaze_log{};
    bool have_gaze{};
    float shift_mm{};
    if (settings.enabled && !toggled_off && (quad || settings.simulate)) {
        for (std::uint32_t eye = 0; eye < 2; ++eye) {
            const Quat qp = to_quat(views[eye].pose.orientation);
            Vec3 gaze;
            if (settings.simulate) {
                gaze = Vec3{std::tan(settings.simulate_x_deg / 57.2958f), std::tan(settings.simulate_y_deg / 57.2958f), -1};
                gaze = gaze * (1.0f / length(gaze));
            } else {
                // The focus view's FOV is centred on the gaze; express it in the primary view's frame.
                const Vec3 local = gaze_from_focus_fov(to_fov(views[eye + 2].fov));
                gaze = rotate(conjugate(qp), rotate(to_quat(views[eye + 2].pose.orientation), local));
            }
            gaze = clamp_gaze(gaze, settings.max_gaze_deg);
            const Vec3 offset = pupil_offset(gaze, settings.eye_radius);
            const Vec3 world = rotate(qp, offset);
            for (std::uint32_t v = eye; v < n; v += 2) {
                const Quat qv = to_quat(views[v].pose.orientation);
                const Fov f = shifted_fov(to_fov(views[v].fov), rotate(conjugate(qv), world), settings.virtual_image);
                views[v].pose.position.x += world.x;
                views[v].pose.position.y += world.y;
                views[v].pose.position.z += world.z;
                views[v].fov = {f.left, f.right, f.up, f.down};
            }
            if (eye == 0) { gaze_log = gaze; shift_mm = length(offset) * 1000; }
        }
        have_gaze = true;
    }
    for (std::uint32_t i = 0; i < n; ++i) frame.shifted[i] = views[i];
    account_locked(frame.original.data(), n, have_gaze ? &gaze_log : nullptr, shift_mm);
    if (have_gaze) {
        frames.push_back(frame);
        while (frames.size() > 8) frames.pop_front();
    }
    return result;
}

bool matches(const XrView& a, const XrPosef& pose, const XrFovf& fov) {
    auto within = [](float x, float y, float tol) { return std::fabs(x - y) <= tol; };
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
        for (auto& layer : layers) {
            if (layer == nullptr || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
            const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            view_copies.emplace_back(projection->views, projection->views + projection->viewCount);
            for (std::uint32_t v = 0; v < projection->viewCount; ++v) {
                auto& view = view_copies.back()[v];
                // Only views that still carry what this layer handed out are put back.
                for (const Frame* frame : candidates) {
                    if (v >= frame->count || !matches(frame->shifted[v], view.pose, view.fov)) continue;
                    view.pose = frame->original[v].pose;
                    view.fov = frame->original[v].fov;
                    ++restored;
                    break;
                }
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
    const XrResult result = next_gipa(instance, name, function);
    if (XR_FAILED(result) || name == nullptr) return result;
    if (std::strcmp(name, "xrLocateViews") == 0) {
        next_locate_views = reinterpret_cast<PFN_xrLocateViews>(*function);
        *function = reinterpret_cast<PFN_xrVoidFunction>(hook_LocateViews);
    } else if (std::strcmp(name, "xrEndFrame") == 0) {
        next_end_frame = reinterpret_cast<PFN_xrEndFrame>(*function);
        *function = reinterpret_cast<PFN_xrVoidFunction>(hook_EndFrame);
    }
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
    const XrResult result = layer_info->nextInfo->nextCreateApiLayerInstance(info, &next_info, instance);
    if (XR_SUCCEEDED(result)) {
        PFN_xrVoidFunction f{};
        if (XR_SUCCEEDED(next_gipa(*instance, "xrLocateViews", &f))) next_locate_views = reinterpret_cast<PFN_xrLocateViews>(f);
        if (XR_SUCCEEDED(next_gipa(*instance, "xrEndFrame", &f))) next_end_frame = reinterpret_cast<PFN_xrEndFrame>(f);
        std::lock_guard lock(mutex);
        log("instance created for %s\n", info->applicationInfo.applicationName);
        reload_settings_locked(true);
    }
    return result;
}

}  // namespace

// For the in-headset diagnostic panel (OFXR), which finds it with GetModuleHandle/GetProcAddress.
extern "C" __declspec(dllexport) unsigned long long PupilShift_Status() { return status_word.load(std::memory_order_acquire); }

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

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(module, path, MAX_PATH);
        module_dir = path;
        module_dir = module_dir.substr(0, module_dir.find_last_of(L"\\/") + 1);
        _wfopen_s(&log_file, (module_dir + L"PupilShift.log").c_str(), L"w");
        log("%s loaded\n", layer_name);
    } else if (reason == DLL_PROCESS_DETACH && log_file) {
        std::fclose(log_file);
    }
    return TRUE;
}
