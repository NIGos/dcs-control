// DCS Control: live post-process settings for Quad-Views-Foveated (copied in by scripts/build-quadviews.ps1; tested by
// tests/quadviews-live).
//
// Upstream reads settings.cfg once, when the system is set up. DCS Control changes some settings while DCS runs, so the
// layer now re-reads its files when they change and applies the keys that only feed the per-frame post-process passes
// (sharpening, edge blend, periphery contrast, periphery stability). Every other key keeps its start value until the
// next start: sizes, multipliers, sections, shape, turbo and eye tracking decide swapchains, views and resources.
//
//  - Off the hot path: the render thread (xrEndFrame) only reads a clock and an atomic. At most once per second it
//    hands a poll to the Windows thread pool, which compares each file's write time and size and parses only when one
//    changed. The parse starts from Quad Views' own defaults and reads the same files in the same order with the same
//    section rules ([app:x], [exe:x], [runtime or system name]) as LoadConfiguration.
//  - Atomic: the parsed values are one struct behind a mutex; the render thread takes them with a try-lock (busy: next
//    frame) and applies all of them between two frames, on the thread that reads them.
//  - A file reload sets every live key to its file value, also one Quad Views' debug keys (Ctrl+J/K/L, Ctrl+U/I/O)
//    changed since: the layer and the engine module's mirror of the same file (header.qv) then agree again.
//  - A file is stamped before it is read, so a read that overlaps a write is repeated at the next poll; a file that
//    cannot be opened (a writer holds it) leaves the stamps as they were and is retried at the next poll.
#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dcsvr::live {

// The live keys and their meaning in the layer (member names in layer.cpp).
//   sharpen_focus_view         m_sharpenFocusView        CAS strength, 0: off (debug keys change it too)
//   smoothen_focus_view_edges  m_smoothenFocusViewEdges  composition fade band (debug keys change it too)
//   dcsvr_sharpen_taper        m_dcsvrSharpenTaper       sharpening fades out over that band
//   dcsvr_periphery_sharpen    m_dcsvrPeripherySharpen   CAS on the periphery, 0: off
//   dcsvr_temporal             m_dcsvrTemporal.enabled   periphery stability on/off
//   dcsvr_temporal_tau_ms      m_dcsvrTemporal.tauMs     its strength (DCS Control: 80 ms x strength)
//   dcsvr_temporal_ramp_deg    m_dcsvrTemporal.rampDeg   from the focus edge to full weight
//   dcsvr_temporal_clip        m_dcsvrTemporal.clip      neighbourhood clamp, standard deviations
inline constexpr const char* Keys[] = {"sharpen_focus_view",      "smoothen_focus_view_edges", "dcsvr_sharpen_taper",
                                       "dcsvr_periphery_sharpen", "dcsvr_temporal",            "dcsvr_temporal_tau_ms",
                                       "dcsvr_temporal_ramp_deg", "dcsvr_temporal_clip"};

inline bool IsLiveKey(const std::string& name) {
    for (const char* key : Keys) {
        if (name == key) return true;
    }
    return false;
}

struct Values {
    float sharpen{0.7f};
    float smoothing{0.2f};
    bool taper{false};
    float peripherySharpen{0.f};
    bool temporal{false};
    float tauMs{40.f};
    float rampDeg{8.f};
    float clip{1.f};
};

inline bool operator==(const Values& a, const Values& b) {
    return a.sharpen == b.sharpen && a.smoothing == b.smoothing && a.taper == b.taper &&
           a.peripherySharpen == b.peripherySharpen && a.temporal == b.temporal && a.tauMs == b.tauMs &&
           a.rampDeg == b.rampDeg && a.clip == b.clip;
}

// What the section rules compare against (layer.cpp ParseConfigurationStatement).
struct Context {
    std::string application;  // XrApplicationInfo's name ("DCS World")
    std::string executable;   // "DCS.exe"
    std::string runtime;      // the runtime's name
    std::string system;       // the system's name
};

// std::stof / std::stoi as Quad Views calls them (leading blanks, trailing text ignored; nothing parsed: the key is
// skipped). A value that is not finite is skipped too (Quad Views would keep it, the engine module's mirror skips it).
inline bool ToFloat(const std::string& value, float& out) {
    try {
        const float f = std::stof(value);
        if (!std::isfinite(f)) return false;
        out = f;
        return true;
    } catch (...) {
        return false;
    }
}
inline bool ToInt(const std::string& value, int& out) {
    try {
        out = std::stoi(value);
        return true;
    } catch (...) {
        return false;
    }
}

// One live key with Quad Views' own clamps. False: not a live key, or the value did not parse.
inline bool ApplyKey(const std::string& name, const std::string& value, Values& io) {
    float f = 0.f;
    int i = 0;
    if (name == "sharpen_focus_view") {
        if (!ToFloat(value, f)) return false;
        io.sharpen = std::clamp(f, 0.f, 1.f);
    } else if (name == "smoothen_focus_view_edges") {
        if (!ToFloat(value, f)) return false;
        io.smoothing = std::clamp(f, 0.f, 0.5f);
    } else if (name == "dcsvr_sharpen_taper") {
        if (!ToInt(value, i)) return false;
        io.taper = i != 0;
    } else if (name == "dcsvr_periphery_sharpen") {
        if (!ToFloat(value, f)) return false;
        io.peripherySharpen = std::clamp(f, 0.f, 1.f);
    } else if (name == "dcsvr_temporal") {
        if (!ToInt(value, i)) return false;
        io.temporal = i != 0;
    } else if (name == "dcsvr_temporal_tau_ms") {
        if (!ToFloat(value, f)) return false;
        io.tauMs = std::clamp(f, 0.f, 200.f);
    } else if (name == "dcsvr_temporal_ramp_deg") {
        if (!ToFloat(value, f)) return false;
        io.rampDeg = std::clamp(f, 1.f, 30.f);
    } else if (name == "dcsvr_temporal_clip") {
        if (!ToFloat(value, f)) return false;
        io.clip = std::clamp(f, 0.25f, 4.f);
    } else {
        return false;
    }
    return true;
}

// One file's text, line by line as LoadConfiguration reads it (std::getline on a text-mode stream: CRLF ends a line
// like LF). Live keys go into io; every name=value of an active section into effective (the last one wins), so a
// reload can name the other keys that changed and were not applied.
inline void ParseText(const std::string& text, const Context& context, Values& io,
                      std::map<std::string, std::string>* effective = nullptr) {
    bool active = true;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        const bool terminated = end != std::string::npos;
        if (!terminated) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (terminated && !line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if ((line[0] == '/' && line.size() > 1 && line[1] == '/') || line[0] == '#') continue;
        if (line[0] == '[' && line[line.size() - 1] == ']') {
            if (line.substr(1, 4) == "app:") {
                active = context.application.find(line.substr(5, line.size() - 6)) != std::string::npos;
            } else if (line.substr(1, 4) == "exe:") {
                active = context.executable.find(line.substr(5, line.size() - 6)) != std::string::npos;
            } else {
                const std::string section = line.substr(1, line.size() - 2);
                active = context.runtime.find(section) != std::string::npos ||
                         context.system.find(section) != std::string::npos;
            }
            continue;
        }
        if (!active) continue;
        const size_t offset = line.find('=');
        if (offset == std::string::npos) continue;
        const std::string name = line.substr(0, offset);
        const std::string value = line.substr(offset + 1);
        if (effective) (*effective)[name] = value;
        ApplyKey(name, value, io);
    }
}

// A file's identity for change detection: present, size, last write time.
struct Stamp {
    bool exists{false};
    uint64_t size{0};
    uint64_t time{0};
};
inline bool operator==(const Stamp& a, const Stamp& b) {
    return a.exists == b.exists && a.size == b.size && a.time == b.time;
}

inline Stamp StampOf(const std::wstring& path) {
    Stamp stamp;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!path.empty() && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) &&
        !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        stamp.exists = true;
        stamp.size = (uint64_t)data.nFileSizeHigh << 32 | data.nFileSizeLow;
        stamp.time = (uint64_t)data.ftLastWriteTime.dwHighDateTime << 32 | data.ftLastWriteTime.dwLowDateTime;
    }
    return stamp;
}

// The whole file (at most 1 MB), shared for reading, writing and deletion so a writer's replace never waits on us.
// False: it could not be opened or read.
inline bool ReadAll(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    char buffer[4096];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0) break;
        out.append(buffer, read);
        if (out.size() > (1u << 20)) break;
    }
    CloseHandle(file);
    return ok;
}

// "name old -> new" for each live key that differs, comma separated; empty: none. Only key names and numbers (the
// text is safe as a printf argument and holds no '%').
inline std::string Changes(const Values& before, const Values& after) {
    std::string out;
    char item[96];
    auto add = [&](const char* name, double a, double b, const char* format) {
        if (a == b) return;
        char from[32], to[32];
        std::snprintf(from, sizeof(from), format, a);
        std::snprintf(to, sizeof(to), format, b);
        std::snprintf(item, sizeof(item), "%s%s %s -> %s", out.empty() ? "" : ", ", name, from, to);
        out += item;
    };
    add("sharpen_focus_view", before.sharpen, after.sharpen, "%.2f");
    add("smoothen_focus_view_edges", before.smoothing, after.smoothing, "%.2f");
    add("dcsvr_sharpen_taper", before.taper, after.taper, "%.0f");
    add("dcsvr_periphery_sharpen", before.peripherySharpen, after.peripherySharpen, "%.2f");
    add("dcsvr_temporal", before.temporal, after.temporal, "%.0f");
    add("dcsvr_temporal_tau_ms", before.tauMs, after.tauMs, "%.1f");
    add("dcsvr_temporal_ramp_deg", before.rampDeg, after.rampDeg, "%.1f");
    add("dcsvr_temporal_clip", before.clip, after.clip, "%.2f");
    return out;
}

// A reload's outcome: the live values of the files, and the other keys whose value differs from the start (kept).
struct Result {
    Values values;
    std::vector<std::string> ignored;
};

class Reloader : public std::enable_shared_from_this<Reloader> {
  public:
    // files: in Quad Views' order (the layer's folder, then the profile's or LocalAppData's). defaults: the layer's
    // values before any file (its member initialisers), where every parse starts.
    Reloader(std::vector<std::wstring> files, Context context, const Values& defaults,
             std::chrono::milliseconds interval = std::chrono::milliseconds(1000))
        : m_files(std::move(files)), m_context(std::move(context)), m_defaults(defaults), m_interval(interval) {
        m_stamps.resize(m_files.size());
    }

    // On the setup thread, as the layer loads its configuration: the stamps and the keys the layer starts with.
    void Prime() {
        for (size_t i = 0; i < m_files.size(); i++) m_stamps[i] = StampOf(m_files[i]);
        Values values = m_defaults;
        m_start.clear();
        for (size_t i = 0; i < m_files.size(); i++) {
            std::string text;
            if (m_stamps[i].exists && ReadAll(m_files[i], text)) ParseText(text, m_context, values, &m_start);
        }
    }

    // Off the hot path (one poll at a time): stats the files and parses them only when a stamp changed. True: parsed
    // and published.
    bool Poll() {
        std::vector<Stamp> now(m_files.size());
        for (size_t i = 0; i < m_files.size(); i++) now[i] = StampOf(m_files[i]);
        if (now == m_stamps) return false;
        Result result;
        result.values = m_defaults;
        std::map<std::string, std::string> effective;
        for (size_t i = 0; i < m_files.size(); i++) {
            if (!now[i].exists) continue;
            std::string text;
            if (!ReadAll(m_files[i], text)) return false;  // held by a writer: the next poll tries again
            ParseText(text, m_context, result.values, &effective);
        }
        m_stamps = now;
        m_parses.fetch_add(1, std::memory_order_relaxed);
        for (const auto& [name, value] : effective) {
            const auto start = m_start.find(name);
            if (!IsLiveKey(name) && (start == m_start.end() || start->second != value)) result.ignored.push_back(name);
        }
        for (const auto& [name, value] : m_start) {
            if (!IsLiveKey(name) && effective.find(name) == effective.end()) result.ignored.push_back(name);
        }
        {
            std::lock_guard lock(m_mutex);
            m_pending = std::move(result);
            m_generation.fetch_add(1, std::memory_order_release);
        }
        return true;
    }

    // Render thread: the newest published reload not taken yet. Never blocks: false when none or while a poll
    // publishes (taken at the next frame).
    bool Take(Result& out) {
        if (m_generation.load(std::memory_order_acquire) == m_taken) return false;
        std::unique_lock lock(m_mutex, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        out = m_pending;
        m_taken = m_generation.load(std::memory_order_relaxed);
        return true;
    }

    // Render thread, once per frame: a clock read and an atomic. Hands a poll to the thread pool when the interval
    // passed and no poll is running. The pool's callback holds this object and a reference on the module, so neither
    // the layer's teardown nor the DLL's unload can pull them away under it.
    void Tick(std::chrono::steady_clock::time_point now) {
        if (now - m_lastPoll < m_interval) return;
        if (m_polling.exchange(true, std::memory_order_acq_rel)) return;
        m_lastPoll = now;
        auto job = std::make_unique<Job>();
        job->reloader = shared_from_this();
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&Callback), &job->module);
        if (TrySubmitThreadpoolCallback(&Callback, job.get(), nullptr)) {
            job.release();
        } else {
            if (job->module) FreeLibrary(job->module);
            m_polling.store(false, std::memory_order_release);
        }
    }

    uint64_t Parses() const { return m_parses.load(std::memory_order_relaxed); }
    bool Polling() const { return m_polling.load(std::memory_order_acquire); }

  private:
    struct Job {
        std::shared_ptr<Reloader> reloader;
        HMODULE module{nullptr};
    };
    static void CALLBACK Callback(PTP_CALLBACK_INSTANCE instance, void* context) {
        Job* job = static_cast<Job*>(context);
        try {
            job->reloader->Poll();
        } catch (...) {
        }
        job->reloader->m_polling.store(false, std::memory_order_release);
        const HMODULE module = job->module;
        delete job;
        if (module) FreeLibraryWhenCallbackReturns(instance, module);
    }

    const std::vector<std::wstring> m_files;
    const Context m_context;
    const Values m_defaults;
    const std::chrono::milliseconds m_interval;
    std::vector<Stamp> m_stamps;                    // Prime, then the poll (one at a time)
    std::map<std::string, std::string> m_start;     // every key the layer started with
    std::mutex m_mutex;
    Result m_pending;                               // under m_mutex
    std::atomic<uint64_t> m_generation{0};
    uint64_t m_taken{0};                            // render thread
    std::atomic<bool> m_polling{false};
    std::chrono::steady_clock::time_point m_lastPoll{};  // render thread
    std::atomic<uint64_t> m_parses{0};
};

}  // namespace dcsvr::live
