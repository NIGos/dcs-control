// Worker start, after LoadConfig(true): [D3D] ChainFast's creation hook and the self tests' private device.
// R115 P6b: both only with the engine kill switch off (the device was created and held for the session
// even with the engine off). Not released if the kill switch turns on later: TestPending / PrepareClear
// re-acquire it lazily on the worker, so a release there would need their gating too.
void ChainFastAtStart() {
  if (g_perceptionOnly.load() || g_engineOff.load()) return;
  // R34 [D3D] ChainFast: the creation hook goes in first, before DCS loads the mission's terrain kernels.
  if (g_cfg.chainFast) chainfast::Apply(g_cfg.chainFast);
  // R90 F4: the self tests' private device now, not when the first kernel turns up mid-flight.
  if (g_cfg.chainFast || g_cfg.shaderFast) chainfast::AcquirePrivate();
}

DWORD WINAPI WorkerThread(void*) {
  reloc::g_log = [](const char* line) { Log("%s", line); };
  reloc::g_slotRead = [](void** slot) { return SlotOriginal(slot); };
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_qpcToUs = 1e6 / static_cast<double>(f.QuadPart);
  // R90 section 7: output caps, before the config read applies shadow instancing (its workers read cache\).
  {
    const qvdev::DevPath dp = qvdev::ReadDevPath(L"IniPath", g_dir + L"DcsQvCull.ini");
    std::wstring devDir;
    if (dp.state == qvdev::kDevOk) devDir = dp.path.substr(0, dp.path.find_last_of(L"\\/") + 1);
    const ULONGLONG t0 = GetTickCount64();
    const prune::Stats ps = prune::RunAtStart(g_dir, devDir.empty() ? g_dir : devDir);
    if (ps.files || ps.dirs)
      Log("disk: pruned %u files and %u folders (%.1f MB) in %llu ms (newest %u per output family, cache\\*.qvc "
          "least recently used past %u MB or %u files, orphaned cache temp files)",
          ps.files, ps.dirs, ps.bytes / 1048576.0, static_cast<unsigned long long>(GetTickCount64() - t0),
          prune::kKeepFiles, static_cast<unsigned>(prune::kQvcCapBytes >> 20), prune::kQvcCapFiles);
  }
  LoadConfig(true);
  ChainFastAtStart();
  perfmon::Start();  // R30 L3: the env line (DCS, CPU, OpenXR runtime and layers, streamers)
  g_dumpsLeft = g_cfg.dumpViews;
  {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    uint64_t t0 = __rdtsc();
    Sleep(100);
    QueryPerformanceCounter(&b);
    g_tscHz = (__rdtsc() - t0) / ((b.QuadPart - a.QuadPart) * g_qpcToUs / 1e6);
  }
  const bool po = g_perceptionOnly.load();  // latched by LoadConfig(true)
  if (!Install()) {  // the collect hook (observer only in PerceptionOnly: HookCollect)
    if (!po || g_stop.load()) return 0;
    Log("perception only: continuing without the collect hook (no measured eye separation, no camera cut "
        "detection)");
  }
  if (po) {
    // Perception only: nothing of the engine. DCS's device for the depth
    // features (shadow instancing and the split filter are not there to find
    // it) and the NGModel globals for the cockpit ipd, retried each second.
    pdev::Install();
    PerceptionGlobalsTick();
  } else {
    if (g_cfg.diagnostics) InstallDiagnostics();
    if (g_cfg.allocSlabs) allocslab::Prepare();
    if (g_cfg.costWeights) partw::Install();
    if (g_cfg.texDedupe) texbind::Install(g_tscHz);
    ApplyShadowTexSkip();
    if (timercache::Install(g_cfg.shaderTimeCacheUs)) ApplyTimerConfig();
    ApplyTaskClock();
    if (g_cfg.d3dMeter && !d3ds::Install()) Log("d3d meter: not installed");
  }

  bool f9 = false, f10 = false, f11 = false, f12 = false;
  ULONGLONG lastCensus = GetTickCount64();
  uint64_t censusFrames0 = 0;
  bool autoStarted = false;
  ULONGLONG lastStats = GetTickCount64();
  ULONGLONG lastCfg = lastStats;
  g_statusSink.Publish(StatusWordNow());  // hooks installed: running (PerceptionOnly: running + bit 38 only)
  while (!g_stop.load() && !qvexit::Exiting()) {
    Sleep(50);
    const HeldKeys keys = ReadKeys(g_cfg, po, &KeyDown);  // PerceptionOnly: the cockpit ipd toggle only
    bool k9 = keys.f9;
    bool k10 = keys.f10;
    bool k11 = keys.f11;
    bool k8 = keys.f8;
    bool kT = keys.toggle;
    static bool fT = false;
    if (kT && !fT && ToggleEngine()) {
      if (g_engineOff.load()) {
        Chime(500, 120);
      } else {
        Chime(1200, 90);
        Chime(1200, 90);
      }
    }
    fT = kT;
    bool kC = keys.cockpit;
    static bool fC = false;
    static ULONGLONG lastC = 0;
    if (kC && !fC && GetTickCount64() - lastC >= 300) {
      lastC = GetTickCount64();
      bool off = !fixdepth::ckipd::g_userOff.load();
      fixdepth::ckipd::g_userOff = off;
      Log("hotkey: cockpit true IPD %s", off ? "OFF (cockpit at the boosted eye)" : "ON");
      if (off) {
        Chime(500, 120);
      } else {
        Chime(1200, 90);
        Chime(1200, 90);
      }
    }
    fC = kC;
    {  // R42: [Color] ModeKey (default off, e.g. 120:6 = Alt+Shift+F9) cycles the colour mode: off -> Fix -> Enhanced -> off
      static bool fK = false;
      const bool kK = g_colorKeyVk > 0 && KeyDown(g_colorKeyVk) && ModifiersMatch(g_colorKeyMods);
      if (kK && !fK) {
        const int cur = g_colorKeyMode.load() >= 0 ? g_colorKeyMode.load() : g_colorCfg.mode;
        const int next = (cur + 1) % 3;
        g_colorKeyMode = next;
        Log("hotkey: colour mode %d (%s)", next, cpass::ModeName(next));
        for (int i = 0; i <= next; ++i) Chime(next ? 900 : 400, 70);  // 1 low beep off, 2 beeps Fix, 3 Enhanced
        ColorTick();
      }
      fK = kK;
    }
    static bool f8 = false;
    if (k8 && !f8) {
      bool on = !g_shadowDebugInvert.load();
      g_shadowDebugInvert = on;
      Log("hotkey: shadow debug invert %s (only the casters tightening removes are drawn)", on ? "ON" : "OFF");
      Chime(on ? 1600 : 300, 120);
    }
    f8 = k8;
    if (k11 && !f11) {
      if (g_benchRunningFlag.load()) {
        g_benchAbort = true;
      } else {
        StartBench();
      }
    }
    f11 = k11;
    bool kUp = keys.up;
    bool kDn = keys.down;
    static bool fUp = false, fDn = false;
    if ((kUp && !fUp) || (kDn && !fDn)) {
      double k = EffectiveKeep() + (kUp ? 0.05 : -0.05);
      k = Clamp(k, 0.0, 1.0);
      g_keepOverride = k;
      Log("hotkey: keep fraction %.2f", k);
      Chime(static_cast<DWORD>(400 + k * 1200), 80);
    }
    fUp = kUp;
    fDn = kDn;
    bool k12 = keys.f12;
    if (k12 && !f12 && !prof::g_running.load()) {
      HANDLE t = CreateThread(nullptr, 0, qvexit::Thread<ProfileThread>, nullptr, 0, nullptr);
      if (t) CloseHandle(t);
    }
    f12 = k12;
    if (!autoStarted && !po && g_cfg.benchAutoStartSec > 0 && g_firstQuadQpc.load()) {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      if ((now.QuadPart - g_firstQuadQpc.load()) * g_qpcToUs / 1e6 > g_cfg.benchAutoStartSec) {
        autoStarted = true;
        Log("bench: auto start");
        StartBench();
      }
    }
    if (k9 && !f9 && !g_benchRunningFlag.load()) {
      bool on = !g_enabled.load();
      g_enabled = on;
      Log("hotkey: optimisation %s", on ? "ON" : "OFF");
      Chime(on ? 1200 : 500, 120);
    }
    if (k10 && !f10) {
      bool on = !g_debugHole.load();
      g_debugHole = on;
      Log("hotkey: debug hole %s", on ? "ON" : "OFF");
      Chime(on ? 1600 : 300, 120);
    }
    f9 = k9;
    f10 = k10;

    ULONGLONG now = GetTickCount64();
    {
      ftl::TaskScope ts(ftl::kTPerf);
      perfmon::Tick();  // once per second: samples, governor; perf/features/vram/env lines on their own cadence
    }
    {
      ftl::TaskScope ts(ftl::kTPlace);
      tplace::Tick(g_cfg.threadCensus, g_cfg.threadCensusSec, g_engineOff.load() ? 0 : g_cfg.workerPlacement,
                   g_cfg.placeRecheckSec, g_cfg.placeRecheckMode);  // R39 / R44 mode 2; R52 re-check; R90 mode
    }
    {
      LARGE_INTEGER qf;
      QueryPerformanceFrequency(&qf);
      ftl::Tick({g_cfg.frameTimeline, g_cfg.timelineSec, g_cfg.poseProbe, g_cfg.eventLog}, g_dir, g_qpcToUs, g_tscHz,
                qf.QuadPart);  // R52 measure only ([Perf] keys, default 0)
    }
    if (now - lastCfg >= 1000) {
      lastCfg = now;
      ftl::TaskScope ts(ftl::kTConfig);
      LoadConfig(false);
      if (!po) pffix::Tick();  // new modules (terrain DLLs per mission), slot verify, 10 s statistics line
      g_statusSink.Publish(StatusWordNow());  // last second's states (the retries below show next time)
      g_dualStatusSink.Publish(fixdepth::ckipd::DualCameraStatusNow());  // [Perception] CockpitTrueIpd
      if (po) {  // PerceptionOnly: the perception prerequisites retried, no engine module
        pdev::Install();
        PerceptionGlobalsTick();
      }
      // NGModel.dll may load after the payload: retry until the hook installs.
      if (!po) {
        if (g_shadowTexSkipOn.load() && (shadowtex::g_state.load() == 0 || shadowinst::g_state.load() == 0) &&
            !g_benchRunningFlag.load())
          ApplyShadowTexSkip();
        if (g_cfg.bigPages && !g_engineOff.load() && bigpages::g_state.load() == 0 && allocslab::g_state.load() >= 1 &&
            !g_benchRunningFlag.load())
          ApplyBigPages(true);
        else if ((!g_cfg.bigPages || g_engineOff.load()) && bigpages::g_state.load() == 0 &&
                 !bigpages::g_undoDone.load() && !g_benchRunningFlag.load())
          bigpages::UndoLeftover();  // R63: retried until NGModel / the reset hook is there
        if (g_cfg.shadowInst && !g_engineOff.load() && shadowinst::g_state.load() == 0) ApplyShadowInst();
        if (g_cfg.shadowBatch && g_cfg.shadowInst && !g_engineOff.load() && shadowbatch::g_state.load() == 0 &&
            !g_benchRunningFlag.load())
          ApplyShadowBatch();
        if (g_cfg.gbufferBatch && g_cfg.shadowInst && !g_engineOff.load() && gbbatch::g_state.load() == 0 &&
            !g_benchRunningFlag.load())
          ApplyGBufferBatch();
        if (g_cfg.shadowRecorder && !g_engineOff.load() && shrec::g_state.load() == 0 && !g_benchRunningFlag.load())
          ApplyShadowRecorder();
        if (g_cfg.gbufferRecorder && !g_engineOff.load() && gbrec::g_state.load() == 0 && !g_benchRunningFlag.load())
          ApplyGBufferRecorder();
        if (g_cfg.passFlush && !g_engineOff.load() && !pflush::Ready() && !g_benchRunningFlag.load()) ApplyPassFlush();
        if (!g_benchRunningFlag.load()) plod::Apply(g_engineOff.load() ? 0 : g_cfg.peripheralLodScale);
        if (!g_benchRunningFlag.load() && (g_cfg.cascadeCache || cascache::g_on.load())) ApplyCascadeCache();
        if (!g_benchRunningFlag.load() && (g_cfg.chainFast || chainfast::g_mask.load()))
          chainfast::Apply(g_engineOff.load() ? 0 : g_cfg.chainFast);  // R34 (default 7; off at 0)
        if (!g_benchRunningFlag.load() && (g_cfg.shaderFast || sfast::g_mask.load()))
          sfast::Apply(g_engineOff.load() ? 0 : g_cfg.shaderFast);  // R36 (default 3; off at 0)
        // R122 per-view DLSS preset (opt-in, default 0: nothing installed). The kill switch makes its hook pass
        // through; the features DCS creates at the next mission load get DCS's own hint again.
        dlssv::Tick(dlssv::ParsePreset(g_cfg.dlssPeripheryPreset.c_str()), dlssv::ParsePreset(g_cfg.dlssFocusPreset.c_str()),
                    g_cfg.dlssIgnoreDriverPreset, g_engineOff.load(), g_cfg.dlssInstrument);  // R130: + Instrument
        // R123 focus twin (opt-in, default 0: nothing installed). The kill switch / a benchmark / key 0 take it out.
        if (g_cfg.focusTwin || ftwin::g_state.load() == 1) {
          ftwin::g_shadowKey = g_cfg.suiteFocusTwinShadow;
          ftwin::g_auditEvery = g_cfg.suiteFocusTwinAuditEvery;
          ftwin::g_trustNeeded = g_cfg.suiteFocusTwinTrust;
          ftwin::g_island = g_cfg.suiteFocusTwinIsland;
          ftwin::g_chunk = g_cfg.suiteFocusTwinChunk;
          ftwin::g_predTol = g_cfg.suiteFocusTwinPredTol;
          ftwin::g_verifyEvery = g_cfg.suiteFocusTwinVerify;
          ftwin::g_instKey = g_cfg.focusTwinInstancer;    // R125
          ftwin::g_patchesKey = g_cfg.suiteFocusTwinPatches;
          ftwin::Tick(g_cfg.focusTwin, g_engineOff.load(), g_benchRunningFlag.load(), g_quadFrame.load());
        }
      }
      // Fixation depth (Depth boost): not an optimization, so neither the kill
      // switch nor a benchmark turns it off.
      ConfigurePerception();  // scene cut, cockpit ipd, helmet, ownship (PerceptionOnly too)
      nighttone::Tick({g_cfg.nightTone, g_cfg.nightFloor, g_cfg.nightToe, g_cfg.nightEv, g_cfg.nightGate});  // R37, opt-in
      ColorTick();  // R42 colour modes, opt-in ([Color] Mode, default 0)
      if (!po) {  // [Suite] GBufferRecVerifyLive (test only): the recorder's same-frame verify while flying, logged when off
        gbrec::g_verifyNoLatch = g_cfg.suiteGBufferRecVerifyNoLatch;
        static bool live = false;
        static uint64_t f0 = 0;
        if (g_cfg.suiteGBufferRecVerifyLive != live) {
          live = g_cfg.suiteGBufferRecVerifyLive;
          if (live) {
            gbrec::ResetCounters();
            f0 = g_quadFrame.load();
            gbrec::g_verify = true;
          } else {
            gbrec::g_verify = false;
            gbrec::LogCounters("verify (live)", static_cast<double>(g_quadFrame.load() - f0));
          }
        }
      }
      TickFixationDepth();  // installs / ticks the depth features (PerceptionOnly too)
      // Split filter: waits for dx11backend and the first draw; detaches after a verify mismatch.
      if (!po && !g_benchRunningFlag.load() &&
          ((g_cfg.splitFilter && !g_engineOff.load() && sfilt::g_state.load() >= 0 && !sfilt::Live()) ||
           (sfilt::g_disabled.load() && sfilt::g_attached.load())))
        ApplySplitFilter(g_cfg.splitFilter && !g_engineOff.load());
      // Srv tail trim: waits for dx11backend; detaches after a verify mismatch.
      if (!po && !g_benchRunningFlag.load() &&
          ((g_cfg.srvTailTrim && !g_engineOff.load() && srvtrim::g_state.load() == 0) ||
           (srvtrim::g_disabled.load() && srvtrim::g_patched.load())))
        ApplySrvTailTrim(g_cfg.srvTailTrim && !g_engineOff.load());
    }
    // Re-read the Quad-Views-Foveated edge smoothing every 10 s once quad views run.
    static ULONGLONG lastQv = 0;
    if (g_firstQuadQpc.load() && now - lastQv >= 10000) {
      lastQv = now;
      bool loaded = GetModuleHandleW(L"XR_APILAYER_MBUCCHIA_quad_views_foveated.dll") != nullptr;
      static int lastLoaded = -1;
      if (static_cast<int>(loaded) != lastLoaded) {
        lastLoaded = loaded;
        g_qvfrLoaded = loaded;
        Log("compositor: %s, keep=%.2f",
            loaded ? "Quad-Views-Foveated layer" : "runtime native quad views (QVFR not loaded)",
            EffectiveKeep());
      }
      double sm = loaded ? ReadQvfrSmoothing() : -1.0;
      if (sm >= 0 && sm != g_qvSmoothing.load()) {
        g_qvSmoothing = sm;
        Log("qvfr: edge smoothing %.2f -> excluded focus area %.0f%% x %.0f%% of each focus view", sm,
            EffectiveKeep() * 100, EffectiveKeep() * 100);
      }
    }
    // Motion profile: run_motion.flag (next to the DLL or the dev ini) records
    // MotionSeconds of per-frame cost by head speed. Two beeps = start (move
    // your head, look around, pan the camera), three beeps = done.
    static ULONGLONG motionEnd = 0;
    static ULONGLONG lastMotionFlag = 0;
    if (now - lastMotionFlag >= 1000) {
      lastMotionFlag = now;
      std::wstring mflag = g_dir + L"run_motion.flag";
      std::wstring mdev = g_devDir.empty() ? std::wstring() : g_devDir + L"run_motion.flag";
      if (!mdev.empty() && GetFileAttributesW(mdev.c_str()) != INVALID_FILE_ATTRIBUTES) mflag = mdev;
      if (!po && !motionEnd && !g_benchRunningFlag.load() &&
          GetFileAttributesW(mflag.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(mflag.c_str());
        InstallDiagnostics();
        motion::g_counters = g_cfg.motionCounters;
        motion::Start(g_tscHz);
        posesweep::g_taxi = g_cfg.motionTaxi;
        if (g_cfg.motionSweep) posesweep::Start();
        partw::g_censusRequest = true;
        motionEnd = now + static_cast<ULONGLONG>(std::max(5, g_cfg.motionSeconds)) * 1000;
        Log("motion: recording %d s", std::max(5, g_cfg.motionSeconds));
        Chime(800, 90);
        Chime(1200, 90);
      }
    }
    // Optional CPU profile during the synthetic sweep: from 8 s with MotionTaxi=1 (the
    // forward slide), else at 69 s (after the yaw phases; the sweep no longer slides).
    static ULONGLONG motionStart = 0;
    static bool motionProfiled = false;
    if (motionEnd && !motionStart) {
      motionStart = now;
      motionProfiled = false;
    }
    if (motionEnd && g_cfg.motionSweep && g_cfg.motionProfile && !motionProfiled &&
        now - motionStart >= (g_cfg.motionTaxi ? 8000u : 69000u) &&
        !prof::g_running.load()) {
      motionProfiled = true;
      HANDLE t = CreateThread(nullptr, 0, qvexit::Thread<ProfileThread>, nullptr, 0, nullptr);
      if (t) CloseHandle(t);
    }
    if (!motionEnd) motionStart = 0;
    if (motionEnd && now >= motionEnd) {
      motionEnd = 0;
      posesweep::Stop();
      motion::StopAndReport();
      for (int k = 0; k < 3; ++k) Chime(1500, 90);
    }
    // Remote trigger: creating run_suite.flag next to the DLL starts the suite.
    static ULONGLONG lastFlag = 0;
    if (!po && now - lastFlag >= 1000) {  // PerceptionOnly: no engine toggle, profile or suite trigger
      lastFlag = now;
      ftl::TaskScope ts(ftl::kTFlags);
      // Remote kill switch: toggle_engine.flag (next to the DLL or the dev ini)
      // toggles every measured optimization, like the [Hotkeys] Toggle key.
      for (const std::wstring& tf : {g_dir + L"toggle_engine.flag",
                                     g_devDir.empty() ? std::wstring() : g_devDir + L"toggle_engine.flag"}) {
        if (!tf.empty() && GetFileAttributesW(tf.c_str()) != INVALID_FILE_ATTRIBUTES) {
          DeleteFileW(tf.c_str());
          ToggleEngine();
        }
      }
      // Remote trigger: run_profile.flag starts a CPU profile alone (no suite
      // phases, so no diagnostic hooks: the production-mode profile, R15 F4).
      for (const std::wstring& pf : {g_dir + L"run_profile.flag",
                                     g_devDir.empty() ? std::wstring() : g_devDir + L"run_profile.flag"}) {
        if (!pf.empty() && GetFileAttributesW(pf.c_str()) != INVALID_FILE_ATTRIBUTES) {
          DeleteFileW(pf.c_str());
          if (!g_benchRunningFlag.load()) {
            Log("profile: started by run_profile.flag");
            if (HANDLE t = CreateThread(nullptr, 0, qvexit::Thread<ProfileThread>, nullptr, 0, nullptr)) CloseHandle(t);
          }
        }
      }
      std::wstring flag = g_dir + L"run_suite.flag";
      std::wstring devFlag = g_devDir.empty() ? std::wstring() : g_devDir + L"run_suite.flag";
      if (!devFlag.empty() && GetFileAttributesW(devFlag.c_str()) != INVALID_FILE_ATTRIBUTES) flag = devFlag;
      if (GetFileAttributesW(flag.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(flag.c_str());
        if (!g_benchRunningFlag.load()) {
          Log("suite: started by run_suite.flag");
          StartBench();
        }
      }
    }
    static ULONGLONG lastXr = 0;
    if (now - lastXr >= 2000) {
      lastXr = now;
      ftl::TaskScope ts(ftl::kTXrState);
      bool focused = XrSessionState() == "XR_SESSION_STATE_FOCUSED";
      if (g_xrFocused.exchange(focused) != focused) {
        if (!focused) g_focusLosses++;
        Log("xr: session %s", focused ? "FOCUSED" : "left FOCUSED (headset asleep or not worn)");
      }
    }
    if (now - lastCensus >= 10000) {
      lastCensus = now;
      ftl::TaskScope ts(ftl::kTCensus);
      uint64_t fr = g_quadFrame.load();
      CensusDump(fr - censusFrames0);
      // Recorders outside suites: every 10 s while on, one summary line each
      // ([Log] RecorderDetail=1: the full blocks, about 55 lines).
      if (shrec::g_on.load() && shrec::Ready() && !g_benchRunningFlag.load()) {
        if (g_cfg.recorderDetail)
          shrec::LogCounters("10 s", static_cast<double>(fr - censusFrames0));
        else
          shrec::LogSummary("10 s", static_cast<double>(fr - censusFrames0));
        shrec::ResetCounters();
      }
      if (gbrec::g_on.load() && gbrec::Ready() && !g_benchRunningFlag.load()) {
        if (g_cfg.recorderDetail)
          gbrec::LogCounters("10 s", static_cast<double>(fr - censusFrames0));
        else
          gbrec::LogSummary("10 s", static_cast<double>(fr - censusFrames0));
        gbrec::ResetCounters();
      }
      censusFrames0 = fr;
    }
    if (now - lastStats >= static_cast<ULONGLONG>(g_cfg.statsIntervalSec) * 1000) {
      double secs = (now - lastStats) / 1000.0;
      lastStats = now;
      ftl::TaskScope ts(ftl::kTStats);
      uint64_t calls = g_stats.calls.exchange(0);
      uint64_t q = g_stats.quadCalls.exchange(0);
      uint64_t pv = g_stats.patchedViews.exchange(0);
      uint64_t usq = g_stats.usQuad.exchange(0);
      uint64_t usa = g_stats.usAll.exchange(0);
      uint64_t pr = g_stats.periphRenderables.exchange(0);
      uint64_t fr = g_stats.focusRenderables.exchange(0);
      uint64_t mv = g_stats.maxViews.exchange(0);
      uint64_t sac = g_saccades.exchange(0);
      static uint64_t refreshPrev = 0;
      uint64_t tRefresh = timercache::RefreshesSince(&refreshPrev);  // R63: g_refreshes stays monotonic
      uint64_t shRend = g_shadowStats.shadowRend.exchange(0);
      uint64_t shFrames = g_shadowStats.frames.exchange(0);
      uint64_t shPlanes = g_shadowStats.planes.exchange(0);
      uint64_t shNoLight = g_shadowStats.skipNoLight.exchange(0);
      uint64_t shNoRecv = g_shadowStats.skipReceiver.exchange(0);
      if (calls == 0) continue;
      Log("stats: %s%s boost=%s cull=%s engine=%s slabs=%s texdedupe=%s timer=%s refresh/s=%.0f keep=%.2f saccades/s=%.1f calls/s=%.1f quad/s=%.1f maxViews=%llu "
          "patched/quad=%.2f collect_ms/quad=%.3f collect_ms/s(all)=%.1f periph_rend/quad=%.0f "
          "focus_rend/quad=%.0f shadow=%s%s shadow_rend/quad=%.0f planes/frame=%.2f skips(light/receiver)=%llu/%llu",
          g_enabled.load() ? "ON" : "OFF", g_faulted.load() ? " FAULTED" : "", g_engineOff.load() ? "off" : "on",
          g_enabled.load() ? "on" : "off", g_engineOff.load() ? "off" : "on", g_allocSlabsOn.load() ? "on" : "off", g_texDedupeOn.load() ? "on" : "off", timercache::g_on.load() ? "cache" : "real", tRefresh / secs, EffectiveKeep(),
          sac / secs, calls / secs,
          q / secs, static_cast<unsigned long long>(mv), q ? double(pv) / q : 0.0,
          q ? usq / 1000.0 / q : 0.0, usa / 1000.0 / secs, q ? double(pr) / q : 0.0,
          q ? double(fr) / q : 0.0, g_shadowTight.load() ? "tight" : "dcs",
          g_shadowDebugInvert.load() ? "+invert" : "", q ? double(shRend) / q : 0.0,
          shFrames ? double(shPlanes) / shFrames : 0.0, static_cast<unsigned long long>(shNoLight),
          static_cast<unsigned long long>(shNoRecv));
    }
  }
  Log("payload: worker stopped");
  return 0;
}

