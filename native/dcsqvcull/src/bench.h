// Automatic A/B benchmark: alternates the optimisation OFF/ON in short blocks
// and compares per-frame CPU cost. Included once from main.cpp after the
// globals it uses (Log, g_cfg, g_enabled, g_dir, g_qpcToUs).
#pragma once

#include <intrin.h>

#include <algorithm>

struct BenchBlock {
  bool on = false;
  double seconds = 0;
  uint64_t frames = 0;
  std::vector<float> intervalsMs;
  double periphRend = 0, focusRend = 0, collectMs = 0, shadowRend = 0;
  uint64_t procCycles = 0;
  uint64_t rtCycles = 0;  // cycles of the collecting thread between consecutive frames
  uint64_t rtFrames = 0;
  double gpuSum = 0;  // NVML utilisation samples
  int gpuSamples = 0;
};

struct BenchState {
  std::mutex mutex;
  bool recording = false;
  BenchBlock cur;
  // Kept across blocks so the first frame of a block still has an interval.
  int64_t lastFrameQpc = 0;
  DWORD lastTid = 0;
  uint64_t lastThreadCycles = 0;
};
BenchState g_bench;
std::atomic<bool> g_benchAbort{false};

// Called once per quad-view frame from the collect hook.
void BenchOnFrame(int64_t qpcNow, uint64_t periph, uint64_t focus, uint64_t collectUs) {
  DWORD tid = GetCurrentThreadId();
  ULONG64 cyc = 0;
  QueryThreadCycleTime(GetCurrentThread(), &cyc);
  std::lock_guard<std::mutex> lock(g_bench.mutex);
  int64_t prev = g_bench.lastFrameQpc;
  DWORD prevTid = g_bench.lastTid;
  uint64_t prevCyc = g_bench.lastThreadCycles;
  g_bench.lastFrameQpc = qpcNow;
  g_bench.lastTid = tid;
  g_bench.lastThreadCycles = cyc;
  if (!g_bench.recording) return;
  BenchBlock& b = g_bench.cur;
  b.frames++;
  if (prev) b.intervalsMs.push_back(static_cast<float>((qpcNow - prev) * g_qpcToUs / 1000.0));
  if (prevTid == tid && cyc > prevCyc) {
    b.rtCycles += cyc - prevCyc;
    b.rtFrames++;
  }
  b.periphRend += static_cast<double>(periph);
  b.focusRend += static_cast<double>(focus);
  b.shadowRend += static_cast<double>(g_frameShadowRend.load());
  b.collectMs += collectUs / 1000.0;
}

uint64_t ProcessCycles() {
  ULONG64 c = 0;
  QueryProcessCycleTime(GetCurrentProcess(), &c);
  return c;
}

struct BlockMetrics {
  double fps, frameMs, p95Ms, procCpuMs, rtCpuMs, periph, focus, collectMs, gpuUtil, shadow;
};

BlockMetrics Metrics(const BenchBlock& b, double tscHz) {
  BlockMetrics m{};
  if (!b.frames) return m;
  double f = static_cast<double>(b.frames);
  m.fps = f / b.seconds;
  std::vector<float> v = b.intervalsMs;
  if (!v.empty()) {
    double sum = 0;
    for (float x : v) sum += x;
    m.frameMs = sum / v.size();
    size_t k = static_cast<size_t>(v.size() * 0.95);
    if (k >= v.size()) k = v.size() - 1;
    std::nth_element(v.begin(), v.begin() + k, v.end());
    m.p95Ms = v[k];
  }
  m.procCpuMs = b.procCycles / tscHz * 1000.0 / f;
  m.rtCpuMs = b.rtFrames ? b.rtCycles / tscHz * 1000.0 / b.rtFrames : 0;
  m.periph = b.periphRend / f;
  m.focus = b.focusRend / f;
  m.shadow = b.shadowRend / f;
  m.collectMs = b.collectMs / f;
  m.gpuUtil = b.gpuSamples ? b.gpuSum / b.gpuSamples : -1;
  return m;
}

void Summarize(const std::vector<BenchBlock>& blocks, double tscHz, bool restoreOn, const char* what) {
  std::vector<BlockMetrics> ms;
  for (const auto& b : blocks) ms.push_back(Metrics(b, tscHz));

  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t name[64];
  swprintf_s(name, L"bench_%04d%02d%02d_%02d%02d%02d.csv", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond);
  FILE* csv = nullptr;
  _wfopen_s(&csv, (g_dir + name).c_str(), L"w");
  if (csv) {
    fprintf(csv,
            "block,on,seconds,frames,fps,frame_ms,p95_ms,process_cpu_ms_per_frame,"
            "collect_thread_cpu_ms_per_frame,collect_ms,periph_renderables,focus_renderables,gpu_util\n");
    for (size_t i = 0; i < blocks.size(); ++i) {
      const BlockMetrics& m = ms[i];
      fprintf(csv, "%zu,%d,%.2f,%llu,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.0f,%.0f,%.1f\n", i,
              blocks[i].on ? 1 : 0, blocks[i].seconds,
              static_cast<unsigned long long>(blocks[i].frames), m.fps, m.frameMs, m.p95Ms,
              m.procCpuMs, m.rtCpuMs, m.collectMs, m.periph, m.focus, m.gpuUtil);
    }
    fclose(csv);
  }

  // Each ON block is compared with the mean of its OFF neighbours, which
  // cancels slow drift (heat, streaming, scene changes).
  struct Field {
    const char* name;
    double BlockMetrics::*ptr;
  };
  const Field fields[] = {{"fps", &BlockMetrics::fps},
                          {"frame_ms", &BlockMetrics::frameMs},
                          {"p95_frame_ms", &BlockMetrics::p95Ms},
                          {"process_cpu_ms/frame", &BlockMetrics::procCpuMs},
                          {"collect_thread_cpu_ms/frame", &BlockMetrics::rtCpuMs},
                          {"collect_ms/frame", &BlockMetrics::collectMs},
                          {"periph_renderables", &BlockMetrics::periph},
                          {"focus_renderables", &BlockMetrics::focus},
                          {"shadow_renderables", &BlockMetrics::shadow},
                          {"gpu_util_%", &BlockMetrics::gpuUtil}};
  Log("==== BENCH RESULT: %s (%zu blocks, file %ls) ====", what, blocks.size(), name);
  // R30: a frame-rate cap in both states (VD/SteamVR at refresh/N with SSW or a cap) pins fps: say so first.
  {
    std::vector<float> offIv, onIv;
    for (const auto& b : blocks) {
      if (!b.frames) continue;
      std::vector<float>& d = b.on ? onIv : offIv;
      d.insert(d.end(), b.intervalsMs.begin(), b.intervalsMs.end());
    }
    const perfgov::Cap offCap = perfgov::DetectCap(offIv), onCap = perfgov::DetectCap(onIv);
    if (offCap.fps > 0 && onCap.fps > 0) {
      if (offCap == onCap)
        Log("NOTE: frame rate was locked at %.1f fps (refresh %d Hz / %d) in both OFF and ON blocks; fps cannot show "
            "CPU gains in this run. Compare process_cpu_ms/frame and render-thread busy instead, or disable SSW / the "
            "frame cap for the test",
            onCap.fps, onCap.hz, onCap.div);
      else
        Log("NOTE: frame rate was locked in both states (OFF %.1f fps = %d/%d, ON %.1f fps = %d/%d); fps differences "
            "are cap steps, not gains. Compare process_cpu_ms/frame and render-thread busy instead, or disable SSW / "
            "the frame cap for the test",
            offCap.fps, offCap.hz, offCap.div, onCap.fps, onCap.hz, onCap.div);
    }
  }
  for (const Field& fd : fields) {
    std::vector<double> diffsPct;
    double sumOn = 0, sumOff = 0;
    int nOn = 0, nOff = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
      double v = ms[i].*fd.ptr;
      if (!blocks[i].frames) continue;
      if (blocks[i].on) {
        sumOn += v;
        ++nOn;
      } else {
        sumOff += v;
        ++nOff;
      }
      if (!blocks[i].on) continue;
      double base = 0;
      int nb = 0;
      if (i > 0 && !blocks[i - 1].on && blocks[i - 1].frames) {
        base += ms[i - 1].*fd.ptr;
        ++nb;
      }
      if (i + 1 < blocks.size() && !blocks[i + 1].on && blocks[i + 1].frames) {
        base += ms[i + 1].*fd.ptr;
        ++nb;
      }
      if (!nb) continue;
      base /= nb;
      if (base != 0) diffsPct.push_back((v - base) / base * 100.0);
    }
    double mean = 0, sd = 0;
    for (double d : diffsPct) mean += d;
    if (!diffsPct.empty()) mean /= diffsPct.size();
    for (double d : diffsPct) sd += (d - mean) * (d - mean);
    if (diffsPct.size() > 1) sd = std::sqrt(sd / (diffsPct.size() - 1));
    // 95% interval with Student's t for n-1 degrees of freedom (short runs
    // have few pairs, so a fixed t would overstate the confidence).
    static const double kT95[] = {0, 12.71, 4.30, 3.18, 2.78, 2.57, 2.45, 2.36, 2.31, 2.26, 2.23, 2.20, 2.18};
    size_t dof = diffsPct.size() > 1 ? diffsPct.size() - 1 : 0;
    double t95 = dof < sizeof(kT95) / sizeof(kT95[0]) ? kT95[dof] : 2.10;
    double ci = diffsPct.size() > 1 ? t95 * sd / std::sqrt(static_cast<double>(diffsPct.size())) : 0;
    bool significant = diffsPct.size() > 1 && std::fabs(mean) > ci;
    Log("  %-28s OFF %9.3f  ON %9.3f  change %+6.2f%% +/- %.2f%% %s", fd.name,
        nOff ? sumOff / nOff : 0.0, nOn ? sumOn / nOn : 0.0, mean, ci,
        significant ? "(significant)" : "(within noise)");
  }
  Log("==== end of bench, optimisation restored to %s ====", restoreOn ? "ON" : "OFF");
}

std::atomic<int> g_benchActiveMode{0};  // read by SetBenchVariant
// Optional per-block callbacks (suite thread), set by a mode's phase around
// RunBenchmark: after the settle time, right before a block records; after it
// recorded (valid = not discarded).
void (*g_benchBlockBegin)(bool on) = nullptr;
void (*g_benchBlockEnd)(bool on, bool valid) = nullptr;

// Blocks until done. mode: 0 = exclusion OFF/ON, 1 = collect threads.
// Returns false if aborted.
bool RunBenchmark(int mode, bool beeps) {
  g_benchActiveMode = mode;
  gpu::Init();
  const int blocks = std::max(4, g_cfg.benchBlocks & ~1);
  const int blockMs = std::max(2000, g_cfg.benchBlockSec * 1000);
  const int settleMs = std::min(blockMs / 2, std::max(0, static_cast<int>(g_cfg.benchSettleSec * 1000)));
  const bool restoreOn = g_enabled.load();
  // R77: the kill switch (Alt+Shift+F11) as the user left it: mode 11 switches it per block and used to end with
  // everything ON, so a user who had switched the engine OFF found it ON and their next toggle turned it OFF.
  const bool engineWasOff = g_engineOff.load();
  g_benchAbort = false;
  if (mode >= 2) {
    g_enabled = restoreOn;
    Log(mode == 2   ? "bench: mode shader time cache, OFF = real timer per draw, ON = cached"
        : mode == 3 ? "bench: mode culling partition, OFF = DCS default, ON = 16 parse tasks"
        : mode == 4 ? "bench: mode render thread isolation, OFF = DCS scheduling, ON = own core + high priority"
        : mode == 5 ? "bench: mode timer resolution, OFF = system default, ON = 0.5 ms"
        : mode == 6 ? "bench: mode D3D11 filter, OFF = all calls forwarded, ON = redundant calls skipped"
        : mode == 7 ? "bench: mode D3D11 filter end to end, OFF = no hooks installed, ON = filter"
        : mode == 8 ? "bench: mode shadow casters, OFF = DCS culling, ON = tightened"
        : mode == 9 ? "bench: mode allocator slabs, OFF = DCS allocator (locked), ON = per-thread slabs"
        : mode == 10 ? "bench: mode texture streaming dedupe, OFF = every request, ON = repeats within 1 ms skipped"
        : mode == 11 ? "bench: mode all engine optimizations, OFF = stock DCS, ON = as configured"
        : mode == 12 ? "bench: mode triangle counter, OFF = locked add, ON = plain add"
        : mode == 13 ? "bench: mode FX constant buffer, OFF = every set, ON = same buffer skipped"
        : mode == 14 ? "bench: mode micro, OFF = both off, ON = constant buffer skip + plain triangle counter"
        : mode == 15 ? "bench: mode cost weights, OFF = DCS weights, ON = measured per-object cost"
        : mode == 16 ? "bench: mode constant-buffer upload, OFF = every upload, ON = identical uploads skipped"
        : mode == 17 ? "bench: mode pacer, OFF = PAUSE spin, ON = MWAITX wait"
        : mode == 18 ? "bench: mode frame heap, OFF = shared cursor, ON = per-thread slabs"
        : mode == 19 ? "bench: mode shadow texture skip, OFF = every texture set, ON = sets no shadow pass reads skipped"
        : mode == 20 ? "bench: mode texture dedupe table, OFF = previous 24-byte entries, ON = 16-byte entries"
        : mode == 21 ? "bench: mode big model pages, OFF = stock 63.5 KB pages, ON = big pages"
        : mode == 25 ? "bench: mode shadow batching plans, OFF = planned on the render thread, ON = planner threads"
        : mode == 26 ? "bench: mode direct upload, OFF = stock memcpy upload, ON = model data written into mapped pages"
        : mode == 27 ? "bench: mode split-path D3D11 state filter, OFF = stock (no patch or hook), ON = redundant state calls skipped at the dx11backend sites"
        : mode == 28 ? "bench: mode shadow recorder, OFF = DCS draws every caster, ON = recorded casters from a worker's command list"
        : mode == 29 ? "bench: mode g-buffer recorder, OFF = DCS draws every item, ON = recorded G-buffer draws from command lists"
        : mode == 30 ? "bench: mode pass flush, OFF = no Flush, ON = Flush at the configured pass boundaries"
        : mode == 31 ? "bench: mode setShaderResources tail trim, OFF = stock call sites, ON = identical tail not rebound"
        : mode == 32 ? "bench: mode periphery-only instancer LOD (LOSSY), OFF = stock tables, ON = scaled tables in the peripheral views"
        : mode == 33 ? "bench: mode terrain prefetch fix, OFF = observe (every call to the kernel), ON = repeats within WindowMs skipped"
                    : "bench: mode shadow instancing, OFF = one draw per caster, ON = one instanced draw per group");
  }
  if (mode == 1) {
    g_enabled = restoreOn;  // exclusion stays as it was; the variant is the thread count
    Log("bench: mode threads, OFF = DCS default, ON = %d collect threads", g_cfg.benchThreadsB);
  }
  Log("bench: start, %d blocks x %d s (settle %.1f s), alternating OFF/ON", blocks, blockMs / 1000,
      settleMs / 1000.0);
  if (beeps) {
    Chime(800, 100);
    Chime(1200, 100);
  }

  LARGE_INTEGER q0;
  QueryPerformanceCounter(&q0);
  uint64_t tsc0 = __rdtsc();
  std::vector<BenchBlock> done;
  int zeroQuad = 0;  // consecutive focused blocks without a quad-view frame
  const uint64_t frames0 = perfgov::g_frames.load();
  for (int i = 0; i < blocks && !g_benchAbort; ++i) {
    bool on = (i % 2) == 1;
    SetBenchVariant(on);
    Sleep(settleMs);
    if (g_benchBlockBegin) g_benchBlockBegin(on);
    uint64_t losses0 = g_focusLosses.load();
    bool focusedAtStart = g_xrFocused.load();
    LARGE_INTEGER a, b;
    uint64_t pc0;
    {
      std::lock_guard<std::mutex> lock(g_bench.mutex);
      g_bench.cur = BenchBlock{};
      g_bench.cur.on = on;
      g_bench.cur.intervalsMs.reserve(1024);
      g_bench.recording = true;
      QueryPerformanceCounter(&a);
      pc0 = ProcessCycles();
    }
    for (int t = 0; t < blockMs - settleMs && !g_benchAbort; t += 50) {
      Sleep(50);
      if (t % 250 == 0) {
        int u = gpu::Utilization();
        if (u >= 0) {
          std::lock_guard<std::mutex> lock(g_bench.mutex);
          g_bench.cur.gpuSum += u;
          g_bench.cur.gpuSamples++;
        }
      }
    }
    BenchBlock blk;
    {
      std::lock_guard<std::mutex> lock(g_bench.mutex);
      g_bench.recording = false;
      QueryPerformanceCounter(&b);
      g_bench.cur.procCycles = ProcessCycles() - pc0;
      g_bench.cur.seconds = (b.QuadPart - a.QuadPart) * g_qpcToUs / 1e6;
      blk = std::move(g_bench.cur);
    }
    if (!focusedAtStart || g_focusLosses.load() != losses0 || !g_xrFocused.load()) {
      Log("bench: block %d/%d discarded (OpenXR session not FOCUSED)", i + 1, blocks);
      blk.frames = 0;
    } else if (!blk.frames && ++zeroQuad >= 2 && !g_benchAbort) {
      // R30 L8: focused, but no frame was classified as quad views: nothing to compare.
      const int views = perfgov::g_views.load(), pairs = perfgov::g_pairs.load();
      const uint64_t all = perfgov::g_frames.load() - frames0;
      Log("bench: 0 quad-view frames in 2 blocks while %llu frames were drawn: this A/B bench counts only frames "
          "with recognised quad views (a focus and a peripheral view per eye); the runtime shows %d views per frame "
          "(%d focus/peripheral pairs)%s. Stopped: nothing to compare. Use the kill switch (Alt+Shift+F11) and the "
          "perf: lines instead",
          static_cast<unsigned long long>(all), views, pairs,
          views >= 0 && views < 4 ? " (stereo)" : views < 0 ? " (no frame classified)" : "");
      g_benchAbort = true;
    } else if (blk.frames) {
      zeroQuad = 0;
    }
    if (g_benchBlockEnd) g_benchBlockEnd(on, blk.frames != 0);
    Log("bench: block %d/%d %s frames=%llu", i + 1, blocks, on ? "ON " : "OFF",
        static_cast<unsigned long long>(blk.frames));
    done.push_back(std::move(blk));
  }
  LARGE_INTEGER q1;
  QueryPerformanceCounter(&q1);
  double tscHz = (__rdtsc() - tsc0) / ((q1.QuadPart - q0.QuadPart) * g_qpcToUs / 1e6);
  SetBenchVariant(false);
  g_enabled = restoreOn;
  if (mode == 2) ApplyTimerConfig();  // back to the configured state
  if (mode == 3) g_partitionBoost = g_cfg.partitionBoost;
  if (mode == 5) SetTimerResolution(g_cfg.fineTimer);
  if (mode == 8) g_shadowTight = g_cfg.shadowTight;
  if (mode == 9) g_allocSlabsOn = g_cfg.allocSlabs && !g_engineOff.load();
  if (mode == 10) {
    g_texDedupeOn = g_cfg.texDedupe && !g_engineOff.load();
    ApplyTextureHook();
  }
  if (mode == 11) {
    BenchSetEngineOff(engineWasOff);
    if (engineWasOff) Log("bench: engine optimizations left OFF (the kill switch as before the A/B)");
  }
  if (mode == 18) g_frameHeapOn = g_cfg.frameHeapSlabs && !g_engineOff.load();
  if (mode == 20) BenchUseCompactTexTable(true);
  if (mode == 21) ApplyBigPages(g_cfg.bigPages && !g_engineOff.load());
  if (mode == 22 || mode == 25) ApplyShadowBatch();
  if (mode == 24) ApplyParUpload(g_cfg.parUpload && !g_engineOff.load());
  if (mode == 26) ApplyDirectUpload(g_cfg.directUpload && !g_engineOff.load());
  if (mode == 27) ApplySplitFilter(g_cfg.splitFilter && !g_engineOff.load());
  if (mode == 28) ApplyShadowRecorder();
  if (mode == 29) ApplyGBufferRecorder();
  if (mode == 30) ApplyPassFlush();
  if (mode == 31) ApplySrvTailTrim(g_cfg.srvTailTrim && !g_engineOff.load());
  if (mode == 33) ApplyPrefetch();
  if (mode == 23) ApplyGBufferBatch();
  if (mode == 19) {
    g_shadowTexSkipOn = g_cfg.shadowTexSkip && !g_engineOff.load();
    ApplyShadowTexSkip();
  }
  if (mode == 17) {
    g_pacerLowPowerOn = g_cfg.pacerLowPower && !g_engineOff.load();
    ApplyPacer();
  }
  if (mode == 16) g_cbUploadSkipOn = g_cfg.cbUploadSkip && !g_engineOff.load();
  if (mode == 15) g_costWeightsOn = g_cfg.costWeights && !g_engineOff.load();
  if (mode == 14) {
    g_cbSkipOn = g_cfg.cbSkip && !g_engineOff.load();
    ApplyCbSkip();
    g_triPlainOn = g_cfg.triPlain && !g_engineOff.load();
    ApplyTriCounter();
  }
  if (mode == 13) {
    g_cbSkipOn = g_cfg.cbSkip && !g_engineOff.load();
    ApplyCbSkip();
  }
  if (mode == 12) {
    g_triPlainOn = g_cfg.triPlain && !g_engineOff.load();
    ApplyTriCounter();
  }
  if (mode == 6 || mode == 7) g_d3dMode = g_cfg.d3dFilter ? 1 : 0;
  if (mode == 7) RestoreD3dHooks();
  if (g_benchAbort) {
    Log("bench: aborted");
    Chime(300, 300);
    return false;
  }
  Log("bench: TSC %.0f MHz", tscHz / 1e6);
  Summarize(done, tscHz, restoreOn,
            mode == 1   ? "collect threads (OFF = DCS default, ON = ThreadsB)"
            : mode == 2 ? "shader time cache OFF/ON"
            : mode == 3 ? "culling partition 12 -> 16 tasks"
            : mode == 4 ? "render thread isolation"
            : mode == 5 ? "system timer resolution 0.5 ms"
            : mode == 6 ? "skip redundant D3D11 state calls"
            : mode == 7 ? "D3D11 filter end to end (OFF = no hooks)"
            : mode == 8 ? "tighter shadow caster culling"
            : mode == 9 ? "per-thread slabs for the model data allocator"
            : mode == 10 ? "skip repeated texture streaming requests within 1 ms"
            : mode == 11 ? "all engine optimizations vs stock DCS"
            : mode == 12 ? "plain add on the triangle statistics counter"
            : mode == 13 ? "skip setting the same FX constant buffer again"
            : mode == 14 ? "micro: constant buffer skip + plain triangle counter"
            : mode == 15 ? "culling partition weighted by measured cost"
            : mode == 16 ? "skip identical constant-buffer uploads"
            : mode == 17 ? "Main-thread pacer wait with MWAITX"
            : mode == 18 ? "per-thread slabs for the edCore frame heap"
            : mode == 19 ? "skip shadow-caster texture sets no shadow pass reads"
            : mode == 20 ? "texture dedupe: 16-byte table vs previous 24-byte table"
            : mode == 21 ? "big model data pages"
            : mode == 22 ? "shadow caster instancing"
            : mode == 24 ? "parallel copy for large structured-buffer uploads"
            : mode == 23 ? "g-buffer instancing"
            : mode == 25 ? "shadow batching plans on planner threads"
            : mode == 26 ? "direct upload into mapped GPU pages"
            : mode == 27 ? "split-path D3D11 state filter"
            : mode == 28 ? "shadow recorder (R17 S3)"
            : mode == 29 ? "g-buffer recorder (R18 S3)"
            : mode == 30 ? "Flush at pass boundaries (R20 h)"
            : mode == 31 ? "setShaderResources identical-tail trim (R15 F2)"
            : mode == 32 ? "periphery-only instancer LOD (R26 13, LOSSY)"
            : mode == 33 ? "terrain prefetch fix (observe vs skip)"
                        : "peripheral exclusion OFF/ON");
  if (beeps)
    for (int k = 0; k < 3; ++k) Chime(1500, 90);
  return true;
}
