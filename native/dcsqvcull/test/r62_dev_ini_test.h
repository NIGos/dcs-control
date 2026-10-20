// R62 a1-dev-path-maxpath, payload side: [Dev] IniPath of 260+ characters (dev_path.h).
#pragma once

namespace r62dev {

void DevIni() {
  // a1-dev-path-maxpath, payload side: the ini DumpIni (and LoadConfig) reads, with a 260+ character IniPath.
  const std::wstring dir0 = g_dir;
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  g_dir = std::wstring(tmp) + L"qv_r62_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
  CreateDirectoryW(g_dir.c_str(), nullptr);
  const std::wstring ini = g_dir + L"DcsQvCull.ini", dev = g_dir + L"dev.ini";
  FILE* f = nullptr;
  if (_wfopen_s(&f, dev.c_str(), L"w") == 0 && f) fclose(f);
  WritePrivateProfileStringW(L"Dev", L"IniPath", dev.c_str(), ini.c_str());
  const bool shortOk = gbrec::DumpIni() == dev;
  std::wstring longPath = g_dir;
  while (longPath.size() < 280) longPath += L"x";
  longPath += L"\\dev.ini";
  WritePrivateProfileStringW(L"Dev", L"IniPath", longPath.c_str(), ini.c_str());
  const qvdev::DevPath d = qvdev::ReadDevPath(L"IniPath", ini);
  const bool longRefused = gbrec::DumpIni() == ini && d.state == qvdev::kDevTooLong && d.path == longPath;
  DeleteFileW(ini.c_str());
  DeleteFileW(dev.c_str());
  RemoveDirectoryW(g_dir.c_str());
  g_dir = dir0;
  Check(shortOk && longRefused,
        "R62 dev ini: [Dev] IniPath of 260+ characters read whole and refused as too long (deployed ini used)");
}

void Run() {
  DevIni();
}

}  // namespace r62dev
