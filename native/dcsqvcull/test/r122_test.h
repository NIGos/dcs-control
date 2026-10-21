// R122: the optional per-view DLSS preset (src\dlss_views.h). A fake NGX API: a parameter object with the NGX vtable
// layout, a fake NVSDK_NGX_D3D11_CreateFeature and a fake NvAPI_DRS_GetSetting in executable memory, both starting
// with the real prologues of the driver this was written on (_nvngx.dll 32.0.16.1742, nvapi64_impl.dll), so the real
// detour code patches them. Checks: preset parsing, view classification by size, QVFR log parsing, the prologue
// decoder, the driver-profile rule, the decision (only the periphery gets the preset, key 0 = no-op, unknown sizes left
// alone, kill switch, a fault contained), the hook end to end, the hot-reload order (NGX after the payload, a second
// payload generation reusing the patch, stop = pass-through), and IgnoreDriverPreset (filter only before NGX, only the
// preset setting, only DLSS callers; the driver's preset made explicit on the focus). No NvAPI or NGX call is made.
#pragma once

namespace r122test {

using dlssv::NgxParam;

// A parameter block with NGX's vtable layout: unsigned values by name, Set calls counted.
struct FakeParam : NgxParam {
  std::map<std::string, unsigned long long> v;
  int sets = 0;
  bool fault = false;  // Get faults (an access violation), as a bad DCS pointer would
  void Set(const char* n, unsigned long long x) override { v[n] = x, ++sets; }
  void Set(const char* n, float x) override { v[n] = static_cast<unsigned long long>(x), ++sets; }
  void Set(const char* n, double x) override { v[n] = static_cast<unsigned long long>(x), ++sets; }
  void Set(const char* n, unsigned int x) override { v[n] = x, ++sets; }
  void Set(const char* n, int x) override { v[n] = static_cast<unsigned long long>(x), ++sets; }
  void Set(const char*, dlssv::D3D11Res*) override { ++sets; }
  void Set(const char*, dlssv::D3D12Res*) override { ++sets; }
  void Set(const char*, void*) override { ++sets; }
  template <class T>
  int G(const char* n, T* x) const {
    if (fault) {
      volatile int* bad = nullptr;
      *x = static_cast<T>(*bad);  // access violation: Decide's SEH must contain it
    }
    auto it = v.find(n);
    if (it == v.end()) return static_cast<int>(0xBAD00005);
    *x = static_cast<T>(it->second);
    return dlssv::kNgxSuccess;
  }
  int Get(const char* n, unsigned long long* x) const override { return G(n, x); }
  int Get(const char* n, float* x) const override { return G(n, x); }
  int Get(const char* n, double* x) const override { return G(n, x); }
  int Get(const char* n, unsigned int* x) const override { return G(n, x); }
  int Get(const char* n, int* x) const override { return G(n, x); }
  int Get(const char*, dlssv::D3D11Res**) const override { return static_cast<int>(0xBAD00005); }
  int Get(const char*, dlssv::D3D12Res**) const override { return static_cast<int>(0xBAD00005); }
  int Get(const char*, void**) const override { return static_cast<int>(0xBAD00005); }
  void Reset() override {}
};
// A feature as DCS creates it: every preset hint 'C' (3).
void DcsFeature(FakeParam& p, unsigned inW, unsigned inH, unsigned outW, unsigned outH) {
  p.v.clear();
  p.v["Width"] = inW, p.v["Height"] = inH, p.v["OutWidth"] = outW, p.v["OutHeight"] = outH;
  for (const char* k : dlssv::kHintKeys) p.v[k] = 3;
  p.sets = 0;
}
bool AllHints(const FakeParam& p, unsigned want) {
  for (const char* k : dlssv::kHintKeys) {
    auto it = p.v.find(k);
    if (it == p.v.end() || it->second != want) return false;
  }
  return true;
}
dlssv::ViewSizes QvSizes() {
  dlssv::ViewSizes s;
  s.periph = {1040, 1028};
  s.focus = {2028, 2004};
  return s;
}

// ---- the fakes in executable memory ---------------------------------------------------------------------------------
// What the fake CreateFeature body saw (it tail-jumps here after its prologue).
int g_createCalls = 0;
unsigned g_seenHint = 0;
int __cdecl FakeCreateBody(void* ctx, int feature, NgxParam* params, void** handle) {
  ++g_createCalls;
  g_seenHint = 0;
  if (params) params->Get(dlssv::kHintKeys[2], &g_seenHint);
  if (handle) *handle = ctx;
  return feature == dlssv::kFeatureSuperSampling ? dlssv::kNgxSuccess : static_cast<int>(0xBAD00001);
}
int g_gsCalls = 0;
uint32_t g_gsLastId = 0;
int __cdecl FakeGetSettingBody(void*, void*, uint32_t id, void*) {
  ++g_gsCalls;
  g_gsLastId = id;
  return 0;  // NVAPI_OK
}
// _nvngx.dll's NVSDK_NGX_D3D11_CreateFeature prologue (16 bytes: mov [rsp+18h],rbp; push rsi; push rdi; push r14;
// sub rsp,40h; mov rax,[rcx]), then its undo and a tail jump to FakeCreateBody (arguments untouched).
const uint8_t kCreatePrologue[16] = {0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57, 0x41,
                                     0x56, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0x01};
// nvapi64_impl.dll's NvAPI_DRS_GetSetting prologue (15 bytes of stores to the home area).
const uint8_t kGetSettingPrologue[15] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
                                         0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};
uint8_t* MakeFake(const uint8_t* prologue, size_t n, const uint8_t* undo, size_t undoLen, void* body) {
  auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!p) return nullptr;
  memset(p, 0xCC, 4096);
  size_t o = 0;
  memcpy(p + o, prologue, n), o += n;
  if (undo && undoLen) memcpy(p + o, undo, undoLen), o += undoLen;
  p[o++] = 0x48, p[o++] = 0xB8;  // mov rax, imm64
  memcpy(p + o, &body, 8), o += 8;
  p[o++] = 0xFF, p[o++] = 0xE0;  // jmp rax
  FlushInstructionCache(GetCurrentProcess(), p, 4096);
  return p;
}
uint8_t* MakeFakeCreate() {
  // add rsp,40h; pop r14; pop rdi; pop rsi; mov rbp,[rsp+18h]
  const uint8_t undo[] = {0x48, 0x83, 0xC4, 0x40, 0x41, 0x5E, 0x5F, 0x5E, 0x48, 0x8B, 0x6C, 0x24, 0x18};
  return MakeFake(kCreatePrologue, sizeof(kCreatePrologue), undo, sizeof(undo), reinterpret_cast<void*>(&FakeCreateBody));
}
uint8_t* MakeFakeGetSetting() {
  return MakeFake(kGetSettingPrologue, sizeof(kGetSettingPrologue), nullptr, 0,
                  reinterpret_cast<void*>(&FakeGetSettingBody));
}

// Back to a fresh payload generation (statics as at load); the patched code and its pages stay, as in DCS.
void NewGeneration() {
  using namespace dlssv;
  g_periph = 0, g_focus = 0, g_ignore = false, g_killed = false;
  g_createState = 0, g_getSettingState = 0;
  g_create = Detour{"NVSDK_NGX_D3D11_CreateFeature"};
  g_getSetting = Detour{"NvAPI_DRS_GetSetting"};
  g_createOrig = nullptr, g_getSettingOrig = nullptr;
  g_nvGetSetting = nullptr;
  g_drs = DrsInfo{};
  g_drsState = -1;  // tests: never touch the real NvAPI (set to 1 with a fake profile where needed)
  g_pW = g_pH = g_fW = g_fH = 0;
  g_qvfrLogTime = 0;
  g_creates = 0, g_applied = 0, g_hiddenReads = 0, g_getSettingCalls = 0;
  g_waitLogged = false, g_warned = false, g_offLogged = true, g_badKeyLogged = false;
  g_testCreateTarget = nullptr, g_testGetSettingTarget = nullptr;
  g_testNgxUp = false, g_testAnyCaller = false, g_testNoQvfrModule = false;
  g_testQvfrLog.clear();
  g_testGetSettingObs = nullptr;
  SetEnvironmentVariableW(kHiddenEnv, nullptr);
}
std::wstring WriteQvfrLog(const char* text) {
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring path = std::wstring(tmp) + L"qv_r122_qvfr_" + std::to_wstring(GetCurrentProcessId()) + L".log";
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
    fputs(text, f);
    fclose(f);
  }
  return path;
}
const char* const kQvfrText =
    "2026-10-21 20:47:12 +0200: Recommended peripheral resolution: 1040x1028 (0.192x density)\n"
    "2026-10-21 20:47:12 +0200: Recommended focus resolution: 2028x2004 (1.100x density)\n";

void Pure() {
  using namespace dlssv;
  // Preset keys.
  Check(ParsePreset(L"0") == 0 && ParsePreset(L"") == 0 && ParsePreset(nullptr) == 0 && ParsePreset(L" 0 ") == 0,
        "R122 preset key: empty / 0 = off");
  Check(ParsePreset(L"E") == 5 && ParsePreset(L"e") == 5 && ParsePreset(L" K ") == 11 && ParsePreset(L"M") == 13 &&
            ParsePreset(L"5") == 5 && ParsePreset(L"13") == 13 && ParsePreset(L"F") == 6 && ParsePreset(L"J") == 10,
        "R122 preset key: letters (either case) and NGX numbers");
  Check(ParsePreset(L"G") == -1 && ParsePreset(L"N") == -1 && ParsePreset(L"14") == -1 && ParsePreset(L"7") == -1 &&
            ParsePreset(L"E5") == -1 && ParsePreset(L"1000") == -1 && ParsePreset(L"-5") == -1,
        "R122 preset key: the SDK's do-not-use presets and junk are refused (-1)");
  Check(strcmp(PresetName(5), "E") == 0 && strcmp(PresetName(13), "M") == 0 && strcmp(PresetName(kDrsPresetLatest), "latest") == 0,
        "R122 preset names");

  // View classification by output size.
  const ViewSizes s = QvSizes();
  Check(Classify(1040, 1028, s) == kPeriph && Classify(2028, 2004, s) == kFocus, "R122 classify: periphery and focus by output size");
  Check(Classify(1042, 1026, s) == kPeriph && Classify(2026, 2006, s) == kFocus, "R122 classify: 2 px tolerance");
  Check(Classify(1043, 1028, s) == kUnknown && Classify(1500, 1500, s) == kUnknown && Classify(0, 0, s) == kUnknown &&
            Classify(604, 596, s) == kUnknown && Classify(5424, 5356, s) == kUnknown,
        "R122 classify: any other size (render size, stereo, 2D) is unknown");
  ViewSizes bad = s;
  std::swap(bad.periph, bad.focus);
  ViewSizes zero;
  ViewSizes same = s;
  same.focus = same.periph;
  Check(!SizesUsable(bad) && !SizesUsable(zero) && !SizesUsable(same) && Classify(1040, 1028, bad) == kUnknown &&
            Classify(1040, 1028, zero) == kUnknown && Classify(1040, 1028, same) == kUnknown,
        "R122 classify: sizes that make no sense (periphery not smaller, zero, equal) classify nothing");

  // QVFR's log.
  ViewSizes v;
  const std::string two = std::string(
      "x: Recommended peripheral resolution: 900x890 (0.166x density)\nx: Recommended focus resolution: 1800x1780 (1.0x)\n") + kQvfrText;
  Check(ParseQvfrLog(two, &v) && v.periph.w == 1040 && v.periph.h == 1028 && v.focus.w == 2028 && v.focus.h == 2004,
        "R122 QVFR log: the last session's sizes");
  ViewSizes u;
  Check(!ParseQvfrLog("Recommended peripheral resolution: 1040x1028\n", &u) && !ParseQvfrLog("", &u) &&
            !ParseQvfrLog("Recommended peripheral resolution: 2028x2004\nRecommended focus resolution: 1040x1028\n", &u) &&
            u.periph.w == 0,
        "R122 QVFR log: a missing or inconsistent line gives no sizes");

  // Prologue decoder.
  const uint8_t evalP[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
                           0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x30, 0, 0, 0, 0, 0, 0, 0, 0};
  const uint8_t allocP[] = {0x40, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x83, 0x3d, 0xea, 0x74, 0x07, 0x00, 0x00, 0x48, 0x8b,
                            0xf9, 0x75, 0x46, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  const uint8_t initP[] = {0x48, 0x89, 0x5c, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                           0x48, 0x81, 0xec, 0x90, 0x02, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  uint8_t cp[32] = {}, gp[32] = {}, jmp[32] = {0xE9, 1, 2, 3, 4};
  memcpy(cp, kCreatePrologue, 16), memcpy(gp, kGetSettingPrologue, 15);
  Check(PrologueLen(cp) == 16 && PrologueLen(gp) == 15 && PrologueLen(evalP) == 15 && PrologueLen(initP) == 14,
        "R122 decoder: the NGX core's CreateFeature / EvaluateFeature / Init and NvAPI's GetSetting prologues");
  Check(PrologueLen(allocP) == 0 && PrologueLen(jmp) == 0, "R122 decoder: a RIP-relative operand or a jump is refused");

  // The driver-profile rule (the snippet's order: application profile, then base profile).
  DrsInfo d;
  d.state = 1;
  d.appPresetSet = true, d.appPreset = 13;
  Check(ForcedPreset(d) == 13, "R122 driver profile: application preset M forces M");
  d.appPreset = 0;
  d.basePresetSet = true, d.basePreset = 13;
  Check(ForcedPreset(d) == 0, "R122 driver profile: application preset off wins over the base profile");
  d.appPresetSet = false;
  Check(ForcedPreset(d) == 13, "R122 driver profile: no application value -> the base profile's");
  d.basePreset = kDrsPresetDefault;
  Check(ForcedPreset(d) == 0, "R122 driver profile: 'default' forces nothing");
  d.basePreset = kDrsPresetLatest;
  Check(ForcedPreset(d) == kDrsPresetLatest, "R122 driver profile: 'latest' forces");
  d.state = -1;
  Check(ForcedPreset(d) == 0, "R122 driver profile: unread forces nothing");

  // DLSS modules (the callers whose preset read IgnoreDriverPreset filters).
  Check(IsDlssModulePath(L"E:\\DCS World\\bin\\nvngx_dlss.dll") &&
            IsDlssModulePath(L"C:\\ProgramData\\NVIDIA\\NGX\\models\\dlss\\versions\\20318480\\files\\160_E658700.bin") &&
            IsDlssModulePath(L"c:/programdata/nvidia/ngx/models/dlss/x.bin") && IsDlssModulePath(L"NVNGX_DLSS.DLL"),
        "R122 caller check: DCS's nvngx_dlss.dll and the driver's OTA copy are DLSS modules");
  Check(!IsDlssModulePath(L"C:\\Windows\\System32\\DriverStore\\FileRepository\\nv\\_nvngx.dll") &&
            !IsDlssModulePath(L"C:\\Windows\\System32\\nvapi64.dll") && !IsDlssModulePath(L"E:\\DCS\\bin\\DCS.exe") &&
            !IsDlssModulePath(L"") &&
            !IsDlssModulePath(nullptr),
        "R122 caller check: the NGX core, NvAPI, DCS and empty paths are not");
}

void Decisions() {
  using namespace dlssv;
  const ViewSizes s = QvSizes();
  FakeParam p;
  // Only the periphery gets the preset.
  DcsFeature(p, 604, 596, 1040, 1028);
  Decision d;
  Decide(&p, 5, 0, false, 0, s, &d);
  Check(d.cls == kPeriph && d.preset == 5 && AllHints(p, 5) && d.dcsHintRead && d.dcsHint == 3 && d.inW == 604,
        "R122 decide: periphery feature -> all six hints = E (DCS's C read for the log)");
  DcsFeature(p, 1176, 1164, 2028, 2004);
  d = Decision{};
  Decide(&p, 5, 0, false, 0, s, &d);
  Check(d.cls == kFocus && d.preset == 0 && p.sets == 0 && AllHints(p, 3),
        "R122 decide: focus feature with FocusPreset=0 -> untouched (no Set)");
  d = Decision{};
  Decide(&p, 5, 11, false, 0, s, &d);
  Check(d.preset == 11 && AllHints(p, 11), "R122 decide: FocusPreset=K -> the focus gets K");
  // Key 0: no-op.
  DcsFeature(p, 604, 596, 1040, 1028);
  d = Decision{};
  Decide(&p, 0, 11, false, 0, s, &d);
  Check(d.preset == 0 && p.sets == 0 && AllHints(p, 3), "R122 decide: PeripheryPreset=0 -> no change (FocusPreset ignored)");
  // Kill switch.
  d = Decision{};
  Decide(&p, 5, 0, true, 0, s, &d);
  Check(d.preset == 0 && p.sets == 0, "R122 decide: kill switch -> no change");
  // Unknown sizes left alone.
  DcsFeature(p, 900, 900, 1500, 1500);
  d = Decision{};
  Decide(&p, 5, 11, false, 0, s, &d);
  Check(d.cls == kUnknown && d.preset == 0 && p.sets == 0, "R122 decide: a size matching no view -> untouched");
  DcsFeature(p, 604, 596, 1040, 1028);
  d = Decision{};
  Decide(&p, 5, 11, false, 0, ViewSizes{}, &d);
  Check(d.preset == 0 && p.sets == 0, "R122 decide: Quad Views sizes unknown -> untouched");
  // IgnoreDriverPreset in force: the driver's preset made explicit where we set none.
  DcsFeature(p, 1176, 1164, 2028, 2004);
  d = Decision{};
  Decide(&p, 5, 0, false, 13, s, &d);
  Check(d.preset == 13 && d.fromDriver && AllHints(p, 13), "R122 decide: driver preset hidden -> the focus gets it (M) explicitly");
  DcsFeature(p, 604, 596, 1040, 1028);
  d = Decision{};
  Decide(&p, 5, 0, false, 13, s, &d);
  Check(d.preset == 5 && !d.fromDriver, "R122 decide: ... while the periphery gets PeripheryPreset");
  d = Decision{};
  Decide(&p, 5, 0, true, 13, s, &d);
  Check(d.preset == 13 && d.fromDriver, "R122 decide: ... and with the kill switch every feature gets the driver's preset back");
  // A fault in DCS's parameter object is contained.
  FakeParam f;
  DcsFeature(f, 604, 596, 1040, 1028);
  f.fault = true;
  d = Decision{};
  Decide(&f, 5, 0, false, 0, s, &d);
  Check(d.fault && d.preset == 0 && f.sets == 0, "R122 decide: an access violation reading the parameters is contained (feature untouched)");
}

void Hooks() {
  using namespace dlssv;
  NewGeneration();
  uint8_t* fake = MakeFakeCreate();
  if (!fake) {
    QvSkip("R122 hooks: no executable memory\n");
    return;
  }
  const auto create = reinterpret_cast<CreateFn>(fake);
  int ctxDummy = 0;
  FakeParam p;
  void* h = nullptr;
  // Unhooked fake works.
  DcsFeature(p, 604, 596, 1040, 1028);
  g_createCalls = 0;
  int r = create(&ctxDummy, kFeatureSuperSampling, &p, &h);
  Check(r == kNgxSuccess && g_createCalls == 1 && g_seenHint == 3, "R122 fake CreateFeature runs unhooked (DCS's hint C)");

  // Key 0: Tick installs nothing.
  g_testCreateTarget = fake;
  Tick(0, 0, false, false);
  uint8_t b0[16] = {};
  memcpy(b0, fake, 16);
  Check(g_createState.load() == 0 && memcmp(b0, kCreatePrologue, 16) == 0, "R122 key 0: nothing installed, code untouched");

  // NGX not loaded yet (the payload starts first): waiting, retried by the next tick.
  g_testCreateTarget = nullptr;
  g_testQvfrLog = WriteQvfrLog(kQvfrText);
  Tick(5, 0, false, false);
  const bool waited = g_createState.load() == 0 && g_waitLogged;
  g_testCreateTarget = fake;  // "NGX loads"
  Tick(5, 0, false, false);
  Check(waited && g_createState.load() == 1 && fake[0] == 0xFF && fake[1] == 0x25,
        "R122 hot-reload order: NGX loading after the payload -> waits, installs at the next tick");
  Check(SizesUsable(CurrentSizes()) && g_pW.load() == 1040 && g_fH.load() == 2004, "R122 sizes read from the QVFR log");

  // The hook end to end: periphery E, focus untouched, unknown untouched, one log line per creation.
  StartReportCapture();
  DcsFeature(p, 604, 596, 1040, 1028);
  r = create(&ctxDummy, kFeatureSuperSampling, &p, &h);
  const bool periphOk = r == kNgxSuccess && g_seenHint == 5 && h == &ctxDummy;
  DcsFeature(p, 1176, 1164, 2028, 2004);
  r = create(&ctxDummy, kFeatureSuperSampling, &p, &h);
  const bool focusOk = r == kNgxSuccess && g_seenHint == 3;
  DcsFeature(p, 300, 300, 640, 480);
  r = create(&ctxDummy, kFeatureSuperSampling, &p, &h);
  const bool unknownOk = r == kNgxSuccess && g_seenHint == 3;
  DcsFeature(p, 604, 596, 1040, 1028);
  const int callsBefore = g_createCalls;
  r = create(&ctxDummy, 11 /* another feature */, &p, &h);
  const bool otherOk = r != kNgxSuccess && g_createCalls == callsBefore + 1 && g_seenHint == 3;
  const std::string log = StopReportCapture();
  Check(periphOk && focusOk && unknownOk && otherOk,
        "R122 hook: periphery gets E, focus and unknown sizes keep DCS's hint, other NGX features pass through");
  Check(log.find("create #1 periphery 604x596->1040x1028: preset E requested (DCS hint C)") != std::string::npos &&
            log.find("create #2 focus 1176x1164->2028x2004: unchanged, FocusPreset=0") != std::string::npos &&
            log.find("create #3 unknown view 300x300->640x480: unchanged, size matches no Quad Views view") != std::string::npos &&
            log.find("driver override unknown") != std::string::npos && log.find("create #4") == std::string::npos,
        "R122 log: one line per DLSS creation with the view class, the requested preset and the driver override state");

  // Kill switch at the next tick: pass-through.
  Tick(5, 0, false, true);
  DcsFeature(p, 604, 596, 1040, 1028);
  create(&ctxDummy, kFeatureSuperSampling, &p, &h);
  const bool killOk = g_seenHint == 3;
  Tick(5, 0, false, false);
  DcsFeature(p, 604, 596, 1040, 1028);
  create(&ctxDummy, kFeatureSuperSampling, &p, &h);
  Check(killOk && g_seenHint == 5, "R122 kill switch: DCS's hint while on, the preset again when off");

  // A second payload generation (hot reload): the patch is found again and reused, not stacked.
  uint8_t patched[16];
  memcpy(patched, fake, 16);
  NewGeneration();
  g_testQvfrLog = WriteQvfrLog(kQvfrText);
  g_testCreateTarget = fake;
  StartReportCapture();
  Tick(13, 0, false, false);  // the new generation asks for M on the periphery
  const std::string log2 = StopReportCapture();
  DcsFeature(p, 604, 596, 1040, 1028);
  create(&ctxDummy, kFeatureSuperSampling, &p, &h);
  Check(g_createState.load() == 1 && memcmp(patched, fake, 16) == 0 && g_seenHint == 13 &&
            log2.find("earlier payload's patch reused") != std::string::npos,
        "R122 hot reload: the next payload reuses the patch (same bytes) and its hook is the one called");

  // Payload stop: pass-through.
  Stop();
  DcsFeature(p, 604, 596, 1040, 1028);
  create(&ctxDummy, kFeatureSuperSampling, &p, &h);
  Check(g_seenHint == 3, "R122 stop: the hook passes through (DCS's hint)");

  // A foreign patch: nothing installed.
  uint8_t* foreign = MakeFakeCreate();
  if (foreign) {
    const uint8_t jmpFar[14] = {0xFF, 0x25, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
    memcpy(foreign, jmpFar, 14);
    NewGeneration();
    g_testCreateTarget = foreign;
    Tick(5, 0, false, false);
    Check(g_createState.load() == -1 && memcmp(foreign, jmpFar, 14) == 0, "R122 a foreign patch on CreateFeature: not hooked, left alone");
  }
  uint8_t* odd = MakeFakeCreate();
  if (odd) {
    odd[0] = 0x48, odd[1] = 0x8b, odd[2] = 0x05;  // mov rax,[rip+...]: an unknown driver build's prologue
    uint8_t before[16];
    memcpy(before, odd, 16);
    NewGeneration();
    g_testCreateTarget = odd;
    Tick(5, 0, false, false);
    Check(g_createState.load() == -1 && memcmp(before, odd, 16) == 0, "R122 an unknown CreateFeature prologue: not hooked, code untouched");
  }
  NewGeneration();
}

void IgnoreDriver() {
  using namespace dlssv;
  uint8_t* fakeCreate = MakeFakeCreate();
  uint8_t* fakeGs = MakeFakeGetSetting();
  uint8_t* lateGs = MakeFakeGetSetting();
  if (!fakeCreate || !fakeGs || !lateGs) {
    QvSkip("R122 IgnoreDriverPreset: no executable memory\n");
    return;
  }
  const auto create = reinterpret_cast<CreateFn>(fakeCreate);
  const auto gs = reinterpret_cast<GetSettingFn>(fakeGs);
  int ctxDummy = 0;
  void* h = nullptr;
  FakeParam p;
  // NGX already up (DCS today): nothing installed, the code untouched, logged.
  NewGeneration();
  g_drsState = 1;
  g_drs.state = 1, g_drs.appPresetSet = true, g_drs.appPreset = 13;
  g_testGetSettingTarget = lateGs;
  g_testNgxUp = true;
  g_testCreateTarget = fakeCreate;
  g_testQvfrLog = WriteQvfrLog(kQvfrText);
  StartReportCapture();
  Tick(5, 0, true, false);
  const std::string late = StopReportCapture();
  Check(g_getSettingState.load() == -2 && memcmp(lateGs, kGetSettingPrologue, 15) == 0 &&
            late.find("IgnoreDriverPreset: not possible in this DCS session") != std::string::npos &&
            late.find("WARNING: the NVIDIA driver profile forces DLSS preset M") != std::string::npos,
        "R122 IgnoreDriverPreset after NGX started: nothing installed, logged, plus the one-time NVIDIA App warning");
  StartReportCapture();
  DcsFeature(p, 604, 596, 1040, 1028);
  create(&ctxDummy, kFeatureSuperSampling, &p, &h);
  const std::string lateLine = StopReportCapture();
  Check(g_seenHint == 5 && lateLine.find("driver override: forces preset M, ACTIVE") != std::string::npos,
        "R122 log: the creation line says the driver override is active");
  Stop();

  // Before NGX: the filter is installed; only DLSS modules' reads of the preset setting are hidden.
  NewGeneration();
  g_drsState = 1;
  g_drs.state = 1, g_drs.appPresetSet = true, g_drs.appPreset = 13;
  g_testGetSettingTarget = fakeGs;
  g_testNgxUp = false;
  g_testCreateTarget = fakeCreate;
  g_testQvfrLog = WriteQvfrLog(kQvfrText);
  StartReportCapture();
  Tick(5, 0, true, false);
  const std::string early = StopReportCapture();
  Check(g_getSettingState.load() == 1 && fakeGs[0] == 0xFF && early.find("NvAPI_DRS_GetSetting filtered") != std::string::npos &&
            early.find("WARNING") == std::string::npos,
        "R122 IgnoreDriverPreset before NGX: the GetSetting filter is installed (no App warning)");
  g_gsCalls = 0;
  int r1 = gs(nullptr, nullptr, kDrsSrPreset, nullptr);  // caller: the test exe, not a DLSS module
  Check(r1 == 0 && g_gsCalls == 1 && g_hiddenReads.load() == 0, "R122 filter: a non-DLSS caller's preset read passes");
  g_testAnyCaller = true;  // from here every caller counts as the DLSS snippet
  int r2 = gs(nullptr, nullptr, kDrsSrOverride, nullptr);
  int r3 = gs(nullptr, nullptr, 0x10AFB768 /* SR mode */, nullptr);
  Check(r2 == 0 && r3 == 0 && g_gsCalls == 3 && g_gsLastId == 0x10AFB768, "R122 filter: other settings pass (latest-DLL override, mode)");
  int r4 = gs(nullptr, nullptr, kDrsSrPreset, nullptr);
  Check(r4 == kNvapiSettingNotFound && g_gsCalls == 3 && g_hiddenReads.load() == 1 && HiddenThisSession(),
        "R122 filter: DLSS's read of the preset setting returns NVAPI_SETTING_NOT_FOUND");
  // Then the creations: periphery E, focus = the driver's M made explicit.
  StartReportCapture();
  DcsFeature(p, 604, 596, 1040, 1028);
  create(&ctxDummy, kFeatureSuperSampling, &p, &h);
  const unsigned pe = g_seenHint;
  DcsFeature(p, 1176, 1164, 2028, 2004);
  create(&ctxDummy, kFeatureSuperSampling, &p, &h);
  const unsigned fo = g_seenHint;
  const std::string lines = StopReportCapture();
  Check(pe == 5 && fo == 13 && lines.find("preset M requested (the driver profile's preset, made explicit)") != std::string::npos &&
            lines.find("hidden from DLSS since NGX start") != std::string::npos,
        "R122 IgnoreDriverPreset: periphery E, focus keeps the driver's M (explicit hint), logged");
  // Key back to 0 in the same session: DLSS still knows no driver preset, so every feature gets M explicitly.
  Tick(0, 0, false, false);
  DcsFeature(p, 604, 596, 1040, 1028);
  create(&ctxDummy, kFeatureSuperSampling, &p, &h);
  const unsigned off0 = g_seenHint;
  int r5 = gs(nullptr, nullptr, kDrsSrPreset, nullptr);
  Check(off0 == 13 && r5 == 0, "R122 IgnoreDriverPreset then key 0: the driver's M restored explicitly, the filter passes");
  // A new payload generation keeps that knowledge (process environment).
  NewGeneration();
  SetEnvironmentVariableW(kHiddenEnv, L"1");
  g_drsState = 1;
  g_drs.state = 1, g_drs.appPresetSet = true, g_drs.appPreset = 13;
  g_testCreateTarget = fakeCreate;
  Tick(0, 0, false, false);
  g_createState = 0;
  Tick(5, 0, false, false);  // installs (reuse); IgnoreDriverPreset now 0
  Tick(0, 0, false, false);
  DcsFeature(p, 1176, 1164, 2028, 2004);
  create(&ctxDummy, kFeatureSuperSampling, &p, &h);
  Check(g_seenHint == 13, "R122 hot reload after a hidden read: the next payload still restores the driver's M");
  Stop();
  Release(&g_getSetting);
  NewGeneration();
}

void Run() {
  Pure();
  Decisions();
  Hooks();
  IgnoreDriver();
}

}  // namespace r122test
