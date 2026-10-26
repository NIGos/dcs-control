#pragma once
// DCS Control: live INI reload. Cheeky reads CheekyFoveatedDLSS.ini once at start and writes it back itself (the
// persistence worker). While DCS runs, DCS Control may rewrite the file for a DLSS 5 tuning change; the runtime then
// re-reads it (by its stamp, at most once a second, on the persistence worker, never on the render thread) and applies
// the changed keys through the same update_settings() path the F8 overlay's "set" command uses, but only the keys in
// dcs_live_keys. Every other changed key is staged: it takes effect at the next start, and every later save by Cheeky
// writes the staged value, so Cheeky's own save never undoes a change DCS Control made for the next launch.
// Cheeky's own writes never trigger a reload: their content hash is remembered and skipped.
#include "settings_io.hpp"
#include "dcs_session_settings.hpp"
#include <cstdint>
#include <iterator>
#include <map>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace cheeky::dcsvr {

// Keys a running Cheeky applies without re-initialisation. Evidence (src/dlss_nr.cpp):
// - Read per evaluation, no allocation: NrPaperWhiteScale, NrHdrTransferStrength, NrColorStrength (codec constants),
//   NrRoundness, NrTransitionWidth (NR region shape constants; the region size comes from NrWidth/NrHeight only),
//   NrMotionScaleX/YMultiplier and NrDepthConvention (evaluation parameters).
// - Part of the NR FeatureKey at the same working size: NrIntensity, NrLocalToneStrength, NrLocalStructureStrength,
//   NrSkinStructureStrength, NrStyle, NrAutomaticMask, NrUiCorrection. A change creates one new NGX DLSS-NR feature
//   of the same size; the replaced one stays as the single cached alternate (retired_feature_capacity) and is released
//   when its GPU work is done. The F8 overlay applies these the same way.
inline constexpr std::string_view dcs_live_keys[]{
    "NrIntensity", "NrLocalToneStrength", "NrLocalStructureStrength", "NrSkinStructureStrength", "NrStyle",
    "NrAutomaticMask", "NrUiCorrection", "NrColorStrength", "NrHdrTransferStrength", "NrPaperWhiteScale",
    "NrDepthConvention", "NrMotionScaleXMultiplier", "NrMotionScaleYMultiplier", "NrTransitionWidth", "NrRoundness",
};

inline bool dcs_live_key(std::string_view key) noexcept {
    for (const auto live : dcs_live_keys) if (live == key) return true;
    return false;
}

// Cheeky's own learned calibration: never taken from the file while running.
inline bool dcs_runtime_owned_key(std::string_view key) noexcept { return key.starts_with("EyeCalibrationLearned"); }

inline std::string_view dcs_trim(std::string_view s) noexcept {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == s.npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// The keys of serialize_settings text ("Key=value" lines), in order, without SchemaVersion.
inline std::vector<std::string> dcs_ini_keys(std::string_view ini) {
    std::vector<std::string> keys;
    std::size_t start{};
    while (start < ini.size()) {
        auto end = ini.find('\n', start);
        if (end == std::string_view::npos) end = ini.size();
        const auto line = dcs_trim(ini.substr(start, end - start));
        const auto eq = line.find('=');
        if (eq != std::string_view::npos && !line.starts_with('[') && line.substr(0, eq) != "SchemaVersion")
            keys.emplace_back(line.substr(0, eq));
        start = end + 1;
    }
    return keys;
}

// INI text parsed like read_settings_file (same section, comments, SchemaVersion check, unknown keys ignored, one
// malformed known value rejects the whole file), but from the bytes already read and with a key the file lacks
// keeping its value in `s`: a live reload never resets a field the writer left out.
inline bool dcs_parse_settings_text(std::string_view text, foveated_dlss::Settings& s, std::string& error) {
    static const auto known = dcs_ini_keys(foveated_dlss::serialize_settings(foveated_dlss::Settings{}));
    auto candidate = s;
    bool section = false;
    std::size_t start{};
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const auto line = dcs_trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') { section = line == "[CheekyFoveatedDLSS]"; continue; }
        if (!section) continue;
        const auto split = line.find('=');
        if (split == std::string_view::npos) continue;
        const auto key = dcs_trim(line.substr(0, split)), value = dcs_trim(line.substr(split + 1));
        if (key == "SchemaVersion") {
            if (value != "1") { error = "Unsupported settings schema"; return false; }
            continue;
        }
        bool is_known = false;
        for (const auto& name : known) is_known |= name == key;
        if (is_known && !foveated_dlss::set_named_setting(candidate, key, value)) {
            error = "Invalid setting: " + std::string(key);
            return false;
        }
    }
    s = candidate;
    error.clear();
    return true;
}

struct DcsReloadPlan {
    std::vector<std::string> live, start_only;
    [[nodiscard]] bool empty() const noexcept { return live.empty() && start_only.empty(); }
};

// Keys whose value differs between `baseline` (the previous file content) and `file` (the new one), both parsed and
// serialized again, so "0.04" and "0.0399999991" are the same float. Learned calibration is skipped.
inline DcsReloadPlan dcs_reload_plan(std::string_view baseline, std::string_view file) {
    DcsReloadPlan plan;
    for (const auto& key : dcs_ini_keys(baseline)) {
        if (dcs_runtime_owned_key(key)) continue;
        std::string before, after;
        if (!dcs_ini_value(baseline, key, before) || !dcs_ini_value(file, key, after) || before == after) continue;
        (dcs_live_key(key) ? plan.live : plan.start_only).push_back(key);
    }
    return plan;
}

inline std::uint64_t dcs_text_hash(std::string_view text) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char c : text) { hash ^= c; hash *= 1099511628211ULL; }
    return hash ^ text.size();
}

// The file's last-write time and size: a reload re-reads the file only when they change.
struct DcsIniStamp {
    bool exists{};
    std::uint64_t write_time{}, size{};
    bool operator==(const DcsIniStamp&) const = default;
};

class DcsLiveReload {
public:
    // Stamp bookkeeping (persistence worker only).
    [[nodiscard]] bool stamp_changed(const DcsIniStamp& now) const noexcept { return !(now == stamp_); }
    void seen(const DcsIniStamp& now) noexcept { stamp_ = now; }
    // The file content last read or written by Cheeky: `bytes` as on disk, `settings` the same parsed and serialized
    // again. A reload applies only what changed against it, so a change made in Cheeky since (overlay, not yet saved)
    // is never reverted by a write that left that key alone. Re-reading the same bytes is not a reload.
    void note_content(std::string_view bytes, std::string settings) {
        known_ = dcs_text_hash(bytes); has_known_ = true; last_ = std::move(settings);
    }
    [[nodiscard]] bool known_content(std::string_view bytes) const noexcept {
        return has_known_ && known_ == dcs_text_hash(bytes);
    }
    [[nodiscard]] const std::string& last() const noexcept { return last_; }

    // Staged start-only values (under the runtime state mutex). `file` is the value on disk, `running` the value a save
    // would write without staging; back at the running value nothing is staged any more.
    void stage(const std::string& key, const std::string& file, const std::string& running) {
        if (file == running) staged_.erase(key);
        else staged_[key] = file;
    }
    // An explicit change in Cheeky (overlay "set") of these keys replaces what was staged for them.
    void forget(const std::vector<std::string>& keys) { for (const auto& key : keys) staged_.erase(key); }
    void clear() noexcept { staged_.clear(); }
    // set(key, value) for each staged key: a save writes these instead of the running values.
    template<class Set> void apply_staged(Set set) const {
        for (const auto& [key, value] : staged_) set(key, value);
    }
    [[nodiscard]] std::size_t staged() const noexcept { return staged_.size(); }

private:
    DcsIniStamp stamp_{};
    std::uint64_t known_{};
    bool has_known_{};
    std::string last_;
    std::map<std::string, std::string, std::less<>> staged_;
};

// One per process.
inline DcsLiveReload& dcs_live_reload() {
    static DcsLiveReload reload;
    return reload;
}

// "Key=value, Key=value" for the log line.
inline std::string dcs_reload_list(const std::vector<std::string>& keys, std::string_view ini) {
    std::string text;
    for (const auto& key : keys) {
        std::string value;
        static_cast<void>(dcs_ini_value(ini, key, value));
        if (!text.empty()) text += ", ";
        text += key + '=' + value;
    }
    return text.empty() ? "none" : text;
}
} // namespace cheeky::dcsvr
