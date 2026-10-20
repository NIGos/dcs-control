// R97: the fixes to the HUD symbology export after its first live run (docs/research/R96_symbology_live.md,
// docs/research/R97_symbology_fixes.md). Offline, one regression test per fix:
//  1. the coverage alpha flag: the per-view verdict (failing texels, opaque cores), the read-back vote (complete view
//     sets, still windows), the latch per generation; a replay of R96's per-view alpha statistics through R95's vote
//     (flips) and R97's (none);
//  2. overflow: the field projected with the view frustum clipping (a HUD leaving through a side, partly behind the
//     view), the field learnt from view-relative angles (the GPU's statistics) instead of pixel boxes, the helmet's
//     decaying extent, the HUD-only fallback (present, overflow clear); and a replay of R96's runs (the F-16 / F/A-18
//     geometry fitted from R96's monitor windows, 30 deg/s yaw, the JHMCS) through R95's and R97's window logic;
//  3. cost: the render thread's caches, the class-gated finalize on WARP; the cost breakdown in the stats;
//  4. the record render point's log line follows its state (not the install order);
//  5. [Dev] SymbologyPublishAlways (the pipeline test in r95_pipe_run.h has the live path; here the configuration);
//  6. the VRAM figures of the creation line.
#pragma once

#include <d3d12.h>
#include <deque>
#include <map>

namespace r97test {

using namespace fixdepth;
namespace sy = fixdepth::cmask::sym;
using sy::Box;
using sy::Cone;
using sy::Pose;
using fdtest::Rel;

constexpr double kDeg = 3.14159265358979 / 180;

// A view: its size and FOV tangents (left, right, up, down).
struct ViewGeo {
  uint32_t W, H;
  float tan[4];
};
// R96's views (fitted from the monitor's window centres under the 30 deg/s yaw, night F-16, r97\tools\fitfov2.py):
// periphery left / right (rms 9.5 / 10.9 px); the focus views are QV's moving sub-frusta (not a fixed fit: rms ~80 px),
// here the fit's span centred on the periphery's axis.
const ViewGeo kGeo[4] = {{1040, 1028, {-1.517f, 0.938f, 1.210f, -1.2167f}},
                         {1040, 1028, {-0.908f, 1.596f, 1.267f, -1.2081f}},
                         {2028, 2004, {-0.533f, 0.336f, 0.424f, -0.4347f}},
                         {2028, 2004, {-0.353f, 0.614f, 0.526f, -0.4296f}}};

inline void Quat(double yawDeg, double pitchDeg, float q[4]) { r95test::QuatYawPitch(yawDeg, pitchDeg, 0, q); }
inline Pose PoseQ(const float q[4], const float tan[4]) {
  Pose p;
  memcpy(p.q, q, sizeof(p.q));
  memcpy(p.tan, tan, sizeof(p.tan));
  p.ok = true;
  return p;
}

// ---------------------------------------------------------------------------
// R95's window logic, frozen as the replay's control (symbology.h before R97)
// ---------------------------------------------------------------------------
namespace old {
inline Box ProjectConeR95(const Cone& c, const Pose& p, uint32_t outW, uint32_t outH) {
  if (!c.ok || !p.ok) return Box{};
  double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
  int front = 0, behind = 0;
  for (int side = 0; side < 4; ++side)
    for (int i = 0; i <= sy::kEdgeSamples; ++i) {
      const double t = double(i) / sy::kEdgeSamples;
      const double az = side == 0 ? c.az0 + t * (c.az1 - c.az0) : side == 1 ? c.az1 : side == 2 ? c.az1 - t * (c.az1 - c.az0) : c.az0;
      const double el = side == 0 ? c.el0 : side == 1 ? c.el0 + t * (c.el1 - c.el0) : side == 2 ? c.el1 : c.el1 - t * (c.el1 - c.el0);
      double d[3], px, py;
      sy::DirOf(az, el, d);
      if (!sy::DirPixel(p, outW, outH, d, px, py)) {
        ++behind;
        continue;
      }
      ++front;
      x0 = std::min(x0, px), y0 = std::min(y0, py), x1 = std::max(x1, px), y1 = std::max(y1, py);
    }
  if (!front) return Box{};
  if (behind) return {true, 0, 0, static_cast<int>(outW), static_cast<int>(outH)};
  const double lim = 1e7;
  return {true, static_cast<int>(std::floor(std::max(x0, -lim))), static_cast<int>(std::floor(std::max(y0, -lim))),
          static_cast<int>(std::ceil(std::min(x1, lim))), static_cast<int>(std::ceil(std::min(y1, lim)))};
}
struct Field {
  Cone col;
  Box hel;
  uint64_t helSeen = 0;
  int extra = 0;
  uint64_t lastEdge = 0, lastScan = 0;
  bool scanReq = true;
  sy::Hold hold;
};
inline sy::WinOut Decide(Field& L, const Pose& pose, uint32_t W, uint32_t H, uint32_t cap, uint64_t frame) {
  if (L.hel.ok && frame - L.helSeen > 900) L.hel = Box{};
  const Box col = L.col.ok && pose.ok ? ProjectConeR95(L.col, pose, W, H) : Box{};
  if (!col.ok && !L.hel.ok) {
    L.hold = sy::Hold{};
    return sy::WinOut{};
  }
  return sy::Stabilize(L.hold, sy::ChooseWindow(col, L.hel, W, H, cap, cap, L.extra), W, H, cap);
}
}  // namespace old

// ---------------------------------------------------------------------------
// The replay: a HUD (an azimuth / elevation rectangle fixed in the cockpit), a helmet (view-pixel boxes, head-fixed)
// and a world-referenced helmet cue, rendered as dense point sets per view; the statistics the GPU would read back
// (with its group granularity) two frames later; scans as the render thread schedules them.
// ---------------------------------------------------------------------------
struct Scene {
  double az0, az1, el0, el1;   // the HUD (degrees, cockpit space)
  Box helmet[4];               // head-fixed helmet symbols per view (pixels), ok = present
  bool cue = false;            // a world-referenced JHMCS cue (a 3 x 3 deg box at az 30, el -12 in the world)
};
struct Pt {
  double x, y;
};
// The view pixels of the scene's HUD points visible in view v at orientation q.
inline void HudPoints(const Scene& s, const float q[4], int v, std::vector<Pt>& out) {
  out.clear();
  const ViewGeo& g = kGeo[v];
  const Pose p = PoseQ(q, g.tan);
  constexpr int N = 31;
  for (int i = 0; i < N; ++i)
    for (int j = 0; j < N; ++j) {
      const double az = (s.az0 + (s.az1 - s.az0) * i / (N - 1)) * kDeg, el = (s.el0 + (s.el1 - s.el0) * j / (N - 1)) * kDeg;
      double d[3], x, y;
      sy::DirOf(az, el, d);
      if (sy::DirPixel(p, g.W, g.H, d, x, y) && x >= 0 && y >= 0 && x < g.W && y < g.H) out.push_back({x, y});
    }
}
inline void CuePoints(const float q[4], double worldYaw, int v, std::vector<Pt>& out) {
  out.clear();
  const ViewGeo& g = kGeo[v];
  const Pose p = PoseQ(q, g.tan);
  for (int i = 0; i < 7; ++i)
    for (int j = 0; j < 7; ++j) {
      const double az = (30 + 3.0 * i / 6) * kDeg + worldYaw, el = (-12 + 3.0 * j / 6) * kDeg;
      double d[3], x, y;
      sy::DirOf(az, el, d);
      if (sy::DirPixel(p, g.W, g.H, d, x, y) && x >= 0 && y >= 0 && x < g.W && y < g.H) out.push_back({x, y});
    }
}
// Words of the statistics a set of changed texels gives: the pixel box (kStBox*), and for type 1 the angle box of the
// 8 x 8 groups' pixel boxes (the GPU's AngOut per group).
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
// Points binned into groups of `g` pixels (8 for the accumulation; 64 = 8 x 8 tiles for the scan, tile-rounded).
inline void GroupStats(const std::vector<Pt>& pts, int v, int g, bool tiles, uint32_t* box, uint32_t* ang,
                       const Pose& pose) {
  static std::vector<Box> grid;
  static std::vector<int> touched;
  const int gw = int(kGeo[v].W) / g + 1, gh = int(kGeo[v].H) / g + 1;
  if (grid.size() < size_t(gw) * gh) grid.resize(size_t(gw) * gh);
  touched.clear();
  for (const Pt& p : pts) {
    const int ix = int(p.x), iy = int(p.y);
    Box px = tiles ? Box{true, ix / 8 * 8, iy / 8 * 8, std::min(ix / 8 * 8 + 8, int(kGeo[v].W)),
                         std::min(iy / 8 * 8 + 8, int(kGeo[v].H))}
                   : Box{true, ix, iy, ix + 1, iy + 1};
    const int k = (iy / g) * gw + ix / g;
    if (!grid[k].ok) touched.push_back(k);
    grid[k] = sy::Union(grid[k], px);
  }
  for (int k : touched) {
    EncodeBox(box, grid[k]);
    if (ang) EncodeAng(ang, pose, kGeo[v].W, kGeo[v].H, grid[k]);
    grid[k] = Box{};
  }
}

struct ViewStats {
  uint64_t visible = 0, present = 0, overflow = 0, miss = 0, hudOnly = 0, flagOverflowWithPresent = 0;
  uint32_t maxW = 0, maxH = 0;
};
struct RunOut {
  ViewStats v[4];
};
// One replay arm. arm 0: R95 (old::), 1: R97 (production). Phases: still `s0` frames, yaw `dps` for `yawFrames`, still
// `s2` frames; 45 frames per second; the read-back two frames late.
inline RunOut Replay(int arm, const Scene& sc, int s0, int yawFrames, double dps, int s2, int phaseFrom, int phaseTo) {
  RunOut out;
  sy::Field nf[4];
  old::Field of[4];
  struct Pending {
    uint64_t frame;
    bool win[4], scan[4], hudOnly[4];
    sy::StatMeta ob[4];  // R101: the sides each view's window / view showed (DecideWindow's masks)
    float q[4];
    uint32_t wx[4], wy[4], ww[4], wh[4];
    uint32_t w[4][sy::kStatU];
  };
  std::deque<Pending> q;
  const int total = s0 + yawFrames + s2;
  std::vector<Pt> hud, hel, cue;
  for (int f = 1; f <= total; ++f) {
    const uint64_t frame = static_cast<uint64_t>(f);
    const int yf = std::clamp(f - s0, 0, yawFrames);
    const double yaw = dps * yf / 45.0;
    float qq[4];
    Quat(yaw, -3, qq);
    Pending pd{};
    pd.frame = frame;
    memcpy(pd.q, qq, sizeof(qq));
    for (int v = 0; v < 4; ++v) {
      const ViewGeo& g = kGeo[v];
      const Pose pose = PoseQ(qq, g.tan);
      const uint32_t cap = v < 2 ? 512 : 1024;
      bool scan;
      sy::WinOut w;
      bool hudOnly = false;
      if (arm == 0) {
        old::Field& L = of[v];
        const bool learnt = L.col.ok || L.hel.ok;
        scan = L.scanReq || (!learnt && frame - L.lastScan >= sy::kScanRetry) ||
               (learnt && (frame + uint64_t(v) * 16) % sy::kScanEvery == 0);
        if (scan) L.scanReq = false, L.lastScan = frame;
        w = old::Decide(L, pose, g.W, g.H, cap, frame);
      } else {
        sy::Field& L = nf[v];
        const bool learnt = L.col.ok || sy::HelmetExtent(L, frame).ok;
        scan = sy::ScanDue(L, frame, v, learnt);  // R101: production's schedule (+ the fallback's)
        if (scan) L.scanReq = false, L.lastScan = frame;
        w = sy::DecideWindow(L, pose, g.W, g.H, cap, frame);
        hudOnly = L.hudOnly;
        sy::NoteObs(pd.ob[v], L);
      }
      // What this frame renders in the view.
      HudPoints(sc, qq, v, hud);
      hel.clear();
      if (sc.helmet[v].ok)
        for (int i = 0; i <= 8; ++i)
          for (int j = 0; j <= 8; ++j)
            hel.push_back({sc.helmet[v].x0 + (sc.helmet[v].x1 - 1 - sc.helmet[v].x0) * i / 8.0,
                           sc.helmet[v].y0 + (sc.helmet[v].y1 - 1 - sc.helmet[v].y0) * j / 8.0});
      if (sc.cue && (v & 1)) {
        CuePoints(qq, 0, v, cue);
        hel.insert(hel.end(), cue.begin(), cue.end());
      }
      const bool inPhase = f >= phaseFrom && f < phaseTo;
      ViewStats& st = out.v[v];
      if (inPhase && !hud.empty()) {
        ++st.visible;
        if (w.overflow) ++st.overflow;
        if (w.any && !w.overflow) {
          ++st.present;
          if (hudOnly) ++st.hudOnly;
          st.maxW = std::max(st.maxW, w.w), st.maxH = std::max(st.maxH, w.h);
          bool miss = false;
          for (const Pt& p : hud) miss = miss || p.x < w.x || p.y < w.y || p.x >= w.x + w.w || p.y >= w.y + w.h;
          st.miss += miss;
        }
      }
      // The statistics of this frame (read back two frames later).
      uint32_t* s = pd.w[v];
      pd.win[v] = w.any && !w.overflow;
      pd.scan[v] = scan;
      pd.hudOnly[v] = hudOnly;
      pd.wx[v] = w.x, pd.wy[v] = w.y, pd.ww[v] = w.w, pd.wh[v] = w.h;
      auto inWin = [&](const Pt& p) { return p.x >= w.x && p.y >= w.y && p.x < w.x + w.w && p.y < w.y + w.h; };
      if (pd.win[v]) {
        std::vector<Pt> in1, in2;
        bool edge = false;
        for (const Pt& p : hud) {
          if (inWin(p)) {
            in1.push_back(p);
            const bool e = (w.x > 0 && p.x < w.x + 2) || (w.y > 0 && p.y < w.y + 2) ||
                           (w.x + w.w < g.W && p.x >= w.x + w.w - 2) || (w.y + w.h < g.H && p.y >= w.y + w.h - 2);
            edge = edge || e;
          } else {
            edge = true;  // the HUD crosses the window's edge: changed texels at its inner edge
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
        s[sy::kStAlphaCore] = s[sy::kStChanged];
      }
      if (scan) {
        GroupStats(hud, v, 64, true, s + sy::kStScan1, s + sy::kStScanAng1, pose);
        GroupStats(hel, v, 64, true, s + sy::kStScan2, nullptr, pose);
      }
    }
    q.push_back(pd);
    // The read-back of the frame two before this one.
    while (!q.empty() && q.front().frame + 2 <= frame) {
      const Pending& r = q.front();
      for (int v = 0; v < 4; ++v) {
        const ViewGeo& g = kGeo[v];
        const uint32_t* s = r.w[v];
        if (arm == 0) {
          old::Field& L = of[v];
          const Pose rp = PoseQ(r.q, g.tan);
          for (int k : {sy::kStBox1, sy::kStScan1}) sy::LearnCone(L.col, rp, g.W, g.H, sy::DecodeBox(s + k));
          for (int k : {sy::kStBox2, sy::kStScan2}) {
            const Box b = sy::DecodeBox(s + k);
            if (b.ok) L.hel = sy::Union(L.hel, b), L.helSeen = r.frame;
          }
          if (r.win[v] && s[sy::kStEdge]) {
            L.scanReq = true;
            L.extra = std::min(L.extra + 8, sy::kMaxExtra);
            L.lastEdge = r.frame;
          } else if (L.extra > 0 && r.frame - L.lastEdge > 600) {
            --L.extra;
          }
        } else {
          sy::StatMeta m;
          m.win = r.win[v], m.scan = r.scan[v], m.hudOnly = r.hudOnly[v], m.angOk = true;
          memcpy(m.q, r.q, sizeof(m.q));
          m.pose = PoseQ(r.q, g.tan);
          m.W = g.W, m.H = g.H, m.frame = r.frame;
          m.wx = r.wx[v], m.wy = r.wy[v], m.ww = r.ww[v], m.wh = r.wh[v];
          m.obsWinL = r.ob[v].obsWinL, m.obsViewL = r.ob[v].obsViewL, m.obsWinR = r.ob[v].obsWinR,
          m.obsViewR = r.ob[v].obsViewR;  // R101
          sy::LearnCount n;
          sy::LearnStats(nf[v], m, s, n);
        }
      }
      q.pop_front();
    }
  }
  return out;
}

inline double Pct(uint64_t a, uint64_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; }

void TestReplay() {
  // The HUD of R96's F-16 / F/A-18 (still windows: periphery ~154, focus ~808: about 0.31 tangent units = 17.6 deg
  // square), its centre 10.8 deg below the view axis at the pose hook's pitch -3 (R96's fitted tanEl -0.19).
  Scene f16{-8.8, 8.8, -22.8, -4.8, {}, false};
  // R96's run: still 75 s, yaw 30 deg/s for 72 s (six turns), still2 15 s. Measured: still 600 frames (the windows
  // settle), the yaw phase, then still2.
  const int s0 = 600, yf = 72 * 45, s2 = 15 * 45;
  const RunOut o16 = Replay(0, f16, s0, yf, 30, s2, s0, s0 + yf), n16 = Replay(1, f16, s0, yf, 30, s2, s0, s0 + yf);
  // F/A-18: the JHMCS in the right eye: head-fixed symbols beside the HUD (in the focus view the union needs ~960 px,
  // R94's figure; the HUD alone ~750) and a world-referenced cue.
  Scene f18 = f16;
  {
    float q0[4];
    Quat(0, -3, q0);
    std::vector<Pt> h;
    HudPoints(f18, q0, 3, h);
    double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
    for (const Pt& p : h) x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
    f18.helmet[3] = {true, int(x1) - 960, int(y0) + 100, int(x1) - 960 + 120, int(y0) + 220};
    HudPoints(f18, q0, 1, h);
    x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
    for (const Pt& p : h) x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
    f18.helmet[1] = {true, int(x0) - 30, int(y1) + 10, int(x0) - 10, int(y1) + 30};
    f18.cue = true;
  }
  const RunOut oStill = Replay(0, f18, s0, yf, 30, s2, 100, s0), nStill = Replay(1, f18, s0, yf, 30, s2, 100, s0);
  const RunOut o18 = Replay(0, f18, s0, yf, 30, s2, s0, s0 + yf), n18 = Replay(1, f18, s0, yf, 30, s2, s0, s0 + yf);
  const RunOut o18b = Replay(0, f18, s0, yf, 30, s2, s0 + yf + 60, s0 + yf + s2),
               n18b = Replay(1, f18, s0, yf, 30, s2, s0 + yf + 60, s0 + yf + s2);
  auto line = [](const char* name, const RunOut& o, const RunOut& n) {
    printf("     (R97 replay %s: overflow / HUD-visible view-frames per view, R95 -> R97:", name);
    for (int v = 0; v < 4; ++v)
      printf(" v%d %.1f%% -> %.1f%% (%llu, present %.1f%% -> %.1f%%, max %ux%u -> %ux%u, misses %llu -> %llu, HUD-only %llu)",
             v, Pct(o.v[v].overflow, o.v[v].visible), Pct(n.v[v].overflow, n.v[v].visible),
             static_cast<unsigned long long>(n.v[v].visible), Pct(o.v[v].present, o.v[v].visible),
             Pct(n.v[v].present, n.v[v].visible), o.v[v].maxW, o.v[v].maxH, n.v[v].maxW, n.v[v].maxH,
             static_cast<unsigned long long>(o.v[v].miss), static_cast<unsigned long long>(n.v[v].miss),
             static_cast<unsigned long long>(n.v[v].hudOnly));
    printf(")\n");
  };
  line("F-16 yaw 30 deg/s", o16, n16);
  line("F/A-18 + JHMCS still", oStill, nStill);
  line("F/A-18 + JHMCS yaw 30 deg/s", o18, n18);
  line("F/A-18 + JHMCS still2 (after the yaw)", o18b, n18b);
  uint64_t oo = 0, ov = 0, no = 0, nv = 0, nm = 0, nm2 = 0;
  for (int v = 0; v < 4; ++v) {
    oo += o16.v[v].overflow + o18.v[v].overflow, ov += o16.v[v].visible + o18.v[v].visible;
    no += n16.v[v].overflow + n18.v[v].overflow, nv += n16.v[v].visible + n18.v[v].visible;
    nm += n16.v[v].miss + n18.v[v].miss + nStill.v[v].miss;
    nm2 += Pct(n16.v[v].overflow, n16.v[v].visible) >= 5 || Pct(n18.v[v].overflow, n18.v[v].visible) >= 5;
  }
  char what[600];
  sprintf_s(what,
            "R97 replay (R96's geometry, 30 deg/s yaw, F-16 and F/A-18 with the JHMCS): overflow on %.1f%% of the "
            "HUD-visible view-frames with R95's logic (R96 measured 29-37%%), %.1f%% with R97's (target < 5%% in every "
            "view: %llu views over); the F/A-18's right focus view while still: R95 %.0f%% overflow, R97 %.0f%% (present "
            "%.0f%%, HUD-only); after the yaw (still2) its right periphery %.0f%% -> %.0f%%; window misses with R97 %llu",
            Pct(oo, ov), Pct(no, nv), static_cast<unsigned long long>(nm2), Pct(oStill.v[3].overflow, oStill.v[3].visible),
            Pct(nStill.v[3].overflow, nStill.v[3].visible), Pct(nStill.v[3].present, nStill.v[3].visible),
            Pct(o18b.v[1].overflow, o18b.v[1].visible), Pct(n18b.v[1].overflow, n18b.v[1].visible),
            static_cast<unsigned long long>(nm));
  Check(Pct(oo, ov) > 15 && nm2 == 0 && Pct(no, nv) < 5 && oStill.v[3].overflow > 0 && nStill.v[3].overflow == 0 &&
            nStill.v[3].present == nStill.v[3].visible && nStill.v[3].hudOnly == nStill.v[3].present &&
            n18b.v[1].overflow == 0 && nm == 0,
        what);
}

// ---------------------------------------------------------------------------
// Pure parts
// ---------------------------------------------------------------------------
void TestProject() {
  // A HUD leaving the left periphery through its right side (yaw): the clipped projection's height is the visible
  // part's (R95: the whole outline's, 211 -> 347 px at the edge in R96); every visible HUD point inside the box.
  Scene s{-8.8, 8.8, -22.8, -4.8, {}, false};
  Cone c;
  c.ok = true, c.az0 = s.az0 * kDeg, c.az1 = s.az1 * kDeg, c.el0 = s.el0 * kDeg, c.el1 = s.el1 * kDeg;
  int worstOld = 0, worstNew = 0, outside = 0, wholeOld = 0, wholeNew = 0, ghostOld = 0, ghostNew = 0;
  std::vector<Pt> pts;
  for (double yaw = 0; yaw > -180; yaw -= 1.0) {  // the HUD moves right in view 0 as the head turns left
    float q[4];
    Quat(yaw, -3, q);
    const Pose p = PoseQ(q, kGeo[0].tan);
    const Box bn = sy::ProjectCone(c, p, 1040, 1028);
    const Box bo = sy::Clip(old::ProjectConeR95(c, p, 1040, 1028), 1040, 1028);
    HudPoints(s, q, 0, pts);
    if (pts.empty()) {
      ghostOld += bo.ok;  // nothing visible, a box anyway (R95: partly behind -> the whole view)
      ghostNew += bn.ok;
      continue;
    }
    double y0 = 1e9, y1 = -1e9;
    for (const Pt& t : pts) {
      y0 = std::min(y0, t.y), y1 = std::max(y1, t.y);
      outside += !bn.ok || t.x < bn.x0 - 0.5 || t.x > bn.x1 + 0.5 || t.y < bn.y0 - 0.5 || t.y > bn.y1 + 0.5;
    }
    const int h = int(y1 - y0);
    worstNew = std::max(worstNew, bn.ok ? (bn.y1 - bn.y0) - h : 0);
    worstOld = std::max(worstOld, bo.ok ? (bo.y1 - bo.y0) - h : 0);
    wholeOld += bo.ok && bo.x1 - bo.x0 == 1040 && bo.y1 - bo.y0 == 1028;
    wholeNew += bn.ok && bn.x1 - bn.x0 == 1040 && bn.y1 - bn.y0 == 1028;
  }
  char what[500];
  sprintf_s(what,
            "R97 field projection clipped to the view frustum: a HUD crossing the left periphery's edge under yaw: the "
            "box is the visible part's (height over the visible HUD's at most %d px; R95 %d px); %d visible HUD points "
            "outside it; HUD partly behind / outside the view: whole-view boxes R95 %d, R97 %d; a box with nothing "
            "visible R95 %d, R97 %d",
            worstNew, worstOld, outside, wholeOld, wholeNew, ghostOld, ghostNew);
  Check(worstNew <= 12 && worstOld > 60 && outside == 0 && wholeNew == 0 && ghostNew == 0 && ghostOld > 0, what);
}

void TestLearnAngles() {
  // The field learnt at a view edge (yaw 40 deg: the HUD near the right periphery's edge), from the pixel box (R95)
  // and from the view-relative angles of the 8 x 8 groups (R97, as the GPU computes them): R97 within a pixel's angle
  // of the true rectangle, R95 inflated.
  Scene s{-8.8, 8.8, -22.8, -4.8, {}, false};
  float q[4];
  Quat(-40, -3, q);
  std::vector<Pt> pts;
  HudPoints(s, q, 0, pts);
  uint32_t w[8] = {};
  GroupStats(pts, 0, 8, false, w, w + 4, PoseQ(q, kGeo[0].tan));
  Cone a, b;
  const bool la = sy::LearnConeAng(a, sy::DecodeAng(w + 4));
  const bool lb = sy::LearnCone(b, PoseQ(q, kGeo[0].tan), 1040, 1028, sy::DecodeBox(w));
  const double tw = 17.6 * kDeg, th = 18.0 * kDeg;
  const double ga = (a.az1 - a.az0) * (a.el1 - a.el0) / (tw * th), gb = (b.az1 - b.az0) * (b.el1 - b.el0) / (tw * th);
  const double eA = std::max(std::max(std::fabs(a.az0 - s.az0 * kDeg), std::fabs(a.az1 - s.az1 * kDeg)),
                             std::max(std::fabs(a.el0 - s.el0 * kDeg), std::fabs(a.el1 - s.el1 * kDeg)));
  // the angle encoding round trip
  double worst = 0;
  for (double x = -1.55; x < 1.55; x += 0.0137) worst = std::max(worst, std::fabs(sy::DecAng(sy::EncAng(x)) - x));
  char what[400];
  sprintf_s(what,
            "R97 field learning at a periphery edge (yaw 40 deg, pitch -3): from the groups' tracking-space angles "
            "%.3f x the true "
            "rectangle's area (worst edge %.2f mrad: a pixel there ~3 mrad, the texels' own extent); from the pixel box (R95) %.3f x; the angle "
            "encoding round trip %.1e rad",
            ga, eA * 1000, gb, worst);
  Check(la && lb && ga < 1.06 && eA < 6e-3 && gb > 1.15 && worst < 1e-8, what);
}

void TestHelmetAndFallback() {
  sy::Field L;
  // Decay: a cue moving across the view for 300 frames, then only the head-fixed box: the extent shrinks back.
  for (uint64_t f = 1; f <= 300; ++f) {
    sy::NoteHelmet(L, Box{true, 100, 100, 140, 140}, f);
    sy::NoteHelmet(L, Box{true, int(200 + f * 2), 300, int(210 + f * 2), 310}, f);
  }
  const Box grown = sy::HelmetExtent(L, 300);
  for (uint64_t f = 301; f <= 600; ++f) sy::NoteHelmet(L, Box{true, 100, 100, 140, 140}, f);
  const Box after = sy::HelmetExtent(L, 600);
  const bool decay = grown.x1 >= 800 && after.ok && after.x0 == 100 && after.x1 == 140 && after.y1 == 140;
  // The HUD-only fallback: a focus view whose HUD needs ~760 px and the helmet's union ~1000 (> 1024 with the margin).
  sy::Field F;
  F.col.ok = true;
  F.col.az0 = -8.8 * kDeg, F.col.az1 = 8.8 * kDeg, F.col.el0 = -22.8 * kDeg, F.col.el1 = -4.8 * kDeg;
  float q[4];
  Quat(0, -3, q);
  const Pose p = PoseQ(q, kGeo[3].tan);
  const Box hud = sy::ProjectCone(F.col, p, 2028, 2004);
  sy::NoteHelmet(F, Box{true, hud.x1 - 980, hud.y0 + 100, hud.x1 - 900, hud.y0 + 200}, 1);
  const sy::WinOut w1 = sy::DecideWindow(F, p, 2028, 2004, 1024, 1);
  const bool fb = F.hudOnly && w1.any && !w1.overflow && w1.w <= 1024 && int(w1.x) <= hud.x0 && int(w1.x + w1.w) >= hud.x1;
  // The helmet moves inside (the union fits): the helmet comes back after kHelmetBack frames (not earlier).
  int stillOnly = 0;
  uint64_t f = 2;
  for (; f < 2 + sy::kHelmetBack - 1; ++f) {
    sy::NoteHelmet(F, Box{true, hud.x0 + 20, hud.y0 + 20, hud.x0 + 60, hud.y0 + 60}, f + 400);
    sy::DecideWindow(F, p, 2028, 2004, 1024, f + 400);
    stillOnly += F.hudOnly;
  }
  sy::DecideWindow(F, p, 2028, 2004, 1024, f + 400);
  const bool back = stillOnly == sy::kHelmetBack - 1 && !F.hudOnly;
  // Even the HUD alone overflowing (a 600 px capacity): overflow (present 0).
  sy::Field G = sy::Field{};
  G.col = F.col;
  const sy::WinOut w3 = sy::DecideWindow(G, p, 2028, 2004, 600, 1);
  printf("     (R97 helmet test parts: decay %d (grown x1 %d, after %d..%d), fallback %d (window %u at %u, HUD %d..%d), back %d "
         "(%d HUD-only frames), HUD-alone overflow %d)\n",
         decay, grown.x1, after.x0, after.x1, fb, w1.w, w1.x, hud.x0, hud.x1, back, stillOnly, w3.overflow);
  Check(decay && fb && back && w3.overflow,
        "R97 helmet: its extent decays (a cue's sweep forgotten after ~192 frames, the head-fixed box kept); the "
        "HUD-only fallback when the union overflows and the HUD fits (a window, no overflow: present with the HUD only), "
        "the helmet back after 45 fitting frames; overflow only when the HUD alone does not fit");
}

// R96's alpha statistics per view (failing texels %, cores): R95's vote (8 agreeing, any view invalid -> no) and
// R97's (complete still read-backs, cores, latch) over R96's phases.
void TestAlphaReplay() {
  struct Case {
    const char* name;
    double badPct[4];
    bool cores[4];
  };
  // night F-16: left views 0-0.6 % failing, right 1.2-2.2 %; the added alpha's max 0.23-0.39 (no core) [R96 §3.2]
  // night F-4E: 12 % (the sight's additive alpha); day: 100 % (alpha before not 0)
  const Case cases[] = {{"night F-16", {0.6, 1.5, 0.0, 1.6}, {false, false, false, false}},
                        {"night F-4E", {12, 12, 12, 12}, {true, true, true, true}},
                        {"night F/A-18", {45, 46, 44, 46}, {false, false, false, false}},
                        {"day", {100, 100, 100, 100}, {false, false, false, false}},
                        {"a valid coverage alpha", {0.2, 0.2, 0.1, 0.1}, {true, true, true, true}}};
  const int keep = sy::g_alphaOnRun.load();
  sy::g_alphaOnRun = 256;
  char what[700];
  int flipsOld = 0, flipsNew = 0;
  bool validOn = false;
  std::string per;
  for (const Case& c : cases) {
    sy::AlphaHold a;  // R97
    bool oldOn = false;
    int oldRun = 0, fo = 0;
    uint32_t prevWin[4][4] = {};
    // still 75 s, yaw 72 s at 30 deg/s (the HUD in each view about a third of each 12 s turn, views 2 / 3 less), still2
    for (int f = 0; f < 75 * 45 + 72 * 45 + 15 * 45; ++f) {
      const bool yaw = f >= 75 * 45 && f < 147 * 45;
      const double ph = yaw ? std::fmod((f - 75 * 45) / 45.0 * 30.0, 360.0) : 0.0;
      bool seen[4] = {true, true, true, true}, win[4], same[4];
      int verdict[4] = {}, oldVerdict = 0;
      for (int v = 0; v < 4; ++v) {
        const double half = v < 2 ? 60 : 25, off = (v & 1) ? 8 : -8;  // the HUD's visible span of the view, per eye
        const double d = std::fmod(ph + 180 + off, 360.0) - 180;
        win[v] = std::fabs(d) < half;
        const uint32_t pos = win[v] ? static_cast<uint32_t>(500 + d * 10) : 0;
        same[v] = prevWin[v][0] == pos;
        prevWin[v][0] = pos;
        if (!win[v]) continue;
        const uint32_t changed = 1000, bad = static_cast<uint32_t>(c.badPct[v] * 10), core = c.cores[v] ? 200 : 0;
        verdict[v] = sy::AlphaViewVerdict(changed, bad, core);
        const bool okOld = uint64_t(bad) * 100 <= changed;  // R95's per-view rule
        oldVerdict = !okOld ? -1 : oldVerdict == 0 ? 1 : oldVerdict;
      }
      // R95: flips after 8 agreeing read-backs
      if (oldVerdict) {
        if ((oldVerdict > 0) == oldOn) oldRun = 0;
        else if (++oldRun >= 8) oldOn = oldVerdict > 0, oldRun = 0, ++fo;
      }
      sy::AlphaVote(a, sy::AlphaFrameVote(seen, win, verdict, same));
    }
    if (c.name[0] == 'a') {
      validOn = a.on && a.flips == 1;
    } else {
      flipsOld += fo, flipsNew += static_cast<int>(a.flips);
    }
    char b[160];
    sprintf_s(b, "%s%s: R95 %d flips, R97 %u (%s)", per.empty() ? "" : "; ", c.name, fo, a.flips, a.on ? "valid" : "clear");
    per += b;
  }
  sy::g_alphaOnRun = keep;
  sprintf_s(what,
            "R97 alpha over R96's per-view statistics and phases: %s. R96's scenarios: R95 %d flips, R97 %d; a valid "
            "coverage alpha comes on once and stays (%d)",
            per.c_str(), flipsOld, flipsNew, validOn);
  Check(flipsOld > 0 && flipsNew == 0 && validOn, what);
  // The per-view verdict and the frame vote.
  bool s4[4] = {true, true, true, true}, w4[4] = {true, true, true, true}, m4[4] = {true, true, true, false};
  int v4[4] = {1, 1, 1, 1}, v4b[4] = {1, -1, 1, 1};
  bool w3[4] = {true, true, false, true};
  Check(sy::AlphaViewVerdict(1000, 0, 0) == -1 && sy::AlphaViewVerdict(1000, 11, 500) == -1 &&
            sy::AlphaViewVerdict(1000, 10, 10) == 1 && sy::AlphaViewVerdict(10, 0, 10) == 0 &&
            sy::AlphaFrameVote(s4, w4, v4, s4) == 1 && sy::AlphaFrameVote(s4, w4, v4b, s4) == -1 &&
            sy::AlphaFrameVote(s4, w4, v4b, m4) == 0 && sy::AlphaFrameVote(s4, w3, v4, s4) == 0,
        "R97 alpha verdicts: a view without opaque cores (R96 night: added alpha <= 0.39) or > 1 % failing texels is "
        "invalid; a read-back votes only with every view carrying symbols and no window moved");
}

void TestLogAndConfig() {
  // The record render point's state line: hooked logged at the first tick that sees it; not hooked only after the
  // grace (R96: the install line said NOT hooked before ovl_cpu.h's install in the same millisecond).
  auto capture = [](bool on) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (on) g_reportBuf.clear();
    g_reportThread = GetCurrentThreadId();
    g_reportCapture = on;
  };
  auto taken = [] {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::string r;
    r.swap(g_reportBuf);
    return r;
  };
  const bool keepRp = sy::g_testRenderPoint.load();
  capture(true);
  sy::g_testRenderPoint = false;
  sy::NoteInstall();
  sy::TickRenderPoint();  // within the grace: nothing
  const std::string a = taken();
  sy::g_testRenderPoint = true;
  sy::TickRenderPoint();  // hooked now
  sy::TickRenderPoint();  // (once)
  const std::string b = taken();
  sy::g_testRenderPoint = false;
  sy::NoteInstall();
  sy::g_rpSince = GetTickCount64() - sy::kRpGraceMs - 1;
  sy::TickRenderPoint();
  const std::string c = taken();
  capture(false);
  sy::g_testRenderPoint = keepRp;
  auto count = [](const std::string& s, const char* w) {
    int n = 0;
    for (size_t at = s.find(w); at != std::string::npos; at = s.find(w, at + 1)) ++n;
    return n;
  };
  Check(a.find("render point") == std::string::npos && count(b, "record render point hooked") == 1 &&
            count(c, "record render point NOT hooked") == 1,
        "R97 log: the record render point's state is logged once it is known (hooked at the first tick that sees it; "
        "not hooked only after a 10 s grace), not at the install before ovl_cpu.h hooked it");
  // The VRAM figures of the creation line (the default capacities): ~97 MB as R95 designed.
  const sy::Vram v = sy::VramOf(2560, 1024, 512, 1024);
  char what[300];
  sprintf_s(what, "R97 VRAM line: shared %.1f MB (3 x (E %.1f + class %.1f)), private %.1f MB (snapshot %.1f, accumulator "
                  "%.1f, signatures %.1f), in all %.1f MB",
            v.shared, v.e, v.cls, v.priv, v.pre, v.acc, v.sig, v.total);
  Check(std::fabs(v.shared - 70.8) < 0.1 && std::fabs(v.priv - 26.2) < 0.1 && v.total < 97.1 && v.snap == 1024, what);
  // [Dev] SymbologyPublishAlways reaches the export (ConfigureSymbology's third argument; default off).
  const bool k0 = cmask::g_symPublishAlways.load();
  cmask::ConfigureSymbology(cmask::g_symOn.load(), cmask::g_symAlpha.load(), true);
  const bool on = cmask::g_symPublishAlways.load();
  cmask::ConfigureSymbology(cmask::g_symOn.load(), cmask::g_symAlpha.load());
  Check(!k0 && on && !cmask::g_symPublishAlways.load(),
        "R97 [Dev] SymbologyPublishAlways: default off, set by the configuration, off again without it");
}

// The GPU's view-relative angles (WARP): the accumulation's type 1 angle box against the CPU formula over the changed
// texels' 8 x 8 groups; the class-gated finalize leaves texels without a class untouched.
void TestGpuAngles() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* c = nullptr;
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 1, D3D11_SDK_VERSION, &dev, nullptr, &c))) {
    Check(false, "R97 GPU angles: a WARP device");
    return;
  }
  const UINT W = 320, H = 300, wx = 40, wy = 30, ww = 200, wh = 180;
  const float tan[4] = {-1.5f, 0.95f, 1.2f, -1.2f};
  float gq[4];
  r95test::QuatYawPitch(35, -4, 3, gq);
  std::vector<uint16_t> pre(size_t(W) * H * 4, 0), post;
  post = pre;
  std::vector<Pt> pts;
  for (UINT y = wy; y < wy + wh; ++y)
    for (UINT x = wx; x < wx + ww; ++x)
      if (((x * 7 + y * 3) % 23 == 0) && x > 60 && y > 50 && x < 220 && y < 190) {
        post[(size_t(y) * W + x) * 4 + 1] = r95test::F2H(1.0f);
        pts.push_back({double(x), double(y)});
      }
  D3D11_TEXTURE2D_DESC td{};
  td.Width = W, td.Height = H, td.MipLevels = td.ArraySize = 1, td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  td.SampleDesc.Count = 1, td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA a{post.data(), W * 8, 0};
  ID3D11Texture2D *hdr = nullptr, *pw = nullptr, *acc = nullptr, *cls = nullptr;
  ID3D11ShaderResourceView *hdrSrv = nullptr, *pwSrv = nullptr;
  ID3D11UnorderedAccessView *accUav = nullptr, *clsUav = nullptr;
  bool ok = SUCCEEDED(dev->CreateTexture2D(&td, &a, &hdr)) && SUCCEEDED(dev->CreateShaderResourceView(hdr, nullptr, &hdrSrv));
  std::vector<uint16_t> preWin(size_t(ww) * wh * 4, 0);
  D3D11_TEXTURE2D_DESC pd = td;
  pd.Width = ww, pd.Height = wh;
  D3D11_SUBRESOURCE_DATA pz{preWin.data(), ww * 8, 0};
  ok = ok && SUCCEEDED(dev->CreateTexture2D(&pd, &pz, &pw)) && SUCCEEDED(dev->CreateShaderResourceView(pw, nullptr, &pwSrv));
  D3D11_TEXTURE2D_DESC ad = pd;
  ad.Format = DXGI_FORMAT_R32G32B32A32_FLOAT, ad.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  ok = ok && SUCCEEDED(dev->CreateTexture2D(&ad, nullptr, &acc)) && SUCCEEDED(dev->CreateUnorderedAccessView(acc, nullptr, &accUav));
  D3D11_TEXTURE2D_DESC cd = ad;
  cd.Format = DXGI_FORMAT_R8_UINT;
  ok = ok && SUCCEEDED(dev->CreateTexture2D(&cd, nullptr, &cls)) && SUCCEEDED(dev->CreateUnorderedAccessView(cls, nullptr, &clsUav));
  ok = ok && cmask::Compile() && sy::MakeBase(dev);
  uint32_t got[sy::kStatU] = {};
  if (ok) {
    uint32_t p[32] = {wx, wy, ww, wh, 0, 0, 0, 15, 0, 0, W, H, (W + 7) / 8, (H + 7) / 8, 0, 0};
    memcpy(p + 16, tan, sizeof(tan));
    float rot[12];
    sy::RotRows(gq, rot);
    memcpy(p + 20, rot, sizeof(rot));
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sy::kCbBytes, bd.Usage = D3D11_USAGE_DEFAULT, bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cbd{p, 0, 0};
    ID3D11Buffer* cb = nullptr;
    dev->CreateBuffer(&bd, &cbd, &cb);
    const UINT zero[4] = {};
    c->ClearUnorderedAccessViewUint(sy::g_b.statUav, zero);
    c->CSSetShader(sy::g_b.cs[0], nullptr, 0);
    c->CSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView* srv[2] = {hdrSrv, pwSrv};
    c->CSSetShaderResources(0, 2, srv);
    ID3D11UnorderedAccessView* uav[3] = {accUav, sy::g_b.statUav, clsUav};
    c->CSSetUnorderedAccessViews(0, 3, uav, nullptr);
    c->Dispatch((ww + 7) / 8, (wh + 7) / 8, 1);
    c->CopyResource(sy::g_b.stage[0], sy::g_b.stat);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(c->Map(sy::g_b.stage[0], 0, D3D11_MAP_READ, 0, &m))) {
      memcpy(got, m.pData, sizeof(got));
      c->Unmap(sy::g_b.stage[0], 0);
    }
    Rel(cb);
  }
  // The CPU expectation: the changed texels' groups (window-relative 8 x 8 groups)
  uint32_t want4[4] = {};
  std::map<std::pair<int, int>, Box> groups;
  for (const Pt& p : pts) {
    Box& b = groups[{(int(p.x) - int(wx)) / 8, (int(p.y) - int(wy)) / 8}];
    b = sy::Union(b, Box{true, int(p.x), int(p.y), int(p.x) + 1, int(p.y) + 1});
  }
  for (const auto& kv : groups) EncodeAng(want4, PoseQ(gq, tan), W, H, kv.second);
  const sy::AngBox g = sy::DecodeAng(got + sy::kStAng1), w = sy::DecodeAng(want4);
  const double err = std::max(std::max(std::fabs(g.az0 - w.az0), std::fabs(g.az1 - w.az1)),
                              std::max(std::fabs(g.el0 - w.el0), std::fabs(g.el1 - w.el1)));
  char what[300];
  sprintf_s(what, "R97 GPU (WARP): the type 1 run's tracking-space angle box (%.4f..%.4f / %.4f..%.4f rad; head yaw 35, "
                  "pitch -4, roll 3) equals the CPU formula over its 8 x 8 groups (worst %.1e rad; a pixel ~6e-3 here); "
                  "changed %u",
            g.az0, g.az1, g.el0, g.el1, err, got[sy::kStChanged]);
  Check(ok && g.ok && w.ok && err < 5e-5 && got[sy::kStChanged] == pts.size(), what);
  sy::ReleaseBase();
  Rel(hdr), Rel(pw), Rel(acc), Rel(cls), Rel(hdrSrv), Rel(pwSrv), Rel(accUav), Rel(clsUav), Rel(c), Rel(dev);
}

// CPU cost of the pure parts (this machine): the window's projection with and without the caches, the learning.
void TestCpuCost() {
  sy::Field F;
  F.col.ok = true;
  F.col.az0 = -8.8 * kDeg, F.col.az1 = 8.8 * kDeg, F.col.el0 = -22.8 * kDeg, F.col.el1 = -4.8 * kDeg;
  float q[4];
  Quat(3, -3, q);
  const Pose p = PoseQ(q, kGeo[2].tan);
  LARGE_INTEGER f0, f1, fr;
  QueryPerformanceFrequency(&fr);
  constexpr int N = 4000;
  QueryPerformanceCounter(&f0);
  int sink = 0;
  for (int i = 0; i < N; ++i) sink += sy::ProjectCone(F.col, p, 2028, 2004).x0;
  QueryPerformanceCounter(&f1);
  const double full = (f1.QuadPart - f0.QuadPart) * 1e6 / fr.QuadPart / N;
  QueryPerformanceCounter(&f0);
  for (int i = 0; i < N; ++i) sink += int(sy::DecideWindow(F, p, 2028, 2004, 1024, 5).w);
  QueryPerformanceCounter(&f1);
  const double still = (f1.QuadPart - f0.QuadPart) * 1e6 / fr.QuadPart / N;
  Pose pm = p;
  QueryPerformanceCounter(&f0);
  for (int i = 0; i < N; ++i) {
    Quat(3 + i * 1e-3, -3, pm.q);
    sink += int(sy::DecideWindow(F, pm, 2028, 2004, 1024, 5).w);
  }
  QueryPerformanceCounter(&f1);
  const double moving = (f1.QuadPart - f0.QuadPart) * 1e6 / fr.QuadPart / N;
  sy::AngBox ab;
  ab.ok = true, ab.az0 = -0.15, ab.az1 = 0.15, ab.el0 = -0.2, ab.el1 = 0.0;
  Cone cc;
  QueryPerformanceCounter(&f0);
  for (int i = 0; i < N; ++i) sy::LearnConeAng(cc, ab);
  QueryPerformanceCounter(&f1);
  const double learn = (f1.QuadPart - f0.QuadPart) * 1e6 / fr.QuadPart / N;
  printf("     (R97 render-thread pure parts, this CPU: the field's projection %.2f us (R95's every frame per view), the "
         "window with the caches %.2f us (still head) / %.2f us (moving), a learning %.2f us (skipped while the "
         "observation repeats)) [%d]\n",
         full, still, moving, learn, sink & 1);
  Check(still < full / 4, "R97 cost: a still head's window comes from the caches (the field's outline and its last "
                          "projection), not the trigonometry");
}

// QV_R95_HW=1: the render thread's D3D11 calls on the hardware driver (CPU microseconds per call), the profile behind
// R97's cost choices.
void HwCallCost() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* c = nullptr;
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 1, D3D11_SDK_VERSION, &dev, nullptr, &c)))
    return;
  ID3D11DeviceContext1* c1 = nullptr;
  c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1));
  D3D11_TEXTURE2D_DESC td{};
  td.Width = 1040, td.Height = 1028, td.MipLevels = td.ArraySize = 1, td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  td.SampleDesc.Count = 1, td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D *hdr = nullptr, *pre = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  dev->CreateTexture2D(&td, nullptr, &hdr);
  dev->CreateRenderTargetView(hdr, nullptr, &rtv);
  dev->CreateShaderResourceView(hdr, nullptr, &srv);
  td.Width = td.Height = 512, td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  dev->CreateTexture2D(&td, nullptr, &pre);
  const bool ok = hdr && rtv && srv && pre && cmask::Compile() && sy::MakeBase(dev);
  if (ok) {
    ID3D11DeviceContext* keepC = fixdepth::g_s.ctx;
    ID3D11DeviceContext1* keepC1 = fixdepth::g_s.ctx1;
    fixdepth::g_s.ctx = c, fixdepth::g_s.ctx1 = c1;
    c->OMSetRenderTargets(1, &rtv, nullptr);
    LARGE_INTEGER f0, f1, fr;
    QueryPerformanceFrequency(&fr);
    auto us = [&](auto fn) {
      constexpr int N = 400;
      for (int i = 0; i < 20; ++i) fn();
      c->Flush();
      QueryPerformanceCounter(&f0);
      for (int i = 0; i < N; ++i) fn();
      QueryPerformanceCounter(&f1);
      c->Flush();
      return (f1.QuadPart - f0.QuadPart) * 1e6 / fr.QuadPart / N;
    };
    const double ovConflict = us([&] {
      cmask::covl::RunOv(sy::g_b.cs[4], sy::g_b.cb[0], srv, nullptr, nullptr, sy::g_b.statUav, nullptr, 1, 1, hdr);
    });
    const double ovPlain = us([&] {
      cmask::covl::RunOv(sy::g_b.cs[4], sy::g_b.cb[0], nullptr, nullptr, nullptr, sy::g_b.statUav, nullptr, 1, 1, nullptr);
    });
    const D3D11_BOX b{100, 100, 0, 300, 300, 1};
    const double copy = us([&] { c->CopySubresourceRegion(pre, 0, 0, 0, 0, hdr, 0, &b); });
    uint32_t p[32] = {};
    int k = 0;
    const double upd = us([&] {
      p[0] = ++k;
      c->UpdateSubresource(sy::g_b.cb[1], 0, nullptr, p, 0, 0);
    });
    const double qend = us([&] { c->End(sy::g_b.ts[0][0][0]); });
    const double getRt = us([&] {
      ID3D11RenderTargetView* r = nullptr;
      c->OMGetRenderTargets(1, &r, nullptr);
      ID3D11Resource* res = nullptr;
      if (r) r->GetResource(&res), r->Release();
      if (res) res->Release();
    });
    printf("     (R97 render-thread D3D11 calls on the hardware driver, us per call: RunOv with DCS's target bound (OM "
           "unbind / rebind) %.2f, RunOv without %.2f, CopySubresourceRegion %.2f, UpdateSubresource 128 B %.2f, query "
           "End %.2f, OMGetRenderTargets + GetResource %.2f)\n",
           ovConflict, ovPlain, copy, upd, qend, getRt);
    fixdepth::g_s.ctx = keepC, fixdepth::g_s.ctx1 = keepC1;
  }
  sy::ReleaseBase();
  Rel(hdr), Rel(pre), Rel(rtv), Rel(srv), Rel(c1), Rel(c), Rel(dev);
}

// QV_R95_HW=1: the driver's allocation sizes of the symbology's textures (D3D12 GetResourceAllocationInfo on the same
// adapter, the same formats and flags; D3D11 has no such query), for Dev's VRAM split.
void HwAllocSizes() {
  HMODULE m = LoadLibraryW(L"d3d12.dll");
  if (!m) return;
  using Create = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
  auto create = reinterpret_cast<Create>(GetProcAddress(m, "D3D12CreateDevice"));
  ID3D12Device* d = nullptr;
  if (!create || FAILED(create(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&d))) || !d)
    return;
  auto size = [&](UINT w, UINT h, DXGI_FORMAT f, bool shared, bool rt) {
    D3D12_RESOURCE_DESC r{};
    r.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, r.Width = w, r.Height = h, r.DepthOrArraySize = 1, r.MipLevels = 1;
    r.Format = f, r.SampleDesc.Count = 1, r.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    r.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | (shared ? D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS : D3D12_RESOURCE_FLAG_NONE) |
              (rt ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE);
    return d->GetResourceAllocationInfo(0, 1, &r).SizeInBytes;
  };
  const UINT64 e = size(2560, 1024, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false),
               k = size(2560, 1024, DXGI_FORMAT_R8_UINT, true, false),
               pre = size(1024, 1024, DXGI_FORMAT_R16G16B16A16_FLOAT, false, false),
               acc = size(1024, 1024, DXGI_FORMAT_R32G32B32A32_FLOAT, false, false),
               sig = size(512, 512, DXGI_FORMAT_R32_UINT, false, false);
  printf("     (R97 the driver's allocation sizes (D3D12 GetResourceAllocationInfo, this GPU): E 2560x1024 RGBA16F %llu B, "
         "class R8_UINT %llu B (x 3 slots each), snapshot 1024^2 RGBA16F %llu B, accumulator RGBA32F %llu B, signatures "
         "%llu B: %.2f MB in all)\n",
         static_cast<unsigned long long>(e), static_cast<unsigned long long>(k), static_cast<unsigned long long>(pre),
         static_cast<unsigned long long>(acc), static_cast<unsigned long long>(sig),
         (3.0 * (e + k) + pre + acc + sig) / 1e6);
  d->Release();
}

inline void Run() {
  printf("-- R97 symbology fixes --\n");
  if (const char* e = getenv("QV_R95_HW"); e && atoi(e) != 0) HwAllocSizes();
  if (const char* e = getenv("QV_R95_HW"); e && atoi(e) != 0) HwCallCost();
  TestProject();
  TestLearnAngles();
  TestHelmetAndFallback();
  TestAlphaReplay();
  TestLogAndConfig();
  TestGpuAngles();
  TestCpuCost();
  TestReplay();
}

}  // namespace r97test
