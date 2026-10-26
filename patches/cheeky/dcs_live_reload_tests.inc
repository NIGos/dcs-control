// DCS Control: live INI reload logic (dcs_live_reload.hpp): parse, diff, live/start-only split, staging, self-writes.
// Included by tests/uevr_tests.cpp inside its anonymous namespace (it includes dcs_live_reload.hpp and links
// settings_io.cpp); uses its require().

void dcs_live_reload_tests() {
    using namespace cheeky::dcsvr;
    const auto all_keys = dcs_ini_keys(serialize_settings(Settings{}));
    for (const auto key : dcs_live_keys) {
        bool found = false;
        for (const auto& name : all_keys) found |= name == key;
        require(found, "Every live key is a persisted Cheeky setting");
    }
    for (const auto key : {"NrIntensity", "NrLocalToneStrength", "NrLocalStructureStrength", "NrColorStrength",
            "NrHdrTransferStrength", "NrPaperWhiteScale", "NrMotionScaleXMultiplier", "NrMotionScaleYMultiplier",
            "NrTransitionWidth", "NrRoundness", "NrStyle"})
        require(dcs_live_key(key), "DLSS 5 tuning keys are live");
    for (const auto key : {"Enabled", "D3D11D3D12Transport", "D3D12LowerHook", "NrEnabled", "NrWidth", "NrHeight",
            "NrFoveated", "NrUseSrFoveation", "NrWorkingScale", "NrProcessingOrder", "CenterPreset", "PeripheralDlaa",
            "PeripheralDlaaScale", "Width", "Height", "CenterMode", "AutoStereoAlignment", "EyeCalibrationLearnedMethod"})
        require(!dcs_live_key(key), "Keys that re-initialise stay start-only");
    require(all_keys.front() == "Enabled" && all_keys.size() > 40, "INI key listing skips the section and the schema");

    // Parse: DCS Control's writer format (integers for booleans, G9 numbers), comments, other sections, missing keys.
    Settings base; base.nr_width = 0.6F; base.nr_intensity = 1.0F; base.transition_width = 0.04F;
    std::string error;
    auto parsed = base;
    require(dcs_parse_settings_text("; DCS Control\r\n[Other]\r\nNrIntensity=0.1\r\n[CheekyFoveatedDLSS]\r\nSchemaVersion=1\r\n"
        "NrIntensity = 0.5\r\nTransitionWidth=0.04\r\nNrAutomaticMask=1\r\nFutureKey=7\r\n", parsed, error),
        "Parse a DCS Control INI");
    require(parsed.nr_intensity == 0.5F && parsed.nr_automatic_mask && parsed.nr_width == 0.6F,
        "Parsed values applied; a missing key keeps its value");
    const auto before = serialize_settings(parsed);
    require(!dcs_parse_settings_text("[CheekyFoveatedDLSS]\nNrIntensity=0.25\nNrColorStrength=bad\n", parsed, error) &&
        error == "Invalid setting: NrColorStrength" && serialize_settings(parsed) == before, "A malformed INI is rejected whole");
    require(!dcs_parse_settings_text("[CheekyFoveatedDLSS]\nSchemaVersion=2\nNrIntensity=0.25\n", parsed, error) &&
        serialize_settings(parsed) == before, "Another schema is rejected");

    // Diff against the previous file content: float spelling is no change; learned calibration is Cheeky's own.
    const auto previous = serialize_settings(base);
    auto changed = base;
    require(dcs_parse_settings_text("[CheekyFoveatedDLSS]\nTransitionWidth=0.0399999991\nNrIntensity=0.5\nNrWidth=0.3\n"
        "NrColorStrength=0.75\nEnabled=0\nEyeCalibrationLearnedMethod=2\n", changed, error), "Parse a changed INI");
    auto plan = dcs_reload_plan(previous, serialize_settings(changed));
    require(plan.live == std::vector<std::string>{"NrIntensity", "NrColorStrength"}, "Live keys found in INI order");
    require(plan.start_only == std::vector<std::string>{"Enabled", "NrWidth"}, "Start-only keys found, learned calibration skipped");
    require(dcs_reload_list(plan.live, serialize_settings(changed)) == "NrIntensity=0.5, NrColorStrength=0.75" &&
        dcs_reload_list({}, previous) == "none", "Log line lists key=value");
    require(dcs_reload_plan(previous, previous).empty(), "Same content: nothing to reload");

    // Self-writes: the content Cheeky saved is known, so its own write never reloads; a stamp change alone re-reads.
    DcsLiveReload reload;
    const DcsIniStamp first{true, 100, 50}, second{true, 200, 50};
    require(reload.stamp_changed(first), "Unseen file");
    reload.seen(first);
    require(!reload.stamp_changed(first) && reload.stamp_changed(second) && reload.stamp_changed(DcsIniStamp{}),
        "Only a different write time, size or existence re-reads the file");
    const auto own = serialize_settings(base);
    reload.note_content(own, own);
    require(reload.known_content(own) && !reload.known_content(own + "NrIntensity=0.5\n") && reload.last() == own,
        "Cheeky's own save is recognised by its content");

    // Staging: a start-only value from DCS Control is written by every later save until it is back at the running
    // value, or the overlay sets that key itself.
    reload.stage("NrWidth", "0.300000012", "0.600000024");
    reload.stage("Enabled", "0", "1");
    require(reload.staged() == 2, "Start-only keys staged");
    auto save = base;
    reload.apply_staged([&](const std::string& key, const std::string& value) {
        require(set_named_setting(save, key, value), "Staged value is a valid setting"); });
    require(save.nr_width == 0.3F && !save.enabled && save.nr_intensity == base.nr_intensity, "A save writes the staged values");
    // The saved file is the next baseline: re-reading it finds nothing (no reload loop).
    reload.note_content(serialize_settings(save), serialize_settings(save));
    require(dcs_reload_plan(reload.last(), serialize_settings(save)).empty() && reload.known_content(serialize_settings(save)),
        "Cheeky's save of staged values does not trigger a reload");
    reload.stage("NrWidth", "0.600000024", "0.600000024");
    require(reload.staged() == 1, "Back at the running value: no longer staged");
    reload.forget({"Enabled"});
    require(reload.staged() == 0, "An explicit change in Cheeky replaces the staged value");
    reload.stage("NrWidth", "0.3", "0.6");
    reload.clear();
    require(reload.staged() == 0, "Defaults clear staged values");

    // A session-only change (the DLSS 5 key) is not a file change: the INI keeps NrEnabled=1, nothing to reload.
    DcsSessionOverrides overrides;
    auto session = base; session.nr_enabled = true;
    const auto on = serialize_settings(session);
    session.nr_enabled = false;
    overrides.session(on, serialize_settings(session), {"NrEnabled"});
    auto persisted = session;
    overrides.apply([&](const std::string& key, const std::string& value) { set_named_setting(persisted, key, value); });
    require(persisted.nr_enabled && dcs_reload_plan(on, serialize_settings(persisted)).empty(),
        "The DLSS 5 key's session toggle saves the persisted value and is no reload");
}
