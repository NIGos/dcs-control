// Offline invariants of the engine module against DCS Control's own ini (the parts table: src/main/parts.inc):
//   1. registration: every Config field is a row of kEngineParts (src/main/parts.inc) or classified here as not a part;
//   2. all off = stock: the app's "every Engine option off" ini (test/app_ini/engine_all_off.ini, written by
//      DcsVr.Tests) configures no part, and after LoadConfig and the worker's TickEngineParts none is requested;
//   3. kill switch = stock: every part configured (the app's all-on ini plus the module-only keys), the kill switch on:
//      SetEngineOff(true) and TickEngineParts leave no part requested; the worker-loop parts pass it to their ticks;
//   4. round trip: every key in both app inis is read back by the module with the value written;
//   5. defaults: the module default of every user-facing key (no ini) is the value in kKeys (a change is deliberate).
// No DCS module is loaded: installs fail offline, so `requested` is set to "on" by hand first where its off path is a
// plain assignment (the kill switch must clear it); the rest read the module's real state.
#pragma once
#include <filesystem>
#include <regex>

namespace invtest {

std::wstring ExeDir() {
  wchar_t p[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, p, MAX_PATH);
  std::wstring d(p);
  return d.substr(0, d.find_last_of(L"\\/") + 1);
}
std::wstring EnvDir(const wchar_t* name, const std::wstring& fallback) {
  wchar_t env[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(name, env, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return fallback;
  std::wstring d(env);
  if (d.back() != L'\\' && d.back() != L'/') d += L'\\';
  return d;
}
// The module's sources (CMake copies them to <build>\src) and the app's inis (<build>\app_ini).
std::wstring SrcDir() { return EnvDir(L"DCSQV_SRC_DIR", ExeDir() + L"..\\src\\"); }
std::wstring IniDir() { return EnvDir(L"DCSQV_APP_INI_DIR", ExeDir() + L"..\\app_ini\\"); }
std::string ReadText(const std::wstring& path) {
  std::string s;
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return s;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}
std::set<std::string> Words(const char* list) {
  std::set<std::string> s;
  std::string w;
  for (const char* p = list;; ++p) {
    if (*p && *p != ' ') {
      w += *p;
      continue;
    }
    if (!w.empty()) s.insert(w);
    w.clear();
    if (!*p) break;
  }
  return s;
}

// ---- 1. registration -------------------------------------------------------------------------------------------
// The Config fields that are not engine parts, by kind. A new field must go into kEngineParts or one of these.
struct NotParts {
  const char* why;
  const char* fields;
};
const NotParts kNotParts[] = {
    {"tuning of a part: acts only while that part is on",
     "keepFraction safetyNdc nativeKeepFraction saccadeDeg saccadeHoldFrames focusRatio apexTolerance debugHole "
     "shadowMarginFrac shadowMarginMin shadowFloorY slabBytes pacerTimeoutUs dlssFocusPreset dlssIgnoreDriverPreset "
     "dlssReleaseAfterEvals cascadeCacheMask cascadeCacheMaxAge cascadeCacheMarginPct cascadeCacheVerify "
     "threeViewsStripNdc threeViewsIsolate threeViewsKeyVk threeViewsKeyMods splitFilterOps shadowRecorderScope "
     "shadowRecorderWaitUs shadowRecorderPriority shadowRecorderSplit shadowRecorderHelpers shadowRecorderInstancing "
     "shadowRecorderNest smallGpuBundle recordersEmulatedLists focusTwinInstancer suiteFocusTwinTapBaseline "
     "suiteFocusTwinEarlyStop suiteFocusTwinShadow suiteFocusTwinAuditEvery suiteFocusTwinTrust suiteFocusTwinIsland "
     "suiteFocusTwinChunk suiteFocusTwinPredTol suiteFocusTwinPatches suiteFocusTwinVerify gbufferRecorderScope "
     "gbufferRecorderWaitUs gbufferRecorderIsland gbufferRecorderMaxSegments gbufferRecorderHelpers "
     "gbufferRecorderCockpit gbufferRecorderByOrdinal fxApplySkipVerify fxApplySkipVerifyEvery fxApplySkipLogSec "
     "waterProbe prefetchWindowMs prefetchTableBits prefetchSpinCap placeRecheckSec placeRecheckMode shadowPlanWaitUs "
     "bigPageBytes"},
    {"governor: steps the recorders' settings only, nothing of its own",
     "govEnabled govGpuBusyPct govRenderIdlePct govCpuBusyPct govGpuFreePct govGpuFullPct govWindowSec govNeedSec "
     "govDwellSec govMinGainPct govTrialSec govRecheckSec govRevertBackoffSec"},
    {"measurement, test or log only: acts while a suite, benchmark, probe or dump runs (dev ini), or only logs",
     "statsIntervalSec dumpViews benchBlocks benchBlockSec benchSettleSec benchAutoStartSec benchThreadsB "
     "profileSeconds profileThreads profilePeriodMs suiteProfile suiteSelfTest suiteBenchPartition suiteBenchIsolation "
     "suiteBenchTimerRes diagnostics suiteBenchShadow suiteBenchAllocSlabs suiteBenchTexDedupe suiteBenchEngine "
     "motionSeconds suiteQuick suiteBenchTriPlain suiteBenchCbSkip suiteBenchMicro suiteBenchCostWeights suiteTerrain "
     "suiteBenchCbUpload suiteBenchPacer motionSweep motionProfile motionTaxi suiteBenchShadowTex suiteBenchTexTable "
     "suiteBenchBigPages suiteBenchParUpload suiteDirectUploadVerify suiteBenchDirectUpload motionCounters "
     "suiteShadowInstCompile suiteGBufferInstCompile suiteGBufferTexCount suiteFxApplyCount suitePerEyePassCensus "
     "suiteDirectUploadCount suiteSrvSpanCount suiteJoinTailCount suiteShadowRecCount suiteGBufferRecCount "
     "suiteForwardRecCount suiteForwardSplit suiteGpuPassTiming suiteGpuPassStats suitePeriphLodTiming "
     "suitePeriphLodImage suiteBenchPeripheralLod dlssInstrument suiteCascadeCacheTiming suiteChainFastTiming "
     "suiteChainFastVerify suiteChainDeriveCensus suiteChainDeriveCensusMB suiteThreeViewsTiming suiteEarlyShaderCensus "
     "suitePeriphCensus suiteShaderFastTiming suiteShaderFastVerify suiteVramCount suiteVramCountSec "
     "suiteVramCountCreates suiteSplitFilterVerify suiteBenchSplitFilter suiteShadowInstVerify suiteBenchShadowInst "
     "suiteBenchShadowPlanAsync suiteShadowRecVerify suiteShadowRecVerifySec suiteBenchShadowRecorder "
     "suiteGBufferInstVerify suiteBenchGBufferInst suiteGBufferRecVerify suiteGBufferRecVerifySec "
     "suiteGBufferRecVerifyLive suiteGBufferRecVerifyNoLatch suiteGBufferRecVerifyDump suiteGBufferRecVerifyDumpAll "
     "suiteGBufferRecVerifyStride suiteBenchGBufferRecorder suiteBenchPassFlush suiteBenchPassFlushMask "
     "suiteSrvTailTrimVerify suiteSrvTailTrimVerifySec suiteBenchSrvTailTrim suiteBenchFxApplySkip "
     "suiteWaterSkipTiming suiteWaterSkipTimingFrames suiteBenchPrefetch benchPassMs suitePassTimelineSec "
     "suiteChainCensusSec suiteGpuBundleTiming suiteGpuBundlePairs suiteBenchPrefetchTableBits "
     "suiteBenchGBufferRecorderCockpit suiteLabel threadCensus threadCensusSec frameTimeline timelineSec poseProbe "
     "eventLog holdYawDeg holdPitchDeg holdPose poseWallClock poseRotateDps suiteYawScan suiteYawProfile "
     "suiteYawProfileStep suiteRotationProfile suiteRotationDegPerSec suiteRotationSeconds suiteFrameStartGap "
     "suiteFrameStartGapSec suiteRunnableThreads suiteRunnableThreadsSec suiteRunnableThreadsEvery suiteBenchFrameHeap "
     "suiteBenchFilter suiteBenchCull suiteBenchThreads suiteBenchTimer d3dMeter recorderDetail perfIntervalSec "
     "reloadFrameTimes vramIntervalSec shadowDebugInvert"},
    {"perception / visual feature (Depth boost, far depth, Framegen exports, night tone): not an engine "
     "optimization, outside the kill switch by design",
     "fixationDepth sceneCutReset depthGrid fixationDepthVerify fixationDepthDump fixationTestGaze farDepthExport "
     "farDepthSlots farDepthLogProj farDepthPreOverlay farDepthCoverageMode farDepthPostOverlay farDepthOverlayTrace "
     "cockpitMaskExport cockpitMaskDivisor cockpitMaskViews cockpitMaskNearM cockpitMaskDilate cockpitMaskDump "
     "cockpitMaskLog cockpitDepthExport cockpitDepthDump cockpitMotionExport cockpitMotionClass cockpitMotionDump "
     "cockpitMotionFocus cockpitMotionDepth cockpitMotionFocusAssumeJitterFree "
     "cockpitFgDump symbologyExport symbologyAlpha symbologyPublishAlways cockpitTrueIpd cockpitTrueIpdLighting "
     "cockpitTrueIpdForward cockpitTrueIpdHelmet cockpitTrueIpdOwnship cockpitIpdDormant cockpitTrueIpdOverlays "
     "cockpitTrueIpdOverlayMvp cockpitTrueIpdLog cockpitTrueIpdPrevLog cockpitTrueIpdFrameDump cockpitTrueIpdMvpDump "
     "cockpitTrueIpdFrameDumpViews cockpitTrueIpdOverlayRestore cockpitTrueIpdOverlayPath cockpitTrueIpdTestShiftMm "
     "cockpitTrueIpdToggleVk cockpitTrueIpdToggleMods nightTone nightFloor nightToe nightEv nightGate"},
    {"infrastructure: the mode latch, the keys, lossless patch / log / safety paths",
     "perceptionOnly toggleVk toggleMods developerKeys proxyGuard patchMode asyncLog"},
};

// Config's field names, from config.inc's struct.
std::vector<std::string> ConfigFields(const std::string& src) {
  std::vector<std::string> out;
  const size_t a = src.find("struct Config {"), e = a == std::string::npos ? a : src.find("\n};", a);
  if (e == std::string::npos) return out;
  const std::regex decl(R"(^\s*(bool|int|double|uint32_t|std::wstring|std::string)\s+([^;]+);)");
  size_t pos = a;
  while (pos < e) {
    size_t nl = src.find('\n', pos);
    if (nl == std::string::npos || nl > e) nl = e;
    std::string line = src.substr(pos, nl - pos);
    pos = nl + 1;
    const size_t cm = line.find("//");
    if (cm != std::string::npos) line.resize(cm);
    std::smatch m;
    if (!std::regex_search(line, m, decl)) continue;
    std::string names = m[2].str();
    size_t s = 0;
    while (s <= names.size()) {
      size_t c = names.find(',', s);
      if (c == std::string::npos) c = names.size();
      std::string d = names.substr(s, c - s);
      d = d.substr(0, d.find('='));
      const size_t b = d.find_first_not_of(" \t"), z = d.find_last_not_of(" \t\r");
      if (b != std::string::npos) out.push_back(d.substr(b, z - b + 1));
      s = c + 1;
    }
  }
  return out;
}

void Registration() {
  const std::string cfg = ReadText(SrcDir() + L"main\\config.inc"), parts = ReadText(SrcDir() + L"main\\parts.inc");
  const std::vector<std::string> fields = ConfigFields(cfg);
  std::set<std::string> inParts;
  {
    const size_t a = parts.find("kEngineParts[] = {"), e = a == std::string::npos ? a : parts.find("\n};", a);
    const std::string table = e == std::string::npos ? std::string() : parts.substr(a, e - a);
    const std::regex use(R"(\bc\.(\w+))");
    for (std::sregex_iterator it(table.begin(), table.end(), use), end; it != end; ++it) inParts.insert((*it)[1].str());
  }
  std::map<std::string, int> classified;
  for (const NotParts& n : kNotParts)
    for (const std::string& f : Words(n.fields)) ++classified[f];
  int unclassified = 0, both = 0, twice = 0, unknown = 0;
  const std::set<std::string> fieldSet(fields.begin(), fields.end());
  for (const std::string& f : fields) {
    const bool p = inParts.count(f) != 0, c = classified.count(f) != 0;
    if (!p && !c) {
      ++unclassified;
      printf("     Config.%s is neither an engine part (src/main/parts.inc) nor classified (invariants_test.h kNotParts)\n",
             f.c_str());
    }
    if (p && c) {
      ++both;
      printf("     Config.%s is an engine part and also classified as not one\n", f.c_str());
    }
  }
  for (const auto& kv : classified) {
    if (kv.second > 1) ++twice, printf("     Config.%s is classified twice\n", kv.first.c_str());
    if (!fieldSet.count(kv.first)) ++unknown, printf("     kNotParts names %s, which Config does not have\n", kv.first.c_str());
  }
  for (const std::string& f : inParts)
    if (!fieldSet.count(f)) ++unknown, printf("     parts.inc reads c.%s, which Config does not have\n", f.c_str());
  printf("     Config fields: %zu (%zu read by %d engine parts, %zu classified as not parts)\n", fields.size(),
         inParts.size(), kEnginePartCount, classified.size());
  Check(fields.size() > 250 && inParts.size() > 30 && unclassified == 0 && both == 0 && twice == 0 && unknown == 0,
        "invariants: every Config field is an engine part (parts.inc kEngineParts) or classified as not one, once");
  // The parts that may stay on under the kill switch: exactly the pre-kill-switch research experiments (a new part
  // cannot join them silently).
  std::set<std::string> none;
  for (const EnginePart& p : kEngineParts)
    if (p.gate == kGateNone) none.insert(p.name);
  Check(none == std::set<std::string>{"peripheral exclusion", "collect threads max", "tight shadow casters",
                                      "D3D state filter (old)", "fine timer resolution"},
        "invariants: only the five research experiments ([Cull] Enabled, CollectThreadsMax, TightCasters, [D3D] "
        "Filter, FineTimerResolution) are outside the kill switch");
}

// ---- helpers for the config passes -----------------------------------------------------------------------------
struct Saved {
  Config cfg = g_cfg;
  std::wstring dir = g_dir;
  bool engineOff = g_engineOff.load(), po = g_perceptionOnly.load(), startDone = g_startInstallsDone.load();
  bool enabled = g_enabled.load(), hole = g_debugHole.load(), tight = g_shadowTight.load(), beeps = g_beeps.load();
  bool sig = reloc::g_enabled.load(), wantDof = vrfx::g_wantDof.load(), wantGv = vrfx::g_wantGv.load();
  int d3d = g_d3dMode.load(), colorKeyVk = g_colorKeyVk, colorKeyMods = g_colorKeyMods;
  cpass::Cfg color = g_colorCfg;
  ~Saved() {
    g_cfg = cfg;
    g_dir = dir;
    g_engineOff = engineOff;
    g_perceptionOnly = po;
    g_startInstallsDone = startDone;
    g_enabled = enabled;
    g_debugHole = hole;
    g_shadowTight = tight;
    g_beeps = beeps;
    reloc::g_enabled = sig;
    vrfx::g_wantDof = wantDof;
    vrfx::g_wantGv = wantGv;
    g_d3dMode = d3d;
    g_colorKeyVk = colorKeyVk;
    g_colorKeyMods = colorKeyMods;
    g_colorCfg = color;
    SetTimerResolution(false);
  }
};

std::wstring IniPath() { return g_dir + L"DcsQvCull.ini"; }
void WriteIni(const std::string& text) {
  FILE* f = nullptr;
  _wfopen_s(&f, IniPath().c_str(), L"wb");
  if (!f) return;
  fwrite(text.data(), 1, text.size(), f);
  fclose(f);
}
void SetKey(const wchar_t* sec, const wchar_t* key, const wchar_t* v) {
  WritePrivateProfileStringW(sec, key, v, IniPath().c_str());
}
// LoadConfig(true) on the current file. engineOff: the kill switch is on before it (nothing installs).
void Load(bool engineOff) {
  g_engineOff = engineOff;
  g_startInstallsDone = false;  // ApplyDiagnostics / live installs: nothing offline
  LoadConfig(true);
}

std::string Configured(const Config& c, bool skipExempt) {
  std::string s;
  for (const EnginePart& p : kEngineParts) {
    if (skipExempt && !strcmp(p.name, "worker placement")) continue;
    if (p.configured(c)) s += (s.empty() ? "" : ", ") + std::string(p.name);
  }
  return s;
}

// ---- 2. all off = stock ----------------------------------------------------------------------------------------
void AllOff(const std::string& offIni) {
  WriteIni(offIni);
  Load(false);
  // [Threads] WorkerPlacement (default 2) is not on the Engine page and the app writes it only in developer mode: it
  // moves only DCS Control's own threads (thread_place.h: DCS's threads are never touched), so it is exempt here.
  const std::string cfg = Configured(g_cfg, true);
  if (!cfg.empty()) printf("     all off still configures: %s\n", cfg.c_str());
  TickEngineParts();
  const std::string req = EnginePartsRequested();
  if (!req.empty()) printf("     all off still requests: %s\n", req.c_str());
  const uint64_t w = ComputeStatus();
  Check(!g_perceptionOnly.load() && cfg.empty() && req.empty() && qvstatus::ConfiguredCount(w) == 0 &&
            qvstatus::ActiveCount(w) == 0,
        "invariants: DCS Control's every-Engine-option-off ini (app_ini/engine_all_off.ini) configures and requests no "
        "engine part (GitHub #28), and the status word counts 0 configured / 0 active");
  Check(g_cfg.workerPlacement == 2, "invariants: all off keeps [Threads] WorkerPlacement 2 (it moves DCS Control's own threads only)");
}

// bugfix-07: the engine A/B (bench mode 11) switches the kill switch per block while the bench holds the features'
// configuration. Early shaders' held mask was zeroed by the first OFF block's tick and every later ON block ran
// without them. Run on the all-off ini (nothing else installs offline) with [D3D] EarlyShaders=3 only.
void EarlyShadersUnderBench() {
  const bool off0 = g_engineOff.load();
  const uint32_t es0 = g_cfg.earlyShaders;
  g_cfg.earlyShaders = 3;
  g_engineOff = false;
  TickEngineParts();  // no bench: the configured mask is taken
  const uint32_t before = eshd::g_mask.load();
  g_benchRunningFlag = true;
  g_engineOff = true;  // an OFF block
  TickEngineParts();
  const uint32_t offBlock = eshd::g_mask.load();
  g_engineOff = false;  // the next ON block
  TickEngineParts();
  const uint32_t onBlock = eshd::g_mask.load();
  g_benchRunningFlag = false;
  g_cfg.earlyShaders = es0;
  g_engineOff = off0;
  TickEngineParts();
  printf("     early shaders mask: before %u, OFF block %u, ON block %u\n", before, offBlock, onBlock);
  Check(before == 3 && offBlock == 0 && onBlock == 3,
        "invariants: early shaders under the engine A/B: 0 in the OFF blocks, the configured mask again in the ON blocks");
  // The kill switch takes them out at once (not at the worker's next tick).
  eshd::g_mask = 3;
  g_cfg.earlyShaders = 3;
  SetEngineOff(true);
  const uint32_t killed = eshd::g_mask.load();
  SetEngineOff(false);
  g_cfg.earlyShaders = es0;
  g_engineOff = off0;
  eshd::g_mask = 0;
  Check(killed == 0, "invariants: the kill switch zeroes the early shaders mask in SetEngineOff");
  // A mission event reaches the parts that keep per-scene state (exports.inc's text: OnMissionEvent is not run
  // offline, it touches every feature).
  const std::string ex = ReadText(SrcDir() + L"main\\exports.inc");
  const size_t a = ex.find("void OnMissionEvent("), e = a == std::string::npos ? a : ex.find("\n}", a);
  const std::string body = e == std::string::npos ? std::string() : ex.substr(a, e - a);
  Check(body.find("perfmon::g_missionEvents.fetch_add") != std::string::npos &&
            body.find("w1::g_rearms.fetch_add") != std::string::npos &&
            body.find("eshd::NoteMissionEvent()") != std::string::npos,
        "invariants: a mission event reverts the governor's unproven step, re-arms W1's start guard and drops early "
        "shaders' pass caches");
}

// ---- 3. kill switch = stock ------------------------------------------------------------------------------------
// The module-only parts the app never switches on (written on top of the all-on ini), and the second pass:
// G-buffer batching excludes both recorders, so it gets a pass of its own.
struct KeyVal {
  const wchar_t *sec, *key, *val;
};
const KeyVal kModuleOnlyOn[] = {
    {L"Model", L"ParallelUpload", L"1"},  {L"Model", L"DirectUpload", L"1"},     {L"Model", L"SrvTailTrim", L"1"},
    {L"Model", L"PassFlush", L"0xff"},    {L"Model", L"PeripheralLodScale", L"50"}, {L"Model", L"FocusTwin", L"1"},
    {L"Effects", L"SkipSameConstantUpload", L"1"}, {L"General", L"LowPowerPacer", L"1"}, {L"D3D", L"FxApplySkip", L"2"},
    {L"DLSS", L"ReleaseIdleSets", L"1"},  {L"Views", L"ThreeViews", L"1"},       {L"Cull", L"Enabled", L"1"},
    {L"Scene", L"CollectThreadsMax", L"8"}, {L"Scene", L"FineTimerResolution", L"1"}, {L"Shadow", L"TightCasters", L"1"},
    {L"D3D", L"Filter", L"1"},            {L"General", L"Diagnostics", L"0"},
};
// "On" by hand where the off path is a plain assignment, so the kill switch must clear it (offline nothing installs).
void PresetRequested() {
  g_allocSlabsOn = g_frameHeapOn = g_triPlainOn = g_taskClockOn = g_texDedupeOn = g_cbSkipOn = g_cbUploadSkipOn = true;
  g_costWeightsOn = g_partitionBoost = g_pacerLowPowerOn = g_shadowTexSkipOn = true;
  timercache::g_on = true;
  shadowbatch::g_on = gbbatch::g_on = shrec::g_on = gbrec::g_on = parupload::g_on = directupload::g_on = true;
  chainfast::g_mask = 7;
  sfast::g_mask = 3;
  eshd::g_mask = 3;
  plod::g_pct = 50;
  plod::g_swapOn = true;
  w1::g_mode = 3;
  fxskip::g_mode = 2;
  ftwin::g_active = true;
}

void KillSwitch(const std::string& onIni) {
  std::set<std::string> configured;
  for (int pass = 0; pass < 2; ++pass) {
    WriteIni(onIni);
    for (const KeyVal& k : kModuleOnlyOn) SetKey(k.sec, k.key, k.val);
    if (pass == 1) SetKey(L"Model", L"GBufferBatching", L"1");
    Load(true);  // the kill switch is on before the read: nothing installs offline
    for (const EnginePart& p : kEngineParts)
      if (p.configured(g_cfg)) configured.insert(p.name);
    // The DLSS tick reads the NVIDIA driver profile with a preset set: not run offline (its row is source-checked).
    g_cfg.dlssPeripheryPreset = L"0";
    g_cfg.dlssReleaseIdleSets = false;
    g_cfg.dlssInstrument = false;
    // The timer cache's switch acts only once hooked (ApplyTimerConfig returns before it without g_orig): a stand-in
    // original for this pass, so its off path runs (nothing is patched; the stand-in is never called).
    const timercache::GetTimeFn timerOrig0 = timercache::g_orig;
    timercache::g_orig = [] { return 0.0; };
    PresetRequested();
    SetEngineOff(true);
    TickEngineParts();
    std::string left;
    for (const EnginePart& p : kEngineParts)
      if (p.gate == kGateApplied && p.requested && p.requested()) left += (left.empty() ? "" : ", ") + std::string(p.name);
    timercache::g_orig = timerOrig0;
    if (!left.empty()) printf("     pass %d: still requested under the kill switch: %s\n", pass, left.c_str());
    Check(left.empty(), pass == 0 ? "invariants: every part on (app all-on ini + module-only keys), kill switch on: "
                                    "SetEngineOff(true) + TickEngineParts leave no engine part requested"
                                  : "invariants: the same with [Model] GBufferBatching=1 (its own pass: it excludes "
                                    "both recorders)");
  }
  std::string never;
  for (const EnginePart& p : kEngineParts)
    if (!configured.count(p.name)) never += (never.empty() ? "" : ", ") + std::string(p.name);
  if (!never.empty()) printf("     never configured by the kill switch passes: %s\n", never.c_str());
  Check(never.empty(),
        "invariants: the kill switch passes configure every engine part (a new part needs its key in kModuleOnlyOn)");
  // The worker-loop parts: the kill switch reaches their ticks (worker.inc's text).
  const std::string w = ReadText(SrcDir() + L"main\\worker.inc");
  struct Needle {
    const char* part;
    const char* text;
  };
  const Needle needles[] = {
      {"DLSS per-view preset", "g_cfg.dlssIgnoreDriverPreset, g_engineOff.load(),"},
      {"DLSS idle set release", "g_cfg.dlssIgnoreDriverPreset, g_engineOff.load(),"},
      {"worker placement", "g_engineOff.load() ? 0 : g_cfg.workerPlacement"},
      {"three views", "g_cfg.threeViewsKeyMods, g_engineOff.load() || g_faulted.load()"},
  };
  int bad = 0;
  for (const EnginePart& p : kEngineParts) {
    if (p.gate != kGateWorkerLoop) continue;
    const Needle* n = nullptr;
    for (const Needle& x : needles)
      if (!strcmp(x.part, p.name)) n = &x;
    if (!n || w.find(n->text) == std::string::npos) {
      ++bad;
      printf("     %s: %s\n", p.name, n ? "the worker no longer passes the kill switch as before" : "no source check");
    }
  }
  Check(w.size() > 10000 && bad == 0,
        "invariants: the worker passes the kill switch to the DLSS, worker placement and three views ticks");
  // Perception only (Engine Optimizations off): the applied config has no engine part at all.
  WriteIni(onIni);
  Load(true);
  const std::string po = Configured(PerceptionOnlyConfig(g_cfg), false);
  if (!po.empty()) printf("     PerceptionOnlyConfig keeps: %s\n", po.c_str());
  Check(po.empty(), "invariants: PerceptionOnlyConfig of the all-on ini configures no engine part");
}

// ---- 4. / 5. keys: round trip and defaults ---------------------------------------------------------------------
constexpr int kNum = 0, kHotkey = 1, kPrefetch = 2, kPreset = 3;
struct KeyRow {
  const wchar_t *sec, *key;
  int kind;
  const wchar_t* def;  // the module default (no key); nullptr: not pinned here (always written with its feature)
  double (*get)(const wchar_t* ini);
};
#define IV(expr) [](const wchar_t* ini) -> double { return (void)ini, static_cast<double>(expr); }
#define IVF(m) [](const wchar_t* ini) -> double { return static_cast<double>(cgrade::ReadModeIni(ini, 1).m); }
#define IVE(m) [](const wchar_t* ini) -> double { return static_cast<double>(cgrade::ReadModeIni(ini, 2).m); }
#define IVX(m) [](const wchar_t* ini) -> double { return static_cast<double>(vrfx::ReadIni(ini).m); }
#define IVA(expr) [](const wchar_t* ini) -> double { const audcfg::Read r = audcfg::ReadIni(ini); return static_cast<double>(expr); }
const KeyRow kKeys[] = {
    {L"General", L"PerceptionOnly", kNum, L"0", IV(g_perceptionOnly.load())},
    {L"General", L"Diagnostics", kNum, L"0", IV(g_cfg.diagnostics)},
    {L"General", L"Beeps", kNum, L"1", IV(g_beeps.load())},
    {L"Timing", L"ShaderTimeCache", kNum, L"1", IV(g_cfg.shaderTimeCache)},
    {L"Timing", L"TaskQueueClock", kNum, L"0", IV(g_cfg.taskClock)},
    {L"Timing", L"CacheUs", kNum, L"1000", IV(g_cfg.shaderTimeCacheUs)},
    {L"Hotkeys", L"Toggle", kHotkey, L"122:6", IV(g_cfg.toggleVk * 8 + g_cfg.toggleMods)},
    {L"Scene", L"PartitionBoost", kNum, L"0", IV(g_cfg.partitionBoost)},
    {L"Scene", L"CostWeights", kNum, L"0", IV(g_cfg.costWeights)},
    {L"D3D", L"SplitFilter", kNum, L"1", IV(g_cfg.splitFilter)},
    {L"D3D", L"ChainFast", kNum, L"7", IV(g_cfg.chainFast)},
    {L"D3D", L"ShaderFast", kNum, L"3", IV(g_cfg.shaderFast)},
    {L"D3D", L"EarlyShaders", kNum, L"0", IV(g_cfg.earlyShaders)},
    {L"Shadows", L"CascadeCache", kNum, L"0", IV(g_cfg.cascadeCache)},
    {L"Model", L"AllocSlabs", kNum, L"0", IV(g_cfg.allocSlabs)},
    {L"Model", L"PlainTriangleCounter", kNum, L"0", IV(g_cfg.triPlain)},
    {L"Model", L"FrameHeapSlabs", kNum, L"0", IV(g_cfg.frameHeapSlabs)},
    {L"Model", L"BigModelPages", kNum, L"1", IV(g_cfg.bigPages)},
    {L"Model", L"ShadowInstancing", kNum, L"1", IV(g_cfg.shadowInst)},
    {L"Model", L"ShadowBatching", kNum, L"1", IV(g_cfg.shadowBatch)},
    {L"Model", L"ShadowPlanAsync", kNum, L"1", IV(g_cfg.shadowPlanAsync)},
    {L"Model", L"ShadowTextureSkip", kNum, L"1", IV(g_cfg.shadowTexSkip)},
    {L"Model", L"ShadowRecorder", kNum, L"0", IV(g_cfg.shadowRecorder)},
    {L"Model", L"ShadowRecorderHelpers", kNum, L"3", IV(g_cfg.shadowRecorderHelpers)},
    {L"Model", L"GBufferRecorder", kNum, L"0", IV(g_cfg.gbufferRecorder)},
    {L"Model", L"RecordersEmulatedLists", kNum, L"0", IV(g_cfg.recordersEmulatedLists)},
    {L"Model", L"ShaderCachePrewarm", kNum, L"1", IV(qvcachedir::ReadPrewarm(ini))},
    {L"Model", L"ShaderCompileWorkers", kNum, L"0", IV(qvcachedir::ReadWorkers(ini))},
    {L"Texture", L"StreamDedupe", kNum, L"0", IV(g_cfg.texDedupe)},
    {L"Effects", L"SkipSameConstantBuffer", kNum, L"0", IV(g_cfg.cbSkip)},
    {L"Water", L"SkipHiddenWater", kNum, L"0", IV(g_cfg.waterSkip)},
    {L"Prefetch", L"Mode", kPrefetch, L"off", IV(g_cfg.prefetchMode)},
    {L"Threads", L"WorkerPlacement", kNum, L"2", IV(g_cfg.workerPlacement)},
    {L"Governor", L"Enabled", kNum, L"1", IV(g_cfg.govEnabled)},
    {L"DLSS", L"PeripheryPreset", kPreset, L"0", IV(dlssv::ParsePreset(g_cfg.dlssPeripheryPreset.c_str()))},
    {L"DLSS", L"FocusPreset", kPreset, L"0", IV(dlssv::ParsePreset(g_cfg.dlssFocusPreset.c_str()))},
    {L"DLSS", L"IgnoreDriverPreset", kNum, L"0", IV(g_cfg.dlssIgnoreDriverPreset)},
    {L"DLSS", L"ReleaseIdleSets", kNum, L"0", IV(g_cfg.dlssReleaseIdleSets)},
    {L"Log", L"ReloadFrameTimes", kNum, L"0", IV(g_cfg.reloadFrameTimes)},
    {L"Perception", L"FixationDepth", kNum, L"0", IV(g_cfg.fixationDepth)},
    {L"Perception", L"FarDepthExport", kNum, L"0", IV(g_cfg.farDepthExport)},
    {L"Perception", L"CockpitMaskExport", kNum, L"0", IV(g_cfg.cockpitMaskExport)},
    {L"Perception", L"CockpitDepthExport", kNum, L"0", IV(g_cfg.cockpitDepthExport)},
    {L"Perception", L"CockpitMotionExport", kNum, L"0", IV(g_cfg.cockpitMotionExport)},
    {L"Perception", L"CockpitMotionFocus", kNum, L"0", IV(g_cfg.cockpitMotionFocus)},  // motion contract revision 4
    {L"Perception", L"CockpitMotionDepth", kNum, L"0", IV(g_cfg.cockpitMotionDepth)},
    {L"Perception", L"SymbologyExport", kNum, L"0", IV(g_cfg.symbologyExport)},
    {L"Perception", L"CockpitTrueIpd", kNum, L"0", IV(g_cfg.cockpitTrueIpd)},
    {L"Perception", L"CockpitTrueIpdOwnship", kNum, L"0", IV(g_cfg.cockpitTrueIpdOwnship)},
    {L"Color", L"Mode", kNum, L"0", IV(g_colorCfg.mode)},
    {L"Color", L"PanelGamut", kNum, L"0", IVF(panelGamut)},
    {L"Color", L"ModeKey", kHotkey, L"0:0", IV(g_colorKeyVk * 8 + g_colorKeyMods)},
    {L"ColorFix", L"ExposureEV", kNum, nullptr, IVF(exposureEv)},
    {L"ColorFix", L"NightEV", kNum, nullptr, IVF(nightEv)},
    {L"ColorFix", L"Saturation", kNum, nullptr, IVF(saturation)},
    {L"ColorFix", L"Warmth", kNum, nullptr, IVF(warmth)},
    {L"ColorFix", L"AdaptBright", kNum, nullptr, IVF(adaptBright)},
    {L"ColorFix", L"AdaptDark", kNum, nullptr, IVF(adaptDark)},
    {L"ColorEnhanced", L"ExposureEV", kNum, nullptr, IVE(exposureEv)},
    {L"ColorEnhanced", L"NightEV", kNum, nullptr, IVE(nightEv)},
    {L"ColorEnhanced", L"Saturation", kNum, nullptr, IVE(saturation)},
    {L"ColorEnhanced", L"Warmth", kNum, nullptr, IVE(warmth)},
    {L"ColorEnhanced", L"AdaptBright", kNum, nullptr, IVE(adaptBright)},
    {L"ColorEnhanced", L"AdaptDark", kNum, nullptr, IVE(adaptDark)},
    {L"ColorEnhanced", L"NightVision", kNum, nullptr, IVE(nightVision)},
    {L"ColorEnhanced", L"NightVisionStrength", kNum, nullptr, IVE(nvStrength)},
    {L"ColorEnhanced", L"NightVisionBlue", kNum, nullptr, IVE(nvBlue)},
    {L"ColorEnhanced", L"NightVisionBlur", kNum, nullptr, IVE(nvBlur)},
    {L"ColorEnhanced", L"NightVisionScotopic", kNum, nullptr, IVE(nvScotopic)},
    {L"ColorEnhanced", L"NightVisionPhotopic", kNum, nullptr, IVE(nvPhotopic)},
    {L"ColorEnhanced", L"Glare", kNum, nullptr, IVE(glare)},
    {L"ColorEnhanced", L"GlareStrength", kNum, nullptr, IVE(glareStrength)},
    {L"ColorEnhanced", L"GlareThresholdEV", kNum, nullptr, IVE(glareThresholdEv)},
    {L"ColorEnhanced", L"GlareRadius", kNum, nullptr, IVE(glareRadiusDeg)},
    {L"ColorEnhanced", L"GlareStreak", kNum, nullptr, IVE(glareStreak)},
    {L"VrFx", L"GazeDof", kNum, L"0", IVX(dof)},
    {L"VrFx", L"GazeDofStrength", kNum, nullptr, IVX(dofStrength)},
    {L"VrFx", L"GazeDofMaxArcmin", kNum, nullptr, IVX(dofMaxArcmin)},
    {L"VrFx", L"GazeDofSaccadeRelease", kNum, nullptr, IVX(dofSaccadeRelease)},
    {L"VrFx", L"GazeDofSettleMs", kNum, nullptr, IVX(dofSettleMs)},
    {L"VrFx", L"GazeDofFadeInMs", kNum, nullptr, IVX(dofFadeInMs)},
    {L"VrFx", L"GazeDofToleranceD", kNum, nullptr, IVX(dofToleranceD)},
    {L"VrFx", L"GazeDofFoveaGuardDeg", kNum, nullptr, IVX(dofGuardDeg)},
    {L"VrFx", L"GazeDofPupilMm", kNum, nullptr, IVX(dofPupilMm)},
    {L"VrFx", L"GVision", kNum, L"0", IVX(gv)},
    {L"VrFx", L"GVisionStrength", kNum, nullptr, IVX(gvStrength)},
    {L"VrFx", L"GVisionUseGaze", kNum, nullptr, IVX(gvUseGaze)},
    {L"VrFx", L"GVisionRedout", kNum, nullptr, IVX(gvRedout)},
    {L"VrFx", L"GVisionTolerance", kNum, nullptr, IVX(gvTolerance)},
    {L"VrFx", L"GVisionMode", kNum, nullptr, IVX(gvMode)},
    // Binaural audio (native/binaural/include/binaural_config.h; read by the audio module's own reader).
    {L"Audio", L"Binaural", kNum, L"0", IVA(r.cfg.binaural)},
    {L"Audio", L"HeadCircumferenceCm", kNum, L"57", IVA(r.cfg.headCircumferenceCm)},
    {L"Audio", L"LateHeadTracking", kNum, L"1", IVA(r.cfg.lateHeadTracking)},
    {L"Audio", L"NearField", kNum, L"1", IVA(r.cfg.nearField)},
    {L"Audio", L"MaxObjects", kNum, L"48", IVA(r.cfg.maxObjects)},
    {L"Audio", L"LevelTrimDb", kNum, L"0", IVA(r.cfg.levelTrimDb)},
    {L"Audio", L"CpuBudgetPercent", kNum, L"25", IVA(r.cfg.cpuBudgetPercent)},
    {L"Audio", L"LogVoices", kNum, L"0", IVA(r.cfg.logVoices)},
    {L"Hotkeys", L"AudioToggle", kHotkey, nullptr, IVA(r.toggleVk * 8 + r.toggleMods)},
};
#undef IV
#undef IVF
#undef IVE
#undef IVX
#undef IVA
// Keys the app writes that kKeys does not read back here, with the reason.
const struct {
  const wchar_t *sec, *key, *why;
} kKeysNotRead[] = {
    {L"Dev", L"PayloadPath", L"developer path, empty unless developer mode; read whole by the loader (r62_dev_ini_test)"},
    {L"Dev", L"IniPath", L"developer path, empty unless developer mode; read whole by qvdev::ReadDevPath (r62_dev_ini_test)"},
    {L"Model", L"ShaderCacheDir", L"a folder; resolved by qvcachedir (shader_cache_dir_test)"},
    {L"Audio", L"Hrtf", L"a set name (text); read and validated by audcfg::ReadIni (audio_test Config)"},
    {L"Audio", L"ExcludeContexts", L"a list of context ids (text); parsed by binaural_config::ParseExcludeContexts"},
};

double Expected(int kind, const std::wstring& v, bool* ok) {
  *ok = true;
  if (kind == kHotkey) {
    int vk = 0, mods = 0;
    *ok = ParseHotkey(v.c_str(), vk, mods);
    return vk * 8 + mods;
  }
  if (kind == kPrefetch) return _wcsicmp(v.c_str(), L"skip") == 0 ? 2 : _wcsicmp(v.c_str(), L"observe") == 0 ? 1 : 0;
  if (kind == kPreset) return dlssv::ParsePreset(v.c_str());
  wchar_t* end = nullptr;
  const double d = (v.size() > 2 && v[0] == L'0' && (v[1] == L'x' || v[1] == L'X'))
                       ? static_cast<double>(wcstoul(v.c_str(), &end, 16))
                       : wcstod(v.c_str(), &end);
  *ok = end && end != v.c_str();
  return d;
}
bool Same(double a, double b) { return std::fabs(a - b) <= 1e-4 * (std::max)(1.0, std::fabs(b)); }

// The [section] key=value lines of an ini text.
struct Line {
  std::wstring sec, key, val;
};
std::vector<Line> Lines(const std::string& text) {
  std::vector<Line> out;
  std::wstring sec;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    std::string l = text.substr(pos, nl - pos);
    pos = nl + 1;
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
    if (l.empty() || l[0] == ';' || l[0] == '#') continue;
    if (l[0] == '[' && l.back() == ']') {
      sec = std::wstring(l.begin() + 1, l.end() - 1);
      continue;
    }
    const size_t eq = l.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    out.push_back({sec, std::wstring(l.begin(), l.begin() + eq), std::wstring(l.begin() + eq + 1, l.end())});
  }
  return out;
}

void RoundTrip(const char* name, const std::string& text) {
  WriteIni(text);
  Load(true);
  const std::wstring ini = IniPath();
  int checked = 0, bad = 0, unknown = 0;
  for (const Line& l : Lines(text)) {
    const KeyRow* row = nullptr;
    for (const KeyRow& r : kKeys)
      if (!_wcsicmp(r.sec, l.sec.c_str()) && !_wcsicmp(r.key, l.key.c_str())) row = &r;
    if (!row) {
      bool listed = false;
      for (const auto& n : kKeysNotRead) listed = listed || (!_wcsicmp(n.sec, l.sec.c_str()) && !_wcsicmp(n.key, l.key.c_str()));
      if (!listed) ++unknown, printf("     %s: [%ls] %ls has no read-back row (invariants_test.h kKeys)\n", name, l.sec.c_str(), l.key.c_str());
      continue;
    }
    bool ok = false;
    const double want = Expected(row->kind, l.val, &ok), got = row->get(ini.c_str());
    ++checked;
    if (!ok || !Same(got, want)) {
      ++bad;
      printf("     %s: [%ls] %ls=%ls read back as %g (want %g)\n", name, l.sec.c_str(), l.key.c_str(), l.val.c_str(), got,
             want);
    }
  }
  printf("     %s: %d keys read back\n", name, checked);
  char what[200];
  snprintf(what, sizeof(what), "invariants: every key of app_ini/%s is read by the module with the value the app wrote", name);
  Check(checked > 20 && bad == 0 && unknown == 0, what);
}

void Defaults() {
  WriteIni("; no keys\n");
  Load(true);
  const std::wstring ini = IniPath();
  int pinned = 0, bad = 0;
  for (const KeyRow& r : kKeys) {
    if (!r.def) continue;
    bool ok = false;
    const double want = Expected(r.kind, r.def, &ok), got = r.get(ini.c_str());
    ++pinned;
    if (!ok || !Same(got, want)) {
      ++bad;
      printf("     default [%ls] %ls is %g, the table says %ls\n", r.sec, r.key, got, r.def);
    }
  }
  printf("     module defaults pinned: %d keys\n", pinned);
  Check(pinned > 40 && bad == 0,
        "invariants: the module's default of every user-facing key (no ini) is the value in kKeys (change both on purpose)");
}

// ---- 6. system DLLs ---------------------------------------------------------------------------------------------
// Inside DCS a bare LoadLibraryW(L"x.dll") searches DCS's own folder first: a stray x.dll there would be loaded into
// the game. Every system DLL the module loads by name goes through LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_SYSTEM32)
// (or a full path). Returns the offending "file: call" lines; exposed for the self-check below.
std::vector<std::string> BareSystemLoads(const std::string& text, const std::string& file) {
  static const std::regex bare(R"(LoadLibrary[AW]?\s*\(\s*L?"[^"\\/]+"\s*\))");
  static const std::regex exZero(R"(LoadLibraryEx[AW]\s*\(\s*L?"[^"\\/]+"\s*,\s*[A-Za-z_]+\s*,\s*0\s*\))");
  std::vector<std::string> out;
  for (const std::regex* re : {&bare, &exZero})
    for (std::sregex_iterator it(text.begin(), text.end(), *re), end; it != end; ++it) out.push_back(file + ": " + it->str());
  return out;
}
void SystemDllLoads() {
  // Self-check of the pattern: the old forms are caught, the fixed ones are not.
  const bool self = BareSystemLoads(R"(LoadLibraryW(L"nvml.dll");)", "t").size() == 1 &&
                    BareSystemLoads(R"(LoadLibraryExW(L"version.dll", nullptr, 0))", "t").size() == 1 &&
                    BareSystemLoads(R"(LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))", "t").empty() &&
                    BareSystemLoads(R"(LoadLibraryW((std::wstring(sys) + L"\\d3dcompiler_47.dll").c_str()))", "t").empty();
  std::vector<std::string> bad;
  int files = 0;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(SrcDir(), ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    const auto ext = it->path().extension().wstring();
    if (!it->is_regular_file() || (ext != L".h" && ext != L".inc" && ext != L".cpp" && ext != L".c")) continue;
    ++files;
    for (auto& b : BareSystemLoads(ReadText(it->path().wstring()), it->path().filename().string())) bad.push_back(b);
  }
  for (auto& b : bad) printf("     bare system DLL load: %s\n", b.c_str());
  printf("     system DLL loads: %d source files scanned\n", files);
  Check(self && files > 50 && bad.empty(),
        "invariants: every system DLL the module loads by name is searched in System32 only (no DCS-folder hijack)");
}

void Run() {
  const std::string off = ReadText(IniDir() + L"engine_all_off.ini"), on = ReadText(IniDir() + L"engine_all_on.ini");
  Registration();
  SystemDllLoads();
  if (off.size() < 200 || on.size() < 200) {
    Check(false, "invariants: the app's inis (app_ini/engine_all_off.ini, engine_all_on.ini) were found");
    return;
  }
  if (g_benchRunningFlag.load()) {
    Check(false, "invariants: no benchmark runs");
    return;
  }
  Saved saved;
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  g_dir = std::wstring(tmp) + L"qv_invariants_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
  CreateDirectoryW(g_dir.c_str(), nullptr);
  AllOff(off);
  EarlyShadersUnderBench();
  KillSwitch(on);
  RoundTrip("engine_all_off.ini", off);
  RoundTrip("engine_all_on.ini", on);
  Defaults();
  DeleteFileW(IniPath().c_str());
  RemoveDirectoryW(g_dir.c_str());
}

}  // namespace invtest
