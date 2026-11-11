// One geometry pass per eye, stage 2: the live captures of PER_EYE_PASS_DESIGN.md section 7. Measure only.
//
//   [Suite] PerEyeC5=N   N s. The TerrainContext (b11, 2176 B) bound at the end of every top-level GBufferPass is
//                        copied to a staging buffer and read back without waiting. Per frame and eye, periphery
//                        against focus: (1) are the focus VP rows 20 / 21 exactly pep::PredictFocusRows of the
//                        periphery's shared rows and the focus's own four projection scalars; (2) does any row
//                        outside pep::kTcViewRows differ between the two views (the clone patcher's list); (3) an
//                        early source for the four scalars with a fixed gaze: a table from the periphery's jittered
//                        centre (known at the periphery pass) to the focus scalars last seen with it; (4) the
//                        jitter step in pixels, periphery against focus. Pass: table hits >= 99 % and (1) = 100 %.
//                        Also (5), for the pixel-shader clone's lists: which rows of the pixel stage's b7 / b10
//                        (first 4096 B, as bound at the pass end) differ between the two views ("pep C5 ps rows:").
//   [Suite] PerEyeC4=N   N s. The item vector of every G-buffer pass execution (gb_batch.h's gbpass) is classed by
//                        the items' vtables before the pass runs: terrain family (the merged families' carrier) or
//                        not. Per focus main pass: pep::ClassifyOrder. Pass: first or last in >= 95 % of the passes.
//   [Suite] PerEyeC2=N   N s in five blocks OFF / ON / OFF / ON / OFF of GPU pass timing. LOSSY while ON (as R53's
//                        FocusGeomProbe, which this restores): every RSSetViewports inside the focus views' main
//                        G-buffer pass becomes a 16 x 16 viewport off target, so the pass still fetches, shades,
//                        clips and sets up every triangle but writes no pixel. ON / OFF of that pass's GPU time is
//                        the front-end share a merged pass can save. Pass: >= 0.6. Nothing else in this file
//                        changes what DCS draws; C2 does only inside its ON blocks. Also "pep C2 terrain:": the
//                        terrain item alone (the only mergeable part), timed as a nested pass in both arms.
//   [Suite] PerEyeC7=N   N s of measure_kit.h's chain census, plus one line for the view chains: are the focus
//                        chain's per-object instance counts <= the periphery's of the same eye in every frame (the
//                        "focus list = periphery list cut by the focus planes, same LOD" half). The ORDER half of C7
//                        cannot pass and is not captured: DCS's culling chain orders the instances of an object by
//                        racing atomics, so two stock runs on the same input already differ (R34 4.1, measured).
//   [Suite] PerEyeLogSec=N  a line every N s while C4 / C5 run (default 10).
// Every line starts with "pep C2:", "pep C4:", "pep C5:" or "pep C7:". With every key at 0 nothing here runs.
//
// Frames: by pass structure (mkit::FrameTrack: the first top-level HeightMapPass / shadow cascade after a view).
// The first live run (2026-11-11) used pass_timing.h's g_execFrame, which never changes live: one set filled once
// ("snapshots 24"), nothing was ever read back, every line said 0 frames. A "pep live diag:" line now says once per
// capture what was seen and names the cause when nothing pairs.
// How views are told apart: by the TerrainContext, as R53 / focus_twin_math.h (ftwin::ClassifyTc; a focus view's eye
// is the periphery of the same frame whose shared rows it has). The G-buffer passes of a view are counted in frame
// order; its main pass is the second to last of three or more, else the first (R44 live). Never by fixed ordinals.
//
// Device objects: at most kSets * kMaxGb staging buffers of 2176 B, created on the render thread inside the phase
// only, after frames have been steady for kSteadySec s (no creation during a mission load: R53), released there.
// No NvAPI call. Hooks: periph_lod.h's boundary chain (plod::g_probe), gbpass's second observer (C4), and for C2 only
// the RSSetViewports slot of DCS's immediate context from a focus main pass's begin to its end.
// Every render-thread body runs under SEH; a fault latches the captures off for the session.
//
// Included once from main.cpp (features.inc) after measure_kit.h and per_eye_pass.h: uses ptiming, plod, gbpass, gpt,
// mkit, fixdepth (its viewport hook, for the chain rule), d3ds, ftwin (focus_twin_math.h), pep, Log, g_benchAbort.
#pragma once

namespace pepl {

// ---------------------------------------------------------------------------
// Pure parts (offline tested: test/per_eye_live_test.h)
// ---------------------------------------------------------------------------
constexpr int kMaxGb = 24;  // top-level G-buffer pass executions kept per frame
struct GbRec {
  uint8_t ord = 0;          // order of the execution among the frame's top-level G-buffer passes
  bool tcOk = false;
  uint8_t order = 0;        // pep::MergeOrder of the item vector (C4; kMoNone when not walked)
  uint32_t items = 0, merged = 0, firstMerged = 0, lastMerged = 0;
  uint8_t tc[pep::kTcBytes];
};
struct FrameRec {
  uint32_t frame = 0;
  int n = 0;
  GbRec gb[kMaxGb];
};

struct Resolved {
  int kind[kMaxGb];  // ftwin::Kind
  int eye[kMaxGb];   // 0 / 1, -1 unknown
  int mainP[2], mainF[2];  // index into FrameRec::gb of each eye's periphery / focus main pass, -1 none
  int tier = 0;            // how the focus views found their eye this frame (kTier*), 0 none
};
// A focus view's eye, strictest rule first. Live in flight (2026-11-11, moving camera) no focus shared every
// non-per-view row with a periphery, so nothing paired and C2 had no passes to probe. The weaker rules keep the
// pairing; C5 then reports which rows differ ("rows differing outside the per-view list").
enum : int {
  kTierShared = 1,  // every row outside the per-view list byte-equal (ftwin::SharedRowsEqual)
  kTierView = 2,    // the view matrix rows 0-2 byte-equal (what the exact focus rows are built from)
  kTierOrder = 3,   // by frame order: the first focus view belongs to the first periphery's eye
};
// The view's main G-buffer pass among its n passes in frame order (R44 live: a small first pass, the main pass, a
// tiny last one; two per view in the R38 census: main, last).
inline int MainOrdinal(int n) { return n >= 3 ? n - 2 : 0; }
inline Resolved Resolve(const FrameRec& f) {
  Resolved r;
  for (int i = 0; i < kMaxGb; ++i) r.kind[i] = ftwin::kOther, r.eye[i] = -1;
  r.mainP[0] = r.mainP[1] = r.mainF[0] = r.mainF[1] = -1;
  int periph[2] = {-1, -1};
  for (int i = 0; i < f.n; ++i) {
    if (!f.gb[i].tcOk) continue;
    const ftwin::ViewId v = ftwin::ClassifyTc(f.gb[i].tc);
    r.kind[i] = v.kind;
    if (v.kind == ftwin::kPeriph && (v.eye == 0 || v.eye == 1)) {
      r.eye[i] = v.eye;
      if (periph[v.eye] < 0) periph[v.eye] = i;
    }
  }
  for (int tier = kTierShared; tier <= kTierView; ++tier)
    for (int i = 0; i < f.n; ++i) {
      if (r.kind[i] != ftwin::kFocus || r.eye[i] >= 0) continue;
      for (int e = 0; e < 2; ++e) {
        if (periph[e] < 0) continue;
        const uint8_t* p = f.gb[periph[e]].tc;
        if (tier == kTierShared ? ftwin::SharedRowsEqual(p, f.gb[i].tc) : memcmp(p, f.gb[i].tc, 48) == 0) r.eye[i] = e, r.tier = tier;
      }
    }
  if (!r.tier && periph[0] >= 0 && periph[1] >= 0) {
    // Frame order: the focus records fall into runs of one TerrainContext each; exactly two runs, as two peripheries.
    int run[kMaxGb], runs = 0, last = -1;
    for (int i = 0; i < f.n; ++i) {
      if (r.kind[i] != ftwin::kFocus) continue;
      if (last < 0 || memcmp(f.gb[last].tc, f.gb[i].tc, pep::kTcBytes) != 0) ++runs;
      run[i] = runs - 1;
      last = i;
    }
    if (runs == 2) {
      const int firstEye = periph[0] < periph[1] ? 0 : 1;
      for (int i = 0; i < f.n; ++i)
        if (r.kind[i] == ftwin::kFocus) r.eye[i] = run[i] == 0 ? firstEye : 1 - firstEye;
      r.tier = kTierOrder;
    }
  }
  for (int e = 0; e < 2; ++e)
    for (int k : {static_cast<int>(ftwin::kPeriph), static_cast<int>(ftwin::kFocus)}) {
      int list[kMaxGb], n = 0;
      for (int i = 0; i < f.n; ++i)
        if (r.kind[i] == k && r.eye[i] == e) list[n++] = i;
      if (!n) continue;
      (k == ftwin::kPeriph ? r.mainP : r.mainF)[e] = list[MainOrdinal(n)];
    }
  return r;
}

inline uint32_t Bits(float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}
// Rows that differ between two TerrainContexts outside the per-view list (component-wise). Returns the count and
// the first such row (-1 none).
inline int UnknownViewRows(const uint8_t* p, const uint8_t* f, int* first) {
  int n = 0;
  *first = -1;
  const pep::PatchOpts o;
  for (int r = 0; r < pep::kTcBytes / 16; ++r) {
    uint32_t diff = 0;
    for (int k = 0; k < 4; ++k)
      if (memcmp(p + 16 * r + 4 * k, f + 16 * r + 4 * k, 4) != 0) diff |= 1u << k;
    if (!diff) continue;
    uint32_t allowed = 0;
    for (uint32_t i = 0; i < o.nRows; ++i)
      if (o.rows[i].row == static_cast<uint32_t>(r)) allowed = o.rows[i].comps;
    if (diff & ~allowed) {
      if (*first < 0) *first = r;
      ++n;
    }
  }
  return n;
}

struct C5Acc {
  uint64_t frames = 0, pairs = 0, rowsExact = 0, unknownPairs = 0;
  int firstUnknownRow = -1;
  uint64_t lookups = 0, hits = 0, prevLookups = 0, prevHits = 0, zoomSame = 0, tableFull = 0;
  double jitterMaxPx = 0;
  struct Phase {
    uint32_t key[2], val[4];
  };
  std::vector<Phase> table[2];
  bool havePrev[2] = {false, false};
  float prevP[2][2] = {}, prevF[2][4] = {}, prevSize[2][4] = {};
  std::vector<std::string> samples;  // hex, for pinning the jitter's float form offline
};
constexpr size_t kC5Phases = 256, kC5Samples = 12;
inline void C5Add(C5Acc& a, const FrameRec& f, const Resolved& r) {
  ++a.frames;
  for (int e = 0; e < 2; ++e) {
    if (r.mainP[e] < 0 || r.mainF[e] < 0) continue;
    const uint8_t* P = f.gb[r.mainP[e]].tc;
    const uint8_t* F = f.gb[r.mainF[e]].tc;
    ++a.pairs;
    const float* fx = ftwin::TcRow(F, pep::kRowPx);
    const float* fy = ftwin::TcRow(F, pep::kRowPy);
    const float* px = ftwin::TcRow(P, pep::kRowPx);
    const float* py = ftwin::TcRow(P, pep::kRowPy);
    const float* ps = ftwin::TcRow(P, ftwin::kRowSize);
    const float* fs = ftwin::TcRow(F, ftwin::kRowSize);
    pep::FocusP own;
    own.sx = fx[0], own.cx = fx[2], own.sy = fy[1], own.cy = fy[2];
    uint8_t pred[64];
    pep::PredictFocusRows(P, own, pred);
    if (pep::VerifyFocusRows(pred, F) == 0) ++a.rowsExact;
    int first = -1;
    if (UnknownViewRows(P, F, &first) > 0) {
      ++a.unknownPairs;
      if (a.firstUnknownRow < 0) a.firstUnknownRow = first;
    }
    const uint32_t key[2] = {Bits(px[2]), Bits(py[2])};
    const uint32_t val[4] = {Bits(own.sx), Bits(own.cx), Bits(own.sy), Bits(own.cy)};
    C5Acc::Phase* ph = nullptr;
    for (C5Acc::Phase& q : a.table[e])
      if (q.key[0] == key[0] && q.key[1] == key[1]) ph = &q;
    if (ph) {
      ++a.lookups;
      if (memcmp(ph->val, val, sizeof(val)) == 0) ++a.hits;
      memcpy(ph->val, val, sizeof(val));
    } else if (a.table[e].size() < kC5Phases) {
      C5Acc::Phase q;
      memcpy(q.key, key, sizeof(key));
      memcpy(q.val, val, sizeof(val));
      a.table[e].push_back(q);
      if (a.samples.size() < kC5Samples) {
        char b[256];
        snprintf(b, sizeof(b),
                 "eye %d periphery sx %08x cx %08x sy %08x cy %08x size %.0fx%.0f | focus sx %08x cx %08x sy %08x cy "
                 "%08x size %.0fx%.0f",
                 e, Bits(px[0]), key[0], Bits(py[1]), key[1], ps[2], ps[3], val[0], val[1], val[2], val[3], fs[2], fs[3]);
        a.samples.push_back(b);
      }
    } else {
      ++a.tableFull;
    }
    if (a.havePrev[e]) {
      ++a.prevLookups;
      const float cur[4] = {own.sx, own.cx, own.sy, own.cy};
      if (memcmp(cur, a.prevF[e], sizeof(cur)) == 0) ++a.prevHits;
      if (Bits(own.sx) == Bits(a.prevF[e][0]) && Bits(own.sy) == Bits(a.prevF[e][2])) ++a.zoomSame;
      // The jitter step since the previous captured frame, in pixels of each view (centre = c0 + 2 j / size).
      const double dpx = (static_cast<double>(px[2]) - a.prevP[e][0]) * ps[2] / 2, dfx = (static_cast<double>(own.cx) - a.prevF[e][1]) * fs[2] / 2;
      const double dpy = (static_cast<double>(py[2]) - a.prevP[e][1]) * ps[3] / 2, dfy = (static_cast<double>(own.cy) - a.prevF[e][3]) * fs[3] / 2;
      const bool sameSize = ps[2] == a.prevSize[e][0] && ps[3] == a.prevSize[e][1] && fs[2] == a.prevSize[e][2] && fs[3] == a.prevSize[e][3];
      if (sameSize) a.jitterMaxPx = std::max(a.jitterMaxPx, std::max(std::fabs(dpx - dfx), std::fabs(dpy - dfy)));
    }
    a.havePrev[e] = true;
    a.prevP[e][0] = px[2], a.prevP[e][1] = py[2];
    a.prevF[e][0] = own.sx, a.prevF[e][1] = own.cx, a.prevF[e][2] = own.sy, a.prevF[e][3] = own.cy;
    a.prevSize[e][0] = ps[2], a.prevSize[e][1] = ps[3], a.prevSize[e][2] = fs[2], a.prevSize[e][3] = fs[3];
  }
}
inline double Pct(uint64_t a, uint64_t b) { return b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; }
// 1 pass, 0 fail, -1 not measured.
inline int C5Verdict(const C5Acc& a) {
  if (a.pairs < 100 || a.lookups < 100) return -1;
  return a.rowsExact == a.pairs && a.hits * 100 >= a.lookups * 99 ? 1 : 0;
}
inline const char* Word(int v) { return v > 0 ? "PASS" : v == 0 ? "FAIL" : "NOT MEASURED"; }
inline std::string C5Line(const C5Acc& a) {
  char b[768];
  snprintf(b, sizeof(b),
           "pep C5: frames %llu, eye pairs %llu | focus VP rows from the periphery's shared rows and the focus's own "
           "scalars: exact %.2f %% | rows differing outside the per-view list: %llu pairs (first row %d) | early "
           "source (periphery centre -> focus scalars): %zu + %zu phases, hit %.2f %% of %llu lookups (table full "
           "%llu) | equal to the previous frame %.2f %%, zoom equal %.2f %% | jitter step periphery vs focus: max "
           "%.4f px | fixed gaze, pass >= 99 %% and rows 100 %%: %s",
           static_cast<unsigned long long>(a.frames), static_cast<unsigned long long>(a.pairs), Pct(a.rowsExact, a.pairs),
           static_cast<unsigned long long>(a.unknownPairs), a.firstUnknownRow, a.table[0].size(), a.table[1].size(),
           Pct(a.hits, a.lookups), static_cast<unsigned long long>(a.lookups), static_cast<unsigned long long>(a.tableFull),
           Pct(a.prevHits, a.prevLookups), Pct(a.zoomSame, a.prevLookups), a.jitterMaxPx, Word(C5Verdict(a)));
  return b;
}

// Why a captured frame gave no periphery + focus pair (the first cause that applies), for the diagnostic line.
enum NoPair : int { kNpOk = 0, kNpNoPass, kNpNoTc, kNpNoPeriph, kNpNoFocus, kNpFocusNoEye, kNpCount };
inline const char* NoPairName(int w) {
  static const char* const n[kNpCount] = {"paired", "no G-buffer pass in the frame", "no TerrainContext at b11",
                                          "no periphery view (sx < 1.5, off-centre)", "no focus view (sx >= 1.5)",
                                          "a focus view shares no periphery's rows"};
  return w >= 0 && w < kNpCount ? n[w] : "?";
}
inline int WhyNoPair(const FrameRec& f, const Resolved& r) {
  if (r.mainP[0] >= 0 && r.mainF[0] >= 0 && r.mainP[1] >= 0 && r.mainF[1] >= 0) return kNpOk;
  if (!f.n) return kNpNoPass;
  int tc = 0, periph = 0, focus = 0;
  for (int i = 0; i < f.n; ++i) {
    tc += f.gb[i].tcOk;
    periph += r.kind[i] == ftwin::kPeriph;
    focus += r.kind[i] == ftwin::kFocus;
  }
  if (!tc) return kNpNoTc;
  if (!periph) return kNpNoPeriph;
  if (!focus) return kNpNoFocus;
  return kNpFocusNoEye;
}
struct Diag {
  uint64_t frames = 0, passes = 0, why[kNpCount] = {}, tier[4] = {};
  std::string sample;  // the P rows of the first TerrainContext of the first frame without a pair
};
inline void DiagAdd(Diag& d, const FrameRec& f, const Resolved& r) {
  const int w = WhyNoPair(f, r);
  ++d.frames;
  d.passes += static_cast<uint64_t>(f.n);
  ++d.why[w];
  ++d.tier[r.tier & 3];
  if (w == kNpOk || !d.sample.empty()) return;
  for (int i = 0; i < f.n; ++i) {
    if (!f.gb[i].tcOk) continue;
    uint32_t x[4], y[4], pw[4];
    memcpy(x, f.gb[i].tc + 16 * ftwin::kRowPx, 16);
    memcpy(y, f.gb[i].tc + 16 * ftwin::kRowPy, 16);
    memcpy(pw, f.gb[i].tc + 16 * ftwin::kRowPw, 16);
    const float* s = ftwin::TcRow(f.gb[i].tc, ftwin::kRowSize);
    char b[256];
    snprintf(b, sizeof(b), "pass %d row16 %08x %08x %08x %08x row17 %08x %08x %08x %08x row19 %08x %08x %08x %08x size %.0fx%.0f",
             f.gb[i].ord, x[0], x[1], x[2], x[3], y[0], y[1], y[2], y[3], pw[0], pw[1], pw[2], pw[3], s[2], s[3]);
    d.sample = b;
    return;
  }
}
inline std::string DiagLine(const Diag& d, uint64_t frameStarts, uint64_t gbPasses, uint64_t snaps) {
  char b[640];
  snprintf(b, sizeof(b),
           "pep live diag: %llu frame starts seen by pass structure, %llu top-level G-buffer passes, %llu "
           "TerrainContext snapshots; %llu frames read back with %.1f passes each: paired %llu",
           static_cast<unsigned long long>(frameStarts), static_cast<unsigned long long>(gbPasses),
           static_cast<unsigned long long>(snaps), static_cast<unsigned long long>(d.frames),
           d.frames ? static_cast<double>(d.passes) / static_cast<double>(d.frames) : 0.0,
           static_cast<unsigned long long>(d.why[kNpOk]));
  std::string s = b;
  snprintf(b, sizeof(b), " (focus to eye by: every shared row %llu, view rows 0-2 only %llu, frame order %llu)",
           static_cast<unsigned long long>(d.tier[kTierShared]), static_cast<unsigned long long>(d.tier[kTierView]),
           static_cast<unsigned long long>(d.tier[kTierOrder]));
  s += b;
  for (int w = kNpNoPass; w < kNpCount; ++w)
    if (d.why[w]) {
      snprintf(b, sizeof(b), ", %s %llu", NoPairName(w), static_cast<unsigned long long>(d.why[w]));
      s += b;
    }
  if (!frameStarts)
    s += " | CAUSE: no frame start (no top-level HeightMapPass / shadow cascade after a view): frames cannot be told apart";
  else if (!gbPasses)
    s += " | CAUSE: no top-level pass named GBufferPass";
  else if (!d.frames)
    s += " | CAUSE: no frame was read back (copies not ready, or no free set)";
  else if (!d.why[kNpOk])
    s += " | CAUSE: no frame had both eyes' periphery and focus views";
  if (!d.sample.empty()) s += " | first unpaired: " + d.sample;
  return s;
}

// The per-view rows of the pixel stage's b7 (cPerView) and b10, for pep::PatchPsViewSelect's lists: the first
// kCbMax bytes of both, as bound at the end of each G-buffer pass, periphery against focus of the same eye.
constexpr int kCbSlots = 2, kCbMax = 4096;
constexpr uint32_t kCbSlot[kCbSlots] = {7, 10};
struct CbSnap {
  uint32_t frame = 0;
  uint8_t ord = 0, slot = 0;  // slot: index into kCbSlot
  uint16_t bytes = 0;
  uint8_t data[kCbMax];
};
struct PsRowsAcc {
  uint64_t pairs[kCbSlots] = {}, sizeMismatch[kCbSlots] = {};
  uint32_t bytes[kCbSlots] = {};
  uint8_t comps[kCbSlots][kCbMax / 16] = {};  // per row: the components that differed in any pair
};
inline void PsRowsAdd(PsRowsAcc& a, int slot, const uint8_t* p, uint32_t pBytes, const uint8_t* f, uint32_t fBytes) {
  if (slot < 0 || slot >= kCbSlots || !pBytes) return;
  if (pBytes != fBytes) {
    ++a.sizeMismatch[slot];
    return;
  }
  ++a.pairs[slot];
  a.bytes[slot] = pBytes;
  for (uint32_t r = 0; r * 16 + 16 <= pBytes && r < kCbMax / 16; ++r)
    for (int k = 0; k < 4; ++k)
      if (memcmp(p + 16 * r + 4 * k, f + 16 * r + 4 * k, 4) != 0) a.comps[slot][r] |= static_cast<uint8_t>(1u << k);
}
inline std::string PsRowsLine(const PsRowsAcc& a, int slot) {
  char b[160];
  snprintf(b, sizeof(b), "pep C5 ps rows: b%u, first %u B, %llu periphery/focus pairs (%llu of different sizes): per-view rows",
           kCbSlot[slot], a.bytes[slot], static_cast<unsigned long long>(a.pairs[slot]),
           static_cast<unsigned long long>(a.sizeMismatch[slot]));
  std::string s = b;
  int n = 0;
  for (int r = 0; r < kCbMax / 16; ++r)
    if (a.comps[slot][r]) {
      snprintf(b, sizeof(b), " %d:%x", r, a.comps[slot][r]);
      s += b;
      ++n;
    }
  if (!a.pairs[slot]) return s + " not captured (no buffer at the slot, or no pair)";
  return n ? s + " (row:component mask)" : s + " none";
}

struct C4Acc {
  struct View {
    uint64_t passes = 0, order[4] = {};  // pep::MergeOrder
    double items = 0, merged = 0, first = 0, last = 0;
  } v[2];  // 0 periphery main, 1 focus main
  uint64_t notWalked = 0;
};
inline void C4Add(C4Acc& a, const FrameRec& f, const Resolved& r) {
  for (int e = 0; e < 2; ++e)
    for (int k = 0; k < 2; ++k) {
      const int i = k ? r.mainF[e] : r.mainP[e];
      if (i < 0) continue;
      const GbRec& g = f.gb[i];
      if (!g.items) {
        ++a.notWalked;
        continue;
      }
      C4Acc::View& v = a.v[k];
      ++v.passes;
      ++v.order[g.order & 3];
      v.items += g.items, v.merged += g.merged, v.first += g.firstMerged, v.last += g.lastMerged;
    }
}
inline int C4Verdict(const C4Acc& a) {
  const C4Acc::View& f = a.v[1];
  if (f.passes < 100) return -1;
  return (f.order[pep::kMoFirst] + f.order[pep::kMoLast]) * 100 >= f.passes * 95 ? 1 : 0;
}
inline std::string C4Line(const C4Acc& a) {
  char b[768];
  const char* names[2] = {"periphery", "focus"};
  std::string s = "pep C4:";
  for (int k = 1; k >= 0; --k) {
    const C4Acc::View& v = a.v[k];
    const double n = static_cast<double>(std::max<uint64_t>(1, v.passes));
    snprintf(b, sizeof(b),
             " %s main G-buffer passes %llu: items %.0f, terrain-family items %.1f at %.0f..%.0f | order first %.1f %% "
             "last %.1f %% mixed %.1f %% none %.1f %% |",
             names[k], static_cast<unsigned long long>(v.passes), v.items / n, v.merged / n, v.first / n, v.last / n,
             Pct(v.order[pep::kMoFirst], v.passes), Pct(v.order[pep::kMoLast], v.passes),
             Pct(v.order[pep::kMoMixed], v.passes), Pct(v.order[pep::kMoNone], v.passes));
    s += b;
  }
  snprintf(b, sizeof(b), " passes without an item vector %llu | focus first or last >= 95 %%: %s",
           static_cast<unsigned long long>(a.notWalked), Word(C4Verdict(a)));
  return s + b;
}

// C2: the front-end share from the blocks' focus main pass GPU ms (OFF blocks stock, ON blocks geometry only).
struct C2Out {
  double off = 0, on = 0, share = 0;
  int verdict = -1;
};
inline C2Out C2Share(const std::vector<double>& offMs, const std::vector<double>& onMs) {
  C2Out o;
  if (offMs.empty() || onMs.empty()) return o;
  for (double v : offMs) o.off += v / static_cast<double>(offMs.size());
  for (double v : onMs) o.on += v / static_cast<double>(onMs.size());
  if (!(o.off > 0)) return o;
  o.share = o.on / o.off;
  o.verdict = o.share >= 0.6 ? 1 : 0;
  return o;
}

// C2's terrain split. The terrain is one item of the pass (C4 live: Visualizer's TerrainObject, last of 640 in the
// focus main pass), and only its geometry can be merged; the models before it stay per view. Its GPU ms are timed
// as a nested "pass" in both arms: stock = the whole item, geometry only = its front end, the part a merged pass
// saves. Stage 3's gate is >= 0.30 ms GPU saved; the merge's copies cost about 0.11 ms (R53 live), so the terrain
// item's front end has to be >= 0.41 ms per frame (both eyes).
constexpr double kC2TerrainGateMs = 0.41;
struct C2TerrainOut {
  double stock = 0, frontEnd = 0;
  int verdict = -1;
};
inline C2TerrainOut C2Terrain(const std::vector<double>& offMs, const std::vector<double>& onMs) {
  C2TerrainOut o;
  if (offMs.empty() || onMs.empty()) return o;
  for (double v : offMs) o.stock += v / static_cast<double>(offMs.size());
  for (double v : onMs) o.frontEnd += v / static_cast<double>(onMs.size());
  if (!(o.stock > 0)) return o;
  o.verdict = o.frontEnd >= kC2TerrainGateMs ? 1 : 0;
  return o;
}

// C7, counts half, per frame of measure_kit.h's census. The view chains are the four chains of the frame that one
// pass object runs (TerrainModule). Which of the four are peripheries is read from their sizes (the two with the most
// instances), never from their place: the second live run (2026-11-11) paired by chain index over the frames with
// the modal chain count and failed for one eye only, with a paused scene whose per-chain means were mixtures (#3
// 74,635 against #4 88,825 instances): the layout of a frame with a given chain count is not fixed. Each focus is
// paired with the periphery of the same rank in frame order (first focus with first periphery). The layouts seen
// are counted and logged, so a wrong assumption shows.
struct C7Acc {
  uint64_t frames = 0, noFour = 0;
  uint64_t n[2] = {}, subset[2] = {}, maxOver[2] = {};
  double excess[2] = {};
  std::map<std::string, uint64_t> layouts;  // e.g. "CCH|PPFF": the other chains' passes, then the view chains by size
};
inline void C7Add(C7Acc& a, const mkit::CensusFrame& f, const std::function<char(void*)>& kindOf) {
  ++a.frames;
  const size_t n = f.counts.size();
  // The view chains: the chains run inside a TerrainModule pass (kind 'T'). Each view has its own pass object
  // (live 2026-11-11: four distinct objects, so "four chains of one object" never matched).
  int v[4] = {-1, -1, -1, -1};
  {
    int g[4], m = 0;
    for (size_t j = 0; j < n; ++j)
      if (kindOf && kindOf(f.pass[j]) == 'T') {
        if (m < 4) g[m] = static_cast<int>(j);
        ++m;
      }
    if (m == 4) memcpy(v, g, sizeof(v));
  }
  std::string sig;
  for (size_t i = 0; i < n; ++i)
    if (v[0] < 0 || (static_cast<int>(i) != v[0] && static_cast<int>(i) != v[1] && static_cast<int>(i) != v[2] && static_cast<int>(i) != v[3]))
      sig += kindOf ? kindOf(f.pass[i]) : '?';
  if (v[0] < 0) {
    ++a.noFour;
    ++a.layouts[sig + "|not four view chains"];
    return;
  }
  uint64_t tot[4] = {};
  for (int k = 0; k < 4; ++k)
    for (uint32_t x : f.counts[static_cast<size_t>(v[k])]) tot[k] += x;
  int big[4] = {0, 1, 2, 3};
  std::stable_sort(big, big + 4, [&](int x, int y) { return tot[x] > tot[y]; });
  bool isP[4] = {};
  isP[big[0]] = isP[big[1]] = true;
  int pi[2], fi[2], np = 0, nf = 0;
  sig += '|';
  for (int k = 0; k < 4; ++k) {
    sig += isP[k] ? 'P' : 'F';
    (isP[k] ? pi[np++] : fi[nf++]) = v[k];
  }
  ++a.layouts[sig];
  for (int e = 0; e < 2; ++e) {
    const std::vector<uint32_t>& cp = f.counts[static_cast<size_t>(pi[e])];
    const std::vector<uint32_t>& cf = f.counts[static_cast<size_t>(fi[e])];
    if (cp.size() != cf.size()) continue;
    uint64_t over = 0;
    double ex = 0;
    for (size_t k = 0; k < cp.size(); ++k)
      if (cf[k] > cp[k]) ++over, ex += cf[k] - cp[k];
    ++a.n[e];
    if (!over) ++a.subset[e];
    a.maxOver[e] = std::max(a.maxOver[e], over);
    a.excess[e] += ex;
  }
}
inline int C7Verdict(const C7Acc& a) {
  if (a.n[0] < 25 || a.n[1] < 25) return -1;
  return a.subset[0] == a.n[0] && a.subset[1] == a.n[1] ? 1 : 0;
}
inline std::string C7Line(const C7Acc& a) {
  char b[768];
  snprintf(b, sizeof(b),
           "pep C7: %llu frames, %llu without exactly four view chains; focus instance counts <= its periphery's for "
           "every object: first eye %llu of %llu frames (worst %llu objects over, %.1f extra instances/frame), second "
           "%llu of %llu (worst %llu, %.1f) | counts contained in every frame: %s | instance order: not comparable "
           "(stock orders an object's instances by racing atomics, R34 4.1), so merged instancer draws are exact only "
           "up to that order",
           static_cast<unsigned long long>(a.frames), static_cast<unsigned long long>(a.noFour),
           static_cast<unsigned long long>(a.subset[0]), static_cast<unsigned long long>(a.n[0]),
           static_cast<unsigned long long>(a.maxOver[0]), a.n[0] ? a.excess[0] / static_cast<double>(a.n[0]) : 0.0,
           static_cast<unsigned long long>(a.subset[1]), static_cast<unsigned long long>(a.n[1]),
           static_cast<unsigned long long>(a.maxOver[1]), a.n[1] ? a.excess[1] / static_cast<double>(a.n[1]) : 0.0,
           Word(C7Verdict(a)));
  return b;
}
// The layouts, most frequent first (at most `most` lines).
inline std::vector<std::string> C7Layouts(const C7Acc& a, size_t most = 6) {
  std::vector<std::pair<uint64_t, std::string>> v;
  for (const auto& kv : a.layouts) v.emplace_back(kv.second, kv.first);
  std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.first != y.first ? x.first > y.first : x.second < y.second; });
  std::vector<std::string> out;
  for (size_t i = 0; i < v.size() && i < most; ++i)
    out.push_back("pep C7 layout: " + std::to_string(v[i].first) + " frames " + v[i].second);
  return out;
}

#ifndef DCSQV_PEP_NO_RUNTIME
// ---------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------
constexpr int kSets = 6, kRing = 64, kSteadySec = 3, kMaxCls = 64;
std::atomic<bool> g_capture{false};   // TC snapshots (C4 / C5 / C2's learning)
std::atomic<bool> g_walk{false};      // C4 item walk
std::atomic<bool> g_release{false}, g_released{false};
std::atomic<bool> g_fault{false};
std::atomic<uint64_t> g_noTc{0}, g_noSet{0}, g_copyFail{0}, g_ringFull{0}, g_snaps{0}, g_frameStarts{0}, g_gbPasses{0};
// C2: the frame-order numbers (among top-level G-buffer passes) of the two focus main passes, -1 unknown.
std::atomic<int> g_c2Ord[2]{{-1}, {-1}};
std::atomic<bool> g_c2On{false};
std::atomic<uint64_t> g_c2Passes{0}, g_c2Vps{0}, g_c2Missed{0}, g_c2Buried{0};

struct Set {
  uint32_t frame = 0;
  int n = 0;
  bool pending = false;
  ID3D11Buffer* st[kMaxGb] = {};
  GbRec meta[kMaxGb];  // tc filled at the read
  ID3D11Buffer* stc[kMaxGb][kCbSlots] = {};  // pixel-stage b7 / b10 (C5's per-view row list)
  uint16_t cbBytes[kMaxGb][kCbSlots] = {};
  std::vector<uint8_t> cb;                   // kMaxGb * kCbSlots * kCbMax, sized by the phase
};
std::vector<CbSnap> g_cbRing;  // with g_ring, under g_ringMu
size_t g_cbN = 0;
std::atomic<bool> g_psRows{false};
Set g_sets[kSets];
int g_cur = -1;
mkit::FrameTrack g_track;  // frames by pass structure (ptiming::g_execFrame is constant live)
int g_gbOrd = 0;
bool g_inGb = false;
GbRec g_meta;
std::mutex g_ringMu;
std::vector<FrameRec> g_ring;  // sized by the phase; the render thread only writes slots
size_t g_ringN = 0;

// Item classes by vtable (render thread; read by the suite thread after the phase).
struct Cls {
  void* vt = nullptr;
  bool merged = false;
  uint64_t count = 0;
  char name[96] = {};
};
Cls g_cls[kMaxCls];
int g_clsN = 0;
HMODULE g_terrainDll = nullptr;

bool ReadVt(void* item, void** vt) {
  __try {
    *vt = *static_cast<void**>(item);
    return *vt != nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
int ClassOf(void* item) {
  void* vt = nullptr;
  if (!item || !ReadVt(item, &vt)) return -1;
  for (int i = 0; i < g_clsN; ++i)
    if (g_cls[i].vt == vt) return i;
  if (g_clsN >= kMaxCls) return -1;
  Cls& c = g_cls[g_clsN];
  c = Cls();
  c.vt = vt;
  ptiming::RttiRaw(item, c.name, sizeof(c.name));
  HMODULE mod = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     static_cast<const wchar_t*>(vt), &mod);
  c.merged = (g_terrainDll && mod == g_terrainDll) || strstr(c.name, "Terrain") != nullptr;
  return g_clsN++;
}
bool ReadVec(void** vec, void* const** b, void* const** e) {
  __try {
    *b = static_cast<void* const*>(vec[0]);
    *e = static_cast<void* const*>(vec[1]);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
// gbpass's second observer: the item vector, before the pass runs.
void WalkBody(void* pass, void* ctx) {
  void** vec = gbpass::ItemVector(pass, ctx);
  void* const* b = nullptr;
  void* const* e = nullptr;
  if (!vec || !ReadVec(vec, &b, &e) || !b || e <= b) return;
  const size_t n = static_cast<size_t>(e - b);
  if (n > 32768 || n <= g_meta.items) return;  // a second, smaller execution inside the same pass: the larger one counts
  static uint8_t flags[32768];
  uint32_t merged = 0, first = 0, last = 0;
  for (size_t i = 0; i < n; ++i) {
    void* item = nullptr;
    __try {
      item = b[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      return;
    }
    const int c = ClassOf(item);
    const bool m = c >= 0 && g_cls[c].merged;
    if (c >= 0) ++g_cls[c].count;
    flags[i] = m ? 1 : 0;
    if (m) {
      if (!merged) first = static_cast<uint32_t>(i);
      last = static_cast<uint32_t>(i);
      ++merged;
    }
  }
  g_meta.items = static_cast<uint32_t>(n);
  g_meta.merged = merged, g_meta.firstMerged = first, g_meta.lastMerged = last;
  g_meta.order = static_cast<uint8_t>(pep::ClassifyOrder(flags, n));
}
void ObsGb(void* pass, void* ctx, bool begin) {
  if (!begin || !g_inGb || !g_walk.load(std::memory_order_relaxed) || g_fault.load(std::memory_order_relaxed)) return;
  if (GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed)) return;
  __try {
    WalkBody(pass, ctx);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_fault = true;
  }
}

void ReleaseSets() {
  for (Set& s : g_sets) {
    for (ID3D11Buffer*& b : s.st)
      if (b) b->Release(), b = nullptr;
    for (auto& row : s.stc)
      for (ID3D11Buffer*& b : row)
        if (b) b->Release(), b = nullptr;
    s.n = 0, s.pending = false;
  }
  g_cur = -1;
}
void Publish(const Set& s) {
  std::lock_guard<std::mutex> lock(g_ringMu);
  if (g_ringN >= g_ring.size()) {
    g_ringFull.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  FrameRec& f = g_ring[g_ringN++];
  f.frame = s.frame;
  f.n = s.n;
  for (int i = 0; i < s.n; ++i) f.gb[i] = s.meta[i];
  for (int i = 0; i < s.n; ++i)
    for (int j = 0; j < kCbSlots; ++j) {
      if (!s.cbBytes[i][j] || g_cbN >= g_cbRing.size() || s.cb.empty()) continue;
      CbSnap& cs = g_cbRing[g_cbN++];
      cs.frame = s.frame, cs.ord = s.meta[i].ord, cs.slot = static_cast<uint8_t>(j), cs.bytes = s.cbBytes[i][j];
      memcpy(cs.data, &s.cb[(static_cast<size_t>(i) * kCbSlots + static_cast<size_t>(j)) * kCbMax], cs.bytes);
    }
}
// Reads the finished sets whose copies are ready (never waits).
void Poll(ID3D11DeviceContext* c) {
  for (Set& s : g_sets) {
    if (!s.pending) continue;
    bool ready = true;
    for (int i = 0; i < s.n && ready; ++i) {
      if (!s.meta[i].tcOk || !s.st[i]) continue;
      D3D11_MAPPED_SUBRESOURCE m{};
      if (FAILED(c->Map(s.st[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
        ready = false;
        break;
      }
      memcpy(s.meta[i].tc, m.pData, pep::kTcBytes);
      c->Unmap(s.st[i], 0);
    }
    for (int i = 0; i < s.n && ready; ++i)
      for (int j = 0; j < kCbSlots && ready; ++j) {
        if (!s.cbBytes[i][j] || !s.stc[i][j] || s.cb.empty()) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(c->Map(s.stc[i][j], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
          ready = false;
          break;
        }
        memcpy(&s.cb[(static_cast<size_t>(i) * kCbSlots + static_cast<size_t>(j)) * kCbMax], m.pData, s.cbBytes[i][j]);
        c->Unmap(s.stc[i][j], 0);
      }
    if (!ready) continue;
    s.pending = false;
    Publish(s);
  }
}
// The TerrainContext bound to the vertex stage at the pass end, into the set's next staging buffer.
void Snapshot(ID3D11DeviceContext* c) {
  if (g_cur < 0) return;
  Set& s = g_sets[g_cur];
  if (s.n >= kMaxGb) return;
  GbRec& m = s.meta[s.n];
  m.ord = g_meta.ord, m.order = g_meta.order, m.items = g_meta.items, m.merged = g_meta.merged;
  m.firstMerged = g_meta.firstMerged, m.lastMerged = g_meta.lastMerged;
  m.tcOk = false;
  ID3D11Buffer* b = nullptr;
  c->VSGetConstantBuffers(11, 1, &b);
  if (b) {
    D3D11_BUFFER_DESC d{};
    b->GetDesc(&d);
    if (d.ByteWidth == static_cast<UINT>(pep::kTcBytes)) {
      if (!s.st[s.n]) {
        ID3D11Device* dev = nullptr;
        c->GetDevice(&dev);
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = static_cast<UINT>(pep::kTcBytes);
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (dev) {
          if (FAILED(dev->CreateBuffer(&sd, nullptr, &s.st[s.n]))) s.st[s.n] = nullptr;
          dev->Release();
        }
      }
      if (s.st[s.n]) {
        c->CopyResource(s.st[s.n], b);
        m.tcOk = true;
        g_snaps.fetch_add(1, std::memory_order_relaxed);
      } else {
        g_copyFail.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      g_noTc.fetch_add(1, std::memory_order_relaxed);
    }
    b->Release();
  } else {
    g_noTc.fetch_add(1, std::memory_order_relaxed);
  }
  // The pixel stage's b7 / b10 (their first kCbMax bytes), for the per-view row list of the pixel-shader clone.
  for (int j = 0; j < kCbSlots; ++j) {
    s.cbBytes[s.n][j] = 0;
    if (!g_psRows.load(std::memory_order_relaxed) || s.cb.empty()) continue;
    ID3D11Buffer* pb = nullptr;
    c->PSGetConstantBuffers(kCbSlot[j], 1, &pb);
    if (!pb) continue;
    D3D11_BUFFER_DESC d{};
    pb->GetDesc(&d);
    const UINT take = std::min<UINT>(d.ByteWidth, kCbMax) & ~15u;
    if (take && !s.stc[s.n][j]) {
      ID3D11Device* dev = nullptr;
      c->GetDevice(&dev);
      D3D11_BUFFER_DESC sd{};
      sd.ByteWidth = kCbMax;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (dev) {
        if (FAILED(dev->CreateBuffer(&sd, nullptr, &s.stc[s.n][j]))) s.stc[s.n][j] = nullptr;
        dev->Release();
      }
    }
    if (take && s.stc[s.n][j]) {
      const D3D11_BOX box{0, 0, 0, take, 1, 1};
      c->CopySubresourceRegion(s.stc[s.n][j], 0, 0, 0, 0, pb, 0, &box);
      s.cbBytes[s.n][j] = static_cast<uint16_t>(take);
    }
    pb->Release();
  }
  ++s.n;
}

// --- C2: the viewport replacement inside a focus main pass (R53's FocusGeomProbe) ---
using VpFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
bool g_vpActive = false;
void** g_vpTable = nullptr;
VpFn g_vpNext = nullptr, g_vpD3d = nullptr;
D3D11_VIEWPORT g_vpReq[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
UINT g_vpReqN = 0;
bool g_vpReqSeen = false;
thread_local int t_vpIn = 0;
int VpSlot() {
  static int s = DcsQv_ContextSlot("RSSetViewports");
  return s;
}
void STDMETHODCALLTYPE VpHook(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v) {
  VpFn next = g_vpNext;
  if (t_vpIn > 0) next = g_vpD3d;  // a cycle through another hook that took ours as its original: straight to d3d11
  if (!next) return;
  if (!g_vpActive || c != plod::g_ctx || GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed) ||
      !n || !v) {
    ++t_vpIn;
    next(c, n, v);
    --t_vpIn;
    return;
  }
  const UINT m = std::min<UINT>(n, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
  memcpy(g_vpReq, v, m * sizeof(D3D11_VIEWPORT));
  g_vpReqN = m;
  g_vpReqSeen = true;
  D3D11_VIEWPORT off[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  for (UINT i = 0; i < m; ++i) off[i] = D3D11_VIEWPORT{-64.0f, -64.0f, 16.0f, 16.0f, v[i].MinDepth, v[i].MaxDepth};
  g_c2Vps.fetch_add(1, std::memory_order_relaxed);
  ++t_vpIn;
  next(c, m, off);
  --t_vpIn;
}
void VpArm() {
  const int s = VpSlot();
  if (s < 0) return;
  void** vt = *reinterpret_cast<void***>(plod::g_ctx);
  void* cur = vt[s];
  if (cur != reinterpret_cast<void*>(&VpHook)) {
    // The depth publisher's hook on top whose original is ours already: we are in its chain; not wrapped again.
    const bool chained = cur == reinterpret_cast<void*>(&fixdepth::VpHook) &&
                         reinterpret_cast<void*>(fixdepth::g_vpOrig) == reinterpret_cast<void*>(&VpHook);
    if (!chained) {
      if (cur != reinterpret_cast<void*>(&fixdepth::VpHook)) g_vpD3d = reinterpret_cast<VpFn>(cur);
      g_vpNext = reinterpret_cast<VpFn>(cur);
      g_vpTable = vt;
      d3ds::WriteTablePointer(&vt[s], reinterpret_cast<void*>(&VpHook));
    }
  }
  if (!g_vpD3d) g_vpD3d = g_vpNext;
  UINT n = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  plod::g_ctx->RSGetViewports(&n, g_vpReq);  // bound before the pass (DCS may set none inside it): saved, replaced
  g_vpReqN = n;
  g_vpActive = true;
  if (n) {
    D3D11_VIEWPORT pre[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    memcpy(pre, g_vpReq, n * sizeof(D3D11_VIEWPORT));
    VpHook(plod::g_ctx, n, pre);
  }
  g_vpReqSeen = false;
  g_c2Passes.fetch_add(1, std::memory_order_relaxed);
}
void VpDisarm() {
  if (!g_vpActive) return;
  g_vpActive = false;
  if (g_vpReqN && g_vpNext) {  // stock state for whatever follows: DCS's last request
    ++t_vpIn;
    g_vpNext(plod::g_ctx, g_vpReqN, g_vpReq);
    --t_vpIn;
  }
  if (!g_vpReqSeen) g_c2Missed.fetch_add(1, std::memory_order_relaxed);
  const int s = VpSlot();
  if (g_vpTable && s >= 0) {
    if (g_vpTable[s] == reinterpret_cast<void*>(&VpHook)) {
      d3ds::WriteTablePointer(&g_vpTable[s], reinterpret_cast<void*>(g_vpNext));
      g_vpTable = nullptr;
    } else {
      g_c2Buried.fetch_add(1, std::memory_order_relaxed);  // another hook went on top: ours stays a pass-through
    }
  }
}
// Suite thread, after the phase: our hook out of the table or out of the depth publisher's chain.
void VpUnwrap() {
  if (!plod::g_ctx || !g_vpNext) return;
  const int s = VpSlot();
  if (s < 0) return;
  void** vt = *reinterpret_cast<void***>(plod::g_ctx);
  if (vt[s] == reinterpret_cast<void*>(&VpHook)) d3ds::WriteTablePointer(&vt[s], reinterpret_cast<void*>(g_vpNext));
  if (reinterpret_cast<void*>(fixdepth::g_vpOrig) == reinterpret_cast<void*>(&VpHook))
    fixdepth::g_vpOrig = reinterpret_cast<fixdepth::VpFn>(g_vpNext);
  g_vpTable = nullptr;
}

// --- C2: the terrain item of the main passes as a nested "pass" of the GPU timing ---
// gb_rec_count.h's pattern: a forwarding thunk on the item class's vt[1] (render), only while C2 runs and only
// when the slot holds DCS's own function. Inside a focus / periphery main G-buffer pass the item is bracketed by
// the GPU pass timing's boundary callback with a stand-in pass object (never dereferenced: it is a map key and a
// name through PassName).
using ItemFn = uint64_t(__fastcall*)(void*, void*, void*, void*);
void** g_tiSlot = nullptr;
void* volatile g_tiOrig = nullptr;
std::atomic<bool> g_tiOn{false};
std::atomic<int> g_tiOrdP[2]{{-1}, {-1}};
std::atomic<uint64_t> g_tiCalls{0};
uint8_t g_tiPassF[64] = {}, g_tiPassP[64] = {};  // the stand-in pass objects
uint64_t __fastcall TerrainItemHook(void* a, void* b, void* c, void* d) {
  const ItemFn orig = reinterpret_cast<ItemFn>(g_tiOrig);
  void* tag = nullptr;
  if (g_tiOn.load(std::memory_order_relaxed) && g_inGb &&
      GetCurrentThreadId() == ptiming::g_topTid.load(std::memory_order_relaxed)) {
    if (g_gbOrd == g_c2Ord[0].load(std::memory_order_relaxed) || g_gbOrd == g_c2Ord[1].load(std::memory_order_relaxed))
      tag = g_tiPassF;
    else if (g_gbOrd == g_tiOrdP[0].load(std::memory_order_relaxed) || g_gbOrd == g_tiOrdP[1].load(std::memory_order_relaxed))
      tag = g_tiPassP;
  }
  const ptiming::BoundaryFn bf = tag ? ptiming::g_boundary.load(std::memory_order_relaxed) : nullptr;
  if (!bf) return orig(a, b, c, d);
  g_tiCalls.fetch_add(1, std::memory_order_relaxed);
  bf(tag, true, 1);
  const uint64_t r = orig(a, b, c, d);
  bf(tag, false, 1);
  return r;
}
// Suite thread. The terrain item's class from the walk (the terrain-family class with the most items).
bool HookTerrainItem() {
  int best = -1;
  for (int i = 0; i < g_clsN; ++i)
    if (g_cls[i].merged && (best < 0 || g_cls[i].count > g_cls[best].count)) best = i;
  if (best < 0 || !g_cls[best].vt) return false;
  void** slot = &static_cast<void**>(g_cls[best].vt)[1];
  if (*slot != SlotOriginal(slot)) return false;  // another module's hook: never touched
  g_tiOrig = *slot;
  if (!HookSlot(slot, reinterpret_cast<void*>(&TerrainItemHook), nullptr)) return false;
  g_tiSlot = slot;
  return true;
}
void UnhookTerrainItem() {
  g_tiOn = false;
  if (!g_tiSlot) return;
  UnhookSlot(g_tiSlot, g_tiOrig);  // g_tiOrig stays for calls in flight
  g_tiSlot = nullptr;
}

void BoundaryBody(void* pass, bool begin) {
  ID3D11DeviceContext* c = plod::g_ctx;
  if (g_release.load(std::memory_order_relaxed)) {
    if (!g_released.load(std::memory_order_relaxed)) {
      VpDisarm();
      ReleaseSets();
      g_released = true;
    }
    return;
  }
  const bool cap = g_capture.load(std::memory_order_relaxed);
  const int kind = plod::KindOf(pass);
  // A capture starts with a whole frame: nothing is kept before the first frame start it sees.
  if (g_track.OnTop(kind, begin)) {
    g_frameStarts.fetch_add(1, std::memory_order_relaxed);
    const uint64_t frame = g_track.serial;
    if (g_cur >= 0) g_sets[g_cur].pending = g_sets[g_cur].n > 0;
    g_cur = -1;
    g_gbOrd = 0;
    g_inGb = false;
    Poll(c);
    if (cap) {
      for (int s = 0; s < kSets && g_cur < 0; ++s)
        if (!g_sets[s].pending) g_cur = s;
      if (g_cur < 0) {
        g_noSet.fetch_add(1, std::memory_order_relaxed);
      } else {
        g_sets[g_cur].n = 0;
        g_sets[g_cur].frame = static_cast<uint32_t>(frame);
      }
    }
  }
  if (kind != plod::kGBuffer) return;
  if (begin) {
    g_gbPasses.fetch_add(1, std::memory_order_relaxed);
    g_meta = GbRec();
    g_meta.ord = static_cast<uint8_t>(std::min(g_gbOrd, 255));
    g_inGb = true;
    if (g_c2On.load(std::memory_order_relaxed) &&
        (g_gbOrd == g_c2Ord[0].load(std::memory_order_relaxed) || g_gbOrd == g_c2Ord[1].load(std::memory_order_relaxed)))
      VpArm();
  } else {
    VpDisarm();
    if (g_inGb && cap) Snapshot(c);
    g_inGb = false;
    ++g_gbOrd;
  }
}
// plod's boundary chain (render thread only; every pass, nested ones too).
void OnBoundary(void* pass, bool begin, int depth) {
  if (depth != 0 || !plod::g_ctx || g_fault.load(std::memory_order_relaxed)) return;
  __try {
    BoundaryBody(pass, begin);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_fault = true;
    g_vpActive = false;
  }
}

// ---------------------------------------------------------------------------
// Suite thread
// ---------------------------------------------------------------------------
struct Keys {
  int c2 = 0, c4 = 0, c5 = 0, c7 = 0, logSec = 10;
};
inline Keys ReadKeys(const wchar_t* ini) {
  Keys k;
  auto sec = [&](const wchar_t* name) {
    return std::max(0, std::min(3600, static_cast<int>(GetPrivateProfileIntW(L"Suite", name, 0, ini))));
  };
  k.c2 = sec(L"PerEyeC2"), k.c4 = sec(L"PerEyeC4"), k.c5 = sec(L"PerEyeC5"), k.c7 = sec(L"PerEyeC7");
  k.logSec = std::max(1, std::min(600, static_cast<int>(GetPrivateProfileIntW(L"Suite", L"PerEyeLogSec", 10, ini))));
  return k;
}
Keys g_keys;
void ReadConfig(const wchar_t* ini) { g_keys = ReadKeys(ini); }  // LoadConfig, every second
bool g_ownGbHook = false, g_haveWalk = false;

std::vector<FrameRec> Drain(std::vector<CbSnap>* cbs = nullptr) {
  std::vector<FrameRec> out;
  std::lock_guard<std::mutex> lock(g_ringMu);
  out.assign(g_ring.begin(), g_ring.begin() + static_cast<std::ptrdiff_t>(g_ringN));
  g_ringN = 0;
  if (cbs) cbs->assign(g_cbRing.begin(), g_cbRing.begin() + static_cast<std::ptrdiff_t>(g_cbN));
  g_cbN = 0;
  return out;
}
// The pixel-stage buffers of a frame's main passes, periphery against focus.
void PsRowsOfFrame(PsRowsAcc& a, const FrameRec& f, const Resolved& r, const std::vector<CbSnap>& cbs) {
  for (int e = 0; e < 2; ++e) {
    if (r.mainP[e] < 0 || r.mainF[e] < 0) continue;
    for (int j = 0; j < kCbSlots; ++j) {
      const CbSnap* p = nullptr;
      const CbSnap* q = nullptr;
      for (const CbSnap& c : cbs) {
        if (c.frame != f.frame || c.slot != j) continue;
        if (c.ord == f.gb[r.mainP[e]].ord) p = &c;
        if (c.ord == f.gb[r.mainF[e]].ord) q = &c;
      }
      if (p && q) PsRowsAdd(a, j, p->data, p->bytes, q->data, q->bytes);
    }
  }
}
bool Steady(std::atomic<uint64_t>& frames) {
  int run = 0;
  for (int t = 0; t < 30 && run < kSteadySec && !g_benchAbort; ++t) {
    const uint64_t f0 = frames.load();
    Sleep(1000);
    run = frames.load() - f0 >= 20 ? run + 1 : 0;
  }
  return run >= kSteadySec;
}
bool StartCapture(bool walk) {
  if (!plod::InstallCallback()) {
    Log("pep live: pass boundary callback not available; skipped");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(g_ringMu);
    g_ring.assign(kRing, FrameRec());
    g_ringN = 0;
    g_cbRing.assign(g_psRows.load() ? 1024 : 0, CbSnap());
    g_cbN = 0;
    for (Set& s : g_sets) s.cb.assign(g_psRows.load() ? static_cast<size_t>(kMaxGb) * kCbSlots * kCbMax : 0, 0);
  }
  g_haveWalk = false;
  if (walk) {
    g_terrainDll = GetModuleHandleW(L"edterrainGraphics41.dll");
    g_ownGbHook = gbpass::g_state == 0;
    gbpass::ObsFn none = nullptr;
    if (gbpass::Install() && gbpass::g_obs2.compare_exchange_strong(none, &ObsGb))
      g_haveWalk = true;
    else
      Log("pep live: the G-buffer pass hook or its observer is not available (another feature holds it); C4 not "
          "measured");
  }
  g_release = false, g_released = false;
  g_frameStarts = g_gbPasses = g_snaps = g_noTc = g_noSet = g_copyFail = g_ringFull = 0;
  plod::BFn none = nullptr;
  if (!plod::g_probe.compare_exchange_strong(none, &OnBoundary)) {
    Log("pep live: the boundary probe slot is taken; skipped");
    return false;
  }
  g_walk = g_haveWalk;
  g_capture = true;
  return true;
}
void StopCapture() {
  g_capture = false;
  g_walk = false;
  g_c2On = false;
  Sleep(100);
  g_release = true;  // the render thread releases its staging buffers at its next pass boundary
  for (int i = 0; i < 100 && !g_released.load(); ++i) Sleep(10);
  plod::BFn me = &OnBoundary;
  plod::g_probe.compare_exchange_strong(me, nullptr);
  gbpass::ObsFn obs = &ObsGb;
  gbpass::g_obs2.compare_exchange_strong(obs, nullptr);
  if (g_ownGbHook && !g_cfg.gbufferBatch) gbpass::Uninstall();
  g_ownGbHook = false;
  Sleep(50);
  VpUnwrap();
  if (!g_released.load()) Log("pep live: the render thread did not release the staging buffers (no pass since); kept");
}
void LogCounters() {
  Log("pep live: TerrainContext snapshots %llu, passes without it at b11 %llu, frames without a free set %llu, "
      "copy failures %llu, frames dropped (queue full) %llu%s",
      static_cast<unsigned long long>(g_snaps.load()), static_cast<unsigned long long>(g_noTc.load()),
      static_cast<unsigned long long>(g_noSet.load()), static_cast<unsigned long long>(g_copyFail.load()),
      static_cast<unsigned long long>(g_ringFull.load()), g_fault.load() ? "; FAULT: captures latched off" : "");
}

// C4 / C5 over one window.
void CapturePhase(const Keys& k) {
  const int seconds = std::max(k.c4, k.c5);
  C5Acc a5;
  C4Acc a4;
  Diag dg;
  bool diagDone = false;
  PsRowsAcc pr;
  std::vector<CbSnap> cbs;
  g_psRows = k.c5 > 0;
  if (!StartCapture(k.c4 > 0)) {
    g_psRows = false;
    StopCapture();
    return;
  }
  const ULONGLONG t0 = GetTickCount64();
  ULONGLONG next = t0 + static_cast<ULONGLONG>(k.logSec) * 1000;
  auto lines = [&] {
    if (k.c5 > 0) Log("%s", C5Line(a5).c_str());
    if (k.c4 > 0 && g_haveWalk) Log("%s", C4Line(a4).c_str());
  };
  while (GetTickCount64() - t0 < static_cast<ULONGLONG>(seconds) * 1000 && !g_benchAbort && !g_fault.load()) {
    Sleep(100);
    for (const FrameRec& f : Drain(&cbs)) {
      const Resolved r = Resolve(f);
      DiagAdd(dg, f, r);
      if (k.c5 > 0) C5Add(a5, f, r);
      if (k.c5 > 0) PsRowsOfFrame(pr, f, r, cbs);
      if (k.c4 > 0) C4Add(a4, f, r);
    }
    if (!diagDone && GetTickCount64() - t0 >= 3000) {  // once: what the capture sees, and the cause when it sees nothing
      diagDone = true;
      Log("%s", DiagLine(dg, g_frameStarts.load(), g_gbPasses.load(), g_snaps.load()).c_str());
    }
    if (GetTickCount64() >= next) {
      lines();
      next += static_cast<ULONGLONG>(k.logSec) * 1000;
    }
  }
  StopCapture();
  g_psRows = false;
  lines();
  if (k.c5 > 0) {
    for (const std::string& s : a5.samples) Log("pep C5 sample: %s", s.c_str());
    for (int j = 0; j < kCbSlots; ++j) Log("%s", PsRowsLine(pr, j).c_str());
  }
  if (k.c4 > 0 && g_haveWalk) {
    std::vector<int> order;
    for (int i = 0; i < g_clsN; ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [](int x, int y) { return g_cls[x].count > g_cls[y].count; });
    for (size_t i = 0; i < order.size() && i < 12; ++i)
      Log("pep C4 class: %llu items, %s, %s", static_cast<unsigned long long>(g_cls[order[i]].count),
          g_cls[order[i]].merged ? "terrain family" : "other", g_cls[order[i]].name);
  }
  LogCounters();
}

std::string PassName(void* p) {
  if (p == g_tiPassF) return "pepTerrainItemFocus";
  if (p == g_tiPassP) return "pepTerrainItemPeriphery";
  return ptiming::RttiName(p);
}
double KindMs(gpt::Result& r, const char* name) {
  const auto it = r.kinds.find(name);
  return it == r.kinds.end() ? -1.0 : gpt::Mean(it->second.perFrame);
}
// The mean exclusive GPU ms per frame of the G-buffer kind's calls `ord[0..1]` (frame order), or -1.
double GbCallsMs(gpt::Result& r, const int ord[2], int expectCalls, int* calls) {
  for (auto& kv : r.kinds) {
    if (kv.first.find("GBufferPass") == std::string::npos) continue;
    const gpt::KindStat& s = kv.second;
    *calls = static_cast<int>(s.occMs.size());
    if (*calls != expectCalls) continue;  // another kind with the name in it
    double ms = 0;
    for (int e = 0; e < 2; ++e) {
      if (ord[e] < 0 || ord[e] >= *calls || !s.occN[static_cast<size_t>(ord[e])]) return -1;
      ms += s.occMs[static_cast<size_t>(ord[e])] / static_cast<double>(s.occN[static_cast<size_t>(ord[e])]);
    }
    return ms;
  }
  return -1;
}
void C2Phase(const Keys& k, std::atomic<uint64_t>& frames) {
  if (VpSlot() < 0) {
    Log("pep C2: RSSetViewports slot unknown; NOT MEASURED");
    return;
  }
  if (!StartCapture(true)) {  // with the item walk: it finds the terrain item's class
    StopCapture();
    return;
  }
  // Learn which G-buffer executions are the focus views' main passes (2 s of TerrainContext snapshots).
  int ord[2] = {-1, -1}, ordP[2] = {-1, -1}, gbCalls = 0, agree = 0, seen = 0;
  Diag dg;
  for (int t = 0; t < 30 && seen < 30 && !g_benchAbort; ++t) {
    Sleep(100);
    for (const FrameRec& f : Drain()) {
      const Resolved r = Resolve(f);
      DiagAdd(dg, f, r);
      if (r.mainF[0] < 0 || r.mainF[1] < 0 || r.mainP[0] < 0 || r.mainP[1] < 0) continue;
      const int o[2] = {f.gb[r.mainF[0]].ord, f.gb[r.mainF[1]].ord};
      ++seen;
      if (o[0] == ord[0] && o[1] == ord[1] && f.n == gbCalls) ++agree;
      ord[0] = o[0], ord[1] = o[1], gbCalls = f.n;
      ordP[0] = f.gb[r.mainP[0]].ord, ordP[1] = f.gb[r.mainP[1]].ord;
    }
  }
  g_capture = false;
  Log("%s", DiagLine(dg, g_frameStarts.load(), g_gbPasses.load(), g_snaps.load()).c_str());
  if (seen < 10 || agree * 10 < seen * 9) {
    Log("pep C2: the focus main passes were not found at a stable place in the frame (%d frames, %d agreeing); NOT "
        "MEASURED", seen, agree);
    StopCapture();
    LogCounters();
    return;
  }
  g_c2Ord[0] = ord[0], g_c2Ord[1] = ord[1];
  g_tiOrdP[0] = ordP[0], g_tiOrdP[1] = ordP[1];
  g_walk = false;
  g_tiCalls = 0;
  const bool item = g_haveWalk && HookTerrainItem();
  if (!item) Log("pep C2: the terrain item cannot be timed on its own (no item walk, or its render slot is hooked by another feature)");
  g_tiOn = item;
  std::vector<double> offT, onT, offTp, onTp;
  g_c2Passes = g_c2Vps = g_c2Missed = 0;
  const int blockMs = std::max(2000, k.c2 * 1000 / 5);
  std::vector<double> offF, onF, offP, onP;
  int badBlocks = 0;
  for (int i = 0; i < 5 && !g_benchAbort && !g_fault.load(); ++i) {
    const bool on = (i & 1) != 0;
    g_c2On = on;
    Sleep(500);
    const bool began = gpt::Begin(frames, false);
    for (int t = 0; began && t < blockMs / 100 && !g_benchAbort; ++t) Sleep(100);
    gpt::End();
    std::vector<gpt::Done> done = gpt::TakeDone();
    gpt::Result r = gpt::Analyze(done, &PassName);
    int calls = 0;
    // With the terrain item timed as a nested pass, the G-buffer pass's own (exclusive) time lacks it: added back.
    const double tf = item ? KindMs(r, "pepTerrainItemFocus") : -1, tp = item ? KindMs(r, "pepTerrainItemPeriphery") : -1;
    double f = GbCallsMs(r, ord, gbCalls, &calls), p = GbCallsMs(r, ordP, gbCalls, &calls);
    if (f >= 0 && tf > 0) f += tf;
    if (p >= 0 && tp > 0) p += tp;
    if (began && tf >= 0) (on ? onT : offT).push_back(tf);
    if (began && tp >= 0) (on ? onTp : offTp).push_back(tp);
    if (!began || f < 0 || p < 0) {
      ++badBlocks;
      Log("pep C2 block %d (%s): not usable (GPU timing %s, %d G-buffer calls per frame, expected %d)", i + 1,
          on ? "ON" : "OFF", began ? "ran" : "did not start", calls, gbCalls);
      continue;
    }
    (on ? onF : offF).push_back(f);
    (on ? onP : offP).push_back(p);
    Log("pep C2 block %d (%s): focus main G-buffer passes %.3f ms GPU per frame (terrain item %.3f), periphery main "
        "%.3f ms (terrain item %.3f), %d frames",
        i + 1, on ? "ON: geometry only" : "OFF: stock", f, tf, p, tp, r.frames);
  }
  g_c2On = false;
  UnhookTerrainItem();
  StopCapture();
  const C2Out o = C2Share(offF, onF);
  const C2Out op = C2Share(offP, onP);
  Log("pep C2: focus main G-buffer GPU ms per frame (both eyes): stock %.3f, geometry only %.3f, front-end share "
      "%.2f (pass >= 0.60): %s | periphery main, untouched: %.3f / %.3f | passes probed %llu, viewports replaced "
      "%llu, passes with no viewport set inside %llu, buried %llu, unusable blocks %d | focus main = G-buffer call "
      "%d and %d of %d",
      o.off, o.on, o.share, Word(badBlocks ? -1 : o.verdict), op.off, op.on,
      static_cast<unsigned long long>(g_c2Passes.load()), static_cast<unsigned long long>(g_c2Vps.load()),
      static_cast<unsigned long long>(g_c2Missed.load()), static_cast<unsigned long long>(g_c2Buried.load()), badBlocks,
      ord[0], ord[1], gbCalls);
  if (item) {
    const C2TerrainOut t = C2Terrain(offT, onT);
    const C2TerrainOut tp = C2Terrain(offTp, onTp);
    Log("pep C2 terrain: the focus main passes' terrain item, GPU ms per frame (both eyes): stock %.3f, geometry only "
        "(the front end a merged pass saves) %.3f; the models before it: stock %.3f, geometry only %.3f | stage 3 "
        "gate, terrain front end >= %.2f ms: %s | periphery terrain item, untouched: %.3f / %.3f | item calls timed "
        "%llu",
        t.stock, t.frontEnd, o.off - t.stock, o.on - t.frontEnd, kC2TerrainGateMs, Word(badBlocks ? -1 : t.verdict), tp.stock,
        tp.frontEnd, static_cast<unsigned long long>(g_tiCalls.load()));
  }
  LogCounters();
}

void SuitePhase(std::atomic<uint64_t>& frames) {
  const Keys k = g_keys;
  if (!(k.c2 || k.c4 || k.c5 || k.c7)) return;
  Log("-- per-eye pass, live captures (per_eye_live.h; measure only%s): C2 %d s, C4 %d s, C5 %d s, C7 %d s --",
      k.c2 ? "; C2 is LOSSY in its ON blocks" : "", k.c2, k.c4, k.c5, k.c7);
  if (g_fault.load()) {
    Log("pep live: a capture faulted earlier in this session; skipped");
    return;
  }
  if (!Steady(frames)) {
    Log("pep live: frames are not steady (a mission load?); skipped");
    return;
  }
  if ((k.c4 || k.c5) && !g_benchAbort) CapturePhase(k);
  if (k.c7 && !g_benchAbort)
  {
    C7Acc a7;
    std::unordered_map<void*, char> kinds;  // suite thread
    const auto kindOf = [&](void* p) -> char {
      auto it = kinds.find(p);
      if (it != kinds.end()) return it->second;
      const std::string nm = p ? ptiming::RttiName(p) : std::string();
      const char c = nm.find("Cascade") != std::string::npos ? 'C' : nm.find("HeightMap") != std::string::npos ? 'H'
                     : nm.find("TerrainModule") != std::string::npos ? 'T' : '?';
      kinds.emplace(p, c);
      return c;
    };
    mkit::ChainCensusPhase(
        k.c7, frames,
        [&](const mkit::CensusAcc&) {
          Log("%s", C7Line(a7).c_str());
          for (const std::string& l : C7Layouts(a7)) Log("%s (C cascade, H height map, T view chain, ? other; four view chains by size: P periphery, F focus)", l.c_str());
        },
        [&](const mkit::CensusFrame& f) { C7Add(a7, f, kindOf); });
  }
  if (k.c2 && !g_benchAbort) C2Phase(k, frames);
}

// Payload stop.
void Shutdown() {
  UnhookTerrainItem();
  if (g_capture.exchange(false) || g_c2On.exchange(false)) Sleep(100);
  plod::BFn me = &OnBoundary;
  if (plod::g_probe.compare_exchange_strong(me, nullptr)) {
    gbpass::ObsFn obs = &ObsGb;
    gbpass::g_obs2.compare_exchange_strong(obs, nullptr);
    Sleep(50);
    VpUnwrap();
  }
}
#endif  // DCSQV_PEP_NO_RUNTIME

}  // namespace pepl
