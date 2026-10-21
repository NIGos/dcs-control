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

