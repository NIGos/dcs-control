// Built as nvngx_dlss_r122b_probe.dll: its base name starts with "nvngx_dlss", so the filter classifies it as a DLSS
// module. It resolves NvAPI the way the DLSS snippet does (LoadLibrary("nvapi64.dll") + GetProcAddress
// ("nvapi_QueryInterface") at call time) and calls NvAPI_DRS_GetSetting from inside this module. Read-only.
#include <windows.h>
#include <stdint.h>

using QueryFn = void*(__cdecl*)(uint32_t);
using GetSettingFn = int(__cdecl*)(void*, void*, uint32_t, void*);

// Returns the GetSetting status, or -100000 when NvAPI is not reachable. *qiOut / *getSettingOut: what this "snippet"
// resolved (so the test can see it got the hooked pointers).
extern "C" __declspec(dllexport) int __cdecl ProbeGetSetting(void* session, void* profile, uint32_t id, void* setting,
                                                            void** qiOut, void** getSettingOut) {
  HMODULE nv = LoadLibraryW(L"nvapi64.dll");
  if (!nv) return -100000;
  auto qi = reinterpret_cast<QueryFn>(GetProcAddress(nv, "nvapi_QueryInterface"));
  if (!qi) return -100000;
  auto gs = reinterpret_cast<GetSettingFn>(qi(0x73bf8338));  // NvAPI_DRS_GetSetting
  if (qiOut) *qiOut = reinterpret_cast<void*>(qi);
  if (getSettingOut) *getSettingOut = reinterpret_cast<void*>(gs);
  if (!gs) return -100000;
  const int r = gs(session, profile, id, setting);
  FreeLibrary(nv);
  return r;
}
