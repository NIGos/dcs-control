// DCS Control launcher: the DcsControl.exe at the top of the release folder. It starts the application kept in the
// files\ folder next to it (files\DcsControl.exe) with the same arguments and working directory, waits for it and
// returns its exit code, so the release folder shows users only what they need: this program, "Install DCS
// Control.cmd" and README.txt.
#include <windows.h>

#include <string>

namespace {
// The command line after this program's own name, passed on unchanged. The name is quoted, or it is this program's
// full path written without quotes (possibly with spaces), or, as a last resort, the text up to the first space.
const wchar_t* Arguments(const wchar_t* line, const wchar_t* self) {
    if (*line == L'"') {
        ++line;
        while (*line && *line != L'"') ++line;
        if (*line) ++line;
    } else {
        const size_t length = wcslen(self);
        if (_wcsnicmp(line, self, length) == 0 && (line[length] == 0 || line[length] == L' ' || line[length] == L'\t')) {
            line += length;
        } else {
            while (*line && *line != L' ' && *line != L'\t') ++line;
        }
    }
    while (*line == L' ' || *line == L'\t') ++line;
    return line;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int show) {
    wchar_t self[MAX_PATH * 4]{};
    const DWORD length = GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    if (length == 0 || length >= ARRAYSIZE(self)) return 1;
    std::wstring folder(self);
    folder.resize(folder.find_last_of(L'\\'));
    const std::wstring app = folder + L"\\files\\DcsControl.exe";
    if (GetFileAttributesW(app.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(nullptr, L"The files folder next to DcsControl.exe is missing. Extract the whole ZIP and start DcsControl.exe again.",
            L"DCS Control", MB_OK | MB_ICONERROR);
        return 2;
    }
    std::wstring command = L"\"" + app + L"\"";
    const wchar_t* rest = Arguments(GetCommandLineW(), self);
    if (*rest) command += L" " + std::wstring(rest);
    STARTUPINFOW startup{sizeof(startup)};
    // A shortcut's "Run: Minimized/Maximized" reaches the app too.
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = static_cast<WORD>(show);
    PROCESS_INFORMATION process{};
    // The caller's working directory, so relative arguments mean the same as for the launcher; the app itself finds
    // everything next to its own executable.
    if (!CreateProcessW(app.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        MessageBoxW(nullptr, L"DCS Control could not be started from the files folder.", L"DCS Control", MB_OK | MB_ICONERROR);
        return 3;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}
