// Averages focus/peripheral renderables per frame over `ms` milliseconds.
void MeasureRenderables(int ms, double* periph, double* focus, uint64_t* frames) {
  {
    std::lock_guard<std::mutex> lock(g_bench.mutex);
    g_bench.cur = BenchBlock{};
    g_bench.recording = true;
  }
  Sleep(ms);
  std::lock_guard<std::mutex> lock(g_bench.mutex);
  g_bench.recording = false;
  *frames = g_bench.cur.frames;
  *periph = g_bench.cur.frames ? g_bench.cur.periphRend / g_bench.cur.frames : 0;
  *focus = g_bench.cur.frames ? g_bench.cur.focusRend / g_bench.cur.frames : 0;
}

// Checks that exclusion volumes actually remove objects: with DebugHole the
// focus views exclude their own full frustum, so every object Scene.dll can
// exclude disappears from them.
// Frame time distribution over `ms` milliseconds (quad-view frames).
void FrameTimes(int ms) {
  {
    std::lock_guard<std::mutex> lock(g_bench.mutex);
    g_bench.cur = BenchBlock{};
    g_bench.cur.intervalsMs.reserve(4096);
    g_bench.recording = true;
  }
  int64_t c0 = 0;
  Sleep(ms);
  std::vector<float> v;
  {
    std::lock_guard<std::mutex> lock(g_bench.mutex);
    g_bench.recording = false;
    v = g_bench.cur.intervalsMs;
  }
  (void)c0;
  if (v.size() < 10) {
    Log("  not enough frames");
    return;
  }
  std::sort(v.begin(), v.end());
  auto q = [&](double p) { return v[std::min(v.size() - 1, static_cast<size_t>(p * v.size()))]; };
  double sum = 0;
  int over = 0;
  for (float x : v) {
    sum += x;
    if (x > 1.5 * q(0.5)) ++over;
  }
  Log("  %zu frames: mean %.2f ms (%.1f fps), p50 %.2f, p95 %.2f, p99 %.2f, max %.2f ms, spikes >1.5x median: %d",
      v.size(), sum / v.size(), 1000.0 * v.size() / sum, q(0.5), q(0.95), q(0.99), v.back(), over);
}

void ExclusionSelfTest() {
  Log("-- exclusion self-test --");
  bool wasOn = g_enabled.load(), wasHole = g_debugHole.load();
  g_enabled = true;
  double p0, f0, p1, f1;
  uint64_t n0, n1;
  g_debugHole = false;
  Sleep(500);
  MeasureRenderables(2000, &p0, &f0, &n0);
  g_debugHole = true;
  Sleep(500);
  MeasureRenderables(2000, &p1, &f1, &n1);
  g_debugHole = wasHole;
  g_enabled = wasOn;
  double drop = f0 > 0 ? (f0 - f1) / f0 * 100.0 : 0.0;
  Log("  focus renderables: normal %.0f, with focus excluding itself %.0f (-%.1f%%), frames %llu/%llu",
      f0, f1, drop, static_cast<unsigned long long>(n0), static_cast<unsigned long long>(n1));
  Log("  peripheral renderables: %.0f -> %.0f", p0, p1);
  if (n0 == 0 || n1 == 0)
    Log("  RESULT: no quad-view frames measured");
  else if (drop > 80)
    Log("  RESULT: exclusion volumes work on almost every object");
  else if (drop > 20)
    Log("  RESULT: exclusion volumes work on part of the objects (the rest lacks the OBB flag)");
  else
    Log("  RESULT: exclusion volumes have almost no effect   <-- check");
}

// Measurement hooks (timelines, culling tasks, per-model cost, shadow and
// render passes). Not needed for the optimisations themselves: installed at
// start only with [General] Diagnostics=1, otherwise when a suite starts.
// R144: also on a live [General] Diagnostics 0 -> 1 (ApplyDiagnostics). Every hook here is a vtable / IAT slot write
// (the allocator's 14-byte patch waits for the next collect, alloc_slab.h), pass-through outside a measurement.
void InstallDiagnostics() {
  static std::atomic<bool> done{false};
  if (done.exchange(true)) return;
  ctiming::Install(GetModuleHandleW(L"Scene.dll"));
  ttiming::Install(GetModuleHandleW(L"Scene.dll"));
  mtiming::Install();
  shadowpass::Install();
  ptiming::Install();
  allocslab::Prepare();
  texbind::Install(g_tscHz);
  g_diagInstalled = true;
}

// R144: [General] Diagnostics on an ini reload (LoadConfig, worker). On: installed now if not yet. Off: logged once
// per 1 -> 0; the hooks stay until the next DCS start (DiagnosticsLiveAction, config.inc).
void ApplyDiagnostics(bool want) {
  static bool prevWant = false;
  const int a = DiagnosticsLiveAction(want, prevWant, g_diagInstalled.load(), g_startInstallsDone.load(),
                                      g_perceptionOnly.load());
  prevWant = want;
  if (a == kDiagInstall) {
    InstallDiagnostics();
    Log("diagnostics: measurement hooks installed on a live enable ([General] Diagnostics)");
  } else if (a == kDiagStaysUntilRestart) {
    Log("diagnostics: [General] Diagnostics=0: the measurement hooks stay installed (pass-through) until the next DCS "
        "start");
  }
}

// Suite: the recorder installed and warmed up (2 s on: the probes learn the
// keys and meshes). verify: then the same-frame compare and 2 s recorded,
// and the configured state back. False (logged) when it cannot run.
bool ShadowRecPhase(bool verify) {
  shrec::g_scope = g_cfg.shadowRecorderScope;
  shrec::g_waitUs = static_cast<uint32_t>(g_cfg.shadowRecorderWaitUs);
  shrec::g_priority = g_cfg.shadowRecorderPriority;
  shrec::g_split = g_cfg.shadowRecorderSplit;
  shrec::g_helpers = static_cast<uint32_t>(g_cfg.shadowRecorderHelpers);
  shrec::g_instancing = g_cfg.shadowRecorderInstancing;
  if (g_cfg.gbufferBatch && g_cfg.shadowInst) {
    Log("  shadow recorder: off while [Model] GBufferBatching=1");
    return false;
  }
  for (int i = 0; i < 20 && shrec::g_state.load() == 0 && !shrec::Install(); ++i) Sleep(250);
  if (!shrec::Ready()) {
    Log("  shadow recorder not available (state %d%s; needs [Model] ShadowInstancing=1 and shadow batching's hooks, "
        "see the log above)",
        shrec::g_state.load(), shrec::g_disabled.load() ? ", latched off" : "");
    return false;
  }
  const bool was = shrec::g_on.load();
  shrec::g_on = true;
  Sleep(2000);
  if (!verify) return true;  // the A/B toggles it per block, then ApplyShadowRecorder
  shrec::ResetCounters();
  uint64_t f0 = g_quadFrame.load();
  shrec::g_verify = true;
  for (int t = 0; t < g_cfg.suiteShadowRecVerifySec * 10 && !g_benchAbort && !shrec::g_disabled.load(); ++t) Sleep(100);
  shrec::g_verify = false;
  Sleep(100);
  shrec::LogCounters("verify", static_cast<double>(g_quadFrame.load() - f0));
  if (shrec::g_disabled.load()) {
    Log("  verify: differences found or a fault -> recorder NOT SAFE, latched off for this session");
  } else {
    shrec::ResetCounters();
    f0 = g_quadFrame.load();
    Sleep(2000);
    shrec::LogCounters("2 s recorded", static_cast<double>(g_quadFrame.load() - f0));
  }
  shrec::g_on = was;
  ApplyShadowRecorder();
  return true;
}

// Suite: the G-buffer recorder installed and warmed up (the G-buffer keys
// collected and compiled, then 3 s on: reads, probes, texture table). verify:
// then the same-frame compare and 2 s recorded, and the configured state back.
// False (logged) when it cannot run.
bool GBufferRecPhase(bool verify) {
  gbrec::g_scope = g_cfg.gbufferRecorderScope;
  gbrec::g_waitUs = static_cast<uint32_t>(g_cfg.gbufferRecorderWaitUs);
  gbrec::g_island = static_cast<uint32_t>(g_cfg.gbufferRecorderIsland);
  gbrec::g_stride = static_cast<uint32_t>(g_cfg.suiteGBufferRecVerifyStride);
  gbrec::g_maxSeg = static_cast<uint32_t>(g_cfg.gbufferRecorderMaxSegments);
  gbrec::g_helpers = static_cast<uint32_t>(g_cfg.gbufferRecorderHelpers);
  gbrec::g_cockpit = g_cfg.gbufferRecorderCockpit;
  gbrec::g_byOrdinal = g_cfg.gbufferRecorderByOrdinal;
  if (g_cfg.gbufferBatch && g_cfg.shadowInst) {
    Log("  gbuffer recorder: off while [Model] GBufferBatching=1");
    return false;
  }
  if (!g_cfg.shadowInst) {
    Log("  gbuffer recorder: needs [Model] ShadowInstancing=1 (the G-buffer key compiles)");
    return false;
  }
  const bool wasCollecting = shadowinst::g_onGb.load();
  shadowinst::SetOnGb(true);
  for (int i = 0; i < 20 && gbrec::g_state.load() == 0 && !gbrec::Install(); ++i) Sleep(250);
  if (!gbrec::Ready()) {
    Log("  gbuffer recorder not available (state %d%s; see the log above)", gbrec::g_state.load(),
        gbrec::g_disabled.load() ? ", latched off" : "");
    if (!wasCollecting) ApplyGBufferBatch();
    return false;
  }
  const bool was = gbrec::g_on.load();
  gbrec::g_on = true;
  Sleep(2000);
  const double waitS = shadowinst::WaitCompiles(g_benchAbort);
  const shadowinst::Totals keys = shadowinst::Snap(shadowinst::kKindGb);
  Log("  g-buffer keys: %u OK of %u (waited %.1f s)", keys.ok, keys.keys, waitS);
  Sleep(3000);
  if (!verify) return true;  // the A/B toggles it per block, then ApplyGBufferRecorder
  gbrec::ResetCounters();
  uint64_t f0 = g_quadFrame.load();
  gbrec::g_verify = true;
  for (int t = 0; t < g_cfg.suiteGBufferRecVerifySec * 10 && !g_benchAbort && !gbrec::g_disabled.load(); ++t)
    Sleep(100);
  gbrec::g_verify = false;
  Sleep(100);
  gbrec::LogCounters("verify", static_cast<double>(g_quadFrame.load() - f0));
  if (gbrec::g_disabled.load()) {
    Log("  verify: differences found or a fault -> G-buffer recorder NOT SAFE, latched off for this session");
  } else {
    Sleep(300);
    gbrec::ResetCounters();
    f0 = g_quadFrame.load();
    Sleep(2000);
    gbrec::LogCounters("2 s recorded", static_cast<double>(g_quadFrame.load() - f0));
  }
  gbrec::g_on = was;
  ApplyGBufferRecorder();
  if (!wasCollecting) ApplyGBufferBatch();
  return true;
}

DWORD WINAPI SuiteThread(void*) {
  InstallDiagnostics();
  g_benchRunningFlag = true;
  g_benchAbort = false;
  if (g_stop.load()) g_benchAbort = true;  // payload stopping (StopWorkerThreads sets g_stop first)
  StartReportCapture();
  SYSTEMTIME st;
  GetLocalTime(&st);
  Log("==== DcsQvCull test suite %04d-%02d-%02d %02d:%02d ====", st.wYear, st.wMonth, st.wDay, st.wHour,
      st.wMinute);
  Chime(800, 100);
  Chime(1200, 100);
  CheckConfiguration();
  if (!WaitForFocus()) {
    Log("==== suite aborted: session never became FOCUSED ====");
    StopReportCapture();
    g_benchRunningFlag = false;
    return 0;
  }
  if (!g_cfg.suiteQuick) {
  Log("-- frame times (10 s) --");
  FrameTimes(10000);
  Log("-- render passes, render thread CPU (5 s) --");
  ptiming::Measure(5000, g_quadFrame);
  if (g_cfg.suiteSelfTest) ExclusionSelfTest();
  {
    Log("-- culling partition entries (one frame) --");
    {
      std::lock_guard<std::mutex> lock(g_entryDumpMutex);
      g_entryDump.clear();
    }
    g_entryDumpRequest = true;
    for (int i = 0; i < 50 && g_entryDumpRequest.load(); ++i) Sleep(20);
    Sleep(100);
    std::lock_guard<std::mutex> lock(g_entryDumpMutex);
    for (auto& l : g_entryDump) Log("%s", l.c_str());
  }
  Chime(1000, 60);
  }  // !suiteQuick
  bool ok = true;
  if (g_cfg.suiteProfile) {
    Log("-- CPU profile --");
    char label[64];
    snprintf(label, sizeof(label), "suite, optimisation %s, keep %.2f", g_enabled.load() ? "ON" : "OFF",
             EffectiveKeep());
    prof::Run(std::max(2, g_cfg.profileSeconds), std::max(1, g_cfg.profileThreads),
              std::max(1, g_cfg.profilePeriodMs), label);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchCull && !g_benchAbort) {
    Log("-- A/B: peripheral exclusion --");
    ok = RunBenchmark(0, false);
    Chime(1000, 60);
  }
  if (!g_cfg.suiteQuick) {
    Log("-- culling timeline (5 s) --");
    ctiming::Snap a = ctiming::Take();
    uint64_t f0 = g_quadFrame.load();
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    Sleep(5000);
    QueryPerformanceCounter(&q1);
    uint64_t frames = g_quadFrame.load() - f0;
    double frameMs = frames ? (q1.QuadPart - q0.QuadPart) * g_qpcToUs / 1000.0 / frames : 0.0;
    ctiming::Report(a, ctiming::Take(), frames, frameMs);
    Log("-- culling tasks (5 s) --");
    ttiming::Measure(5000, g_quadFrame);
    Log("-- per-model culling work (5 s) --");
    mtiming::Measure(5000, g_quadFrame);
    Log("-- shadow cascades (5 s) --");
    shadowpass::Measure(5000, g_quadFrame);
    {
      Log("-- model data allocator and texture binds (5 s) --");
      g_hookMeasure = true;
      texbind::SetAttached(true);
      allocslab::Totals sa = allocslab::Snapshot();
      texbind::Totals ta = texbind::Snapshot();
      uint64_t af0 = g_quadFrame.load();
      Sleep(5000);
      uint64_t aframes = g_quadFrame.load() - af0;
      allocslab::Report(sa, allocslab::Snapshot(), aframes, g_tscHz);
      texbind::Report(ta, texbind::Snapshot(), aframes, g_tscHz);
      g_hookMeasure = false;
      texbind::SetAttached(g_texDedupeOn.load());
    }
    Chime(1000, 60);
  }
  bool filterSafe = false;
  if (d3ds::g_vtblCount.load() > 0) {
    const int savedMode = g_d3dMode.load();
    g_d3dMode = 0;
    Log("-- D3D11 state calls (5 s) --");
    d3ds::Totals a = d3ds::Snapshot();
    uint64_t f0 = g_quadFrame.load();
    Sleep(5000);
    d3ds::Report(a, d3ds::Snapshot(), g_quadFrame.load() - f0);
    // Verification: every call the filter would skip is checked against the
    // binding d3d11 really has. Any mismatch means skipping is not safe.
    Log("-- D3D11 filter verification (10 s): would-be-skipped calls checked against d3d11 --");
    g_d3dMode = 2;
    a = d3ds::Snapshot();
    f0 = g_quadFrame.load();
    Sleep(10000);
    d3ds::Totals b = d3ds::Snapshot();
    g_d3dMode = savedMode;
    d3ds::Report(a, b, g_quadFrame.load() - f0);
    uint64_t mm = d3ds::Mismatches(a, b);
    filterSafe = mm == 0;
    Log("  verification: %llu mismatches -> filter %s", static_cast<unsigned long long>(mm),
        filterSafe ? "SAFE" : "NOT SAFE (A/B skipped)");
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchTimer && timercache::g_orig && !g_benchAbort) {
    Log("-- A/B: shader time cache --");
    ok = RunBenchmark(2, false);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchPartition && !g_benchAbort) {
    Log("-- A/B: culling partition 12 -> 16 tasks --");
    ok = RunBenchmark(3, false);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchAllocSlabs && allocslab::g_state.load() >= 2 && !g_benchAbort) {
    Log("-- A/B: per-thread slabs for the model data allocator --");
    ok = RunBenchmark(9, false);
    Chime(1000, 60);
  }
  if (g_cfg.suiteTerrain) {
    Log("-- terrain draws (5 s) --");
    terrcount::Measure(5000, g_quadFrame, g_tscHz);
    Log("-- metashader binder repeats (5 s) --");
    bindcount::Measure(5000, g_quadFrame);
    imcount::Measure(5000, g_quadFrame, g_tscHz, bindcount::g_thread);  // g_thread = render thread seen by bindcount
    Log("-- model data pages --");
    allocslab::LogClasses();
    Log("-- g-buffer batching potential (5 s) --");
    gbcount::Measure(5000, g_quadFrame);
    Log("-- shadow caster batching potential (5 s) --");
    instcount::Measure(5000, g_quadFrame);
    Log("-- shadow casters through material slot 5 (5 s) --");
    shadowcount::Measure(5000, g_quadFrame, g_tscHz);
    if (cbupload::Install()) {
      Log("-- per-view context buffer uploads (5 s) --");
      memset(cbupload::g_view, 0, sizeof(cbupload::g_view));
      cbupload::g_viewCalls = cbupload::g_viewSame = 0;
      const bool wasAttached = cbupload::g_attached.load();
      cbupload::SetAttached(true);
      const uint64_t f0 = g_quadFrame.load();
      cbupload::g_viewMeasure = true;
      Sleep(5000);
      cbupload::g_viewMeasure = false;
      Sleep(50);
      if (!wasAttached) cbupload::SetAttached(false);
      const double f = static_cast<double>(g_quadFrame.load() - f0);
      if (f > 0)
        Log("  0x970-byte uploads: %.1f/frame, identical to that buffer's previous upload %.1f%%",
            cbupload::g_viewCalls / f,
            cbupload::g_viewCalls ? 100.0 * cbupload::g_viewSame / cbupload::g_viewCalls : 0.0);
    }
  }
  if (ok && g_cfg.suiteShadowInstCompile && !g_benchAbort) {
    Log("-- shadow instancing stage 1: compile shadow-caster VS variants (collect 3 s, then wait) --");
    shadowinst::SuitePhase(3000, g_benchAbort);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteGBufferInstCompile && !g_benchAbort) {
    Log("-- g-buffer instancing stage 1: compile main-pass model_vs variants, reflection gate (collect 3 s, then "
        "wait) --");
    shadowinst::SuitePhaseGb(3000, g_benchAbort, &g_quadFrame);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteGBufferTexCount && !g_benchAbort) {
    Log("-- g-buffer texture sets the deferred P0 never reads (R14 lead 3 counter; collect 1 s, wait for the keys, "
        "count 5 s) --");
    shadowinst::SuitePhaseGbTex(1000, 5000, g_benchAbort, &g_quadFrame);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteDirectUploadCount && !g_benchAbort) {
    Log("-- direct upload gates: draws and allocations around the mapped-page window (R16 G1-G3, 5 s) --");
    ducount::Measure(5000, g_quadFrame, g_tscHz, g_benchAbort);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteFxApplyCount && !g_benchAbort) {
    Log("-- FX pass Apply: same pass again (R14 lead 4 counter, 5 s) --");
    srvtrim::Detach();  // its code check covers the trim's call sites: measured on stock
    fxapply::Measure(5000, g_quadFrame, g_tscHz);
    ApplySrvTailTrim(g_cfg.srvTailTrim && !g_engineOff.load());
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteSrvSpanCount && !g_benchAbort) {
    Log("-- setShaderResources: passed span vs changed span, identical tail (R15 F2 counter, 5 s) --");
    srvtrim::Detach();  // same call sites: the counter measures stock
    srvspan::Measure(5000, g_quadFrame, g_tscHz);
    ApplySrvTailTrim(g_cfg.srvTailTrim && !g_engineOff.load());
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteJoinTailCount && !g_benchAbort) {
    Log("-- culling join: render-thread chunk tail after the last worker chunk (R15 F3 counter, 5 s) --");
    jointail::Measure(5000, g_quadFrame, g_qpcToUs);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteShadowRecCount && !g_benchAbort) {
    Log("-- shadow recorder S0 counters: caster mix and loop time per cascade, pass setup stability, material and "
        "texture checks, driver support (R17, 1 s discovery + 5 s) --");
    shadowrec::Measure(5000, g_quadFrame, g_tscHz);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteGBufferRecCount && !g_benchAbort) {
    Log("-- g-buffer recorder S0 counters: item mix and loop time per execution, segments per candidate set and "
        "island, RDEF census, pass setup stability, material bytes, Execute(TRUE) cost, textures (R18, 1 s discovery, "
        "key compiles, 5 s) --");
    gbreccount::Measure(5000, g_quadFrame, g_tscHz, g_benchAbort);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteForwardRecCount && !g_benchAbort) {
    Log("-- forward recorder S0 counters: SimplePassData calls by pass name, item mix and loop time, gb_rec rules per "
        "model draw, segments per island, blended share, RDEF census, pass setup stability, material bytes (R21, 1 s "
        "discovery, key compiles, 5 s) --");
    fwdreccount::Measure(5000, g_quadFrame, g_tscHz, g_benchAbort);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteGpuPassTiming && !g_benchAbort) {
    Log("-- render passes, GPU time (D3D11 timestamps) and context copies/clears/dispatches per pass kind, with "
        "render-thread CPU for comparison (5 s) --");
    gpt::g_statsWanted = g_cfg.suiteGpuPassStats;
    gpt::Measure(5000, g_quadFrame);
    gpt::g_statsWanted = false;
    Chime(1000, 60);
  }
  if (ok && vrsp::g_keyBlock.load() > 0 && !g_benchAbort) {  // R139: measure-only VRS probe ([Suite] VrsProbe)
    vrsp::SuitePhase(g_quadFrame);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suitePeriphLodTiming > 0 && !g_benchAbort) {
    Log("-- LOSSY prototype: periphery-only instancer LOD tables x %.2f, GPU timing OFF/ON/OFF/ON/OFF (periph_lod.h, "
        "5 x 5 s) --",
        g_cfg.suitePeriphLodTiming / 100.0);
    gpt::g_statsWanted = g_cfg.suiteGpuPassStats;
    plod::TimingPhase(g_cfg.suitePeriphLodTiming, g_engineOff.load() ? 0 : g_cfg.peripheralLodScale, g_quadFrame);
    gpt::g_statsWanted = false;
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteCascadeCacheTiming && !g_benchAbort) {
    Log("-- PROTOTYPE: far shadow cascade cache, GPU timing OFF/ON/OFF/ON/OFF (cascade_cache.h, mask 0x%x, max age %d, "
        "margin %d %%, 5 x 5 s) --",
        g_cfg.cascadeCacheMask, g_cfg.cascadeCacheMaxAge, g_cfg.cascadeCacheMarginPct);
    gpt::g_statsWanted = g_cfg.suiteGpuPassStats;
    cascache::TimingPhase(g_cfg.cascadeCache && !g_engineOff.load(), g_cfg.cascadeCacheMask,
                          static_cast<uint32_t>(g_cfg.cascadeCacheMaxAge), static_cast<uint32_t>(g_cfg.cascadeCacheMarginPct),
                          g_quadFrame);
    gpt::g_statsWanted = false;
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteChainFastTiming && !g_benchAbort) {
    Log("-- PROTOTYPE: shorter culling chains, GPU timing OFF/ON/OFF/ON/OFF (chain_fast.h, mask 0x%x, 5 x 5 s) --",
        g_cfg.chainFast ? g_cfg.chainFast : 7u);
    gpt::g_statsWanted = g_cfg.suiteGpuPassStats;
    chainfast::TimingPhase(g_engineOff.load() ? 0 : g_cfg.chainFast, g_quadFrame);
    gpt::g_statsWanted = false;
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteChainFastVerify > 0 && !g_benchAbort) {
    Log("-- PROTOTYPE: shorter culling chains, same-frame check against the stock kernels (chain_fast.h, mask 0x%x, "
        "%d s) --",
        g_cfg.chainFast, g_cfg.suiteChainFastVerify);
    chainfast::VerifyPhase(g_cfg.suiteChainFastVerify, g_engineOff.load() ? 0 : g_cfg.chainFast, g_quadFrame);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteShaderFastTiming && !g_benchAbort) {
    Log("-- PROTOTYPE: wider grids for single-SM kernels, GPU timing OFF/ON/OFF/ON/OFF (shader_fast.h, mask 0x%x, "
        "5 x 5 s) --",
        g_cfg.shaderFast ? g_cfg.shaderFast : 3u);
    gpt::g_statsWanted = g_cfg.suiteGpuPassStats;
    sfast::TimingPhase(g_engineOff.load() ? 0 : g_cfg.shaderFast, g_quadFrame);
    gpt::g_statsWanted = false;
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteShaderFastVerify > 0 && !g_benchAbort) {
    Log("-- PROTOTYPE: wider grids for single-SM kernels, same-frame check against the stock kernels (shader_fast.h, "
        "mask 0x%x, %d s) --",
        g_cfg.shaderFast ? g_cfg.shaderFast : 3u, g_cfg.suiteShaderFastVerify);
    sfast::VerifyPhase(g_cfg.suiteShaderFastVerify, g_engineOff.load() ? 0 : g_cfg.shaderFast, g_quadFrame);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suitePeriphLodImage > 0 && !g_benchAbort && !g_devDir.empty()) {
    const int pct = g_cfg.suitePeriphLodTiming > 0 ? g_cfg.suitePeriphLodTiming : 50;
    Log("-- LOSSY prototype: final per-view images (CopyToHMDPass targets), periphery LOD x %.2f, OFF/ON/OFF, %d "
        "frames each, raw files in the dev folder's plod folder (periph_lod.h) --",
        pct / 100.0, g_cfg.suitePeriphLodImage);
    plod::ImagePhase(pct, g_engineOff.load() ? 0 : g_cfg.peripheralLodScale, g_cfg.suitePeriphLodImage, g_devDir);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchPeripheralLod && !g_benchAbort) {
    const int pct = g_cfg.suitePeriphLodTiming > 0 ? g_cfg.suitePeriphLodTiming : 50;
    Log("-- A/B: periphery-only instancer LOD x %.2f (LOSSY; OFF = stock tables, ON = scaled tables in the peripheral "
        "views) --",
        pct / 100.0);
    plod::Apply(pct);
    if (plod::WaitReady(3000)) {
      ok = RunBenchmark(32, false);
      plod::LogCounters("after the A/B");
    } else {
      Log("  periph lod: tables not captured; A/B skipped");
    }
    plod::Apply(g_engineOff.load() ? 0 : g_cfg.peripheralLodScale);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteVramCount && !g_benchAbort) {
    Log("-- video memory: DXGI budget and usage per second, DcsQvCull's own buffers, model-data pages, the recorders' "
        "texture tables (stale and orphaned views)%s (%d s) --",
        g_cfg.suiteVramCountCreates ? ", DCS's buffer/texture creates" : "", g_cfg.suiteVramCountSec);
    vramc::Measure(g_cfg.suiteVramCountSec, g_cfg.suiteVramCountCreates, g_quadFrame, g_benchAbort);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteYawScan && !g_benchAbort) {
    Log("-- yaw scan: fps per held view yaw (2 s settle + 2 s each) --");
    int best = 0;
    double bestFps = 1e9;
    for (int deg = 0; deg < 360 && !g_benchAbort; deg += 15) {
      posesweep::Hold(true, deg);
      Sleep(2000);
      const uint64_t f0 = g_quadFrame.load();
      LARGE_INTEGER a, b;
      QueryPerformanceCounter(&a);
      Sleep(2000);
      QueryPerformanceCounter(&b);
      const double fps = (g_quadFrame.load() - f0) / ((b.QuadPart - a.QuadPart) * g_qpcToUs / 1e6);
      Log("  yaw %3d deg: %.1f fps", deg, fps);
      if (fps < bestFps) {
        bestFps = fps;
        best = deg;
      }
    }
    Log("  heaviest view: yaw %d deg (%.1f fps); set [Suite] HoldYawDeg=%d to keep it", best, bestFps, best);
    if (g_cfg.holdYawDeg >= 0)
      posesweep::Hold(true, g_cfg.holdYawDeg);
    else
      posesweep::Hold(false, 0);
  }
  if (ok && g_cfg.suiteFrameStartGap && g_cfg.suiteFrameStartGapSec > 0 && !g_benchAbort) {
    Log("-- frame start: xrEndFrame return -> first pass on the render thread, GPU drain and gap, OpenXR and driver "
        "waits, per-frame class (R22 E1, %d s%s) --",
        g_cfg.suiteFrameStartGapSec, posesweep::Holding() ? ", held view" : "");
    fstart::RunPhase(g_cfg.suiteFrameStartGapSec, g_quadFrame, g_benchAbort);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteRunnableThreads && g_cfg.suiteRunnableThreadsSec > 0 && !g_benchAbort) {
    Log("-- runnable threads: DCS threads running / runnable-waiting at render entry and at a recorder job start, "
        "late recorder jobs, per-thread CPU (R22 E3, %d s, a snapshot every %d frames%s) --",
        g_cfg.suiteRunnableThreadsSec, g_cfg.suiteRunnableThreadsEvery, posesweep::Holding() ? ", held view" : "");
    rthreads::RunPhase(g_cfg.suiteRunnableThreadsSec, g_cfg.suiteRunnableThreadsEvery, g_tscHz, g_benchAbort);
    Chime(1000, 60);
  }
  vprof::g_withFs = g_cfg.suiteFrameStartGap;
  vprof::g_withRt = g_cfg.suiteRunnableThreads;
  vprof::g_rtEvery = g_cfg.suiteRunnableThreadsEvery;
  if (ok && g_cfg.suiteYawProfile && !g_benchAbort) {
    Log("-- yaw profile: cost split and limit per held view yaw, every %d deg (2 s settle + 2.5 s each) --",
        std::max(5, std::min(180, g_cfg.suiteYawProfileStep)));
    vprof::RunYawProfile(g_cfg.suiteYawProfileStep, g_quadFrame, g_benchAbort, g_tscHz);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteRotationProfile && !g_benchAbort) {
    Log("-- rotation profile: 3 s still, then a %d deg/s turn for %d s: cost split, transients, spikes --",
        g_cfg.suiteRotationDegPerSec, g_cfg.suiteRotationSeconds);
    vprof::RunRotationProfile(g_cfg.suiteRotationDegPerSec, g_cfg.suiteRotationSeconds, g_quadFrame, g_benchAbort,
                              g_tscHz);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteShadowInstVerify && !g_benchAbort) {
    Log("-- shadow instancing stage 2: same-frame depth compare, stock vs batched (3 s) --");
    if (!shadowbatch::Install()) {
      Log("  shadow batching not ready (needs [Model] ShadowInstancing=1 and the compiled variants)");
    } else {
      const bool was = shadowbatch::g_on.load();
      shadowbatch::ResetCounters();
      shadowbatch::g_verify = true;
      shadowbatch::g_on = true;
      Sleep(3000);
      shadowbatch::g_verify = false;
      shadowbatch::g_on = was;
      Sleep(100);
      shadowbatch::LogCounters("verify");
      shadowbatch::ResetCounters();
      shadowbatch::g_on = true;
      Sleep(2000);
      shadowbatch::g_on = was;
      Sleep(100);
      shadowbatch::LogCounters("2 s batched");
    }
  }
  if (ok && g_cfg.suiteShadowRecVerify && !g_benchAbort) {
    Log("-- shadow recorder (R17 S3-S6, scope 0x%x): 2 s warm-up (probes), then same-frame depth compare per "
        "recorded cascade, stock vs recorded (%d s), then 2 s recorded --",
        g_cfg.shadowRecorderScope, g_cfg.suiteShadowRecVerifySec);
    ShadowRecPhase(true);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchParUpload && parupload::Install() && !g_benchAbort) {
    Log("-- A/B: parallel copy for large dynamic structured-buffer uploads --");
    parupload::g_calls = parupload::g_bytes = parupload::g_cycles = 0;
    const uint64_t f0 = g_quadFrame.load();
    ok = RunBenchmark(24, false);
    const double f = static_cast<double>(g_quadFrame.load() - f0);
    if (f > 0)
      Log("  parallel uploads during the ON blocks: %.1f/frame, %.2f MB/frame, %.3f ms/frame (whole run frames)",
          parupload::g_calls / f, parupload::g_bytes / f / 1048576.0, parupload::g_cycles / g_tscHz * 1000.0 / f);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteDirectUploadVerify && !g_benchAbort) {
    Log("-- direct upload: sentinel verify of the mapped model data pages (R16 checks a-c, 3 s), then 2 s direct --");
    if (!DirectUploadReady()) {
      // logged
    } else if (directupload::g_disabled.load()) {
      Log("  direct upload is latched off for this session; not verified");
    } else if (!directupload::StartVerify()) {
      Log("  direct upload verify: no memory for the block log");
    } else {
      const bool was = directupload::g_on.load();
      directupload::ResetCounters();
      uint64_t f0 = g_quadFrame.load();
      directupload::g_on = true;
      Sleep(3000);
      directupload::g_on = was;
      directupload::StopVerify();
      Sleep(100);
      directupload::LogCounters("verify", static_cast<double>(g_quadFrame.load() - f0));
      directupload::LogVerify();
      if (!directupload::g_disabled.load()) {
        directupload::ResetCounters();
        f0 = g_quadFrame.load();
        directupload::g_on = true;
        Sleep(2000);
        directupload::g_on = was;
        Sleep(100);
        directupload::LogCounters("2 s direct", static_cast<double>(g_quadFrame.load() - f0));
      }
    }
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchDirectUpload && !g_benchAbort && DirectUploadReady()) {
    Log("-- A/B: direct upload into mapped GPU pages (OFF = stock memcpy upload, ON = direct) --");
    if (directupload::g_disabled.load()) {
      Log("  direct upload is latched off for this session; A/B skipped");
    } else {
      directupload::ResetCounters();
      const uint64_t f0 = g_quadFrame.load();
      ok = RunBenchmark(26, false);
      directupload::LogCounters("A/B, whole run (mapping in ON blocks only)",
                                static_cast<double>(g_quadFrame.load() - f0));
    }
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteSplitFilterVerify && !g_benchAbort) {
    Log("-- split-path D3D11 state filter (R15 F5): every skip checked against d3d11 (10 s), then 5 s filtered --");
    if (SplitFilterReady()) {
      sfilt::ResetCounters();
      sfilt::g_count = true;
      sfilt::g_verify = true;
      uint64_t f0 = g_quadFrame.load();
      Sleep(10000);
      sfilt::g_verify = false;
      sfilt::LogCounters("verify", static_cast<double>(g_quadFrame.load() - f0));
      if (sfilt::g_disabled.load()) {
        Log("  verify: mismatches found -> split filter NOT SAFE, latched off for this session");
      } else {
        Log("  verify: 0 mismatches");
        sfilt::ResetCounters();
        f0 = g_quadFrame.load();
        Sleep(5000);
        sfilt::LogCounters("5 s filtered", static_cast<double>(g_quadFrame.load() - f0));
      }
      sfilt::g_count = false;
      ApplySplitFilter(g_cfg.splitFilter && !g_engineOff.load());
    }
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchSplitFilter && !g_benchAbort && SplitFilterReady()) {
    Log("-- A/B: split-path D3D11 state filter (OFF = stock, no patch or hook; ON = filter, no counting) --");
    // No counting in the A/B: the per-call counters (site lookup by return
    // address) cost more than the skips save [M 2026-10-09: -2.6% with them].
    sfilt::ResetCounters();
    sfilt::g_count = false;
    const uint64_t f0 = g_quadFrame.load();
    ok = RunBenchmark(27, false);
    sfilt::g_count = false;
    sfilt::LogCounters("A/B, whole run (filter in ON blocks only)", static_cast<double>(g_quadFrame.load() - f0));
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteSrvTailTrimVerify && !g_benchAbort) {
    Log("-- setShaderResources identical-tail trim (R15 F2): every trimmed call checked against stock's call on "
        "d3d11 (%d s) --",
        g_cfg.suiteSrvTailTrimVerifySec);
    if (SrvTailTrimReady()) {
      srvtrim::ResetCounters();
      const uint64_t f0 = g_quadFrame.load();
      if (srvtrim::Attach(true)) {
        for (int t = 0; t < g_cfg.suiteSrvTailTrimVerifySec * 20 && !g_benchAbort && !srvtrim::g_disabled.load(); ++t)
          Sleep(50);
        srvtrim::Detach();
        srvtrim::LogCounters("verify", static_cast<double>(g_quadFrame.load() - f0));
        if (srvtrim::g_disabled.load())
          Log("  verify: mismatches found -> srv tail trim NOT SAFE, latched off for this session");
        else
          Log("  verify: 0 mismatches");
      }
      ApplySrvTailTrim(g_cfg.srvTailTrim && !g_engineOff.load());
    }
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchPrefetch && !g_benchAbort) {
    // Both variants installed (same hook path), so the A/B is the skip alone.
    if (pffix::g_state.load() == 0) pffix::Install(pffix::kObserve);
    if (pffix::g_state.load() == 1) {
      Log("-- A/B: terrain prefetch fix (OFF = observe, every call to the kernel; ON = repeats skipped) --");
      const uint64_t c0 = pffix::SumCalls(), s0 = pffix::SumSkipped();
      ok = RunBenchmark(33, false);  // ends with ApplyPrefetch (the configured state back)
      const uint64_t c = pffix::SumCalls() - c0, sk = pffix::SumSkipped() - s0;
      Log("  prefetch calls during the bench: %llu, skipped %.1f%% (ON blocks only skip)",
          static_cast<unsigned long long>(c), c ? 100.0 * static_cast<double>(sk) / static_cast<double>(c) : 0.0);
      Chime(1000, 60);
    } else {
      Log("  prefetch fix unavailable this session: bench skipped");
    }
  }
  if (ok && g_cfg.suiteBenchSrvTailTrim && !g_benchAbort && SrvTailTrimReady()) {
    Log("-- A/B: setShaderResources identical-tail trim (OFF = stock call sites, ON = trim, no counting) --");
    ok = RunBenchmark(31, false);  // ends with ApplySrvTailTrim (the configured state back)
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchShadowInst && shadowbatch::Install() && !g_benchAbort) {
    Log("-- A/B: shadow caster instancing --");
    ok = RunBenchmark(22, false);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchShadowRecorder && !g_benchAbort) {
    Log("-- A/B: shadow recorder (OFF = DCS draws every caster, ON = the recorded casters from a worker's command "
        "list; scope 0x%x) --",
        g_cfg.shadowRecorderScope);
    if (ShadowRecPhase(false)) {
      shrec::ResetCounters();
      const uint64_t f0 = g_quadFrame.load();
      ok = RunBenchmark(28, false);
      shrec::LogCounters("A/B, whole run (recorder in ON blocks only)", static_cast<double>(g_quadFrame.load() - f0));
    }
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteGBufferRecVerify && !g_benchAbort) {
    Log("-- g-buffer recorder (R18 S3, scope 0x%x): key compiles, 3 s warm-up (reads, probes, texture table), then "
        "same-frame compare of all G-buffer targets per scoped execution, stock vs stock then stock vs recorded (%d "
        "s), then 2 s recorded --",
        g_cfg.gbufferRecorderScope, g_cfg.suiteGBufferRecVerifySec);
    GBufferRecPhase(true);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchGBufferRecorder && !g_benchAbort) {
    Log("-- A/B: g-buffer recorder (OFF = DCS draws every item, ON = the recorded draws from command lists; scope "
        "0x%x) --",
        g_cfg.gbufferRecorderScope);
    if (GBufferRecPhase(false)) {
      gbrec::ResetCounters();
      const uint64_t f0 = g_quadFrame.load();
      ok = RunBenchmark(29, false);
      gbrec::LogCounters("A/B, whole run (recorder in ON blocks only)", static_cast<double>(g_quadFrame.load() - f0));
    }
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchPassFlush && !g_benchAbort) {
    const uint32_t mask = g_cfg.suiteBenchPassFlushMask ? g_cfg.suiteBenchPassFlushMask : g_cfg.passFlush;
    Log("-- A/B: Flush at pass boundaries (OFF = no Flush, ON = mask 0x%x: %s); GPU timestamps (light) and the "
        "frame-start probe run in both --",
        mask, pflush::MaskText(mask).c_str());
    if (!mask)
      Log("  nothing to compare: [Suite] BenchPassFlushMask and [Model] PassFlush are 0");
    else
      ok = PassFlushBench(mask);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchShadowPlanAsync && shadowbatch::Install() && !g_benchAbort) {
    Log("-- A/B: shadow batching plans on planner threads --");
    if (!shadowbatch::g_on.load()) Log("  shadow batching is off ([Model] ShadowBatching or the kill switch)");
    shadowbatch::ResetCounters();
    ok = RunBenchmark(25, false);
    shadowbatch::LogCounters("plan A/B");
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteGBufferInstVerify && !g_benchAbort) {
    Log("-- g-buffer instancing stage 2: same-frame compare of all G-buffer targets, stock vs stock then stock vs "
        "batched --");
    // The G-buffer keys must be compiled first (collect 3 s, then wait).
    const bool wasCollecting = shadowinst::g_onGb.load();
    shadowinst::SetOnGb(true);
    Sleep(3000);
    const double waitS = shadowinst::WaitCompiles(g_benchAbort);
    const shadowinst::Totals t = shadowinst::Snap(shadowinst::kKindGb);
    Log("  g-buffer keys: %u OK of %u (waited %.1f s)", t.ok, t.keys, waitS);
    if (!gbbatch::Install() || !gbverify::Install()) {
      Log("  g-buffer batching not ready (needs [Model] ShadowInstancing=1, the compiled variants and the analysed "
          "build)");
    } else if (gbbatch::g_disabled.load()) {
      Log("  g-buffer batching is latched off for this session; not verified");
    } else {
      const bool was = gbbatch::g_on.load();
      gbbatch::ResetCounters();
      gbverify::ResetCounters();
      gbbatch::g_verify = true;
      gbbatch::g_on = true;
      Sleep(6000);
      gbbatch::g_verify = false;
      gbbatch::g_on = was;
      Sleep(200);
      gbbatch::LogCounters("verify");
      gbverify::LogCounters();
      if (!gbbatch::g_disabled.load()) {
        gbbatch::ResetCounters();
        gbbatch::g_on = true;
        Sleep(2000);
        gbbatch::g_on = was;
        Sleep(100);
        gbbatch::LogCounters("2 s batched");
      }
    }
    if (!wasCollecting) ApplyGBufferBatch();
    if (!wasCollecting && !shadowinst::g_onGb.load()) shadowinst::SetOnGb(false);
  }
  if (ok && g_cfg.suiteBenchGBufferInst && gbbatch::Install() && !g_benchAbort) {
    Log("-- A/B: g-buffer instancing --");
    ok = RunBenchmark(23, false);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchBigPages && allocslab::g_state.load() >= 1 && bigpages::Install() && !g_benchAbort) {
    Log("-- A/B: big model data pages vs stock 63.5 KB pages --");
    ok = RunBenchmark(21, false);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchTexTable && texbind::g_attached.load() && g_texDedupeOn.load() && !g_benchAbort) {
    Log("-- A/B: texture dedupe table, 16-byte entries vs previous 24-byte entries --");
    ok = RunBenchmark(20, false);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchShadowTex && shadowtex::Install() && !g_benchAbort) {
    Log("-- shadow-caster texture sets with the skip on (5 s) --");
    {
      g_shadowTexSkipOn = true;
      ApplyShadowTexSkip();
      // The masks need the casters' keys compiled: collect 3 s, wait for the
      // compile workers, then let first sight build each shader's mask.
      Sleep(3000);
      const double waitS = shadowinst::WaitCompiles(g_benchAbort);
      const shadowinst::Totals keys = shadowinst::Snap();
      Log("  shadow keys for the masks: %u keys, %u compiled (%u with both variants OK), %u not finished; "
          "waited %.1f s",
          keys.keys, keys.done, keys.ok, keys.keys - keys.done, waitS);
      Sleep(500);
      shadowtex::Totals a = shadowtex::Snapshot();
      const uint64_t f0 = g_quadFrame.load();
      Sleep(5000);
      shadowtex::Report(a, shadowtex::Snapshot(), g_quadFrame.load() - f0);
      g_shadowTexSkipOn = g_cfg.shadowTexSkip && !g_engineOff.load();
      ApplyShadowTexSkip();
    }
    Log("-- A/B: skip shadow-caster texture sets the shadow passes do not read --");
    ok = RunBenchmark(19, false);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchFrameHeap && frameheap::Install() && !g_benchAbort) {
    Log("-- A/B: per-thread slabs for the edCore frame heap --");
    ok = RunBenchmark(18, false);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchPacer && pacer::Install(g_tscHz, g_cfg.pacerTimeoutUs) && !g_benchAbort) {
    Log("-- A/B: Main-thread pacer wait with MWAITX instead of PAUSE spinning --");
    ok = RunBenchmark(17, false);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchCbUpload && cbupload::Install() && !g_benchAbort) {
    Log("-- A/B: skip constant-buffer uploads identical to the last one --");
    g_hookMeasure = true;  // count identical uploads (the hook stays attached in OFF blocks too)
    ApplyCbUpload();
    uint64_t c0 = cbupload::g_calls.load(), s0 = cbupload::g_same.load();
    ok = RunBenchmark(16, false);
    uint64_t c = cbupload::g_calls.load() - c0, sm = cbupload::g_same.load() - s0;
    g_hookMeasure = false;
    ApplyCbUpload();
    Log("  constant-buffer uploads seen: %llu, identical to the last upload: %.1f%%",
        static_cast<unsigned long long>(c), c ? 100.0 * sm / c : 0.0);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchCostWeights && partw::Install() && !g_benchAbort) {
    Log("-- A/B: culling partition weighted by measured per-object cost --");
    Sleep(1500);  // let the per-object costs fill in
    uint64_t e0 = partw::g_getterCalls.load(), m0 = partw::g_getterRewritten.load();
    ok = RunBenchmark(15, false);
    uint64_t e = partw::g_getterCalls.load() - e0, m = partw::g_getterRewritten.load() - m0;
    Log("  weight requests from Scene.dll: %llu, given a measured cost: %.1f%%", static_cast<unsigned long long>(e),
        e ? 100.0 * m / e : 0.0);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchMicro && cbskip::Install() && tricount::Prepare() && !g_benchAbort) {
    Log("-- A/B: micro (constant buffer skip + plain triangle counter together) --");
    ok = RunBenchmark(14, false);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchCbSkip && cbskip::Install() && !g_benchAbort) {
    Log("-- A/B: skip setting the same FX constant buffer again --");
    g_hookMeasure = true;  // count hits during the A/B (same cost in OFF and ON blocks: hook detached in OFF)
    uint64_t c0 = cbskip::g_calls.load(), h0 = cbskip::g_hits.load();
    ok = RunBenchmark(13, false);
    g_hookMeasure = false;
    uint64_t c = cbskip::g_calls.load() - c0, h = cbskip::g_hits.load() - h0;
    Log("  constant buffer sets seen in ON blocks: %llu, already set (skipped) %.1f%%",
        static_cast<unsigned long long>(c), c ? 100.0 * h / c : 0.0);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchTriPlain && tricount::Prepare() && !g_benchAbort) {
    Log("-- A/B: plain add on NGModel's triangle statistics counter --");
    ok = RunBenchmark(12, false);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchEngine && !g_benchAbort) {
    Log("-- A/B: all engine optimizations (OFF = stock DCS behaviour, ON = as configured) --");
    ok = RunBenchmark(11, false);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchTexDedupe && texbind::g_orig && !g_benchAbort) {
    Log("-- A/B: skip repeated texture streaming requests within 1 ms --");
    ok = RunBenchmark(10, false);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchShadow && !g_benchAbort) {
    Log("-- A/B: tighter shadow caster culling --");
    ok = RunBenchmark(8, false);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchFilter && filterSafe && !g_benchAbort) {
    if (g_cfg.suiteBenchFilter > 1) {
      Log("-- A/B: D3D11 filter, OFF = hooks forwarding everything, ON = filter --");
      ok = RunBenchmark(6, false);
    }
    if (ok && !g_benchAbort) {
      Log("-- A/B: D3D11 filter end to end (OFF = no hooks at all, ON = filter) --");
      ok = RunBenchmark(7, false);
    }
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchTimerRes && !g_benchAbort) {
    Log("-- A/B: system timer resolution 0.5 ms --");
    ok = RunBenchmark(5, false);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchIsolation && !g_benchAbort) {
    Log("-- A/B: render thread isolation --");
    ok = RunBenchmark(4, false);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteBenchThreads && !g_benchAbort) {
    Log("-- A/B: collect threads --");
    ok = RunBenchmark(1, false);
  }
  ok = ok && !g_benchAbort;
  Log("==== suite %s ====", ok ? "finished" : "aborted");
  std::string report = StopReportCapture();
  wchar_t name[64];
  swprintf_s(name, L"report_%04d%02d%02d_%02d%02d%02d.txt", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond);
  FILE* f = nullptr;
  _wfopen_s(&f, (g_dir + name).c_str(), L"w");
  if (f) {
    fputs(report.c_str(), f);
    fclose(f);
  }
  Log("suite: report written to %ls", name);
  if (ok)
    for (int k = 0; k < 3; ++k) Chime(1500, 90);
  else
    Chime(300, 300);
  g_benchRunningFlag = false;
  return 0;
}

