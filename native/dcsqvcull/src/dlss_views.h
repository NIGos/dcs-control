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
// R130: [DLSS] Instrument = 0 (default) | 1 (dev): READ-ONLY lifetime counters for DCS's DLSS features (creations
// with their cost, evaluations, releases per handle; summaries in DcsQvCull.log). See the "R130" section below.
//
// R132: [DLSS] ReleaseIdleSets = 0 (default) | 1 (opt-in): RELEASE DCS's DLSS feature sets that DCS never evaluates
// again (R131: the first of the two sets DCS creates at every load, and the previous load's sets; about 0.66 GB of
// private memory each, which NGX reuses for the next creations). [DLSS] ReleaseAfterEvals = N (default 300): every
// handle of the newest set must have been evaluated N times since the last mission event before an older set goes.
// It needs R130's lifetime tracking and turns it on (without the summaries). See the "R132" section below.
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
constexpr int kNgxFeatureNotFound = static_cast<int>(0xBAD00004);  // NVSDK_NGX_Result_FAIL_FeatureNotFound (R132)
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
    // R126 (DCS Control's review of the DRS filter): the slot is written with one atomic exchange; a page that is not
    // page-aligned (every VirtualAlloc result is) is not ours.
    if (reinterpret_cast<uintptr_t>(page) & (kPage - 1)) return -3;
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

void Report(const Decision& d, int result, uint32_t seq, const char* tail = "") {
  const std::string ovr = OverrideText();
  char hint[24];
  snprintf(hint, sizeof(hint), "%s", d.dcsHintRead ? PresetName(d.dcsHint) : "?");
  if (d.fault)
    Log("dlss views: create #%u: fault reading DCS's parameters; feature left as DCS asked; %s; result 0x%x%s", seq,
        ovr.c_str(), static_cast<unsigned>(result), tail);
  else if (d.preset)
    Log("dlss views: create #%u %s %ux%u->%ux%u: preset %s requested%s (DCS hint %s); %s; result 0x%x%s", seq,
        ClassName(d.cls), d.inW, d.inH, d.outW, d.outH, PresetName(static_cast<uint32_t>(d.preset)),
        d.fromDriver ? " (the driver profile's preset, made explicit)" : "", hint, ovr.c_str(),
        static_cast<unsigned>(result), tail);
  else
    Log("dlss views: create #%u %s %ux%u->%ux%u: unchanged, %s (DCS hint %s); %s; result 0x%x%s", seq,
        ClassName(d.cls), d.inW, d.inH, d.outW, d.outH, d.why, hint, ovr.c_str(), static_cast<unsigned>(result), tail);
}

// R126 (R124: DCS created the four DLSS features twice, about 1 s apart, at every mission restart): who called
// CreateFeature (module file name + offset of the return address) and the handle it got. Log only.
void CallerText(void* ret, char* out, size_t n) {
  HMODULE m = nullptr;
  wchar_t p[MAX_PATH * 2] = {};
  DWORD len = 0;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCWSTR>(ret), &m) &&
      m)
    len = GetModuleFileNameW(m, p, MAX_PATH * 2);
  if (!m || !len || len >= MAX_PATH * 2) {
    snprintf(out, n, "0x%llx (no module)", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ret)));
    return;
  }
  const wchar_t* base = p;
  for (const wchar_t* c = p; *c; ++c)
    if (*c == L'\\' || *c == L'/') base = c + 1;
  char name[MAX_PATH] = {};
  if (!WideCharToMultiByte(CP_UTF8, 0, base, -1, name, sizeof(name), nullptr, nullptr)) snprintf(name, sizeof(name), "?");
  snprintf(out, n, "%s+0x%llx", name,
           static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ret) - reinterpret_cast<uintptr_t>(m)));
}
void CreateTail(void* ret, void** handle, char* out, size_t n) {
  char caller[MAX_PATH + 40];
  CallerText(ret, caller, sizeof(caller));
  void* h = nullptr;
  uint32_t id = 0;
  const bool haveH = handle && SafeRead(handle, &h, sizeof(h));
  const bool haveId = haveH && h && SafeRead(h, &id, sizeof(id));
  char hs[64];
  if (!haveH)
    snprintf(hs, sizeof(hs), "none");
  else if (haveId)
    snprintf(hs, sizeof(hs), "0x%llx (id %u)", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)), id);
  else
    snprintf(hs, sizeof(hs), "0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)));
  snprintf(out, n, "; caller %s; handle %s", caller, hs);
}

// ---- R130: DLSS feature lifetime instrumentation (READ-ONLY; [DLSS] Instrument=1, dev key, default 0) -------------
// R127 saw DCS create two full sets of four DLSS features (P, P, F, F) about 7 ms apart at every mission load, all from
// the same dx11backend call site, and never release any (R119: +1.1 GB private memory per restart). Two facts before
// any release can be considered: (1) is the first set ever evaluated once the second exists, (2) what one feature
// costs. With the key on:
//   - NVSDK_NGX_D3D11_EvaluateFeature (the core has no _C export: the static loader's EvaluateFeature_C lands here
//     too) and NVSDK_NGX_D3D11_ReleaseFeature are detoured (the R122 scheme), pure pass-through: after the original
//     returns, the evaluation is counted on the handle's entry (first / last time, count, count since the handle's set
//     was superseded); a successful release marks the entry released (DCS is not expected to release any).
//   - Each DLSS-SR creation (the R122 CreateFeature hook) gets an entry: handle, NGX id, class, its creation set and
//     mission load. A load is a run of creations less than 1 s apart; inside a load a new set starts when an output
//     size seen in the current set comes back after another size (P,P,F,F,P,... -> the second P starts set 2). A new
//     set supersedes every earlier live entry.
//   - Around each creation: DXGI local-segment CurrentUsage of DCS's adapter (from the context DCS passes), the
//     process's private bytes, and DLSS's own VRAM figure (NGX_DLSS_GET_STATS: the snippet's DLSSGetStatsCallback,
//     called on a capability parameter block of our own, never on DCS's) before and after; the deltas are appended
//     to the create line, and the per-set totals are logged when the load closes (2 s after its last creation).
//   - A summary every 10 s and at each mission load (before the new load's first creation): per set, the
//     evaluations since the last summary and the time since the last evaluation of each live handle; older sets with
//     no evaluation since they were superseded are flagged IDLE, any that are still evaluated are flagged STILL
//     EVALUATED; plus what the later opt-in release rule would say (observation only, nothing is released). A summary
//     with nothing new after an idle one is skipped.
// Nothing is changed: no NGX call on DCS's handles or parameters, no ReleaseFeature, no reuse. The kill switch and
// key 0 stop the counting (the detours stay, pass-through). The key works with or without the [DLSS] preset keys;
// with Instrument=0 and PeripheryPreset=0 nothing is installed at all.
using EvalFn = int(__cdecl*)(void* ctx, const void* handle, const NgxParam* params, void* callback);
using ReleaseFn = int(__cdecl*)(void* handle);
using CapParamsFn = int(__cdecl*)(NgxParam** out);
using StatsCbFn = int(__cdecl*)(NgxParam* params);

namespace life {
constexpr uint32_t kMax = 256;            // entries (8 per mission load in DCS: 32 loads); more are counted, not kept
constexpr double kLoadGapSec = 1.0;       // creations further apart start a new mission load
constexpr double kLoadDoneSec = 2.0;      // a load's totals are logged this long after its last creation
constexpr double kSummarySec = 10.0;
constexpr uint64_t kRuleMinEvals = 300;   // the release rule's N (observation only): newest set's handles each >= N
constexpr int kMaxCapTries = 8;

struct Feat {
  void* handle = nullptr;
  uint32_t id = 0, seq = 0, set = 0, load = 0;
  int cls = kUnknown;
  uint32_t outW = 0, outH = 0;
  int64_t createQpc = 0;
  std::atomic<uint64_t> evals{0}, evalsAfterSuper{0};
  std::atomic<int64_t> firstEval{0}, lastEval{0}, superQpc{0};
  std::atomic<uint32_t> superBy{0};
  std::atomic<bool> released{false};
  uint64_t evalsAtSummary = 0;  // summaries only (under g_mx)
  bool vramOk = false, privOk = false, statsOk = false;
  int64_t vramDelta = 0, privDelta = 0, statsDelta = 0;
  // R132: evaluations since the last mission event (gateGen = the event generation they count for; the evaluating
  // thread resets them lazily), released by us (and NGX's answer), the set refused by the guard (under g_mx).
  std::atomic<uint32_t> gateGen{0};
  std::atomic<uint64_t> gateEvals{0};
  std::atomic<bool> ours{false};
  int relResult = 0;
  bool refused = false;
};
Feat g_f[kMax];
std::atomic<uint32_t> g_n{0};  // published entries (written under g_mx, read lock-free by the evaluate hook)
std::atomic<bool> g_on{false};  // counting: Instrument=1 and the kill switch off
bool g_key = false;             // Instrument=1 (worker)
std::atomic<uint64_t> g_evalTotal{0}, g_evalFail{0}, g_untracked{0}, g_evalFault{0}, g_releases{0}, g_dropped{0};
std::atomic<int> g_evalState{0}, g_releaseState{0};  // 0 not installed, 1 installed, -1 failed (latched)
Detour g_eval{"NVSDK_NGX_D3D11_EvaluateFeature"}, g_release{"NVSDK_NGX_D3D11_ReleaseFeature"};
std::atomic<EvalFn> g_evalOrig{nullptr};
std::atomic<ReleaseFn> g_releaseOrig{nullptr};
std::mutex g_mx;
uint32_t g_load = 0, g_set = 0, g_setFirst = 0, g_loadFirst = 0;
uint32_t g_prevW = 0, g_prevH = 0;
int64_t g_lastCreate = 0, g_loadStart = 0;
bool g_loadOpen = false;
int64_t g_lastSummary = 0;
uint64_t g_evalAtSummary = 0, g_untrackedAtSummary = 0, g_failAtSummary = 0, g_releasesAtSummary = 0;
uint32_t g_changes = 0;  // creations and releases since the last summary
bool g_lastIdle = false, g_onLogged = false, g_pausedLogged = false, g_fullLogged = false;
NgxParam* g_cap = nullptr;  // our own capability parameter block (kept for the process; never DCS's)
StatsCbFn g_statsCb = nullptr;
int g_capTries = 0;
// Tests only: fake targets, a clock, the memory readings.
uint8_t* g_testEvalTarget = nullptr;
uint8_t* g_testReleaseTarget = nullptr;
int64_t (*g_testClock)() = nullptr;
bool (*g_testVram)(void* ctx, uint64_t* local) = nullptr;
bool (*g_testPriv)(uint64_t* bytes) = nullptr;
CapParamsFn g_testCapParams = nullptr;

// R132 ([DLSS] ReleaseIdleSets; the rule in the "R132" section below).
bool g_verbose = false;                  // Instrument=1: summaries (the release key alone tracks without them)
std::atomic<bool> g_relOn{false};        // ReleaseIdleSets=1, kill switch off, not latched, not stopping
bool g_relKey = false;                   // ReleaseIdleSets=1 (worker)
std::atomic<uint64_t> g_relN{kRuleMinEvals};
std::atomic<bool> g_relLatched{false};   // a fault, a failed release, a refused evaluate or a full guard: off for the session
std::atomic<bool> g_noMore{false};       // payload stop or DCS shutdown (quiesce): never another release
std::atomic<uint32_t> g_gateGen{0};      // mission events (onMissionLoadBegin / onSimulationStop / slot change)
std::atomic<uint32_t> g_changeGen{0};    // creations, events, releases: a "no candidate" verdict is redone after one
std::atomic<uint32_t> g_checkedGen{~0u};
std::atomic<uint32_t> g_newestSetA{0};   // the newest set (lock-free copy of g_set)
std::atomic<int64_t> g_lastCreateA{0};   // the last tracked creation (lock-free copy of g_lastCreate)
std::atomic<int> g_inFlight{0};          // EvaluateFeature calls inside NGX right now (any thread)
std::atomic<int> g_creating{0};          // CreateFeature calls inside NGX right now (DCS's loading thread)
std::atomic<int> g_releasing{0};         // our ReleaseFeature calls in progress (CreateHook waits for them)
std::atomic<uint64_t> g_lastRelEval{0};  // g_evalTotal at the last release (one set per frame)
std::atomic<uint64_t> g_ourSets{0}, g_ourFeats{0}, g_refusedEvals{0}, g_swallowed{0}, g_refusedSets{0}, g_relFaults{0},
    g_relFailed{0}, g_relBusy{0};
const char* g_latchWhy = "";
bool g_relLogged = false;

inline int64_t Now() {
  if (g_testClock) return g_testClock();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}
inline double Sec(int64_t d) {
  static const int64_t f = [] {
    LARGE_INTEGER x;
    QueryPerformanceFrequency(&x);
    return x.QuadPart;
  }();
  return static_cast<double>(d) / static_cast<double>(f);
}
inline int64_t Ticks(double s) { return static_cast<int64_t>(s / Sec(1)); }
inline char ClassLetter(int c) { return c == kPeriph ? 'P' : c == kFocus ? 'F' : 'U'; }
inline double MB(int64_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); }

void ResetEntry(Feat& f) {
  f.handle = nullptr, f.id = f.seq = f.set = f.load = 0, f.cls = kUnknown, f.outW = f.outH = 0, f.createQpc = 0;
  f.evals = 0, f.evalsAfterSuper = 0, f.firstEval = 0, f.lastEval = 0, f.superQpc = 0, f.superBy = 0;
  f.released = false, f.evalsAtSummary = 0;
  f.vramOk = f.privOk = f.statsOk = false, f.vramDelta = f.privDelta = f.statsDelta = 0;
  f.gateGen = 0, f.gateEvals = 0, f.ours = false, f.relResult = 0, f.refused = false;
}

// The newest live entry for a handle pointer (NGX may reuse a released handle's memory), -1 if none.
inline int Find(const void* h) {
  const uint32_t n = g_n.load(std::memory_order_acquire);
  for (uint32_t i = n; i-- > 0;)
    if (g_f[i].handle == h && !g_f[i].released.load(std::memory_order_relaxed)) return static_cast<int>(i);
  return -1;
}
// No C++ objects with destructors (SEH). *idx: the entry counted (-1 untracked).
void CountEvalImpl(const void* h, int r, int* idx) {
  g_evalTotal.fetch_add(1, std::memory_order_relaxed);
  if (r != kNgxSuccess) g_evalFail.fetch_add(1, std::memory_order_relaxed);
  const int i = Find(h);
  *idx = i;
  if (i < 0) {
    g_untracked.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Feat& f = g_f[i];
  const int64_t t = Now();
  f.evals.fetch_add(1, std::memory_order_relaxed);
  int64_t zero = 0;
  f.firstEval.compare_exchange_strong(zero, t, std::memory_order_relaxed);
  f.lastEval.store(t, std::memory_order_relaxed);
  if (f.superQpc.load(std::memory_order_relaxed)) f.evalsAfterSuper.fetch_add(1, std::memory_order_relaxed);
  // R132: evaluations since the last mission event (the release rule's proof counts only these).
  const uint32_t gen = g_gateGen.load(std::memory_order_acquire);
  if (f.gateGen.load(std::memory_order_relaxed) != gen) {
    f.gateEvals.store(0, std::memory_order_relaxed);
    f.gateGen.store(gen, std::memory_order_relaxed);
  }
  f.gateEvals.fetch_add(1, std::memory_order_relaxed);
}
int CountEval(const void* h, int r) {
  int idx = -1;
  __try {
    CountEvalImpl(h, r, &idx);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_evalFault.fetch_add(1, std::memory_order_relaxed);
    idx = -1;
  }
  return idx;
}
int FindGuarded(const void* h) {
  __try {
    return Find(h);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

// ---- cost readings ----
// DXGI local-segment CurrentUsage of the adapter of DCS's context. Raw COM, no destructors (SEH).
bool LocalUsageImpl(void* ctx, uint64_t* out) {
  auto* c = static_cast<ID3D11DeviceContext*>(ctx);
  ID3D11Device* dev = nullptr;
  c->GetDevice(&dev);
  if (!dev) return false;
  IDXGIDevice* dx = nullptr;
  IDXGIAdapter* a = nullptr;
  if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dx))) && dx) {
    dx->GetAdapter(&a);
    dx->Release();
  }
  dev->Release();
  if (!a) return false;
  IDXGIAdapter3* a3 = nullptr;
  a->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3));
  a->Release();
  if (!a3) return false;
  DXGI_QUERY_VIDEO_MEMORY_INFO i{};
  const bool ok = SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &i));
  a3->Release();
  if (ok) *out = i.CurrentUsage;
  return ok;
}
bool LocalUsage(void* ctx, uint64_t* out) {
  if (g_testVram) return g_testVram(ctx, out);
  if (!ctx) return false;
  __try {
    return LocalUsageImpl(ctx, out);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
bool PrivateBytes(uint64_t* out) {
  if (g_testPriv) return g_testPriv(out);
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
    return false;
  *out = pmc.PrivateUsage;
  return true;
}
// NGX_DLSS_GET_STATS (nvsdk_ngx_helpers.h) on our own capability block: the snippet's callback writes SizeInBytes
// (VRAM allocated by DLSS) into the block it is given. No destructors (SEH).
int CallCap(CapParamsFn fn, NgxParam** p) {
  __try {
    return fn(p);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}
bool StatsImpl(uint64_t* out) {
  if (g_statsCb(g_cap) != kNgxSuccess) return false;
  unsigned long long v = 0;
  if (g_cap->Get("SizeInBytes", &v) != kNgxSuccess) return false;
  *out = v;
  return true;
}
bool GetCbImpl(void** cb) { return g_cap->Get("DLSSGetStatsCallback", cb) == kNgxSuccess; }
bool GetCbGuarded(void** cb) {
  __try {
    return GetCbImpl(cb);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
void AcquireStats() {
  if (g_statsCb || g_capTries >= kMaxCapTries) return;
  ++g_capTries;
  CapParamsFn get = g_testCapParams;
  if (!get) {
    HMODULE core = GetModuleHandleW(kNgxCore);
    if (!core) return;
    get = reinterpret_cast<CapParamsFn>(GetProcAddress(core, "NVSDK_NGX_D3D11_GetCapabilityParameters"));
  }
  if (!get) return;
  if (!g_cap) {
    NgxParam* p = nullptr;
    if (CallCap(get, &p) != kNgxSuccess || !p) return;
    g_cap = p;
  }
  void* cb = nullptr;
  if (GetCbGuarded(&cb) && cb) {
    g_statsCb = reinterpret_cast<StatsCbFn>(cb);
    return;
  }
  // A block taken before the snippet was loaded has no callback: a fresh one next time (the old one is left; it is a
  // few KB, at most kMaxCapTries per payload).
  g_cap = nullptr;
}
bool DlssStats(uint64_t* out) {
  AcquireStats();
  if (!g_statsCb || !g_cap) return false;
  __try {
    return StatsImpl(out);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
struct Meas {
  bool vram = false, priv = false, stats = false;
  uint64_t local = 0, privBytes = 0, statsBytes = 0;
};
void Measure(void* ctx, Meas* m) {
  m->vram = LocalUsage(ctx, &m->local);
  m->priv = PrivateBytes(&m->privBytes);
  m->stats = DlssStats(&m->statsBytes);
}

// ---- summaries (under g_mx) ----
inline bool Live(const Feat& f) { return !f.released.load(std::memory_order_relaxed); }
// The newest set with a live entry (0 = none).
uint32_t NewestLiveSet(uint32_t n) {
  for (uint32_t i = n; i-- > 0;)
    if (Live(g_f[i])) return g_f[i].set;
  return 0;
}
// The release rule's verdict for an older set (observation only): 1 eligible, 0 not yet (the newest set has not been
// evaluated N times by each handle), -1 refused by the guard (evaluated after it was superseded), -2 not superseded.
struct Rule {
  uint64_t newestMin = 0;
  bool newestKnown = false;
};
Rule NewestMin(uint32_t n, uint32_t newest) {
  Rule r;
  for (uint32_t i = 0; i < n; ++i) {
    const Feat& f = g_f[i];
    if (f.set != newest || !Live(f)) continue;
    const uint64_t e = f.evals.load(std::memory_order_relaxed);
    r.newestMin = r.newestKnown ? (std::min)(r.newestMin, e) : e;
    r.newestKnown = true;
  }
  return r;
}
int SetVerdict(uint32_t n, uint32_t set, const Rule& nr, uint64_t minEvals) {
  bool any = false;
  for (uint32_t i = 0; i < n; ++i) {
    const Feat& f = g_f[i];
    if (f.set != set || !Live(f)) continue;
    any = true;
    if (!f.superQpc.load(std::memory_order_relaxed)) return -2;
    if (f.evalsAfterSuper.load(std::memory_order_relaxed)) return -1;
  }
  if (!any) return -2;
  return nr.newestKnown && nr.newestMin >= minEvals ? 1 : 0;
}

// Appends to a bounded line buffer.
struct Line {
  char b[1800];
  size_t n = 0;
  bool cut = false;
  void Add(const char* fmt, ...) {
    if (n >= sizeof(b) - 1) {
      cut = true;
      return;
    }
    va_list a;
    va_start(a, fmt);
    const int w = vsnprintf(b + n, sizeof(b) - n, fmt, a);
    va_end(a);
    if (w < 0 || static_cast<size_t>(w) >= sizeof(b) - n) {
      n = sizeof(b) - 1;
      cut = true;
    } else {
      n += static_cast<size_t>(w);
    }
  }
};

// One summary: a header, one line per set that is newest or still evaluated, one line for the idle older sets, one
// rule line. why: "periodic" / "mission load".
void SummaryLocked(const char* why, int64_t now) {
  const uint32_t n = g_n.load(std::memory_order_acquire);
  const uint64_t ev = g_evalTotal.load(), un = g_untracked.load(), fl = g_evalFail.load(), rl = g_releases.load();
  uint32_t live = 0, minSet = 0, maxSet = 0;
  for (uint32_t i = 0; i < n; ++i)
    if (Live(g_f[i])) {
      ++live;
      if (!minSet || g_f[i].set < minSet) minSet = g_f[i].set;
      maxSet = (std::max)(maxSet, g_f[i].set);
    }
  const uint32_t newest = NewestLiveSet(n);
  Log("dlss life: summary (%s, %.1f s since the last): %u live DLSS handles in sets %u-%u (newest set %u, load %u); "
      "%llu evaluations since the last summary (%llu of untracked handles, %llu failed); %llu releases seen%s",
      why, g_lastSummary ? Sec(now - g_lastSummary) : 0.0, live, minSet, maxSet, newest, g_load,
      static_cast<unsigned long long>(ev - g_evalAtSummary), static_cast<unsigned long long>(un - g_untrackedAtSummary),
      static_cast<unsigned long long>(fl - g_failAtSummary), static_cast<unsigned long long>(rl),
      g_dropped.load() ? " (entry table full: later creations not tracked)" : "");
  if (g_ourSets.load() || g_refusedEvals.load() || g_swallowed.load())  // R132
    Log("dlss life:   released by DcsQvCull: %llu set(s), %llu feature(s); %llu evaluation(s) of them refused, %llu DCS "
        "release(s) of them swallowed",
        static_cast<unsigned long long>(g_ourSets.load()), static_cast<unsigned long long>(g_ourFeats.load()),
        static_cast<unsigned long long>(g_refusedEvals.load()), static_cast<unsigned long long>(g_swallowed.load()));
  // Sets, newest first.
  Line idle;
  idle.Add("dlss life:   IDLE older sets (no evaluation since superseded):");
  uint32_t idleSets = 0, idleHandles = 0, idleListed = 0;
  for (uint32_t s = maxSet; s >= minSet && s > 0; --s) {
    uint32_t cnt = 0, firstId = 0, lastId = 0, load = 0, by = 0;
    uint64_t after = 0, lifetime = 0;
    int64_t sup = 0, last = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const Feat& f = g_f[i];
      if (f.set != s || !Live(f)) continue;
      if (!cnt) firstId = f.id, load = f.load, sup = f.superQpc.load(), by = f.superBy.load();
      lastId = f.id;
      ++cnt;
      after += f.evalsAfterSuper.load();
      lifetime += f.evals.load();
      last = (std::max)(last, f.lastEval.load());
    }
    if (!cnt) continue;
    if (s != newest && sup && !after) {  // idle older set
      ++idleSets, idleHandles += cnt;
      if (idleListed < 8) {
        ++idleListed;
        if (last)
          idle.Add(" set %u (ids %u-%u, load %u, superseded by set %u %.1f s ago; %llu evals before, last %.1f s ago);", s,
                   firstId, lastId, load, by, Sec(now - sup), static_cast<unsigned long long>(lifetime), Sec(now - last));
        else
          idle.Add(" set %u (ids %u-%u, load %u, superseded by set %u %.1f s ago; never evaluated);", s, firstId, lastId,
                   load, by, Sec(now - sup));
      }
      continue;
    }
    Line l;
    if (s == newest)
      l.Add("dlss life:   set %u (newest, load %u):", s, load);
    else
      l.Add("dlss life:   set %u (load %u, superseded by set %u %.1f s ago): STILL EVALUATED (%llu since superseded):", s,
            load, by, sup ? Sec(now - sup) : 0.0, static_cast<unsigned long long>(after));
    for (uint32_t i = 0; i < n; ++i) {
      Feat& f = g_f[i];
      if (f.set != s || !Live(f)) continue;
      const uint64_t e = f.evals.load(), le = f.lastEval.load();
      if (le)
        l.Add(" id %u %c +%llu (total %llu) last %.2f s ago;", f.id, ClassLetter(f.cls),
              static_cast<unsigned long long>(e - f.evalsAtSummary), static_cast<unsigned long long>(e), Sec(now - le));
      else
        l.Add(" id %u %c never evaluated;", f.id, ClassLetter(f.cls));
    }
    Log("%s%s", l.b, l.cut ? " ..." : "");
  }
  if (idleSets) {
    if (idleSets > idleListed) idle.Add(" and %u more set(s)", idleSets - idleListed);
    idle.Add(" [%u handle(s)]", idleHandles);
    Log("%s%s", idle.b, idle.cut ? " ..." : "");
  }
  // The release rule, as it would decide now (R130: observation only; R132: ReleaseIdleSets=1 acts on it).
  if (newest && minSet < newest) {
    const Rule nr = NewestMin(n, newest);
    const uint64_t ruleN = g_relKey ? g_relN.load() : kRuleMinEvals;
    Line r;
    r.Add("dlss life:   release rule (%s, N=%llu; newest set %u min %llu evals per handle):",
          g_relKey ? "ReleaseIdleSets=1" : "observation only", static_cast<unsigned long long>(ruleN), newest,
          static_cast<unsigned long long>(nr.newestMin));
    for (uint32_t s = minSet; s < newest; ++s) {
      const int v = SetVerdict(n, s, nr, ruleN);
      if (v == -2) continue;
      r.Add(" set %u %s;", s, v == 1 ? "eligible" : v == 0 ? "not yet (newest set below N)" : "REFUSED (guard: evaluated after superseded)");
    }
    Log("%s%s", r.b, r.cut ? " ..." : "");
  }
  for (uint32_t i = 0; i < n; ++i) g_f[i].evalsAtSummary = g_f[i].evals.load();
  g_lastIdle = ev == g_evalAtSummary && !g_changes;
  g_evalAtSummary = ev, g_untrackedAtSummary = un, g_failAtSummary = fl, g_releasesAtSummary = rl;
  g_lastSummary = now;
  g_changes = 0;
}

// Per-set totals of a load that has closed.
void LoadTotalsLocked(int64_t now) {
  const uint32_t n = g_n.load(std::memory_order_acquire);
  Line l;
  uint32_t creates = 0, sets = 0, lastSet = 0;
  int64_t tv = 0, tp = 0, ts = 0;
  bool okv = true, okp = true, oks = true;
  for (uint32_t i = g_loadFirst; i < n; ++i) creates += g_f[i].load == g_load;
  l.Add("dlss life: load %u done: %u DLSS creations over %.1f ms;", g_load, creates, Sec(g_lastCreate - g_loadStart) * 1000.0);
  for (uint32_t i = g_loadFirst; i < n; ++i) {
    const Feat& f = g_f[i];
    if (f.load != g_load || f.set == lastSet) continue;
    lastSet = f.set;
    ++sets;
    int64_t v = 0, p = 0, s = 0;
    bool sv = true, sp = true, ss = true;
    uint32_t firstId = 0, lastId = 0;
    char pat[40] = {};
    size_t pn = 0;
    for (uint32_t j = i; j < n; ++j) {
      const Feat& g = g_f[j];
      if (g.set != f.set) continue;
      if (!firstId) firstId = g.id;
      lastId = g.id;
      if (pn + 2 < sizeof(pat)) {
        if (pn) pat[pn++] = ',';
        pat[pn++] = ClassLetter(g.cls);
      }
      v += g.vramDelta, p += g.privDelta, s += g.statsDelta;
      sv = sv && g.vramOk, sp = sp && g.privOk, ss = ss && g.statsOk;
    }
    tv += v, tp += p, ts += s;
    okv = okv && sv, okp = okp && sp, oks = oks && ss;
    l.Add(" set %u ids %u-%u (%s): VRAM %s%+.1f MB, private %s%+.1f MB, DLSS stats %s%+.1f MB;", f.set, firstId, lastId,
          pat, sv ? "" : "~", MB(v), sp ? "" : "~", MB(p), ss ? "" : "~", MB(s));
  }
  l.Add(" load total VRAM %s%+.1f MB, private %s%+.1f MB, DLSS stats %s%+.1f MB (~ = some readings missing)",
        okv ? "" : "~", MB(tv), okp ? "" : "~", MB(tp), oks ? "" : "~", MB(ts));
  (void)sets;
  (void)now;
  Log("%s%s", l.b, l.cut ? " ..." : "");
}

// R132: one line with the release counters (at each mission load, at the shutdown signal, at a latch).
void RelStatus(const char* why) {
  Log("dlss release: status %s: %llu set(s) / %llu feature(s) released this session; %llu evaluation(s) of a released "
      "handle refused; %llu DCS release(s) of one swallowed; %llu set(s) refused by the guard; %llu fault(s), %llu failed "
      "release(s); %s%s",
      why, static_cast<unsigned long long>(g_ourSets.load()), static_cast<unsigned long long>(g_ourFeats.load()),
      static_cast<unsigned long long>(g_refusedEvals.load()), static_cast<unsigned long long>(g_swallowed.load()),
      static_cast<unsigned long long>(g_refusedSets.load()), static_cast<unsigned long long>(g_relFaults.load()),
      static_cast<unsigned long long>(g_relFailed.load()),
      g_relLatched.load() ? "OFF for this session: " : g_noMore.load() ? "stopped" : g_relOn.load() ? "on" : "paused",
      g_relLatched.load() ? g_latchWhy : "");
}

// A DLSS-SR creation (from CreateHook, after the original). t0: just before the original. Returns the text appended to
// the create line.
void NoteCreate(int64_t t0, uint32_t seq, void** handle, int cls, uint32_t outW, uint32_t outH, int r, const Meas& m0,
                const Meas& m1, char* tail, size_t tn) {
  std::lock_guard<std::mutex> lock(g_mx);
  const bool newLoad = !g_load || t0 - g_lastCreate > Ticks(kLoadGapSec);
  if (newLoad) {
    if (g_loadOpen) LoadTotalsLocked(t0);  // (a load closed by the next one before the worker saw it)
    if (g_load && g_verbose) SummaryLocked("mission load", t0);
    if (g_load && g_relKey) RelStatus("at a mission load");  // R132
    ++g_load;
    g_loadOpen = true;
    g_loadStart = t0;
    g_loadFirst = g_n.load();
  }
  const uint32_t n = g_n.load();
  bool seen = false;
  for (uint32_t i = g_setFirst; i < n && !newLoad; ++i)
    if (g_f[i].set == g_set && g_f[i].outW == outW && g_f[i].outH == outH) seen = true;
  const bool newSet = newLoad || (seen && (g_prevW != outW || g_prevH != outH));
  if (newSet) {
    if (g_set)
      for (uint32_t i = 0; i < n; ++i) {
        int64_t z = 0;
        if (Live(g_f[i]) && g_f[i].superQpc.compare_exchange_strong(z, t0)) g_f[i].superBy = g_set + 1;
      }
    ++g_set;
    g_setFirst = n;
  }
  g_prevW = outW, g_prevH = outH;
  g_lastCreate = t0;
  ++g_changes;
  g_newestSetA.store(g_set, std::memory_order_release);  // R132
  g_lastCreateA.store(t0, std::memory_order_release);
  g_changeGen.fetch_add(1, std::memory_order_acq_rel);
  void* h = nullptr;
  uint32_t id = 0;
  const bool haveH = handle && SafeRead(handle, &h, sizeof(h)) && h;
  if (haveH) SafeRead(h, &id, sizeof(id));
  const int64_t dv = static_cast<int64_t>(m1.local - m0.local), dp = static_cast<int64_t>(m1.privBytes - m0.privBytes),
                ds = static_cast<int64_t>(m1.statsBytes - m0.statsBytes);
  const bool okv = m0.vram && m1.vram, okp = m0.priv && m1.priv, oks = m0.stats && m1.stats;
  char sv[32] = "n/a", sp[32] = "n/a", ss[32] = "n/a";
  if (okv) snprintf(sv, sizeof(sv), "%+.1f MB", MB(dv));
  if (okp) snprintf(sp, sizeof(sp), "%+.1f MB", MB(dp));
  if (oks) snprintf(ss, sizeof(ss), "%+.1f MB", MB(ds));
  snprintf(tail, tn, "; life: set %u, load %u; VRAM local %s, private %s, DLSS stats %s", g_set, g_load, sv, sp, ss);
  if (r != kNgxSuccess || !haveH) return;  // nothing to track
  if (n >= kMax) {
    g_dropped.fetch_add(1);
    if (!g_fullLogged) Log("dlss life: entry table full (%u); later DLSS creations are not tracked", kMax);
    g_fullLogged = true;
    return;
  }
  Feat& f = g_f[n];
  ResetEntry(f);
  f.handle = h, f.id = id, f.seq = seq, f.set = g_set, f.load = g_load, f.cls = cls, f.outW = outW, f.outH = outH;
  f.createQpc = t0;
  f.vramOk = okv, f.privOk = okp, f.statsOk = oks, f.vramDelta = dv, f.privDelta = dp, f.statsDelta = ds;
  g_n.store(n + 1, std::memory_order_release);
}

void NoteRelease(int i, const void* h, int r) {
  std::lock_guard<std::mutex> lock(g_mx);
  ++g_changes;
  if (r != kNgxSuccess) {
    Log("dlss life: ReleaseFeature(0x%llx) by DCS failed 0x%x", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)),
        static_cast<unsigned>(r));
    return;
  }
  g_releases.fetch_add(1);
  if (i < 0) {
    Log("dlss life: DCS released an untracked DLSS handle 0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)));
    return;
  }
  Feat& f = g_f[i];
  f.released = true;
  const int64_t le = f.lastEval.load();
  char last[48] = "never";
  if (le) snprintf(last, sizeof(last), "last %.1f s ago", Sec(Now() - le));
  Log("dlss life: DCS released id %u (set %u, load %u, %c): evaluated %llu times, %s", f.id, f.set, f.load,
      ClassLetter(f.cls), static_cast<unsigned long long>(f.evals.load()), last);
}

// The trampoline: g_*Orig, or (in the instant between the detour's slot switch and the g_*Orig store, which DCS's
// render thread can hit: it evaluates every frame) the detour's own trampoline, written before the switch.
inline EvalFn EvalOrig() {
  const EvalFn o = g_evalOrig.load(std::memory_order_acquire);
  return o ? o : reinterpret_cast<EvalFn>(g_eval.tramp);
}
inline ReleaseFn ReleaseOrig() {
  const ReleaseFn o = g_releaseOrig.load(std::memory_order_acquire);
  return o ? o : reinterpret_cast<ReleaseFn>(g_release.tramp);
}

// ---- R132: release of idle DLSS sets ([DLSS] ReleaseIdleSets=1, opt-in, default 0) ---------------------------------
// R131 (live): at every load DCS creates two sets of four features about 1 s apart and evaluates only the second; the
// previous load's sets are never evaluated again; DCS never calls ReleaseFeature; it also creates a set of four while
// it closes. Each idle set holds about 0.66-0.7 GB of private memory. R130's harness: a release returns nothing at
// once (NGX keeps the memory) but the next creations reuse it, so releasing stops the growth per load.
// The rule (agreed with Dev):
//   - Candidates: an older set none of whose handles was evaluated after a newer set was created
//     (evalsAfterSuper == 0 on every handle; a set evaluated BEFORE it was superseded qualifies).
//   - Proof of replacement: every live handle of the newest set has >= N evaluations ([DLSS] ReleaseAfterEvals,
//     default 300) since the last mission event (onMissionLoadBegin / onSimulationStop / slot change) and since its
//     creation, and the load has closed (2 s without a creation).
//   - Guard before: a candidate handle evaluated after it was superseded (even once) refuses its set (logged once;
//     stricter than "for this load": the count never goes back to 0, so for the session).
//   - Guard after: the released handles are kept in a persistent guard block (never freed; found again by a later
//     payload through the process environment). An evaluate of one never reaches NGX: it returns
//     NVSDK_NGX_Result_FAIL_FeatureNotFound and switches releasing off for the session. A ReleaseFeature by DCS on
//     one is swallowed (success, no double free). After a payload stop the two hooks' slots point at small persistent
//     filters in the guard block that do the same lookup in front of the trampolines. A new creation that gets a
//     released handle's address back (NGX reuses memory) clears that address from the block first.
//   - Where: on DCS's render thread, in the evaluate hook of a newest-set handle after NGX returned success; one set
//     per frame (every newest handle evaluated once more in between); never while a load is open, never after the
//     payload stop or DCS's shutdown signal. The set created while DCS closes is the newest set and never evaluated:
//     the proof never holds after it, so it (and everything older) is left alone.
//   - Safety: the release step runs under SEH (our bookkeeping and NGX's ReleaseFeature); a fault, a failed release
//     or a full guard block latches releasing off; handles already in the block stay guarded.
//   - Logging: one line per release (set, handles, why, private and DLSS-stats deltas), one line per refused set, a
//     status line at each mission load and at shutdown.
constexpr uint32_t kGuardMagic = 0x32333152;  // "R132"
constexpr uint32_t kGuardCap = (kPage - 64) / 8;
const wchar_t* const kGuardEnv = L"DCSQV_DLSS_RELEASE_GUARD";
void* const kTomb = reinterpret_cast<void*>(static_cast<uintptr_t>(1));  // a cleared slot (never a handle)
struct GuardData {  // the block's data page (read-write)
  uint32_t magic;
  std::atomic<uint32_t> count;  // slots in use (cleared ones included); the filters scan [0, count)
  void* evalTramp;              // where the filters go on: the detours' trampolines
  void* relTramp;
  uint64_t reserved[5];
  std::atomic<void*> h[kGuardCap];
};
static_assert(sizeof(GuardData) == kPage, "one page");
constexpr size_t kEvalFilterOff = 0, kRelFilterOff = 64;
uint8_t* g_guardBase = nullptr;  // code page (+kPage: GuardData)
inline GuardData* Gd() { return g_guardBase ? reinterpret_cast<GuardData*>(g_guardBase + kPage) : nullptr; }

// The persistent filter (x64, 45 bytes): the handle (rdx for EvaluateFeature, rcx for ReleaseFeature) against the
// block's slots; a match returns `refuse` without calling NGX, anything else jumps to the trampoline untouched.
//   mov r10d,[count]; lea rax,[h]; test r10,r10; jz pass; loop: cmp rdx|rcx,[rax]; je refuse; add rax,8; dec r10;
//   jnz loop; pass: jmp [tramp]; refuse: mov eax,imm32; ret
void WriteFilter(uint8_t* code, bool handleInRdx, int32_t refuse, size_t trampOff) {
  uint8_t* data = g_guardBase + kPage;
  const auto rel = [](const uint8_t* at, size_t len, const uint8_t* target) {
    return static_cast<int32_t>(target - (at + len));
  };
  uint8_t c[45] = {0x44, 0x8B, 0x15, 0, 0, 0, 0, 0x48, 0x8D, 0x05, 0, 0, 0, 0, 0x4D, 0x85, 0xD2, 0x74, 0x0E,
                   0x48, 0x3B, static_cast<uint8_t>(handleInRdx ? 0x10 : 0x08), 0x74, 0x0F, 0x48, 0x83, 0xC0, 0x08,
                   0x49, 0xFF, 0xCA, 0x75, 0xF2, 0xFF, 0x25, 0, 0, 0, 0, 0xB8, 0, 0, 0, 0, 0xC3};
  const int32_t dCount = rel(code + 0, 7, data + offsetof(GuardData, count));
  const int32_t dH = rel(code + 7, 7, data + offsetof(GuardData, h));
  const int32_t dT = rel(code + 33, 6, data + trampOff);
  memcpy(c + 3, &dCount, 4);
  memcpy(c + 10, &dH, 4);
  memcpy(c + 35, &dT, 4);
  memcpy(c + 40, &refuse, 4);
  memcpy(code, c, sizeof(c));
}
// An earlier payload's guard block (named by the process environment), taken over so that its released handles stay
// guarded through this payload's hooks. Any key that installs a DLSS hook adopts it (a payload with only Instrument or
// PeripheryPreset must not lose the guard). True when this payload has a block.
bool GuardAdopt() {
  if (g_guardBase) return true;
  wchar_t v[32] = {};
  if (GetEnvironmentVariableW(kGuardEnv, v, 32) > 0) {
    const unsigned long long a = wcstoull(v, nullptr, 16);
    uint8_t* base = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(a));
    uint32_t magic = 0;
    uint8_t code[3] = {};
    if (base && !(a & (kPage - 1)) && SafeRead(base + kPage, &magic, 4) && magic == kGuardMagic &&
        SafeRead(base, code, 3) && code[0] == 0x44 && code[1] == 0x8B && code[2] == 0x15) {
      g_guardBase = base;
      GuardData* d = Gd();
      if (g_eval.tramp) d->evalTramp = g_eval.tramp;
      if (g_release.tramp) d->relTramp = g_release.tramp;
      uint32_t live = 0;
      for (uint32_t i = 0; i < d->count.load(); ++i) live += d->h[i].load() != kTomb;
      Log("dlss release: guard block of an earlier payload adopted (%u released handle(s) still guarded)", live);
      return true;
    }
  }
  return false;
}
// The guard block: this payload's, an earlier payload's (adopted), or a new one. False: no memory.
bool GuardEnsure() {
  if (GuardAdopt()) return true;
  auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 2 * kPage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!p) return false;
  memset(p, 0xCC, kPage);
  g_guardBase = p;
  GuardData* d = Gd();
  d->magic = kGuardMagic;
  d->evalTramp = g_eval.tramp;
  d->relTramp = g_release.tramp;
  WriteFilter(p + kEvalFilterOff, true, kNgxFeatureNotFound, offsetof(GuardData, evalTramp));
  WriteFilter(p + kRelFilterOff, false, kNgxSuccess, offsetof(GuardData, relTramp));
  DWORD old;
  if (!VirtualProtect(p, kPage, PAGE_EXECUTE_READ, &old)) {
    g_guardBase = nullptr;
    VirtualFree(p, 0, MEM_RELEASE);
    return false;
  }
  FlushInstructionCache(GetCurrentProcess(), p, kPage);
  wchar_t s[32];
  swprintf_s(s, L"%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p)));
  SetEnvironmentVariableW(kGuardEnv, s);
  return true;
}
// Lock-free: is h a handle we released (or are releasing)? Any thread.
inline bool GuardHas(const void* h) {
  GuardData* d = Gd();
  if (!d || !h) return false;
  const uint32_t n = d->count.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n; ++i)
    if (d->h[i].load(std::memory_order_relaxed) == h) return true;
  return false;
}
inline bool GuardAny() {
  GuardData* d = Gd();
  return d && d->count.load(std::memory_order_relaxed) != 0;
}
// Under g_mx. The slot used, -1 when full.
int GuardAdd(void* h) {
  GuardData* d = Gd();
  if (!d) return -1;
  const uint32_t n = d->count.load();
  for (uint32_t i = 0; i < n; ++i)
    if (d->h[i].load() == kTomb) {
      d->h[i].store(h, std::memory_order_seq_cst);
      return static_cast<int>(i);
    }
  if (n >= kGuardCap) return -1;
  d->h[n].store(h, std::memory_order_seq_cst);
  d->count.store(n + 1, std::memory_order_seq_cst);
  return static_cast<int>(n);
}
void GuardClear(int slot) {
  GuardData* d = Gd();
  if (d && slot >= 0) d->h[slot].store(kTomb, std::memory_order_seq_cst);
}
// A new feature got the address of a handle we released: it is DCS's new feature now (CreateHook, after NGX returned).
uint32_t GuardForget(const void* h) {
  GuardData* d = Gd();
  if (!d || !h) return 0;
  uint32_t k = 0;
  const uint32_t n = d->count.load();
  for (uint32_t i = 0; i < n; ++i)
    if (d->h[i].load() == h) d->h[i].store(kTomb), ++k;
  return k;
}

void Latch(const char* why) {
  if (g_relLatched.exchange(true)) return;
  g_relOn = false;
  g_latchWhy = why;
  Log("dlss release: OFF for this session: %s; released handles stay guarded", why);
}

// The entry (newest first, released ones included) for a handle, -1 if none. Logging only.
int FindAny(const void* h) {
  const uint32_t n = g_n.load(std::memory_order_acquire);
  for (uint32_t i = n; i-- > 0;)
    if (g_f[i].handle == h) return static_cast<int>(i);
  return -1;
}
// An evaluate of a handle we released: refused before NGX (the guard after).
int RefuseEval(const void* h) {
  const uint64_t k = g_refusedEvals.fetch_add(1) + 1;
  try {
    if (k <= 8) {
      const int i = FindAny(h);
      Log("dlss release: DCS evaluated handle 0x%llx (id %u, set %u) that DcsQvCull released: refused before NGX "
          "(0x%x FeatureNotFound)%s",
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)), i >= 0 ? g_f[i].id : 0u,
          i >= 0 ? g_f[i].set : 0u, static_cast<unsigned>(kNgxFeatureNotFound), k == 8 ? "; further ones counted only" : "");
    }
    Latch("DCS evaluated a released handle (guard after)");
  } catch (...) {
  }
  return kNgxFeatureNotFound;
}

// The release step (render thread, under g_mx). No C++ objects with destructors (SEH).
struct RelOut {
  int what = 0;  // 0 nothing to do, 1 released, 2 set refused (guard before), 3 busy (retry), 4 guard full, 5 release failed
  uint32_t set = 0, by = 0, load = 0, count = 0, newest = 0, firstId = 0, lastId = 0;
  int idx[16] = {};
  uint64_t newestMin = 0, lifetime = 0, after = 0;
  int64_t supQpc = 0, lastEval = 0;
  bool privOk = false, statsOk = false;
  int64_t privDelta = 0, statsDelta = 0;
  int failResult = 0;
  bool proof = false;  // the newest set's proof held (what 0: then no candidate)
};
// Tests: the release step faults on this handle (stands for a fault inside NGX's ReleaseFeature or our bookkeeping).
const void* g_testFaultOn = nullptr;
void ReleaseStepImpl(int64_t now, RelOut* o) {
  const uint32_t n = g_n.load(std::memory_order_acquire);
  const uint32_t newest = g_set;
  const uint64_t need = g_relN.load();
  const uint32_t gen = g_gateGen.load(std::memory_order_acquire);
  // Proof: every live handle of the newest set evaluated >= N times since the last event.
  uint64_t mn = 0;
  uint32_t nh = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const Feat& f = g_f[i];
    if (f.set != newest || !Live(f)) continue;
    const uint64_t e = f.gateGen.load() == gen ? f.gateEvals.load() : 0;
    mn = nh ? (std::min)(mn, e) : e;
    ++nh;
  }
  o->newest = newest, o->newestMin = mn;
  if (!nh || mn < need || now - g_lastCreate <= Ticks(kLoadDoneSec)) return;  // (the load closes 2 s after its last creation)
  o->proof = true;
  // The oldest candidate set.
  for (uint32_t s = 1; s < newest; ++s) {
    uint32_t cnt = 0;
    bool unsup = false, used = false, refused = false;
    for (uint32_t i = 0; i < n; ++i) {
      const Feat& f = g_f[i];
      if (f.set != s || !Live(f)) continue;
      ++cnt;
      unsup = unsup || !f.superQpc.load();
      used = used || f.evalsAfterSuper.load() != 0;
      refused = refused || f.refused;
    }
    if (!cnt || unsup || refused || cnt > 16) continue;
    o->set = s, o->count = 0, o->lifetime = 0, o->after = 0, o->lastEval = 0;
    for (uint32_t i = 0; i < n; ++i) {
      Feat& f = g_f[i];
      if (f.set != s || !Live(f)) continue;
      if (!o->count) o->firstId = f.id, o->load = f.load, o->by = f.superBy.load(), o->supQpc = f.superQpc.load();
      o->lastId = f.id;
      o->idx[o->count++] = static_cast<int>(i);
      o->lifetime += f.evals.load();
      o->after += f.evalsAfterSuper.load();
      o->lastEval = (std::max)(o->lastEval, f.lastEval.load());
      if (used) f.refused = true;
    }
    if (used) {  // the guard before
      o->what = 2;
      return;
    }
    // Guard the handles first, then make sure no evaluate is inside NGX right now (another thread could hold one of
    // them): any evaluate that starts from here on sees the guard.
    int slots[16];
    for (uint32_t k = 0; k < o->count; ++k) {
      slots[k] = GuardAdd(g_f[o->idx[k]].handle);
      if (slots[k] < 0) {
        for (uint32_t j = 0; j < k; ++j) GuardClear(slots[j]);
        o->what = 4;
        return;
      }
    }
    bool late = false;
    for (uint32_t k = 0; k < o->count; ++k) late = late || g_f[o->idx[k]].evalsAfterSuper.load() != 0;
    // No NGX creation may run beside our release (DCS creates on another thread, e.g. the set made while it closes):
    // CreateHook counts itself in before calling NGX and waits while g_releasing is set.
    g_releasing.store(1, std::memory_order_seq_cst);
    if (g_inFlight.load(std::memory_order_seq_cst) != 0 || g_creating.load(std::memory_order_seq_cst) != 0 || late) {
      g_releasing.store(0, std::memory_order_seq_cst);
      for (uint32_t k = 0; k < o->count; ++k) GuardClear(slots[k]);
      if (late) {
        for (uint32_t k = 0; k < o->count; ++k) g_f[o->idx[k]].refused = true;
        o->what = 2;
      } else {
        o->what = 3;
      }
      return;
    }
    uint64_t p0 = 0, p1 = 0, s0 = 0, s1 = 0;
    const bool pa = PrivateBytes(&p0), sa = DlssStats(&s0);
    const ReleaseFn rel = ReleaseOrig();
    int bad = 0;
    for (uint32_t k = 0; k < o->count; ++k) {
      Feat& f = g_f[o->idx[k]];
      if (g_testFaultOn && f.handle == g_testFaultOn) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
      f.relResult = rel(f.handle);  // NGX frees the feature (the handle stays in the guard block)
      f.ours = true;
      f.released = true;
      if (f.relResult != kNgxSuccess && !bad) bad = f.relResult;
    }
    g_releasing.store(0, std::memory_order_seq_cst);
    o->privOk = pa && PrivateBytes(&p1);
    o->statsOk = sa && DlssStats(&s1);
    o->privDelta = static_cast<int64_t>(p1 - p0), o->statsDelta = static_cast<int64_t>(s1 - s0);
    o->what = bad ? 5 : 1;
    o->failResult = bad;
    return;
  }
}
bool ReleaseStep(int64_t now, RelOut* o) {
  __try {
    ReleaseStepImpl(now, o);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_releasing.store(0, std::memory_order_seq_cst);
    return false;
  }
}
void ReportRelease(const RelOut& o, int64_t now) {
  Line l;
  l.Add("dlss release: released set %u (load %u; ids %u-%u;", o.set, o.load, o.firstId, o.lastId);
  for (uint32_t k = 0; k < o.count; ++k) {
    const Feat& f = g_f[o.idx[k]];
    l.Add(" %c 0x%llx%s", ClassLetter(f.cls), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(f.handle)),
          f.relResult == kNgxSuccess ? "" : " (NGX FAILED)");
  }
  char last[64] = "never evaluated";
  if (o.lastEval)
    snprintf(last, sizeof(last), "%llu evaluations before (%u handles), last %.1f s ago",
             static_cast<unsigned long long>(o.lifetime), o.count, Sec(now - o.lastEval));
  char pv[32] = "n/a", sv[32] = "n/a";
  if (o.privOk) snprintf(pv, sizeof(pv), "%+.1f MB", MB(o.privDelta));
  if (o.statsOk) snprintf(sv, sizeof(sv), "%+.1f MB", MB(o.statsDelta));
  l.Add("): idle since superseded by set %u %.1f s ago (0 evaluations since; %s); newest set %u has >= %llu evaluations "
        "per handle since the last mission event (min %llu); private %s, DLSS stats %s; %llu set(s) / %llu feature(s) "
        "released this session",
        o.by, Sec(now - o.supQpc), last, o.newest, static_cast<unsigned long long>(g_relN.load()),
        static_cast<unsigned long long>(o.newestMin), pv, sv, static_cast<unsigned long long>(g_ourSets.load()),
        static_cast<unsigned long long>(g_ourFeats.load()));
  Log("%s%s", l.b, l.cut ? " ..." : "");
}

// After a successful evaluate of handle entry i (render thread). Cheap checks first; the step itself under try_lock.
void MaybeRelease(int i) {
  if (i < 0 || g_relLatched.load(std::memory_order_relaxed) || g_noMore.load(std::memory_order_relaxed)) return;
  const Feat& f = g_f[i];
  if (f.set != g_newestSetA.load(std::memory_order_acquire)) return;
  if (f.gateGen.load(std::memory_order_relaxed) != g_gateGen.load(std::memory_order_relaxed) ||
      f.gateEvals.load(std::memory_order_relaxed) < g_relN.load(std::memory_order_relaxed))
    return;
  const int64_t now = Now();
  if (now - g_lastCreateA.load(std::memory_order_acquire) <= Ticks(kLoadDoneSec)) return;
  const uint32_t cg = g_changeGen.load(std::memory_order_acquire);
  if (g_checkedGen.load(std::memory_order_relaxed) == cg) return;  // nothing to release since the last look
  const uint64_t ev = g_evalTotal.load(std::memory_order_relaxed);
  if (g_ourSets.load(std::memory_order_relaxed) && ev - g_lastRelEval.load(std::memory_order_relaxed) < 4) return;
  if (!g_mx.try_lock()) return;  // a creation or a summary right now: next evaluate
  RelOut o;
  const bool ok = ReleaseStep(now, &o);
  if (ok && o.what == 1) {
    g_ourSets.fetch_add(1);
    g_ourFeats.fetch_add(o.count);
  }
  if (ok && (o.what == 1 || o.what == 5)) {
    g_lastRelEval.store(ev);
    ++g_changes;
    g_changeGen.fetch_add(1);
  } else if (ok && o.what == 2) {
    g_refusedSets.fetch_add(1);
    ++g_changes;
    g_changeGen.fetch_add(1);  // look again: an older or newer candidate may remain
  } else if (ok && o.what == 0 && o.proof) {
    g_checkedGen.store(cg);  // proof held, no candidate: wait for a creation, an event or a release
  }
  g_mx.unlock();
  try {
    if (!ok) {
      g_relFaults.fetch_add(1);
      Latch("a fault in the release step (SEH)");
    } else if (o.what == 1) {
      ReportRelease(o, now);
    } else if (o.what == 5) {
      g_relFailed.fetch_add(1);
      ReportRelease(o, now);
      Latch("NGX ReleaseFeature failed");
    } else if (o.what == 2) {
      Log("dlss release: set %u (load %u; ids %u-%u) REFUSED for this session (guard before): %llu evaluation(s) after "
          "set %u was created; DCS still uses it",
          o.set, o.load, o.firstId, o.lastId, static_cast<unsigned long long>(o.after), o.by);
    } else if (o.what == 3) {
      g_relBusy.fetch_add(1);
    } else if (o.what == 4) {
      Latch("the guard block is full");
    }
  } catch (...) {
  }
}

int __cdecl EvalHook(void* ctx, const void* h, const NgxParam* p, void* cb) {
  g_inFlight.fetch_add(1, std::memory_order_seq_cst);
  if (GuardAny()) {  // R132 guard after: a handle we released never reaches NGX
    bool hit = false;
    __try {
      hit = GuardHas(h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      hit = false;
    }
    if (hit) {
      g_inFlight.fetch_sub(1, std::memory_order_seq_cst);
      return RefuseEval(h);
    }
  }
  const int r = EvalOrig()(ctx, h, p, cb);
  g_inFlight.fetch_sub(1, std::memory_order_seq_cst);
  if (g_on.load(std::memory_order_relaxed)) {
    const int i = CountEval(h, r);
    if (r == kNgxSuccess && g_relOn.load(std::memory_order_relaxed)) MaybeRelease(i);
  }
  return r;
}
int __cdecl ReleaseHook(void* h) {
  if (GuardAny() && GuardHas(h)) {  // R132: DCS releasing a handle we released: no double free
    const uint64_t k = g_swallowed.fetch_add(1) + 1;
    if (k <= 8) {
      try {
        const int i = FindAny(h);
        Log("dlss release: DCS released handle 0x%llx (id %u, set %u) that DcsQvCull had released: swallowed (success "
            "returned, NGX not called)",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)), i >= 0 ? g_f[i].id : 0u,
            i >= 0 ? g_f[i].set : 0u);
      } catch (...) {
      }
    }
    return kNgxSuccess;
  }
  const bool on = g_on.load(std::memory_order_relaxed);
  const int i = on ? FindGuarded(h) : -1;
  const ReleaseFn o = g_releaseOrig.load(std::memory_order_acquire);
  const int r = (o ? o : reinterpret_cast<ReleaseFn>(g_release.tramp))(h);
  if (on) {
    try {
      NoteRelease(i, h, r);
    } catch (...) {
    }
  }
  return r;
}

// Worker: the two observation hooks (once the core is up; the CreateFeature hook is the R122 one).
void Install() {
  for (int k = 0; k < 2; ++k) {
    std::atomic<int>& st = k ? g_releaseState : g_evalState;
    if (st.load() != 0) continue;
    Detour* d = k ? &g_release : &g_eval;
    uint8_t* target = k ? g_testReleaseTarget : g_testEvalTarget;
    if (!target) {
      HMODULE core = GetModuleHandleW(kNgxCore);
      if (!core) return;  // retried by the next tick
      target = reinterpret_cast<uint8_t*>(GetProcAddress(core, d->name));
      if (!target) {
        st = -1;
        Log("dlss life: the NGX core has no %s export; not instrumented", d->name);
        continue;
      }
    }
    const int r = InstallDetour(target, k ? reinterpret_cast<void*>(&ReleaseHook) : reinterpret_cast<void*>(&EvalHook), d);
    if (r < 0) {
      st = -1;
      Log("dlss life: %s not hooked: %s; not instrumented", d->name, DetourError(r));
      continue;
    }
    if (k)
      g_releaseOrig.store(reinterpret_cast<ReleaseFn>(d->tramp), std::memory_order_release);
    else
      g_evalOrig.store(reinterpret_cast<EvalFn>(d->tramp), std::memory_order_release);
    st = 1;
    Log("dlss life: %s hooked (%s; %s)", d->name, r == 2 ? "earlier payload's patch reused" : "patched",
        g_verbose || !g_relKey ? "observation only, pass-through" : "lifetime tracking for [DLSS] ReleaseIdleSets");
  }
}

// Worker tick: the keys, the kill switch, a closed load's totals, the 10 s summary (Instrument only). R132: release
// (ReleaseIdleSets) turns the tracking on too; releasing needs both observation hooks and the guard block.
void Tick(bool key, bool killed, bool release = false, int releaseAfter = static_cast<int>(kRuleMinEvals)) {
  g_key = key;
  g_verbose = key;
  g_relKey = release;
  g_relN = static_cast<uint64_t>(releaseAfter > 0 ? releaseAfter : static_cast<int>(kRuleMinEvals));
  const bool on = (key || release) && !killed;
  const bool was = g_on.exchange(on);
  if (on && key && !g_onLogged) {
    g_onLogged = true;
    Log("dlss life: on ([DLSS] Instrument=1, dev): DLSS creations, evaluations and releases are counted (read-only); "
        "a summary every %.0f s and at each mission load", kSummarySec);
  }
  // R132: releasing.
  const bool canRel = on && release && !g_relLatched.load() && !g_noMore.load() && g_evalState.load() == 1 &&
                      g_releaseState.load() == 1;
  bool relNow = false;
  if (canRel) {
    relNow = GuardEnsure();
    if (!relNow) Latch("no memory for the guard block");
  }
  const bool relWas = g_relOn.exchange(relNow);
  if (relNow && !relWas) {
    if (!g_relLogged)
      Log("dlss release: on ([DLSS] ReleaseIdleSets=1, ReleaseAfterEvals=%llu): DLSS feature sets DCS no longer "
          "evaluates are released on DCS's render thread once each handle of the newest set has %llu evaluations since "
          "the last mission event; one set per frame, never during a load; released handles are guarded",
          static_cast<unsigned long long>(g_relN.load()), static_cast<unsigned long long>(g_relN.load()));
    else
      Log("dlss release: resumed");
    g_relLogged = true;
  } else if (!relNow && relWas && !g_relLatched.load()) {
    Log("dlss release: paused (%s)", killed ? "kill switch" : !release ? "[DLSS] ReleaseIdleSets=0" : "stopping");
  }
  if (!on) {
    if (was) Log("dlss life: counting paused (%s)", killed ? "kill switch" : "[DLSS] Instrument=0");
    return;
  }
  std::lock_guard<std::mutex> lock(g_mx);
  const int64_t now = Now();
  if (g_loadOpen && now - g_lastCreate > Ticks(kLoadDoneSec)) {
    LoadTotalsLocked(now);
    g_loadOpen = false;
  }
  if (!key) return;  // summaries: Instrument only
  if (!g_lastSummary) g_lastSummary = now;
  if (now - g_lastSummary < Ticks(kSummarySec)) return;
  const bool nothing = g_evalTotal.load() == g_evalAtSummary && !g_changes;
  if (nothing && g_lastIdle) {
    g_lastSummary = now;  // nothing new since an idle summary: skipped
    return;
  }
  SummaryLocked("periodic", now);
}

// R132: a mission event (the Lua hook's onMissionLoadBegin / onSimulationStop / own slot change, through the loader):
// the release proof starts again (N new evaluations of the newest set after this point).
void NoteMissionEvent() {
  g_gateGen.fetch_add(1, std::memory_order_acq_rel);
  g_changeGen.fetch_add(1, std::memory_order_acq_rel);
}
// R132: DCS is shutting down (quiesce) or the payload stops: never another release.
void NoteNoMore(const char* why) {
  const bool was = g_noMore.exchange(true);
  g_relOn = false;
  if (!was && g_relKey) {
    try {
      RelStatus(why);
    } catch (...) {
    }
  }
}
// R132, payload stop: the two observation hooks go to pass-through, or, when handles we released are still guarded,
// to the guard block's persistent filters (which then go on to the trampolines).
void StopHooks() {
  GuardData* d = Gd();
  if (d && d->count.load() && g_eval.slot && g_eval.tramp && g_release.slot && g_release.tramp) {
    d->evalTramp = g_eval.tramp;
    d->relTramp = g_release.tramp;
    InterlockedExchangePointer(g_eval.slot, g_guardBase + kEvalFilterOff);
    InterlockedExchangePointer(g_release.slot, g_guardBase + kRelFilterOff);
    return;
  }
  Release(&g_eval);
  Release(&g_release);
}

// Tests: a fresh table and clock state (the detours stay, as in DCS).
void ResetForTest() {
  std::lock_guard<std::mutex> lock(g_mx);
  for (auto& f : g_f) ResetEntry(f);
  g_n = 0, g_on = false, g_key = false;
  g_evalTotal = g_evalFail = g_untracked = g_evalFault = g_releases = g_dropped = 0;
  g_evalState = 0, g_releaseState = 0;
  g_eval = Detour{"NVSDK_NGX_D3D11_EvaluateFeature"};
  g_release = Detour{"NVSDK_NGX_D3D11_ReleaseFeature"};
  g_evalOrig = nullptr, g_releaseOrig = nullptr;
  g_load = g_set = g_setFirst = g_loadFirst = 0, g_prevW = g_prevH = 0;
  g_lastCreate = g_loadStart = 0, g_loadOpen = false, g_lastSummary = 0;
  g_evalAtSummary = g_untrackedAtSummary = g_failAtSummary = g_releasesAtSummary = 0, g_changes = 0;
  g_lastIdle = g_onLogged = g_pausedLogged = g_fullLogged = false;
  g_cap = nullptr, g_statsCb = nullptr, g_capTries = 0;
  g_testEvalTarget = g_testReleaseTarget = nullptr;
  g_testClock = nullptr, g_testVram = nullptr, g_testPriv = nullptr, g_testCapParams = nullptr;
  // R132 (the guard block is left allocated: a test may still call through its filters)
  g_verbose = false, g_relOn = false, g_relKey = false, g_relN = kRuleMinEvals, g_relLatched = false, g_noMore = false;
  g_gateGen = 0, g_changeGen = 0, g_checkedGen = ~0u, g_newestSetA = 0, g_lastCreateA = 0, g_inFlight = 0;
  g_lastRelEval = 0, g_creating = 0, g_releasing = 0;
  g_ourSets = g_ourFeats = g_refusedEvals = g_swallowed = g_refusedSets = g_relFaults = g_relFailed = g_relBusy = 0;
  g_latchWhy = "", g_relLogged = false;
  g_guardBase = nullptr;
  g_testFaultOn = nullptr;
  SetEnvironmentVariableW(kGuardEnv, nullptr);
}
}  // namespace life

int __cdecl CreateHook(void* ctx, int feature, NgxParam* params, void** handle) {
  CreateFn orig = g_createOrig.load(std::memory_order_acquire);
  if (!orig) orig = reinterpret_cast<CreateFn>(g_create.tramp);  // R130: the install instant (see life::EvalOrig)
  if (feature != kFeatureSuperSampling || !params) return orig(ctx, feature, params, handle);
  void* const ret = _ReturnAddress();  // R126: the caller, for the create line
  const uint32_t seq = g_creates.fetch_add(1) + 1;
  Decision d;
  const int periph = g_periph.load(), focus = g_focus.load();
  const bool killed = g_killed.load();
  const bool lifeOn = life::g_on.load();  // R130
  if ((periph > 0 && !killed) || lifeOn) {
    try {
      if (!SizesUsable(CurrentSizes())) RefreshSizes();  // mission load: QVFR's log is small and read once
    } catch (...) {
    }
  }
  const int driverPreset = HiddenThisSession() ? NameableForced() : 0;
  Decide(params, periph, focus, killed, driverPreset, CurrentSizes(), &d);
  if (d.preset) g_applied.fetch_add(1);
  life::Meas m0, m1;
  int64_t t0 = 0;
  if (lifeOn) {
    t0 = life::Now();
    life::Measure(ctx, &m0);
  }
  // R132: never beside one of our releases (the render thread's release step checks g_creating after setting
  // g_releasing; a release is four NGX calls, well under a millisecond).
  life::g_creating.fetch_add(1, std::memory_order_seq_cst);
  while (life::g_releasing.load(std::memory_order_seq_cst)) SwitchToThread();
  const int r = orig(ctx, feature, params, handle);
  life::g_creating.fetch_sub(1, std::memory_order_seq_cst);
  if (r == kNgxSuccess && life::GuardAny()) {  // R132: NGX gave a released handle's address to this new feature
    void* nh = nullptr;
    if (handle && SafeRead(handle, &nh, sizeof(nh)) && nh && life::GuardForget(nh)) {
      try {
        Log("dlss release: NGX reused the address 0x%llx of a released handle for a new feature: no longer guarded",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(nh)));
      } catch (...) {
      }
    }
  }
  try {
    char tail[MAX_PATH + 160] = {};
    CreateTail(ret, handle, tail, sizeof(tail));
    if (lifeOn) {
      life::Measure(ctx, &m1);
      char lt[200] = {};
      life::NoteCreate(t0, seq, handle, d.fault ? kUnknown : d.cls, d.outW, d.outH, r, m0, m1, lt, sizeof(lt));
      strncat_s(tail, sizeof(tail), lt, _TRUNCATE);
    }
    Report(d, r, seq, tail);
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
    // R126: DCS Control's OpenXR layer hosts the same filter (it loads before NGX) and says so in the environment.
    wchar_t hosted[4] = {};
    if (GetEnvironmentVariableW(kHiddenEnv, hosted, 4) == 1 && hosted[0] == L'1')
      Log("dlss views: IgnoreDriverPreset: driver preset filter hosted by the OpenXR layer (%ls=1); nothing installed "
          "here",
          kHiddenEnv);
    else
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

// Worker tick (about once per second). periph / focus: the parsed keys (-1 = a value we cannot use). instrument: R130
// [DLSS] Instrument (read-only lifetime counters). release / releaseAfter: R132 [DLSS] ReleaseIdleSets /
// ReleaseAfterEvals. Nothing is installed while PeripheryPreset, Instrument and ReleaseIdleSets are 0.
void Tick(int periph, int focus, bool ignore, bool engineOff, bool instrument = false, bool release = false,
          int releaseAfter = static_cast<int>(life::kRuleMinEvals)) {
  if ((periph < 0 || focus < 0) && !g_badKeyLogged) {
    g_badKeyLogged = true;
    Log("dlss views: [DLSS] %s is not a preset this build accepts (0, A-F, J-M or their NGX numbers); treated as 0",
        periph < 0 ? "PeripheryPreset" : "FocusPreset");
  }
  periph = periph > 0 ? periph : 0;
  focus = focus > 0 ? focus : 0;
  const bool was = g_periph.load() > 0;
  g_periph = periph, g_focus = focus, g_ignore = ignore && periph > 0, g_killed = engineOff;
  // R130: the CreateFeature hook carries the creation side of the instrumentation, so it is installed for either key.
  const bool track = instrument || release;  // R132: releasing needs the tracking
  if ((track || periph > 0) && !life::g_guardBase) life::GuardAdopt();  // R132: an earlier payload's released handles
  if (track || life::g_on.load() || life::g_key || life::g_relKey) {
    if (track) life::g_verbose = instrument, life::g_relKey = release;  // (the install lines' wording)
    if (track && g_createState.load() == 0) TryInstallCreate();
    if (track && g_createState.load() == 1) life::Install();
    life::Tick(instrument, engineOff, release, releaseAfter);
  }
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
  life::NoteNoMore("at the payload stop");  // R132: no release from here on
  life::StopHooks();  // R130 pass-through; R132: the guard filters when released handles are guarded
}

}  // namespace dlssv
