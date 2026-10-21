// R122 (docs/research/R122_dlss_per_view_preset.md): an OPTIONAL per-view DLSS preset. LOSSY, opt-in, off by default.
//
// DCS creates four DLSS-SR features per mission load (periphery 604x596->1040x1028 twice, then focus
// 1176x1164->2028x2004 twice with the user's Quad Views settings) and gives all four the same render-preset hint. Keys:
//   [DLSS] PeripheryPreset = 0 (default: nothing installed, DCS's DLSS untouched) | a preset letter (A-F, J-M) | its NGX
//          number (E = 5, K = 11, M = 13). The periphery features get that preset. R121 measured E (the last CNN
//          preset) at about half the cost of M/K in the periphery: -0.31 ms per frame with the focus left at M.
//   [DLSS] FocusPreset = 0 (default: the focus features keep what DCS / the driver give them) | a letter / number as above.
//          Honoured only while PeripheryPreset is set.
//   [DLSS] IgnoreDriverPreset = 0 (default) | 1: while PeripheryPreset is set, hide the NVIDIA driver profile's forced
//          DLSS-SR preset (NVIDIA App > DLSS Override - Model presets) from DLSS inside DCS, and give every feature an
//          explicit preset instead (periphery: PeripheryPreset; focus: FocusPreset, else the driver's own preset). ONLY
//          POSSIBLE WHEN DcsQvCull STARTS BEFORE DCS INITIALISES NGX (see "Driver override" below); with today's Lua-hook
//          load order it never is, and the key only logs why. Never writes the driver profile.
//
// Hook point. The driver's NGX core (_nvngx.dll, loaded by dx11backend's static NGX loader through the NGXCore registry
// path) exports NVSDK_NGX_D3D11_CreateFeature. We detour that export inline (14-byte absolute jump to a persistent
// stub, the pose_sweep.h / alloc_slab.h scheme: code and target pointer on separate pages, the patch written with every
// other thread suspended, an earlier payload's patch found again and reused after a hot reload). In the detour, for
// NVSDK_NGX_Feature_SuperSampling only: read OutWidth/OutHeight through the parameter object's virtual Get, classify
// the view by its output size, Set the six DLSS.Hint.Render.Preset.* values for a class that has a preset, call the
// original, log one line. The core is the same whichever nvngx_dlss.dll it then loads (DCS's 310.2.1 or the driver's
// OTA 310.9.16 under the "latest DLL" override), so the DLL swap does not matter.
//
// View identification. Quad Views (QVFR) logs "Recommended peripheral resolution: WxH" and "Recommended focus
// resolution: WxH" in %LOCALAPPDATA%\Quad-Views-Foveated\Quad-Views-Foveated.log when DCS's OpenXR session starts.
// DCS's DLSS output size is the view size, so a feature whose output matches the periphery size (within 2 px) is
// periphery and one matching the focus size is focus. The sizes are used only when the QVFR layer is loaded in this
// process and the log was written since this process started; anything else (no QVFR, stereo VR, 2D, a size that
// matches neither, sizes that do not make sense) leaves the feature untouched and is logged.
//
// When it applies. DCS never releases its DLSS features (R82), so a change applies at the NEXT MISSION LOAD (when DCS
// creates its four features). The kill switch, a key set back to 0 and a payload stop make the hook pass-through: DCS's
// own hint again from the next mission load.
//
// Driver override (R121 1.1, R122 3; measured with the R122 harness run as DCS.exe). The DLSS snippet (nvngx_dlss.dll,
// or the driver's OTA copy) reads the DRS setting NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION (0x10E41DF3) itself,
// through NvAPI (nvapi_QueryInterface -> NvAPI_DRS_GetSetting, implemented in nvapi64_impl.dll), ONCE, inside
// NVSDK_NGX_D3D11_Init, and caches it for every feature of the process. A value other than off / default wins over the
// app hint ("Using DRS Overridden Preset M"): with the user's NVIDIA App "DLSS Override - Model presets = M" for DCS,
// our hint alone is ignored. DCS initialises NGX at renderer start, about 25 s before the Lua hook loads DcsQvCull. So:
//   - the driver profile is READ (read-only NvAPI DRS: session, load, find application, get setting) once, and every
//     creation line says whether it forces a preset; a one-time warning tells the user where to change it.
//   - IgnoreDriverPreset=1 can only act if it is in place before NGX initialises: then it detours NvAPI_DRS_GetSetting's
//     implementation inline (same scheme), and a read of 0x10E41DF3 BY A DLSS MODULE (the caller's module is the
//     snippet: nvngx_dlss*.dll or a file under an NGX models folder) returns NVAPI_SETTING_NOT_FOUND (what a profile
//     without the setting returns). Every other read, of any setting, by any other module, is passed through unchanged.
//     Since the snippet then ignores the driver for all four features, the CreateFeature hook makes the driver's
//     preset explicit on every feature we have no other preset for (the focus with FocusPreset=0: still M). Nothing is
//     written to the driver profile, no NvAPI_DRS_Set*/Save* is ever called, nothing persists past the process; the
//     "latest DLL" override (0x10E41E01, read by the NGX core) is not touched. When NGX is already up as the payload
//     starts (today: always), nothing is installed and the reason is logged.
//
// Safety. Everything we do inside DCS's call runs under SEH; a fault leaves the feature as DCS asked. An unknown NGX
// core / NvAPI build (a prologue our decoder does not accept, a foreign patch), a missing module or export: nothing
// installed, logged once. Keys 0: nothing installed at all.
//
// Included once from main.cpp inside its anonymous namespace (features.inc); the R122 harness includes it standalone
// (it then supplies Log and patch_threads.h).
// Needs (included by main.cpp before its anonymous namespace): <string>, <vector>, <atomic>, <mutex>, <intrin.h>.
#pragma once

namespace dlssv {

// ---- NGX / NvAPI constants (the DLSS SDK 310.9.16 headers and NvApiDriverSettings.h) ------------------------------
constexpr int kFeatureSuperSampling = 1;       // NVSDK_NGX_Feature_SuperSampling
constexpr int kNgxSuccess = 1;                 // NVSDK_NGX_Result_Success
constexpr uint32_t kDrsSrPreset = 0x10E41DF3;  // NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION_ID (read by the snippet)
constexpr uint32_t kDrsSrOverride = 0x10E41E01;  // NGX_DLSS_SR_OVERRIDE_ID (the "latest DLL" swap, read by the core)
constexpr uint32_t kDrsPresetOff = 0, kDrsPresetDefault = 0x00fffffe, kDrsPresetLatest = 0x00ffffff;
constexpr int32_t kNvapiOk = 0, kNvapiSettingNotFound = -160;
// nvapi_QueryInterface ids (nvapi_interface.h)
constexpr uint32_t kQiInitialize = 0x0150e828, kQiCreateSession = 0x0694d52e, kQiLoadSettings = 0x375dbd6b,
                   kQiFindApplicationByName = 0xeee566b2, kQiGetSetting = 0x73bf8338, kQiGetBaseProfile = 0xda8466a0,
                   kQiDestroySession = 0xdad9cff8;
// NVDRS_SETTING_V1 / NVDRS_APPLICATION_V4 (nvapi.h R580 SDK): sizes, versions, offsets.
constexpr size_t kSettingSize = 12320, kSettingLocation = 4108, kSettingU32Current = 8220;
constexpr uint32_t kSettingVer = 0x13020;
constexpr size_t kAppSize = 20492;
constexpr uint32_t kAppVer = 0x4500c;
const char* const kHintKeys[6] = {"DLSS.Hint.Render.Preset.DLAA",        "DLSS.Hint.Render.Preset.Quality",
                                  "DLSS.Hint.Render.Preset.Balanced",    "DLSS.Hint.Render.Preset.Performance",
                                  "DLSS.Hint.Render.Preset.UltraPerformance", "DLSS.Hint.Render.Preset.UltraQuality"};
const wchar_t* const kNgxCore = L"_nvngx.dll";
const wchar_t* const kQvfrModule = L"XR_APILAYER_MBUCCHIA_quad_views_foveated.dll";

// NVSDK_NGX_Parameter's ABI (nvsdk_ngx_params.h): the same virtual declarations in the same order, so MSVC gives it
// the same vtable as the SDK's (DCS and the core are MSVC builds too). D3D11Res / D3D12Res stand for ID3D11Resource /
// ID3D12Resource (only pointers to them pass; the types matter only for telling the overloads apart).
struct D3D11Res;
struct D3D12Res;
struct NgxParam {
  virtual void Set(const char* name, unsigned long long v) = 0;
  virtual void Set(const char* name, float v) = 0;
  virtual void Set(const char* name, double v) = 0;
  virtual void Set(const char* name, unsigned int v) = 0;
  virtual void Set(const char* name, int v) = 0;
  virtual void Set(const char* name, D3D11Res* v) = 0;
  virtual void Set(const char* name, D3D12Res* v) = 0;
  virtual void Set(const char* name, void* v) = 0;
  virtual int Get(const char* name, unsigned long long* v) const = 0;
  virtual int Get(const char* name, float* v) const = 0;
  virtual int Get(const char* name, double* v) const = 0;
  virtual int Get(const char* name, unsigned int* v) const = 0;
  virtual int Get(const char* name, int* v) const = 0;
  virtual int Get(const char* name, D3D11Res** v) const = 0;
  virtual int Get(const char* name, D3D12Res** v) const = 0;
  virtual int Get(const char* name, void** v) const = 0;
  virtual void Reset() = 0;
};

using CreateFn = int(__cdecl*)(void* ctx, int feature, NgxParam* params, void** handle);
using GetSettingFn = int(__cdecl*)(void* session, void* profile, uint32_t id, void* setting);
using QueryFn = void*(__cdecl*)(uint32_t id);

// ---- presets ---------------------------------------------------------------------------------------------------
inline const char* PresetName(uint32_t v) {
  static const char* const n[] = {"default", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                  "N",       "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
  if (v <= 26) return n[v];
  if (v == kDrsPresetDefault) return "default";
  if (v == kDrsPresetLatest) return "latest";
  return "?";
}
// The presets a key may ask for: the CNN presets A-F (A-D exist only in DCS's 310.2.1; 310.9 falls back to its default
// for them) and the transformers J-M. G-I and N-O are "do not use" in the SDK.
inline bool PresetAllowed(int v) { return (v >= 1 && v <= 6) || (v >= 10 && v <= 13); }
// An ini value: empty or "0" = 0 (off); one letter (either case) or the NGX number of an allowed preset; anything else
// -1 (not usable: treated as off and logged).
inline int ParsePreset(const wchar_t* s) {
  if (!s) return 0;
  while (*s == L' ' || *s == L'\t') ++s;
  size_t n = wcslen(s);
  while (n && (s[n - 1] == L' ' || s[n - 1] == L'\t' || s[n - 1] == L'\r' || s[n - 1] == L'\n')) --n;
  if (n == 0) return 0;
  if (n == 1 && ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z'))) {
    const int v = (s[0] >= L'a' ? s[0] - L'a' : s[0] - L'A') + 1;
    return PresetAllowed(v) ? v : -1;
  }
  int v = 0;
  for (size_t i = 0; i < n; ++i) {
    if (s[i] < L'0' || s[i] > L'9' || i >= 3) return -1;
    v = v * 10 + (s[i] - L'0');
  }
  if (v == 0) return 0;
  return PresetAllowed(v) ? v : -1;
}

// ---- view classification by output size ---------------------------------------------------------------------------
struct Size {
  uint32_t w = 0, h = 0;
};
struct ViewSizes {
  Size periph, focus;
};
enum Class { kUnknown = 0, kPeriph = 1, kFocus = 2 };
constexpr uint32_t kSizeTolerance = 2;
inline const char* ClassName(int c) { return c == kPeriph ? "periphery" : c == kFocus ? "focus" : "unknown view"; }
inline bool SizesUsable(const ViewSizes& s) {
  const auto ok = [](const Size& z) { return z.w >= 16 && z.h >= 16 && z.w <= 16384 && z.h <= 16384; };
  return ok(s.periph) && ok(s.focus) &&
         static_cast<uint64_t>(s.periph.w) * s.periph.h < static_cast<uint64_t>(s.focus.w) * s.focus.h &&
         (s.periph.w + kSizeTolerance < s.focus.w || s.periph.h + kSizeTolerance < s.focus.h);
}
inline bool Near(uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) <= kSizeTolerance; }
inline Class Classify(uint32_t outW, uint32_t outH, const ViewSizes& s) {
  if (!SizesUsable(s) || !outW || !outH) return kUnknown;
  const bool p = Near(outW, s.periph.w) && Near(outH, s.periph.h);
  const bool f = Near(outW, s.focus.w) && Near(outH, s.focus.h);
  if (p == f) return kUnknown;
  return p ? kPeriph : kFocus;
}
// The last "Recommended peripheral resolution: WxH" and "Recommended focus resolution: WxH" of QVFR's log text.
inline bool ParseQvfrLog(const std::string& t, ViewSizes* out) {
  const auto last = [&](const char* key, Size* z) {
    const size_t at = t.rfind(key);
    if (at == std::string::npos) return false;
    unsigned w = 0, h = 0;
    if (sscanf_s(t.c_str() + at + strlen(key), "%ux%u", &w, &h) != 2) return false;
    z->w = w, z->h = h;
    return true;
  };
  ViewSizes v;
  if (!last("Recommended peripheral resolution: ", &v.periph) || !last("Recommended focus resolution: ", &v.focus))
    return false;
  if (!SizesUsable(v)) return false;
  *out = v;
  return true;
}

// ---- prologue length decoder -----------------------------------------------------------------------------------------
// Accepts only the position-independent instructions compilers put in x64 prologues: push r64, mov/lea/alu r <-> r/m
// (no RIP-relative or absolute operand), sub/add/and r/m, imm, movups/movaps xmm <-> r/m. Anything else is 0 (rejected).
inline size_t ModrmLen(const uint8_t* p) {
  const uint8_t m = p[0], mod = m >> 6, rm = m & 7;
  if (mod == 3) return 1;
  size_t n = 1;
  if (rm == 4) {
    n = 2;
    if (mod == 0 && (p[1] & 7) == 5) return 0;  // SIB without base: disp32 absolute
  } else if (mod == 0 && rm == 5) {
    return 0;  // RIP-relative: would need relocation
  }
  if (mod == 1) n += 1;
  else if (mod == 2) n += 4;
  return n;
}
inline size_t InsnLen(const uint8_t* p) {
  size_t i = 0;
  if ((p[0] & 0xF0) == 0x40) i = 1;  // REX
  const uint8_t op = p[i];
  if (op >= 0x50 && op <= 0x57) return i + 1;  // push r64
  switch (op) {
    case 0x89: case 0x8B: case 0x8D: case 0x01: case 0x03: case 0x29: case 0x2B: case 0x31: case 0x33: case 0x85:
    case 0x39: case 0x3B: {
      const size_t m = ModrmLen(p + i + 1);
      return m ? i + 1 + m : 0;
    }
    case 0x83: {
      const size_t m = ModrmLen(p + i + 1);
      return m ? i + 1 + m + 1 : 0;
    }
    case 0x81: {
      const size_t m = ModrmLen(p + i + 1);
      return m ? i + 1 + m + 4 : 0;
    }
    case 0x0F: {
      const uint8_t o2 = p[i + 1];
      if (o2 != 0x10 && o2 != 0x11 && o2 != 0x28 && o2 != 0x29) return 0;
      const size_t m = ModrmLen(p + i + 2);
      return m ? i + 2 + m : 0;
    }
    default:
      return 0;
  }
}
constexpr size_t kPatchLen = 14, kMaxCopy = 32;
// Whole instructions covering at least kPatchLen bytes (at most kMaxCopy), or 0.
inline size_t PrologueLen(const uint8_t* p) {
  size_t n = 0;
  while (n < kPatchLen) {
    const size_t l = InsnLen(p + n);
    if (!l) return 0;
    n += l;
  }
  return n <= kMaxCopy ? n : 0;
}

// ---- persistent inline detour --------------------------------------------------------------------------------------
// Code page (execute-read): +0 the stub, jmp [rip -> data page +0]; +16 the trampoline, the original n bytes then
// jmp [rip+0] back to target + n. Data page (read-write): +0 the hook pointer (the slot), +8 magic and n, +16 the
// original bytes. The target's first n bytes become jmp [rip+0] -> code page, padded with int3. Never freed: a later
// payload finds it again from the patched bytes and repoints the slot.
constexpr size_t kPage = 4096;
constexpr uint32_t kMagic = 0x32325152;  // "RQ22"
struct Detour {
  const char* name = "";
  void** slot = nullptr;
  void* tramp = nullptr;
};

bool SafeRead(const void* p, void* out, size_t n) {
  __try {
    memcpy(out, p, n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Result: 1 = patched now, 2 = an earlier payload's patch reused, < 0 = nothing installed (-1 unreadable, -2 prologue
// not accepted, -3 a foreign patch, -4 no memory, -5 the write failed).
int InstallDetour(uint8_t* target, void* hook, Detour* d) {
  uint8_t cur[kMaxCopy] = {};
  if (!target || !SafeRead(target, cur, sizeof(cur))) return -1;
  if (cur[0] == 0xFF && cur[1] == 0x25 && !cur[2] && !cur[3] && !cur[4] && !cur[5]) {
    uint8_t* page = nullptr;
    memcpy(&page, cur + 6, 8);
    uint8_t stub[6] = {};
    uint32_t hdr[2] = {};
    uint8_t orig[kMaxCopy] = {}, tr[kMaxCopy + 14] = {};
    const int32_t wantRel = static_cast<int32_t>(kPage - 6);
    if (!page || !SafeRead(page, stub, 6) || !SafeRead(page + kPage + 8, hdr, 8) || stub[0] != 0xFF ||
        stub[1] != 0x25 || memcmp(stub + 2, &wantRel, 4) != 0 || hdr[0] != kMagic || hdr[1] < kPatchLen ||
        hdr[1] > kMaxCopy)
      return -3;
    const size_t n = hdr[1];
    uint8_t* back = nullptr;
    if (!SafeRead(page + kPage + 16, orig, n) || !SafeRead(page + 16, tr, n + 14) || memcmp(tr, orig, n) != 0 ||
        tr[n] != 0xFF || tr[n + 1] != 0x25)
      return -3;
    memcpy(&back, tr + n + 6, 8);
    if (back != target + n) return -3;
    d->slot = reinterpret_cast<void**>(page + kPage);
    d->tramp = page + 16;
    InterlockedExchangePointer(d->slot, hook);
    return 2;
  }
  const size_t n = PrologueLen(cur);
  if (!n) return -2;
  auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 2 * kPage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!page) return -4;
  memset(page, 0xCC, kPage);
  const int32_t rel = static_cast<int32_t>(kPage - 6);
  page[0] = 0xFF, page[1] = 0x25;
  memcpy(page + 2, &rel, 4);
  uint8_t* tr = page + 16;
  memcpy(tr, cur, n);
  tr[n] = 0xFF, tr[n + 1] = 0x25;
  memset(tr + n + 2, 0, 4);
  uint8_t* back = target + n;
  memcpy(tr + n + 6, &back, 8);
  void** slot = reinterpret_cast<void**>(page + kPage);
  *slot = tr;  // pass-through until the hook is set below
  const uint32_t hdr[2] = {kMagic, static_cast<uint32_t>(n)};
  memcpy(page + kPage + 8, hdr, 8);
  memcpy(page + kPage + 16, cur, n);
  DWORD old;
  if (!VirtualProtect(page, kPage, PAGE_EXECUTE_READ, &old)) {
    VirtualFree(page, 0, MEM_RELEASE);
    return -4;
  }
  FlushInstructionCache(GetCurrentProcess(), page, kPage);
  uint8_t patch[kMaxCopy];
  memset(patch, 0xCC, sizeof(patch));
  patch[0] = 0xFF, patch[1] = 0x25;
  memset(patch + 2, 0, 4);
  memcpy(patch + 6, &page, 8);
  const qvpatch::Site site{target, patch, n};
  if (!qvpatch::WriteSuspended(&site, 1, 20)) {
    VirtualFree(page, 0, MEM_RELEASE);  // the target was not written: nobody can reach the page
    return -5;
  }
  d->slot = slot;
  d->tramp = tr;
  InterlockedExchangePointer(d->slot, hook);
  return 1;
}
inline const char* DetourError(int r) {
  switch (r) {
    case -1: return "its code is not readable";
    case -2: return "its prologue is not one this build knows how to relocate (another driver version?)";
    case -3: return "it carries a patch that is not ours";
    case -4: return "no memory for the stub";
    case -5: return "the patch could not be written safely";
    default: return "?";
  }
}
// Pass-through (the hook pointer back to the trampoline); the patch and pages stay for the next payload.
inline void Release(Detour* d) {
  if (d->slot && d->tramp) InterlockedExchangePointer(d->slot, d->tramp);
}

// ---- driver profile (read-only) ------------------------------------------------------------------------------------
struct DrsInfo {
  int state = 0;  // 0 not read, 1 read, -1 NvAPI unavailable, -2 the read faulted
  bool appFound = false;
  bool appPresetSet = false, basePresetSet = false, appOverrideSet = false;
  uint32_t appPreset = 0, basePreset = 0, appOverride = 0;
  int appPresetLocation = -1;  // NVDRS_SETTING_LOCATION: 0 current profile, 1 global, 2 base, 3 default
};
inline bool PresetForces(uint32_t v) { return v != kDrsPresetOff && v != kDrsPresetDefault; }
// The preset the driver forces on DLSS-SR for this process (0 = none): the snippet's order, the application profile's
// value, else the base profile's.
inline uint32_t ForcedPreset(const DrsInfo& d) {
  if (d.state != 1) return 0;
  if (d.appPresetSet) return PresetForces(d.appPreset) ? d.appPreset : 0;
  if (d.basePresetSet && PresetForces(d.basePreset)) return d.basePreset;
  return 0;
}

// ---- state ---------------------------------------------------------------------------------------------------------
std::atomic<int> g_periph{0}, g_focus{0};
std::atomic<bool> g_ignore{false}, g_killed{false};
// 0 not installed yet, 1 installed, -1 failed (latched), -2 (GetSetting only) too late: NGX was already initialised
std::atomic<int> g_createState{0}, g_getSettingState{0};
Detour g_create{"NVSDK_NGX_D3D11_CreateFeature"}, g_getSetting{"NvAPI_DRS_GetSetting"};
std::atomic<CreateFn> g_createOrig{nullptr};
std::atomic<GetSettingFn> g_getSettingOrig{nullptr};
GetSettingFn g_nvGetSetting = nullptr;  // NvAPI's own (resolved by ReadDrs; the GetSetting hook's target)
DrsInfo g_drs;
std::atomic<int> g_drsState{0};
std::atomic<uint32_t> g_pW{0}, g_pH{0}, g_fW{0}, g_fH{0};
std::atomic<uint64_t> g_qvfrLogTime{0};
std::atomic<uint32_t> g_creates{0}, g_applied{0}, g_hiddenReads{0}, g_getSettingCalls{0};
std::mutex g_sizeMutex;
bool g_waitLogged = false, g_warned = false, g_offLogged = true, g_badKeyLogged = false;
// Tests only (null / empty / false in the payload): stand-ins for the modules, the caller check and QVFR's log.
uint8_t* g_testCreateTarget = nullptr;
uint8_t* g_testGetSettingTarget = nullptr;
bool g_testNgxUp = false;      // with g_testGetSettingTarget: "NGX is already initialised"
bool g_testAnyCaller = false;  // every GetSetting caller counts as a DLSS module
std::wstring g_testQvfrLog;
bool g_testNoQvfrModule = false;
void (*g_testGetSettingObs)(uint32_t id, bool hidden) = nullptr;  // every GetSetting call through the hook

inline ViewSizes CurrentSizes() {
  ViewSizes s;
  s.periph = {g_pW.load(), g_pH.load()};
  s.focus = {g_fW.load(), g_fH.load()};
  return s;
}

std::wstring QvfrLogPath() {
  if (!g_testQvfrLog.empty()) return g_testQvfrLog;
  wchar_t buf[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  if (!n || n >= MAX_PATH) return L"";
  return std::wstring(buf) + L"\\Quad-Views-Foveated\\Quad-Views-Foveated.log";
}
uint64_t FileTime64(const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

// Reads QVFR's log when it changed since the last read. True when usable sizes are known afterwards.
bool RefreshSizes() {
  std::lock_guard<std::mutex> lock(g_sizeMutex);
  if (!g_testNoQvfrModule && g_testQvfrLog.empty() && !GetModuleHandleW(kQvfrModule)) {
    g_pW = g_pH = g_fW = g_fH = 0;  // no Quad Views in this process: no class
    return false;
  }
  const std::wstring path = QvfrLogPath();
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return SizesUsable(CurrentSizes());
  const uint64_t t = FileTime64(fa.ftLastWriteTime);
  if (t == g_qvfrLogTime.load()) return SizesUsable(CurrentSizes());
  if (g_testQvfrLog.empty()) {  // written by this process's session, not an older one
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) && t < FileTime64(c)) return false;
  }
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return SizesUsable(CurrentSizes());
  LARGE_INTEGER sz{};
  GetFileSizeEx(h, &sz);
  constexpr int64_t kCap = 4 << 20;  // the tail: the session's lines are the last ones
  const int64_t from = sz.QuadPart > kCap ? sz.QuadPart - kCap : 0;
  std::string text(static_cast<size_t>(sz.QuadPart - from), '\0');
  LARGE_INTEGER off{};
  off.QuadPart = from;
  DWORD got = 0;
  const bool ok = SetFilePointerEx(h, off, nullptr, FILE_BEGIN) && !text.empty() &&
                  ReadFile(h, &text[0], static_cast<DWORD>(text.size()), &got, nullptr);
  CloseHandle(h);
  if (!ok) return SizesUsable(CurrentSizes());
  text.resize(got);
  g_qvfrLogTime = t;
  ViewSizes v;
  if (!ParseQvfrLog(text, &v)) {
    g_pW = g_pH = g_fW = g_fH = 0;
    return false;
  }
  const ViewSizes old = CurrentSizes();
  g_pW = v.periph.w, g_pH = v.periph.h, g_fW = v.focus.w, g_fH = v.focus.h;
  if (old.periph.w != v.periph.w || old.periph.h != v.periph.h || old.focus.w != v.focus.w || old.focus.h != v.focus.h)
    Log("dlss views: Quad Views sizes periphery %ux%u, focus %ux%u (from Quad-Views-Foveated.log)", v.periph.w,
        v.periph.h, v.focus.w, v.focus.h);
  return true;
}

// ---- the hooks ----------------------------------------------------------------------------------------------------
struct Decision {
  int cls = kUnknown;
  uint32_t inW = 0, inH = 0, outW = 0, outH = 0;
  uint32_t dcsHint = 0;
  bool dcsHintRead = false;
  int preset = 0;           // what we set (0 = left as DCS asked)
  bool fromDriver = false;  // preset = the driver profile's, made explicit (IgnoreDriverPreset hid it from DLSS)
  bool fault = false;
  const char* why = "";
};

inline uint32_t GetU(const NgxParam* p, const char* name, bool* ok) {
  unsigned int u = 0;
  if (p->Get(name, &u) == kNgxSuccess) {
    if (ok) *ok = true;
    return u;
  }
  int i = 0;
  if (p->Get(name, &i) == kNgxSuccess && i >= 0) {
    if (ok) *ok = true;
    return static_cast<uint32_t>(i);
  }
  if (ok) *ok = false;
  return 0;
}

// No C++ objects with destructors here (SEH). driverPreset: the driver profile's forced preset, to make explicit on a
// feature we would otherwise leave alone (0 = none). Non-zero only once IgnoreDriverPreset has hidden the profile from
// DLSS in this process: DLSS then knows no driver preset at all, and this keeps the user's preset where we set none.
void DecideImpl(NgxParam* p, int periph, int focus, bool killed, int driverPreset, const ViewSizes& sizes,
                Decision* d) {
  d->outW = GetU(p, "OutWidth", nullptr);
  d->outH = GetU(p, "OutHeight", nullptr);
  d->inW = GetU(p, "Width", nullptr);
  d->inH = GetU(p, "Height", nullptr);
  d->dcsHint = GetU(p, kHintKeys[2], &d->dcsHintRead);  // Balanced (DCS sets all six alike)
  d->cls = Classify(d->outW, d->outH, sizes);
  int want = 0;
  if (periph <= 0) {
    d->why = "off ([DLSS] PeripheryPreset=0)";
  } else if (killed) {
    d->why = "kill switch on";
  } else if (!SizesUsable(sizes)) {
    d->why = "Quad Views sizes unknown";
  } else if (d->cls == kUnknown) {
    d->why = "size matches no Quad Views view";
  } else {
    want = d->cls == kPeriph ? periph : focus;
    if (want <= 0) d->why = "FocusPreset=0";
  }
  if (want <= 0 && driverPreset > 0) {
    want = driverPreset;
    d->fromDriver = true;
  }
  if (want <= 0) return;
  for (const char* k : kHintKeys) p->Set(k, static_cast<unsigned int>(want));
  d->preset = want;
  if (!d->fromDriver) d->why = "set";
}
void Decide(NgxParam* p, int periph, int focus, bool killed, int driverPreset, const ViewSizes& sizes, Decision* d) {
  __try {
    DecideImpl(p, periph, focus, killed, driverPreset, sizes, d);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    d->fault = true;
    d->preset = 0;
  }
}

// The driver's forced preset when it is one a hint can name (A-Z), else 0 ("latest" or none).
inline int NameableForced() {
  if (g_drsState.load() != 1) return 0;
  const uint32_t f = ForcedPreset(g_drs);
  return f >= 1 && f <= 26 ? static_cast<int>(f) : 0;
}
// "DLSS was shown no driver preset in this process": our counter, or the process environment (it survives payload
// hot reloads; process-local, gone with DCS).
const wchar_t* const kHiddenEnv = L"DCSQV_DLSS_DRIVER_PRESET_HIDDEN";
inline bool HiddenThisSession() {
  if (g_hiddenReads.load() > 0) return true;
  wchar_t v[4] = {};
  return GetEnvironmentVariableW(kHiddenEnv, v, 4) > 0;
}

std::string OverrideText() {
  const int st = g_drsState.load();
  if (st == 0) return "driver override unknown (profile not read yet)";
  if (st < 0) return "driver override unknown (NvAPI unavailable)";
  const uint32_t f = ForcedPreset(g_drs);
  if (!f) return "driver override: none";
  char b[160];
  if (HiddenThisSession())
    snprintf(b, sizeof(b), "driver override: forces preset %s, hidden from DLSS since NGX start (IgnoreDriverPreset)",
             PresetName(f));
  else
    snprintf(b, sizeof(b), "driver override: forces preset %s, ACTIVE (it wins over the preset requested here)",
             PresetName(f));
  return b;
}

void Report(const Decision& d, int result, uint32_t seq) {
  const std::string ovr = OverrideText();
  char hint[24];
  snprintf(hint, sizeof(hint), "%s", d.dcsHintRead ? PresetName(d.dcsHint) : "?");
  if (d.fault)
    Log("dlss views: create #%u: fault reading DCS's parameters; feature left as DCS asked; %s; result 0x%x", seq,
        ovr.c_str(), static_cast<unsigned>(result));
  else if (d.preset)
    Log("dlss views: create #%u %s %ux%u->%ux%u: preset %s requested%s (DCS hint %s); %s; result 0x%x", seq,
        ClassName(d.cls), d.inW, d.inH, d.outW, d.outH, PresetName(static_cast<uint32_t>(d.preset)),
        d.fromDriver ? " (the driver profile's preset, made explicit)" : "", hint, ovr.c_str(),
        static_cast<unsigned>(result));
  else
    Log("dlss views: create #%u %s %ux%u->%ux%u: unchanged, %s (DCS hint %s); %s; result 0x%x", seq, ClassName(d.cls),
        d.inW, d.inH, d.outW, d.outH, d.why, hint, ovr.c_str(), static_cast<unsigned>(result));
}

int __cdecl CreateHook(void* ctx, int feature, NgxParam* params, void** handle) {
  const CreateFn orig = g_createOrig.load(std::memory_order_acquire);
  if (feature != kFeatureSuperSampling || !params) return orig(ctx, feature, params, handle);
  const uint32_t seq = g_creates.fetch_add(1) + 1;
  Decision d;
  const int periph = g_periph.load(), focus = g_focus.load();
  const bool killed = g_killed.load();
  if (periph > 0 && !killed) {
    try {
      if (!SizesUsable(CurrentSizes())) RefreshSizes();  // mission load: QVFR's log is small and read once
    } catch (...) {
    }
  }
  const int driverPreset = HiddenThisSession() ? NameableForced() : 0;
  Decide(params, periph, focus, killed, driverPreset, CurrentSizes(), &d);
  if (d.preset) g_applied.fetch_add(1);
  const int r = orig(ctx, feature, params, handle);
  try {
    Report(d, r, seq);
  } catch (...) {
  }
  return r;
}

// "\nvngx_dlss*.dll" (DCS's copy, or one an app ships) or a file under an NGX models folder (the driver's OTA copy,
// "...\NGX\models\dlss\versions\<n>\files\<x>.bin"). Case-insensitive.
inline bool IsDlssModulePath(const wchar_t* path) {
  if (!path || !*path) return false;
  std::wstring lo(path);
  for (auto& c : lo) {
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    if (c == L'/') c = L'\\';
  }
  const size_t slash = lo.find_last_of(L'\\');
  const std::wstring base = slash == std::wstring::npos ? lo : lo.substr(slash + 1);
  return base.rfind(L"nvngx_dlss", 0) == 0 || lo.find(L"\\ngx\\models\\") != std::wstring::npos;
}
bool CallerIsDlss(void* ret) {
  if (g_testAnyCaller) return true;
  HMODULE m = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(ret), &m) ||
      !m)
    return false;
  wchar_t p[MAX_PATH * 2] = {};
  const DWORD n = GetModuleFileNameW(m, p, MAX_PATH * 2);
  return n > 0 && n < MAX_PATH * 2 && IsDlssModulePath(p);
}

int __cdecl GetSettingHook(void* session, void* profile, uint32_t id, void* setting) {
  g_getSettingCalls.fetch_add(1, std::memory_order_relaxed);
  bool hide = false;
  if (id == kDrsSrPreset && g_ignore.load(std::memory_order_relaxed) && !g_killed.load(std::memory_order_relaxed)) {
    try {
      hide = CallerIsDlss(_ReturnAddress());
    } catch (...) {
      hide = false;
    }
  }
  if (g_testGetSettingObs) g_testGetSettingObs(id, hide);
  if (hide) {
    g_hiddenReads.fetch_add(1, std::memory_order_relaxed);
    SetEnvironmentVariableW(kHiddenEnv, L"1");
    return kNvapiSettingNotFound;
  }
  return g_getSettingOrig.load(std::memory_order_acquire)(session, profile, id, setting);
}

// ---- driver profile read (read-only) ---------------------------------------------------------------------------------
struct NvFns {
  int(__cdecl* init)();
  int(__cdecl* createSession)(void** s);
  int(__cdecl* loadSettings)(void* s);
  int(__cdecl* findApp)(void* s, const uint16_t* name, void** profile, void* app);
  GetSettingFn getSetting;
  int(__cdecl* getBase)(void* s, void** profile);
  int(__cdecl* destroySession)(void* s);
};
// Reads one DWORD setting of a profile: true when the profile has it (any location).
bool ReadU32(const NvFns& f, void* s, void* prof, uint32_t id, uint8_t* buf, uint32_t* v, int* loc) {
  memset(buf, 0, kSettingSize);
  memcpy(buf, &kSettingVer, 4);
  if (f.getSetting(s, prof, id, buf) != kNvapiOk) return false;
  memcpy(v, buf + kSettingU32Current, 4);
  if (loc) memcpy(loc, buf + kSettingLocation, 4);
  return true;
}
// No C++ objects with destructors (SEH). The buffers come from the caller.
int ReadDrsImpl(const NvFns& f, const uint16_t* fullPath, const uint16_t* baseName, uint8_t* setting, uint8_t* app,
                DrsInfo* out) {
  if (f.init() != kNvapiOk) return -1;
  void* s = nullptr;
  if (f.createSession(&s) != kNvapiOk || !s) return -1;
  if (f.loadSettings(s) == kNvapiOk) {
    void* prof = nullptr;
    memset(app, 0, kAppSize);
    memcpy(app, &kAppVer, 4);
    bool found = f.findApp(s, fullPath, &prof, app) == kNvapiOk && prof;
    if (!found) {
      prof = nullptr;
      memset(app, 0, kAppSize);
      memcpy(app, &kAppVer, 4);
      found = f.findApp(s, baseName, &prof, app) == kNvapiOk && prof;
    }
    out->appFound = found;
    if (found) {
      out->appPresetSet = ReadU32(f, s, prof, kDrsSrPreset, setting, &out->appPreset, &out->appPresetLocation);
      out->appOverrideSet = ReadU32(f, s, prof, kDrsSrOverride, setting, &out->appOverride, nullptr);
    }
    void* base = nullptr;
    if (f.getBase(s, &base) == kNvapiOk && base)
      out->basePresetSet = ReadU32(f, s, base, kDrsSrPreset, setting, &out->basePreset, nullptr);
  }
  f.destroySession(s);
  return 1;
}
int ReadDrsGuarded(const NvFns& f, const uint16_t* fullPath, const uint16_t* baseName, uint8_t* setting, uint8_t* app,
                   DrsInfo* out) {
  __try {
    return ReadDrsImpl(f, fullPath, baseName, setting, app, out);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -2;
  }
}

// Resolves NvAPI (nvapi64.dll from System32, pinned with its implementation module: the GetSetting detour lives there)
// and reads the profile once. Never writes it.
void ReadDrs() {
  HMODULE nv = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  HMODULE pinned = nullptr;
  if (nv) GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"nvapi64.dll", &pinned);
  const auto qi = nv ? reinterpret_cast<QueryFn>(GetProcAddress(nv, "nvapi_QueryInterface")) : nullptr;
  NvFns f{};
  if (qi) {
    f.init = reinterpret_cast<int(__cdecl*)()>(qi(kQiInitialize));
    f.createSession = reinterpret_cast<int(__cdecl*)(void**)>(qi(kQiCreateSession));
    f.loadSettings = reinterpret_cast<int(__cdecl*)(void*)>(qi(kQiLoadSettings));
    f.findApp = reinterpret_cast<int(__cdecl*)(void*, const uint16_t*, void**, void*)>(qi(kQiFindApplicationByName));
    f.getSetting = reinterpret_cast<GetSettingFn>(qi(kQiGetSetting));
    f.getBase = reinterpret_cast<int(__cdecl*)(void*, void**)>(qi(kQiGetBaseProfile));
    f.destroySession = reinterpret_cast<int(__cdecl*)(void*)>(qi(kQiDestroySession));
  }
  DrsInfo info;
  if (!f.init || !f.createSession || !f.loadSettings || !f.findApp || !f.getSetting || !f.getBase ||
      !f.destroySession) {
    info.state = -1;
  } else {
    HMODULE impl = nullptr;  // the module that implements GetSetting (nvapi64_impl.dll on current drivers)
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(f.getSetting), &impl);
    g_nvGetSetting = f.getSetting;
    std::vector<uint16_t> full(2048, 0), base(2048, 0);
    std::vector<wchar_t> path(32768, 0);
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    const wchar_t* b = path.data();
    for (DWORD i = 0; i < n; ++i)
      if (path[i] == L'\\' || path[i] == L'/') b = path.data() + i + 1;
    for (size_t i = 0; i < 2047 && path[i]; ++i) full[i] = path[i];
    for (size_t i = 0; i < 2047 && b[i]; ++i) base[i] = b[i];
    std::vector<uint8_t> setting(kSettingSize), app(kAppSize);
    const int r = ReadDrsGuarded(f, full.data(), base.data(), setting.data(), app.data(), &info);
    info.state = r == 1 ? 1 : r;
  }
  g_drs = info;
  g_drsState = info.state;
  if (info.state != 1) {
    Log("dlss views: driver profile not readable (%s); the per-feature lines say \"unknown\"",
        info.state == -2 ? "the NvAPI read faulted" : "NvAPI unavailable");
    return;
  }
  const uint32_t forced = ForcedPreset(info);
  Log("dlss views: driver profile (read-only): application profile %s; DLSS-SR preset override %s%s%s; latest-DLL "
      "override %s",
      info.appFound ? "found" : "not found",
      info.appPresetSet ? PresetName(info.appPreset) : (info.basePresetSet ? PresetName(info.basePreset) : "not set"),
      info.appPresetSet ? " (application)" : (info.basePresetSet ? " (base profile)" : ""),
      forced ? " -> forces the preset" : " -> app hints apply",
      info.appOverrideSet ? (info.appOverride ? "on" : "off") : "not set");
}


void WarnOnce() {
  if (g_warned || g_drsState.load() != 1) return;
  const uint32_t forced = ForcedPreset(g_drs);
  if (!forced || HiddenThisSession() || g_getSettingState.load() == 1) return;  // filtered in time: no warning
  g_warned = true;
  Log("dlss views: WARNING: the NVIDIA driver profile forces DLSS preset %s for this program, and that wins over "
      "[DLSS] PeripheryPreset: the periphery keeps preset %s. To use the per-view preset, set NVIDIA App > Graphics > "
      "Program settings > DCS World > DLSS Override - Model presets back to its default (the latest-DLL override can "
      "stay), then restart DCS",
      PresetName(forced), PresetName(forced));
}

bool TryInstallCreate() {
  uint8_t* target = g_testCreateTarget;
  if (!target) {
    HMODULE core = GetModuleHandleW(kNgxCore);
    if (!core) {
      if (!g_waitLogged) Log("dlss views: waiting for the NGX core (_nvngx.dll) to load");
      g_waitLogged = true;
      return false;  // retried by the next tick
    }
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, kNgxCore, &pinned);  // our patch never outlives the module
    target = reinterpret_cast<uint8_t*>(GetProcAddress(core, "NVSDK_NGX_D3D11_CreateFeature"));
    if (!target) {
      g_createState = -1;
      Log("dlss views: the NGX core has no NVSDK_NGX_D3D11_CreateFeature export; off for this session");
      return false;
    }
  }
  const int r = InstallDetour(target, reinterpret_cast<void*>(&CreateHook), &g_create);
  if (r < 0) {
    g_createState = -1;
    Log("dlss views: NVSDK_NGX_D3D11_CreateFeature not hooked: %s; DLSS left as DCS sets it", DetourError(r));
    return false;
  }
  g_createOrig.store(reinterpret_cast<CreateFn>(g_create.tramp), std::memory_order_release);
  g_createState = 1;
  Log("dlss views: NVSDK_NGX_D3D11_CreateFeature hooked (%s); presets apply from the next mission load",
      r == 2 ? "earlier payload's patch reused" : "patched");
  return true;
}

// IgnoreDriverPreset. DLSS reads the driver profile once, inside NGX's initialisation: a filter installed after that
// changes nothing. So: installed only while NGX is not up yet (or reused when an earlier payload installed it in time).
bool TryInstallGetSetting() {
  uint8_t* target = g_testGetSettingTarget ? g_testGetSettingTarget : reinterpret_cast<uint8_t*>(g_nvGetSetting);
  if (!target) {
    g_getSettingState = -1;
    Log("dlss views: IgnoreDriverPreset: NvAPI_DRS_GetSetting unavailable; not installed");
    return false;
  }
  const bool ngxUp = g_testGetSettingTarget ? g_testNgxUp : GetModuleHandleW(kNgxCore) != nullptr;
  uint8_t first[2] = {};
  const bool ourPatchMaybe = SafeRead(target, first, 2) && first[0] == 0xFF && first[1] == 0x25;
  if (ngxUp && !ourPatchMaybe) {
    g_getSettingState = -2;
    Log("dlss views: IgnoreDriverPreset: not possible in this DCS session: DCS initialised NGX (and DLSS read the "
        "driver profile, once) before DcsQvCull started. Nothing installed; the driver's preset stays in force");
    return false;
  }
  const int r = InstallDetour(target, reinterpret_cast<void*>(&GetSettingHook), &g_getSetting);
  if (r < 0 || (ngxUp && r != 2)) {
    if (r == 1) Release(&g_getSetting);  // cannot happen (ngxUp needs our patch), kept pass-through if it did
    g_getSettingState = -1;
    Log("dlss views: IgnoreDriverPreset: NvAPI_DRS_GetSetting not hooked: %s; the driver's preset stays in force",
        r < 0 ? DetourError(r) : "unexpected state");
    return false;
  }
  g_getSettingOrig.store(reinterpret_cast<GetSettingFn>(g_getSetting.tramp), std::memory_order_release);
  g_getSettingState = 1;
  Log("dlss views: IgnoreDriverPreset: NvAPI_DRS_GetSetting filtered (%s): DLSS modules reading the DLSS-SR preset "
      "setting get \"not found\"; every other read passes; nothing is written",
      r == 2 ? "earlier payload's patch reused" : "patched before NGX started");
  return true;
}

// Worker tick (about once per second). periph / focus: the parsed keys (-1 = a value we cannot use). Nothing is
// installed while PeripheryPreset is 0.
void Tick(int periph, int focus, bool ignore, bool engineOff) {
  if ((periph < 0 || focus < 0) && !g_badKeyLogged) {
    g_badKeyLogged = true;
    Log("dlss views: [DLSS] %s is not a preset this build accepts (0, A-F, J-M or their NGX numbers); treated as 0",
        periph < 0 ? "PeripheryPreset" : "FocusPreset");
  }
  periph = periph > 0 ? periph : 0;
  focus = focus > 0 ? focus : 0;
  const bool was = g_periph.load() > 0;
  g_periph = periph, g_focus = focus, g_ignore = ignore && periph > 0, g_killed = engineOff;
  if (!periph) {
    if (was && !g_offLogged)
      Log("dlss views: off; %s again from the next mission load",
          HiddenThisSession() ? "the driver profile's preset (made explicit)" : "DCS's own preset hint");
    g_offLogged = true;
    return;
  }
  if (g_offLogged) {
    g_offLogged = false;
    Log("dlss views: on: periphery preset %s, focus %s, IgnoreDriverPreset=%d (applies at the next mission load)",
        PresetName(static_cast<uint32_t>(periph)), focus ? PresetName(static_cast<uint32_t>(focus)) : "unchanged",
        ignore ? 1 : 0);
  }
  if (g_drsState.load() == 0) ReadDrs();
  if (ignore && g_getSettingState.load() == 0 && g_drsState.load() == 1) TryInstallGetSetting();  // before NGX, or never
  if (g_createState.load() == 0) TryInstallCreate();
  WarnOnce();
  try {
    RefreshSizes();
  } catch (...) {
  }
}

// Payload stop: both hooks pass-through (the patches and their pages stay for the next payload).
void Stop() {
  Release(&g_create);
  Release(&g_getSetting);
}

}  // namespace dlssv
