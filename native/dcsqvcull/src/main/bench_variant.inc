}  // namespace

// Benchmark variant switch (declared before bench.h).
void RestoreD3dHooks() { d3ds::g_hooksWanted = true; }

void BenchSetEngineOff(bool off) { SetEngineOff(off); }

void SetBenchVariant(bool on) {
  if (g_benchActiveMode.load() == 0) {
    g_enabled = on;
    return;
  }
  if (g_benchActiveMode.load() == 2) {
    timercache::g_on = on;
    return;
  }
  if (g_benchActiveMode.load() == 3) {
    g_partitionBoost = on;
    return;
  }
  if (g_benchActiveMode.load() == 6) {
    g_d3dMode = on ? 1 : 0;
    return;
  }
  if (g_benchActiveMode.load() == 24) {
    ApplyParUpload(on);
    return;
  }
  if (g_benchActiveMode.load() == 26) {
    directupload::g_on = on;
    return;
  }
  if (g_benchActiveMode.load() == 27) {
    if (on)
      sfilt::Attach();
    else
      sfilt::Detach();
    return;
  }
  if (g_benchActiveMode.load() == 22) {
    shadowbatch::g_on = on;
    return;
  }
  if (g_benchActiveMode.load() == 28) {
    shrec::g_on = on;
    return;
  }
  if (g_benchActiveMode.load() == 29) {
    gbrec::g_on = on;
    return;
  }
  if (g_benchActiveMode.load() == 30) {
    pflush::SetMask(on ? g_passFlushBenchMask.load() : 0);
    return;
  }
  if (g_benchActiveMode.load() == 32) {
    plod::g_swapOn = on;
    return;
  }
  if (g_benchActiveMode.load() == 33) {
    pffix::g_mode = on ? pffix::kSkip : pffix::kObserve;
    return;
  }
  if (g_benchActiveMode.load() == 31) {
    if (on)
      srvtrim::Attach(false);
    else
      srvtrim::Detach();
    return;
  }
  if (g_benchActiveMode.load() == 25) {
    shadowbatch::g_async = on;
    return;
  }
  if (g_benchActiveMode.load() == 23) {
    gbbatch::g_on = on;
    return;
  }
  if (g_benchActiveMode.load() == 21) {
    ApplyBigPages(on);
    return;
  }
  if (g_benchActiveMode.load() == 20) {
    BenchUseCompactTexTable(on);
    return;
  }
  if (g_benchActiveMode.load() == 19) {
    g_shadowTexSkipOn = on;
    ApplyShadowTexSkip();
    return;
  }
  if (g_benchActiveMode.load() == 18) {
    g_frameHeapOn = on;
    return;
  }
  if (g_benchActiveMode.load() == 17) {
    g_pacerLowPowerOn = on;
    ApplyPacer();
    return;
  }
  if (g_benchActiveMode.load() == 16) {
    g_cbUploadSkipOn = on;
    return;
  }
  if (g_benchActiveMode.load() == 15) {
    g_costWeightsOn = on;
    return;
  }
  if (g_benchActiveMode.load() == 14) {
    g_cbSkipOn = on;
    ApplyCbSkip();
    g_triPlainOn = on;
    ApplyTriCounter();
    return;
  }
  if (g_benchActiveMode.load() == 13) {
    g_cbSkipOn = on;
    ApplyCbSkip();
    return;
  }
  if (g_benchActiveMode.load() == 12) {
    g_triPlainOn = on;
    ApplyTriCounter();
    return;
  }
  if (g_benchActiveMode.load() == 11) {
    BenchSetEngineOff(!on);
    return;
  }
  if (g_benchActiveMode.load() == 10) {
    g_texDedupeOn = on;
    ApplyTextureHook();
    return;
  }
  if (g_benchActiveMode.load() == 9) {
    g_allocSlabsOn = on;
    return;
  }
  if (g_benchActiveMode.load() == 8) {
    g_shadowTight = on;
    return;
  }
  if (g_benchActiveMode.load() == 7) {
    d3ds::g_hooksWanted = on;
    g_d3dMode = on ? 1 : 0;
    return;
  }
  if (g_benchActiveMode.load() == 5) {
    SetTimerResolution(on);
    return;
  }
  if (g_benchActiveMode.load() == 4) {
    if (on)
      threadtune::Apply();
    else
      threadtune::Restore();
    return;
  }
  static uint32_t defaultThreads = 0;
  void* scene = g_scene.load();
  if (!scene || g_sceneLayout.load() != 1) return;
  auto* p = reinterpret_cast<volatile uint32_t*>(static_cast<uint8_t*>(scene) + kSceneCollectThreadsMax);
  if (!defaultThreads) defaultThreads = *p;
  *p = on ? static_cast<uint32_t>(std::min(16, std::max(1, g_cfg.benchThreadsB))) : defaultThreads;
}

