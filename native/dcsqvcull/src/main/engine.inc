// Installed on first use; detached (DCS's own slot 5) while off. Its masks
// come from shadow_inst's compiles of the casters' keys, so shadow_inst
// collects them while the skip is on (until a key is compiled, its shaders
// keep every texture set).
void ApplyShadowTexSkip() {
  const bool on = g_shadowTexSkipOn.load();
  // Shadow verification compares a stock run (texture skip detached) with the
  // batched run (texture skip attached when configured on).
  shadowbatch::g_verifyStock = [](bool stock) {
    if (g_shadowTexSkipOn.load() && shadowtex::g_state.load() > 0) shadowtex::SetAttached(!stock);
  };
  if (on != shadowinst::g_onTex.load() || (on && shadowinst::g_state.load() == 0)) shadowinst::SetOnTex(on);
  if (on && !shadowtex::Install()) return;
  shadowtex::SetAttached(on);
}

// The kill switch flipped (hotkey, bench mode 11, the suite's A/B): every
// recorder job and re-record in flight belongs to the other state. A new
// frame-heap generation makes them stale (their workers stop before their
// next read of a renderable), and the scene-change mark contains a fault that
// still slips in per job (shrec::SceneChanging) instead of latching a recorder
// off for the session (issue #3: G-buffer recorder fault right after an
// Alt+Shift+F11 off/on in flight). The heap's reset hooks stay installed and
// keep counting while the kill switch is off; this only adds a step.
void OnEngineSwitch() {
  g_sceneChangeQpc.store(defrec::Qpc(), std::memory_order_release);
  g_frameHeapGen.fetch_add(1, std::memory_order_acq_rel);
}

// Applies the kill switch state to every measured optimization.
void SetEngineOff(bool off) {
  g_engineOff = off;
  OnEngineSwitch();
  // Off: the recorders stop first, so no job starts or arms while the rest is switched off (they only arm at a
  // render entry seen while active, so on they start at the next frame after everything below is back).
  if (off) {
    ApplyShadowRecorder();
    ApplyGBufferRecorder();
  }
  g_partitionBoost = g_cfg.partitionBoost && !off;
  g_allocSlabsOn = g_cfg.allocSlabs && !off;
  ApplyAllocSlabs();  // R144
  g_texDedupeOn = g_cfg.texDedupe && !off;
  g_triPlainOn = g_cfg.triPlain && !off;
  ApplyTriCounter();
  g_cbSkipOn = g_cfg.cbSkip && !off;
  ApplyCbSkip();
  g_costWeightsOn = g_cfg.costWeights && !off;
  ApplyCostWeights();  // R142
  g_cbUploadSkipOn = g_cfg.cbUploadSkip && !off;
  ApplyCbUpload();
  g_pacerLowPowerOn = g_cfg.pacerLowPower && !off;
  ApplyPacer();
  g_taskClockOn = g_cfg.taskClock && !off;
  ApplyTaskClock();
  g_frameHeapOn = g_cfg.frameHeapSlabs && !off;
  g_shadowTexSkipOn = g_cfg.shadowTexSkip && !off;
  ApplyShadowTexSkip();
  ApplyBigPages(g_cfg.bigPages && !off);
  ApplyParUpload(g_cfg.parUpload && !off);
  ApplyDirectUpload(g_cfg.directUpload && !off);
  ApplySplitFilter(g_cfg.splitFilter && !off);
  ApplyShadowInst();
  ApplyShadowBatch();
  ApplyGBufferBatch();
  ApplyShadowRecorder();
  ApplyGBufferRecorder();
  ApplyPassFlush();
  ApplySrvTailTrim(g_cfg.srvTailTrim && !off);
  ApplyPrefetch();
  // The texture hook only does work when dedupe is on: otherwise remove it.
  ApplyTextureHook();
  ApplyTimerConfig();
  // The switch takes a while (the prefetch fix's module scan): the containment window (2 s after the mark) counts
  // from its end, which covers the recorders' first frames back on.
  g_sceneChangeQpc.store(defrec::Qpc(), std::memory_order_release);
}

// Status word for DcsQvCull_Status (status_word.h), recomputed by the worker
// about once per second. Production optimizations = the features the app's
// ini turns on by default, each from its module's real state:
//   [Model] AllocSlabs, BigModelPages, FrameHeapSlabs, PlainTriangleCounter,
//   ShadowInstancing, ShadowBatching, ShadowPlanAsync, ShadowTextureSkip,
//   ShadowRecorder, GBufferRecorder; [Timing] TaskQueueClock, ShaderTimeCache;
//   [Texture] StreamDedupe; [Effects] SkipSameConstantBuffer;
//   [Scene] CostWeights, PartitionBoost; [D3D] SplitFilter; [Prefetch] Mode=skip
//   (18 features).
// Latched = off for the rest of the session (build or verify mismatch,
// contained exception, install failed with its DLL loaded).
uint64_t ComputeStatus() {
  const bool dx11 = GetModuleHandleW(L"dx11backend.dll") != nullptr;
  const bool ngModel = GetModuleHandleW(L"NGModel.dll") != nullptr;
  const uint32_t threadsDef = g_defaultThreadsMax.load();
  const bool batchLatched = shadowbatch::g_disabled.load() || shadowbatch::g_state.load() < 0;
  const bool batchActive = shadowbatch::g_on.load() && shadowbatch::g_state.load() == 1 && !batchLatched;
  // The recorders count as active only when they drew passes from command
  // lists since the last update: enabled but idle (scene layout not learned,
  // no quad frames) is not active. Only the worker calls this, about once
  // per second.
  static uint64_t shWork = 0, gbWork = 0;
  const uint64_t shNow = shrec::g_workPasses.load(std::memory_order_relaxed);
  const uint64_t gbNow = gbrec::g_workPasses.load(std::memory_order_relaxed);
  const bool shWorked = shNow != shWork, gbWorked = gbNow != gbWork;
  shWork = shNow;
  gbWork = gbNow;
  const qvstatus::Feature f[] = {
      {g_allocSlabsOn.load() && allocslab::g_state.load() == 2, allocslab::g_state.load() < 0, g_cfg.allocSlabs},
      {bigpages::g_wanted.load() && bigpages::g_state.load() == 1, bigpages::g_state.load() < 0, g_cfg.bigPages},
      {g_frameHeapOn.load() && frameheap::g_state.load() == 1, frameheap::g_state.load() < 0, g_cfg.frameHeapSlabs},
      // Latched from the configuration, not the kill-switch-gated flag: a
      // feature latched off this session stays latched under the kill switch.
      {g_triPlainOn.load() && tricount::g_plain.load(), g_cfg.triPlain && !tricount::g_site && ngModel,
       g_cfg.triPlain},
      {g_taskClockOn.load() && edtime::g_state.load() == 1, edtime::g_state.load() < 0, g_cfg.taskClock},
      {timercache::g_on.load() && timercache::g_orig, g_cfg.shaderTimeCache && !timercache::g_orig,
       g_cfg.shaderTimeCache},
      {g_texDedupeOn.load() && texbind::g_attached.load(), g_cfg.texDedupe && !texbind::g_orig && dx11,
       g_cfg.texDedupe},
      {g_cbSkipOn.load() && cbskip::g_attached.load(), g_cfg.cbSkip && !cbskip::g_orig && dx11, g_cfg.cbSkip},
      {g_costWeightsOn.load() && partw::g_orig, g_cfg.costWeights && !partw::g_orig, g_cfg.costWeights},
      {g_partitionBoost.load() && g_sceneLayout.load() == 1 && threadsDef >= 1 && threadsDef <= 32,
       threadsDef > 32 || g_sceneLayout.load() < 0, g_cfg.partitionBoost},
      {shadowinst::g_on.load() && shadowinst::g_state.load() == 1, shadowinst::g_state.load() == -1,
       g_cfg.shadowInst},
      {batchActive, batchLatched, g_cfg.shadowBatch && g_cfg.shadowInst},
      {batchActive && shadowbatch::g_async.load() && shadowbatch::g_workersUp.load(),
       g_cfg.shadowPlanAsync && batchLatched, g_cfg.shadowPlanAsync && g_cfg.shadowBatch && g_cfg.shadowInst},
      {g_shadowTexSkipOn.load() && shadowtex::g_state.load() == 1 && shadowtex::g_attached.load(),
       shadowtex::g_state.load() == -1, g_cfg.shadowTexSkip},  // -3 = installing (shadow_tex.h), not latched
      {shrec::g_on.load() && shrec::Ready() && shWorked, shrec::g_disabled.load() || shrec::g_state.load() < 0,
       g_cfg.shadowRecorder},
      {gbrec::g_on.load() && gbrec::Ready() && gbWorked, gbrec::g_disabled.load() || gbrec::g_state.load() < 0,
       g_cfg.gbufferRecorder},
      {sfilt::Live(), sfilt::g_disabled.load() || sfilt::g_state.load() < 0, g_cfg.splitFilter},
      {pffix::Active(), pffix::g_state.load() < 0, g_cfg.prefetchMode == pffix::kSkip},
  };
  static_assert(sizeof(f) / sizeof(f[0]) == sizeof(g_featLast) / sizeof(g_featLast[0]), "features line: 18 entries");
  memcpy(g_featLast, f, sizeof(f));
  const uint64_t w = qvstatus::Compose(f, sizeof(f) / sizeof(f[0]), true, g_engineOff.load());
  return qvstatus::WithExtras(w, g_warming.load(), static_cast<uint32_t>(g_govStage.load()),
                              static_cast<uint32_t>(g_verdictNow.load()), g_cfg.govEnabled);
}

// DcsQvCull_Status's word. R62: in PerceptionOnly kRunning | kPerceptionOnly (was 0, which the in-headset panel
// reads as "not running yet": a grey "Engine Opt.: starting" for the whole flight; nothing hid the line). A panel
// unaware of bit 38 now shows "Engine Opt.: OFF"; one aware of it leaves the line out (status_word.h).
uint64_t StatusWordNow() {
  // R65: bits 39 / 40 (cockpit mask export) in both modes; 0 unless [Perception] CockpitMaskExport=1 and committing.
  // R69: bit 41 (its depth companion) likewise. R70: bit 42 (its motion companion) likewise.
  return (g_perceptionOnly.load() ? (qvstatus::kRunning | qvstatus::kPerceptionOnly) : ComputeStatus()) |
         fixdepth::cmask::StatusBits();
}

// R42 colour modes: the ini's configuration (or the ModeKey's mode) and the sun to the pass; compile / create / log.
void ColorTick() {
  cpass::g_log = &Log;
  cpass::Cfg c = g_colorCfg;
  if (g_colorKeyMode.load() >= 0) c.mode = g_colorKeyMode.load();
  float y = 0;
  const bool sun = (c.mode != 0 || c.timing != 0 || vrfx::Wanted()) && nighttone::SunY(&y);  // R51: pupil estimate
  if (vrfx::g_wantGv.load()) {  // R51: DCS's G-EFFECT option (options.lua, read only; every 10 s)
    static ULONGLONG last = 0;
    static int logged = -2;
    const ULONGLONG now = GetTickCount64();
    if (!last || now - last >= 10000) {
      last = now;
      int v = -1;
      HANDLE h = CreateFileW((g_dir + L"..\\..\\Config\\options.lua").c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
      if (h != INVALID_HANDLE_VALUE) {
        std::string text(256 * 1024, '\0');
        DWORD n = 0;
        if (ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr)) text.resize(n);
        else text.clear();
        CloseHandle(h);
        v = vrfx::ParseGEffect(text);
      }
      vrfx::g_dcsGEffect = v;
      if (v != logged) {
        logged = v;
        static const char* const kName[4] = {"unknown (options.lua not read)", "none", "reduced", "realistic"};
        Log("vrfx: DCS's G-EFFECT option is %s: GVision mode %d (%s)", kName[v + 1],
            vrfx::EffectiveGvMode(vrfx::g_params.gvMode, v),
            vrfx::EffectiveGvMode(vrfx::g_params.gvMode, v) == 2 ? "full: ours darkens, tunnels and drains colour"
                                                                  : "complement: DCS darkens, ours adds the gaze tunnel and colour loss");
      }
    }
  }
  cpass::Tick(c, sun, y);
}

bool ToggleEngine() {
  if (g_perceptionOnly.load() || g_benchRunningFlag.load()) return false;
  bool off = !g_engineOff.load();
  const uint64_t w0 = qvpatch::g_writes.load(), h0 = qvpatch::g_heldUsTotal.load();
  qvpatch::g_heldUsMax = 0;
  const uint64_t t0 = qvpatch::NowUs();
  SetEngineOff(off);
  const uint64_t t1 = qvpatch::NowUs();
  Log("hotkey: engine optimizations %s", off ? "OFF" : "ON");
  // R90: what the switch cost (R82: 78 ms p50 render-thread spike per flag with the Toolhelp path).
  Log("hotkey: switch took %.1f ms on the worker; %llu code-site writes, other threads held %.2f ms in total "
      "(%.2f ms max, %u threads, PatchMode %d)",
      (t1 - t0) / 1000.0, static_cast<unsigned long long>(qvpatch::g_writes.load() - w0),
      (qvpatch::g_heldUsTotal.load() - h0) / 1000.0, qvpatch::g_heldUsMax.load() / 1000.0,
      qvpatch::g_lastThreads.load(), qvpatch::g_mode.load());
  PerfOnToggle(off);  // R30 L5: the 10 s before now, the 10 s after in 12 s
  return true;
}

// True when exactly the configured modifiers are held (either side).
bool ModifiersMatch(int mods) {
  int held = (KeyDown(VK_CONTROL) ? 1 : 0) | (KeyDown(VK_MENU) ? 2 : 0) | (KeyDown(VK_SHIFT) ? 4 : 0);
  return held == mods;
}

// The worker's keys this poll (held, not edges). PerceptionOnly: only the
// cockpit ipd's own toggle ([Perception] CockpitTrueIpdToggle); the engine
// toggle and every Ctrl+Alt key are inert whatever the ini says.
struct HeldKeys {
  bool toggle = false, cockpit = false, f8 = false, f9 = false, f10 = false, f11 = false, f12 = false, up = false,
       down = false;
};
using KeyDownFn = bool (*)(int vk);
HeldKeys ReadKeys(const Config& c, bool perceptionOnly, KeyDownFn down) {
  HeldKeys k;
  auto mods = [&](int m) {
    return ((down(VK_CONTROL) ? 1 : 0) | (down(VK_MENU) ? 2 : 0) | (down(VK_SHIFT) ? 4 : 0)) == m;
  };
  k.cockpit = c.cockpitTrueIpd == 1 && c.cockpitTrueIpdToggleVk > 0 && down(c.cockpitTrueIpdToggleVk) &&
              mods(c.cockpitTrueIpdToggleMods);
  if (perceptionOnly) return k;
  const bool chord = down(VK_CONTROL) && down(VK_MENU) && !down(VK_SHIFT);  // exactly Ctrl+Alt
  const bool dev = c.developerKeys;
  k.toggle = c.toggleVk > 0 && down(c.toggleVk) && mods(c.toggleMods);
  k.f8 = dev && chord && down(VK_F8);
  k.f9 = dev && chord && down(VK_F9);
  k.f10 = dev && chord && down(VK_F10);
  k.f11 = chord && down(VK_F11);
  k.f12 = dev && chord && down(VK_F12);
  k.up = dev && chord && down(VK_PRIOR);
  k.down = dev && chord && down(VK_NEXT);
  return k;
}

