// DCS Control setup: DcsControl-<version>-Setup.exe, the one file a pilot downloads and double-clicks. This small
// program carries the release ZIP appended to itself (scripts/build-setup.ps1 adds it with a trailer: the ZIP's SHA-256,
// offset and length, then the marker "DCSCSFX1"). Its window lets the pilot choose the install folder and the shortcuts,
// then it checks the ZIP, unpacks it into a temporary folder and runs the release's own installer
// (files\scripts\installer.ps1: package verification, WebView2 / Visual C++ runtimes from Microsoft when missing, the
// journaled per-user installation with upgrade and rollback, the shortcuts and the Apps & features entry). No
// administrator rights; nothing outside the installation, the shortcuts and that entry changes. The temporary folder is
// removed at the end.
//
// The program alone (without the ZIP) is also the uninstaller: setup copies it into the installation as
// "Uninstall DCS Control.exe", which Settings > Apps runs with --uninstall. It runs from a copy in %TEMP% (its own folder
// goes away), can put DCS back to stock first and can delete the pilot's profiles and settings when asked.
//
// The app's updater starts the setup with --update --wait-pid <pid> --relaunch: no first page, wait for the app to exit,
// install into the existing installation, start the new version. --quiet shows no window at all (the exit code and
// %TEMP%\DcsControl-Setup.log tell the result). --destination, --start-menu, --desktop and --uninstall-key move the
// installation, the shortcuts and the Apps & features entry (the release tests install into a scratch folder);
// --no-launch and --skip-prerequisites are for those tests too.
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "version.h"  // written by scripts/build-setup.ps1 from Directory.Build.props

namespace {
constexpr wchar_t kSetupTitle[] = L"DCS Control " DCSC_VERSION_TEXT L" Setup";
constexpr wchar_t kUninstallTitle[] = L"Uninstall DCS Control";
constexpr wchar_t kUninstallerName[] = L"Uninstall DCS Control.exe";
constexpr wchar_t kDefaultUninstallKey[] = L"HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DcsControl";
constexpr char kMarker[8] = {'D', 'C', 'S', 'C', 'S', 'F', 'X', '1'};
constexpr size_t kTrailer = 32 + 8 + 8 + 8;  // SHA-256, offset, length, marker

struct Options {
    bool update = false, quiet = false, relaunch = false, noLaunch = false, skipPrerequisites = false;
    bool uninstall = false, relocated = false;
    bool desktopShortcut = true, startMenuShortcut = true;
    DWORD waitPid = 0;
    std::wstring destination, startMenu, desktop, uninstallKey;
    std::wstring relaunchPath;  // the app that started the update (an unzipped copy may live elsewhere)
};

// What the worker reports to the window, and its result.
struct Progress {
    std::mutex lock;
    std::wstring stage;
    std::atomic<bool> done{false};
    bool ok = false;
    std::wstring error, note;
    void Stage(const std::wstring& text) { std::lock_guard<std::mutex> guard(lock); stage = text; }
    std::wstring Current() { std::lock_guard<std::mutex> guard(lock); return stage; }
};

std::wstring g_log;  // %TEMP%\DcsControl-Setup.log

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) result = path;
    CoTaskMemFree(path);
    return result;
}

std::wstring TempFolder() {
    wchar_t buffer[MAX_PATH + 2]{};
    const DWORD length = GetTempPathW(ARRAYSIZE(buffer), buffer);
    std::wstring path(buffer, length);
    while (!path.empty() && path.back() == L'\\') path.pop_back();
    return path;
}

std::wstring FullPath(const std::wstring& path) {
    wchar_t buffer[MAX_PATH * 4]{};
    const DWORD length = GetFullPathNameW(path.c_str(), ARRAYSIZE(buffer), buffer, nullptr);
    std::wstring result = length > 0 && length < ARRAYSIZE(buffer) ? std::wstring(buffer, length) : path;
    // A trailing backslash would escape the closing quote of a command-line argument.
    while (result.size() > 3 && result.back() == L'\\') result.pop_back();
    return result;
}

std::wstring SelfPath() {
    wchar_t self[MAX_PATH * 4]{};
    const DWORD length = GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    return length > 0 && length < ARRAYSIZE(self) ? std::wstring(self, length) : std::wstring();
}

std::wstring Parent(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool Same(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
// <path> is <folder> or inside it.
bool Within(const std::wstring& path, const std::wstring& folder) {
    if (folder.empty()) return false;
    const std::wstring base = folder.back() == L'\\' ? folder : folder + L"\\";
    return Same(path, folder) || (path.size() > base.size() && _wcsnicmp(path.c_str(), base.c_str(), base.size()) == 0);
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool IsFolder(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool FolderEmpty(const std::wstring& folder) {
    WIN32_FIND_DATAW item{};
    const HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &item);
    if (find == INVALID_HANDLE_VALUE) return true;
    bool empty = true;
    do {
        const std::wstring name = item.cFileName;
        if (name != L"." && name != L"..") empty = false;
    } while (empty && FindNextFileW(find, &item));
    FindClose(find);
    return empty;
}

void Log(const std::wstring& line) {
    const HANDLE file = CreateFileW(g_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::wstring text = line + L"\r\n";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), bytes, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
}

std::wstring Decode(const std::string& bytes) {
    // Windows PowerShell writes redirected output in the console's code page; the setup's own lines are UTF-8. The log
    // mixes both, so each line is decoded on its own: UTF-8 when it is valid UTF-8, the OEM code page otherwise (a user
    // name with an accent in a PowerShell line must not turn the setup's own lines into mojibake).
    std::wstring text;
    size_t start = 0;
    while (start < bytes.size()) {
        size_t end = bytes.find('\n', start);
        end = end == std::string::npos ? bytes.size() : end + 1;
        const char* line = bytes.data() + start;
        const int length = static_cast<int>(end - start);
        const UINT page = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line, length, nullptr, 0) > 0 ? CP_UTF8 : GetOEMCP();
        const int wide = MultiByteToWideChar(page, 0, line, length, nullptr, 0);
        const size_t at = text.size();
        text.resize(at + static_cast<size_t>(wide));
        MultiByteToWideChar(page, 0, line, length, text.data() + at, wide);
        start = end;
    }
    return text;
}

std::string ReadBytes(const std::wstring& path, size_t maxBytes) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::string();
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    const LONGLONG start = size.QuadPart > static_cast<LONGLONG>(maxBytes) ? size.QuadPart - static_cast<LONGLONG>(maxBytes) : 0;
    LARGE_INTEGER position{};
    position.QuadPart = start;
    SetFilePointerEx(file, position, nullptr, FILE_BEGIN);
    std::string bytes(static_cast<size_t>(size.QuadPart - start), '\0');
    DWORD read = 0;
    ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    bytes.resize(read);
    return bytes;
}

// The last lines of the log (the installer's own messages), for the progress page and an error's details.
std::wstring LogTail(size_t maxBytes, bool lastLineOnly) {
    std::wstring text = Decode(ReadBytes(g_log, maxBytes));
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    if (!lastLineOnly) return text;
    const size_t newline = text.find_last_of(L'\n');
    std::wstring line = newline == std::wstring::npos ? text : text.substr(newline + 1);
    while (!line.empty() && line.back() == L'\r') line.pop_back();
    if (line.size() > 140) line = line.substr(0, 137) + L"...";
    return line;
}

bool Running(const wchar_t* name) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
        found = entry.th32ProcessID != GetCurrentProcessId() && _wcsicmp(entry.szExeFile, name) == 0;
    CloseHandle(snapshot);
    return found;
}
// DCS Control as it is named now and before 0.5.0 (DCS VR Control).
bool AppRunning() { return Running(L"DcsControl.exe") || Running(L"DcsVrControl.exe"); }

// Runs a program without a window, its output appended to <output> (the log by default); returns its exit code (or -1
// when it did not start).
int RunHidden(const std::wstring& program, std::wstring commandLine, const std::wstring& output = std::wstring()) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    const std::wstring target = output.empty() ? g_log : output;
    const HANDLE log = CreateFileW(target.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullptr;
    startup.hStdOutput = startup.hStdError = log;
    PROCESS_INFORMATION process{};
    Log(L"> " + commandLine);
    const BOOL started = CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, log != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!started) { Log(L"Could not start " + program + L" (error " + std::to_wstring(GetLastError()) + L")."); return -1; }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}

std::wstring SystemFolder() { wchar_t root[MAX_PATH]{}; GetSystemDirectoryW(root, MAX_PATH); return root; }
std::wstring PowerShell() { return SystemFolder() + L"\\WindowsPowerShell\\v1.0\\powershell.exe"; }
std::wstring Quote(const std::wstring& value) { return L"\"" + value + L"\""; }

void DeleteTree(const std::wstring& folder) {
    WIN32_FIND_DATAW item{};
    const HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &item);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = item.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring path = folder + L"\\" + name;
            if ((item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) DeleteTree(path);
            else if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(path.c_str());  // a link, never followed
            else { SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(path.c_str()); }
        } while (FindNextFileW(find, &item));
        FindClose(find);
    }
    // An antivirus may still hold a file it scans for a moment.
    for (int attempt = 0; attempt < 10 && !RemoveDirectoryW(folder.c_str()) && Exists(folder); ++attempt) Sleep(200);
}

// Removes the empty folders under <folder> and <folder> itself when it ends up empty; files are never touched.
void RemoveEmptyFolders(const std::wstring& folder) {
    WIN32_FIND_DATAW item{};
    const HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &item);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = item.cFileName;
            if (name != L"." && name != L".." && (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                RemoveEmptyFolders(folder + L"\\" + name);
        } while (FindNextFileW(find, &item));
        FindClose(find);
    }
    RemoveDirectoryW(folder.c_str());
}

// The trailer of this program: where the appended ZIP is and its SHA-256. False for the uninstaller (no ZIP).
struct Payload { uint64_t offset = 0, bytes = 0; unsigned char digest[32]{}; };
bool ReadPayload(Payload& payload) {
    const HANDLE file = CreateFileW(SelfPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    unsigned char trailer[kTrailer]{};
    LARGE_INTEGER position{};
    position.QuadPart = size.QuadPart - static_cast<LONGLONG>(kTrailer);
    DWORD read = 0;
    const bool ok = size.QuadPart >= static_cast<LONGLONG>(kTrailer) && SetFilePointerEx(file, position, nullptr, FILE_BEGIN) && ReadFile(file, trailer, kTrailer, &read, nullptr)
        && read == kTrailer && memcmp(trailer + 48, kMarker, sizeof(kMarker)) == 0;
    CloseHandle(file);
    if (!ok) return false;
    memcpy(payload.digest, trailer, 32);
    memcpy(&payload.offset, trailer + 32, 8);
    memcpy(&payload.bytes, trailer + 40, 8);
    return payload.offset + payload.bytes + kTrailer == static_cast<uint64_t>(size.QuadPart);
}

// What the appended ZIP holds once unpacked (the sum of its entries' sizes), for the space shown on the first page.
uint64_t PayloadUnpackedBytes(const Payload& payload) {
    const HANDLE file = CreateFileW(SelfPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    uint64_t total = 0;
    auto readAt = [&](uint64_t at, void* into, DWORD count) {
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(payload.offset + at);
        DWORD got = 0;
        return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) && ReadFile(file, into, count, &got, nullptr) && got == count;
    };
    // The end-of-central-directory record: in the last 64 KB + 22 bytes (after an optional comment).
    const uint64_t tail = payload.bytes < 65557 ? payload.bytes : 65557;
    std::vector<unsigned char> end(static_cast<size_t>(tail));
    if (tail >= 22 && readAt(payload.bytes - tail, end.data(), static_cast<DWORD>(tail))) {
        for (size_t at = end.size() - 22 + 1; at-- > 0;) {
            if (end[at] != 0x50 || end[at + 1] != 0x4b || end[at + 2] != 0x05 || end[at + 3] != 0x06) continue;
            uint32_t directorySize = 0, directoryOffset = 0;
            memcpy(&directorySize, &end[at + 12], 4);
            memcpy(&directoryOffset, &end[at + 16], 4);
            std::vector<unsigned char> directory(directorySize);
            if (directorySize > 0 && directorySize < (64u << 20) && readAt(directoryOffset, directory.data(), directorySize)) {
                for (size_t entry = 0; entry + 46 <= directory.size();) {
                    if (directory[entry] != 0x50 || directory[entry + 1] != 0x4b || directory[entry + 2] != 0x01 || directory[entry + 3] != 0x02) break;
                    uint32_t unpacked = 0;
                    uint16_t name = 0, extra = 0, comment = 0;
                    memcpy(&unpacked, &directory[entry + 24], 4);
                    memcpy(&name, &directory[entry + 28], 2);
                    memcpy(&extra, &directory[entry + 30], 2);
                    memcpy(&comment, &directory[entry + 32], 2);
                    total += unpacked;
                    entry += 46u + name + extra + comment;
                }
            }
            break;
        }
    }
    CloseHandle(file);
    return total;
}

// Copies the ZIP appended to this program into <zip>, checking its SHA-256 against the trailer.
bool ExtractPayload(const std::wstring& zip, std::wstring& error) {
    Payload payload;
    if (!ReadPayload(payload)) { error = L"This setup is incomplete or damaged (no DCS Control package inside). Download it again."; return false; }
    const HANDLE file = CreateFileW(SelfPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = L"The setup could not read its own file."; return false; }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    const HANDLE output = CreateFileW(zip.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = output != INVALID_HANDLE_VALUE;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(payload.offset);
    ok = ok && SetFilePointerEx(file, position, nullptr, FILE_BEGIN);
    std::vector<unsigned char> buffer(1 << 20);
    for (uint64_t left = payload.bytes; ok && left > 0;) {
        const DWORD chunk = static_cast<DWORD>(left < buffer.size() ? left : buffer.size());
        DWORD got = 0, written = 0;
        ok = ReadFile(file, buffer.data(), chunk, &got, nullptr) && got == chunk && WriteFile(output, buffer.data(), got, &written, nullptr) && written == got;
        if (ok) BCryptHashData(hash, buffer.data(), got, 0);
        left -= got;
    }
    unsigned char digest[32]{};
    BCryptFinishHash(hash, digest, sizeof(digest), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    CloseHandle(file);
    if (!ok) { error = L"The package could not be unpacked into the temporary folder (" + zip + L"). Check the free disk space."; return false; }
    if (memcmp(digest, payload.digest, sizeof(digest)) != 0) { error = L"This setup is damaged (its package does not match its checksum). Download it again."; return false; }
    return true;
}

// The release folder inside the unpacked ZIP: the one folder that holds release-manifest.json.
std::wstring FindRelease(const std::wstring& folder) {
    WIN32_FIND_DATAW item{};
    const HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &item);
    std::wstring found;
    if (find == INVALID_HANDLE_VALUE) return found;
    do {
        const std::wstring name = item.cFileName;
        if (name != L"." && name != L".." && (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && Exists(folder + L"\\" + name + L"\\release-manifest.json")) found = folder + L"\\" + name;
    } while (found.empty() && FindNextFileW(find, &item));
    FindClose(find);
    return found;
}

std::wstring DefaultDestination() { return KnownFolder(FOLDERID_LocalAppData) + L"\\Programs\\DcsControl"; }
std::wstring StartMenuFolder(const Options& options) { return options.startMenu.empty() ? KnownFolder(FOLDERID_Programs) : options.startMenu; }
std::wstring DesktopFolder(const Options& options) { return options.desktop.empty() ? KnownFolder(FOLDERID_Desktop) : options.desktop; }
bool Installed(const std::wstring& folder) { return !folder.empty() && Exists(folder + L"\\installation.json"); }
// A DCS Control installation, or what an earlier one left (its installer's journals): the setup may install there.
bool Ours(const std::wstring& folder) { return Installed(folder) || IsFolder(folder + L"\\.installer"); }

// The Apps & features key below HKEY_CURRENT_USER (the option is in PowerShell's form, HKCU:\...).
std::wstring UninstallSubkey(const Options& options) {
    std::wstring key = options.uninstallKey.empty() ? kDefaultUninstallKey : options.uninstallKey;
    for (const wchar_t* prefix : {L"HKCU:\\", L"HKEY_CURRENT_USER\\"})
        if (_wcsnicmp(key.c_str(), prefix, wcslen(prefix)) == 0) return key.substr(wcslen(prefix));
    return key;
}

std::wstring EntryValue(const Options& options, const wchar_t* name) {
    wchar_t value[MAX_PATH * 4]{};
    DWORD bytes = sizeof(value) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, UninstallSubkey(options).c_str(), name, RRF_RT_REG_SZ, nullptr, value, &bytes) != ERROR_SUCCESS) return std::wstring();
    return value;
}

// The installation this setup updates: the one named on the command line, the one Apps & features knows, or the default.
std::wstring ExistingInstallation(const Options& options) {
    if (!options.destination.empty()) return Installed(options.destination) ? options.destination : std::wstring();
    const std::wstring registered = EntryValue(options, L"InstallLocation");
    if (Installed(registered)) return FullPath(registered);
    return Installed(DefaultDestination()) ? DefaultDestination() : std::wstring();
}

// The state folder the app uses (profiles, settings, DCS backups, logs), as ControlService.StateRootIn picks it:
// DcsControl, or DcsVrControl (its name before 0.5.0) when only that one exists. Only this folder is what the CLI's
// status describes, so only this one is ever offered for deletion.
std::vector<std::wstring> StateFolders() {
    const std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    if (IsFolder(local + L"\\DcsControl")) return {local + L"\\DcsControl"};
    if (IsFolder(local + L"\\DcsVrControl")) return {local + L"\\DcsVrControl"};
    return {};
}

std::wstring Size(uint64_t bytes) {
    wchar_t text[64]{};
    StrFormatByteSizeW(static_cast<LONGLONG>(bytes), text, ARRAYSIZE(text));
    return text;
}

// Free space on the drive of <path> (or of its nearest folder that exists); 0 when unknown.
uint64_t FreeSpace(std::wstring path) {
    while (!path.empty() && !IsFolder(path)) path = Parent(path);
    ULARGE_INTEGER free{};
    return !path.empty() && GetDiskFreeSpaceExW((path + L"\\").c_str(), &free, nullptr, nullptr) ? free.QuadPart : 0;
}

// Copies this program without its package into the installation as its uninstaller and points Apps & features at it.
bool WriteUninstaller(const std::wstring& root, const Options& options) {
    const std::wstring self = SelfPath(), target = root + L"\\" + kUninstallerName, partial = target + L".partial";
    Payload payload;
    const uint64_t keep = ReadPayload(payload) ? payload.offset : UINT64_MAX;
    bool ok = false;
    if (keep == UINT64_MAX) ok = Same(self, target) || CopyFileW(self.c_str(), partial.c_str(), FALSE);
    else {
        const std::string program = [&]() {
            const HANDLE file = CreateFileW(self.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            std::string bytes(static_cast<size_t>(keep), '\0');
            DWORD read = 0;
            const bool done = file != INVALID_HANDLE_VALUE && ReadFile(file, bytes.data(), static_cast<DWORD>(keep), &read, nullptr) && read == keep;
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            return done ? bytes : std::string();
        }();
        const HANDLE output = program.empty() ? INVALID_HANDLE_VALUE : CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        ok = output != INVALID_HANDLE_VALUE && WriteFile(output, program.data(), static_cast<DWORD>(program.size()), &written, nullptr) && written == program.size();
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    }
    if (ok && Exists(partial)) ok = MoveFileExW(partial.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) { DeleteFileW(partial.c_str()); Log(L"The uninstaller could not be written to " + target + L"; Settings > Apps keeps the installation's own Uninstall.cmd."); return false; }
    const std::wstring command = Quote(target) + L" --uninstall";
    const std::wstring subkey = UninstallSubkey(options);
    const auto bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    if (RegSetKeyValueW(HKEY_CURRENT_USER, subkey.c_str(), L"UninstallString", REG_SZ, command.c_str(), bytes) != ERROR_SUCCESS) {
        Log(L"Settings > Apps could not be pointed at " + target + L".");
        return false;
    }
    return true;
}

bool WaitForParent(const Options& options, Progress& progress, const wchar_t* who) {
    if (!options.waitPid) return true;
    progress.Stage(std::wstring(L"Waiting for ") + who + L" to close...");
    if (const HANDLE app = OpenProcess(SYNCHRONIZE, FALSE, options.waitPid)) {
        const DWORD waited = WaitForSingleObject(app, 120000);
        CloseHandle(app);
        if (waited == WAIT_TIMEOUT) return false;
    }
    return true;
}

// The whole installation; runs on the worker thread (or directly with --quiet).
void Install(const Options& options, Progress& progress) {
    auto fail = [&](const std::wstring& message) { Log(L"Error: " + message); progress.error = message; };
    if (!WaitForParent(options, progress, L"DCS Control")) return fail(L"DCS Control did not close. Close it, then start the update again.");
    // The launcher (DcsControl.exe at the top of the installation) exits right after the app.
    for (int attempt = 0; attempt < 60 && AppRunning(); ++attempt) Sleep(500);
    if (AppRunning()) return fail(L"DCS Control is running. Close it, then run the setup again.");
    if (Running(L"DCS.exe")) return fail(L"DCS is running. Close DCS, then run the setup again.");

    progress.Stage(L"Unpacking DCS Control " DCSC_VERSION_TEXT L"...");
    const std::wstring temp = TempFolder() + L"\\DcsControl-Setup-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const std::wstring zip = temp + L"\\package.zip", unpacked = temp + L"\\package";
    CreateDirectoryW(temp.c_str(), nullptr);
    std::wstring error;
    bool ok = ExtractPayload(zip, error);
    std::wstring release;
    if (ok) {
        // .NET's ZIP reader (in Windows PowerShell) unpacks it; the paths go by environment, never through quoting.
        SetEnvironmentVariableW(L"DCSC_SETUP_ZIP", zip.c_str());
        SetEnvironmentVariableW(L"DCSC_SETUP_FOLDER", unpacked.c_str());
        const int code = RunHidden(PowerShell(), Quote(PowerShell()) + L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"Add-Type -AssemblyName System.IO.Compression.FileSystem; "
            L"[IO.Compression.ZipFile]::ExtractToDirectory($env:DCSC_SETUP_ZIP, $env:DCSC_SETUP_FOLDER)\"");
        DeleteFileW(zip.c_str());
        release = code == 0 ? FindRelease(unpacked) : L"";
        ok = !release.empty();
        if (!ok) error = L"The package could not be unpacked into " + temp + L".";
    }
    if (ok) {
        progress.Stage(L"Installing DCS Control " DCSC_VERSION_TEXT L"...");
        std::wstring command = Quote(PowerShell()) + L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + Quote(release + L"\\files\\scripts\\installer.ps1")
            + L" -Action Install -Destination " + Quote(options.destination);
        command += options.desktopShortcut ? L" -DesktopShortcut" : L" -NoDesktopShortcut";
        if (!options.startMenuShortcut) command += L" -NoStartMenuShortcut";
        if (!options.skipPrerequisites) command += L" -InstallPrerequisites";
        if (!options.startMenu.empty()) command += L" -StartMenuFolder " + Quote(options.startMenu);
        if (!options.desktop.empty()) command += L" -DesktopFolder " + Quote(options.desktop);
        if (!options.uninstallKey.empty()) command += L" -UninstallKey " + Quote(options.uninstallKey);
        const int code = RunHidden(PowerShell(), command);
        ok = code == 0;
        if (!ok) {
            const std::wstring last = LogTail(4096, true);
            // The installer's last steps (shortcuts, the Settings > Apps entry) run after the files are in place.
            const std::string ledger = ReadBytes(options.destination + L"\\installation.json", 1 << 20);
            const bool placed = ledger.find("\"" DCSC_VERSION_TEXT "\"") != std::string::npos || ledger.find("\"version\": \"" DCSC_VERSION_TEXT "\"") != std::string::npos;
            error = L"The installer stopped" + (last.empty() ? std::wstring(L".") : L": " + last) + (placed
                ? L"\n\nDCS Control " DCSC_VERSION_TEXT L" is installed, but its shortcuts or its Settings \u2192 Apps entry may be missing. Run the setup again to complete them."
                : L"\n\nThe installation was rolled back: the version you had, if any, is unchanged.");
        }
    }
    progress.Stage(L"Removing temporary files...");
    DeleteTree(temp);
    if (!ok) return fail(error);
    WriteUninstaller(options.destination, options);
    Log(L"Installed in " + options.destination + L".");
    progress.ok = true;
}

// How many DCS files DCS Control has changed (the CLI's status), or -1 when that could not be read.
int AppliedFiles(const std::wstring& root) {
    const std::wstring cli = root + L"\\files\\DcsVr.Cli.exe";
    if (!Exists(cli)) return -1;
    const std::wstring output = TempFolder() + L"\\DcsControl-Uninstall-status-" + std::to_wstring(GetCurrentProcessId()) + L".json";
    DeleteFileW(output.c_str());
    const int code = RunHidden(cli, Quote(cli) + L" status", output);
    const std::wstring text = Decode(ReadBytes(output, 1 << 20));
    DeleteFileW(output.c_str());
    const size_t at = text.find(L"\"count\":");
    if (code != 0 || at == std::wstring::npos) return -1;
    return static_cast<int>(wcstol(text.c_str() + at + 8, nullptr, 10));
}

// The whole removal; runs on the worker thread (or directly with --quiet).
void Uninstall(const Options& options, Progress& progress, bool restoreDcs, bool deleteState) {
    auto fail = [&](const std::wstring& message) { Log(L"Error: " + message); progress.error = message; };
    const std::wstring root = options.destination;
    WaitForParent(options, progress, L"the uninstaller");
    for (int attempt = 0; attempt < 20 && AppRunning(); ++attempt) Sleep(500);
    if (AppRunning()) return fail(L"DCS Control is running. Close it, then uninstall again.");
    if (Running(L"DCS.exe")) return fail(L"DCS is running. Close DCS, then uninstall again.");
    if (restoreDcs) {
        progress.Stage(L"Putting DCS back to stock...");
        const std::wstring cli = root + L"\\files\\DcsVr.Cli.exe";
        if (RunHidden(cli, Quote(cli) + L" restore") != 0)
            return fail(L"DCS could not be put back to stock, so nothing was uninstalled. Open DCS Control and press Back to stock DCS to see why.");
    }
    progress.Stage(L"Removing DCS Control...");
    std::wstring command = Quote(PowerShell()) + L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + Quote(root + L"\\files\\scripts\\installer.ps1")
        + L" -Action Uninstall -Destination " + Quote(root);
    if (!options.startMenu.empty()) command += L" -StartMenuFolder " + Quote(options.startMenu);
    if (!options.desktop.empty()) command += L" -DesktopFolder " + Quote(options.desktop);
    if (!options.uninstallKey.empty()) command += L" -UninstallKey " + Quote(options.uninstallKey);
    if (RunHidden(PowerShell(), command) != 0) {
        const std::wstring last = LogTail(4096, true);
        return fail(L"The uninstaller stopped" + (last.empty() ? std::wstring(L".") : L": " + last));
    }
    // What the installer's ledger does not list: this uninstaller, the ledger's own journals and the folders left empty.
    const std::wstring uninstaller = root + L"\\" + kUninstallerName;
    for (int attempt = 0; attempt < 20 && Exists(uninstaller) && !DeleteFileW(uninstaller.c_str()); ++attempt) Sleep(250);
    if (!Installed(root) && IsFolder(root + L"\\.installer")) DeleteTree(root + L"\\.installer");
    RemoveEmptyFolders(root);
    if (deleteState) {
        progress.Stage(L"Deleting your profiles and settings...");
        for (const std::wstring& folder : StateFolders()) DeleteTree(folder);
    }
    if (Exists(root)) progress.note = L"Files you added yourself are still in " + root + L".";
    Log(L"Uninstalled from " + root + L".");
    progress.ok = true;
}

bool StartApp(const std::wstring& folder) {
    const std::wstring app = folder + L"\\DcsControl.exe";
    std::wstring command = Quote(app);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!Exists(app) || !CreateProcessW(app.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup, &process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The window: a header with the title, a page, and a bar with the buttons. Pages: the choices (install folder and
// shortcuts, or what the uninstaller removes), the progress, and the result.

enum class Page { Choose, Work, Result };
enum Control : int {
    kPrimary = IDOK, kSecondary = IDCANCEL, kText = 100, kFolderLabel, kFolder, kBrowse, kSpace, kDesktop, kStartMenu, kWarning,
    kBar, kStage, kDetail, kLaunch, kRestore, kDelete, kOpenLog
};
constexpr UINT kDone = WM_APP + 1;
constexpr UINT_PTR kCheckTimer = 1, kProgressTimer = 2;
constexpr int kWidth = 620, kHeight = 430, kHeader = 78, kFooter = 56;

struct Ui {
    HWND window = nullptr;
    Options options;
    Page page = Page::Choose;
    bool upgrade = false;
    std::wstring existingVersion;
    uint64_t needed = 0;
    int applied = -1;  // uninstall: DCS files DCS Control changed (-1: unknown)
    std::vector<std::wstring> state;
    std::wstring title, subtitle;
    Progress progress;
    std::thread worker;
    std::wstring shownStage, shownDetail;
    std::wstring createdFolder;  // the topmost folder the setup created for the installation
    bool restoring = false;
    int result = 2;
    UINT dpi = 96;
    HFONT body = nullptr, heading = nullptr;
    HICON icon = nullptr;
    HBRUSH white = nullptr, bar = nullptr;
};
Ui g_ui;

int Scale(int value) { return MulDiv(value, static_cast<int>(g_ui.dpi), 96); }
HWND Item(int id) { return GetDlgItem(g_ui.window, id); }
void SetText(int id, const std::wstring& text) { SetWindowTextW(Item(id), text.c_str()); }
std::wstring Text(int id) {
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(Item(id))) + 1, L'\0');
    text.resize(static_cast<size_t>(GetWindowTextW(Item(id), text.data(), static_cast<int>(text.size()))));
    return text;
}
bool Checked(int id) { return IsWindowVisible(Item(id)) && SendMessageW(Item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
void Check(int id, bool on) { SendMessageW(Item(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
void Show(int id, bool on) { ShowWindow(Item(id), on ? SW_SHOW : SW_HIDE); }

void Header(const std::wstring& title, const std::wstring& subtitle) {
    g_ui.title = title;
    g_ui.subtitle = subtitle;
    RECT header{0, 0, Scale(kWidth), Scale(kHeader)};
    InvalidateRect(g_ui.window, &header, TRUE);
}

void Fonts() {
    if (g_ui.body) DeleteObject(g_ui.body);
    if (g_ui.heading) DeleteObject(g_ui.heading);
    if (g_ui.icon) DestroyIcon(g_ui.icon);
    const int dpi = static_cast<int>(g_ui.dpi);
    g_ui.body = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_ui.heading = CreateFontW(-MulDiv(14, dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_ui.icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, Scale(44), Scale(44), LR_DEFAULTCOLOR));
    for (HWND child = GetWindow(g_ui.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui.body), TRUE);
}

void Place(int id, int x, int y, int width, int height) { SetWindowPos(Item(id), nullptr, Scale(x), Scale(y), Scale(width), Scale(height), SWP_NOZORDER | SWP_NOACTIVATE); }

void Layout() {
    const int left = 28, width = kWidth - 2 * left, top = kHeader + 18, buttons = kHeight - kFooter + 14;
    const bool uninstall = g_ui.options.uninstall;
    switch (g_ui.page) {
    case Page::Choose:
        if (uninstall) {
            Place(kText, left, top, width, 96);
            Place(kRestore, left, top + 104, width, 24);
            Place(kDelete, left, top + 132, width, 40);
            Place(kWarning, left, top + 182, width, 44);
        } else {
            Place(kText, left, top, width, 64);
            Place(kFolderLabel, left, top + 72, width, 20);
            Place(kFolder, left, top + 94, width - 116, 26);
            Place(kBrowse, left + width - 106, top + 93, 106, 28);
            Place(kSpace, left, top + 126, width, 20);
            Place(kStartMenu, left, top + 156, width, 24);
            Place(kDesktop, left, top + 182, width, 24);
            Place(kWarning, left, top + 214, width, 44);
        }
        break;
    case Page::Work:
        Place(kStage, left, top + 14, width, 22);
        Place(kBar, left, top + 42, width, 18);
        Place(kDetail, left, top + 70, width, 80);
        break;
    case Page::Result:
        Place(kText, left, top, width, 176);
        Place(kLaunch, left, top + 184, width, 24);
        Place(kOpenLog, left, buttons, 120, 30);
        break;
    }
    Place(kPrimary, kWidth - left - 2 * 104 - 8, buttons, 104, 30);
    Place(kSecondary, kWidth - left - 104, buttons, 104, 30);
}

void ShowPage(Page page) {
    g_ui.page = page;
    const bool uninstall = g_ui.options.uninstall;
    const bool choose = page == Page::Choose, work = page == Page::Work, result = page == Page::Result;
    Show(kText, choose || result);
    for (int id : {kFolderLabel, kFolder, kBrowse, kSpace, kDesktop, kStartMenu}) Show(id, choose && !uninstall);
    Show(kRestore, choose && uninstall && g_ui.applied > 0);
    // Deleting the settings is offered only when DCS Control knows what DCS still needs from them (its backups).
    Show(kDelete, choose && uninstall && !g_ui.state.empty() && g_ui.applied >= 0);
    Show(kWarning, false);
    for (int id : {kBar, kStage, kDetail}) Show(id, work);
    Show(kLaunch, result && !uninstall && g_ui.progress.ok && !g_ui.options.noLaunch);
    Show(kOpenLog, result && !g_ui.progress.ok);
    Show(kSecondary, !result);
    EnableWindow(Item(kSecondary), !work);
    EnableWindow(Item(kPrimary), !work);
    Layout();
    KillTimer(g_ui.window, kCheckTimer);
    KillTimer(g_ui.window, kProgressTimer);
    if (choose) SetTimer(g_ui.window, kCheckTimer, 1000, nullptr);
    if (work) {
        SendMessageW(Item(kBar), PBM_SETMARQUEE, TRUE, 30);
        SetTimer(g_ui.window, kProgressTimer, 250, nullptr);
    }
    InvalidateRect(g_ui.window, nullptr, TRUE);
}

// The first page's live checks: DCS and DCS Control closed, enough space. Install / Uninstall waits for them.
void CheckChoices() {
    std::wstring warning;
    if (Running(L"DCS.exe")) warning = L"DCS is running. Close DCS to continue.";
    else if (AppRunning()) warning = L"DCS Control is running. Close it to continue.";
    if (!g_ui.options.uninstall) {
        const std::wstring folder = FullPath(Text(kFolder));
        const uint64_t free = FreeSpace(folder);
        std::wstring space = g_ui.needed ? L"Space needed: about " + Size(g_ui.needed) : std::wstring();
        if (free) space += (space.empty() ? L"" : L"     ") + std::wstring(L"Free on that drive: ") + Size(free);
        SetText(kSpace, space);
        if (warning.empty() && free && g_ui.needed && free < g_ui.needed + (64ull << 20)) warning = L"There is not enough free space on that drive. Choose another folder or free some space.";
    }
    if (g_ui.options.uninstall && IsWindowVisible(Item(kRestore))) {
        // Deleting the settings would also delete the backups of the DCS files DCS Control changed.
        const bool restore = Checked(kRestore);
        EnableWindow(Item(kDelete), restore);
        if (!restore) Check(kDelete, false);
    }
    if (Text(kWarning) != warning) SetText(kWarning, warning);
    Show(kWarning, !warning.empty());
    EnableWindow(Item(kPrimary), warning.empty());
}

// The folder a pick in the folder dialog means: an installation or an empty folder as it is, otherwise a DcsControl
// folder inside it (never the files of another program, a drive or Documents).
std::wstring Suggested(const std::wstring& picked) {
    const std::wstring folder = FullPath(picked);
    if (Ours(folder) || (folder.size() > 3 && FolderEmpty(folder))) return folder;
    const std::wstring name = folder.substr(folder.find_last_of(L'\\') + 1);
    if (Same(name, L"DcsControl") || Same(name, L"DCS Control")) return folder;
    return (folder.back() == L'\\' ? folder : folder + L"\\") + L"DcsControl";
}

void Browse() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Choose where to install DCS Control");
    dialog->SetOkButtonLabel(L"Select folder");
    std::wstring start = FullPath(Text(kFolder));
    while (!start.empty() && !IsFolder(start)) start = Parent(start);
    IShellItem* folder = nullptr;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder)))) { dialog->SetFolder(folder); folder->Release(); }
    IShellItem* picked = nullptr;
    PWSTR path = nullptr;
    if (SUCCEEDED(dialog->Show(g_ui.window)) && SUCCEEDED(dialog->GetResult(&picked)) && SUCCEEDED(picked->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        SetText(kFolder, Suggested(path));
        CheckChoices();
    }
    CoTaskMemFree(path);
    if (picked) picked->Release();
    dialog->Release();
}

// Checks the chosen install folder and creates it; returns why it cannot be used, or nothing.
std::wstring PrepareFolder(const std::wstring& typed) {
    if (typed.empty() || PathIsRelativeW(typed.c_str())) return L"Enter a full folder path, for example C:\\Games\\DcsControl.";
    const std::wstring folder = FullPath(typed);
    if (folder.size() <= 3) return L"Choose a folder, not the top of a drive. For example " + folder.substr(0, 2) + L"\\DcsControl.";
    if (Same(folder, KnownFolder(FOLDERID_Profile)) || Same(folder, KnownFolder(FOLDERID_Documents)) || Same(folder, KnownFolder(FOLDERID_Desktop)))
        return L"Choose a folder of its own for DCS Control, for example " + folder + L"\\DcsControl.";
    for (const KNOWNFOLDERID* id : {&FOLDERID_Windows, &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86})
        if (Within(folder, KnownFolder(*id)))
            return L"Windows only lets administrators install there, and DCS Control installs for your user without administrator rights. Choose a folder in your user folder or on another drive, for example D:\\Games\\DcsControl.";
    if (Exists(folder + L"\\bin\\DCS.exe") || Exists(folder + L"\\DCS.exe") || Exists(folder + L"\\bin-mt\\DCS.exe")) return L"That is DCS's own folder. Choose a separate folder for DCS Control.";
    if (Exists(folder) && !IsFolder(folder)) return L"A file has that name. Choose another folder.";
    if (IsFolder(folder) && !Ours(folder) && !FolderEmpty(folder)) {
        if (Same(Suggested(folder), folder)) return L"That folder already holds other files. Choose an empty folder or a new one for DCS Control.";
        SetText(kFolder, Suggested(folder));
        return L"That folder already holds other files. DCS Control goes into a folder of its own: press Install again to use " + Suggested(folder) + L".";
    }
    if (!IsFolder(folder)) {
        std::wstring top = folder;
        while (!Parent(top).empty() && !IsFolder(Parent(top))) top = Parent(top);
        const int created = SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
        if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS) return L"Windows does not let your user create that folder. Choose another one.";
        if (g_ui.createdFolder.empty()) g_ui.createdFolder = top;
    }
    const std::wstring probe = folder + L"\\.dcs-control-write-test";
    const HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (!g_ui.createdFolder.empty()) { RemoveEmptyFolders(g_ui.createdFolder); g_ui.createdFolder.clear(); }
        return L"Windows does not let your user write into that folder (an antivirus's folder protection can do that too). Choose another one.";
    }
    CloseHandle(file);
    g_ui.options.destination = folder;
    return std::wstring();
}

void Start() {
    if (!g_ui.options.uninstall) {
        const std::wstring problem = PrepareFolder(Text(kFolder));
        if (!problem.empty()) { MessageBoxW(g_ui.window, problem.c_str(), kSetupTitle, MB_OK | MB_ICONWARNING); return; }
        g_ui.options.desktopShortcut = Checked(kDesktop);
        g_ui.options.startMenuShortcut = Checked(kStartMenu);
        Header(g_ui.upgrade ? L"Updating DCS Control" : L"Installing DCS Control", L"Version " DCSC_VERSION_TEXT L" into " + g_ui.options.destination);
    } else {
        Header(L"Removing DCS Control", g_ui.options.destination);
    }
    // Read while the first page shows them: Checked() is false for a hidden control.
    const bool restore = Checked(kRestore), remove = Checked(kDelete);
    g_ui.restoring = restore;
    ShowPage(Page::Work);
    g_ui.worker = std::thread([restore, remove]() {
        if (g_ui.options.uninstall) Uninstall(g_ui.options, g_ui.progress, restore, remove);
        else Install(g_ui.options, g_ui.progress);
        g_ui.progress.done = true;
        PostMessageW(g_ui.window, kDone, 0, 0);
    });
}

void Finish() {
    if (g_ui.worker.joinable()) g_ui.worker.join();
    Progress& progress = g_ui.progress;
    g_ui.result = progress.ok ? 0 : 1;
    const bool uninstall = g_ui.options.uninstall;
    if (progress.ok && g_ui.options.update) {
        // The updater's run: the new version opens straight away.
        if (!g_ui.options.noLaunch) StartApp(g_ui.options.destination);
        DestroyWindow(g_ui.window);
        return;
    }
    if (!progress.ok && !g_ui.createdFolder.empty()) RemoveEmptyFolders(g_ui.createdFolder);
    std::wstring text;
    if (uninstall) {
        Header(progress.ok ? L"DCS Control was removed" : L"DCS Control was not removed", progress.ok ? L"Thank you for flying with it." : L"Nothing more was changed.");
        if (progress.ok) {
            text = L"DCS Control's program files, its shortcuts and its entry in Settings \u2192 Apps are gone.";
            const std::vector<std::wstring> state = StateFolders();
            if (!state.empty()) text += L"\n\nYour profiles, settings and DCS backups are kept in " + state.front() + L"; installing DCS Control again picks them up.";
            if (g_ui.applied > 0 && !g_ui.restoring) text += L"\n\nDCS still has DCS Control's changes; installing DCS Control again and pressing Back to stock DCS undoes them.";
            if (!progress.note.empty()) text += L"\n\n" + progress.note;
        }
    } else {
        const wchar_t* done = g_ui.upgrade ? L"DCS Control is updated" : L"DCS Control is installed";
        const wchar_t* failed = g_ui.options.update || g_ui.upgrade ? L"DCS Control was not updated" : L"DCS Control was not installed";
        Header(progress.ok ? done : failed, L"Version " DCSC_VERSION_TEXT);
        if (progress.ok)
            text = L"DCS Control " DCSC_VERSION_TEXT L" is in " + g_ui.options.destination + L".\n\nOpen it from the Start menu" + (g_ui.options.desktopShortcut ? L" or the desktop shortcut" : L"")
                + L". To remove it, use Settings \u2192 Apps \u2192 DCS Control \u2192 Uninstall.";
    }
    if (!progress.ok) text = progress.error + L"\n\nThe log has the details: " + g_log;
    SetText(kText, text);
    SetText(kPrimary, progress.ok && !uninstall ? L"Finish" : L"Close");
    Check(kLaunch, true);
    ShowPage(Page::Result);
    SetFocus(Item(kPrimary));
}

void Close() {
    if (g_ui.page == Page::Work) return;  // the work is never interrupted halfway
    if (g_ui.page == Page::Result && g_ui.progress.ok && !g_ui.options.uninstall && Checked(kLaunch)) StartApp(g_ui.options.destination);
    // After a failed update (which rolled back) the pilot gets the DCS Control they had back.
    if (g_ui.page == Page::Result && !g_ui.progress.ok && g_ui.options.relaunch && !g_ui.options.noLaunch)
        StartApp(g_ui.options.relaunchPath.empty() ? g_ui.options.destination : Parent(g_ui.options.relaunchPath));
    if (g_ui.page == Page::Choose && !g_ui.createdFolder.empty()) RemoveEmptyFolders(g_ui.createdFolder);
    DestroyWindow(g_ui.window);
}

void Paint(HWND window) {
    PAINTSTRUCT paint;
    const HDC dc = BeginPaint(window, &paint);
    RECT client;
    GetClientRect(window, &client);
    RECT footer = client;
    footer.top = client.bottom - Scale(kFooter);
    FillRect(dc, &footer, g_ui.bar);
    RECT line{0, footer.top, client.right, footer.top + 1};
    const HBRUSH border = CreateSolidBrush(RGB(223, 223, 223));
    FillRect(dc, &line, border);
    line = {0, Scale(kHeader) - 1, client.right, Scale(kHeader)};
    FillRect(dc, &line, border);
    DeleteObject(border);
    SetBkMode(dc, TRANSPARENT);
    const int iconSize = Scale(44);
    if (g_ui.icon) DrawIconEx(dc, client.right - Scale(28) - iconSize, (Scale(kHeader) - iconSize) / 2, g_ui.icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
    RECT title{Scale(28), Scale(14), client.right - Scale(28) - iconSize - Scale(12), Scale(44)};
    SelectObject(dc, g_ui.heading);
    SetTextColor(dc, RGB(0, 51, 102));
    DrawTextW(dc, g_ui.title.c_str(), -1, &title, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT subtitle{Scale(28), Scale(46), title.right, Scale(kHeader) - Scale(6)};
    SelectObject(dc, g_ui.body);
    SetTextColor(dc, RGB(90, 90, 90));
    DrawTextW(dc, g_ui.subtitle.c_str(), -1, &subtitle, DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    EndPaint(window, &paint);
}

LRESULT CALLBACK WindowEvents(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: Paint(window); return 0;
    case WM_ERASEBKGND: {
        RECT client;
        GetClientRect(window, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client, g_ui.white);
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        const HDC dc = reinterpret_cast<HDC>(wParam);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
        SetTextColor(dc, id == kWarning ? RGB(178, 34, 34) : id == kSpace || id == kDetail ? RGB(96, 96, 96) : GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(dc, RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(g_ui.white);
    }
    case WM_CTLCOLORBTN: return reinterpret_cast<LRESULT>(g_ui.bar);
    case DM_GETDEFID: return MAKELRESULT(kPrimary, DC_HASDEFID);
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kPrimary: if (g_ui.page == Page::Choose) Start(); else if (g_ui.page == Page::Result) Close(); break;
        case kSecondary: Close(); break;
        case kBrowse: Browse(); break;
        case kOpenLog: ShellExecuteW(window, L"open", (SystemFolder() + L"\\notepad.exe").c_str(), Quote(g_log).c_str(), nullptr, SW_SHOWNORMAL); break;
        case kRestore: CheckChoices(); break;
        case kFolder: if (HIWORD(wParam) == EN_CHANGE && g_ui.page == Page::Choose) CheckChoices(); break;
        }
        return 0;
    case WM_TIMER:
        if (wParam == kCheckTimer) CheckChoices();
        if (wParam == kProgressTimer) {
            const std::wstring stage = g_ui.progress.Current();
            if (stage != g_ui.shownStage) { g_ui.shownStage = stage; SetText(kStage, stage); }
            std::wstring detail = LogTail(4096, true);
            if (detail.rfind(L"> ", 0) == 0) detail.clear();  // a command line, not a message
            if (detail != g_ui.shownDetail) { g_ui.shownDetail = detail; SetText(kDetail, detail); }
        }
        return 0;
    case kDone: Finish(); return 0;
    case WM_DPICHANGED: {
        g_ui.dpi = HIWORD(wParam);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Fonts();
        Layout();
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case WM_CLOSE: Close(); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

HWND Add(const wchar_t* type, int id, const wchar_t* text, DWORD style, DWORD extended = 0) {
    return CreateWindowExW(extended, type, text, WS_CHILD | style, 0, 0, 10, 10, g_ui.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

int RunWindow(bool startWorking) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW type{sizeof(type)};
    type.lpfnWndProc = WindowEvents;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    type.lpszClassName = L"DcsControlSetup";
    RegisterClassExW(&type);
    g_ui.white = CreateSolidBrush(RGB(255, 255, 255));
    g_ui.bar = CreateSolidBrush(RGB(243, 243, 243));
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    g_ui.window = CreateWindowExW(WS_EX_CONTROLPARENT, type.lpszClassName, g_ui.options.uninstall ? kUninstallTitle : kSetupTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, instance, nullptr);
    if (!g_ui.window) return 1;
    g_ui.dpi = GetDpiForWindow(g_ui.window);

    const DWORD text = SS_LEFT | SS_NOPREFIX, check = BS_AUTOCHECKBOX | WS_TABSTOP | BS_MULTILINE;
    Add(L"STATIC", kText, L"", text);
    Add(L"STATIC", kFolderLabel, L"", text);
    Add(L"EDIT", kFolder, L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE);
    Add(L"BUTTON", kBrowse, L"Browse...", BS_PUSHBUTTON | WS_TABSTOP);
    Add(L"STATIC", kSpace, L"", text);
    Add(L"BUTTON", kStartMenu, L"Add DCS Control to the Start menu", check);
    Add(L"BUTTON", kDesktop, L"Put a DCS Control shortcut on the desktop", check);
    Add(L"BUTTON", kRestore, L"Put DCS back to stock first (recommended)", check);
    Add(L"BUTTON", kDelete, L"", check);
    Add(L"STATIC", kWarning, L"", text);
    Add(PROGRESS_CLASSW, kBar, L"", PBS_MARQUEE);
    Add(L"STATIC", kStage, L"", text | SS_ENDELLIPSIS);
    Add(L"STATIC", kDetail, L"", text);
    Add(L"BUTTON", kLaunch, L"Open DCS Control now", check);
    Add(L"BUTTON", kOpenLog, L"Open the log", BS_PUSHBUTTON | WS_TABSTOP);
    Add(L"BUTTON", kPrimary, L"", BS_DEFPUSHBUTTON | WS_TABSTOP | WS_VISIBLE);
    Add(L"BUTTON", kSecondary, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP | WS_VISIBLE);
    Fonts();

    if (g_ui.options.uninstall) {
        Header(L"Uninstall DCS Control", g_ui.options.destination);
        std::wstring about = L"This removes DCS Control from " + g_ui.options.destination + L": its program files, its Start menu and desktop shortcuts and its entry in Settings \u2192 Apps.";
        if (g_ui.applied > 0) about += L"\n\nDCS Control's changes are still applied to DCS (" + std::to_wstring(g_ui.applied) + L" files). Putting DCS back to stock restores DCS's own files and settings.";
        else if (g_ui.applied == 0) about += L"\n\nDCS has no changes from DCS Control.";
        else about += L"\n\nIf a DCS Control profile is applied to DCS, open DCS Control and press Back to stock DCS first; otherwise DCS keeps its files.";
        SetText(kText, about);
        if (!g_ui.state.empty()) SetText(kDelete, L"Also delete my DCS Control profiles, settings and logs (" + g_ui.state.front() + L")");
        Check(kRestore, true);
        SetText(kPrimary, L"Uninstall");
    } else {
        Header(g_ui.upgrade ? L"Update DCS Control to " DCSC_VERSION_TEXT : L"Install DCS Control " DCSC_VERSION_TEXT,
            g_ui.upgrade ? L"Your profiles and settings are kept." : L"DLSS 5, frame generation and Quad Views for DCS in VR");
        SetText(kText, g_ui.upgrade
            ? L"DCS Control " + (g_ui.existingVersion.empty() ? std::wstring() : g_ui.existingVersion + L" ") + L"is installed in the folder below; this updates it. If the update fails, the version you have now stays."
            : L"DCS Control installs for your Windows user: no administrator rights are needed. DCS itself is not changed until you press Launch DCS in DCS Control, and Back to stock DCS undoes that. "
              L"WebView2 and the Visual C++ runtime are downloaded from Microsoft if this PC lacks them.");
        SetText(kFolderLabel, g_ui.upgrade ? L"Installed in (to move it, uninstall it first):" : L"Install folder:");
        SetText(kFolder, g_ui.options.destination);
        SendMessageW(Item(kFolder), EM_SETREADONLY, g_ui.upgrade, 0);
        EnableWindow(Item(kBrowse), !g_ui.upgrade);
        Check(kStartMenu, g_ui.options.startMenuShortcut);
        Check(kDesktop, g_ui.options.desktopShortcut);
        SetText(kPrimary, g_ui.upgrade ? L"Update" : L"Install");
    }

    // The window's size for its DPI, centred on the monitor under the mouse.
    RECT frame{0, 0, Scale(kWidth), Scale(kHeight)};
    AdjustWindowRectExForDpi(&frame, style, FALSE, WS_EX_CONTROLPARENT, g_ui.dpi);
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const int width = frame.right - frame.left, height = frame.bottom - frame.top;
    SetWindowPos(g_ui.window, nullptr, monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
        monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2, width, height, SWP_NOZORDER);
    if (startWorking) {
        Header(L"Updating DCS Control", L"Version " DCSC_VERSION_TEXT L" into " + g_ui.options.destination);
        ShowPage(Page::Work);
        g_ui.worker = std::thread([]() { Install(g_ui.options, g_ui.progress); g_ui.progress.done = true; PostMessageW(g_ui.window, kDone, 0, 0); });
    } else {
        ShowPage(Page::Choose);
        CheckChoices();
    }
    ShowWindow(g_ui.window, SW_SHOWNORMAL);
    SetForegroundWindow(g_ui.window);
    SetFocus(Item(kPrimary));
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g_ui.window, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_ui.worker.joinable()) g_ui.worker.join();
    return g_ui.result;
}

Options Parse() {
    Options options;
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 1; args && i < count; ++i) {
        const std::wstring arg = args[i];
        auto value = [&]() { return i + 1 < count ? std::wstring(args[++i]) : std::wstring(); };
        if (arg == L"--update") options.update = true;
        else if (arg == L"--quiet") options.quiet = true;
        else if (arg == L"--relaunch") options.relaunch = true;
        else if (arg == L"--no-launch") options.noLaunch = true;
        else if (arg == L"--skip-prerequisites") options.skipPrerequisites = true;
        else if (arg == L"--uninstall") options.uninstall = true;
        else if (arg == L"--relocated") options.relocated = true;
        else if (arg == L"--wait-pid") options.waitPid = static_cast<DWORD>(wcstoul(value().c_str(), nullptr, 10));
        else if (arg == L"--destination") options.destination = FullPath(value());
        else if (arg == L"--start-menu") options.startMenu = FullPath(value());
        else if (arg == L"--desktop") options.desktop = FullPath(value());
        else if (arg == L"--uninstall-key") options.uninstallKey = value();
        else if (arg == L"--relaunch-path") options.relaunchPath = FullPath(value());
    }
    LocalFree(args);
    return options;
}

// The uninstaller lives in the folder it removes: it runs from a copy in %TEMP% instead (which removes itself at the end).
int Relocate(const Options& options) {
    const std::wstring folder = TempFolder() + L"\\DcsControl-Uninstall-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const std::wstring copy = folder + L"\\" + kUninstallerName;
    if (!CreateDirectoryW(folder.c_str(), nullptr) || !CopyFileW(SelfPath().c_str(), copy.c_str(), FALSE)) {
        if (!options.quiet) MessageBoxW(nullptr, L"The uninstaller could not copy itself into the temporary folder. Check the free disk space.", kUninstallTitle, MB_OK | MB_ICONERROR);
        return 1;
    }
    std::wstring command = Quote(copy) + L" --uninstall --relocated --wait-pid " + std::to_wstring(GetCurrentProcessId()) + L" --destination " + Quote(options.destination);
    if (options.quiet) command += L" --quiet";
    if (!options.startMenu.empty()) command += L" --start-menu " + Quote(options.startMenu);
    if (!options.desktop.empty()) command += L" --desktop " + Quote(options.desktop);
    if (!options.uninstallKey.empty()) command += L" --uninstall-key " + Quote(options.uninstallKey);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(copy.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup, &process)) { DeleteTree(folder); return 1; }
    AllowSetForegroundWindow(process.dwProcessId);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}

// The temporary copy's folder goes once this program has exited.
void RemoveSelfLater() {
    const std::wstring folder = Parent(SelfPath());
    std::wstring command = Quote(SystemFolder() + L"\\cmd.exe") + L" /d /c ping -n 3 127.0.0.1 >nul & rd /s /q " + Quote(folder);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (CreateProcessW((SystemFolder() + L"\\cmd.exe").c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, SystemFolder().c_str(), &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}
}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // DLLs loaded later come from System32 only, never from the download folder this setup was started from.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    Options options = Parse();
    Payload payload;
    const bool hasPayload = ReadPayload(payload);
    // The program without its package is the uninstaller, also when double-clicked in the installation.
    if (!hasPayload) {
        options.uninstall = true;
        if (options.destination.empty()) options.destination = Parent(SelfPath());
    }
    g_log = TempFolder() + (options.uninstall ? L"\\DcsControl-Uninstall.log" : L"\\DcsControl-Setup.log");
    if (!options.relocated) DeleteFileW(g_log.c_str());
    Log(std::wstring(options.uninstall ? kUninstallTitle : kSetupTitle) + L": " + GetCommandLineW());
    const wchar_t* title = options.uninstall ? kUninstallTitle : kSetupTitle;

    if (options.uninstall) {
        if (options.destination.empty()) options.destination = ExistingInstallation(options);
        if (!Ours(options.destination)) {
            if (!options.quiet) MessageBoxW(nullptr, (L"There is no DCS Control installation in " + options.destination + L".").c_str(), title, MB_OK | MB_ICONINFORMATION);
            if (options.relocated) RemoveSelfLater();
            return 1;
        }
        if (!options.relocated && Within(SelfPath(), options.destination)) return Relocate(options);
    }
    // One setup at a time: two would install over each other.
    const HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\DcsControl.Setup");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!options.quiet) MessageBoxW(nullptr, L"DCS Control's setup or uninstaller is already running. Finish it first.", title, MB_OK | MB_ICONINFORMATION);
        if (options.relaunch && !options.noLaunch) StartApp(options.relaunchPath.empty() ? ExistingInstallation(options) : Parent(options.relaunchPath));
        if (options.relocated) RemoveSelfLater();
        return 1;
    }
    g_ui.options = options;
    Options& chosen = g_ui.options;
    int result = 0;
    if (chosen.uninstall) {
        g_ui.applied = AppliedFiles(chosen.destination);
        g_ui.state = StateFolders();
        if (chosen.quiet) {
            Uninstall(chosen, g_ui.progress, false, false);
            result = g_ui.progress.ok ? 0 : 1;
        } else {
            result = RunWindow(false);
        }
    } else {
        // An installation already there is updated in place, its shortcuts as the pilot left them.
        const std::wstring existing = ExistingInstallation(chosen);
        g_ui.upgrade = !existing.empty();
        if (chosen.destination.empty()) chosen.destination = g_ui.upgrade ? existing : DefaultDestination();
        if (g_ui.upgrade) {
            chosen.desktopShortcut = Exists(DesktopFolder(chosen) + L"\\DCS Control.lnk");
            chosen.startMenuShortcut = Exists(StartMenuFolder(chosen) + L"\\DCS Control.lnk");
            g_ui.existingVersion = EntryValue(chosen, L"DisplayVersion");
        }
        g_ui.needed = PayloadUnpackedBytes(payload);
        if (chosen.quiet) {
            Install(chosen, g_ui.progress);
            result = g_ui.progress.ok ? 0 : 1;
            if (!chosen.noLaunch && chosen.relaunch)
                StartApp(g_ui.progress.ok || chosen.relaunchPath.empty() ? chosen.destination : Parent(chosen.relaunchPath));
        } else {
            result = RunWindow(chosen.update);
        }
    }
    Log(result == 0 ? L"Done." : result == 2 ? L"Cancelled." : L"Failed.");
    if (single) CloseHandle(single);
    if (chosen.relocated) RemoveSelfLater();
    CoUninitialize();
    return result;
}
