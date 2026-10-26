// Offline checks of native/quadviews/dcsvr_live.h: Quad Views' live post-process settings. The parser against Quad
// Views' section rules and file order, non-live keys never applied, no parse while the files' stamps stay, the
// non-blocking hand-over and the thread-pool poll. Built and run by scripts/build-quadviews.ps1.
#include <windows.h>

#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <cstdio>
#include <string>
#include <thread>

#include "dcsvr_live.h"

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

using dcsvr::live::Context;
using dcsvr::live::Reloader;
using dcsvr::live::Result;
using dcsvr::live::Values;

const Context Dcs{"DCS World", "DCS.exe", "Pimax OpenXR", "Pimax Crystal Super"};

void WriteText(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr);
    CloseHandle(file);
}

// A distinct write time (a second later than the file's), so a rewrite within the clock's resolution still counts.
void Touch(const std::wstring& path, int64_t seconds) {
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    FILETIME time{};
    GetFileTime(file, nullptr, nullptr, &time);
    ULARGE_INTEGER t{};
    t.LowPart = time.dwLowDateTime;
    t.HighPart = time.dwHighDateTime;
    t.QuadPart += (uint64_t)seconds * 10'000'000ull;
    time.dwLowDateTime = t.LowPart;
    time.dwHighDateTime = t.HighPart;
    SetFileTime(file, nullptr, nullptr, &time);
    CloseHandle(file);
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

bool Has(const std::vector<std::string>& list, const char* name) {
    for (const auto& item : list) {
        if (item == name) return true;
    }
    return false;
}
}  // namespace

int main() {
    // --- The parser: Quad Views' section rules, comments, CRLF, clamps, values that do not parse.
    {
        Values v;
        dcsvr::live::ParseText("sharpen_focus_view=0.3\r\n"
                               "[exe:DCS]\r\n"
                               "smoothen_focus_view_edges=0.9\r\n"  // clamped to 0.5
                               "# dcsvr_periphery_sharpen=0.9\r\n"
                               "// dcsvr_temporal=1\r\n"
                               "dcsvr_periphery_sharpen=0.4\r\n"
                               "[exe:Other]\r\n"
                               "sharpen_focus_view=0.9\r\n"
                               "[app:DCS World]\r\n"
                               "dcsvr_sharpen_taper=1\r\n"
                               "[Pimax]\r\n"  // a runtime or system name: active
                               "dcsvr_temporal=1\r\n"
                               "[Varjo]\r\n"
                               "dcsvr_temporal_tau_ms=10\r\n"
                               "[Crystal Super]\r\n"
                               "dcsvr_temporal_tau_ms=500\r\n"  // clamped to 200
                               "dcsvr_temporal_clip=x\r\n"       // does not parse: kept
                               "dcsvr_temporal_ramp_deg=nan\r\n"  // not finite: kept
                               "dcsvr_temporal_clip= 2.5\r\n",
                               Dcs, v);
        Check(Near(v.sharpen, 0.3f), "parser: a key before any section is active; [exe:Other] is not");
        Check(Near(v.smoothing, 0.5f), "parser: [exe:DCS] active, smoothen_focus_view_edges clamped to 0.5");
        Check(Near(v.peripherySharpen, 0.4f), "parser: '#' and '//' comments skipped");
        Check(v.taper, "parser: [app:DCS World] matches the application name");
        Check(v.temporal, "parser: [Pimax] matches the runtime name, CRLF lines");
        Check(Near(v.tauMs, 200.f), "parser: [Varjo] inactive, [Crystal Super] matches the system name, tau clamped");
        Check(Near(v.clip, 2.5f), "parser: a value that does not parse is skipped, leading blanks parse");
        Check(Near(v.rampDeg, 8.f), "parser: a non-finite value is skipped");

        Values w;
        dcsvr::live::ParseText("[app:DCS]\nsharpen_focus_view=0.1\n[exe:dcs]\nsharpen_focus_view=0.2\n", Dcs, w);
        Check(Near(w.sharpen, 0.1f), "parser: section names are case-sensitive substrings, LF lines");

        Values x;
        std::map<std::string, std::string> effective;
        dcsvr::live::ParseText("[exe:DCS]\nfocus_multiplier=1.25\nturbo_mode=1\nsharpen_focus_view", Dcs, x, &effective);
        Check(x == Values{}, "parser: non-live keys and a line without '=' change no live value");
        Check(effective.size() == 2 && effective["focus_multiplier"] == "1.25", "parser: every active key is recorded");
    }

    // --- Files: the layer's folder, then the profile's (the second wins), as LoadConfiguration.
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir = std::wstring(temp) + L"dcsvr-qv-live-test-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring layerFile = dir + L"\\layer-settings.cfg";
    const std::wstring profileFile = dir + L"\\settings.cfg";
    WriteText(layerFile, "sharpen_focus_view=0.5\ndcsvr_temporal_ramp_deg=12\n");
    const std::string profile =
        "[exe:DCS]\nperipheral_multiplier=0.5\nfocus_multiplier=1.25\nsharpen_focus_view=0.7\n"
        "smoothen_focus_view_edges=0.2\nfocus_view_shape=2\ndcsvr_sharpen_taper=1\ndcsvr_periphery_sharpen=0.3\n"
        "horizontal_focus_section=0.5\nturbo_mode=1\ndcsvr_temporal=1\ndcsvr_temporal_tau_ms=40\n";
    WriteText(profileFile, profile);

    Values defaults;  // Quad Views' member initialisers
    auto reloader = std::make_shared<Reloader>(std::vector<std::wstring>{layerFile, profileFile}, Dcs, defaults,
                                               std::chrono::milliseconds(0));
    reloader->Prime();
    Result result;
    Check(!reloader->Poll(), "stamps unchanged after Prime: no parse");
    Check(!reloader->Poll() && reloader->Parses() == 0, "stamps unchanged again: still no parse");
    Check(!reloader->Take(result), "nothing to take before a reload");

    // A live and two non-live keys change.
    std::string edited = profile;
    edited.replace(edited.find("sharpen_focus_view=0.7"), 22, "sharpen_focus_view=0.4");
    edited.replace(edited.find("focus_multiplier=1.25"), 21, "focus_multiplier=1.50");
    edited.replace(edited.find("turbo_mode=1"), 12, "turbo_mode=0");
    WriteText(profileFile, edited);
    Touch(profileFile, 1);
    Check(reloader->Poll() && reloader->Parses() == 1, "a changed stamp: one parse");
    Check(reloader->Take(result), "the reload is taken");
    Check(!reloader->Take(result), "taken once");
    Check(Near(result.values.sharpen, 0.4f), "reload: the profile's sharpen_focus_view applies over the layer's");
    Check(Near(result.values.rampDeg, 12.f), "reload: the layer file's key applies when the profile lacks it");
    Check(result.values.taper && result.values.temporal && Near(result.values.peripherySharpen, 0.3f) &&
              Near(result.values.tauMs, 40.f) && Near(result.values.smoothing, 0.2f),
          "reload: unchanged live keys keep their file values");
    Check(result.ignored.size() == 2 && Has(result.ignored, "focus_multiplier") && Has(result.ignored, "turbo_mode"),
          "reload: changed non-live keys are reported as ignored, not applied");
    Check(!reloader->Poll() && reloader->Parses() == 1, "after the reload, unchanged stamps: no parse");

    // Same text, newer write time: parsed again, the same values (nothing for the layer to change).
    Touch(profileFile, 2);
    Check(reloader->Poll() && reloader->Parses() == 2 && reloader->Take(result) &&
              Near(result.values.sharpen, 0.4f),
          "a rewrite with the same text: parsed, same values");

    // A file held by a writer without sharing: no parse, the stamps stay, the next poll reads it.
    WriteText(profileFile, profile);
    Touch(profileFile, 3);
    HANDLE held = CreateFileW(profileFile.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(held != INVALID_HANDLE_VALUE && !reloader->Poll() && !reloader->Take(result),
          "a file held exclusively: nothing published");
    CloseHandle(held);
    Check(reloader->Poll() && reloader->Take(result) && Near(result.values.sharpen, 0.7f) && result.ignored.empty(),
          "released: read at the next poll, back to the start values, nothing ignored");

    // The profile file removed: its keys fall back to the layer's file and the defaults (as a start without it).
    DeleteFileW(profileFile.c_str());
    Check(reloader->Poll() && reloader->Take(result) && Near(result.values.sharpen, 0.5f) && !result.values.temporal &&
              Near(result.values.peripherySharpen, 0.f),
          "the profile file removed: the layer file and Quad Views' defaults");

    // The thread-pool poll: Tick hands it over, the render side takes the result without waiting.
    WriteText(profileFile, profile);
    Touch(profileFile, 4);
    auto now = std::chrono::steady_clock::now();
    reloader->Tick(now);
    for (int i = 0; i < 500 && reloader->Polling(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Check(!reloader->Polling() && reloader->Take(result) && Near(result.values.sharpen, 0.7f),
          "Tick: the pool's poll publishes the reload");

    auto slow = std::make_shared<Reloader>(std::vector<std::wstring>{layerFile, profileFile}, Dcs, defaults,
                                           std::chrono::milliseconds(1000));
    slow->Prime();
    const uint64_t before = slow->Parses();
    slow->Tick(now + std::chrono::seconds(5));  // the first poll
    for (int i = 0; i < 500 && slow->Polling(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    WriteText(profileFile, edited);
    Touch(profileFile, 5);
    slow->Tick(now + std::chrono::seconds(5) + std::chrono::milliseconds(500));
    Check(!slow->Polling() && slow->Parses() == before, "Tick: no poll within the interval");
    slow->Tick(now + std::chrono::seconds(6));
    for (int i = 0; i < 500 && slow->Polling(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Check(slow->Parses() == before + 1 && slow->Take(result) && Near(result.values.sharpen, 0.4f),
          "Tick: the next interval polls and parses the change");

    // The layer dropped while a poll runs: the callback keeps its own reference.
    Touch(profileFile, 6);
    slow->Tick(now + std::chrono::seconds(8));
    std::weak_ptr<Reloader> weak = slow;
    slow.reset();
    for (int i = 0; i < 500 && !weak.expired(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Check(weak.expired(), "a poll in flight outlives the owner and then releases the reloader");

    // The key list matches the parser.
    bool all = true;
    for (const char* key : dcsvr::live::Keys) {
        Values v;
        v.sharpen = v.smoothing = v.peripherySharpen = v.tauMs = v.rampDeg = v.clip = -1.f;
        all = all && dcsvr::live::ApplyKey(key, "1", v);
    }
    Check(all && !dcsvr::live::ApplyKey("focus_view_shape", "2", defaults) &&
              !dcsvr::live::ApplyKey("focus_multiplier", "2", defaults),
          "every listed key is applied, focus_view_shape and focus_multiplier are not live");
    Check(dcsvr::live::Changes(Values{}, Values{}).empty() &&
              dcsvr::live::Changes(Values{}, Values{0.5f}) == "sharpen_focus_view 0.70 -> 0.50",
          "the log text names the changed keys");

    DeleteFileW(layerFile.c_str());
    DeleteFileW(profileFile.c_str());
    RemoveDirectoryW(dir.c_str());
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
