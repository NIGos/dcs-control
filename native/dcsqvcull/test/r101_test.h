// R101 (docs/research/R101_symbology_window_decay.md): the learnt HUD field recovers after a bank (R100 §5.2: the
// F-16's focus views overflowed from the first roll weave on and never came back, wings level or not). Offline:
//  1. the aging field: grows at once, shrinks only after kColBuckets buckets of frames that saw the side; a side the
//     frame could not see (out of view, outside the window) keeps its extent;
//  2. the side masks (the visible part of each side inside the window / the view);
//  3. the recent fallback's window (the margin shrinks to fit the capacity, the rest covers the long field);
//  4. the scan schedule (faster in the fallback and in overflow);
//  5. the R100 sequence replayed (level 40 s, a +-50 deg / 8 s weave for 70 s, level 10 s, a 30 deg turn 40 s, level
//     12 s, a 25 s weave, level 12 s): an F-16 HUD whose horizon line rotates with the bank and is clipped to the HUD's
//     field of view, the focus views cut at the HUD's upper part (R100's 813-818 x 274-280 px windows), the HMCS beside
//     it in the right focus view; R97's window logic (frozen here) against R101's.
#pragma once

#include <deque>

namespace r101test {

using namespace fixdepth;
namespace sy = fixdepth::cmask::sym;
using sy::Box;
using sy::Cone;
using sy::Pose;

constexpr double kDeg = 3.14159265358979 / 180;

struct ViewGeo {
  uint32_t W, H;
  float tan[4];
};
// R100's views: the periphery as R96's fit (r97_test.h kGeo); the focus views at ~41.8 px / deg (the F-16's HUD,
// 17.6 deg, gives R100's 813-818 px level windows) and cut 7.8 deg below the axis (pitch -3: at elevation -10.8, the
// HUD's upper 6 deg in view: R100's 274-280 px tall windows, bottom at the view's edge). Left focus a little left.
const ViewGeo kGeo[4] = {{1040, 1028, {-1.517f, 0.938f, 1.210f, -1.2167f}},
                         {1040, 1028, {-0.908f, 1.596f, 1.267f, -1.2081f}},
                         {2028, 2004, {-0.45f, 0.396f, 0.699f, -0.137f}},
                         {2028, 2004, {-0.42f, 0.426f, 0.699f, -0.137f}}};

inline Pose PoseQ(const float q[4], const float tan[4]) {
  Pose p;
  memcpy(p.q, q, sizeof(p.q));
  memcpy(p.tan, tan, sizeof(p.tan));
  p.ok = true;
  return p;
}
struct Pt {
  double x, y;
};

// ---------------------------------------------------------------------------
// The F-16 HUD under bank (degrees, cockpit space; the head fixed, so the tracking space is the cockpit's):
//  - the fixed symbology: azimuth -8.8..8.8, elevation -22.8..-4.8 (R97's fit of R96's windows);
//  - the horizon line: through (0, -16.6), +-20 deg long, rotated by the bank, clipped to the HUD's field of view
//    (azimuth -13..13, elevation -26..-1.8). At wings level it lies below the focus views; from ~24 deg of bank one end
//    rises into them at the field of view's side (R100 v3: 818 -> 1014 px from 24.7 to 31.5 deg, then flat).
// ---------------------------------------------------------------------------
inline void HudDirs(double bankDeg, std::vector<std::array<double, 2>>& out) {
  out.clear();
  constexpr int N = 31;
  for (int i = 0; i < N; ++i)
    for (int j = 0; j < N; ++j) out.push_back({-8.8 + 17.6 * i / (N - 1), -22.8 + 18.0 * j / (N - 1)});
  const double b = bankDeg * kDeg;
  for (int i = 0; i <= 160; ++i) {
    const double t = -20 + 40.0 * i / 160;
    const double az = t * std::cos(b), el = -16.6 + t * std::sin(b);
    if (az >= -13 && az <= 13 && el >= -26 && el <= -1.8) out.push_back({az, el});
  }
}
inline void ViewPoints(const std::vector<std::array<double, 2>>& dirs, const float q[4], int v, std::vector<Pt>& out) {
  out.clear();
  const ViewGeo& g = kGeo[v];
  const Pose p = PoseQ(q, g.tan);
  for (const auto& a : dirs) {
    double d[3], x, y;
    sy::DirOf(a[0] * kDeg, a[1] * kDeg, d);
    if (sy::DirPixel(p, g.W, g.H, d, x, y) && x >= 0 && y >= 0 && x < g.W && y < g.H) out.push_back({x, y});
  }
}
// The statistics' words (as r97_test.h: the GPU's groups of 8 px, a scan's 8 x 8 tiles in groups of 64 px).
inline void EncodeBox(uint32_t* w, const Box& b) {
  if (!b.ok) return;
  w[0] = std::max(w[0], 0xffffffffu - uint32_t(b.x0)), w[1] = std::max(w[1], 0xffffffffu - uint32_t(b.y0));
  w[2] = std::max(w[2], uint32_t(b.x1)), w[3] = std::max(w[3], uint32_t(b.y1));
}
inline void EncodeAng(uint32_t* w, const Pose& p, double W, double H, const Box& b) {
  double az[4], el[4];
  const double xs[4] = {double(b.x0), double(b.x1), double(b.x0), double(b.x1)};
  const double ys[4] = {double(b.y0), double(b.y0), double(b.y1), double(b.y1)};
  for (int k = 0; k < 4; ++k) sy::PixelTrackAng(p, W, H, xs[k], ys[k], az[k], el[k]);
  w[0] = std::max(w[0], 0xffffffffu - sy::EncAng(std::min(std::min(az[0], az[1]), std::min(az[2], az[3]))));
  w[2] = std::max(w[2], sy::EncAng(std::max(std::max(az[0], az[1]), std::max(az[2], az[3]))));
  w[1] = std::max(w[1], 0xffffffffu - sy::EncAng(std::min(std::min(el[0], el[1]), std::min(el[2], el[3]))));
  w[3] = std::max(w[3], sy::EncAng(std::max(std::max(el[0], el[1]), std::max(el[2], el[3]))));
}
inline void GroupStats(const std::vector<Pt>& pts, int v, int g, bool tiles, uint32_t* box, uint32_t* ang,
                       const Pose& pose) {
  std::map<int, Box> grid;
  const int W = int(kGeo[v].W), H = int(kGeo[v].H), gw = W / g + 1;
  for (const Pt& p : pts) {
    const int ix = int(p.x), iy = int(p.y);
    const Box px = tiles ? Box{true, ix / 8 * 8, iy / 8 * 8, std::min(ix / 8 * 8 + 8, W), std::min(iy / 8 * 8 + 8, H)}
                         : Box{true, ix, iy, ix + 1, iy + 1};
    Box& c = grid[(iy / g) * gw + ix / g];
    c = sy::Union(c, px);
  }
  for (auto& kv : grid) {
    EncodeBox(box, kv.second);
    if (ang) EncodeAng(ang, pose, W, H, kv.second);
  }
}

// ---------------------------------------------------------------------------
// R97's window logic, frozen as the control (symbology.h before R101: the collimated field a union for the session)
// ---------------------------------------------------------------------------
namespace r97 {
struct Field {
  sy::Field h;  // (its helmet buckets, extra, scans and hold are R97's; its col is the union)
};
inline sy::WinOut Decide(Field& F, const Pose& pose, uint32_t W, uint32_t H, uint32_t cap, uint64_t frame) {
  sy::Field& L = F.h;
  const Box col = L.col.ok && pose.ok ? sy::ProjectCone(L.col, pose, W, H) : Box{};
  const Box hel = sy::HelmetExtent(L, frame);
  if (!col.ok && !hel.ok) {
    L.hold = sy::Hold{};
    L.hudOnly = false, L.helFit = 0;
    return sy::WinOut{};
  }
  const sy::WinOut both = sy::ChooseWindow(col, hel, W, H, cap, cap, L.extra);
  sy::WinOut c = both;
  if (L.hudOnly) {
    if (!both.overflow && ++L.helFit >= sy::kHelmetBack) {
      L.hudOnly = false, L.helFit = 0;
    } else {
      if (both.overflow) L.helFit = 0;
      c = sy::ChooseWindow(col, Box{}, W, H, cap, cap, L.extra);
    }
  } else if (both.overflow && col.ok && hel.ok) {
    const sy::WinOut hud = sy::ChooseWindow(col, Box{}, W, H, cap, cap, L.extra);
    if (!hud.overflow) {
      L.hudOnly = true, L.helFit = 0;
      c = hud;
    }
  }
  if (L.hudOnly && !col.ok) c = sy::WinOut{};
  return sy::Stabilize(L.hold, c, W, H, cap);
}
inline void Learn(Field& F, const sy::StatMeta& m, const uint32_t* s) {
  sy::Field& L = F.h;
  for (int i = 0; i < 2; ++i) {
    const uint32_t* w = s + (i ? sy::kStScanAng1 : sy::kStAng1);
    if (w[2] && sy::LearnConeAng(L.col, sy::DecodeAng(w))) L.col.seen = m.frame;
  }
  for (int k : {sy::kStBox2, sy::kStScan2}) {
    const Box b = sy::DecodeBox(s + k);
    if (b.ok) sy::NoteHelmet(L, b, m.frame);
  }
  if (m.win && s[sy::kStEdge]) {
    L.scanReq = true;
    L.extra = std::min(L.extra + 8, sy::kMaxExtra);
    L.lastEdge = m.frame;
  } else if (L.extra > 0 && m.frame - L.lastEdge > 600) {
    --L.extra;
  }
}
inline bool ScanDue(const Field& F, uint64_t frame, int v, bool learnt) {
  const sy::Field& L = F.h;
  return L.scanReq || (!learnt && frame - L.lastScan >= sy::kScanRetry) ||
         (learnt && (frame + uint64_t(v) * 16) % sy::kScanEvery == 0);
}
}  // namespace r97

// ---------------------------------------------------------------------------
// The R100 sequence
// ---------------------------------------------------------------------------
enum Phase { kLevel, kWeave, kLevel2, kTurn, kLevel3, kDump, kLevel4, kPhases };
const char* const kPhaseName[kPhases] = {"level", "weave", "level2", "turn", "level3", "fgdump", "level4"};
struct Seq {
  double weaveAmp = 50, weavePeriod = 8;  // deg, s (R100: +-45 commanded, +-48-52 flown)
  int len[kPhases] = {40 * 45, 70 * 45, 10 * 45, 40 * 45, 12 * 45, 25 * 45, 12 * 45};
};
// The bank at frame f of phase ph (t frames into it), with the FLY loop's slew of 60 deg/s between targets.
struct Bank {
  double cur = 0;
  double Step(int ph, int t, const Seq& s) {
    double target = 0;
    if (ph == kWeave || ph == kDump) target = s.weaveAmp * std::sin(2 * 3.14159265358979 * (t / 45.0) / s.weavePeriod);
    if (ph == kTurn) target = 30;
    if (ph == kWeave || ph == kDump) {
      cur = target;  // (the weave's own rate is below the slew limit: <= 39 deg/s)
    } else {
      const double step = 60.0 / 45;
      cur += std::clamp(target - cur, -step, step);
    }
    return cur;
  }
};
struct PhaseStats {
  uint64_t visible = 0, present = 0, overflow = 0, miss = 0, recent = 0, hudOnly = 0;
  uint32_t maxW = 0, maxH = 0;
  uint32_t lastW = 0, lastH = 0;  // the phase's last window (0: none)
  int recoverFrame = -1;  // the first frame of the phase from which the view stays present to its end (-1 never)
  int longFrame = -1;     // R101: the first frame from which the window stays on the long field
};
struct SeqOut {
  PhaseStats p[kPhases][4];
  double windowUs = 0;  // R101 arm: DecideWindow time per view-frame (us)
};
inline SeqOut RunSeq(int arm, const Seq& sq, bool helmet) {
  SeqOut out;
  sy::Field nf[4];
  r97::Field of[4];
  struct Pending {
    uint64_t frame;
    bool win[4], scan[4], hudOnly[4];
    sy::StatMeta ob[4];
    float q[4];
    uint32_t w[4][sy::kStatU];
  };
  std::deque<Pending> q;
  float qq[4];
  r95test::QuatYawPitch(0, -3, 0, qq);  // the held head (HoldPose, pitch -3)
  // The HMCS in the right focus view (head-fixed): beside the HUD, the union ~1000 px (R100: HUD-only at level).
  Box hmcs{};
  if (helmet) {
    std::vector<std::array<double, 2>> d;
    std::vector<Pt> p;
    HudDirs(0, d);
    ViewPoints(d, qq, 3, p);
    double x0 = 1e9, y0 = 1e9;
    for (const Pt& t : p) x0 = std::min(x0, t.x), y0 = std::min(y0, t.y);
    hmcs = {true, int(x0) - 260, int(y0) + 20, int(x0) - 200, int(y0) + 120};
  }
  Bank bank;
  std::vector<std::array<double, 2>> dirs;
  std::vector<Pt> hud, hel;
  uint64_t frame = 0;
  LARGE_INTEGER fq, t0, t1;
  QueryPerformanceFrequency(&fq);
  double us = 0;
  uint64_t calls = 0;
  bool presentRun[4] = {}, longRun[4] = {};
  for (int ph = 0; ph < kPhases; ++ph) {
    for (int v = 0; v < 4; ++v) presentRun[v] = longRun[v] = false;
    for (int t = 0; t < sq.len[ph]; ++t) {
      ++frame;
      const double b = bank.Step(ph, t, sq);
      HudDirs(b, dirs);
      Pending pd{};
      pd.frame = frame;
      memcpy(pd.q, qq, sizeof(qq));
      for (int v = 0; v < 4; ++v) {
        const ViewGeo& g = kGeo[v];
        const Pose pose = PoseQ(qq, g.tan);
        const uint32_t cap = v < 2 ? 512 : 1024;
        bool scan, hudOnly, recent = false;
        sy::WinOut w;
        if (arm == 0) {
          r97::Field& L = of[v];
          scan = r97::ScanDue(L, frame, v, L.h.col.ok || sy::HelmetExtent(L.h, frame).ok);
          if (scan) L.h.scanReq = false, L.h.lastScan = frame;
          w = r97::Decide(L, pose, g.W, g.H, cap, frame);
          hudOnly = L.h.hudOnly;
        } else {
          sy::Field& L = nf[v];
          scan = sy::ScanDue(L, frame, v, L.col.ok || sy::HelmetExtent(L, frame).ok);
          if (scan) L.scanReq = false, L.lastScan = frame;
          QueryPerformanceCounter(&t0);
          w = sy::DecideWindow(L, pose, g.W, g.H, cap, frame);
          QueryPerformanceCounter(&t1);
          us += double(t1.QuadPart - t0.QuadPart) * 1e6 / double(fq.QuadPart), ++calls;
          hudOnly = L.hudOnly;
          recent = L.recent;
          sy::NoteObs(pd.ob[v], L);
        }
        ViewPoints(dirs, qq, v, hud);
        hel.clear();
        if (v == 3 && hmcs.ok)
          for (int i = 0; i <= 8; ++i)
            for (int j = 0; j <= 8; ++j)
              hel.push_back({hmcs.x0 + (hmcs.x1 - 1 - hmcs.x0) * i / 8.0, hmcs.y0 + (hmcs.y1 - 1 - hmcs.y0) * j / 8.0});
        PhaseStats& st = out.p[ph][v];
        const bool present = w.any && !w.overflow;
        if (!hud.empty()) {
          ++st.visible;
          if (w.overflow) ++st.overflow;
          if (present) {
            ++st.present;
            st.hudOnly += hudOnly, st.recent += recent;
            st.maxW = std::max(st.maxW, w.w), st.maxH = std::max(st.maxH, w.h);
            st.lastW = w.w, st.lastH = w.h;
            bool miss = false;
            for (const Pt& p : hud) miss = miss || p.x < w.x || p.y < w.y || p.x >= w.x + w.w || p.y >= w.y + w.h;
            st.miss += miss;
          }
        }
        if (present && !presentRun[v]) presentRun[v] = true, st.recoverFrame = t;
        if (!present) presentRun[v] = false, st.recoverFrame = -1;
        const bool onLong = present && !recent;
        if (onLong && !longRun[v]) longRun[v] = true, st.longFrame = t;
        if (!onLong) longRun[v] = false, st.longFrame = -1;
        // the frame's statistics (read back two frames later)
        uint32_t* s = pd.w[v];
        pd.win[v] = present, pd.scan[v] = scan, pd.hudOnly[v] = hudOnly;
        auto inWin = [&](const Pt& p) { return p.x >= w.x && p.y >= w.y && p.x < w.x + w.w && p.y < w.y + w.h; };
        if (present) {
          std::vector<Pt> in1, in2;
          bool edge = false;
          for (const Pt& p : hud) {
            if (inWin(p)) {
              in1.push_back(p);
              edge = edge || (w.x > 0 && p.x < w.x + 2) || (w.y > 0 && p.y < w.y + 2) ||
                     (w.x + w.w < g.W && p.x >= w.x + w.w - 2) || (w.y + w.h < g.H && p.y >= w.y + w.h - 2);
            } else {
              edge = true;  // the HUD crosses the window's edge (changed texels at its inner edge)
            }
          }
          GroupStats(in1, v, 8, false, s + sy::kStBox1, s + sy::kStAng1, pose);
          if (!hudOnly) {
            for (const Pt& p : hel)
              if (inWin(p)) in2.push_back(p);
            GroupStats(in2, v, 8, false, s + sy::kStBox2, nullptr, pose);
          }
          s[sy::kStEdge] = edge ? 1 : 0;
          s[sy::kStChanged] = static_cast<uint32_t>(in1.size() * 20);
        }
        if (scan) {
          GroupStats(hud, v, 64, true, s + sy::kStScan1, s + sy::kStScanAng1, pose);
          GroupStats(hel, v, 64, true, s + sy::kStScan2, nullptr, pose);
        }
      }
      q.push_back(pd);
      while (!q.empty() && q.front().frame + 2 <= frame) {
        const Pending& r = q.front();
        for (int v = 0; v < 4; ++v) {
          const ViewGeo& g = kGeo[v];
          sy::StatMeta m = r.ob[v];
          m.win = r.win[v], m.scan = r.scan[v], m.hudOnly = r.hudOnly[v], m.angOk = true;
          memcpy(m.q, r.q, sizeof(m.q));
          m.pose = PoseQ(r.q, g.tan);
          m.W = g.W, m.H = g.H, m.frame = r.frame;
          if (arm == 0) {
            r97::Learn(of[v], m, r.w[v]);
          } else {
            sy::LearnCount n;
            sy::LearnStats(nf[v], m, r.w[v], n);
          }
        }
        q.pop_front();
      }
    }
  }
  out.windowUs = calls ? us / double(calls) : 0;
  return out;
}
inline double Pct(uint64_t a, uint64_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; }

void TestSequence() {
  const Seq sq;
  const SeqOut o = RunSeq(0, sq, true), n = RunSeq(1, sq, true);
  for (int ph = 0; ph < kPhases; ++ph) {
    printf("     (R101 replay %-6s R97 -> R101 per view: overflow %%, present %%, misses, max window, recent-field %%:",
           kPhaseName[ph]);
    for (int v = 0; v < 4; ++v) {
      const PhaseStats &a = o.p[ph][v], &b = n.p[ph][v];
      printf(" v%d %.1f->%.1f, %.1f->%.1f, %llu->%llu, %ux%u->%ux%u, %.1f", v, Pct(a.overflow, a.visible),
             Pct(b.overflow, b.visible), Pct(a.present, a.visible), Pct(b.present, b.visible),
             static_cast<unsigned long long>(a.miss), static_cast<unsigned long long>(b.miss), a.maxW, a.maxH, b.maxW,
             b.maxH, Pct(b.recent, b.present));
    }
    printf("; R101 present from frame %d / %d, on the long field from %d / %d (v2 / v3))\n", n.p[ph][2].recoverFrame,
           n.p[ph][3].recoverFrame, n.p[ph][2].longFrame, n.p[ph][3].longFrame);
  }
  printf("     (R101 replay: DecideWindow %.2f us per view-frame)\n", n.windowUs);
  // Verdicts.
  // R97 (control) must reproduce R100: the focus views overflow in the weave and stay so to the end.
  const bool ctlSticky = Pct(o.p[kWeave][3].overflow, o.p[kWeave][3].visible) > 50 &&
                         o.p[kLevel2][2].present == 0 && o.p[kLevel2][3].present == 0 &&
                         o.p[kLevel4][2].present == 0 && o.p[kLevel4][3].present == 0 && o.p[kLevel][3].overflow == 0;
  // R101: every phase < 5 % overflow in every view; the focus views present again within 3 s (135 frames) of each
  // level phase's start and back on the long field within 12 s; no window misses in the level phases; misses in the
  // weaves < 1 % of the present view-frames: only the first sight of a side the long field never held (or forgot after
  // ~8.5 s), ~6 frames each, as R97 at its first bank (R97 then has no window at all). Without the lean (the spare
  // capacity evenly split) they were 2.5-3 % of the focus views' weave frames: every alternate bank was a first sight.
  bool ovOk = true, recOk = true, longOk = true, levelMiss = true;
  uint64_t weaveMiss = 0, weavePresent = 0, ctlWeaveMiss = 0;
  for (int ph = 0; ph < kPhases; ++ph)
    for (int v = 0; v < 4; ++v) {
      const PhaseStats& b = n.p[ph][v];
      ovOk = ovOk && Pct(b.overflow, b.visible) < 5;
      if (ph == kLevel2 || ph == kLevel3 || ph == kLevel4) {
        recOk = recOk && b.recoverFrame >= 0 && b.recoverFrame <= 135;
        levelMiss = levelMiss && b.miss == 0;
      }
      if (ph == kWeave || ph == kDump) weaveMiss += b.miss, weavePresent += b.present, ctlWeaveMiss += o.p[ph][v].miss;
    }
  // The windows back to the level ones by the end of the last level phase (the bank's extents forgotten).
  bool backOk = true;
  for (int v = 0; v < 4; ++v)
    backOk = backOk && n.p[kLevel4][v].lastW > 0 && std::abs(int(n.p[kLevel4][v].lastW) - int(n.p[kLevel][v].lastW)) <= 8 &&
             std::abs(int(n.p[kLevel4][v].lastH) - int(n.p[kLevel][v].lastH)) <= 8;
  printf("     (R101 replay: last windows level -> level4: v0 %ux%u -> %ux%u, v1 %ux%u -> %ux%u, v2 %ux%u -> %ux%u, "
         "v3 %ux%u -> %ux%u; R97 v2 / v3 at level4 %ux%u / %ux%u)" "\n",
         n.p[kLevel][0].lastW, n.p[kLevel][0].lastH, n.p[kLevel4][0].lastW, n.p[kLevel4][0].lastH, n.p[kLevel][1].lastW,
         n.p[kLevel][1].lastH, n.p[kLevel4][1].lastW, n.p[kLevel4][1].lastH, n.p[kLevel][2].lastW, n.p[kLevel][2].lastH,
         n.p[kLevel4][2].lastW, n.p[kLevel4][2].lastH, n.p[kLevel][3].lastW, n.p[kLevel][3].lastH, n.p[kLevel4][3].lastW,
         n.p[kLevel4][3].lastH, o.p[kLevel4][2].lastW, o.p[kLevel4][2].lastH, o.p[kLevel4][3].lastW,
         o.p[kLevel4][3].lastH);
  for (int ph : {kLevel3, kLevel4})
    for (int v = 2; v < 4; ++v) longOk = longOk && n.p[ph][v].longFrame >= 0 && n.p[ph][v].longFrame <= 540;
  char what[700];
  sprintf_s(what,
            "R101 replay of R100's sequence (F-16 HUD, horizon line rotating with the bank, focus views cut at the HUD's "
            "top, HMCS): R97 reproduces the sticky focus overflow (weave v3 %.0f%%, level2 / level4 focus present 0) %d; "
            "R101 overflow < 5%% in every phase and view %d (weave v2 %.1f%% v3 %.1f%%); focus present again within 3 s "
            "of wings level %d (level2 v2 / v3 from frame %d / %d); back on the long field within 12 s after the turn and "
            "the last weave %d, its windows back to the level ones %d; no misses at level %d; weave misses %llu of %llu present view-frames (R97 %llu)",
            Pct(o.p[kWeave][3].overflow, o.p[kWeave][3].visible), ctlSticky, ovOk,
            Pct(n.p[kWeave][2].overflow, n.p[kWeave][2].visible), Pct(n.p[kWeave][3].overflow, n.p[kWeave][3].visible),
            recOk, n.p[kLevel2][2].recoverFrame, n.p[kLevel2][3].recoverFrame, longOk, backOk, levelMiss,
            static_cast<unsigned long long>(weaveMiss), static_cast<unsigned long long>(weavePresent),
            static_cast<unsigned long long>(ctlWeaveMiss));
  Check(ctlSticky && ovOk && recOk && longOk && backOk && levelMiss && weavePresent > 0 &&
            Pct(weaveMiss, weavePresent) < 1.0,
        what);
}

// ---------------------------------------------------------------------------
// Pure parts
// ---------------------------------------------------------------------------
void TestAging() {
  using sy::AngBox;
  auto box = [](double a0, double a1, double e0, double e1) {
    AngBox b;
    b.ok = true, b.az0 = a0 * kDeg, b.az1 = a1 * kDeg, b.el0 = e0 * kDeg, b.el1 = e1 * kDeg;
    return b;
  };
  const AngBox base = box(-8.8, 8.8, -22.8, -4.8), wide = box(-8.8, 13, -22.8, -4.8);
  sy::AgedCone a;
  uint64_t f = 1;
  for (; f <= 100; ++f) sy::AgeObserve(a, base, f, sy::kColBucket, sy::kColBuckets, 15);
  sy::AgeObserve(a, wide, f++, sy::kColBucket, sy::kColBuckets, 15);
  const Cone grown = sy::AgedField(a, sy::kColBuckets);
  const bool growsAtOnce = grown.ok && std::fabs(grown.az1 - 13 * kDeg) < 1e-9;
  // Not seen (obs 0) for 2000 frames: kept.
  sy::AgedCone hidden = a;
  for (uint64_t g = f; g < f + 2000; ++g) sy::AgeObserve(hidden, base, g, sy::kColBucket, sy::kColBuckets, 0);
  const bool keptHidden = std::fabs(sy::AgedField(hidden, sy::kColBuckets).az1 - 13 * kDeg) < 1e-9;
  // Only the left / bottom / top sides seen (bit 1, az1, not): az1 kept, the others follow.
  sy::AgedCone part = a;
  for (uint64_t g = f; g < f + 2000; ++g) sy::AgeObserve(part, base, g, sy::kColBucket, sy::kColBuckets, 15u & ~2u);
  const bool keptSide = std::fabs(sy::AgedField(part, sy::kColBuckets).az1 - 13 * kDeg) < 1e-9;
  // Seen: shrinks back after kColBuckets buckets, not before kColBuckets - 1.
  int shrunkAt = -1;
  for (uint64_t g = f; g < f + 1000 && shrunkAt < 0; ++g) {
    sy::AgeObserve(a, base, g, sy::kColBucket, sy::kColBuckets, 15);
    if (std::fabs(sy::AgedField(a, sy::kColBuckets).az1 - 8.8 * kDeg) < 1e-9) shrunkAt = int(g - f);
  }
  const bool shrinks = shrunkAt >= int(sy::kColBucket) * (sy::kColBuckets - 1) && shrunkAt <= int(sy::kColBucket) * (sy::kColBuckets + 1);
  // The recent ring: ~kRecBuckets x kRecBucket frames.
  sy::AgedCone r;
  for (uint64_t g = 1; g <= 50; ++g) sy::AgeObserve(r, g == 50 ? wide : base, g, sy::kRecBucket, sy::kRecBuckets, 15);
  int recAt = -1;
  for (uint64_t g = 51; g < 300 && recAt < 0; ++g) {
    sy::AgeObserve(r, base, g, sy::kRecBucket, sy::kRecBuckets, 15);
    if (std::fabs(sy::AgedField(r, sy::kRecBuckets).az1 - 8.8 * kDeg) < 1e-9) recAt = int(g - 51);
  }
  // A still head: the same observation every frame ages the field (R97 skipped repeats: a field that stopped growing
  // could never shrink).
  sy::Field L;
  sy::StatMeta m;
  m.angOk = true, m.win = true, m.obsWinL = m.obsWinR = 15, m.obsViewL = m.obsViewR = 15;
  uint32_t s[sy::kStatU] = {};
  auto put = [&](const AngBox& b) {
    memset(s, 0, sizeof(s));
    s[sy::kStAng1 + 0] = 0xffffffffu - sy::EncAng(b.az0), s[sy::kStAng1 + 1] = 0xffffffffu - sy::EncAng(b.el0);
    s[sy::kStAng1 + 2] = sy::EncAng(b.az1), s[sy::kStAng1 + 3] = sy::EncAng(b.el1);
  };
  sy::LearnCount n;
  put(wide);
  m.frame = 1;
  sy::LearnStats(L, m, s, n);
  put(base);
  for (uint64_t g = 2; g < 400; ++g) m.frame = g, sy::LearnStats(L, m, s, n);
  const bool still = L.col.ok && std::fabs(L.col.az1 - 8.8 * kDeg) < 1e-6 && L.rec.ok;
  char what[900];
  sprintf_s(what,
            "R101 aging field: grows at once %d; a side not seen keeps its extent (hidden 2000 frames %d, one side "
            "unseen %d); a seen side shrinks after %d frames (%u x %llu) %d; the recent ring after %d frames %d; a still "
            "head's repeated observation ages it (R97 skipped repeats) %d",
            growsAtOnce, keptHidden, keptSide, shrunkAt, sy::kColBuckets, static_cast<unsigned long long>(sy::kColBucket),
            shrinks, recAt, recAt >= 0 && recAt <= int(sy::kRecBucket) * (sy::kRecBuckets + 1), still);
  Check(growsAtOnce && keptHidden && keptSide && shrinks && recAt >= 0 &&
            recAt <= int(sy::kRecBucket) * (sy::kRecBuckets + 1) && still,
        what);
}

void TestSideMasks() {
  // The HUD in the right focus view (cut at the bottom): the azimuth sides and the top are visible inside the window,
  // the bottom side is below the view (not visible: never aged from this view).
  Cone c;
  c.ok = true, c.az0 = -8.8 * kDeg, c.az1 = 8.8 * kDeg, c.el0 = -22.8 * kDeg, c.el1 = -4.8 * kDeg;
  float q[4];
  r95test::QuatYawPitch(0, -3, 0, q);
  const Pose p = PoseQ(q, kGeo[3].tan);
  double d[sy::kOutline][3];
  sy::ConeOutline(c, d);
  sy::SideVis vis;
  const Box b = sy::ProjectDirs(d, p, 2028, 2004, &vis);
  const uint32_t inWin = sy::SideMask(vis, b.x0 - 10, b.y0 - 10, b.x1 + 10, b.y1 + 10);
  const uint32_t view = sy::SideMask(vis, 0, 0, 2028, 2004);
  const uint32_t narrow = sy::SideMask(vis, b.x0 + 50, b.y0 - 10, b.x1 + 10, b.y1 + 10);  // the left side outside
  // Under yaw: the HUD half out of the left periphery (one azimuth side outside the view, the other in): the side out of
  // view is not seen, the one in view is.
  uint32_t yawMask = 0;
  bool yawOk = false;
  for (double yaw : {-40.0, 40.0}) {
    r95test::QuatYawPitch(yaw, -3, 0, q);
    const Pose py = PoseQ(q, kGeo[0].tan);
    sy::SideVis vy;
    const Box by = sy::ProjectDirs(d, py, 1040, 1028, &vy);
    const uint32_t m = sy::SideMask(vy, 0, 0, 1040, 1028);
    if (by.ok && vy.any[0] != vy.any[1]) yawMask = m, yawOk = ((m & 3u) == (vy.any[0] ? 1u : 2u)) && (m & 12u) == 12u;
  }
  // DecideWindow sets the masks for the frame (the window it returns).
  sy::Field F;
  F.col = c;
  const sy::WinOut w = sy::DecideWindow(F, p, 2028, 2004, 1024, 1);
  char what[900];
  sprintf_s(what,
            "R101 side masks: focus view cut at the HUD's top: window 0x%x, view 0x%x (az0, az1, el1 seen; el0 below "
            "the view), the left side (and the top's left end) outside a narrower rectangle 0x%x; HUD leaving the left periphery: 0x%x (the side out of "
            "view not seen, the half in view's sides seen: %d); DecideWindow's frame masks win 0x%x view 0x%x (window %ux%u)",
            inWin, view, narrow, yawMask, yawOk, F.obsWinL, F.obsViewL, w.w, w.h);
  Check(inWin == 0xb && view == 0xb && narrow == 0x2 && yawOk && F.obsWinL == 0xb &&
            F.obsViewL == 0xb && F.obsWinR == 0xb,
        what);
}

void TestFitWindow() {
  // The recent field 915 px wide (the banked HUD), the long one 1090 (both sides): full margin 1015 fits; 990 px -> the
  // margin shrinks (1024); 1020 px -> overflow (less than kMinMargin a side).
  auto fit = [](int recW, int lngW) {
    const Box rec{true, 500, 1700, 500 + recW, 1990};
    const Box lng{true, 500 + recW - lngW, 1700, 500 + recW, 1990};
    return sy::FitWindow(rec, lng, 2028, 2004, 1024, 0);
  };
  const sy::WinOut a = fit(915, 1090), b = fit(990, 1090), c = fit(1020, 1090), d = fit(600, 700);
  const bool aOk = a.any && !a.overflow && a.w == 1024 && int(a.x) < 500 - 46 && int(a.x + a.w) >= 500 + 915 + 4;
  const bool bOk = b.any && !b.overflow && b.w <= 1024 && int(b.x) <= 500 - sy::kMinMargin && int(b.x + b.w) >= 1490 + sy::kMinMargin;
  const bool cOk = c.overflow;
  // Leaning low (the content just left the high side): the spare capacity covers the long field's low side first.
  const sy::WinOut e = sy::FitWindow(Box{true, 700, 1700, 1436, 1990}, Box{true, 525, 1700, 1611, 1990}, 2028, 2004, 1024,
                                     0, -1, 0);
  const sy::WinOut e0 = sy::FitWindow(Box{true, 700, 1700, 1436, 1990}, Box{true, 525, 1700, 1611, 1990}, 2028, 2004, 1024, 0);
  const bool eOk = e.any && !e.overflow && e.w <= 1024 && int(e.x) <= 525 - 59 && int(e0.x) > 525 - 59;
  const bool dOk = d.any && !d.overflow && int(d.x) <= 500 + 600 - 700 - 39 && int(d.x + d.w) >= 1100 + 34;  // all of it
  // A HUD whose union overflows but whose recent field fits: DecideWindow takes the recent field (present, no
  // overflow, the helmet left out); back to the long field after kRecentBack fitting frames.
  sy::Field F;
  F.col.ok = F.rec.ok = true;
  F.col.az0 = -13 * kDeg, F.col.az1 = 13 * kDeg, F.col.el0 = -22.8 * kDeg, F.col.el1 = -4.8 * kDeg;
  F.rec = F.col, F.rec.az0 = -8.8 * kDeg, F.rec.az1 = 11 * kDeg;
  float q[4];
  r95test::QuatYawPitch(0, -3, 0, q);
  const Pose p = PoseQ(q, kGeo[3].tan);
  const sy::WinOut w1 = sy::DecideWindow(F, p, 2028, 2004, 1024, 1);
  const bool fb = F.recent && F.hudOnly && w1.any && !w1.overflow;
  F.col = F.rec;  // the long field aged back
  int still = 0;
  uint64_t f = 2;
  for (; f < 2 + sy::kRecentBack - 1; ++f) sy::DecideWindow(F, p, 2028, 2004, 1024, f), still += F.recent;
  sy::DecideWindow(F, p, 2028, 2004, 1024, f);
  const bool back = still == sy::kRecentBack - 1 && !F.recent;
  // Nothing recent learnt: R97's overflow.
  sy::Field G;
  G.col = F.col, G.col.az0 = -13 * kDeg;
  const sy::WinOut w3 = sy::DecideWindow(G, p, 2028, 2004, 1024, 1);
  char what[900];
  sprintf_s(what,
            "R101 recent fallback window: full margin and the rest toward the long field %d (%u at %u), margin shrunk to "
            "fit %d (%u), overflow below %d px a side %d, a small one covers the whole long field %d, leaning low covers the low side (%u, even split %u) %d; DecideWindow "
            "falls back (present, HUD only) %d, back to the long field after %d fitting frames %d; no recent field: "
            "overflow %d",
            aOk, a.w, a.x, bOk, b.w, sy::kMinMargin, cOk, dOk, e.x, e0.x, eOk, fb, sy::kRecentBack, back, w3.overflow);
  Check(aOk && bOk && cOk && dOk && eOk && fb && back && w3.overflow, what);
}

void TestScanDue() {
  sy::Field L;
  L.scanReq = false;
  int normal = 0, fallback = 0, over = 0;
  for (uint64_t f = 1; f <= 640; ++f) normal += sy::ScanDue(L, f, 3, true);
  L.recent = true;
  for (uint64_t f = 1; f <= 640; ++f) fallback += sy::ScanDue(L, f, 3, true);
  L.recent = false, L.hold.over = true;
  for (uint64_t f = 1; f <= 640; ++f) over += sy::ScanDue(L, f, 3, true);
  char what[300];
  sprintf_s(what, "R101 scan schedule over 640 frames: normal %d (every %llu), recent fallback %d, overflow %d (every %llu)",
            normal, static_cast<unsigned long long>(sy::kScanEvery), fallback, over,
            static_cast<unsigned long long>(sy::kScanFallback));
  Check(normal == 10 && fallback == 90 && over == 90, what);  // (the 64-frame scans fall between the 8-frame ones)
}

inline void Run() {
  printf("-- R101 symbology window decay --\n");
  TestAging();
  TestSideMasks();
  TestFitWindow();
  TestScanDue();
  TestSequence();
}

}  // namespace r101test
