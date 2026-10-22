// Tests for dcsvr_drs_preset_filter.h against the REAL NvAPI DRS of this machine, read-only.
// Only read functions are ever requested: Initialize, DRS_CreateSession, LoadSettings, GetBaseProfile,
// GetCurrentGlobalProfile, FindApplicationByName, EnumSettings, GetSetting, DestroySession.
// Exit code 0 = all checks passed.
// DCS Control: built by native/pupil_shift/CMakeLists.txt as the ctest "drs_filter" with the probe DLL next to it. Exit
// code 2 (no nvapi64.dll / no NvAPI DRS: no NVIDIA driver) is reported by ctest as skipped, not passed.
#include "dcsvr_drs_preset_filter.h"

#include <stdio.h>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                         \
  do {                                           \
    if (cond) {                                  \
      ++g_pass;                                  \
    } else {                                     \
      ++g_fail;                                  \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                       \
      printf("\n");                              \
    }                                            \
  } while (0)

// NVDRS_SETTING_V1 (nvapi.h R580): 12320 bytes, version MAKE_NVAPI_VERSION(NVDRS_SETTING_V1, 1) = 0x13020.
constexpr size_t kSettingSize = 12320, kSettingU32Current = 8220;
constexpr uint32_t kSettingVer = 0x13020;
// NVDRS_APPLICATION_V4: 20492 bytes, version 0x4500c.
constexpr size_t kAppSize = 20492;
constexpr uint32_t kAppVer = 0x4500c;
// NvApiDriverSettings.h
constexpr uint32_t kOglThreadControl = 0x20C1221E;  // OGL_THREAD_CONTROL_ID ("Threaded optimization")
constexpr uint32_t kSrOverride = 0x10E41E01, kSrMode = 0x10AFB768, kRrPreset = 0x10E41DF7, kFgPreset = 0x10E41DF1;
constexpr int kEndEnum = -7;  // NVAPI_END_ENUMERATION

using QueryFn = dcsvr_drs::QueryFn;
using GetSettingFn = dcsvr_drs::GetSettingFn;
struct Drs {
  int(__cdecl* init)();
  int(__cdecl* createSession)(void**);
  int(__cdecl* destroySession)(void*);
  int(__cdecl* loadSettings)(void*);
  int(__cdecl* getBase)(void*, void**);
  int(__cdecl* getGlobal)(void*, void**);
  int(__cdecl* findApp)(void*, const uint16_t*, void**, void*);
  int(__cdecl* enumSettings)(void*, void*, uint32_t, uint32_t*, void*);
  GetSettingFn getSetting;
};
bool Resolve(QueryFn qi, Drs* d) {
  d->init = reinterpret_cast<int(__cdecl*)()>(qi(0x0150e828));
  d->createSession = reinterpret_cast<int(__cdecl*)(void**)>(qi(0x0694d52e));
  d->destroySession = reinterpret_cast<int(__cdecl*)(void*)>(qi(0xdad9cff8));
  d->loadSettings = reinterpret_cast<int(__cdecl*)(void*)>(qi(0x375dbd6b));
  d->getBase = reinterpret_cast<int(__cdecl*)(void*, void**)>(qi(0xda8466a0));
  d->getGlobal = reinterpret_cast<int(__cdecl*)(void*, void**)>(qi(0x617bff9f));
  d->findApp = reinterpret_cast<int(__cdecl*)(void*, const uint16_t*, void**, void*)>(qi(0xeee566b2));
  d->enumSettings = reinterpret_cast<int(__cdecl*)(void*, void*, uint32_t, uint32_t*, void*)>(qi(0xae3039da));
  d->getSetting = reinterpret_cast<GetSettingFn>(qi(0x73bf8338));
  return d->init && d->createSession && d->destroySession && d->loadSettings && d->getBase && d->getGlobal &&
         d->findApp && d->enumSettings && d->getSetting;
}

struct Read {
  int status = 0;
  std::vector<uint8_t> buf;
  uint32_t u32() const { uint32_t v; memcpy(&v, buf.data() + kSettingU32Current, 4); return v; }
  bool operator==(const Read& o) const { return status == o.status && buf == o.buf; }
};
Read DoRead(GetSettingFn gs, void* s, void* p, uint32_t id) {
  Read r;
  r.buf.assign(kSettingSize, 0);
  memcpy(r.buf.data(), &kSettingVer, 4);
  r.status = gs(s, p, id, r.buf.data());
  return r;
}
using ProbeFn = int(__cdecl*)(void*, void*, uint32_t, void*, void**, void**);
ProbeFn g_probe = nullptr;
Read ProbeRead(void* s, void* p, uint32_t id, void** qi = nullptr, void** gs = nullptr) {
  Read r;
  r.buf.assign(kSettingSize, 0);
  memcpy(r.buf.data(), &kSettingVer, 4);
  r.status = g_probe(s, p, id, r.buf.data(), qi, gs);
  return r;
}
std::string Desc(const Read& r) {
  char b[96];
  if (r.status == 0) snprintf(b, sizeof(b), "OK value 0x%X", r.u32());
  else snprintf(b, sizeof(b), "status %d%s", r.status, r.status == -160 ? " (SETTING_NOT_FOUND)" : "");
  return b;
}

// Gate controlled by the test.
std::atomic<bool> g_gateOn{false};
bool TestGate(void*) { return g_gateOn.load(); }
bool FaultingGate(void*) {
  volatile int* p = nullptr;
  return *p == 1;  // access violation
}
bool ThrowingGate(void*) { throw 42; }

void SetGate(dcsvr_drs::GatePredicate g, bool requireDlss, const wchar_t* ini = nullptr) {
  dcsvr_drs::Config c;
  c.gate = g;
  c.iniPath = ini;
  c.requireDlssCaller = requireDlss;
  c.publishHiddenEnv = true;
  dcsvr_drs::SetConfig(c);
}

struct FileStamp {
  std::wstring name;
  FILETIME t{};
  uint64_t size = 0;
};
std::vector<FileStamp> DrsStamps() {
  std::vector<FileStamp> v;
  const wchar_t* dir = L"C:\\ProgramData\\NVIDIA Corporation\\Drs\\";
  for (const wchar_t* n : {L"nvdrsdb0.bin", L"nvdrsdb1.bin", L"nvdrssel.bin"}) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    FileStamp f;
    f.name = n;
    if (GetFileAttributesExW((std::wstring(dir) + n).c_str(), GetFileExInfoStandard, &a)) {
      f.t = a.ftLastWriteTime;
      f.size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    }
    v.push_back(f);
  }
  return v;
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  printf("dcsvr_drs_preset_filter test (real NvAPI DRS, read-only)\n");
  const auto stampsBefore = DrsStamps();

  // ---- 0. pure helpers -----------------------------------------------------------------------------------------------
  printf("[0] helpers\n");
  CHECK(dcsvr_drs::IsFilteredSettingId(0x10E41DF3), "SR preset must be filtered");
  for (uint32_t id : {kOglThreadControl, kSrOverride, kSrMode, kRrPreset, kFgPreset, 0u, 0xFFFFFFFFu})
    CHECK(!dcsvr_drs::IsFilteredSettingId(id), "0x%X must not be filtered", id);
  CHECK(sizeof(dcsvr_drs::kFilteredSettingIds) / sizeof(uint32_t) == 1, "exactly one filtered id");
  CHECK(dcsvr_drs::IsDlssModulePath(L"E:\\DCS World\\bin-mt\\nvngx_dlss.dll"), "DCS's snippet");
  CHECK(dcsvr_drs::IsDlssModulePath(L"C:/X/NVNGX_DLSSG.DLL"), "upper case, forward slashes");
  CHECK(dcsvr_drs::IsDlssModulePath(
            L"C:\\ProgramData\\NVIDIA\\NGX\\models\\dlss\\versions\\20316928\\files\\160_E658703.bin"),
        "OTA copy");
  CHECK(!dcsvr_drs::IsDlssModulePath(L"C:\\Windows\\System32\\DriverStore\\FileRepository\\x\\_nvngx.dll"),
        "NGX core is not DLSS");
  CHECK(!dcsvr_drs::IsDlssModulePath(L"E:\\DCS World\\bin-mt\\DCS.exe"), "DCS.exe");
  CHECK(!dcsvr_drs::IsDlssModulePath(L"C:\\x\\my_nvngx_dlss.dll"), "prefix must be at the base name");
  CHECK(!dcsvr_drs::IsDlssModulePath(L""), "empty");
  CHECK(dcsvr_drs::ParsePreset(L"E") == 5 && dcsvr_drs::ParsePreset(L"m") == 13 && dcsvr_drs::ParsePreset(L" 11 ") == 11,
        "presets");
  CHECK(dcsvr_drs::ParsePreset(L"0") == 0 && dcsvr_drs::ParsePreset(L"") == 0 && dcsvr_drs::ParsePreset(L"G") == -1 &&
            dcsvr_drs::ParsePreset(L"7") == -1 && dcsvr_drs::ParsePreset(L"EE") == -1,
        "non-presets");

  // ---- 1. original NvAPI (before install) -------------------------------------------------------------------------
  HMODULE nv = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!nv) {
    printf("nvapi64.dll not available: cannot test\n");
    return 2;
  }
  const auto origQi = reinterpret_cast<QueryFn>(GetProcAddress(nv, "nvapi_QueryInterface"));
  Drs orig{};
  if (!origQi || !Resolve(origQi, &orig) || orig.init() != 0) {
    printf("NvAPI DRS not available: cannot test\n");
    return 2;
  }
  printf("[1] original nvapi_QueryInterface %p, GetSetting %p\n", (void*)origQi, (void*)orig.getSetting);

  // ---- 2. install --------------------------------------------------------------------------------------------------
  printf("[2] install\n");
  // The export table page's protection before the patch (DCS Control: it must be the same afterwards).
  DWORD eatProtectBefore = 0;
  {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(nv);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(nv) + dos->e_lfanew);
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        reinterpret_cast<const uint8_t*>(nv) + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    MEMORY_BASIC_INFORMATION m{};
    if (VirtualQuery(reinterpret_cast<const uint8_t*>(nv) + exp->AddressOfFunctions, &m, sizeof(m))) eatProtectBefore = m.Protect;
  }
  dcsvr_drs::Config cfg;  // gate off until the test sets one
  const dcsvr_drs::InstallResult ir = dcsvr_drs::Install(cfg);
  {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(nv);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(nv) + dos->e_lfanew);
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        reinterpret_cast<const uint8_t*>(nv) + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    MEMORY_BASIC_INFORMATION m{};
    CHECK(VirtualQuery(reinterpret_cast<const uint8_t*>(nv) + exp->AddressOfFunctions, &m, sizeof(m)) &&
              m.Protect == eatProtectBefore,
          "export table protection restored (0x%lX, was 0x%lX)", m.Protect, eatProtectBefore);
    MEMORY_BASIC_INFORMATION t{};
    CHECK(ir.thunk && VirtualQuery(ir.thunk, &t, sizeof(t)) && t.Protect == PAGE_EXECUTE_READ, "thunk page executable, not writable");
  }
  printf("    status %d (%s), thunk %p, original %p, ngx core loaded %d, dlss loaded %d\n", ir.status,
         dcsvr_drs::StatusText(ir.status), ir.thunk, ir.originalQueryInterface, ir.ngxCoreLoadedAtInstall,
         ir.dlssLoadedAtInstall);
  CHECK(ir.status == dcsvr_drs::kInstalled, "install status %d", ir.status);
  CHECK(ir.originalQueryInterface == (void*)origQi, "original pointer recorded");
  CHECK(!ir.ngxCoreLoadedAtInstall && !ir.dlssLoadedAtInstall, "nothing NGX loaded in the test");
  const auto hookedQi = reinterpret_cast<QueryFn>(GetProcAddress(nv, "nvapi_QueryInterface"));
  CHECK(hookedQi == ir.thunk && hookedQi != origQi, "GetProcAddress now returns the thunk (%p)", (void*)hookedQi);
  CHECK((uintptr_t)ir.thunk > (uintptr_t)nv && (uintptr_t)ir.thunk - (uintptr_t)nv < 0xFFFFFFFFull, "thunk in RVA range");
  const auto ir2 = dcsvr_drs::Install(cfg);
  CHECK(ir2.status == dcsvr_drs::kInstalled && ir2.thunk == ir.thunk, "second Install: same result, no second patch");
  CHECK(GetProcAddress(nv, "nvapi_QueryInterface") == ir.thunk, "still one thunk");
  // A static-import-like rebind (LoadLibrary by name, as DLSS does) sees the same thunk.
  HMODULE nv2 = LoadLibraryW(L"nvapi64.dll");
  CHECK(nv2 == nv && GetProcAddress(nv2, "nvapi_QueryInterface") == ir.thunk, "LoadLibrary by name: same module");
  if (nv2) FreeLibrary(nv2);

  // Hooked QueryInterface: GetSetting wrapped, every other id identical to the original.
  Drs hk{};
  CHECK(Resolve(hookedQi, &hk), "resolve through the hook");
  CHECK((void*)hk.getSetting == dcsvr_drs::GetSettingHookAddress(), "GetSetting is wrapped");
  CHECK(hk.init == orig.init && hk.createSession == orig.createSession && hk.loadSettings == orig.loadSettings &&
            hk.getBase == orig.getBase && hk.getGlobal == orig.getGlobal && hk.findApp == orig.findApp &&
            hk.enumSettings == orig.enumSettings && hk.destroySession == orig.destroySession,
        "every other read function passes through unchanged");
  for (uint32_t id : {0xd22bdd7eu /*Unload*/, 0x12345678u /*unknown*/, 0x0150e828u})
    CHECK(hookedQi(id) == origQi(id), "QI(0x%X) identical", id);

  // ---- 3. real session -----------------------------------------------------------------------------------------------
  void* s = nullptr;
  CHECK(hk.createSession(&s) == 0 && s, "create session");
  CHECK(hk.loadSettings(s) == 0, "load settings");
  void* base = nullptr;
  void* global = nullptr;
  void* app = nullptr;
  CHECK(hk.getBase(s, &base) == 0 && base, "base profile");
  CHECK(hk.getGlobal(s, &global) == 0 && global, "global profile");
  {
    std::vector<uint8_t> a(kAppSize, 0);
    memcpy(a.data(), &kAppVer, 4);
    uint16_t name[2048] = {};
    const wchar_t* n = L"DCS.exe";
    for (int i = 0; n[i]; ++i) name[i] = n[i];
    const int r = hk.findApp(s, name, &app, a.data());
    printf("    DCS.exe application profile: %s\n", r == 0 && app ? "found" : "not found (status only)");
    if (r != 0) app = nullptr;
  }
  struct Prof {
    const char* name;
    void* h;
  };
  std::vector<Prof> profs = {{"base", base}, {"global", global}};
  if (app) profs.push_back({"DCS.exe", app});

  // Every setting each profile has (EnumSettings, read-only) plus a fixed list.
  auto settingIds = [&](void* p) {
    std::vector<uint32_t> ids = {kOglThreadControl, 0x10E41DF3, kSrOverride, kSrMode, kRrPreset, kFgPreset};
    std::vector<uint8_t> arr(kSettingSize * 32);
    for (uint32_t start = 0;;) {
      for (int i = 0; i < 32; ++i) {
        memset(arr.data() + i * kSettingSize, 0, kSettingSize);
        memcpy(arr.data() + i * kSettingSize, &kSettingVer, 4);
      }
      uint32_t count = 32;
      const int r = hk.enumSettings(s, p, start, &count, arr.data());
      if (r != 0 || count == 0) break;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t id;
        memcpy(&id, arr.data() + i * kSettingSize + 4 + 4096, 4);  // settingId follows version + settingName
        bool dup = false;
        for (uint32_t x : ids) dup |= x == id;
        if (!dup) ids.push_back(id);
      }
      start += count;
      if (count < 32) break;
    }
    return ids;
  };

  printf("[3] non-DLSS reads: original vs filter off vs filter on (any caller), every setting of every profile\n");
  size_t compared = 0, presetChecks = 0;
  for (const Prof& p : profs) {
    const auto ids = settingIds(p.h);
    for (uint32_t id : ids) {
      const Read a = DoRead(orig.getSetting, s, p.h, id);
      SetGate(nullptr, false);  // gate off
      const Read b = DoRead(hk.getSetting, s, p.h, id);
      g_gateOn = true;
      SetGate(&TestGate, false);  // gate on, any caller
      const Read c = DoRead(hk.getSetting, s, p.h, id);
      g_gateOn = false;
      if (id == 0x10E41DF3) {
        ++presetChecks;
        CHECK(b == a, "%s: preset with gate off must equal the original", p.name);
        CHECK(c.status == -160, "%s: preset with gate on must be NOT_FOUND (got %d)", p.name, c.status);
        printf("    %-8s 0x10E41DF3 DLSS-SR preset: original %s | off %s | on %s\n", p.name, Desc(a).c_str(),
               Desc(b).c_str(), Desc(c).c_str());
      } else {
        ++compared;
        CHECK(a == b && b == c, "%s: 0x%X differs (orig %d off %d on %d)", p.name, id, a.status, b.status, c.status);
        if (id == kOglThreadControl)
          printf("    %-8s 0x%08X threaded optimization: original %s | off %s | on %s\n", p.name, id, Desc(a).c_str(),
                 Desc(b).c_str(), Desc(c).c_str());
      }
    }
  }
  printf("    %zu non-filtered (profile, id) reads compared byte-for-byte; %zu preset reads\n", compared, presetChecks);
  CHECK(compared >= 10, "enough settings compared");

  // ---- 4. caller check with the real DLSS-named probe ---------------------------------------------------------------
  printf("[4] caller check (requireDlssCaller = true)\n");
  HMODULE probe = LoadLibraryW(L"nvngx_dlss_r122b_probe.dll");
  g_probe = probe ? reinterpret_cast<ProbeFn>(GetProcAddress(probe, "ProbeGetSetting")) : nullptr;
  CHECK(g_probe != nullptr, "probe DLL loaded");
  void* presetProf = app ? app : base;
  const Read truth = DoRead(orig.getSetting, s, presetProf, 0x10E41DF3);
  const Read truthOgl = DoRead(orig.getSetting, s, presetProf, kOglThreadControl);
  if (g_probe) {
    g_gateOn = false;
    SetGate(&TestGate, true);
    void *pq = nullptr, *pg = nullptr;
    const Read off = ProbeRead(s, presetProf, 0x10E41DF3, &pq, &pg);
    CHECK(pq == ir.thunk && pg == dcsvr_drs::GetSettingHookAddress(), "the probe resolved the hooked pointers");
    CHECK(off == truth, "probe, gate off: real preset");
    g_gateOn = true;
    const Read on = ProbeRead(s, presetProf, 0x10E41DF3);
    CHECK(on.status == -160, "probe (DLSS module), gate on: NOT_FOUND (got %d)", on.status);
    const Read exeOn = DoRead(hk.getSetting, s, presetProf, 0x10E41DF3);
    CHECK(exeOn == truth, "test exe (not DLSS), gate on: real preset");
    const Read oglOn = ProbeRead(s, presetProf, kOglThreadControl);
    CHECK(oglOn == truthOgl, "probe, gate on, threaded optimization: real value");
    printf("    probe preset: gate off %s | gate on %s; exe gate on %s; probe threaded opt gate on %s\n",
           Desc(off).c_str(), Desc(on).c_str(), Desc(exeOn).c_str(), Desc(oglOn).c_str());
    wchar_t env[8] = {};
    CHECK(GetEnvironmentVariableW(L"DCSQV_DLSS_DRIVER_PRESET_HIDDEN", env, 8) && env[0] == L'1',
          "hidden env published");
    g_gateOn = false;
  }

  // ---- 5. ini gate ---------------------------------------------------------------------------------------------------
  printf("[5] ini gate ([DLSS] PeripheryPreset set AND IgnoreDriverPreset nonzero)\n");
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring ini = std::wstring(tmp) + L"dcsvr_drs_filter_test.ini";
  struct IniCase {
    const char* text;
    bool hide;
  } cases[] = {
      {"[DLSS]\r\nPeripheryPreset=E\r\nIgnoreDriverPreset=1\r\n", true},
      {"[dlss]\r\nperipherypreset = m\r\nignoredriverpreset=1\r\n", true},
      {"[DLSS]\r\nPeripheryPreset=13\r\nIgnoreDriverPreset=1\r\n", true},
      {"[DLSS]\r\nPeripheryPreset=0\r\nIgnoreDriverPreset=1\r\n", false},
      {"[DLSS]\r\nPeripheryPreset=E\r\nIgnoreDriverPreset=0\r\n", false},
      {"[DLSS]\r\nPeripheryPreset=E\r\n", false},
      {"[DLSS]\r\nIgnoreDriverPreset=1\r\n", false},
      {"[DLSS]\r\nPeripheryPreset=G\r\nIgnoreDriverPreset=1\r\n", false},
      {"[Other]\r\nPeripheryPreset=E\r\nIgnoreDriverPreset=1\r\n", false},
      {nullptr, false},  // file missing
  };
  for (const IniCase& k : cases) {
    DeleteFileW(ini.c_str());
    if (k.text) {
      HANDLE h = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
      DWORD w = 0;
      WriteFile(h, k.text, (DWORD)strlen(k.text), &w, nullptr);
      CloseHandle(h);
    }
    SetGate(nullptr, true, ini.c_str());
    CHECK(dcsvr_drs::IniGateOn(ini.c_str()) == k.hide, "IniGateOn: %s", k.text ? k.text : "(missing)");
    if (g_probe) {
      const Read r = ProbeRead(s, presetProf, 0x10E41DF3);
      CHECK(k.hide ? r.status == -160 : r == truth, "ini case %s: probe got %d", k.text ? k.text : "(missing)",
            r.status);
      const Read o = ProbeRead(s, presetProf, kOglThreadControl);
      CHECK(o == truthOgl, "ini case: threaded optimization untouched");
    }
    const Read e = DoRead(hk.getSetting, s, presetProf, 0x10E41DF3);
    CHECK(e == truth, "ini case: exe caller always real");
  }
  DeleteFileW(ini.c_str());
  printf("    %zu ini cases\n", sizeof(cases) / sizeof(cases[0]));

  // ---- 6. SEH: faulting / throwing gates pass through -------------------------------------------------------------
  printf("[6] faulting gates\n");
  {
    const uint32_t f0 = dcsvr_drs::GetStats().gateFaults;
    SetGate(&FaultingGate, false);
    const Read r1 = DoRead(hk.getSetting, s, presetProf, 0x10E41DF3);
    SetGate(&ThrowingGate, false);
    const Read r2 = DoRead(hk.getSetting, s, presetProf, 0x10E41DF3);
    CHECK(r1 == truth && r2 == truth, "faulting gate: pass-through");
    CHECK(dcsvr_drs::GetStats().gateFaults == f0 + 2, "two faults counted");
    printf("    access violation and C++ throw in the gate: both passed through, faults counted %u\n",
           dcsvr_drs::GetStats().gateFaults - f0);
  }

  // ---- 7. threads: concurrent reads while the gate flips ----------------------------------------------------------
  printf("[7] 6 threads x 400 reads while another thread flips the gate and swaps configs\n");
  {
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0}, done{0};
    SetGate(&TestGate, false);
    std::thread flipper([&] {
      int i = 0;
      while (!stop.load()) {
        g_gateOn = (i & 1) != 0;
        if ((i % 7) == 0) SetGate(&TestGate, false);
        ++i;
        Sleep(0);
      }
    });
    std::vector<std::thread> ts;
    for (int t = 0; t < 6; ++t)
      ts.emplace_back([&] {
        void* ts_s = nullptr;
        void* ts_p = nullptr;
        if (hk.createSession(&ts_s) != 0 || hk.loadSettings(ts_s) != 0 || hk.getBase(ts_s, &ts_p) != 0) {
          bad += 1000;
          return;
        }
        const Read tOgl = DoRead(orig.getSetting, ts_s, ts_p, kOglThreadControl);
        const Read tPre = DoRead(orig.getSetting, ts_s, ts_p, 0x10E41DF3);
        for (int i = 0; i < 400; ++i) {
          if (!(DoRead(hk.getSetting, ts_s, ts_p, kOglThreadControl) == tOgl)) ++bad;
          const Read p = DoRead(hk.getSetting, ts_s, ts_p, 0x10E41DF3);
          if (!(p == tPre) && p.status != -160) ++bad;
        }
        hk.destroySession(ts_s);
        ++done;
      });
    for (auto& t : ts) t.join();
    stop = true;
    flipper.join();
    CHECK(bad.load() == 0 && done.load() == 6, "threads: %d bad results, %d done", bad.load(), done.load());
    g_gateOn = false;
  }

  hk.destroySession(s);

  // ---- 8. nothing written ---------------------------------------------------------------------------------------------
  printf("[8] no writes\n");
  const dcsvr_drs::Stats st = dcsvr_drs::GetStats();
  size_t wrapped = 0;
  for (size_t i = 0; i < dcsvr_drs::kHookTableSize; ++i) {
    const auto& e = dcsvr_drs::kHookTable[i];
    if (e.treatment == dcsvr_drs::kWrapped) {
      ++wrapped;
      CHECK(e.qiId == 0x73bf8338, "the only wrapped id is GetSetting");
    } else {
      CHECK(st.tableHits[i] == 0, "%s was requested %u times", e.name, st.tableHits[i]);
    }
  }
  CHECK(wrapped == 1, "exactly one wrapped function");
  CHECK(st.getSettingMismatch == 0, "GetSetting pointer stable");
  const auto stampsAfter = DrsStamps();
  for (size_t i = 0; i < stampsBefore.size(); ++i)
    CHECK(CompareFileTime(&stampsBefore[i].t, &stampsAfter[i].t) == 0 && stampsBefore[i].size == stampsAfter[i].size,
          "%ls changed", stampsBefore[i].name.c_str());
  printf("    hook table: %zu write ids, 0 requested by anyone in the process; DRS database files unchanged\n",
         dcsvr_drs::kHookTableSize - wrapped);
  printf("    stats: qi %u, GetSetting %u, filtered-id reads %u, hidden %u, gate faults %u\n", st.qiCalls,
         st.getSettingCalls, st.filteredIdReads, st.hiddenReads, st.gateFaults);

  printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
