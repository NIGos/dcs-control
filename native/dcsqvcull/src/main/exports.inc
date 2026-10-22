namespace {
// The payload's worker thread, joined by DcsQvPayload_Stop.
HANDLE g_workerThread = nullptr;

// Sets g_stop and waits (bounded) until the worker has left its loop and no
// suite runs. Before this, Stop tore the features down while the worker could
// still be inside LoadConfig -> Apply*() -> Install(): a hook patched after
// the feature's Shutdown, possibly after the loader had started the next
// generation (then RestoreStale never restores it), left a stopped payload's
// hook live with its workers already joined. Both threads poll g_stop /
// g_benchAbort (worker every 50 ms, Install's Scene.dll wait every 500 ms);
// the bounds only guard against a thread stuck in DCS. Returns true when both
// are idle.
bool StopWorkerThreads(DWORD timeoutMs) {
  // g_stop before g_benchAbort: a suite starting right now clears g_benchAbort
  // and then checks g_stop (SuiteThread), so one of the two sees the stop.
  g_stop = true;
  g_benchAbort = true;
  bool idle = true;
  if (g_workerThread) {
    if (WaitForSingleObject(g_workerThread, timeoutMs) == WAIT_OBJECT_0) {
      CloseHandle(g_workerThread);
      g_workerThread = nullptr;
    } else {
      idle = false;
      Log("payload: worker did not stop within %lu ms; continuing the stop", timeoutMs);
    }
  }
  const ULONGLONG until = GetTickCount64() + timeoutMs;
  while (g_benchRunningFlag.load() && GetTickCount64() < until) Sleep(20);
  if (g_benchRunningFlag.load()) {
    idle = false;
    Log("payload: suite still running after %lu ms; continuing the stop", timeoutMs);
  }
  return idle;
}

// A mission ends or another one starts loading (the Lua hook's
// onSimulationStop / onMissionLoadBegin, through the loader): DCS is about to
// free the mission's objects (renderables, items, meshes), possibly without a
// frame-heap reset in between (no frame renders while it unloads). A new
// generation makes every recorder job in flight stale: its workers stop
// before their next read phase, and a fault that still slips in is contained
// per job (RecycledFilter) instead of latching the recorder off for the
// session (field report 0.5.5: "gbuffer recorder: disabled for this session
// (exception 0xc0000005 ...)" after a mission restart).
void OnMissionEvent(uint32_t event) {
  g_sceneChangeQpc.store(defrec::Qpc(), std::memory_order_release);
  g_frameHeapGen.fetch_add(1, std::memory_order_acq_rel);
  // R32: the cockpit symbology's learned record classes name heap objects of this mission: cleared by the render
  // thread at its next frame end; that frame moves no symbology.
  fixdepth::ckipd::ocpu::OnMissionEvent();
  // Option 3's ownship (CockpitTrueIpdOwnship): the locked exterior instance belongs to this mission; unlocked at
  // the render thread's next lock step / frame end, the next one locked on its own kOwnLockFrames later.
  fixdepth::ckipd::OwnOnMissionEvent();
  // R59: the helmet switch's material verdicts and instance sides name this mission's heap objects too.
  fixdepth::ckipd::HelmetOnMissionEvent();
  cascache::OnMissionEvent();  // R33: every held cascade is re-rendered at the next frame
  vrfx::NoteMissionEvent();  // R58: VrFx's G / DoF state belongs to the previous mission or aircraft
  cpass::NoteMissionEvent();  // R77: so does the colour mode's eye-like adaptation
  fixdepth::cmask::sym::NoteMissionEvent();  // R95: the symbology's learnt HUD field / helmet extent: another aircraft
  // R77: the recorders' texture tables hold references on this scene's textures: released at their next entry.
  gbrec::NoteMissionEvent();
  shrec::NoteMissionEvent();
  Log("payload: %s; recorder jobs in flight are dropped",
      event == 2 ? "mission load begins" : event == 3 ? "local player changed slot" : "simulation stopped");
}

// DCS is shutting down (the Lua hook's exit finalizer, through the loader's
// DcsQvCull_Shutdown, before DCS frees its objects and closes the Lua state
// that loaded us). Every payload thread that polls DCS on its own stops
// touching DCS: the worker leaves its loop (perf/governor/scene-cut ticks,
// prefetch slot checks, fixation-depth ticks, the 1-s Apply retries, status
// publishing), suites and profiles abort, the D3D meter's refresher stops,
// recorder jobs in flight are dropped. Hooks stay installed and the payload
// stays mapped: DCS may still call them while it tears down, and they only
// pass through or read our own memory. Not a Stop: nothing is released and
// no slot is restored (D3D and DCS objects may already be gone). The timer
// cache keeps refreshing (DCS's shutdown waits may read that clock).
// Idempotent; called on the Lua thread (never under the loader lock).
bool QuiesceForExit(DWORD timeoutMs) {
  if (!qvexit::Begin()) return g_workerThread == nullptr;
  Log("payload: DCS is shutting down; stopping the payload's threads (hooks stay in place)");
  g_frameHeapGen.fetch_add(1, std::memory_order_acq_rel);
  d3ds::g_stopRefresher = true;
  const bool idle = StopWorkerThreads(timeoutMs);
  g_statusSink.Detach();  // stores 0: "not running" for the app's panel
  g_dualStatusSink.Detach();
  Log("payload: quiesced for exit (%s)", idle ? "worker stopped" : "worker still busy");
  StopAsyncLog();  // R90: every queued line out now; the rest of the exit is synchronous
  return idle;
}
}  // namespace

#ifndef DCSQV_NO_ENTRY
// Payload entry points, called by the loader (loader.cpp).
extern "C" __declspec(dllexport) int DcsQvPayload_Start(const DcsQvLoaderApi* api) {
  if (!api || api->version != kDcsQvLoaderApiVersion) return 1;
  g_api = api;
  g_dir = api->dir;
  StartAsyncLog();  // R90: DCS threads' lines through the ring from here
  Log("payload: started (build %s %s)", __DATE__, __TIME__);
  HANDLE t = CreateThread(nullptr, 0, qvexit::Thread<WorkerThread>, nullptr, 0, nullptr);
  if (!t) return 2;
  g_workerThread = t;  // joined by DcsQvPayload_Stop
  return 0;
}

// The prefetch fix's replacements (0 = PrefetchVirtualMemory, 1 =
// GetProcAddress): lets a later payload generation recognize and take over
// slots this one still holds (prefetch_fix.h).
extern "C" __declspec(dllexport) void* DcsQvPayload_PrefetchHooks(unsigned which) {
  return which == 0 ? reinterpret_cast<void*>(&pffix::Replacement) : reinterpret_cast<void*>(&pffix::FixedGetProc);
}

// Optional export, called by the loader before DcsQvPayload_Start.
extern "C" __declspec(dllexport) void DcsQvPayload_SetStatusWord(std::atomic<uint64_t>* word) {
  g_statusSink.Attach(word);
}
// Optional export, called by the loader before DcsQvPayload_Start: the dual
// camera status word (DcsQvCull_DualCameraStatus, loader_api.h).
extern "C" __declspec(dllexport) void DcsQvPayload_SetDualCameraStatusWord(std::atomic<uint64_t>* word) {
  g_dualStatusSink.Attach(word);
}

// Optional exports for the loader's Lua callbacks (loader_api.h).
extern "C" __declspec(dllexport) int DcsQvPayload_Quiesce(uint32_t timeoutMs) {
  return QuiesceForExit(timeoutMs) ? 1 : 0;
}
extern "C" __declspec(dllexport) void DcsQvPayload_MissionEvent(uint32_t event) { OnMissionEvent(event); }
// R51 VrFx GVision: the ownship's acceleration in G (body axes; y = normal load), from the Lua hook's
// onSimulationFrame (Export LoGetAccelerationUnits) through the loader's DcsQvCull_GLoad.
extern "C" __declspec(dllexport) void DcsQvPayload_GLoad(float gx, float gy, float gz, double modelTime) {
  vrfx::NoteG(gx, gy, gz, modelTime);
}
// R90 F7: whether the G samples are used at all ([VrFx] GVision on). The Lua hook's onSimulationFrame asks
// first (loader export DcsQvCull_GLoadWanted) and returns before any Export call or allocation when not.
extern "C" __declspec(dllexport) int DcsQvPayload_GLoadWanted() { return vrfx::g_wantGv.load() ? 1 : 0; }

// After Stop the loader keeps this payload's IAT and vtable hooks patched
// until the next generation repoints each slot (one at a time, from its
// worker) or RestoreStale puts the original back. Hooks that keep per-thread
// state keyed on this payload's own counters must then pass straight through:
// the frame heap and model allocator slabs are keyed on generations that only
// this payload's reset hooks bump. Once the new payload has hooked a reset
// slot but not yet the matching get/alloc slot, a reset no longer bumps the
// old generation, and the old hook served a pre-reset slab that the original
// then handed out again (two objects at one address).
void PassThroughAfterStop() {
  timercache::g_on = false;  // until the new payload re-patches, pass through
  g_frameHeapOn = false;
  g_allocSlabsOn = false;
  // R63: our reset hook may still run until the next payload repoints the slot: from its next reset it puts the
  // stock page size back and returns the retired pages (shared list) instead of retiring more.
  bigpages::g_wanted = false;
}

extern "C" __declspec(dllexport) void DcsQvPayload_Stop() {
  Log("payload: stopping");
  g_statusSink.Detach();  // stores 0; this payload never writes the word again
  g_dualStatusSink.Detach();
  // Worker and suite out first: neither may install a hook after the
  // Shutdown calls below (bounded wait, see StopWorkerThreads).
  const bool workerIdle = StopWorkerThreads(15000);
  if (workerIdle) perfmon::Shutdown();  // the render-thread handle and our adapter reference (worker-owned)
  if (workerIdle) tplace::Shutdown();   // R39: our threads' previous affinity masks back (worker-owned)
  ftl::Shutdown(workerIdle);            // R52 timeline: OpenXR dispatch and device create slots back (no-op unless used)
  // Prefetch fix: every slot still ours back to the real kernel function,
  // in-flight calls drained (the module is pinned: copies stay callable).
  pffix::Uninstall(2000);
  threadtune::Restore();
  allocslab::Invalidate();
  posesweep::Release();  // hold and sweep off, xrLocateViews back to pass-through
  dlssv::Stop();         // R122: CreateFeature (and GetSetting) hooks back to pass-through (no-op unless used)
  ftwin::Shutdown();     // R123: focus twin hooks out, its workers joined (no-op unless used)
  d3ds::Uninstall();
  pdev::Uninstall();    // [General] PerceptionOnly: DX11Renderer::draw back to DCS's if the first draw never came
  srvspan::Shutdown();  // restores the ApplyShaderBlock call sites if a count was running
  srvtrim::Shutdown();  // the same two call sites back to stock if the trim was on
  plod::Shutdown();     // stock LOD tables back, view callback and vt[24] hook out (no-op unless used)
  cascache::Shutdown(); // R33 cascade cache: SceneRender slot and the innermost execute out, textures released (no-op unless used)
  fixdepth::Shutdown(); // depth callback out and drained, worker joined, D3D objects and file mappings released
  cpass::Shutdown();    // R42: our shaders and buffers (the draw hooks are out with fixation depth)
  gpt::Shutdown();      // GPU pass timing's context-table hooks and xrEndFrame hook, if a phase was running
  sfast::Shutdown();     // R36: Dispatch hook out, identified kernels and variants released (no-op unless used)
  nighttone::Shutdown(); // R37: tone mapper fields and exposure bias back, addToneMappingPass import back (no-op unless used)
  chainfast::Shutdown(); // R34: CSSetShader and CreateComputeShader hooks out, kernel pairs released (no-op unless used)
  pflush::Shutdown();   // pass flush callback and xrBeginFrame hook out, our context reference released
  vramc::Shutdown();    // the VRAM count's device create hooks, if a phase was running
  sfilt::Shutdown();    // call sites, FX call-table entries and context table back to stock
  jointail::Shutdown();
  shadowrec::Shutdown();  // chained pointers and class slots back before batching's own shutdown
  fwdreccount::Shutdown();  // forward S0: SimplePassData slot 19, chain, class slots, sort observer out
  gbreccount::Shutdown();  // G-buffer S0 chain, class slots, sort observer (and its own sort patch) out
  // Each Shutdown below reports whether it drained its passes; if one did
  // not, shadow_inst's VS objects and results (which they read) stay alive.
  bool drained = gbrec::Shutdown();  // G-buffer recorder chain out, its workers joined (before the shadow recorder's sort sites)
  drained &= shrec::Shutdown();      // recorder chain out, its worker joined, before batching's shutdown
  PassThroughAfterStop();  // timer cache, frame heap and alloc slabs: our leftover hooks pass through
  timercache::g_stopUpdater = true;
  const bool gbDrained = gbbatch::Shutdown();  // G-buffer batching callbacks first
  if (gbDrained) gbverify::Shutdown();  // else a pass (maybe inside Verify) still uses its copies
  drained &= gbDrained;
  directupload::Shutdown();  // waits for mapped pages to be restored; before the update hook goes
  parupload::Shutdown();
  drained &= shadowbatch::Shutdown();  // unhooks the batching callbacks before the VS objects go
  shadowinst::Shutdown(drained);  // joins the compile workers, then releases our VS objects (if nothing still reads them)
  StopAsyncLog();  // R90: the queued lines out; the next payload starts its own writer
}
#endif
