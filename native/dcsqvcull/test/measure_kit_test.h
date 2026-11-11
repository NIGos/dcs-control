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
    Check(!d.shadowRecorderNest && !d.smallGpuBundle && !shrec::g_nest.load() && shrec::g_nestForce.load() == -1,
          "A6: [Model] ShadowRecorderNest and SmallGpuBundle off by default (the lists run one by one, as before)");
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
  {  // (a) live condition (2026-11-11 s1_airfield): DCS passes 0 as execute's frame argument; the frame boundary comes
     // from the quad-view collect counter, latched per top-level execute (nested passes keep their tree's id)
    Check(ptiming::TopFrameId(0, 0) == 0 && ptiming::TopFrameId(7, 0) == 7 && ptiming::TopFrameId(0, 42) == 42 &&
              ptiming::TopFrameId(7, 42) == 42,
          "measure kit: top-level frame id = quad-view frame counter, the execute argument only before it starts");
    static int top = 0, child = 0;
    struct Fake {
      static void __fastcall Exec(void* pass, uint64_t) {
        if (pass == &top) {
          ptiming::Hook(&child, 0);
          ptiming::Hook(&child, 0);
        }
      }
    };
    const ptiming::ExecFn origSave = ptiming::g_orig;
    const uint64_t quadSave = g_quadFrame.load();
    const uint64_t execSave = ptiming::g_execFrame.load();
    ptiming::g_orig = &Fake::Exec;
    {
      std::lock_guard<std::mutex> lock(ptiming::g_mutex);
      ptiming::g_tl.clear();
      ptiming::g_tlCap = 10000;
    }
    ptiming::Reset();
    g_quadFrame = 1000;
    ptiming::g_timeline = true;
    ptiming::g_recording = true;
    for (int f = 0; f < 6; ++f) {
      ++g_quadFrame;  // HookCollect -> UpdateGazeAndCheckHold, once per quad-view frame, before the first pass
      ptiming::Hook(&top, 0);
      ptiming::Hook(&top, 0);  // a second top-level tree in the same frame
    }
    ptiming::g_recording = false;
    ptiming::g_timeline = false;
    std::vector<ptiming::TlEvent> ev;
    {
      std::lock_guard<std::mutex> lock(ptiming::g_mutex);
      ev.swap(ptiming::g_tl);
      ptiming::g_tlCap = 0;
    }
    ptiming::TakeByName(nullptr, nullptr);
    ptiming::g_orig = origSave;
    g_quadFrame = quadSave;
    ptiming::g_execFrame = execSave;
    std::vector<double> tot;
    std::vector<std::pair<uint32_t, std::map<std::string, double>>> frames;
    const auto rows =
        mkit::TimelineRows(ev, [&](void* p) { return std::string(p == &top ? "top" : "child"); }, &tot, &frames);
    Check(ev.size() == 36, "measure kit: live condition, every pass recorded (6 frames x 2 trees x 3 passes)");
    Check(tot.size() == 4 && frames.size() == 4 && frames.front().first == 1002 && frames.back().first == 1005,
          "measure kit: live condition (frame argument 0) gives one timeline frame per quad-view frame, not one");
    Check(rows.size() == 2 && std::fabs(rows[0].callsPerFrame + rows[1].callsPerFrame - 6.0) < 1e-9,
          "measure kit: live condition, a frame holds both of its trees, nested passes included");
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
    bool named = true;  // every item not built says why
    for (int i = 0; i < mkit::kBundleItems; ++i)
      named = named && (mkit::kBundle[i].apply != nullptr) == (mkit::kBundle[i].dropped == nullptr);
    Check(mkit::BundleBuiltMask(0x1f) == 0x2 && mkit::BundleBuiltMask(0x1d) == 0 && mkit::kBundleItems == 5 && named,
          "measure kit: gpu bundle has the five audit items, only bit 1 (nested shadow lists) built, the others name "
          "why (a mask without bit 1 is an A/A)");
  }
  {  // (e) bit 1: the ABBA arm overrides [Model] ShadowRecorderNest, the end restores it; the pass's decision
    const int force0 = shrec::g_nestForce.load();
    const bool nest0 = shrec::g_nest.load();
    shrec::g_nest = false;
    mkit::ApplyBundle(0x2, 1);
    const bool on = shrec::NestWanted(shrec::g_nest.load(), shrec::g_nestForce.load());
    shrec::g_nest = true;
    mkit::ApplyBundle(0x2, 0);
    const bool off = !shrec::NestWanted(shrec::g_nest.load(), shrec::g_nestForce.load());
    mkit::ApplyBundle(0x2, -1);
    const bool back = shrec::g_nestForce.load() == -1 && shrec::NestWanted(true, -1) && !shrec::NestWanted(false, -1);
    mkit::ApplyBundle(0x1d, 1);  // not built: no effect
    const bool untouched = shrec::g_nestForce.load() == -1;
    shrec::g_nestForce = force0;
    shrec::g_nest = nest0;
    ID3D11CommandList* const fake = reinterpret_cast<ID3D11CommandList*>(uintptr_t{0x10});
    ID3D11CommandList* const none[4] = {};
    ID3D11CommandList* const one[4] = {fake, nullptr, nullptr, nullptr};
    ID3D11CommandList* const gap[4] = {fake, nullptr, fake, nullptr};  // helper 1 took no chunk, helper 2 did
    const bool counts = shrec::ListCount(none, 4) == 0 && shrec::ListCount(one, 4) == 1 && shrec::ListCount(gap, 4) == 2;
    const bool decide = !shrec::ShouldNest(true, 1, false) && shrec::ShouldNest(true, 2, false) &&
                        shrec::ShouldNest(true, 4, false) && !shrec::ShouldNest(false, 4, false) &&
                        !shrec::ShouldNest(true, 4, true) && !shrec::ShouldNest(true, 0, false);
    Check(on && off && back && untouched && counts && decide,
          "measure kit: nested shadow lists follow the ABBA arm (1 on, 0 off, -1 the ini key), nest only 2+ lists, "
          "never after a failure");
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
