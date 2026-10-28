// Per-view-direction and in-rotation cost profile (suite phases, measure only):
//  - [Suite] YawProfile=1: for yaw 0..330 step [Suite] YawProfileStep (30),
//    the view is held (posesweep::Hold), 2 s settle, 2.5 s measured; one row
//    per yaw, then the heaviest yaw and what limits each direction. The hold
//    in force before the phase is put back afterwards.
//  - [Suite] RotationProfile=1: 3 s still (the current view), then a
//    constant-rate turn at [Suite] RotationDegPerSec (60) for
//    [Suite] RotationSeconds (20); per-frame cost split as in the yaw rows,
//    the transient costs that appear only while turning, and the frame-time
//    spikes (> 1.5x the median) with what was elevated in those frames; per
//    frame CSV rotation_*.csv next to the log.
//
// Cost of the measurement (no per-draw work added): one record per quad frame
// from the collect hook (QueryThreadCycleTime of the render thread, a few
// dozen relaxed loads of the recorders' counters, one double read of
// Visualizer's pacer object); pass_timing's per-pass CPU timer (QPC pair and a
// map update per pass execute); gpu_pass_timing in light mode (one D3D11
// timestamp per pass boundary, no context-op hooks); xrEndFrame wall time
// (QPC pair per frame); a counting hook on ID3D11Device::CreateTexture2D
// (texture creations, i.e. streaming uploads; rare calls); and the texture
// streaming hook's plain request counter (texbind::g_passed, always on).
// The render-thread cost of the timestamps is logged with the results.
//
// Bound classification (Classify, offline tested), per window, in order:
//  GPU-bound      the render thread blocks in xrEndFrame (the driver waits
//                 there for the GPU [V R15: nvwgf2umx wait inside xrEndFrame
//                 in motion]) for >= 10% of the frame and the GPU is idle
//                 (between frames + between top-level passes) <= 10% of the
//                 frame, or xrEndFrame alone takes >= 25% of the frame;
//  pacing-bound   the render thread waits for Main's pacer (Visualizer
//                 S+0x118 [V R2 §4]) >= 10% of the frame;
//  worker-bound   recorder jobs not ready at their pass (fallback to stock
//                 drawing) >= 0.25 per frame, or the render thread's wait for
//                 recorder jobs >= 5% of the frame;
//  CPU-bound      otherwise (the render thread's own work).
// Included once from main.cpp inside its anonymous namespace, after motion.h.
#pragma once

namespace vprof {

// ---------------------------------------------------------------------------
// Pure helpers (offline tested)
// ---------------------------------------------------------------------------

enum Bound : int { kBCpu = 0, kBWorker, kBGpu, kBPacing, kBounds };
const char* const kBoundName[kBounds] = {"CPU-bound (render thread)", "worker-bound (recorder late)", "GPU-bound",
                                         "pacing-bound"};

struct BoundIn {
  double frameMs = 0;      // mean frame interval
  double xrEndMs = 0;      // render-thread wall time in the xrEndFrame wrapper per frame
  double pacerMs = 0;      // render-thread wait for Main's pacer per frame
  double recWaitMs = 0;    // render-thread wait for recorder jobs per frame
  double recLate = 0;      // recorder jobs still running at their pass, per frame
  bool gpuValid = false;   // GPU timestamps read for this window
  double gpuIdleMs = 0;    // GPU time between frames and between top-level passes per frame
};

inline Bound Classify(const BoundIn& in) {
  const double f = in.frameMs > 0 ? in.frameMs : 1.0;
  if (in.xrEndMs >= 0.25 * f) return kBGpu;
  if (in.xrEndMs >= 0.10 * f && in.gpuValid && in.gpuIdleMs <= 0.10 * f) return kBGpu;
  if (in.pacerMs >= 0.10 * f) return kBPacing;
  if (in.recLate >= 0.25 || in.recWaitMs >= 0.05 * f) return kBWorker;
  return kBCpu;
}

inline double Pct(std::vector<float> v, double p) {
  if (v.empty()) return 0;
  const size_t k = std::min(v.size() - 1, static_cast<size_t>(v.size() * p));
  std::nth_element(v.begin(), v.begin() + k, v.end());
  return v[k];
}
inline double MeanF(const std::vector<float>& v) {
  if (v.empty()) return 0;
  double s = 0;
  for (float x : v) s += x;
  return s / v.size();
}

// Frames whose time exceeds factor x the median (indices into frameMs).
inline std::vector<size_t> Spikes(const std::vector<float>& frameMs, double factor, double* median) {
  std::vector<size_t> out;
  const double m = Pct(frameMs, 0.5);
  if (median) *median = m;
  if (m <= 0) return out;
  for (size_t i = 0; i < frameMs.size(); ++i)
    if (frameMs[i] > factor * m) out.push_back(i);
  return out;
}

// One per-frame quantity; ms = a time (else a count). Negative values = not
// measured in that frame (skipped).
struct Series {
  const char* name;
  bool ms;
  std::vector<float> v;
};
struct Elev {
  const char* name;
  bool ms;
  double median, spikeMean;
};
// Quantities elevated in the spike frames: spike mean > 1.25 x the median of
// all frames and above it by >= 0.25 ms (times) or >= 1 (counts); times first,
// each group by the excess, largest first.
inline std::vector<Elev> Elevated(const std::vector<Series>& series, const std::vector<size_t>& idx) {
  std::vector<Elev> out;
  for (const Series& s : series) {
    std::vector<float> all;
    for (float x : s.v)
      if (x >= 0) all.push_back(x);
    if (all.empty()) continue;
    double sum = 0;
    size_t n = 0;
    for (size_t i : idx)
      if (i < s.v.size() && s.v[i] >= 0) {
        sum += s.v[i];
        ++n;
      }
    if (!n) continue;
    const double med = Pct(all, 0.5), mean = sum / n;
    if (mean > 1.25 * med && mean - med >= (s.ms ? 0.25 : 1.0)) out.push_back({s.name, s.ms, med, mean});
  }
  std::stable_sort(out.begin(), out.end(), [](const Elev& a, const Elev& b) {
    if (a.ms != b.ms) return a.ms;
    return a.spikeMean - a.median > b.spikeMean - b.median;
  });
  return out;
}

// Approximate bytes of a 2D texture's initial data (pitch x rows per
// subresource; block-compressed formats have 4-pixel rows).
inline uint64_t InitBytes(const D3D11_TEXTURE2D_DESC& d, const D3D11_SUBRESOURCE_DATA* init) {
  if (!init) return 0;
  const UINT f = d.Format;
  const bool bc = (f >= 70 && f <= 84) || (f >= 94 && f <= 99);
  const UINT mips = d.MipLevels ? d.MipLevels : 1;
  const UINT arr = d.ArraySize ? d.ArraySize : 1;
  uint64_t bytes = 0;
  for (UINT a = 0; a < arr; ++a)
    for (UINT m = 0; m < mips; ++m) {
      const UINT h = std::max(1u, d.Height >> m);
      const UINT rows = bc ? std::max(1u, (h + 3) / 4) : h;
      bytes += static_cast<uint64_t>(init[a * mips + m].SysMemPitch) * rows;
    }
  return bytes;
}

// ---------------------------------------------------------------------------
// Texture creations: ID3D11Device::CreateTexture2D (vtable slot 5), counted
// while a profile runs (DCS creates a texture per streamed mip set [I]).
// ---------------------------------------------------------------------------

using CreateTex2DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                  const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
CreateTex2DFn g_ct2Orig = nullptr;
void** g_ct2Slot = nullptr;
std::atomic<uint64_t> g_texNew{0}, g_texNewBytes{0};

HRESULT STDMETHODCALLTYPE HookCreateTex2D(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* d,
                                          const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
  if (d && init && out) {
    g_texNew.fetch_add(1, std::memory_order_relaxed);
    g_texNewBytes.fetch_add(InitBytes(*d, init), std::memory_order_relaxed);
  }
  return g_ct2Orig(dev, d, init, out);
}

bool InstallTexCreate(ID3D11Device* dev) {
  if (g_ct2Slot) return true;
  if (!dev) return false;
  void** vt = *reinterpret_cast<void***>(dev);
  void* cur = SlotOriginal(&vt[5]);
  HMODULE owner = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(cur), &owner) ||
      owner != GetModuleHandleW(L"d3d11.dll")) {
    Log("  view profile: ID3D11Device::CreateTexture2D is not d3d11's own entry (another layer hooks it); texture "
        "creations not counted");
    return false;
  }
  g_ct2Orig = reinterpret_cast<CreateTex2DFn>(cur);
  if (!HookSlot(&vt[5], reinterpret_cast<void*>(&HookCreateTex2D), nullptr)) {
    g_ct2Orig = nullptr;
    return false;
  }
  g_ct2Slot = &vt[5];
  return true;
}

void UninstallTexCreate() {
  if (!g_ct2Slot) return;
  UnhookSlot(g_ct2Slot, reinterpret_cast<void*>(g_ct2Orig));
  g_ct2Slot = nullptr;
}

// ---------------------------------------------------------------------------
// Visualizer's frame pacer object (R2 §4): the render side records its last
// wait for Main in S+0x118 (double, ms [I]); S = *(Visualizer+0x2c1538) on the
// analysed build (PE timestamp 0x6ac12454, DCS 2.9.30) and *(Visualizer+0x2ef548)
// on DCS 2.9.31 (0x6ad87745; R147: the static the render-side wait 0xba850 is
// called with, which stores the wait at +0x118), only.
// ---------------------------------------------------------------------------

void* const* g_pacerObj = nullptr;
struct PacerBuild {
  uint32_t stamp, objRva;
};
constexpr PacerBuild kPacerBuilds[] = {{0x6ac12454u, 0x2c1538}, {0x6ad87745u, 0x2ef548}};

void InitPacer() {
  if (g_pacerObj) return;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"Visualizer.dll"));
  if (!base) return;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  for (const PacerBuild& b : kPacerBuilds)
    if (nt->FileHeader.TimeDateStamp == b.stamp) {
      g_pacerObj = reinterpret_cast<void* const*>(base + b.objRva);
      return;
    }
  Log("  view profile: Visualizer.dll is not an analysed build; pacer wait not read");
}

double ReadPacerMs() {
  if (!g_pacerObj) return -1;
  __try {
    const uint8_t* s = static_cast<const uint8_t*>(*g_pacerObj);
    if (!s) return -1;
    const double v = *reinterpret_cast<const double*>(s + 0x118);
    return (v >= 0 && v < 1000) ? v : -1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

// ---------------------------------------------------------------------------
// Recorder counters (cumulative; read as differences)
// ---------------------------------------------------------------------------

struct RecSnap {
  uint64_t shCasters = 0, shRecorded = 0, shPasses = 0, shExec = 0, shLate = 0, shVec = 0, shTex = 0, shNoJob = 0,
           shWaitNs = 0, shProbes = 0, shKeys = 0, shMeshes = 0, shPending = 0;
  uint64_t gbItems = 0, gbDraws = 0, gbPasses = 0, gbExec = 0, gbLate = 0, gbVec = 0, gbTex = 0, gbNoJob = 0,
           gbWaitNs = 0, gbProbes = 0, gbKeys = 0, gbMeshes = 0, gbTexBuilt = 0, gbPending = 0;
};

RecSnap TakeRec() {
  RecSnap r;
  const auto rl = std::memory_order_relaxed;
  if (shrec::g_state.load(rl) == 1) {
    for (int c = 0; c < shrec::kSlots; ++c) {
      const shrec::Stat& s = shrec::g_casc[c].st;
      r.shCasters += s.casters.load(rl);
      r.shRecorded += s.recorded.load(rl);
      r.shPasses += s.passes.load(rl);
      r.shExec += s.passReason[shrec::kPExecuted].load(rl);
      r.shLate += s.passReason[shrec::kPLate].load(rl);
      r.shVec += s.passReason[shrec::kPIdentity].load(rl);
      r.shTex += s.passReason[shrec::kPTexture].load(rl);
      r.shNoJob += s.passReason[shrec::kPNoJob].load(rl);
      r.shWaitNs += s.waitNs.load(rl);
      r.shPending += s.casterReason[shrec::kRKeyPending].load(rl) + s.casterReason[shrec::kRMeshPending].load(rl) +
                     s.casterReason[shrec::kRMeshChanged].load(rl);
    }
    r.shProbes = shrec::g_probes.load(rl);
    r.shKeys = shrec::g_tab.keysUsed;
    r.shMeshes = shrec::g_tab.meshesUsed;
  }
  if (gbrec::g_state.load(rl) == 1) {
    for (int k = 0; k < gbrec::kSlots; ++k) {
      const gbrec::Stat& s = gbrec::g_slot[k].st;
      r.gbItems += s.items.load(rl);
      r.gbDraws += s.draws.load(rl);
      r.gbPasses += s.passes.load(rl);
      r.gbExec += s.passReason[gbrec::kPExecuted].load(rl);
      r.gbLate += s.passReason[gbrec::kPLate].load(rl);
      r.gbVec += s.passReason[gbrec::kPIdentity].load(rl);
      r.gbTex += s.segReason[gbrec::kSTexture].load(rl);
      r.gbNoJob += s.passReason[gbrec::kPNoJob].load(rl);
      r.gbWaitNs += s.waitNs.load(rl);
      r.gbPending += s.itemReason[gbrec::kRKeyPending].load(rl) + s.itemReason[gbrec::kRMeshPending].load(rl) +
                     s.itemReason[gbrec::kRMeshChanged].load(rl) + s.itemReason[gbrec::kRTexMiss].load(rl);
    }
    r.gbProbes = gbrec::g_probes.load(rl);
    r.gbKeys = gbrec::g_tab.keysUsed;
    r.gbMeshes = gbrec::g_tab.mesh.meshesUsed;
    r.gbTexBuilt = gbrec::g_texBuilt.load(rl) + gbrec::g_texRefreshed.load(rl);
  }
  return r;
}

// ---------------------------------------------------------------------------
// Per-frame records (collect hook thread)
// ---------------------------------------------------------------------------

struct Rec {
  int64_t qpc = 0;
  float yaw = 0, frameMs = 0, rtCpuMs = 0, collectMs = 0, passMs = 0, pacerMs = -1, xrEndMs = 0;
  float shWaitMs = 0, gbWaitMs = 0;
  float shLate = 0, shVec = 0, shTex = 0, shNoJob = 0, gbLate = 0, gbVec = 0, gbTex = 0, gbNoJob = 0;
  float shCasters = 0, shRecorded = 0, shPasses = 0, shExec = 0, gbItems = 0, gbDraws = 0, gbPasses = 0, gbExec = 0;
  float shPending = 0, gbPending = 0;
  float texReq = 0, texNew = 0, texNewKB = 0;
  float periph = 0, focus = 0, shadow = 0, entering = 0;
  float learn = 0, texTable = 0;
  float gpuBusyMs = -1, gpuSpanMs = -1;  // filled after the run from the GPU frames
  // [Suite] FrameStartGap / RunnableThreads (rotation CSV), filled after the run; -1 = none
  float fsCls = -1, fsCpuMs = -1, fsGapMs = -1, fsDrain = -1, rtEntry = -1, rtJob = -1;
};

constexpr size_t kMaxRecs = 1 << 15;
std::atomic<bool> g_recording{false};
std::mutex g_mutex;
std::vector<Rec> g_recs;  // guarded by g_mutex
bool g_havePrev = false;
int64_t g_prevQpc = 0;
uint64_t g_prevCycles = 0, g_prevTex = 0, g_prevTexNew = 0, g_prevTexBytes = 0, g_prevXrTicks = 0;
double g_prevPassUs = 0, g_prevPeriphFocus = 0;
RecSnap g_prevRec;
double g_tscHz = 1;

void ResetPrev() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_havePrev = false;
}

// Called by the collect hook after every quad frame.
void OnFrame(int64_t qpcNow, uint64_t collectUs, uint64_t periph, uint64_t focus, uint64_t shadow) {
  if (!g_recording.load(std::memory_order_relaxed)) return;
  static HANDLE rt = nullptr;
  static DWORD rtId = 0;
  const DWORD want = ptiming::g_topTid.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (want && want != rtId) {
    if (rt) CloseHandle(rt);
    rt = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, want);
    rtId = want;
    g_havePrev = false;
  }
  ULONG64 cyc = 0;
  if (rt) QueryThreadCycleTime(rt, &cyc);
  double passUs;
  {
    std::lock_guard<std::mutex> pl(ptiming::g_mutex);
    passUs = ptiming::g_topLevelUs;
  }
  const uint64_t tex = texbind::g_passed, texNew = g_texNew.load(), texBytes = g_texNewBytes.load();
  const uint64_t xr = gpt::g_xrWallTicks.load();
  const RecSnap rs = TakeRec();
  const double pf = static_cast<double>(periph + focus);
  if (g_havePrev && qpcNow > g_prevQpc && g_recs.size() < kMaxRecs) {
    Rec r;
    r.qpc = qpcNow;
    r.yaw = static_cast<float>(posesweep::g_on.load() ? posesweep::g_yawDeg.load() : 0.0);
    r.frameMs = static_cast<float>((qpcNow - g_prevQpc) * g_qpcToUs / 1000.0);
    r.rtCpuMs = static_cast<float>((cyc - g_prevCycles) / g_tscHz * 1000.0);
    r.collectMs = static_cast<float>(collectUs / 1000.0);
    r.passMs = static_cast<float>((passUs >= g_prevPassUs ? passUs - g_prevPassUs : passUs) / 1000.0);  // Reset -> 0
    r.pacerMs = static_cast<float>(ReadPacerMs());
    r.xrEndMs = static_cast<float>((xr - g_prevXrTicks) * g_qpcToUs / 1000.0);
    auto d = [](uint64_t a, uint64_t b) { return static_cast<float>(a >= b ? a - b : 0); };
    r.shWaitMs = d(rs.shWaitNs, g_prevRec.shWaitNs) / 1e6f;
    r.gbWaitMs = d(rs.gbWaitNs, g_prevRec.gbWaitNs) / 1e6f;
    r.shLate = d(rs.shLate, g_prevRec.shLate);
    r.shVec = d(rs.shVec, g_prevRec.shVec);
    r.shTex = d(rs.shTex, g_prevRec.shTex);
    r.shNoJob = d(rs.shNoJob, g_prevRec.shNoJob);
    r.gbLate = d(rs.gbLate, g_prevRec.gbLate);
    r.gbVec = d(rs.gbVec, g_prevRec.gbVec);
    r.gbTex = d(rs.gbTex, g_prevRec.gbTex);
    r.gbNoJob = d(rs.gbNoJob, g_prevRec.gbNoJob);
    r.shCasters = d(rs.shCasters, g_prevRec.shCasters);
    r.shRecorded = d(rs.shRecorded, g_prevRec.shRecorded);
    r.shPasses = d(rs.shPasses, g_prevRec.shPasses);
    r.shExec = d(rs.shExec, g_prevRec.shExec);
    r.gbItems = d(rs.gbItems, g_prevRec.gbItems);
    r.gbDraws = d(rs.gbDraws, g_prevRec.gbDraws);
    r.gbPasses = d(rs.gbPasses, g_prevRec.gbPasses);
    r.gbExec = d(rs.gbExec, g_prevRec.gbExec);
    r.shPending = d(rs.shPending, g_prevRec.shPending);
    r.gbPending = d(rs.gbPending, g_prevRec.gbPending);
    r.texReq = d(tex, g_prevTex);
    r.texNew = d(texNew, g_prevTexNew);
    r.texNewKB = d(texBytes, g_prevTexBytes) / 1024.0f;
    r.periph = static_cast<float>(periph);
    r.focus = static_cast<float>(focus);
    r.shadow = static_cast<float>(shadow);
    r.entering = static_cast<float>(pf > g_prevPeriphFocus ? pf - g_prevPeriphFocus : 0.0);
    r.learn = d(rs.shProbes, g_prevRec.shProbes) + d(rs.gbProbes, g_prevRec.gbProbes) +
              d(rs.shKeys, g_prevRec.shKeys) + d(rs.gbKeys, g_prevRec.gbKeys) + d(rs.shMeshes, g_prevRec.shMeshes) +
              d(rs.gbMeshes, g_prevRec.gbMeshes);
    r.texTable = d(rs.gbTexBuilt, g_prevRec.gbTexBuilt);
    g_recs.push_back(r);
  }
  g_havePrev = true;
  g_prevQpc = qpcNow;
  g_prevCycles = cyc;
  g_prevPassUs = passUs;
  g_prevTex = tex;
  g_prevTexNew = texNew;
  g_prevTexBytes = texBytes;
  g_prevXrTicks = xr;
  g_prevRec = rs;
  g_prevPeriphFocus = pf;
}

size_t RecCount() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_recs.size();
}
std::vector<Rec> CopyRecs(size_t a, size_t b) {
  std::lock_guard<std::mutex> lock(g_mutex);
  b = std::min(b, g_recs.size());
  return a < b ? std::vector<Rec>(g_recs.begin() + a, g_recs.begin() + b) : std::vector<Rec>();
}

// ---------------------------------------------------------------------------
// Window aggregation
// ---------------------------------------------------------------------------

struct Agg {
  size_t frames = 0;
  double fps = 0, frameMs = 0, p95 = 0, rtCpuMs = 0, collectMs = 0, passMs = 0, pacerMs = -1, xrEndMs = 0;
  double recWaitMs = 0, recLate = 0, shLate = 0, gbLate = 0, shVec = 0, gbVec = 0, shTex = 0, gbTex = 0, shNoJob = 0,
         gbNoJob = 0;
  double shCov = -1, gbCov = -1, shExecPct = -1, gbExecPct = -1, shPending = 0, gbPending = 0;
  double texReq = 0, texNew = 0, texNewKB = 0, periph = 0, focus = 0, shadow = 0, entering = 0, learn = 0,
         texTable = 0;
  // GPU (light gpu_pass_timing over the same window)
  bool gpuValid = false;
  int gpuFrames = 0;
  double gpuPeriod = 0, gpuSpan = 0, gpuBusy = 0, gpuIdle = 0, gpuXr = -1;
  std::vector<std::pair<std::string, double>> gpuTop;  // kind, exclusive GPU ms per frame
  std::vector<std::pair<std::string, double>> cpuTop;  // kind, exclusive render-thread CPU ms per frame
  // [Suite] FrameStartGap / RunnableThreads over the same window
  bool fsValid = false, rtValid = false;
  fstart::Result fs;
  rthreads::Result rt;
};

Agg Aggregate(const std::vector<Rec>& v, double wallS) {
  Agg a;
  a.frames = v.size();
  if (v.empty()) return a;
  const double n = static_cast<double>(v.size());
  std::vector<float> ft;
  ft.reserve(v.size());
  double sumFt = 0, pacerSum = 0;
  size_t pacerN = 0;
  double shC = 0, shR = 0, gbI = 0, gbD = 0, shP = 0, shE = 0, gbP = 0, gbE = 0;
  for (const Rec& r : v) {
    ft.push_back(r.frameMs);
    sumFt += r.frameMs;
    a.rtCpuMs += r.rtCpuMs;
    a.collectMs += r.collectMs;
    a.passMs += r.passMs;
    if (r.pacerMs >= 0) {
      pacerSum += r.pacerMs;
      ++pacerN;
    }
    a.xrEndMs += r.xrEndMs;
    a.recWaitMs += r.shWaitMs + r.gbWaitMs;
    a.shLate += r.shLate;
    a.gbLate += r.gbLate;
    a.shVec += r.shVec;
    a.gbVec += r.gbVec;
    a.shTex += r.shTex;
    a.gbTex += r.gbTex;
    a.shNoJob += r.shNoJob;
    a.gbNoJob += r.gbNoJob;
    a.shPending += r.shPending;
    a.gbPending += r.gbPending;
    shC += r.shCasters;
    shR += r.shRecorded;
    gbI += r.gbItems;
    gbD += r.gbDraws;
    shP += r.shPasses;
    shE += r.shExec;
    gbP += r.gbPasses;
    gbE += r.gbExec;
    a.texReq += r.texReq;
    a.texNew += r.texNew;
    a.texNewKB += r.texNewKB;
    a.periph += r.periph;
    a.focus += r.focus;
    a.shadow += r.shadow;
    a.entering += r.entering;
    a.learn += r.learn;
    a.texTable += r.texTable;
  }
  a.frameMs = sumFt / n;
  a.fps = wallS > 0 ? n / wallS : (a.frameMs > 0 ? 1000.0 / a.frameMs : 0);
  a.p95 = Pct(ft, 0.95);
  for (double* x : {&a.rtCpuMs, &a.collectMs, &a.passMs, &a.xrEndMs, &a.recWaitMs, &a.shLate, &a.gbLate, &a.shVec,
                    &a.gbVec, &a.shTex, &a.gbTex, &a.shNoJob, &a.gbNoJob, &a.shPending, &a.gbPending, &a.texReq,
                    &a.texNew, &a.texNewKB, &a.periph, &a.focus, &a.shadow, &a.entering, &a.learn, &a.texTable})
    *x /= n;
  a.pacerMs = pacerN ? pacerSum / pacerN : -1;
  a.recLate = a.shLate + a.gbLate;
  if (shC > 0) a.shCov = 100.0 * shR / shC;
  if (gbI > 0) a.gbCov = 100.0 * gbD / gbI;
  if (shP > 0) a.shExecPct = 100.0 * shE / shP;
  if (gbP > 0) a.gbExecPct = 100.0 * gbE / gbP;
  return a;
}

// GPU frames opened in [qa, qb] (QPC) into the aggregate.
void AddGpu(Agg& a, std::vector<gpt::Done>& done, int64_t qa, int64_t qb) {
  std::vector<gpt::Done> in;
  for (gpt::Done& d : done)
    if (d.qpc >= qa && d.qpc <= qb) in.push_back(std::move(d));
  if (in.size() < 3) return;
  gpt::Result r = gpt::Analyze(in, [](void* p) -> std::string { return ptiming::RttiName(p); });
  if (!r.frames || r.span.empty()) return;
  a.gpuValid = true;
  a.gpuFrames = r.frames;
  a.gpuPeriod = gpt::Mean(r.period);
  a.gpuSpan = gpt::Mean(r.span);
  const double gaps = gpt::Mean(r.gaps);
  a.gpuBusy = a.gpuSpan - gaps;
  a.gpuIdle = gaps + (r.afterXr.empty() ? std::max(0.0, a.gpuPeriod - a.gpuSpan) : gpt::Mean(r.afterXr));
  a.gpuXr = r.xr.empty() ? -1 : gpt::Mean(r.xr);
  for (auto& kv : r.kinds) a.gpuTop.push_back({kv.first, gpt::Mean(kv.second.perFrame)});
  std::sort(a.gpuTop.begin(), a.gpuTop.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
  if (a.gpuTop.size() > 3) a.gpuTop.resize(3);
}

void AddCpu(Agg& a, const std::map<std::string, ptiming::Stat>& cpu) {
  if (!a.frames) return;
  const double f = static_cast<double>(a.frames);
  for (auto& kv : cpu) a.cpuTop.push_back({kv.first, kv.second.exclUs / 1000.0 / f});
  std::sort(a.cpuTop.begin(), a.cpuTop.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
  if (a.cpuTop.size() > 3) a.cpuTop.resize(3);
}

Bound BoundOf(const Agg& a) {
  BoundIn in;
  in.frameMs = a.frameMs;
  in.xrEndMs = a.xrEndMs;
  in.pacerMs = a.pacerMs > 0 ? a.pacerMs : 0;
  in.recWaitMs = a.recWaitMs;
  in.recLate = a.recLate;
  in.gpuValid = a.gpuValid;
  in.gpuIdleMs = a.gpuIdle;
  return Classify(in);
}

std::string TopList(const std::vector<std::pair<std::string, double>>& v) {
  std::string s;
  char buf[96];
  for (auto& kv : v) {
    snprintf(buf, sizeof(buf), "%s%.24s %.2f", s.empty() ? "" : ", ", kv.first.c_str(), kv.second);
    s += buf;
  }
  return s.empty() ? "-" : s;
}

std::string Pct1(double v) {
  if (v < 0) return "-";
  char b[16];
  snprintf(b, sizeof(b), "%.0f%%", v);
  return b;
}

// The compact cost split of one window (two log lines).
void LogAgg(const char* label, const Agg& a) {
  char gpu[256] = "n/a", pacer[16] = "n/a";
  if (a.gpuValid)
    snprintf(gpu, sizeof(gpu), "busy %.2f of period %.2f, idle %.2f (top %s)", a.gpuBusy, a.gpuPeriod, a.gpuIdle,
             TopList(a.gpuTop).c_str());
  if (a.pacerMs >= 0) snprintf(pacer, sizeof(pacer), "%.2f", a.pacerMs);
  Log("  %s: %.1f fps, frame %.2f ms p95 %.2f | render thread %.2f ms/frame (passes %.2f; top %s) | culling %.2f | "
      "gpu %s | xrEnd %.2f, pacer %s, recorder wait %.3f | %s",
      label, a.fps, a.frameMs, a.p95, a.rtCpuMs, a.passMs, TopList(a.cpuTop).c_str(), a.collectMs, gpu, a.xrEndMs,
      pacer, a.recWaitMs, kBoundName[BoundOf(a)]);
  Log("  %s: renderables periph %.0f focus %.0f shadow %.0f | shadow rec: CL %s of casters, passes executed %s, late "
      "%.2f/frame | gbuffer rec: CL %s of items, executions %s, late %.2f/frame",
      label, a.periph, a.focus, a.shadow, Pct1(a.shCov).c_str(), Pct1(a.shExecPct).c_str(), a.shLate,
      Pct1(a.gbCov).c_str(), Pct1(a.gbExecPct).c_str(), a.gbLate);
}
// ---------------------------------------------------------------------------
// Session (suite thread)
// ---------------------------------------------------------------------------

bool g_gpuOn = false;
// Set by the suite: run the frame-start / runnable-threads counters inside the profiles.
bool g_withFs = false, g_withRt = false;
int g_rtEvery = 16;
bool g_fsOn = false, g_rtOn = false;

void SessionBegin(std::atomic<uint64_t>& frameCounter, double tscHz) {
  g_tscHz = tscHz > 0 ? tscHz : 1;
  InitPacer();
  InstallTexCreate(shadowinst::g_device);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_recs.clear();
    g_recs.reserve(8192);
    g_havePrev = false;
  }
  ptiming::Reset();
  ptiming::g_recording = true;
  g_gpuOn = gpt::Begin(frameCounter, true);
  if (!g_gpuOn) Log("  view profile: GPU timestamps not available; GPU columns n/a");
  if (!gpt::g_xrSlot)
    Log("  view profile: xrEndFrame not timed (xrEnd column 0; GPU-bound then only from the GPU idle time)");
  if (!g_pacerObj) Log("  view profile: pacer wait not read (pacer column n/a)");
  if (!g_ct2Slot) Log("  view profile: texture creations not counted");
  g_fsOn = g_withFs && g_gpuOn && fstart::Begin(frameCounter);
  g_rtOn = g_withRt && rthreads::Begin(g_rtEvery, g_tscHz);
  g_recording = true;
  Sleep(300);  // first records
}

void SessionEnd() {
  g_recording = false;
  ptiming::g_recording = false;
  if (g_fsOn) Log("  view profile: frame-start probes %.4f ms/frame on the render thread", fstart::OverheadMsPerFrame());
  if (g_rtOn)
    Log("  view profile: runnable-threads counters and snapshots %.4f ms/frame at render entry (averaged)",
        rthreads::g_frame ? rthreads::g_entryTicks.load() * g_qpcToUs / 1000.0 / rthreads::g_frame : 0.0);
  fstart::End();
  rthreads::End();
  g_fsOn = g_rtOn = false;
  if (g_gpuOn)
    Log("  view profile: GPU frames %llu opened, %llu read (truncated %llu, disjoint %llu, dropped unread %llu); "
        "measurement cost on the render thread: GPU timestamps %.3f ms/frame (pass CPU timer not included)",
        static_cast<unsigned long long>(gpt::g_s.framesOpened), static_cast<unsigned long long>(gpt::g_s.framesRead),
        static_cast<unsigned long long>(gpt::g_s.truncated), static_cast<unsigned long long>(gpt::g_s.disjoint),
        static_cast<unsigned long long>(gpt::g_s.dropped), gpt::OverheadMsPerFrame());
  gpt::End();
  g_gpuOn = false;
  UninstallTexCreate();
}

int64_t Qpc() {
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  return q.QuadPart;
}

// One measured window: records, CPU per pass kind and GPU frames between now
// and ms later. The GPU frames are taken after a short wait (read back a few
// frames late) and filtered by their open time.
Agg MeasureWindow(int ms, std::atomic<bool>& abort) {
  ptiming::Reset();  // pass CPU per kind for this window only
  rthreads::Window rw;
  fstart::Window fw;
  if (g_rtOn) rw = rthreads::Open();
  if (g_fsOn) fw = fstart::Open();
  const size_t a = RecCount();
  const int64_t qa = Qpc();
  for (int t = 0; t < ms && !abort; t += 50) Sleep(50);
  const int64_t qb = Qpc();
  const size_t b = RecCount();
  std::map<std::string, ptiming::Stat> cpu = ptiming::TakeByName(nullptr, nullptr);
  rthreads::Result rr;
  if (g_rtOn) rr = rthreads::Close(rw);
  Sleep(150);
  std::vector<gpt::Done> done = gpt::TakeDone();
  Agg g = Aggregate(CopyRecs(a, b), (qb - qa) * g_qpcToUs / 1e6);
  AddCpu(g, cpu);
  if (g_fsOn) {
    g.fs = fstart::Close(fw, done, qb);
    g.fsValid = g.fs.sum.valid;
  }
  if (g_rtOn) {
    g.rt = std::move(rr);
    g.rtValid = g.rt.sum.valid;
  }
  AddGpu(g, done, qa, qb);
  return g;
}

// One-line extras of a yaw row.
std::string CounterCols(const Agg& a) {
  std::string s;
  char b[192];
  if (a.fsValid) {
    const fstart::Summary& f = a.fs.sum;
    const double n = std::max(1, f.frames);
    snprintf(b, sizeof(b), ", turns %.0f%% cpu-late %.0f%% fence %.0f%% runtime %.0f%%, cpu seg %.2f gap %.2f",
             100.0 * f.cls[fstart::kTurns] / n, 100.0 * f.cls[fstart::kCpuLate] / n, 100.0 * f.cls[fstart::kFence] / n,
             100.0 * f.cls[fstart::kRuntime] / n, f.seg[fstart::sCpu].mean, f.seg[fstart::sGap].mean);
    s += b;
  }
  if (a.rtValid) {
    snprintf(b, sizeof(b), ", waiting thr %.2f (>=2 %s, heavy %s) gate %s", a.rt.sum.entry.runnable,
             rthreads::Share(a.rt.sum.entry.share2).c_str(), rthreads::Share(a.rt.sum.heavyShare2).c_str(),
             a.rt.sum.gate ? "PASS" : "FAIL");
    s += b;
  }
  return s;
}

// Whole-frame parts still -> turning, and the one that grows most.
void LogGrowth(const Agg& a, const Agg& b) {
  struct P {
    const char* name;
    double x, y;
  };
  std::vector<P> v = {{"render thread CPU", a.rtCpuMs, b.rtCpuMs}, {"culling", a.collectMs, b.collectMs},
                      {"pass CPU", a.passMs, b.passMs},          {"xrEndFrame", a.xrEndMs, b.xrEndMs},
                      {"recorder wait", a.recWaitMs, b.recWaitMs}};
  if (a.gpuValid && b.gpuValid) {
    v.push_back({"GPU busy", a.gpuBusy, b.gpuBusy});
    v.push_back({"GPU idle", a.gpuIdle, b.gpuIdle});
  }
  std::string line;
  char buf[96];
  const P* most = nullptr;
  for (const P& p : v) {
    snprintf(buf, sizeof(buf), "%s%s %.2f -> %.2f", line.empty() ? "" : ", ", p.name, p.x, p.y);
    line += buf;
    if (!most || p.y - p.x > most->y - most->x) most = &p;
  }
  Log("  rotation frame parts still -> turning (ms/frame): frame %.2f -> %.2f; %s; grows most: %s (%+.2f ms); late "
      "recorder jobs %.2f -> %.2f /frame",
      a.frameMs, b.frameMs, line.c_str(), most ? most->name : "-", most ? most->y - most->x : 0.0, a.recLate,
      b.recLate);
}

// ---------------------------------------------------------------------------
// [Suite] YawProfile
// ---------------------------------------------------------------------------

void RunYawProfile(int stepDeg, std::atomic<uint64_t>& frameCounter, std::atomic<bool>& abort, double tscHz) {
  stepDeg = std::max(5, std::min(180, stepDeg));
  const bool wasHolding = posesweep::Holding();
  const double wasHold = posesweep::g_hold.load();
  if (!posesweep::Install()) {
    Log("  yaw profile: the view cannot be held (Quad-Views-Foveated hook unavailable)");
    return;
  }
  SessionBegin(frameCounter, tscHz);
  struct Row {
    int yaw;
    Agg a;
  };
  std::vector<Row> rows;
  for (int deg = 0; deg < 360 && !abort; deg += stepDeg) {
    posesweep::Hold(true, deg);
    for (int t = 0; t < 2000 && !abort; t += 50) Sleep(50);
    gpt::TakeDone();  // settle frames out
    const shrec::StockSnap sh0 = shrec::TakeStock();
    const gbrec::StockSnap gb0 = gbrec::TakeStock();
    const uint64_t f0 = frameCounter.load();
    Agg a = MeasureWindow(2500, abort);
    if (abort) break;
    const double frames = static_cast<double>(frameCounter.load() - f0);
    char label[32];
    snprintf(label, sizeof(label), "yaw %3d", deg);
    LogAgg(label, a);
    if (a.fsValid) fstart::LogResult(label, a.fs, false);
    if (a.rtValid) rthreads::LogResult(label, a.rt, false);
    // What each recorder drew stock, per frame (MeasureWindow's tail wait included).
    if (shrec::g_state.load() == 1)
      Log("  %s: shadow rec drawn stock: %s", label, shrec::StockText(sh0, shrec::TakeStock(), frames).c_str());
    if (gbrec::g_state.load() == 1)
      Log("  %s: gbuffer rec drawn stock: %s", label, gbrec::StockText(gb0, gbrec::TakeStock(), frames).c_str());
    rows.push_back({deg, std::move(a)});
  }
  SessionEnd();
  if (wasHolding)
    posesweep::Hold(true, wasHold);
  else
    posesweep::Hold(false, 0);
  if (rows.empty()) return;
  const Row* worst = &rows[0];
  for (const Row& r : rows)
    if (r.a.fps < worst->a.fps) worst = &r;
  Log("  yaw profile summary: heaviest yaw %d deg (%.1f fps, frame %.2f ms, p95 %.2f): %s", worst->yaw,
      worst->a.fps, worst->a.frameMs, worst->a.p95, kBoundName[BoundOf(worst->a)]);
  int count[kBounds] = {};
  for (const Row& r : rows) {
    const Bound b = BoundOf(r.a);
    ++count[b];
    char idle[32] = "n/a";
    if (r.a.gpuValid) snprintf(idle, sizeof(idle), "%.2f ms", r.a.gpuIdle);
    Log("    yaw %3d: %5.1f fps  %-28s render thread %.2f, xrEnd %.2f, gpu idle %s, recorder late %.2f/frame%s", r.yaw,
        r.a.fps, kBoundName[b], r.a.rtCpuMs, r.a.xrEndMs, idle, r.a.recLate, CounterCols(r.a).c_str());
  }
  Log("  yaw profile: %d CPU-bound, %d worker-bound, %d GPU-bound, %d pacing-bound directions (restored %s)",
      count[kBCpu], count[kBWorker], count[kBGpu], count[kBPacing],
      wasHolding ? "the previous held yaw" : "the unheld view");
}

// ---------------------------------------------------------------------------
// [Suite] RotationProfile
// ---------------------------------------------------------------------------

void WriteCsv(const std::vector<Rec>& v, int64_t q0) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t name[64];
  swprintf_s(name, L"rotation_%04d%02d%02d_%02d%02d%02d.csv", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);
  FILE* f = nullptr;
  _wfopen_s(&f, (g_dir + name).c_str(), L"w");
  if (!f) return;
  fprintf(f, "t_s,yaw,frame_ms,rt_cpu_ms,culling_ms,passes_ms,pacer_ms,xrend_ms,gpu_busy_ms,rec_wait_ms,sh_late,"
             "sh_vec,sh_tex,gb_late,gb_vec,gb_tex,tex_req,tex_new,tex_new_kb,periph,focus,shadow,entering,learn,"
             "tex_table,fs_class,fs_cpu_seg_ms,fs_gpu_gap_ms,fs_drain,rt_waiting_entry,rt_waiting_job\n");
  for (const Rec& r : v)
    fprintf(f, "%.3f,%.1f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.1f,%.0f,"
               "%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.3f,%.3f,%.0f,%.0f,%.0f\n",
            (r.qpc - q0) * g_qpcToUs / 1e6, r.yaw, r.frameMs, r.rtCpuMs, r.collectMs, r.passMs, r.pacerMs, r.xrEndMs,
            r.gpuBusyMs, r.shWaitMs + r.gbWaitMs, r.shLate, r.shVec, r.shTex, r.gbLate, r.gbVec, r.gbTex, r.texReq,
            r.texNew, r.texNewKB, r.periph, r.focus, r.shadow, r.entering, r.learn, r.texTable, r.fsCls, r.fsCpuMs,
            r.fsGapMs, r.fsDrain, r.rtEntry, r.rtJob);
  fclose(f);
  Log("  rotation profile: per-frame CSV %ls", name);
}

void RunRotationProfile(double rateDps, int seconds, std::atomic<uint64_t>& frameCounter, std::atomic<bool>& abort,
                        double tscHz) {
  rateDps = std::max(5.0, std::min(720.0, rateDps));
  seconds = std::max(5, std::min(120, seconds));
  if (!posesweep::Install()) {
    Log("  rotation profile: the view cannot be rotated (Quad-Views-Foveated hook unavailable)");
    return;
  }
  SessionBegin(frameCounter, tscHz);
  // Still baseline (the current view), then the turn.
  Agg still = MeasureWindow(3000, abort);
  ptiming::Reset();
  gpt::TakeDone();
  rthreads::Window rw;
  fstart::Window fw;
  if (g_rtOn) rw = rthreads::Open();
  if (g_fsOn) fw = fstart::Open();
  const size_t a = RecCount();
  const int64_t qa = Qpc();
  posesweep::Start(rateDps);
  for (int t = 0; t < seconds * 1000 && !abort; t += 50) Sleep(50);
  const int64_t qb = Qpc();
  const size_t b = RecCount();
  posesweep::Stop();  // a hold stays as it was
  std::map<std::string, ptiming::Stat> cpu = ptiming::TakeByName(nullptr, nullptr);
  rthreads::Result rotRt;
  if (g_rtOn) rotRt = rthreads::Close(rw);
  Sleep(150);
  std::vector<gpt::Done> done = gpt::TakeDone();
  fstart::Result rotFs;
  if (g_fsOn) rotFs = fstart::Close(fw, done, qb);
  const bool fsOn = g_fsOn, rtOn = g_rtOn;
  SessionEnd();
  std::vector<Rec> v = CopyRecs(a, b);
  // GPU busy per CPU frame: the GPU frame opened between the previous collect and this one.
  {
    std::vector<std::pair<int64_t, double>> gf;
    for (const gpt::Done& d : done) {
      double span = 0, busy = 0;
      if (gpt::FrameGpu(d, &span, &busy, nullptr)) gf.push_back({d.qpc, busy});
    }
    std::sort(gf.begin(), gf.end());
    size_t k = 0;
    int64_t prev = qa;
    for (Rec& r : v) {
      while (k < gf.size() && gf[k].first < prev) ++k;
      if (k < gf.size() && gf[k].first <= r.qpc) r.gpuBusyMs = static_cast<float>(gf[k].second);
      prev = r.qpc;
    }
  }
  // Frame-start class and runnable threads per CPU frame (the record's qpc is
  // its collect end = render entry; the first pass follows before the next).
  if (fsOn || rtOn) {
    size_t kf = 0, ks = 0;
    for (size_t i = 0; i < v.size(); ++i) {
      Rec& r = v[i];
      const int64_t next = i + 1 < v.size() ? v[i + 1].qpc : INT64_MAX;
      if (fsOn) {
        const auto& fo = rotFs.frames;
        while (kf < fo.size() && fo[kf].p0 <= r.qpc) ++kf;
        if (kf < fo.size() && fo[kf].p0 < next && !fo[kf].perturbed) {
          r.fsCls = static_cast<float>(fo[kf].cls);
          r.fsCpuMs = fo[kf].seg[fstart::sCpu];
          r.fsGapMs = std::isnan(fo[kf].seg[fstart::sGap]) ? -1.0f : fo[kf].seg[fstart::sGap];
          r.fsDrain = static_cast<float>(fo[kf].drain);
        }
      }
      if (rtOn) {
        const auto& sm = rotRt.samples;
        while (ks < sm.size() && sm[ks].qpc < r.qpc) ++ks;
        for (size_t k = ks; k < sm.size() && sm[k].qpc < next; ++k)
          (sm[k].point == rthreads::kEntry ? r.rtEntry : r.rtJob) = static_cast<float>(sm[k].runnable);
      }
    }
  }
  Agg rot = Aggregate(v, (qb - qa) * g_qpcToUs / 1e6);
  AddCpu(rot, cpu);
  if (fsOn) {
    rot.fs = std::move(rotFs);
    rot.fsValid = rot.fs.sum.valid;
  }
  if (rtOn) {
    rot.rt = std::move(rotRt);
    rot.rtValid = rot.rt.sum.valid;
  }
  AddGpu(rot, done, qa, qb);
  Log("  rotation profile: %.0f deg/s for %.1f s (%zu frames), still baseline 3 s (%zu frames)", rateDps,
      (qb - qa) * g_qpcToUs / 1e6, rot.frames, still.frames);
  LogAgg("still   ", still);
  LogAgg("rotating", rot);
  LogGrowth(still, rot);
  if (still.fsValid) fstart::LogResult("still", still.fs, true);
  if (rot.fsValid) fstart::LogResult("turning", rot.fs, true);
  if (still.fsValid && rot.fsValid) fstart::LogCompare(still.fs, rot.fs);
  if (still.rtValid) rthreads::LogResult("still", still.rt, true);
  if (rot.rtValid) rthreads::LogResult("turning", rot.rt, true);
  if (still.rtValid && rot.rtValid) rthreads::LogCompare(still.rt, rot.rt);
  Log("  rotation transients per frame (still -> rotating): texture streaming requests %.0f -> %.0f, texture "
      "creations %.2f -> %.2f (%.0f -> %.0f KB), renderables added %.0f -> %.0f, recorder learning (probes, keys, "
      "meshes) %.2f -> %.2f, G-buffer texture table builds %.1f -> %.1f, items pending a probe (shadow + gbuffer) "
      "%.0f -> %.0f",
      still.texReq, rot.texReq, still.texNew, rot.texNew, still.texNewKB, rot.texNewKB, still.entering,
      rot.entering, still.learn, rot.learn, still.texTable, rot.texTable, still.shPending + still.gbPending,
      rot.shPending + rot.gbPending);
  Log("  rotation job fallbacks per frame (still -> rotating): shadow: job still running %.2f -> %.2f, vector "
      "changed %.2f -> %.2f, texture view changed or swap due %.2f -> %.2f, no job %.2f -> %.2f; gbuffer: job "
      "still running %.2f -> %.2f, vector changed %.2f -> %.2f, segment texture view changed or swap due %.2f -> "
      "%.2f, no job %.2f -> %.2f",
      still.shLate, rot.shLate, still.shVec, rot.shVec, still.shTex, rot.shTex, still.shNoJob, rot.shNoJob,
      still.gbLate, rot.gbLate, still.gbVec, rot.gbVec, still.gbTex, rot.gbTex, still.gbNoJob, rot.gbNoJob);
  // Per 45-degree sector while turning.
  {
    double sum[8] = {}, n[8] = {};
    for (const Rec& r : v) {
      double y = std::fmod(r.yaw, 360.0);
      if (y < 0) y += 360.0;
      const int s = std::min(7, static_cast<int>(y / 45.0));
      sum[s] += r.frameMs;
      n[s] += 1;
    }
    std::string line;
    char buf[48];
    for (int s = 0; s < 8; ++s) {
      if (!n[s]) continue;
      snprintf(buf, sizeof(buf), " %d-%d: %.2f", s * 45, s * 45 + 45, sum[s] / n[s]);
      line += buf;
    }
    Log("  rotation frame ms by applied yaw sector (deg, hold base + turn):%s", line.c_str());
  }
  // Spikes.
  std::vector<float> ft;
  for (const Rec& r : v) ft.push_back(r.frameMs);
  double median = 0;
  const std::vector<size_t> sp = Spikes(ft, 1.5, &median);
  std::vector<Series> ser = {{"render thread CPU", true, {}}, {"culling", true, {}},     {"pass CPU", true, {}},
                             {"xrEndFrame", true, {}},        {"pacer wait", true, {}},  {"recorder wait", true, {}},
                             {"GPU busy", true, {}},          {"recorder fallbacks", false, {}},
                             {"texture requests", false, {}}, {"texture creations", false, {}},
                             {"renderables added", false, {}}, {"recorder learning", false, {}},
                             {"texture table builds", false, {}}};
  for (const Rec& r : v) {
    ser[0].v.push_back(r.rtCpuMs);
    ser[1].v.push_back(r.collectMs);
    ser[2].v.push_back(r.passMs);
    ser[3].v.push_back(r.xrEndMs);
    ser[4].v.push_back(r.pacerMs);
    ser[5].v.push_back(r.shWaitMs + r.gbWaitMs);
    ser[6].v.push_back(r.gpuBusyMs);
    ser[7].v.push_back(r.shLate + r.shVec + r.shTex + r.shNoJob + r.gbLate + r.gbVec + r.gbTex + r.gbNoJob);
    ser[8].v.push_back(r.texReq);
    ser[9].v.push_back(r.texNew);
    ser[10].v.push_back(r.entering);
    ser[11].v.push_back(r.learn);
    ser[12].v.push_back(r.texTable);
  }
  const std::vector<Elev> el = Elevated(ser, sp);
  std::string line;
  char buf[128];
  for (const Elev& e : el) {
    snprintf(buf, sizeof(buf), "%s%s %.2f -> %.2f%s", line.empty() ? "" : "; ", e.name, e.median, e.spikeMean,
             e.ms ? " ms" : "");
    line += buf;
  }
  double spikeMs = 0;
  for (size_t i : sp) spikeMs += ft[i];
  Log("  rotation spikes: %zu frames > 1.5 x median %.2f ms (%.1f%% of frames, mean %.2f ms); elevated in them "
      "(median of all frames -> spike mean): %s",
      sp.size(), median, v.empty() ? 0.0 : 100.0 * sp.size() / v.size(), sp.empty() ? 0.0 : spikeMs / sp.size(),
      line.empty() ? "none above the thresholds" : line.c_str());
  WriteCsv(v, qa);
}

}  // namespace vprof
