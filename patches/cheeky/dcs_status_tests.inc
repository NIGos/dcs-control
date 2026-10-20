// DCS VR Control: status block for the in-headset diagnostic overlay (dcs_status_block.hpp).
// Included by tests/openxr_calibration_tests.cpp; uses its require().
// The DLSS 5 key's session-only change (dcs_session_settings.hpp): saves keep the persisted value.
void dcs_session_settings_test() {
    using namespace cheeky::dcsvr;
    const std::string on = "[CheekyFoveatedDLSS]\nSchemaVersion=1\nEnabled=0\nNrEnabled=1\nNrStyle=0\n";
    const std::string off = "[CheekyFoveatedDLSS]\nSchemaVersion=1\nEnabled=0\nNrEnabled=0\nNrStyle=0\n";
    const std::string off_styled = "[CheekyFoveatedDLSS]\nSchemaVersion=1\nEnabled=0\nNrEnabled=0\nNrStyle=2\n";
    std::string value;
    require(dcs_ini_value(on, "NrEnabled", value) && value == "1" && !dcs_ini_value(on, "Nr", value) &&
        !dcs_ini_value(on, "Missing", value), "INI value lookup");
    const auto saved = [](const DcsSessionOverrides& overrides) {
        std::string text;
        overrides.apply([&](const std::string& key, const std::string& v) { text += key + '=' + v + ';'; });
        return text;
    };
    DcsSessionOverrides overrides;
    overrides.session(on, off, {"NrEnabled"});
    require(saved(overrides) == "NrEnabled=1;", "A session-only toggle saves the persisted value");
    overrides.session(off, on, {"NrEnabled"});
    require(overrides.empty(), "Toggled back: nothing to override");
    overrides.session(on, off, {"NrEnabled"});
    overrides.session(off, on, {"NrEnabled"});
    overrides.session(on, off, {"NrEnabled"});
    require(saved(overrides) == "NrEnabled=1;", "Repeated toggles keep the first persisted value");
    overrides.persistent(off, off_styled, {"NrStyle"});
    require(saved(overrides) == "NrEnabled=1;", "An unrelated saved change keeps the session override");
    overrides.persistent(off_styled, off_styled, {"NrEnabled"});
    require(overrides.empty(), "A saved change of the same key makes it persistent");
    overrides.session(on, off, {"NrEnabled"});
    overrides.persistent(off, on, {});
    require(overrides.empty(), "Defaults that change the key make it persistent");
    overrides.session(on, off, {"NrEnabled"});
    overrides.clear();
    require(overrides.empty(), "Defaults that leave the key unchanged still end the override");
}

void dcs_status_block_test() {
    using namespace cheeky::dcsvr;
    dcs_session_settings_test();
    // ABI shared with the OFXR fork patch (include/xrfg/dcsvr_status.hpp pins the same values).
    require(sizeof(DcsVrStatusV1) == 128U && offsetof(DcsVrStatusV1, reason) == 72U && status_magic == 0x53525644U,
        "Status block layout changed");
    require(dcs_status_mapping_name(1234U) == L"Local\\DcsVrControlStatus-1234", "Status mapping name");

    // Seqlock: a settled block round-trips, a block caught mid-write is refused.
    DcsVrStatusV1 shared{}, value{}, read{};
    value.magic = status_magic; value.version = status_version; value.structure_size = sizeof(value);
    value.flags = status_nr_enabled | status_quad_focus; value.nr_evaluations_per_second = 90U;
    value.focus_views_mapped = 2U; value.nr_gpu_ms = 1.75F; value.sequence = 999U;
    dcs_status_set_reason(value, "abc");
    require(!dcs_status_load(shared, read), "Empty block accepted");
    dcs_status_store(shared, value);
    require(shared.sequence == 2U, "Writer sequence not even after a publication");
    require(dcs_status_load(shared, read) && read.focus_views_mapped == 2U && read.nr_gpu_ms == 1.75F &&
        std::string(read.reason) == "abc", "Published status not read back");
    dcs_status_store(shared, value);
    require(shared.sequence == 4U, "Writer sequence not advanced");
    shared.sequence = 5U;
    require(!dcs_status_load(shared, read), "Torn (odd sequence) block accepted");
    shared.sequence = 6U; shared.version = 2U;
    require(!dcs_status_load(shared, read), "Unknown status version accepted");

    // Rates over about one second of 100 ms publications; counter resets restart the window.
    DcsRateWindow rate;
    require(rate.push(1000U, 0U) == 0.0F, "Rate without history");
    float last{};
    for (std::uint64_t t = 1100U; t <= 3000U; t += 100U) last = rate.push(t, (t - 1000U) * 90U / 1000U);
    require(last > 89.0F && last < 91.0F, "90 per second not measured");
    require(rate.push(3100U, 5U) == 0.0F, "Counter reset produced a rate");
    for (std::uint64_t t = 3200U; t <= 4200U; t += 100U) last = rate.push(t, 5U);
    require(last == 0.0F, "Idle counter produced a rate");

    // Focus activity: distinct views in the last second, expiry, last rejection.
    DcsFocusActivity activity;
    for (int frame = 0; frame < 45; ++frame) {
        activity.accepted(0x10U, 1000U + frame * 22U);
        activity.accepted(0x20U, 1000U + frame * 22U);
        activity.rejected(4U);
    }
    auto sample = activity.sample(2000U);
    require(sample.views == 2U && sample.accepted == 90U && sample.rejected == 45U && sample.last_reject == 4U,
        "Focus views not counted");
    require(activity.sample(3100U).views == 0U, "Stale focus views still counted");
    for (std::uint64_t view = 1; view <= 12; ++view) activity.accepted(view, 5000U + view);
    require(activity.sample(5100U).views == 8U, "Focus slots overflowed");

    // Reasons when NR is on and nothing was evaluated.
    DcsVrStatusV1 s{};
    s.flags = status_nr_enabled | status_graphics_ready;
    s.nr_evaluations_per_second = 10U;
    require(dcs_status_reason_text(s) == nullptr, "Reason while NR evaluates");
    s.nr_evaluations_per_second = 0U; s.nr_state = 2U;
    require(std::string(dcs_status_reason_text(s)) == "nvngx_dlssnr.dll not loaded", "Runtime-missing reason");
    s.flags |= status_quad_focus; s.focus_rejects_per_second = 90U; s.focus_last_reject = 4U;
    require(std::string(dcs_status_reason_text(s)) == "focus views not identified", "Focus reason");
    s.focus_rejects_per_second = 0U;
    require(std::string(dcs_status_reason_text(s)) == "no DLSS frames from DCS", "No-frames reason");
    s.flags &= ~status_graphics_ready;
    require(std::string(dcs_status_reason_text(s)) == "Cheeky not attached to the game", "Attach reason");
    s.flags &= ~status_nr_enabled;
    require(dcs_status_reason_text(s) == nullptr, "Reason while NR is off");

    // The publisher's named mapping is visible to an independent reader of the same process.
    const auto pid = GetCurrentProcessId();
    {
        DcsStatusPublisher publisher;
        require(publisher.open(pid) && publisher.due(100U) && !publisher.due(150U) && publisher.due(200U),
            "Publisher cadence");
        value.publish_tick_ms = 77U;
        publisher.publish(value);
        const auto name = dcs_status_mapping_name(pid);
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        require(mapping != nullptr, "Status mapping not found by name");
        const auto* view = static_cast<const DcsVrStatusV1*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(DcsVrStatusV1)));
        require(view != nullptr && dcs_status_load(*view, read) && read.publish_tick_ms == 77U &&
            read.nr_evaluations_per_second == 90U, "Status not readable through the mapping");
        UnmapViewOfFile(view);
        CloseHandle(mapping);
    }
}
