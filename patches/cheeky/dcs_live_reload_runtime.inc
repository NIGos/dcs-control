// DCS Control: live INI reload (dcs_live_reload.hpp), included into uevr/runtime.cpp's anonymous namespace before the
// persistence worker. Everything here runs on the persistence worker, except dcs_live_reload_start (Start, before the
// worker exists); the staged values are read and written under s.mutex.
cheeky::dcsvr::DcsIniStamp dcs_ini_stamp(const std::filesystem::path& path) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return {};
    return {true, (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime,
        (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow};
}
bool dcs_read_ini(const std::filesystem::path& path, std::string& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}
// What a save writes now: the running settings, with each session-only field at its persisted value and, with
// `staged`, each start-only key DCS Control changed on disk at its value there. Under s.mutex.
Settings dcs_persisted_settings_locked(const bool staged = true) {
    auto settings = configured_settings();
    const auto set = [&](const std::string& key, const std::string& value) {
        static_cast<void>(set_named_setting(settings, key, value)); };
    cheeky::dcsvr::dcs_session_overrides().apply(set);
    if (staged) cheeky::dcsvr::dcs_live_reload().apply_staged(set);
    return settings;
}
// The file read at start (`read`, before any clamping) is the baseline: only a later change is a reload.
void dcs_live_reload_start(const std::filesystem::path& config, const Settings& read) {
    auto& reload = cheeky::dcsvr::dcs_live_reload();
    const auto stamp = dcs_ini_stamp(config);
    std::string bytes;
    reload.seen(stamp);
    if (stamp.exists) static_cast<void>(dcs_read_ini(config, bytes));
    reload.note_content(bytes, serialize_settings(read));
}
// Polled at most once a second and before every save. A changed stamp re-reads the file; content Cheeky wrote itself
// (or already handled) is skipped. Only the keys whose value changed against the previous file content count: live
// keys are applied like the overlay's "set" (update_settings, revision), start-only keys are staged for the next start
// and kept by every later save. One log line per reload.
void dcs_live_reload_check(State& s) {
    using namespace cheeky::dcsvr;
    auto& reload = dcs_live_reload();
    const auto stamp = dcs_ini_stamp(s.config);
    if (!reload.stamp_changed(stamp)) return;
    std::string bytes;
    if (!stamp.exists) { reload.seen(stamp); return; } // Removed: the next save writes it again.
    if (!dcs_read_ini(s.config, bytes)) return;        // Retried on the next poll.
    if (!(dcs_ini_stamp(s.config) == stamp)) return;   // Replaced while reading: the next poll reads the new file.
    reload.seen(stamp);
    if (reload.known_content(bytes)) return;           // Cheeky's own save, or a content already handled.
    std::string error;
    Settings previous;
    static_cast<void>(dcs_parse_settings_text(reload.last(), previous, error)); // Cheeky's own serialization.
    auto parsed = previous;
    if (!dcs_parse_settings_text(bytes, parsed, error)) {
        reload.note_content(bytes, reload.last()); // Reported once; the previous content stays the baseline.
        trace_event("DCS Control live reload: %s; the INI is ignored until it changes", error.c_str());
        return;
    }
    const auto file = serialize_settings(parsed);
    const auto plan = dcs_reload_plan(reload.last(), file);
    reload.note_content(bytes, file);
    if (plan.empty()) return;
    std::lock_guard lock(s.mutex);
    const auto running = serialize_settings(dcs_persisted_settings_locked(false));
    for (const auto& key : plan.start_only) {
        std::string on_disk, current;
        static_cast<void>(dcs_ini_value(file, key, on_disk));
        static_cast<void>(dcs_ini_value(running, key, current));
        reload.stage(key, on_disk, current);
    }
    if (!plan.live.empty()) {
        auto live = configured_settings();
        const auto before = serialize_settings(live);
        for (const auto& key : plan.live) {
            std::string value;
            if (dcs_ini_value(file, key, value)) static_cast<void>(set_named_setting(live, key, value));
        }
        // As a persistent "set": a session-only override of these keys ends.
        dcs_session_overrides().persistent(before, serialize_settings(live), plan.live);
        update_settings(live);
        const bool saved = s.saved_revision == s.revision; // The file already holds these values.
        ++s.revision;
        if (saved) s.saved_revision = s.revision;
    }
    trace_event("DCS Control live reload: applied %s; next start: %s", dcs_reload_list(plan.live, file).c_str(),
        dcs_reload_list(plan.start_only, file).c_str());
}
