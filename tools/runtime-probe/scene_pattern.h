// Runtime probe: the "scene" motion pattern (--motion-pattern scene). Shared by probe_run (which renders it into the four
// views) and probe_analyze (which knows where its markers are, and renders it for the self-test).
//
// A world pattern in tangent space, the same for the periphery and the focus views (each renders it at its own
// resolution), made from one seed and nothing else: a low-contrast multi-scale noise background, a field of discs of
// random size, brightness and colour over it, and a sparse set of MARKERS. Nothing in it repeats: there is no period
// in either direction, so a shifted copy matches the original at one displacement only.
//
// The content of frame p is the world shifted by p * v: a pixel at tangent t shows world(t - p * v). Because the world
// does not repeat, the pre-made images cannot loop: the probe sweeps p from 0 to phases - 1 and back (see probe_run).
//
// Colours: background and discs always have G >= max(R, B). A marker is a bullseye on black: a coloured disc (radius
// kMkDisc), a black gap, a white ring, a black rim. The disc has G = 0 and is RED in a focus view and BLUE in a
// periphery view, so max(R, B) - G finds markers and nothing else, and where the fade shows both renderings of one
// marker the red part is the focus layer's and the blue part the periphery layer's (the seam).
#pragma once
#include "common.h"
#include <algorithm>
#include <thread>

constexpr double kMkDisc = 0.008, kMkGap = 0.012, kMkRing = 0.016, kMkRim = 0.019; // radii, tangent
constexpr double kMkSpacing = 0.15;   // least distance between two markers (9 disc diameters, 4 bullseyes)
constexpr double kMkDensity = 13.0;   // markers per unit of tangent area
constexpr double kSceneHalf = 2.4;    // the world is defined for |x|, |y| up to this (its border values go on beyond)
constexpr double kSceneMarkers = 2.1; // markers are placed for |x|, |y| up to this
constexpr double kSceneSweep = 0.5;   // the longest shift the probe may ask for: FOV edge + shift stays inside the markers' area

struct ScenePattern {
  struct Table { int n = 2; double cell = 1; std::vector<float> v; };
  struct Disc { float x, y, r, c[3]; };
  struct Marker { double x, y; };
  uint32_t seed = 0;
  Table noise[5];              // 0-2 luminance octaves, 3 red tint, 4 blue tint
  int bn = 1; double bcell = 0.045; std::vector<Disc> discs;
  std::vector<Marker> markers; double minDist = 0;
  std::vector<std::vector<int>> rows; static constexpr double kRow = 0.05;
  uint64_t s_ = 0;
  uint64_t Next() { uint64_t z = (s_ += 0x9E3779B97F4A7C15ull); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
  double Uni() { return double(Next() >> 11) / 9007199254740992.0; }

  void Build(uint32_t seed_) {
    seed = seed_; s_ = 0x5CE11E5EEDull ^ (uint64_t(seed_) << 20);
    const double cell[5] = {0.09, 0.035, 0.013, 0.16, 0.16};
    for (int o = 0; o < 5; ++o) { Table& t = noise[o]; t.cell = cell[o]; t.n = int(2 * kSceneHalf / t.cell) + 3; t.v.resize(size_t(t.n) * t.n); for (auto& x : t.v) x = float(Uni()); }
    bn = int(2 * kSceneHalf / bcell) + 1; discs.resize(size_t(bn) * bn);
    for (int j = 0; j < bn; ++j) for (int i = 0; i < bn; ++i) { Disc& d = discs[size_t(j) * bn + i]; double a = Uni(), b = Uni(), k = Uni(), q = Uni(), br = Uni(), tr = Uni(), tb = Uni(), which = Uni();
      d.x = float(-kSceneHalf + (i + a) * bcell); d.y = float(-kSceneHalf + (j + b) * bcell); d.r = k < 0.25 ? 0.f : float(0.020 * (0.2 + 0.8 * q * q));
      double v = br < 0.45 ? 0.01 + 0.07 * br / 0.45 : 0.30 + 0.70 * (br - 0.45) / 0.55; // darker than the background, or brighter
      double sr = which < 0.35 ? 0 : 0.85 * tr, sb = which > 0.65 ? 0 : 0.85 * tb;         // green, yellow, cyan, grey, white: never G below R or B
      d.c[0] = float(v * (1 - sr)); d.c[1] = float(v); d.c[2] = float(v * (1 - sb)); }
    // Markers: best of 40 candidates each time (the one farthest from the markers so far), so they spread evenly.
    markers.clear(); int want = int(kMkDensity * 4 * kSceneMarkers * kSceneMarkers + 0.5); minDist = 1e9;
    for (int n = 0; n < want; ++n) { Marker best{0, 0}; double bestD = -1;
      for (int c = 0; c < 40; ++c) { Marker k{(Uni() * 2 - 1) * kSceneMarkers, (Uni() * 2 - 1) * kSceneMarkers}; double dmin = 1e9;
        for (auto& o : markers) dmin = std::min(dmin, hypot(k.x - o.x, k.y - o.y));
        if (dmin > bestD) { bestD = dmin; best = k; } }
      if (bestD >= kMkSpacing) { markers.push_back(best); if (bestD < minDist) minDist = bestD; } }
    rows.assign(size_t(2 * kSceneHalf / kRow) + 1, {});
    for (size_t r = 0; r < rows.size(); ++r) { double y0 = -kSceneHalf + r * kRow; for (size_t i = 0; i < markers.size(); ++i) if (markers[i].y > y0 - kMkRim - 0.01 && markers[i].y < y0 + kRow + kMkRim + 0.01) rows[r].push_back(int(i)); }
  }
  double Sample(const Table& t, double x, double y) const {
    double fx = (x + kSceneHalf) / t.cell, fy = (y + kSceneHalf) / t.cell, hi = t.n - 1.001;
    fx = fx < 0 ? 0 : (fx > hi ? hi : fx); fy = fy < 0 ? 0 : (fy > hi ? hi : fy);
    int i = int(fx), j = int(fy); double a = fx - i, b = fy - j; a = a * a * (3 - 2 * a); b = b * b * (3 - 2 * b);
    const float* r0 = &t.v[size_t(j) * t.n + i]; const float* r1 = r0 + t.n;
    return (r0[0] * (1 - a) + r0[1] * a) * (1 - b) + (r1[0] * (1 - a) + r1[1] * a) * b;
  }
  // Linear-light colour of the world at tangent (x, y). ppt = pixels per unit of tangent of the image being made (edge
  // antialiasing); focusView picks the marker disc's colour.
  void Eval(double x, double y, double ppt, bool focusView, double rgb[3]) const {
    double l = 0.5 * Sample(noise[0], x, y) + 0.3 * Sample(noise[1], x, y) + 0.2 * Sample(noise[2], x, y), L = 0.10 + 0.16 * l;
    rgb[0] = L * (1 - 0.35 * Sample(noise[3], x, y)); rgb[1] = L; rgb[2] = L * (1 - 0.35 * Sample(noise[4], x, y));
    int ci = int(floor((x + kSceneHalf) / bcell)), cj = int(floor((y + kSceneHalf) / bcell));
    for (int j = cj - 1; j <= cj + 1; ++j) { if (j < 0 || j >= bn) continue;
      for (int i = ci - 1; i <= ci + 1; ++i) { if (i < 0 || i >= bn) continue; const Disc& d = discs[size_t(j) * bn + i]; if (d.r <= 0) continue;
        double dx = x - d.x, dy = y - d.y, cov = (d.r - sqrt(dx * dx + dy * dy)) * ppt + 0.5; if (cov <= 0) continue; if (cov > 1) cov = 1;
        for (int k = 0; k < 3; ++k) rgb[k] += (d.c[k] - rgb[k]) * cov; } }
    int row = int(floor((y + kSceneHalf) / kRow)); if (row < 0 || row >= int(rows.size())) return;
    for (int mi : rows[row]) { const Marker& m = markers[mi]; double dx = x - m.x; if (dx > kMkRim + 0.01 || dx < -kMkRim - 0.01) continue; double dist = hypot(dx, y - m.y);
      auto cov = [&](double r) { double c = (r - dist) * ppt + 0.5; return c < 0 ? 0.0 : (c > 1 ? 1.0 : c); };
      double c0 = cov(kMkRim); if (c0 <= 0) continue; double c1 = cov(kMkRing), c2 = cov(kMkGap), c3 = cov(kMkDisc);
      const double disc[3] = {focusView ? 1.0 : 0.0, 0.0, focusView ? 0.0 : 1.0};
      for (int k = 0; k < 3; ++k) { double v = rgb[k] * (1 - c0); v += (1 - v) * c1; v *= 1 - c2; v += (disc[k] - v) * c3; rgb[k] = v; } }
  }
};

// Runs fn(y) for every row on all cores but one.
template <typename Fn> inline void SceneRows(int h, Fn fn) {
  unsigned n = std::thread::hardware_concurrency(); n = n > 2 ? std::min(n - 1, 32u) : 1;
  if (n == 1) { for (int y = 0; y < h; ++y) fn(y); return; }
  std::vector<std::thread> th; for (unsigned t = 0; t < n; ++t) th.emplace_back([=]() { for (int y = int(t); y < h; y += int(n)) fn(y); });
  for (auto& t : th) t.join();
}
inline const uint8_t* SceneEncLut() { static uint8_t lut[4097]; static bool init = false; if (!init) { for (int i = 0; i <= 4096; ++i) lut[i] = Enc(i / 4096.0); init = true; } return lut; }
// The pattern as one view shows it: FOV f (tangents), w x h pixels, content shifted by (sx, sy) of tangent. Opaque.
inline Image SceneImage(const ScenePattern& sp, int w, int h, const Fov& f, double sx, double sy, bool focusView) {
  Image im(w, h); const uint8_t* lut = SceneEncLut(); double ppt = w / (f.r - f.l);
  SceneRows(h, [&](int y) { double ty = f.u - (y + 0.5) / h * (f.u - f.d) - sy; uint8_t* o = im.at(0, y);
    for (int x = 0; x < w; ++x) { double rgb[3]; sp.Eval(f.l + (x + 0.5) / w * (f.r - f.l) - sx, ty, ppt, focusView, rgb);
      for (int k = 0; k < 3; ++k) { double q = rgb[k] < 0 ? 0 : (rgb[k] > 1 ? 1 : rgb[k]); o[x * 4 + k] = lut[int(q * 4096 + 0.5)]; } o[x * 4 + 3] = 255; } });
  return im;
}
