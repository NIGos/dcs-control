void LoadConfig(bool initial) {
  std::wstring ini = g_dir + L"DcsQvCull.ini";
  const std::wstring deployedIni = ini;  // R97: [Dev] keys may live only in the deployed ini (dev mirrors drop [Dev])
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  // A missing ini is read like an empty one: every key takes its fallback
  // below, and the same Apply* calls run. Before, the payload kept the bare
  // Config defaults (which had peripheral exclusion and the D3D meter on)
  // and applied none of the features. Parsed once until the file appears.
  static bool missing = false;
  if (!GetFileAttributesExW(ini.c_str(), GetFileExInfoStandard, &fad)) {
    if (missing && !initial) return;
    missing = true;
    Log("config: %ls not found, using defaults", ini.c_str());
    fad = WIN32_FILE_ATTRIBUTE_DATA{};
  } else {
    missing = false;
  }
  // R62: read whole (a path of 260+ characters was cut and dev mode stayed off silently); an unusable one is logged.
  const qvdev::DevPath devPath = qvdev::ReadDevPath(L"IniPath", ini);
  static std::wstring s_devIniReported;
  if (qvdev::ReportOnce(devPath, &s_devIniReported))
    Log("config: [Dev] IniPath ignored (%s): %ls; settings from the deployed ini", qvdev::Why(devPath),
        devPath.path.c_str());
  const wchar_t* devIni = devPath.path.c_str();
  WIN32_FILE_ATTRIBUTE_DATA dfad{};
  const bool dev = devPath.state == qvdev::kDevOk && GetFileAttributesExW(devIni, GetFileExInfoStandard, &dfad);
  const FILETIME devTime = dev ? dfad.ftLastWriteTime : FILETIME{};
  if (!initial && CompareFileTime(&fad.ftLastWriteTime, &g_iniTime) == 0 &&
      CompareFileTime(&devTime, &g_devIniTime) == 0)
    return;
  g_iniTime = fad.ftLastWriteTime;
  g_devIniTime = devTime;
  std::wstring devDir;
  if (dev) {
    ini = devIni;
    devDir = ini.substr(0, ini.find_last_of(L"\\/") + 1);
  }
  if (devDir != g_devDir) {
    g_devDir = devDir;
    if (dev) Log("config: dev mode, settings from %ls", ini.c_str());
    else if (!initial) Log("config: dev mode off, settings from the deployed ini");
  }

  NoteRetiredKeys(ini);  // R107: keys of removed research features: ignored, a non-zero one logged once
  Config c;
  c.enabled = GetPrivateProfileIntW(L"Cull", L"Enabled", 0, ini.c_str()) != 0;
  c.debugHole = GetPrivateProfileIntW(L"Cull", L"DebugHole", 0, ini.c_str()) != 0;
  c.keepFraction = ReadDouble(L"Cull", L"FocusKeepFraction", c.keepFraction, ini);
  c.safetyNdc = ReadDouble(L"Cull", L"SafetyNdc", c.safetyNdc, ini);
  c.nativeKeepFraction = ReadDouble(L"Cull", L"NativeKeepFraction", c.nativeKeepFraction, ini);
  c.saccadeDeg = ReadDouble(L"Cull", L"SaccadeDeg", c.saccadeDeg, ini);
  c.saccadeHoldFrames = GetPrivateProfileIntW(L"Cull", L"SaccadeHoldFrames", 6, ini.c_str());
  c.focusRatio = ReadDouble(L"Cull", L"FocusRatio", c.focusRatio, ini);
  c.apexTolerance = ReadDouble(L"Cull", L"ApexTolerance", c.apexTolerance, ini);
  c.collectThreadsMax = GetPrivateProfileIntW(L"Scene", L"CollectThreadsMax", 0, ini.c_str());
  c.statsIntervalSec = GetPrivateProfileIntW(L"Log", L"StatsIntervalSec", 2, ini.c_str());
  c.dumpViews = GetPrivateProfileIntW(L"Log", L"DumpViews", 3, ini.c_str());
  c.benchBlocks = GetPrivateProfileIntW(L"Bench", L"Blocks", c.benchBlocks, ini.c_str());
  c.benchBlockSec = GetPrivateProfileIntW(L"Bench", L"BlockSec", c.benchBlockSec, ini.c_str());
  c.benchSettleSec = ReadDouble(L"Bench", L"SettleSec", c.benchSettleSec, ini);
  c.benchAutoStartSec = GetPrivateProfileIntW(L"Bench", L"AutoStartSec", 0, ini.c_str());
  c.benchThreadsB = GetPrivateProfileIntW(L"Bench", L"ThreadsB", c.benchThreadsB, ini.c_str());
  c.suiteProfile = GetPrivateProfileIntW(L"Suite", L"Profile", 0, ini.c_str()) != 0;
  c.suiteSelfTest = GetPrivateProfileIntW(L"Suite", L"SelfTest", 0, ini.c_str()) != 0;
  c.suiteBenchPartition = GetPrivateProfileIntW(L"Suite", L"BenchPartition", 0, ini.c_str()) != 0;
  c.suiteBenchIsolation = GetPrivateProfileIntW(L"Suite", L"BenchIsolation", 0, ini.c_str()) != 0;
  c.diagnostics = GetPrivateProfileIntW(L"General", L"Diagnostics", 0, ini.c_str()) != 0;
  g_beeps = GetPrivateProfileIntW(L"General", L"Beeps", 1, ini.c_str()) != 0;
  // Re-locate moved DCS code by signature (reloc.h); 0 = recorded addresses only.
  const bool sigScan = GetPrivateProfileIntW(L"General", L"SigScan", 1, ini.c_str()) != 0;
  if (reloc::g_enabled.exchange(sigScan) != sigScan || (initial && !sigScan))
    Log("config: signature scan %s", sigScan ? "on" : "off (recorded DCS addresses only)");
  {
    wchar_t hk[64];
    GetPrivateProfileStringW(L"Hotkeys", L"Toggle", L"122:6", hk, 64, ini.c_str());
    if (!ParseHotkey(hk, c.toggleVk, c.toggleMods)) Log("config: [Hotkeys] Toggle=%ls is invalid; key disabled", hk);
    c.developerKeys = GetPrivateProfileIntW(L"Hotkeys", L"DeveloperKeys", 0, ini.c_str()) != 0;
  }
  c.shadowTight = GetPrivateProfileIntW(L"Shadow", L"TightCasters", 0, ini.c_str()) != 0;
  c.shadowDebugInvert = GetPrivateProfileIntW(L"Shadow", L"DebugInvert", 0, ini.c_str()) != 0;
  c.shadowMarginFrac = ReadDouble(L"Shadow", L"MarginFrac", 0.03, ini);
  c.shadowMarginMin = ReadDouble(L"Shadow", L"MarginMin", 2.0, ini);
  c.shadowFloorY = ReadDouble(L"Shadow", L"FloorY", -1000.0, ini);
  c.suiteBenchShadow = GetPrivateProfileIntW(L"Suite", L"BenchShadow", 0, ini.c_str()) != 0;
  c.allocSlabs = GetPrivateProfileIntW(L"Model", L"AllocSlabs", 0, ini.c_str()) != 0;
  c.slabBytes = GetPrivateProfileIntW(L"Model", L"SlabBytes", 4096, ini.c_str());
  if (c.slabBytes < 256) c.slabBytes = 256;
  if (c.slabBytes > 65536) c.slabBytes = 65536;
  c.suiteBenchAllocSlabs = GetPrivateProfileIntW(L"Suite", L"BenchAllocSlabs", 0, ini.c_str()) != 0;
  c.texDedupe = GetPrivateProfileIntW(L"Texture", L"StreamDedupe", 0, ini.c_str()) != 0;
  c.suiteBenchTexDedupe = GetPrivateProfileIntW(L"Suite", L"BenchTexDedupe", 0, ini.c_str()) != 0;
  c.suiteBenchEngine = GetPrivateProfileIntW(L"Suite", L"BenchEngine", 1, ini.c_str()) != 0;
  // R90: at most 10 min (a motion_*.csv grows ~3 KB/s; the key was unclamped above).
  c.motionSeconds = std::min(600, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"MotionSeconds", 60, ini.c_str())));
  c.suiteQuick = GetPrivateProfileIntW(L"Suite", L"Quick", 1, ini.c_str()) != 0;
  c.triPlain = GetPrivateProfileIntW(L"Model", L"PlainTriangleCounter", 0, ini.c_str()) != 0;
  c.suiteBenchTriPlain = GetPrivateProfileIntW(L"Suite", L"BenchTriPlain", 0, ini.c_str()) != 0;
  c.cbSkip = GetPrivateProfileIntW(L"Effects", L"SkipSameConstantBuffer", 0, ini.c_str()) != 0;
  c.suiteBenchCbSkip = GetPrivateProfileIntW(L"Suite", L"BenchCbSkip", 0, ini.c_str()) != 0;
  c.suiteBenchMicro = GetPrivateProfileIntW(L"Suite", L"BenchMicro", 0, ini.c_str()) != 0;
  c.costWeights = GetPrivateProfileIntW(L"Scene", L"CostWeights", 0, ini.c_str()) != 0;
  g_costWeightsSanity = GetPrivateProfileIntW(L"Scene", L"CostWeightsSanity", 0, ini.c_str()) != 0;
  c.suiteBenchCostWeights = GetPrivateProfileIntW(L"Suite", L"BenchCostWeights", 0, ini.c_str()) != 0;
  c.suiteTerrain = GetPrivateProfileIntW(L"Suite", L"Terrain", 0, ini.c_str()) != 0;
  c.cbUploadSkip = GetPrivateProfileIntW(L"Effects", L"SkipSameConstantUpload", 0, ini.c_str()) != 0;
  c.suiteBenchCbUpload = GetPrivateProfileIntW(L"Suite", L"BenchCbUpload", 0, ini.c_str()) != 0;
  c.pacerLowPower = GetPrivateProfileIntW(L"General", L"LowPowerPacer", 0, ini.c_str()) != 0;
  c.pacerTimeoutUs = GetPrivateProfileIntW(L"General", L"LowPowerPacerTimeoutUs", 10, ini.c_str());
  c.suiteBenchPacer = GetPrivateProfileIntW(L"Suite", L"BenchPacer", 0, ini.c_str()) != 0;
  c.motionSweep = GetPrivateProfileIntW(L"Suite", L"MotionSweep", 0, ini.c_str()) != 0;
  c.motionProfile = GetPrivateProfileIntW(L"Suite", L"MotionProfile", 0, ini.c_str()) != 0;
  c.motionTaxi = GetPrivateProfileIntW(L"Suite", L"MotionTaxi", 0, ini.c_str()) != 0;
  c.taskClock = GetPrivateProfileIntW(L"Timing", L"TaskQueueClock", 0, ini.c_str()) != 0;
  c.frameHeapSlabs = GetPrivateProfileIntW(L"Model", L"FrameHeapSlabs", 0, ini.c_str()) != 0;
  c.suiteBenchFrameHeap = GetPrivateProfileIntW(L"Suite", L"BenchFrameHeap", 0, ini.c_str()) != 0;
  c.shadowTexSkip = GetPrivateProfileIntW(L"Model", L"ShadowTextureSkip", 1, ini.c_str()) != 0;
  c.suiteBenchShadowTex = GetPrivateProfileIntW(L"Suite", L"BenchShadowTex", 0, ini.c_str()) != 0;
  c.suiteBenchTexTable = GetPrivateProfileIntW(L"Suite", L"BenchTexTable", 0, ini.c_str()) != 0;
  c.suiteBenchBigPages = GetPrivateProfileIntW(L"Suite", L"BenchBigPages", 0, ini.c_str()) != 0;
  c.bigPages = GetPrivateProfileIntW(L"Model", L"BigModelPages", 1, ini.c_str()) != 0;
  c.parUpload = GetPrivateProfileIntW(L"Model", L"ParallelUpload", 0, ini.c_str()) != 0;
  c.suiteBenchParUpload = GetPrivateProfileIntW(L"Suite", L"BenchParallelUpload", 0, ini.c_str()) != 0;
  c.directUpload = GetPrivateProfileIntW(L"Model", L"DirectUpload", 0, ini.c_str()) != 0;
  c.suiteDirectUploadVerify = GetPrivateProfileIntW(L"Suite", L"DirectUploadVerify", 0, ini.c_str()) != 0;
  c.suiteBenchDirectUpload = GetPrivateProfileIntW(L"Suite", L"BenchDirectUpload", 0, ini.c_str()) != 0;
  c.motionCounters = GetPrivateProfileIntW(L"Suite", L"MotionCounters", 1, ini.c_str()) != 0;
  c.shadowInst = GetPrivateProfileIntW(L"Model", L"ShadowInstancing", 1, ini.c_str()) != 0;
  c.suiteShadowInstCompile = GetPrivateProfileIntW(L"Suite", L"ShadowInstCompile", 0, ini.c_str()) != 0;
  c.suiteGBufferInstCompile = GetPrivateProfileIntW(L"Suite", L"GBufferInstCompile", 0, ini.c_str()) != 0;
  c.suiteGBufferTexCount = GetPrivateProfileIntW(L"Suite", L"GBufferTexCount", 0, ini.c_str()) != 0;
  c.suiteFxApplyCount = GetPrivateProfileIntW(L"Suite", L"FxApplyCount", 0, ini.c_str()) != 0;
  c.suiteDirectUploadCount = GetPrivateProfileIntW(L"Suite", L"DirectUploadCount", 0, ini.c_str()) != 0;
  c.suiteSrvSpanCount = GetPrivateProfileIntW(L"Suite", L"SrvSpanCount", 0, ini.c_str()) != 0;
  c.suiteJoinTailCount = GetPrivateProfileIntW(L"Suite", L"JoinTailCount", 0, ini.c_str()) != 0;
  c.suiteShadowRecCount = GetPrivateProfileIntW(L"Suite", L"ShadowRecCount", 0, ini.c_str()) != 0;
  c.suiteGBufferRecCount = GetPrivateProfileIntW(L"Suite", L"GBufferRecCount", 0, ini.c_str()) != 0;
  c.suiteForwardRecCount = GetPrivateProfileIntW(L"Suite", L"ForwardRecCount", 0, ini.c_str()) != 0;
  c.suiteGpuPassTiming = GetPrivateProfileIntW(L"Suite", L"GpuPassTiming", 0, ini.c_str()) != 0;
  c.suiteGpuPassStats = GetPrivateProfileIntW(L"Suite", L"GpuPassStats", 0, ini.c_str()) != 0;
  c.peripheralLodScale = (std::max)(0, (std::min)(99, static_cast<int>(GetPrivateProfileIntW(L"Model", L"PeripheralLodScale", 0, ini.c_str()))));
  c.suitePeriphLodTiming = (std::max)(0, (std::min)(99, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"PeriphLodTiming", 0, ini.c_str()))));
  c.suitePeriphLodImage = (std::max)(0, (std::min)(120, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"PeriphLodImage", 0, ini.c_str()))));
  c.suiteBenchPeripheralLod = GetPrivateProfileIntW(L"Suite", L"BenchPeripheralLod", 0, ini.c_str()) != 0;
  c.cascadeCache = GetPrivateProfileIntW(L"Shadows", L"CascadeCache", 0, ini.c_str()) != 0;
  c.cascadeCacheMask = qvini::Mask(L"Shadows", L"CascadeCacheMask", 0xc, 0xf, ini.c_str());  // decimal or 0x hex
  c.cascadeCacheMaxAge = (std::max)(1, (std::min)(64, static_cast<int>(GetPrivateProfileIntW(L"Shadows", L"CascadeCacheMaxAge", 4, ini.c_str()))));
  c.cascadeCacheMarginPct = (std::max)(0, (std::min)(25, static_cast<int>(GetPrivateProfileIntW(L"Shadows", L"CascadeCacheMarginPct", 3, ini.c_str()))));
  c.cascadeCacheVerify = (std::max)(0, (std::min)(10000, static_cast<int>(GetPrivateProfileIntW(L"Shadows", L"CascadeCacheVerify", 0, ini.c_str()))));
  c.suiteCascadeCacheTiming = GetPrivateProfileIntW(L"Suite", L"CascadeCacheTiming", 0, ini.c_str()) != 0;
  c.chainFast = qvini::Mask(L"D3D", L"ChainFast", 7, 0x7, ini.c_str());  // decimal or 0x hex
  c.suiteChainFastTiming = GetPrivateProfileIntW(L"Suite", L"ChainFastTiming", 0, ini.c_str()) != 0;
  c.suiteChainFastVerify = (std::max)(0, (std::min)(600, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"ChainFastVerify", 0, ini.c_str()))));
  c.shaderFast = qvini::Mask(L"D3D", L"ShaderFast", 3, 0x3, ini.c_str());  // R36, decimal or 0x hex
  c.suiteShaderFastTiming = GetPrivateProfileIntW(L"Suite", L"ShaderFastTiming", 0, ini.c_str()) != 0;
  c.suiteShaderFastVerify = (std::max)(0, (std::min)(600, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"ShaderFastVerify", 0, ini.c_str()))));
  c.nightTone = static_cast<int>(GetPrivateProfileIntW(L"Night", L"NightTone", 0, ini.c_str()));  // R37
  c.nightFloor = ReadDouble(L"Night", L"NightExposureFloor", 0.0, ini);
  c.nightToe = ReadDouble(L"Night", L"NightToe", 0.0, ini);
  c.nightEv = ReadDouble(L"Night", L"NightExposureEV", 0.0, ini);
  c.nightGate = GetPrivateProfileIntW(L"Night", L"NightGate", 1, ini.c_str()) != 0;
  c.suiteVramCount = GetPrivateProfileIntW(L"Suite", L"VramCount", 0, ini.c_str()) != 0;
  c.suiteVramCountSec = static_cast<int>(GetPrivateProfileIntW(L"Suite", L"VramCountSec", 20, ini.c_str()));
  c.suiteVramCountCreates = GetPrivateProfileIntW(L"Suite", L"VramCountCreates", 1, ini.c_str()) != 0;
  c.splitFilter = GetPrivateProfileIntW(L"D3D", L"SplitFilter", 1, ini.c_str()) != 0;  // R49: default 1 (shipped)
  c.splitFilterOps = qvini::Mask(L"D3D", L"SplitFilterOps", 0x4ff, 0x7ff, ini.c_str());  // decimal or 0x hex
  c.suiteSplitFilterVerify = GetPrivateProfileIntW(L"Suite", L"SplitFilterVerify", 0, ini.c_str()) != 0;
  c.suiteBenchSplitFilter = GetPrivateProfileIntW(L"Suite", L"BenchSplitFilter", 0, ini.c_str()) != 0;
  c.shadowBatch = GetPrivateProfileIntW(L"Model", L"ShadowBatching", 1, ini.c_str()) != 0;
  c.suiteShadowInstVerify = GetPrivateProfileIntW(L"Suite", L"ShadowInstVerify", 0, ini.c_str()) != 0;
  c.suiteBenchShadowInst = GetPrivateProfileIntW(L"Suite", L"BenchShadowInst", 0, ini.c_str()) != 0;
  c.shadowPlanAsync = GetPrivateProfileIntW(L"Model", L"ShadowPlanAsync", 1, ini.c_str()) != 0;
  c.suiteBenchShadowPlanAsync = GetPrivateProfileIntW(L"Suite", L"BenchShadowPlanAsync", 0, ini.c_str()) != 0;
  c.shadowRecorder = GetPrivateProfileIntW(L"Model", L"ShadowRecorder", 0, ini.c_str()) != 0;
  c.shadowRecorderScope = qvini::Mask(L"Model", L"ShadowRecorderScope", 0x30f, 0x3ff, ini.c_str());  // decimal or 0x hex
  c.shadowRecorderWaitUs = GetPrivateProfileIntW(L"Model", L"ShadowRecorderWaitUs", 200, ini.c_str());
  if (c.shadowRecorderWaitUs < 0) c.shadowRecorderWaitUs = 0;
  if (c.shadowRecorderWaitUs > 20000) c.shadowRecorderWaitUs = 20000;
  c.shadowRecorderPriority = static_cast<int>(GetPrivateProfileIntW(L"Model", L"ShadowRecorderPriority", 0, ini.c_str()));
  if (c.shadowRecorderPriority < -2) c.shadowRecorderPriority = -2;
  if (c.shadowRecorderPriority > 2) c.shadowRecorderPriority = 2;
  c.shadowRecorderSplit = qvini::Mask(L"Model", L"ShadowRecorderSplit", 0xf, 0xf, ini.c_str());
  c.shadowRecorderHelpers = static_cast<int>(GetPrivateProfileIntW(L"Model", L"ShadowRecorderHelpers", 3, ini.c_str()));
  if (c.shadowRecorderHelpers < 0) c.shadowRecorderHelpers = 0;
  if (c.shadowRecorderHelpers > 3) c.shadowRecorderHelpers = 3;
  c.shadowRecorderInstancing = GetPrivateProfileIntW(L"Model", L"ShadowRecorderInstancing", 1, ini.c_str()) != 0;
  c.suiteShadowRecVerify = GetPrivateProfileIntW(L"Suite", L"ShadowRecVerify", 0, ini.c_str()) != 0;
  c.suiteShadowRecVerifySec = GetPrivateProfileIntW(L"Suite", L"ShadowRecVerifySec", 5, ini.c_str());
  if (c.suiteShadowRecVerifySec < 1) c.suiteShadowRecVerifySec = 1;
  if (c.suiteShadowRecVerifySec > 120) c.suiteShadowRecVerifySec = 120;
  c.suiteBenchShadowRecorder = GetPrivateProfileIntW(L"Suite", L"BenchShadowRecorder", 0, ini.c_str()) != 0;
  c.gbufferBatch = GetPrivateProfileIntW(L"Model", L"GBufferBatching", 0, ini.c_str()) != 0;
  c.suiteGBufferInstVerify = GetPrivateProfileIntW(L"Suite", L"GBufferInstVerify", 0, ini.c_str()) != 0;
  c.suiteBenchGBufferInst = GetPrivateProfileIntW(L"Suite", L"BenchGBufferInst", 0, ini.c_str()) != 0;
  c.gbufferRecorder = GetPrivateProfileIntW(L"Model", L"GBufferRecorder", 0, ini.c_str()) != 0;
  c.gbufferRecorderScope = qvini::Mask(L"Model", L"GBufferRecorderScope", 0x10055, 0x1ffff, ini.c_str());  // dec or 0x hex
  c.gbufferRecorderWaitUs = GetPrivateProfileIntW(L"Model", L"GBufferRecorderWaitUs", 300, ini.c_str());
  if (c.gbufferRecorderWaitUs < 0) c.gbufferRecorderWaitUs = 0;
  if (c.gbufferRecorderWaitUs > 20000) c.gbufferRecorderWaitUs = 20000;
  c.gbufferRecorderIsland = GetPrivateProfileIntW(L"Model", L"GBufferRecorderIsland", 30, ini.c_str());
  if (c.gbufferRecorderIsland < 1) c.gbufferRecorderIsland = 1;
  if (c.gbufferRecorderIsland > 100000) c.gbufferRecorderIsland = 100000;
  c.gbufferRecorderMaxSegments = GetPrivateProfileIntW(L"Model", L"GBufferRecorderMaxSegments", 12, ini.c_str());
  if (c.gbufferRecorderMaxSegments < 1) c.gbufferRecorderMaxSegments = 1;
  if (c.gbufferRecorderMaxSegments > 16) c.gbufferRecorderMaxSegments = 16;
  c.gbufferRecorderHelpers = GetPrivateProfileIntW(L"Model", L"GBufferRecorderHelpers", 2, ini.c_str());
  if (c.gbufferRecorderHelpers < 0) c.gbufferRecorderHelpers = 0;
  if (c.gbufferRecorderHelpers > 2) c.gbufferRecorderHelpers = 2;
  c.gbufferRecorderCockpit = GetPrivateProfileIntW(L"Model", L"GBufferRecorderCockpit", 0, ini.c_str()) != 0;
  c.gbufferRecorderByOrdinal = GetPrivateProfileIntW(L"Model", L"GBufferRecorderByOrdinal", 1, ini.c_str()) != 0;
  c.suiteGBufferRecVerify = GetPrivateProfileIntW(L"Suite", L"GBufferRecVerify", 0, ini.c_str()) != 0;
  c.suiteGBufferRecVerifySec = GetPrivateProfileIntW(L"Suite", L"GBufferRecVerifySec", 6, ini.c_str());
  c.suiteGBufferRecVerifyLive = GetPrivateProfileIntW(L"Suite", L"GBufferRecVerifyLive", 0, ini.c_str()) != 0;
  c.suiteGBufferRecVerifyNoLatch = GetPrivateProfileIntW(L"Suite", L"GBufferRecVerifyNoLatch", 0, ini.c_str()) != 0;
  if (c.suiteGBufferRecVerifySec < 1) c.suiteGBufferRecVerifySec = 1;
  if (c.suiteGBufferRecVerifySec > 120) c.suiteGBufferRecVerifySec = 120;
  c.suiteGBufferRecVerifyStride = GetPrivateProfileIntW(L"Suite", L"GBufferRecVerifyStride", 0, ini.c_str());
  if (c.suiteGBufferRecVerifyStride < 0 || c.suiteGBufferRecVerifyStride == 1) c.suiteGBufferRecVerifyStride = 0;
  c.suiteBenchGBufferRecorder = GetPrivateProfileIntW(L"Suite", L"BenchGBufferRecorder", 0, ini.c_str()) != 0;
  c.passFlush = qvini::Mask(L"Model", L"PassFlush", 0, 0xff, ini.c_str());  // decimal or 0x hex
  c.suiteBenchPassFlushMask = qvini::Mask(L"Suite", L"BenchPassFlushMask", 0, 0xff, ini.c_str());
  c.suiteBenchPassFlush = GetPrivateProfileIntW(L"Suite", L"BenchPassFlush", 0, ini.c_str()) != 0;
  c.srvTailTrim = GetPrivateProfileIntW(L"Model", L"SrvTailTrim", 0, ini.c_str()) != 0;
  c.suiteSrvTailTrimVerify = GetPrivateProfileIntW(L"Suite", L"SrvTailTrimVerify", 0, ini.c_str()) != 0;
  c.suiteSrvTailTrimVerifySec = GetPrivateProfileIntW(L"Suite", L"SrvTailTrimVerifySec", 10, ini.c_str());
  if (c.suiteSrvTailTrimVerifySec < 1) c.suiteSrvTailTrimVerifySec = 1;
  if (c.suiteSrvTailTrimVerifySec > 120) c.suiteSrvTailTrimVerifySec = 120;
  c.suiteBenchSrvTailTrim = GetPrivateProfileIntW(L"Suite", L"BenchSrvTailTrim", 0, ini.c_str()) != 0;
  {
    wchar_t mode[32] = {};
    GetPrivateProfileStringW(L"Prefetch", L"Mode", L"off", mode, 32, ini.c_str());
    c.prefetchMode = _wcsicmp(mode, L"skip") == 0 ? 2 : _wcsicmp(mode, L"observe") == 0 ? 1 : 0;
    if (c.prefetchMode == 0 && _wcsicmp(mode, L"off") != 0 && mode[0])
      Log("config: [Prefetch] Mode=%ls not understood (off, observe or skip); off", mode);
    const int w = static_cast<int>(GetPrivateProfileIntW(L"Prefetch", L"WindowMs", 5000, ini.c_str()));
    c.prefetchWindowMs = static_cast<uint32_t>(std::max(1, std::min(60000, w)));
    const int tb = static_cast<int>(GetPrivateProfileIntW(L"Prefetch", L"TableBits", 20, ini.c_str()));
    c.prefetchTableBits = std::max(16, std::min(22, tb));
  }
  c.suiteBenchPrefetch = GetPrivateProfileIntW(L"Suite", L"BenchPrefetch", 0, ini.c_str()) != 0;
  c.threadCensus = GetPrivateProfileIntW(L"Threads", L"Census", 0, ini.c_str()) != 0;
  c.threadCensusSec = std::max(10, std::min(3600, static_cast<int>(GetPrivateProfileIntW(L"Threads", L"CensusSec", 60, ini.c_str()))));
  {
    // R44 (docs\research\R44_autonomous_session.md section 1): default 2. With the placement pass made cheap
    // (thread_place.h), CPU-bound airfield: 2 vs 0 fps +1.26 +- 0.39, p95 -0.57 +- 0.12 ms, render -0.29 ms
    // (4/4 triplets; that start had DCS's affinity pass keep our recorder workers off the render core);
    // GPU-bound Germany: no change (fps +0.15 +- 0.15, p95 -0.06 +- 0.20). 0 restores DCS's masks.
    const int wp = static_cast<int>(GetPrivateProfileIntW(L"Threads", L"WorkerPlacement", 2, ini.c_str()));
    c.workerPlacement = wp >= 0 && wp <= 2 ? wp : 2;
  }
  // R52 (docs/research/R52_pacing_latency.md): the placement pass's steady-state re-check (it takes a
  // system-wide thread snapshot); new threads of ours still trigger a pass within 1 s.
  c.placeRecheckSec = std::max(10, std::min(3600, static_cast<int>(GetPrivateProfileIntW(L"Threads", L"PlaceRecheckSec", 60, ini.c_str()))));
  // R90 (docs/research/R90_hitch_hunting.md section 6): the lossless hitch fixes, each with its old path behind a key
  // for the A/B. PlaceRecheckMode: the placement re-check over tracked handles (no system-wide snapshot); PatchMode:
  // code-site writes without the system-wide snapshot, all suspensions batched; Async: DCS threads' log lines queued.
  c.placeRecheckMode = GetPrivateProfileIntW(L"Threads", L"PlaceRecheckMode", 1, ini.c_str()) != 0 ? 1 : 0;
  c.patchMode = GetPrivateProfileIntW(L"General", L"PatchMode", 1, ini.c_str()) != 0 ? 1 : 0;
  c.asyncLog = GetPrivateProfileIntW(L"Log", L"Async", 1, ini.c_str()) != 0;
  c.shadowPlanWaitUs = std::max(0, std::min(20000, static_cast<int>(GetPrivateProfileIntW(L"Model", L"ShadowPlanWaitUs", 300, ini.c_str()))));
  // R52 measurement keys (measure only, all default 0).
  c.frameTimeline = std::max(0, std::min(2, static_cast<int>(GetPrivateProfileIntW(L"Perf", L"FrameTimeline", 0, ini.c_str()))));
  c.timelineSec = std::max(2, std::min(600, static_cast<int>(GetPrivateProfileIntW(L"Perf", L"TimelineSec", 10, ini.c_str()))));
  c.poseProbe = GetPrivateProfileIntW(L"Perf", L"PoseProbe", 0, ini.c_str()) != 0;
  c.eventLog = GetPrivateProfileIntW(L"Perf", L"EventLog", 0, ini.c_str()) != 0;
  c.holdYawDeg = static_cast<int>(GetPrivateProfileIntW(L"Suite", L"HoldYawDeg", -1, ini.c_str()));
  c.holdPitchDeg = ReadDouble(L"Suite", L"HoldPitchDeg", 0.0, ini);
  c.poseRotateDps = ReadDouble(L"Suite", L"PoseRotateDps", 0.0, ini);
  c.holdPose = GetPrivateProfileIntW(L"Suite", L"HoldPose", 0, ini.c_str()) != 0;
  c.poseWallClock = GetPrivateProfileIntW(L"Suite", L"PoseWallClock", 0, ini.c_str()) != 0;  // R99
  c.suiteYawScan = GetPrivateProfileIntW(L"Suite", L"YawScan", 0, ini.c_str()) != 0;
  c.suiteYawProfile = GetPrivateProfileIntW(L"Suite", L"YawProfile", 0, ini.c_str()) != 0;
  c.suiteYawProfileStep = static_cast<int>(GetPrivateProfileIntW(L"Suite", L"YawProfileStep", 30, ini.c_str()));
  c.suiteRotationProfile = GetPrivateProfileIntW(L"Suite", L"RotationProfile", 0, ini.c_str()) != 0;
  c.suiteRotationDegPerSec = static_cast<int>(GetPrivateProfileIntW(L"Suite", L"RotationDegPerSec", 60, ini.c_str()));
  c.suiteRotationSeconds = static_cast<int>(GetPrivateProfileIntW(L"Suite", L"RotationSeconds", 20, ini.c_str()));
  c.suiteFrameStartGap = GetPrivateProfileIntW(L"Suite", L"FrameStartGap", 0, ini.c_str()) != 0;
  c.suiteFrameStartGapSec =
      std::max(0, std::min(120, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"FrameStartGapSec", 10, ini.c_str()))));
  c.suiteRunnableThreads = GetPrivateProfileIntW(L"Suite", L"RunnableThreads", 0, ini.c_str()) != 0;
  c.suiteRunnableThreadsSec =
      std::max(0, std::min(120, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"RunnableThreadsSec", 10, ini.c_str()))));
  c.suiteRunnableThreadsEvery = std::max(
      2, std::min(1024, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"RunnableThreadsEvery", 16, ini.c_str()))));
  {
    const uint32_t v = GetPrivateProfileIntW(L"Model", L"BigPageBytes", 4 << 20, ini.c_str());
    c.bigPageBytes = v < (1u << 20) ? (1u << 20) : v > (64u << 20) ? (64u << 20) : v;
  }
  c.suiteBenchTimerRes = GetPrivateProfileIntW(L"Suite", L"BenchTimerRes", 0, ini.c_str()) != 0;
  c.suiteBenchFilter = GetPrivateProfileIntW(L"Suite", L"BenchFilter", 0, ini.c_str());
  c.d3dFilter = GetPrivateProfileIntW(L"D3D", L"Filter", 0, ini.c_str()) != 0;
  c.fineTimer = GetPrivateProfileIntW(L"Scene", L"FineTimerResolution", 0, ini.c_str()) != 0;
  c.partitionBoost = GetPrivateProfileIntW(L"Scene", L"PartitionBoost", 0, ini.c_str()) != 0;
  c.suiteBenchCull = GetPrivateProfileIntW(L"Suite", L"BenchCull", 0, ini.c_str()) != 0;
  c.suiteBenchThreads = GetPrivateProfileIntW(L"Suite", L"BenchThreads", 0, ini.c_str()) != 0;
  c.suiteBenchTimer = GetPrivateProfileIntW(L"Suite", L"BenchTimer", 0, ini.c_str()) != 0;
  c.d3dMeter = GetPrivateProfileIntW(L"D3D", L"Meter", 0, ini.c_str()) != 0;
  c.shaderTimeCache = GetPrivateProfileIntW(L"Timing", L"ShaderTimeCache", 1, ini.c_str()) != 0;
  c.shaderTimeCacheUs = GetPrivateProfileIntW(L"Timing", L"CacheUs", c.shaderTimeCacheUs, ini.c_str());
  c.profileSeconds = GetPrivateProfileIntW(L"Profile", L"Seconds", c.profileSeconds, ini.c_str());
  c.profileThreads = GetPrivateProfileIntW(L"Profile", L"Threads", c.profileThreads, ini.c_str());
  c.profilePeriodMs = GetPrivateProfileIntW(L"Profile", L"PeriodMs", c.profilePeriodMs, ini.c_str());
  {
    auto clampInt = [&](const wchar_t* sec, const wchar_t* key, int def, int lo, int hi) {
      const int v = static_cast<int>(GetPrivateProfileIntW(sec, key, def, ini.c_str()));
      return v < lo ? lo : v > hi ? hi : v;
    };
    c.recorderDetail = GetPrivateProfileIntW(L"Log", L"RecorderDetail", 0, ini.c_str()) != 0;
    c.perfIntervalSec = clampInt(L"Log", L"PerfIntervalSec", 10, 0, 3600);
    c.vramIntervalSec = clampInt(L"Log", L"VramIntervalSec", 60, 0, 3600);
    c.govEnabled = GetPrivateProfileIntW(L"Governor", L"Enabled", 1, ini.c_str()) != 0;
    c.govGpuBusyPct = clampInt(L"Governor", L"GpuBusyPct", 92, 1, 100);
    c.govRenderIdlePct = clampInt(L"Governor", L"RenderIdlePct", 85, 1, 100);
    c.govCpuBusyPct = clampInt(L"Governor", L"CpuBusyPct", 92, 1, 100);
    c.govGpuFreePct = clampInt(L"Governor", L"GpuFreePct", 80, 1, 100);
    c.govGpuFullPct = clampInt(L"Governor", L"GpuFullPct", 98, 1, 101);
    c.govWindowSec = clampInt(L"Governor", L"WindowSec", 8, 1, 60);
    c.govNeedSec = clampInt(L"Governor", L"NeedSec", 6, 1, 60);
    c.govDwellSec = clampInt(L"Governor", L"MinDwellSec", 10, 1, 3600);
    c.govMinGainPct = (std::max)(0.0, (std::min)(50.0, ReadDouble(L"Governor", L"MinGainPct", 2.0, ini)));
    c.govTrialSec = clampInt(L"Governor", L"TrialSec", 20, 5, 60);
    c.govRecheckSec = clampInt(L"Governor", L"RecheckSec", 10, 0, 60);
    c.govRevertBackoffSec = clampInt(L"Governor", L"RevertBackoffSec", 300, 10, 3600);
  }
  c.fixationDepth = GetPrivateProfileIntW(L"Perception", L"FixationDepth", 0, ini.c_str()) != 0;
  c.depthGrid = GetPrivateProfileIntW(L"Perception", L"DepthGrid", 0, ini.c_str()) != 0;
  c.sceneCutReset = GetPrivateProfileIntW(L"Perception", L"SceneCutReset", 1, ini.c_str()) != 0;
  c.fixationDepthVerify = GetPrivateProfileIntW(L"Perception", L"FixationDepthVerify", 0, ini.c_str());
  c.fixationDepthDump = GetPrivateProfileIntW(L"Perception", L"FixationDepthDump", 0, ini.c_str()) != 0;
  c.farDepthExport = GetPrivateProfileIntW(L"Perception", L"FarDepthExport", 0, ini.c_str()) != 0;
  c.farDepthSlots = GetPrivateProfileIntW(L"Perception", L"FarDepthExportSlots", 2, ini.c_str()) == 1 ? 1 : 2;
  c.farDepthLogProj = static_cast<int>(GetPrivateProfileIntW(L"Perception", L"FarDepthLogProj", 0, ini.c_str()));
  c.farDepthLogProj = (std::max)(0, (std::min)(1000, c.farDepthLogProj));
  c.farDepthPreOverlay = GetPrivateProfileIntW(L"Perception", L"FarDepthPreOverlay", 1, ini.c_str()) != 0;
  {
    wchar_t m[32] = {};
    GetPrivateProfileStringW(L"Perception", L"FarDepthCoverageMode", L"hdr", m, 32, ini.c_str());
    bool invalid = false;
    c.farDepthCoverageMode = ParseFarCoverageMode(m, &invalid);
    if (invalid && !g_farCovInvalidLogged) {
      g_farCovInvalidLogged = true;
      g_farCovInvalidLogs++;
      Log("config: [Perception] FarDepthCoverageMode=%ls is invalid (hdr or overlay); hdr used", m);
    }
  }
  c.farDepthPostOverlay = GetPrivateProfileIntW(L"Perception", L"FarDepthPostOverlay", 1, ini.c_str()) != 0;
  c.farDepthOverlayTrace =
      (std::max)(0, (std::min)(600, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"FarDepthOverlayTrace", 0,
                                                                           ini.c_str()))));
  c.cockpitMaskExport = GetPrivateProfileIntW(L"Perception", L"CockpitMaskExport", 0, ini.c_str()) != 0;  // R65
  c.cockpitMaskDivisor =
      (std::max)(0, (std::min)(8, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitMaskDivisor", 0, ini.c_str()))));
  c.cockpitMaskViews = static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitMaskViews", 3, ini.c_str())) & 15;
  if (!c.cockpitMaskViews) c.cockpitMaskViews = 3;
  c.cockpitMaskNearM = (std::max)(0.0, (std::min)(20.0, ReadDouble(L"Perception", L"CockpitMaskNearM", 1.0, ini)));
  c.cockpitMaskDilate =
      (std::max)(0, (std::min)(4, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitMaskDilate", 0, ini.c_str()))));
  c.cockpitMaskDump =
      (std::max)(0, (std::min)(300, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitMaskDump", 0, ini.c_str()))));
  c.cockpitMaskLog =
      (std::max)(0, (std::min)(1000, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitMaskLog", 0, ini.c_str()))));
  c.cockpitDepthExport = GetPrivateProfileIntW(L"Perception", L"CockpitDepthExport", 0, ini.c_str()) != 0;  // R69
  c.cockpitDepthDump =
      (std::max)(0, (std::min)(300, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitDepthDump", 0, ini.c_str()))));
  c.cockpitMotionExport = GetPrivateProfileIntW(L"Perception", L"CockpitMotionExport", 0, ini.c_str()) != 0;  // R70
  c.cockpitMotionClass = GetPrivateProfileIntW(L"Perception", L"CockpitMotionClass", 1, ini.c_str()) != 0;
  c.cockpitMotionDump =
      (std::max)(0, (std::min)(300, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitMotionDump", 0, ini.c_str()))));
  c.cockpitFgDump =  // R71
      (std::max)(0, (std::min)(300, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitFgDump", 0, ini.c_str()))));
  c.symbologyExport = GetPrivateProfileIntW(L"Perception", L"SymbologyExport", 0, ini.c_str()) != 0;  // R95
  c.symbologyAlpha = GetPrivateProfileIntW(L"Perception", L"SymbologyAlpha", 1, ini.c_str()) != 0;
  {  // R97: [Dev] SymbologyPublishAlways (default 0) from the settings' ini, else from the deployed one
    int pa = static_cast<int>(GetPrivateProfileIntW(L"Dev", L"SymbologyPublishAlways", -1, ini.c_str()));
    if (pa < 0 && ini != deployedIni)
      pa = static_cast<int>(GetPrivateProfileIntW(L"Dev", L"SymbologyPublishAlways", -1, deployedIni.c_str()));
    c.symbologyPublishAlways = pa > 0;
  }
  c.cockpitTrueIpd = static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpd", 0, ini.c_str()));
  if (c.cockpitTrueIpd < 0 || c.cockpitTrueIpd > 2) c.cockpitTrueIpd = 0;
  c.cockpitTrueIpdLighting = GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdLighting", 1, ini.c_str()) != 0;
  c.cockpitTrueIpdForward = GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdForward", 1, ini.c_str()) != 0;
  c.cockpitTrueIpdHelmet = GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdHelmet", 1, ini.c_str()) != 0;
  c.cockpitTrueIpdOwnship =
      std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdOwnship", 0, ini.c_str())), 0, 2);
  c.cockpitTrueIpdOverlays = GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdOverlays", 1, ini.c_str()) != 0;
  c.cockpitTrueIpdOverlayMvp = GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdOverlayMvp", 1, ini.c_str()) != 0;
  c.cockpitTrueIpdLog =
      (std::max)(0, (std::min)(600, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdLog", 0,
                                                                           ini.c_str()))));
  c.cockpitTrueIpdPrevLog = (std::max)(
      0, (std::min)(150, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdPrevLog", 0, ini.c_str()))));
  c.cockpitTrueIpdFrameDump = (std::max)(
      0, (std::min)(600, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdFrameDump", 0, ini.c_str()))));
  c.cockpitTrueIpdMvpDump = (std::max)(
      0, (std::min)(2000, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdMvpDump", 0, ini.c_str()))));
  {
    wchar_t dv[32] = {};
    GetPrivateProfileStringW(L"Perception", L"CockpitTrueIpdFrameDumpViews", L"23", dv, 32, ini.c_str());
    int mask = 0;
    for (const wchar_t* q = dv; *q; ++q) {
      if (*q >= L'0' && *q <= L'3') mask |= 1 << (*q - L'0');
      if (*q == L'm' || *q == L'M') mask |= 1 << 4;
    }
    c.cockpitTrueIpdFrameDumpViews = mask ? mask : 0x0C;
  }
  c.cockpitTrueIpdOverlayRestore =
      GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdOverlayRestore", 0, ini.c_str()) != 0;
  {
    wchar_t op[32] = {};
    GetPrivateProfileStringW(L"Perception", L"CockpitTrueIpdOverlayPath", L"cpu", op, 32, ini.c_str());
    if (_wcsicmp(op, L"d3d11") == 0) {
      c.cockpitTrueIpdOverlayPath = 1;
    } else {
      c.cockpitTrueIpdOverlayPath = 0;
      if (_wcsicmp(op, L"cpu") != 0) Log("config: [Perception] CockpitTrueIpdOverlayPath=%ls is invalid; cpu", op);
    }
  }
  c.cockpitTrueIpdTestShiftMm =
      (std::max)(-40.0, (std::min)(40.0, ReadDouble(L"Perception", L"CockpitTrueIpdTestShiftMm", 0.0, ini)));
  {
    wchar_t hk[64] = {};
    GetPrivateProfileStringW(L"Perception", L"CockpitTrueIpdToggle", L"0:0", hk, 64, ini.c_str());  // R107: off
    if (!ParseHotkey(hk, c.cockpitTrueIpdToggleVk, c.cockpitTrueIpdToggleMods))
      Log("config: [Perception] CockpitTrueIpdToggle=%ls is invalid; key disabled", hk);
  }
  {
    wchar_t g[64] = {};
    GetPrivateProfileStringW(L"Perception", L"FixationDepthTestGaze", L"", g, 64, ini.c_str());
    c.fixationTestGaze = g;
  }
  if (c.statsIntervalSec < 1) c.statsIntervalSec = 1;
  {
    // [General] PerceptionOnly: latched by the initial read (the hooks it
    // leaves out are never installed; nothing installed is taken out later).
    const bool po = GetPrivateProfileIntW(L"General", L"PerceptionOnly", 0, ini.c_str()) != 0;
    static bool poChangeLogged = false;
    if (initial) {
      g_perceptionOnly = po;
      poChangeLogged = false;
      if (po)
        Log("config: perception only ([General] PerceptionOnly=1): fixation depth, far depth export and the cockpit "
            "ipd options only; no engine optimization, recorder, benchmark, suite, governor action or engine key; "
            "status word 0");
    } else if (po != g_perceptionOnly.load() && !poChangeLogged) {
      poChangeLogged = true;
      Log("config: [General] PerceptionOnly=%d takes effect at the next DCS start (now %d)", po ? 1 : 0,
          g_perceptionOnly.load() ? 1 : 0);
    }
    if (g_perceptionOnly.load()) c = PerceptionOnlyConfig(c);
  }
  {  // R42 colour modes (color_pass.h): [Color] Mode / ModeKey / Timing, [ColorFix], [ColorEnhanced]
    const int m = static_cast<int>(GetPrivateProfileIntW(L"Color", L"Mode", 0, ini.c_str()));
    g_colorCfg.mode = m >= 0 && m <= 2 ? m : 0;
    g_colorCfg.timing = GetPrivateProfileIntW(L"Color", L"Timing", 0, ini.c_str()) != 0 ? 1 : 0;
    // R47 (dev A/B, default 3): bit 0 normalize the context table at the tone-map arm, bit 1 its state-call toggle
    // points and the normal-not-ours repair.
    cpass::g_armRepair = static_cast<int>(GetPrivateProfileIntW(L"Color", L"ArmRepair", 3, ini.c_str())) & 3;
    g_colorCfg.fix = cgrade::ReadModeIni(ini.c_str(), 1);
    g_colorCfg.enh = cgrade::ReadModeIni(ini.c_str(), 2);
    wchar_t hk[64] = {};
    GetPrivateProfileStringW(L"Color", L"ModeKey", L"0:0", hk, 64, ini.c_str());  // default off; 120:6 = Alt+Shift+F9
    if (!ParseHotkey(hk, g_colorKeyVk, g_colorKeyMods)) Log("config: [Color] ModeKey=%ls is invalid; key disabled", hk);
    static int lastIniMode = -1;
    if (g_colorCfg.mode != lastIniMode) g_colorKeyMode = -1;  // a new ini value wins over the key's choice
    lastIniMode = g_colorCfg.mode;
  }
  {  // R51 VrFx (vrfx.h, color_pass.h): [VrFx] GazeDof / GVision, default off; any change applies within a second
    const vrfx::Params p = vrfx::ReadIni(ini.c_str());
    static vrfx::Params last;
    static bool first = true;
    if (first || !vrfx::Same(p, last)) {
      while (vrfx::g_paramLock.test_and_set(std::memory_order_acquire)) YieldProcessor();
      vrfx::g_params = p;
      vrfx::g_paramLock.clear(std::memory_order_release);
      vrfx::g_paramVer++;
      if (!first || p.dof || p.gv)
        Log("config: [VrFx] GazeDof=%d (strength %.2f, max %.1f arcmin, saccade release %d, settle %.0f ms, fade-in "
            "%.0f ms, tolerance %.2f D, fovea guard %.1f deg, pupil %s%.1f mm) GVision=%d (strength %.2f, use gaze %d, "
            "red-out %d, tolerance %.1f G, mode %d)",
            p.dof, p.dofStrength, p.dofMaxArcmin, p.dofSaccadeRelease, p.dofSettleMs, p.dofFadeInMs, p.dofToleranceD,
            p.dofGuardDeg, p.dofPupilMm > 0 ? "fixed " : "from the scene; ", p.dofPupilMm, p.gv, p.gvStrength,
            p.gvUseGaze, p.gvRedout, p.gvTolerance, p.gvMode);
      last = p;
      first = false;
    }
    vrfx::g_wantDof = p.dof != 0;
    vrfx::g_wantGv = p.gv != 0;
  }
  {  // R41 live probes (dev, measure only; color_grade.h): a new non-zero value starts a probe session
    static int lastTone = 0, lastSwap = 0, lastGeo = 0;
    const int tone = static_cast<int>(GetPrivateProfileIntW(L"Color", L"ToneProbe", 0, ini.c_str()));
    const int swap = static_cast<int>(GetPrivateProfileIntW(L"Color", L"SwapProbe", 0, ini.c_str()));
    const int geo = GetPrivateProfileIntW(L"Suite", L"GeoOnlyProbe", 0, ini.c_str()) != 0 ? 1 : 0;
    cgrade::g_toneProbeViews = static_cast<int>(GetPrivateProfileIntW(L"Color", L"ToneProbeViews", 15, ini.c_str())) & 15;
    cgrade::g_toneProbeHdr = GetPrivateProfileIntW(L"Color", L"ToneProbeHdr", 0, ini.c_str()) != 0 ? 1 : 0;
    if (tone != lastTone && tone > 0) cgrade::g_toneProbeReq = tone;
    if (swap != lastSwap && swap > 0) cgrade::g_swapProbeReq = swap;
    if (geo != lastGeo) Log("probe: [Suite] GeoOnlyProbe=%d (focus main G-buffer into an off-target viewport; lossy, test only)", geo);
    cgrade::g_geoOnlyProbe = geo;
    lastTone = tone;
    lastSwap = swap;
    lastGeo = geo;
    // R94: [Perception] SymbologyTrace (dev, measure only; sym_trace_cfg.h): a new value > 0 starts a session.
    const int st = static_cast<int>(GetPrivateProfileIntW(L"Perception", L"SymbologyTrace", 0, ini.c_str()));
    symtrcfg::Configure(st, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"SymbologyTraceViews", 15, ini.c_str())),
                        static_cast<int>(GetPrivateProfileIntW(L"Perception", L"SymbologyTraceSegments", 0, ini.c_str())),
                        static_cast<int>(GetPrivateProfileIntW(L"Perception", L"SymbologyTraceTone", 1, ini.c_str())),
                        g_devDir.empty() ? g_dir : g_devDir);
  }
  g_cfg = c;
  ApplyR90Config();
  if (g_perceptionOnly.load()) {
    PerceptionOnlyFlags();
    return;
  }
  if (!initial && g_engineOff.exchange(false)) Log("hotkey: engine optimizations back ON (ini changed)");
  if (g_benchRunningFlag.load()) return;  // the benchmark owns the toggle while it runs
  g_enabled = c.enabled;
  g_debugHole = c.debugHole;
  g_partitionBoost = c.partitionBoost && !g_engineOff.load();
  g_allocSlabBytes = static_cast<uint32_t>(c.slabBytes);
  g_allocSlabsOn = c.allocSlabs && !g_engineOff.load();
  g_texDedupeOn = c.texDedupe && !g_engineOff.load();
  ApplyTextureHook();
  g_triPlainOn = c.triPlain && !g_engineOff.load();
  ApplyTriCounter();
  g_cbSkipOn = c.cbSkip && !g_engineOff.load();
  ApplyCbSkip();
  g_costWeightsOn = c.costWeights && !g_engineOff.load();
  g_cbUploadSkipOn = c.cbUploadSkip && !g_engineOff.load();
  ApplyCbUpload();
  g_pacerLowPowerOn = c.pacerLowPower && !g_engineOff.load();
  ApplyPacer();
  g_taskClockOn = c.taskClock && !g_engineOff.load();
  ApplyTaskClock();
  g_frameHeapOn = c.frameHeapSlabs && !g_engineOff.load();
  if (g_frameHeapOn.load()) ApplyFrameHeap();
  g_shadowTexSkipOn = c.shadowTexSkip && !g_engineOff.load();
  ApplyShadowTexSkip();
  ApplyBigPages(c.bigPages && !g_engineOff.load());
  ApplyParUpload(c.parUpload && !g_engineOff.load());
  ApplyDirectUpload(c.directUpload && !g_engineOff.load());
  ApplySplitFilter(c.splitFilter && !g_engineOff.load());
  ApplyShadowInst();
  ApplyShadowBatch();
  ApplyGBufferBatch();
  ApplyShadowRecorder();
  ApplyGBufferRecorder();
  ApplyPassFlush();
  ApplySrvTailTrim(c.srvTailTrim && !g_engineOff.load());
  ApplyPrefetch();
  ApplyHoldYaw(c.holdYawDeg);
  ApplyPoseTest(c.holdPitchDeg, c.poseRotateDps, c.holdPose, c.poseWallClock);
  SetTimerResolution(c.fineTimer);
  g_d3dMode = c.d3dFilter ? 1 : 0;
  g_shadowTight = c.shadowTight;
  g_shadowDebugInvert = c.shadowDebugInvert;
  ApplyTimerConfig();
  Log("config: enabled=%d debugHole=%d keepFraction=%.3f safetyNdc=%.3f saccadeDeg=%.2f "
      "saccadeHold=%d focusRatio=%.2f apexTol=%.3f collectThreadsMax=%d",
      c.enabled, c.debugHole, c.keepFraction, c.safetyNdc, c.saccadeDeg, c.saccadeHoldFrames,
      c.focusRatio, c.apexTolerance, c.collectThreadsMax);
}

