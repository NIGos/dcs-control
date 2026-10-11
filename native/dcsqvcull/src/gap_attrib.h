// Between-pass GPU gap attribution ([Suite] GapAttrib, default 0; R26 §9).
// Measure only: nothing DCS issues is skipped, reordered or waited for, except
// the clock calibration below (one drain every CalEvery frames, those frames
// and the next two are left out of the attribution).
//
// Runs gpu_pass_timing.h in full mode (not light) and adds, while g_on:
//  - the CPU QPC at which each GPU timestamp was issued (just before its End);
//  - a timestamp pair around every context op issued on the render thread
//    outside any pass (copies, resolves, updates, clears, mips, dispatches:
//    the slots gpu_pass_timing.h hooks), so their GPU time is known;
//  - a D3D11_QUERY_PIPELINE_STATISTICS pair over every gap between two
//    top-level passes (draws / dispatches nobody hooked show up as IA, VS, PS
//    or CS invocations);
//  - clock calibration: every CalEvery frames, at the frame's first top-level
//    pass, an event query is ended and flushed and the render thread spins
//    until the GPU has drained (DONOTFLUSH polls, 50 ms cap), then a
//    timestamp is ended and flushed. The idle GPU runs it within the Flush
//    window [c0, c1], so GPU ms - QPC ms = offset +- (c1 - c0) / 2 plus the
//    submit latency. A line fitted over the samples gives offset and drift.
//    Without samples the causality bound is used (min over all stamps of GPU
//    time - issue time).
//
// Per boundary (top-level pass A ends -> top-level pass B begins, same
// frame), with tB = B's begin stamp issue time on the GPU clock:
//  gap    = GPU(B begin) - GPU(A end);
//  ops    = GPU time inside the bracketed outside-pass ops between them;
//  starve = the part of [GPU(A end), min(tB, GPU(B begin))] not covered by
//           those ops: the GPU had finished A before the CPU even issued B,
//           and had nothing else (true idle, GPU waits for the CPU);
//  rest   = gap - ops - starve: unbracketed work (draws, ExecuteCommandList,
//           other processes' GPU work time-sliced in), driver submission
//           delay, or the timestamps' own drain/refill;
//  lag    = GPU(B begin) - tB: how far the GPU runs behind the CPU there.
//
// Included once from main.cpp after gpu_pass_timing.h.
#pragma once

namespace gapat {

std::atomic<bool> g_on{false};
int g_calEvery = 40;
std::wstring g_dumpPath;  // [Suite] GapAttribDump: top-level pass intervals on the QPC clock (CSV), empty = off
thread_local bool t_opOpen = false;
thread_local int t_op = 0;
thread_local void* t_caller = nullptr;
HMODULE g_self = nullptr;
// Caller ids (two return addresses outside this module) -> index stored in the kOpEnd stamp's pass field.
struct Caller {
  void* a = nullptr;
  void* b = nullptr;
};
std::mutex g_callMutex;
std::vector<Caller> g_callers;  // index + 1 = id (render thread appends)
ID3D11Query* g_evq = nullptr;  // calibration drain event (render thread)
bool g_noGapStats = false;

struct Stats {
  uint64_t calDone, calTimeout, gapFull, gapLate, gapRead, opBr;
  double calDrainMs;
};
Stats g_st{};

// ---------------------------------------------------------------------------
// Render-thread hooks (called from gpu_pass_timing.h while g_on)
// ---------------------------------------------------------------------------

bool OpBegin(int op) {
  if (t_opOpen || gpt::g_rOpen < 0 || !gpt::g_active.load(std::memory_order_relaxed) ||
      GetCurrentThreadId() != gpt::g_rt.load(std::memory_order_relaxed))
    return false;
  gpt::Stamp(gpt::kOpBegin, reinterpret_cast<void*>(static_cast<intptr_t>(op)), 0);
  t_opOpen = true;
  t_op = op;
  // Who issued it: the first two return addresses outside this module.
  void* fr[12] = {};
  const USHORT n = RtlCaptureStackBackTrace(1, 12, fr, nullptr);
  Caller c;
  for (USHORT i = 0; i < n; ++i) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(fr[i]), &m);
    if (m == g_self) continue;
    if (!c.a)
      c.a = fr[i];
    else {
      c.b = fr[i];
      break;
    }
  }
  intptr_t id = 0;
  {
    std::lock_guard<std::mutex> lock(g_callMutex);
    for (size_t i = 0; i < g_callers.size(); ++i)
      if (g_callers[i].a == c.a && g_callers[i].b == c.b) id = static_cast<intptr_t>(i + 1);
    if (!id && g_callers.size() < 4096) {
      g_callers.push_back(c);
      id = static_cast<intptr_t>(g_callers.size());
    }
  }
  t_caller = reinterpret_cast<void*>(id);
  ++g_st.opBr;
  return true;
}

void OpEnd() {
  if (!t_opOpen) return;
  t_opOpen = false;
  gpt::Stamp(gpt::kOpEnd, t_caller, 0);  // the caller id (kOpBegin holds the op)
}

void OnOpen(gpt::FrameSlot& f) {
  f.ng = 0;
  f.gOpen = false;
  f.calAfter = 0;
  if (!g_noGapStats && !f.gq[0]) {
    D3D11_QUERY_DESC pd{D3D11_QUERY_PIPELINE_STATISTICS, 0};
    for (int i = 0; i < gpt::kMaxStats; ++i)
      if (!f.gq[i] && FAILED(gpt::g_dev->CreateQuery(&pd, &f.gq[i]))) {
        f.gq[i] = nullptr;
        g_noGapStats = true;
        break;
      }
  }
  if (!g_evq) {
    D3D11_QUERY_DESC ed{D3D11_QUERY_EVENT, 0};
    if (FAILED(gpt::g_dev->CreateQuery(&ed, &g_evq))) g_evq = nullptr;
  }
}

void EndGap(gpt::FrameSlot& f) {
  if (!f.gOpen) return;
  gpt::g_ctx->End(f.gq[f.ng]);
  ++f.ng;
  f.gOpen = false;
}

void OnClose(gpt::FrameSlot& f) { EndGap(f); }

void Calibrate(gpt::FrameSlot& f) {
  ID3D11DeviceContext* ctx = gpt::g_ctx;
  ctx->End(g_evq);
  ctx->Flush();
  LARGE_INTEGER a, b;
  QueryPerformanceCounter(&a);
  bool ok = false;
  for (;;) {
    BOOL x = FALSE;
    if (ctx->GetData(g_evq, &x, sizeof(x), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
      ok = true;
      break;
    }
    QueryPerformanceCounter(&b);
    if ((b.QuadPart - a.QuadPart) * g_qpcToUs > 50000.0) break;
    YieldProcessor();
  }
  QueryPerformanceCounter(&b);
  g_st.calDrainMs += (b.QuadPart - a.QuadPart) * g_qpcToUs / 1000.0;
  if (!ok) {
    ++g_st.calTimeout;
    return;
  }
  gpt::Stamp(gpt::kCal, nullptr, 0);  // its cpu[] entry is c0
  ctx->Flush();
  QueryPerformanceCounter(&b);
  f.calAfter = b.QuadPart;  // c1
  ++g_st.calDone;
}

void OnTopBegin() {
  if (gpt::g_rOpen < 0) return;
  gpt::FrameSlot& f = gpt::g_ring[gpt::g_rOpen];
  EndGap(f);
  if (f.n == 0 && g_evq && g_calEvery > 0 && f.serial % static_cast<uint64_t>(g_calEvery) ==
                                                  static_cast<uint64_t>(g_calEvery / 2))
    Calibrate(f);
}

void OnTopEnd() {
  if (gpt::g_rOpen < 0) return;
  gpt::FrameSlot& f = gpt::g_ring[gpt::g_rOpen];
  if (f.gOpen || g_noGapStats || f.n == 0) return;
  if (f.ng >= gpt::kMaxStats || !f.gq[f.ng]) {
    ++g_st.gapFull;
    return;
  }
  gpt::g_ctx->Begin(f.gq[f.ng]);
  f.gEv[f.ng] = f.n - 1;  // the end stamp just issued
  f.gOpen = true;
}

void OnRead(gpt::FrameSlot& f, gpt::Done& d) {
  d.cpu.assign(f.cpu, f.cpu + f.n);
  d.calAfter = f.calAfter;
  for (int i = 0; i < f.ng; ++i) {
    D3D11_QUERY_DATA_PIPELINE_STATISTICS p{};
    if (gpt::g_ctx->GetData(f.gq[i], &p, sizeof(p), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
      d.gapStats.clear();
      d.gapEv.clear();
      ++g_st.gapLate;
      return;
    }
    d.gapStats.push_back({nullptr, p.IAPrimitives, p.VSInvocations, p.CPrimitives, p.PSInvocations, p.CSInvocations, 0});
    d.gapEv.push_back(f.gEv[i]);
  }
  if (f.ng) ++g_st.gapRead;
}

// ---------------------------------------------------------------------------
// Analysis (suite thread, offline tested)
// ---------------------------------------------------------------------------

struct Cal {
  int n = 0;             // calibration samples
  bool fromCal = false;  // false: causality bound
  double a = 0, b = 0;   // offset(ms) = a + b * (cpuMs - c0Ms)
  double c0Ms = 0;
  double halfMs = 0;     // mean (c1 - c0) / 2
  double residMs = 0;    // max |sample - fit|
  double minLagMs = 0;   // min over all stamps of GPU - (issue + offset); < 0 = calibration too late
  double Off(double cpuMs) const { return a + b * (cpuMs - c0Ms); }
};

struct Agg {
  uint64_t n = 0;
  double gap = 0, ops = 0, starve = 0, rest = 0, cpuGap = 0;
  std::vector<double> lag;
  uint64_t opN[gpt::kOpCount] = {};
  double opMs[gpt::kOpCount] = {};
  uint64_t nSt = 0;
  gpt::PassStats st;
  std::map<intptr_t, std::pair<uint64_t, double>> callers;  // caller id -> ops, GPU ms
};

struct Result {
  int frames = 0;      // frames attributed
  int excluded = 0;    // calibration frames and the two after each
  Cal cal;
  std::map<std::string, Agg> by;
  std::vector<double> fGap, fOps, fStarve, fRest, fRestLowLag;  // per frame sums
  std::vector<double> lagAll;                                    // at every top-level begin with a gap
  double opMs[gpt::kOpCount] = {};
  uint64_t opN[gpt::kOpCount] = {};
  uint64_t boundaries = 0;
  std::map<std::pair<intptr_t, int>, std::pair<uint64_t, double>> callers;  // (caller id, op) -> ops, GPU ms
};

constexpr double kLowLagMs = 0.5;

inline double GpuMs(const gpt::Done& d, size_t i) {
  return static_cast<double>(d.ticks[i]) * 1000.0 / static_cast<double>(d.freq);
}

Cal Calibrate(const std::vector<gpt::Done>& done, double qpcFreq) {
  Cal c;
  std::vector<double> xs, ys, hs;
  for (const gpt::Done& d : done) {
    if (!d.freq || d.cpu.size() != d.ev.size() || !d.calAfter) continue;
    for (size_t i = 0; i < d.ev.size(); ++i)
      if (d.ev[i].type == gpt::kCal) {
        const double c0 = d.cpu[i] * 1000.0 / qpcFreq, c1 = d.calAfter * 1000.0 / qpcFreq;
        xs.push_back(c0);
        ys.push_back(GpuMs(d, i) - 0.5 * (c0 + c1));
        hs.push_back(0.5 * (c1 - c0));
      }
  }
  c.n = static_cast<int>(xs.size());
  if (c.n) {
    c.fromCal = true;
    c.c0Ms = xs[0];
    double mx = 0, my = 0;
    for (int i = 0; i < c.n; ++i) mx += xs[i] - c.c0Ms, my += ys[i];
    mx /= c.n, my /= c.n;
    double sxx = 0, sxy = 0;
    for (int i = 0; i < c.n; ++i) {
      const double dx = xs[i] - c.c0Ms - mx;
      sxx += dx * dx, sxy += dx * (ys[i] - my);
    }
    c.b = c.n >= 3 && sxx > 0 ? sxy / sxx : 0.0;
    c.a = my - c.b * mx;
    for (int i = 0; i < c.n; ++i) {
      c.halfMs += hs[i] / c.n;
      c.residMs = std::max(c.residMs, std::fabs(ys[i] - c.Off(xs[i])));
    }
  }
  // Causality: a stamp cannot run before it was issued.
  double minLag = 1e300, minDiff = 1e300;
  for (const gpt::Done& d : done) {
    if (!d.freq || d.cpu.size() != d.ev.size()) continue;
    for (size_t i = 0; i < d.ev.size(); ++i) {
      const double cm = d.cpu[i] * 1000.0 / qpcFreq;
      minDiff = std::min(minDiff, GpuMs(d, i) - cm);
      if (c.fromCal) minLag = std::min(minLag, GpuMs(d, i) - (cm + c.Off(cm)));
    }
  }
  if (!c.fromCal && minDiff < 1e299) c.a = minDiff;
  c.minLagMs = c.fromCal ? (minLag < 1e299 ? minLag : 0.0) : 0.0;
  return c;
}

Result Analyze(std::vector<gpt::Done>& done, std::string (*nameOf)(void*), double qpcFreq) {
  Result r;
  std::sort(done.begin(), done.end(), [](const gpt::Done& a, const gpt::Done& b) { return a.serial < b.serial; });
  r.cal = Calibrate(done, qpcFreq);
  std::unordered_set<uint64_t> skip;
  for (const gpt::Done& d : done)
    for (const gpt::Event& e : d.ev)
      if (e.type == gpt::kCal) {
        skip.insert(d.serial);
        skip.insert(d.serial + 1);
        skip.insert(d.serial + 2);
      }
  std::unordered_map<void*, std::string> names;
  auto name = [&](void* p) -> const std::string& {
    auto it = names.find(p);
    if (it == names.end()) it = names.emplace(p, nameOf(p)).first;
    return it->second;
  };
  for (const gpt::Done& d : done) {
    if (!d.freq || d.cpu.size() != d.ev.size() || d.ticks.size() != d.ev.size()) continue;
    if (skip.count(d.serial)) {
      ++r.excluded;
      continue;
    }
    ++r.frames;
    double fg = 0, fo = 0, fs = 0, fr = 0, frl = 0;
    int depth = 0;
    long lastTopEnd = -1;
    std::map<std::string, int> occ;
    std::string lastKey;
    for (size_t i = 0; i < d.ev.size(); ++i) {
      const gpt::Event& e = d.ev[i];
      if (e.type == gpt::kBegin) {
        if (depth++ != 0) continue;
        const std::string& nm = name(e.pass);
        const std::string key = nm + "#" + std::to_string(occ[nm]++);
        if (lastTopEnd >= 0) {
          const size_t ia = static_cast<size_t>(lastTopEnd);
          const double gA = GpuMs(d, ia), gB = GpuMs(d, i);
          const double cA = d.cpu[ia] * 1000.0 / qpcFreq, cB = d.cpu[i] * 1000.0 / qpcFreq;
          const double tB = cB + r.cal.Off(cB);
          const double gap = std::max(0.0, gB - gA);
          const double w = std::min(tB, gB);
          double ops = 0, opsIn = 0;
          Agg& g = r.by[lastKey + " > " + key];
          for (size_t j = ia + 1; j < i; ++j) {
            if (d.ev[j].type != gpt::kOpBegin || j + 1 >= i || d.ev[j + 1].type != gpt::kOpEnd) continue;
            const double ob = GpuMs(d, j), oe = GpuMs(d, j + 1);
            const double dur = std::max(0.0, oe - ob);
            const int op = static_cast<int>(reinterpret_cast<intptr_t>(d.ev[j].pass));
            ops += dur;
            if (op >= 0 && op < gpt::kOpCount) {
              ++g.opN[op], g.opMs[op] += dur;
              ++r.opN[op], r.opMs[op] += dur;
            }
            const intptr_t cid = reinterpret_cast<intptr_t>(d.ev[j + 1].pass);
            auto& gc = g.callers[cid];
            ++gc.first, gc.second += dur;
            auto& rc = r.callers[{cid, op}];
            ++rc.first, rc.second += dur;
            const double lo = std::max(ob, gA), hi = std::min(oe, w);
            if (hi > lo) opsIn += hi - lo;
          }
          ops = std::min(ops, gap);
          const double starve = w > gA ? std::max(0.0, (w - gA) - opsIn) : 0.0;
          const double st = std::min(starve, gap - ops);
          const double rest = std::max(0.0, gap - ops - st);
          const double lag = gB - tB;
          ++g.n;
          g.gap += gap, g.ops += ops, g.starve += st, g.rest += rest, g.cpuGap += cB - cA;
          g.lag.push_back(lag);
          for (size_t k = 0; k < d.gapEv.size() && k < d.gapStats.size(); ++k)
            if (d.gapEv[k] == static_cast<int>(ia)) {
              const gpt::PassStats& p = d.gapStats[k];
              ++g.nSt;
              g.st.ia += p.ia, g.st.vs += p.vs, g.st.cPrims += p.cPrims, g.st.ps += p.ps, g.st.cs += p.cs;
              break;
            }
          r.lagAll.push_back(lag);
          ++r.boundaries;
          fg += gap, fo += ops, fs += st, fr += rest;
          if (lag < kLowLagMs) frl += rest;
        }
        lastKey = key;
      } else if (e.type == gpt::kEnd) {
        if (depth > 0 && --depth == 0) lastTopEnd = static_cast<long>(i);
      }
    }
    r.fGap.push_back(fg), r.fOps.push_back(fo), r.fStarve.push_back(fs), r.fRest.push_back(fr);
    r.fRestLowLag.push_back(frl);
  }
  return r;
}

double Pct(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(v.size() * p))];
}

void Report(const Result& r) {
  const double f = std::max(1, r.frames);
  Log("  gap attribution: %d frames attributed, %d left out (calibration and the 2 frames after); calibrations %llu "
      "done, %llu timed out, drain wait %.2f ms each; outside-pass ops bracketed %.1f/frame; gap statistics read "
      "%llu frames, late %llu, over the limit %llu",
      r.frames, r.excluded, static_cast<unsigned long long>(g_st.calDone),
      static_cast<unsigned long long>(g_st.calTimeout), g_st.calDone ? g_st.calDrainMs / g_st.calDone : 0.0,
      g_st.opBr / std::max(1.0, f + r.excluded), static_cast<unsigned long long>(g_st.gapRead),
      static_cast<unsigned long long>(g_st.gapLate), static_cast<unsigned long long>(g_st.gapFull));
  if (r.cal.fromCal)
    Log("  clock: %d calibration samples, GPU - QPC offset fitted, drift %.2f ppm, Flush window +-%.3f ms, max "
        "residual %.3f ms; causality check: min(GPU - issue) over all stamps %.3f ms (>= -window = consistent)",
        r.cal.n, r.cal.b * 1e6, r.cal.halfMs, r.cal.residMs, r.cal.minLagMs);
  else
    Log("  clock: no calibration sample; offset = causality bound (min GPU - issue over all stamps), so starvation "
        "is a lower bound");
  if (!r.frames) return;
  auto mean = [](const std::vector<double>& v) {
    double s = 0;
    for (double x : v) s += x;
    return v.empty() ? 0.0 : s / v.size();
  };
  Log("  between top-level passes per frame: gap %.3f ms (p95 %.3f) = bracketed outside-pass ops %.3f + GPU starved "
      "(finished the previous pass before the CPU issued the next) %.3f + rest %.3f (of which at a GPU lag < %.1f ms: "
      "%.3f); %.1f boundaries/frame",
      mean(r.fGap), Pct(r.fGap, 0.95), mean(r.fOps), mean(r.fStarve), mean(r.fRest), kLowLagMs, mean(r.fRestLowLag),
      r.boundaries / f);
  Log("  GPU lag behind the CPU at top-level pass begins (GPU start - issue): p1 %.3f, p5 %.3f, p50 %.3f, p95 %.3f ms",
      Pct(r.lagAll, 0.01), Pct(r.lagAll, 0.05), Pct(r.lagAll, 0.5), Pct(r.lagAll, 0.95));
  {
    std::string line;
    char buf[64];
    for (int i = 0; i < gpt::kOpCount; ++i)
      if (r.opN[i]) {
        snprintf(buf, sizeof(buf), " %s %.1f/frame %.3f ms;", gpt::kOpShort[i], r.opN[i] / f, r.opMs[i] / f);
        line += buf;
      }
    Log("  outside-pass ops between top-level passes (GPU time inside their brackets):%s", line.c_str());
  }
  {
    std::vector<std::pair<double, std::pair<intptr_t, int>>> cs;
    for (auto& kv : r.callers) cs.push_back({kv.second.second, kv.first});
    std::sort(cs.begin(), cs.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    Log("  outside-pass ops by issuer (the first two return addresses outside DcsQvCull):");
    for (size_t i = 0; i < cs.size() && i < 16; ++i) {
      const auto& v = r.callers.at(cs[i].second);
      char a[200] = "?", b[200] = "?";
      Caller c;
      {
        std::lock_guard<std::mutex> lock(g_callMutex);
        const intptr_t id = cs[i].second.first;
        if (id > 0 && static_cast<size_t>(id) <= g_callers.size()) c = g_callers[id - 1];
      }
      if (!gpt::ModRva(c.a, a, sizeof(a))) snprintf(a, sizeof(a), "%p", c.a);
      if (!gpt::ModRva(c.b, b, sizeof(b))) snprintf(b, sizeof(b), "%p", c.b);
      const int op = cs[i].second.second;
      Log("    %-8s %6.1f/frame %7.3f ms/frame  %s <- %s", op >= 0 && op < gpt::kOpCount ? gpt::kOpShort[op] : "?",
          v.first / f, v.second / f, a, b);
    }
  }
  struct Row {
    const std::string* k;
    const Agg* a;
  };
  std::vector<Row> rows;
  for (auto& kv : r.by) rows.push_back({&kv.first, &kv.second});
  std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.a->gap > y.a->gap; });
  Log("  boundaries by gap (ms per frame; lag = median GPU lag at the next pass; stats = millions per gap):");
  Log("    %-62s %6s %7s %7s %7s %7s %7s %7s  %s", "previous > next", "n/fr", "gap", "ops", "starve", "rest", "cpuGap",
      "lag", "ops and gap statistics");
  for (size_t i = 0; i < rows.size() && i < 30; ++i) {
    const Agg& a = *rows[i].a;
    const double n = static_cast<double>(a.n);
    std::string extra;
    char buf[96];
    for (int o = 0; o < gpt::kOpCount; ++o)
      if (a.opN[o]) {
        snprintf(buf, sizeof(buf), " %s %.1f/%.3f", gpt::kOpShort[o], a.opN[o] / n, a.opMs[o] / n);
        extra += buf;
      }
    if (a.nSt) {
      const double s = static_cast<double>(a.nSt), M = 1e6;
      snprintf(buf, sizeof(buf), " | IA %.3f VS %.3f PS %.3f CS %.3f", a.st.ia / s / M, a.st.vs / s / M,
               a.st.ps / s / M, a.st.cs / s / M);
      extra += buf;
    }
    Log("    %-62.62s %6.2f %7.3f %7.3f %7.3f %7.3f %7.3f %7.3f %s", rows[i].k->c_str(), n / f, a.gap / f, a.ops / f,
        a.starve / f, a.rest / f, a.cpuGap / n, Pct(a.lag, 0.5), extra.c_str());
  }
}

// CSV of every attributed frame's top-level passes and outside-pass ops on the
// QPC clock (ns), for correlation with other QPC-based traces (Nsight Systems'
// systemClockNs), plus the calibration stamps (drain markers).
void Dump(const std::vector<gpt::Done>& done, const Cal& cal, double qpcFreq, std::string (*nameOf)(void*)) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, g_dumpPath.c_str(), L"w") || !f) {
    Log("  gap attribution: could not write %ls", g_dumpPath.c_str());
    return;
  }
  fprintf(f, "serial,kind,depth,start_qpc_ns,end_qpc_ns\n");
  auto toNs = [&](double gpuMs) {
    const double q = gpuMs - cal.Off(gpuMs - cal.a);
    return static_cast<long long>(std::llround(q * 1e6));
  };
  (void)qpcFreq;
  size_t rows = 0;
  for (const gpt::Done& d : done) {
    if (!d.freq || d.ticks.size() != d.ev.size()) continue;
    struct O {
      void* p;
      size_t i;
    };
    std::vector<O> st;
    std::map<std::string, int> occ;
    for (size_t i = 0; i < d.ev.size(); ++i) {
      const gpt::Event& e = d.ev[i];
      switch (e.type) {
        case gpt::kBegin:
          st.push_back({e.pass, i});
          break;
        case gpt::kEnd:
          if (!st.empty() && st.back().p == e.pass) {
            const O o = st.back();
            st.pop_back();
            if (st.size() <= 1) {
              const std::string nm = nameOf(e.pass);
              const int k = st.empty() ? occ[nm]++ : -1;
              fprintf(f, "%llu,%s#%d,%zu,%lld,%lld\n", static_cast<unsigned long long>(d.serial), nm.c_str(), k,
                      st.size(), toNs(GpuMs(d, o.i)), toNs(GpuMs(d, i)));
              ++rows;
            }
          }
          break;
        case gpt::kOpBegin:
          if (i + 1 < d.ev.size() && d.ev[i + 1].type == gpt::kOpEnd) {
            const int op = static_cast<int>(reinterpret_cast<intptr_t>(e.pass));
            fprintf(f, "%llu,op:%s,0,%lld,%lld\n", static_cast<unsigned long long>(d.serial),
                    op >= 0 && op < gpt::kOpCount ? gpt::kOpShort[op] : "?", toNs(GpuMs(d, i)), toNs(GpuMs(d, i + 1)));
            ++rows;
          }
          break;
        case gpt::kCal:
          fprintf(f, "%llu,calibration,0,%lld,%lld\n", static_cast<unsigned long long>(d.serial), toNs(GpuMs(d, i)),
                  toNs(GpuMs(d, i)));
          ++rows;
          break;
        case gpt::kXrBegin:
        case gpt::kXrEnd:
          fprintf(f, "%llu,%s,0,%lld,%lld\n", static_cast<unsigned long long>(d.serial),
                  e.type == gpt::kXrBegin ? "xrEndFrame_begin" : "xrEndFrame_end", toNs(GpuMs(d, i)), toNs(GpuMs(d, i)));
          ++rows;
          break;
      }
    }
  }
  fclose(f);
  Log("  gap attribution: %zu rows written to %ls (QPC ns = QPC ticks x %.0f)", rows, g_dumpPath.c_str(),
      1e9 / qpcFreq);
}

// Suite phase: gpu_pass_timing.h full mode plus the above, then both reports.
void Measure(int ms, int calEvery, std::atomic<uint64_t>& frameCounter) {
  g_st = {};
  if (!g_self)
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&OpBegin), &g_self);
  g_calEvery = calEvery;
  g_noGapStats = false;
  g_on = true;
  if (!gpt::Begin(frameCounter, false) && !gpt::g_sessDev) {
    g_on = false;
    return;
  }
  ptiming::Reset();
  ptiming::g_recording = true;
  const uint64_t f0 = frameCounter.load();
  if (gpt::g_active.load()) Sleep(ms);
  ptiming::g_recording = false;
  const uint64_t frames = frameCounter.load() - f0;
  gpt::End();
  g_on = false;
  if (g_evq) {
    // The render thread no longer uses it (deactivated). On a single-threaded
    // device no other thread may call into it (as frame_start.h): left alive.
    if (!shadowinst::g_deviceSingleThreaded) g_evq->Release();
    g_evq = nullptr;
  }
  std::vector<gpt::Done> done = gpt::TakeDone();
  std::vector<gpt::Done> copy = done;
  Result r = Analyze(copy, [](void* p) -> std::string { return ptiming::RttiName(p); },
                     1e6 / g_qpcToUs);
  if (!g_dumpPath.empty())
    Dump(copy, r.cal, 1e6 / g_qpcToUs, [](void* p) -> std::string { return ptiming::RttiName(p); });
  {
    std::lock_guard<std::mutex> lock(gpt::g_doneMutex);
    gpt::g_done = std::move(done);
  }
  std::map<std::string, ptiming::Stat> cpu = ptiming::TakeByName(nullptr, nullptr);
  if (gpt::g_s.framesOpened) {
    gpt::Report(frames, cpu);
    Report(r);
  } else {
    Log("  gap attribution: no frame measured");
  }
}

}  // namespace gapat
