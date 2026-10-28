// Frame-start GPU bubble attribution ([Suite] FrameStartGap, default 0; R22
// E1). Measure only: nothing is flushed, waited for or reordered, and with the
// key at 0 none of this is installed.
//
// CPU probes (QPC, the render thread = the thread calling DCS's xrEndFrame
// wrapper), per frame, from the end of frame N to the first pass of N+1:
//  - xrEndFrame wrapper entry and return (gpu_pass_timing.h's vt[10] hook);
//  - xrWaitFrame, xrBeginFrame, xrAcquireSwapchainImage and
//    xrWaitSwapchainImage: slots 33, 34, 27, 28 of the OpenXR loader's
//    dispatch table that Visualizer's trampolines call through
//    ([V Visualizer.dll 0x6ac12454: active instance static at 0x2c8068
//    (getter 0x1c75c0), table at instance+0x48; trampolines 0x1b7800
//    (+0x108), 0x1b6450 (+0x110), 0x1b62d0 (+0xd8), 0x1b7880 (+0xe0); the
//    xrBeginFrame wrapper vt[9] 0x1244c0 calls xrBeginFrame, then per
//    swapchain 0x1a2ff0 = acquire + wait]; R147: DCS 2.9.31's Visualizer
//    0x6ad87745 has the same code, .text +0x2c790, the static +0x2e010:
//    kVisBuilds). xrWaitFrame's thread is recorded (R15: Main);
//  - culling start/end (collectRenderablesAggregated, the timeline's IAT
//    hook on the render thread: ctiming::g_aggObserver) and the first top-level pass
//    (gpu_pass_timing.h's boundary callback; the same point that opens a GPU
//    frame there, whose serial joins the two).
// Inside xrEndFrame on the render thread: IDXGIDevice2::EnqueueSetEvent
// (slot 16 of d3d11's static IDXGIDevice vtable, only hooked when both table
// and entry are d3d11.dll's) and the Pimax client's kernel waits
// (libPVRClient64.dll IAT: WaitForSingleObjectEx, Sleep). Flush is not timed:
// d3d11 rewrites the context table inside xrEndFrame (QVFR's
// SwapDeviceContextState), so a table hook would miss it; it is in "rest".
//
// GPU drained? At the xrEndFrame wrapper entry one D3D11_QUERY_EVENT is ended
// on DCS's immediate context (after all of frame N's passes, before QVFR's
// composition); it is polled with DONOTFLUSH at xrEndFrame return, the first
// xrBeginFrame, culling start, culling end and the first pass. The first
// poll that finds it done is the frame's drain stage. (If nothing submits the
// context inside xrEndFrame the event is only seen at a later stage; the GPU
// rows tell the two apart.)
//
// GPU side: gpu_pass_timing.h in light mode (one timestamp per pass
// boundary, plus one before and after xrEndFrame): the frame-start gap is
// the GPU time from the xrEndFrame-return stamp of frame N to the first pass
// of N+1.
//
// Classes per frame (Classify, offline tested), in order:
//  no gap        GPU gap < 0.3 ms;
//  take turns    drained at xrEndFrame return and gap <= CPU segment + tol
//                (tol = max(0.3 ms, 20% of it)): the GPU idles while the
//                render thread runs xrEndFrame return -> first pass (R22 A);
//  CPU late      not drained at return but gap <= CPU segment + tol: the
//                CPU segment still covers the gap;
//  fence/driver  render-thread time in EnqueueSetEvent + the client's waits
//                >= 0.5 ms and >= half of the xrEndFrame wrapper;
//  runtime/compositor  otherwise: the GPU starts the first pass later than
//                the CPU segment explains (compositor GPU work or a
//                cross-process fence, R22 B/C);
//  unknown       no GPU frame pair.
// Frames whose render entry took a [Suite] RunnableThreads snapshot are left
// out (its cost lands in the CPU segment).
//
// Included once from main.cpp after gpu_pass_timing.h and run_threads.h.
#pragma once

namespace fstart {

// ---------------------------------------------------------------------------
// Pure helpers (offline tested)
// ---------------------------------------------------------------------------

enum Cls : int { kNoGap = 0, kTurns, kCpuLate, kFence, kRuntime, kUnknown, kCls };
const char* const kClsName[kCls] = {"no gap", "take turns", "CPU late", "fence/driver wait", "runtime/compositor wait",
                                    "unknown"};

struct FrameIn {
  bool gpuValid = false;
  double cpuSegMs = 0;  // xrEndFrame return -> first pass submit (CPU)
  double gpuGapMs = 0;  // xrEndFrame stamp -> first pass start (GPU)
  double xrWallMs = 0;  // xrEndFrame wrapper wall time (CPU)
  double drvWaitMs = 0; // EnqueueSetEvent + client waits inside it (CPU)
  int drain = -1;       // 0 = done at xrEndFrame return
};

inline Cls Classify(const FrameIn& f) {
  if (!f.gpuValid) return kUnknown;
  if (f.gpuGapMs < 0.3) return kNoGap;
  const double tol = std::max(0.3, 0.2 * f.cpuSegMs);
  if (f.gpuGapMs <= f.cpuSegMs + tol) return f.drain == 0 ? kTurns : kCpuLate;
  if (f.drvWaitMs >= 0.5 && f.drvWaitMs >= 0.5 * f.xrWallMs) return kFence;
  return kRuntime;
}

// GPU edges of one gpu_pass_timing frame (ticks; -1 = absent).
struct Edge {
  uint64_t serial = 0, freq = 0;
  int64_t first = -1, lastEnd = -1, xrB = -1, xrE = -1;
};
inline bool EdgeOf(const gpt::Done& d, Edge* e) {
  if (!d.freq || d.ticks.size() != d.ev.size()) return false;
  Edge x;
  x.serial = d.serial;
  x.freq = d.freq;
  int depth = 0;
  for (size_t i = 0; i < d.ev.size(); ++i) {
    const int64_t t = static_cast<int64_t>(d.ticks[i]);
    switch (d.ev[i].type) {
      case gpt::kBegin:
        if (depth == 0 && x.first < 0) x.first = t;
        ++depth;
        break;
      case gpt::kEnd:
        if (depth > 0 && --depth == 0) x.lastEnd = t;
        break;
      case gpt::kXrBegin:
        x.xrB = t;
        break;
      case gpt::kXrEnd:
        x.xrE = t;
        break;
    }
  }
  if (x.first < 0) return false;
  *e = x;
  return true;
}

// One frame's CPU record: QPC values, 0 = not seen.
struct Rec {
  int64_t xe0 = 0, xe1 = 0;           // xrEndFrame wrapper of the previous frame
  int64_t bx0 = 0, bx1 = 0, bfEnd = 0;  // xrBeginFrame call; last acquire/wait return
  int64_t acqTicks = 0, waitTicks = 0;
  int acqN = 0, waitN = 0;
  int64_t c0 = 0, c1 = 0;             // first culling start, last culling end
  int collectN = 0;
  int64_t p0 = 0;                     // first top-level pass
  int64_t enqTicks = 0, wsoTicks = 0, sleepTicks = 0;  // inside xrEndFrame, render thread
  int enqN = 0, wsoN = 0, sleepN = 0;
  int64_t wf0 = 0, wf1 = 0;           // the last xrWaitFrame call completed by the first pass
  DWORD wfTid = 0;
  bool wfInFlight = false;            // an xrWaitFrame call was running at xrBeginFrame
  int drain = -1;                     // 0..4 poll stage, 5 = not done by the first pass, -1 = no event
  uint64_t serial = ~0ull;            // gpu_pass_timing frame opened at p0
  bool perturbed = false;             // a RunnableThreads snapshot ran at this frame's render entry
};

enum Seg : int {
  sCpu = 0, sToBegin, sBegin, sAcq, sWait, sToCollect, sCollect, sToPass,
  sGap, sExcess, sGpuXr, sLastToXr,
  sXrWall, sEnq, sWso, sSleep, sXrRest,
  sWf, sWfVsXe, sBeginVsWf, kSegs
};
const char* const kSegName[kSegs] = {
    "cpu: xrEndFrame return -> first pass", "  -> xrBeginFrame", "  xrBeginFrame call", "  swapchain acquire",
    "  swapchain wait", "  -> culling start", "  culling", "  culling end -> first pass",
    "gpu: frame-start gap (xrEndFrame -> first pass)", "gpu: gap - cpu segment", "gpu: xrEndFrame wrapper",
    "gpu: last pass end -> xrEndFrame", "xrEndFrame wrapper (cpu)", "  EnqueueSetEvent", "  WaitForSingleObjectEx (client)",
    "  Sleep (client)", "  rest (QVFR, RPC, Flush)", "xrWaitFrame call", "xrWaitFrame return - xrEndFrame return",
    "xrBeginFrame - xrWaitFrame return"};

struct FrameOut {
  int64_t p0 = 0;
  Cls cls = kUnknown;
  int drain = -1;
  bool perturbed = false;
  bool wfOnRender = false;
  float seg[kSegs];
};

// Per frame: segments (ms, NaN = not measured) and class. rt = render thread.
inline std::vector<FrameOut> Derive(const std::vector<Rec>& recs, const std::vector<Edge>& edges, double qpcToMs,
                                    DWORD rt) {
  std::unordered_map<uint64_t, const Edge*> bySerial;
  for (const Edge& e : edges) bySerial[e.serial] = &e;
  std::vector<FrameOut> out;
  out.reserve(recs.size());
  const float nan = NAN;
  for (const Rec& r : recs) {
    if (!r.xe1 || !r.p0 || r.p0 < r.xe1) continue;
    FrameOut f;
    for (float& v : f.seg) v = nan;
    f.p0 = r.p0;
    f.drain = r.drain;
    f.perturbed = r.perturbed;
    f.wfOnRender = r.wfTid && r.wfTid == rt;
    auto ms = [&](int64_t a, int64_t b) { return static_cast<float>((b - a) * qpcToMs); };
    f.seg[sCpu] = ms(r.xe1, r.p0);
    if (r.bx0) {
      f.seg[sToBegin] = ms(r.xe1, r.bx0);
      f.seg[sBegin] = ms(r.bx0, r.bx1);
      f.seg[sAcq] = static_cast<float>(r.acqTicks * qpcToMs);
      f.seg[sWait] = static_cast<float>(r.waitTicks * qpcToMs);
      if (r.c0 && r.c0 >= r.bfEnd) f.seg[sToCollect] = ms(r.bfEnd, r.c0);
    }
    if (r.c0 && r.c1 >= r.c0 && r.p0 >= r.c1) {
      f.seg[sCollect] = ms(r.c0, r.c1);
      f.seg[sToPass] = ms(r.c1, r.p0);
    }
    double drv = 0;
    if (r.xe0 && r.xe1 >= r.xe0) {
      f.seg[sXrWall] = ms(r.xe0, r.xe1);
      f.seg[sEnq] = static_cast<float>(r.enqTicks * qpcToMs);
      f.seg[sWso] = static_cast<float>(r.wsoTicks * qpcToMs);
      f.seg[sSleep] = static_cast<float>(r.sleepTicks * qpcToMs);
      drv = (r.enqTicks + r.wsoTicks + r.sleepTicks) * qpcToMs;
      f.seg[sXrRest] = static_cast<float>(f.seg[sXrWall] - drv);
    }
    if (r.wf1 && r.wf1 >= r.wf0) {
      f.seg[sWf] = ms(r.wf0, r.wf1);
      f.seg[sWfVsXe] = ms(r.xe1, r.wf1);
      if (r.bx0) f.seg[sBeginVsWf] = ms(r.wf1, r.bx0);
    }
    FrameIn in;
    in.cpuSegMs = f.seg[sCpu];
    in.xrWallMs = std::isnan(f.seg[sXrWall]) ? 0 : f.seg[sXrWall];
    in.drvWaitMs = drv;
    in.drain = r.drain;
    if (r.serial != ~0ull && r.serial > 0) {
      auto a = bySerial.find(r.serial - 1), b = bySerial.find(r.serial);
      if (a != bySerial.end() && b != bySerial.end() && a->second->freq == b->second->freq) {
        const Edge& ea = *a->second;
        const Edge& eb = *b->second;
        const double tk = 1000.0 / static_cast<double>(ea.freq);
        const int64_t from = ea.xrE >= 0 ? ea.xrE : ea.lastEnd;
        if (from >= 0 && eb.first >= from) {
          in.gpuValid = true;
          in.gpuGapMs = (eb.first - from) * tk;
          f.seg[sGap] = static_cast<float>(in.gpuGapMs);
          f.seg[sExcess] = static_cast<float>(in.gpuGapMs - in.cpuSegMs);
        }
        if (ea.xrB >= 0 && ea.xrE >= ea.xrB) f.seg[sGpuXr] = static_cast<float>((ea.xrE - ea.xrB) * tk);
        if (ea.xrB >= 0 && ea.lastEnd >= 0 && ea.xrB >= ea.lastEnd)
          f.seg[sLastToXr] = static_cast<float>((ea.xrB - ea.lastEnd) * tk);
      }
    }
    f.cls = Classify(in);
    out.push_back(f);
  }
  return out;
}

struct SegStat {
  int n = 0;
  double mean = 0, p95 = 0;
};
struct Summary {
  int frames = 0, excluded = 0, gpuFrames = 0;
  int cls[kCls] = {};
  int drain[7] = {};  // index drain + 1 (-1 .. 5)
  int wfOnRender = 0, wfSeen = 0;
  SegStat seg[kSegs];
  bool valid = false;
};

inline Summary Summarize(const std::vector<FrameOut>& v) {
  Summary s;
  std::vector<float> col[kSegs];
  for (const FrameOut& f : v) {
    if (f.perturbed) {
      ++s.excluded;
      continue;
    }
    ++s.frames;
    ++s.cls[f.cls];
    if (f.cls != kUnknown) ++s.gpuFrames;
    ++s.drain[std::max(-1, std::min(5, f.drain)) + 1];
    if (!std::isnan(f.seg[sWf])) {
      ++s.wfSeen;
      if (f.wfOnRender) ++s.wfOnRender;
    }
    for (int k = 0; k < kSegs; ++k)
      if (!std::isnan(f.seg[k])) col[k].push_back(f.seg[k]);
  }
  for (int k = 0; k < kSegs; ++k) {
    std::vector<float>& c = col[k];
    if (c.empty()) continue;
    double sum = 0;
    for (float x : c) sum += x;
    s.seg[k].n = static_cast<int>(c.size());
    s.seg[k].mean = sum / c.size();
    const size_t i = std::min(c.size() - 1, static_cast<size_t>(c.size() * 0.95));
    std::nth_element(c.begin(), c.begin() + i, c.end());
    s.seg[k].p95 = c[i];
  }
  s.valid = s.frames > 0;
  return s;
}

// The segment that grows most from a to b among the frame-start parts.
inline int GrowsMost(const Summary& a, const Summary& b, double* delta) {
  static const int kParts[] = {sToBegin, sBegin, sAcq, sWait, sToCollect, sCollect, sToPass, sGap, sXrWall, sWf};
  int best = -1;
  double d = -1e9;
  for (int k : kParts) {
    if (!a.seg[k].n || !b.seg[k].n) continue;
    const double x = b.seg[k].mean - a.seg[k].mean;
    if (x > d) d = x, best = k;
  }
  if (delta) *delta = d;
  return best;
}

// ---------------------------------------------------------------------------
// State (render thread unless noted)
// ---------------------------------------------------------------------------

std::atomic<bool> g_on{false};
std::atomic<DWORD> g_rt{0};
Rec g_cur;
bool g_haveXe = false;  // xrEndFrame returned, first pass not seen yet
bool g_inXr = false;
int64_t g_ovTicks = 0;  // our render-thread code
uint64_t g_recFrames = 0;

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
constexpr int kEv = 4;
ID3D11Query* g_ev[kEv] = {};
int g_evIdx = 0;
bool g_evPending = false;
std::atomic<bool> g_evReady{false}, g_evFailed{false}, g_releaseReq{false};

std::mutex g_mutex;
std::vector<Rec> g_recs;  // g_mutex
constexpr size_t kMaxRecs = 1 << 16;

// Any thread.
std::atomic<int64_t> g_wf0{0}, g_wf1{0}, g_wfInFlight{0};
std::atomic<DWORD> g_wfTid{0};
std::atomic<uint64_t> g_wfCalls{0};
// Driver calls not inside the render thread's xrEndFrame (calls, ticks).
std::atomic<uint64_t> g_enqElse{0}, g_wsoElse{0}, g_sleepElse{0}, g_enqElseTicks{0}, g_wsoElseTicks{0};

inline int64_t Qpc() {
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  return q.QuadPart;
}
inline bool OnRt() {
  return g_on.load(std::memory_order_relaxed) && GetCurrentThreadId() == g_rt.load(std::memory_order_relaxed);
}

void ReleaseQueries() {
  for (auto*& q : g_ev) {
    if (q) q->Release();
    q = nullptr;
  }
  g_evPending = false;
  g_evReady = false;
}

void CreateQueries() {
  D3D11_QUERY_DESC d{D3D11_QUERY_EVENT, 0};
  for (auto*& q : g_ev)
    if (!q && FAILED(g_dev->CreateQuery(&d, &q))) {
      q = nullptr;
      ReleaseQueries();
      g_evFailed = true;
      return;
    }
  g_evReady = true;
}

void Poll(int stage) {
  if (!g_evPending || g_cur.drain >= 0) return;
  BOOL done = FALSE;
  if (g_ctx->GetData(g_ev[g_evIdx], &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
    g_cur.drain = stage;
    g_evPending = false;
  }
}

// ---------------------------------------------------------------------------
// Probes called from other files
// ---------------------------------------------------------------------------

void OnXrEndEnter(DWORD tid) {
  if (g_releaseReq.load(std::memory_order_relaxed) && tid == g_rt.load(std::memory_order_relaxed)) {
    ReleaseQueries();
    g_releaseReq = false;
  }
  if (!g_on.load(std::memory_order_relaxed)) return;
  DWORD rt = g_rt.load(std::memory_order_relaxed);
  if (!rt) g_rt.store(rt = tid, std::memory_order_relaxed);
  if (tid != rt) return;
  const int64_t t = Qpc();
  if (!g_evReady.load(std::memory_order_relaxed) && !g_evFailed.load(std::memory_order_relaxed) && g_dev) CreateQueries();
  g_cur = Rec{};
  g_haveXe = false;
  g_inXr = true;
  if (g_evReady.load(std::memory_order_relaxed)) {
    g_evIdx = (g_evIdx + 1) % kEv;
    g_ctx->End(g_ev[g_evIdx]);
    g_evPending = true;
  }
  const int64_t e = Qpc();
  g_cur.xe0 = e;
  g_ovTicks += e - t;
}

void OnXrEndReturn(DWORD tid) {
  if (!g_on.load(std::memory_order_relaxed) || tid != g_rt.load(std::memory_order_relaxed) || !g_inXr) return;
  const int64_t t = Qpc();
  g_cur.xe1 = t;
  g_inXr = false;
  g_haveXe = true;
  Poll(0);
  g_ovTicks += Qpc() - t;
}

void OnCollectBegin(int64_t t0) {
  if (!OnRt() || !g_haveXe) return;
  const int64_t a = Qpc();
  if (!g_cur.c0) {
    Poll(2);
    g_cur.c0 = t0;
  }
  g_ovTicks += Qpc() - a;
}

void OnCollectEnd(int64_t t1) {
  if (!OnRt() || !g_haveXe) return;
  const int64_t a = Qpc();
  if (g_cur.c0) {
    g_cur.c1 = t1;
    ++g_cur.collectN;
    Poll(3);
  }
  g_ovTicks += Qpc() - a;
}

// ctiming::HookAggregated (render thread): culling start / end.
void OnAggregated(int64_t t, bool begin) {
  if (begin)
    OnCollectBegin(t);
  else
    OnCollectEnd(t);
}

void OnTopPass(int64_t qpc, uint64_t gpuSerial) {
  if (!g_haveXe || !OnRt()) return;
  const int64_t a = Qpc();
  Poll(4);
  if (g_cur.drain < 0 && g_evPending) g_cur.drain = 5;
  g_cur.p0 = qpc;
  g_cur.serial = gpuSerial;
  g_cur.wf0 = g_wf0.load(std::memory_order_relaxed);
  g_cur.wf1 = g_wf1.load(std::memory_order_relaxed);
  g_cur.wfTid = g_wfTid.load(std::memory_order_relaxed);
  if (g_cur.wf1 < g_cur.wf0 || g_cur.wf1 > qpc) g_cur.wf0 = g_cur.wf1 = 0;  // torn or after the pass
  g_cur.perturbed = rthreads::g_on.load(std::memory_order_relaxed) && rthreads::g_entrySampled;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_recs.size() < kMaxRecs) g_recs.push_back(g_cur);
  }
  ++g_recFrames;
  g_haveXe = false;
  g_ovTicks += Qpc() - a;
}

// ---------------------------------------------------------------------------
// OpenXR dispatch table hooks
// ---------------------------------------------------------------------------

using Xr2 = int32_t(__stdcall*)(void*, const void*);
using Xr3 = int32_t(__stdcall*)(void*, const void*, void*);
enum XrSlot { kWaitFrame = 0, kBeginFrame, kAcquire, kWaitImage, kXrSlots };
constexpr int kDispIndex[kXrSlots] = {33, 34, 27, 28};
const char* const kXrName[kXrSlots] = {"xrWaitFrame", "xrBeginFrame", "xrAcquireSwapchainImage",
                                       "xrWaitSwapchainImage"};
void* g_xrOrig[kXrSlots] = {};
void** g_xrSlotPtr[kXrSlots] = {};

int32_t __stdcall H_WaitFrame(void* s, const void* info, void* state) {
  const int64_t t0 = Qpc();
  g_wfInFlight.store(t0, std::memory_order_relaxed);
  const int32_t r = reinterpret_cast<Xr3>(g_xrOrig[kWaitFrame])(s, info, state);
  const int64_t t1 = Qpc();
  const DWORD tid = GetCurrentThreadId();
  g_wf0.store(t0, std::memory_order_relaxed);
  g_wf1.store(t1, std::memory_order_relaxed);
  g_wfTid.store(tid, std::memory_order_relaxed);
  g_wfInFlight.store(0, std::memory_order_relaxed);
  g_wfCalls.fetch_add(1, std::memory_order_relaxed);
  rthreads::g_xrWaitTid.store(tid, std::memory_order_relaxed);
  return r;
}

int32_t __stdcall H_BeginFrame(void* s, const void* info) {
  const bool rt = OnRt() && g_haveXe && !g_cur.bx0;
  if (rt) {
    Poll(1);
    g_cur.wfInFlight = g_wfInFlight.load(std::memory_order_relaxed) != 0;
  }
  const int64_t t0 = rt ? Qpc() : 0;
  const int32_t r = reinterpret_cast<Xr2>(g_xrOrig[kBeginFrame])(s, info);
  if (rt) {
    g_cur.bx0 = t0;
    g_cur.bx1 = g_cur.bfEnd = Qpc();
  }
  return r;
}

int32_t __stdcall H_Acquire(void* sc, const void* info, void* index) {
  const bool rt = OnRt() && g_haveXe;
  const int64_t t0 = rt ? Qpc() : 0;
  const int32_t r = reinterpret_cast<Xr3>(g_xrOrig[kAcquire])(sc, info, index);
  if (rt) {
    const int64_t t1 = Qpc();
    g_cur.acqTicks += t1 - t0;
    ++g_cur.acqN;
    g_cur.bfEnd = t1;
  }
  return r;
}

int32_t __stdcall H_WaitImage(void* sc, const void* info) {
  const bool rt = OnRt() && g_haveXe;
  const int64_t t0 = rt ? Qpc() : 0;
  const int32_t r = reinterpret_cast<Xr2>(g_xrOrig[kWaitImage])(sc, info);
  if (rt) {
    const int64_t t1 = Qpc();
    g_cur.waitTicks += t1 - t0;
    ++g_cur.waitN;
    g_cur.bfEnd = t1;
  }
  return r;
}

void* const kXrHook[kXrSlots] = {reinterpret_cast<void*>(&H_WaitFrame), reinterpret_cast<void*>(&H_BeginFrame),
                                  reinterpret_cast<void*>(&H_Acquire), reinterpret_cast<void*>(&H_WaitImage)};

// The analysed Visualizer.dll builds (R147: DCS 2.9.30 and 2.9.31). 2.9.31 (PE 0x6ad87745) has the same OpenXR
// trampolines, getter and instance layout; its .text moved +0x2c790 and the instance static +0x2e010 (re-derived
// offline from the 2.9.31 binary: unique byte matches, the getter's lea). Index = the column of the per-build RVAs
// in kTramp and in frame_timeline.h / cockpit_mask.h / cockpit_time.h / view_profile.h.
constexpr int kVisBuildCount = 2;
struct VisBuild {
  uint32_t stamp;
  const char* dcs;
  uint32_t instStaticRva, instLeaRva;
  uint8_t instLea[7];  // the getter's lea rax, [instance static]
};
const VisBuild kVisBuilds[kVisBuildCount] = {
    {0x6ac12454u, "2.9.30", 0x2c8068, 0x1c75e7, {0x48, 0x8d, 0x05, 0x7a, 0x0a, 0x10, 0x00}},
    {0x6ad87745u, "2.9.31", 0x2f6078, 0x1f3d77, {0x48, 0x8d, 0x05, 0xfa, 0x22, 0x10, 0x00}},
};
// Which analysed build vis is (PE timestamp and the getter's lea, byte for byte); -1: none (hooks stay off).
int VisBuildOf(uint8_t* vis) {
  if (!vis) return -1;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(vis + reinterpret_cast<IMAGE_DOS_HEADER*>(vis)->e_lfanew);
  for (int b = 0; b < kVisBuildCount; ++b)
    if (nt->FileHeader.TimeDateStamp == kVisBuilds[b].stamp &&
        gpt::CodeMatches(vis + kVisBuilds[b].instLeaRva, kVisBuilds[b].instLea, sizeof(kVisBuilds[b].instLea)))
      return b;
  return -1;
}
struct Tramp {
  uint32_t rva[kVisBuildCount];
  uint8_t code[11];  // mov rX, [rax+48h]; mov rax, [rX+slot*8]
};
const Tramp kTramp[kXrSlots] = {
    {{0x1b7840, 0x1e3fd0}, {0x4c, 0x8b, 0x48, 0x48, 0x49, 0x8b, 0x81, 0x08, 0x01, 0x00, 0x00}},
    {{0x1b648f, 0x1e2c1f}, {0x4c, 0x8b, 0x40, 0x48, 0x49, 0x8b, 0x80, 0x10, 0x01, 0x00, 0x00}},
    {{0x1b6310, 0x1e2aa0}, {0x4c, 0x8b, 0x48, 0x48, 0x49, 0x8b, 0x81, 0xd8, 0x00, 0x00, 0x00}},
    {{0x1b78bf, 0x1e404f}, {0x4c, 0x8b, 0x40, 0x48, 0x49, 0x8b, 0x80, 0xe0, 0x00, 0x00, 0x00}}};

// build: VisBuildOf(vis), checked by the caller (>= 0).
void** DispatchGuarded(uint8_t* vis, int build) {
  __try {
    uint8_t* inst = *reinterpret_cast<uint8_t**>(vis + kVisBuilds[build].instStaticRva);
    if (!inst) return nullptr;
    return *reinterpret_cast<void***>(inst + 0x48);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

bool InstallXr() {
  auto* vis = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"Visualizer.dll"));
  if (!vis) {
    Log("  frame start: Visualizer.dll not loaded; OpenXR calls not timed");
    return false;
  }
  const int build = VisBuildOf(vis);
  if (build < 0) {
    Log("  frame start: Visualizer.dll is not an analysed build; OpenXR calls not timed");
    return false;
  }
  for (const Tramp& t : kTramp)
    if (!gpt::CodeMatches(vis + t.rva[build], t.code, sizeof(t.code))) {
      Log("  frame start: OpenXR trampoline at Visualizer+0x%x differs; OpenXR calls not timed", t.rva[build]);
      return false;
    }
  void** disp = DispatchGuarded(vis, build);
  if (!disp || !gpt::Readable(disp) || !gpt::Readable(&disp[40])) {
    Log("  frame start: OpenXR dispatch table not found; OpenXR calls not timed");
    return false;
  }
  char m[200];
  for (int i = 0; i < kXrSlots; ++i)
    if (!gpt::ModRva(SlotOriginal(&disp[kDispIndex[i]]), m, sizeof(m))) {
      Log("  frame start: dispatch entry %s is not in a loaded image; OpenXR calls not timed", kXrName[i]);
      return false;
    }
  std::string where;
  for (int i = 0; i < kXrSlots; ++i) {
    void** slot = &disp[kDispIndex[i]];
    g_xrOrig[i] = SlotOriginal(slot);  // before the slot points at the hook
    if (!HookSlot(slot, kXrHook[i], nullptr)) {
      Log("  frame start: could not hook %s", kXrName[i]);
      continue;
    }
    g_xrSlotPtr[i] = slot;
    gpt::ModRva(g_xrOrig[i], m, sizeof(m));
    where += where.empty() ? "" : ", ";
    where += std::string(kXrName[i]) + " -> " + m;
  }
  Log("  frame start: OpenXR dispatch hooked: %s", where.c_str());
  return true;
}

void UninstallXr() {
  for (int i = 0; i < kXrSlots; ++i)
    if (g_xrSlotPtr[i]) {
      UnhookSlot(g_xrSlotPtr[i], g_xrOrig[i]);
      g_xrSlotPtr[i] = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Driver calls inside xrEndFrame: EnqueueSetEvent, the Pimax client's waits
// ---------------------------------------------------------------------------

using EnqFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIDevice2*, HANDLE);
using WsoFn = DWORD(WINAPI*)(HANDLE, DWORD, BOOL);
using SleepFn = void(WINAPI*)(DWORD);
EnqFn g_enqOrig = nullptr;
WsoFn g_wsoOrig = nullptr;
SleepFn g_sleepOrig = nullptr;
void** g_enqSlot = nullptr;
void** g_wsoSlot = nullptr;
void** g_sleepSlot = nullptr;

inline bool InXrOnRt() { return g_inXr && OnRt(); }

HRESULT STDMETHODCALLTYPE H_Enqueue(IDXGIDevice2* d, HANDLE e) {
  const int64_t t0 = Qpc();
  const HRESULT r = g_enqOrig(d, e);
  const int64_t dt = Qpc() - t0;
  if (InXrOnRt()) {
    g_cur.enqTicks += dt;
    ++g_cur.enqN;
  } else if (g_on.load(std::memory_order_relaxed)) {
    g_enqElse.fetch_add(1, std::memory_order_relaxed);
    g_enqElseTicks.fetch_add(static_cast<uint64_t>(dt), std::memory_order_relaxed);
  }
  return r;
}

DWORD WINAPI H_Wso(HANDLE h, DWORD ms, BOOL alertable) {
  const int64_t t0 = Qpc();
  const DWORD r = g_wsoOrig(h, ms, alertable);
  const int64_t dt = Qpc() - t0;
  if (InXrOnRt()) {
    g_cur.wsoTicks += dt;
    ++g_cur.wsoN;
  } else if (g_on.load(std::memory_order_relaxed)) {
    g_wsoElse.fetch_add(1, std::memory_order_relaxed);
    g_wsoElseTicks.fetch_add(static_cast<uint64_t>(dt), std::memory_order_relaxed);
  }
  return r;
}

void WINAPI H_Sleep(DWORD ms) {
  const int64_t t0 = Qpc();
  g_sleepOrig(ms);
  if (InXrOnRt()) {
    g_cur.sleepTicks += Qpc() - t0;
    ++g_cur.sleepN;
  } else if (g_on.load(std::memory_order_relaxed)) {
    g_sleepElse.fetch_add(1, std::memory_order_relaxed);
  }
}

HMODULE OwnerOf(const void* p) {
  HMODULE m = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     static_cast<LPCWSTR>(p), &m);
  return m;
}

void InstallDriver(ID3D11Device* dev) {
  IDXGIDevice2* dx = nullptr;
  if (dev && SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice2), reinterpret_cast<void**>(&dx))) && dx) {
    void** vt = *reinterpret_cast<void***>(dx);
    const HMODULE d3d = GetModuleHandleW(L"d3d11.dll");
    void* cur = SlotOriginal(&vt[16]);
    if (d3d && OwnerOf(vt) == d3d && OwnerOf(cur) == d3d) {
      g_enqOrig = reinterpret_cast<EnqFn>(cur);
      if (HookSlot(&vt[16], reinterpret_cast<void*>(&H_Enqueue), nullptr)) g_enqSlot = &vt[16];
    } else {
      Log("  frame start: IDXGIDevice2 table is not d3d11.dll's own; EnqueueSetEvent not timed");
    }
    dx->Release();
  }
  if (HMODULE pvr = GetModuleHandleW(L"libPVRClient64.dll")) {
    if (void** s = timercache::FindImport(pvr, "KERNEL32.dll", "WaitForSingleObjectEx")) {
      g_wsoOrig = reinterpret_cast<WsoFn>(SlotOriginal(s));
      if (HookSlot(s, reinterpret_cast<void*>(&H_Wso), nullptr)) g_wsoSlot = s;
    }
    if (void** s = timercache::FindImport(pvr, "KERNEL32.dll", "Sleep")) {
      g_sleepOrig = reinterpret_cast<SleepFn>(SlotOriginal(s));
      if (HookSlot(s, reinterpret_cast<void*>(&H_Sleep), nullptr)) g_sleepSlot = s;
    }
  }
  Log("  frame start: inside xrEndFrame timed: EnqueueSetEvent %s, libPVRClient64 WaitForSingleObjectEx %s, Sleep %s "
      "(Flush not timed: the context table is rewritten inside xrEndFrame)",
      g_enqSlot ? "yes" : "no", g_wsoSlot ? "yes" : "no", g_sleepSlot ? "yes" : "no");
}

void UninstallDriver() {
  if (g_enqSlot) UnhookSlot(g_enqSlot, reinterpret_cast<void*>(g_enqOrig));
  if (g_wsoSlot) UnhookSlot(g_wsoSlot, reinterpret_cast<void*>(g_wsoOrig));
  if (g_sleepSlot) UnhookSlot(g_sleepSlot, reinterpret_cast<void*>(g_sleepOrig));
  g_enqSlot = g_wsoSlot = g_sleepSlot = nullptr;
}

// ---------------------------------------------------------------------------
// Session and windows (suite thread)
// ---------------------------------------------------------------------------

bool g_startedGpt = false;
std::atomic<bool> g_sessionOn{false};

// Needs gpu_pass_timing's session (light mode): started here when none runs.
bool Begin(std::atomic<uint64_t>& frameCounter) {
  if (g_sessionOn.load()) return true;
  g_startedGpt = false;
  if (!gpt::g_sessDev) {
    gpt::Begin(frameCounter, true);
    g_startedGpt = gpt::g_sessDev != nullptr;
  }
  if (!gpt::g_sessDev || !gpt::g_xrSlot) {
    Log("  frame start: needs GPU pass timing and its xrEndFrame hook (device from [Model] ShadowInstancing=1 or [D3D] "
        "SplitFilter=1, the analysed Visualizer.dll); not measured");
    if (g_startedGpt) gpt::End();
    g_startedGpt = false;
    return false;
  }
  g_dev = gpt::g_sessDev;
  g_ctx = gpt::g_sessCtx;
  g_dev->AddRef();
  g_ctx->AddRef();
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_recs.clear();
    g_recs.reserve(8192);
  }
  g_ovTicks = 0;
  g_recFrames = 0;
  g_haveXe = g_inXr = false;
  g_evFailed = false;
  g_releaseReq = false;
  g_rt = gpt::g_xrTid.load();
  g_wf0 = g_wf1 = g_wfInFlight = 0;
  g_wfTid = 0;
  g_wfCalls = 0;
  g_enqElse = g_wsoElse = g_sleepElse = g_enqElseTicks = g_wsoElseTicks = 0;
  InstallXr();
  InstallDriver(g_dev);
  if (!ctiming::g_aggregated) Log("  frame start: collectRenderablesAggregated not hooked; culling rows n/a");
  ctiming::g_aggObserver = &OnAggregated;
  g_sessionOn = true;
  g_on = true;
  return true;
}

void End() {
  if (!g_sessionOn.exchange(false)) return;
  g_on = false;
  void (*ob)(int64_t, bool) = &OnAggregated;
  ctiming::g_aggObserver.compare_exchange_strong(ob, nullptr);
  UninstallXr();
  UninstallDriver();
  // The event queries go on the render thread (single-threaded devices).
  if (g_evReady.load()) {
    g_releaseReq = true;
    for (int i = 0; i < 100 && g_releaseReq.load(); ++i) Sleep(10);
    if (g_releaseReq.exchange(false)) {
      if (!shadowinst::g_deviceSingleThreaded) {
        Sleep(50);
        ReleaseQueries();
      } else {
        Log("  frame start: render thread did not release the event queries (no frame); left alive");
      }
    }
  }
  if (g_evFailed.load()) Log("  frame start: event queries could not be created; drain stage n/a");
  g_ctx->Release();
  g_dev->Release();
  g_ctx = nullptr;
  g_dev = nullptr;
  if (g_startedGpt) gpt::End();
  g_startedGpt = false;
}

struct Window {
  int64_t qa = 0;
  uint64_t wf0 = 0, enq0 = 0, wso0 = 0, sleep0 = 0;
};
Window Open() {
  Window w;
  w.qa = Qpc();
  w.wf0 = g_wfCalls.load();
  w.enq0 = g_enqElse.load();
  w.wso0 = g_wsoElse.load();
  w.sleep0 = g_sleepElse.load();
  return w;
}

struct Result {
  Summary sum;
  std::vector<FrameOut> frames;
  double wfPerFrame = 0, enqElse = 0, wsoElse = 0, sleepElse = 0;  // per frame
  DWORD wfTid = 0, rt = 0;
};

// done: gpu_pass_timing frames covering the window (not consumed).
// qb: the window's end (0 = now).
Result Close(const Window& w, const std::vector<gpt::Done>& done, int64_t qb = 0) {
  Result r;
  if (!qb) qb = Qpc();
  std::vector<Rec> recs;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const Rec& x : g_recs)
      if (x.p0 >= w.qa && x.p0 <= qb) recs.push_back(x);
  }
  std::vector<Edge> edges;
  for (const gpt::Done& d : done) {
    Edge e;
    if (EdgeOf(d, &e)) edges.push_back(e);
  }
  r.rt = g_rt.load();
  r.wfTid = g_wfTid.load();
  r.frames = Derive(recs, edges, g_qpcToUs / 1000.0, r.rt);
  r.sum = Summarize(r.frames);
  const double n = static_cast<double>(std::max<size_t>(1, recs.size()));
  r.wfPerFrame = (g_wfCalls.load() - w.wf0) / n;
  r.enqElse = (g_enqElse.load() - w.enq0) / n;
  r.wsoElse = (g_wsoElse.load() - w.wso0) / n;
  r.sleepElse = (g_sleepElse.load() - w.sleep0) / n;
  return r;
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

std::string Ms(const SegStat& s) {
  if (!s.n) return "n/a";
  char b[48];
  snprintf(b, sizeof(b), "%.3f (p95 %.3f)", s.mean, s.p95);
  return b;
}
std::string M(const SegStat& s) {
  if (!s.n) return "n/a";
  char b[24];
  snprintf(b, sizeof(b), "%.3f", s.mean);
  return b;
}

std::string ClassLine(const Summary& s) {
  std::string out;
  char b[64];
  for (int c = 0; c < kCls; ++c) {
    if (!s.cls[c]) continue;
    snprintf(b, sizeof(b), "%s%s %.0f%%", out.empty() ? "" : ", ", kClsName[c], 100.0 * s.cls[c] / s.frames);
    out += b;
  }
  return out.empty() ? "-" : out;
}

std::string DrainLine(const Summary& s) {
  static const char* const kStage[7] = {"no event", "at xrEndFrame return", "by xrBeginFrame", "by culling start",
                                        "by culling end", "by first pass", "later"};
  std::string out;
  char b[64];
  for (int i = 0; i < 7; ++i) {
    if (!s.drain[i]) continue;
    snprintf(b, sizeof(b), "%s%s %.0f%%", out.empty() ? "" : ", ", kStage[i], 100.0 * s.drain[i] / s.frames);
    out += b;
  }
  return out.empty() ? "-" : out;
}

void LogResult(const char* label, const Result& r, bool full) {
  const Summary& s = r.sum;
  if (!s.valid) {
    Log("  frame start %s: no frame recorded (xrEndFrame -> first pass not seen on the render thread)", label);
    return;
  }
  const SegStat* g = s.seg;
  Log("  frame start %s: %d frames (%d with a GPU pair, %d left out: runnable-threads snapshot) | %s | cpu xrEndFrame "
      "return -> first pass %s, gpu gap %s, gap - cpu %s | drained %s",
      label, s.frames, s.gpuFrames, s.excluded, ClassLine(s).c_str(), Ms(g[sCpu]).c_str(), Ms(g[sGap]).c_str(),
      M(g[sExcess]).c_str(), DrainLine(s).c_str());
  Log("  frame start %s: cpu segment = to xrBeginFrame %s + xrBeginFrame %s + acquire %s + wait %s + to culling %s + "
      "culling %s + culling end -> first pass %s | xrEndFrame %s = EnqueueSetEvent %s + client waits %s + Sleep %s + "
      "rest %s",
      label, M(g[sToBegin]).c_str(), M(g[sBegin]).c_str(), M(g[sAcq]).c_str(), M(g[sWait]).c_str(),
      M(g[sToCollect]).c_str(), M(g[sCollect]).c_str(), M(g[sToPass]).c_str(), M(g[sXrWall]).c_str(),
      M(g[sEnq]).c_str(), M(g[sWso]).c_str(), M(g[sSleep]).c_str(), M(g[sXrRest]).c_str());
  if (!full) return;
  for (int k = 0; k < kSegs; ++k) Log("    %-48s %s", kSegName[k], Ms(g[k]).c_str());
  Log("  frame start %s: xrWaitFrame %.2f calls/frame on thread %lu (%s), the render thread is %lu; frames whose last "
      "xrWaitFrame ran on the render thread %d of %d",
      label, r.wfPerFrame, static_cast<unsigned long>(r.wfTid),
      !r.wfTid ? "not seen" : r.wfTid == r.rt ? "the render thread" : "not the render thread",
      static_cast<unsigned long>(r.rt), s.wfOnRender, s.wfSeen);
  Log("  frame start %s: driver calls outside the render thread's xrEndFrame per frame: EnqueueSetEvent %.2f, client "
      "WaitForSingleObjectEx %.2f, Sleep %.2f",
      label, r.enqElse, r.wsoElse, r.sleepElse);
}

// still -> turning (RotationProfile): every segment and the part that grows most.
void LogCompare(const Result& a, const Result& b) {
  const Summary& s = a.sum;
  const Summary& t = b.sum;
  if (!s.valid || !t.valid) return;
  Log("  frame start still -> turning: classes %s -> %s", ClassLine(s).c_str(), ClassLine(t).c_str());
  for (int k = 0; k < kSegs; ++k) {
    if (!s.seg[k].n && !t.seg[k].n) continue;
    Log("    %-48s %8s -> %8s  (%+.3f)", kSegName[k], M(s.seg[k]).c_str(), M(t.seg[k]).c_str(),
        t.seg[k].mean - s.seg[k].mean);
  }
  double d = 0;
  const int k = GrowsMost(s, t, &d);
  if (k >= 0) Log("  frame start: grows most when turning: %s (%+.3f ms)", kSegName[k], d);
}

double OverheadMsPerFrame() {
  return g_recFrames ? g_ovTicks * g_qpcToUs / 1000.0 / static_cast<double>(g_recFrames) : 0.0;
}

// Standalone phase: [Suite] FrameStartGapSec at the current (held) view.
void RunPhase(int sec, std::atomic<uint64_t>& frameCounter, std::atomic<bool>& abort) {
  if (!Begin(frameCounter)) return;
  Sleep(500);
  gpt::TakeDone();
  const Window w = Open();
  for (int t = 0; t < sec * 1000 && !abort; t += 50) Sleep(50);
  Sleep(150);  // GPU frames are read back a few frames late
  std::vector<gpt::Done> done = gpt::TakeDone();
  const Result r = Close(w, done);
  const double gpuOv = gpt::OverheadMsPerFrame();
  End();
  LogResult("phase", r, true);
  Log("  frame start: measurement cost on the render thread %.4f ms/frame (probes) + %.4f ms/frame (GPU timestamps, "
      "light mode)",
      OverheadMsPerFrame(), gpuOv);
}

}  // namespace fstart
