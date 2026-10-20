namespace {
DcsQuadHandleCensus dcs_quad_focus_census;
HMODULE dcs_quad_layout_module{};
DcsQuadLayoutFn dcs_quad_layout_function{};
// Optional side channel from the adapted layer; older layers simply lack it.
bool dcs_quad_load_layout(DcsQuadLayoutV1& layout) noexcept {
    if (snapshot_module == nullptr) return false;
    if (dcs_quad_layout_module != snapshot_module) {
        dcs_quad_layout_module = snapshot_module;
        dcs_quad_layout_function = reinterpret_cast<DcsQuadLayoutFn>(
            GetProcAddress(snapshot_module, dcs_quad_layout_export));
    }
    layout = {};
    return dcs_quad_layout_function != nullptr &&
        dcs_quad_layout_function(&layout, sizeof(layout)) != 0U && layout.structure_size >= sizeof(layout);
}
void dcs_quad_flag_names(std::uint32_t bits, char* out, std::size_t size) noexcept {
    struct Name { std::uint32_t bit; const char* text; };
    constexpr Name names[]{{dcs_quad_focus_status, "dcs-quad-session"},
        {CHEEKY_GAZE_STATUS_LAYER_ACTIVE, "layer-active"}, {CHEEKY_GAZE_STATUS_SESSION_FOCUSED, "session-focused"},
        {CHEEKY_GAZE_STATUS_MAPPING_READY, "mapping-ready"}, {CHEEKY_GAZE_STATUS_AMBIGUOUS_RESOURCE, "ambiguous-resource"},
        {CHEEKY_GAZE_STATUS_UNSUPPORTED_VIEW_CONFIG, "unsupported-view-config"}};
    out[0] = 0;
    for (const auto& name : names) if (bits & name.bit) {
        if (out[0]) strncat_s(out, size, "|", _TRUNCATE);
        strncat_s(out, size, name.text, _TRUNCATE);
    }
    if (!out[0]) strncpy_s(out, size, "none", _TRUNCATE);
}
void dcs_quad_problem_names(std::uint32_t bits, char* out, std::size_t size) noexcept {
    constexpr const char* names[]{"abi", "view-count", "no-session", "no-display-time", "stale", "future-or-unpublished",
        "missing-flags", "rejected-flags"};
    out[0] = 0;
    for (unsigned i = 0; i < std::size(names); ++i) if (bits & (1U << i)) {
        if (out[0]) strncat_s(out, size, "|", _TRUNCATE);
        strncat_s(out, size, names[i], _TRUNCATE);
    }
    if (!out[0]) strncpy_s(out, size, "none", _TRUNCATE);
}
double dcs_quad_age_ms(std::uint64_t now, std::uint64_t then, std::uint64_t frequency) noexcept {
    if (!frequency || !then) return -1.0;
    return now >= then ? double(now - then) * 1000.0 / double(frequency) : -double(then - now) * 1000.0 / double(frequency);
}
} // namespace

bool dcs_quad_focus_processing_allowed(DlssViewId view_id, IUnknown* output,
    unsigned x, unsigned y, unsigned width, unsigned height, bool& reset_original) noexcept {
    reset_original = false;
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"DCSVR_QUAD_FOCUS", enabled, 2) != 1 || enabled[0] != L'1') return true;
    // Eye identity is only unnecessary when neither SR foveation nor an eye-dependent NR region runs. A fixed NR
    // region centred in each focus view (DLSS 5 area: no SR crop, no gaze or auto-aligned centre, no mirrored
    // offset) is identical for both eyes, exactly like whole-view NR.
    const auto settings = current_settings();
    const bool nr_region_per_eye = settings.nr_enabled && settings.nr_foveated && (settings.nr_use_sr_foveation ||
        settings.eye_independent_coverage || uses_coordinated_center(settings) || settings.x_offset != 0.0F);
    const bool eye_identity_required = settings.enabled || nr_region_per_eye;
    std::lock_guard lock(coordinator_mutex);
    auto& transition = state_for_view(view_id).dcs_quad_focus_transition;
    const auto finish = [&](bool allowed) { reset_original = transition.observe(allowed); return allowed; };
    CheekyGazeSnapshotV1 sample{};
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    const auto freq = static_cast<std::uint64_t>(frequency.QuadPart);
    const bool loaded = load_snapshot(sample);
    DcsQuadLayoutV1 layout{};
    const bool layout_loaded = loaded && dcs_quad_load_layout(layout);
    const auto identity = canonical_identity(output);
    const auto now = qpc_now();
    const auto now_ms = GetTickCount64();
    auto decision = dcs_quad_focus_decide(static_cast<std::uint64_t>(view_id), loaded, sample, now, freq,
        identity, x, y, width, height,
        [&](const CheekyGazeViewV1& target) {
            return dcs_quad_focus_copy_reaches(copy_graph, identity, x, y, width, height, target, now_ms);
        }, layout_loaded ? &layout : nullptr, dcs_quad_focus_census, now_ms, eye_identity_required);
    // A size-proven focus handle survives transient rejections for a bounded
    // grace period instead of releasing (and later rebuilding) its transport/NR.
    const bool was_holding = transition.hold.holding;
    const auto proven_ms = transition.hold.proven_ms;
    const bool held = dcs_quad_focus_apply_hold(decision, transition.hold, sample, layout_loaded ? &layout : nullptr,
        width, height, now_ms, eye_identity_required);
    if (held) {
        static std::atomic<std::uint64_t> held_frames{};
        const auto n = ++held_frames;
        if (!was_holding || n <= 4 || (n & (n - 1)) == 0) {
            char problems[160]{};
            dcs_quad_problem_names(decision.snapshot_problems, problems, sizeof(problems));
            trace_event("DCS quad focus: holding focus handle=0x%llX through transient reject reason=%s problems=%s size_route=%s since_proof_ms=%llu held_frames=%llu",
                static_cast<unsigned long long>(view_id), dcs_quad_focus_reason_name(decision.reason), problems,
                dcs_quad_size_proof_name(decision.size), static_cast<unsigned long long>(now_ms - proven_ms),
                static_cast<unsigned long long>(n));
        }
        cheeky::dcsvr::dcs_focus_activity().accepted(static_cast<std::uint64_t>(view_id), now_ms);
        return finish(true);
    }
    if (was_holding)
        trace_event("DCS quad focus: hold %s handle=0x%llX route=%s reason=%s size_route=%s since_proof_ms=%llu",
            decision.allowed ? "ended, focus re-proven" : "released", static_cast<unsigned long long>(view_id),
            decision.route, dcs_quad_focus_reason_name(decision.reason), dcs_quad_size_proof_name(decision.size),
            static_cast<unsigned long long>(now_ms - proven_ms));
    if (decision.allowed) {
        // No call-order or unverified eye assignment fallback; the size route is
        // a full layout/census bijection and never runs when eyes matter.
        if (!transition.active)
            trace_event("DCS quad focus: mapped focus accepted handle=0x%llX resource=0x%llX route=%s output=(%u,%u %ux%u) census=focus%u/peripheral%u/other%u",
                static_cast<unsigned long long>(view_id), static_cast<unsigned long long>(identity), decision.route,
                x, y, width, height, decision.census.focus, decision.census.peripheral, decision.census.other);
        transition.trace.accepted();
        cheeky::dcsvr::dcs_focus_activity().accepted(static_cast<std::uint64_t>(view_id), now_ms);
        return finish(true);
    }
    cheeky::dcsvr::dcs_focus_activity().rejected(static_cast<std::uint32_t>(decision.reason));
    if (transition.trace.should_log(static_cast<unsigned>(decision.reason), decision.detail(), now_ms)) {
        const auto count = static_cast<unsigned long long>(transition.trace.rejects);
        const auto handle = static_cast<unsigned long long>(view_id);
        const char* reason = dcs_quad_focus_reason_name(decision.reason);
        if (decision.reason == DcsQuadFocusReason::snapshot_unavailable) {
            trace_event("DCS quad focus: reject handle=0x%llX count=%llu reason=%s layer_module=%s abi_compatible=%u",
                handle, count, reason, diagnostics.layer_present ? "loaded" : "missing", diagnostics.abi_compatible ? 1U : 0U);
        } else if (decision.reason == DcsQuadFocusReason::snapshot_not_ready) {
            char problems[160]{}, missing[160]{}, rejected[96]{};
            dcs_quad_problem_names(decision.snapshot_problems, problems, sizeof(problems));
            dcs_quad_flag_names(decision.missing_flags, missing, sizeof(missing));
            dcs_quad_flag_names(decision.rejected_flags, rejected, sizeof(rejected));
            trace_event("DCS quad focus: reject handle=0x%llX count=%llu reason=%s problems=%s flags=0x%X missing=%s rejected=%s views=%u generation=%llu display_time=%lld age_ms=%.1f abi=%u size=%u",
                handle, count, reason, problems, sample.status_flags, missing, rejected, sample.view_count,
                static_cast<unsigned long long>(sample.session_generation),
                static_cast<long long>(sample.predicted_display_time),
                dcs_quad_age_ms(now, sample.publication_qpc, freq), sample.abi_version, sample.structure_size);
        } else {
            const auto& v0 = sample.views[0];
            const auto& v1 = sample.views[1];
            const auto& l = layout;
            trace_event("DCS quad focus: reject handle=0x%llX count=%llu reason=%s matches=%u exact=%u copy=%u size_route=%s ngx=(%u,%u %ux%u) resource=0x%llX",
                handle, count, reason, decision.matches, decision.exact_matches, decision.copy_matches,
                dcs_quad_size_proof_name(decision.size), x, y, width, height, static_cast<unsigned long long>(identity));
            trace_event("DCS quad focus: detail handle=0x%llX focusL=(%d,%d %ux%u arr=%u flags=0x%X res=0x%llX chain=0x%llX size=%s) focusR=(%d,%d %ux%u arr=%u flags=0x%X res=0x%llX chain=0x%llX size=%s) copy_edges=%llu submitted_copies=%llu",
                handle, v0.image_rect_x, v0.image_rect_y, v0.image_rect_width, v0.image_rect_height, v0.array_index, v0.flags,
                static_cast<unsigned long long>(v0.resource_identity), static_cast<unsigned long long>(v0.swapchain_identity),
                v0.image_rect_width == width && v0.image_rect_height == height ? "match" : "differ",
                v1.image_rect_x, v1.image_rect_y, v1.image_rect_width, v1.image_rect_height, v1.array_index, v1.flags,
                static_cast<unsigned long long>(v1.resource_identity), static_cast<unsigned long long>(v1.swapchain_identity),
                v1.image_rect_width == width && v1.image_rect_height == height ? "match" : "differ",
                static_cast<unsigned long long>(copy_graph.recent_edges(now_ms).size()),
                static_cast<unsigned long long>(diagnostics.submitted_copies));
            trace_event("DCS quad focus: layout handle=0x%llX export=%s valid=%u generation=%llu/%llu age_ms=%.1f peripheral=%ux%u,%ux%u focus=%ux%u,%ux%u arrays=%u,%u,%u,%u census=focus%u/peripheral%u/other%u eye_identity_required=%u",
                handle, layout_loaded ? "present" : "missing", l.valid,
                static_cast<unsigned long long>(l.session_generation), static_cast<unsigned long long>(sample.session_generation),
                dcs_quad_age_ms(now, l.publication_qpc, freq), l.width[0], l.height[0], l.width[1], l.height[1],
                l.width[2], l.height[2], l.width[3], l.height[3], l.array_index[0], l.array_index[1], l.array_index[2],
                l.array_index[3], decision.census.focus, decision.census.peripheral, decision.census.other,
                eye_identity_required ? 1U : 0U);
        }
    }
    return finish(false);
}

// Creation-time classification for the deferred DX11 game feature: 1 when the
// output extent is a Quad Views focus extent of the published layout (and not a
// peripheral one), 0 when a valid layout says it is not, -1 when no valid layout
// is available yet (the creation then cannot be classified).
// require_fresh: only a layout of the snapshot's session published within the
// last 250 ms (the size proof's own bound) counts; an older one may describe the
// previous mission's swapchains.
int dcs_quad_focus_extent(unsigned width, unsigned height, bool require_fresh) noexcept {
    std::lock_guard lock(coordinator_mutex);
    CheekyGazeSnapshotV1 sample{};
    DcsQuadLayoutV1 layout{};
    if (!load_snapshot(sample) || !dcs_quad_load_layout(layout) || !layout.valid) return -1;
    if (require_fresh) {
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
        const auto freq = static_cast<std::uint64_t>(frequency.QuadPart);
        const auto now = qpc_now();
        if (layout.session_generation != sample.session_generation || !freq || !layout.publication_qpc ||
            now < layout.publication_qpc || now - layout.publication_qpc > freq / 4U) return -1;
    }
    const auto is = [&](unsigned view) { return layout.width[view] == width && layout.height[view] == height; };
    return (is(2) || is(3)) && !is(0) && !is(1) ? 1 : 0;
}
int dcs_quad_focus_create_extent(unsigned width, unsigned height) noexcept {
    return dcs_quad_focus_extent(width, height, true);
}
// Any valid layout, however old: only used to keep a view on the private SR
// (DCS-equivalent output for any view), never to create DCS's own feature.
int dcs_quad_focus_layout_extent(unsigned width, unsigned height) noexcept {
    return dcs_quad_focus_extent(width, height, false);
}
