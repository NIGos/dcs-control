// R107: the ini keys DCS Control writes (dlss5vrframegen src\DcsVr.Core\ConfigurationWriters.cs, DcsQvCull(),
// ColorModeLine and VisualSections, as of 2026-10-20) are all still read by the payload: either live (the key's
// literal is in the source) or retired (kRetiredKeys: ignored, a non-zero value logged once). An unknown key is
// harmless by construction (GetPrivateProfile* reads only the keys asked for), so a removal can never break the
// app's ini; this keeps a removal from silently dropping a key the app still sets with a meaning. `constant`: the
// template writes a fixed value (a non-constant, profile-driven key must stay live, never retired).
// Since Dev's F07 trim (2026-10-21) the template writes only the constant=false entries (75 keys); the constant
// ones stay listed so an older profile ini (full template) still reads the same values or the default.
#pragma once

namespace iktest {

struct AppKey {
  const wchar_t* sec;
  const wchar_t* key;
  bool constant;
};
const AppKey kAppKeys[] = {
    {L"Color", L"Mode", false}, {L"ColorFix", L"ExposureEV", false}, {L"ColorFix", L"NightEV", false},
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

void AppKeysRead() {
  const std::string src = lptest::ReadAllSources();
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
  Check(missing == 0 && retiredNonConst == 0 && src.size() > 100000,
        "R107 ini keys: every key DCS Control's template writes is read by the payload, live or retired (and a "
        "retired one is only ever written as a constant)");
}

void RetiredKeysLogged() {
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring ini = std::wstring(tmp) + L"qv_retired_" + std::to_wstring(GetCurrentProcessId()) + L".ini";
  // The app's template: every retired key it writes is a constant 0 -> nothing logged.
  {
    FILE* f = nullptr;
    _wfopen_s(&f, ini.c_str(), L"w");
    if (f) {
      for (const AppKey& k : kAppKeys)
        if (Retired(k.sec, k.key)) fprintf(f, "[%ls]\n%ls=0\n", k.sec, k.key);
      fclose(f);
    }
  }
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
  DeleteFileW(ini.c_str());
  Check(none == 0 && first == 1 && again == 0,
        "R107 retired keys: the template's constant 0s are silent; a non-zero retired key is logged once per session "
        "and ignored");
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
  RetiredKeysLogged();
  IniHelpers();
}

}  // namespace iktest
