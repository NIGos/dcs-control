// R90: the keys of the hitch fixes that are not read where they act (LoadConfig, every config read).
void ApplyR90Config() {
  qvpatch::g_mode = g_cfg.patchMode;
  SetAsyncLogWanted(g_cfg.asyncLog);  // R115 (L3): g_on only while a writer runs (log_writer.inc)
  shadowbatch::g_planWaitCapUs = static_cast<uint32_t>(g_cfg.shadowPlanWaitUs);
}

// [Prefetch] Mode/WindowMs, live: off (or the kill switch) = every patched
// slot restored to the real function; observe = installed, pass-through.
void ApplyPrefetch() {
  pffix::Apply(g_engineOff.load() ? pffix::kOff : g_cfg.prefetchMode, g_cfg.prefetchWindowMs);
  // R39 [Prefetch] TableBits: the repeat table's size (16 = the original 2^16 table).
  const unsigned bits = static_cast<unsigned>(g_cfg.prefetchTableBits);
  if (pffix::TableBits() != bits) {
    if (pffix::SetTableBits(bits))
      Log("prefetch: repeat table 2^%u entries (%u KB)", bits, static_cast<unsigned>((8ull << bits) >> 10));
    else
      Log("prefetch: repeat table 2^%u could not be allocated; 2^%u kept", bits, pffix::TableBits());
  }
}

// [General] PerceptionOnly: the NGModel globals pointer the cockpit ipd's
// helmet and ownship matching read (gbrec::g_env.globals; with
// PerceptionOnly=0 the G-buffer recorder's install sets it). Taken once
// NGModel is the analysed build: its location by signature (shadow_tex.h
// ResolveAddrs), ModelMaterialMT's RTTI and two of its functions' code hashes
// (shadow_tex.h VerifyBuild's NGModel code checks, without its slot checks:
// reads and compares only, patches nothing, shadowtex::g_at untouched).
// Worker, each second until decided.
int g_poGlobals = 0;  // 0 = waiting for NGModel, 1 = set, -1 = build mismatch
void PerceptionGlobalsTick() {
  if (g_poGlobals != 0) return;
  auto* ng = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  if (!ng) return;
  shadowtex::Addrs a;
  if (!shadowtex::ResolveAddrs(a) ||
      !allocslab::RttiIs(ng, reinterpret_cast<void**>(ng + a.model), ".?AVModelMaterialMT@model@@") ||
      !reloc::CodeIs(hooksig::NG_ModelMaterialMT_drawShadow, ng, shadowtex::kSlot5, shadowtex::kSlot5End,
                     shadowtex::kSlot5Hash) ||
      !reloc::CodeIs(hooksig::NG_ModelMaterialMT_submit, ng, shadowtex::kSubmit, shadowtex::kSubmitEnd,
                     shadowtex::kSubmitHash)) {
    g_poGlobals = -1;
    Log("perception only: NGModel is not the analysed build; the cockpit ipd's helmet and ownship matching stay off");
    return;
  }
  if (!gbrec::g_env.globals) gbrec::g_env.globals = reinterpret_cast<uint8_t* const*>(ng + a.globals);
  g_poGlobals = 1;
  Log("perception only: NGModel globals at NGModel+0x%x (the cockpit ipd's helmet and ownship matching)", a.globals);
}

void ApplyThreadsMax(void* scene) {
  if (g_cfg.collectThreadsMax <= 0 || g_sceneLayout.load() != 1 || g_threadsApplied.exchange(true)) return;
  auto* p = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(scene) + kSceneCollectThreadsMax);
  uint32_t before = *p;
  uint32_t v = static_cast<uint32_t>(g_cfg.collectThreadsMax);
  if (v > 16) v = 16;
  *p = v;
  Log("scene: collect threads max %u -> %u", before, v);
}

// Returns false if an exception was raised; `patched` always reflects the
// slots that were swapped so they can be restored.
bool SehPrepare(uint32_t count, uint8_t* infos, Patch* patches, size_t maxPatches, int* pairs,
                int* patched, std::vector<ViewInfo>* views) {
  __try {
    Prepare(count, infos, patches, maxPatches, pairs, patched, *views);
    if (g_shadowTight.load() && *pairs >= 1 && !g_perceptionOnly.load(std::memory_order_relaxed))
      PrepareShadows(count, infos, patches, maxPatches, patched);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_faulted = true;
    return false;
  }
}

// One-shot copy of the CollectionInfo array, taken by the collect hook when
// requested, to compare per-view fields (LOD inputs) between views.
std::atomic<bool> g_infoSnapRequest{false};
std::mutex g_infoSnapMutex;
std::vector<uint8_t> g_infoSnap;
uint32_t g_infoSnapCount = 0;

// One-frame dump of Scene.dll's per-object partition entries (IView+0x190,
// 7 collections x {ptr,end,cap}, entry stride 0x48, weight float at +0x10),
// taken right after collectSceneObjectsRenderables returns.
std::atomic<bool> g_entryDumpRequest{false};
std::mutex g_entryDumpMutex;
std::vector<std::string> g_entryDump;

void SafeCopy(void* dst, const void* src, size_t n, bool* ok) {
  __try {
    memcpy(dst, src, n);
    *ok = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *ok = false;
  }
}

void DumpEntries(uint8_t* view) {
  std::vector<std::string> out;
  char line[512];
  for (int k = 0; k < 7; ++k) {
    uint32_t n = 0;
    float totalW = 0;
    bool ok = false;
    SafeCopy(&totalW, view + 0x7c + 8 * k, 4, &ok);
    SafeCopy(&n, view + 0x80 + 8 * k, 4, &ok);
    uint8_t* arr = nullptr;
    SafeCopy(&arr, view + 0x190 + 0x18 * k, 8, &ok);
    snprintf(line, sizeof(line), "  collection %d: %u entries, total weight %.1f, array %p", k, n, totalW, arr);
    out.push_back(line);
    if (!arr || n == 0) continue;
    std::map<float, int> weights;
    for (uint32_t i = 0; i < n && i < 4096; ++i) {
      float w = 0;
      SafeCopy(&w, arr + i * 0x48 + 0x10, 4, &ok);
      if (ok) weights[w]++;
    }
    std::string ws = "    weights:";
    int shown = 0;
    for (auto& kv : weights) {
      if (shown++ > 12) break;
      snprintf(line, sizeof(line), " %g x%d", kv.first, kv.second);
      ws += line;
    }
    out.push_back(ws);
    for (uint32_t i = 0; i < n && i < 3; ++i) {
      uint64_t q[9];
      SafeCopy(q, arr + i * 0x48, sizeof(q), &ok);
      if (!ok) break;
      snprintf(line, sizeof(line), "    entry %u: %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx", i,
               q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8]);
      out.push_back(line);
    }
  }
  std::lock_guard<std::mutex> lock(g_entryDumpMutex);
  g_entryDump.swap(out);
}

// Census of collect calls: which view sets are culled each frame, keyed by
// view count, shading models present and aux views.
struct CensusEntry {
  uint64_t calls = 0, us = 0, views = 0;
};
std::mutex g_censusMutex;
std::map<uint64_t, CensusEntry> g_census;

void CensusRecord(uint32_t count, const uint8_t* infos, uint64_t us) {
  uint32_t shadingMask = 0, aux = 0;
  if (infos && count <= kMaxCollectInfos) {
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* ci = infos + i * kCollectionInfoStride;
      uint16_t sm = *reinterpret_cast<const uint16_t*>(ci + kCiShadingModel);
      shadingMask |= 1u << (sm < 31 ? sm : 31);
      if (*reinterpret_cast<void* const*>(ci + kCiAuxCallback)) ++aux;
    }
  }
  uint64_t key = (static_cast<uint64_t>(count) << 40) | (static_cast<uint64_t>(shadingMask) << 8) | aux;
  std::lock_guard<std::mutex> lock(g_censusMutex);
  CensusEntry& e = g_census[key];
  e.calls++;
  e.us += us;
  e.views += count;
}

void CensusDump(uint64_t quadFrames) {
  std::map<uint64_t, CensusEntry> snap;
  {
    std::lock_guard<std::mutex> lock(g_censusMutex);
    snap.swap(g_census);
  }
  if (snap.empty() || !quadFrames) return;
  double f = static_cast<double>(quadFrames);
  uint64_t totalUs = 0;
  for (auto& kv : snap) totalUs += kv.second.us;
  Log("census over %llu quad frames: collect total %.3f ms/frame",
      static_cast<unsigned long long>(quadFrames), totalUs / 1000.0 / f);
  for (auto& kv : snap) {
    uint32_t count = static_cast<uint32_t>(kv.first >> 40);
    uint32_t mask = static_cast<uint32_t>(kv.first >> 8) & 0xffffffffu;
    uint32_t aux = static_cast<uint32_t>(kv.first & 0xff);
    Log("  views=%2u shading=0x%08x aux=%u  calls/frame=%.2f  ms/frame=%.3f", count, mask, aux,
        kv.second.calls / f, kv.second.us / 1000.0 / f);
  }
}

// The quad views' ClippingVolumes of a classified call (the infos hold the
// originals again), for the G-buffer recorder's execution filter (R24 12).
void PublishQuadViewsRaw(const uint8_t* infos, const ViewInfo* views, size_t n, int64_t qpc) {
  const void* vols[gbrec::kQuadVols];
  int k = 0;
  for (size_t i = 0; i < n && k < gbrec::kQuadVols; ++i) {
    const ViewInfo& v = views[i];
    const bool focusOfPair = v.role == 2 && [&] {
      for (size_t j = 0; j < n; ++j)
        if (views[j].role == 1 && views[j].partner == static_cast<int>(i)) return true;
      return false;
    }();
    if (!((v.role == 1 && v.partner >= 0) || focusOfPair)) continue;
    vols[k++] = *reinterpret_cast<void* const*>(infos + v.index * kCollectionInfoStride + kCiClipVolume);
  }
  gbrec::PublishQuadVolumes(vols, k, qpc);
}
// [Perception] CockpitTrueIpd: the frame's eye separation as DCS renders it, measured before its passes (the two
// peripheral views' frustum apexes; only with exactly two peripheral / focus pairs). cockpit_ipd.h NoteCullSep.
void NoteCullSeparation(const std::vector<ViewInfo>& views) {
  const ViewInfo* p[2] = {};
  int n = 0;
  for (const ViewInfo& v : views)
    if (v.role == 1 && v.partner >= 0 && v.fr.valid) {
      if (n < 2) p[n] = &v;
      ++n;
    }
  if (n == 2) fixdepth::ckipd::NoteCullSep(Len(p[1]->fr.apex - p[0]->fr.apex));
}
void PublishQuadViews(const uint8_t* infos, const std::vector<ViewInfo>& views, int64_t qpc) {
  __try {
    PublishQuadViewsRaw(infos, views.data(), views.size(), qpc);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Separate function: __try cannot share a frame with C++ objects.
void SafeApplyWeights(uint8_t* view) {
  __try {
    partw::Apply(view);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_costWeightsOn = false;
  }
}

void __fastcall HookCollect(void* self, uint32_t count, uint8_t* infos, uint8_t* outs,
                            void* taskQueue, uint32_t flags) {
  qvalog::NoteFrameThread();  // R93: DCS's render thread (its log lines never wait for ring space)
  void* scene = static_cast<uint8_t*>(self) - 8;
  if (!g_scene.exchange(scene)) Log("scene: DCSScene at %p", scene);
  g_lastCollectFlags = flags;
  // [General] PerceptionOnly: observer only (no thread limit, partition or
  // volume touched): the views are classified for the eye separation
  // (NoteCullSeparation) and the camera cut (scene_cut.h), and DCS's call
  // runs with its own arguments.
  const bool po = g_perceptionOnly.load(std::memory_order_relaxed);
  if (!po) {
    auto* tmax = reinterpret_cast<volatile uint32_t*>(static_cast<uint8_t*>(scene) + kSceneCollectThreadsMax);
    uint32_t expect = 0;
    g_defaultThreadsMax.compare_exchange_strong(expect, *tmax);
    // Layout: Install verified that this Scene.dll's collect code uses the
    // field and the argument as on 2.9.30 (R63 U2; the value test alone is no
    // layout check). Plausibility guard on top: the field holds DCS's
    // worker-thread limit (6 on 2.9.30). Otherwise leave both alone.
    const uint32_t def = g_defaultThreadsMax.load();
    const bool codeOk = g_sceneLayout.load(std::memory_order_relaxed) == 1;
    const bool layoutOk = codeOk && def >= 1 && def <= 32;
    if (!layoutOk) {
      static std::atomic<bool> warned{false};
      if (!warned.exchange(true)) {
        if (!codeOk)
          Log("partition boost: unexpected scene layout (Scene.dll collect code does not use +0x%zx as on 2.9.30); "
              "disabled, field and argument left alone", kSceneCollectThreadsMax);
        else
          Log("partition boost: unexpected scene layout (value %u); disabled", def);
      }
    } else if (g_partitionBoost.load()) {
      if (*tmax < 8) *tmax = 8;
      if (flags < 7) flags = 7;
    } else if (*tmax != g_defaultThreadsMax.load() && g_cfg.collectThreadsMax <= 0) {
      *tmax = g_defaultThreadsMax.load();
    }
    ApplyThreadsMax(scene);
  }

  thread_local std::vector<ViewInfo> views;
  // Every info slot is patched at most once by the views and once by the shadows.
  thread_local std::vector<Patch> patchBuf;
  // Sized only for calls we classify (count is DCS's; an unbounded count would
  // size the buffer from it), and a failed allocation must not throw through
  // DCS's frames: such a call is simply not classified.
  bool havePatchBuf = false;
  if (count <= kMaxCollectInfos) {
    try {
      if (patchBuf.size() < 2u * count + 16u) patchBuf.resize(2u * count + 16u);
      havePatchBuf = true;
    } catch (...) {
    }
  }
  Patch* patches = patchBuf.data();
  int pairs = 0;
  int patched = 0;
  bool classified = false;
  if (count >= 4 && count <= kMaxCollectInfos && infos && havePatchBuf) {
    classified = SehPrepare(count, infos, patches, patchBuf.size(), &pairs, &patched, &views);
    if (!classified) Log("ERROR: exception while preparing views; optimisation disabled");
  } else if (count > kMaxCollectInfos) {
    static std::atomic<bool> once{false};
    if (!once.exchange(true)) Log("collect: a call with %u collections (above %u) is not classified", count, kMaxCollectInfos);
  }

  if (!po) {  // never installed in PerceptionOnly; skipped outright
    if (allocslab::g_state.load(std::memory_order_relaxed) > 0) allocslab::OnCollect();
    if (directupload::g_state.load(std::memory_order_relaxed) > 0) directupload::OnCollect();
    if (partw::g_orig) SafeApplyWeights(static_cast<uint8_t*>(self));
  }

  LARGE_INTEGER t0, t1;
  const uint32_t joinId = jointail::Begin();  // 0 unless [Suite] JoinTailCount is counting
  QueryPerformanceCounter(&t0);
  g_origCollect(self, count, infos, outs, taskQueue, flags);
  QueryPerformanceCounter(&t1);
  jointail::End(joinId);

  for (int i = 0; i < patched; ++i) *reinterpret_cast<uint8_t**>(patches[i].slot) = patches[i].original;
  if (pairs >= 1 && !po) PublishQuadViews(infos, views, t1.QuadPart);  // the G-buffer recorder's filter
  if (pairs >= 2 && fixdepth::ckipd::g_on.load(std::memory_order_relaxed)) NoteCullSeparation(views);
  // R30 L1: every frame's main call (quad or not) for the perf line and the governor.
  perfgov::OnCollect(count, classified ? static_cast<int>(views.size()) : -1, classified ? pairs : -1, t1.QuadPart);

  uint64_t us = static_cast<uint64_t>((t1.QuadPart - t0.QuadPart) * g_qpcToUs);
  CensusRecord(count, infos, us);
  if (g_entryDumpRequest.exchange(false)) DumpEntries(static_cast<uint8_t*>(self));
  if (g_infoSnapRequest.load() && infos && count <= kMaxCollectInfos) {
    std::lock_guard<std::mutex> lock(g_infoSnapMutex);
    g_infoSnap.assign(infos, infos + count * kCollectionInfoStride);
    g_infoSnapCount = count;
    g_infoSnapRequest = false;
  }
  ctiming::Add(ctiming::kObjects, t0.QuadPart, t1.QuadPart);
  mtiming::EndWindow((t1.QuadPart - t0.QuadPart) * g_qpcToUs / 1000.0, GetCurrentThreadId());
  g_stats.calls++;
  g_stats.usAll += us;
  uint64_t prevMax = g_stats.maxViews.load();
  while (count > prevMax && !g_stats.maxViews.compare_exchange_weak(prevMax, count)) {
  }
  if (pairs >= 1 && outs) {
    g_stats.quadCalls++;
    g_stats.usQuad += us;
    g_stats.patchedViews += patched;
    uint64_t pr = 0, fr = 0;
    for (const ViewInfo& v : views) {
      size_t c = VectorCount(outs + v.index * kVectorStride);
      if (v.role == 1) pr += c;
      if (v.role == 2) fr += c;
    }
    g_stats.periphRenderables += pr;
    g_stats.focusRenderables += fr;
    uint64_t sr = 0;
    for (uint32_t i = 0; i < count; ++i)
      if (*reinterpret_cast<const uint16_t*>(infos + i * kCollectionInfoStride + kCiShadingModel) == 14)
        sr += VectorCount(outs + i * kVectorStride);
    g_shadowStats.shadowRend += sr;
    g_frameShadowRend = sr;
    motion::OnFrame(views, t1.QuadPart, us, pr, fr, sr);
    vprof::OnFrame(t1.QuadPart, us, pr, fr, sr);
    int64_t zero = 0;
    g_firstQuadQpc.compare_exchange_strong(zero, t1.QuadPart);
    BenchOnFrame(t1.QuadPart, pr, fr, us);
  }
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

const char kViewVtable[] = "??_7DCSScene@@6BIView@SceneAggregator@Graphics@@@";
const char kCollectExport[] =
    "?collectSceneObjectsRenderables@?$SceneBase@UObjectCollections@DCSSceneCollections@@"
    "ULightCollections@2@@Graphics@@MEBAXIQEBUCollectionInfo@render@@QEAV?$vector@"
    "PEAUISceneRenderable@render@@V?$allocator@PEAUISceneRenderable@render@@@ed@@@ed@@"
    "PEAVTaskQueue@6@I@Z";
constexpr int kCollectSlot = 3;

// R63 U2: the collect function's own use of scene+0x40 and of its last
// argument (scene_check.h). Read inside the image only, under ReadBytes.
bool CheckSceneLayout(HMODULE scene, void* collect) {
  const auto* base = reinterpret_cast<const uint8_t*>(scene);
  const auto* fn = static_cast<const uint8_t*>(collect);
  const IMAGE_NT_HEADERS64* nt = sigscan::NtHeaders(base);
  if (!nt || fn < base) return false;
  const size_t rva = static_cast<size_t>(fn - base);
  if (rva >= nt->OptionalHeader.SizeOfImage) return false;
  const size_t n = std::min<size_t>(scenecheck::kScanBytes, nt->OptionalHeader.SizeOfImage - rva);
  std::vector<uint8_t> code(n);
  if (!allocslab::ReadBytes(fn, code.data(), n)) return false;
  const size_t at = scenecheck::FindThreadsMaxUse(code.data(), n);
  if (at == SIZE_MAX) return false;
  Log("scene: collect threads max verified (collect+0x%zx reads scene+0x%zx, capped by the last argument)", at,
      kSceneCollectThreadsMax);
  return true;
}

bool Install() {
  HMODULE scene = nullptr;
  for (int i = 0; i < 600 && !scene && !g_stop.load(); ++i) {
    scene = GetModuleHandleW(L"Scene.dll");
    if (!scene) Sleep(500);
  }
  if (!scene) {
    Log("ERROR: Scene.dll not loaded after 5 minutes");
    return false;
  }
  auto vtable = reinterpret_cast<void**>(GetProcAddress(scene, kViewVtable));
  void* collect = reinterpret_cast<void*>(GetProcAddress(scene, kCollectExport));
  if (!vtable || !collect) {
    Log("ERROR: Scene.dll exports not found (vtable=%p collect=%p); DCS version changed?", vtable,
        collect);
    return false;
  }
  if (SlotOriginal(&vtable[kCollectSlot]) != collect) {
    Log("ERROR: vtable slot %d is %p, expected %p; refusing to patch", kCollectSlot,
        SlotOriginal(&vtable[kCollectSlot]), collect);
    return false;
  }
  g_sceneLayout = CheckSceneLayout(scene, collect) ? 1 : -1;
  void* orig = nullptr;
  if (!HookSlot(&vtable[kCollectSlot], reinterpret_cast<void*>(&HookCollect), &orig)) {
    Log("ERROR: VirtualProtect failed (%lu)", GetLastError());
    return false;
  }
  g_origCollect = reinterpret_cast<CollectFn>(orig);
  Log("hooked IView::collectSceneObjectsRenderables (Scene.dll+0x%llx)",
      static_cast<unsigned long long>(static_cast<uint8_t*>(collect) - reinterpret_cast<uint8_t*>(scene)));
  return true;
}

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Flips the in-flight kill switch. Refused while a suite or benchmark runs
// (they toggle the optimizations themselves).
void ApplyFrameHeap() { frameheap::Install(); }

void ApplyTaskClock() {
  if (g_taskClockOn.load() && !edtime::Install()) return;
  edtime::SetOn(g_taskClockOn.load());
}

void ApplyPacer() {
  if (g_pacerLowPowerOn.load() && !pacer::Install(g_tscHz, g_cfg.pacerTimeoutUs)) return;
  pacer::SetLowPower(g_pacerLowPowerOn.load());
}

void ApplyCbUpload() {
  if ((g_cbUploadSkipOn.load() || g_hookMeasure.load()) && !cbupload::Install()) return;
  cbupload::SetAttached(g_cbUploadSkipOn.load() || g_hookMeasure.load());
}

void ApplyCbSkip() {
  if (g_cbSkipOn.load() && !cbskip::Install()) return;
  cbskip::SetAttached(g_cbSkipOn.load());
}

void ApplyTriCounter() {
  if (g_triPlainOn.load() && !tricount::Prepare()) return;
  tricount::SetPlain(g_triPlainOn.load());
}

// R142: a live enable ([Texture] StreamDedupe 0 -> 1 in the ini, or the kill switch back ON) installs the hook when
// the worker's start did not (off at start, or dx11backend.dll not loaded yet): the same vtable slot write as
// SetAttached. A build mismatch (Install logs it) is not tried again this session.
void ApplyTextureHook() {
  static bool refused = false;
  if (g_texDedupeOn.load() && !texbind::g_orig && !refused && g_startInstallsDone.load() &&
      GetModuleHandleW(L"dx11backend.dll")) {
    if (texbind::Install(g_tscHz)) Log("texture binds: installed on a live enable ([Texture] StreamDedupe)");
    else refused = true;
  }
  texbind::SetAttached(g_texDedupeOn.load() || g_hookMeasure.load());
}
// R142: [Scene] CostWeights likewise (two vtable slots in Visualizer / animator; partw starts with an empty cost
// table: entries keep DCS's weights until measured, as after the worker's start install).
void ApplyCostWeights() {
  static bool refused = false;
  if (g_costWeightsOn.load() && !partw::g_orig && !refused && g_startInstallsDone.load() &&
      GetModuleHandleW(L"Scene.dll") && GetModuleHandleW(L"Visualizer.dll")) {
    if (partw::Install()) Log("partition weights: installed on a live enable ([Scene] CostWeights)");
    else refused = true;
  }
}
void BenchUseCompactTexTable(bool on) { texbind::UseCompactTable(on); }
void ApplyParUpload(bool on) {
  if (on && !parupload::Install()) {
    parupload::g_on = false;
    return;
  }
  parupload::g_on = on;
}

// Installed on first use (needs the model allocator hooks, written at a
// collect call: retried every second by LoadConfig); off = pass-through.
void ApplyDirectUpload(bool on) {
  if (on && allocslab::g_state.load() == 0) allocslab::Prepare();
  if (on && !directupload::Install()) {
    directupload::g_on = false;
    return;
  }
  directupload::g_on = on;
}

// Installed on first use (dx11backend checks; the first draw then hands it
// DCS's context: retried every second). Off = sites, call-table entries and
// context table restored (stock). A verify mismatch latches it off.
void ApplySplitFilter(bool on) {
  if (sfilt::g_disabled.load()) on = false;
  if (!on) {
    if (sfilt::g_attached.load() || sfilt::g_hooked.load() || sfilt::g_sitesPatched.load()) {
      sfilt::Detach();
      if (sfilt::g_disabled.load()) Log("split filter: off for this session (verify mismatch or fault)");
    }
    return;
  }
  sfilt::g_opMask = g_cfg.splitFilterOps;
  if (!sfilt::Install()) return;
  if (sfilt::g_attached.load() && sfilt::g_active != sfilt::g_opMask.load()) sfilt::Detach();  // ops changed
  if (!sfilt::Live()) sfilt::Attach();
}

// R15 F2 [Model] SrvTailTrim: installed on first use (dx11backend checks;
// retried every second until dx11backend is loaded). Off = both call sites
// restored (stock). A verify mismatch latches it off.
void ApplySrvTailTrim(bool on) {
  if (srvtrim::g_disabled.load()) on = false;
  if (!on) {
    if (srvtrim::g_patched.load()) {
      srvtrim::Detach();
      if (srvtrim::g_disabled.load()) Log("srv tail trim: off for this session (verify mismatch)");
    }
    return;
  }
  if (!srvtrim::Install()) return;
  if (srvtrim::g_patched.load() != 1 && srvtrim::Attach(false)) Log("srv tail trim: on");
}

// Suite: installs (whatever [Model] SrvTailTrim says); false (logged) when it cannot run.
bool SrvTailTrimReady() {
  if (srvtrim::g_disabled.load()) {
    Log("  srv tail trim is latched off for this session");
    return false;
  }
  if (!srvtrim::Install()) {
    Log("  srv tail trim not available (install state %d, see the log above)", srvtrim::g_state.load());
    return false;
  }
  return true;
}

// Suite: installs and attaches (whatever [D3D] SplitFilter says) and waits
// for the context; false (logged) when the filter cannot run.
bool SplitFilterReady() {
  if (sfilt::g_disabled.load()) {
    Log("  split filter is latched off for this session");
    return false;
  }
  sfilt::g_opMask = g_cfg.splitFilterOps;
  if (!sfilt::Install()) {
    Log("  split filter not available (install state %d, see the log above)", sfilt::g_state.load());
    return false;
  }
  if (sfilt::g_attached.load() && sfilt::g_active != sfilt::g_opMask.load()) sfilt::Detach();
  for (int i = 0; i < 60 && !sfilt::Attach(); ++i) Sleep(50);
  if (sfilt::Live()) return true;
  Log("  split filter: DCS's context was not captured (no DX11Renderer::draw seen)");
  ApplySplitFilter(g_cfg.splitFilter && !g_engineOff.load());
  return false;
}

// Suite: waits for the allocate detour (written at the next collect) and installs.
bool DirectUploadReady() {
  if (allocslab::g_state.load() == 0) allocslab::Prepare();
  for (int i = 0; i < 40 && allocslab::g_state.load() == 1; ++i) Sleep(50);
  if (directupload::Install()) return true;
  Log("  direct upload not ready (model allocator state %d, direct upload state %d)", allocslab::g_state.load(),
      directupload::g_state.load());
  return false;
}

void ApplyBigPages(bool on) {
  if (bigpages::g_state.load() == 0) bigpages::g_bigBytes = g_cfg.bigPageBytes;
  if (on && !bigpages::Install()) return;
  if (!on) bigpages::UndoLeftover();  // R63: what an earlier payload of this process left patched / retired
  bigpages::SetOn(on);
}

// Stage 1 (compile + log only); installed on first use, retried every second.
void ApplyShadowInst() {
  const bool on = g_cfg.shadowInst && !g_engineOff.load();
  if (on != shadowinst::g_on.load() || (on && shadowinst::g_state.load() == 0)) shadowinst::SetOn(on);
}

// [Suite] HoldYawDeg: constant view yaw for unattended benchmark views.
void ApplyHoldYaw(int deg) {
  if (deg >= 0)
    posesweep::Hold(true, deg);
  else if (posesweep::Holding())
    posesweep::Hold(false, 0);
}

// [Suite] HoldPitchDeg / PoseRotateDps (test only, posesweep): the held pitch, and a constant-rate synthetic yaw
// turn started / stopped when the rate changes (head motion for unattended option-3 captures).
void ApplyPoseTest(double pitchDeg, double rateDps, bool holdPose, bool wallClock) {
  posesweep::g_holdPose = holdPose;
  if (posesweep::g_wallClock.exchange(wallClock) != wallClock)  // R99
    Log("pose sweep: synthetic yaw from %s", wallClock ? "the wall clock at each locate ([Suite] PoseWallClock=1)"
                                                       : "each locate's displayTime");
  posesweep::g_pitchDeg = std::max(-60.0, std::min(60.0, pitchDeg));
  static double lastRate = 0;
  if (rateDps == lastRate) return;
  lastRate = rateDps;
  if (rateDps > 0) {
    if (posesweep::Start(rateDps)) Log("pose sweep: synthetic yaw turn at %.1f deg/s", rateDps);
  } else {
    posesweep::Stop();
    Log("pose sweep: synthetic yaw turn stopped");
  }
}

// Stage 2: batching on top of the compiled variants; retried every second
// until the compile pipeline and hooks are ready.
void ApplyShadowBatch() {
  const bool on = g_cfg.shadowBatch && g_cfg.shadowInst && !g_engineOff.load();
  if (on && !shadowbatch::Install()) {
    shadowbatch::g_on = false;
    return;
  }
  shadowbatch::g_async = g_cfg.shadowPlanAsync;
  shadowbatch::g_on = on;
}

// G-buffer stage 2: the G-buffer keys are collected and compiled while on;
// batching starts once the hooks are ready (retried every second).
// The G-buffer keys are collected and compiled for G-buffer batching and for
// the G-buffer recorder (off while batching is on).
bool GbKeysWanted() { return g_cfg.shadowInst && !g_engineOff.load() && (g_cfg.gbufferBatch || g_cfg.gbufferRecorder); }

void ApplyGBufferBatch() {
  const bool on = g_cfg.gbufferBatch && g_cfg.shadowInst && !g_engineOff.load();
  const bool keys = GbKeysWanted();
  if (keys != shadowinst::g_onGb.load() || (keys && !shadowinst::g_gbInstalled.load())) shadowinst::SetOnGb(keys);
  if (on && !(gbbatch::Install() && gbverify::Install())) {
    gbbatch::g_on = false;
    return;
  }
  gbbatch::g_on = on;
}

// R17 S3-S6 recorder: installed on first use (needs shadow batching's hooks and
// the compiled keys: retried every second); off = pass-through. Off while
// G-buffer batching is on (its leaders swap [item+0xd4]).
// The governor's stage (gov.h) picks the split mask and may pause it: the
// same switches the kill switch and bench mode 28 use at run time (the split
// mask is read when a job is armed; a job armed or started before a pause is
// dropped as stale or fails its vector check at the pass: DCS draws stock).
// The recorders' workers read DCS's per-frame renderables after the job
// started; the frame-heap generation (frame_heap.h's reset_memory hooks) is
// what tells them the heap was recycled under them (gb_rec.h HeapRecycled).
// Without those hooks the generation never moves and that check is blind
// (0.5.5 field crash: AV in RecordRange -> BuildWindow on [item+0xd4] after a
// mission restart), so the recorders stay off. Logged once per recorder.
inline bool RecycleGuardReadyFor(int frameHeapState) { return frameHeapState == 1; }
bool RecycleGuardReady(const char* who) {
  if (RecycleGuardReadyFor(frameheap::g_state.load())) return true;
  static std::atomic<int> logged{0};
  const int bit = who[0] == 's' ? 1 : 2;
  if (!(logged.fetch_or(bit) & bit))
    Log("%s: off for this session (the frame-heap reset hooks are not installed, so a job could read recycled "
        "renderables)", who);
  return false;
}

void ApplyShadowRecorder() {
  const perfgov::StageSettings gs = perfgov::ForStage(g_govStage.load(), g_cfg.shadowRecorderSplit,
                                                      static_cast<uint32_t>(g_cfg.gbufferRecorderHelpers));
  shrec::g_scope = g_cfg.shadowRecorderScope;
  shrec::g_waitUs = static_cast<uint32_t>(g_cfg.shadowRecorderWaitUs);
  shrec::g_priority = g_cfg.shadowRecorderPriority;  // THREAD_PRIORITY_* are -2..2
  shrec::g_split = gs.shadowSplit;
  shrec::g_helpers = static_cast<uint32_t>(g_cfg.shadowRecorderHelpers);
  shrec::g_instancing = g_cfg.shadowRecorderInstancing;
  const bool on = g_cfg.shadowRecorder && !g_engineOff.load() && !gs.pause && !(g_cfg.gbufferBatch && g_cfg.shadowInst);
  if (on && frameheap::g_state.load() == 0) frameheap::Install();  // the workers' recycled-heap check needs the generation
  if (on && !RecycleGuardReady("shadow recorder")) {
    shrec::g_on = false;
    return;
  }
  if (on && !shrec::Install()) {
    shrec::g_on = false;
    return;
  }
  shrec::g_on = on;
}

// R18 S1-S3 G-buffer recorder: installed on first use (needs shadow_inst's
// G-buffer keys: collected while it is on, retried every second); off =
// pass-through. Off while G-buffer batching is on.
void ApplyGBufferRecorder() {
  const perfgov::StageSettings gs = perfgov::ForStage(g_govStage.load(), g_cfg.shadowRecorderSplit,
                                                      static_cast<uint32_t>(g_cfg.gbufferRecorderHelpers));
  gbrec::g_scope = g_cfg.gbufferRecorderScope;
  gbrec::g_waitUs = static_cast<uint32_t>(g_cfg.gbufferRecorderWaitUs);
  gbrec::g_island = static_cast<uint32_t>(g_cfg.gbufferRecorderIsland);
  gbrec::g_stride = static_cast<uint32_t>(g_cfg.suiteGBufferRecVerifyStride);
  gbrec::g_maxSeg = static_cast<uint32_t>(g_cfg.gbufferRecorderMaxSegments);
  gbrec::g_helpers = gs.gbHelpers;
  gbrec::g_cockpit = g_cfg.gbufferRecorderCockpit;
  gbrec::g_byOrdinal = g_cfg.gbufferRecorderByOrdinal;
  const bool on = g_cfg.gbufferRecorder && g_cfg.shadowInst && !g_engineOff.load() && !gs.pause && !g_cfg.gbufferBatch;
  // The frame-heap reset hooks (pass-through unless [Model] FrameHeapSlabs): the
  // workers' recycled-memory check needs the generation (retried every second).
  if (on && frameheap::g_state.load() == 0) frameheap::Install();
  if (on && !RecycleGuardReady("gbuffer recorder")) {
    gbrec::g_on = false;
    return;
  }
  if (on && (!shadowinst::g_onGb.load() || !shadowinst::g_gbInstalled.load())) shadowinst::SetOnGb(true);
  if (on && !gbrec::Install()) {
    gbrec::g_on = false;
    return;
  }
  gbrec::g_on = on;
}

// pass_flush.h: DCS's immediate context and the pass execute hook. The device
// comes from shadow_inst ([Model] ShadowInstancing) or the split filter.
bool PassFlushPrepare() {
  if (pflush::Ready()) return true;
  ptiming::Install();
  ID3D11Device* dev = shadowinst::g_device;
  if (dev)
    dev->AddRef();
  else if (sfilt::Ctx* c = sfilt::g_ctx.load())
    c->GetDevice(&dev);
  const bool ok = dev && pflush::Prepare(dev, &g_quadFrame);
  if (dev) dev->Release();
  return ok;
}

std::atomic<bool> g_passFlushPhase{false};  // bench mode 30 running: its xrBeginFrame hook stays

// R20 (h) [Model] PassFlush: Flush at the chosen pass boundaries; waits for
// DCS's device and GraphicsCore (retried every second). 0 = callback removed,
// xrBeginFrame hook removed: as without pass_flush.h.
// R33 [Shadows] CascadeCache (cascade_cache.h, prototype): on/off with the
// configured parameters; installs on first use (retried every second while
// GraphicsCore or the terrain module are not loaded). Counters every 60 s.
void ApplyCascadeCache() {
  const bool on = g_cfg.cascadeCache && !g_engineOff.load();
  cascache::Apply(on, g_cfg.cascadeCacheMask, static_cast<uint32_t>(g_cfg.cascadeCacheMaxAge),
                  static_cast<uint32_t>(g_cfg.cascadeCacheMarginPct), static_cast<uint32_t>(g_cfg.cascadeCacheVerify));
  static ULONGLONG last = 0;
  if (on && cascache::g_on.load()) {
    const ULONGLONG now = GetTickCount64();
    if (!last) last = now;
    if (now - last >= 60000) {
      last = now;
      cascache::LogCounters("periodic");
    }
  }
}

void ApplyPassFlush() {
  static uint32_t logged = 0;
  const uint32_t mask = g_engineOff.load() ? 0 : g_cfg.passFlush;
  if (mask && !PassFlushPrepare()) return;  // retried
  if (mask & pflush::kAfterXrBegin)
    pflush::InstallXr();
  else if (!g_passFlushPhase.load())
    pflush::UninstallXr();
  pflush::SetMask(mask);
  if (pflush::g_mask.load() != logged) {
    logged = pflush::g_mask.load();
    Log("pass flush: mask 0x%x (%s)", logged, pflush::MaskText(logged).c_str());
  }
}

