// DcsQvCull: CPU culling optimisation for DCS World in quad-view VR.
//
// The four quad views (two wide peripheral views, two narrow focus views) are
// culled independently by Graphics::SceneBase. Every object inside a focus
// frustum is drawn twice: once at high resolution in the focus view and once in
// the peripheral view, where the compositor then covers it with the focus
// layer. This DLL hands the peripheral views an extra "exclusion volume" equal
// to the (slightly shrunk) focus frustum of the same eye. Scene.dll already
// supports exclusion volumes natively: an object whose OBB lies entirely inside
// one is skipped, so it never reaches LOD selection, sorting, batching or the
// draw call submission of the peripheral view.
//
// Hooking is done by patching the DCSScene vtable exported by Scene.dll, so no
// code bytes are modified.

#include <windows.h>
#include <share.h>
#include <winternl.h>   // NTSTATUS for d3dkmthk.h
#include <d3dkmthk.h>   // D3DKMTQueryStatistics: GPU engine busy for the perf line and the governor (perf_mon.h)

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <tuple>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "loader_api.h"
#include "log_utf8.h"
#include "dev_path.h"  // R62: [Dev] IniPath read whole, an unusable one logged
#include "reloc.h"
#include "status_word.h"
#include "scene_layout.h"
#include "scene_check.h"  // R63 U2: partition boost only on a Scene.dll whose collect code uses +0x40 as verified
#include "gov.h"       // R30: frame sampler, CPU/GPU-bound detector, governor stages (pure)
#include "env_info.h"  // R30 L3: DCS version, OpenXR runtime and layers, streamers
#include "dcsvr_depth_export.h"  // Far depth export contract with Quad Views (verbatim copy; far_depth.h)
#include "dcsvr_cockpit_mask.h"  // R65: cockpit mask export for a frame-generation layer (cockpit_mask.h writes it)
#include "dcsvr_cockpit_depth.h"  // R69: its depth companion (R68 Stage 1; cockpit_mask.h writes it)
#include "dcsvr_motion.h"  // R70: its motion-vector companion (R68 Stage 2; cockpit_mask.h writes it)
#include "dcsvr_symbology.h"  // R95: its symbology emission companion (R89; cockpit_mask.h's symbology.h writes it)
#include "dcsvr_pupil_fov.h"  // R95: the pupil shift layer's FOV pairs (Dev's header, verbatim; the focus layout)
#include "dcsvr_perception_shm.h"  // Perception layer's shared blocks (verbatim copy of perception_shm.h; cockpit_ipd.h reads BoostOut)
#include "color_grade.h"  // R40: [Color] opt-in post-tonemap grading keys (default 0; pure parts, pass not wired yet)
#include "color_pass.h"   // R42: [Color] Mode 0/1/2 (off / Fix / Enhanced): tone-map pixel shader swap (default 0)
#include "sym_trace_cfg.h"  // R94: [Perception] SymbologyTrace keys (dev, default off; sym_trace.h does the trace)

using namespace layout;

namespace {

// ---------------------------------------------------------------------------
// Logging and configuration
// ---------------------------------------------------------------------------

std::wstring g_dir;
std::mutex g_logMutex;
FILE* g_log = nullptr;                   // standalone builds (tests) only
const DcsQvLoaderApi* g_api = nullptr;   // set when running under the loader
std::atomic<bool> g_stop{false};         // payload is being replaced
qvstatus::Sink g_statusSink;             // loader-owned status word (status_word.h)
qvstatus::Sink g_dualStatusSink;         // loader-owned dual camera status word (DcsQvCull_DualCameraStatus)
bool g_reportCapture = false;  // guarded by g_logMutex
DWORD g_reportThread = 0;      // only lines logged by the suite thread go to the report
std::string g_reportBuf;

// R90 (docs/research/R90_hitch_hunting.md F2): asynchronous log path for DCS's threads. Log() used to format,
// write and flush (WriteFile) on the calling thread under two mutexes (this one, then the loader's): a render-
// thread line could wait behind a worker's burst (the census, the perception block) or a write stalled by a
// reader of the file (DCS Control and the harness read it) or a filter driver. Now:
//   - a thread that did not start through qvexit (DCS's render, collect and Main threads) formats the line,
//     stamps the local time and puts it into a bounded lock-free ring (Vyukov MPMC, kAsyncSlots lines); the
//     payload's log writer thread writes the lines in ring order, so each thread's lines keep their order, with
//     the time stamped at the call (through the loader's DcsQvCull_LogAt; a loader without it stamps the write
//     time, at most one writer period later);
//   - a full ring drops the line and counts it; the writer then logs "log: N lines from DCS threads dropped";
//     R93 (docs/research/R92_live_hitch_fg.md D1): the ring is 4096 lines (was 256: ~950 lines of the first mission
//     load's burst were dropped at every DCS start), and only a frame thread drops: a DCS thread that never ran one
//     of our per-frame hooks (the loading / Main threads; qvalog::t_frameThread false) waits for ring space instead
//     (the writer is woken; at most kFullWaitMs per line, then the line is dropped and counted). It still never
//     writes the file itself. A frame thread (the render thread: HookCollect, the pass hook, the xrEndFrame hooks)
//     never waits and never writes: a full ring drops its line, as before;
//   - the payload's own threads (worker, recorders, suite) write synchronously as before; a synchronous line
//     first drains the ring (a fault line of ours comes after every queued line), and Stop / Quiesce drain it.
// [Log] Async=0 writes every line synchronously (the old path). The line format and the .prev roll are the
// loader's, unchanged.
thread_local bool g_tOurThread = false;  // set by qvexit::NoteStart: a payload thread (Run / RunVoid)

namespace qvalog {
constexpr uint32_t kAsyncSlots = 4096;  // power of two (R93: was 256); 8 MiB, committed as the ring is first used
constexpr uint32_t kFullWaitMs = 250;   // R93: a non-frame DCS thread waits at most this long per line for ring space
// R93: set on a DCS thread by our per-frame hooks (HookCollect, the top-level pass hook, the xrEndFrame hooks): the
// render thread. Its lines never wait for ring space (a full ring drops them). Every other DCS thread may wait.
thread_local bool t_frameThread = false;
inline void NoteFrameThread() { t_frameThread = true; }
struct Slot {
  std::atomic<uint32_t> seq{0};
  SYSTEMTIME st{};
  char text[2048];
};
Slot g_ring[kAsyncSlots];
const bool g_ringInit = [] {
  for (uint32_t i = 0; i < kAsyncSlots; ++i) g_ring[i].seq.store(i, std::memory_order_relaxed);
  return true;
}();
std::atomic<uint32_t> g_enq{0};
uint32_t g_deq = 0;                 // under g_drainMutex
std::mutex g_drainMutex;            // one consumer at a time (the writer, or a synchronous line)
std::atomic<bool> g_on{false};      // writer running and [Log] Async=1: DCS threads enqueue
std::atomic<bool> g_want{true};     // [Log] Async
std::atomic<bool> g_testHold{false};  // tests: the writer leaves the ring alone
std::atomic<uint64_t> g_queued{0}, g_dropped{0}, g_written{0};
uint64_t g_droppedLogged = 0;       // under g_drainMutex
// R93: lines a non-frame DCS thread queued after waiting for ring space, the longest wait (us), the drops by frame
// threads (never waited) and the writer's wake event (set by a waiting thread; null: the writer polls every 10 ms).
std::atomic<uint64_t> g_waited{0}, g_waitMaxUs{0}, g_frameDropped{0};
uint64_t g_waitedLogged = 0;        // under g_drainMutex
HANDLE g_wake = nullptr;
using LogAtFn = void (*)(const char* line, const SYSTEMTIME* st);
LogAtFn g_logAt = nullptr;          // the loader's DcsQvCull_LogAt (resolved at start; null: g_api->log)

// Any thread. False when the ring is full (the caller counts the drop).
bool Push(const char* line, const SYSTEMTIME& st) {
  uint32_t pos = g_enq.load(std::memory_order_relaxed);
  Slot* s;
  for (;;) {
    s = &g_ring[pos & (kAsyncSlots - 1)];
    const uint32_t seq = s->seq.load(std::memory_order_acquire);
    const int32_t dif = static_cast<int32_t>(seq - pos);
    if (dif == 0) {
      if (g_enq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
    } else if (dif < 0) {
      return false;  // full
    } else {
      pos = g_enq.load(std::memory_order_relaxed);
    }
  }
  s->st = st;
  strncpy_s(s->text, sizeof(s->text), line, _TRUNCATE);
  s->seq.store(pos + 1, std::memory_order_release);
  g_queued.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// R93: a non-frame DCS thread with a full ring: wake the writer and retry for up to kFullWaitMs. 1: queued; 0: still
// full (the caller counts the drop); -1: the async path went off meanwhile (the caller writes synchronously, as with
// [Log] Async=0). Never touches the file or a log lock.
int WaitPush(const char* line, const SYSTEMTIME& st) {
  LARGE_INTEGER f, t0, t1;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  const int64_t limit = static_cast<int64_t>(f.QuadPart) * kFullWaitMs / 1000;
  for (;;) {
    if (!g_on.load(std::memory_order_acquire)) return -1;
    if (g_wake) SetEvent(g_wake);
    Sleep(1);
    const bool ok = Push(line, st);
    QueryPerformanceCounter(&t1);
    if (ok) {
      g_waited.fetch_add(1, std::memory_order_relaxed);
      const uint64_t us = static_cast<uint64_t>((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
      uint64_t m = g_waitMaxUs.load(std::memory_order_relaxed);
      while (us > m && !g_waitMaxUs.compare_exchange_weak(m, us, std::memory_order_relaxed)) {
      }
      return 1;
    }
    if (t1.QuadPart - t0.QuadPart >= limit) return 0;
  }
}

// One line to the log file (caller holds g_logMutex). st: the time to stamp (nullptr = now).
void Emit(const char* line, const SYSTEMTIME* st) {
  if (g_api) {
    if (st && g_logAt)
      g_logAt(line, st);
    else
      g_api->log(line);
    return;
  }
  if (!g_log) return;
  SYSTEMTIME now;
  if (!st) {
    GetLocalTime(&now);
    st = &now;
  }
  fprintf(g_log, "%02d:%02d:%02d.%03d %s\n", st->wHour, st->wMinute, st->wSecond, st->wMilliseconds, line);
  fflush(g_log);
}

// Set while this thread drains: a contained fault inside a drain (the SEH unwind skips the lock's destructor)
// must not make that thread's fault line wait for the drain lock it still holds.
thread_local bool t_inDrain = false;

bool Pending() {
  if (t_inDrain) return false;
  std::lock_guard<std::mutex> d(g_drainMutex);
  const Slot& s = g_ring[g_deq & (kAsyncSlots - 1)];
  return s.seq.load(std::memory_order_acquire) == g_deq + 1;
}

// Writes every queued line in ring order (writer thread, a synchronous line, Stop). Returns the lines written.
uint32_t Drain() {
  if (t_inDrain) return 0;
  std::lock_guard<std::mutex> d(g_drainMutex);
  t_inDrain = true;
  uint32_t n = 0;
  for (;;) {
    Slot& s = g_ring[g_deq & (kAsyncSlots - 1)];
    if (s.seq.load(std::memory_order_acquire) != g_deq + 1) break;  // empty, or the next slot not written yet
    {
      std::lock_guard<std::mutex> lock(g_logMutex);
      Emit(s.text, &s.st);
    }
    s.seq.store(g_deq + kAsyncSlots, std::memory_order_release);
    ++g_deq;
    ++n;
  }
  g_written.fetch_add(n, std::memory_order_relaxed);
  const uint64_t dropped = g_dropped.load(std::memory_order_relaxed);
  if (dropped != g_droppedLogged) {
    char line[160];
    snprintf(line, sizeof(line), "log: %llu lines from DCS threads dropped (async log ring of %u lines full)",
             static_cast<unsigned long long>(dropped - g_droppedLogged), kAsyncSlots);
    g_droppedLogged = dropped;
    std::lock_guard<std::mutex> lock(g_logMutex);
    Emit(line, nullptr);
  }
  const uint64_t waited = g_waited.load(std::memory_order_relaxed);
  if (waited != g_waitedLogged) {  // R93: a burst a loading thread outran the ring with (nothing lost)
    char line[200];
    snprintf(line, sizeof(line),
             "log: %llu lines from DCS non-frame threads waited for ring space (async log ring of %u lines full; "
             "longest wait %.1f ms); none of them dropped",
             static_cast<unsigned long long>(waited - g_waitedLogged), kAsyncSlots,
             g_waitMaxUs.load(std::memory_order_relaxed) / 1000.0);
    g_waitedLogged = waited;
    std::lock_guard<std::mutex> lock(g_logMutex);
    Emit(line, nullptr);
  }
  t_inDrain = false;
  return n;
}
}  // namespace qvalog

void Log(const char* fmt, ...) {
  if (!g_api && !g_log) return;
  char line[2048];
  va_list ap;
  va_start(ap, fmt);
  qvlog::FormatV(line, sizeof(line), fmt, ap);  // %ls as UTF-8 (log_utf8.h)
  va_end(ap);
  if (!g_tOurThread && qvalog::g_on.load(std::memory_order_acquire)) {  // R90: DCS's threads never write
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (qvalog::Push(line, st)) return;
    if (qvalog::t_frameThread) {  // R93: the render thread never waits: dropped and counted
      qvalog::g_frameDropped.fetch_add(1, std::memory_order_relaxed);
      qvalog::g_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const int w = qvalog::WaitPush(line, st);  // R93: a loading / Main thread waits for ring space (bounded)
    if (w > 0) return;
    if (w == 0) {
      qvalog::g_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    // w < 0: the async path went off while it waited: the synchronous path below (as with [Log] Async=0)
  }
  if (qvalog::Pending()) qvalog::Drain();  // lines queued before this one first
  std::lock_guard<std::mutex> lock(g_logMutex);
  if (!g_api && !g_log) return;
  qvalog::Emit(line, nullptr);
  if (g_reportCapture && GetCurrentThreadId() == g_reportThread) {
    g_reportBuf += line;
    g_reportBuf += '\n';
  }
}

// Hook slots go through the loader's registry so a reloaded payload always
// receives the true original function, never an older payload's hook.
bool HookSlot(void** slot, void* hook, void** original) {
  if (g_api) return g_api->patch(slot, hook, original);
  DWORD old;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
  void* prev = InterlockedExchangePointer(slot, hook);
  if (original) *original = prev;
  VirtualProtect(slot, sizeof(void*), old, &old);
  return true;
}

void* SlotOriginal(void** slot) { return g_api ? g_api->original(slot) : *slot; }

void UnhookSlot(void** slot, void* original) {
  if (g_api) {
    g_api->restore(slot);
    return;
  }
  DWORD old;
  if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
    InterlockedExchangePointer(slot, original);
    VirtualProtect(slot, sizeof(void*), old, &old);
  }
}

void StartReportCapture() {
  std::lock_guard<std::mutex> lock(g_logMutex);
  g_reportBuf.clear();
  g_reportThread = GetCurrentThreadId();
  g_reportCapture = true;
}

std::string StopReportCapture() {
  std::lock_guard<std::mutex> lock(g_logMutex);
  g_reportCapture = false;
  return std::move(g_reportBuf);
}

struct Config {
  // Defaults below equal LoadConfig's ini fallbacks (an empty or missing ini
  // gives the same configuration as a fresh Config).
  bool enabled = false;          // [Cull] Enabled (peripheral exclusion: opt-in)
  bool debugHole = false;
  bool perceptionOnly = false;   // [General] PerceptionOnly (see g_perceptionOnly)
  // Fraction of the focus image (per axis, about its centre) that may be
  // excluded from the periphery. <0 = derive from Quad-Views-Foveated edge
  // smoothing: only the area where the focus layer is fully opaque.
  double keepFraction = -1.0;
  double safetyNdc = 0.05;       // extra NDC border kept when deriving it
  double nativeKeepFraction = 0.35;  // used when QVFR is not loaded (runtime-native quad views)
  double saccadeDeg = 1.0;       // gaze-in-head motion per frame treated as a saccade
  int saccadeHoldFrames = 6;     // frames without exclusion after a saccade
  double focusRatio = 0.80;      // focus half-angle must be below ratio * peripheral
  double apexTolerance = 0.02;   // metres between focus and peripheral eye position
  int collectThreadsMax = 0;     // 0 = leave DCS default
  int statsIntervalSec = 2;
  int dumpViews = 3;             // number of view dumps written to the log
  int benchBlocks = 24;          // A/B benchmark: number of blocks (even)
  int benchBlockSec = 5;
  double benchSettleSec = 1.0;   // ignored time at the start of each block
  int benchAutoStartSec = 0;     // >0: start automatically this long after quad views appear
  int benchThreadsB = 12;
  int profileSeconds = 20;
  int profileThreads = 24;
  int profilePeriodMs = 2;
  bool suiteProfile = false;
  bool suiteSelfTest = false;
  bool suiteBenchPartition = false;
  bool suiteBenchIsolation = false;
  bool suiteBenchTimerRes = false;
  bool diagnostics = false;      // measurement hooks always on
  int toggleVk = 122;            // [Hotkeys] Toggle, default Alt+Shift+F11
  int toggleMods = 6;
  bool developerKeys = false;    // Ctrl+Alt+F8/F9/F10/F12/PgUp/PgDn
  bool shadowTight = false;      // tighter cascade caster culling (shadow_tight.h)
  bool shadowDebugInvert = false;
  double shadowMarginFrac = 0.03;
  double shadowMarginMin = 2.0;
  double shadowFloorY = -1000.0;  // lowest visible receiver height (m)
  bool suiteBenchShadow = false;
  bool allocSlabs = false;       // [Model] AllocSlabs, per-thread slabs for NGModel's instance-data allocator
  int slabBytes = 4096;
  bool suiteBenchAllocSlabs = false;
  bool texDedupe = false;        // [Texture] StreamDedupe
  bool suiteBenchTexDedupe = false;
  bool suiteBenchEngine = false;  // all measured optimizations vs stock DCS
  int motionSeconds = 60;         // run_motion.flag recording length
  bool suiteQuick = false;
  bool triPlain = false;          // [Model] PlainTriangleCounter
  bool suiteBenchTriPlain = false;
  bool cbSkip = false;            // [Effects] SkipSameConstantBuffer
  bool suiteBenchCbSkip = false;
  bool suiteBenchMicro = false;
  bool costWeights = false;       // [Scene] CostWeights
  bool suiteBenchCostWeights = false;
  bool suiteTerrain = false;
  bool cbUploadSkip = false;      // [Effects] SkipSameConstantUpload
  bool suiteBenchCbUpload = false;
  bool pacerLowPower = false;     // [General] LowPowerPacer
  int pacerTimeoutUs = 10;
  bool suiteBenchPacer = false;
  bool motionSweep = false;       // [Suite] MotionSweep: synthetic head yaw during run_motion.flag
  bool motionProfile = false;
  bool motionTaxi = false;        // [Suite] MotionTaxi
  bool taskClock = false;         // [Timing] TaskQueueClock
  bool frameHeapSlabs = false;    // [Model] FrameHeapSlabs
  bool shadowTexSkip = true;      // [Model] ShadowTextureSkip (shadow_tex.h; masks from shadow_inst compiles)
  bool suiteBenchShadowTex = false;  // [Suite] BenchShadowTex
  bool suiteBenchTexTable = false;   // [Suite] BenchTexTable (texture dedupe table layout)
  bool suiteBenchBigPages = false;   // [Suite] BenchBigPages
  bool bigPages = true;              // [Model] BigModelPages (big_pages.h)
  bool parUpload = false;            // [Model] ParallelUpload (par_upload.h)
  bool suiteBenchParUpload = false;  // [Suite] BenchParallelUpload
  bool directUpload = false;            // [Model] DirectUpload (direct_upload.h, R16)
  bool suiteDirectUploadVerify = false;  // [Suite] DirectUploadVerify
  bool suiteBenchDirectUpload = false;   // [Suite] BenchDirectUpload
  bool motionCounters = true;        // [Suite] MotionCounters
  bool shadowInst = true;            // [Model] ShadowInstancing (shadow_inst.h: compiles the instanced shadow VS variants)
  bool suiteShadowInstCompile = false;  // [Suite] ShadowInstCompile
  bool suiteGBufferInstCompile = false;  // [Suite] GBufferInstCompile (R13 stage 1: main-pass model_vs variants)
  bool suiteGBufferTexCount = false;     // [Suite] GBufferTexCount (R14 lead 3: unread G-buffer texture sets)
  bool suiteFxApplyCount = false;        // [Suite] FxApplyCount (R14 lead 4: same-pass FX Apply counter)
  bool suiteDirectUploadCount = false;   // [Suite] DirectUploadCount (R16 gates G1-G3 for direct page upload)
  bool suiteSrvSpanCount = false;        // [Suite] SrvSpanCount (R15 F2: setShaderResources identical-tail counter)
  bool suiteJoinTailCount = false;       // [Suite] JoinTailCount (R15 F3: render-thread chunk tail at the culling join)
  bool suiteShadowRecCount = false;      // [Suite] ShadowRecCount (R17 S0: shadow recorder counters)
  bool suiteGBufferRecCount = false;     // [Suite] GBufferRecCount (R18 S0: G-buffer recorder counters and gates)
  bool suiteForwardRecCount = false;     // [Suite] ForwardRecCount (R21 S0: SimplePassData recorder counters and gates)
  bool suiteGpuPassTiming = false;       // [Suite] GpuPassTiming (gpu_pass_timing.h: GPU ms and context ops per pass)
  bool suiteGpuPassStats = false;        // [Suite] GpuPassStats (with GpuPassTiming: pipeline statistics and samples passed per pass)
  int suiteGpuSplit = 0;                 // [Suite] GpuSplit (gpu_split.h: 1 = recorder replay brackets with GpuPassTiming, 2 = also the mesh census)
  int suiteGpuSplitFrames = 60;          // [Suite] GpuSplitFrames (census frames)
  int suiteGapAttrib = 0;                // [Suite] GapAttrib (gap_attrib.h: between-pass GPU gap attribution, seconds; 0 = off)
  int suiteGapAttribCalEvery = 40;       // [Suite] GapAttribCalEvery (frames between GPU clock calibrations; 0 = none)
  int suiteLodProbe = 0;                 // [Suite] LodProbe (lod_probe.h: LOSSY test, instancer LOD factors x percent/100 for one timing run; 0 = off)
  bool suiteGapAttribDump = false;       // [Suite] GapAttribDump (1 = gap_passes.csv in the dev folder: pass intervals on the QPC clock)
  int suiteGpuDedup = 0;                 // [Suite] GpuDedup (gpu_dedup.h: exact-redundancy census, frames captured; 0 = off)
  int suiteGpuDedupWarm = 30;            // [Suite] GpuDedupWarm (frames of content-id tracking before the capture)
  int suiteGpuDedupHashCap = 16384;      // [Suite] GpuDedupHashCap (largest mapped buffer hashed, bytes)
  bool suiteGpuDedupSnap = false;        // [Suite] GpuDedupSnap (1 = also write the cascades' culled instance buffers)
  int peripheralLodScale = 0;            // [Model] PeripheralLodScale (periph_lod.h: LOSSY prototype, periphery instancer LOD factors x percent/100; 0 = off)
  int suiteFocusGeomProbe = 0;           // [Suite] FocusGeomProbe (focus_bcast.h, R38 rank 2: view mask whose main GBufferPass runs geometry-only, timing A/B; 0 = off)
  int focusBroadcast = 0;                // [D3D] FocusBroadcast (focus_bcast.h, R38 rank 2: 1 = readiness check only, nothing bound; 0 = off)
  int suiteUavOverlap = 0;               // [Suite] UavOverlap (uav_overlap.h: 1 = culling-chain passes, 2 = all passes inside NVAPI UAV overlap, 3/4 = 1/3 renamed duplicates of every chain dispatch; timing A/B; 0 = off)
  int suitePeriphLodTiming = 0;          // [Suite] PeriphLodTiming (periph_lod.h: GPU timing OFF/ON x percent; 0 = off)
  int suitePeriphLodImage = 0;           // [Suite] PeriphLodImage (periph_lod.h: frames of final per-view images per configuration; 0 = off)
  bool suiteBenchPeripheralLod = false;  // [Suite] BenchPeripheralLod (bench mode 32, scale = PeriphLodTiming or 50)
  bool cascadeCache = false;             // [Shadows] CascadeCache (cascade_cache.h, R33: PROTOTYPE far-cascade static depth cache; 0 = off, nothing installed)
  uint32_t cascadeCacheMask = 0xc;       // [Shadows] CascadeCacheMask: cascades by CascadeShadowPass number (bit 2 = about 70-370 m, bit 3 = about 370-1500 m; bit 0 always refreshes: its unsnapped box is never seen)
  int cascadeCacheMaxAge = 4;            // [Shadows] CascadeCacheMaxAge: frames one capture may serve (1 = refresh every frame)
  int cascadeCacheMarginPct = 3;         // [Shadows] CascadeCacheMarginPct: window margin per side, percent of DCS's cascade extent
  int cascadeCacheVerify = 0;            // [Shadows] CascadeCacheVerify: compare every Nth reuse with a full redraw (test only; 0 = off)
  bool suiteCascadeCacheTiming = false;  // [Suite] CascadeCacheTiming (cascade_cache.h: GPU timing OFF/ON/OFF/ON/OFF, 5 s each)
  uint32_t chainFast = 7;                // [D3D] ChainFast (chain_fast.h, R34: shorter culling chains; bit 0 calcoffsets, bit 1 calccounters, bit 2 airflow offsets; default 7; 0 = off, nothing installed)
  bool suiteChainFastTiming = false;     // [Suite] ChainFastTiming (chain_fast.h: GPU timing OFF/ON/OFF/ON/OFF, 5 s each)
  int suiteChainFastVerify = 0;          // [Suite] ChainFastVerify: seconds of same-frame checks against the stock kernels (0 = off)
  uint32_t shaderFast = 3;               // [D3D] ShaderFast (shader_fast.h, R36: wider grids for single-SM kernels; bit 0 grass cull, bit 1 IM2 counts clear; default 3; 0 = off)
  bool suiteShaderFastTiming = false;    // [Suite] ShaderFastTiming (shader_fast.h: GPU timing OFF/ON/OFF/ON/OFF, 5 s each)
  int suiteShaderFastVerify = 0;         // [Suite] ShaderFastVerify: seconds of same-frame checks against the stock kernels (0 = off)
  int nightTone = 0;                     // [Night] NightTone (night_tone.h, R37: opt-in night darkening; 0 = off, nothing installed; 1 apply; 2 observe)
  double nightFloor = 0, nightToe = 0, nightEv = 0;  // [Night] NightExposureFloor, NightToe, NightExposureEV (0 = DCS's value)
  bool nightGate = true;                 // [Night] NightGate: scale by the night weight from the sun elevation
  bool suiteVramCount = false;           // [Suite] VramCount (vram_count.h: DXGI budget/usage, our memory, texture tables)
  int suiteVramCountSec = 20;            // [Suite] VramCountSec
  bool suiteVramCountCreates = true;     // [Suite] VramCountCreates: count DCS's buffer/texture creates during the phase
  // R49: default 1 = the shipped setting (DCS Control writes SplitFilter=1; ChainFast and ShaderFast rely on the split
  // filter's hooks). Before R49 the code default was 0, so an ini without the key ran without them.
  bool splitFilter = true;               // [D3D] SplitFilter (split_filter.h, R15 F5)
  uint32_t splitFilterOps = 0x4ff;       // [D3D] SplitFilterOps (sfilt::kDefaultOps: no blend, no depth)
  bool suiteSplitFilterVerify = false;   // [Suite] SplitFilterVerify
  bool suiteBenchSplitFilter = false;    // [Suite] BenchSplitFilter (bench mode 27)
  bool shadowBatch = true;           // [Model] ShadowBatching (shadow_batch.h; needs ShadowInstancing)
  bool suiteShadowInstVerify = false;   // [Suite] ShadowInstVerify
  bool suiteBenchShadowInst = false;    // [Suite] BenchShadowInst
  bool shadowPlanAsync = true;          // [Model] ShadowPlanAsync (shadow batching plans on planner threads)
  bool suiteBenchShadowPlanAsync = false;  // [Suite] BenchShadowPlanAsync
  bool shadowRecorder = false;          // [Model] ShadowRecorder (shadow_rec.h, R17 S3)
  uint32_t shadowRecorderScope = 0x101; // [Model] ShadowRecorderScope: bits 0-3 cascades, 8 untextured, 9 textured (0x30f: all)
  int shadowRecorderWaitUs = 200;       // [Model] ShadowRecorderWaitUs: render-thread wait for a job at the pass
  int shadowRecorderPriority = 0;       // [Model] ShadowRecorderPriority: worker THREAD_PRIORITY_* (-2..2)
  uint32_t shadowRecorderSplit = 0xf;   // [Model] ShadowRecorderSplit: cascades with helper workers (bits 0-3)
  int shadowRecorderHelpers = 3;        // [Model] ShadowRecorderHelpers: at most this many helpers per job (0-3)
  bool shadowRecorderInstancing = true;  // [Model] ShadowRecorderInstancing
  bool suiteShadowRecVerify = false;    // [Suite] ShadowRecVerify
  int suiteShadowRecVerifySec = 5;      // [Suite] ShadowRecVerifySec
  bool suiteBenchShadowRecorder = false;  // [Suite] BenchShadowRecorder (bench mode 28)
  bool gbufferBatch = false;            // [Model] GBufferBatching (gb_batch.h; needs ShadowInstancing)
  bool suiteGBufferInstVerify = false;  // [Suite] GBufferInstVerify
  bool suiteBenchGBufferInst = false;   // [Suite] BenchGBufferInst
  bool gbufferRecorder = false;            // [Model] GBufferRecorder (gb_rec.h, R18 S1-S3)
  uint32_t gbufferRecorderScope = 0x10004; // [Model] GBufferRecorderScope: bits 0-15 executions by ordinal, 16 A2C
  int gbufferRecorderWaitUs = 300;         // [Model] GBufferRecorderWaitUs: render-thread wait for a job at the pass
  int gbufferRecorderIsland = 30;          // [Model] GBufferRecorderIsland: shortest recorded run
  int gbufferRecorderMaxSegments = 12;     // [Model] GBufferRecorderMaxSegments: lists per execution (1-16)
  int gbufferRecorderHelpers = 2;          // [Model] GBufferRecorderHelpers: helper workers per job (0-2)
  bool gbufferRecorderCockpit = false;     // [Model] GBufferRecorderCockpit: cockpit draws and executions (R24)
  bool gbufferRecorderByOrdinal = false;   // [Model] GBufferRecorderByOrdinal: slots by plain ordinal (before R24)
  bool suiteGBufferRecVerify = false;      // [Suite] GBufferRecVerify
  int suiteGBufferRecVerifySec = 6;        // [Suite] GBufferRecVerifySec
  bool suiteGBufferRecVerifyLive = false;  // [Suite] GBufferRecVerifyLive: test only, the recorder's verify while flying
  bool suiteGBufferRecVerifyNoLatch = false;  // [Suite] GBufferRecVerifyNoLatch: test only, differences logged only
  int suiteGBufferRecVerifyStride = 0;     // [Suite] GBufferRecVerifyStride: every N-th draw residual (R18 T10)
  bool suiteBenchGBufferRecorder = false;  // [Suite] BenchGBufferRecorder (bench mode 29)
  uint32_t passFlush = 0;                  // [Model] PassFlush: pass_flush.h mask (0 = off)
  bool suiteBenchPassFlush = false;        // [Suite] BenchPassFlush (bench mode 30)
  uint32_t suiteBenchPassFlushMask = 0;    // [Suite] BenchPassFlushMask: the ON mask (0 = [Model] PassFlush)
  bool srvTailTrim = false;                // [Model] SrvTailTrim (srv_tail_trim.h, R15 F2)
  bool suiteSrvTailTrimVerify = false;     // [Suite] SrvTailTrimVerify
  int suiteSrvTailTrimVerifySec = 10;      // [Suite] SrvTailTrimVerifySec
  bool suiteBenchSrvTailTrim = false;      // [Suite] BenchSrvTailTrim (bench mode 31)
  int prefetchMode = 0;                    // [Prefetch] Mode: off (0) | observe (1) | skip (2), prefetch_fix.h
  uint32_t prefetchWindowMs = 1000;        // [Prefetch] WindowMs: repeat window (1-60000)
  bool suiteBenchPrefetch = false;         // [Suite] BenchPrefetch (bench mode 33: observe vs skip)
  int prefetchTableBits = 20;              // [Prefetch] TableBits: repeat table 2^n entries (16-22, default 20; R39)
  bool threadCensus = false;               // [Threads] Census: per-thread CPU/affinity lines (thread_place.h, R39)
  int threadCensusSec = 60;                // [Threads] CensusSec (10-3600)
  int placeRecheckSec = 60;                // [Threads] PlaceRecheckSec: placement re-check once nothing moved (R52; R44 used 10 s)
  int placeRecheckMode = 1;                // [Threads] PlaceRecheckMode: 1 tracked handles, no system-wide snapshot (R90 default); 0 the snapshot pass
  int patchMode = 1;                       // [General] PatchMode: 1 code-site writes without the system-wide snapshot (R90 default); 0 the Toolhelp path
  bool asyncLog = true;                    // [Log] Async: DCS threads' log lines through the async ring (R90 default); 0 synchronous
  int shadowPlanWaitUs = 300;              // [Model] ShadowPlanWaitUs: render-thread wait for a planner's plan before the inline one (R90 H1; 20000 = pre-R90)
  int frameTimeline = 0;                   // [Perf] FrameTimeline: 0 off, 1 summaries, 2 + per-frame CSV (frame_timeline.h, R52)
  int timelineSec = 10;                    // [Perf] TimelineSec: summary window (2-600 s)
  bool poseProbe = false;                  // [Perf] PoseProbe: head pose re-located at xrBeginFrame / xrEndFrame (R52)
  bool eventLog = false;                   // [Perf] EventLog: worker tasks, device creates, page faults per frame (R52)
  int workerPlacement = 2;                 // [Threads] WorkerPlacement: 0 off, 1 all our threads off DCS's render core (R39; R41 §9 measured it slower), 2 selective (R44 default): threads off the render path moved, recorder workers / planners / upload helpers on the whole process mask
  int holdYawDeg = -1;               // [Suite] HoldYawDeg: constant view yaw for unattended runs (-1 = off)
  double holdPitchDeg = 0;           // [Suite] HoldPitchDeg: test only, constant view pitch with the held yaw (down +)
  bool holdPose = false;             // [Suite] HoldPose: test only, the head pose frozen (pose_sweep.h)
  bool poseWallClock = false;        // [Suite] PoseWallClock: test only, the synthetic yaw from the wall clock at each locate (pre-R99) instead of its displayTime
  double poseRotateDps = 0;          // [Suite] PoseRotateDps: test only, constant-rate synthetic yaw turn (0 = off)
  bool suiteYawScan = false;         // [Suite] YawScan: fps per held yaw, 0..345 in 15 deg steps
  bool suiteYawProfile = false;      // [Suite] YawProfile: cost split and bound per held yaw (view_profile.h)
  int suiteYawProfileStep = 30;      // [Suite] YawProfileStep: degrees between profiled yaws
  bool suiteRotationProfile = false;  // [Suite] RotationProfile: cost split, transients and spikes while turning
  int suiteRotationDegPerSec = 60;    // [Suite] RotationDegPerSec
  int suiteRotationSeconds = 20;      // [Suite] RotationSeconds
  bool suiteFrameStartGap = false;    // [Suite] FrameStartGap: frame-start GPU bubble attribution (frame_start.h, R22 E1)
  int suiteFrameStartGapSec = 10;     // [Suite] FrameStartGapSec: standalone phase (0 = only inside Yaw/RotationProfile)
  bool suiteRunnableThreads = false;  // [Suite] RunnableThreads: runnable-waiting DCS threads (run_threads.h, R22 E3)
  int suiteRunnableThreadsSec = 10;   // [Suite] RunnableThreadsSec: standalone phase (0 = only inside the profiles)
  int suiteRunnableThreadsEvery = 16; // [Suite] RunnableThreadsEvery: frames between snapshots
  uint32_t bigPageBytes = 4u << 20;  // [Model] BigPageBytes
  bool suiteBenchFrameHeap = false;     // [Suite] MotionProfile: CPU profile of the sweep's translation phase        // [Suite] Quick: configuration check + A/B only
  int suiteBenchFilter = 0;  // 1 = end-to-end A/B, 2 = also filter vs hooks
  bool d3dFilter = false;        // skip redundant D3D11 state calls
  bool fineTimer = false;
  bool partitionBoost = false;
  bool suiteBenchCull = false;
  bool suiteBenchThreads = false;
  bool suiteBenchTimer = false;
  bool d3dMeter = false;         // [D3D] Meter: count redundant D3D11 state calls (diagnostic)
  bool shaderTimeCache = true;   // cache dx11backend's per-draw ED_get_time
  int shaderTimeCacheUs = 1000;
  // R30 field diagnostics (perf_mon.h) and the GPU-bound governor (gov.h).
  bool recorderDetail = false;   // [Log] RecorderDetail: the recorders' full 10-s blocks (else one line each)
  int perfIntervalSec = 10;      // [Log] PerfIntervalSec: perf/features lines (0 = off)
  int vramIntervalSec = 60;      // [Log] VramIntervalSec: vram line (0 = off)
  bool govEnabled = true;        // [Governor] Enabled
  int govGpuBusyPct = 92;        // [Governor] GpuBusyPct: GPU busy at or above = the GPU is full
  int govRenderIdlePct = 85;     // [Governor] RenderIdlePct: render thread busy at or below = it waits
  int govCpuBusyPct = 92;        // [Governor] CpuBusyPct: render thread busy at or above = CPU-bound
  int govGpuFreePct = 80;        // [Governor] GpuFreePct: GPU busy below = the GPU has headroom
  int govGpuFullPct = 98;        // [Governor] GpuFullPct: GPU busy at or above = GPU-bound (render thread may spin-wait)
  int govWindowSec = 8;          // [Governor] WindowSec: verdicts considered
  int govNeedSec = 6;            // [Governor] NeedSec: of them needed for a step
  int govDwellSec = 10;          // [Governor] MinDwellSec: minimum time per stage
  double govMinGainPct = 2.0;    // [Governor] MinGainPct: fps gain a step down must show (and a step up may lose at most)
  int govTrialSec = 20;          // [Governor] TrialSec: fps window before and after a step (after a 2 s settle)
  int govRecheckSec = 10;        // [Governor] RecheckSec: the old stage again for this long (A-B-A; 0 = before/after only)
  int govRevertBackoffSec = 300; // [Governor] RevertBackoffSec: a reverted stage is blocked this long (doubling)
  // Depth boost's fixation depth (fixation_depth.h). Not an optimization:
  // outside the kill switch and the status word's features.
  bool fixationDepth = false;     // [Perception] FixationDepth: publish Local\DcsVrFixationDepth_v1
  bool sceneCutReset = true;      // [Perception] SceneCutReset: reset Quad Views' temporal history on camera cuts
  bool depthGrid = false;         // [Perception] DepthGrid: publish Local\DcsVrDepthGrid_v1 (32 x 32 grid of the left periphery)
  int fixationDepthVerify = 0;    // [Perception] FixationDepthVerify: sample every view run in turn, log the view map
  bool fixationDepthDump = false; // [Perception] FixationDepthDump: one window as a PGM in the dev folder
  std::wstring fixationTestGaze;  // [Perception] FixationDepthTestGaze: "x,y" tangents written as GazeOut if absent
  bool farDepthExport = false;    // [Perception] FarDepthExport: focus views' depth to Quad Views (when it wants them)
  int farDepthSlots = 2;          // [Perception] FarDepthExportSlots: copies per focus view (1 or 2)
  int farDepthLogProj = 0;        // [Perception] FarDepthLogProj: log the focus views' P20/P21 for N frames
  bool farDepthPreOverlay = true; // [Perception] FarDepthPreOverlay: each focus view's colour before the overlays
  int farDepthCoverageMode = 1;   // [Perception] FarDepthCoverageMode: hdr (1, the default; per-view fallback to overlay) or overlay (0)
  bool farDepthPostOverlay = true; // [Perception] FarDepthPostOverlay: also each focus view's colour after the overlays (at (w, 0))
  int farDepthOverlayTrace = 0;   // [Perception] FarDepthOverlayTrace: log the focus views' tone-map..overlay draws for N frames
  // R65 cockpit mask export (cockpit_mask.h, contract dcsvr_cockpit_mask.h): per HMD view, where DCS drew cockpit
  // geometry, for a frame-generation layer. Off by default.
  bool cockpitMaskExport = false;  // [Perception] CockpitMaskExport
  int cockpitMaskDivisor = 0;      // [Perception] CockpitMaskDivisor: source texels per mask texel (1-8; 0 auto: <= 512 wide)
  int cockpitMaskViews = 3;        // [Perception] CockpitMaskViews: HMD view bit mask (3 periphery, 15 + focus)
  double cockpitMaskNearM = 1.0;   // [Perception] CockpitMaskNearM: also nearer than this (m; 0 = stencil only)
  int cockpitMaskDilate = 0;       // [Perception] CockpitMaskDilate: extra mask texels of max filter (0-4)
  int cockpitMaskDump = 0;         // [Perception] CockpitMaskDump: write N committed masks as PGM (debug)
  int cockpitMaskLog = 0;          // [Perception] CockpitMaskLog: log N commits (displayTime pairing check)
  bool cockpitMaskOverlay = false; // [Perception] CockpitMaskOverlay (R74b): mask v1.1, 128 = overlay symbology (opt-in)
  bool cockpitDepthExport = false; // [Perception] CockpitDepthExport (R69): the mask's depth companion (needs the mask)
  int cockpitDepthDump = 0;        // [Perception] CockpitDepthDump: write N committed depth atlases (debug)
  bool cockpitMotionExport = false; // [Perception] CockpitMotionExport (R70): DCS's MVs of views 0/1 (needs the mask)
  bool cockpitMotionClass = true;   // [Perception] CockpitMotionClass: the class byte atlas with the MVs
  int cockpitMotionDump = 0;        // [Perception] CockpitMotionDump: write N committed motion slots (debug)
  int cockpitFgDump = 0;            // [Perception] CockpitFgDump (R71): N consecutive frames of MV / class / depth /
                                    // colour + JSON for frame-generation fixtures (dev; needs mask, depth, motion)
  bool symbologyExport = false;     // [Perception] SymbologyExport (R95): HUD / helmet emission companion (needs the mask)
  bool symbologyAlpha = true;       // [Perception] SymbologyAlpha (R95): its coverage alpha (default 1)
  bool symbologyPublishAlways = false;  // [Dev] SymbologyPublishAlways (R97): without the symbology reader's heartbeat
  // [Perception] CockpitTrueIpd* (cockpit_ipd.h, R31 option 3): cockpit passes at the true eye distance while
  // Depth boost renders the world at the boosted one. CockpitTrueIpd defaults off (the app's ini turns it on);
  // Forward, Helmet, Overlays and OverlayMvp default on (they act only with CockpitTrueIpd), the rest off / neutral.
  int cockpitTrueIpd = 0;          // CockpitTrueIpd: 0 off, 1 on, 2 observe (armed, nothing modified, logs only)
  bool cockpitTrueIpdLighting = false;  // CockpitTrueIpdLighting: phase 2, cockpit-stencil pixels lit from the true eye
  bool cockpitTrueIpdForward = true;    // CockpitTrueIpdForward: also TransparentCockpitPass / ...DecalPass
  bool cockpitTrueIpdHelmet = true;     // CockpitTrueIpdHelmet: helmet models drawn with their eye's camera
  int cockpitTrueIpdOwnship = 0;        // CockpitTrueIpdOwnship: the own aircraft's exterior model at the true eye
  bool cockpitTrueIpdOverlays = true;   // CockpitTrueIpdOverlays: also the per-view overlay passes (HUD / MFD symbology)
  bool cockpitTrueIpdOverlayMvp = true; // CockpitTrueIpdOverlayMvp: in them, the effect buffers' CPU-built MVPs too
  int cockpitTrueIpdLog = 0;       // CockpitTrueIpdLog: per-upload trace for the next N frames
  int cockpitTrueIpdPrevLog = 0;   // CockpitTrueIpdPrevLog: gPrevFrameViewProj trace for the next N frames
  int cockpitTrueIpdFrameDump = 0;  // CockpitTrueIpdFrameDump: dev only, N frames of HMD views 2 / 3 as BMP + CSV
  int cockpitTrueIpdMvpDump = 0;    // CockpitTrueIpdMvpDump: dev only, the first N overlay matrices, one line each
  int cockpitTrueIpdFrameDumpViews = 0x0C;  // CockpitTrueIpdFrameDumpViews: digits 0-3 and m (mirror), default 23
  bool cockpitTrueIpdOverlayRestore = false;  // CockpitTrueIpdOverlayRestore: effect buffers back to DCS's bytes per pass
  int cockpitTrueIpdOverlayPath = 0;  // CockpitTrueIpdOverlayPath: cpu (0, default, R32) or d3d11 (1, A/B only)
  double cockpitTrueIpdTestShiftMm = 0;  // CockpitTrueIpdTestShiftMm: dev only, shift per eye without the layer
  int cockpitTrueIpdToggleVk = 118;  // CockpitTrueIpdToggle: in-flight A/B, default Alt+Shift+F7 (118:6); 0:0 off
  int cockpitTrueIpdToggleMods = 6;
};

Config g_cfg;
bool g_mute = false;  // set by the offline tests

// Audible cue for the user in the headset (Beep is synchronous).
std::atomic<bool> g_beeps{true};  // [General] Beeps
void Chime(DWORD freq, DWORD ms) {
  if (!g_mute && g_beeps.load(std::memory_order_relaxed)) Beep(freq, ms);
}
double g_tscHz = 0;
void ApplyTimerConfig();
void BenchUseCompactTexTable(bool on);
void ApplyBigPages(bool on);
void ApplyParUpload(bool on);
void ApplyDirectUpload(bool on);
void ApplySplitFilter(bool on);
void ApplyTextureHook();
void ApplyTriCounter();
void ApplyCbSkip();
void ApplyCbUpload();
void ApplyPacer();
void ApplyTaskClock();
void ApplyFrameHeap();
void ApplyShadowTexSkip();
void ApplyShadowInst();
void ApplyShadowBatch();
void ApplyGBufferBatch();
void ApplyShadowRecorder();
void ApplyGBufferRecorder();
void ApplyPassFlush();
void ApplySrvTailTrim(bool on);
void ApplyPrefetch();
void ApplyHoldYaw(int deg);
void ApplyPoseTest(double pitchDeg, double rateDps, bool holdPose, bool wallClock);
void ApplyR90Config();  // R90: PatchMode, [Log] Async (main.cpp, after patch_threads.h)
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_debugHole{false};
std::atomic<bool> g_faulted{false};
FILETIME g_iniTime{};
std::atomic<bool> g_benchRunningFlag{false};
std::atomic<bool> g_partitionBoost{false};  // see HookCollect
// In-flight kill switch ([Hotkeys] Toggle): true turns off, together, the
// measured optimizations the ini enabled. Reset whenever the ini changes.
std::atomic<bool> g_engineOff{false};
// [General] PerceptionOnly=1 (default 0): the app installs DcsQvCull for Depth
// boost alone (Engine Optimizations off). Latched by the initial config read
// (a later ini change takes effect at the next DCS start). While set, only the
// perception features are installed: fixation depth / DepthGrid, the far
// depth export, CockpitTrueIpd (option 3, its R32 CPU path, StereoAck,
// BoostOut) and CockpitTrueIpdOwnship (stage 1: the G-buffer recorder is off).
// No engine optimization, recorder, benchmark, suite, diagnostics hook,
// governor action, engine toggle or Ctrl+Alt key; DcsQvCull_Status reports
// running + PerceptionOnly only (R62, status_word.h bit 38).
// The hooks it keeps, all pass-through or observing:
//   - Scene.dll collectSceneObjectsRenderables (HookCollect): view
//     classification only, for the eye separation (NoteCullSeparation,
//     option 3's measured-separation record pick) and the camera cut
//     (SceneCutReset); no volume patched, no thread limit touched.
//   - GraphicsCore BaseRenderingPass::execute (pass_timing.h): fixation
//     depth's pass-boundary callback; no timing recorded.
//   - G-buffer pass execute (gb_batch.h gbpass): option 3's observer, no
//     batching wrapper.
//   - DX11Renderer::draw (perception_device.h): one call, to learn DCS's
//     D3D11 context (shadow instancing / the split filter found it before);
//     the slot is restored at once.
//   - The perception modules' own hooks (fixation_depth.h, cockpit_ipd.h,
//     ovl_cpu.h, far_depth.h), as with PerceptionOnly=0.
std::atomic<bool> g_perceptionOnly{false};
// R30 governor (gov.h, perf_mon.h): the recorders' stage (0 = as configured,
// 1 = shadow split & 0x8, 2 = no helper lists, 3 = recorders paused), read by
// ApplyShadowRecorder/ApplyGBufferRecorder; the last verdict and the
// shader-variant warm-up for the status word.
std::atomic<int> g_govStage{0};
std::atomic<int> g_verdictNow{0};
std::atomic<bool> g_warming{false};
qvstatus::Feature g_featLast[18] = {};  // ComputeStatus's last states (the features line)
void PerfOnToggle(bool off);

// Parses "virtual-key:modifiers" (decimal; modifiers Ctrl 1, Alt 2, Shift 4).
// "0:0" disables the key. Returns false for anything else malformed or out of
// range (key 1-254, modifiers 0-7), leaving vk = mods = 0.
bool ParseHotkey(const wchar_t* text, int& vk, int& mods) {
  vk = mods = 0;
  if (!text) return false;
  wchar_t* end = nullptr;
  long k = wcstol(text, &end, 10);
  if (end == text || *end != L':') return false;
  const wchar_t* m0 = end + 1;
  long m = wcstol(m0, &end, 10);
  if (end == m0) return false;
  while (*end == L' ' || *end == L'\t') ++end;
  if (*end != 0) return false;
  if (k == 0 && m == 0) return true;  // explicitly off
  if (k < 1 || k > 254 || m < 0 || m > 7) return false;
  vk = static_cast<int>(k);
  mods = static_cast<int>(m);
  return true;
}
// R42 colour modes (color_pass.h): the ini's [Color] values (config thread) and the ModeKey's choice
// (-1 = none; it cycles 0 -> 1 -> 2 -> 0 until the ini's Mode changes).
cpass::Cfg g_colorCfg;
int g_colorKeyVk = 0, g_colorKeyMods = 0;  // [Color] ModeKey, off unless set
std::atomic<int> g_colorKeyMode{-1};
std::atomic<int> g_d3dMode{0};  // D3D11 filter mode, see d3dstate.h
std::atomic<bool> g_shadowTight{false};        // shadow_tight.h
std::atomic<bool> g_shadowDebugInvert{false};  // draw only the casters that would be removed
std::atomic<uint64_t> g_frameShadowRend{0};    // cascade renderables of the last quad frame (benchmark)
std::atomic<bool> g_allocSlabsOn{false};       // alloc_slab.h
std::atomic<uint32_t> g_allocSlabBytes{4096};
std::atomic<bool> g_texDedupeOn{false};        // tex_bind.h
std::atomic<bool> g_hookMeasure{false};
std::atomic<bool> g_triPlainOn{false};         // tricount (alloc_slab.h)
std::atomic<bool> g_cbSkipOn{false};           // cbskip (tex_bind.h)
std::atomic<bool> g_costWeightsOn{false};      // part_weights.h
std::atomic<bool> g_costWeightsSanity{false};
std::atomic<bool> g_cbUploadSkipOn{false};     // cbupload (tex_bind.h)
std::atomic<bool> g_pacerLowPowerOn{false};    // pacer.h
std::atomic<bool> g_taskClockOn{false};        // edtime.h
std::atomic<bool> g_frameHeapOn{false};        // frame_heap.h
// Frame-heap generation: +1 at every TFrameMemoryHeap<1>::reset_memory (frame_heap.h's
// hook). The renderables the recorders' workers read live in that heap: a job whose
// generation changed reads recycled memory (gb_rec.h HeapRecycled).
std::atomic<uint64_t> g_frameHeapGen{1};
// QPC of the last scene change: the G-buffer executions moved to another
// render graph (gb_rec.h), or the Lua hook reported a simulation stop, a
// mission load or a local player slot change. DCS frees the previous scene's
// objects around it while frames still render (no heap reset in between), so
// a recorder fault in a job started near it is contained per job
// (shrec::SceneChanging) instead of latching the recorder off.
std::atomic<int64_t> g_sceneChangeQpc{0};
std::atomic<bool> g_shadowTexSkipOn{false};    // shadow_tex.h        // time the allocator/texture hooks (suite counters only)

// System timer resolution for the DCS process (NtSetTimerResolution, 100 ns
// units). DCS reports 15.5 ms; Sleep(1) then lasts up to a full tick.
ULONG g_timerResCurrent = 0;
bool SetTimerResolution(bool fine) {
  using Fn = LONG(NTAPI*)(ULONG, BOOLEAN, PULONG);
  static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetTimerResolution"));
  if (!fn) return false;
  ULONG actual = 0;
  LONG st = fn(5000, fine ? TRUE : FALSE, &actual);
  g_timerResCurrent = actual;
  return st == 0;
}
// OpenXR session state tracked from dcs.log by the worker thread.
std::atomic<bool> g_xrFocused{true};
std::atomic<uint64_t> g_focusLosses{0};

double ReadDouble(const wchar_t* sec, const wchar_t* key, double def, const std::wstring& ini) {
  wchar_t buf[64];
  wchar_t defStr[64];
  swprintf_s(defStr, L"%g", def);
  GetPrivateProfileStringW(sec, key, defStr, buf, 64, ini.c_str());
  return _wtof(buf);
}

// Dev mode: [Dev] IniPath in the deployed ini names a developer ini that
// replaces it entirely (both are watched); run_suite.flag is also accepted
// next to that file.
std::wstring g_devDir;
FILETIME g_devIniTime{};

// [Perception] FarDepthCoverageMode: "overlay" -> 0; absent, "hdr" or anything
// else -> 1 (hdr, far_depth.h 5d/5e); "anything else" sets *invalid.
int ParseFarCoverageMode(const wchar_t* m, bool* invalid) {
  *invalid = false;
  if (_wcsicmp(m, L"overlay") == 0) return 0;
  if (m[0] && _wcsicmp(m, L"hdr") != 0) *invalid = true;
  return 1;
}
bool g_farCovInvalidLogged = false;  // the invalid value is logged once per process
int g_farCovInvalidLogs = 0;

// [General] PerceptionOnly=1: the configuration that is applied. A fresh
// Config (every suite, benchmark and diagnostics key off) with the engine
// optimizations whose defaults are on switched off, the engine keys off, and
// the [Perception] keys, the view classification thresholds and the log
// cadence taken from the ini. A new engine feature whose Config default is
// on must be added to the list below.
Config PerceptionOnlyConfig(const Config& c) {
  Config p;
  p.perceptionOnly = true;
  // Engine optimizations on by default.
  p.shadowTexSkip = p.bigPages = p.shadowInst = p.shadowBatch = p.shadowPlanAsync = false;
  p.shaderTimeCache = false;
  p.govEnabled = false;
  p.motionCounters = false;
  // R48: the R34 / R36 / R44 defaults that are on (ChainFast=7, ShaderFast=3, WorkerPlacement=2) were missing from
  // this list. ChainFast / ShaderFast were held off by the worker's PerceptionOnly guard only, and WorkerPlacement
  // (tplace::Tick, not guarded) only by the render thread staying unknown without the engine recorders.
  p.chainFast = p.shaderFast = 0;
  p.workerPlacement = 0;
  p.splitFilter = false;  // R49: its Config default is now on (the shipped setting)
  // The engine toggle and the Ctrl+Alt keys (inert in the worker regardless).
  p.toggleVk = p.toggleMods = 0;
  p.developerKeys = false;
  // View classification (HookCollect: eye separation, camera cuts); [Cull] Enabled stays off.
  p.keepFraction = c.keepFraction;
  p.safetyNdc = c.safetyNdc;
  p.nativeKeepFraction = c.nativeKeepFraction;
  p.saccadeDeg = c.saccadeDeg;
  p.saccadeHoldFrames = c.saccadeHoldFrames;
  p.focusRatio = c.focusRatio;
  p.apexTolerance = c.apexTolerance;
  // Log lines.
  p.statsIntervalSec = c.statsIntervalSec;
  p.dumpViews = c.dumpViews;
  p.perfIntervalSec = c.perfIntervalSec;
  p.vramIntervalSec = c.vramIntervalSec;
  // [Perception]
  p.fixationDepth = c.fixationDepth;
  p.sceneCutReset = c.sceneCutReset;
  p.depthGrid = c.depthGrid;
  p.fixationDepthVerify = c.fixationDepthVerify;
  p.fixationDepthDump = c.fixationDepthDump;
  p.fixationTestGaze = c.fixationTestGaze;
  p.farDepthExport = c.farDepthExport;
  p.farDepthSlots = c.farDepthSlots;
  p.farDepthLogProj = c.farDepthLogProj;
  p.farDepthPreOverlay = c.farDepthPreOverlay;
  p.farDepthCoverageMode = c.farDepthCoverageMode;
  p.farDepthPostOverlay = c.farDepthPostOverlay;
  p.farDepthOverlayTrace = c.farDepthOverlayTrace;
  p.cockpitMaskExport = c.cockpitMaskExport;  // R65
  p.cockpitMaskDivisor = c.cockpitMaskDivisor;
  p.cockpitMaskViews = c.cockpitMaskViews;
  p.cockpitMaskNearM = c.cockpitMaskNearM;
  p.cockpitMaskDilate = c.cockpitMaskDilate;
  p.cockpitMaskDump = c.cockpitMaskDump;
  p.cockpitMaskLog = c.cockpitMaskLog;
  p.cockpitMaskOverlay = c.cockpitMaskOverlay;  // R74b
  p.cockpitDepthExport = c.cockpitDepthExport;  // R69
  p.cockpitDepthDump = c.cockpitDepthDump;
  p.cockpitMotionExport = c.cockpitMotionExport;  // R70
  p.cockpitMotionClass = c.cockpitMotionClass;
  p.cockpitMotionDump = c.cockpitMotionDump;
  p.cockpitTrueIpd = c.cockpitTrueIpd;
  p.cockpitTrueIpdLighting = c.cockpitTrueIpdLighting;
  p.cockpitTrueIpdForward = c.cockpitTrueIpdForward;
  p.cockpitTrueIpdHelmet = c.cockpitTrueIpdHelmet;
  p.cockpitTrueIpdOwnship = c.cockpitTrueIpdOwnship;  // stage 2 runs as stage 1 (no G-buffer recorder)
  p.cockpitTrueIpdOverlays = c.cockpitTrueIpdOverlays;
  p.cockpitTrueIpdOverlayMvp = c.cockpitTrueIpdOverlayMvp;
  p.cockpitTrueIpdLog = c.cockpitTrueIpdLog;
  p.cockpitTrueIpdPrevLog = c.cockpitTrueIpdPrevLog;
  p.cockpitTrueIpdFrameDump = c.cockpitTrueIpdFrameDump;
  p.cockpitTrueIpdMvpDump = c.cockpitTrueIpdMvpDump;
  p.cockpitTrueIpdFrameDumpViews = c.cockpitTrueIpdFrameDumpViews;
  p.cockpitTrueIpdOverlayRestore = c.cockpitTrueIpdOverlayRestore;
  p.cockpitTrueIpdOverlayPath = c.cockpitTrueIpdOverlayPath;
  p.cockpitTrueIpdTestShiftMm = c.cockpitTrueIpdTestShiftMm;
  p.cockpitTrueIpdToggleVk = c.cockpitTrueIpdToggleVk;
  p.cockpitTrueIpdToggleMods = c.cockpitTrueIpdToggleMods;
  return p;
}

// [General] PerceptionOnly: every engine-side runtime flag off (LoadConfig
// applies nothing else in this mode).
void PerceptionOnlyFlags() {
  g_enabled = false;
  g_debugHole = false;
  g_partitionBoost = false;
  g_engineOff = false;
  g_allocSlabsOn = false;
  g_texDedupeOn = false;
  g_triPlainOn = false;
  g_cbSkipOn = false;
  g_costWeightsOn = false;
  g_cbUploadSkipOn = false;
  g_pacerLowPowerOn = false;
  g_taskClockOn = false;
  g_frameHeapOn = false;
  g_shadowTexSkipOn = false;
  g_shadowTight = false;
  g_shadowDebugInvert = false;
  g_d3dMode = 0;
}

namespace pifilt { void ReadConfig(const wchar_t* ini); }  // R38 rank 1 (periph_inst_filter.h)
namespace vsig { void ReadConfig(const wchar_t* ini); }    // R38 rank 2 G4 probe (vs_sig_probe.h)
namespace fbcr { void ReadConfig(const wchar_t* ini); }    // R53: FastGS focus broadcast (focus_bcast_run.h)
void LoadConfig(bool initial) {
  std::wstring ini = g_dir + L"DcsQvCull.ini";
  const std::wstring deployedIni = ini;  // R97: [Dev] keys may live only in the deployed ini (dev mirrors drop [Dev])
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  // A missing ini is read like an empty one: every key takes its fallback
  // below, and the same Apply* calls run. Before, the payload kept the bare
  // Config defaults (which had peripheral exclusion and the D3D meter on)
  // and applied none of the features. Parsed once until the file appears.
  static bool missing = false;
  if (!GetFileAttributesExW(ini.c_str(), GetFileExInfoStandard, &fad)) {
    if (missing && !initial) return;
    missing = true;
    Log("config: %ls not found, using defaults", ini.c_str());
    fad = WIN32_FILE_ATTRIBUTE_DATA{};
  } else {
    missing = false;
  }
  // R62: read whole (a path of 260+ characters was cut and dev mode stayed off silently); an unusable one is logged.
  const qvdev::DevPath devPath = qvdev::ReadDevPath(L"IniPath", ini);
  static std::wstring s_devIniReported;
  if (qvdev::ReportOnce(devPath, &s_devIniReported))
    Log("config: [Dev] IniPath ignored (%s): %ls; settings from the deployed ini", qvdev::Why(devPath),
        devPath.path.c_str());
  const wchar_t* devIni = devPath.path.c_str();
  WIN32_FILE_ATTRIBUTE_DATA dfad{};
  const bool dev = devPath.state == qvdev::kDevOk && GetFileAttributesExW(devIni, GetFileExInfoStandard, &dfad);
  const FILETIME devTime = dev ? dfad.ftLastWriteTime : FILETIME{};
  if (!initial && CompareFileTime(&fad.ftLastWriteTime, &g_iniTime) == 0 &&
      CompareFileTime(&devTime, &g_devIniTime) == 0)
    return;
  g_iniTime = fad.ftLastWriteTime;
  g_devIniTime = devTime;
  std::wstring devDir;
  if (dev) {
    ini = devIni;
    devDir = ini.substr(0, ini.find_last_of(L"\\/") + 1);
  }
  if (devDir != g_devDir) {
    g_devDir = devDir;
    if (dev) Log("config: dev mode, settings from %ls", ini.c_str());
    else if (!initial) Log("config: dev mode off, settings from the deployed ini");
  }

  Config c;
  c.enabled = GetPrivateProfileIntW(L"Cull", L"Enabled", 0, ini.c_str()) != 0;
  c.debugHole = GetPrivateProfileIntW(L"Cull", L"DebugHole", 0, ini.c_str()) != 0;
  c.keepFraction = ReadDouble(L"Cull", L"FocusKeepFraction", c.keepFraction, ini);
  c.safetyNdc = ReadDouble(L"Cull", L"SafetyNdc", c.safetyNdc, ini);
  c.nativeKeepFraction = ReadDouble(L"Cull", L"NativeKeepFraction", c.nativeKeepFraction, ini);
  c.saccadeDeg = ReadDouble(L"Cull", L"SaccadeDeg", c.saccadeDeg, ini);
  c.saccadeHoldFrames = GetPrivateProfileIntW(L"Cull", L"SaccadeHoldFrames", 6, ini.c_str());
  c.focusRatio = ReadDouble(L"Cull", L"FocusRatio", c.focusRatio, ini);
  c.apexTolerance = ReadDouble(L"Cull", L"ApexTolerance", c.apexTolerance, ini);
  c.collectThreadsMax = GetPrivateProfileIntW(L"Scene", L"CollectThreadsMax", 0, ini.c_str());
  c.statsIntervalSec = GetPrivateProfileIntW(L"Log", L"StatsIntervalSec", 2, ini.c_str());
  c.dumpViews = GetPrivateProfileIntW(L"Log", L"DumpViews", 3, ini.c_str());
  c.benchBlocks = GetPrivateProfileIntW(L"Bench", L"Blocks", c.benchBlocks, ini.c_str());
  c.benchBlockSec = GetPrivateProfileIntW(L"Bench", L"BlockSec", c.benchBlockSec, ini.c_str());
  c.benchSettleSec = ReadDouble(L"Bench", L"SettleSec", c.benchSettleSec, ini);
  c.benchAutoStartSec = GetPrivateProfileIntW(L"Bench", L"AutoStartSec", 0, ini.c_str());
  c.benchThreadsB = GetPrivateProfileIntW(L"Bench", L"ThreadsB", c.benchThreadsB, ini.c_str());
  c.suiteProfile = GetPrivateProfileIntW(L"Suite", L"Profile", 0, ini.c_str()) != 0;
  c.suiteSelfTest = GetPrivateProfileIntW(L"Suite", L"SelfTest", 0, ini.c_str()) != 0;
  c.suiteBenchPartition = GetPrivateProfileIntW(L"Suite", L"BenchPartition", 0, ini.c_str()) != 0;
  c.suiteBenchIsolation = GetPrivateProfileIntW(L"Suite", L"BenchIsolation", 0, ini.c_str()) != 0;
  c.diagnostics = GetPrivateProfileIntW(L"General", L"Diagnostics", 0, ini.c_str()) != 0;
  g_beeps = GetPrivateProfileIntW(L"General", L"Beeps", 1, ini.c_str()) != 0;
  // Re-locate moved DCS code by signature (reloc.h); 0 = recorded addresses only.
  const bool sigScan = GetPrivateProfileIntW(L"General", L"SigScan", 1, ini.c_str()) != 0;
  if (reloc::g_enabled.exchange(sigScan) != sigScan || (initial && !sigScan))
    Log("config: signature scan %s", sigScan ? "on" : "off (recorded DCS addresses only)");
  {
    wchar_t hk[64];
    GetPrivateProfileStringW(L"Hotkeys", L"Toggle", L"122:6", hk, 64, ini.c_str());
    if (!ParseHotkey(hk, c.toggleVk, c.toggleMods)) Log("config: [Hotkeys] Toggle=%ls is invalid; key disabled", hk);
    c.developerKeys = GetPrivateProfileIntW(L"Hotkeys", L"DeveloperKeys", 0, ini.c_str()) != 0;
  }
  c.shadowTight = GetPrivateProfileIntW(L"Shadow", L"TightCasters", 0, ini.c_str()) != 0;
  c.shadowDebugInvert = GetPrivateProfileIntW(L"Shadow", L"DebugInvert", 0, ini.c_str()) != 0;
  c.shadowMarginFrac = ReadDouble(L"Shadow", L"MarginFrac", 0.03, ini);
  c.shadowMarginMin = ReadDouble(L"Shadow", L"MarginMin", 2.0, ini);
  c.shadowFloorY = ReadDouble(L"Shadow", L"FloorY", -1000.0, ini);
  c.suiteBenchShadow = GetPrivateProfileIntW(L"Suite", L"BenchShadow", 0, ini.c_str()) != 0;
  c.allocSlabs = GetPrivateProfileIntW(L"Model", L"AllocSlabs", 0, ini.c_str()) != 0;
  c.slabBytes = GetPrivateProfileIntW(L"Model", L"SlabBytes", 4096, ini.c_str());
  if (c.slabBytes < 256) c.slabBytes = 256;
  if (c.slabBytes > 65536) c.slabBytes = 65536;
  c.suiteBenchAllocSlabs = GetPrivateProfileIntW(L"Suite", L"BenchAllocSlabs", 0, ini.c_str()) != 0;
  c.texDedupe = GetPrivateProfileIntW(L"Texture", L"StreamDedupe", 0, ini.c_str()) != 0;
  c.suiteBenchTexDedupe = GetPrivateProfileIntW(L"Suite", L"BenchTexDedupe", 0, ini.c_str()) != 0;
  c.suiteBenchEngine = GetPrivateProfileIntW(L"Suite", L"BenchEngine", 0, ini.c_str()) != 0;
  // R90: at most 10 min (a motion_*.csv grows ~3 KB/s; the key was unclamped above).
  c.motionSeconds = std::min(600, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"MotionSeconds", 60, ini.c_str())));
  c.suiteQuick = GetPrivateProfileIntW(L"Suite", L"Quick", 0, ini.c_str()) != 0;
  c.triPlain = GetPrivateProfileIntW(L"Model", L"PlainTriangleCounter", 0, ini.c_str()) != 0;
  c.suiteBenchTriPlain = GetPrivateProfileIntW(L"Suite", L"BenchTriPlain", 0, ini.c_str()) != 0;
  c.cbSkip = GetPrivateProfileIntW(L"Effects", L"SkipSameConstantBuffer", 0, ini.c_str()) != 0;
  c.suiteBenchCbSkip = GetPrivateProfileIntW(L"Suite", L"BenchCbSkip", 0, ini.c_str()) != 0;
  c.suiteBenchMicro = GetPrivateProfileIntW(L"Suite", L"BenchMicro", 0, ini.c_str()) != 0;
  c.costWeights = GetPrivateProfileIntW(L"Scene", L"CostWeights", 0, ini.c_str()) != 0;
  g_costWeightsSanity = GetPrivateProfileIntW(L"Scene", L"CostWeightsSanity", 0, ini.c_str()) != 0;
  c.suiteBenchCostWeights = GetPrivateProfileIntW(L"Suite", L"BenchCostWeights", 0, ini.c_str()) != 0;
  c.suiteTerrain = GetPrivateProfileIntW(L"Suite", L"Terrain", 0, ini.c_str()) != 0;
  c.cbUploadSkip = GetPrivateProfileIntW(L"Effects", L"SkipSameConstantUpload", 0, ini.c_str()) != 0;
  c.suiteBenchCbUpload = GetPrivateProfileIntW(L"Suite", L"BenchCbUpload", 0, ini.c_str()) != 0;
  c.pacerLowPower = GetPrivateProfileIntW(L"General", L"LowPowerPacer", 0, ini.c_str()) != 0;
  c.pacerTimeoutUs = GetPrivateProfileIntW(L"General", L"LowPowerPacerTimeoutUs", 10, ini.c_str());
  c.suiteBenchPacer = GetPrivateProfileIntW(L"Suite", L"BenchPacer", 0, ini.c_str()) != 0;
  c.motionSweep = GetPrivateProfileIntW(L"Suite", L"MotionSweep", 0, ini.c_str()) != 0;
  c.motionProfile = GetPrivateProfileIntW(L"Suite", L"MotionProfile", 0, ini.c_str()) != 0;
  c.motionTaxi = GetPrivateProfileIntW(L"Suite", L"MotionTaxi", 0, ini.c_str()) != 0;
  c.taskClock = GetPrivateProfileIntW(L"Timing", L"TaskQueueClock", 0, ini.c_str()) != 0;
  c.frameHeapSlabs = GetPrivateProfileIntW(L"Model", L"FrameHeapSlabs", 0, ini.c_str()) != 0;
  c.suiteBenchFrameHeap = GetPrivateProfileIntW(L"Suite", L"BenchFrameHeap", 0, ini.c_str()) != 0;
  c.shadowTexSkip = GetPrivateProfileIntW(L"Model", L"ShadowTextureSkip", 1, ini.c_str()) != 0;
  c.suiteBenchShadowTex = GetPrivateProfileIntW(L"Suite", L"BenchShadowTex", 0, ini.c_str()) != 0;
  c.suiteBenchTexTable = GetPrivateProfileIntW(L"Suite", L"BenchTexTable", 0, ini.c_str()) != 0;
  c.suiteBenchBigPages = GetPrivateProfileIntW(L"Suite", L"BenchBigPages", 0, ini.c_str()) != 0;
  c.bigPages = GetPrivateProfileIntW(L"Model", L"BigModelPages", 1, ini.c_str()) != 0;
  c.parUpload = GetPrivateProfileIntW(L"Model", L"ParallelUpload", 0, ini.c_str()) != 0;
  c.suiteBenchParUpload = GetPrivateProfileIntW(L"Suite", L"BenchParallelUpload", 0, ini.c_str()) != 0;
  c.directUpload = GetPrivateProfileIntW(L"Model", L"DirectUpload", 0, ini.c_str()) != 0;
  c.suiteDirectUploadVerify = GetPrivateProfileIntW(L"Suite", L"DirectUploadVerify", 0, ini.c_str()) != 0;
  c.suiteBenchDirectUpload = GetPrivateProfileIntW(L"Suite", L"BenchDirectUpload", 0, ini.c_str()) != 0;
  c.motionCounters = GetPrivateProfileIntW(L"Suite", L"MotionCounters", 1, ini.c_str()) != 0;
  c.shadowInst = GetPrivateProfileIntW(L"Model", L"ShadowInstancing", 1, ini.c_str()) != 0;
  c.suiteShadowInstCompile = GetPrivateProfileIntW(L"Suite", L"ShadowInstCompile", 0, ini.c_str()) != 0;
  c.suiteGBufferInstCompile = GetPrivateProfileIntW(L"Suite", L"GBufferInstCompile", 0, ini.c_str()) != 0;
  c.suiteGBufferTexCount = GetPrivateProfileIntW(L"Suite", L"GBufferTexCount", 0, ini.c_str()) != 0;
  c.suiteFxApplyCount = GetPrivateProfileIntW(L"Suite", L"FxApplyCount", 0, ini.c_str()) != 0;
  c.suiteDirectUploadCount = GetPrivateProfileIntW(L"Suite", L"DirectUploadCount", 0, ini.c_str()) != 0;
  c.suiteSrvSpanCount = GetPrivateProfileIntW(L"Suite", L"SrvSpanCount", 0, ini.c_str()) != 0;
  c.suiteJoinTailCount = GetPrivateProfileIntW(L"Suite", L"JoinTailCount", 0, ini.c_str()) != 0;
  c.suiteShadowRecCount = GetPrivateProfileIntW(L"Suite", L"ShadowRecCount", 0, ini.c_str()) != 0;
  c.suiteGBufferRecCount = GetPrivateProfileIntW(L"Suite", L"GBufferRecCount", 0, ini.c_str()) != 0;
  c.suiteForwardRecCount = GetPrivateProfileIntW(L"Suite", L"ForwardRecCount", 0, ini.c_str()) != 0;
  c.suiteGpuPassTiming = GetPrivateProfileIntW(L"Suite", L"GpuPassTiming", 0, ini.c_str()) != 0;
  c.suiteGpuPassStats = GetPrivateProfileIntW(L"Suite", L"GpuPassStats", 0, ini.c_str()) != 0;
  c.suiteGpuSplit = GetPrivateProfileIntW(L"Suite", L"GpuSplit", 0, ini.c_str());
  c.suiteGpuSplitFrames = (std::max)(1, (std::min)(600, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"GpuSplitFrames", 60, ini.c_str()))));
  c.suiteGapAttrib = (std::max)(0, (std::min)(60, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"GapAttrib", 0, ini.c_str()))));
  c.suiteGapAttribCalEvery = (std::max)(0, (std::min)(1000, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"GapAttribCalEvery", 40, ini.c_str()))));
  c.suiteGapAttribDump = GetPrivateProfileIntW(L"Suite", L"GapAttribDump", 0, ini.c_str()) != 0;
  c.suiteLodProbe = (std::max)(0, (std::min)(100, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"LodProbe", 0, ini.c_str()))));
  c.suiteGpuDedup = (std::max)(0, (std::min)(12, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"GpuDedup", 0, ini.c_str()))));
  c.suiteGpuDedupWarm = (std::max)(2, (std::min)(1000, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"GpuDedupWarm", 30, ini.c_str()))));
  c.suiteGpuDedupSnap = GetPrivateProfileIntW(L"Suite", L"GpuDedupSnap", 0, ini.c_str()) != 0;
  c.suiteGpuDedupHashCap = (std::max)(0, (std::min)(1 << 24, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"GpuDedupHashCap", 16384, ini.c_str()))));
  c.peripheralLodScale = (std::max)(0, (std::min)(99, static_cast<int>(GetPrivateProfileIntW(L"Model", L"PeripheralLodScale", 0, ini.c_str()))));
  c.suiteFocusGeomProbe = (std::max)(0, (std::min)(15, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"FocusGeomProbe", 0, ini.c_str()))));
  c.focusBroadcast = (std::max)(0, (std::min)(1, static_cast<int>(GetPrivateProfileIntW(L"D3D", L"FocusBroadcast", 0, ini.c_str()))));
  c.suiteUavOverlap = (std::max)(0, (std::min)(4, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"UavOverlap", 0, ini.c_str()))));
  c.suitePeriphLodTiming = (std::max)(0, (std::min)(99, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"PeriphLodTiming", 0, ini.c_str()))));
  c.suitePeriphLodImage = (std::max)(0, (std::min)(120, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"PeriphLodImage", 0, ini.c_str()))));
  c.suiteBenchPeripheralLod = GetPrivateProfileIntW(L"Suite", L"BenchPeripheralLod", 0, ini.c_str()) != 0;
  c.cascadeCache = GetPrivateProfileIntW(L"Shadows", L"CascadeCache", 0, ini.c_str()) != 0;
  {
    wchar_t mask[32];
    GetPrivateProfileStringW(L"Shadows", L"CascadeCacheMask", L"0xc", mask, 32, ini.c_str());
    c.cascadeCacheMask = static_cast<uint32_t>(wcstoul(mask, nullptr, 0)) & 0xf;  // decimal or 0x hex
  }
  c.cascadeCacheMaxAge = (std::max)(1, (std::min)(64, static_cast<int>(GetPrivateProfileIntW(L"Shadows", L"CascadeCacheMaxAge", 4, ini.c_str()))));
  c.cascadeCacheMarginPct = (std::max)(0, (std::min)(25, static_cast<int>(GetPrivateProfileIntW(L"Shadows", L"CascadeCacheMarginPct", 3, ini.c_str()))));
  c.cascadeCacheVerify = (std::max)(0, (std::min)(10000, static_cast<int>(GetPrivateProfileIntW(L"Shadows", L"CascadeCacheVerify", 0, ini.c_str()))));
  c.suiteCascadeCacheTiming = GetPrivateProfileIntW(L"Suite", L"CascadeCacheTiming", 0, ini.c_str()) != 0;
  {
    wchar_t cf[32];
    GetPrivateProfileStringW(L"D3D", L"ChainFast", L"7", cf, 32, ini.c_str());
    c.chainFast = static_cast<uint32_t>(wcstoul(cf, nullptr, 0)) & 0x7;  // decimal or 0x hex
  }
  c.suiteChainFastTiming = GetPrivateProfileIntW(L"Suite", L"ChainFastTiming", 0, ini.c_str()) != 0;
  c.suiteChainFastVerify = (std::max)(0, (std::min)(600, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"ChainFastVerify", 0, ini.c_str()))));
  {
    wchar_t sf[32];
    GetPrivateProfileStringW(L"D3D", L"ShaderFast", L"3", sf, 32, ini.c_str());
    c.shaderFast = static_cast<uint32_t>(wcstoul(sf, nullptr, 0)) & 0x3;  // R36, decimal or 0x hex
  }
  c.suiteShaderFastTiming = GetPrivateProfileIntW(L"Suite", L"ShaderFastTiming", 0, ini.c_str()) != 0;
  c.suiteShaderFastVerify = (std::max)(0, (std::min)(600, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"ShaderFastVerify", 0, ini.c_str()))));
  pifilt::ReadConfig(ini.c_str());  // R38 rank 1: [D3D] PeriphInstFilter* and its [Suite] keys (default 0)
  vsig::ReadConfig(ini.c_str());    // R38 rank 2 G4: [Suite] FocusVsCensus (default 0)
  fbcr::ReadConfig(ini.c_str());    // R53: [D3D] FocusBroadcast=2 / FocusBroadcast* and [Suite] FocusBcast* (default 0)
  c.nightTone = static_cast<int>(GetPrivateProfileIntW(L"Night", L"NightTone", 0, ini.c_str()));  // R37
  c.nightFloor = ReadDouble(L"Night", L"NightExposureFloor", 0.0, ini);
  c.nightToe = ReadDouble(L"Night", L"NightToe", 0.0, ini);
  c.nightEv = ReadDouble(L"Night", L"NightExposureEV", 0.0, ini);
  c.nightGate = GetPrivateProfileIntW(L"Night", L"NightGate", 1, ini.c_str()) != 0;
  c.suiteVramCount = GetPrivateProfileIntW(L"Suite", L"VramCount", 0, ini.c_str()) != 0;
  c.suiteVramCountSec = static_cast<int>(GetPrivateProfileIntW(L"Suite", L"VramCountSec", 20, ini.c_str()));
  c.suiteVramCountCreates = GetPrivateProfileIntW(L"Suite", L"VramCountCreates", 1, ini.c_str()) != 0;
  c.splitFilter = GetPrivateProfileIntW(L"D3D", L"SplitFilter", 1, ini.c_str()) != 0;  // R49: default 1 (shipped)
  {
    wchar_t ops[32];
    GetPrivateProfileStringW(L"D3D", L"SplitFilterOps", L"0x4ff", ops, 32, ini.c_str());
    c.splitFilterOps = static_cast<uint32_t>(wcstoul(ops, nullptr, 0)) & 0x7ff;  // decimal or 0x hex
  }
  c.suiteSplitFilterVerify = GetPrivateProfileIntW(L"Suite", L"SplitFilterVerify", 0, ini.c_str()) != 0;
  c.suiteBenchSplitFilter = GetPrivateProfileIntW(L"Suite", L"BenchSplitFilter", 0, ini.c_str()) != 0;
  c.shadowBatch = GetPrivateProfileIntW(L"Model", L"ShadowBatching", 1, ini.c_str()) != 0;
  c.suiteShadowInstVerify = GetPrivateProfileIntW(L"Suite", L"ShadowInstVerify", 0, ini.c_str()) != 0;
  c.suiteBenchShadowInst = GetPrivateProfileIntW(L"Suite", L"BenchShadowInst", 0, ini.c_str()) != 0;
  c.shadowPlanAsync = GetPrivateProfileIntW(L"Model", L"ShadowPlanAsync", 1, ini.c_str()) != 0;
  c.suiteBenchShadowPlanAsync = GetPrivateProfileIntW(L"Suite", L"BenchShadowPlanAsync", 0, ini.c_str()) != 0;
  c.shadowRecorder = GetPrivateProfileIntW(L"Model", L"ShadowRecorder", 0, ini.c_str()) != 0;
  {
    wchar_t scope[32];
    GetPrivateProfileStringW(L"Model", L"ShadowRecorderScope", L"0x101", scope, 32, ini.c_str());
    c.shadowRecorderScope = static_cast<uint32_t>(wcstoul(scope, nullptr, 0)) & 0x3ff;  // decimal or 0x hex
  }
  c.shadowRecorderWaitUs = GetPrivateProfileIntW(L"Model", L"ShadowRecorderWaitUs", 200, ini.c_str());
  if (c.shadowRecorderWaitUs < 0) c.shadowRecorderWaitUs = 0;
  if (c.shadowRecorderWaitUs > 20000) c.shadowRecorderWaitUs = 20000;
  c.shadowRecorderPriority = static_cast<int>(GetPrivateProfileIntW(L"Model", L"ShadowRecorderPriority", 0, ini.c_str()));
  if (c.shadowRecorderPriority < -2) c.shadowRecorderPriority = -2;
  if (c.shadowRecorderPriority > 2) c.shadowRecorderPriority = 2;
  {
    wchar_t split[32];
    GetPrivateProfileStringW(L"Model", L"ShadowRecorderSplit", L"0xf", split, 32, ini.c_str());
    c.shadowRecorderSplit = static_cast<uint32_t>(wcstoul(split, nullptr, 0)) & 0xf;
  }
  c.shadowRecorderHelpers = static_cast<int>(GetPrivateProfileIntW(L"Model", L"ShadowRecorderHelpers", 3, ini.c_str()));
  if (c.shadowRecorderHelpers < 0) c.shadowRecorderHelpers = 0;
  if (c.shadowRecorderHelpers > 3) c.shadowRecorderHelpers = 3;
  c.shadowRecorderInstancing = GetPrivateProfileIntW(L"Model", L"ShadowRecorderInstancing", 1, ini.c_str()) != 0;
  c.suiteShadowRecVerify = GetPrivateProfileIntW(L"Suite", L"ShadowRecVerify", 0, ini.c_str()) != 0;
  c.suiteShadowRecVerifySec = GetPrivateProfileIntW(L"Suite", L"ShadowRecVerifySec", 5, ini.c_str());
  if (c.suiteShadowRecVerifySec < 1) c.suiteShadowRecVerifySec = 1;
  if (c.suiteShadowRecVerifySec > 120) c.suiteShadowRecVerifySec = 120;
  c.suiteBenchShadowRecorder = GetPrivateProfileIntW(L"Suite", L"BenchShadowRecorder", 0, ini.c_str()) != 0;
  c.gbufferBatch = GetPrivateProfileIntW(L"Model", L"GBufferBatching", 0, ini.c_str()) != 0;
  c.suiteGBufferInstVerify = GetPrivateProfileIntW(L"Suite", L"GBufferInstVerify", 0, ini.c_str()) != 0;
  c.suiteBenchGBufferInst = GetPrivateProfileIntW(L"Suite", L"BenchGBufferInst", 0, ini.c_str()) != 0;
  c.gbufferRecorder = GetPrivateProfileIntW(L"Model", L"GBufferRecorder", 0, ini.c_str()) != 0;
  {
    wchar_t scope[32];
    GetPrivateProfileStringW(L"Model", L"GBufferRecorderScope", L"0x10004", scope, 32, ini.c_str());
    c.gbufferRecorderScope = static_cast<uint32_t>(wcstoul(scope, nullptr, 0)) & 0x1ffff;  // decimal or 0x hex
  }
  c.gbufferRecorderWaitUs = GetPrivateProfileIntW(L"Model", L"GBufferRecorderWaitUs", 300, ini.c_str());
  if (c.gbufferRecorderWaitUs < 0) c.gbufferRecorderWaitUs = 0;
  if (c.gbufferRecorderWaitUs > 20000) c.gbufferRecorderWaitUs = 20000;
  c.gbufferRecorderIsland = GetPrivateProfileIntW(L"Model", L"GBufferRecorderIsland", 30, ini.c_str());
  if (c.gbufferRecorderIsland < 1) c.gbufferRecorderIsland = 1;
  if (c.gbufferRecorderIsland > 100000) c.gbufferRecorderIsland = 100000;
  c.gbufferRecorderMaxSegments = GetPrivateProfileIntW(L"Model", L"GBufferRecorderMaxSegments", 12, ini.c_str());
  if (c.gbufferRecorderMaxSegments < 1) c.gbufferRecorderMaxSegments = 1;
  if (c.gbufferRecorderMaxSegments > 16) c.gbufferRecorderMaxSegments = 16;
  c.gbufferRecorderHelpers = GetPrivateProfileIntW(L"Model", L"GBufferRecorderHelpers", 2, ini.c_str());
  if (c.gbufferRecorderHelpers < 0) c.gbufferRecorderHelpers = 0;
  if (c.gbufferRecorderHelpers > 2) c.gbufferRecorderHelpers = 2;
  c.gbufferRecorderCockpit = GetPrivateProfileIntW(L"Model", L"GBufferRecorderCockpit", 0, ini.c_str()) != 0;
  c.gbufferRecorderByOrdinal = GetPrivateProfileIntW(L"Model", L"GBufferRecorderByOrdinal", 0, ini.c_str()) != 0;
  c.suiteGBufferRecVerify = GetPrivateProfileIntW(L"Suite", L"GBufferRecVerify", 0, ini.c_str()) != 0;
  c.suiteGBufferRecVerifySec = GetPrivateProfileIntW(L"Suite", L"GBufferRecVerifySec", 6, ini.c_str());
  c.suiteGBufferRecVerifyLive = GetPrivateProfileIntW(L"Suite", L"GBufferRecVerifyLive", 0, ini.c_str()) != 0;
  c.suiteGBufferRecVerifyNoLatch = GetPrivateProfileIntW(L"Suite", L"GBufferRecVerifyNoLatch", 0, ini.c_str()) != 0;
  if (c.suiteGBufferRecVerifySec < 1) c.suiteGBufferRecVerifySec = 1;
  if (c.suiteGBufferRecVerifySec > 120) c.suiteGBufferRecVerifySec = 120;
  c.suiteGBufferRecVerifyStride = GetPrivateProfileIntW(L"Suite", L"GBufferRecVerifyStride", 0, ini.c_str());
  if (c.suiteGBufferRecVerifyStride < 0 || c.suiteGBufferRecVerifyStride == 1) c.suiteGBufferRecVerifyStride = 0;
  c.suiteBenchGBufferRecorder = GetPrivateProfileIntW(L"Suite", L"BenchGBufferRecorder", 0, ini.c_str()) != 0;
  {
    wchar_t mask[32];
    GetPrivateProfileStringW(L"Model", L"PassFlush", L"0", mask, 32, ini.c_str());
    c.passFlush = static_cast<uint32_t>(wcstoul(mask, nullptr, 0)) & 0xff;  // decimal or 0x hex
    GetPrivateProfileStringW(L"Suite", L"BenchPassFlushMask", L"0", mask, 32, ini.c_str());
    c.suiteBenchPassFlushMask = static_cast<uint32_t>(wcstoul(mask, nullptr, 0)) & 0xff;
  }
  c.suiteBenchPassFlush = GetPrivateProfileIntW(L"Suite", L"BenchPassFlush", 0, ini.c_str()) != 0;
  c.srvTailTrim = GetPrivateProfileIntW(L"Model", L"SrvTailTrim", 0, ini.c_str()) != 0;
  c.suiteSrvTailTrimVerify = GetPrivateProfileIntW(L"Suite", L"SrvTailTrimVerify", 0, ini.c_str()) != 0;
  c.suiteSrvTailTrimVerifySec = GetPrivateProfileIntW(L"Suite", L"SrvTailTrimVerifySec", 10, ini.c_str());
  if (c.suiteSrvTailTrimVerifySec < 1) c.suiteSrvTailTrimVerifySec = 1;
  if (c.suiteSrvTailTrimVerifySec > 120) c.suiteSrvTailTrimVerifySec = 120;
  c.suiteBenchSrvTailTrim = GetPrivateProfileIntW(L"Suite", L"BenchSrvTailTrim", 0, ini.c_str()) != 0;
  {
    wchar_t mode[32] = {};
    GetPrivateProfileStringW(L"Prefetch", L"Mode", L"off", mode, 32, ini.c_str());
    c.prefetchMode = _wcsicmp(mode, L"skip") == 0 ? 2 : _wcsicmp(mode, L"observe") == 0 ? 1 : 0;
    if (c.prefetchMode == 0 && _wcsicmp(mode, L"off") != 0 && mode[0])
      Log("config: [Prefetch] Mode=%ls not understood (off, observe or skip); off", mode);
    const int w = static_cast<int>(GetPrivateProfileIntW(L"Prefetch", L"WindowMs", 1000, ini.c_str()));
    c.prefetchWindowMs = static_cast<uint32_t>(std::max(1, std::min(60000, w)));
    const int tb = static_cast<int>(GetPrivateProfileIntW(L"Prefetch", L"TableBits", 20, ini.c_str()));
    c.prefetchTableBits = std::max(16, std::min(22, tb));
  }
  c.suiteBenchPrefetch = GetPrivateProfileIntW(L"Suite", L"BenchPrefetch", 0, ini.c_str()) != 0;
  c.threadCensus = GetPrivateProfileIntW(L"Threads", L"Census", 0, ini.c_str()) != 0;
  c.threadCensusSec = std::max(10, std::min(3600, static_cast<int>(GetPrivateProfileIntW(L"Threads", L"CensusSec", 60, ini.c_str()))));
  {
    // R44 (docsesearch\R44_autonomous_session.md section 1): default 2. With the placement pass made cheap
    // (thread_place.h), CPU-bound airfield: 2 vs 0 fps +1.26 +- 0.39, p95 -0.57 +- 0.12 ms, render -0.29 ms
    // (4/4 triplets; that start had DCS's affinity pass keep our recorder workers off the render core);
    // GPU-bound Germany: no change (fps +0.15 +- 0.15, p95 -0.06 +- 0.20). 0 restores DCS's masks.
    const int wp = static_cast<int>(GetPrivateProfileIntW(L"Threads", L"WorkerPlacement", 2, ini.c_str()));
    c.workerPlacement = wp >= 0 && wp <= 2 ? wp : 2;
  }
  // R52 (docs/research/R52_pacing_latency.md): the placement pass's steady-state re-check (it takes a
  // system-wide thread snapshot); new threads of ours still trigger a pass within 1 s.
  c.placeRecheckSec = std::max(10, std::min(3600, static_cast<int>(GetPrivateProfileIntW(L"Threads", L"PlaceRecheckSec", 60, ini.c_str()))));
  // R90 (docs/research/R90_hitch_hunting.md section 6): the lossless hitch fixes, each with its old path behind a key
  // for the A/B. PlaceRecheckMode: the placement re-check over tracked handles (no system-wide snapshot); PatchMode:
  // code-site writes without the system-wide snapshot, all suspensions batched; Async: DCS threads' log lines queued.
  c.placeRecheckMode = GetPrivateProfileIntW(L"Threads", L"PlaceRecheckMode", 1, ini.c_str()) != 0 ? 1 : 0;
  c.patchMode = GetPrivateProfileIntW(L"General", L"PatchMode", 1, ini.c_str()) != 0 ? 1 : 0;
  c.asyncLog = GetPrivateProfileIntW(L"Log", L"Async", 1, ini.c_str()) != 0;
  c.shadowPlanWaitUs = std::max(0, std::min(20000, static_cast<int>(GetPrivateProfileIntW(L"Model", L"ShadowPlanWaitUs", 300, ini.c_str()))));
  // R52 measurement keys (measure only, all default 0).
  c.frameTimeline = std::max(0, std::min(2, static_cast<int>(GetPrivateProfileIntW(L"Perf", L"FrameTimeline", 0, ini.c_str()))));
  c.timelineSec = std::max(2, std::min(600, static_cast<int>(GetPrivateProfileIntW(L"Perf", L"TimelineSec", 10, ini.c_str()))));
  c.poseProbe = GetPrivateProfileIntW(L"Perf", L"PoseProbe", 0, ini.c_str()) != 0;
  c.eventLog = GetPrivateProfileIntW(L"Perf", L"EventLog", 0, ini.c_str()) != 0;
  c.holdYawDeg = static_cast<int>(GetPrivateProfileIntW(L"Suite", L"HoldYawDeg", -1, ini.c_str()));
  c.holdPitchDeg = ReadDouble(L"Suite", L"HoldPitchDeg", 0.0, ini);
  c.poseRotateDps = ReadDouble(L"Suite", L"PoseRotateDps", 0.0, ini);
  c.holdPose = GetPrivateProfileIntW(L"Suite", L"HoldPose", 0, ini.c_str()) != 0;
  c.poseWallClock = GetPrivateProfileIntW(L"Suite", L"PoseWallClock", 0, ini.c_str()) != 0;  // R99
  c.suiteYawScan = GetPrivateProfileIntW(L"Suite", L"YawScan", 0, ini.c_str()) != 0;
  c.suiteYawProfile = GetPrivateProfileIntW(L"Suite", L"YawProfile", 0, ini.c_str()) != 0;
  c.suiteYawProfileStep = static_cast<int>(GetPrivateProfileIntW(L"Suite", L"YawProfileStep", 30, ini.c_str()));
  c.suiteRotationProfile = GetPrivateProfileIntW(L"Suite", L"RotationProfile", 0, ini.c_str()) != 0;
  c.suiteRotationDegPerSec = static_cast<int>(GetPrivateProfileIntW(L"Suite", L"RotationDegPerSec", 60, ini.c_str()));
  c.suiteRotationSeconds = static_cast<int>(GetPrivateProfileIntW(L"Suite", L"RotationSeconds", 20, ini.c_str()));
  c.suiteFrameStartGap = GetPrivateProfileIntW(L"Suite", L"FrameStartGap", 0, ini.c_str()) != 0;
  c.suiteFrameStartGapSec =
      std::max(0, std::min(120, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"FrameStartGapSec", 10, ini.c_str()))));
  c.suiteRunnableThreads = GetPrivateProfileIntW(L"Suite", L"RunnableThreads", 0, ini.c_str()) != 0;
  c.suiteRunnableThreadsSec =
      std::max(0, std::min(120, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"RunnableThreadsSec", 10, ini.c_str()))));
  c.suiteRunnableThreadsEvery = std::max(
      2, std::min(1024, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"RunnableThreadsEvery", 16, ini.c_str()))));
  {
    const uint32_t v = GetPrivateProfileIntW(L"Model", L"BigPageBytes", 4 << 20, ini.c_str());
    c.bigPageBytes = v < (1u << 20) ? (1u << 20) : v > (64u << 20) ? (64u << 20) : v;
  }
  c.suiteBenchTimerRes = GetPrivateProfileIntW(L"Suite", L"BenchTimerRes", 0, ini.c_str()) != 0;
  c.suiteBenchFilter = GetPrivateProfileIntW(L"Suite", L"BenchFilter", 0, ini.c_str());
  c.d3dFilter = GetPrivateProfileIntW(L"D3D", L"Filter", 0, ini.c_str()) != 0;
  c.fineTimer = GetPrivateProfileIntW(L"Scene", L"FineTimerResolution", 0, ini.c_str()) != 0;
  c.partitionBoost = GetPrivateProfileIntW(L"Scene", L"PartitionBoost", 0, ini.c_str()) != 0;
  c.suiteBenchCull = GetPrivateProfileIntW(L"Suite", L"BenchCull", 0, ini.c_str()) != 0;
  c.suiteBenchThreads = GetPrivateProfileIntW(L"Suite", L"BenchThreads", 0, ini.c_str()) != 0;
  c.suiteBenchTimer = GetPrivateProfileIntW(L"Suite", L"BenchTimer", 0, ini.c_str()) != 0;
  c.d3dMeter = GetPrivateProfileIntW(L"D3D", L"Meter", 0, ini.c_str()) != 0;
  c.shaderTimeCache = GetPrivateProfileIntW(L"Timing", L"ShaderTimeCache", 1, ini.c_str()) != 0;
  c.shaderTimeCacheUs = GetPrivateProfileIntW(L"Timing", L"CacheUs", c.shaderTimeCacheUs, ini.c_str());
  c.profileSeconds = GetPrivateProfileIntW(L"Profile", L"Seconds", c.profileSeconds, ini.c_str());
  c.profileThreads = GetPrivateProfileIntW(L"Profile", L"Threads", c.profileThreads, ini.c_str());
  c.profilePeriodMs = GetPrivateProfileIntW(L"Profile", L"PeriodMs", c.profilePeriodMs, ini.c_str());
  {
    auto clampInt = [&](const wchar_t* sec, const wchar_t* key, int def, int lo, int hi) {
      const int v = static_cast<int>(GetPrivateProfileIntW(sec, key, def, ini.c_str()));
      return v < lo ? lo : v > hi ? hi : v;
    };
    c.recorderDetail = GetPrivateProfileIntW(L"Log", L"RecorderDetail", 0, ini.c_str()) != 0;
    c.perfIntervalSec = clampInt(L"Log", L"PerfIntervalSec", 10, 0, 3600);
    c.vramIntervalSec = clampInt(L"Log", L"VramIntervalSec", 60, 0, 3600);
    c.govEnabled = GetPrivateProfileIntW(L"Governor", L"Enabled", 1, ini.c_str()) != 0;
    c.govGpuBusyPct = clampInt(L"Governor", L"GpuBusyPct", 92, 1, 100);
    c.govRenderIdlePct = clampInt(L"Governor", L"RenderIdlePct", 85, 1, 100);
    c.govCpuBusyPct = clampInt(L"Governor", L"CpuBusyPct", 92, 1, 100);
    c.govGpuFreePct = clampInt(L"Governor", L"GpuFreePct", 80, 1, 100);
    c.govGpuFullPct = clampInt(L"Governor", L"GpuFullPct", 98, 1, 101);
    c.govWindowSec = clampInt(L"Governor", L"WindowSec", 8, 1, 60);
    c.govNeedSec = clampInt(L"Governor", L"NeedSec", 6, 1, 60);
    c.govDwellSec = clampInt(L"Governor", L"MinDwellSec", 10, 1, 3600);
    c.govMinGainPct = (std::max)(0.0, (std::min)(50.0, ReadDouble(L"Governor", L"MinGainPct", 2.0, ini)));
    c.govTrialSec = clampInt(L"Governor", L"TrialSec", 20, 5, 60);
    c.govRecheckSec = clampInt(L"Governor", L"RecheckSec", 10, 0, 60);
    c.govRevertBackoffSec = clampInt(L"Governor", L"RevertBackoffSec", 300, 10, 3600);
  }
  c.fixationDepth = GetPrivateProfileIntW(L"Perception", L"FixationDepth", 0, ini.c_str()) != 0;
  c.depthGrid = GetPrivateProfileIntW(L"Perception", L"DepthGrid", 0, ini.c_str()) != 0;
  c.sceneCutReset = GetPrivateProfileIntW(L"Perception", L"SceneCutReset", 1, ini.c_str()) != 0;
  c.fixationDepthVerify = GetPrivateProfileIntW(L"Perception", L"FixationDepthVerify", 0, ini.c_str());
  c.fixationDepthDump = GetPrivateProfileIntW(L"Perception", L"FixationDepthDump", 0, ini.c_str()) != 0;
  c.farDepthExport = GetPrivateProfileIntW(L"Perception", L"FarDepthExport", 0, ini.c_str()) != 0;
  c.farDepthSlots = GetPrivateProfileIntW(L"Perception", L"FarDepthExportSlots", 2, ini.c_str()) == 1 ? 1 : 2;
  c.farDepthLogProj = static_cast<int>(GetPrivateProfileIntW(L"Perception", L"FarDepthLogProj", 0, ini.c_str()));
  c.farDepthLogProj = (std::max)(0, (std::min)(1000, c.farDepthLogProj));
  c.farDepthPreOverlay = GetPrivateProfileIntW(L"Perception", L"FarDepthPreOverlay", 1, ini.c_str()) != 0;
  {
    wchar_t m[32] = {};
    GetPrivateProfileStringW(L"Perception", L"FarDepthCoverageMode", L"hdr", m, 32, ini.c_str());
    bool invalid = false;
    c.farDepthCoverageMode = ParseFarCoverageMode(m, &invalid);
    if (invalid && !g_farCovInvalidLogged) {
      g_farCovInvalidLogged = true;
      g_farCovInvalidLogs++;
      Log("config: [Perception] FarDepthCoverageMode=%ls is invalid (hdr or overlay); hdr used", m);
    }
  }
  c.farDepthPostOverlay = GetPrivateProfileIntW(L"Perception", L"FarDepthPostOverlay", 1, ini.c_str()) != 0;
  c.farDepthOverlayTrace =
      (std::max)(0, (std::min)(600, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"FarDepthOverlayTrace", 0,
                                                                           ini.c_str()))));
  c.cockpitMaskExport = GetPrivateProfileIntW(L"Perception", L"CockpitMaskExport", 0, ini.c_str()) != 0;  // R65
  c.cockpitMaskDivisor =
      (std::max)(0, (std::min)(8, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitMaskDivisor", 0, ini.c_str()))));
  c.cockpitMaskViews = static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitMaskViews", 3, ini.c_str())) & 15;
  if (!c.cockpitMaskViews) c.cockpitMaskViews = 3;
  c.cockpitMaskNearM = (std::max)(0.0, (std::min)(20.0, ReadDouble(L"Perception", L"CockpitMaskNearM", 1.0, ini)));
  c.cockpitMaskDilate =
      (std::max)(0, (std::min)(4, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitMaskDilate", 0, ini.c_str()))));
  c.cockpitMaskDump =
      (std::max)(0, (std::min)(300, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitMaskDump", 0, ini.c_str()))));
  c.cockpitMaskLog =
      (std::max)(0, (std::min)(1000, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitMaskLog", 0, ini.c_str()))));
  c.cockpitMaskOverlay = GetPrivateProfileIntW(L"Perception", L"CockpitMaskOverlay", 0, ini.c_str()) != 0;  // R74b
  c.cockpitDepthExport = GetPrivateProfileIntW(L"Perception", L"CockpitDepthExport", 0, ini.c_str()) != 0;  // R69
  c.cockpitDepthDump =
      (std::max)(0, (std::min)(300, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitDepthDump", 0, ini.c_str()))));
  c.cockpitMotionExport = GetPrivateProfileIntW(L"Perception", L"CockpitMotionExport", 0, ini.c_str()) != 0;  // R70
  c.cockpitMotionClass = GetPrivateProfileIntW(L"Perception", L"CockpitMotionClass", 1, ini.c_str()) != 0;
  c.cockpitMotionDump =
      (std::max)(0, (std::min)(300, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitMotionDump", 0, ini.c_str()))));
  c.cockpitFgDump =  // R71
      (std::max)(0, (std::min)(300, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitFgDump", 0, ini.c_str()))));
  c.symbologyExport = GetPrivateProfileIntW(L"Perception", L"SymbologyExport", 0, ini.c_str()) != 0;  // R95
  c.symbologyAlpha = GetPrivateProfileIntW(L"Perception", L"SymbologyAlpha", 1, ini.c_str()) != 0;
  {  // R97: [Dev] SymbologyPublishAlways (default 0) from the settings' ini, else from the deployed one
    int pa = static_cast<int>(GetPrivateProfileIntW(L"Dev", L"SymbologyPublishAlways", -1, ini.c_str()));
    if (pa < 0 && ini != deployedIni)
      pa = static_cast<int>(GetPrivateProfileIntW(L"Dev", L"SymbologyPublishAlways", -1, deployedIni.c_str()));
    c.symbologyPublishAlways = pa > 0;
  }
  c.cockpitTrueIpd = static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpd", 0, ini.c_str()));
  if (c.cockpitTrueIpd < 0 || c.cockpitTrueIpd > 2) c.cockpitTrueIpd = 0;
  c.cockpitTrueIpdLighting = GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdLighting", 0, ini.c_str()) != 0;
  c.cockpitTrueIpdForward = GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdForward", 1, ini.c_str()) != 0;
  c.cockpitTrueIpdHelmet = GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdHelmet", 1, ini.c_str()) != 0;
  c.cockpitTrueIpdOwnship =
      std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdOwnship", 0, ini.c_str())), 0, 2);
  c.cockpitTrueIpdOverlays = GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdOverlays", 1, ini.c_str()) != 0;
  c.cockpitTrueIpdOverlayMvp = GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdOverlayMvp", 1, ini.c_str()) != 0;
  c.cockpitTrueIpdLog =
      (std::max)(0, (std::min)(600, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdLog", 0,
                                                                           ini.c_str()))));
  c.cockpitTrueIpdPrevLog = (std::max)(
      0, (std::min)(150, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdPrevLog", 0, ini.c_str()))));
  c.cockpitTrueIpdFrameDump = (std::max)(
      0, (std::min)(600, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdFrameDump", 0, ini.c_str()))));
  c.cockpitTrueIpdMvpDump = (std::max)(
      0, (std::min)(2000, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdMvpDump", 0, ini.c_str()))));
  {
    wchar_t dv[32] = {};
    GetPrivateProfileStringW(L"Perception", L"CockpitTrueIpdFrameDumpViews", L"23", dv, 32, ini.c_str());
    int mask = 0;
    for (const wchar_t* q = dv; *q; ++q) {
      if (*q >= L'0' && *q <= L'3') mask |= 1 << (*q - L'0');
      if (*q == L'm' || *q == L'M') mask |= 1 << 4;
    }
    c.cockpitTrueIpdFrameDumpViews = mask ? mask : 0x0C;
  }
  c.cockpitTrueIpdOverlayRestore =
      GetPrivateProfileIntW(L"Perception", L"CockpitTrueIpdOverlayRestore", 0, ini.c_str()) != 0;
  {
    wchar_t op[32] = {};
    GetPrivateProfileStringW(L"Perception", L"CockpitTrueIpdOverlayPath", L"cpu", op, 32, ini.c_str());
    if (_wcsicmp(op, L"d3d11") == 0) {
      c.cockpitTrueIpdOverlayPath = 1;
    } else {
      c.cockpitTrueIpdOverlayPath = 0;
      if (_wcsicmp(op, L"cpu") != 0) Log("config: [Perception] CockpitTrueIpdOverlayPath=%ls is invalid; cpu", op);
    }
  }
  c.cockpitTrueIpdTestShiftMm =
      (std::max)(-40.0, (std::min)(40.0, ReadDouble(L"Perception", L"CockpitTrueIpdTestShiftMm", 0.0, ini)));
  {
    wchar_t hk[64] = {};
    GetPrivateProfileStringW(L"Perception", L"CockpitTrueIpdToggle", L"118:6", hk, 64, ini.c_str());
    if (!ParseHotkey(hk, c.cockpitTrueIpdToggleVk, c.cockpitTrueIpdToggleMods))
      Log("config: [Perception] CockpitTrueIpdToggle=%ls is invalid; key disabled", hk);
  }
  {
    wchar_t g[64] = {};
    GetPrivateProfileStringW(L"Perception", L"FixationDepthTestGaze", L"", g, 64, ini.c_str());
    c.fixationTestGaze = g;
  }
  if (c.statsIntervalSec < 1) c.statsIntervalSec = 1;
  {
    // [General] PerceptionOnly: latched by the initial read (the hooks it
    // leaves out are never installed; nothing installed is taken out later).
    const bool po = GetPrivateProfileIntW(L"General", L"PerceptionOnly", 0, ini.c_str()) != 0;
    static bool poChangeLogged = false;
    if (initial) {
      g_perceptionOnly = po;
      poChangeLogged = false;
      if (po)
        Log("config: perception only ([General] PerceptionOnly=1): fixation depth, far depth export and the cockpit "
            "ipd options only; no engine optimization, recorder, benchmark, suite, governor action or engine key; "
            "status word 0");
    } else if (po != g_perceptionOnly.load() && !poChangeLogged) {
      poChangeLogged = true;
      Log("config: [General] PerceptionOnly=%d takes effect at the next DCS start (now %d)", po ? 1 : 0,
          g_perceptionOnly.load() ? 1 : 0);
    }
    if (g_perceptionOnly.load()) c = PerceptionOnlyConfig(c);
  }
  {  // R40 [Color]: read and logged only; the grading pass is not wired yet (R40_color_oled.md section 6)
    const cgrade::Params cp = cgrade::ReadIni(ini.c_str());
    if (initial || !cgrade::Same(cp, cgrade::g_params)) {
      cgrade::g_params = cp;
      if (cgrade::Active(cp))
        Log("config: [Color] SrgbMatch=%d BlackFloor=%d Contrast=%d Saturation=%d gamma %.2f: R40 keys, not used "
            "(superseded by [Color] Mode and [ColorFix] / [ColorEnhanced], R42)",
            cp.srgbMatch, cp.blackFloor, cp.contrast, cp.saturation, cp.outputGamma);
    }
  }
  {  // R42 colour modes (color_pass.h): [Color] Mode / ModeKey / Timing, [ColorFix], [ColorEnhanced]
    const int m = static_cast<int>(GetPrivateProfileIntW(L"Color", L"Mode", 0, ini.c_str()));
    g_colorCfg.mode = m >= 0 && m <= 2 ? m : 0;
    g_colorCfg.timing = GetPrivateProfileIntW(L"Color", L"Timing", 0, ini.c_str()) != 0 ? 1 : 0;
    // R47 (dev A/B, default 3): bit 0 normalize the context table at the tone-map arm, bit 1 its state-call toggle
    // points and the normal-not-ours repair.
    cpass::g_armRepair = static_cast<int>(GetPrivateProfileIntW(L"Color", L"ArmRepair", 3, ini.c_str())) & 3;
    g_colorCfg.fix = cgrade::ReadModeIni(ini.c_str(), 1);
    g_colorCfg.enh = cgrade::ReadModeIni(ini.c_str(), 2);
    wchar_t hk[64] = {};
    GetPrivateProfileStringW(L"Color", L"ModeKey", L"0:0", hk, 64, ini.c_str());  // default off; 120:6 = Alt+Shift+F9
    if (!ParseHotkey(hk, g_colorKeyVk, g_colorKeyMods)) Log("config: [Color] ModeKey=%ls is invalid; key disabled", hk);
    static int lastIniMode = -1;
    if (g_colorCfg.mode != lastIniMode) g_colorKeyMode = -1;  // a new ini value wins over the key's choice
    lastIniMode = g_colorCfg.mode;
  }
  {  // R51 VrFx (vrfx.h, color_pass.h): [VrFx] GazeDof / GVision, default off; any change applies within a second
    const vrfx::Params p = vrfx::ReadIni(ini.c_str());
    static vrfx::Params last;
    static bool first = true;
    if (first || !vrfx::Same(p, last)) {
      while (vrfx::g_paramLock.test_and_set(std::memory_order_acquire)) YieldProcessor();
      vrfx::g_params = p;
      vrfx::g_paramLock.clear(std::memory_order_release);
      vrfx::g_paramVer++;
      if (!first || p.dof || p.gv)
        Log("config: [VrFx] GazeDof=%d (strength %.2f, max %.1f arcmin, saccade release %d, settle %.0f ms, fade-in "
            "%.0f ms, tolerance %.2f D, fovea guard %.1f deg, pupil %s%.1f mm) GVision=%d (strength %.2f, use gaze %d, "
            "red-out %d, tolerance %.1f G, mode %d)",
            p.dof, p.dofStrength, p.dofMaxArcmin, p.dofSaccadeRelease, p.dofSettleMs, p.dofFadeInMs, p.dofToleranceD,
            p.dofGuardDeg, p.dofPupilMm > 0 ? "fixed " : "from the scene; ", p.dofPupilMm, p.gv, p.gvStrength,
            p.gvUseGaze, p.gvRedout, p.gvTolerance, p.gvMode);
      last = p;
      first = false;
    }
    vrfx::g_wantDof = p.dof != 0;
    vrfx::g_wantGv = p.gv != 0;
  }
  {  // R41 live probes (dev, measure only; color_grade.h): a new non-zero value starts a probe session
    static int lastTone = 0, lastSwap = 0, lastGeo = 0;
    const int tone = static_cast<int>(GetPrivateProfileIntW(L"Color", L"ToneProbe", 0, ini.c_str()));
    const int swap = static_cast<int>(GetPrivateProfileIntW(L"Color", L"SwapProbe", 0, ini.c_str()));
    const int geo = GetPrivateProfileIntW(L"Suite", L"GeoOnlyProbe", 0, ini.c_str()) != 0 ? 1 : 0;
    cgrade::g_toneProbeViews = static_cast<int>(GetPrivateProfileIntW(L"Color", L"ToneProbeViews", 15, ini.c_str())) & 15;
    cgrade::g_toneProbeHdr = GetPrivateProfileIntW(L"Color", L"ToneProbeHdr", 0, ini.c_str()) != 0 ? 1 : 0;
    if (tone != lastTone && tone > 0) cgrade::g_toneProbeReq = tone;
    if (swap != lastSwap && swap > 0) cgrade::g_swapProbeReq = swap;
    if (geo != lastGeo) Log("probe: [Suite] GeoOnlyProbe=%d (focus main G-buffer into an off-target viewport; lossy, test only)", geo);
    cgrade::g_geoOnlyProbe = geo;
    lastTone = tone;
    lastSwap = swap;
    lastGeo = geo;
    // R94: [Perception] SymbologyTrace (dev, measure only; sym_trace_cfg.h): a new value > 0 starts a session.
    const int st = static_cast<int>(GetPrivateProfileIntW(L"Perception", L"SymbologyTrace", 0, ini.c_str()));
    symtrcfg::Configure(st, static_cast<int>(GetPrivateProfileIntW(L"Perception", L"SymbologyTraceViews", 15, ini.c_str())),
                        static_cast<int>(GetPrivateProfileIntW(L"Perception", L"SymbologyTraceSegments", 0, ini.c_str())),
                        static_cast<int>(GetPrivateProfileIntW(L"Perception", L"SymbologyTraceTone", 1, ini.c_str())),
                        g_devDir.empty() ? g_dir : g_devDir);
  }
  g_cfg = c;
  ApplyR90Config();
  if (g_perceptionOnly.load()) {
    PerceptionOnlyFlags();
    return;
  }
  if (!initial && g_engineOff.exchange(false)) Log("hotkey: engine optimizations back ON (ini changed)");
  if (g_benchRunningFlag.load()) return;  // the benchmark owns the toggle while it runs
  g_enabled = c.enabled;
  g_debugHole = c.debugHole;
  g_partitionBoost = c.partitionBoost && !g_engineOff.load();
  g_allocSlabBytes = static_cast<uint32_t>(c.slabBytes);
  g_allocSlabsOn = c.allocSlabs && !g_engineOff.load();
  g_texDedupeOn = c.texDedupe && !g_engineOff.load();
  ApplyTextureHook();
  g_triPlainOn = c.triPlain && !g_engineOff.load();
  ApplyTriCounter();
  g_cbSkipOn = c.cbSkip && !g_engineOff.load();
  ApplyCbSkip();
  g_costWeightsOn = c.costWeights && !g_engineOff.load();
  g_cbUploadSkipOn = c.cbUploadSkip && !g_engineOff.load();
  ApplyCbUpload();
  g_pacerLowPowerOn = c.pacerLowPower && !g_engineOff.load();
  ApplyPacer();
  g_taskClockOn = c.taskClock && !g_engineOff.load();
  ApplyTaskClock();
  g_frameHeapOn = c.frameHeapSlabs && !g_engineOff.load();
  if (g_frameHeapOn.load()) ApplyFrameHeap();
  g_shadowTexSkipOn = c.shadowTexSkip && !g_engineOff.load();
  ApplyShadowTexSkip();
  ApplyBigPages(c.bigPages && !g_engineOff.load());
  ApplyParUpload(c.parUpload && !g_engineOff.load());
  ApplyDirectUpload(c.directUpload && !g_engineOff.load());
  ApplySplitFilter(c.splitFilter && !g_engineOff.load());
  ApplyShadowInst();
  ApplyShadowBatch();
  ApplyGBufferBatch();
  ApplyShadowRecorder();
  ApplyGBufferRecorder();
  ApplyPassFlush();
  ApplySrvTailTrim(c.srvTailTrim && !g_engineOff.load());
  ApplyPrefetch();
  ApplyHoldYaw(c.holdYawDeg);
  ApplyPoseTest(c.holdPitchDeg, c.poseRotateDps, c.holdPose, c.poseWallClock);
  SetTimerResolution(c.fineTimer);
  g_d3dMode = c.d3dFilter ? 1 : 0;
  g_shadowTight = c.shadowTight;
  g_shadowDebugInvert = c.shadowDebugInvert;
  ApplyTimerConfig();
  Log("config: enabled=%d debugHole=%d keepFraction=%.3f safetyNdc=%.3f saccadeDeg=%.2f "
      "saccadeHold=%d focusRatio=%.2f apexTol=%.3f collectThreadsMax=%d",
      c.enabled, c.debugHole, c.keepFraction, c.safetyNdc, c.saccadeDeg, c.saccadeHoldFrames,
      c.focusRatio, c.apexTolerance, c.collectThreadsMax);
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

struct Vec3 {
  double x, y, z;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double Len(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 Normalize(Vec3 a) { return a * (1.0 / Len(a)); }

struct Plane {
  Vec3 n;  // unit, pointing inside
  double d;
};

double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// A view frustum decoded from a ClippingVolume plane block.
struct Frustum {
  bool valid = false;
  Plane nearP{}, farP{};
  bool hasFar = false;
  Plane sides[4]{};
  Vec3 apex{}, forward{};
  double halfAngle[4]{};  // radians, angle between the image centre and each corner
  double meanHalfAngle = 0;
  // Image-plane rectangle at distance 1 along `forward`, relative to the apex,
  // in winding order. Quad-Views-Foveated's texture coordinates are linear in it.
  Vec3 corners[4]{};
  Vec3 center{};     // centre of that rectangle (relative to apex)
  Vec3 centerDir{};  // unit direction through the image centre
};

bool ReadPlanes(const uint8_t* block, std::vector<Plane>& out) {
  int count = *reinterpret_cast<const int32_t*>(block + kPlaneCount);
  if (count < 5 || count > kMaxPlanes) return false;
  out.clear();
  for (int i = 0; i < count; ++i) {
    const double* p = reinterpret_cast<const double*>(block + i * kPlaneStride);
    Vec3 n{p[0], p[1], p[2]};
    double len = Len(n);
    if (!(len > 1e-9) || !std::isfinite(p[3])) return false;
    out.push_back({n * (1.0 / len), p[3] / len});
  }
  return true;
}

bool SolveApex(const Plane* sides, int n, Vec3& apex) {
  // Least squares point closest to all side planes: (sum n n^T) x = -sum d n.
  double a[3][3] = {};
  double b[3] = {};
  for (int i = 0; i < n; ++i) {
    const double v[3] = {sides[i].n.x, sides[i].n.y, sides[i].n.z};
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) a[r][c] += v[r] * v[c];
      b[r] -= sides[i].d * v[r];
    }
  }
  double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
               a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  if (std::fabs(det) < 1e-9) return false;
  auto detWith = [&](int col) {
    double m[3][3];
    memcpy(m, a, sizeof(m));
    for (int r = 0; r < 3; ++r) m[r][col] = b[r];
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  };
  apex = {detWith(0) / det, detWith(1) / det, detWith(2) / det};
  return std::isfinite(apex.x) && std::isfinite(apex.y) && std::isfinite(apex.z);
}

Frustum DecodeFrustum(const uint8_t* block) {
  Frustum f;
  std::vector<Plane> planes;
  if (!ReadPlanes(block, planes)) return f;
  const int count = static_cast<int>(planes.size());

  // Near and far planes face each other; with no far plane, the near plane is
  // the one whose normal is closest to the average of all normals.
  int ni = -1, fi = -1;
  double best = 0.0;
  for (int i = 0; i < count; ++i)
    for (int j = i + 1; j < count; ++j) {
      double dd = Dot(planes[i].n, planes[j].n);
      if (dd < best) {
        best = dd;
        ni = i;
        fi = j;
      }
    }
  if (best > -0.99) return f;  // no opposing pair: not a perspective frustum we understand

  int sideIdx[kMaxPlanes];
  int ns = 0;
  for (int i = 0; i < count; ++i)
    if (i != ni && i != fi) sideIdx[ns++] = i;
  if (ns != 4) return f;
  for (int i = 0; i < 4; ++i) f.sides[i] = planes[sideIdx[i]];
  if (!SolveApex(f.sides, 4, f.apex)) return f;

  // The near plane has the apex behind it (negative side).
  Plane a = planes[ni], b = planes[fi];
  if (Dot(a.n, f.apex) + a.d > Dot(b.n, f.apex) + b.d) std::swap(a, b);
  f.nearP = a;
  f.farP = b;
  f.hasFar = true;
  f.forward = a.n;

  // Side planes must pass through the apex. They may face away from the
  // forward axis: an off-centre focus view (gaze far from the eye axis) has
  // an asymmetric frustum entirely on one side of it.
  for (int i = 0; i < 4; ++i)
    if (std::fabs(Dot(f.sides[i].n, f.apex) + f.sides[i].d) > 0.05) return f;

  // Frustum edges: intersection lines of adjacent side planes, i.e. those that
  // point forward and lie inside the two remaining side planes.
  int nc = 0;
  for (int i = 0; i < 4; ++i)
    for (int j = i + 1; j < 4; ++j) {
      Vec3 d = Cross(f.sides[i].n, f.sides[j].n);
      double l = Len(d);
      if (l < 1e-9) continue;
      d = d * (1.0 / l);
      if (Dot(d, f.forward) < 0) d = d * -1.0;
      if (Dot(d, f.forward) < 1e-3) continue;
      bool inside = true;
      for (int k = 0; k < 4; ++k)
        if (k != i && k != j && Dot(f.sides[k].n, d) < -1e-6) inside = false;
      if (!inside) continue;
      if (nc == 4) return f;
      f.corners[nc++] = d * (1.0 / Dot(d, f.forward));
    }
  if (nc != 4) return f;
  f.center = (f.corners[0] + f.corners[1] + f.corners[2] + f.corners[3]) * 0.25;
  // Sort corners by angle around the centre so consecutive ones share an edge.
  Vec3 e1 = Normalize(f.corners[0] - f.center);
  Vec3 e2 = Cross(f.forward, e1);
  double ang[4];
  for (int i = 0; i < 4; ++i) {
    Vec3 v = f.corners[i] - f.center;
    ang[i] = std::atan2(Dot(v, e2), Dot(v, e1));
  }
  for (int i = 0; i < 4; ++i)
    for (int j = i + 1; j < 4; ++j)
      if (ang[j] < ang[i]) {
        std::swap(ang[i], ang[j]);
        std::swap(f.corners[i], f.corners[j]);
      }
  f.centerDir = Normalize(f.center);
  double sum = 0;
  for (int i = 0; i < 4; ++i) {
    f.halfAngle[i] = std::acos(Clamp(Dot(Normalize(f.corners[i]), f.centerDir), -1.0, 1.0));
    sum += f.halfAngle[i];
  }
  f.meanHalfAngle = sum / 4.0;  // mean angle from the image centre to the corners
  f.valid = true;
  return f;
}

void WritePlane(uint8_t* block, int idx, const Plane& p) {
  uint8_t* dst = block + idx * kPlaneStride;
  double* d = reinterpret_cast<double*>(dst);
  d[0] = p.n.x;
  d[1] = p.n.y;
  d[2] = p.n.z;
  d[3] = p.d;
  int pmask = (p.n.x >= 0 ? 1 : 0) | (p.n.y >= 0 ? 2 : 0) | (p.n.z >= 0 ? 4 : 0);
  *reinterpret_cast<int32_t*>(dst + kPlaneMaskP) = pmask;
  *reinterpret_cast<int32_t*>(dst + kPlaneMaskN) = (~pmask) & 7;
}

// Builds a plane block for frustum `f` with its image rectangle scaled by
// `keep` about its centre (keep = 1: the full frustum).
void BuildExclusionBlock(uint8_t* block, const Frustum& f, double keep) {
  memset(block, 0, kPlaneBlockSize);
  int idx = 0;
  WritePlane(block, idx++, f.nearP);
  if (f.hasFar) WritePlane(block, idx++, f.farP);
  Vec3 p[4];
  for (int i = 0; i < 4; ++i) p[i] = f.center + (f.corners[i] - f.center) * keep;
  for (int i = 0; i < 4; ++i) {
    Vec3 n = Normalize(Cross(p[i], p[(i + 1) & 3]));
    if (Dot(n, f.center) < 0) n = n * -1.0;
    WritePlane(block, idx++, Plane{n, -Dot(n, f.apex)});
  }
  *reinterpret_cast<int32_t*>(block + kPlaneCount) = idx;
  *reinterpret_cast<int32_t*>(block + kPlaneExtraCount) = 0;
}

// ---------------------------------------------------------------------------
// Persistent replacement volumes
// ---------------------------------------------------------------------------

// Each patched view gets its own long-lived ClippingVolume copies. They are
// never freed: a renderer thread may still hold a pointer from an earlier
// frame, so the memory must stay valid for the life of the process.
struct VolumeSlot {
  uint8_t* buf[2] = {};
  uint32_t flip = 0;
};

std::mutex g_slotMutex;
std::unordered_map<uint64_t, VolumeSlot> g_slots;

uint8_t* AcquireVolume(uint64_t key) {
  std::lock_guard<std::mutex> lock(g_slotMutex);
  VolumeSlot& s = g_slots[key];
  if (!s.buf[0] || !s.buf[1]) {
    // Checked: a failed allocation used to fault in memset with g_slotMutex
    // held; SehPrepare's __except does not unwind the lock_guard, so the next
    // collect call deadlocked the culling thread. Null = view not patched.
    for (auto& b : s.buf) {
      if (!b) b = static_cast<uint8_t*>(_aligned_malloc(0x700, 64));
      if (!b) return nullptr;
      memset(b, 0, 0x700);
    }
  }
  s.flip ^= 1;
  return s.buf[s.flip];
}

// Copies `src` into `dst` and appends `extra` to its exclusion list. Returns
// false when the source already has a full inline exclusion list.
bool MakeVolumeWithExclusion(uint8_t* dst, const uint8_t* src, const uint8_t* extra) {
  uint64_t srcSize = *reinterpret_cast<const uint64_t*>(src + kVolExclSize);
  const uint8_t* srcData = *reinterpret_cast<uint8_t* const*>(src + kVolExclPtr);
  if (srcSize >= static_cast<uint64_t>(kVolExclInlineCap)) return false;
  if (srcSize > 0 && !srcData) return false;

  memcpy(dst, src, kVolExclPtr);
  uint8_t* inl = dst + kVolExclInline;
  for (uint64_t i = 0; i < srcSize; ++i)
    memcpy(inl + i * kPlaneBlockSize, srcData + i * kPlaneBlockSize, kPlaneBlockSize);
  memcpy(inl + srcSize * kPlaneBlockSize, extra, kPlaneBlockSize);
  *reinterpret_cast<uint8_t**>(dst + kVolExclPtr) = inl;
  *reinterpret_cast<uint64_t*>(dst + kVolExclCap) = kVolExclInlineCap;
  *reinterpret_cast<uint64_t*>(dst + kVolExclSize) = srcSize + 1;
  return true;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct Stats {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> quadCalls{0};     // calls with >= 2 focus/peripheral pairs
  std::atomic<uint64_t> patchedViews{0};
  std::atomic<uint64_t> usQuad{0};        // time spent in quad calls
  std::atomic<uint64_t> periphRenderables{0};
  std::atomic<uint64_t> focusRenderables{0};
  std::atomic<uint64_t> usAll{0};
  std::atomic<uint64_t> maxViews{0};
};
Stats g_stats;
std::atomic<int> g_dumpsLeft{0};
double g_qpcToUs = 0;
std::atomic<int64_t> g_firstQuadQpc{0};

}  // namespace
void SetBenchVariant(bool on);
void BenchSetEngineOff(bool off);
void RestoreD3dHooks();
#include "exit_guard.h"  // DCS shutdown: quiesce, late faults on our threads end the thread only
#include "patch_threads.h"  // R90: this process's threads without a system-wide snapshot; suspended code writes
#include "disk_prune.h"     // R90: output caps at payload start (newest-N families, .qvc LRU, orphaned .tmp)
namespace {
// R90: the log writer thread (qvalog above). Started by DcsQvPayload_Start, stopped (and the ring drained) by
// Stop and Quiesce.
HANDLE g_logWriter = nullptr;
std::atomic<bool> g_logWriterStop{false};
DWORD WINAPI LogWriterThread(void*) {
  while (!g_logWriterStop.load(std::memory_order_acquire)) {
    if (!qvalog::g_testHold.load(std::memory_order_relaxed)) qvalog::Drain();
    if (qvalog::g_wake)
      WaitForSingleObject(qvalog::g_wake, 10);  // R93: a thread waiting for ring space wakes the writer at once
    else
      Sleep(10);
  }
  qvalog::Drain();
  return 0;
}
void StartAsyncLog() {
  if (g_logWriter) return;
  if (g_api && !qvalog::g_logAt) {  // the loader's DcsQvCull_LogAt (R90 loaders): lines keep their call time
    HMODULE loader = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(g_api->log), &loader) &&
        loader)
      qvalog::g_logAt = reinterpret_cast<qvalog::LogAtFn>(GetProcAddress(loader, "DcsQvCull_LogAt"));
  }
  g_logWriterStop = false;
  if (!qvalog::g_wake) qvalog::g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // R93 (kept for the payload)
  g_logWriter = CreateThread(nullptr, 0, qvexit::Thread<LogWriterThread>, nullptr, 0, nullptr);
  qvalog::g_on = g_logWriter && qvalog::g_want.load();
}
// Every later line is synchronous; the queued ones are written first (in order).
void StopAsyncLog() {
  qvalog::g_on = false;
  if (g_logWriter) {
    g_logWriterStop = true;
    if (qvalog::g_wake) SetEvent(qvalog::g_wake);  // R93: out of its wait now
    WaitForSingleObject(g_logWriter, 5000);
    CloseHandle(g_logWriter);
    g_logWriter = nullptr;
  }
  qvalog::Drain();
}
}  // namespace
#include "gpu.h"
#include "scene_cut.h"  // [Perception] SceneCutReset: Quad Views history reset on camera cuts
#include "bench.h"
#include "profiler.h"
#include "timer_cache.h"
#include "d3dstate.h"
#include "collect_timing.h"
#include "task_timing.h"
#include "model_timing.h"
#include "shadow_pass.h"
#include "pass_timing.h"
#include "thread_tuning.h"
#include <condition_variable>
#include <functional>
#include <thread>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <dxgi1_4.h>
#include <d3d11_4.h>  // R65: ID3D11Device5 / ID3D11Fence (cockpit mask: shared fence)
#include "deferred_rec.h"  // R15 A1: deferred-context worker pool of the shadow and G-buffer recorders
namespace {

void ApplyTimerConfig() {
  if (!timercache::g_orig) return;
  timercache::g_periodUs = g_cfg.shaderTimeCacheUs;
  timercache::g_on = g_cfg.shaderTimeCache && g_cfg.shaderTimeCacheUs > 0 && !g_engineOff.load();
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

using CollectFn = void(__fastcall*)(void* self, uint32_t count, uint8_t* infos, uint8_t* outs,
                                    void* taskQueue, uint32_t flags);
CollectFn g_origCollect = nullptr;
std::atomic<void*> g_scene{nullptr};
std::atomic<uint32_t> g_lastCollectFlags{0};  // last argument of collectSceneObjectsRenderables
// Finer culling partition: Scene.dll splits object parsing into
// min((min(threadsMax, arg) + 1) * 2, 16) contiguous chunks weighted by object
// count. Raising threadsMax to 8 and the argument to 7 yields the maximum of
// 16 chunks, so one chunk of heavy models delays the render thread less.
std::atomic<uint32_t> g_defaultThreadsMax{0};
// R63 U2: 1 = the running Scene.dll's collect function reads scene+0x40 and
// takes min(it, its last argument) (scenecheck::FindThreadsMaxUse, checked by
// Install); -1 = it does not: nothing writes +0x40 or raises the argument.
std::atomic<int> g_sceneLayout{0};
std::atomic<bool> g_threadsApplied{false};

struct ViewInfo {
  int index;
  Frustum fr;
  int role;     // 0 = other, 1 = peripheral, 2 = focus
  int partner;  // index of the paired focus view (peripheral only)
};

struct Patch {
  uint8_t* slot;    // address of CollectionInfo::clipVolume
  uint8_t* original;
};

size_t VectorCount(const uint8_t* vec) {
  auto b = *reinterpret_cast<uint8_t* const*>(vec);
  auto e = *reinterpret_cast<uint8_t* const*>(vec + 8);
  return (b && e > b) ? static_cast<size_t>(e - b) / sizeof(void*) : 0;
}

void DumpViews(uint32_t count, const uint8_t* infos, const std::vector<ViewInfo>& views) {
  Log("---- view dump: %u infos ----", count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* ci = infos + i * kCollectionInfoStride;
    uint16_t shading = *reinterpret_cast<const uint16_t*>(ci + kCiShadingModel);
    uint32_t tag = *reinterpret_cast<const uint32_t*>(ci + kCiViewportTag);
    const void* aux = *reinterpret_cast<void* const*>(ci + kCiAuxCallback);
    const uint8_t* vol = *reinterpret_cast<uint8_t* const*>(ci + kCiClipVolume);
    Log("[%u] shading=%u tag=%u aux=%p vol=%p", i, shading, tag, aux, vol);
    if (!vol) continue;
    int np = *reinterpret_cast<const int32_t*>(vol + kPlaneCount);
    Log("     planes=%d extra=%d distMax=%.1f excl=%llu", np,
        *reinterpret_cast<const int32_t*>(vol + kPlaneExtraCount),
        *reinterpret_cast<const double*>(vol + kVolDistanceMax),
        static_cast<unsigned long long>(*reinterpret_cast<const uint64_t*>(vol + kVolExclSize)));
    for (int p = 0; p < np && p < kMaxPlanes; ++p) {
      const double* d = reinterpret_cast<const double*>(vol + p * kPlaneStride);
      Log("       n=(%+.4f %+.4f %+.4f) d=%+.2f", d[0], d[1], d[2], d[3]);
    }
    const float* m = reinterpret_cast<const float*>(ci + kCiViewProj);
    Log("     vp row0 %+.3f %+.3f %+.3f %+.3f | row3 %+.3f %+.3f %+.3f %+.3f", m[0], m[1], m[2],
        m[3], m[12], m[13], m[14], m[15]);
    for (const ViewInfo& v : views) {
      if (v.index != static_cast<int>(i)) continue;
      if (!v.fr.valid) {
        Log("     frustum: not decoded");
        break;
      }
      const double r2d = 57.29577951308232;
      Log("     apex=(%.3f %.3f %.3f) fwd=(%+.3f %+.3f %+.3f) half=[%.1f %.1f %.1f %.1f] role=%d "
          "partner=%d",
          v.fr.apex.x, v.fr.apex.y, v.fr.apex.z, v.fr.forward.x, v.fr.forward.y, v.fr.forward.z,
          v.fr.halfAngle[0] * r2d, v.fr.halfAngle[1] * r2d, v.fr.halfAngle[2] * r2d,
          v.fr.halfAngle[3] * r2d, v.role, v.partner);
    }
  }
}

// Collections per collect call we classify and patch. It was 64: an F-4 cockpit
// scene has 68 (R24 12), so no call was classified, no quad frame counted, and
// every feature keyed on quad frames stood still. This is only a sanity bound.
constexpr uint32_t kMaxCollectInfos = 4096;

// Classifies the views of one call into peripheral/focus pairs.
int Classify(uint32_t count, const uint8_t* infos, std::vector<ViewInfo>& views) {
  views.clear();
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* ci = infos + i * kCollectionInfoStride;
    if (*reinterpret_cast<const uint16_t*>(ci + kCiShadingModel) != 0) continue;
    if (*reinterpret_cast<void* const*>(ci + kCiAuxCallback)) continue;
    const uint8_t* vol = *reinterpret_cast<uint8_t* const*>(ci + kCiClipVolume);
    if (!vol) continue;
    ViewInfo v{static_cast<int>(i), DecodeFrustum(vol), 0, -1};
    views.push_back(v);
  }
  if (views.size() < 4) return 0;

  double widest = 0;
  for (auto& v : views)
    if (v.fr.valid && v.fr.meanHalfAngle > widest) widest = v.fr.meanHalfAngle;
  for (auto& v : views)
    if (v.fr.valid) v.role = (v.fr.meanHalfAngle < widest * g_cfg.focusRatio) ? 2 : 1;

  int pairs = 0;
  for (auto& p : views) {
    if (p.role != 1) continue;
    double bestDist = g_cfg.apexTolerance;
    int best = -1;
    for (size_t k = 0; k < views.size(); ++k) {
      const ViewInfo& f = views[k];
      if (f.role != 2) continue;
      double dist = Len(f.fr.apex - p.fr.apex);
      // The focus view must look somewhere inside the peripheral view.
      if (Dot(f.fr.centerDir, p.fr.centerDir) < std::cos(p.fr.meanHalfAngle)) continue;
      if (dist <= bestDist) {
        bestDist = dist;
        best = static_cast<int>(k);
      }
    }
    if (best >= 0) {
      p.partner = best;
      ++pairs;
    }
  }
  return pairs;
}

// Quad-Views-Foveated blends the focus layer over the periphery with
// alpha = max(0.5, s.x * s.y), s = smoothstep ramps of width `smoothing` in
// focus texture coordinates. The focus is fully opaque only where both
// texcoords are in [smoothing, 1 - smoothing], i.e. |ndc| < 1 - 2 * smoothing.
std::atomic<double> g_qvSmoothing{-1.0};  // <0 = unknown
std::atomic<bool> g_qvfrLoaded{false};       // Quad-Views-Foveated layer present in DCS
std::atomic<double> g_keepOverride{-1.0};    // set live with Ctrl+Alt+PgUp/PgDn

double ReadQvfrSmoothing() {
  wchar_t base[MAX_PATH];
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return -1;
  std::wstring path = std::wstring(base) + L"\\Quad-Views-Foveated\\Quad-Views-Foveated.log";
  // QVFR keeps its log open for writing: _wfopen_s would demand exclusive
  // read access and fail with a sharing violation.
  FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
  if (!f) return -1;
  std::string text;
  char buf[65536];
  size_t r;
  while ((r = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, r);
  fclose(f);
  size_t session = text.rfind("Session is using quad views");
  if (session == std::string::npos) return -1;
  size_t pos = text.find("Edge smoothing: ", session);
  size_t next = text.find("Session is using quad views", session + 1);
  if (pos == std::string::npos || (next != std::string::npos && pos > next)) return 0.0;  // not logged = off
  return atof(text.c_str() + pos + 16);
}

double EffectiveKeep() {
  if (g_keepOverride.load() >= 0) return Clamp(g_keepOverride.load(), 0.0, 1.0);
  if (g_cfg.keepFraction > 0) return Clamp(g_cfg.keepFraction, 0.0, 1.0);
  // Without QVFR the runtime composites the quad views itself (Pimax native)
  // and its edge blend is unknown: use the configured conservative value.
  if (!g_qvfrLoaded.load()) return Clamp(g_cfg.nativeKeepFraction, 0.0, 1.0);
  double sm = g_qvSmoothing.load();
  if (sm < 0) sm = 0.30;  // conservative until the log is read
  return Clamp(1.0 - 2.0 * sm - g_cfg.safetyNdc, 0.0, 1.0);
}

// Saccade detection. Gaze is measured relative to the head (dot products of
// the focus centre with the peripheral side planes), so head rotation alone
// does not count as a saccade.
std::mutex g_gazeMutex;
double g_prevGaze[2][4] = {};
bool g_havePrevGaze[2] = {};
std::atomic<uint64_t> g_quadFrame{0};
std::atomic<uint64_t> g_holdUntil{0};
std::atomic<uint64_t> g_saccades{0};

bool UpdateGazeAndCheckHold(const std::vector<ViewInfo>& views) {
  uint64_t frame = ++g_quadFrame;
  double threshold = std::sin(g_cfg.saccadeDeg * 0.017453292519943295);
  bool saccade = false;
  {
    std::lock_guard<std::mutex> lock(g_gazeMutex);
    int eye = 0;
    for (const ViewInfo& v : views) {
      if (v.role != 1 || v.partner < 0 || eye >= 2) continue;
      const Frustum& fo = views[v.partner].fr;
      double g[4];
      for (int i = 0; i < 4; ++i) g[i] = Dot(fo.centerDir, v.fr.sides[i].n);
      if (g_havePrevGaze[eye])
        for (int i = 0; i < 4; ++i)
          if (std::fabs(g[i] - g_prevGaze[eye][i]) > threshold) saccade = true;
      memcpy(g_prevGaze[eye], g, sizeof(g));
      g_havePrevGaze[eye] = true;
      ++eye;
    }
  }
  if (saccade && g_cfg.saccadeHoldFrames > 0) {
    g_saccades++;
    g_holdUntil = frame + static_cast<uint64_t>(g_cfg.saccadeHoldFrames) - 1;
  }
  return frame <= g_holdUntil.load();
}

// Swaps in replacement volumes. Returns the number of patched views.
void Prepare(uint32_t count, uint8_t* infos, Patch* patches, size_t maxPatches, int* pairsOut,
             int* patchedOut, std::vector<ViewInfo>& views) {
  int pairs = Classify(count, infos, views);
  *pairsOut = pairs;
  if (g_dumpsLeft.load() > 0 && views.size() >= 4) {
    if (g_dumpsLeft.fetch_sub(1) > 0) DumpViews(count, infos, views);
  }
  if (pairs < 1) return;
  bool hold = UpdateGazeAndCheckHold(views);
  // Camera cut detection (scene_cut.h): the left periphery's camera.
  for (const ViewInfo& v : views)
    if (v.role == 1 && v.fr.valid) {
      const double pos[3] = {v.fr.apex.x, v.fr.apex.y, v.fr.apex.z};
      const double fwd[3] = {v.fr.forward.x, v.fr.forward.y, v.fr.forward.z};
      scenecut::OnCamera(pos, fwd);
      break;
    }
  if (!g_enabled.load() || g_faulted.load() || g_perceptionOnly.load(std::memory_order_relaxed)) return;
  const bool hole = g_debugHole.load();
  if (hold && !hole) return;
  const double keep = EffectiveKeep();
  if (keep <= 0.0 && !hole) return;

  int& n = *patchedOut;
  for (const ViewInfo& v : views) {
    if (static_cast<size_t>(n) >= maxPatches) break;
    const Frustum* excl = nullptr;
    bool shrink = true;
    if (v.role == 1 && v.partner >= 0) {
      excl = &views[v.partner].fr;
    } else if (hole && v.role == 2) {
      excl = &v.fr;  // debug: hide everything fully inside the focus frustum
      shrink = false;
    }
    if (!excl) continue;

    uint8_t* ci = infos + v.index * kCollectionInfoStride;
    uint8_t* src = *reinterpret_cast<uint8_t**>(ci + kCiClipVolume);
    uint8_t block[kPlaneBlockSize];
    BuildExclusionBlock(block, *excl, shrink ? keep : 1.0);

    uint64_t key = (static_cast<uint64_t>(*reinterpret_cast<uint32_t*>(ci + kCiViewportTag)) << 8) |
                   static_cast<uint64_t>(v.index);
    uint8_t* dst = AcquireVolume(key);
    if (!dst || !MakeVolumeWithExclusion(dst, src, block)) continue;
    // DCS culls every pass of a view (depth prepass, G-buffer, forward,
    // transparent...) in the same call, and passes of one view share one
    // ClippingVolume so Scene.dll culls them once. Swap the volume of all of
    // them: consistent results across passes and the sharing is preserved.
    for (uint32_t i = 0; i < count && static_cast<size_t>(n) < maxPatches; ++i) {
      uint8_t* other = infos + i * kCollectionInfoStride;
      if (*reinterpret_cast<uint8_t**>(other + kCiClipVolume) != src) continue;
      patches[n].slot = other + kCiClipVolume;
      patches[n].original = src;
      *reinterpret_cast<uint8_t**>(other + kCiClipVolume) = dst;
      ++n;
    }
  }
}

#include "shadow_tight.h"
#include "alloc_slab.h"
#include "tex_bind.h"
#include "part_weights.h"
#include "terrain_count.h"
#include "binder_count.h"
#include "inst_count.h"
#include "gb_count.h"
#include "shadow_tex.h"
#include "shadow_inst.h"
#include "shadow_batch.h"
#include "gb_batch.h"
#include "fx_apply_count.h"
#include "pacer.h"
#include "pose_sweep.h"
#include "edtime.h"
#include "srv_span_count.h"
#include "join_tail_count.h"
#include "shadow_rec_count.h"
#include "split_filter.h"
#include "srv_tail_trim.h"
#include "pass_flush.h"
#include "shadow_rec.h"
#include "gb_rec_count.h"
#include "gb_rec.h"
#include "fwd_rec_count.h"
#include "gpu_pass_timing.h"
#include "gpu_split.h"
#include "gap_attrib.h"
#include "lod_probe.h"
#include "gpu_dedup.h"
#include "periph_lod.h"
#include "cascade_cache.h"  // R33: far shadow cascade static depth cache ([Shadows] CascadeCache, prototype)
#include "uav_overlap.h"
#include "chain_fast.h"  // R34: shorter terrain/forest culling chains ([D3D] ChainFast, default 7)
#include "shader_fast.h"  // R36: wider grids for two single-SM kernels ([D3D] ShaderFast, default 0)
#include "night_tone.h"   // R37: opt-in night darkening through DCS's tone mapper ([Night] NightTone, default 0)
#include "run_threads.h"
#include "frame_start.h"
#include "frame_heap.h"
#include "big_pages.h"
#include "par_upload.h"
#include "du_count.h"
#include "direct_upload.h"
#include "motion_probes.h"
#include "motion.h"
#include "view_profile.h"
#include "vram_count.h"
#include "prefetch_fix.h"
#include "perception_device.h"  // [General] PerceptionOnly: DCS's D3D11 context without the engine modules
#include "fixation_depth.h"  // Depth boost: fixation depth publisher ([Perception] FixationDepth)
#include "periph_inst_filter.h"  // R38 rank 1: periphery instance filter ([D3D] PeriphInstFilter, default 0)
#include "focus_bcast.h"  // R38 rank 2: [Suite] FocusGeomProbe, [D3D] FocusBroadcast readiness (default 0)
#include "vs_sig_probe.h"  // R38 rank 2 gate G4: [Suite] FocusVsCensus (measure only, default 0)
#include "focus_bcast_run.h"  // R53: FastGS focus broadcast ([D3D] FocusBroadcast=2, default 0)
#include "perf_mon.h"  // R30: env/perf/features/warmup/toggle/vram lines and the governor's driver
#include "thread_place.h"  // R39: [Threads] Census (default off) / WorkerPlacement (R44: default 2, selective)
#include "frame_timeline.h"  // R52: [Perf] FrameTimeline / PoseProbe / EventLog (measure only, default 0)
// R90: the keys of the hitch fixes that are not read where they act (LoadConfig, every config read).
void ApplyR90Config() {
  qvpatch::g_mode = g_cfg.patchMode;
  qvalog::g_want = g_cfg.asyncLog;
  qvalog::g_on = g_logWriter && g_cfg.asyncLog;
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
  if (pairs >= 2 && fbcr::g_probe.load(std::memory_order_relaxed)) {  // R53 census: collect-time view matrices
    int idx[8], role[8], par[8];
    size_t nv = 0;
    for (const ViewInfo& v : views)
      if (nv < 8 && v.role) idx[nv] = v.index, role[nv] = v.role, par[nv] = v.partner, ++nv;
    fbcr::OnCollect(infos, idx, role, par, nv);
  }
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

void ApplyTextureHook() { texbind::SetAttached(g_texDedupeOn.load() || g_hookMeasure.load()); }
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

// ---------------------------------------------------------------------------
// Bench mode 30 ([Suite] BenchPassFlush): OFF = no Flush, ON = the mask. Per
// block: GPU timestamps in light mode (gpu_pass_timing.h: idle between
// top-level passes, frame-start gap), Flush counts and CPU, and the
// frame-start probe (render thread from xrEndFrame to the first pass).
// ---------------------------------------------------------------------------

std::atomic<uint32_t> g_passFlushBenchMask{0};

struct PfBlock {
  bool on = false, valid = false, gpu = false;
  int64_t qa = 0, qb = 0;
  uint64_t frames = 0, flushes = 0, ticks = 0;
  uint64_t reasons[pflush::kReasons] = {};
  double period = 0, span = 0, gaps = 0, afterXr = -1;
  int gpuFrames = 0;
  pflush::Probe probe;
};
std::vector<PfBlock> g_pfBlocks;
uint64_t g_pfF0 = 0, g_pfFl0 = 0, g_pfTk0 = 0, g_pfR0[pflush::kReasons] = {};

void PfBlockBegin(bool on) {
  gpt::TakeDone();  // frames before this block
  pflush::g_probe = false;
  Sleep(20);  // a render thread still writing the probe
  pflush::ProbeReset();
  PfBlock b;
  b.on = on;
  g_pfF0 = g_quadFrame.load();
  pflush::Snapshot(&g_pfFl0, &g_pfTk0, g_pfR0);
  b.qa = pflush::Qpc();
  g_pfBlocks.push_back(std::move(b));
  pflush::g_probe = true;
}

void PfBlockEnd(bool on, bool valid) {
  if (g_pfBlocks.empty()) return;
  PfBlock& b = g_pfBlocks.back();
  b.qb = pflush::Qpc();
  b.on = on;
  b.valid = valid;
  b.frames = g_quadFrame.load() - g_pfF0;
  pflush::g_probe = false;
  uint64_t fl = 0, tk = 0, r[pflush::kReasons] = {};
  pflush::Snapshot(&fl, &tk, r);
  b.flushes = fl - g_pfFl0;
  b.ticks = tk - g_pfTk0;
  for (int i = 0; i < pflush::kReasons; ++i) b.reasons[i] = r[i] - g_pfR0[i];
  Sleep(150);  // GPU frames are read back a few frames late
  b.probe = pflush::g_pr;
  std::vector<gpt::Done> done = gpt::TakeDone(), in;
  for (gpt::Done& d : done)
    if (d.qpc >= b.qa && d.qpc <= b.qb) in.push_back(std::move(d));
  if (in.size() < 3) return;
  gpt::Result res = gpt::Analyze(in, [](void* p) -> std::string { return ptiming::RttiName(p); });
  if (!res.frames || res.span.empty()) return;
  b.gpu = true;
  b.gpuFrames = res.frames;
  b.period = gpt::Mean(res.period);
  b.span = gpt::Mean(res.span);
  b.gaps = gpt::Mean(res.gaps);
  b.afterXr = res.afterXr.empty() ? -1 : gpt::Mean(res.afterXr);
}

double PfMs(double us, uint64_t n) { return n ? us / static_cast<double>(n) / 1000.0 : 0.0; }

void PfReport() {
  const size_t n = g_pfBlocks.size();
  std::vector<bool> on(n), valid(n), gpuValid(n), xrValid(n), prValid(n), bgValid(n);
  for (size_t i = 0; i < n; ++i) {
    const PfBlock& b = g_pfBlocks[i];
    on[i] = b.on;
    valid[i] = b.valid && b.frames > 0;
    gpuValid[i] = valid[i] && b.gpu;
    xrValid[i] = gpuValid[i] && b.afterXr >= 0;
    prValid[i] = valid[i] && b.probe.withXr > 0;
    bgValid[i] = valid[i] && b.probe.withBegin > 0;
  }
  auto field = [&](double (*get)(const PfBlock&)) {
    std::vector<double> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = get(g_pfBlocks[i]);
    return v;
  };
  auto row = [&](const char* name, const std::vector<double>& v, const std::vector<bool>& ok) {
    const pflush::Paired p = pflush::PairedChange(v, on, ok);
    if (!p.pairs) {
      Log("  %-50s n/a", name);
      return;
    }
    Log("  %-50s OFF %8.3f  ON %8.3f  change %+7.3f +/- %.3f %s", name, p.off, p.on, p.delta, p.ci,
        p.pairs > 1 && std::fabs(p.delta) > p.ci ? "(significant)" : "(within noise)");
  };
  Log("  pass flush, ON blocks vs the mean of their OFF neighbours (ms unless noted):");
  row("gpu: idle between top-level passes", field([](const PfBlock& b) { return b.gaps; }), gpuValid);
  row("gpu: xrEndFrame end -> next first pass", field([](const PfBlock& b) { return b.afterXr; }), xrValid);
  row("gpu: passes span", field([](const PfBlock& b) { return b.span; }), gpuValid);
  row("gpu: frame period", field([](const PfBlock& b) { return b.period; }), gpuValid);
  row("flushes per frame (count)",
      field([](const PfBlock& b) { return b.frames ? b.flushes / static_cast<double>(b.frames) : 0.0; }), valid);
  row("render-thread CPU in Flush per frame",
      field([](const PfBlock& b) { return PfMs(b.ticks * g_qpcToUs, b.frames); }), valid);
  row("cpu: xrEndFrame end -> first pass",
      field([](const PfBlock& b) { return PfMs(b.probe.xrToPassUs, b.probe.withXr); }), prValid);
  row("cpu: xrEndFrame end -> xrBeginFrame wrapper",
      field([](const PfBlock& b) { return PfMs(b.probe.xrToBeginUs, b.probe.withBegin); }), bgValid);
  row("cpu: xrBeginFrame wrapper (begin, 4 x acquire/wait)",
      field([](const PfBlock& b) { return PfMs(b.probe.beginUs, b.probe.withBegin); }), bgValid);
  row("cpu: xrBeginFrame wrapper end -> first pass",
      field([](const PfBlock& b) { return PfMs(b.probe.beginToPassUs, b.probe.withBegin); }), bgValid);
  row("cpu: first top-level pass",
      field([](const PfBlock& b) { return PfMs(b.probe.firstPassUs, b.probe.firstPassN); }), prValid);
  // Totals by side: Flush reasons, probe coverage and the frame-start p95.
  for (int side = 0; side < 2; ++side) {
    uint64_t frames = 0, fl = 0, r[pflush::kReasons] = {}, pf = 0, px = 0, pb = 0, po = 0, pc = 0;
    std::vector<float> x2p;
    void* firstVt = nullptr;
    for (size_t i = 0; i < n; ++i) {
      const PfBlock& b = g_pfBlocks[i];
      if (!valid[i] || b.on != (side == 1)) continue;
      frames += b.frames;
      fl += b.flushes;
      for (int k = 0; k < pflush::kReasons; ++k) r[k] += b.reasons[k];
      pf += b.probe.frames;
      px += b.probe.withXr;
      pb += b.probe.withBegin;
      po += b.probe.beginOutside;
      pc += b.probe.counterFirst;
      x2p.insert(x2p.end(), b.probe.xrToPass.begin(), b.probe.xrToPass.end());
      if (!firstVt) firstVt = b.probe.firstVt;
    }
    if (!frames) continue;
    std::string reasons;
    char buf[96];
    for (int k = 0; k < pflush::kReasons; ++k)
      if (r[k]) {
        snprintf(buf, sizeof(buf), "%s%s %.2f", reasons.empty() ? "" : ", ", pflush::kReasonNames[k],
                 r[k] / static_cast<double>(frames));
        reasons += buf;
      }
    double p95 = 0;
    if (!x2p.empty()) {
      const size_t k = std::min(x2p.size() - 1, static_cast<size_t>(x2p.size() * 0.95));
      std::nth_element(x2p.begin(), x2p.begin() + k, x2p.end());
      p95 = x2p[k] / 1000.0;
    }
    const std::string firstName = firstVt ? ptiming::RttiName(firstVt) : std::string("?");
    Log("  %s: %.2f flushes/frame%s%s%s; probe: %llu first passes, %llu after an xrEndFrame end, %llu also first "
        "since the frame counter moved (bit 0x80's point), %llu with the xrBeginFrame wrapper between, %llu wrapper "
        "calls elsewhere; cpu xrEndFrame end -> first pass p95 %.3f ms; first pass %s",
        side ? "ON " : "OFF", fl / static_cast<double>(frames), reasons.empty() ? "" : " (", reasons.c_str(),
        reasons.empty() ? "" : ")", static_cast<unsigned long long>(pf), static_cast<unsigned long long>(px),
        static_cast<unsigned long long>(pc), static_cast<unsigned long long>(pb), static_cast<unsigned long long>(po),
        p95, firstName.c_str());
  }
  Log("  (gpu rows: GpuPassTiming light mode, %s; cpu rows: render-thread QPC)",
      gpt::g_xrSlot ? "xrEndFrame stamped" : "xrEndFrame not stamped, no frame-start row");
}

// The phase. False when aborted.
bool PassFlushBench(uint32_t mask) {
  if (!PassFlushPrepare()) {
    Log("  pass flush: DCS's device or the pass execute hook is not available (the device comes from [Model] "
        "ShadowInstancing=1 or [D3D] SplitFilter=1)");
    return true;
  }
  g_passFlushPhase = true;
  g_passFlushBenchMask = mask & pflush::kMaskBits;
  if (!pflush::InstallXr())
    Log("  pass flush: xrBeginFrame not hooked: no wrapper rows%s",
        (mask & pflush::kAfterXrBegin) ? ", and the after-xrBeginFrame bit does nothing" : "");
  pflush::SetMask(0);
  pflush::SetSession(true);
  const bool gpu = gpt::Begin(g_quadFrame, true);
  if (!gpu) Log("  pass flush: GPU timestamps not available; GPU rows n/a");
  g_pfBlocks.clear();
  g_benchBlockBegin = &PfBlockBegin;
  g_benchBlockEnd = &PfBlockEnd;
  const uint64_t xb0 = pflush::g_xrBeginCalls.load(), f0 = g_quadFrame.load();
  const bool ok = RunBenchmark(30, false);  // ends with ApplyPassFlush (the configured mask back)
  g_benchBlockBegin = nullptr;
  g_benchBlockEnd = nullptr;
  const uint64_t frames = std::max<uint64_t>(1, g_quadFrame.load() - f0);
  if (gpu)
    Log("  pass flush: GPU frames %llu opened, %llu read (truncated %llu, disjoint %llu, dropped unread %llu); "
        "timestamp cost on the render thread %.3f ms/frame (both sides)",
        static_cast<unsigned long long>(gpt::g_s.framesOpened), static_cast<unsigned long long>(gpt::g_s.framesRead),
        static_cast<unsigned long long>(gpt::g_s.truncated), static_cast<unsigned long long>(gpt::g_s.disjoint),
        static_cast<unsigned long long>(gpt::g_s.dropped), gpt::OverheadMsPerFrame());
  gpt::End();
  pflush::SetSession(false);
  g_passFlushPhase = false;
  ApplyPassFlush();  // the configured mask; the xrBeginFrame hook only if it asks for it
  Log("  pass flush: render thread %s; xrBeginFrame wrapper calls on it %.2f per frame",
      pflush::g_rt.load() ? "latched at the first cascade" : "NOT latched (no cascade pass seen)",
      (pflush::g_xrBeginCalls.load() - xb0) / static_cast<double>(frames));
  if (ok) PfReport();
  g_pfBlocks.clear();
  return ok;
}

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
  g_texDedupeOn = g_cfg.texDedupe && !off;
  g_triPlainOn = g_cfg.triPlain && !off;
  ApplyTriCounter();
  g_cbSkipOn = g_cfg.cbSkip && !off;
  ApplyCbSkip();
  g_costWeightsOn = g_cfg.costWeights && !off;
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

DWORD WINAPI ProfileThread(void*) {
  char label[64];
  snprintf(label, sizeof(label), "optimisation %s, keep %.2f", g_enabled.load() ? "ON" : "OFF",
           EffectiveKeep());
  Chime(1000, 60);
  prof::Run(std::max(2, g_cfg.profileSeconds), std::max(1, g_cfg.profileThreads),
            std::max(1, g_cfg.profilePeriodMs), label);
  Chime(1000, 60);
  Chime(1000, 60);
  return 0;
}

// Last occurrence of `marker` in a text file (read with full sharing), with
// the rest of that line.
std::string LastLineWith(const std::wstring& path, const char* marker) {
  FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
  if (!f) return {};
  std::string text;
  char buf[65536];
  size_t r;
  if (_fseeki64(f, 0, SEEK_END) == 0) {
    long long size = _ftelli64(f);
    _fseeki64(f, size > 4 * 1024 * 1024 ? size - 4 * 1024 * 1024 : 0, SEEK_SET);
  }
  while ((r = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, r);
  fclose(f);
  size_t p = text.rfind(marker);
  if (p == std::string::npos) return {};
  size_t b = text.rfind('\n', p);
  size_t e = text.find('\n', p);
  return text.substr(b == std::string::npos ? 0 : b + 1, (e == std::string::npos ? text.size() : e) - (b == std::string::npos ? 0 : b + 1));
}

// The state a session-state line of dcs.log names ("... -> XR_SESSION_STATE_X ..."); empty when none.
std::string XrStateOfLine(const std::string& st) {
  size_t a = st.rfind("->");
  if (a == std::string::npos) return {};
  size_t e = st.find_first_of(" \r", a + 2);
  return st.substr(a + 2, (e == std::string::npos ? st.size() : e) - a - 2);
}

// The last OpenXR session state DCS logged. Each poll reads only what the log
// gained since the previous one (all of it at the first poll, again after it
// shrank: a new log), and a state found is kept until a newer line names
// another. Before, every poll (each 2 s, the whole session) re-read the last
// 4 MB of dcs.log, and once more than 4 MB followed the last state change
// (which DCS logs only on transitions) the state read as "unknown", so a
// worn, FOCUSED headset counted as a focus loss (suite waits, bench blocks
// discarded).
struct XrStateTail {
  long long offset = 0;
  std::string partial;  // the last line read, not finished yet
  std::string state = "unknown";
  std::string Poll(const std::wstring& path) {
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return state;
    if (_fseeki64(f, 0, SEEK_END) == 0) {
      const long long size = _ftelli64(f);
      if (size < offset) {  // truncated or replaced
        offset = 0;
        partial.clear();
        state = "unknown";
      }
    }
    _fseeki64(f, offset, SEEK_SET);
    static const char kMarker[] = "XrEventDataSessionStateChanged";
    char buf[65536];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
      offset += static_cast<long long>(r);
      size_t from = 0;
      for (size_t i = 0; i < r; ++i) {
        if (buf[i] != '\n') continue;
        partial.append(buf + from, i - from);
        if (partial.find(kMarker) != std::string::npos) {
          std::string s = XrStateOfLine(partial);
          if (!s.empty()) state = s;
        }
        partial.clear();
        from = i + 1;
      }
      if (partial.size() < 4096) partial.append(buf + from, std::min<size_t>(r - from, 4096));  // no line is longer
    }
    fclose(f);
    return state;
  }
};

std::string XrSessionState() {
  static std::mutex m;  // the worker and the suite's report both ask
  static XrStateTail tail;
  std::lock_guard<std::mutex> lock(m);
  return tail.Poll(g_dir + L"..\\..\\Logs\\dcs.log");
}

// Blocks until the session is FOCUSED (headset awake), up to 60 minutes.
bool WaitForFocus() {
  if (g_xrFocused.load()) return true;
  Log("  session not FOCUSED (headset asleep?): move the headset, the test waits");
  for (int i = 0; i < 1800 && !g_benchAbort.load() && !g_stop.load(); ++i) {
    if (i % 15 == 0) {
      Chime(400, 150);
      Chime(400, 150);
    }
    Sleep(2000);
    if (g_xrFocused.load()) {
      Log("  session FOCUSED again, continuing in 3 s");
      Sleep(3000);
      return true;
    }
  }
  return false;
}

// Folder of the process image (DCS.exe in DCS's bin folder), with trailing
// backslash; empty when it cannot be read. Long paths are handled (the
// buffer grows while GetModuleFileNameW truncates).
std::wstring ProcessImageDir() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, &path[0], static_cast<DWORD>(path.size()));
    if (n == 0) return std::wstring();
    if (n < path.size()) {
      path.resize(n);
      break;
    }
    if (path.size() >= 32768) return std::wstring();
    path.resize(path.size() * 2);
  }
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
}

// One-key test suite: configuration check, CPU profile, exclusion A/B and
// collect-threads A/B, summarised in one report file.
void CheckConfiguration() {
  struct Mod {
    const wchar_t* file;
    const char* what;
    bool wanted;
  };
  const Mod mods[] = {
      {L"XR_APILAYER_MBUCCHIA_quad_views_foveated.dll", "Quad-Views-Foveated layer", true},
      // The old loader-chain prefetch fix; replaced by [Prefetch] (prefetch_fix.h), which takes over its slots.
      {L"dxgi2.dll", "old prefetch fix (bin/dxgi2.dll, now in the payload)", false},
      {L"PiOpenXR_64.dll", "Pimax OpenXR runtime", true},
      {L"CheekyOpenXRLayer.dll", "Cheeky OpenXR layer (absent in base)", false},
      {L"XR_APILAYER_XRFrameBridge_diagnostic.dll", "OFXR frame bridge / frame-gen (absent in base)", false},
  };
  Log("-- configuration --");
  // R30 L8: name the runtime; Pimax and QVFR are only expected on a Pimax runtime (no false warnings for others).
  const std::string runtime = envinfo::XrRuntime();
  const bool pimax = runtime.find("Pimax") != std::string::npos || runtime.find("PiOpenXR") != std::string::npos ||
                     GetModuleHandleW(L"PiOpenXR_64.dll") != nullptr;
  Log("  OpenXR runtime: %s; view layout: %d views per frame, %d focus/peripheral pairs", runtime.c_str(),
      perfgov::g_views.load(), perfgov::g_pairs.load());
  for (const Mod& m : mods) {
    bool present = GetModuleHandleW(m.file) != nullptr;
    const bool pimaxOnly = wcscmp(m.file, L"PiOpenXR_64.dll") == 0 ||
                           wcscmp(m.file, L"XR_APILAYER_MBUCCHIA_quad_views_foveated.dll") == 0;
    const bool check = pimaxOnly && !pimax ? false : present != m.wanted;
    Log("  %-50s %s%s", m.what, present ? "loaded" : "not loaded", check ? "   <-- check" : "");
  }
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  // The prefetch fix logs next to DCS.exe: DCS's bin folder from the process
  // image (was a hard-coded install path of one machine).
  const std::wstring binDir = ProcessImageDir();
  if (!binDir.empty() &&
      GetFileAttributesExW((binDir + L"DcsVrPrefetchFix.log").c_str(), GetFileExInfoStandard, &fad)) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = now.dwLowDateTime;
    a.HighPart = now.dwHighDateTime;
    b.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    b.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    double age = (a.QuadPart - b.QuadPart) / 1e7;
    Log("  prefetch fix log last written %.0f s ago%s", age, age > 60 ? "   <-- fix not active?" : "");
  }
  Log("  payload prefetch fix: %s, mode %s, pointers=%u imports=%u takeovers=%llu",
      pffix::g_state.load() == 1 ? "installed" : pffix::g_state.load() < 0 ? "unavailable" : "not installed",
      pffix::g_mode.load() == pffix::kSkip ? "skip" : pffix::g_mode.load() == pffix::kObserve ? "observe" : "off",
      pffix::g_livePointers.load(), pffix::g_liveImports.load(),
      static_cast<unsigned long long>(pffix::g_takeovers.load()));
  Log("  keep fraction %.2f, QVFR edge smoothing %.2f", EffectiveKeep(), g_qvSmoothing.load());
  // OpenXR session state (DCS log) and headset screen state (Pimax runtime log):
  // with the headset still for longer than Pimax's "Headset Sleep" time the
  // screen turns off, the session drops to VISIBLE and DCS slows down.
  {
    std::string state = XrSessionState();
    bool focused = state == "XR_SESSION_STATE_FOCUSED";
    Log("  OpenXR session state: %s%s", state.c_str(), focused ? "" : "   <-- not FOCUSED: DCS throttles, results invalid");
    wchar_t lad[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH)) {
      std::wstring dir = std::wstring(lad) + L"\\Pimax\\runtime\\";
      WIN32_FIND_DATAW fd;
      HANDLE h = FindFirstFileW((dir + L"pvr_srv_log_*.txt").c_str(), &fd);
      std::wstring newest;
      FILETIME best{};
      if (h != INVALID_HANDLE_VALUE) {
        do {
          if (CompareFileTime(&fd.ftLastWriteTime, &best) > 0) {
            best = fd.ftLastWriteTime;
            newest = fd.cFileName;
          }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
      }
      if (!newest.empty()) {
        std::string scr = LastLineWith(dir + newest, "setScreenState: ");
        bool on = !scr.empty() && scr.back() != '0' && scr.find("setScreenState: 0") == std::string::npos;
        Log("  headset screen: %s%s", scr.empty() ? "unknown" : (on ? "on" : "OFF"),
            on || scr.empty() ? "" : "   <-- screen off (Pimax Headset Sleep): move the headset");
      }
    }
  }
  Log("  GPU utilisation now: %d%%", gpu::Init() ? gpu::Utilization() : -1);
  if (void* sc = g_scene.load()) {
    uint32_t tmax = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(sc) + kSceneCollectThreadsMax);
    uint32_t f = g_lastCollectFlags.load();
    uint32_t m = std::min(tmax, f);
    uint32_t tasks = std::min<uint32_t>((m + 1) * (tmax > 1 ? 2 : 4), 16);
    Log("  culling partition: threadsMax %u, collect arg %u -> up to %u parse tasks per collect", tmax, f, tasks);
  }
}

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

void DumpLodFields() {
  g_infoSnapRequest = true;
  for (int i = 0; i < 100 && g_infoSnapRequest.load(); ++i) Sleep(20);
  std::lock_guard<std::mutex> lock(g_infoSnapMutex);
  if (g_infoSnap.empty()) {
    Log("  no snapshot");
    return;
  }
  for (uint32_t i = 0; i < g_infoSnapCount; ++i) {
    const uint8_t* ci = g_infoSnap.data() + i * kCollectionInfoStride;
    uint16_t sm = *reinterpret_cast<const uint16_t*>(ci);
    uint32_t tag = *reinterpret_cast<const uint32_t*>(ci + kCiViewportTag);
    if (!(sm == 0 || (sm == 14 && tag == 300))) continue;
    Log("  view tag %u shading %u:", tag, sm);
    for (size_t off = 0x340; off < 0x3e0; off += 0x10) {
      const uint8_t* p = ci + off;
      const float* f = reinterpret_cast<const float*>(p);
      const int32_t* n = reinterpret_cast<const int32_t*>(p);
      const double* d = reinterpret_cast<const double*>(p);
      Log("    +%03zx f[%10.4g %10.4g %10.4g %10.4g] i[%d %d %d %d] d[%.6g %.6g]", off, f[0], f[1], f[2], f[3], n[0],
          n[1], n[2], n[3], d[0], d[1]);
    }
  }
}

// Measurement hooks (timelines, culling tasks, per-model cost, shadow and
// render passes). Not needed for the optimisations themselves: installed at
// start only with [General] Diagnostics=1, otherwise when a suite starts.
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
    const bool split = g_cfg.suiteGpuSplit >= 1 && gsplit::BeginBrackets();
    gpt::Measure(5000, g_quadFrame);
    if (split) gsplit::EndBrackets();
    gpt::g_statsWanted = false;
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteGapAttrib > 0 && !g_benchAbort) {
    Log("-- between-pass GPU gaps: outside-pass ops bracketed, CPU issue time of every stamp, gap pipeline "
        "statistics, GPU clock calibrated every %d frames (gap_attrib.h, %d s) --",
        g_cfg.suiteGapAttribCalEvery, g_cfg.suiteGapAttrib);
    gapat::g_dumpPath = g_cfg.suiteGapAttribDump && !g_devDir.empty() ? g_devDir + L"gap_passes.csv" : std::wstring();
    gapat::Measure(g_cfg.suiteGapAttrib * 1000, g_cfg.suiteGapAttribCalEvery, g_quadFrame);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteGpuDedup > 0 && !g_benchAbort) {
    Log("-- exact-redundancy census: content ids of every resource, signature of every dispatch and draw, %d frames "
        "captured after %d (gpu_dedup.h) --",
        g_cfg.suiteGpuDedup, g_cfg.suiteGpuDedupWarm);
    gdd::Measure(g_cfg.suiteGpuDedup, g_cfg.suiteGpuDedupWarm, static_cast<size_t>(g_cfg.suiteGpuDedupHashCap),
                 g_devDir, g_cfg.suiteGpuDedupSnap, g_quadFrame);
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteLodProbe > 0 && g_cfg.suiteLodProbe < 100 && !g_benchAbort) {
    Log("-- LOSSY test: instancer (trees, buildings) LOD factors x %.2f in all views for one GPU timing run, then "
        "restored (lod_probe.h, 3 x 5 s) --",
        g_cfg.suiteLodProbe / 100.0);
    gpt::g_statsWanted = g_cfg.suiteGpuPassStats;
    lodp::RunPhase(g_cfg.suiteLodProbe / 100.0, 5000, g_quadFrame);
    gpt::g_statsWanted = false;
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteUavOverlap > 0 && !g_benchAbort) {
    Log("-- MEASUREMENT ONLY (output may be wrong while ON in modes 1-2): %s, GPU timing OFF/ON/OFF/ON/OFF "
        "(uav_overlap.h mode %d, 5 x 5 s) --",
        g_cfg.suiteUavOverlap == 2   ? "NVAPI UAV overlap around every top-level pass"
        : g_cfg.suiteUavOverlap == 1 ? "NVAPI UAV overlap around the culling-chain passes"
                                     : "renamed duplicates of every culling-chain dispatch",
        g_cfg.suiteUavOverlap);
    gpt::g_statsWanted = g_cfg.suiteGpuPassStats;
    uovl::RunPhase(g_cfg.suiteUavOverlap, g_quadFrame);
    gpt::g_statsWanted = false;
    Chime(1000, 60);
  }
  if (ok && g_cfg.suiteFocusGeomProbe > 0 && !g_benchAbort) {
    Log("-- LOSSY probe (5 s ON runs): main GBufferPass of view mask 0x%x geometry-only (off-target viewport), GPU "
        "timing OFF/ON/OFF/ON/OFF (focus_bcast.h, 5 x 5 s) --",
        g_cfg.suiteFocusGeomProbe);
    gpt::g_statsWanted = g_cfg.suiteGpuPassStats;
    fbc::RunPhase(static_cast<uint32_t>(g_cfg.suiteFocusGeomProbe), g_quadFrame);
    gpt::g_statsWanted = false;
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
  if (ok && !g_benchAbort) {  // R38 rank 1: [Suite] PeriphCensus / PeriphInstTiming / PeriphInstVerify (no-op at 0)
    gpt::g_statsWanted = g_cfg.suiteGpuPassStats;
    pifilt::SuitePhases(g_quadFrame);
    gpt::g_statsWanted = false;
  }
  if (ok && !g_benchAbort) vsig::SuitePhase(g_quadFrame);  // R38 rank 2 G4: [Suite] FocusVsCensus (no-op at 0)
  if (ok && !g_benchAbort) fbcr::SuitePhases(g_quadFrame);  // R53: [Suite] FocusBcastProbe / Timing / Verify (no-op at 0)
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
  if (ok && g_cfg.suiteGpuSplit >= 2 && !g_benchAbort) {
    Log("-- recorded meshes: index buffers read back once, post-transform vertex reuse as drawn and after a reorder "
        "(gpu_split.h census, %d frames) --", g_cfg.suiteGpuSplitFrames);
    gsplit::Census(g_cfg.suiteGpuSplitFrames);
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
                             g_cfg.d3dMeter || g_cfg.suiteGpuPassTiming || g_cfg.suiteGpuDedup > 0,
                             g_cfg.cockpitTrueIpdPrevLog, g_cfg.cockpitTrueIpdOverlays,
                             g_cfg.cockpitTrueIpdOverlayMvp, g_cfg.cockpitTrueIpdFrameDump,
                             g_cfg.cockpitTrueIpdMvpDump, g_dir, g_cfg.cockpitTrueIpdOverlayRestore,
                             g_cfg.cockpitTrueIpdFrameDumpViews, g_cfg.cockpitTrueIpdOverlayPath);
  fixdepth::ckipd::g_cfgHelmet = g_cfg.cockpitTrueIpdHelmet;
  fixdepth::ckipd::SetOwnship(OwnshipMode(g_cfg.cockpitTrueIpdOwnship, g_perceptionOnly.load()));
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
  fixdepth::cmask::covl::g_on = g_cfg.cockpitMaskOverlay;  // R74b: read per pass boundary (no reinstall)
  fixdepth::cmask::Configure(g_cfg.cockpitMaskExport, g_cfg.cockpitMaskDivisor, g_cfg.cockpitMaskViews,
                             g_cfg.cockpitMaskNearM, g_cfg.cockpitMaskDilate, g_cfg.cockpitMaskDump, g_cfg.cockpitMaskLog,
                             g_devDir.empty() ? g_dir : g_devDir);
  // R51: GazeDof needs FixationDepth's published cone (DepthIn): [VrFx] GazeDof turns it on by itself.
  fixdepth::Tick(g_cfg.fixationDepth || vrfx::g_wantDof.load(), g_cfg.depthGrid, g_cfg.fixationDepthVerify, g_cfg.fixationTestGaze.c_str(),
                 g_cfg.fixationDepthDump, g_devDir.empty() ? g_dir : g_devDir, g_cfg.farDepthExport,
                 g_cfg.farDepthSlots, g_cfg.farDepthLogProj, g_cfg.farDepthPreOverlay, g_cfg.farDepthOverlayTrace,
                 g_cfg.farDepthPostOverlay, g_cfg.farDepthCoverageMode);
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
  // R34 [D3D] ChainFast: the creation hook goes in first, before DCS loads the mission's terrain kernels.
  if (!g_perceptionOnly.load() && g_cfg.chainFast && !g_engineOff.load()) chainfast::Apply(g_cfg.chainFast);
  // R90 F4: the self tests' private device now, not when the first kernel turns up mid-flight.
  if (!g_perceptionOnly.load() && (g_cfg.chainFast || g_cfg.shaderFast)) chainfast::AcquirePrivate();
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
        fbcr::NoteFrames(g_quadFrame.load());  // R53: start gate (nothing created before frames flow)
        if (g_cfg.focusBroadcast) fbc::Readiness(g_cfg.focusBroadcast);  // R38 rank 2: logs only (default 0)
        if (!g_benchRunningFlag.load()) {  // R53: FastGS focus broadcast (default 0: nothing installed)
          static bool fbcrDirs = false;
          if (!fbcrDirs) fbcr::SetDirs(g_dir, g_devDir.empty() ? g_dir : g_devDir), fbcrDirs = true;
          fbcr::Apply(g_engineOff.load());
        }
        if (!g_benchRunningFlag.load()) pifilt::Apply(g_engineOff.load());  // R38 rank 1 (default 0: nothing installed)
        vsig::Apply();  // R38 rank 2 G4 probe: device hooks only while [Suite] FocusVsCensus > 0
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
  d3ds::Uninstall();
  pdev::Uninstall();    // [General] PerceptionOnly: DX11Renderer::draw back to DCS's if the first draw never came
  srvspan::Shutdown();  // restores the ApplyShaderBlock call sites if a count was running
  srvtrim::Shutdown();  // the same two call sites back to stock if the trim was on
  fbc::Shutdown();      // R38 rank 2 probe off, its RSSetViewports hook out (no-op unless used)
  uovl::Shutdown();     // NVAPI overlap probe off (its region ends at the pass end)
  plod::Shutdown();     // stock LOD tables back, view callback and vt[24] hook out (no-op unless used)
  cascache::Shutdown(); // R33 cascade cache: SceneRender slot and the innermost execute out, textures released (no-op unless used)
  fixdepth::Shutdown(); // depth callback out and drained, worker joined, D3D objects and file mappings released
  cpass::Shutdown();    // R42: our shaders and buffers (the draw hooks are out with fixation depth)
  gpt::Shutdown();      // GPU pass timing's context-table hooks and xrEndFrame hook, if a phase was running
  fbcr::Shutdown();      // R53: blocks closed, callbacks and upload observer out, worker joined (no-op unless used)
  vsig::Shutdown();      // R38 rank 2 G4 probe: device hooks out (no-op unless used)
  pifilt::Shutdown();    // R38 rank 1: boundary callback and table arms out, DCS's lists stock (no-op unless used)
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
