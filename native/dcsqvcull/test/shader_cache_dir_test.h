// The shader cache's folder and worker count (shader_cache_dir.h): [Model] ShaderCacheDir / ShaderCompileWorkers /
// ShaderCachePrewarm. Pure parts and the folder creation, in a per-process temp folder. The prewarm itself is tested
// with the cache (shadow_inst_test.h, "shadow inst prewarm").
#pragma once

namespace scdtest {

void Run() {
  using namespace qvcachedir;
  const std::wstring mod = L"C:\\sg\\DCS\\Scripts\\DcsQvCull\\";
  Check(Absolute(L"C:\\a\\") && Absolute(L"d:/a") && Absolute(L"\\\\server\\share\\x") && !Absolute(L"cache\\") &&
            !Absolute(L"\\cache") && !Absolute(L"C:cache") && !Absolute(L"") && !Absolute(L"\\\\?\\C:\\a"),
        "shader cache dir: only a drive or UNC path is absolute");
  Check(Normalize(L"  \"C:/state/engine-shader-cache\" ") == L"C:\\state\\engine-shader-cache\\" &&
            Normalize(L"C:\\a\\\\") == L"C:\\a\\" && Normalize(L"C:\\") == L"C:\\" && Normalize(L"").empty(),
        "shader cache dir: quotes and blanks off, forward slashes turned, one trailing backslash");
  {
    const Choice none = Resolve(mod, L"");
    const Choice ok = Resolve(mod, L"C:\\state\\engine-shader-cache");
    const Choice rel = Resolve(mod, L"cache2");
    const Choice up = Resolve(mod, L"C:\\state\\..\\other");
    const Choice lng = Resolve(mod, L"C:\\" + std::wstring(kMaxDirChars, L'x'));
    const Choice noMod = Resolve(L"", L"");
    Check(none.dir == mod + L"cache\\" && !none.configured && !none.why && ok.configured &&
              ok.dir == L"C:\\state\\engine-shader-cache\\" && !ok.why,
          "shader cache dir: the configured folder is used; no key means the module's own cache\\ as before");
    Check(!rel.configured && rel.dir == mod + L"cache\\" && rel.why && !up.configured && up.why && !lng.configured &&
              lng.why && up.dir == mod + L"cache\\" && noMod.dir.empty(),
          "shader cache dir: a relative, .. or too long folder is refused with a reason and falls back");
  }
  Check(AutoWorkers(4) == 2 && AutoWorkers(8) == 2 && AutoWorkers(12) == 3 && AutoWorkers(16) == 4 &&
            AutoWorkers(24) == 6 && AutoWorkers(64) == 6 && AutoWorkers(1) == 2,
        "shader compile workers: a quarter of the logical processors, 2 to 6");
  Check(Workers(0, 16) == 4 && Workers(1, 16) == 1 && Workers(kMaxWorkers, 16) == kMaxWorkers &&
            Workers(kMaxWorkers + 1, 16) == 4 && Workers(-3, 16) == 4 && shadowinst::kMaxWorkers == kMaxWorkers,
        "shader compile workers: [Model] ShaderCompileWorkers 1..8 is taken, anything else is automatic");
  // Folder creation and the ini keys, in this process's temp folder.
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring root = std::wstring(tmp) + L"qv_scd_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
  const std::wstring deep = root + L"state\\engine-shader-cache\\";
  const bool made = CreateTree(deep) && IsDir(deep) && CreateTree(deep);
  // A file where a parent folder should be: not creatable.
  const std::wstring blocker = root + L"file";
  HANDLE h = CreateFileW(blocker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  const bool refused = !CreateTree(blocker + L"\\cache\\") && !CreateTree(L"");
  Check(made && refused, "shader cache dir: the folder is created with its parents; a file in the way is reported");
  const std::wstring ini = root + L"DcsQvCull.ini";
  const bool defaults = ReadConfigured(ini).empty() && ReadWorkers(ini) == 0 && ReadPrewarm(ini);
  std::wstring longDir = L"C:\\";
  while (longDir.size() < 400) longDir += L"abcdefgh\\";
  WritePrivateProfileStringW(L"Model", L"ShaderCacheDir", longDir.c_str(), ini.c_str());
  WritePrivateProfileStringW(L"Model", L"ShaderCompileWorkers", L"3", ini.c_str());
  WritePrivateProfileStringW(L"Model", L"ShaderCachePrewarm", L"0", ini.c_str());
  const bool longWhole = ReadConfigured(ini) == longDir && !Resolve(mod, ReadConfigured(ini)).configured;
  WritePrivateProfileStringW(L"Model", L"ShaderCacheDir", deep.c_str(), ini.c_str());
  const Choice fromIni = Resolve(mod, ReadConfigured(ini));
  Check(defaults && longWhole && fromIni.configured && fromIni.dir == deep && ReadWorkers(ini) == 3 && !ReadPrewarm(ini),
        "shader cache dir: the three [Model] keys are read from the ini (defaults: none, automatic, prewarm on; a 400 "
        "character folder is read whole and refused)");
  // The disk caps follow the cache to its folder, and still cover the module's own cache\.
  {
    const std::wstring m = root + L"mod\\";
    CreateTree(m + L"cache\\");
    auto old = [](const std::wstring& p) {
      HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (f == INVALID_HANDLE_VALUE) return;
      FILETIME t;
      GetSystemTimeAsFileTime(&t);
      ULARGE_INTEGER u;
      u.LowPart = t.dwLowDateTime;
      u.HighPart = t.dwHighDateTime;
      u.QuadPart -= 2ull * 36000000000ull;  // two hours ago
      t.dwLowDateTime = u.LowPart;
      t.dwHighDateTime = u.HighPart;
      SetFileTime(f, nullptr, nullptr, &t);
      CloseHandle(f);
    };
    old(deep + L"s_a.qvc.1.tmp");
    old(m + L"cache\\s_b.qvc.2.tmp");
    old(deep + L"s_keep.qvc");
    const prune::Stats st = prune::RunAtStart(m, m, deep);
    Check(st.files == 2 && GetFileAttributesW((deep + L"s_a.qvc.1.tmp").c_str()) == INVALID_FILE_ATTRIBUTES &&
              GetFileAttributesW((m + L"cache\\s_b.qvc.2.tmp").c_str()) == INVALID_FILE_ATTRIBUTES &&
              GetFileAttributesW((deep + L"s_keep.qvc").c_str()) != INVALID_FILE_ATTRIBUTES,
          "shader cache dir: the disk caps prune the configured folder and the module's own cache\\ (orphaned temp files)");
    DeleteFileW((deep + L"s_keep.qvc").c_str());
    RemoveDirectoryW((m + L"cache\\").c_str());
    RemoveDirectoryW(m.c_str());
  }
  DeleteFileW(ini.c_str());
  DeleteFileW(blocker.c_str());
  RemoveDirectoryW(deep.c_str());
  RemoveDirectoryW((root + L"state\\").c_str());
  RemoveDirectoryW(root.c_str());
}

}  // namespace scdtest
