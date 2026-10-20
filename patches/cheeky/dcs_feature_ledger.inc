// DCS VR Control: NGX feature ledger with VRAM sampling, and forward declarations
// for the deferred DX11 feature (dcs_deferred_feature.inc). Included inside the
// anonymous namespace of hooks.cpp, before the first feature-creation hook.
//
// Every game-visible DLSS feature create/release is logged with the process'
// local-segment VRAM (QueryVideoMemoryInfo) before and after the call, plus the
// number of features still alive and how often each one was evaluated. This is
// what distinguishes a legitimate recreation (old handles released first) from a
// leak (old handles never released, never evaluated). Creates and releases are
// rare (a handful per mission), so these lines are never rate-limited.

struct DcsVram { bool valid{}; std::uint64_t usage{}, budget{}; };

std::mutex dcs_vram_mutex;
IDXGIAdapter3* dcs_vram_adapter{}; // First adapter seen; kept for release-time samples.
LUID dcs_vram_luid{};

[[nodiscard]] DcsVram dcs_vram_query(IDXGIAdapter3* adapter) noexcept {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (adapter == nullptr || FAILED(adapter->QueryVideoMemoryInfo(0U, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        return {};
    return {true, info.CurrentUsage, info.Budget};
}

void dcs_vram_remember(IDXGIAdapter3* adapter) noexcept {
    if (adapter == nullptr) return;
    std::lock_guard lock(dcs_vram_mutex);
    if (dcs_vram_adapter != nullptr) return;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) return;
    adapter->AddRef();
    dcs_vram_adapter = adapter;
    dcs_vram_luid = desc.AdapterLuid;
}

[[nodiscard]] DcsVram dcs_vram_sample() noexcept {
    std::lock_guard lock(dcs_vram_mutex);
    return dcs_vram_query(dcs_vram_adapter);
}

[[nodiscard]] DcsVram dcs_vram_sample(ID3D11DeviceContext* context) noexcept {
    if (context == nullptr) return dcs_vram_sample();
    ID3D11Device* device{};
    context->GetDevice(&device);
    IDXGIDevice* dxgi{};
    IDXGIAdapter* adapter{};
    IDXGIAdapter3* adapter3{};
    if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)))
        static_cast<void>(adapter->QueryInterface(IID_PPV_ARGS(&adapter3)));
    dcs_vram_remember(adapter3);
    const auto sample = dcs_vram_query(adapter3);
    if (adapter3) adapter3->Release();
    if (adapter) adapter->Release();
    if (dxgi) dxgi->Release();
    if (device) device->Release();
    return sample.valid ? sample : dcs_vram_sample();
}

[[nodiscard]] DcsVram dcs_vram_sample(ID3D12GraphicsCommandList* list) noexcept {
    if (list == nullptr) return dcs_vram_sample();
    ID3D12Device* device{};
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) return dcs_vram_sample();
    const auto luid = device->GetAdapterLuid();
    device->Release();
    {
        std::lock_guard lock(dcs_vram_mutex);
        if (dcs_vram_adapter && dcs_vram_luid.LowPart == luid.LowPart && dcs_vram_luid.HighPart == luid.HighPart)
            return dcs_vram_query(dcs_vram_adapter);
    }
    IDXGIFactory4* factory{};
    IDXGIAdapter3* adapter{};
    DcsVram sample{};
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
        dcs_vram_remember(adapter);
        sample = dcs_vram_query(adapter);
    }
    if (adapter) adapter->Release();
    if (factory) factory->Release();
    return sample;
}

[[nodiscard]] double dcs_mb(std::uint64_t bytes) noexcept { return static_cast<double>(bytes) / (1024.0 * 1024.0); }
[[nodiscard]] double dcs_delta_mb(const DcsVram& before, const DcsVram& after) noexcept {
    return before.valid && after.valid
        ? (static_cast<double>(after.usage) - static_cast<double>(before.usage)) / (1024.0 * 1024.0) : 0.0;
}

// Game-visible features only (the handles the game owns), never private ones.
struct DcsFeatureRecord {
    const void* handle{};
    unsigned route{};
    std::uint32_t feature{}, width{}, height{}, out_width{}, out_height{};
    std::uint64_t created_ms{}, evaluations{}, sequence{}, last_evaluated_ms{};
    bool deferred{};
    // The view's private resources were released while it sat idle (see
    // dcs_release_idle_views); an evaluation clears it.
    bool idle_released{};
};
std::mutex dcs_ledger_mutex;
std::vector<DcsFeatureRecord> dcs_ledger;
std::uint64_t dcs_ledger_sequence{};
const char* const dcs_route_names[]{"D3D11-public", "D3D11-core", "D3D12-public", "D3D12-core"};

// Returns the live count after recording.
unsigned dcs_ledger_created(const void* handle, unsigned route, std::uint32_t feature,
    const NgxParameters* parameters, bool deferred) noexcept {
    std::lock_guard lock(dcs_ledger_mutex);
    for (auto& record : dcs_ledger) if (record.handle == handle) { record.route = route; return static_cast<unsigned>(dcs_ledger.size()); }
    DcsFeatureRecord record{};
    record.handle = handle; record.route = route; record.feature = feature;
    record.width = get_ui(parameters, "Width"); record.height = get_ui(parameters, "Height");
    record.out_width = get_ui(parameters, "OutWidth"); record.out_height = get_ui(parameters, "OutHeight");
    record.created_ms = GetTickCount64(); record.sequence = ++dcs_ledger_sequence; record.deferred = deferred;
    dcs_ledger.push_back(record);
    return static_cast<unsigned>(dcs_ledger.size());
}

// True on the first evaluation of a recorded game feature.
bool dcs_ledger_evaluated(const void* handle) noexcept {
    std::lock_guard lock(dcs_ledger_mutex);
    for (auto& record : dcs_ledger) if (record.handle == handle) {
        record.last_evaluated_ms = GetTickCount64();
        record.idle_released = false;
        return ++record.evaluations == 1U;
    }
    return false;
}

// Removes the record; false when the handle was never recorded (a private or
// nested-route handle).
bool dcs_ledger_released(const void* handle, DcsFeatureRecord& out, unsigned& live) noexcept {
    std::lock_guard lock(dcs_ledger_mutex);
    for (auto it = dcs_ledger.begin(); it != dcs_ledger.end(); ++it) {
        if (it->handle != handle) continue;
        out = *it;
        dcs_ledger.erase(it);
        live = static_cast<unsigned>(dcs_ledger.size());
        return true;
    }
    live = static_cast<unsigned>(dcs_ledger.size());
    return false;
}

// One line per live game feature, so a log shows which handles survived a
// recreation (never evaluated since creation and never released = leaked).
void dcs_ledger_trace(const char* reason) noexcept {
    std::lock_guard lock(dcs_ledger_mutex);
    const auto now = GetTickCount64();
    trace_event("FEATURE_LEDGER %s live=%u", reason, static_cast<unsigned>(dcs_ledger.size()));
    for (const auto& r : dcs_ledger)
        trace_event("FEATURE_LEDGER   #%llu handle=%p route=%s feature=%u input=%ux%u output=%ux%u age_s=%.1f evaluations=%llu deferred=%u",
            static_cast<unsigned long long>(r.sequence), r.handle, dcs_route_names[r.route & 3U], r.feature,
            r.width, r.height, r.out_width, r.out_height, static_cast<double>(now - r.created_ms) / 1000.0,
            static_cast<unsigned long long>(r.evaluations), r.deferred ? 1U : 0U);
}

void dcs_release_idle_views(const void* created) noexcept;

void dcs_trace_feature_create(unsigned route, std::uint32_t feature, const NgxParameters* parameters,
    NgxResult result, const NgxHandle* handle, const DcsVram& before, const DcsVram& after,
    std::uint64_t elapsed_ms, bool record) noexcept {
    unsigned live{};
    if (record && ngx_succeeded(result) && handle) live = dcs_ledger_created(handle, route, feature, parameters, false);
    trace_event("FEATURE_VRAM create route=%s feature=%u handle=%p result=0x%08X input=%ux%u output=%ux%u elapsed_ms=%llu "
        "vram_before_mb=%.1f vram_after_mb=%.1f delta_mb=%+.1f budget_mb=%.1f live_game_features=%u",
        dcs_route_names[route & 3U], feature, handle, unsigned(result), get_ui(parameters, "Width"),
        get_ui(parameters, "Height"), get_ui(parameters, "OutWidth"), get_ui(parameters, "OutHeight"),
        static_cast<unsigned long long>(elapsed_ms), dcs_mb(before.usage), dcs_mb(after.usage),
        dcs_delta_mb(before, after), dcs_mb(after.budget), live);
    if (record && ngx_succeeded(result) && handle && live > 1U) dcs_ledger_trace("after-create");
    if (record && ngx_succeeded(result) && handle) dcs_release_idle_views(handle);
}

// DCS never releases its DLSS features: every mission load creates a new set
// and the previous mission's stay alive, never evaluated again, until DCS
// exits (flight 2026-10-06: 0 releases, 16 live features at the second load).
// The private resources this layer keeps per view - the D3D11->D3D12
// transport textures, the private D3D12 SR feature and the DLSS-NR feature,
// about half a GiB per mission for the two focus views - would stay with
// them. When the game creates a feature, every other view idle for at least
// dcs_idle_view_ms gives them back. Nothing else changes: the game's handle
// stays valid, and should it ever be evaluated again its view is rebuilt as
// on its first evaluation (the bilinear hold releases a view the same way).
constexpr std::uint64_t dcs_idle_view_ms = 5000;

void dcs_release_idle_views(const void* created) noexcept {
    // The composite's and the depth conversion's cached views may hold the
    // previous mission's targets and depth buffers (each keeps its resource alive).
    dcs_flush_composite_views();
    dcs_flush_transport_depth_views();
    struct Idle { const void* handle; double idle_s; std::uint64_t evaluations; };
    std::vector<Idle> idle;
    {
        std::lock_guard lock(dcs_ledger_mutex);
        const auto now = GetTickCount64();
        for (auto& record : dcs_ledger) {
            if (record.handle == created || record.idle_released || record.evaluations == 0U) continue;
            const auto since = now - std::max(record.last_evaluated_ms, record.created_ms);
            if (since < dcs_idle_view_ms) continue;
            record.idle_released = true;
            try { idle.push_back({record.handle, static_cast<double>(since) / 1000.0, record.evaluations}); }
            catch (...) { record.idle_released = false; }
        }
    }
    for (const auto& view : idle) {
        const auto before = dcs_vram_sample();
        auto* const handle = static_cast<const NgxHandle*>(view.handle);
        // Bounded GPU wait on DCS's render thread: a view whose private work has not
        // completed is kept and retried at the next create.
        if (!release_d3d11_transport_view(handle, dcs_transport_release_wait_ms)) {
            std::lock_guard lock(dcs_ledger_mutex);
            for (auto& record : dcs_ledger) if (record.handle == view.handle) record.idle_released = false;
            trace_event("FEATURE_IDLE kept handle=%p: its private DX12 work is still running; retried at the next create",
                view.handle);
            continue;
        }
        release_d3d11_peripheral_dlaa_view(handle);
        const auto after = dcs_vram_sample();
        trace_event("FEATURE_IDLE released private resources handle=%p idle_s=%.1f evaluations=%llu "
            "vram_before_mb=%.1f vram_after_mb=%.1f delta_mb=%+.1f (the game's handle stays valid)",
            view.handle, view.idle_s, static_cast<unsigned long long>(view.evaluations), dcs_mb(before.usage),
            dcs_mb(after.usage), dcs_delta_mb(before, after));
    }
}

// Release logging around the real release call; logs only handles the game owns.
// The outermost hook takes the record before releasing, so a nested route
// (core -> snippet) never logs the same handle twice.
template<class Release>
NgxResult dcs_traced_feature_release(unsigned route, NgxHandle* handle, Release release) {
    DcsFeatureRecord record{};
    unsigned live{};
    const bool recorded = dcs_ledger_released(handle, record, live);
    const auto before = recorded ? dcs_vram_sample() : DcsVram{};
    const auto release_started = GetTickCount64();
    const auto result = release();
    if (recorded) {
        const auto after = dcs_vram_sample();
        trace_event("FEATURE_RELEASE route=%s handle=%p feature=%u input=%ux%u output=%ux%u result=0x%08X age_s=%.1f "
            "evaluations=%llu deferred=%u elapsed_ms=%llu vram_before_mb=%.1f vram_after_mb=%.1f delta_mb=%+.1f live_game_features=%u",
            dcs_route_names[route & 3U], handle, record.feature, record.width, record.height, record.out_width,
            record.out_height, unsigned(result), static_cast<double>(release_started - record.created_ms) / 1000.0,
            static_cast<unsigned long long>(record.evaluations), record.deferred ? 1U : 0U,
            static_cast<unsigned long long>(GetTickCount64() - release_started), dcs_mb(before.usage), dcs_mb(after.usage),
            dcs_delta_mb(before, after), live);
    }
    return result;
}

// Deferred DX11 game feature (defined in dcs_deferred_feature.inc).
thread_local bool dcs_materializing_feature{};
[[nodiscard]] bool dcs_try_defer_d3d11_create(ID3D11DeviceContext* context, std::uint32_t feature,
    NgxParameters* parameters, NgxHandle** handle, NgxResult& result) noexcept;
[[nodiscard]] bool dcs_release_deferred_d3d11(NgxHandle* handle, NgxResult& result) noexcept;
[[nodiscard]] bool dcs_is_deferred_d3d11(const NgxHandle* handle) noexcept;
[[nodiscard]] bool dcs_deferred_unmaterialized(const NgxHandle* handle) noexcept;
// DCS's creation-time DLSS parameters on its shared bag while a deferred view's
// private SR evaluates (restored afterwards); no-op for every other handle.
[[nodiscard]] bool dcs_deferred_push_create_values(const NgxHandle* handle, const NgxParameters* parameters) noexcept;
void dcs_deferred_pop_create_values() noexcept;
struct DcsDeferredCreateValuesScope {
    bool active{};
    DcsDeferredCreateValuesScope(const NgxHandle* handle, const NgxParameters* parameters) noexcept
        : active(dcs_deferred_push_create_values(handle, parameters)) {}
    ~DcsDeferredCreateValuesScope() { if (active) dcs_deferred_pop_create_values(); }
    DcsDeferredCreateValuesScope(const DcsDeferredCreateValuesScope&) = delete;
    DcsDeferredCreateValuesScope& operator=(const DcsDeferredCreateValuesScope&) = delete;
};
// Frames of a deferred view produced without DCS's own feature (see dcs_deferred_feature.inc).
[[nodiscard]] bool dcs_deferred_hold_unproven(const D3D11RuntimeCallbacks& runtime, ID3D11DeviceContext* context,
    const NgxHandle* handle, const NgxParameters* parameters, NgxResult& result) noexcept;
[[nodiscard]] bool dcs_deferred_keep_transport(const D3D11RuntimeCallbacks& runtime, ID3D11DeviceContext* context,
    const NgxHandle* handle, const NgxParameters* parameters, const Settings& settings, NgxResult& result) noexcept;
// A brief run of transport failures on an accepted deferred view is held (bilinear) instead of creating DCS's feature.
[[nodiscard]] bool dcs_deferred_hold_transport_failure(ID3D11DeviceContext* context, const NgxHandle* handle,
    const NgxParameters* parameters, NgxResult& held) noexcept;
void dcs_deferred_transport_ok(const NgxHandle* handle) noexcept;
