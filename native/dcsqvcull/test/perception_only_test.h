// Offline tests for [General] PerceptionOnly (main.cpp LoadConfig, HookCollect,
// ReadKeys, StatusWordNow, ConfigurePerception / TickFixationDepth;
// perception_device.h; perf_mon.h GovTick). PerceptionOnly=1: no engine
// module installs, the perception configuration reaches its modules, the
// status word is 0, the engine toggle and the Ctrl+Alt keys are inert, the
// dual camera status works, the collect hook only observes (the eye
// separation for option 3's measured-separation record pick still arrives).
// PerceptionOnly=0: as before.
// No real Local\ mapping is opened (the fixation depth install stops at the
// missing GraphicsCore.dll, checked first; nothing else here maps one).
#pragma once

namespace potest {

// Every engine module's install / attach state.
struct Snap {
  int allocslab, bigpages, frameheap, edtime, shadowinst, shadowbatch, gbbatch, shadowtex, shrec, gbrec, sfilt,
      srvtrim, pffix, directupload, parupload, pacer, plodState;
  bool shadowinstGb, timerOn, texAttached, cbSkipAttached, cbUploadAttached, shadowtexAttached, plodSwap;
  const void *timerOrig, *partwOrig, *drawSlot;
  int govStage;
  bool operator==(const Snap& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};
Snap Take() {
  Snap s;
  memset(&s, 0, sizeof(s));
  s.allocslab = allocslab::g_state.load();
  s.bigpages = bigpages::g_state.load();
  s.frameheap = frameheap::g_state.load();
  s.edtime = edtime::g_state.load();
  s.shadowinst = shadowinst::g_state.load();
  s.shadowbatch = shadowbatch::g_state.load();
  s.gbbatch = gbbatch::g_state.load();
  s.shadowtex = shadowtex::g_state.load();
  s.shrec = shrec::g_state.load();
  s.gbrec = gbrec::g_state.load();
  s.sfilt = sfilt::g_state.load();
  s.srvtrim = srvtrim::g_state.load();
  s.pffix = pffix::g_state.load();
  s.directupload = directupload::g_state.load();
  s.parupload = parupload::g_state.load();
  s.pacer = pacer::g_state.load();
  s.plodState = plod::g_state;
  s.shadowinstGb = shadowinst::g_gbInstalled.load();
  s.timerOn = timercache::g_on.load();
  s.texAttached = texbind::g_attached.load();
  s.cbSkipAttached = cbskip::g_attached.load();
  s.cbUploadAttached = cbupload::g_attached.load();
  s.shadowtexAttached = shadowtex::g_attached.load();
  s.plodSwap = plod::g_swapOn.load();
  s.timerOrig = reinterpret_cast<const void*>(timercache::g_orig);
  s.partwOrig = reinterpret_cast<const void*>(partw::g_orig);
  s.drawSlot = d3ds::g_rendererDrawSlot;
  s.govStage = g_govStage.load();
  return s;
}

void WriteIni(const char* text) {
  FILE* f = nullptr;
  _wfopen_s(&f, L"./DcsQvCull.ini", L"w");
  fputs(text, f);
  fclose(f);
}

// Every engine key on, the perception keys of option 3 with ownship stage 2.
const char kEngineAndPerception[] =
    "[Cull]\nEnabled=1\nDebugHole=1\n"
    "[Scene]\nPartitionBoost=1\nCostWeights=1\nCollectThreadsMax=8\nFineTimerResolution=1\n"
    "[Model]\nAllocSlabs=1\nBigModelPages=1\nFrameHeapSlabs=1\nShadowInstancing=1\nShadowBatching=1\n"
    "ShadowPlanAsync=1\nShadowRecorder=1\nGBufferRecorder=1\nGBufferBatching=1\nShadowTextureSkip=1\n"
    "ParallelUpload=1\nDirectUpload=1\nPlainTriangleCounter=1\nSrvTailTrim=1\nPassFlush=0xff\n"
    "PeripheralLodScale=50\n"
    "[Texture]\nStreamDedupe=1\n[Effects]\nSkipSameConstantBuffer=1\nSkipSameConstantUpload=1\n"
    "[Timing]\nShaderTimeCache=1\nTaskQueueClock=1\n[D3D]\nSplitFilter=1\nFilter=1\nMeter=1\n"
    "[Shadow]\nTightCasters=1\n[Prefetch]\nMode=skip\n[Governor]\nEnabled=1\n"
    "[Hotkeys]\nToggle=122:6\nDeveloperKeys=1\n[Bench]\nAutoStartSec=5\n"
    "[Suite]\nBenchEngine=1\nGBufferRecVerifyLive=1\nHoldYawDeg=90\nGpuPassTiming=1\nGpuDedup=4\nProfile=1\n"
    "[Perception]\nFixationDepth=1\nDepthGrid=1\nFarDepthExport=1\nCockpitTrueIpd=1\nCockpitTrueIpdOwnship=2\n"
    "CockpitTrueIpdHelmet=1\nCockpitTrueIpdToggle=118:6\nSceneCutReset=1\n";

bool g_ngFresh = false;  // NGModel mapped by this test (no earlier test patched its image)
std::set<int> g_held;
bool FakeDown(int vk) { return g_held.count(vk) != 0; }
HeldKeys Press(std::initializer_list<int> keys, const Config& c, bool po) {
  g_held = std::set<int>(keys);
  const HeldKeys k = ReadKeys(c, po, &FakeDown);
  g_held.clear();
  return k;
}
bool AnyEngineKey(const HeldKeys& k) {
  return k.toggle || k.f8 || k.f9 || k.f10 || k.f11 || k.f12 || k.up || k.down;
}

// The collect hook's original: what DCS received.
uint32_t g_seenFlags = 0;
const uint8_t* g_seenVol[5] = {};
void __fastcall FakeCollect(void*, uint32_t count, uint8_t* infos, uint8_t*, void*, uint32_t flags) {
  g_seenFlags = flags;
  for (uint32_t i = 0; i < count && i < 5; ++i)
    g_seenVol[i] = *reinterpret_cast<uint8_t**>(infos + i * kCollectionInfoStride + kCiClipVolume);
}

// DX11Renderer::draw stand-in for perception_device.h's capture.
int g_draws = 0;
int g_drawArg = 0;
void __fastcall FakeDraw(void*, int a, void*, int, int, int, int, const char*) {
  ++g_draws;
  g_drawArg = a;
}

void Config_() {
  const Snap before = Take();
  WriteIni((std::string("[General]\nPerceptionOnly=1\nDiagnostics=1\nLowPowerPacer=1\n") + kEngineAndPerception).c_str());
  LoadConfig(true);
  const Config& c = g_cfg;
  Check(g_perceptionOnly.load() && c.perceptionOnly, "perception only: [General] PerceptionOnly=1 latched by the initial read");
  Check(!c.enabled && !c.debugHole && !c.partitionBoost && !c.costWeights && c.collectThreadsMax == 0 && !c.fineTimer &&
            !c.allocSlabs && !c.bigPages && !c.frameHeapSlabs && !c.shadowInst && !c.shadowBatch && !c.shadowPlanAsync &&
            !c.shadowRecorder && !c.gbufferRecorder && !c.gbufferBatch && !c.shadowTexSkip && !c.parUpload &&
            !c.directUpload && !c.triPlain && !c.srvTailTrim && c.passFlush == 0 && c.peripheralLodScale == 0 &&
            !c.texDedupe && !c.cbSkip && !c.cbUploadSkip && !c.shaderTimeCache && !c.taskClock && !c.splitFilter &&
            !c.d3dFilter && !c.d3dMeter && !c.shadowTight && c.prefetchMode == 0 && !c.pacerLowPower && !c.diagnostics,
        "perception only: every engine optimization and diagnostics hook of the ini is off in the applied config");
  Check(!c.govEnabled && c.toggleVk == 0 && !c.developerKeys && c.benchAutoStartSec == 0 && !c.suiteBenchEngine &&
            !c.suiteGBufferRecVerifyLive && c.holdYawDeg == -1 && !c.suiteGpuPassTiming && c.suiteGpuDedup == 0 &&
            !c.suiteProfile,
        "perception only: governor, engine key, developer keys, benchmark auto start and suite keys off");
  Check(c.fixationDepth && c.depthGrid && c.farDepthExport && c.cockpitTrueIpd == 1 && c.cockpitTrueIpdOwnship == 2 &&
            c.cockpitTrueIpdHelmet && c.sceneCutReset && c.cockpitTrueIpdToggleVk == 118 &&
            c.cockpitTrueIpdToggleMods == 6,
        "perception only: the [Perception] keys (fixation depth, DepthGrid, far depth, option 3, ownship) are kept");
  Check(!g_enabled.load() && !g_debugHole.load() && !g_partitionBoost.load() && !g_allocSlabsOn.load() &&
            !g_texDedupeOn.load() && !g_triPlainOn.load() && !g_cbSkipOn.load() && !g_costWeightsOn.load() &&
            !g_cbUploadSkipOn.load() && !g_pacerLowPowerOn.load() && !g_taskClockOn.load() && !g_frameHeapOn.load() &&
            !g_shadowTexSkipOn.load() && !g_shadowTight.load() && g_d3dMode.load() == 0 && !g_engineOff.load(),
        "perception only: every engine runtime flag off");
  Check(Take() == before, "perception only: the config read installs or attaches no engine module");
  // A re-read (the worker's each second) keeps it; switching the key off takes effect at the next start.
  Sleep(20);
  WriteIni((std::string("[General]\nPerceptionOnly=0\n") + kEngineAndPerception).c_str());
  {
    HANDLE h = CreateFileW(L"./DcsQvCull.ini", FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    FILETIME t = g_iniTime;
    reinterpret_cast<ULARGE_INTEGER*>(&t)->QuadPart += 10000000;
    SetFileTime(h, nullptr, nullptr, &t);
    CloseHandle(h);
  }
  LoadConfig(false);
  Check(g_perceptionOnly.load() && g_cfg.perceptionOnly && !g_cfg.shadowInst && !g_partitionBoost.load() &&
            Take() == before,
        "perception only: PerceptionOnly=0 in a later read is not applied before the next start (nothing installed)");
  // Back to PerceptionOnly=1 for the next steps.
  WriteIni((std::string("[General]\nPerceptionOnly=1\n") + kEngineAndPerception).c_str());
  LoadConfig(true);
}

void StatusAndKeys() {
  const Config& c = g_cfg;
  // R62 e1: running + PerceptionOnly and nothing else (was 0 = "Engine Opt.: starting" on the panel all flight).
  Check(StatusWordNow() == (qvstatus::kRunning | qvstatus::kPerceptionOnly) && qvstatus::PerceptionOnly(StatusWordNow()) &&
            !qvstatus::Optimizing(StatusWordNow()) && qvstatus::ConfiguredCount(StatusWordNow()) == 0,
        "perception only: DcsQvCull_Status word is running + PerceptionOnly only (panel: not 'starting')");
  {
    std::atomic<uint64_t> word{0x55};
    g_statusSink.Attach(&word);
    g_statusSink.Publish(StatusWordNow());
    Check(word.load() == (qvstatus::kRunning | qvstatus::kPerceptionOnly),
          "perception only: the worker's status publish writes running + PerceptionOnly");
    g_statusSink.Detach();
  }
  // Keys: the engine toggle (Alt+Shift+F11) and every Ctrl+Alt key inert; the cockpit ipd toggle live.
  bool anyEngine = false;
  anyEngine |= AnyEngineKey(Press({VK_MENU, VK_SHIFT, VK_F11}, c, true));
  for (int vk : {VK_F8, VK_F9, VK_F10, VK_F11, VK_F12, VK_PRIOR, VK_NEXT})
    anyEngine |= AnyEngineKey(Press({VK_CONTROL, VK_MENU, vk}, c, true));
  Config dev = c;  // even with the engine keys forced back on in the config
  dev.toggleVk = 122;
  dev.toggleMods = 6;
  dev.developerKeys = true;
  anyEngine |= AnyEngineKey(Press({VK_MENU, VK_SHIFT, VK_F11}, dev, true));
  for (int vk : {VK_F8, VK_F9, VK_F10, VK_F11, VK_F12, VK_PRIOR, VK_NEXT})
    anyEngine |= AnyEngineKey(Press({VK_CONTROL, VK_MENU, vk}, dev, true));
  Check(!anyEngine, "perception only: the engine toggle and every Ctrl+Alt key are inert, whatever the ini");
  Check(Press({VK_MENU, VK_SHIFT, VK_F7}, c, true).cockpit && !Press({VK_MENU, VK_F7}, c, true).cockpit,
        "perception only: the cockpit ipd toggle (Alt+Shift+F7) still works");
  // The same keys with PerceptionOnly=0 (ReadKeys as the worker had it).
  const HeldKeys t = Press({VK_MENU, VK_SHIFT, VK_F11}, dev, false);
  const HeldKeys f11 = Press({VK_CONTROL, VK_MENU, VK_F11}, dev, false);
  const HeldKeys f9 = Press({VK_CONTROL, VK_MENU, VK_F9}, dev, false);
  const HeldKeys up = Press({VK_CONTROL, VK_MENU, VK_PRIOR}, dev, false);
  Config nodev = dev;
  nodev.developerKeys = false;
  const HeldKeys f9nd = Press({VK_CONTROL, VK_MENU, VK_F9}, nodev, false);
  const HeldKeys f11nd = Press({VK_CONTROL, VK_MENU, VK_F11}, nodev, false);
  Check(t.toggle && !t.f11 && f11.f11 && !f11.toggle && f9.f9 && up.up && !f9nd.f9 && f11nd.f11 &&
            !Press({VK_CONTROL, VK_MENU, VK_SHIFT, VK_F11}, dev, false).f11,
        "PerceptionOnly=0: engine toggle, Ctrl+Alt+F11 (always) and the developer keys (DeveloperKeys=1) as before");
  // The toggle and the benchmark refuse.
  Check(!ToggleEngine() && !g_engineOff.load(), "perception only: the engine toggle is refused");
  StartBench();
  Check(!g_benchRunningFlag.load(), "perception only: no benchmark or suite starts (Ctrl+Alt+F11, run_suite.flag)");
  // Governor: no action (with PerceptionOnly=0 and the governor off this tick would SetStage(0)).
  {
    const int st0 = g_govStage.load();
    g_govStage = 2;
    perfmon::Sec s;
    s.t = 1000.0;
    perfmon::GovTick(s);
    Check(g_govStage.load() == 2, "perception only: the governor tick takes no action");
    g_govStage = st0;
  }
}

void Collect() {
  // Fake L/R periphery + L/R focus (64 mm apart) and a pass sharing the left periphery's volume.
  alignas(64) static uint8_t vols[4][0x700];
  const Vec3 eye{1000.0, 50.0, -2000.0}, fwd{0, 0, 1}, up{0, 1, 0};
  const Vec3 eyeR = eye + Vec3{0.064, 0, 0};
  MakeFrustum(vols[0], eye, fwd, up, -1.2, 1.0, -1.1, 1.0, 0.05, 20000);
  MakeFrustum(vols[1], eyeR, fwd, up, -1.0, 1.2, -1.1, 1.0, 0.05, 20000);
  MakeFrustum(vols[2], eye, fwd, up, -0.3, 0.3, -0.28, 0.3, 0.05, 20000);
  MakeFrustum(vols[3], eyeR, fwd, up, -0.3, 0.3, -0.28, 0.3, 0.05, 20000);
  static uint8_t infos[5 * kCollectionInfoStride];
  memset(infos, 0, sizeof(infos));
  for (int i = 0; i < 4; ++i)
    *reinterpret_cast<uint8_t**>(infos + i * kCollectionInfoStride + kCiClipVolume) = vols[i];
  *reinterpret_cast<uint16_t*>(infos + 4 * kCollectionInfoStride) = 8;
  *reinterpret_cast<uint8_t**>(infos + 4 * kCollectionInfoStride + kCiClipVolume) = vols[0];
  alignas(16) static uint8_t sceneObj[0x200];
  memset(sceneObj, 0, sizeof(sceneObj));
  *reinterpret_cast<uint32_t*>(sceneObj + kSceneCollectThreadsMax) = 6;
  void* self = sceneObj + 8;

  const CollectFn orig0 = g_origCollect;
  void* const scene0 = g_scene.load();
  const uint32_t def0 = g_defaultThreadsMax.load();
  const bool ck0 = fixdepth::ckipd::g_on.load();
  const double sep0 = fixdepth::ckipd::g_cullSep.load();
  const uint64_t seq0 = fixdepth::ckipd::g_cullSeq.load();
  g_origCollect = &FakeCollect;
  g_defaultThreadsMax = 0;
  fixdepth::ckipd::g_cullSep = 0.0;
  // Stray engine flags (as if a key or a bug had set them): PerceptionOnly must still not patch.
  g_enabled = true;
  g_debugHole = true;
  g_partitionBoost = true;
  g_shadowTight = true;
  fixdepth::ckipd::g_on = true;  // option 3 running: the hook notes the eye separation
  HookCollect(self, 5, infos, nullptr, nullptr, 3);
  bool same = true;
  for (int i = 0; i < 4; ++i) same = same && g_seenVol[i] == vols[i];
  same = same && g_seenVol[4] == vols[0];
  Check(same && g_seenFlags == 3 && *reinterpret_cast<uint32_t*>(sceneObj + kSceneCollectThreadsMax) == 6 &&
            g_defaultThreadsMax.load() == 0,
        "perception only: the collect hook passes DCS's volumes, flags and thread limit through unchanged");
  const double sep = fixdepth::ckipd::g_cullSep.load();
  Check(fixdepth::ckipd::g_cullSeq.load() == seq0 + 1 && std::fabs(sep - 0.064) < 1e-6,
        "perception only: the collect hook still measures the eye separation (option 3's measured-separation pick)");
  // PerceptionOnly=0 (same call): the stray flags act as before (volumes swapped, boost raises the limit).
  // The engine modules the hook would drive are held off for this call (fake scene object).
  const int as0 = allocslab::g_state.load(), du0 = directupload::g_state.load();
  auto* const pw0 = partw::g_orig;
  const bool fl0 = g_faulted.load();
  allocslab::g_state = 0;
  directupload::g_state = 0;
  partw::g_orig = nullptr;
  g_faulted = false;
  g_perceptionOnly = false;
  const int sl0 = g_sceneLayout.load();
  g_sceneLayout = 1;  // R63 U2: a Scene.dll whose collect code was verified
  HookCollect(self, 5, infos, nullptr, nullptr, 3);
  g_sceneLayout = sl0;
  allocslab::g_state = as0;
  directupload::g_state = du0;
  partw::g_orig = pw0;
  g_faulted = fl0;
  const bool patchedBefore = g_seenVol[0] != vols[0] && g_seenFlags == 7 &&
                             *reinterpret_cast<uint32_t*>(sceneObj + kSceneCollectThreadsMax) == 8;
  Check(patchedBefore && *reinterpret_cast<uint8_t**>(infos + kCiClipVolume) == vols[0] &&
            fixdepth::ckipd::g_cullSeq.load() == seq0 + 2,
        "PerceptionOnly=0: the collect hook culls and boosts as before (volumes restored after the call)");
  g_perceptionOnly = true;
  fixdepth::ckipd::g_on = ck0;
  fixdepth::ckipd::g_cullSep = sep0;
  g_enabled = false;
  g_debugHole = false;
  g_partitionBoost = false;
  g_shadowTight = false;
  g_defaultThreadsMax = def0;
  g_scene = scene0;
  g_origCollect = orig0;
}

void Perception() {
  namespace ck = fixdepth::ckipd;
  const Snap before = Take();
  // The worker's per-second perception steps with the PerceptionOnly config.
  ConfigurePerception();
  Check(ck::Wanted() && ck::g_cfgMode.load() == 1 && ck::g_cfgHelmet.load() && ck::g_cfgOwnship.load() == 1,
        "perception only: option 3 configured (CockpitTrueIpd=1, helmet), ownship stage 2 applied as stage 1");
  Check(!gbrec::Active(), "perception only: no G-buffer recorder, so its stage-2 hand-over can never engage");
  // TickFixationDepth installs the depth features. Offline it stops at the missing GraphicsCore.dll
  // (pass_timing.h) before any mapping is opened: checked first, else skipped. A fake execute hook an
  // earlier test left is held off for the call (else the install would go on to open the real mappings).
  const auto ptOrig0 = ptiming::g_orig;
  if (!GetModuleHandleW(L"GraphicsCore.dll") && !fixdepth::g_installed.load()) {
    ptiming::g_orig = nullptr;
    TickFixationDepth();
    ptiming::g_orig = ptOrig0;
    Check(fixdepth::g_coneOn.load() && fixdepth::g_gridOn.load() && fixdepth::fdexport::g_on.load() &&
              !fixdepth::g_installed.load(),
          "perception only: fixation depth, DepthGrid and the far depth export reach their module (install retried "
          "until GraphicsCore loads)");
  } else {
    printf("SKIP perception only: fixation depth tick (GraphicsCore loaded or fixation depth installed)\n");
  }
  // Dual camera status: reported (configured bit) and published.
  const uint64_t dc = ck::DualCameraStatusNow();
  {
    std::atomic<uint64_t> word{0};
    g_dualStatusSink.Attach(&word);
    g_dualStatusSink.Publish(ck::DualCameraStatusNow());
    Check(qvstatus::DcConfigured(dc) && qvstatus::DcConfigured(word.load()) && !qvstatus::DcObserve(dc),
          "perception only: DcsQvCull_DualCameraStatus keeps working (configured, published)");
    g_dualStatusSink.Detach();
  }
  // The perception prerequisites against the DCS binaries the earlier tests mapped (not run: they wait).
  {
    auto* dx = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
    auto* ng = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
    const DcsQvLoaderApi* api0 = g_api;
    g_api = nullptr;  // HookSlot / UnhookSlot write the mapped image's slot directly
    if (!dx) {
      Check(!pdev::Install() && pdev::g_state.load() == 0,
            "perception only: the device capture waits for dx11backend (retried each second)");
    } else {
      auto** vt = reinterpret_cast<void**>(GetProcAddress(reinterpret_cast<HMODULE>(dx), pdev::kRendererVtbl));
      void* draw = reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(dx), pdev::kRendererDraw));
      const bool hooked = pdev::Install() && pdev::g_state.load() == 1 && pdev::g_slot &&
                          *pdev::g_slot == reinterpret_cast<void*>(&pdev::Hook) && vt && draw &&
                          pdev::g_slot >= vt && pdev::g_slot < vt + 256;
      pdev::Uninstall();
      Check(hooked && *pdev::g_slot == draw && pdev::g_state.load() == 0,
            "perception only: DX11Renderer::draw found in DCS's dx11backend vtable, hooked, and put back at stop");
      pdev::g_slot = nullptr;
      pdev::g_orig = nullptr;
    }
    g_api = api0;
    uint8_t* const* const gl0 = gbrec::g_env.globals;
    gbrec::g_env.globals = nullptr;
    g_poGlobals = 0;
    PerceptionGlobalsTick();
    if (!ng) {
      Check(g_poGlobals == 0 && !gbrec::g_env.globals,
            "perception only: the NGModel globals wait for NGModel (retried each second)");
    } else {
      // Freshly mapped here (QV_PO_ONLY): DCS's own bytes, so it must resolve. After the other tests the
      // mapped image carries their code patches: refused then (logged), never a wrong pointer.
      shadowtex::Addrs a;
      const bool set = g_poGlobals == 1 && shadowtex::ResolveAddrs(a) &&
                       gbrec::g_env.globals == reinterpret_cast<uint8_t* const*>(ng + a.globals);
      if (g_ngFresh)
        Check(set, "perception only: the NGModel globals for helmet / ownship matching resolved from DCS's NGModel");
      else
        Check(set || (g_poGlobals == -1 && !gbrec::g_env.globals),
              "perception only: the NGModel globals resolved, or refused on a patched NGModel image (never a "
              "wrong pointer)");
    }
    gbrec::g_env.globals = gl0;
    g_poGlobals = 0;
  }
  Check(Take() == before, "perception only: the perception steps install or attach no engine module");
  // Reset the perception modules to off (the next tests and the suite).
  Config off;
  const Config keep = g_cfg;
  g_cfg = off;
  ConfigurePerception();
  if (!fixdepth::g_installed.load()) {
    ptiming::g_orig = nullptr;
    TickFixationDepth();
    ptiming::g_orig = ptOrig0;
  }
  g_cfg = keep;
  Check(!ck::Wanted() && !fixdepth::g_coneOn.load() && !fixdepth::fdexport::g_on.load(),
        "perception only: the perception modules back off with the config");
}

void Device() {
  // A real immediate context passes the check; a deferred one, a non-d3d11 object and null do not.
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &imm))) {
    printf("SKIP perception device: no WARP device\n");
    return;
  }
  ID3D11DeviceContext* def = nullptr;
  dev->CreateDeferredContext(0, &def);
  struct NotAContext {
    virtual void A() {}
    void* pad[8] = {};
  } other;
  alignas(8) static uint8_t junk[64] = {};
  Check(pdev::CheckContextSeh(imm) == imm && pdev::CheckContextSeh(def) == nullptr &&
            pdev::CheckContextSeh(&other) == nullptr && pdev::CheckContextSeh(junk) == nullptr &&
            pdev::CheckContextSeh(nullptr) == nullptr,
        "perception device: only d3d11's immediate context of its own device is accepted");
  // The one-shot capture: a fake renderer with the context at +0x30, a fake vtable slot.
  alignas(16) uint8_t renderer[0x80] = {};
  *reinterpret_cast<void**>(renderer + pdev::kRendererContext) = imm;
  void* slot[1] = {reinterpret_cast<void*>(&pdev::Hook)};
  const DcsQvLoaderApi* api0 = g_api;
  g_api = nullptr;  // HookSlot / UnhookSlot write the slot directly
  pdev::g_orig = &FakeDraw;
  pdev::g_slot = slot;
  pdev::g_taken = false;
  pdev::g_tries = 0;
  pdev::g_state = 1;
  g_draws = 0;
  pdev::Hook(renderer, 42, nullptr, 0, 0, 0, 0, nullptr);
  ID3D11Device* got = pdev::Device();
  Check(pdev::g_state.load() == 2 && pdev::g_ctx.load() == imm && got == dev && slot[0] == reinterpret_cast<void*>(&FakeDraw) &&
            g_draws == 1 && g_drawArg == 42,
        "perception device: the first draw learns DCS's context, forwards the call unchanged and restores the slot");
  if (got) got->Release();
  // A renderer whose +0x30 is not a context: the slot stays until kTries draws, then is restored; no device.
  alignas(16) uint8_t bad[0x80] = {};
  *reinterpret_cast<void**>(bad + pdev::kRendererContext) = def;
  pdev::g_ctx = nullptr;
  slot[0] = reinterpret_cast<void*>(&pdev::Hook);
  pdev::g_taken = false;
  pdev::g_tries = 0;
  pdev::g_state = 1;
  g_draws = 0;
  for (int i = 0; i < pdev::kTries - 1; ++i) pdev::Hook(bad, 1, nullptr, 0, 0, 0, 0, nullptr);
  const bool waiting = pdev::g_state.load() == 1 && slot[0] == reinterpret_cast<void*>(&pdev::Hook);
  pdev::Hook(bad, 1, nullptr, 0, 0, 0, 0, nullptr);
  Check(waiting && pdev::g_state.load() == -1 && slot[0] == reinterpret_cast<void*>(&FakeDraw) &&
            g_draws == pdev::kTries && !pdev::Device(),
        "perception device: no context after its tries: slot restored, every draw forwarded, no device");
  // Uninstall before the first draw restores the slot.
  slot[0] = reinterpret_cast<void*>(&pdev::Hook);
  pdev::g_state = 1;
  pdev::g_taken = false;
  pdev::Uninstall();
  Check(slot[0] == reinterpret_cast<void*>(&FakeDraw) && pdev::g_state.load() == 0,
        "perception device: payload stop before the first draw puts DCS's draw back");
  pdev::g_state = 0;
  pdev::g_ctx = nullptr;
  pdev::g_slot = nullptr;
  pdev::g_orig = nullptr;
  pdev::g_taken = false;
  pdev::g_tries = 0;
  g_api = api0;
  if (def) def->Release();
  imm->Release();
  dev->Release();
}

// PerceptionOnly=0 is unchanged: the engine keys apply as before.
void Unchanged() {
  WriteIni("[Timing]\nShaderTimeCache=1\nCacheUs=1000\n[Scene]\nPartitionBoost=1\n[Hotkeys]\nToggle=122:6\n"
           "[Perception]\nCockpitTrueIpd=1\nCockpitTrueIpdOwnship=2\n");
  LoadConfig(true);
  Check(!g_perceptionOnly.load() && !g_cfg.perceptionOnly && g_partitionBoost.load() && g_cfg.shadowInst &&
            g_cfg.bigPages && g_cfg.shaderTimeCache && g_cfg.govEnabled && g_cfg.toggleVk == 122 &&
            g_cfg.cockpitTrueIpdOwnship == 2 && OwnshipMode(2, false) == 2 && OwnshipMode(1, true) == 1 &&
            OwnshipMode(0, true) == 0,
        "PerceptionOnly=0 (absent): engine config, boost and ownship stage 2 as before");
  Check(ToggleEngine() && g_engineOff.load() && !g_partitionBoost.load() && ToggleEngine() && !g_engineOff.load() &&
            g_partitionBoost.load(),
        "PerceptionOnly=0: the engine toggle works as before");
  const uint64_t w = StatusWordNow();
  Check(qvstatus::Running(w) && !qvstatus::PerceptionOnly(w),
        "PerceptionOnly=0: the status word reports running as before (no PerceptionOnly bit)");
  g_partitionBoost = false;
}

void Run() {
  // DCS's NGModel / dx11backend (when installed), mapped by this test when no earlier one did.
  g_ngFresh = !GetModuleHandleW(L"NGModel.dll");
  if (dcsinstall::Found()) {
    dcsinstall::LoadBinary(L"NGModel.dll");
    dcsinstall::LoadBinary(L"dx11backend.dll");
  }
  g_ngFresh = g_ngFresh && GetModuleHandleW(L"NGModel.dll");
  const Config saved = g_cfg;
  const std::wstring dir0 = g_dir;
  const bool po0 = g_perceptionOnly.load();
  g_dir = L"./";
  Config_();
  {
    // R48: the engine defaults that are on (ChainFast=7, ShaderFast=3, WorkerPlacement=2) are off in PerceptionOnly.
    Config full;
    const Config p = PerceptionOnlyConfig(full);
    Check(full.chainFast == 7 && full.shaderFast == 3 && full.workerPlacement == 2 && p.chainFast == 0 &&
              p.shaderFast == 0 && p.workerPlacement == 0,
          "R48 perception only: ChainFast, ShaderFast and WorkerPlacement off");
  }
  StatusAndKeys();
  Collect();
  Perception();
  Device();
  Unchanged();
  DeleteFileW(L"./DcsQvCull.ini");
  g_cfg = saved;
  g_dir = dir0;
  g_perceptionOnly = po0;
  g_engineOff = false;
}

}  // namespace potest
