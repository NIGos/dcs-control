"""Apply the opt-in DCS focus-pair adaptation to the pinned Cheeky checkout.

Reset-then-apply, like the OFXR fork's patches: the patches only ever see a pristine tree at the pinned revision. A
marker in the checkout's git folder records the hash of this script and patches/cheeky; while it matches, the tree is
already patched and nothing runs. Otherwise every tracked file is restored, untracked files are removed (ignored build
output stays), and the whole adaptation is applied again. Nothing is ever fetched.
"""
from pathlib import Path
import hashlib
import re
import shutil
import subprocess

root = Path(__file__).resolve().parent.parent
source = root / 'external/cheeky'
pinned_revision = 'd6c18b2'

def git(*arguments):
    return subprocess.run(['git', '-C', str(source), *arguments], check=True, capture_output=True, text=True).stdout.strip()

def patch_hash():
    digest = hashlib.sha256(Path(__file__).resolve().read_bytes())
    patches = root / 'patches/cheeky'
    for path in sorted(p for p in patches.rglob('*') if p.is_file()):
        digest.update(b'\0' + path.relative_to(patches).as_posix().encode('utf-8') + b'\0' + path.read_bytes())
    return digest.hexdigest()

if not git('rev-parse', 'HEAD').startswith(pinned_revision):
    raise RuntimeError(f'external/cheeky is not at the pinned revision {pinned_revision}; check it out first (never fetched here)')
marker = source / git('rev-parse', '--git-dir') / 'dcs-control-patches'  # an absolute git dir replaces source
wanted = patch_hash()
if marker.is_file() and marker.read_text(encoding='ascii').strip() == wanted:
    print('Cheeky patches unchanged since they were applied; skipped.')
    raise SystemExit(0)
# A failed apply leaves no marker, so the next run resets again.
marker.unlink(missing_ok=True)
git('checkout', '--', '.')
git('clean', '-fdq')

def replace(relative, before, after, count=1):
    """Replaces exactly `count` anchors; a pristine tree is the only valid input."""
    path = source / relative
    value = path.read_text(encoding='utf-8')
    actual = value.count(before)
    if actual != count:
        raise RuntimeError(f'{relative}: expected {count} anchors, found {actual}')
    path.write_text(value.replace(before, after), encoding='utf-8', newline='\n')

shutil.copyfile(root / 'patches/cheeky/dcs_quad_focus_policy.hpp', source / 'src/dcs_quad_focus_policy.hpp')
shutil.copyfile(root / 'patches/cheeky/dcs_quad_focus_gate.inc', source / 'src/dcs_quad_focus_gate.inc')
replace('openxr_layer/projection_selection.hpp', '    bool unsupported{};', '    bool unsupported{};\n    unsigned first_view{};')
replace('openxr_layer/projection_selection.hpp', 'ProjectionSelection select_projection(const XrFrameEndInfo* info, IsDummySwapchain is_dummy_swapchain)', 'ProjectionSelection select_projection(const XrFrameEndInfo* info, IsDummySwapchain is_dummy_swapchain, bool quad_focus = false)')
replace('openxr_layer/projection_selection.hpp', 'if (projection->viewCount != 2 || !projection->views)', 'if ((projection->viewCount != 2 && !(quad_focus && projection->viewCount == 4)) || !projection->views)')
replace('openxr_layer/projection_selection.hpp', '        selected.projection = projection;', '        selected.projection = projection;\n        selected.first_view = projection->viewCount == 4 ? 2U : 0U;')

helper = '''// DCS VR Control: opt-in focus-pair adapter; stereo behavior stays upstream.
bool dcs_quad_focus_enabled() noexcept {
    wchar_t value[2]{};
    return GetEnvironmentVariableW(L"DCSVR_QUAD_FOCUS", value, 2) == 1 && value[0] == L'1';
}
bool dcs_supported_view_configuration(XrViewConfigurationType type) noexcept {
    return type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO ||
        (dcs_quad_focus_enabled() && type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO);
}
'''
replace('openxr_layer/openxr_layer.cpp', '#include <vector>\n\nnamespace {\n', '#include <vector>\n\nnamespace {\n\n' + helper)
replace('openxr_layer/openxr_layer.cpp', '#include "projection_selection.hpp"', '#include "projection_selection.hpp"\n#include "../src/dcs_quad_focus_policy.hpp"')
replace('openxr_layer/openxr_layer.cpp', '            target.array_index = source.array_index;', '''            const auto focus_chain = swapchains.find(source.swapchain);
            if (dcs_quad_focus_enabled() && focus_chain != swapchains.end() && focus_chain->second.create_info.mipCount == 1)
                target.flags |= cheeky::foveated_dlss::dcs_quad_focus_single_mip;
            target.array_index = source.array_index;''')
replace('openxr_layer/openxr_layer.cpp', '''                    if (sub_image.imageArrayIndex != 0U) {
                        state.ambiguous_resource = true;
                    }''', '''                    if (selected.first_view == 2U) {
                        const auto focus_chain = swapchains.find(sub_image.swapchain);
                        if (focus_chain == swapchains.end() || focus_chain->second.session != session ||
                            !cheeky::foveated_dlss::dcs_quad_focus_subimage_valid(
                                focus_chain->second.create_info.width, focus_chain->second.create_info.height,
                                focus_chain->second.create_info.arraySize, focus_chain->second.create_info.mipCount,
                                sub_image.imageArrayIndex, sub_image.imageRect.offset.x, sub_image.imageRect.offset.y,
                                sub_image.imageRect.extent.width, sub_image.imageRect.extent.height))
                            state.ambiguous_resource = true;
                    } else if (sub_image.imageArrayIndex != 0U) {
                        state.ambiguous_resource = true;
                    }''')
replace('openxr_layer/openxr_layer.cpp', '        extension_availability(next_gipa);', '        (dcs_quad_focus_enabled() ? ExtensionAvailability::absent : extension_availability(next_gipa));')
replace('openxr_layer/openxr_layer.cpp', '        extension_availability(next_gipa, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);', '        (dcs_quad_focus_enabled() ? ExtensionAvailability::absent : extension_availability(next_gipa, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME));')
replace('openxr_layer/openxr_layer.cpp', 'info->primaryViewConfigurationType !=\n                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;', '!dcs_supported_view_configuration(info->primaryViewConfigurationType);')
replace('openxr_layer/openxr_layer.cpp', 'locate_info->viewConfigurationType !=\n                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;', '!dcs_supported_view_configuration(locate_info->viewConfigurationType);')
replace('openxr_layer/openxr_layer.cpp', '    if (XR_FAILED(result) || locate_info == nullptr || count == nullptr ||\n        views == nullptr || capacity < CHEEKY_GAZE_MAX_VIEWS ||\n        *count != CHEEKY_GAZE_MAX_VIEWS) {', '''    const bool quad_focus = locate_info && dcs_quad_focus_enabled() &&
        locate_info->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
    const unsigned expected_views = quad_focus ? 4U : 2U;
    if (XR_FAILED(result) || locate_info == nullptr || count == nullptr ||
        views == nullptr || capacity < expected_views || *count != expected_views) {''')
replace('openxr_layer/openxr_layer.cpp', '    // App views may be head-relative.', '    const auto* focus_views = views + (quad_focus ? 2U : 0U);\n\n    // App views may be head-relative.')
# The focus pair's views: every views[ inside this function reads focus_views[ (\bviews never matches focus_views).
path = source / 'openxr_layer/openxr_layer.cpp'
text = path.read_text(encoding='utf-8')
start = text.index('    const auto* focus_views = views + (quad_focus ? 2U : 0U);')
# Rewrite only this function (it ends at the first column-0 brace), never later functions.
end = text.index('\n}\n', start)
text = text[:start] + re.sub(r'\bviews\[', 'focus_views[', text[start:end]) + text[end:]
path.write_text(text, encoding='utf-8', newline='\n')
replace('openxr_layer/openxr_layer.cpp', '    std::array<XrView, 2> motion_views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};', '    std::array<XrView, 4> motion_storage{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};\n    auto* motion_views = motion_storage.data() + (quad_focus ? 2U : 0U);')
replace('openxr_layer/openxr_layer.cpp', '            2, &motion_count, motion_views.data())) && motion_count == 2 &&', '            expected_views, &motion_count, motion_storage.data())) && motion_count == expected_views &&')
replace('openxr_layer/openxr_layer.cpp', '        snapshot.session_generation = session->generation;', '''        snapshot.session_generation = session->generation;
        if (dcs_quad_focus_enabled() && session->view_configuration == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO)
            snapshot.status_flags |= 1U << 12U;''')
replace('openxr_layer/openxr_layer.cpp', '            it->second.create_info.arraySize == 1;\n    });', '            it->second.create_info.arraySize == 1;\n    }, dcs_quad_focus_enabled());')
replace('openxr_layer/openxr_layer.cpp', 'projection->views[eye].pose', 'projection->views[eye + selected.first_view].pose')
replace('openxr_layer/openxr_layer.cpp', 'projection->views[eye].fov', 'projection->views[eye + selected.first_view].fov')
replace('openxr_layer/openxr_layer.cpp', 'projection->views[eye].subImage', 'projection->views[eye + selected.first_view].subImage')
replace('openxr_layer/openxr_layer.cpp', 'projection->views[view_index].subImage', 'projection->views[view_index + selected.first_view].subImage')
replace('openxr_layer/openxr_layer.cpp', 'owner->second.view_configuration == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;', 'dcs_supported_view_configuration(owner->second.view_configuration);')
replace('openxr_layer/openxr_layer.cpp', 'state.view_configuration != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;', '!dcs_supported_view_configuration(state.view_configuration);')

replace('src/gaze_foveation.hpp', 'void apply_next_jump_preview', 'bool dcs_quad_focus_processing_allowed(DlssViewId view_id, IUnknown* output, unsigned x, unsigned y, unsigned width, unsigned height, bool& reset_original) noexcept;\n\nvoid apply_next_jump_preview')
replace('src/gaze_foveation.cpp', '#include "gaze_foveation.hpp"', '#include "gaze_foveation.hpp"\n#include "dcs_quad_focus_policy.hpp"')
replace('src/gaze_foveation.cpp', 'struct ViewState {', 'struct ViewState {\n    DcsQuadFocusTransition dcs_quad_focus_transition{};')
replace('src/gaze_foveation.cpp', 'bool calculate_coordinated_crop(', '#include "dcs_quad_focus_gate.inc"\n\nbool calculate_coordinated_crop(')
gate = '''    ID3D11Resource* dcs_output{};
    if (parameters) parameters->Get("Output", &dcs_output);
    if (!dcs_quad_focus_processing_allowed(reinterpret_cast<DlssViewId>(handle), dcs_output,
            get_ui(parameters, "DLSS.Output.Subrect.Base.X"), get_ui(parameters, "DLSS.Output.Subrect.Base.Y"),
            get_ui(parameters, "OutWidth"), get_ui(parameters, "OutHeight"))) {
        release_d3d11_transport_view(handle);
        diagnostic_note_d3d11_execution_path(D3D11ExecutionPath::game_fallback);
        static std::atomic<std::uint64_t> skipped{};
        const auto n = ++skipped;
        if (n <= 8 || (n & (n - 1)) == 0)
            trace_event("DCS quad focus: preserving unmapped/peripheral view handle=%p count=%llu", handle,
                static_cast<unsigned long long>(n));
        return original(context, handle, parameters, callback);
    }
'''
# The two DX11 callback variants share the same gate. Calibration wrapping still
# observes their original output, so cold-start mapping can converge.
for name in ['evaluate_d3d11_impl', 'evaluate_d3d11_c_impl']:
    path = source / 'src/hooks.cpp'
    text = path.read_text(encoding='utf-8')
    start = text.index('NgxResult ' + name + '(')
    anchor = text.index('    const auto settings = current_settings();', start)
    text = text[:anchor] + gate + text[anchor:]
    path.write_text(text, encoding='utf-8', newline='\n')
replace('src/hooks.cpp', '    ID3D11Resource* dcs_output{};', '    bool dcs_reset_original{};\n    ID3D11Resource* dcs_output{};', count=2)
replace('src/hooks.cpp', 'get_ui(parameters, "OutWidth"), get_ui(parameters, "OutHeight"))) {', 'get_ui(parameters, "OutWidth"), get_ui(parameters, "OutHeight"), dcs_reset_original)) {', count=2)
replace('src/hooks.cpp', '        release_d3d11_transport_view(handle);\n        diagnostic_note_d3d11_execution_path(D3D11ExecutionPath::game_fallback);', '''        release_d3d11_transport_view(handle);
        skip_dlss_nr_history(reinterpret_cast<DlssViewId>(handle));
        struct ResetScope {
            NgxParameters* bag{}; unsigned saved{};
            ResetScope(const NgxParameters* p, bool reset) {
                if (reset && p && ngx_succeeded(p->Get("Reset", &saved))) {
                    bag = const_cast<NgxParameters*>(p); bag->Set("Reset", 1U);
                }
            }
            ~ResetScope() { if (bag) bag->Set("Reset", saved); }
        } reset_scope(parameters, dcs_reset_original);
        diagnostic_note_d3d11_execution_path(D3D11ExecutionPath::game_fallback);''', count=2)
replace('CMakeLists.txt', 'set_tests_properties(CheekyMixedCalibrationTests PROPERTIES TIMEOUT 60)', 'set_tests_properties(CheekyMixedCalibrationTests PROPERTIES TIMEOUT 300)')
shutil.copyfile(root / 'patches/cheeky/dcs_quad_focus_layer_tests.inc', source / 'tests/dcs_quad_focus_layer_tests.inc')
replace('tests/openxr_calibration_tests.cpp', 'void layer_policy() {', '#include "dcs_quad_focus_layer_tests.inc"\n\nvoid layer_policy() {')
replace('tests/openxr_calibration_tests.cpp', '        projection_policy();', '        projection_policy();\n        dcs_quad_focus_layer_test();')
replace('tests/openxr_calibration_tests.cpp', 'int run_openxr_calibration_tests() {', '''int run_dcs_quad_focus_layer_tests() {
    try { dcs_quad_focus_layer_test(); return 0; }
    catch (const std::exception& e) {
        SetEnvironmentVariableW(L"DCSVR_QUAD_FOCUS", nullptr);
        std::cerr << "DCS quad focus adapter: " << e.what() << '\\n';
        return 1;
    }
}
int run_openxr_calibration_tests() {''')
replace('tests/gaze_tests.cpp', 'int main(int argc, char** argv) {', '''int run_dcs_quad_focus_layer_tests();
int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dcs-quad-focus") == 0) return run_dcs_quad_focus_layer_tests();''')
replace('CMakeLists.txt', 'add_test(NAME CheekyGazeTests COMMAND CheekyTests)', '''add_test(NAME CheekyGazeTests COMMAND CheekyTests)
add_test(NAME DcsQuadFocusLayerTests COMMAND CheekyTests --dcs-quad-focus)''')
# Four-view layout side channel: the NGX gate's size-bijection proof and live
# diagnostics need the peripheral extents, which the two-slot gaze ABI omits.
replace('openxr_layer/openxr_layer.cpp', '        (dcs_quad_focus_enabled() && type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO);\n}\n', """        (dcs_quad_focus_enabled() && type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO);
}
// Last submitted VARJO layout (views 0/1 peripheral, 2/3 focus) for the NGX gate.
std::mutex dcs_quad_layout_mutex;
cheeky::foveated_dlss::DcsQuadLayoutV1 dcs_quad_layout{};
std::uint64_t dcs_quad_layout_sequence{};
""")
replace('openxr_layer/openxr_layer.cpp', """            if (!selected.projection) {
                for (auto& view : state.submitted_views) view = {};
            }""", """            if (dcs_quad_focus_enabled()) {
                cheeky::foveated_dlss::DcsQuadLayoutV1 layout{};
                layout.structure_size = sizeof(layout);
                layout.session_generation = state.generation;
                if (const auto* quad = selected.projection; quad && selected.first_view == 2U && !state.ambiguous_resource) {
                    layout.valid = 1U;
                    for (unsigned view = 0; view < 4U; ++view) {
                        const auto& image = quad->views[view].subImage;
                        layout.width[view] = image.imageRect.extent.width > 0 ? static_cast<std::uint32_t>(image.imageRect.extent.width) : 0U;
                        layout.height[view] = image.imageRect.extent.height > 0 ? static_cast<std::uint32_t>(image.imageRect.extent.height) : 0U;
                        layout.array_index[view] = image.imageArrayIndex;
                        layout.swapchain[view] = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(image.swapchain));
                    }
                }
                std::lock_guard layout_lock(dcs_quad_layout_mutex);
                const bool changed = layout.valid != dcs_quad_layout.valid || layout.width != dcs_quad_layout.width ||
                    layout.height != dcs_quad_layout.height || layout.array_index != dcs_quad_layout.array_index ||
                    layout.session_generation != dcs_quad_layout.session_generation;
                layout.sequence = ++dcs_quad_layout_sequence;
                layout.publication_qpc = query_qpc();
                dcs_quad_layout = layout;
                if (changed)
                    log_startup("dcs_quad_layout valid=%u generation=%llu peripheral=%ux%u,%ux%u focus=%ux%u,%ux%u arrays=%u,%u,%u,%u selected=%u first_view=%u selection_ambiguous=%u selection_unsupported=%u ambiguous_resource=%u\\n",
                        layout.valid, static_cast<unsigned long long>(layout.session_generation),
                        layout.width[0], layout.height[0], layout.width[1], layout.height[1],
                        layout.width[2], layout.height[2], layout.width[3], layout.height[3],
                        layout.array_index[0], layout.array_index[1], layout.array_index[2], layout.array_index[3],
                        selected.projection ? 1U : 0U, selected.first_view, selected.ambiguous ? 1U : 0U,
                        selected.unsupported ? 1U : 0U, state.ambiguous_resource ? 1U : 0U);
            }
            if (!selected.projection) {
                for (auto& view : state.submitted_views) view = {};
            }""")
replace('openxr_layer/openxr_layer.cpp', 'extern "C" __declspec(dllexport) void __cdecl\nCheekyOpenXR_SetSimulationPattern(', """extern "C" __declspec(dllexport) std::uint32_t __cdecl
CheekyOpenXR_GetDcsQuadLayout(void* const output, const std::uint32_t output_size) {
    if (output == nullptr || output_size < sizeof(cheeky::foveated_dlss::DcsQuadLayoutV1)) return 0U;
    std::lock_guard lock(dcs_quad_layout_mutex);
    std::memcpy(output, &dcs_quad_layout, sizeof(dcs_quad_layout));
    return dcs_quad_layout.structure_size != 0U ? 1U : 0U;
}

extern "C" __declspec(dllexport) void __cdecl
CheekyOpenXR_SetSimulationPattern(""")

# Session-only settings change for the in-flight toggle: same validation as "set", but never written to the INI,
# so every launch starts from the applied profile and restore never sees a hotkey state as a user edit.
replace('uevr/runtime.cpp', '        else if (action == "set") {', '        else if (action == "set" || action == "set_session") {')
replace('uevr/runtime.cpp', '        s.message = "Settings applied"; request_save(s); return true;', '        s.message = "Settings applied"; if (action != "set_session") request_save(s); return true;')

# With the Quad Views focus adapter nothing in Cheeky is placed by gaze (Quad Views owns foveation and DLSS-NR covers
# whole focus views), so the stereo marker calibration, which stamps coloured squares into the image and never
# converges on four views, stays off in DCS VR Control sessions.
replace('uevr/runtime.cpp', '        eye_calibration_enable(true);\n        s.cadence.reset();', '''        {
            wchar_t quad_focus[2]{};
            eye_calibration_enable(!(GetEnvironmentVariableW(L"DCSVR_QUAD_FOCUS", quad_focus, 2) == 1 && quad_focus[0] == L'1'));
        }
        s.cadence.reset();''')

# In-game DLSS-NR toggle: the key chosen in DCS VR Control (DCSVR_NR_HOTKEY), polled at the game's own Present.
shutil.copyfile(root / 'patches/cheeky/dcs_nr_hotkey.hpp', source / 'standalone/dcs_nr_hotkey.hpp')
replace('standalone/host.cpp', '#include "overlay.hpp"\n', '#include "overlay.hpp"\n#include "dcs_nr_hotkey.hpp"\n')
replace('standalone/host.cpp', '        cheeky::standalone::overlay_present(chain, queue.Get(), ui);\n', """        // DCS VR Control: the chosen key toggles DLSS-NR while the game window is in front.
        static cheeky::standalone::DcsNrHotkey nr_hotkey;
        static const cheeky::standalone::DcsNrHotkeySpec nr_key = [] {
            cheeky::standalone::DcsNrHotkeySpec spec{};
            char text[16]{};
            const auto length = GetEnvironmentVariableA("DCSVR_NR_HOTKEY", text, sizeof(text));
            if (length > 0 && length < sizeof(text)) cheeky::standalone::dcs_nr_parse_hotkey(text, spec);
            return spec;
        }();
        const auto foreground = GetForegroundWindow();
        DWORD foreground_process{};
        const bool game_foreground = foreground && GetWindowThreadProcessId(foreground, &foreground_process) &&
            foreground_process == GetCurrentProcessId();
        nr_hotkey.poll(cheeky::standalone::dcs_nr_hotkey_down(nr_key, [](unsigned key) { return (GetAsyncKeyState(int(key)) & 0x8000) != 0; }), game_foreground,
            [](std::string& json) {
                std::vector<char> buffer(32768);
                if (!runtime_snapshot || !runtime_snapshot(buffer.data(), static_cast<std::uint32_t>(buffer.size()))) return false;
                json = buffer.data();
                return true;
            },
            [](const char* text) { return runtime_command && runtime_command(attachment, text); },
            [](const char* message) { log_host(message); });
        cheeky::standalone::overlay_present(chain, queue.Get(), ui);
""")
replace('tests/openxr_calibration_tests.cpp', '#include "../openxr_layer/projection_selection.hpp"\n', '#include "../openxr_layer/projection_selection.hpp"\n#include "../src/dcs_quad_focus_policy.hpp"\n#include "../standalone/dcs_nr_hotkey.hpp"\n')
# Live status block for the in-headset diagnostic overlay (OFXR fork patch): Local\DcsVrControlStatus-<pid>,
# published from the runtime tick at most every 100 ms; the quad focus gate feeds its accept/reject counts.
# Each anchor below keeps the earlier insertions contiguous, so re-running stays idempotent.
shutil.copyfile(root / 'patches/cheeky/dcs_status_block.hpp', source / 'src/dcs_status_block.hpp')
shutil.copyfile(root / 'patches/cheeky/dcs_status_runtime.inc', source / 'uevr/dcs_status_runtime.inc')
shutil.copyfile(root / 'patches/cheeky/dcs_status_tests.inc', source / 'tests/dcs_status_tests.inc')
replace('src/gaze_foveation.cpp', '#include "dcs_quad_focus_policy.hpp"\n', '#include "dcs_quad_focus_policy.hpp"\n#include "dcs_status_block.hpp"\n')
replace('uevr/runtime.cpp', 'extern "C" __declspec(dllexport) void CheekyRuntime_Tick(', '#include "dcs_status_block.hpp"\n#include "../standalone/dcs_nr_hotkey.hpp"\n#include "dcs_status_runtime.inc"\nextern "C" __declspec(dllexport) void CheekyRuntime_Tick(')
replace('uevr/runtime.cpp', '        nr_was_down = nr_down;\n    } catch (...) { set_processing_allowed(false); }', '        nr_was_down = nr_down;\n        dcs_status_publish(s, renderer);\n    } catch (...) { set_processing_allowed(false); }')
replace('tests/openxr_calibration_tests.cpp', '#include "../standalone/dcs_nr_hotkey.hpp"\n', '#include "../standalone/dcs_nr_hotkey.hpp"\n#include "../src/dcs_status_block.hpp"\n')
replace('tests/openxr_calibration_tests.cpp', 'int run_dcs_quad_focus_layer_tests() {', '''#include "dcs_status_tests.inc"
int run_dcs_status_tests() {
    try { dcs_status_block_test(); return 0; }
    catch (const std::exception& e) { std::cerr << "DCS VR status block: " << e.what() << '\\n'; return 1; }
}
int run_dcs_quad_focus_layer_tests() {''')
replace('tests/gaze_tests.cpp', '    if (argc == 2 && std::strcmp(argv[1], "--dcs-quad-focus") == 0) return run_dcs_quad_focus_layer_tests();', '''    if (argc == 2 && std::strcmp(argv[1], "--dcs-quad-focus") == 0) return run_dcs_quad_focus_layer_tests();
    if (argc == 2 && std::strcmp(argv[1], "--dcs-status") == 0) { int run_dcs_status_tests(); return run_dcs_status_tests(); }''')
replace('CMakeLists.txt', 'add_test(NAME DcsQuadFocusLayerTests COMMAND CheekyTests --dcs-quad-focus)', 'add_test(NAME DcsQuadFocusLayerTests COMMAND CheekyTests --dcs-quad-focus)\nadd_test(NAME DcsVrStatusTests COMMAND CheekyTests --dcs-status)')
# DX11->DX12 transport: one texture set per view instead of one per ring slot. Every frame is serialized by the shared
# fence (D3D11 writes -> Signal -> D3D12 Wait ... D3D12 Signal -> D3D11 Wait before any later D3D11 command), so the
# extra copies never overlapped (~105 MB per slot at 1764x2480 NR). Allocators and timing queries stay per slot.
transport = 'src/d3d11_d3d12_transport.cpp'
texture_fields = ('color|depth|motion_vectors|output|peripheral_color|peripheral_depth|peripheral_motion_vectors|'
    'peripheral_output|nr_color|nr_depth|nr_motion_vectors|input_width|input_height|sr_motion_width|sr_motion_height|'
    'output_width|output_height|peripheral_render_width|peripheral_render_height|peripheral_output_width|'
    'peripheral_output_height|peripheral_motion_width|peripheral_motion_height|peripheral_enabled|nr_input_width|'
    'nr_input_height|nr_output_width|nr_output_height|nr_motion_width|nr_motion_height|color_format|motion_format|output_format')
replace(transport, """struct TransportSlot {
    SharedTexture color;
    SharedTexture depth;
    SharedTexture motion_vectors;
    SharedTexture output;
    SharedTexture peripheral_color;
    SharedTexture peripheral_depth;
    SharedTexture peripheral_motion_vectors;
    SharedTexture peripheral_output;
    bool nr_before{};
    SharedTexture nr_color;
    SharedTexture nr_depth;
    SharedTexture nr_motion_vectors;
    ID3D12CommandAllocator* allocator{};""", """// DCS VR Control: one shared texture set per view. D3D11 and the private D3D12
// queue are serialized by the shared fence on every frame: D3D11 writes the
// inputs, signals, D3D12 waits; D3D12 signals and D3D11 waits before any later
// D3D11 command (composite, NR copies, the next frame's input copies). A second
// or third texture copy therefore never overlapped. Only CPU-reset objects
// (command allocators, timing queries) need one copy per in-flight ring slot.
// Error paths that could not queue the D3D11 wait mark the view unordered and
// the next frame waits on the CPU (TransportView::unordered).
struct TransportTextures {
    SharedTexture color;
    SharedTexture depth;
    SharedTexture motion_vectors;
    SharedTexture output;
    SharedTexture peripheral_color;
    SharedTexture peripheral_depth;
    SharedTexture peripheral_motion_vectors;
    SharedTexture peripheral_output;
    bool nr_before{};
    SharedTexture nr_color;
    SharedTexture nr_depth;
    SharedTexture nr_motion_vectors;
    bool native_sr{}; // NR-only on the game's own DX11 DLSS: no SR textures.
    bool nr_in_place{}; // NR before upscaling runs on the SR input textures.
    std::uint32_t input_width{};
    std::uint32_t input_height{};
    std::uint32_t sr_motion_width{}, sr_motion_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t peripheral_render_width{};
    std::uint32_t peripheral_render_height{};
    std::uint32_t peripheral_output_width{};
    std::uint32_t peripheral_output_height{};
    std::uint32_t peripheral_motion_width{};
    std::uint32_t peripheral_motion_height{};
    bool peripheral_enabled{};
    std::uint32_t nr_input_width{};
    std::uint32_t nr_input_height{};
    std::uint32_t nr_output_width{};
    std::uint32_t nr_output_height{};
    std::uint32_t nr_motion_width{};
    std::uint32_t nr_motion_height{};
    DXGI_FORMAT color_format{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT motion_format{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT output_format{DXGI_FORMAT_UNKNOWN};
};

struct TransportSlot {
    bool nr_before{}; // Processing order of this slot's pending timing queries.
    ID3D12CommandAllocator* allocator{};""")
replace(transport, """    std::uint64_t done_value{};
    std::uint32_t input_width{};
    std::uint32_t input_height{};
    std::uint32_t sr_motion_width{}, sr_motion_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t peripheral_render_width{};
    std::uint32_t peripheral_render_height{};
    std::uint32_t peripheral_output_width{};
    std::uint32_t peripheral_output_height{};
    std::uint32_t peripheral_motion_width{};
    std::uint32_t peripheral_motion_height{};
    bool peripheral_enabled{};
    std::uint32_t nr_input_width{};
    std::uint32_t nr_input_height{};
    std::uint32_t nr_output_width{};
    std::uint32_t nr_output_height{};
    std::uint32_t nr_motion_width{};
    std::uint32_t nr_motion_height{};
    DXGI_FORMAT color_format{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT motion_format{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT output_format{DXGI_FORMAT_UNKNOWN};
};

struct TransportView {
    DlssViewId view_id{};
    std::array<TransportSlot, transport_slot_count> slots{};
    std::uint32_t next_slot{};
};""", """    std::uint64_t done_value{};
};

struct TransportView {
    DlssViewId view_id{};
    std::array<TransportSlot, transport_slot_count> slots{};
    std::uint32_t next_slot{};
    TransportTextures textures{};
    bool unordered{};
};""")
replace(transport, """    release_shared_texture(slot.output);
    release_shared_texture(slot.motion_vectors);
    release_shared_texture(slot.depth);
    release_shared_texture(slot.color);
    release_shared_texture(slot.peripheral_output);
    release_shared_texture(slot.peripheral_motion_vectors);
    release_shared_texture(slot.peripheral_depth);
    release_shared_texture(slot.peripheral_color);
    release_shared_texture(slot.nr_motion_vectors);
    release_shared_texture(slot.nr_depth);
    release_shared_texture(slot.nr_color);
    release(slot.nr_allocator);
    release(slot.allocator);
    slot = {};
}""", """    release(slot.nr_allocator);
    release(slot.allocator);
    slot = {};
}

void release_textures(TransportTextures& textures) noexcept {
    release_shared_texture(textures.output);
    release_shared_texture(textures.motion_vectors);
    release_shared_texture(textures.depth);
    release_shared_texture(textures.color);
    release_shared_texture(textures.peripheral_output);
    release_shared_texture(textures.peripheral_motion_vectors);
    release_shared_texture(textures.peripheral_depth);
    release_shared_texture(textures.peripheral_color);
    release_shared_texture(textures.nr_motion_vectors);
    release_shared_texture(textures.nr_depth);
    release_shared_texture(textures.nr_color);
    textures = {};
}""")
replace(transport, """        for (auto& slot : view.slots) release_slot(slot);
    }
    device.views.clear();""", """        for (auto& slot : view.slots) release_slot(slot);
        release_textures(view.textures);
    }
    device.views.clear();""")
replace(transport, """            for (auto& slot : iterator->slots) release_slot(slot);
            device.views.erase(iterator);""", """            for (auto& slot : iterator->slots) release_slot(slot);
            release_textures(iterator->textures);
            device.views.erase(iterator);""")
replace(transport, """    // Only called after the slot's completion fence has passed. Release the""",
    """    // Only called after the view's last private submission completed. Release the""")
replace(transport, """[[nodiscard]] bool slot_matches(
    const TransportSlot& slot,""", """[[nodiscard]] bool slot_matches(
    const TransportTextures& slot,""")
replace(transport, """    TransportDevice& device,
    TransportSlot& slot,
    const CropGeometry& crop,""", """    TransportDevice& device,
    TransportSlot& slot,
    TransportTextures& textures,
    const CropGeometry& crop,""")
replace(transport, """    const DXGI_FORMAT output_format,
    const bool nr_before
) noexcept {
    // Slot is idle; preserve textures""", """    const DXGI_FORMAT output_format,
    const bool nr_before,
    const bool textures_ready,
    const bool nr_in_place
) noexcept {
    // Slot is idle; preserve textures""")
replace(transport, """    // Disabled groups must not retain stale textures behind newly written""", """    // Matching textures may still be in use by queued work: never touch them.
    if (textures_ready) return true;
    // Disabled groups must not retain stale textures behind newly written""")
replace(transport, """        ))) {
        release_slot(slot);
        return false;
    }""", """        ))) {
        release_textures(textures);
        return false;
    }""")
# Texture fields inside initialize_slot (from its texture section to the end of the function).
value = (source / transport).read_text(encoding='utf-8')
start = value.index('    // Matching textures may still be in use by queued work: never touch them.')
end = value.index('\n}\n', start)
value = value[:start] + re.sub(r'\bslot\.(' + texture_fields + r'|nr_before)\b', r'textures.\1', value[start:end]) + value[end:]
(source / transport).write_text(value, encoding='utf-8', newline='\n')
replace(transport, """[[nodiscard]] bool wait_for_slot(TransportDevice& device,
    ID3D11DeviceContext* context, const TransportSlot& slot) noexcept {
    if (!slot.done_value) return true;""", """[[nodiscard]] bool wait_for_value(TransportDevice& device,
    ID3D11DeviceContext* context, const std::uint64_t done_value) noexcept {
    if (!done_value) return true;""")
value = (source / transport).read_text(encoding='utf-8')
start = value.index('[[nodiscard]] bool wait_for_value(TransportDevice& device,')
end = value.index('\n}\n', start)
value = value[:start] + value[start:end].replace('slot.done_value', 'done_value') + value[end:]
(source / transport).write_text(value, encoding='utf-8', newline='\n')
replace(transport, """    return ready;
}

[[nodiscard]] bool recover_init_contract(""", """    return ready;
}

[[nodiscard]] bool wait_for_slot(TransportDevice& device,
    ID3D11DeviceContext* context, const TransportSlot& slot) noexcept {
    return wait_for_value(device, context, slot.done_value);
}

// After a frame whose D3D11 wait was not queued (or whose Signal failed), fence
// the queue again and wait on the CPU before D3D11 touches the shared textures.
[[nodiscard]] bool wait_for_transport_idle(TransportDevice& device,
    ID3D11DeviceContext* context) noexcept {
    const auto target = device.next_fence_value++;
    if (FAILED(device.queue12->Signal(device.fence12, target))) return false;
    return wait_for_value(device, context, target);
}

[[nodiscard]] bool recover_init_contract(""")
# Texture fields in the evaluation path (slot -> view textures); timing/allocator fields stay per slot.
value = (source / transport).read_text(encoding='utf-8')
start = value.index('    auto& slot = view->slots[view->next_slot++ % transport_slot_count];')
# The DCS review round turned the function's return type into bool; match either spelling.
end = start + re.search(r'(?:void|bool) release_d3d11_transport_view\(', value[start:]).start()
value = value[:start] + re.sub(r'\bslot\.(' + texture_fields + r')\b', r'textures.\1', value[start:end]) + value[end:]
(source / transport).write_text(value, encoding='utf-8', newline='\n')
replace(transport, """    auto& slot = view->slots[view->next_slot++ % transport_slot_count];
    if (!wait_for_slot(*device, context, slot)) {""", """    auto& slot = view->slots[view->next_slot++ % transport_slot_count];
    auto& textures = view->textures;
    // Shared textures (create_shared_texture): any unordered view orders them all.
    const bool device_unordered = std::any_of(device->views.begin(), device->views.end(),
        [](const TransportView& other) { return other.unordered; });
    if (device_unordered) {
        if (!wait_for_transport_idle(*device, context)) {
            release(context4);
            return reject_transport(D3D11TransportStatus::transport_slot_busy);
        }
        for (auto& other : device->views) other.unordered = false;
    }
    if (!wait_for_slot(*device, context, slot)) {""")
replace(transport, """    if (!slot_matches(
            slot, reconstruction,""", """    const bool textures_match = textures.native_sr == native_sr && slot_matches(
            textures, reconstruction,""")
replace(transport, """        ) && !initialize_slot(
            *device, slot, reconstruction,""", """        );
    // Resizing frees textures the previous frame (any slot) may still use.
    if (!textures_match && !wait_for_value(*device, context, pending_work(*view))) {
        release(context4);
        return reject_transport(D3D11TransportStatus::transport_slot_busy);
    }
    if (!textures_match) textures.native_sr = native_sr;
    if ((!textures_match || !slot.allocator || !slot.timing_end || (settings.nr_enabled && !slot.nr_allocator)) &&
        !initialize_slot(
            *device, slot, textures, reconstruction,""")
replace(transport, """            color_desc.Format, motion_desc.Format, output_desc.Format, nr_before
        )) {
        release(context4);""", """            color_desc.Format, motion_desc.Format, output_desc.Format, nr_before, textures_match, nr_in_place
        )) {
        release(context4);""")
replace(transport, """    TimingScope timing{context, slot};""", """    slot.nr_before = nr_before;
    TimingScope timing{context, slot};""")
replace(transport, """    if (FAILED(device->queue12->Signal(device->fence12, slot.done_value)) ||
        FAILED(context4->Wait(device->fence11, slot.done_value))) {
        release(context4);""", """    if (FAILED(device->queue12->Signal(device->fence12, slot.done_value)) ||
        FAILED(context4->Wait(device->fence11, slot.done_value))) {
        view->unordered = true;
        release(context4);""")
replace(transport, """                    slot.done_value = nr_done_value;
                    if (SUCCEEDED(context4->Wait(
                            device->fence11, nr_done_value
                        )) && nr_succeeded) {""", """                    slot.done_value = nr_done_value;
                    const bool nr_ordered = SUCCEEDED(context4->Wait(
                            device->fence11, nr_done_value
                        ));
                    if (!nr_ordered) view->unordered = true;
                    if (nr_ordered && nr_succeeded) {""")
replace(transport, """                } else {
                    nr_succeeded = false;
                    slot.dlss_timing_query_count = 2U;
                }""", """                } else {
                    view->unordered = true;
                    nr_succeeded = false;
                    slot.dlss_timing_query_count = 2U;
                }""")

# DCS NR-only transport (Enabled=0, NrEnabled=1, full-view NR, no peripheral DLAA): switching to it rebuilds exactly
# one 7-texture set for the view (SR color/depth/motion/output + NR color/depth/motion), not one per ring slot, and
# steady frames allocate nothing.
replace('tests/runtime_host_tests.cpp', '''    require(command(attachment, "1\\n22\\nset\\nEnabled=false\\nNrEnabled=true"), "Enable independent NR-only transport");
    require(ngx_succeeded(evaluate(context, handle, &parameters, nullptr)), "NR-only transport evaluation");''', '''    const auto total_allocations = [&] { const auto counts = allocations(); return counts[0] + counts[1] + counts[2]; };
    const auto log_count = [&](const char* text) {
        std::ifstream log(log_path);
        unsigned count{};
        for (std::string line; std::getline(log, line);) count += line.find(text) != std::string::npos ? 1U : 0U;
        return count;
    };
    // Before NR over the whole view still uses the private SR feature; NR runs in place on its input:
    // one 4-texture set per view (SR colour/depth/motion/output), no separate NR textures.
    const auto before_nr_only = total_allocations();
    require(command(attachment, "1\\n22\\nset\\nEnabled=false\\nNrEnabled=true\\nNrFoveated=false\\nPeripheralDlaa=false\\nNrProcessingOrder=1"), "Enable independent NR-only transport");
    frames();
    const auto nr_only_allocations = total_allocations() - before_nr_only;
    printf("NR-only (before) switch allocated %u transport textures for one view\\n", nr_only_allocations);
    require(nr_only_allocations == 4, "NR-only (before) allocates exactly one 4-texture set per view (NR in place)");
    require(contains(snapshot(get), "\\"nr\\":\\"Active\\""), "NR before upscaling active in place on the SR input");
    frames();
    require(total_allocations() == before_nr_only + nr_only_allocations, "Steady NR-only frames allocate no transport textures");
    // After NR over the whole view: the game's DX11 evaluate runs natively, no private SR feature is created.
    const auto canonical_before = log_count("D3D12 canonical create");
    const auto evaluates_before = proc<unsigned(*)()>(ngx, "CheekyFakeEvaluates")();
    const auto native_allocations_before = total_allocations();
    require(command(attachment, "1\\n31\\nset\\nNrProcessingOrder=0"), "Switch NR-only to after upscaling");
    frames();
    frames();
    const auto native_evaluates = proc<unsigned(*)()>(ngx, "CheekyFakeEvaluates")() - evaluates_before;
    printf("NR-only (after) native DX11 evaluates=%u canonical creates=%u new transport textures=%u\\n", native_evaluates,
        log_count("D3D12 canonical create") - canonical_before, total_allocations() - native_allocations_before);
    require(log_count("game DX11 DLSS evaluated natively") > 0, "NR-only after upscaling evaluates the game's DX11 DLSS");
    require(native_evaluates >= 12, "The fake game DX11 evaluate runs every NR-only frame");
    require(log_count("D3D12 canonical create") == canonical_before, "NR-only after upscaling creates no private D3D12 SR feature");
    require(total_allocations() - native_allocations_before <= 3, "NR-only after upscaling allocates only NR textures");
    require(contains(snapshot(get), "\\"nr\\":\\"Active\\""), "NR active on the native DX11 SR output");''')

# NR-only After-upscaling mode (Enabled=0, NrEnabled=1, NrProcessingOrder=0 "after", full-view NR, no peripheral DLAA):
# the game's own DX11 DLSS evaluates its feature as usual and the transport only runs DLSS-NR on D3D12. No private
# D3D12 SR feature ("D3D12 canonical create"), no SR input/output textures, no SR input copies and no composite.
replace('src/d3d11_d3d12_transport.hpp', '''// Returns true only when the D3D12 transport owned the frame.''', '''// The game's own DX11 evaluation of the current frame (NR-only after-upscaling
// mode runs it instead of a private D3D12 SR feature).
struct D3D11NativeEvaluate {
    NgxResult (*invoke)(void* state) noexcept{};
    void* state{};
};
template<class F>
[[nodiscard]] D3D11NativeEvaluate make_d3d11_native_evaluate(F& evaluate) noexcept {
    return {[](void* state) noexcept -> NgxResult { return (*static_cast<F*>(state))(); }, &evaluate};
}

// Returns true only when the D3D12 transport owned the frame.''')
replace('src/d3d11_d3d12_transport.hpp', '''    const D3D11TransportNgx& ngx,
    NgxResult& result
) noexcept;''', '''    const D3D11TransportNgx& ngx,
    NgxResult& result,
    const D3D11NativeEvaluate* native = nullptr
) noexcept;''')
replace('src/hooks.cpp', '''        if (evaluate_d3d11_via_d3d12(
                context, handle, parameters, settings,
                current_transport_ngx(runtime), transport_result)) {''', '''        // NR-only after upscaling: the transport runs the game's own DX11 DLSS
        // on its handle and adds only DLSS-NR on D3D12.
        auto native_sr = [&]() noexcept { return original(context, handle, parameters, callback); };
        const auto native = make_d3d11_native_evaluate(native_sr);
        if (evaluate_d3d11_via_d3d12(
                context, handle, parameters, settings,
                current_transport_ngx(runtime), transport_result, &native)) {''', count=2)
replace(transport, '''    const D3D11TransportNgx& ngx,
    NgxResult& result
) noexcept {
    // The core can forward''', '''    const D3D11TransportNgx& ngx,
    NgxResult& result,
    const D3D11NativeEvaluate* const native
) noexcept {
    // The core can forward''')
replace(transport, '''    return slot.nr_before == nr_before && slot.color.resource12 != nullptr &&''',
    '''    return slot.nr_before == nr_before && (slot.native_sr || slot.color.resource12 != nullptr) &&''')
replace(transport, '''    if (!create_shared_texture(device, "color",''', '''    if (textures.native_sr) {
        release_shared_texture(textures.output);
        release_shared_texture(textures.motion_vectors);
        release_shared_texture(textures.depth);
        release_shared_texture(textures.color);
    }
    if ((!textures.native_sr && (!create_shared_texture(device, "color",''')
replace(transport, '''            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, textures.output) ||''',
    '''            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, textures.output))) ||''')
replace(transport, '''    const auto nr_processing_width = nr_before ? render_width : out_width;''', '''    // NR-only after upscaling needs no private SR feature: the game's own DX11
    // evaluation produces the upscaled output that NR consumes. That holds for the
    // whole view and for an independent NR region (NrFoveated without
    // NrUseSrFoveation, e.g. the centre of a Quad Views focus view): the NR stage
    // below copies only that region and writes back its feathered result.
    const bool native_sr = native != nullptr && native->invoke != nullptr && !settings.enabled &&
        settings.nr_enabled && (!settings.nr_foveated || !settings.nr_use_sr_foveation) &&
        !settings.peripheral_dlaa_enabled &&
        settings.nr_processing_order != NrProcessingOrder::before_upscaling;
    const auto nr_processing_width = nr_before ? render_width : out_width;''')
replace(transport, '''    D3D11_BOX color_box{''', '''    if (!native_sr) { // The game's own DLSS reads its inputs directly.
    D3D11_BOX color_box{''')
replace(transport, '''        0U, 0U, 0U, motion, 0U, &mv_box);
''', '''        0U, 0U, 0U, motion, 0U, &mv_box);
    }
''')
replace(transport, '''
    const auto ready_value = device->next_fence_value++;''', '''
    const bool measure_dlss = !native_sr && slot.dlss_timing_heap != nullptr &&
        slot.dlss_timing_readback != nullptr &&
        device->timestamp_frequency != 0U &&
        diagnostic_should_sample_gpu_time(
            DiagnosticGpuTiming::foveated_dlss
        );
    ID3D12CommandList* lists[]{device->command_list12};
    struct InputHistoryCompletion {
        DlssViewId view;
        NrProcessingOrder order;
        std::uint32_t width, height;
        bool complete{};
        ~InputHistoryCompletion() {
            if (!complete) static_cast<void>(dlss_nr_input_history_reset(view, order, false, width, height));
        }
    } input_completion{contract.view_id, settings.nr_processing_order, render_width, render_height};
    if (native_sr) {
        contract.reset = dlss_nr_input_history_reset(contract.view_id, settings.nr_processing_order,
            false, render_width, render_height) || contract.reset;
        result = native->invoke(native->state);
        if (!ngx_succeeded(result)) {
            // The game's own evaluation ran and failed: report it, never run it twice.
            release(context4);
            diagnostic_note_d3d11_transport_status(D3D11TransportStatus::ngx_evaluation_failed);
            return true;
        }
        static std::atomic<std::uint64_t> native_frames{};
        const auto native_count = ++native_frames;
        if (native_count <= 4U || (native_count & (native_count - 1U)) == 0U)
            trace_event("DX11 transport NR-only: game DX11 DLSS evaluated natively; DLSS-NR only on D3D12 view=%llu frames=%llu",
                static_cast<unsigned long long>(contract.view_id), static_cast<unsigned long long>(native_count));
    } else {
    const auto ready_value = device->next_fence_value++;''')
replace(transport, '''    const bool measure_dlss = slot.dlss_timing_heap != nullptr &&
        slot.dlss_timing_readback != nullptr &&
        device->timestamp_frequency != 0U &&
        diagnostic_should_sample_gpu_time(
            DiagnosticGpuTiming::foveated_dlss
        );
    if (measure_dlss) {''', '''    // measure_dlss is decided before the native/private SR split.
    if (measure_dlss) {''')
replace(transport, '''    struct InputHistoryCompletion {
        DlssViewId view;
        NrProcessingOrder order;
        std::uint32_t width, height;
        bool complete{};
        ~InputHistoryCompletion() {
            if (!complete) static_cast<void>(dlss_nr_input_history_reset(view, order, false, width, height));
        }
    } input_completion{contract.view_id, settings.nr_processing_order, render_width, render_height};
    contract.reset = dlss_nr_input_history_reset(contract.view_id, settings.nr_processing_order,
        nr_before_succeeded,''', '''    // input_completion is declared before the native/private SR split.
    contract.reset = dlss_nr_input_history_reset(contract.view_id, settings.nr_processing_order,
        nr_before_succeeded,''')
replace(transport, '''    ID3D12CommandList* lists[]{device->command_list12};
    device->queue12->ExecuteCommandLists(1U, lists);''', '''    // lists is declared before the native/private SR split.
    device->queue12->ExecuteCommandLists(1U, lists);''')
replace(transport, '''        return reject_transport(D3D11TransportStatus::compositing_failed);
    }

    bool nr_succeeded{};''', '''        return reject_transport(D3D11TransportStatus::compositing_failed);
    }
    } // private D3D12 SR (skipped when native_sr)

    bool nr_succeeded{};''')

# Depth conversion views: the game's depth SRV and each shared texture's UAV were created and destroyed on every
# conversion (SR + NR, every view, every frame). Cache them; the cached SRV keeps its source alive, so a pointer can
# never be reused for a different resource while cached. The cache is device-owned and returns AddRef'ed views, so the
# conversion's existing release calls stay balanced.
replace(transport, '''struct SharedTexture {
    ID3D12Resource* resource12{};
    ID3D11Texture2D* texture11{};''', '''struct SharedTexture {
    ID3D12Resource* resource12{};
    ID3D11Texture2D* texture11{};
    ID3D11UnorderedAccessView* uav11{}; // Cached R32_FLOAT UAV for depth conversion.''')
replace(transport, '''void release_shared_texture(SharedTexture& texture) noexcept {
    release(texture.texture11);''', '''void release_shared_texture(SharedTexture& texture) noexcept {
    release(texture.uav11);
    release(texture.texture11);''')
replace(transport, '''    HANDLE slot_ready_event{};
    std::uint64_t slot_waits{};''', '''    HANDLE slot_ready_event{};
    std::uint64_t slot_waits{};
    struct DepthSourceView { ID3D11Resource* source{}; DXGI_FORMAT format{}; ID3D11ShaderResourceView* srv{}; };
    std::array<DepthSourceView, 4> depth_source_views{};
    std::uint32_t next_depth_source_view{};''')
replace(transport, '''    release(device.depth_constants);''', '''    for (auto& cached : device.depth_source_views) { release(cached.srv); cached = {}; }
    release(device.depth_constants);''')
replace(transport, '''[[nodiscard]] bool convert_depth_crop(''', '''[[nodiscard]] HRESULT cached_depth_srv(TransportDevice& device, ID3D11Resource* const depth,
    const D3D11_SHADER_RESOURCE_VIEW_DESC& desc, ID3D11ShaderResourceView*& srv) noexcept {
    for (auto& cached : device.depth_source_views) {
        if (cached.srv && cached.source == depth && cached.format == desc.Format) {
            srv = cached.srv; srv->AddRef(); return S_OK;
        }
    }
    const auto result = device.device11->CreateShaderResourceView(depth, &desc, &srv);
    if (FAILED(result)) return result;
    auto& slot = device.depth_source_views[device.next_depth_source_view++ % device.depth_source_views.size()];
    release(slot.srv);
    slot = {depth, desc.Format, srv};
    srv->AddRef();
    return result;
}

[[nodiscard]] HRESULT cached_shared_uav(TransportDevice& device, SharedTexture& texture,
    const D3D11_UNORDERED_ACCESS_VIEW_DESC& desc, ID3D11UnorderedAccessView*& uav) noexcept {
    if (!texture.uav11) {
        const auto result = device.device11->CreateUnorderedAccessView(texture.texture11, &desc, &texture.uav11);
        if (FAILED(result)) return result;
    }
    uav = texture.uav11; uav->AddRef();
    return S_OK;
}

[[nodiscard]] bool convert_depth_crop(''')
replace(transport, '''    const auto srv_result=device.device11->CreateShaderResourceView(depth, &srv_desc, &srv);''',
    '''    const auto srv_result=cached_depth_srv(device, depth, srv_desc, srv);''')
replace(transport, '''    const auto uav_result=device.device11->CreateUnorderedAccessView(destination.texture11, &uav_desc, &uav);''',
    '''    const auto uav_result=cached_shared_uav(device, destination, uav_desc, uav);''')

# DLSS-NR codec cache: entries (region*8 + working*28 bytes each) were kept until the six-entry cap even after a resize
# or order change made them unreachable. Release entries idle for 120 NR calls of the view once their recordings
# completed. With one transport texture set per view the steady state is one codec entry per view.
replace('src/dlss_nr.cpp', '''    const std::uint32_t working_height,
    const bool border_only = false
) noexcept {
    for (auto& gpu : view.gpu_resources) {''', '''    const std::uint32_t working_height,
    const bool border_only = false
) noexcept {
    for (auto it = view.gpu_resources.begin(); it != view.gpu_resources.end();) {
        if (it->last_use + 120U < view.gpu_use_sequence) {
            it->uses.collect();
            if (it->uses.empty()) {
                release_gpu(*it);
                it = view.gpu_resources.erase(it);
                continue;
            }
        }
        ++it;
    }
    for (auto& gpu : view.gpu_resources) {''')

# DLSS 5 area (DCS VR Control): an independent central NR region (NrFoveated without NrUseSrFoveation) keeps the
# native DX11 SR path; only that region is copied to D3D12, processed and written back with its feathered edge.
replace('tests/runtime_host_tests.cpp', """    require(contains(snapshot(get), "\\"nr\\":\\"Active\\""), "NR active on the native DX11 SR output");
    context->Flush();""", """    require(contains(snapshot(get), "\\"nr\\":\\"Active\\""), "NR active on the native DX11 SR output");
    {
        const auto region_evaluates_before = proc<unsigned(*)()>(ngx, "CheekyFakeEvaluates")();
        require(command(attachment, "1\\n32\\nset\\nNrFoveated=true\\nNrUseSrFoveation=false\\nNrWidth=0.7\\nNrHeight=0.7"), "Select a central NR region");
        frames();
        frames();
        const auto text = snapshot(get);
        const auto details = text.substr(text.find("\\"nr_details\\":"));
        const auto region_evaluates = proc<unsigned(*)()>(ngx, "CheekyFakeEvaluates")() - region_evaluates_before;
        printf("NR-only (after) central region %gx%g of %gx%g native DX11 evaluates=%u\\n", field(details, "region_width"),
            field(details, "region_height"), field(details, "output_width"), field(details, "output_height"), region_evaluates);
        require(region_evaluates >= 12, "A central NR region keeps the game's DX11 evaluate on every frame");
        require(log_count("D3D12 canonical create") == canonical_before, "A central NR region creates no private D3D12 SR feature");
        require(contains(text, "\\"nr\\":\\"Active\\""), "NR active on the central region of the native DX11 SR output");
        require(field(details, "region_width") < field(details, "output_width") &&
            field(details, "region_height") < field(details, "output_height"), "NR covers only the central region");
        require(command(attachment, "1\\n33\\nset\\nNrFoveated=false"), "Restore whole-view NR");
        frames();
    }
    context->Flush();""")

# Feature ledger (DCS VR Control): every game-visible DLSS create/release is logged with process VRAM before/after,
# the number of live game features and each feature's evaluation count, so a log shows whether a recreated set of
# features replaced the old one (old handles released) or leaked it (never evaluated, never released).
hooks = 'src/hooks.cpp'
shutil.copyfile(root / 'patches/cheeky/dcs_feature_ledger.inc', source / 'src/dcs_feature_ledger.inc')
shutil.copyfile(root / 'patches/cheeky/dcs_deferred_feature.inc', source / 'src/dcs_deferred_feature.inc')
replace(hooks, '#include <MinHook.h>\n', '#include <MinHook.h>\n#include <dxgi1_4.h>\n')
replace(hooks, '// Bound creation diagnostics per route and feature: first eight, then powers of two.',
    '#include "dcs_feature_ledger.inc"\n\n// Bound creation diagnostics per route and feature: first eight, then powers of two.')
replace(hooks, '''    const auto result = original(context, feature, parameters, handle);
    if (report) trace_event("FEATURE_CREATE end''', '''    const auto dcs_vram_before = dcs_vram_sample(context);
    const auto result = original(context, feature, parameters, handle);
    // D3D11 routes are the game's features (D3D12 creates include the private transport features).
    dcs_trace_feature_create(route, feature, parameters, result, ngx_succeeded(result) && handle ? *handle : nullptr,
        dcs_vram_before, dcs_vram_sample(context), GetTickCount64() - creation_started,
        route < 2U && !dcs_materializing_feature);
    if (report) trace_event("FEATURE_CREATE end''')
replace(hooks, '''    if (depth.outer && !is_d3d11_private_handle(handle))
        log_ngx_exposure<ID3D11Resource>(11, handle, parameters);
    const auto result = invoke();''', '''    bool dcs_first_evaluation{};
    DcsVram dcs_first_before{};
    if (depth.outer && !is_d3d11_private_handle(handle)) {
        log_ngx_exposure<ID3D11Resource>(11, handle, parameters);
        // NGX allocates a feature's working set on its first evaluation, not at creation.
        dcs_first_evaluation = dcs_ledger_evaluated(handle);
        if (dcs_first_evaluation) dcs_first_before = dcs_vram_sample(context);
    }
    const auto result = invoke();
    if (dcs_first_evaluation) {
        const auto dcs_first_after = dcs_vram_sample(context);
        trace_event("FEATURE_VRAM first-evaluation handle=%p result=0x%08X vram_before_mb=%.1f vram_after_mb=%.1f "
            "delta_mb=%+.1f (whichever path evaluated it: the game's own feature or the transport)", handle,
            unsigned(result), dcs_mb(dcs_first_before.usage), dcs_mb(dcs_first_after.usage),
            dcs_delta_mb(dcs_first_before, dcs_first_after));
    }''')
replace(hooks, 'NgxResult runtime_release_d3d11(const D3D11RuntimeCallbacks& runtime, NgxHandle* const handle) {',
    '#include "dcs_deferred_feature.inc"\n\nNgxResult runtime_release_d3d11(const D3D11RuntimeCallbacks& runtime, NgxHandle* const handle) {')
replace(hooks, '''    forget_gaze_view(view_id);
    return original == nullptr ? 0xBAD00007U : original(handle);
}

template<std::size_t Slot> struct D3D11RuntimeHooks {''', '''    forget_gaze_view(view_id);
    return dcs_traced_feature_release(0U, handle, [&]() -> NgxResult {
        return original == nullptr ? 0xBAD00007U : original(handle);
    });
}

template<std::size_t Slot> struct D3D11RuntimeHooks {''')
replace(hooks, '''    forget_gaze_view(view_id);
    return original == nullptr ? 0xBAD00007U : original(handle);
}

NgxResult runtime_create_d3d12(''', '''    forget_gaze_view(view_id);
    return dcs_traced_feature_release(1U, handle, [&]() -> NgxResult {
        return original == nullptr ? 0xBAD00007U : original(handle);
    });
}

NgxResult runtime_create_d3d12(''')
# Deferred DX11 game feature for Quad Views focus views (see dcs_deferred_feature.inc).
replace(hooks, '''NgxResult hook_core_release_d3d11(NgxHandle* const handle) {
    const auto original = real_core_release_d3d11.load(std::memory_order_acquire);''', '''NgxResult hook_core_release_d3d11(NgxHandle* const handle) {
    if (NgxResult deferred{}; dcs_release_deferred_d3d11(handle, deferred)) return deferred;
    const auto original = real_core_release_d3d11.load(std::memory_order_acquire);''')
replace(hooks, '''    const auto original = real_core_create_d3d11.load(std::memory_order_acquire);
    if (original == nullptr) return 0xBAD00007U;
    diagnostic_note_create(DiagnosticApi::d3d11);
''', '''    const auto original = real_core_create_d3d11.load(std::memory_order_acquire);
    if (original == nullptr) return 0xBAD00007U;
    diagnostic_note_create(DiagnosticApi::d3d11);
    if (NgxResult deferred{}; dcs_try_defer_d3d11_create(context, feature, parameters, handle, deferred)) return deferred;
''')
# The nested snippet create of a materialized feature belongs to the opaque handle DCS holds.
replace(hooks, '''    const auto result = traced_feature_create(original, context, feature, parameters, handle, 0);
    if (ngx_succeeded(result) && handle != nullptr && *handle != nullptr &&
        feature == 1U) {''', '''    const auto result = traced_feature_create(original, context, feature, parameters, handle, 0);
    if (ngx_succeeded(result) && handle != nullptr && *handle != nullptr &&
        feature == 1U && !dcs_materializing_feature) {''')
replace(hooks, '''        installed |= install_direct_hook(
            core_runtime,
            "NVSDK_NGX_D3D11_ReleaseFeature",
            reinterpret_cast<void*>(&hook_core_release_d3d11),
            real_core_release_d3d11,
            DiagnosticApi::d3d11
        );
''', '''        installed |= install_direct_hook(
            core_runtime,
            "NVSDK_NGX_D3D11_ReleaseFeature",
            reinterpret_cast<void*>(&hook_core_release_d3d11),
            real_core_release_d3d11,
            DiagnosticApi::d3d11
        );
        // DCS VR Control: opaque deferred handles must never reach the core.
        if (dcs_quad_focus_env()) {
            installed |= install_direct_hook(core_runtime, "NVSDK_NGX_D3D11_EvaluateFeature",
                reinterpret_cast<void*>(&hook_core_evaluate_d3d11), real_core_evaluate_d3d11, DiagnosticApi::d3d11);
            installed |= install_direct_hook(core_runtime, "NVSDK_NGX_D3D11_EvaluateFeature_C",
                reinterpret_cast<void*>(&hook_core_evaluate_d3d11_c), real_core_evaluate_d3d11_c, DiagnosticApi::d3d11);
            installed |= install_direct_hook(core_runtime, "NVSDK_NGX_D3D11_Shutdown",
                reinterpret_cast<void*>(&hook_core_shutdown_d3d11), real_core_shutdown_d3d11, DiagnosticApi::d3d11);
            installed |= install_direct_hook(core_runtime, "NVSDK_NGX_D3D11_Shutdown1",
                reinterpret_cast<void*>(&hook_core_shutdown_d3d11_1), real_core_shutdown_d3d11_1, DiagnosticApi::d3d11);
        }
''')
replace(hooks, '''        CHEEKY_REPLACE("NVSDK_NGX_D3D11_ReleaseFeature", real_core_release_d3d11, hook_core_release_d3d11, DiagnosticApi::d3d11)
''', '''        CHEEKY_REPLACE("NVSDK_NGX_D3D11_ReleaseFeature", real_core_release_d3d11, hook_core_release_d3d11, DiagnosticApi::d3d11)
        if (dcs_quad_focus_env()) {
            CHEEKY_REPLACE("NVSDK_NGX_D3D11_EvaluateFeature", real_core_evaluate_d3d11, hook_core_evaluate_d3d11, DiagnosticApi::d3d11)
            CHEEKY_REPLACE("NVSDK_NGX_D3D11_EvaluateFeature_C", real_core_evaluate_d3d11_c, hook_core_evaluate_d3d11_c, DiagnosticApi::d3d11)
            CHEEKY_REPLACE("NVSDK_NGX_D3D11_Shutdown", real_core_shutdown_d3d11, hook_core_shutdown_d3d11, DiagnosticApi::d3d11)
            CHEEKY_REPLACE("NVSDK_NGX_D3D11_Shutdown1", real_core_shutdown_d3d11_1, hook_core_shutdown_d3d11_1, DiagnosticApi::d3d11)
        }
''')
replace('src/gaze_foveation.hpp', 'bool dcs_quad_focus_processing_allowed(DlssViewId', 'int dcs_quad_focus_create_extent(unsigned width, unsigned height) noexcept;\nbool dcs_quad_focus_processing_allowed(DlssViewId')

# NR before upscaling in place (DCS VR Control): when the SR input already covers the whole render view (no SR crop,
# supersampling 1, no peripheral DLAA) and its depth/motion copies are exactly NR's guide regions, NR reads the SR
# input textures and writes its result straight into the SR colour input. The separate NR colour/depth/motion
# textures, their three copies and the copy-back disappear; every byte DLSS-NR and SR read is unchanged.
# (The texture-set field and the initialize_slot parameter/argument live in the one-texture-set block above.)
replace(transport, '''    const DXGI_FORMAT output_format,
    const bool nr_before
) noexcept {
    return slot.nr_before == nr_before &&''', '''    const DXGI_FORMAT output_format,
    const bool nr_before,
    const bool nr_in_place
) noexcept {
    return slot.nr_before == nr_before &&''')
replace(transport, '''        (!nr_enabled || (
            slot.nr_color.resource12 != nullptr &&''', '''        slot.nr_in_place == nr_in_place &&
        (!nr_enabled || nr_in_place || (
            slot.nr_color.resource12 != nullptr &&''')
replace(transport, '''    if (!nr_enabled) {
        release_shared_texture(textures.nr_color);''', '''    if (!nr_enabled || nr_in_place) {
        release_shared_texture(textures.nr_color);''')
replace(transport, '''    if ((!textures.native_sr && (!create_shared_texture(device, "color",
            crop.input_width, crop.input_height, color_format,
            D3D12_RESOURCE_FLAG_NONE, textures.color) ||''', '''    if ((!textures.native_sr && (!create_shared_texture(device, "color",
            crop.input_width, crop.input_height, color_format,
            nr_in_place ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE, textures.color) ||''')
replace(transport, '''        (nr_enabled && (
            !create_shared_texture(device, "NR composited color",''', '''        (nr_enabled && !nr_in_place && (
            !create_shared_texture(device, "NR composited color",''')
replace(transport, '    textures.nr_before = nr_before;\n', '    textures.nr_before = nr_before;\n    textures.nr_in_place = nr_in_place;\n')
replace(transport, '''    const auto nr_mv_capacity_y = settings.nr_enabled
        ? scaled_capacity(nr_geometry.height, nr_motion_height, nr_processing_height) : 0U;
''', '''    const auto nr_mv_capacity_y = settings.nr_enabled
        ? scaled_capacity(nr_geometry.height, nr_motion_height, nr_processing_height) : 0U;
    // DCS VR Control: NR before upscaling in place on the SR input. Only when the
    // SR input copies are byte-for-byte NR's inputs: the whole render colour,
    // the same depth region and the same motion region (same texture extents).
    const bool nr_in_place = nr_before && !native_sr && !settings.peripheral_dlaa_enabled &&
        reconstruction.input_width == crop.input_width && reconstruction.input_height == crop.input_height &&
        crop.input_base_x == 0U && crop.input_base_y == 0U &&
        crop.input_width == render_width && crop.input_height == render_height &&
        nr_depth_x.base == 0U && nr_depth_y.base == 0U &&
        nr_depth_x.extent == render_width && nr_depth_y.extent == render_height &&
        mv_crop_x == nr_mv_region_x.base && mv_crop_y == nr_mv_region_y.base &&
        mv_width == nr_mv_region_x.extent && mv_height == nr_mv_region_y.extent &&
        mv_width == nr_motion_width && mv_height == nr_motion_height;
''')
replace(transport, '''            color_desc.Format, motion_desc.Format, output_desc.Format, nr_before
        );''', '''            color_desc.Format, motion_desc.Format, output_desc.Format, nr_before, nr_in_place
        );''')
replace(transport, '''    if (settings.nr_enabled) {
        if (!convert_depth_crop(
                *device, context, depth, depth_desc.Format,
                depth_x + nr_depth_x.base,''', '''    if (settings.nr_enabled && !nr_in_place) { // In place: NR reads the SR depth/motion copies above.
        if (!convert_depth_crop(
                *device, context, depth, depth_desc.Format,
                depth_x + nr_depth_x.base,''')
replace(transport, '''    if (nr_before) {
        const D3D11_BOX nr_box{color_x, color_y, 0U,''', '''    if (nr_before && !nr_in_place) {
        const D3D11_BOX nr_box{color_x, color_y, 0U,''')
replace(transport, '''        slot.dlss_nr_timing_foveated = settings.nr_foveated;
        transition(device->command_list12, textures.nr_color.resource12,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(device->command_list12, textures.nr_depth.resource12,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(device->command_list12, textures.nr_motion_vectors.resource12,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        DlssNrFrame nr_frame{contract.view_id, DlssNrRoute::d3d11_transport,
            device->command_list12, textures.nr_color.resource12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            textures.nr_depth.resource12, textures.nr_motion_vectors.resource12,''', '''        slot.dlss_nr_timing_foveated = settings.nr_foveated;
        // In place, the SR inputs are already readable (NON_PIXEL_SHADER_RESOURCE).
        auto& nr_color_input = nr_in_place ? textures.color : textures.nr_color;
        auto& nr_depth_input = nr_in_place ? textures.depth : textures.nr_depth;
        auto& nr_motion_input = nr_in_place ? textures.motion_vectors : textures.nr_motion_vectors;
        transition(device->command_list12, nr_color_input.resource12,
            nr_in_place ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!nr_in_place) {
        transition(device->command_list12, textures.nr_depth.resource12,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(device->command_list12, textures.nr_motion_vectors.resource12,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        DlssNrFrame nr_frame{contract.view_id, DlssNrRoute::d3d11_transport,
            device->command_list12, nr_color_input.resource12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nr_depth_input.resource12, nr_motion_input.resource12,''')
replace(transport, '''        nr_before_succeeded = evaluate_dlss_nr(nr_frame, processing_settings);
        transition(device->command_list12, textures.nr_depth.resource12,''', '''        nr_before_succeeded = evaluate_dlss_nr(nr_frame, processing_settings);
        if (nr_in_place) {
            // NR wrote its result into the SR colour input; on failure it is untouched.
            transition(device->command_list12, textures.color.resource12,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
        transition(device->command_list12, textures.nr_depth.resource12,''')
replace(transport, '''        transition(device->command_list12, textures.nr_color.resource12,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (nr_before && measure_dlss) {''', '''        transition(device->command_list12, textures.nr_color.resource12,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } // separate NR textures
    }
    if (nr_before && measure_dlss) {''')
replace(transport, '''    if (nr_before) transition(device->command_list12, textures.nr_color.resource12,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);''', '''    if (nr_before && !nr_in_place) transition(device->command_list12, textures.nr_color.resource12,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);''')
replace(transport, '''    ID3D11Resource* composite_base = nr_before_succeeded ? textures.nr_color.texture11 : color;''',
    '''    ID3D11Resource* composite_base = nr_before_succeeded
        ? (nr_in_place ? textures.color.texture11 : textures.nr_color.texture11) : color;''')

# Deferred DX11 feature regression test on the generic DX11 host (fixture core forwarding to the snippet).
shutil.copyfile(root / 'patches/cheeky/dcs_deferred_tests.inc', source / 'tests/dcs_deferred_tests.inc')
replace('tests/runtime_host_tests.cpp', '''int main(int argc, char** argv) {
    try {
        bool ota_transport{}, ota_only{};''', '''bool dcs_deferred_mode{};
int dcs_deferred_scenario{};
#include "dcs_deferred_tests.inc"

int main(int argc, char** argv) {
    try {
        bool ota_transport{}, ota_only{};''')
replace('tests/runtime_host_tests.cpp', '''            else if (arg == "--transport-forwarded") { transport = true; dx11 = true; forwarded_transport = true; }
''', '''            else if (arg == "--transport-forwarded") { transport = true; dx11 = true; forwarded_transport = true; }
            else if (arg == "--transport-dcs-deferred") { transport = dx11 = dcs_deferred_mode = true; }
            else if (arg == "--transport-dcs-deferred-unproven") { transport = dx11 = dcs_deferred_mode = true; dcs_deferred_scenario = 1; }
            else if (arg == "--transport-dcs-deferred-nr-off") { transport = dx11 = dcs_deferred_mode = true; dcs_deferred_scenario = 2; }
''')
replace('tests/runtime_host_tests.cpp', '''                proc<void(*)(HMODULE, bool)>(GetModuleHandleW(L"_nvngx.dll"),
                    "CheekyFakeForwardTo")(fake_ngx, false);
            }
''', '''                proc<void(*)(HMODULE, bool)>(GetModuleHandleW(L"_nvngx.dll"),
                    "CheekyFakeForwardTo")(fake_ngx, false);
            }
            if (dcs_deferred_mode) {
                // DCS: the game calls the core, which dispatches DX11 features into the SR snippet.
                SetEnvironmentVariableW(L"DCSVR_QUAD_FOCUS", L"1");
                proc<void(*)(HMODULE)>(GetModuleHandleW(L"_nvngx.dll"), "CheekyFakeForwardDX11To")(fake_ngx);
            }
''')
replace('tests/runtime_host_tests.cpp', '''        if (transport) verify_transport(command, get, attachment,''', '''        if (dcs_deferred_mode) {
            verify_dcs_deferred(command, get, attachment, device11.Get(), context11.Get(), fake_ngx,
                directory / "CheekyFoveatedDLSS-Standalone.log", bin, dcs_deferred_scenario);
            detach(attachment);
            puts(dcs_deferred_scenario == 1 ? "PASS: DCS deferred focus view before the Quad Views proof: private D3D12 SR without NR, NR on the same textures, never DCS's own feature"
                : dcs_deferred_scenario == 2 ? "PASS: DCS deferred focus view with DLSS 5 off in flight: private D3D12 SR without NR, never DCS's own feature"
                : "PASS: DCS deferred DX11 feature: no VRAM until evaluated, bounded bilinear hold without a layout, then materialized once, released once");
            return 0;
        }
        if (transport) verify_transport(command, get, attachment,''')
replace('cmake/Standalone.cmake', '''    add_test(NAME CheekyRuntimeStandalone-TransportInitFailure COMMAND CheekyRuntimeHostTests --transport-init-failure)
''', '''    add_test(NAME CheekyRuntimeStandalone-TransportInitFailure COMMAND CheekyRuntimeHostTests --transport-init-failure)
    add_test(NAME CheekyRuntimeStandalone-TransportDcsDeferred COMMAND CheekyRuntimeHostTests --transport-dcs-deferred)
    add_test(NAME CheekyRuntimeStandalone-TransportDcsDeferredUnproven COMMAND CheekyRuntimeHostTests --transport-dcs-deferred-unproven)
    add_test(NAME CheekyRuntimeStandalone-TransportDcsDeferredNrOff COMMAND CheekyRuntimeHostTests --transport-dcs-deferred-nr-off)
''')

# DCS VR Control: a deferred Quad Views focus view never creates DCS's own DX11 DLSS feature (whose VRAM NGX keeps
# until shutdown) while the private D3D12 SR can produce it: before the Quad Views proof (first frames after a mission
# load) and with DLSS 5 toggled off in flight the transport runs SR without NR on the same feature and textures; with no
# layout at all a bounded bilinear hold (dcs_hold_upscale.hpp). The genuine fallbacks still create it.
shutil.copyfile(root / 'patches/cheeky/dcs_hold_upscale.hpp', source / 'src/dcs_hold_upscale.hpp')
replace(hooks, '#include "settings.hpp"\n', '#include "settings.hpp"\n#include "dcs_hold_upscale.hpp"\n')
replace('src/settings.hpp', """    float nr_motion_scale_y_multiplier{1.0F};
};""", """    float nr_motion_scale_y_multiplier{1.0F};
    // DCS VR Control (transient, never persisted): the D3D11 transport runs SR only and skips the NR pass.
    bool dcs_nr_suppressed{};
};""")
replace(transport, """    } nr_attempt{settings, view_id};
""", """    } nr_attempt{settings, view_id};
    // DCS VR Control: SR only (the caller holds NR off and has reset its history).
    if (settings.dcs_nr_suppressed) nr_attempt.attempted = true;
""")
replace(transport, """    bool nr_before_succeeded{};
    if (nr_before) {
        if (measure_dlss) device->command_list12->EndQuery(slot.dlss_timing_heap, D3D12_QUERY_TYPE_TIMESTAMP, 4U);
        slot.dlss_nr_timing_foveated = settings.nr_foveated;
""", """    bool nr_before_succeeded{};
    if (nr_before && settings.dcs_nr_suppressed) {
        // DCS VR Control: SR only. Same textures and private SR feature as with NR; the NR pass is skipped.
        if (measure_dlss) device->command_list12->EndQuery(slot.dlss_timing_heap, D3D12_QUERY_TYPE_TIMESTAMP, 4U);
        slot.dlss_nr_timing_foveated = settings.nr_foveated;
    } else if (nr_before) {
        if (measure_dlss) device->command_list12->EndQuery(slot.dlss_timing_heap, D3D12_QUERY_TYPE_TIMESTAMP, 4U);
        slot.dlss_nr_timing_foveated = settings.nr_foveated;
""")
replace(transport, '    if (settings.nr_enabled && !nr_before) {\n', '    if (settings.nr_enabled && !nr_before && !settings.dcs_nr_suppressed) {\n')
replace(hooks, """            get_ui(parameters, "OutWidth"), get_ui(parameters, "OutHeight"), dcs_reset_original)) {
        release_d3d11_transport_view(handle);""", """            get_ui(parameters, "OutWidth"), get_ui(parameters, "OutHeight"), dcs_reset_original)) {
        // DCS VR Control: a deferred focus view whose proof is not ready yet never creates DCS's own feature.
        if (NgxResult held{}; dcs_deferred_hold_unproven(runtime, context, handle, parameters, held)) return held;
        release_d3d11_transport_view(handle);""", count=2)
replace(hooks, """    if (!settings.enabled &&
        !(settings.nr_enabled && settings.d3d11_use_d3d12_transport)) {
        diagnostic_note_state(DiagnosticApi::d3d11, DiagnosticState::disabled);""", """    if (!settings.enabled &&
        !(settings.nr_enabled && settings.d3d11_use_d3d12_transport)) {
        // DCS VR Control: DLSS 5 off in flight keeps a deferred focus view on the private D3D12 SR.
        if (NgxResult kept{}; dcs_deferred_keep_transport(runtime, context, handle, parameters, settings, kept)) return kept;
        diagnostic_note_state(DiagnosticApi::d3d11, DiagnosticState::disabled);""", count=2)
# The private SR of a deferred view (with or without NR) always sees DCS's creation-time DLSS parameters.
replace(hooks, """    if (settings.d3d11_use_d3d12_transport) {
        NgxPresetOverrideScope center_preset{
            parameters, settings.center_preset
        };""", """    if (settings.d3d11_use_d3d12_transport) {
        // DCS VR Control: a deferred view's private SR uses DCS's creation-time DLSS parameters.
        DcsDeferredCreateValuesScope dcs_create_values{handle, parameters};
        NgxPresetOverrideScope center_preset{
            parameters, settings.center_preset
        };""", count=2)
# Test fixture named CheekyOpenXRLayer.dll publishing the gate's snapshot and four-view layout.
shutil.copyfile(root / 'patches/cheeky/dcs_fake_layer.cpp', source / 'tests/dcs_fake_layer.cpp')
replace('cmake/Standalone.cmake', """add_dependencies(CheekyRuntimeHostTests CheekyFoveatedDLSSRuntime)
""", """add_dependencies(CheekyRuntimeHostTests CheekyFoveatedDLSSRuntime)
add_library(CheekyFakeDcsLayer MODULE tests/dcs_fake_layer.cpp)
target_include_directories(CheekyFakeDcsLayer PRIVATE src shared)
target_compile_features(CheekyFakeDcsLayer PRIVATE cxx_std_20)
target_compile_definitions(CheekyFakeDcsLayer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_target_properties(CheekyFakeDcsLayer PROPERTIES PREFIX "" OUTPUT_NAME CheekyOpenXRLayer
    ARCHIVE_OUTPUT_NAME CheekyFakeDcsLayer PDB_NAME CheekyFakeDcsLayer
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/test-fixtures/dcs-layer"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/test-fixtures/dcs-layer")
add_dependencies(CheekyRuntimeHostTests CheekyFakeDcsLayer)
""")

# The transport's GPU-time readback polls its queries without flushing DCS's
# immediate context (a result not ready yet is simply read on a later frame),
# as the hooks' own timing already does.
replace(transport, '''    if (context->GetData(slot.timing_disjoint, &disjoint, sizeof(disjoint), 0U) != S_OK ||
        context->GetData(slot.timing_begin, &begin, sizeof(begin), 0U) != S_OK ||
        context->GetData(slot.timing_end, &end, sizeof(end), 0U) != S_OK) {''', '''    constexpr UINT no_flush = D3D11_ASYNC_GETDATA_DONOTFLUSH;
    if (context->GetData(slot.timing_disjoint, &disjoint, sizeof(disjoint), no_flush) != S_OK ||
        context->GetData(slot.timing_begin, &begin, sizeof(begin), no_flush) != S_OK ||
        context->GetData(slot.timing_end, &end, sizeof(end), no_flush) != S_OK) {''')

# One set of transport textures for the device's views. Nothing in them
# outlives a frame (every frame transitions them COMMON -> use -> COMMON), and
# the views run strictly one after the other: D3D11 queues its wait for a
# view's private D3D12 work before any later D3D11 command, the next view's
# input copies included. So the two Quad Views focus views can use the same
# textures; a view adopts another view's texture of the same role, size,
# format and flags (one COM reference each, so either view's release keeps
# the other's alive). An unordered view (an error path that could not queue
# that wait) now makes every view of the device wait on the CPU.
replace(transport, '''    if (texture.resource12 && texture.texture11 && texture.width == width &&
        texture.height == height && texture.format == format && texture.flags == flags) return true;
''', '''    if (texture.resource12 && texture.texture11 && texture.width == width &&
        texture.height == height && texture.format == format && texture.flags == flags) return true;
    // DCS VR Control: another view's texture in the same role (the same member
    // of TransportTextures), size, format and flags, shared with one more
    // reference instead of a new allocation.
    const TransportTextures* own{};
    for (const auto& view : device.views) {
        const auto* begin = reinterpret_cast<const char*>(&view.textures);
        const auto* at = reinterpret_cast<const char*>(&texture);
        if (at >= begin && at < begin + sizeof(TransportTextures)) { own = &view.textures; break; }
    }
    if (own) {
        const auto offset = reinterpret_cast<const char*>(&texture) - reinterpret_cast<const char*>(own);
        for (const auto& view : device.views) {
            if (&view.textures == own) continue;
            const auto& other = *reinterpret_cast<const SharedTexture*>(
                reinterpret_cast<const char*>(&view.textures) + offset);
            if (!other.resource12 || !other.texture11 || other.width != width || other.height != height ||
                other.format != format || other.flags != flags) continue;
            release_shared_texture(texture);
            texture = other;
            texture.resource12->AddRef();
            texture.texture11->AddRef();
            if (texture.uav11) texture.uav11->AddRef();
            trace_event("Transport texture %s %ux%u shared with view=%llu", label, width, height,
                static_cast<unsigned long long>(view.view_id));
            return true;
        }
    }
''')


# DLSS-NR's three intermediate textures (original HDR output, colour proxy,
# neural output) hold nothing past the frame that writes them (the model's
# history lives in the NR feature), and the private D3D12 work of the views is
# serialized as above. Every cache entry of the same sizes on the same device
# therefore uses one set (one COM reference each), instead of one per view.
replace('src/dlss_nr.cpp', '''    if (!gpu.border_only) {
        result = device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &texture,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&gpu.original_output)
        );''', '''    // DCS VR Control: adopt another cache entry's intermediates of the same sizes.
    bool dcs_adopted{};
    if (!gpu.border_only) {
        for (auto& other_view : views) {
            for (auto& other : other_view.gpu_resources) {
                if (dcs_adopted || &other == &gpu || other.border_only || !other.original_output ||
                    !other.color_proxy || !other.neural_output || other.width != gpu.width ||
                    other.height != gpu.height || other.working_width != working_width ||
                    other.working_height != working_height) continue;
                ID3D12Device* other_device{};
                if (FAILED(other.original_output->GetDevice(IID_PPV_ARGS(&other_device)))) continue;
                const bool same_device = other_device == device;
                other_device->Release();
                if (!same_device) continue;
                gpu.original_output = other.original_output; gpu.original_output->AddRef();
                gpu.color_proxy = other.color_proxy; gpu.color_proxy->AddRef();
                gpu.neural_output = other.neural_output; gpu.neural_output->AddRef();
                dcs_adopted = true;
                trace_event("DLSS-NR intermediates %ux%u (working %ux%u) shared between views",
                    gpu.width, gpu.height, working_width, working_height);
            }
        }
    }
    if (!gpu.border_only && !dcs_adopted) {
        result = device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &texture,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&gpu.original_output)
        );''')

# The final composite's views (the game's colour, our DLSS output, the game's
# output) were created and released on every focus evaluation, six creations a
# frame on DCS's render thread. A view of the same resource with the default
# description is the same view: a small ring keeps them. Each kept view holds
# its resource, so an address cannot be reused while it is cached; an evicted
# view lets its resource go.
replace('src/d3d11_backend.cpp', '''bool composite_d3d11_crop(
    ID3D11DeviceContext* const context,''', '''// DCS VR Control: cached default views for composite_d3d11_crop.
template<class View>
struct DcsCompositeViewRing {
    struct Entry { ID3D11Resource* resource{}; View* view{}; std::uint64_t last_use{}; };
    std::array<Entry, 8> entries{};
    std::size_t next{};
    std::uint64_t uses{};
};
std::mutex dcs_composite_view_mutex;
DcsCompositeViewRing<ID3D11ShaderResourceView> dcs_composite_srvs;
DcsCompositeViewRing<ID3D11UnorderedAccessView> dcs_composite_uavs;

template<class View, class Create>
[[nodiscard]] HRESULT dcs_cached_composite_view(DcsCompositeViewRing<View>& ring, ID3D11Resource* const resource,
    View*& view, Create create) noexcept {
    std::lock_guard lock(dcs_composite_view_mutex);
    const std::uint64_t now = ++ring.uses;
    View* found{};
    for (auto& entry : ring.entries) {
        if (!entry.view) continue;
        if (entry.resource == resource) { entry.last_use = now; found = entry.view; continue; }
        // A view unused for 64 composites (a few frames) lets its resource go:
        // the game recreates its targets between missions.
        if (now - entry.last_use > 64U) { entry.view->Release(); entry = {}; }
    }
    if (found) { view = found; view->AddRef(); return S_OK; }
    const HRESULT result = create(&view);
    if (FAILED(result)) return result;
    auto& slot = ring.entries[ring.next++ % ring.entries.size()];
    if (slot.view) slot.view->Release();
    slot = {resource, view, now};
    view->AddRef();
    return result;
}

void dcs_flush_composite_views() noexcept {
    std::lock_guard lock(dcs_composite_view_mutex);
    for (auto& entry : dcs_composite_srvs.entries) { if (entry.view) entry.view->Release(); entry = {}; }
    for (auto& entry : dcs_composite_uavs.entries) { if (entry.view) entry.view->Release(); entry = {}; }
}

bool composite_d3d11_crop(
    ID3D11DeviceContext* const context,''')
replace('src/backend.hpp', '''    const Settings& settings
) noexcept;

[[nodiscard]] D3D11Evaluation* prepare_d3d11(''', '''    const Settings& settings
) noexcept;
// DCS VR Control: drops the composite's cached views (a new mission's features).
void dcs_flush_composite_views() noexcept;

[[nodiscard]] D3D11Evaluation* prepare_d3d11(''')
replace('src/d3d11_backend.cpp', '''    const bool views_ready = device != nullptr &&
        SUCCEEDED(device->CreateShaderResourceView(game_color, nullptr, &color_srv)) &&
        SUCCEEDED(device->CreateShaderResourceView(packed_dlss_output, nullptr, &dlss_srv)) &&
        SUCCEEDED(device->CreateUnorderedAccessView(game_output, nullptr, &output_uav));''', '''    const bool views_ready = device != nullptr &&
        SUCCEEDED(dcs_cached_composite_view(dcs_composite_srvs, game_color, color_srv,
            [&](ID3D11ShaderResourceView** created) { return device->CreateShaderResourceView(game_color, nullptr, created); })) &&
        SUCCEEDED(dcs_cached_composite_view(dcs_composite_srvs, packed_dlss_output, dlss_srv,
            [&](ID3D11ShaderResourceView** created) { return device->CreateShaderResourceView(packed_dlss_output, nullptr, created); })) &&
        SUCCEEDED(dcs_cached_composite_view(dcs_composite_uavs, game_output, output_uav,
            [&](ID3D11UnorderedAccessView** created) { return device->CreateUnorderedAccessView(game_output, nullptr, created); }));''')

# DLSS-NR's encode and decode sample their textures bilinearly; at working
# scale 1.0 every sample falls on a texel centre, where the weights are
# 1, 0, 0, 0. One load then gives the same bits as four.
# DCS Control (2026-10-18): the test is uniform (the working size equals the
# region), not per pixel: at other scales (0.75 by default) the decode then
# costs what upstream's does (the per-pixel test cost +5 % there).
replace('src/nr_codec_shader.hpp', '''    return lerp(
        lerp(Source0.Load(int3(p00, 0)), Source0.Load(int3(p10, 0)), fraction.x),
        lerp(Source0.Load(int3(p01, 0)), Source0.Load(int3(p11, 0)), fraction.x),
        fraction.y
    );''', '''    float4 result = Source0.Load(int3(p00, 0));
    [branch] if (any(ProxySize != SourceSize)) {
        result = lerp(
            lerp(result, Source0.Load(int3(p10, 0)), fraction.x),
            lerp(Source0.Load(int3(p01, 0)), Source0.Load(int3(p11, 0)), fraction.x),
            fraction.y
        );
    }
    return result;''')
replace('src/nr_codec_shader.hpp', '''    return lerp(
        lerp(Source1.Load(int3(p00, 0)), Source1.Load(int3(p10, 0)), fraction.x),
        lerp(Source1.Load(int3(p01, 0)), Source1.Load(int3(p11, 0)), fraction.x),
        fraction.y
    );''', '''    float4 result = Source1.Load(int3(p00, 0));
    [branch] if (any(ProxySize != RegionSize)) {
        result = lerp(
            lerp(result, Source1.Load(int3(p10, 0)), fraction.x),
            lerp(Source1.Load(int3(p01, 0)), Source1.Load(int3(p11, 0)), fraction.x),
            fraction.y
        );
    }
    return result;''')
replace('src/nr_codec_shader.hpp', '''    return lerp(
        lerp(Source2.Load(int3(p00, 0)), Source2.Load(int3(p10, 0)), fraction.x),
        lerp(Source2.Load(int3(p01, 0)), Source2.Load(int3(p11, 0)), fraction.x),
        fraction.y
    );''', '''    float4 result = Source2.Load(int3(p00, 0));
    [branch] if (any(ProxySize != RegionSize)) {
        result = lerp(
            lerp(result, Source2.Load(int3(p10, 0)), fraction.x),
            lerp(Source2.Load(int3(p01, 0)), Source2.Load(int3(p11, 0)), fraction.x),
            fraction.y
        );
    }
    return result;''')

# VRAM_STAGE lines: what the private D3D12 path costs in video memory, stage by
# stage (device + NGX initialization, the private SR feature, the DLSS-NR
# runtime and feature), to tell which part a saving would come from.
shutil.copyfile(root / 'patches/cheeky/dcs_vram_probe.hpp', source / 'src/dcs_vram_probe.hpp')
replace(transport, '#include "depth_formats.hpp"\n', '#include "dcs_vram_probe.hpp"\n#include "depth_formats.hpp"\n')
replace(transport, '''    TransportDevice created{};
    if (!create_transport_device(device11, ngx, created)) {''', '''    TransportDevice created{};
    const double dcs_vram_before = dcs_process_vram_mb();
    const bool dcs_device_created = create_transport_device(device11, ngx, created);
    const double dcs_vram_after = dcs_process_vram_mb();
    if (dcs_device_created) // Failed attempts retry every 5 s; only the one that succeeds is measured.
        trace_event("VRAM_STAGE private D3D12 device + NGX init vram_before_mb=%.1f vram_after_mb=%.1f delta_mb=%+.1f",
            dcs_vram_before, dcs_vram_after, dcs_vram_after - dcs_vram_before);
    if (!dcs_device_created) {''')
backend12 = 'src/d3d12_backend.cpp'
replace(backend12, '#include "backend.hpp"\n', '#include "backend.hpp"\n#include "dcs_vram_probe.hpp"\n')
replace(backend12, '''            NgxHandle* created{};
            result = callbacks.create_feature(
                command_list,
                contract.feature_id,
                parameters,
                &created
            );''', '''            NgxHandle* created{};
            const double dcs_vram_before = dcs_process_vram_mb();
            result = callbacks.create_feature(
                command_list,
                contract.feature_id,
                parameters,
                &created
            );
            const double dcs_vram_after = dcs_process_vram_mb();
            trace_event("VRAM_STAGE private SR feature create view=%llu output=%ux%u vram_before_mb=%.1f "
                "vram_after_mb=%.1f delta_mb=%+.1f", static_cast<unsigned long long>(contract.view_id),
                crop.output_width, crop.output_height, dcs_vram_before, dcs_vram_after, dcs_vram_after - dcs_vram_before);''')
nr = 'src/dlss_nr.cpp'
replace(nr, '#include "dlss_nr_input.hpp"\n', '#include "dlss_nr_input.hpp"\n#include "dcs_vram_probe.hpp"\n', count=1)
replace(nr, '''    const auto result = initialize(''', '''    const double dcs_vram_before = dcs_process_vram_mb();
    const auto result = initialize(''')
replace(nr, '''    trace_event("DLSS-NR 310.8 feature-18 runtime initialized");''', '''    trace_event("DLSS-NR 310.8 feature-18 runtime initialized");
    const double dcs_vram_after = dcs_process_vram_mb();
    trace_event("VRAM_STAGE DLSS-NR runtime init vram_before_mb=%.1f vram_after_mb=%.1f delta_mb=%+.1f",
        dcs_vram_before, dcs_vram_after, dcs_vram_after - dcs_vram_before);''')
replace(nr, '''    constexpr std::uint32_t neural_feature_id = 18U;
    result = runtime.create_feature(''', '''    constexpr std::uint32_t neural_feature_id = 18U;
    const double dcs_vram_before_feature = dcs_process_vram_mb();
    result = runtime.create_feature(''')
replace(nr, '''    view.settings_signature = 0U;
    trace_event(
        "DLSS-NR feature 18 created view=%llu''', '''    view.settings_signature = 0U;
    const double dcs_vram_after_feature = dcs_process_vram_mb();
    trace_event("VRAM_STAGE DLSS-NR feature create view=%llu input=%ux%u vram_before_mb=%.1f vram_after_mb=%.1f delta_mb=%+.1f",
        static_cast<unsigned long long>(frame.view_id), working_width, working_height, dcs_vram_before_feature,
        dcs_vram_after_feature, dcs_vram_after_feature - dcs_vram_before_feature);
    trace_event(
        "DLSS-NR feature 18 created view=%llu''')

# DCS Control (2026-10-18): the DLSS 5 focus round.
# 1. The DLSS 5 edge fade (NrTransitionWidth) may span 0.45 of the NR area, so it can match the bundled Quad Views
#    focus fade band (radius 0.6 to 1.0). The SR TransitionWidth keeps upstream's 0.30. The fade lies inside the NR
#    region (decode weight 1 - smoothstep(1 - width / min(NrWidth, NrHeight), 1, distance)), so a wider fade moves
#    its start inward and never grows the region, its textures or the NR cost.
replace('src/settings.cpp', '''    store_float(
        nr_transition_bits,
        std::clamp(settings.nr_transition_width, 0.0F, 0.30F)
    );''', '''    // DCS Control: the DLSS 5 edge fade may be as wide as the Quad Views focus fade (0.45); SR keeps 0.30.
    store_float(
        nr_transition_bits,
        std::clamp(settings.nr_transition_width, 0.0F, 0.45F)
    );''')
replace('standalone/overlay_ui.cpp', 'slider("NR transition width", s.nr_transition_width, 0.0F, .3F, "%.3f");',
    'slider("NR transition width", s.nr_transition_width, 0.0F, .45F, "%.3f"); // DCS Control: settings.cpp cap')
replace('src/addon.cpp', '''                "NR transition width", &settings.nr_transition_width,
                0.0F, 0.30F, "%.3f", ImGuiSliderFlags_AlwaysClamp''', '''                "NR transition width", &settings.nr_transition_width, // DCS Control: settings.cpp cap
                0.0F, 0.45F, "%.3f", ImGuiSliderFlags_AlwaysClamp''')
replace('uevr/scripts/cheeky_foveated_dlss.lua', 'slider("NR transition", "NrTransitionWidth", 0, 0.3)',
    'slider("NR transition", "NrTransitionWidth", 0, 0.45) -- DCS Control: settings.cpp cap')
shutil.copyfile(root / 'patches/cheeky/dcs_nr_focus_tests.inc', source / 'tests/dcs_nr_focus_tests.inc')
replace('tests/openxr_calibration_tests.cpp', '''#include "dcs_status_tests.inc"
int run_dcs_status_tests() {''', '''#include "dcs_nr_focus_tests.inc"
int run_dcs_nr_focus_tests() {
    try { dcs_nr_focus_test(); return 0; }
    catch (const std::exception& e) { std::cerr << "DCS NR focus shape: " << e.what() << '\\n'; return 1; }
}
#include "dcs_status_tests.inc"
int run_dcs_status_tests() {''')
replace('tests/gaze_tests.cpp', '''    if (argc == 2 && std::strcmp(argv[1], "--dcs-status") == 0) { int run_dcs_status_tests(); return run_dcs_status_tests(); }''',
    '''    if (argc == 2 && std::strcmp(argv[1], "--dcs-status") == 0) { int run_dcs_status_tests(); return run_dcs_status_tests(); }
    if (argc == 2 && std::strcmp(argv[1], "--dcs-nr-focus") == 0) { int run_dcs_nr_focus_tests(); return run_dcs_nr_focus_tests(); }''')
replace('CMakeLists.txt', 'add_test(NAME DcsVrStatusTests COMMAND CheekyTests --dcs-status)',
    'add_test(NAME DcsVrStatusTests COMMAND CheekyTests --dcs-status)\nadd_test(NAME DcsNrFocusTests COMMAND CheekyTests --dcs-nr-focus)')

# 3. DLSS 5 evaluations that keep failing stop for the session (dcs_nr_failure_latch.hpp): after
#    dcs_nr_failure_limit consecutive failures on a view, or at once on a device-removed/hung result, instead of a
#    retry with a history reset on every frame. The transport then runs SR only, as with DLSS 5 off.
shutil.copyfile(root / 'patches/cheeky/dcs_nr_failure_latch.hpp', source / 'src/dcs_nr_failure_latch.hpp')
replace(nr, '#include "dlss_nr_input.hpp"\n#include "dcs_vram_probe.hpp"\n',
    '#include "dlss_nr_input.hpp"\n#include "dcs_vram_probe.hpp"\n#include "dcs_nr_failure_latch.hpp"\n')
replace(nr, '''    std::deque<GpuResources> gpu_resources;
    std::uint64_t gpu_use_sequence{};
};''', '''    std::deque<GpuResources> gpu_resources;
    std::uint64_t gpu_use_sequence{};
    std::uint32_t dcs_failed_run{}; // DCS Control: consecutive failed evaluations (dcs_nr_failure_latch.hpp)
};''')
replace(nr, '''    if (!settings.nr_enabled) {
        diagnostics.state = DlssNrState::disabled;
        for (auto& view : views) view.was_enabled = false;
        return false;
    }
''', '''    if (!settings.nr_enabled) {
        diagnostics.state = DlssNrState::disabled;
        for (auto& view : views) view.was_enabled = false;
        return false;
    }
    // DCS Control: DLSS 5 stopped for this session after repeated failures; nothing is recorded.
    if (dcs_nr_failure_latch().active()) {
        diagnostics.state = DlssNrState::evaluation_failed;
        diagnostics.skip_reason = dcs_nr_failure_latch_reason;
        for (auto& view : views) view.was_enabled = false;
        return false;
    }
''')
replace(nr, '''        trace_event(
            "DLSS-NR feature 18 evaluation failed view=%llu result=0x%08X",
            static_cast<unsigned long long>(frame.view_id),
            result
        );
        return false;''', '''        trace_event(
            "DLSS-NR feature 18 evaluation failed view=%llu result=0x%08X",
            static_cast<unsigned long long>(frame.view_id),
            result
        );
        // DCS Control: a run of failures or a lost device stops DLSS 5 for the session (dcs_nr_failure_latch.hpp).
        HRESULT dcs_removed = S_OK;
        if (ID3D12Device* dcs_device{}; SUCCEEDED(frame.command_list->GetDevice(IID_PPV_ARGS(&dcs_device))) && dcs_device) {
            dcs_removed = dcs_device->GetDeviceRemovedReason();
            dcs_device->Release();
        }
        if (dcs_nr_failure_latch().failed(++view.dcs_failed_run, static_cast<std::uint32_t>(result),
                static_cast<std::uint32_t>(dcs_removed)))
            trace_event("DLSS-NR stopped for this session after %u consecutive failed evaluations view=%llu "
                "result=0x%08X deviceRemovedReason=0x%08X; the game's DLSS image is used without DLSS 5",
                view.dcs_failed_run, static_cast<unsigned long long>(frame.view_id), result,
                static_cast<unsigned>(dcs_removed));
        return false;''')
replace(nr, '''    view.was_enabled = true;
    history_guard.succeeded = true;''', '''    view.was_enabled = true;
    view.dcs_failed_run = 0U; // DCS Control: the failure run is consecutive.
    history_guard.succeeded = true;''')
replace(transport, '#include "dcs_vram_probe.hpp"\n#include "depth_formats.hpp"\n',
    '#include "dcs_vram_probe.hpp"\n#include "depth_formats.hpp"\n#include "dcs_nr_failure_latch.hpp"\n')
replace(transport, '''    const NgxParameters* const parameters,
    const Settings& settings,
    const D3D11TransportNgx& ngx,
    NgxResult& result,
    const D3D11NativeEvaluate* const native
) noexcept {
''', '''    const NgxParameters* const parameters,
    const Settings& dcs_input_settings,
    const D3D11TransportNgx& ngx,
    NgxResult& result,
    const D3D11NativeEvaluate* const native
) noexcept {
''')
replace(transport, '''    D3D12NgxInterceptionScope private_dx12_scope;
    result = 0xBAD00005U;
''', '''    D3D12NgxInterceptionScope private_dx12_scope;
    // DCS Control: once DLSS 5 has stopped for the session (dcs_nr_failure_latch.hpp) the transport runs SR only:
    // the same private SR feature and textures, no NR pass, as with DLSS 5 off.
    Settings dcs_sr_only{};
    const bool dcs_nr_stopped = dcs_input_settings.nr_enabled && !dcs_input_settings.dcs_nr_suppressed &&
        dcs_nr_failure_latch().active();
    if (dcs_nr_stopped) { dcs_sr_only = dcs_input_settings; dcs_sr_only.dcs_nr_suppressed = true; }
    const Settings& settings = dcs_nr_stopped ? dcs_sr_only : dcs_input_settings;
    result = 0xBAD00005U;
''')
replace('tests/runtime_host_tests.cpp', '''            else if (arg == "--transport-dcs-deferred-nr-off") { transport = dx11 = dcs_deferred_mode = true; dcs_deferred_scenario = 2; }
''', '''            else if (arg == "--transport-dcs-deferred-nr-off") { transport = dx11 = dcs_deferred_mode = true; dcs_deferred_scenario = 2; }
            else if (arg == "--transport-dcs-deferred-nr-fail") { transport = dx11 = dcs_deferred_mode = true; dcs_deferred_scenario = 3; }
''')
replace('tests/runtime_host_tests.cpp', '''        if (dcs_deferred_mode) {
            verify_dcs_deferred(command, get, attachment,''', '''        if (dcs_deferred_mode && dcs_deferred_scenario == 3) { // DCS Control: failing DLSS 5 (dcs_nr_failure_latch.hpp)
            verify_dcs_deferred(command, get, attachment, device11.Get(), context11.Get(), fake_ngx,
                directory / "CheekyFoveatedDLSS-Standalone.log", bin, dcs_deferred_scenario);
            detach(attachment);
            puts("PASS: DCS focus view with failing DLSS 5: stopped for the session after the failure limit, private D3D12 SR goes on");
            return 0;
        }
        if (dcs_deferred_mode) {
            verify_dcs_deferred(command, get, attachment,''')
replace('cmake/Standalone.cmake', '''    add_test(NAME CheekyRuntimeStandalone-TransportDcsDeferredNrOff COMMAND CheekyRuntimeHostTests --transport-dcs-deferred-nr-off)
''', '''    add_test(NAME CheekyRuntimeStandalone-TransportDcsDeferredNrOff COMMAND CheekyRuntimeHostTests --transport-dcs-deferred-nr-off)
    add_test(NAME CheekyRuntimeStandalone-TransportDcsDeferredNrFail COMMAND CheekyRuntimeHostTests --transport-dcs-deferred-nr-fail)
''')

# 4. Filtered fetches (SampleLevel through a linear clamp sampler) for the NR decode's proxy and neural reads were
#    measured and rejected: the texture unit's 8-bit subtexel weights and reduced fp16 filter precision put the decode
#    up to 2/1023 off in SDR and many fp16 ULPs off in HDR, where UpgradeToneMap divides by the proxy's luminance
#    (tests/dcs_nr_codec_tests.cpp --sampled prints the numbers). The decode keeps its exact four-load bilinear.
#    The fixture instead proves the shipped codec bit-identical to upstream's unconditional four-load lerp and the
#    embedded bytecode current (see the regeneration step below).
shutil.copyfile(root / 'patches/cheeky/dcs_nr_codec_tests.cpp', source / 'tests/dcs_nr_codec_tests.cpp')
replace('CMakeLists.txt', '''add_test(NAME CheekyNrObserverTestsNoDebugLayer COMMAND CheekyNrObserverTests)
''', '''add_test(NAME CheekyNrObserverTestsNoDebugLayer COMMAND CheekyNrObserverTests)
# DCS Control: the shipped NR codec against upstream's four-load lerp, bit for bit (WARP and the hardware GPU).
add_executable(CheekyDcsNrCodecTests tests/dcs_nr_codec_tests.cpp)
target_compile_features(CheekyDcsNrCodecTests PRIVATE cxx_std_20)
target_compile_definitions(CheekyDcsNrCodecTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_include_directories(CheekyDcsNrCodecTests PRIVATE src)
target_link_libraries(CheekyDcsNrCodecTests PRIVATE d3d12 dxgi d3dcompiler)
set_target_properties(CheekyDcsNrCodecTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
if(MSVC)
    target_compile_options(CheekyDcsNrCodecTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()
add_test(NAME DcsNrCodecTests COMMAND CheekyDcsNrCodecTests)
set_tests_properties(DcsNrCodecTests PROPERTIES TIMEOUT 300)
''')

# Review round 2026-10-19 (DCS Control).
# 1. A brief transport failure on an accepted deferred focus view is held with a bilinear upscale for at most
#    dcs_deferred_unknown_hold_ms of consecutive failures instead of creating DCS's own ~254 MiB feature, which is
#    never freed (dcs_deferred_hold_transport_failure in dcs_deferred_feature.inc). A transport success ends the run.
replace(hooks, '''    D3D11PeripheralDlaaResult peripheral{};''', '''    // DCS VR Control: a brief transport failure on an accepted deferred focus view never creates DCS's own feature.
    if (NgxResult held{}; settings.d3d11_use_d3d12_transport &&
        dcs_deferred_hold_transport_failure(context, handle, parameters, held)) return held;
    D3D11PeripheralDlaaResult peripheral{};''', count=2)
replace(hooks, '''                current_transport_ngx(runtime), transport_result, &native)) {
            release_d3d11_peripheral_dlaa_view(handle);''', '''                current_transport_ngx(runtime), transport_result, &native)) {
            dcs_deferred_transport_ok(handle); // DCS VR Control: ends a run of transport failures.
            release_d3d11_peripheral_dlaa_view(handle);''', count=2)

# 3. The DLSS 5 key's session-only change ("set_session") must never reach the INI, but every later save wrote the
#    whole live settings. The runtime keeps the persisted value of each field a session change overrode
#    (dcs_session_settings.hpp) and the save worker writes those. Under DCS quad focus the two upstream Alt+Shift keys
#    (DLSS toggle and DLSS-NR toggle, both saved to the INI) are off: DCS Control's own key owns DLSS 5 there.
shutil.copyfile(root / 'patches/cheeky/dcs_session_settings.hpp', source / 'uevr/dcs_session_settings.hpp')
replace('uevr/runtime.cpp', '#include "version.h"\n', '#include "version.h"\n#include "dcs_session_settings.hpp"\n')
replace('uevr/runtime.cpp', '''                { std::lock_guard lock(s.mutex); settings = configured_settings(); revision = s.revision; }''',
    '''                { std::lock_guard lock(s.mutex); settings = configured_settings(); revision = s.revision;
                    // DCS VR Control: fields changed only for this session are saved with their persisted values.
                    cheeky::dcsvr::dcs_session_overrides().apply([&](const std::string& key, const std::string& value) {
                        static_cast<void>(set_named_setting(settings, key, value)); });
                }''')
replace('uevr/runtime.cpp', '''        auto settings = configured_settings();
        if (action == "defaults") {''', '''        auto settings = configured_settings();
        std::vector<std::string> dcs_keys; // DCS VR Control: the keys this transaction sets.
        if (action == "defaults") {''')
replace('uevr/runtime.cpp', '''                if (++count > 64) { s.message = "Too many settings"; return false; }''',
    '''                if (++count > 64) { s.message = "Too many settings"; return false; }
                dcs_keys.emplace_back(std::string_view(line).substr(0, eq));''')
replace('uevr/runtime.cpp', '''        update_settings(settings); ++s.revision; s.applied_request = id;''', '''        {
            const auto before = serialize_settings(configured_settings()), after = serialize_settings(settings);
            if (action == "set_session") cheeky::dcsvr::dcs_session_overrides().session(before, after, dcs_keys);
            else if (action.starts_with("defaults")) cheeky::dcsvr::dcs_session_overrides().clear();
            else cheeky::dcsvr::dcs_session_overrides().persistent(before, after, dcs_keys);
        }
        update_settings(settings); ++s.revision; s.applied_request = id;''')
replace('uevr/runtime.cpp', '''        static bool was_down{};
        const bool down = (GetAsyncKeyState(VK_MENU)''', '''        // DCS VR Control: under DCS quad focus DCS Control's own key owns DLSS 5; these keys would save to the INI.
        const bool dcs_quad_focus = [] {
            wchar_t value[2]{};
            return GetEnvironmentVariableW(L"DCSVR_QUAD_FOCUS", value, 2) == 1 && value[0] == L'1';
        }();
        static bool was_down{};
        const bool down = (GetAsyncKeyState(VK_MENU)''')
replace('uevr/runtime.cpp', '        if (down && !was_down) {', '        if (!dcs_quad_focus && down && !was_down) {')
replace('uevr/runtime.cpp', '        if (nr_down && !nr_was_down) {', '        if (!dcs_quad_focus && nr_down && !nr_was_down) {')
replace('tests/openxr_calibration_tests.cpp', '#include "../src/dcs_status_block.hpp"\n',
    '#include "../src/dcs_status_block.hpp"\n#include "../uevr/dcs_session_settings.hpp"\n')

# 4. The depth conversion's cached SRVs keep the game's depth buffers alive; a new mission's create drops them along
#    with the composite's cached views (dcs_release_idle_views).
# 5. Waiting for private DX12 work has a deadline where the DCS patches call it on DCS's render thread (gate rejection,
#    bilinear hold, idle release): on timeout the view is kept whole and released on a later call. Teardown and the
#    game's own ReleaseFeature keep the unbounded wait (their owners go away right after it).
replace(transport, '''void drain_transport_work(TransportDevice& device, const std::uint64_t pending) noexcept {
    if (!pending) return; // No private submissions (including partial initialization).''', '''// DCS VR Control: wait_ms bounds the wait; false when it ran out with the work still pending.
bool drain_transport_work(TransportDevice& device, const std::uint64_t pending,
    const std::uint32_t wait_ms = dcs_transport_release_wait_forever) noexcept {
    if (!pending) return true; // No private submissions (including partial initialization).''')
replace(transport, '''    if (removed()) return;
    if (device.device11 != nullptr) {''', '''    if (removed()) return true;
    if (device.device11 != nullptr) {''')
replace(transport, '''    for (;;) {
        if (removed()) return;
        if (!signaled && device.queue12 && device.fence12)
            signaled = SUCCEEDED(device.queue12->Signal(device.fence12, target));
        const auto completed = device.fence12 ? device.fence12->GetCompletedValue() : 0;
        if (signaled && completed >= target) return;''', '''    for (;;) {
        if (removed()) return true;
        if (!signaled && device.queue12 && device.fence12)
            signaled = SUCCEEDED(device.queue12->Signal(device.fence12, target));
        const auto completed = device.fence12 ? device.fence12->GetCompletedValue() : 0;
        if (signaled && completed >= target) return true;
        if (wait_ms != dcs_transport_release_wait_forever && GetTickCount64() - start >= wait_ms) {
            trace_event("Transport release gave up waiting for private DX12 work after %u ms fence=%llu completed=%llu "
                "signaled=%u; resources kept, released on a later call", wait_ms, static_cast<unsigned long long>(target),
                static_cast<unsigned long long>(completed), signaled ? 1U : 0U);
            return false;
        }''')
replace(transport, '''void release_d3d11_transport_view(const NgxHandle* const game_handle) noexcept {
    D3D12NgxInterceptionScope private_dx12_scope;
    if (game_handle == nullptr) return;''', '''bool release_d3d11_transport_view(const NgxHandle* const game_handle, const std::uint32_t wait_ms) noexcept {
    D3D12NgxInterceptionScope private_dx12_scope;
    if (game_handle == nullptr) return true;''')
replace(transport, '''            drain_transport_work(device, pending_work(*iterator));
            release_peripheral_dlaa_view(view_id);''', '''            // DCS VR Control: on timeout nothing is freed; the view stays whole for a later release.
            if (!drain_transport_work(device, pending_work(*iterator), wait_ms)) return false;
            release_peripheral_dlaa_view(view_id);''')
replace(transport, '''            device.views.erase(iterator);
            break;
        }
    }
}

void release_d3d11_d3d12_transport() noexcept {''', '''            device.views.erase(iterator);
            break;
        }
    }
    return true;
}

// DCS VR Control: drops the depth conversion's cached views of the game's depth
// buffers (each keeps its buffer alive), e.g. the previous mission's.
void dcs_flush_transport_depth_views() noexcept {
    std::lock_guard lock(transport_mutex);
    for (auto& device : transport_devices)
        for (auto& cached : device.depth_source_views) { release(cached.srv); cached = {}; }
}

void release_d3d11_d3d12_transport() noexcept {''')
replace('src/d3d11_d3d12_transport.hpp', '''void release_d3d11_transport_view(const NgxHandle* game_handle) noexcept;''', '''// DCS VR Control: wait_ms bounds the wait for the view's private DX12 work; on
// timeout nothing is released and false is returned (call again later).
inline constexpr std::uint32_t dcs_transport_release_wait_forever = 0xFFFFFFFFU;
inline constexpr std::uint32_t dcs_transport_release_wait_ms = 1500U;
bool release_d3d11_transport_view(const NgxHandle* game_handle,
    std::uint32_t wait_ms = dcs_transport_release_wait_ms) noexcept;''')
# The game's ReleaseFeature frees the handle for good: keep upstream's unbounded wait there.
replace(hooks, '''    release_d3d11_transport_view(handle);
    release_d3d11_peripheral_dlaa_view(handle);
    unregister_d3d11_game_feature(handle);''', '''    release_d3d11_transport_view(handle, dcs_transport_release_wait_forever);
    release_d3d11_peripheral_dlaa_view(handle);
    unregister_d3d11_game_feature(handle);''', count=2)
replace('src/backend.hpp', '''void release_d3d11_resources() noexcept;
''', '''// DCS VR Control: drops the transport's cached depth views (d3d11_d3d12_transport.cpp).
void dcs_flush_transport_depth_views() noexcept;
void release_d3d11_resources() noexcept;
''')

# 6. A create is classified only by a layout of this session published within 250 ms; an older one only keeps a view
#    on the private SR (dcs_quad_focus_layout_extent), never creates DCS's own feature.
#    Inserted before the create_extent line, so the declaration step above still finds its own text on a re-run.
replace('src/gaze_foveation.hpp', 'int dcs_quad_focus_create_extent(unsigned width, unsigned height) noexcept;\nbool dcs_quad_focus_processing_allowed(',
    'int dcs_quad_focus_layout_extent(unsigned width, unsigned height) noexcept;\n'
    'int dcs_quad_focus_create_extent(unsigned width, unsigned height) noexcept;\nbool dcs_quad_focus_processing_allowed(')

# Fixture: transport failures on an accepted deferred focus view (dcs_deferred_tests.inc scenario 4).
replace('tests/runtime_host_tests.cpp', '''            else if (arg == "--transport-dcs-deferred-nr-fail") { transport = dx11 = dcs_deferred_mode = true; dcs_deferred_scenario = 3; }
''', '''            else if (arg == "--transport-dcs-deferred-nr-fail") { transport = dx11 = dcs_deferred_mode = true; dcs_deferred_scenario = 3; }
            else if (arg == "--transport-dcs-deferred-transport-fail") { transport = dx11 = dcs_deferred_mode = true; dcs_deferred_scenario = 4; }
''')
replace('tests/runtime_host_tests.cpp', '''        if (dcs_deferred_mode && dcs_deferred_scenario == 3) {''', '''        if (dcs_deferred_mode && dcs_deferred_scenario == 4) { // DCS Control: transport failures (dcs_deferred_feature.inc)
            verify_dcs_deferred(command, get, attachment, device11.Get(), context11.Get(), fake_ngx,
                directory / "CheekyFoveatedDLSS-Standalone.log", bin, dcs_deferred_scenario);
            detach(attachment);
            puts("PASS: DCS deferred focus view with a failing transport: brief failures held, only a lasting one creates DCS's own feature");
            return 0;
        }
        if (dcs_deferred_mode && dcs_deferred_scenario == 3) {''')
replace('cmake/Standalone.cmake', '''    add_test(NAME CheekyRuntimeStandalone-TransportDcsDeferredNrFail COMMAND CheekyRuntimeHostTests --transport-dcs-deferred-nr-fail)
''', '''    add_test(NAME CheekyRuntimeStandalone-TransportDcsDeferredNrFail COMMAND CheekyRuntimeHostTests --transport-dcs-deferred-nr-fail)
    add_test(NAME CheekyRuntimeStandalone-TransportDcsDeferredTransportFail COMMAND CheekyRuntimeHostTests --transport-dcs-deferred-transport-fail)
''')

# The D3D shaders are embedded as bytecode (src/d3d_shaders.hpp, generated by Cheeky's scripts/build-d3d-shaders.py
# with the Windows SDK fxc); the build never compiles HLSL. Regenerate it whenever the patched shader sources no
# longer match it, so the shader patches above reach the GPU. (Until 2026-10-18 nothing did this: the 2026-10-07
# single-load codec patch above changed only the source, never the shipped bytecode.)
import os
import subprocess
import sys
kits = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Windows Kits/10/bin'
fxc_candidates = sorted(kits.glob('10.*/x64/fxc.exe'),
    key=lambda path: tuple(int(part) for part in path.parent.parent.name.split('.') if part.isdigit()))
if not fxc_candidates:
    raise RuntimeError('fxc.exe (Windows 10/11 SDK) not found: it compiles the patched Cheeky shaders')
shader_generator = [sys.executable, str(source / 'scripts/build-d3d-shaders.py'), '--fxc', str(fxc_candidates[-1])]
if subprocess.run(shader_generator + ['--check'], capture_output=True, text=True).returncode != 0:
    generated = subprocess.run(shader_generator, capture_output=True, text=True)
    if generated.returncode != 0:
        raise RuntimeError('Cheeky shader compilation failed:\n' + generated.stdout + generated.stderr)
    print('Regenerated src/d3d_shaders.hpp from the patched shader sources.')

marker.write_text(wanted + '\n', encoding='ascii')
print('Applied opt-in DCS quad focus adapter to the pinned Cheeky source.')
