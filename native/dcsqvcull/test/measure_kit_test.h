// Measurement kit (measure_kit.h, bench.h's render-thread columns, bench modes 34 / 35): the pure parts offline.
// Nothing here needs DCS: the pass timeline's per-frame aggregation, the chain census's containment statistics, the
// GPU bundle's ABBA order and its "nothing built" A/A, the A/B's render-thread metrics, and every new key off by
// default.
#pragma once

namespace mktest {

void Run() {
  printf("-- measurement kit (perf audit 2026-11-10) --\n");
  {  // every new key is off by default (measure only, nothing runs)
    const Config d{};
    Check(!d.benchPassMs && d.suitePassTimelineSec == 0 && d.suiteChainCensusSec == 0 && d.suiteGpuBundleTiming == 0 &&
              !d.suiteBenchPrefetchTableBits && !d.suiteBenchGBufferRecorderCockpit,
          "measure kit: every new key off by default");
    Check(!ptiming::g_timeline.load() && !mkit::g_census.load(), "measure kit: timeline and census flags off at load");
  }
  {  // (a) per-frame aggregation: first and last frame dropped, absent kinds count 0 in a frame
    int a = 0, b = 0;
    void* pa = &a;
    void* pb = &b;
    std::vector<ptiming::TlEvent> ev = {
        {pa, 10, 9000.f},                                      // partial first frame (dropped)
        {pa, 11, 1000.f}, {pb, 11, 500.f}, {pa, 11, 1000.f},   // A 2.0 ms (two calls), B 0.5
        {pa, 12, 3000.f},                                      // A 3.0, B absent (0)
        {pa, 13, 1000.f}, {pb, 13, 1500.f},                    // A 1.0, B 1.5
        {pb, 14, 9000.f},                                      // partial last frame (dropped)
    };
    std::vector<double> tot;
    std::vector<std::pair<uint32_t, std::map<std::string, double>>> frames;
    const auto rows = mkit::TimelineRows(ev, [&](void* p) { return std::string(p == pa ? "A" : "B"); }, &tot, &frames);
    Check(rows.size() == 2 && rows[0].name == "A" && rows[1].name == "B", "measure kit: timeline kinds by mean");
    Check(rows.size() == 2 && std::fabs(rows[0].mean - 2.0) < 1e-9 && std::fabs(rows[1].mean - 2.0 / 3.0) < 1e-9,
          "measure kit: timeline means over the full frames only");
    Check(rows.size() == 2 && std::fabs(rows[0].p95 - 3.0) < 1e-9 && std::fabs(rows[1].p50 - 0.5) < 1e-9,
          "measure kit: timeline p50 / p95 (an absent kind is 0 in that frame)");
    Check(tot.size() == 3 && std::fabs(tot[0] - 2.5) < 1e-9 && std::fabs(tot[1] - 3.0) < 1e-9 && std::fabs(tot[2] - 2.5) < 1e-9,
          "measure kit: timeline per-frame render-thread totals");
    Check(frames.size() == 3 && frames[0].first == 11 && frames[2].first == 13, "measure kit: timeline CSV frames");
    Check(rows.size() == 2 && std::fabs(rows[0].callsPerFrame - 4.0 / 3.0) < 1e-9, "measure kit: timeline calls per frame");
  }
  {  // (d) census: containment per ordered pair, modal chain count, overflow frames left out
    int p0 = 0, p1 = 0;
    mkit::CensusAcc acc;
    mkit::CensusFrame f;
    f.pass = {&p0, &p0, &p1};
    f.counts = {{3, 2, 0, 5}, {1, 2, 0, 5}, {0, 0, 9}};
    mkit::CensusAdd(acc, f);
    f.counts = {{3, 2, 0, 5}, {4, 2, 0, 6}, {0, 0, 9}};  // chain 1 exceeds chain 0 on two objects
    mkit::CensusAdd(acc, f);
    mkit::CensusFrame other = f;
    other.counts.pop_back();
    other.pass.pop_back();
    mkit::CensusAdd(acc, other);  // another chain count: not compared
    mkit::CensusFrame over = f;
    over.overflow = true;
    mkit::CensusAdd(acc, over);
    Check(acc.frames == 2 && acc.chains == 3 && acc.otherCount == 1 && acc.overflow == 1,
          "measure kit: census frames, modal chain count, other counts and overflows");
    const mkit::PairStat& p01 = acc.pair[{0, 1}];
    const mkit::PairStat& p10 = acc.pair[{1, 0}];
    Check(p01.frames == 2 && p01.subset == 1 && p01.maxObjectsOver == 2 && std::fabs(p01.excessSum - 2.0) < 1e-9,
          "measure kit: census #0 >= #1 contained in 1 of 2 frames, 2 objects over, 2 instances in excess");
    Check(p10.frames == 2 && p10.subset == 1 && p10.maxObjectsOver == 1, "measure kit: census #1 >= #0");
    Check(acc.pair.find({0, 2}) == acc.pair.end(), "measure kit: census compares only chains of the same object count");
    Check(acc.chain.size() == 3 && std::fabs(acc.chain[0].instances - 20.0) < 1e-9 &&
              std::fabs(acc.chain[0].nonEmpty - 6.0) < 1e-9 && acc.chain[2].nObj == 3,
          "measure kit: census per-chain instances and non-empty objects");
  }
  {  // (e) ABBA order; nothing built -> A/A
    const bool want[8] = {false, true, true, false, false, true, true, false};
    bool ok = true;
    for (int b = 0; b < 8; ++b) ok = ok && mkit::BundleBlockOn(b) == want[b];
    Check(ok, "measure kit: gpu bundle blocks run OFF ON ON OFF OFF ON ON OFF");
    Check(mkit::BundleBuiltMask(0x1f) == 0 && mkit::kBundleItems == 5,
          "measure kit: gpu bundle has the five audit items, none built yet (the run is an A/A)");
  }
  {  // (b, f) A/B render-thread metrics: taken when the thread was read, -1 otherwise
    BenchBlock blk;
    blk.frames = 100;
    blk.seconds = 2.0;
    blk.rtThreadCycles = 1000000000ull;  // 1 s at 1 GHz
    blk.rtThreadOk = true;
    blk.passTopUs = 500000;  // 5 ms per frame
    blk.passGbUs = 200000;
    const BlockMetrics m = Metrics(blk, 1e9);
    Check(std::fabs(m.renderCpuMs - 10.0) < 1e-9 && std::fabs(m.renderBusyPct - 50.0) < 1e-9,
          "measure kit: render-thread CPU ms per frame and busy %");
    Check(std::fabs(m.passMs - 5.0) < 1e-9 && std::fabs(m.gbPassMs - 2.0) < 1e-9, "measure kit: pass ms per block");
    BenchBlock none;
    none.frames = 10;
    none.seconds = 1.0;
    const BlockMetrics n = Metrics(none, 1e9);
    Check(n.renderCpuMs < 0 && n.renderBusyPct < 0 && n.passMs < 0 && n.gbPassMs < 0,
          "measure kit: metrics not taken stay -1 (left out of the result)");
    Check(PassNameHasGBuffer("GBufferPass") && PassNameHasGBuffer("SceneRenderer::gbufferModule") &&
              !PassNameHasGBuffer("CascadeShadowPass") && !PassNameHasGBuffer("GBufferLightingPass"),
          "measure kit: G-buffer pass names");
  }
}

}  // namespace mktest
