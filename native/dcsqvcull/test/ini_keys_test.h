// R107: the ini keys DCS Control writes (dlss5vrframegen src\DcsVr.Core\ConfigurationWriters.cs, DcsQvCull(),
// ColorModeLine and VisualSections, as of 2026-10-21) are all still read by the payload: either live (the key's
// literal is in the source) or retired (kRetiredKeys: ignored, a non-zero value logged once). An unknown key is
// harmless by construction (GetPrivateProfile* reads only the keys asked for), so a removal can never break the
// app's ini; this keeps a removal from silently dropping a key the app still sets with a meaning. `constant`: the
// template writes a fixed value (a non-constant, profile-driven key must stay live, never retired).
// Since Dev's F07 trim (2026-10-21) the template writes only the constant=false entries (76 keys, PanelGamut only when not 0); the constant
// ones stay listed so an older profile ini (full template) still reads the same values or the default.
#pragma once

namespace iktest {

struct AppKey {
  const wchar_t* sec;
  const wchar_t* key;
  bool constant;
};
const AppKey kAppKeys[] = {
    {L"Color", L"Mode", false}, {L"Color", L"PanelGamut", false}, {L"ColorFix", L"ExposureEV", false}, {L"ColorFix", L"NightEV", false},
    {L"ColorFix", L"Saturation", false}, {L"ColorFix", L"Warmth", false}, {L"ColorFix", L"AdaptBright", false},
    {L"ColorFix", L"AdaptDark", false}, {L"ColorEnhanced", L"ExposureEV", false},
    {L"ColorEnhanced", L"NightEV", false}, {L"ColorEnhanced", L"Saturation", false},
    {L"ColorEnhanced", L"Warmth", false}, {L"ColorEnhanced", L"AdaptBright", false},
    {L"ColorEnhanced", L"AdaptDark", false}, {L"ColorEnhanced", L"NightVision", false},
    {L"ColorEnhanced", L"NightVisionStrength", false}, {L"ColorEnhanced", L"NightVisionBlue", false},
    {L"ColorEnhanced", L"NightVisionBlur", false}, {L"ColorEnhanced", L"NightVisionScotopic", false},
    {L"ColorEnhanced", L"NightVisionPhotopic", false}, {L"ColorEnhanced", L"Glare", false},
    {L"ColorEnhanced", L"GlareStrength", false}, {L"ColorEnhanced", L"GlareThresholdEV", false},
    {L"ColorEnhanced", L"GlareRadius", false}, {L"ColorEnhanced", L"GlareStreak", false},
    {L"VrFx", L"GazeDof", false}, {L"VrFx", L"GazeDofStrength", false}, {L"VrFx", L"GazeDofMaxArcmin", false},
    {L"VrFx", L"GazeDofSaccadeRelease", false}, {L"VrFx", L"GazeDofSettleMs", false},
    {L"VrFx", L"GazeDofFadeInMs", false}, {L"VrFx", L"GazeDofToleranceD", false},
    {L"VrFx", L"GazeDofFoveaGuardDeg", false}, {L"VrFx", L"GazeDofPupilMm", false}, {L"VrFx", L"GVision", false},
    {L"VrFx", L"GVisionStrength", false}, {L"VrFx", L"GVisionUseGaze", false}, {L"VrFx", L"GVisionRedout", false},
    {L"VrFx", L"GVisionTolerance", false}, {L"VrFx", L"GVisionMode", false}, {L"General", L"PerceptionOnly", false},
    {L"General", L"Diagnostics", false}, {L"General", L"Beeps", false}, {L"General", L"LowPowerPacer", true},
    {L"General", L"LowPowerPacerTimeoutUs", true}, {L"General", L"SigScan", true},
    {L"Timing", L"ShaderTimeCache", false}, {L"Timing", L"TaskQueueClock", false}, {L"Timing", L"CacheUs", false},
    {L"Hotkeys", L"Toggle", false}, {L"Hotkeys", L"DeveloperKeys", true}, {L"Scene", L"PartitionBoost", false},
    {L"Scene", L"CostWeights", false}, {L"Scene", L"CostWeightsSanity", true},
    {L"Scene", L"CollectThreadsMax", true}, {L"Scene", L"FineTimerResolution", true}, {L"Cull", L"Enabled", true},
    {L"Cull", L"DebugHole", true}, {L"Cull", L"FocusKeepFraction", true}, {L"Cull", L"SafetyNdc", true},
    {L"Cull", L"NativeKeepFraction", true}, {L"Cull", L"SaccadeDeg", true}, {L"Cull", L"SaccadeHoldFrames", true},
    {L"Cull", L"FocusRatio", true}, {L"Cull", L"ApexTolerance", true}, {L"D3D", L"Meter", true},
    {L"D3D", L"Filter", true}, {L"D3D", L"SplitFilter", false}, {L"D3D", L"SplitFilterOps", true},
    {L"D3D", L"ChainFast", true}, {L"D3D", L"ShaderFast", true}, {L"D3D", L"PeriphInstFilter", true},
    {L"D3D", L"FocusBroadcast", true}, {L"Shadows", L"CascadeCache", false}, {L"Shadow", L"TightCasters", true},
    {L"Shadow", L"DebugInvert", true}, {L"Model", L"AllocSlabs", false}, {L"Model", L"SlabBytes", true},
    {L"Model", L"PlainTriangleCounter", false}, {L"Model", L"FrameHeapSlabs", false},
    {L"Model", L"BigModelPages", false}, {L"Model", L"BigPageBytes", true}, {L"Model", L"ShadowInstancing", false},
    {L"Model", L"ShadowBatching", false}, {L"Model", L"ShadowPlanAsync", false},
    {L"Model", L"ShadowTextureSkip", false}, {L"Model", L"GBufferBatching", true},
    {L"Model", L"ParallelUpload", true}, {L"Model", L"DirectUpload", true}, {L"Model", L"ShadowRecorder", false},
    {L"Model", L"ShadowRecorderScope", true}, {L"Model", L"ShadowRecorderWaitUs", true},
    {L"Model", L"ShadowRecorderPriority", true}, {L"Model", L"ShadowRecorderSplit", true},
    {L"Model", L"ShadowRecorderInstancing", true}, {L"Model", L"ShadowRecorderHelpers", false},
    {L"Model", L"GBufferRecorder", false}, {L"Model", L"GBufferRecorderScope", true},
    {L"Model", L"GBufferRecorderMaxSegments", true}, {L"Model", L"GBufferRecorderHelpers", true},
    {L"Model", L"GBufferRecorderWaitUs", true}, {L"Model", L"GBufferRecorderIsland", true},
    {L"Model", L"GBufferRecorderRedo", true}, {L"Model", L"GBufferRecorderSwapAhead", true},
    {L"Model", L"GBufferRecorderCockpit", true}, {L"Model", L"GBufferRecorderByOrdinal", true},
    {L"Model", L"SrvTailTrim", true}, {L"Model", L"PassFlush", true}, {L"Texture", L"StreamDedupe", false},
    {L"Effects", L"SkipSameConstantBuffer", false}, {L"Effects", L"SkipSameConstantUpload", true},
    {L"Prefetch", L"Mode", false}, {L"Prefetch", L"WindowMs", true}, {L"Prefetch", L"TableBits", true},
    {L"Threads", L"Census", true}, {L"Threads", L"WorkerPlacement", true}, {L"Threads", L"PlaceRecheckSec", true},
    {L"Color", L"ModeKey", false}, {L"Color", L"ToneProbe", true}, {L"Color", L"SwapProbe", true},
    {L"Log", L"StatsIntervalSec", true}, {L"Log", L"DumpViews", true}, {L"Log", L"RecorderDetail", true},
    {L"Log", L"PerfIntervalSec", true}, {L"Log", L"VramIntervalSec", true}, {L"Governor", L"Enabled", true},
    {L"Governor", L"GpuBusyPct", true}, {L"Governor", L"RenderIdlePct", true}, {L"Governor", L"CpuBusyPct", true},
    {L"Governor", L"GpuFreePct", true}, {L"Governor", L"GpuFullPct", true}, {L"Governor", L"WindowSec", true},
    {L"Governor", L"NeedSec", true}, {L"Governor", L"MinDwellSec", true}, {L"Governor", L"MinGainPct", true},
    {L"Governor", L"TrialSec", true}, {L"Governor", L"RecheckSec", true}, {L"Governor", L"RevertBackoffSec", true},
    {L"Perception", L"FixationDepth", false}, {L"Perception", L"DepthGrid", true},
    {L"Perception", L"SceneCutReset", true}, {L"Perception", L"FixationDepthVerify", true},
    {L"Perception", L"FixationDepthDump", true}, {L"Perception", L"FarDepthExport", false},
    {L"Perception", L"FarDepthExportSlots", true}, {L"Perception", L"FarDepthPreOverlay", true},
    {L"Perception", L"CockpitMaskExport", false}, {L"Perception", L"CockpitMaskViews", true},
    {L"Perception", L"CockpitMaskDivisor", true}, {L"Perception", L"CockpitMaskNearM", true},
    {L"Perception", L"CockpitMaskDilate", true}, {L"Perception", L"CockpitMaskDump", true},
    {L"Perception", L"CockpitMaskLog", true}, {L"Perception", L"CockpitDepthExport", false},
    {L"Perception", L"CockpitDepthDump", true}, {L"Perception", L"CockpitMotionExport", false},
    {L"Perception", L"CockpitMotionClass", true}, {L"Perception", L"CockpitMotionDump", true},
    {L"Perception", L"CockpitFgDump", true}, {L"Perception", L"CockpitMaskOverlay", true},
    {L"Perception", L"SymbologyExport", false}, {L"Perception", L"SymbologyAlpha", true},
    {L"Perception", L"FarDepthCoverageMode", true}, {L"Perception", L"FarDepthLogProj", true},
    {L"Perception", L"CockpitTrueIpd", false}, {L"Perception", L"CockpitTrueIpdLighting", true},
    {L"Perception", L"CockpitTrueIpdForward", true}, {L"Perception", L"CockpitTrueIpdHelmet", true},
    {L"Perception", L"CockpitTrueIpdOverlays", true}, {L"Perception", L"CockpitTrueIpdOverlayPath", true},
    {L"Perception", L"CockpitTrueIpdOwnship", false}, {L"Perception", L"CockpitTrueIpdToggle", true},
    {L"Bench", L"Blocks", true}, {L"Bench", L"BlockSec", true}, {L"Bench", L"SettleSec", true},
    {L"Bench", L"AutoStartSec", true}, {L"Bench", L"ThreadsB", true}, {L"Suite", L"SelfTest", true},
    {L"Suite", L"Profile", true}, {L"Suite", L"BenchCull", true}, {L"Suite", L"BenchTimer", true},
    {L"Suite", L"BenchPartition", true}, {L"Suite", L"BenchIsolation", true}, {L"Suite", L"BenchThreads", true},
    {L"Suite", L"BenchTimerRes", true}, {L"Suite", L"BenchFilter", true}, {L"Suite", L"BenchShadow", true},
    {L"Suite", L"BenchAllocSlabs", true}, {L"Suite", L"BenchCostWeights", true}, {L"Suite", L"BenchCbUpload", true},
    {L"Suite", L"BenchPacer", true}, {L"Suite", L"MotionSweep", true}, {L"Suite", L"BenchFrameHeap", true},
    {L"Suite", L"MotionProfile", true}, {L"Suite", L"MotionTaxi", true}, {L"Suite", L"MotionCounters", true},
    {L"Suite", L"ShadowInstCompile", true}, {L"Suite", L"ShadowInstVerify", true},
    {L"Suite", L"BenchShadowInst", true}, {L"Suite", L"BenchBigPages", true}, {L"Suite", L"BenchShadowTex", true},
    {L"Suite", L"BenchTexTable", true}, {L"Suite", L"YawScan", true}, {L"Suite", L"HoldYawDeg", true},
    {L"Suite", L"GBufferInstCompile", true}, {L"Suite", L"GBufferInstVerify", true},
    {L"Suite", L"BenchGBufferInst", true}, {L"Suite", L"BenchParallelUpload", true},
    {L"Suite", L"BenchShadowPlanAsync", true}, {L"Suite", L"DirectUploadCount", true},
    {L"Suite", L"DirectUploadVerify", true}, {L"Suite", L"BenchDirectUpload", true},
    {L"Suite", L"SplitFilterVerify", true}, {L"Suite", L"ShadowRecVerify", true},
    {L"Suite", L"ShadowRecVerifySec", true}, {L"Suite", L"BenchShadowRecorder", true},
    {L"Suite", L"ShadowRecCount", true}, {L"Suite", L"GBufferRecVerify", true},
    {L"Suite", L"GBufferRecVerifySec", true}, {L"Suite", L"GBufferRecVerifyStride", true},
    {L"Suite", L"BenchGBufferRecorder", true}, {L"Suite", L"GBufferRecCount", true},
    {L"Suite", L"GBufferRecStateDump", true}, {L"Suite", L"GpuPassTiming", true}, {L"Suite", L"YawProfile", true},
    {L"Suite", L"YawProfileStep", true}, {L"Suite", L"RotationProfile", true},
    {L"Suite", L"RotationDegPerSec", true}, {L"Suite", L"RotationSeconds", true},
    {L"Suite", L"FrameStartGap", true}, {L"Suite", L"FrameStartGapSec", true}, {L"Suite", L"RunnableThreads", true},
    {L"Suite", L"RunnableThreadsSec", true}, {L"Suite", L"RunnableThreadsEvery", true},
    {L"Suite", L"VramCount", true}, {L"Suite", L"VramCountSec", true}, {L"Suite", L"VramCountCreates", true},
    {L"Suite", L"GpuPassStats", true}, {L"Suite", L"ForwardRecCount", true}, {L"Suite", L"BenchPassFlush", true},
    {L"Suite", L"BenchPassFlushMask", true}, {L"Suite", L"SrvTailTrimVerify", true},
    {L"Suite", L"SrvTailTrimVerifySec", true}, {L"Suite", L"BenchSrvTailTrim", true},
    {L"Suite", L"BenchSplitFilter", true}, {L"Suite", L"CascadeCacheTiming", true},
    {L"Suite", L"ChainFastTiming", true}, {L"Suite", L"ChainFastVerify", true},
    {L"Suite", L"ShaderFastTiming", true}, {L"Suite", L"ShaderFastVerify", true}, {L"Suite", L"GeoOnlyProbe", true},
    {L"Suite", L"PeriphCensus", true}, {L"Suite", L"PeriphInstTiming", true}, {L"Suite", L"PeriphInstVerify", true},
    {L"Suite", L"FocusGeomProbe", true}, {L"Suite", L"FocusVsCensus", true}, {L"Suite", L"FocusBcastProbe", true},
    {L"Suite", L"FocusBcastTiming", true}, {L"Suite", L"FocusBcastVerify", true},
    {L"Suite", L"JoinTailCount", true}, {L"Suite", L"SrvSpanCount", true}, {L"Suite", L"FxApplyCount", true},
    {L"Suite", L"GBufferTexCount", true}, {L"Suite", L"Terrain", true}, {L"Suite", L"BenchTexDedupe", true},
    {L"Suite", L"BenchCbSkip", true}, {L"Suite", L"BenchTriPlain", true}, {L"Suite", L"BenchEngine", true},
    {L"Suite", L"BenchMicro", true}, {L"Suite", L"BenchPrefetch", true}, {L"Suite", L"Quick", true},
    {L"Suite", L"MotionSeconds", true}, {L"Dev", L"PayloadPath", false}, {L"Dev", L"IniPath", false},
};

bool Retired(const wchar_t* sec, const wchar_t* key) {
  for (const RetiredKey& r : kRetiredKeys)
    if (!_wcsicmp(r.sec, sec) && !_wcsicmp(r.key, key)) return true;
  return false;
}

// R115 I1: the sources without the kRetiredKeys table: its literals ({L"Color", L"Saturation", ...}) would otherwise
// count a key as read. *cut: the table was found (from "kRetiredKeys[] = {" to its "};") and cut out.
std::string WithoutRetiredTable(const std::string& src, bool* cut) {
  *cut = false;
  const size_t a = src.find("kRetiredKeys[] = {");
  if (a == std::string::npos) return src;
  const size_t e = src.find("};", a);
  if (e == std::string::npos) return src;
  const std::string table = src.substr(a, e - a);
  *cut = table.size() < 16384 && table.find("L\"FocusBroadcast\"") != std::string::npos &&
         table.find("L\"CockpitMaskOverlay\"") != std::string::npos;
  return src.substr(0, a) + src.substr(e + 2);
}

void AppKeysRead() {
  bool cut = false;
  const std::string src = WithoutRetiredTable(lptest::ReadAllSources(), &cut);
  int live = 0, retired = 0, missing = 0, retiredNonConst = 0;
  for (const AppKey& k : kAppKeys) {
    if (Retired(k.sec, k.key)) {
      ++retired;
      if (!k.constant) {
        ++retiredNonConst;
        printf("     app key [%ls] %ls is retired but the app writes a profile-driven value\n", k.sec, k.key);
      }
      continue;
    }
    char lit[96];
    snprintf(lit, sizeof(lit), "L\"%ls\"", k.key);
    if (src.find(lit) != std::string::npos) {
      ++live;
    } else {
      ++missing;
      printf("     app key [%ls] %ls: not read by the payload\n", k.sec, k.key);
    }
  }
  printf("     app template keys: %zu (%d live, %d retired)\n", sizeof(kAppKeys) / sizeof(kAppKeys[0]), live, retired);
  Check(missing == 0 && retiredNonConst == 0 && src.size() > 100000 && cut,
        "R107 ini keys: every key DCS Control's template writes is read by the payload, live or retired (and a "
        "retired one is only ever written as a constant; the retired-key table's own literals do not count)");
}

// R115 I1: the colour keys the app writes ([ColorFix], [ColorEnhanced], [Color] PanelGamut) reach ModeParams: each
// written with a distinct in-range value that is not the mode's default, read back by the payload's reader.
void ColorKeysRead() {
  using cgrade::ModeParams;
#define IK_F(m) [](const ModeParams& q) -> double { return static_cast<double>(q.m); }
  struct ColorKey {
    const wchar_t* sec;
    const wchar_t* key;
    const wchar_t* text;
    double want;
    double (*get)(const ModeParams&);
  };
  const ColorKey keys[] = {
      {L"ColorFix", L"ExposureEV", L"0.25", 0.25, IK_F(exposureEv)},
      {L"ColorFix", L"NightEV", L"-0.5", -0.5, IK_F(nightEv)},
      {L"ColorFix", L"Saturation", L"0.75", 0.75, IK_F(saturation)},
      {L"ColorFix", L"Warmth", L"0.125", 0.125, IK_F(warmth)},
      {L"ColorFix", L"AdaptBright", L"2.5", 2.5, IK_F(adaptBright)},
      {L"ColorFix", L"AdaptDark", L"4.5", 4.5, IK_F(adaptDark)},
      {L"ColorEnhanced", L"ExposureEV", L"-0.75", -0.75, IK_F(exposureEv)},
      {L"ColorEnhanced", L"NightEV", L"1.25", 1.25, IK_F(nightEv)},
      {L"ColorEnhanced", L"Saturation", L"1.375", 1.375, IK_F(saturation)},
      {L"ColorEnhanced", L"Warmth", L"0.625", 0.625, IK_F(warmth)},
      {L"ColorEnhanced", L"AdaptBright", L"3.5", 3.5, IK_F(adaptBright)},
      {L"ColorEnhanced", L"AdaptDark", L"7.5", 7.5, IK_F(adaptDark)},
      {L"ColorEnhanced", L"NightVision", L"0", 0, IK_F(nightVision)},
      {L"ColorEnhanced", L"NightVisionStrength", L"0.55", 0.55, IK_F(nvStrength)},
      {L"ColorEnhanced", L"NightVisionBlue", L"0.65", 0.65, IK_F(nvBlue)},
      {L"ColorEnhanced", L"NightVisionBlur", L"0.15", 0.15, IK_F(nvBlur)},
      {L"ColorEnhanced", L"NightVisionScotopic", L"0.02", 0.02, IK_F(nvScotopic)},
      {L"ColorEnhanced", L"NightVisionPhotopic", L"12", 12, IK_F(nvPhotopic)},
      {L"ColorEnhanced", L"Glare", L"0", 0, IK_F(glare)},
      {L"ColorEnhanced", L"GlareStrength", L"0.45", 0.45, IK_F(glareStrength)},
      {L"ColorEnhanced", L"GlareThresholdEV", L"5.5", 5.5, IK_F(glareThresholdEv)},
      {L"ColorEnhanced", L"GlareRadius", L"9", 9, IK_F(glareRadiusDeg)},
      {L"ColorEnhanced", L"GlareStreak", L"0.7", 0.7, IK_F(glareStreak)},
      {L"Color", L"PanelGamut", L"2", 2, IK_F(panelGamut)},  // [Color]: read by both modes
  };
#undef IK_F
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring ini = std::wstring(tmp) + L"qv_colorkeys_" + std::to_wstring(GetCurrentProcessId()) + L".ini";
  DeleteFileW(ini.c_str());
  for (const ColorKey& k : keys) WritePrivateProfileStringW(k.sec, k.key, k.text, ini.c_str());
  const ModeParams f = cgrade::ReadModeIni(ini.c_str(), 1), e = cgrade::ReadModeIni(ini.c_str(), 2);
  const ModeParams fd = cgrade::ModeDefaults(1), ed = cgrade::ModeDefaults(2);
  DeleteFileW(ini.c_str());
  int bad = 0;
  for (const ColorKey& k : keys) {
    const bool fix = !_wcsicmp(k.sec, L"ColorFix"), enh = !_wcsicmp(k.sec, L"ColorEnhanced");
    const bool ok = (enh || k.get(f) == k.want) && (fix || k.get(e) == k.want) && (enh || k.get(fd) != k.want) &&
                    (fix || k.get(ed) != k.want);
    if (!ok) {
      ++bad;
      printf("     [%ls] %ls=%ls: not read into ModeParams (fix %g, enhanced %g)\n", k.sec, k.key, k.text, k.get(f),
             k.get(e));
    }
  }
  // Every colour key in kAppKeys is in the table above (a new app key gets a functional check, not just a literal).
  int uncovered = 0;
  for (const AppKey& a : kAppKeys) {
    const bool colour = !_wcsicmp(a.sec, L"ColorFix") || !_wcsicmp(a.sec, L"ColorEnhanced") ||
                        (!_wcsicmp(a.sec, L"Color") && !_wcsicmp(a.key, L"PanelGamut"));
    if (!colour) continue;
    bool in = false;
    for (const ColorKey& k : keys) in = in || (!_wcsicmp(k.sec, a.sec) && !_wcsicmp(k.key, a.key));
    if (!in) {
      ++uncovered;
      printf("     app colour key [%ls] %ls has no functional read check\n", a.sec, a.key);
    }
  }
  Check(bad == 0 && uncovered == 0,
        "R115 ini keys: every [ColorFix] / [ColorEnhanced] / [Color] PanelGamut key DCS Control writes is read by "
        "ReadModeIni into its own field (distinct non-default values round-trip)");
}

void RetiredKeysLogged() {
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring ini = std::wstring(tmp) + L"qv_retired_" + std::to_wstring(GetCurrentProcessId()) + L".ini";
  // The app's template: every retired key it writes is a constant 0 -> nothing logged. R115 I2: written through
  // WritePrivateProfileStringW, one section per name (GetPrivateProfile* reads only the first section of a name).
  DeleteFileW(ini.c_str());
  int written0 = 0, read0 = 0;
  for (const AppKey& k : kAppKeys)
    if (Retired(k.sec, k.key)) {
      ++written0;
      WritePrivateProfileStringW(k.sec, k.key, L"0", ini.c_str());
    }
  for (const AppKey& k : kAppKeys)
    if (Retired(k.sec, k.key)) read0 += GetPrivateProfileIntW(k.sec, k.key, -1, ini.c_str()) == 0 ? 1 : 0;
  const int none = NoteRetiredKeys(ini);
  // A dev ini that still turns a removed feature on: logged once; 0, empty and junk stay silent.
  {
    FILE* f = nullptr;
    _wfopen_s(&f, ini.c_str(), L"w");
    if (f) {
      fputs("[D3D]\nFocusBroadcast=2\nFocusBroadcastKinds=0\nFocusBroadcastArchive=\n[Suite]\nFocusGeomProbe=abc\n", f);
      fclose(f);
    }
  }
  const int first = NoteRetiredKeys(ini), again = NoteRetiredKeys(ini);
  // R115 I3: the dev ini's old tuning defaults (DcsQvCull\dev\DcsQvCull.dev.ini) are silent.
  {
    FILE* f = nullptr;
    _wfopen_s(&f, ini.c_str(), L"w");
    if (f) {
      fputs("[Suite]\nGpuDedupWarm=30\nGpuDedupHashCap=16384\nGapAttribCalEvery=40\nGpuSplitFrames=60\n[D3D]\n"
            "PeriphInstCoreMargin=16\nPeriphInstCoreEdge=200\nPeriphInstCoreShape=8\n",
            f);
      fclose(f);
    }
  }
  const bool tuningRead = GetPrivateProfileIntW(L"Suite", L"GpuDedupWarm", 0, ini.c_str()) == 30 &&
                          GetPrivateProfileIntW(L"Suite", L"GpuSplitFrames", 0, ini.c_str()) == 60 &&
                          GetPrivateProfileIntW(L"D3D", L"PeriphInstCoreMargin", 0, ini.c_str()) == 16 &&
                          GetPrivateProfileIntW(L"D3D", L"PeriphInstCoreShape", 0, ini.c_str()) == 8;
  const int tuning = NoteRetiredKeys(ini);
  // R115 I2/I3: every retired key switched on (7, a value not used above): each switch key logged once, the tuning
  // keys never; a second call logs nothing. 23 switch + 13 tuning keys as of R115 (update with kRetiredKeys).
  DeleteFileW(ini.c_str());
  int nSwitch = 0, nTuning = 0, readAll = 0;
  for (const RetiredKey& r : kRetiredKeys) {
    (r.sw ? nSwitch : nTuning) += 1;
    WritePrivateProfileStringW(r.sec, r.key, L"7", ini.c_str());
  }
  for (const RetiredKey& r : kRetiredKeys) readAll += GetPrivateProfileIntW(r.sec, r.key, 0, ini.c_str()) == 7 ? 1 : 0;
  const int allOn = NoteRetiredKeys(ini), allAgain = NoteRetiredKeys(ini);
  DeleteFileW(ini.c_str());
  constexpr int kRetiredCount = static_cast<int>(sizeof(kRetiredKeys) / sizeof(kRetiredKeys[0]));
  printf("     retired keys: %d (%d switch, %d tuning); all on -> %d logged, again %d; dev tuning defaults -> %d\n",
         kRetiredCount, nSwitch, nTuning, allOn, allAgain, tuning);
  Check(none == 0 && written0 > 0 && read0 == written0 && first == 1 && again == 0,
        "R107 retired keys: the template's constant 0s are silent (each section once, every key read back); a "
        "non-zero retired key is logged once per session and ignored");
  Check(tuningRead && tuning == 0 && readAll == kRetiredCount && nSwitch == 23 && nTuning == 13 &&
            allOn == nSwitch && allAgain == 0,
        "R115 retired keys: the dev ini's old tuning defaults (GpuDedupWarm=30 ... PeriphInstCoreShape=8) log "
        "nothing; every retired key switched on logs exactly the switch keys, once");
}

// R115 G4: int ini keys (qvini::ToInt, cgrade::IniI): nan / inf / out of int range -> the default instead of an
// undefined cast (MSVC: INT_MIN, which SanitizeMode then mapped to whatever a negative value maps to); in-range values
// truncate as before.
void IniInts() {
  const double inf = HUGE_VAL, nan = std::nan("");
  const bool conv = qvini::ToInt(nan, 5) == 5 && qvini::ToInt(inf, 5) == 5 && qvini::ToInt(-inf, 5) == 5 &&
                    qvini::ToInt(1e10, 5) == 5 && qvini::ToInt(-1e10, 5) == 5 && qvini::ToInt(2147483648.0, 5) == 5 &&
                    qvini::ToInt(2147483647.5, 5) == 2147483647 &&
                    qvini::ToInt(-2147483648.5, 5) == -2147483647 - 1 && qvini::ToInt(1.9, 5) == 1 &&
                    qvini::ToInt(-0.5, 5) == 0 && qvini::ToInt(2, 5) == 2;
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring ini = std::wstring(tmp) + L"qv_iniints_" + std::to_wstring(GetCurrentProcessId()) + L".ini";
  const wchar_t* p = ini.c_str();
  const cgrade::ModeParams fd = cgrade::ModeDefaults(1), ed = cgrade::ModeDefaults(2);
  int bad = 0;
  for (const wchar_t* v : {L"nan", L"inf", L"-inf", L"1e10", L"-1e10"}) {
    DeleteFileW(p);
    WritePrivateProfileStringW(L"Color", L"PanelGamut", v, p);
    for (const wchar_t* s : {L"ColorFix", L"ColorEnhanced"})
      for (const wchar_t* k : {L"Dither", L"EqualizeEc", L"NightGate", L"SrgbMatch", L"NightVision", L"Glare"})
        WritePrivateProfileStringW(s, k, v, p);
    const cgrade::ModeParams f = cgrade::ReadModeIni(p, 1), e = cgrade::ReadModeIni(p, 2);
    const bool ok = f.panelGamut == 0 && e.panelGamut == 0 && f.dither == fd.dither && e.dither == ed.dither &&
                    e.nightVision == ed.nightVision && e.glare == ed.glare && f.equalizeEc == fd.equalizeEc &&
                    f.nightGate == fd.nightGate && f.srgbMatch == fd.srgbMatch;
    if (!ok) {
      ++bad;
      printf("     int keys = %ls: panelGamut %d/%d, dither %d/%d (defaults %d/%d), nightVision %d, glare %d\n", v,
             f.panelGamut, e.panelGamut, f.dither, e.dither, fd.dither, ed.dither, e.nightVision, e.glare);
    }
  }
  // In range: as before (PanelGamut 1 and 2 read, 1.9 truncates to 1; Dither 0.5 -> 0).
  DeleteFileW(p);
  WritePrivateProfileStringW(L"Color", L"PanelGamut", L"1", p);
  WritePrivateProfileStringW(L"ColorEnhanced", L"Dither", L"0.5", p);
  const bool one = cgrade::ReadModeIni(p, 1).panelGamut == 1 && cgrade::ReadModeIni(p, 2).dither == 0;
  WritePrivateProfileStringW(L"Color", L"PanelGamut", L"2", p);
  const bool two = cgrade::ReadModeIni(p, 2).panelGamut == 2;
  WritePrivateProfileStringW(L"Color", L"PanelGamut", L"1.9", p);
  const bool frac = cgrade::ReadModeIni(p, 1).panelGamut == 1;
  DeleteFileW(p);
  Check(conv && bad == 0 && one && two && frac,
        "R115 ini ints: PanelGamut / Dither / NightVision / Glare / EqualizeEc / NightGate / SrgbMatch = nan, inf, "
        "1e10 or -1e10 read as the mode's default (no undefined cast); 1, 2, 1.9 -> 1 and 0.5 -> 0 as before");
}

// R107 (F12): the shared readers (ini_read.h): one rule for doubles (ReadDouble and IniD agree), masks as before.
void IniHelpers() {
  bool bad = false;
  const bool parse = qvini::ParseDouble(L"1.5", 7, &bad) == 1.5 && !bad && qvini::ParseDouble(L"", 7, &bad) == 7 &&
                     !bad && qvini::ParseDouble(nullptr, 7) == 7 && qvini::ParseDouble(L"abc", 7, &bad) == 7 && bad &&
                     qvini::ParseDouble(L"-2e1x", 7, &bad) == -20 && !bad && qvini::ParseDouble(L"0", 7) == 0;
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring ini = std::wstring(tmp) + L"qv_inihelp_" + std::to_wstring(GetCurrentProcessId()) + L".ini";
  FILE* f = nullptr;
  _wfopen_s(&f, ini.c_str(), L"w");
  if (f) {
    fputs("[A]\nHex=0x30f\nDec=17\nEmpty=\nJunk=zz\nBig=0xffffffff\nNum=2.25\nWord=abc\nBlank=\n", f);
    fclose(f);
  }
  const wchar_t* p = ini.c_str();
  const bool mask = qvini::Mask(L"A", L"Hex", 0x101, 0x3ff, p) == 0x30f && qvini::Mask(L"A", L"Dec", 3, 0xff, p) == 17 &&
                    qvini::Mask(L"A", L"Missing", 0x10055, 0x1ffff, p) == 0x10055 &&
                    qvini::Mask(L"A", L"Empty", 7, 0x7, p) == 0 && qvini::Mask(L"A", L"Junk", 7, 0x7, p) == 0 &&
                    qvini::Mask(L"A", L"Big", 0, 0xff, p) == 0xff;
  // Both double readers: the number, else the default (main.cpp's ReadDouble read "abc" / "" as 0 before R107).
  const bool dbl = ReadDouble(L"A", L"Num", 9, ini) == 2.25 && ReadDouble(L"A", L"Word", 9, ini) == 9 &&
                   ReadDouble(L"A", L"Blank", 9, ini) == 9 && ReadDouble(L"A", L"Missing", -1, ini) == -1 &&
                   cgrade::IniD(p, L"A", L"Num", 9) == 2.25 && cgrade::IniD(p, L"A", L"Word", 9) == 9 &&
                   cgrade::IniD(p, L"A", L"Blank", 9) == 9;
  DeleteFileW(p);
  Check(parse && mask && dbl,
        "R107 ini helpers: masks (decimal / 0x hex, missing -> default, empty / junk -> 0, masked) and one double rule "
        "(missing, empty, unparsable -> the default) for ReadDouble and IniD");
}

void Run() {
  AppKeysRead();
  ColorKeysRead();
  RetiredKeysLogged();
  IniHelpers();
  IniInts();
}

}  // namespace iktest
