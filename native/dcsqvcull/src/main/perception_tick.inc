void PerfOnToggle(bool off) { perfmon::OnToggle(off); }

void StartBench() {
  if (g_perceptionOnly.load()) return;  // no benchmark or suite in PerceptionOnly
  if (g_benchRunningFlag.exchange(true)) return;
  perfmon::GovForBench();  // stage 0: the benchmark measures the features as configured
  HANDLE t = CreateThread(nullptr, 0, qvexit::Thread<SuiteThread>, nullptr, 0, nullptr);
  if (t)
    CloseHandle(t);
  else
    g_benchRunningFlag = false;
}

// [Perception] CockpitTrueIpdOwnship as applied. Stage 2 hands the ownship
// executions to the G-buffer recorder (gb_rec.h), which PerceptionOnly never
// installs: there it runs as stage 1 (cockpit_ipd.h OwnBegin; its GbBegin
// also takes stage 1 whenever gbrec::Active() is false), logged once.
int OwnshipMode(int configured, bool perceptionOnly) {
  if (configured != 2 || !perceptionOnly) return configured;
  static bool logged = false;
  if (!logged) Log("cockpit ipd: CockpitTrueIpdOwnship=2 runs as stage 1 (perception only: no G-buffer recorder)");
  logged = true;
  return 1;
}

// The worker's per-second perception steps (with PerceptionOnly=1 the only
// ones that install anything). Cockpit at the true eye distance (R31) is
// configured before fixdepth::Tick, which installs it.
void ConfigurePerception() {
  scenecut::Tick(g_cfg.sceneCutReset);
  fixdepth::ckipd::Configure(g_cfg.cockpitTrueIpd, g_cfg.cockpitTrueIpdLighting, g_cfg.cockpitTrueIpdForward,
                             g_cfg.cockpitTrueIpdLog, g_cfg.cockpitTrueIpdTestShiftMm,
                             g_cfg.d3dMeter || g_cfg.suiteGpuPassTiming,
                             g_cfg.cockpitTrueIpdPrevLog, g_cfg.cockpitTrueIpdOverlays,
                             g_cfg.cockpitTrueIpdOverlayMvp, g_cfg.cockpitTrueIpdFrameDump,
                             g_cfg.cockpitTrueIpdMvpDump, g_dir, g_cfg.cockpitTrueIpdOverlayRestore,
                             g_cfg.cockpitTrueIpdFrameDumpViews, g_cfg.cockpitTrueIpdOverlayPath);
  fixdepth::ckipd::g_cfgHelmet = g_cfg.cockpitTrueIpdHelmet;
  fixdepth::ckipd::SetOwnship(OwnshipMode(g_cfg.cockpitTrueIpdOwnship, g_perceptionOnly.load()));
  fixdepth::ckipd::SetDormant(g_cfg.cockpitIpdDormant);  // R128: [Perception] CockpitIpdDormant
}
void TickFixationDepth() {
  // R65: the cockpit mask export rides on fixation depth's callback (fixdepth::Tick installs it).
  // R69: its depth companion first (CockpitDepthExport needs CockpitMaskExport; without it nothing is mapped).
  {
    static bool loggedNoMask = false;
    if (g_cfg.cockpitDepthExport && !g_cfg.cockpitMaskExport && !loggedNoMask) {
      loggedNoMask = true;
      Log("config: [Perception] CockpitDepthExport=1 needs CockpitMaskExport=1; depth companion off");
    }
    fixdepth::cmask::ConfigureDepth(g_cfg.cockpitDepthExport && g_cfg.cockpitMaskExport, g_cfg.cockpitDepthDump);
    // R70: the motion companion (CockpitMotionExport needs CockpitMaskExport; independent of the depth).
    static bool loggedNoMaskMv = false;
    if (g_cfg.cockpitMotionExport && !g_cfg.cockpitMaskExport && !loggedNoMaskMv) {
      loggedNoMaskMv = true;
      Log("config: [Perception] CockpitMotionExport=1 needs CockpitMaskExport=1; motion companion off");
    }
    fixdepth::cmask::ConfigureMotion(g_cfg.cockpitMotionExport && g_cfg.cockpitMaskExport, g_cfg.cockpitMotionClass,
                                     g_cfg.cockpitMotionDump);
    // R71: the frame-generation fixture dump (dev folder in dev mode; a new value > 0 starts a session).
    fixdepth::cmask::ConfigureFg(g_cfg.cockpitMaskExport ? g_cfg.cockpitFgDump : 0, g_devDir.empty() ? g_dir : g_devDir);
    // R95: the symbology emission companion (SymbologyExport needs CockpitMaskExport).
    static bool loggedNoMaskSym = false;
    if (g_cfg.symbologyExport && !g_cfg.cockpitMaskExport && !loggedNoMaskSym) {
      loggedNoMaskSym = true;
      Log("config: [Perception] SymbologyExport=1 needs CockpitMaskExport=1; symbology companion off");
    }
    fixdepth::cmask::ConfigureSymbology(g_cfg.symbologyExport && g_cfg.cockpitMaskExport, g_cfg.symbologyAlpha,
                                        g_cfg.symbologyPublishAlways);
  }
  fixdepth::cmask::Configure(g_cfg.cockpitMaskExport, g_cfg.cockpitMaskDivisor, g_cfg.cockpitMaskViews,
                             g_cfg.cockpitMaskNearM, g_cfg.cockpitMaskDilate, g_cfg.cockpitMaskDump, g_cfg.cockpitMaskLog,
                             g_devDir.empty() ? g_dir : g_devDir);
  // R51: GazeDof needs FixationDepth's published cone (DepthIn): [VrFx] GazeDof turns it on by itself.
  fixdepth::Tick(g_cfg.fixationDepth || vrfx::g_wantDof.load(), g_cfg.depthGrid, g_cfg.fixationDepthVerify, g_cfg.fixationTestGaze.c_str(),
                 g_cfg.fixationDepthDump, g_devDir.empty() ? g_dir : g_devDir, g_cfg.farDepthExport,
                 g_cfg.farDepthSlots, g_cfg.farDepthLogProj, g_cfg.farDepthPreOverlay, g_cfg.farDepthOverlayTrace,
                 g_cfg.farDepthPostOverlay, g_cfg.farDepthCoverageMode);
}

