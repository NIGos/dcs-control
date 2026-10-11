// Offline test for frame_start.h ([Suite] FrameStartGap, R22 E1) and
// run_threads.h ([Suite] RunnableThreads, R22 E3): the per-frame classes,
// GPU edges, segments and summaries on synthetic records; the
// SystemProcessInformation walk on a synthetic buffer and on this process;
// the window summary (heavy frames, gate, bins, run share); then live: a
// session on this thread (snapshots at "render entry" and a job start, the
// thread table), the event-query drain probe on a real device, and the
// EnqueueSetEvent table hook installed, counted and restored.
#pragma once

namespace fsttest {

bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

void Classes() {
  using namespace fstart;
  FrameIn f;
  Check(Classify(f) == kUnknown, "frame start: no GPU pair -> unknown");
  f.gpuValid = true;
  f.gpuGapMs = 0.2;
  Check(Classify(f) == kNoGap, "frame start: gap < 0.3 ms -> no gap");
  f.gpuGapMs = 2.8;
  f.cpuSegMs = 2.7;
  f.drain = 0;
  Check(Classify(f) == kTurns, "frame start: drained at xrEndFrame return, gap ~ cpu segment -> take turns");
  f.drain = 4;
  Check(Classify(f) == kCpuLate, "frame start: not drained at return, gap covered by the cpu segment -> CPU late");
  f.drain = 0;
  f.cpuSegMs = 1.0;
  f.xrWallMs = 5.0;
  f.drvWaitMs = 3.0;
  Check(Classify(f) == kFence, "frame start: gap > cpu segment, xrEndFrame mostly driver waits -> fence/driver");
  f.drvWaitMs = 0.4;
  Check(Classify(f) == kRuntime, "frame start: gap > cpu segment, no driver wait -> runtime/compositor");
  f.cpuSegMs = 2.4;  // tol = max(0.3, 0.48): 2.8 <= 2.88
  Check(Classify(f) == kTurns, "frame start: the tolerance is 20% of the cpu segment");
}

gpt::Done MakeDone(uint64_t serial, uint64_t o) {
  gpt::Done d;
  d.serial = serial;
  d.freq = 1000000;  // 1 tick = 1 us
  void* A = reinterpret_cast<void*>(0xA000);
  void* B = reinterpret_cast<void*>(0xB000);
  d.ev = {{A, gpt::kBegin, 0}, {B, gpt::kBegin, 1}, {B, gpt::kEnd, 1}, {A, gpt::kEnd, 0},
          {nullptr, gpt::kXrBegin, 0}, {nullptr, gpt::kXrEnd, 0}};
  d.ticks = {o + 3000, o + 4000, o + 9000, o + 15000, o + 15500, o + 17000};
  return d;
}

void Frames() {
  using namespace fstart;
  // GPU: frame 4 at 0, frame 5 at 20000 us: gap = 23000 - 17000 = 6 ms; xr 1.5 ms; last pass -> xr 0.5 ms.
  std::vector<gpt::Done> done = {MakeDone(4, 0), MakeDone(5, 20000)};
  std::vector<Edge> edges;
  for (const gpt::Done& d : done) {
    Edge e;
    if (EdgeOf(d, &e)) edges.push_back(e);
  }
  Check(edges.size() == 2 && edges[0].first == 3000 && edges[0].lastEnd == 15000 && edges[0].xrB == 15500 &&
            edges[0].xrE == 17000,
        "frame start: GPU edges of a frame (first pass, last top-level end, xrEndFrame stamps)");
  // CPU (QPC = us here: qpcToMs = 0.001).
  Rec r;
  r.xe0 = 1000;
  r.xe1 = 6000;  // 5 ms wrapper
  r.enqTicks = 3000;
  r.enqN = 1;
  r.wsoTicks = 500;
  r.wsoN = 2;
  r.bx0 = 6500;
  r.bx1 = 6700;
  r.acqTicks = 100;
  r.waitTicks = 300;
  r.bfEnd = 7300;
  r.c0 = 7400;
  r.c1 = 9400;
  r.p0 = 10000;  // cpu segment 4 ms
  r.wf0 = 2000;
  r.wf1 = 6200;
  r.wfTid = 77;
  r.drain = 0;
  r.serial = 5;
  Rec q = r;
  q.serial = 9;  // no GPU pair
  q.perturbed = true;
  std::vector<FrameOut> out = Derive({r, q}, edges, 0.001, 1);
  Check(out.size() == 2, "frame start: one output per complete record");
  const FrameOut& f = out[0];
  Check(Near(f.seg[sCpu], 4.0, 1e-4) && Near(f.seg[sToBegin], 0.5, 1e-4) && Near(f.seg[sBegin], 0.2, 1e-4) &&
            Near(f.seg[sAcq], 0.1, 1e-4) && Near(f.seg[sWait], 0.3, 1e-4) && Near(f.seg[sToCollect], 0.1, 1e-4) &&
            Near(f.seg[sCollect], 2.0, 1e-4) && Near(f.seg[sToPass], 0.6, 1e-4),
        "frame start: cpu segment and its parts");
  Check(Near(f.seg[sGap], 6.0, 1e-4) && Near(f.seg[sExcess], 2.0, 1e-4) && Near(f.seg[sGpuXr], 1.5, 1e-4) &&
            Near(f.seg[sLastToXr], 0.5, 1e-4),
        "frame start: GPU gap from the previous frame's xrEndFrame stamp, gap - cpu, wrapper GPU interval");
  Check(Near(f.seg[sXrWall], 5.0, 1e-4) && Near(f.seg[sEnq], 3.0, 1e-4) && Near(f.seg[sWso], 0.5, 1e-4) &&
            Near(f.seg[sXrRest], 1.5, 1e-4) && Near(f.seg[sWf], 4.2, 1e-4) && Near(f.seg[sWfVsXe], 0.2, 1e-4) &&
            Near(f.seg[sBeginVsWf], 0.3, 1e-4),
        "frame start: xrEndFrame split and xrWaitFrame relations");
  Check(f.cls == kFence && !f.wfOnRender, "frame start: 6 ms gap vs 4 ms cpu, 3.5 of 5 ms in driver waits -> fence");
  Check(out[1].cls == kUnknown && std::isnan(out[1].seg[sGap]), "frame start: no GPU pair -> unknown, gap n/a");
  Summary s = Summarize(out);
  Check(s.frames == 1 && s.excluded == 1 && s.cls[kFence] == 1 && s.drain[1] == 1 && s.seg[sCpu].n == 1 &&
            Near(s.seg[sCpu].mean, 4.0, 1e-4),
        "frame start: summary leaves out snapshot frames, counts classes and drain stages");
  // Turning: culling grows by 1.5 ms.
  std::vector<FrameOut> t = out;
  t[0].seg[sCollect] += 1.5f;
  t[0].seg[sCpu] += 1.5f;
  double d = 0;
  const int k = GrowsMost(s, Summarize(t), &d);
  Check(k == sCollect && Near(d, 1.5, 1e-4), "frame start: the part that grows most when turning");
}

// A synthetic SystemProcessInformation buffer: process 10 (1 thread), then process 20 (3 threads).
std::vector<uint8_t> FakeBuffer() {
  using namespace rthreads;
  const size_t p1 = kProcThreads + sizeof(SysThread), p2 = kProcThreads + 3 * sizeof(SysThread);
  std::vector<uint8_t> b(p1 + p2, 0);
  auto put = [&](size_t off, ULONG next, ULONG n, uintptr_t pid) {
    *reinterpret_cast<ULONG*>(&b[off]) = next;
    *reinterpret_cast<ULONG*>(&b[off + kProcNumThreads]) = n;
    *reinterpret_cast<uintptr_t*>(&b[off + kProcPid]) = pid;
  };
  put(0, static_cast<ULONG>(p1), 1, 10);
  put(p1, 0, 3, 20);
  SysThread* t = reinterpret_cast<SysThread*>(&b[p1 + kProcThreads]);
  const ULONG states[3] = {2, 1, 5};
  for (int i = 0; i < 3; ++i) {
    t[i].tid = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(100 + i));
    t[i].state = states[i];
  }
  return b;
}

void Walk() {
  using namespace rthreads;
  std::vector<uint8_t> b = FakeBuffer();
  int run = 0, ready = 0, wait = 0, n = 0;
  const bool found = ForEachThread(b.data(), b.size(), 20, [&](const SysThread& t) {
    ++n;
    if (IsRunning(t.state)) ++run;
    else if (IsRunnable(t.state)) ++ready;
    else ++wait;
  });
  Check(found && n == 3 && run == 1 && ready == 1 && wait == 1, "runnable threads: walk of a synthetic process list");
  Check(!ForEachThread(b.data(), b.size(), 30, [](const SysThread&) {}), "runnable threads: absent process");
  Check(!ForEachThread(b.data(), b.size() - 8, 20, [](const SysThread&) {}), "runnable threads: truncated buffer");
  // Live: this process, this thread running.
  std::vector<uint8_t> live;
  const size_t len = QueryAll(live);
  bool selfRunning = false;
  int mine = 0;
  const DWORD self = GetCurrentThreadId();
  ForEachThread(live.data(), len, GetCurrentProcessId(), [&](const SysThread& t) {
    ++mine;
    if (reinterpret_cast<uintptr_t>(t.tid) == self && IsRunning(t.state)) selfRunning = true;
  });
  Check(len > 0 && mine >= 1 && selfRunning, "runnable threads: live snapshot of this process (this thread running)");
}

void WindowSummary() {
  using namespace rthreads;
  // 9 frames, 1 us QPC; frames 1..8 complete. Frame times alternate 10 / 20 ms; late jobs 0 / 2.
  std::vector<FrameRec> fr;
  int64_t q = 0;
  uint64_t late = 0;
  for (int i = 1; i <= 9; ++i) {
    FrameRec r;
    r.frame = static_cast<uint64_t>(i);
    r.qpc = q;
    r.lateCum = late;
    r.waitNsCum = static_cast<uint64_t>(i) * 1000000;  // 1 ms recorder wait per frame
    r.cyc = static_cast<uint64_t>(i) * 100000000;
    r.qpcXr = q + 8000;  // pass region 8 ms
    r.cycXr = r.cyc + 7000000;  // 7 ms of cycles at 1 GHz
    fr.push_back(r);
    const bool heavy = i % 2 == 0;
    q += heavy ? 20000 : 10000;
    late += heavy ? 2 : 0;
  }
  std::vector<SampleHdr> sm;
  auto add = [&](uint64_t f, uint8_t point, uint16_t runnable) {
    SampleHdr h;
    h.frame = f;
    h.point = point;
    h.runnable = runnable;
    h.running = 16;
    h.costTicks = 300;
    sm.push_back(h);
  };
  add(2, kEntry, 3);  // heavy
  add(4, kEntry, 1);  // heavy
  add(6, kEntry, 4);  // heavy
  add(3, kEntry, 0);  // light
  add(5, kJob, 2);
  Summary s = Summarize(fr, sm, 1.0, 1e9);
  Check(s.valid && s.frames == 8 && Near(s.frameMs, 15.0) && Near(s.latePerFrame, 1.0),
        "runnable threads: frames, mean frame time and late jobs per frame");
  Check(s.entry.n == 4 && s.job.n == 1 && Near(s.entry.share2, 50.0) && Near(s.entry.runnable, 2.0) &&
            Near(s.entry.max, 4) && Near(s.entry.costUs, 300),
        "runnable threads: entry and job-start sample statistics");
  Check(s.heavyN == 3 && Near(s.heavyShare2, 200.0 / 3.0), "runnable threads: heavy frames (>= median) with >= 2 waiting");
  Check(Near(s.runShare, 87.5) && Near(s.runShareExWait, 100.0) && Near(s.recWaitMs, 1.0) && Near(s.passRegionMs, 8.0),
        "runnable threads: pass-region run share, with and without the recorder wait");
  Check(s.gate, "runnable threads: gate passes (heavy share > 30%)");
  Check(s.bins[2].n == 1 && Near(s.bins[2].late, 2.0) && s.bins[0].n == 1 && Near(s.bins[0].late, 0.0) &&
            s.corrLate > 0.5,
        "runnable threads: bins by waiting threads and correlation with late jobs");
  std::vector<SampleHdr> calm = {sm[1], sm[3]};
  for (FrameRec& r : fr) r.cycXr = r.cyc + 7800000;
  Summary c = Summarize(fr, calm, 1.0, 1e9);
  Check(!c.gate && Near(c.heavyShare2, 0.0), "runnable threads: gate fails with few waiting threads and run share 97.5%");
}

void LiveSession() {
  using namespace rthreads;
  // This thread plays the render thread: OnEntry every "frame", snapshots every 2nd.
  ptiming::g_topTid = GetCurrentThreadId();
  if (g_qpcToUs <= 0) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcToUs = 1e6 / static_cast<double>(f.QuadPart);
  }
  if (!Begin(2, 1e9)) {
    Check(false, "runnable threads: session begins");
    return;
  }
  const Window w = Open();
  for (int i = 0; i < 12; ++i) {
    OnRenderEntry(nullptr, nullptr);  // RenderGraph::render entry (no xrEndFrame hook: every call)
    OnXrEndEnter(GetCurrentThreadId());
    if (void (*hook)() = defrec::g_jobStartHook.load()) hook();  // a recorder job start
    Sleep(2);
  }
  OnEntry(Qpc());
  const Result r = Close(w);
  End();
  int entries = 0, jobs = 0;
  bool selfSeen = false;
  for (const SampleHdr& h : r.samples) {
    (h.point == kEntry ? entries : jobs) += 1;
    for (uint32_t k = 0; k < h.count; ++k)
      if (g_thr[h.first + k].tid == GetCurrentThreadId() && IsRunning(g_thr[h.first + k].state)) selfSeen = true;
  }
  Check(entries == 6 && jobs == 6 && selfSeen, "runnable threads: live snapshots at entry and job start (self running)");
  bool roleSeen = false;
  for (const ThreadRow& t : r.threads)
    if (t.tid == GetCurrentThreadId() && t.role == "render thread") roleSeen = true;
  Check(r.sum.valid && r.sum.frames == 12 && r.sum.runShare > 0 && roleSeen && !defrec::g_jobStartHook.load(),
        "runnable threads: live window summary, thread roles, hook removed at the end");
  ptiming::g_topTid = 0;
}

void LiveDrain() {
  using namespace fstart;
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                               nullptr, &ctx)) &&
      FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &ctx))) {
    Check(false, "frame start: create a D3D11 device");
    return;
  }
  // The probes on this thread as the render thread (no session: state set directly).
  g_dev = dev;
  g_ctx = ctx;
  g_recs.clear();
  g_rt = GetCurrentThreadId();
  g_on = true;
  const DWORD me = GetCurrentThreadId();
  for (int f = 0; f < 3; ++f) {
    OnXrEndEnter(me);
    ctx->Flush();  // as the runtime's submit inside xrEndFrame
    if (f == 2) {  // the GPU finishes inside "xrEndFrame" (a flushing wait, as a drained frame)
      BOOL done = FALSE;
      // Deadline, not a spin count: a loaded GPU can take longer than 400 ms.
      const ULONGLONG end = GetTickCount64() + 30000;
      while (ctx->GetData(g_ev[g_evIdx], &done, sizeof(done), 0) != S_OK && GetTickCount64() < end) Sleep(1);
    }
    OnXrEndReturn(me);
    const int64_t c0 = Qpc();
    OnAggregated(c0, true);  // ctiming::HookAggregated: culling start, end
    OnAggregated(Qpc(), false);
    OnTopPass(Qpc(), 10 + f);
    OnTopPass(Qpc(), 99);  // later top-level passes of the frame are ignored
  }
  g_on = false;
  Check(g_recs.size() == 3 && g_recs[0].serial == 10 && g_recs[2].serial == 12,
        "frame start: one record per frame at the first pass after xrEndFrame");
  bool ordered = true, drained = true;
  for (const Rec& r : g_recs) {
    ordered = ordered && r.xe0 <= r.xe1 && r.xe1 <= r.c0 && r.c0 <= r.c1 && r.c1 <= r.p0 && r.collectN == 1;
    drained = drained && r.drain >= 0 && r.drain <= 5;
  }
  Check(ordered && g_evReady.load(), "frame start: probe times in order, event queries created on the thread");
  Check(drained && g_recs[2].drain == 0, "frame start: a flushed, idle device is drained at xrEndFrame return");
  // EnqueueSetEvent: the static IDXGIDevice2 table hooked, counted inside xrEndFrame, restored.
  g_on = true;
  InstallDriver(dev);
  IDXGIDevice2* dx = nullptr;
  bool counted = false, restored = false;
  if (g_enqSlot && SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice2), reinterpret_cast<void**>(&dx)))) {
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OnXrEndEnter(me);
    dx->EnqueueSetEvent(ev);
    counted = g_cur.enqN == 1 && WaitForSingleObject(ev, 30000) == WAIT_OBJECT_0;
    OnXrEndReturn(me);
    UninstallDriver();
    restored = (*reinterpret_cast<void***>(dx))[16] == reinterpret_cast<void*>(g_enqOrig);
    CloseHandle(ev);
    dx->Release();
  } else {
    UninstallDriver();
  }
  g_on = false;
  Check(counted && restored, "frame start: EnqueueSetEvent timed inside xrEndFrame, table restored");
  ReleaseQueries();
  g_recs.clear();
  g_dev = nullptr;
  g_ctx = nullptr;
  g_rt = 0;
  ctx->Release();
  dev->Release();
}

void Run() {
  Classes();
  Frames();
  Walk();
  WindowSummary();
  LiveSession();
  LiveDrain();
}

}  // namespace fsttest
