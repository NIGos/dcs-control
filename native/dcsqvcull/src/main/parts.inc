// Engine parts: one row per engine optimization the module can install or switch on (a hook, a patch, a replaced
// slot, a changed DCS setting). The offline invariant tests (test/invariants_test.h) check against this table:
//   - every Config field is either read by a row's `configured` here or classified there as not a part (tuning of a
//     part, measurement, perception / visual, infrastructure), so a new switch cannot be added without a decision;
//   - DCS Control's "every Engine option off" ini leaves every row unconfigured (GitHub #28: two GPU shortcuts stayed on);
//   - with every row configured and the kill switch on (Alt+Shift+F11, g_engineOff), SetEngineOff(true) and the worker's
//     TickEngineParts leave every `requested` false.
// configured: the ini asks for the part (the condition its gating code uses, without the kill switch).
// requested:  what the module currently asks of the part: the flag, mask or mode its Apply / gating code set (a part
//             whose install failed or never ran reads false too). nullptr: not readable offline (see gate).
// gate:       kGateApplied: SetEngineOff / LoadConfig / TickEngineParts apply the kill switch to it;
//             kGateWorkerLoop: the worker passes the kill switch to its tick, whose state is not readable offline
//             (the test checks that call's text in worker.inc); kGateNone: not under the kill switch (research experiments, listed in the test).
// Nothing here is called by the module itself: it only reads the module's state.
constexpr int kGateApplied = 0, kGateWorkerLoop = 1, kGateNone = 2;
struct EnginePart {
  const char* name;
  const char* keys;
  bool (*configured)(const Config& c);
  bool (*requested)();
  int gate;
};
#define QV_PART(name, keys, cfg, req, gate) {name, keys, [](const Config& c) -> bool { return (cfg); }, req, gate}
#define QV_REQ(expr) [] { return static_cast<bool>(expr); }
const EnginePart kEngineParts[] = {
    QV_PART("model allocator slabs", "[Model] AllocSlabs", c.allocSlabs, QV_REQ(g_allocSlabsOn.load()), kGateApplied),
    QV_PART("big model pages", "[Model] BigModelPages", c.bigPages, QV_REQ(bigpages::g_wanted.load()), kGateApplied),
    QV_PART("frame heap slabs", "[Model] FrameHeapSlabs", c.frameHeapSlabs, QV_REQ(g_frameHeapOn.load()), kGateApplied),
    QV_PART("plain triangle counter", "[Model] PlainTriangleCounter", c.triPlain,
            QV_REQ(g_triPlainOn.load() || tricount::g_plain.load()), kGateApplied),
    QV_PART("task queue clock", "[Timing] TaskQueueClock", c.taskClock, QV_REQ(g_taskClockOn.load()), kGateApplied),
    QV_PART("streaming timer cache", "[Timing] ShaderTimeCache, CacheUs", c.shaderTimeCache && c.shaderTimeCacheUs > 0,
            QV_REQ(timercache::g_on.load() && timercache::g_orig), kGateApplied),  // g_on acts only once hooked
    QV_PART("texture stream dedupe", "[Texture] StreamDedupe", c.texDedupe, QV_REQ(g_texDedupeOn.load()), kGateApplied),
    QV_PART("effect constant buffer skip", "[Effects] SkipSameConstantBuffer", c.cbSkip, QV_REQ(g_cbSkipOn.load()),
            kGateApplied),
    QV_PART("effect constant upload skip", "[Effects] SkipSameConstantUpload", c.cbUploadSkip,
            QV_REQ(g_cbUploadSkipOn.load()), kGateApplied),
    QV_PART("culling cost weights", "[Scene] CostWeights", c.costWeights, QV_REQ(g_costWeightsOn.load()), kGateApplied),
    QV_PART("culling partition boost", "[Scene] PartitionBoost", c.partitionBoost, QV_REQ(g_partitionBoost.load()),
            kGateApplied),
    QV_PART("low power pacer", "[General] LowPowerPacer", c.pacerLowPower, QV_REQ(g_pacerLowPowerOn.load()),
            kGateApplied),
    QV_PART("shadow instancing", "[Model] ShadowInstancing", c.shadowInst, QV_REQ(shadowinst::g_on.load()), kGateApplied),
    QV_PART("shadow batching", "[Model] ShadowBatching", c.shadowBatch && c.shadowInst, QV_REQ(shadowbatch::g_on.load()),
            kGateApplied),
    QV_PART("shadow plan async", "[Model] ShadowPlanAsync", c.shadowPlanAsync && c.shadowBatch && c.shadowInst,
            QV_REQ(shadowbatch::g_on.load() && shadowbatch::g_async.load()), kGateApplied),
    QV_PART("shadow texture skip", "[Model] ShadowTextureSkip", c.shadowTexSkip, QV_REQ(g_shadowTexSkipOn.load()),
            kGateApplied),
    QV_PART("G-buffer batching", "[Model] GBufferBatching", c.gbufferBatch && c.shadowInst, QV_REQ(gbbatch::g_on.load()),
            kGateApplied),
    QV_PART("shadow recorder", "[Model] ShadowRecorder", c.shadowRecorder && !(c.gbufferBatch && c.shadowInst),
            QV_REQ(shrec::g_on.load()), kGateApplied),
    QV_PART("G-buffer recorder", "[Model] GBufferRecorder", c.gbufferRecorder && c.shadowInst && !c.gbufferBatch,
            QV_REQ(gbrec::g_on.load()), kGateApplied),
    QV_PART("parallel upload", "[Model] ParallelUpload", c.parUpload, QV_REQ(parupload::g_on.load()), kGateApplied),
    QV_PART("direct upload", "[Model] DirectUpload", c.directUpload, QV_REQ(directupload::g_on.load()), kGateApplied),
    QV_PART("split state filter", "[D3D] SplitFilter", c.splitFilter,
            QV_REQ(sfilt::g_attached.load() || sfilt::g_hooked.load() || sfilt::g_sitesPatched.load()), kGateApplied),
    QV_PART("srv tail trim", "[Model] SrvTailTrim", c.srvTailTrim, QV_REQ(srvtrim::g_patched.load()), kGateApplied),
    QV_PART("pass flush", "[Model] PassFlush", c.passFlush != 0, QV_REQ(pflush::g_mask.load() != 0), kGateApplied),
    QV_PART("fx apply skip", "[D3D] FxApplySkip", c.fxApplySkip != 0, QV_REQ(fxskip::g_mode.load() != 0 || fxskip::Hooked()),
            kGateApplied),
    QV_PART("hidden water skip", "[Water] SkipHiddenWater", c.waterSkip != 0, QV_REQ(w1::g_mode.load() != 0),
            kGateApplied),
    QV_PART("terrain prefetch fix", "[Prefetch] Mode", c.prefetchMode != 0, QV_REQ(pffix::g_state.load() == 1),
            kGateApplied),
    QV_PART("chain fast", "[D3D] ChainFast", c.chainFast != 0, QV_REQ(chainfast::g_mask.load() != 0), kGateApplied),
    QV_PART("shader fast", "[D3D] ShaderFast", c.shaderFast != 0, QV_REQ(sfast::g_mask.load() != 0), kGateApplied),
    QV_PART("early shaders", "[D3D] EarlyShaders", c.earlyShaders != 0, QV_REQ(eshd::g_mask.load() != 0), kGateApplied),
    QV_PART("shadow cascade cache", "[Shadows] CascadeCache", c.cascadeCache, QV_REQ(cascache::g_on.load()),
            kGateApplied),
    QV_PART("periphery LOD scale", "[Model] PeripheralLodScale", c.peripheralLodScale > 0 && c.peripheralLodScale < 100,
            QV_REQ(plod::g_pct.load() != 0 || plod::g_swapOn.load()), kGateApplied),
    QV_PART("focus twin", "[Model] FocusTwin", c.focusTwin == 1, QV_REQ(ftwin::g_active.load()), kGateApplied),
    // Its tick reads the NVIDIA driver profile (NvAPI) when a preset is set: not run offline. The test checks that the
    // worker passes the kill switch to it; dlssv::Tick's own kill path is covered by the R122 tests.
    QV_PART("DLSS per-view preset", "[DLSS] PeripheryPreset, IgnoreDriverPreset",
            dlssv::ParsePreset(c.dlssPeripheryPreset.c_str()) > 0, nullptr, kGateWorkerLoop),
    QV_PART("DLSS idle set release", "[DLSS] ReleaseIdleSets", c.dlssReleaseIdleSets, nullptr, kGateWorkerLoop),
    QV_PART("worker placement", "[Threads] WorkerPlacement", c.workerPlacement != 0, nullptr, kGateWorkerLoop),
    QV_PART("three views", "[Views] ThreeViews", c.threeViews, nullptr, kGateWorkerLoop),
    // Research experiments from before the kill switch (no app option; written 0 only in developer mode).
    QV_PART("peripheral exclusion", "[Cull] Enabled", c.enabled, QV_REQ(g_enabled.load()), kGateNone),
    QV_PART("collect threads max", "[Scene] CollectThreadsMax", c.collectThreadsMax > 0, nullptr, kGateNone),
    QV_PART("tight shadow casters", "[Shadow] TightCasters", c.shadowTight, QV_REQ(g_shadowTight.load()), kGateNone),
    QV_PART("D3D state filter (old)", "[D3D] Filter", c.d3dFilter, QV_REQ(g_d3dMode.load() != 0), kGateNone),
    QV_PART("fine timer resolution", "[Scene] FineTimerResolution", c.fineTimer, nullptr, kGateNone),
};
#undef QV_REQ
#undef QV_PART
constexpr int kEnginePartCount = static_cast<int>(sizeof(kEngineParts) / sizeof(kEngineParts[0]));

// The rows `c` configures / the module requests now (names, ", " separated; empty = none).
std::string EnginePartsConfigured(const Config& c, int gate = -1) {
  std::string s;
  for (const EnginePart& p : kEngineParts)
    if ((gate < 0 || p.gate == gate) && p.configured(c)) s += (s.empty() ? "" : ", ") + std::string(p.name);
  return s;
}
std::string EnginePartsRequested(int gate = -1) {
  std::string s;
  for (const EnginePart& p : kEngineParts)
    if ((gate < 0 || p.gate == gate) && p.requested && p.requested()) s += (s.empty() ? "" : ", ") + std::string(p.name);
  return s;
}
