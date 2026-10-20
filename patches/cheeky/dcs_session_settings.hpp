#pragma once
// DCS VR Control: session-only settings ("set_session", the in-game DLSS 5 key).
// Such a change applies at once but must never reach the INI, yet every later save
// (another setting, learned calibration, the next start) writes the whole live
// settings. The runtime keeps here the persisted value of each field a session
// change overrode, and the save worker writes those values instead of the live
// ones. Values are the INI text of serialize_settings ("Key=value" lines), so this
// header needs nothing from the settings model and is tested on its own.
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace cheeky::dcsvr {
// The value of "key=" in INI text; false when the key is absent.
inline bool dcs_ini_value(std::string_view ini, std::string_view key, std::string& value) {
    std::size_t start{};
    while (start < ini.size()) {
        auto end = ini.find('\n', start);
        if (end == std::string_view::npos) end = ini.size();
        auto line = ini.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() > key.size() && line.starts_with(key) && line[key.size()] == '=') {
            value.assign(line.substr(key.size() + 1));
            return true;
        }
        start = end + 1;
    }
    return false;
}

class DcsSessionOverrides {
public:
    // A session-only transaction moved the live settings from `before` to `after`
    // for `keys`. The first override of a key keeps its persisted value; a key
    // toggled back to that value needs no override any more.
    void session(std::string_view before, std::string_view after, const std::vector<std::string>& keys) {
        for (const auto& key : keys) {
            std::string persisted, live;
            if (!persisted_.contains(key) && dcs_ini_value(before, key, persisted)) persisted_[key] = persisted;
            const auto found = persisted_.find(key);
            if (found != persisted_.end() && dcs_ini_value(after, key, live) && live == found->second)
                persisted_.erase(found);
        }
    }
    // A persistent change (set, defaults): the keys it names, and every overridden
    // key whose value it changed, belong to the saved settings again.
    void persistent(std::string_view before, std::string_view after, const std::vector<std::string>& keys) {
        for (const auto& key : keys) persisted_.erase(key);
        for (auto it = persisted_.begin(); it != persisted_.end();) {
            std::string old_value, new_value;
            const bool had = dcs_ini_value(before, it->first, old_value);
            const bool has = dcs_ini_value(after, it->first, new_value);
            if (had != has || old_value != new_value) it = persisted_.erase(it);
            else ++it;
        }
    }
    // Defaults reset every field, including one a session change left at its default
    // value: no override survives them.
    void clear() noexcept { persisted_.clear(); }
    // set(key, value) for each field the save must write with its persisted value.
    template<class Set> void apply(Set set) const {
        for (const auto& [key, value] : persisted_) set(key, value);
    }
    [[nodiscard]] bool empty() const noexcept { return persisted_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return persisted_.size(); }

private:
    std::map<std::string, std::string, std::less<>> persisted_;
};

// One per process; read and written under the runtime state mutex.
inline DcsSessionOverrides& dcs_session_overrides() {
    static DcsSessionOverrides overrides;
    return overrides;
}
} // namespace cheeky::dcsvr
