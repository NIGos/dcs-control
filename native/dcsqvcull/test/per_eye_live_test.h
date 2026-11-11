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
    lone.gb[9].tc[16 * 1] ^= 1;
    lone.gb[10].tc[16 * 1] ^= 1;
    lone.gb[11].tc[16 * 1] ^= 1;
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
  {  // a moving camera: a focus that does not share every row with its periphery still finds its eye
    pepl::FrameRec moving = fa;  // a row outside the per-view list differs in every focus record
    for (int i = 6; i < 12; ++i) moving.gb[i].tc[16 * 50 + 2] ^= 1;
    pepl::FrameRec loose = fa;   // even the view rows differ: frame order
    for (int i = 6; i < 12; ++i) loose.gb[i].tc[16 * 1 + 2] ^= 1;
    pepl::FrameRec three = loose;  // three different focus contexts: no order rule
    three.gb[11].tc[16 * 60] ^= 1;
    const pepl::Resolved r0 = pepl::Resolve(fa), r1 = pepl::Resolve(moving), r2 = pepl::Resolve(loose), r3 = pepl::Resolve(three);
    pepl::C5Acc a;
    pepl::C5Add(a, moving, r1);
    pepl::Diag d;
    pepl::DiagAdd(d, moving, r1);
    pepl::DiagAdd(d, loose, r2);
    Check(r0.tier == pepl::kTierShared && r1.tier == pepl::kTierView && r1.mainF[0] == 7 && r1.mainF[1] == 10 &&
              r2.tier == pepl::kTierOrder && r2.mainF[0] == 7 && r2.mainF[1] == 10 && r2.eye[6] == 0 && r2.eye[9] == 1 &&
              r3.tier == 0 && r3.mainF[0] == -1 && a.pairs == 2 && a.unknownPairs == 2 && a.firstUnknownRow == 50 &&
              pepl::DiagLine(d, 2, 24, 24).find("view rows 0-2 only 1, frame order 1") != std::string::npos,
          "per-eye live: a focus finds its eye by every shared row, else by the view rows, else by frame order; C5 "
          "then names the row that differs");
  }
  {  // frames by pass structure (the pass execute's frame argument is constant live)
    mkit::FrameTrack t;
    const int H = plod::kHeight, C = plod::kCascade, G = plod::kGBuffer, M = plod::kHmd, L = plod::kLights, O = plod::kOther;
    const int frame[] = {H, C, C, C, C, L, G, G, G, M, L, G, G, G, M, O, L, G, M, L, G, M, O};
    int starts = 0;
    bool early = false;
    for (int rep = 0; rep < 3; ++rep)
      for (int k : frame) {
        const bool s1 = t.OnTop(k, true), s2 = t.OnTop(k, false);
        starts += s1;
        early |= s2 || (s1 && k != H);
      }
    mkit::FrameTrack none;  // no shadow or height pass at all: no frame start, the diagnostic names it
    bool any = false;
    for (int i = 0; i < 50; ++i) any |= none.OnTop(G, true) || none.OnTop(G, false) || none.OnTop(M, true) || none.OnTop(M, false);
    Check(starts == 2 && t.serial == 2 && !early && !any && none.serial == 0,
          "per-eye live: a frame starts at the first HeightMapPass / cascade after a view, once per frame; never "
          "without such a pass");
  }
  {  // the diagnostic: what a capture saw, and the cause when nothing pairs
    pepl::Diag d;
    pepl::DiagAdd(d, fa, pepl::Resolve(fa));
    pepl::FrameRec noFocus = fa;
    noFocus.n = 6;
    pepl::FrameRec noTc = fa;
    for (int i = 0; i < noTc.n; ++i) noTc.gb[i].tcOk = false;
    pepl::FrameRec flat = fa;  // row 19 is not (0, 0, 1, 0): no view is recognised
    for (int i = 0; i < flat.n; ++i) flat.gb[i].tc[16 * ftwin::kRowPw + 3] ^= 0x40;
    pepl::FrameRec empty;
    Check(pepl::WhyNoPair(fa, pepl::Resolve(fa)) == pepl::kNpOk && pepl::WhyNoPair(noFocus, pepl::Resolve(noFocus)) == pepl::kNpNoFocus &&
              pepl::WhyNoPair(noTc, pepl::Resolve(noTc)) == pepl::kNpNoTc && pepl::WhyNoPair(flat, pepl::Resolve(flat)) == pepl::kNpNoPeriph &&
              pepl::WhyNoPair(empty, pepl::Resolve(empty)) == pepl::kNpNoPass,
          "per-eye live: why a frame has no periphery + focus pair (no pass, no TerrainContext, no periphery, no focus)");
    const std::string ok = pepl::DiagLine(d, 180, 2160, 2160);
    pepl::Diag bad;
    pepl::DiagAdd(bad, flat, pepl::Resolve(flat));
    const std::string l0 = pepl::DiagLine(pepl::Diag(), 0, 24, 24), l1 = pepl::DiagLine(pepl::Diag(), 180, 0, 0),
                      l2 = pepl::DiagLine(pepl::Diag(), 180, 2160, 2160), l3 = pepl::DiagLine(bad, 180, 2160, 2160);
    Check(ok.compare(0, 14, "pep live diag:") == 0 && ok.find("CAUSE") == std::string::npos && ok.find("paired 1") != std::string::npos &&
              l0.find("CAUSE: no frame start") != std::string::npos && l1.find("CAUSE: no top-level pass named GBufferPass") != std::string::npos &&
              l2.find("CAUSE: no frame was read back") != std::string::npos &&
              l3.find("CAUSE: no frame had both eyes") != std::string::npos && l3.find("first unpaired: pass 0 row16") != std::string::npos,
          "per-eye live: the diagnostic line names the cause (no frame start, no G-buffer pass, nothing read back, no "
          "views) and shows the first unpaired TerrainContext");
  }
  {  // the pixel stage's per-view rows (for the pixel-shader clone's lists)
    uint8_t p[64] = {}, f[64] = {};
    for (int i = 0; i < 64; ++i) p[i] = f[i] = static_cast<uint8_t>(i * 7);
    f[16 * 1 + 4] ^= 1;   // row 1, y
    f[16 * 3 + 12] ^= 1;  // row 3, w
    pepl::PsRowsAcc a;
    pepl::PsRowsAdd(a, 0, p, 64, f, 64);
    pepl::PsRowsAdd(a, 0, p, 64, p, 64);
    pepl::PsRowsAdd(a, 1, p, 64, f, 48);  // sizes differ: not compared
    pepl::PsRowsAdd(a, 5, p, 64, f, 64);  // no such slot
    const std::string l0 = pepl::PsRowsLine(a, 0), l1 = pepl::PsRowsLine(a, 1);
    Check(a.pairs[0] == 2 && a.comps[0][1] == 2 && a.comps[0][3] == 8 && a.comps[0][0] == 0 && a.sizeMismatch[1] == 1 &&
              l0.find("b7, first 64 B, 2 periphery/focus pairs") != std::string::npos && l0.find(" 1:2 3:8 (row:component mask)") != std::string::npos &&
              l1.find("b10") != std::string::npos && l1.find("not captured") != std::string::npos,
          "per-eye live C5: rows of the pixel stage's b7 / b10 that differ between periphery and focus, per component");
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
    const pepl::C2TerrainOut t = pepl::C2Terrain({0.9, 1.1}, {0.6, 0.5}), u = pepl::C2Terrain({0.5}, {0.3}),
                             w = pepl::C2Terrain({}, {0.3});
    Check(std::fabs(t.stock - 1.0) < 1e-9 && std::fabs(t.frontEnd - 0.55) < 1e-9 && t.verdict == 1 && u.verdict == 0 &&
              w.verdict == -1 && !pepl::g_tiOn.load(),
          "per-eye live C2: the terrain item's front end against stage 3's gate (0.30 ms saved + 0.11 ms of copies)");
  }
  {  // C7: per frame, the four chains of one pass; peripheries by size, whatever the frame's layout
    int module = 0, casc = 0, height = 0;
    int module2 = 0;  // each view's chain runs in its own TerrainModule pass object
    const auto kindOf = [&](void* p) -> char { return p == &casc ? 'C' : p == &height ? 'H' : p == &module || p == &module2 ? 'T' : '?'; };
    // order: the view chains' sizes in frame order (P = periphery, F = focus); pre: chains before them
    auto frame = [&](const char* order, int pre, uint32_t firstFocusCount) {
      mkit::CensusFrame f;
      for (int c = 0; c < pre; ++c) {
        f.pass.push_back(c + 1 == pre ? static_cast<void*>(&height) : static_cast<void*>(&casc));
        f.counts.push_back({0, 9, 9});
      }
      int nf = 0;
      for (int k = 0; k < 4; ++k) {
        f.pass.push_back(k & 1 ? static_cast<void*>(&module2) : static_cast<void*>(&module));
        if (order[k] == 'P')
          f.counts.push_back({5, 4, 2});
        else
          f.counts.push_back(nf++ == 0 ? std::vector<uint32_t>{3, firstFocusCount, 0} : std::vector<uint32_t>{1, 1, 0});
      }
      return f;
    };
    pepl::C7Acc a;
    for (int i = 0; i < 20; ++i) pepl::C7Add(a, frame("PPFF", 3, 4), kindOf);
    for (int i = 0; i < 10; ++i) pepl::C7Add(a, frame("PFPF", 2, 4), kindOf);   // another layout, another chain count
    const int ok = pepl::C7Verdict(a);
    const std::vector<std::string> lay = pepl::C7Layouts(a);
    mkit::CensusFrame three = frame("PPFF", 3, 4);
    three.pass.pop_back();
    three.counts.pop_back();
    pepl::C7Acc b = a;
    pepl::C7Add(b, three, kindOf);                    // a frame with three view chains: counted, never compared
    pepl::C7Add(b, frame("PPFF", 3, 6), kindOf);      // a focus with more instances of one object than its periphery
    pepl::C7Acc few;
    for (int i = 0; i < 5; ++i) pepl::C7Add(few, frame("PPFF", 3, 4), kindOf);
    Check(ok == 1 && a.frames == 30 && a.n[0] == 30 && a.subset[0] == 30 && a.subset[1] == 30 && lay.size() == 2 &&
              lay[0] == "pep C7 layout: 20 frames CCH|PPFF" && lay[1] == "pep C7 layout: 10 frames CH|PFPF",
          "per-eye live C7: the view chains are the TerrainModule ones (distinct pass objects), peripheries by "
          "size: contained in every frame over two layouts");
    Check(b.noFour == 1 && b.n[0] == 31 && b.subset[0] == 30 && b.maxOver[0] == 1 && b.subset[1] == 31 &&
              pepl::C7Verdict(b) == 0 && pepl::C7Verdict(few) == -1 && pepl::C7Verdict(pepl::C7Acc()) == -1 &&
              pepl::C7Line(b).compare(0, 7, "pep C7:") == 0 && pepl::C7Line(b).find(": FAIL") != std::string::npos,
          "per-eye live C7: one frame over: FAIL; a frame without four view chains is not compared; too few: not "
          "measured");
  }
}

}  // namespace pepltest
