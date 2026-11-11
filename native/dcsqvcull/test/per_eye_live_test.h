// One geometry pass per eye, stage 2 (src\per_eye_live.h): the pure parts of the live captures, offline.
//   - every key 0 by default, nothing armed at load;
//   - view resolution from TerrainContexts (four census pairs of per_eye_pass_test.h): periphery by its centre, the
//     focus by its eye's shared rows, the main pass of three per view, a context no view owns;
//   - C5: the exact-row check on census bytes, the per-view row list, the early source (periphery centre -> focus
//     scalars) over two jitter phases, a moved gaze, the verdicts;
//   - C4: the order statistic and its 95 % rule; C2: the share and its 0.6 rule; C7: the view chains' containment.
#pragma once

namespace pepltest {

// A TerrainContext ftwin::ClassifyTc accepts (peptest::MakeTc fills the unused rows with noise; P's w row is fixed).
void Tc(const peptest::CensusPair& cp, bool focus, uint8_t* tc) {
  peptest::MakeTc(cp, focus, tc);
  const float w[4] = {0, 0, 1, 0};
  memcpy(tc + 16 * ftwin::kRowPw, w, 16);
}
// One frame of an HMD: per view three G-buffer passes (small, main, last) in DCS's order: periphery L, periphery R,
// focus L, focus R. `a` / `b`: the census pairs of the two eyes.
pepl::FrameRec Frame(uint32_t id, const peptest::CensusPair& a, const peptest::CensusPair& b) {
  pepl::FrameRec f;
  f.frame = id;
  const peptest::CensusPair* eyes[2] = {&a, &b};
  for (int focus = 0; focus < 2; ++focus)
    for (int e = 0; e < 2; ++e)
      for (int k = 0; k < 3; ++k) {
        pepl::GbRec& g = f.gb[f.n];
        g.ord = static_cast<uint8_t>(f.n);
        g.tcOk = true;
        Tc(*eyes[e], focus != 0, g.tc);
        ++f.n;
      }
  return f;
}

void Run() {
  printf("-- per-eye pass, live captures (pure parts) --\n");
  {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const pepl::Keys k = pepl::ReadKeys((std::wstring(tmp) + L"qv_pepl_missing.ini").c_str());
    Check(k.c2 == 0 && k.c4 == 0 && k.c5 == 0 && k.c7 == 0 && k.logSec == 10 && !pepl::g_capture.load() &&
              !pepl::g_walk.load() && !pepl::g_c2On.load() && pepl::g_c2Ord[0].load() == -1,
          "per-eye live: every capture key 0 by default, nothing armed at load");
  }
  using peptest::kPairs;
  // Census pairs 1 / 0 are one frame's two eyes (periphery centre > 0 = eye 0), pairs 2 / 3 the next jitter phase.
  const pepl::FrameRec fa = Frame(100, kPairs[1], kPairs[0]), fb = Frame(101, kPairs[2], kPairs[3]);
  {
    const pepl::Resolved r = pepl::Resolve(fa);
    bool kinds = true;
    for (int i = 0; i < 12; ++i)
      kinds &= r.kind[i] == (i < 6 ? ftwin::kPeriph : ftwin::kFocus) && r.eye[i] == (i / 3) % 2;
    Check(kinds && r.mainP[0] == 1 && r.mainP[1] == 4 && r.mainF[0] == 7 && r.mainF[1] == 10,
          "per-eye live: views from the TerrainContext (periphery by centre, focus by its eye's shared rows), the "
          "main pass = the second of three");
    pepl::FrameRec two = fa;  // two passes per view (R38 census): the first is the main one; a pass without a context
    two.n = 0;
    for (int i = 0; i < 12; ++i)
      if (i % 3 != 2) two.gb[two.n++] = fa.gb[i];
    two.gb[two.n] = fa.gb[0];
    two.gb[two.n++].tcOk = false;
    pepl::FrameRec lone = fa;  // a narrow camera that shares no periphery's rows (an MFD zoom): no eye, never a main
    lone.gb[9].tc[16 * 3] ^= 1;
    lone.gb[10].tc[16 * 3] ^= 1;
    lone.gb[11].tc[16 * 3] ^= 1;
    const pepl::Resolved r2 = pepl::Resolve(two), r3 = pepl::Resolve(lone);
    Check(r2.mainP[0] == 0 && r2.mainP[1] == 2 && r2.mainF[0] == 4 && r2.mainF[1] == 6 && r2.kind[8] == ftwin::kOther &&
              r3.mainF[1] == -1 && r3.mainF[0] == 7 && r3.eye[10] == -1 && pepl::MainOrdinal(1) == 0 &&
              pepl::MainOrdinal(4) == 2,
          "per-eye live: two passes per view, a pass without a TerrainContext, a focus-like view no eye owns");
  }
  {
    uint8_t p[pep::kTcBytes], f[pep::kTcBytes];
    Tc(kPairs[0], false, p);
    Tc(kPairs[0], true, f);
    int first = 0;
    const int none = pepl::UnknownViewRows(p, f, &first);
    f[16 * 50 + 5] ^= 1;   // a shared row
    f[16 * 44 + 1] ^= 1;   // the render-size row's x (shared: only z, w are per view)
    int first2 = 0;
    const int two = pepl::UnknownViewRows(p, f, &first2);
    Check(none == 0 && first == -1 && two == 2 && first2 == 44,
          "per-eye live: rows differing between the views outside the patcher's per-view list are counted "
          "(component-wise)");
  }
  {  // C5 on census bytes: 120 frames alternating the two jitter phases, fixed gaze
    pepl::C5Acc a;
    for (int i = 0; i < 120; ++i) {
      const pepl::FrameRec& f = i & 1 ? fb : fa;
      pepl::C5Add(a, f, pepl::Resolve(f));
    }
    Check(a.frames == 120 && a.pairs == 240 && a.rowsExact == 240 && a.unknownPairs == 0 && a.table[0].size() == 2 &&
              a.table[1].size() == 2 && a.lookups == 236 && a.hits == 236 && a.prevHits == 0 && a.zoomSame == 238 &&
              a.jitterMaxPx < 0.01 && pepl::C5Verdict(a) == 1 && a.samples.size() == 4,
          "per-eye live C5: census frames: focus rows exact from the periphery's shared rows, the early source hits "
          "100 %, the jitter step equal in pixels in both views: PASS");
    if (!(a.rowsExact == 240 && a.hits == 236)) printf("     %s\n", pepl::C5Line(a).c_str());
    const std::string line = pepl::C5Line(a);
    Check(line.compare(0, 7, "pep C5:") == 0 && line.find("hit 100.00 % of 236 lookups") != std::string::npos &&
              line.find(": PASS") != std::string::npos,
          "per-eye live C5: the log line");
    // The gaze moves (the focus centre changes while the periphery's does not): the early source misses.
    pepl::C5Acc m = a;
    pepl::FrameRec moved = fa;
    for (int i = 6; i < 12; ++i) {
      float* px = reinterpret_cast<float*>(moved.gb[i].tc + 16 * pep::kRowPx);
      px[2] += 0.01f;
    }
    for (int i = 0; i < 4; ++i) pepl::C5Add(m, moved, pepl::Resolve(moved));
    Check(m.hits == a.hits + 6 && m.lookups == a.lookups + 8 && m.rowsExact == a.rowsExact && pepl::C5Verdict(m) == 0,
          "per-eye live C5: a moved focus centre misses the early source and its stale VP rows are not exact: FAIL");
    pepl::C5Acc few;
    for (int i = 0; i < 20; ++i) pepl::C5Add(few, fa, pepl::Resolve(fa));
    Check(pepl::C5Verdict(few) == -1 && pepl::C5Line(few).find("NOT MEASURED") != std::string::npos,
          "per-eye live C5: too few frames: not measured");
  }
  {  // C4
    auto run = [&](int first, int mixed) {
      pepl::C4Acc a;
      for (int i = 0; i < first + mixed; ++i) {
        pepl::FrameRec f = fa;
        for (int g = 0; g < f.n; ++g) {
          f.gb[g].items = 300, f.gb[g].merged = 2, f.gb[g].firstMerged = 0, f.gb[g].lastMerged = 1;
          f.gb[g].order = static_cast<uint8_t>(i < first ? pep::kMoFirst : pep::kMoMixed);
        }
        f.gb[7].order = static_cast<uint8_t>(i < first ? pep::kMoLast : pep::kMoMixed);  // eye 0's focus main
        pepl::C4Add(a, f, pepl::Resolve(f));
      }
      return a;
    };
    const pepl::C4Acc ok = run(96, 4), bad = run(90, 10), few = run(20, 0);
    pepl::C4Acc none;
    pepl::C4Add(none, fa, pepl::Resolve(fa));
    Check(ok.v[1].passes == 200 && ok.v[1].order[pep::kMoFirst] == 96 && ok.v[1].order[pep::kMoLast] == 96 &&
              pepl::C4Verdict(ok) == 1 && pepl::C4Verdict(bad) == 0 && pepl::C4Verdict(few) == -1 &&
              none.notWalked == 4 && none.v[1].passes == 0 &&
              pepl::C4Line(ok).find("focus first or last >= 95 %: PASS") != std::string::npos,
          "per-eye live C4: order of the focus main passes (first or last in >= 95 %), passes without an item vector");
  }
  {  // C2
    const pepl::C2Out a = pepl::C2Share({1.0, 1.2, 0.8}, {0.7, 0.7}), b = pepl::C2Share({1.0}, {0.5}), c = pepl::C2Share({}, {0.5}),
                      d = pepl::C2Share({0.0}, {0.5});
    Check(std::fabs(a.share - 0.7) < 1e-9 && a.verdict == 1 && b.verdict == 0 && c.verdict == -1 && d.verdict == -1,
          "per-eye live C2: front-end share = geometry-only / stock of the focus main passes (pass >= 0.6)");
  }
  {  // C7: nine chains (4 cascades, height map, 4 views); focus counts inside the periphery's
    auto frame = [](uint32_t id, uint32_t focusL) {
      mkit::CensusFrame f;
      f.frame = id;
      for (int c = 0; c < 9; ++c) {
        f.pass.push_back(nullptr);
        f.counts.push_back(c == 7 ? std::vector<uint32_t>{3, focusL, 0} : c == 8 ? std::vector<uint32_t>{1, 1, 0}
                                                                                 : std::vector<uint32_t>{5, 4, 2});
      }
      return f;
    };
    mkit::CensusAcc acc;
    for (uint32_t i = 0; i < 30; ++i) mkit::CensusAdd(acc, frame(i, 4));
    const pepl::C7Out ok = pepl::C7FromCensus(acc);
    mkit::CensusAdd(acc, frame(30, 6));
    const pepl::C7Out bad = pepl::C7FromCensus(acc);
    mkit::CensusAcc fewFrames;
    for (uint32_t i = 0; i < 5; ++i) mkit::CensusAdd(fewFrames, frame(i, 4));
    Check(ok.verdict == 1 && ok.chains == 9 && ok.subset[0] == 30 && ok.subset[1] == 30 && bad.verdict == 0 &&
              bad.subset[0] == 30 && bad.frames[0] == 31 && bad.maxOver[0] == 1 && bad.subset[1] == 31 &&
              pepl::C7FromCensus(fewFrames).verdict == -1 && pepl::C7FromCensus(mkit::CensusAcc()).verdict == -1 &&
              pepl::C7Line(bad).compare(0, 7, "pep C7:") == 0,
          "per-eye live C7: the focus chains' counts inside their periphery's (last four chains), one frame over: FAIL");
  }
}

}  // namespace pepltest
