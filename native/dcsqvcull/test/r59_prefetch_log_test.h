// R59 prefetch-log-unbounded: DcsQvCullPrefetch.log (appended to across
// sessions) becomes DcsQvCullPrefetch.prev.log once past its cap, at Install.
#pragma once

namespace r59pflog {

void Run() {
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring dir = std::wstring(tmp) + L"qv_r59_pflog_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
  CreateDirectoryW(dir.c_str(), nullptr);
  const std::wstring log = dir + L"DcsQvCullPrefetch.log", prev = dir + L"DcsQvCullPrefetch.prev.log";
  auto write = [](const std::wstring& p, size_t bytes) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"wb") || !f) return false;
    std::vector<char> b(bytes, 'x');
    const bool ok = fwrite(b.data(), 1, b.size(), f) == b.size();
    fclose(f);
    return ok;
  };
  auto size = [](const std::wstring& p) -> int64_t {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return -1;
    return (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
  };
  const bool names = pffix::PrevLogPath(L"C:\\a.b\\x.log") == L"C:\\a.b\\x.prev.log" &&
                     pffix::PrevLogPath(L"C:\\a.b\\x") == L"C:\\a.b\\x.prev.log";
  // Small: kept as is.
  bool ok = write(log, 1000) && !pffix::RotateLogIfLarge(log, pffix::kLogCap) && size(log) == 1000 && size(prev) < 0;
  // Past the cap: moved aside (an older .prev.log replaced), the next line starts a new file.
  ok = ok && write(prev, 10) && write(log, static_cast<size_t>(pffix::kLogCap) + 1) &&
       pffix::RotateLogIfLarge(log, pffix::kLogCap) && size(log) < 0 &&
       size(prev) == static_cast<int64_t>(pffix::kLogCap) + 1;
  const std::wstring savedPath = pffix::g_logPath;
  pffix::g_logPath = log;
  pffix::LogFileLine("after rotation");
  pffix::g_logPath = savedPath;
  ok = ok && size(log) > 0 && size(log) < 100;
  DeleteFileW(log.c_str());
  DeleteFileW(prev.c_str());
  RemoveDirectoryW(dir.c_str());
  Check(names && ok, "R59 prefetch log: past 1 MB it becomes DcsQvCullPrefetch.prev.log (one kept); small logs stay");
}

}  // namespace r59pflog
