// Measurement kit for the 2026-11-10 performance audit (artifacts\analysis\fable-perf-audit.md, questions a-f;
// native\dcsqvcull\LIVE_MEASUREMENT_PLAN.md runs it in one live session). Measure only: no phase here changes what
// DCS draws, and with every key at its default (0) nothing in this file runs.
//
//   [Suite] PassTimelineSec=N   (a) N s of render-thread CPU per pass and per frame (pass_timing.h's recording plus
//                               its g_timeline list), mean / p50 / p95 per pass kind, the render thread's own CPU
//                               (QueryThreadCycleTime), and the culling timeline (collect_timing.h) over the same
//                               window; passtl_<time>.csv (frame, pass kind, exclusive ms) next to the log.
//   [Bench] PassMs=1            (b, f) every A/B block also records render-thread pass ms and G-buffer pass ms
//                               (bench.h); the render thread's CPU ms per frame is in every A/B now (two calls per
//                               block).
//   [Suite] ChainCensusSec=N    (d) R38 rank 3 decisive test, counts half: after every calccounters dispatch (an IM2
//                               culling chain) on sampled frames (1 in kCensusEvery), the per-object counts are read
//                               back without waiting; per pair of chains of one frame: are the second chain's counts
//                               <= the first's for every object (a necessary condition for "the focus list is the
//                               periphery list filtered by the focus planes")? Runs inside a GPU pass timing session,
//                               so the same log has the chains' GPU ms (TerrainModule calls #1-#4).
//   [Suite] GpuBundleTiming=M   (e) ABBA GPU pass timing, GpuBundlePairs pairs of 5 s blocks, ON = the bundle items of
//                               mask M that are built. None of the five audit items (R26 11.7a, R26 9.2b, R27 R4,
//                               R28, R36 4b) is built yet: then both arms are stock and the run is the A/A noise floor
//                               the bundle's decision rule needs. A built item registers its switch in kBundle.
// Bench modes 34 (prefetch table 2^16 vs [Prefetch] TableBits) and 35 (G-buffer recorder cockpit part) are in bench.h
// and main\bench_variant.inc; their suite keys in main\suite.inc.
//
// Included once from main.cpp (main\features.inc) after pass_timing.h, collect_timing.h, gpu_pass_timing.h and
// chain_fast.h, inside the anonymous namespace.
#pragma once

// bench.h's per-block render thread and pass recording ([Bench] PassMs) are at the end of pass_timing.h.

namespace mkit {

// ---------------------------------------------------------------------------
// (a) Pass timeline
// ---------------------------------------------------------------------------
inline double Quantile(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  size_t k = static_cast<size_t>(q * static_cast<double>(v.size()));
  if (k >= v.size()) k = v.size() - 1;
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
  return v[k];
}

struct KindRow {
  std::string name;
  double mean, p50, p95, callsPerFrame;
};

// Per-frame totals per pass kind from the timeline events (pure; offline tested). Frames are the distinct frame ids;
// the first and the last id are dropped (partial). perFrameTotal: render-thread pass execution per frame (ms).
std::vector<KindRow> TimelineRows(const std::vector<ptiming::TlEvent>& ev,
                                  const std::function<std::string(void*)>& nameOf, std::vector<double>* perFrameTotal,
                                  std::vector<std::pair<uint32_t, std::map<std::string, double>>>* frames) {
  std::set<uint32_t> ids;
  for (const ptiming::TlEvent& e : ev) ids.insert(e.frame);
  const bool trim = ids.size() > 2;
  const uint32_t firstId = ids.empty() ? 0 : *ids.begin(), lastId = ids.empty() ? 0 : *ids.rbegin();
  std::map<uint32_t, std::map<std::string, double>> byFrame;
  std::map<std::string, uint64_t> calls;
  std::unordered_map<void*, std::string> names;
  for (const ptiming::TlEvent& e : ev) {
    if (trim && (e.frame == firstId || e.frame == lastId)) continue;  // partial frames
    auto it = names.find(e.pass);
    if (it == names.end()) it = names.emplace(e.pass, nameOf(e.pass)).first;
    byFrame[e.frame][it->second] += e.exclUs / 1000.0;
    calls[it->second]++;
  }
  std::map<std::string, std::vector<double>> perKind;
  for (const auto& kv : calls) perKind[kv.first];
  if (perFrameTotal) perFrameTotal->clear();
  for (const auto& f : byFrame) {
    double tot = 0;
    for (auto& k : perKind) {
      auto it = f.second.find(k.first);
      const double ms = it == f.second.end() ? 0.0 : it->second;
      k.second.push_back(ms);
      tot += ms;
    }
    if (perFrameTotal) perFrameTotal->push_back(tot);
    if (frames) frames->emplace_back(f.first, f.second);
  }
  std::vector<KindRow> rows;
  const double nf = static_cast<double>(std::max<size_t>(1, byFrame.size()));
  for (auto& k : perKind) {
    double s = 0;
    for (double x : k.second) s += x;
    rows.push_back({k.first, s / nf, Quantile(k.second, 0.5), Quantile(k.second, 0.95),
                    static_cast<double>(calls[k.first]) / nf});
  }
  std::sort(rows.begin(), rows.end(), [](const KindRow& a, const KindRow& b) { return a.mean > b.mean; });
  return rows;
}

void PassTimelinePhase(int seconds, std::atomic<uint64_t>& frameCounter, double tscHz) {
  if (!ptiming::g_orig) {
    Log("  pass timeline: pass timing not installed; skipped");
    return;
  }
  const size_t cap = std::min<size_t>(static_cast<size_t>(seconds) * 120 * 800, 4000000);
  {
    std::lock_guard<std::mutex> lock(ptiming::g_mutex);
    ptiming::g_tl.clear();
    ptiming::g_tl.reserve(cap);
    ptiming::g_tlCap = cap;
  }
  const DWORD tid = ptiming::g_topTid.load();
  HANDLE rt = tid ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid) : nullptr;
  ULONG64 c0 = 0, c1 = 0;
  const bool cycOk = rt && QueryThreadCycleTime(rt, &c0);
  const ctiming::Snap ca = ctiming::Take();
  LARGE_INTEGER q0, q1;
  ptiming::Reset();
  const uint64_t f0 = frameCounter.load();
  QueryPerformanceCounter(&q0);
  ptiming::g_timeline = true;
  ptiming::g_recording = true;
  for (int t = 0; t < seconds * 10 && !g_benchAbort; ++t) Sleep(100);
  ptiming::g_recording = false;
  ptiming::g_timeline = false;
  QueryPerformanceCounter(&q1);
  const uint64_t frames = frameCounter.load() - f0;
  const ctiming::Snap cb = ctiming::Take();
  const bool cyc1 = cycOk && QueryThreadCycleTime(rt, &c1);
  if (rt) CloseHandle(rt);
  Sleep(50);
  std::vector<ptiming::TlEvent> ev;
  {
    std::lock_guard<std::mutex> lock(ptiming::g_mutex);
    ev.swap(ptiming::g_tl);
    ptiming::g_tlCap = 0;
  }
  ptiming::TakeByName(nullptr, nullptr);  // clears the means (the timeline has them)
  const double sec = (q1.QuadPart - q0.QuadPart) * g_qpcToUs / 1e6;
  std::vector<double> tot;
  std::vector<std::pair<uint32_t, std::map<std::string, double>>> perFrame;
  const std::vector<KindRow> rows =
      TimelineRows(ev, [](void* p) { return ptiming::RttiName(p); }, &tot, &perFrame);
  if (tot.empty() || !frames) {
    Log("  pass timeline: no frame recorded (%zu events)", ev.size());
    return;
  }
  double mean = 0;
  for (double x : tot) mean += x;
  mean /= static_cast<double>(tot.size());
  Log("  pass timeline: %zu frames (%llu quad-view frames, %.1f s, %zu events%s); render-thread pass execution mean "
      "%.3f ms, p50 %.3f, p95 %.3f, max %.3f ms/frame",
      tot.size(), static_cast<unsigned long long>(frames), sec, ev.size(), ev.size() >= cap ? ", CAP REACHED" : "",
      mean, Quantile(tot, 0.5), Quantile(tot, 0.95), *std::max_element(tot.begin(), tot.end()));
  if (cyc1 && c1 >= c0 && sec > 0)
    Log("  pass timeline: render thread CPU %.3f ms/frame (busy %.1f%% of wall time)",
        (c1 - c0) / tscHz * 1000.0 / static_cast<double>(frames), (c1 - c0) / tscHz / sec * 100.0);
  else
    Log("  pass timeline: render thread CPU not measured (thread not known)");
  Log("    %-48s %8s %8s %8s %8s", "pass kind (exclusive, render thread)", "mean ms", "p50", "p95", "calls/f");
  for (size_t i = 0; i < rows.size() && i < 40; ++i)
    Log("    pass timeline %-34.34s %8.3f %8.3f %8.3f %8.1f", rows[i].name.c_str(), rows[i].mean, rows[i].p50,
        rows[i].p95, rows[i].callsPerFrame);
  Log("  pass timeline: culling timeline over the same window:");
  ctiming::Report(ca, cb, frames, sec * 1000.0 / static_cast<double>(frames));
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t name[64];
  swprintf_s(name, L"passtl_%04d%02d%02d_%02d%02d%02d.csv", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);
  FILE* f = nullptr;
  _wfopen_s(&f, (g_dir + name).c_str(), L"w");
  if (f) {
    fprintf(f, "frame,pass_kind,excl_ms\n");
    for (const auto& fr : perFrame)
      for (const auto& kv : fr.second) fprintf(f, "%u,\"%s\",%.4f\n", fr.first, kv.first.c_str(), kv.second);
    fclose(f);
    Log("  pass timeline: per-frame table written to %ls", name);
  }
}

// ---------------------------------------------------------------------------
// (d) Culling-chain counts census
// ---------------------------------------------------------------------------
std::atomic<bool> g_census{false};
std::atomic<bool> g_censusStop{false};     // suite: release the staging buffers on the render thread, then stop
std::atomic<bool> g_censusReleased{false};
constexpr int kCensusChains = 32, kCensusSets = 3, kCensusEvery = 4, kCensusQueue = 8;
constexpr uint32_t kCensusMaxObjects = 1u << 20;
struct CensusChain {
  void* pass = nullptr;
  uint32_t nObj = 0;
  ID3D11Buffer* stage = nullptr;
};
struct CensusSet {
  uint32_t frame = 0;
  int n = 0;
  bool pending = false, overflow = false;
  CensusChain c[kCensusChains];
};
CensusSet g_cs[kCensusSets];  // render thread while g_census
int g_csCur = -1;
uint64_t g_csFrame = ~0ull;
struct CensusFrame {
  uint32_t frame = 0;
  bool overflow = false;
  std::vector<void*> pass;
  std::vector<std::vector<uint32_t>> counts;
};
std::mutex g_cfMu;
std::vector<CensusFrame> g_cfQueue;
std::atomic<uint64_t> g_cDispatches{0}, g_cSampled{0}, g_cSkippedBusy{0}, g_cRead{0}, g_cFail{0};

void ReleaseCensusBuffers() {
  for (CensusSet& s : g_cs) {
    for (CensusChain& c : s.c) {
      if (c.stage) c.stage->Release();
      c = CensusChain{};
    }
    s = CensusSet{};
  }
  g_csCur = -1;
  g_csFrame = ~0ull;
}

// Innermost GPU-timed pass on the render thread (gpt's counters are keyed by the pass object).
void* InnermostPass() {
  const gpt::Counts* cur = gpt::t_cur;
  if (!cur) return nullptr;
  for (auto& kv : gpt::g_passCounts)
    if (&kv.second == cur) return kv.first;
  return nullptr;
}

void PollCensus(ID3D11DeviceContext* c) {
  for (int s = 0; s < kCensusSets; ++s) {
    CensusSet& set = g_cs[s];
    if (!set.pending) continue;
    CensusFrame out;
    out.frame = set.frame;
    out.overflow = set.overflow;
    bool ready = true;
    for (int i = 0; i < set.n && ready; ++i) {
      D3D11_MAPPED_SUBRESOURCE m{};
      if (FAILED(c->Map(set.c[i].stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
        ready = false;  // the first not-ready buffer: the rest of the set later
        break;
      }
      const uint32_t* p = static_cast<const uint32_t*>(m.pData);
      out.pass.push_back(set.c[i].pass);
      out.counts.emplace_back(p, p + set.c[i].nObj);
      c->Unmap(set.c[i].stage, 0);
    }
    if (!ready) continue;
    set.pending = false;
    g_cRead.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_cfMu);
    g_cfQueue.push_back(std::move(out));
  }
}

// After DCS's DispatchIndirect (gpu_pass_timing.h's hook, render thread, only while g_census).
void CensusAfterIndirect(ID3D11DeviceContext* c) {
  if (c != gpt::g_ctx || GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed)) return;
  if (g_censusStop.load()) {
    if (!g_censusReleased.load()) {
      ReleaseCensusBuffers();
      g_censusReleased = true;
    }
    return;
  }
  PollCensus(c);
  const uint64_t frame = ptiming::g_execFrame.load(std::memory_order_relaxed);
  if (frame != g_csFrame) {
    if (g_csCur >= 0 && g_cs[g_csCur].n > 0) g_cs[g_csCur].pending = true;
    g_csCur = -1;
    g_csFrame = frame;
    if (frame % kCensusEvery == 0) {
      size_t queued = 0;
      {
        std::lock_guard<std::mutex> lock(g_cfMu);
        queued = g_cfQueue.size();
      }
      for (int s = 0; s < kCensusSets && g_csCur < 0; ++s)
        if (!g_cs[s].pending && queued < kCensusQueue) g_csCur = s;
      if (g_csCur < 0) {
        g_cSkippedBusy.fetch_add(1, std::memory_order_relaxed);
      } else {
        g_cs[g_csCur].n = 0;
        g_cs[g_csCur].overflow = false;
        g_cs[g_csCur].frame = static_cast<uint32_t>(frame);
        g_cSampled.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  if (g_csCur < 0) return;
  // Is the dispatch just issued a calccounters (stock or the ChainFast variant)?
  chainfast::Entry* e = chainfast::EntryOfBound(c);
  const bool counters = e ? e->kind == chainfast::kCalcCounters : chainfast::ClassifyBound(c, true) == chainfast::kCalcCounters;
  if (!counters) return;
  g_cDispatches.fetch_add(1, std::memory_order_relaxed);
  CensusSet& set = g_cs[g_csCur];
  if (set.n >= kCensusChains) {
    set.overflow = true;
    return;
  }
  ID3D11UnorderedAccessView* u[3] = {};
  c->CSGetUnorderedAccessViews(0, 3, u);
  ID3D11UnorderedAccessView* counts = u[2];
  bool ok = false;
  if (counts) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC vd{};
    counts->GetDesc(&vd);
    ID3D11Resource* r = nullptr;
    counts->GetResource(&r);
    const uint32_t nObj = vd.Buffer.NumElements;
    if (r && vd.ViewDimension == D3D11_UAV_DIMENSION_BUFFER && nObj > 0 && nObj <= kCensusMaxObjects) {
      CensusChain& ch = set.c[set.n];
      if (ch.stage && ch.nObj != nObj) {
        ch.stage->Release();
        ch.stage = nullptr;
      }
      if (!ch.stage) {
        ID3D11Device* d = nullptr;
        c->GetDevice(&d);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = nObj * 4;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (d) {
          if (FAILED(d->CreateBuffer(&bd, nullptr, &ch.stage))) ch.stage = nullptr;
          d->Release();
        }
      }
      if (ch.stage) {
        const D3D11_BOX box{vd.Buffer.FirstElement * 4, 0, 0, (vd.Buffer.FirstElement + nObj) * 4, 1, 1};
        c->CopySubresourceRegion(ch.stage, 0, 0, 0, 0, r, 0, &box);
        ch.nObj = nObj;
        ch.pass = InnermostPass();
        ++set.n;
        ok = true;
      }
    }
    if (r) r->Release();
  }
  for (auto* v : u)
    if (v) v->Release();
  if (!ok) g_cFail.fetch_add(1, std::memory_order_relaxed);
}

// Census statistics per chain ordinal and per ordered pair (suite thread; pure, offline tested).
struct PairStat {
  uint64_t frames = 0, subset = 0;  // frames compared; frames with counts[b] <= counts[a] for every object
  uint64_t maxObjectsOver = 0;      // most objects with counts[b] > counts[a] in one frame
  double excessSum = 0;             // sum over frames of sum(max(0, b - a))
};
struct ChainStat {
  void* pass = nullptr;
  uint32_t nObj = 0;
  uint64_t frames = 0;
  double instances = 0, nonEmpty = 0;  // sums over frames
};
struct CensusAcc {
  int chains = -1;                      // modal chain count (fixed by the first frame)
  uint64_t frames = 0, otherCount = 0, overflow = 0;
  std::vector<ChainStat> chain;
  std::map<std::pair<int, int>, PairStat> pair;  // (a, b): is b contained in a
};

void CensusAdd(CensusAcc& acc, const CensusFrame& f) {
  if (f.overflow) {
    ++acc.overflow;
    return;
  }
  const int n = static_cast<int>(f.counts.size());
  if (acc.chains < 0) {
    acc.chains = n;
    acc.chain.assign(static_cast<size_t>(n), ChainStat{});
  }
  if (n != acc.chains) {
    ++acc.otherCount;
    return;
  }
  ++acc.frames;
  for (int i = 0; i < n; ++i) {
    ChainStat& cs = acc.chain[static_cast<size_t>(i)];
    if (!cs.pass) cs.pass = f.pass[static_cast<size_t>(i)];
    cs.nObj = static_cast<uint32_t>(f.counts[static_cast<size_t>(i)].size());
    ++cs.frames;
    uint64_t inst = 0, ne = 0;
    for (uint32_t x : f.counts[static_cast<size_t>(i)]) {
      inst += x;
      ne += x != 0;
    }
    cs.instances += static_cast<double>(inst);
    cs.nonEmpty += static_cast<double>(ne);
  }
  for (int a = 0; a < n; ++a)
    for (int b = 0; b < n; ++b) {
      if (a == b) continue;
      const std::vector<uint32_t>& ca = f.counts[static_cast<size_t>(a)];
      const std::vector<uint32_t>& cb = f.counts[static_cast<size_t>(b)];
      if (ca.size() != cb.size()) continue;
      uint64_t over = 0;
      double excess = 0;
      for (size_t k = 0; k < ca.size(); ++k)
        if (cb[k] > ca[k]) {
          ++over;
          excess += cb[k] - ca[k];
        }
      PairStat& p = acc.pair[{a, b}];
      ++p.frames;
      if (!over) ++p.subset;
      p.maxObjectsOver = std::max(p.maxObjectsOver, over);
      p.excessSum += excess;
    }
}

void CensusReport(const CensusAcc& acc, const std::function<std::string(void*)>& nameOf) {
  Log("  chain census: %llu frames compared (%d calccounters chains per frame; %llu frames with another chain count, "
      "%llu over %d chains); sampled %llu, read back %llu, skipped while busy %llu, dispatches seen %llu, copy "
      "failures %llu",
      static_cast<unsigned long long>(acc.frames), acc.chains, static_cast<unsigned long long>(acc.otherCount),
      static_cast<unsigned long long>(acc.overflow), kCensusChains, static_cast<unsigned long long>(g_cSampled.load()),
      static_cast<unsigned long long>(g_cRead.load()), static_cast<unsigned long long>(g_cSkippedBusy.load()),
      static_cast<unsigned long long>(g_cDispatches.load()), static_cast<unsigned long long>(g_cFail.load()));
  if (!acc.frames) return;
  std::map<void*, int> occ;  // call number of each chain's pass object within the frame
  for (size_t i = 0; i < acc.chain.size(); ++i) {
    const ChainStat& cs = acc.chain[i];
    const double f = static_cast<double>(std::max<uint64_t>(1, cs.frames));
    const int callNo = occ[cs.pass]++;
    Log("  chain census #%zu %s call #%d: objects %u, instances %.0f/frame, non-empty objects %.0f/frame", i,
        cs.pass ? nameOf(cs.pass).c_str() : "(no timed pass)", callNo, cs.nObj, cs.instances / f, cs.nonEmpty / f);
  }
  // Every pair whose counts are contained in at least half of the frames (either direction), and every pair of
  // chains run by the same pass object (the view chains: TerrainModule calls #1-#4, R34 live).
  for (const auto& kv : acc.pair) {
    const int a = kv.first.first, b = kv.first.second;
    const PairStat& p = kv.second;
    const bool samePass = acc.chain[static_cast<size_t>(a)].pass == acc.chain[static_cast<size_t>(b)].pass;
    if (!samePass && p.subset * 2 < p.frames) continue;
    Log("  chain census pair #%d >= #%d: counts contained in %llu/%llu frames (%.1f%%), objects over max %llu, excess "
        "instances %.1f/frame%s",
        a, b, static_cast<unsigned long long>(p.subset), static_cast<unsigned long long>(p.frames),
        p.frames ? 100.0 * static_cast<double>(p.subset) / static_cast<double>(p.frames) : 0.0,
        static_cast<unsigned long long>(p.maxObjectsOver), p.frames ? p.excessSum / static_cast<double>(p.frames) : 0.0,
        samePass ? " (same pass)" : "");
  }
}

void ChainCensusPhase(int seconds, std::atomic<uint64_t>& frameCounter) {
  g_cDispatches = g_cSampled = g_cSkippedBusy = g_cRead = g_cFail = 0;
  {
    std::lock_guard<std::mutex> lock(g_cfMu);
    g_cfQueue.clear();
  }
  g_censusStop = false;
  g_censusReleased = false;
  if (!gpt::Begin(frameCounter, false)) {
    if (gpt::g_sessDev) gpt::End();
    Log("  chain census: GPU pass timing could not start; skipped");
    return;
  }
  ptiming::Reset();
  ptiming::g_recording = true;
  const uint64_t f0 = frameCounter.load();
  g_census = true;
  CensusAcc acc;
  auto drain = [&] {
    std::vector<CensusFrame> q;
    {
      std::lock_guard<std::mutex> lock(g_cfMu);
      q.swap(g_cfQueue);
    }
    for (const CensusFrame& f : q) CensusAdd(acc, f);
  };
  for (int t = 0; t < seconds * 10 && !g_benchAbort; ++t) {
    Sleep(100);
    drain();
  }
  g_censusStop = true;  // the render thread releases its staging buffers at its next DispatchIndirect
  for (int i = 0; i < 100 && !g_censusReleased.load(); ++i) Sleep(10);
  g_census = false;
  ptiming::g_recording = false;
  const uint64_t frames = frameCounter.load() - f0;
  gpt::End();
  drain();
  if (!g_censusReleased.load())
    Log("  chain census: the render thread did not release the staging buffers (no DispatchIndirect since); kept");
  const std::map<std::string, ptiming::Stat> cpu = ptiming::TakeByName(nullptr, nullptr);
  CensusReport(acc, [](void* p) { return ptiming::RttiName(p); });
  Log("  chain census: GPU pass timing over the same window (the view chains are the TerrainModule calls):");
  gpt::Report(frames, cpu);
}

// ---------------------------------------------------------------------------
// (e) Small exact GPU items, one ABBA bundle
// ---------------------------------------------------------------------------
struct BundleItem {
  const char* name;
  const char* source;
  void (*apply)(bool on);  // nullptr: not built yet
};
const BundleItem kBundle[] = {
    {"recorder caster lists issued ahead of the cascade culling chain", "R26 11.7a", nullptr},
    {"nested ExecuteCommandList for the shadow helper lists", "R26 9.2b", nullptr},
    {"forest before terrain in the cascades", "R27 R4", nullptr},
    {"terrain surface VS without its 44 dead instructions", "R28", nullptr},
    {"animator dispatch repeat skip", "R36 4b", nullptr},
};
constexpr int kBundleItems = static_cast<int>(sizeof(kBundle) / sizeof(kBundle[0]));

uint32_t BundleBuiltMask(uint32_t want) {
  uint32_t m = 0;
  for (int i = 0; i < kBundleItems; ++i)
    if (((want >> i) & 1) && kBundle[i].apply) m |= 1u << i;
  return m;
}

void ApplyBundle(uint32_t mask, bool on) {
  for (int i = 0; i < kBundleItems; ++i)
    if (((mask >> i) & 1) && kBundle[i].apply) kBundle[i].apply(on);
}

// ABBA order: pair p is OFF,ON for even p and ON,OFF for odd p (pure; offline tested).
inline bool BundleBlockOn(int block) { return ((block / 2) & 1) ? (block & 1) == 0 : (block & 1) == 1; }

void GpuBundlePhase(uint32_t want, int pairs, std::atomic<uint64_t>& frameCounter) {
  const uint32_t active = BundleBuiltMask(want);
  for (int i = 0; i < kBundleItems; ++i)
    if ((want >> i) & 1)
      Log("  gpu bundle item %d: %s (%s): %s", i, kBundle[i].name, kBundle[i].source,
          kBundle[i].apply ? "built, in the ON arm" : "NOT BUILT, stock in both arms");
  if (!active)
    Log("  gpu bundle: no requested item is built: this run is an A/A (both arms stock), the noise floor of the "
        "per-block GPU numbers");
  const int blocks = pairs * 2;
  for (int b = 0; b < blocks && !g_benchAbort; ++b) {
    const bool on = BundleBlockOn(b);
    ApplyBundle(active, on);
    Sleep(1500);
    Log("  gpu bundle block %d/%d: %s (items 0x%x)", b + 1, blocks, on ? "ON" : "OFF", on ? active : 0u);
    gpt::Measure(5000, frameCounter);
  }
  ApplyBundle(active, false);
}

}  // namespace mkit
