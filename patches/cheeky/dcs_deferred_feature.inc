// DCS VR Control: deferred DX11 DLSS game feature for Quad Views focus views.
// Included inside the anonymous namespace of hooks.cpp, after the DX11
// evaluation pipeline (evaluate_d3d11_impl, evaluate_with_eye_calibration).
//
// With the D3D11->D3D12 transport, a focus view is upscaled by a private D3D12
// SR feature, so DCS's own DX11 SR feature for that view sits unused in VRAM.
// DCS calls NVSDK_NGX_D3D11_CreateFeature on the NGX core (_nvngx.dll); under
// DCSVR_QUAD_FOCUS=1 and an SR-replacing transport configuration the core hook
// returns an opaque handle instead and stores the creation parameters. The real
// core feature is created only when something evaluates DCS's feature:
//   * every evaluation of an opaque handle enters the core EvaluateFeature hook,
//     which runs the normal DX11 pipeline with a runtime whose "original"
//     evaluate materializes the real feature first (fallback, disabled
//     passthrough, native-SR NR, private-path fallback: all of them call it);
//   * ReleaseFeature releases the real feature only if it exists;
//   * D3D11 Shutdown forgets real features the core destroys itself.
// DCS's own feature is created only when nothing else can produce the view:
//   * the Quad Views focus proof is not ready yet (first frames after a mission
//     load): a handle whose output is the published focus extent keeps running
//     on the private D3D12 SR without NR (the very feature it will use with NR,
//     DCS-equivalent output); with no layout published at all, a bilinear
//     upscale holds the view for at most dcs_deferred_unknown_hold_ms;
//   * DLSS 5 toggled off in flight: the private D3D12 SR keeps running without NR;
//   * a transport that rejects frames (slot timeout, private device or shared
//     texture creation failing under VRAM pressure, a sync failure): a bilinear
//     upscale holds the view for at most dcs_deferred_unknown_hold_ms of
//     consecutive failures, so a brief failure never creates DCS's feature;
//   * the genuine fallbacks (transport unavailable or failing for longer, a
//     peripheral extent, a layout that never appears) create it exactly as before.
// Once created, the real feature lives until DCS releases its handle: NGX's
// DX11 runtime does not return a released feature's VRAM while the game's other
// DX11 DLSS features (the peripheral views) stay alive (measured: 0 MiB back
// for a 254.5 MiB focus feature), so releasing it early would only add a create
// stall to the next fallback. The saving is the feature never evaluated:
// ~3.4 MiB when only created, ~254.5 MiB per 2076x2048 focus view once evaluated.

using NgxD3D11ShutdownFn = NgxResult (*)();
using NgxD3D11Shutdown1Fn = NgxResult (*)(ID3D11Device*);
std::atomic<EvaluateD3D11Fn> real_core_evaluate_d3d11{};
std::atomic<EvaluateD3D11CFn> real_core_evaluate_d3d11_c{};
std::atomic<NgxD3D11ShutdownFn> real_core_shutdown_d3d11{};
std::atomic<NgxD3D11Shutdown1Fn> real_core_shutdown_d3d11_1{};

constexpr unsigned dcs_deferred_magic = 0x44435346U; // "DCSF"
// A deferred view with no Quad Views layout at all is held (bilinear upscale) at
// most this long; then DCS's own feature is created (stereo session, layer gone).
// The same bound applies to a run of consecutive transport failures.
constexpr std::uint64_t dcs_deferred_unknown_hold_ms = 3000;

[[nodiscard]] bool dcs_quad_focus_env() noexcept {
    wchar_t value[2]{};
    return GetEnvironmentVariableW(L"DCSVR_QUAD_FOCUS", value, 2) == 1 && value[0] == L'1';
}

// What DCS holds instead of a core handle. NVSDK_NGX_Handle is { unsigned Id; }.
struct DcsDeferredHandle { unsigned id; unsigned magic; };

// Creation-time values replayed when the real feature is created. DCS shares
// one parameter object between its features and changes sizes between calls,
// so evaluation-time values are not the creation values.
struct DcsCreateValue { const char* name; bool is_int; bool present; unsigned value; };
constexpr const char* dcs_create_uint_keys[]{"Width", "Height", "OutWidth", "OutHeight", "CreationNodeMask",
    "VisibilityNodeMask", "DLSS.Hint.Render.Preset.DLAA", "DLSS.Hint.Render.Preset.Quality",
    "DLSS.Hint.Render.Preset.Balanced", "DLSS.Hint.Render.Preset.Performance",
    "DLSS.Hint.Render.Preset.UltraPerformance", "DLSS.Hint.Render.Preset.UltraQuality"};
constexpr const char* dcs_create_int_keys[]{"PerfQualityValue", "RTXValue", "DLSS.Feature.Create.Flags",
    "DLSS.Enable.Output.Subrects"};
constexpr std::size_t dcs_create_key_count = std::size(dcs_create_uint_keys) + std::size(dcs_create_int_keys);
using DcsCreateValues = std::array<DcsCreateValue, dcs_create_key_count>;

[[nodiscard]] DcsCreateValues dcs_capture_create_values(const NgxParameters* parameters) noexcept {
    DcsCreateValues values{};
    std::size_t index{};
    for (const auto* name : dcs_create_uint_keys) {
        auto& v = values[index++]; v.name = name;
        unsigned u{}; int i{};
        if (parameters && ngx_succeeded(parameters->Get(name, &u))) { v.present = true; v.value = u; }
        else if (parameters && ngx_succeeded(parameters->Get(name, &i))) { v.present = v.is_int = true; v.value = static_cast<unsigned>(i); }
    }
    for (const auto* name : dcs_create_int_keys) {
        auto& v = values[index++]; v.name = name; v.is_int = true;
        unsigned u{}; int i{};
        if (parameters && ngx_succeeded(parameters->Get(name, &i))) { v.present = true; v.value = static_cast<unsigned>(i); }
        else if (parameters && ngx_succeeded(parameters->Get(name, &u))) { v.present = true; v.is_int = false; v.value = u; }
    }
    return values;
}

// The current value of one creation key (index into DcsCreateValues).
void dcs_capture_create_value(const NgxParameters* parameters, std::size_t index, DcsCreateValue& v) noexcept {
    const bool uint_key = index < std::size(dcs_create_uint_keys);
    v = {};
    v.name = uint_key ? dcs_create_uint_keys[index] : dcs_create_int_keys[index - std::size(dcs_create_uint_keys)];
    unsigned u{}; int i{};
    if (uint_key) {
        if (parameters && ngx_succeeded(parameters->Get(v.name, &u))) { v.present = true; v.value = u; }
        else if (parameters && ngx_succeeded(parameters->Get(v.name, &i))) { v.present = v.is_int = true; v.value = static_cast<unsigned>(i); }
    } else {
        v.is_int = true;
        if (parameters && ngx_succeeded(parameters->Get(v.name, &i))) { v.present = true; v.value = static_cast<unsigned>(i); }
        else if (parameters && ngx_succeeded(parameters->Get(v.name, &u))) { v.present = true; v.is_int = false; v.value = u; }
    }
}

void dcs_apply_create_value(NgxParameters* parameters, const DcsCreateValue& v) noexcept {
    if (v.is_int) parameters->Set(v.name, static_cast<int>(v.value));
    else parameters->Set(v.name, v.value);
}

struct DcsDeferredFeature {
    DcsDeferredHandle* fake{};
    NgxHandle* real{};
    std::uint32_t feature{};
    std::size_t slot{};
    DcsCreateValues create{};
    std::uint64_t created_ms{}, materializations{};
    // Frames produced without DCS's own feature, when an unknown layout was first
    // seen, and when the current run of consecutive transport failures began.
    std::uint64_t sr_only_frames{}, held_frames{}, unknown_since_ms{}, transport_fail_since_ms{};
};
std::mutex dcs_deferred_mutex;
std::vector<DcsDeferredFeature> dcs_deferred_features;
std::atomic<unsigned> dcs_deferred_count{};
std::atomic<unsigned> dcs_deferred_next_id{0x7FD00000U};

[[nodiscard]] DcsDeferredFeature* dcs_find_deferred_locked(const NgxHandle* handle) noexcept {
    if (handle == nullptr) return nullptr;
    for (auto& entry : dcs_deferred_features)
        if (reinterpret_cast<const NgxHandle*>(entry.fake) == handle) return &entry;
    return nullptr;
}

bool dcs_is_deferred_d3d11(const NgxHandle* handle) noexcept {
    if (handle == nullptr || dcs_deferred_count.load(std::memory_order_acquire) == 0U) return false;
    std::lock_guard lock(dcs_deferred_mutex);
    return dcs_find_deferred_locked(handle) != nullptr;
}

// The DX11 SR snippet hooked as a public runtime: the transport needs its module
// and private DX11 features use its create/evaluate/release trampolines.
[[nodiscard]] bool dcs_public_d3d11_slot(std::size_t& slot) noexcept {
    for (std::size_t index{}; index < d3d11_runtimes.size(); ++index) {
        const auto& runtime = d3d11_runtimes[index];
        if (runtime.module.load(std::memory_order_acquire) && runtime.create.load(std::memory_order_acquire) &&
            runtime.evaluate.load(std::memory_order_acquire) && runtime.release.load(std::memory_order_acquire)) {
            slot = index;
            return true;
        }
    }
    return false;
}

bool dcs_try_defer_d3d11_create(ID3D11DeviceContext* const context, const std::uint32_t feature,
    NgxParameters* const parameters, NgxHandle** const handle, NgxResult& result) noexcept {
    if (feature != 1U || context == nullptr || parameters == nullptr || handle == nullptr || !dcs_quad_focus_env())
        return false;
    // Without the core evaluate detours an opaque handle could reach the real
    // core. NVIDIA's core exports no EvaluateFeature_C (the SDK's static library
    // routes it to EvaluateFeature); a core that does export it must be detoured too.
    const auto core = find_core_runtime();
    const auto get_proc = real_get_proc_address.load(std::memory_order_acquire);
    const bool core_exports_c = core && get_proc && get_proc(core, "NVSDK_NGX_D3D11_EvaluateFeature_C") != nullptr;
    if (!core || !real_core_evaluate_d3d11.load(std::memory_order_acquire) ||
        (core_exports_c && !real_core_evaluate_d3d11_c.load(std::memory_order_acquire)) ||
        !real_core_create_d3d11.load(std::memory_order_acquire) ||
        !real_core_release_d3d11.load(std::memory_order_acquire)) return false;
    const auto settings = current_settings();
    // Only configurations whose transport replaces DCS's SR on accepted views.
    // NR-only after upscaling evaluates DCS's own feature on every frame. NR
    // toggled off for the session (DLSS 5 key) still defers: those views keep the
    // private D3D12 SR without NR, and NR resumes on the same feature.
    const bool native_sr = !settings.enabled &&
        (!settings.nr_foveated || !settings.nr_use_sr_foveation) && !settings.peripheral_dlaa_enabled &&
        settings.nr_processing_order != NrProcessingOrder::before_upscaling;
    if (!settings.d3d11_use_d3d12_transport || native_sr) return false;
    std::size_t slot{};
    if (!dcs_public_d3d11_slot(slot)) return false;
    const auto out_width = get_ui(parameters, "OutWidth");
    const auto out_height = get_ui(parameters, "OutHeight");
    // Peripheral views always evaluate natively; only focus (or not yet
    // classifiable) extents are deferred.
    const int focus = dcs_quad_focus_create_extent(out_width, out_height);
    if (focus == 0) {
        trace_event("DCS deferred DX11 create skipped: output %ux%u is not a Quad Views focus extent", out_width, out_height);
        return false;
    }
    auto* fake = new (std::nothrow) DcsDeferredHandle{dcs_deferred_next_id.fetch_add(1U), dcs_deferred_magic};
    if (fake == nullptr) return false;
    DcsDeferredFeature entry{};
    entry.fake = fake;
    entry.feature = feature;
    entry.slot = slot;
    entry.create = dcs_capture_create_values(parameters);
    entry.created_ms = GetTickCount64();
    {
        std::lock_guard lock(dcs_deferred_mutex);
        dcs_deferred_features.push_back(entry);
        dcs_deferred_count.store(static_cast<unsigned>(dcs_deferred_features.size()), std::memory_order_release);
    }
    auto* const opaque = reinterpret_cast<NgxHandle*>(fake);
    const auto& runtime = d3d11_runtimes[slot];
    register_d3d11_game_feature(opaque, feature, runtime.create.load(std::memory_order_acquire),
        runtime.release.load(std::memory_order_acquire), NgxOutputExtent{out_width, out_height});
    register_stereo_view(static_cast<DlssViewId>(reinterpret_cast<std::uintptr_t>(opaque)));
    *handle = opaque;
    result = 1U;
    const auto live = dcs_ledger_created(opaque, 1U, feature, parameters, true);
    trace_event("DCS deferred DX11 DLSS create handle=%p input=%ux%u output=%ux%u flags=0x%08X quality=%u focus=%s "
        "slot=%zu: no VRAM until DCS's own feature is evaluated; live_game_features=%u",
        opaque, get_ui(parameters, "Width"), get_ui(parameters, "Height"), out_width, out_height,
        get_ngx_integer_bits(parameters, "DLSS.Feature.Create.Flags"), get_ngx_integer_bits(parameters, "PerfQualityValue"),
        focus > 0 ? "layout" : "unclassified", slot, live);
    if (live > 1U) dcs_ledger_trace("after-create");
    dcs_release_idle_views(opaque);
    return true;
}

// The private D3D12 SR of a deferred view stands in for DCS's own feature, so it
// must see DCS's creation-time DLSS parameters (PerfQualityValue, preset hints,
// create flags, render and output size), not whatever DCS's shared bag holds at
// evaluation. Replayed only where they differ, restored after the evaluation.
struct DcsCreateValuesSaved {
    NgxParameters* bag{};
    DcsCreateValues current{};
    std::array<bool, dcs_create_key_count> changed{};
};
thread_local std::vector<DcsCreateValuesSaved> dcs_create_values_stack;

bool dcs_deferred_push_create_values(const NgxHandle* const handle, const NgxParameters* const parameters) noexcept {
    if (parameters == nullptr || handle == nullptr || dcs_deferred_count.load(std::memory_order_acquire) == 0U) return false;
    DcsCreateValues create{};
    {
        std::lock_guard lock(dcs_deferred_mutex);
        const auto* entry = dcs_find_deferred_locked(handle);
        if (entry == nullptr) return false;
        create = entry->create;
    }
    DcsCreateValuesSaved saved{};
    saved.bag = const_cast<NgxParameters*>(parameters);
    // Only the keys DCS set at creation are replayed and restored, so only
    // those are read now (an absent key costs two failed lookups).
    for (std::size_t i{}; i < create.size(); ++i)
        if (create[i].present) dcs_capture_create_value(saved.bag, i, saved.current[i]);
    unsigned replayed{};
    for (std::size_t i{}; i < create.size(); ++i) {
        if (!create[i].present) continue;
        if (saved.current[i].present && saved.current[i].value == create[i].value &&
            saved.current[i].is_int == create[i].is_int) continue;
        dcs_apply_create_value(saved.bag, create[i]);
        saved.changed[i] = true;
        ++replayed;
    }
    if (replayed) {
        static std::atomic<std::uint64_t> traced{};
        const auto n = ++traced;
        if (n <= 4U || (n & (n - 1U)) == 0U)
            trace_event("DCS deferred DX11 DLSS private SR uses DCS's creation-time DLSS parameters handle=%p replayed=%u "
                "PerfQualityValue=%u (evaluation bag held %u) count=%llu", handle, replayed, create[12].value,
                saved.current[12].value, static_cast<unsigned long long>(n));
    }
    try { dcs_create_values_stack.push_back(saved); }
    catch (...) {
        for (std::size_t i{}; i < saved.current.size(); ++i)
            if (saved.changed[i] && saved.current[i].present) dcs_apply_create_value(saved.bag, saved.current[i]);
        return false;
    }
    return true;
}

void dcs_deferred_pop_create_values() noexcept {
    if (dcs_create_values_stack.empty()) return;
    const auto saved = dcs_create_values_stack.back();
    dcs_create_values_stack.pop_back();
    for (std::size_t i{}; i < saved.current.size(); ++i)
        if (saved.changed[i] && saved.current[i].present) dcs_apply_create_value(saved.bag, saved.current[i]);
}

// Creates the real core feature with the stored creation values. Called on the
// render thread from the evaluation that needs DCS's own feature.
[[nodiscard]] NgxHandle* dcs_materialize_deferred(ID3D11DeviceContext* const context, const NgxHandle* const handle,
    const NgxParameters* const parameters, const char* const reason, NgxResult& result) noexcept {
    DcsCreateValues create{};
    std::uint32_t feature{};
    {
        std::lock_guard lock(dcs_deferred_mutex);
        auto* entry = dcs_find_deferred_locked(handle);
        if (entry == nullptr) { result = 0xBAD00005U; return nullptr; }
        if (entry->real) return entry->real;
        create = entry->create;
        feature = entry->feature;
    }
    const auto create_feature = real_core_create_d3d11.load(std::memory_order_acquire);
    if (create_feature == nullptr || parameters == nullptr || context == nullptr) { result = 0xBAD00007U; return nullptr; }
    auto* const bag = const_cast<NgxParameters*>(parameters);
    // Replay only values that differ; restore exactly those afterwards.
    const auto current = dcs_capture_create_values(bag);
    std::array<bool, dcs_create_key_count> changed{};
    for (std::size_t i{}; i < create.size(); ++i) {
        if (!create[i].present) continue;
        if (current[i].present && current[i].value == create[i].value) continue;
        dcs_apply_create_value(bag, create[i]);
        changed[i] = true;
    }
    const auto before = dcs_vram_sample(context);
    const auto create_started = GetTickCount64();
    NgxHandle* real{};
    dcs_materializing_feature = true;
    result = create_feature(context, feature, bag, &real);
    dcs_materializing_feature = false;
    const auto elapsed = GetTickCount64() - create_started;
    for (std::size_t i{}; i < current.size(); ++i)
        if (changed[i] && current[i].present) dcs_apply_create_value(bag, current[i]);
    const auto after = dcs_vram_sample(context);
    std::uint64_t count{};
    bool keep{};
    if (ngx_succeeded(result) && real) {
        std::lock_guard lock(dcs_deferred_mutex);
        if (auto* entry = dcs_find_deferred_locked(handle); entry && !entry->real) {
            entry->real = real;
            count = ++entry->materializations;
            keep = true;
        }
    }
    if (ngx_succeeded(result) && real && !keep) {
        static_cast<void>(real_core_release_d3d11.load(std::memory_order_acquire)(real));
        real = nullptr;
        result = 0xBAD00005U;
    }
    trace_event("DCS deferred DX11 DLSS materialized handle=%p real=%p reason=%s result=0x%08X count=%llu "
        "input=%ux%u output=%ux%u elapsed_ms=%llu vram_before_mb=%.1f vram_after_mb=%.1f delta_mb=%+.1f",
        handle, real, reason, unsigned(result), static_cast<unsigned long long>(count),
        create[0].value, create[1].value, create[2].value, create[3].value, static_cast<unsigned long long>(elapsed),
        dcs_mb(before.usage), dcs_mb(after.usage), dcs_delta_mb(before, after));
    return keep ? real : nullptr;
}

// Why the next materialization happens (set by the paths that tried to avoid it).
thread_local const char* dcs_deferred_fallback_reason{};

// The pipeline's "original" evaluate for an opaque handle: DCS's own feature is
// needed now, so create it first. Other handles (private DX11 features created
// through the snippet) keep the snippet's own evaluate.
template<class Callback, class Evaluate>
NgxResult dcs_evaluate_real(ID3D11DeviceContext* context, const NgxHandle* handle,
    const NgxParameters* parameters, Callback callback, Evaluate evaluate_real) {
    NgxResult result{};
    bool created_now{};
    {
        std::lock_guard lock(dcs_deferred_mutex);
        const auto* entry = dcs_find_deferred_locked(handle);
        created_now = entry && !entry->real;
    }
    const auto before = created_now ? dcs_vram_sample(context) : DcsVram{};
    const char* const reason = dcs_deferred_fallback_reason ? dcs_deferred_fallback_reason : "game-feature-evaluation";
    dcs_deferred_fallback_reason = nullptr;
    auto* real = dcs_materialize_deferred(context, handle, parameters, reason, result);
    if (real == nullptr) return ngx_succeeded(result) ? 0xBAD00005U : result;
    result = evaluate_real(real, callback);
    // NGX allocates a DLSS feature's working set on its first evaluation.
    if (created_now) {
        const auto after = dcs_vram_sample(context);
        trace_event("DCS deferred DX11 DLSS first evaluation handle=%p real=%p result=0x%08X vram_before_create_mb=%.1f "
            "vram_after_evaluation_mb=%.1f delta_mb=%+.1f", handle, real, unsigned(result), dcs_mb(before.usage),
            dcs_mb(after.usage), dcs_delta_mb(before, after));
    }
    return result;
}

// True for an opaque handle whose real feature does not exist (yet).
bool dcs_deferred_unmaterialized(const NgxHandle* handle) noexcept {
    if (handle == nullptr || dcs_deferred_count.load(std::memory_order_acquire) == 0U) return false;
    std::lock_guard lock(dcs_deferred_mutex);
    const auto* entry = dcs_find_deferred_locked(handle);
    return entry != nullptr && entry->real == nullptr;
}

// Bounded trace: the first four frames of each kind, then powers of two.
[[nodiscard]] bool dcs_deferred_count_frame(const NgxHandle* handle, bool sr_only, std::uint64_t& count) noexcept {
    std::lock_guard lock(dcs_deferred_mutex);
    auto* entry = dcs_find_deferred_locked(handle);
    if (entry == nullptr) return false;
    count = sr_only ? ++entry->sr_only_frames : ++entry->held_frames;
    return count <= 4U || (count & (count - 1U)) == 0U;
}

// The transport produced this frame: a later failure starts a new run.
void dcs_deferred_transport_ok(const NgxHandle* handle) noexcept {
    if (handle == nullptr || dcs_deferred_count.load(std::memory_order_acquire) == 0U) return;
    std::lock_guard lock(dcs_deferred_mutex);
    if (auto* entry = dcs_find_deferred_locked(handle)) entry->transport_fail_since_ms = 0U;
}

// Plain bilinear upscale of DCS's colour input into its output (dcs_hold_upscale.hpp).
[[nodiscard]] bool dcs_deferred_bilinear(ID3D11DeviceContext* const context, const NgxParameters* const parameters) noexcept {
    if (context == nullptr || parameters == nullptr) return false;
    ID3D11Resource* color{};
    ID3D11Resource* output{};
    parameters->Get("Color", &color);
    parameters->Get("Output", &output);
    auto render_width = get_ui(parameters, "DLSS.Render.Subrect.Dimensions.Width");
    auto render_height = get_ui(parameters, "DLSS.Render.Subrect.Dimensions.Height");
    if (!render_width) render_width = get_ui(parameters, "Width");
    if (!render_height) render_height = get_ui(parameters, "Height");
    const DcsHoldUpscaleRegion region{get_ui(parameters, "DLSS.Input.Color.Subrect.Base.X"),
        get_ui(parameters, "DLSS.Input.Color.Subrect.Base.Y"), render_width, render_height,
        get_ui(parameters, "DLSS.Output.Subrect.Base.X"), get_ui(parameters, "DLSS.Output.Subrect.Base.Y"),
        get_ui(parameters, "OutWidth"), get_ui(parameters, "OutHeight")};
    return dcs_hold_upscale(context, color, output, region);
}

// The transport rejected a frame of an accepted deferred view (slot timeout,
// private device start, shared texture creation under VRAM pressure, a sync
// failure). DCS's own feature would never be freed again (~254 MiB), so a run
// of consecutive failures is held with a bilinear upscale for at most
// dcs_deferred_unknown_hold_ms; the view's transport resources are kept, so
// the next frame retries the transport. False leaves the caller's fallback.
bool dcs_deferred_hold_transport_failure(ID3D11DeviceContext* const context, const NgxHandle* const handle,
    const NgxParameters* const parameters, NgxResult& held) noexcept {
    if (!dcs_deferred_unmaterialized(handle) || context == nullptr || parameters == nullptr) return false;
    const auto now = GetTickCount64();
    std::uint64_t since{};
    {
        std::lock_guard lock(dcs_deferred_mutex);
        auto* entry = dcs_find_deferred_locked(handle);
        if (entry == nullptr) return false;
        if (!entry->transport_fail_since_ms) entry->transport_fail_since_ms = now;
        since = entry->transport_fail_since_ms;
    }
    if (now - since > dcs_deferred_unknown_hold_ms) {
        trace_event("DCS deferred DX11 DLSS handle=%p: transport failing for %llu ms; DCS's own feature is created",
            handle, static_cast<unsigned long long>(now - since));
        dcs_deferred_fallback_reason = "transport-failure-timeout";
        return false;
    }
    if (!dcs_deferred_bilinear(context, parameters)) {
        trace_event("DCS deferred DX11 DLSS handle=%p: transport failed and the bilinear hold is unavailable; "
            "DCS's own feature is created", handle);
        dcs_deferred_fallback_reason = "transport-failed-hold-unavailable";
        return false;
    }
    skip_dlss_nr_history(static_cast<DlssViewId>(reinterpret_cast<std::uintptr_t>(handle)));
    diagnostic_note_d3d11_execution_path(D3D11ExecutionPath::game_fallback);
    if (std::uint64_t frames{}; dcs_deferred_count_frame(handle, false, frames))
        trace_event("DCS deferred DX11 DLSS held with a bilinear upscale handle=%p reason=transport-failed frames=%llu "
            "failing_ms=%llu (DCS's own DX11 feature not created)", handle, static_cast<unsigned long long>(frames),
            static_cast<unsigned long long>(now - since));
    held = 1U;
    return true;
}

// DCS's own feature is not needed for this frame: the private D3D12 SR runs on
// the handle exactly as with DLSS 5 (full view, same inputs, same feature and
// textures), only the NR pass is skipped. Its output is DCS's own upscale, so
// this is correct for any view; it is used for focus-extent views and for the
// session with DLSS 5 off. False when the transport cannot run the frame.
[[nodiscard]] bool dcs_deferred_sr_only(const D3D11RuntimeCallbacks& runtime, ID3D11DeviceContext* const context,
    const NgxHandle* const handle, const NgxParameters* const parameters, const Settings& settings,
    const char* const reason, NgxResult& result) noexcept {
    if (!settings.d3d11_use_d3d12_transport || context == nullptr || parameters == nullptr) return false;
    auto sr_only = settings;
    sr_only.enabled = false;            // Full-view SR: the NR-only transport geometry, never a foveated crop.
    sr_only.nr_enabled = true;          // Same textures/feature as with NR, so NR resumes without rebuilding them.
    sr_only.dcs_nr_suppressed = true;   // ...but the NR pass itself is skipped.
    sr_only.peripheral_dlaa_enabled = false;
    skip_dlss_nr_history(static_cast<DlssViewId>(reinterpret_cast<std::uintptr_t>(handle)));
    NgxResult transport_result{};
    bool owned{};
    {
        // DCS's creation-time quality first; the profile's preset override, if any, on top (as on the NR path).
        DcsDeferredCreateValuesScope create_values{handle, parameters};
        NgxPresetOverrideScope center_preset{parameters, settings.center_preset};
        owned = evaluate_d3d11_via_d3d12(context, handle, parameters, sr_only, current_transport_ngx(runtime),
            transport_result, nullptr);
    }
    if (!owned || !ngx_succeeded(transport_result)) {
        trace_event("DCS deferred DX11 DLSS: private D3D12 SR unavailable handle=%p reason=%s owned=%u result=0x%08X",
            handle, reason, owned ? 1U : 0U, unsigned(transport_result));
        // A brief failure is held; only a lasting one creates DCS's own feature.
        if (dcs_deferred_hold_transport_failure(context, handle, parameters, result)) return true;
        if (!dcs_deferred_fallback_reason) dcs_deferred_fallback_reason = "transport-unavailable";
        return false;
    }
    {
        std::lock_guard lock(dcs_deferred_mutex);
        if (auto* entry = dcs_find_deferred_locked(handle)) entry->transport_fail_since_ms = entry->unknown_since_ms = 0U;
    }
    release_d3d11_peripheral_dlaa_view(handle);
    diagnostic_note_d3d11_execution_path(D3D11ExecutionPath::dx12_transport);
    diagnostic_note_state(DiagnosticApi::d3d11, DiagnosticState::active);
    diagnostic_note_result(DiagnosticApi::d3d11, transport_result);
    if (std::uint64_t frames{}; dcs_deferred_count_frame(handle, true, frames))
        trace_event("DCS deferred DX11 DLSS kept on the private D3D12 SR without NR handle=%p reason=%s frames=%llu "
            "(DCS's own DX11 feature not created)", handle, reason, static_cast<unsigned long long>(frames));
    result = transport_result;
    return true;
}

// Gate rejected a deferred focus handle (proof not ready). True when the frame
// was produced without DCS's own feature; false leaves the caller's fallback.
bool dcs_deferred_hold_unproven(const D3D11RuntimeCallbacks& runtime, ID3D11DeviceContext* const context,
    const NgxHandle* const handle, const NgxParameters* const parameters, NgxResult& result) noexcept {
    if (!dcs_deferred_unmaterialized(handle) || parameters == nullptr) return false;
    const auto out_width = get_ui(parameters, "OutWidth");
    const auto out_height = get_ui(parameters, "OutHeight");
    // A fresh layout of this session decides either way. An older one (DCS still
    // loading) may only keep the view on the private SR, whose output is correct
    // for any view; it never creates DCS's own feature.
    int role = dcs_quad_focus_create_extent(out_width, out_height);
    if (role < 0 && dcs_quad_focus_layout_extent(out_width, out_height) > 0) role = 1;
    if (role >= 0) {
        std::lock_guard lock(dcs_deferred_mutex);
        if (auto* entry = dcs_find_deferred_locked(handle)) entry->unknown_since_ms = 0U;
    }
    if (role > 0) // The published layout says focus extent: DCS-equivalent SR on the private feature.
        return dcs_deferred_sr_only(runtime, context, handle, parameters, current_settings(), "focus-proof-pending", result);
    const auto now = GetTickCount64();
    if (role == 0) {
        // A peripheral (or other) extent: DCS's own feature is the right owner.
        trace_event("DCS deferred DX11 DLSS handle=%p output=%ux%u is not a Quad Views focus extent; DCS's own feature is created",
            handle, out_width, out_height);
        dcs_deferred_fallback_reason = "not-a-focus-extent";
        return false;
    }
    // No layout published yet: hold with a bilinear upscale for a bounded time.
    std::uint64_t since{};
    {
        std::lock_guard lock(dcs_deferred_mutex);
        auto* entry = dcs_find_deferred_locked(handle);
        if (entry == nullptr) return false;
        // The hold drops the transport view, so a later transport failure starts a run of its own.
        entry->transport_fail_since_ms = 0U;
        if (!entry->unknown_since_ms) entry->unknown_since_ms = now;
        since = entry->unknown_since_ms;
    }
    if (now - since > dcs_deferred_unknown_hold_ms) {
        trace_event("DCS deferred DX11 DLSS handle=%p: no Quad Views layout after %llu ms; DCS's own feature is created",
            handle, static_cast<unsigned long long>(now - since));
        dcs_deferred_fallback_reason = "layout-unknown-timeout";
        return false;
    }
    if (!dcs_deferred_bilinear(context, parameters)) {
        trace_event("DCS deferred DX11 DLSS handle=%p: bilinear hold unavailable; DCS's own feature is created", handle);
        dcs_deferred_fallback_reason = "hold-unavailable";
        return false;
    }
    static_cast<void>(release_d3d11_transport_view(handle, dcs_transport_release_wait_ms)); // Retried next frame on timeout.
    skip_dlss_nr_history(static_cast<DlssViewId>(reinterpret_cast<std::uintptr_t>(handle)));
    if (std::uint64_t frames{}; dcs_deferred_count_frame(handle, false, frames))
        trace_event("DCS deferred DX11 DLSS held with a bilinear upscale handle=%p reason=layout-unknown frames=%llu "
            "waited_ms=%llu (DCS's own DX11 feature not created)", handle, static_cast<unsigned long long>(frames),
            static_cast<unsigned long long>(now - since));
    result = 1U;
    return true;
}

// Accepted focus view with DLSS 5 off for the session (in-flight key): keep the
// private D3D12 SR instead of creating DCS's own feature.
bool dcs_deferred_keep_transport(const D3D11RuntimeCallbacks& runtime, ID3D11DeviceContext* const context,
    const NgxHandle* const handle, const NgxParameters* const parameters, const Settings& settings,
    NgxResult& result) noexcept {
    return dcs_deferred_unmaterialized(handle) &&
        dcs_deferred_sr_only(runtime, context, handle, parameters, settings, "dlss5-off", result);
}

NgxResult dcs_deferred_evaluate(ID3D11DeviceContext* context, const NgxHandle* handle,
    const NgxParameters* parameters, NgxProgressCallback callback) {
    if (dcs_is_deferred_d3d11(handle)) {
        return dcs_evaluate_real(context, handle, parameters, callback, [&](NgxHandle* real, NgxProgressCallback cb) {
            const auto evaluate = real_core_evaluate_d3d11.load(std::memory_order_acquire);
            return evaluate ? evaluate(context, real, parameters, cb) : 0xBAD00007U;
        });
    }
    std::size_t slot{};
    const auto evaluate = dcs_public_d3d11_slot(slot) ? d3d11_runtimes[slot].evaluate.load(std::memory_order_acquire) : nullptr;
    return evaluate ? evaluate(context, handle, parameters, callback) : 0xBAD00007U;
}

NgxResult dcs_deferred_evaluate_c(ID3D11DeviceContext* context, const NgxHandle* handle,
    const NgxParameters* parameters, NgxProgressCallbackC callback) {
    if (dcs_is_deferred_d3d11(handle)) {
        return dcs_evaluate_real(context, handle, parameters, callback, [&](NgxHandle* real, NgxProgressCallbackC cb) {
            // NVIDIA's core has no EvaluateFeature_C; the SDK routes C callers to EvaluateFeature.
            if (const auto evaluate_c = real_core_evaluate_d3d11_c.load(std::memory_order_acquire))
                return evaluate_c(context, real, parameters, cb);
            const auto evaluate = real_core_evaluate_d3d11.load(std::memory_order_acquire);
            return evaluate ? evaluate(context, real, parameters, nullptr) : 0xBAD00007U;
        });
    }
    std::size_t slot{};
    const auto evaluate = dcs_public_d3d11_slot(slot) ? d3d11_runtimes[slot].evaluate_c.load(std::memory_order_acquire) : nullptr;
    return evaluate ? evaluate(context, handle, parameters, callback) : 0xBAD00007U;
}

// One runtime per public slot: the snippet's module/create/release plus the
// materializing evaluates.
std::array<D3D11RuntimeCallbacks, d3d11_runtime_capacity> dcs_deferred_runtimes{};
[[nodiscard]] const D3D11RuntimeCallbacks& dcs_deferred_runtime(std::size_t slot) noexcept {
    auto& runtime = dcs_deferred_runtimes[slot];
    const auto& source = d3d11_runtimes[slot];
    runtime.module.store(source.module.load(std::memory_order_acquire), std::memory_order_release);
    runtime.init.store(source.init.load(std::memory_order_acquire), std::memory_order_release);
    runtime.create.store(source.create.load(std::memory_order_acquire), std::memory_order_release);
    runtime.release.store(source.release.load(std::memory_order_acquire), std::memory_order_release);
    runtime.evaluate.store(&dcs_deferred_evaluate, std::memory_order_release);
    runtime.evaluate_c.store(&dcs_deferred_evaluate_c, std::memory_order_release);
    return runtime;
}

template<class Invoke>
NgxResult dcs_evaluate_deferred(ID3D11DeviceContext* const context, const NgxHandle* const handle,
    const NgxParameters* const parameters, Invoke invoke) {
    std::size_t slot{};
    {
        std::lock_guard lock(dcs_deferred_mutex);
        auto* entry = dcs_find_deferred_locked(handle);
        if (entry == nullptr) return 0xBAD00005U;
        slot = entry->slot;
    }
    diagnostic_note_dlss_source(DiagnosticApi::d3d11, slot != 0U);
    const auto& runtime = dcs_deferred_runtime(slot);
    return evaluate_with_eye_calibration(context, handle, parameters, [&] { return invoke(runtime); });
}

NgxResult hook_core_evaluate_d3d11(ID3D11DeviceContext* const context, const NgxHandle* const handle,
    const NgxParameters* const parameters, const NgxProgressCallback callback) {
    if (!dcs_is_deferred_d3d11(handle)) {
        const auto original = real_core_evaluate_d3d11.load(std::memory_order_acquire);
        return original ? original(context, handle, parameters, callback) : 0xBAD00007U;
    }
    if (calibration_evaluation_depth) // Nested inside another evaluation: just DCS's feature.
        return dcs_deferred_evaluate(context, handle, parameters, callback);
    return dcs_evaluate_deferred(context, handle, parameters, [&](const D3D11RuntimeCallbacks& runtime) {
        return evaluate_d3d11_impl(runtime, context, handle, parameters, callback);
    });
}

NgxResult hook_core_evaluate_d3d11_c(ID3D11DeviceContext* const context, const NgxHandle* const handle,
    const NgxParameters* const parameters, const NgxProgressCallbackC callback) {
    if (!dcs_is_deferred_d3d11(handle)) {
        const auto original = real_core_evaluate_d3d11_c.load(std::memory_order_acquire);
        return original ? original(context, handle, parameters, callback) : 0xBAD00007U;
    }
    if (calibration_evaluation_depth)
        return dcs_deferred_evaluate_c(context, handle, parameters, callback);
    return dcs_evaluate_deferred(context, handle, parameters, [&](const D3D11RuntimeCallbacks& runtime) {
        return evaluate_d3d11_c_impl(runtime, context, handle, parameters, callback);
    });
}

// ReleaseFeature of an opaque handle: release the real feature only if it was
// ever created, then the per-view state, then the opaque handle itself.
bool dcs_release_deferred_d3d11(NgxHandle* const handle, NgxResult& result) noexcept {
    if (!dcs_is_deferred_d3d11(handle)) return false;
    DcsDeferredFeature entry{};
    {
        std::lock_guard lock(dcs_deferred_mutex);
        auto* found = dcs_find_deferred_locked(handle);
        if (found == nullptr) return false;
        entry = *found;
        dcs_deferred_features.erase(dcs_deferred_features.begin() + (found - dcs_deferred_features.data()));
        dcs_deferred_count.store(static_cast<unsigned>(dcs_deferred_features.size()), std::memory_order_release);
    }
    result = dcs_traced_feature_release(1U, handle, [&]() -> NgxResult {
        if (entry.real) {
            const auto release_feature = real_core_release_d3d11.load(std::memory_order_acquire);
            if (release_feature) return release_feature(entry.real);
        }
        return 1U;
    });
    // The handle is gone for good: wait for its private work as upstream's ReleaseFeature does.
    release_d3d11_transport_view(handle, dcs_transport_release_wait_forever);
    release_d3d11_peripheral_dlaa_view(handle);
    unregister_d3d11_game_feature(handle);
    const auto view_id = static_cast<DlssViewId>(reinterpret_cast<std::uintptr_t>(handle));
    unregister_stereo_view(view_id);
    forget_gaze_view(view_id);
    trace_event("DCS deferred DX11 DLSS release handle=%p real=%p materializations=%llu result=0x%08X",
        handle, entry.real, static_cast<unsigned long long>(entry.materializations), unsigned(result));
    delete entry.fake;
    return true;
}

// The core destroys every feature of the device at shutdown; never release
// those real handles again.
void dcs_forget_deferred_reals(const char* reason) noexcept {
    std::lock_guard lock(dcs_deferred_mutex);
    unsigned forgotten{};
    for (auto& entry : dcs_deferred_features) if (entry.real) { entry.real = nullptr; ++forgotten; }
    if (!dcs_deferred_features.empty())
        trace_event("DCS deferred DX11 DLSS %s: forgot %u core-owned features, %zu opaque handles remain",
            reason, forgotten, dcs_deferred_features.size());
}

NgxResult hook_core_shutdown_d3d11() {
    const auto original = real_core_shutdown_d3d11.load(std::memory_order_acquire);
    const auto result = original ? original() : 0xBAD00007U;
    dcs_forget_deferred_reals("shutdown");
    return result;
}

NgxResult hook_core_shutdown_d3d11_1(ID3D11Device* const device) {
    const auto original = real_core_shutdown_d3d11_1.load(std::memory_order_acquire);
    const auto result = original ? original(device) : 0xBAD00007U;
    dcs_forget_deferred_reals("shutdown1");
    return result;
}
