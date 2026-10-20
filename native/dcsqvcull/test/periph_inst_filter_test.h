// R38 rank 1, periphery instance filter (periph_inst_filter.h):
//  1. the conservative bound against a numerical model of every deformation the instancer VS can apply
//     (transform, LOD cross-fade, the blend<0 axis scaling, SpeedTree bend for any wind, leaf shake for any
//     noise / airflow), the NDC box against projected triangles, the zero-coverage predicate against exact
//     pixel-centre tests, the core predicate against dense sampling;
//  2. the module's GPU path on a synthetic DCS-like scene (WARP; also the hardware adapter with
//     QV_PIF_HW=1): classification against an independent CPU reference, stable partition per bucket, argument
//     counts, every other word unchanged, stock restored at the run end, commit refused on inconsistent lists,
//     census sums, the after-run checks;
//  3. raster exactness: an instancer VS with DCS's indexing draws the stock and the filtered lists into a
//     G-buffer-like target (colour = instance id, reverse-Z depth): bit-identical, and the verify path (dropped
//     set redrawn under occlusion queries) counts 0 samples; a negative control (mesh larger than its bound)
//     is caught by both;
//  4. with QV_R34_STOCK_DIR: DCS's own chain kernels (clear, calccounters, calcoffsets, sort, fillindirect;
//     copies of the 2.9.30 DXBC) build the lists the filter then works on;
//  5. the pass-boundary state machine with the one-shot identification arms on a WARP "DCS" context, the core
//     hole from a GazeOut block (test-only name suffix), and shutdown.
#pragma once

namespace piftest {
using namespace pifilt;

// ---------------------------------------------------------------------------
// Small math
// ---------------------------------------------------------------------------
struct D3 {
  double x, y, z;
};
inline D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double Dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double Len(D3 a) { return std::sqrt(Dot(a, a)); }
inline D3 Cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline D3 Norm(D3 a) {
  const double l = Len(a);
  return l > 0 ? a * (1.0 / l) : D3{0, 0, 0};
}
struct R {
  uint64_t s;
  explicit R(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
  uint32_t U() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>(s >> 33);
  }
  double F() { return (U() & 0xffffff) / double(0x1000000); }  // [0, 1)
  double S() { return F() * 2 - 1; }                            // [-1, 1)
  D3 InBall() {
    for (;;) {
      D3 p{S(), S(), S()};
      if (Dot(p, p) <= 1) return p;
    }
  }
  D3 OnSphere() { return Norm(InBall() + D3{1e-9, 0, 0}); }
};
// Rotation of v about unit axis a by angle t.
inline D3 Rot(D3 v, D3 a, double t) {
  const double c = std::cos(t), s = std::sin(t);
  return v * c + Cross(a, v) * s + a * (Dot(a, v) * (1 - c));
}

// ---------------------------------------------------------------------------
// 1. Pure parts
// ---------------------------------------------------------------------------
// The SpeedTree wind of the instancer VS, as a model: bend (arc of length h, rotation about the horizontal
// axis perpendicular to the wind, the 100 |b| lerp below |b| = 0.01) then the leaf shake (the horizontal offset
// re-pointed along p.xz + S with its length kept, any S).
D3 Wind(D3 p, double bx, double bz, D3 S, bool shake) {
  double k = std::sqrt(bx * bx + bz * bz);
  D3 bent = p;
  if (k >= 1e-6) {
    const double t = k < 0.01 ? 100 * k : 1.0;
    const double kk = std::max(k, 0.01);
    const D3 dir{bx / k, 0, bz / k};
    const double h = p.y, th = kk * h;
    const D3 arc = dir * ((1 - std::cos(th)) / kk) + D3{0, std::sin(th) / kk, 0};
    const D3 axis = Norm(Cross(D3{0, 1, 0}, dir));  // bends towards dir
    const D3 rot = Rot(p - D3{0, h, 0}, axis, th);
    const D3 full = arc + rot;
    bent = p + (full - p) * t;
  }
  if (!shake) return bent;
  const D3 ph{p.x, 0, p.z};
  const double rho = Len(ph);
  D3 w = ph + S;
  const double lw = Len(w);
  w = lw > 0 ? w * (rho / lw) : D3{0, 0, 0};
  return bent + (w - ph);
}

void PureChecks() {
  // (a) the bound covers every modelled deformation
  {
    R r(11);
    int viol = 0, n = 0;
    double worst = 0, ratioSum = 0;
    for (int it = 0; it < 4000; ++it) {
      // instance transform: rotation about y, optional tilt, uniform scale, rarely sheared / non-uniform
      const double yaw = r.F() * 6.2832, tilt = (it % 5 == 0) ? r.S() * 0.3 : 0.0, sc = 0.3 + 2.5 * r.F();
      D3 X = Rot(Rot(D3{1, 0, 0}, D3{0, 1, 0}, yaw), D3{1, 0, 0}, tilt) * sc;
      D3 Z = Rot(Rot(D3{0, 0, 1}, D3{0, 1, 0}, yaw), D3{1, 0, 0}, tilt) * (sc * ((it % 7 == 0) ? 1.6 : 1.0));
      if (it % 11 == 0) Z = Z + X * 0.3;  // shear
      const D3 Y = Norm(Cross(Z, X)) * Len(X);
      const D3 c{r.S() * 2, 2 + 15 * r.F(), r.S() * 2};
      const double rad = 0.5 + 12 * r.F();
      const bool scaled = it % 9 == 0;
      float rr, rn;
      BoundsCpu(F3{float(X.x), float(X.y), float(X.z)}, F3{float(Z.x), float(Z.y), float(Z.z)},
                F3{float(c.x), float(c.y), float(c.z)}, float(rad), scaled, &rr, &rn);
      for (int s = 0; s < 60; ++s, ++n) {
        // LOD cross-fade between two vertices of the mesh (both in the boundsphere)
        const D3 v2 = c + r.InBall() * rad, v6 = c + r.InBall() * rad;
        const double blend = r.F();
        D3 v = v6 + (v2 - v6) * blend;
        D3 ax = X, ay = Y, az = Z;
        if (scaled) {  // blend < 0: chosen axes scaled by |blend|
          const double f = r.F();
          if (s & 1) ax = ax * f;
          if (s & 2) ay = ay * f;
          if (s & 4) az = az * f;
        }
        const D3 p = ax * v.x + ay * v.y + az * v.z;
        // any wind: tiny, moderate, huge bend vectors; any shake vector
        const double mag = (s % 3 == 0) ? 1e-4 * r.F() : (s % 3 == 1 ? r.F() * 0.2 : r.F() * 50);
        const double ang = r.F() * 6.2832;
        const D3 S = r.InBall() * ((s % 4 == 0) ? 1000.0 : 3.0);
        const D3 f = Wind(p, mag * std::cos(ang), mag * std::sin(ang), S, (s % 5) != 0);
        const double l = Len(f);
        worst = std::max(worst, l / rr);
        if (l > rr * (1 + 1e-6)) ++viol;
      }
      ratioSum += rr / std::max(1e-6, double(rn));
    }
    char what[200];
    sprintf_s(what,
              "periph inst: rigorous bound covers transform + LOD blend + axis scaling + bend + shake, %d samples, %d "
              "outside (worst %.3f of the bound; bound / nominal %.2f on average)",
              n, viol, worst, ratioSum / 4000);
    Check(viol == 0, what);
  }
  // (b) the conservative NDC box holds every projected point of the sphere (w > 0), incl. near the eye plane
  {
    R r(12);
    const double P00 = 0.7807443, P02 = 0.193703, P11 = 0.7906425, P12 = -0.0013454;
    int bad = 0, n = 0;
    for (int it = 0; it < 3000; ++it) {
      const D3 pos{r.S() * 300, r.S() * 100, 0.5 + r.F() * 3000};
      const double Rr = 0.1 + r.F() * 40;
      // VP rows (view = identity)
      const float tc0[4] = {float(P00), 0, float(P02), 0}, tc1[4] = {0, float(P11), float(P12), 0},
                  tc3[4] = {0, 0, 1, 0};
      auto dotr = [](const float* row, D3 p) { return row[0] * p.x + row[1] * p.y + row[2] * p.z + row[3]; };
      const double cx = dotr(tc0, pos), cy = dotr(tc1, pos), cw = dotr(tc3, pos);
      auto lrow = [](const float* row) { return std::sqrt(double(row[0]) * row[0] + row[1] * row[1] + row[2] * row[2]); };
      const double ex = Rr * lrow(tc0), ey = Rr * lrow(tc1), ew = Rr * lrow(tc3);
      const double w0 = cw - ew, w1 = cw + ew;
      if (w0 <= 1e-6) continue;
      const double xs[4] = {(cx - ex) / w0, (cx - ex) / w1, (cx + ex) / w0, (cx + ex) / w1};
      const double ys[4] = {(cy - ey) / w0, (cy - ey) / w1, (cy + ey) / w0, (cy + ey) / w1};
      const double x0 = *std::min_element(xs, xs + 4), x1 = *std::max_element(xs, xs + 4);
      const double y0 = *std::min_element(ys, ys + 4), y1 = *std::max_element(ys, ys + 4);
      for (int s = 0; s < 40; ++s, ++n) {
        const D3 q = pos + r.InBall() * Rr;
        const double w = dotr(tc3, q);
        const double nx = dotr(tc0, q) / w, ny = dotr(tc1, q) / w;
        if (nx < x0 - 1e-9 || nx > x1 + 1e-9 || ny < y0 - 1e-9 || ny > y1 + 1e-9) ++bad;
      }
    }
    char what[160];
    sprintf_s(what, "periph inst: NDC box of the sphere holds all %d projected points (%d outside)", n, bad);
    Check(bad == 0, what);
  }
  // (c) zero coverage: no pixel centre inside any triangle whose vertices lie in the box; brute force
  {
    R r(13);
    const double W = 476, H = 468, eps = 1.0 / 16;
    auto has = [](double a, double b, double n) {
      const double k0 = std::max(std::ceil(a - 0.5), 0.0), k1 = std::min(std::floor(b - 0.5), n - 1);
      return k0 <= k1;
    };
    int zeroBoxes = 0, covered = 0;
    for (int it = 0; it < 20000; ++it) {
      // pixel-space box near the viewport, sub-pixel to a few pixels wide
      const double X0 = -3 + r.F() * (W + 6), Y0 = -3 + r.F() * (H + 6);
      const double X1 = X0 + r.F() * (it % 3 ? 0.9 : 2.5), Y1 = Y0 + r.F() * (it % 3 ? 0.9 : 2.5);
      const bool zero = !has(X0 - eps, X1 + eps, W) || !has(Y0 - eps, Y1 + eps, H);
      if (!zero) continue;
      ++zeroBoxes;
      for (int t = 0; t < 4; ++t) {
        double vx[3], vy[3];
        for (int k = 0; k < 3; ++k) {
          vx[k] = X0 + (X1 - X0) * r.F();
          vy[k] = Y0 + (Y1 - Y0) * r.F();
        }
        // every pixel centre near the box: inside the triangle (edge-inclusive)?
        for (int py = int(std::floor(Y0)) - 1; py <= int(std::ceil(Y1)) + 1; ++py)
          for (int px = int(std::floor(X0)) - 1; px <= int(std::ceil(X1)) + 1; ++px) {
            if (px < 0 || py < 0 || px >= W || py >= H) continue;
            const double cx = px + 0.5, cy = py + 0.5;
            double e[3];
            for (int k = 0; k < 3; ++k) {
              const int k1 = (k + 1) % 3;
              e[k] = (vx[k1] - vx[k]) * (cy - vy[k]) - (vy[k1] - vy[k]) * (cx - vx[k]);
            }
            if ((e[0] >= 0 && e[1] >= 0 && e[2] >= 0) || (e[0] <= 0 && e[1] <= 0 && e[2] <= 0)) ++covered;
          }
      }
    }
    char what[160];
    sprintf_s(what, "periph inst: %d zero-coverage boxes, 4 random triangles each: %d pixel centres covered", zeroBoxes,
              covered);
    Check(zeroBoxes > 1000 && covered == 0, what);
  }
  // (d) the core test: a box accepted is inside the superellipse after the margin, dense sampling
  {
    R r(14);
    const double coreR = 0.6, p = 8, W = 476, H = 468, m = 16;
    const double rect[4] = {-0.3, 0.38, -0.33, 0.35};
    auto inside = [&](double x, double y) {
      const double hw = 0.5 * (rect[1] - rect[0]), hh = 0.5 * (rect[3] - rect[2]);
      const double u = std::fabs(x - 0.5 * (rect[0] + rect[1])) / hw, v = std::fabs(y - 0.5 * (rect[2] + rect[3])) / hh;
      return std::pow(u / coreR, p) + std::pow(v / coreR, p) <= 1.0;
    };
    int acc = 0, bad = 0;
    for (int it = 0; it < 20000; ++it) {
      const double x0 = -0.4 + r.F() * 0.8, y0 = -0.4 + r.F() * 0.8;
      const double x1 = x0 + r.F() * 0.15, y1 = y0 + r.F() * 0.15;
      // mirror of InCore
      const double mx = 2 * m / W, my = 2 * m / H, hw = 0.5 * (rect[1] - rect[0]), hh = 0.5 * (rect[3] - rect[2]);
      const double cx = 0.5 * (rect[0] + rect[1]), cy = 0.5 * (rect[2] + rect[3]);
      const double um = std::max(std::fabs(x0 - mx - cx), std::fabs(x1 + mx - cx)) / hw;
      const double vm = std::max(std::fabs(y0 - my - cy), std::fabs(y1 + my - cy)) / hh;
      const bool in = um < coreR && vm < coreR && std::pow(um / coreR, p) + std::pow(vm / coreR, p) <= 0.99999;
      if (!in) continue;
      ++acc;
      for (int s = 0; s < 64; ++s) {
        const double x = (x0 - mx) + (x1 - x0 + 2 * mx) * r.F(), y = (y0 - my) + (y1 - y0 + 2 * my) * r.F();
        if (!inside(x, y)) ++bad;
      }
    }
    char what[160];
    sprintf_s(what, "periph inst: %d boxes accepted in the core, grown by the margin and sampled: %d samples outside", acc,
              bad);
    Check(acc > 100 && bad == 0, what);
  }
  // (e) defaults: nothing at 0
  Check(g_cfgMask.load() == 0 && g_mask.load() == 0 && !g_installed.load() && !pifilt::g_census.load(),
        "periph inst: keys at 0 by default, nothing installed");
}

// ---------------------------------------------------------------------------
// 2. Synthetic DCS-like scene
// ---------------------------------------------------------------------------
struct Inst64 {
  uint32_t w[16];
};
struct Scene {
  uint32_t nb = 300, nObj = 60, nLod = 0, nEntries = 400, idxCap = 32768, mainCap = 32768, N = 0;
  std::vector<uint32_t> objs, lods;      // 16 / 12 words each
  std::vector<float> sph;                // per bucket: c.xyz, r (the VS stand-in's mesh sphere)
  std::vector<Inst64> main;
  std::vector<uint32_t> counts, offsets, indices, args, sums;
  float tcChain[136 * 4] = {}, tcGb[136 * 4] = {};
  std::vector<uint32_t> bucketObj;       // bucket -> object (for the CPU reference)
};
void SetRow(float* tc, int reg, double a, double b, double c, double d) {
  tc[reg * 4 + 0] = float(a);
  tc[reg * 4 + 1] = float(b);
  tc[reg * 4 + 2] = float(c);
  tc[reg * 4 + 3] = float(d);
}
// Periphery-like camera: census projection, a view rotated by yaw / pitch.
void MakeCamera(Scene& s, double yaw, double pitch) {
  const double P[4][4] = {{0.7807443, 0, 0.193703, 0}, {0, 0.7906425, -0.0013454, 0}, {0, 0, -1.33333e-07, 0.02},
                          {0, 0, 1, 0}};
  // view: world -> camera (rotation only, eye at the origin)
  const D3 fx = Rot(Rot(D3{1, 0, 0}, D3{0, 1, 0}, yaw), D3{1, 0, 0}, 0);
  D3 f = Rot(Rot(D3{0, 0, 1}, D3{1, 0, 0}, -pitch), D3{0, 1, 0}, yaw);
  D3 rgt = Norm(Cross(D3{0, 1, 0}, f));
  D3 up = Cross(f, rgt);
  (void)fx;
  const double V[4][4] = {{rgt.x, rgt.y, rgt.z, 0}, {up.x, up.y, up.z, 0}, {f.x, f.y, f.z, 0}, {0, 0, 0, 1}};
  double VP[4][4];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      VP[i][j] = 0;
      for (int k = 0; k < 4; ++k) VP[i][j] += P[i][k] * V[k][j];
    }
  for (float* tc : {s.tcChain, s.tcGb}) {
    for (int i = 0; i < 4; ++i) SetRow(tc, 16 + i, P[i][0], P[i][1], P[i][2], P[i][3]);
    for (int i = 0; i < 4; ++i) SetRow(tc, 20 + i, VP[i][0], VP[i][1], VP[i][2], VP[i][3]);
  }
  // cb11[44]: orthoCamera 0, distance 0, renderTargetSize (chain: output size, G-buffer: render size)
  s.tcChain[44 * 4 + 2] = 816;
  s.tcChain[44 * 4 + 3] = 804;
  s.tcGb[44 * 4 + 2] = 476;
  s.tcGb[44 * 4 + 3] = 468;
}
D3 CamFwd(const Scene& s) {
  return Norm(D3{s.tcChain[23 * 4 + 0], s.tcChain[23 * 4 + 1], s.tcChain[23 * 4 + 2]});
}

void BuildScene(Scene& s, uint64_t seed, int nInst, bool randomArgWords, bool nearOnes = true) {
  R r(seed);
  MakeCamera(s, 0.35, 0.05);
  // objects: lodCount 1..3, consecutive LOD ranges; 20 unreferenced LODs at the end (unmapped)
  s.objs.assign(size_t(s.nObj) * 16, 0);
  uint32_t lod = 0;
  std::vector<uint32_t> lodObj;
  for (uint32_t o = 0; o < s.nObj; ++o) {
    const uint32_t cnt = 1 + r.U() % 3;
    float* f = reinterpret_cast<float*>(&s.objs[o * 16]);
    const float h = 3 + 20 * float(r.F()), rad = 1 + 12 * float(r.F());
    f[3] = float(r.S());  // boundsphere.x
    f[4] = h * 0.5f;      // .y
    f[5] = float(r.S());  // .z
    f[6] = rad;           // .w
    f[7] = h;             // boundingboxMaxY
    s.objs[o * 16 + 8] = cnt;
    s.objs[o * 16 + 9] = lod;
    f[13] = rad + h * 0.5f;  // objectRadius
    for (uint32_t k = 0; k < cnt; ++k) lodObj.push_back(o);
    lod += cnt;
  }
  s.nLod = lod + 20;
  s.lods.assign(size_t(s.nLod) * 12, 0);
  s.bucketObj.assign(s.nb, 0xffffffffu);
  for (uint32_t l = 0; l < s.nLod; ++l) {
    const uint32_t b = l % s.nb;
    s.lods[l * 12 + 4] = b;                  // iFragment = bucket
    s.lods[l * 12 + 7] = 20 + (l * 37) % 900;  // gPrimCount
  }
  // instances
  const D3 fwd = CamFwd(s);
  const D3 rgt = Norm(Cross(D3{0, 1, 0}, fwd)), up = Cross(fwd, rgt);
  s.main.clear();
  s.sph.assign(size_t(s.nb) * 4, 0);
  for (int i = 0; i < nInst; ++i) {
    const uint32_t l = (i % 50 == 7) ? lod + (r.U() % 20) : r.U() % lod;  // a few unmapped
    const uint32_t b = s.lods[l * 12 + 4];
    const uint32_t o = l < lod ? lodObj[l] : 0;
    // where: far (sub-pixel), beyond the side edges, centre (core), near, behind
    double dist, ax, ay;
    const int kind = nearOnes ? i % 10 : i % 8;  // without instances at or behind the eye: no giant occluders
    if (kind < 4) {
      dist = 2000 + 20000 * r.F();
      ax = r.S() * 1.6;
      ay = -0.05 + r.S() * 0.3;
    } else if (kind < 6) {
      dist = 30 + 1500 * r.F();
      ax = (r.U() & 1 ? 1 : -1) * (1.3 + 0.8 * r.F());
      ay = r.S() * 1.2;
    } else if (kind < 8) {
      dist = 50 + 4000 * r.F();
      ax = 0.25 * r.S() + 0.05;
      ay = 0.25 * r.S();
    } else if (kind < 9) {
      dist = 0.5 + 40 * r.F();
      ax = r.S();
      ay = r.S() * 0.8;
    } else {
      dist = -(5 + 100 * r.F());
      ax = r.S();
      ay = r.S();
    }
    const D3 pos = fwd * dist + rgt * (ax * std::fabs(dist)) + up * (ay * std::fabs(dist)) + D3{0, -2, 0};
    const double yaw = r.F() * 6.2832, sc = 0.6 + 1.2 * r.F();
    const D3 X = Rot(D3{1, 0, 0}, D3{0, 1, 0}, yaw) * sc, Z = Rot(D3{0, 0, 1}, D3{0, 1, 0}, yaw) * sc;
    Inst64 in{};
    in.w[0] = b | 0xff000000u;
    in.w[1] = l | 0xff000000u;
    float* f = reinterpret_cast<float*>(in.w);
    f[2] = float(pos.x);
    f[3] = float(pos.y);
    f[4] = float(pos.z);
    f[5] = float(X.x);
    f[6] = float(X.y);
    f[7] = float(X.z);
    f[8] = float(Z.x);
    f[9] = float(Z.y);
    f[10] = float(Z.z);
    in.w[11] = 0xffffffffu;
    f[12] = (i % 13 == 0) ? -float(r.F()) : 1.0f;  // a few with the axis-scaling flag
    f[13] = float(r.F());
    s.main.push_back(in);
    if (l < lod) {
      // the stand-in VS's sphere per bucket = the object's boundsphere (buckets map to one object here)
      const float* of = reinterpret_cast<const float*>(&s.objs[o * 16]);
      s.sph[b * 4 + 0] = of[3];
      s.sph[b * 4 + 1] = of[4];
      s.sph[b * 4 + 2] = of[5];
      s.sph[b * 4 + 3] = of[6];
      s.bucketObj[b] = o;
    }
  }
  s.N = uint32_t(s.main.size());
  // the chain's outputs (as calccounters / calcoffsets / sort / fillindirect): counts, offsets, a random rank
  // order inside each bucket
  s.counts.assign(s.nb, 0);
  for (auto& in : s.main) ++s.counts[in.w[0] & 0xffffff];
  s.offsets.assign(s.nb, 0);
  for (uint32_t b = 1; b < s.nb; ++b) s.offsets[b] = s.offsets[b - 1] + s.counts[b - 1];
  s.indices.assign(s.idxCap, 0xdeadbeefu);  // stale content beyond the total
  std::vector<std::vector<uint32_t>> per(s.nb);
  for (uint32_t i = 0; i < s.N; ++i) per[s.main[i].w[0] & 0xffffff].push_back(i);
  for (uint32_t b = 0; b < s.nb; ++b) {
    auto& v = per[b];
    for (size_t k = v.size(); k > 1; --k) std::swap(v[k - 1], v[r.U() % k]);
    for (size_t k = 0; k < v.size(); ++k) s.indices[s.offsets[b] + k] = v[k];
  }
  s.args.assign(size_t(s.nEntries) * 8, 0);
  for (uint32_t e = 0; e < s.nEntries; ++e) {
    const uint32_t b = e % (s.nb + 25);  // some entries with a bucket >= nb (left alone)
    uint32_t* a = &s.args[e * 8];
    a[0] = 60;  // the stand-in mesh: 20 triangles
    a[1] = b < s.nb ? s.counts[b] : 0;
    a[2] = randomArgWords ? r.U() : 0;
    a[3] = randomArgWords ? r.U() : 0;
    a[4] = 0;
    a[5] = randomArgWords ? r.U() : 0;
    a[6] = randomArgWords ? r.U() : 0;
    a[7] = b;
  }
}

// CPU reference of the classification (double precision); 1 drop zero, 2 drop core, 0 keep, -1 borderline.
int RefClass(const Scene& s, uint32_t i, uint32_t mode, double W, double H, const double* rect) {
  const Inst64& in = s.main[i];
  const uint32_t l = in.w[1] & 0xffffff;
  if (l >= s.nLod) return 0;
  // lod -> object
  uint32_t o = 0xffffffffu;
  for (uint32_t k = 0; k < s.nObj; ++k) {
    const uint32_t cnt = s.objs[k * 16 + 8], st = s.objs[k * 16 + 9];
    if (l >= st && l < st + cnt) o = k;
  }
  if (o == 0xffffffffu) return 0;
  const float* f = reinterpret_cast<const float*>(in.w);
  const float* of = reinterpret_cast<const float*>(&s.objs[o * 16]);
  int res[2];
  for (int pass = 0; pass < 2; ++pass) {  // two perturbations: borderline when they disagree
    const double tweak = pass ? 1.002 : 0.998, eps = (pass ? 1.2 : 0.8) / 16.0;
    float rr, rn;
    BoundsCpu(F3{f[5], f[6], f[7]}, F3{f[8], f[9], f[10]}, F3{of[3], of[4], of[5]}, of[6], f[12] < 0, &rr, &rn);
    const D3 p{f[2], f[3], f[4]};
    const double Rr = (rr + 4e-6 * Len(p) + 1e-3) * tweak;
    auto row = [&](int reg) { return &s.tcChain[reg * 4]; };
    auto dr = [&](const float* q) { return q[0] * p.x + q[1] * p.y + q[2] * p.z + q[3]; };
    auto lr = [&](const float* q) { return std::sqrt(double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2]); };
    const double cx = dr(row(20)), cy = dr(row(21)), cw = dr(row(23));
    const double ex = Rr * lr(row(20)), ey = Rr * lr(row(21)), ew = Rr * lr(row(23));
    const double w0 = cw - ew, w1 = cw + ew;
    if (w0 <= 1e-6) {
      res[pass] = 0;
      continue;
    }
    const double xs[4] = {(cx - ex) / w0, (cx - ex) / w1, (cx + ex) / w0, (cx + ex) / w1};
    const double ys[4] = {(cy - ey) / w0, (cy - ey) / w1, (cy + ey) / w0, (cy + ey) / w1};
    const double x0 = *std::min_element(xs, xs + 4), x1 = *std::max_element(xs, xs + 4);
    const double y0 = *std::min_element(ys, ys + 4), y1 = *std::max_element(ys, ys + 4);
    auto has = [](double a, double b, double n) {
      a = std::clamp(a, -1e7, 1e7);
      b = std::clamp(b, -1e7, 1e7);
      return std::max(std::ceil(a - 0.5), 0.0) <= std::min(std::floor(b - 0.5), n - 1);
    };
    const double X0 = (x0 * 0.5 + 0.5) * W, X1 = (x1 * 0.5 + 0.5) * W, Y0 = (0.5 - y1 * 0.5) * H, Y1 = (0.5 - y0 * 0.5) * H;
    const bool zero = !has(X0 - eps, X1 + eps, W) || !has(Y0 - eps, Y1 + eps, H);
    bool core = false;
    if (!zero && rect) {
      const double m = 16 * (pass ? 1.01 : 0.99), mx = 2 * m / W, my = 2 * m / H;
      const double hw = 0.5 * (rect[1] - rect[0]), hh = 0.5 * (rect[3] - rect[2]);
      const double ccx = 0.5 * (rect[0] + rect[1]), ccy = 0.5 * (rect[2] + rect[3]);
      const double um = std::max(std::fabs(x0 - mx - ccx), std::fabs(x1 + mx - ccx)) / hw;
      const double vm = std::max(std::fabs(y0 - my - ccy), std::fabs(y1 + my - ccy)) / hh;
      core = um < 0.6 && vm < 0.6 && std::pow(um / 0.6, 8) + std::pow(vm / 0.6, 8) <= 0.99999;
    }
    res[pass] = ((mode & kMZero) && zero) ? 1 : (((mode & kMCore) && core) ? 2 : 0);
  }
  return res[0] == res[1] ? res[0] : -1;
}

// "DCS's" buffers on a device, with DCS's descriptions (census gdd_res: SRV|UAV, structured 64 / typed,
// indirect-args misc on the arguments, objects / LODs SRV only).
struct DcsBufs {
  ID3D11Buffer *mainB = nullptr, *idx = nullptr, *offs = nullptr, *cnts = nullptr, *args = nullptr, *objs = nullptr,
               *lods = nullptr, *tc = nullptr, *sph = nullptr;
  ID3D11ShaderResourceView *sMain = nullptr, *sIdx = nullptr, *sOffs = nullptr, *sCnts = nullptr, *sObjs = nullptr,
                           *sLods = nullptr, *sSph = nullptr;
  void Release() {
    for (ID3D11Buffer** b : {&mainB, &idx, &offs, &cnts, &args, &objs, &lods, &tc, &sph}) Rel(*b);
    for (ID3D11ShaderResourceView** v : {&sMain, &sIdx, &sOffs, &sCnts, &sObjs, &sLods, &sSph}) Rel(*v);
  }
};
ID3D11Buffer* MkBuf(ID3D11Device* d, UINT bytes, UINT bind, UINT misc, UINT stride, const void* init) {
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = bytes;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = bind;
  bd.MiscFlags = misc;
  bd.StructureByteStride = stride;
  D3D11_SUBRESOURCE_DATA sd{init, 0, 0};
  ID3D11Buffer* b = nullptr;
  d->CreateBuffer(&bd, init ? &sd : nullptr, &b);
  return b;
}
ID3D11ShaderResourceView* MkSrv(ID3D11Device* d, ID3D11Buffer* b, UINT elems, bool structured) {
  D3D11_SHADER_RESOURCE_VIEW_DESC v{};
  v.Format = structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_UINT;
  v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  v.Buffer.NumElements = elems;
  ID3D11ShaderResourceView* s = nullptr;
  d->CreateShaderResourceView(b, &v, &s);
  return s;
}
bool MakeDcs(ID3D11Device* d, const Scene& s, DcsBufs& o) {
  const UINT sr = D3D11_BIND_SHADER_RESOURCE, ua = D3D11_BIND_UNORDERED_ACCESS;
  std::vector<Inst64> main(s.mainCap);
  std::copy(s.main.begin(), s.main.end(), main.begin());
  o.mainB = MkBuf(d, s.mainCap * 64, sr | ua, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 64, main.data());
  o.idx = MkBuf(d, s.idxCap * 4, sr | ua, 0, 0, s.indices.data());
  o.offs = MkBuf(d, s.nb * 4, sr | ua, 0, 0, s.offsets.data());
  o.cnts = MkBuf(d, s.nb * 4, sr | ua, 0, 0, s.counts.data());
  o.args = MkBuf(d, s.nEntries * 32, sr | ua, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0, s.args.data());
  o.objs = MkBuf(d, s.nObj * 64, sr, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 64, s.objs.data());
  o.lods = MkBuf(d, s.nLod * 48, sr, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 48, s.lods.data());
  o.sph = MkBuf(d, s.nb * 16, sr, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16, s.sph.data());
  D3D11_BUFFER_DESC cd{};
  cd.ByteWidth = 136 * 16;
  cd.Usage = D3D11_USAGE_DEFAULT;
  cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  D3D11_SUBRESOURCE_DATA sd{s.tcChain, 0, 0};
  d->CreateBuffer(&cd, &sd, &o.tc);
  if (!o.mainB || !o.idx || !o.offs || !o.cnts || !o.args || !o.objs || !o.lods || !o.tc || !o.sph) return false;
  o.sMain = MkSrv(d, o.mainB, s.mainCap, true);
  o.sIdx = MkSrv(d, o.idx, s.idxCap, false);
  o.sOffs = MkSrv(d, o.offs, s.nb, false);
  o.sCnts = MkSrv(d, o.cnts, s.nb, false);
  o.sObjs = MkSrv(d, o.objs, s.nObj, true);
  o.sLods = MkSrv(d, o.lods, s.nLod, true);
  o.sSph = MkSrv(d, o.sph, s.nb, true);
  return o.sMain && o.sIdx && o.sOffs && o.sCnts && o.sObjs && o.sLods && o.sSph;
}
// The module's view of "DCS" (as the identification would set it).
void Adopt(const Scene& s, DcsBufs& o) {
  ReleaseDcs();
  auto ad = [](ID3D11Buffer* b) {
    b->AddRef();
    return b;
  };
  g_d.args = ad(o.args);
  g_d.mainB = ad(o.mainB);
  g_d.idx = ad(o.idx);
  g_d.offs = ad(o.offs);
  g_d.cnts = ad(o.cnts);
  g_d.tc = ad(o.tc);
  g_d.objs = ad(o.objs);
  g_d.lods = ad(o.lods);
  o.sIdx->GetDesc(&g_d.idxDesc);
  g_d.nArgs = s.nEntries;
  g_d.mainCap = s.mainCap;
  g_d.idxCap = s.idxCap;
  g_d.nb = s.nb;
  g_d.nObj = s.nObj;
  g_d.nLod = s.nLod;
  g_d.tcBytes = 136 * 16;
  g_drawLearned = true;
  g_dispatchLearned = true;
}
std::vector<uint32_t> Read(ID3D11Device* d, ID3D11DeviceContext* c, ID3D11Buffer* b) {
  std::vector<uint32_t> v;
  chainfast::ReadBuf(d, c, b, BufBytes(b), v);
  return v;
}
void ResetModule() {
  std::lock_guard<std::mutex> lock(g_mu);
  ReleaseOurs();
  ReleaseDcs();
  g_resReady = false;
  g_resFail = 0;
  g_drawLearned = false;
  g_dispatchLearned = false;
  g_rt = Rt{};
  g_latched = 0;
}
void SetRenderSize(int eye, float W, float H) {
  g_rt.W[eye] = W;
  g_rt.H[eye] = H;
  g_rt.zwKnown[eye] = true;
  g_rt.zw[eye][0] = 816;
  g_rt.zw[eye][1] = 804;
  g_rt.samples[eye] = 1;
  g_rt.focusW = 1176;
}

// Structure check: per bucket a stable partition of the stock list by the GPU's keep flags, counts in the
// arguments = kept, every other word unchanged.
struct Struct {
  bool ok = true;
  uint32_t dropped = 0, kept = 0;
  std::string first;
};
Struct CheckStructure(const Scene& s, const std::vector<uint32_t>& newIdx, const std::vector<uint32_t>& newArgs,
                      const std::vector<uint32_t>& flags) {
  Struct r;
  auto fail = [&](const char* m) {
    if (r.ok) r.first = m;
    r.ok = false;
  };
  std::vector<uint32_t> kept(s.nb, 0);
  for (uint32_t b = 0; b < s.nb; ++b) {
    const uint32_t o = s.offsets[b], n = s.counts[b];
    std::vector<uint32_t> keepL, dropL;
    for (uint32_t k = 0; k < n; ++k) (flags[o + k] ? keepL : dropL).push_back(s.indices[o + k]);
    kept[b] = uint32_t(keepL.size());
    r.kept += kept[b];
    r.dropped += uint32_t(dropL.size());
    for (uint32_t k = 0; k < n; ++k) {
      const uint32_t want = k < keepL.size() ? keepL[k] : dropL[k - keepL.size()];
      if (newIdx[o + k] != want) {
        fail("index list is not the stable partition");
        break;
      }
    }
  }
  const uint32_t total = s.offsets[s.nb - 1] + s.counts[s.nb - 1];
  for (uint32_t j = total; j < s.idxCap; ++j)
    if (newIdx[j] != s.indices[j]) {
      fail("words beyond the total changed");
      break;
    }
  for (uint32_t e = 0; e < s.nEntries; ++e)
    for (int w = 0; w < 8; ++w) {
      const uint32_t b = s.args[e * 8 + 7];
      const uint32_t want = (w == 1 && b < s.nb) ? kept[b] : s.args[e * 8 + w];
      if (newArgs[e * 8 + w] != want) {
        fail("argument words");
        e = s.nEntries;
        break;
      }
    }
  return r;
}

// One filtered run on device d (the module's PeriphBegin / RestoreStock / checks); returns false on a setup error.
struct RunOut {
  std::vector<uint32_t> idx, args, flags, stats, idxAfter, argsAfter;
};
bool OneRun(ID3D11Device* d, ID3D11DeviceContext* c, const Scene& s, DcsBufs& o, uint32_t mask, bool census,
            RunOut& out, const float* gazeTan = nullptr) {
  g_mask = mask;
  pifilt::g_census = census;
  (void)gazeTan;
  PeriphBegin(0);
  if (!g_resReady.load()) return false;
  out.idx = Read(d, c, o.idx);
  out.args = Read(d, c, o.args);
  out.flags = Read(d, c, g_o.flags.b);
  // the G-buffer's own TerrainContext upload, then the run end
  c->UpdateSubresource(o.tc, 0, nullptr, s.tcGb, 0, 0);
  g_rt.inRun = true;
  g_rt.runView = 0;
  EndRun();
  out.idxAfter = Read(d, c, o.idx);
  out.argsAfter = Read(d, c, o.args);
  out.stats = Read(d, c, g_o.stats[0].b);
  c->UpdateSubresource(o.tc, 0, nullptr, s.tcChain, 0, 0);
  g_mask = 0;
  pifilt::g_census = false;
  return true;
}

void GpuChecks(ID3D11Device* d, ID3D11DeviceContext* c, const char* tag) {
  char what[300];
  Scene s;
  BuildScene(s, 21, 20000, true);
  DcsBufs o;
  Check(MakeDcs(d, s, o), "periph inst: synthetic DCS buffers");
  ResetModule();
  SetDeviceForTest(d);
  Check(CompileAll(), (std::string("periph inst: kernels compile (") + g_codeErr + ")").c_str());
  Adopt(s, o);
  SetRenderSize(0, 476, 468);
  RunOut r;
  const bool ran = OneRun(d, c, s, o, kBitZero, true, r);
  sprintf_s(what, "periph inst %s: buffers / kernels created and a run committed", tag);
  Check(ran && g_rt.dirty == false && g_commits.load() > 0, what);
  if (!ran) {
    o.Release();
    ResetModule();
    return;
  }
  // classification vs the CPU reference
  {
    int agree = 0, border = 0, disagree = 0, refDrop = 0;
    const uint32_t total = s.offsets[s.nb - 1] + s.counts[s.nb - 1];
    for (uint32_t j = 0; j < total; ++j) {
      const uint32_t i = s.indices[j];
      const int ref = RefClass(s, i, kMZero, 476, 468, nullptr);
      if (ref < 0) {
        ++border;
        continue;
      }
      const bool gpuDrop = r.flags[j] == 0;
      if (gpuDrop == (ref != 0)) ++agree;
      else ++disagree;
      if (ref) ++refDrop;
    }
    sprintf_s(what, "periph inst %s: zero-coverage classification = CPU reference for %d instances (%d dropped), "
                    "%d borderline skipped, %d disagree",
              tag, agree, refDrop, border, disagree);
    Check(disagree == 0 && refDrop > 1000 && agree > 15000, what);
  }
  {
    const Struct st = CheckStructure(s, r.idx, r.args, r.flags);
    sprintf_s(what, "periph inst %s: stable partition per bucket, InstanceCount = kept, other words unchanged (%u kept, "
                    "%u dropped)%s%s",
              tag, st.kept, st.dropped, st.first.empty() ? "" : ": ", st.first.c_str());
    Check(st.ok && st.dropped > 0, what);
  }
  Check(r.idxAfter == s.indices && r.argsAfter == s.args, "periph inst: run end copies DCS's stock lists back exactly");
  {
    uint64_t tri = 0;
    for (uint32_t i = 0; i < s.N; ++i) {
      const uint32_t l = s.main[i].w[1] & 0xffffff;
      if (l < s.nLod) tri += s.lods[l * 12 + 7];
    }
    const uint64_t gTri = uint64_t(r.stats[kSTri]) | (uint64_t(r.stats[kSTri + 1]) << 32);
    sprintf_s(what, "periph inst %s: census totals: %u instances / %llu triangles (CPU %u / %llu), error bits 0x%x, "
                    "after-run check ran %u, kept %u",
              tag, r.stats[kSInst], (unsigned long long)gTri, s.N, (unsigned long long)tri, r.stats[kSErr],
              r.stats[kSCheckRun], r.stats[kSKept]);
    Check(r.stats[kSInst] == s.N && gTri == tri && r.stats[kSErr] == 0 && r.stats[kSCheckRun] == 1, what);
  }
  // census only: nothing written into DCS's buffers
  {
    RunOut q;
    OneRun(d, c, s, o, 0, true, q);
    Check(q.idx == s.indices && q.args == s.args && q.stats[kSInst] == s.N && q.stats[kSZeroI] == r.stats[kSZeroI],
          "periph inst: census only reads (DCS's lists untouched) and counts the same zero-coverage set");
  }
  // inconsistent lists: the commit is a stock copy
  {
    std::vector<uint32_t> badArgs = s.args;
    badArgs[5 * 8 + 1] += 1;  // an InstanceCount that is not counts[bucket]
    c->UpdateSubresource(o.args, 0, nullptr, badArgs.data(), 0, 0);
    RunOut q;
    OneRun(d, c, s, o, kBitZero, false, q);
    Check(q.idx == s.indices && q.args == badArgs && (q.stats[kSErr] & kErrArgs) != 0,
          "periph inst: arguments inconsistent with counts: error 0x10, lists committed unchanged");
    c->UpdateSubresource(o.args, 0, nullptr, s.args.data(), 0, 0);
    std::vector<uint32_t> badIdx = s.indices;
    uint32_t b1 = 0, b2 = 0;
    while (s.counts[b1] == 0) ++b1;
    b2 = b1 + 1;
    while (s.counts[b2] == 0) ++b2;
    std::swap(badIdx[s.offsets[b1]], badIdx[s.offsets[b2]]);  // two slots in the wrong bucket
    c->UpdateSubresource(o.idx, 0, nullptr, badIdx.data(), 0, 0);
    OneRun(d, c, s, o, kBitZero, false, q);
    Check(q.idx == badIdx && q.args == s.args && (q.stats[kSErr] & kErrSlot) != 0,
          "periph inst: a slot outside its bucket: error 0x8, lists committed unchanged");
    c->UpdateSubresource(o.idx, 0, nullptr, s.indices.data(), 0, 0);
  }
  // guards: a multisampled G-buffer (zero coverage off), a view order where the "focus" is not wider (core
  // off), an unknown render size (probe only: the after-run check learns it, nothing classified or committed)
  {
    RunOut q;
    g_rt.samples[0] = 4;
    OneRun(d, c, s, o, kBitZero, false, q);
    const bool msaaOff = q.idx == s.indices && q.args == s.args && !g_rt.committed;
    g_rt.samples[0] = 1;
    const float fw = g_rt.focusW;
    g_rt.focusW = 400;
    OneRun(d, c, s, o, kBitCore, false, q);
    const bool orderOff = q.idx == s.indices && !g_rt.committed;
    g_rt.focusW = fw;
    g_rt.W[0] = 0;
    OneRun(d, c, s, o, kBitZero, true, q);
    float gw = 0, gh = 0;
    memcpy(&gw, &q.stats[kSGbW], 4);
    memcpy(&gh, &q.stats[kSGbH], 4);
    const bool probe = q.idx == s.indices && q.stats[kSInst] == 0 && q.stats[kSCheckRun] == 1 && gw == 476 && gh == 468;
    SetRenderSize(0, 476, 468);
    sprintf_s(what, "periph inst: guards: MSAA target -> no zero-coverage commit %d; focus not wider -> no core commit "
                    "%d; unknown render size -> probe run only, the check reads %.0fx%.0f from the G-buffer's "
                    "TerrainContext %d",
              msaaOff ? 1 : 0, orderOff ? 1 : 0, gw, gh, probe ? 1 : 0);
    Check(msaaOff && orderOff && probe, what);
  }
  // after-run check: a G-buffer camera other than the chain's is caught (latches)
  {
    Scene s2 = s;
    s2.tcGb[20 * 4 + 0] += 1e-3f;
    RunOut q;
    OneRun(d, c, s2, o, kBitZero, false, q);
    Check((q.stats[kSErr] & kErrVp) != 0, "periph inst: G-buffer VP differing from the classified one: error 0x100");
    Scene s3 = s;
    s3.tcGb[44 * 4 + 2] = 480;
    OneRun(d, c, s3, o, kBitZero, false, q);
    Check((q.stats[kSErr] & kErrDims) != 0, "periph inst: render size differing from the viewport: error 0x200");
    // the view guard: a chain camera of another output size refuses the commit
    Scene s4 = s;
    s4.tcChain[44 * 4 + 2] = 1176;
    c->UpdateSubresource(o.tc, 0, nullptr, s4.tcChain, 0, 0);
    OneRun(d, c, s4, o, kBitZero, false, q);
    Check(q.idx == s.indices && (q.stats[kSErr] & kErrView) != 0,
          "periph inst: chain camera size not the learned one: error 0x20, nothing committed");
    c->UpdateSubresource(o.tc, 0, nullptr, s.tcChain, 0, 0);
  }
  // core hole: gaze tangents (the focus of the census: 0.34 of the periphery's NDC around the centre)
  {
    // focus FOV tangents so that its rect in the periphery's NDC is [-0.3, 0.38] x [-0.33, 0.35]
    const double rect[4] = {-0.3, 0.38, -0.33, 0.35};
    const float* P0 = &s.tcChain[16 * 4];
    const float* P1 = &s.tcChain[17 * 4];
    g_override = -1;
    // GazeTangents reads shared memory; here the rect goes in through a test hook of the params
    Params p = BaseParams(0, kMZero | kMCore | kMCensus | kMCommit);
    p.gazeOk = 1;
    p.gazeTan[0] = float((rect[0] - P0[2]) / P0[0]);
    p.gazeTan[1] = float((rect[1] - P0[2]) / P0[0]);
    p.gazeTan[2] = float((rect[3] - P1[2]) / P1[1]);
    p.gazeTan[3] = float((rect[2] - P1[2]) / P1[1]);
    g_testGaze = true;
    memcpy(g_testGazeTan, p.gazeTan, sizeof(g_testGazeTan));
    RunOut q;
    OneRun(d, c, s, o, kBitZero | kBitCore, true, q);
    g_testGaze = false;
    int agree = 0, border = 0, disagree = 0, core = 0;
    const uint32_t total = s.offsets[s.nb - 1] + s.counts[s.nb - 1];
    for (uint32_t j = 0; j < total; ++j) {
      const int ref = RefClass(s, s.indices[j], kMZero | kMCore, 476, 468, rect);
      if (ref < 0) {
        ++border;
        continue;
      }
      ((q.flags[j] == 0) == (ref != 0) ? agree : disagree)++;
      if (ref == 2) ++core;
    }
    const Struct st = CheckStructure(s, q.idx, q.args, q.flags);
    sprintf_s(what, "periph inst %s: zero + core classification = CPU reference (%d agree, %d in the core, %d "
                    "borderline, %d disagree), partition valid, stock back after",
              tag, agree, core, border, disagree);
    Check(disagree == 0 && core > 50 && st.ok && q.idxAfter == s.indices && q.argsAfter == s.args, what);
  }
  o.Release();
  ResetModule();
}

// ---------------------------------------------------------------------------
// 3. Raster exactness and the verify path
// ---------------------------------------------------------------------------
const char kVsHlsl[] = R"(
cbuffer G : register(b0) { uint offsetsBufferIndex; uint3 pad; };
cbuffer T : register(b11) { float4 tc[136]; };
struct Inst { uint4 a, b, c, d; };
StructuredBuffer<Inst> mainB : register(t0);
Buffer<uint> indices : register(t1);
Buffer<uint> offsets : register(t2);
Buffer<uint> counts : register(t3);
StructuredBuffer<float4> sph : register(t4);
struct O { float4 pos : SV_Position; nointerpolation uint id : TEXCOORD0; };
static const float3 kV[12] = {
  float3(0, 0.5257311, 0.8506508), float3(0, 0.5257311, -0.8506508), float3(0, -0.5257311, 0.8506508),
  float3(0, -0.5257311, -0.8506508), float3(0.8506508, 0, 0.5257311), float3(0.8506508, 0, -0.5257311),
  float3(-0.8506508, 0, 0.5257311), float3(-0.8506508, 0, -0.5257311), float3(0.5257311, 0.8506508, 0),
  float3(-0.5257311, 0.8506508, 0), float3(0.5257311, -0.8506508, 0), float3(-0.5257311, -0.8506508, 0)};
cbuffer S : register(b1) { float meshScale; float3 pad2; };
O main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  // DCS's instancer indexing (every instancer VS): offsets of the draw's bucket + SV_InstanceID
  uint i = indices[offsets[offsetsBufferIndex] + iid];
  Inst r = mainB[i];
  float3 pos = asfloat(uint3(r.a.z, r.a.w, r.b.x));
  float3 X = asfloat(r.b.yzw), Z = asfloat(r.c.xyz);
  float3 Y = normalize(cross(Z, X)) * length(X);
  uint b = r.a.x & 0xffffffu;
  float4 s = sph[b];
  float3 v = s.xyz + kV[vid] * s.w * meshScale;  // an icosahedron on the boundsphere (worst case of the bound)
  float4 w = float4(X * v.x + Y * v.y + Z * v.z + pos, 1);
  if (iid >= counts[offsetsBufferIndex]) w = float4(0, 0, 0, 0);  // DCS: an id past the count collapses
  O o;
  o.pos = float4(dot(tc[20], w), dot(tc[21], w), dot(tc[22], w), dot(tc[23], w));
  o.id = i;
  return o;
}
)";
const char kPsHlsl[] = R"(
uint4 main(float4 p : SV_Position, nointerpolation uint id : TEXCOORD0, uint prim : SV_PrimitiveID) : SV_Target {
  return uint4(id, prim, asuint(p.z), 1);
}
)";
const uint16_t kIco[60] = {0, 4, 8, 0, 8, 9, 0, 9, 6, 0, 6, 2, 0, 2, 4, 3, 5, 10, 3, 10, 11, 3, 11, 7, 3, 7, 1, 3, 1, 5,
                           4, 5, 8, 4, 10, 5, 4, 2, 10, 8, 5, 1, 8, 1, 9, 9, 1, 7, 9, 7, 6, 6, 7, 11, 6, 11, 2, 2, 11, 10};

struct Raster {
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11Buffer *cbG = nullptr, *cbS = nullptr, *ib = nullptr;
  ID3D11Texture2D *rt = nullptr, *ds = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11DepthStencilState* dss = nullptr;
  ID3D11RasterizerState* rs = nullptr;
  void Release() {
    Rel(vs);
    Rel(ps);
    Rel(cbG);
    Rel(cbS);
    Rel(ib);
    Rel(rt);
    Rel(ds);
    Rel(rtv);
    Rel(dsv);
    Rel(dss);
    Rel(rs);
  }
};
bool CompileStage(const char* src, const char* prof, std::vector<uint8_t>& out) {
  if (!chainfast::LoadCompiler()) return false;
  ID3DBlob *code = nullptr, *err = nullptr;
  const HRESULT hr = chainfast::g_compile(src, strlen(src), "t", nullptr, nullptr, "main", prof, 0, 0, &code, &err);
  if (err) {
    if (FAILED(hr)) printf("     %s\n", static_cast<const char*>(err->GetBufferPointer()));
    err->Release();
  }
  if (FAILED(hr) || !code) return false;
  const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
  out.assign(p, p + code->GetBufferSize());
  code->Release();
  return true;
}
bool MakeRaster(ID3D11Device* d, Raster& r) {
  std::vector<uint8_t> v, p;
  if (!CompileStage(kVsHlsl, "vs_5_0", v) || !CompileStage(kPsHlsl, "ps_5_0", p)) return false;
  if (FAILED(d->CreateVertexShader(v.data(), v.size(), nullptr, &r.vs)) ||
      FAILED(d->CreatePixelShader(p.data(), p.size(), nullptr, &r.ps)))
    return false;
  D3D11_BUFFER_DESC cd{};
  cd.ByteWidth = 16;
  cd.Usage = D3D11_USAGE_DEFAULT;
  cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  d->CreateBuffer(&cd, nullptr, &r.cbG);
  d->CreateBuffer(&cd, nullptr, &r.cbS);
  r.ib = MkBuf(d, sizeof(kIco), D3D11_BIND_INDEX_BUFFER, 0, 0, kIco);
  D3D11_TEXTURE2D_DESC td{};
  td.Width = 476;
  td.Height = 468;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R32G32B32A32_UINT;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  d->CreateTexture2D(&td, nullptr, &r.rt);
  td.Format = DXGI_FORMAT_R32G8X24_TYPELESS;  // DCS's G-buffer depth (census format 19)
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  d->CreateTexture2D(&td, nullptr, &r.ds);
  if (!r.rt || !r.ds) return false;
  d->CreateRenderTargetView(r.rt, nullptr, &r.rtv);
  D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
  dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
  d->CreateDepthStencilView(r.ds, &dv, &r.dsv);
  D3D11_DEPTH_STENCIL_DESC dd{};
  dd.DepthEnable = TRUE;
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;  // reverse-Z, as DCS
  d->CreateDepthStencilState(&dd, &r.dss);
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  d->CreateRasterizerState(&rd, &r.rs);
  return r.rtv && r.dsv && r.dss && r.rs && r.cbG && r.cbS && r.ib;
}
// Draws every argument entry of a valid bucket (the G-buffer's instancer draws), through the context table
// (so an armed hook sees them). Reads back colour and depth.
void DrawAll(ID3D11Device* d, ID3D11DeviceContext* c, const Scene& s, DcsBufs& o, Raster& r, float meshScale,
             std::vector<uint32_t>& col, std::vector<uint32_t>& dep) {
  const float clr[4] = {};
  c->ClearRenderTargetView(r.rtv, clr);
  c->ClearDepthStencilView(r.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
  c->OMSetRenderTargets(1, &r.rtv, r.dsv);
  c->OMSetDepthStencilState(r.dss, 0);
  c->RSSetState(r.rs);
  D3D11_VIEWPORT vp{0, 0, 476, 468, 0, 1};
  c->RSSetViewports(1, &vp);
  c->IASetIndexBuffer(r.ib, DXGI_FORMAT_R16_UINT, 0);
  c->IASetInputLayout(nullptr);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c->VSSetShader(r.vs, nullptr, 0);
  c->PSSetShader(r.ps, nullptr, 0);
  ID3D11ShaderResourceView* srv[5] = {o.sMain, o.sIdx, o.sOffs, o.sCnts, o.sSph};
  c->VSSetShaderResources(0, 5, srv);
  ID3D11Buffer* cb[2] = {r.cbG, r.cbS};
  c->VSSetConstantBuffers(0, 2, cb);
  c->VSSetConstantBuffers(11, 1, &o.tc);
  const float ms[4] = {meshScale, 0, 0, 0};
  c->UpdateSubresource(r.cbS, 0, nullptr, ms, 0, 0);
  for (uint32_t e = 0; e < s.nEntries; ++e) {
    const uint32_t b = s.args[e * 8 + 7];
    if (b >= s.nb) continue;
    const uint32_t g[4] = {b, 0, 0, 0};
    c->UpdateSubresource(r.cbG, 0, nullptr, g, 0, 0);
    c->DrawIndexedInstancedIndirect(o.args, e * 32);
  }
  ID3D11ShaderResourceView* ns[5] = {};
  c->VSSetShaderResources(0, 5, ns);
  ID3D11RenderTargetView* nr = nullptr;
  c->OMSetRenderTargets(1, &nr, nullptr);
  // readback
  auto readTex = [&](ID3D11Texture2D* t, UINT bpp, std::vector<uint32_t>& out) {
    D3D11_TEXTURE2D_DESC td{};
    t->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    d->CreateTexture2D(&td, nullptr, &st);
    c->CopyResource(st, t);
    D3D11_MAPPED_SUBRESOURCE m{};
    out.assign(size_t(td.Width) * td.Height * bpp / 4, 0);
    if (SUCCEEDED(c->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
      for (UINT y = 0; y < td.Height; ++y)
        memcpy(&out[size_t(y) * td.Width * bpp / 4], static_cast<uint8_t*>(m.pData) + y * m.RowPitch, td.Width * bpp);
      c->Unmap(st, 0);
    }
    st->Release();
  };
  readTex(r.rt, 16, col);
  readTex(r.ds, 8, dep);
}

void RasterChecks(ID3D11Device* d, ID3D11DeviceContext* c, const char* tag) {
  char what[300];
  Scene s;
  BuildScene(s, 31, 12000, false, false);
  DcsBufs o;
  Raster rr;
  if (!MakeDcs(d, s, o) || !MakeRaster(d, rr)) {
    Check(false, "periph inst: raster setup");
    o.Release();
    rr.Release();
    return;
  }
  ResetModule();
  SetDeviceForTest(d);
  Adopt(s, o);
  SetRenderSize(0, 476, 468);
  std::vector<uint32_t> col0, dep0, col1, dep1;
  DrawAll(d, c, s, o, rr, 1.0f, col0, dep0);
  size_t painted = 0;
  std::set<uint32_t> ids;
  for (size_t i = 0; i < col0.size(); i += 4)
    if (col0[i + 3]) {
      ++painted;
      ids.insert(col0[i]);
    }
  // filtered: the module's run, the same draws, then the verify path on a second filtered run
  g_mask = kBitZero;
  PeriphBegin(0);
  const bool committed = g_rt.committed;
  DrawAll(d, c, s, o, rr, 1.0f, col1, dep1);
  RestoreStock();
  std::vector<uint32_t> flags = Read(d, c, g_o.flags.b);
  uint32_t dropped = 0;
  for (uint32_t j = 0; j < s.N; ++j) dropped += flags[j] == 0;
  sprintf_s(what, "periph inst %s: raster: filtered G-buffer bit-identical to stock (%zu px drawn by %zu distinct "
                  "instances, %u of %u instances dropped; colour identical %d, depth identical %d)",
            tag, painted, ids.size(), dropped, s.N, col0 == col1 ? 1 : 0, dep0 == dep1 ? 1 : 0);
  Check(committed && dropped > 500 && painted > 2000 && ids.size() > 300 && col0 == col1 && dep0 == dep1, what);
  // verify: the dropped set redrawn with the same VS / PS / state into a scratch depth under queries
  {
    g_verify = true;
    g_mask = kBitZero;
    g_verDraws = g_verSamples = g_verQueries = g_verNonZero = g_verLost = g_verSkip = 0;
    PeriphBegin(0);
    g_rt.runView = 0;
    PollQueries();  // fills the free list
    const bool armed = ArmDraw(2);
    std::vector<uint32_t> c2, d2;
    DrawAll(d, c, s, o, rr, 1.0f, c2, d2);
    DisarmDraw();
    RestoreStock();
    c->Flush();
    for (int t = 0; t < 200 && !g_o.qPending.empty(); ++t) {
      Sleep(5);
      PollQueries();
    }
    sprintf_s(what, "periph inst %s: verify: %llu dropped-set draws, %llu queries, %llu samples, %llu with samples "
                    "(0 expected), image unchanged by the verify draws",
              tag, (unsigned long long)g_verDraws.load(), (unsigned long long)g_verQueries.load(),
              (unsigned long long)g_verSamples.load(), (unsigned long long)g_verNonZero.load());
    Check(armed && g_verDraws.load() > 100 && g_verQueries.load() == g_verDraws.load() && g_verNonZero.load() == 0 &&
              c2 == col0 && d2 == dep0,
          what);
    // negative control: meshes 10x their boundsphere (an underestimated bound) - the verify counts samples and the
    // image differs
    g_verDraws = g_verSamples = g_verQueries = g_verNonZero = 0;
    std::vector<uint32_t> c3, d3, c4, d4;
    DrawAll(d, c, s, o, rr, 10.0f, c3, d3);  // stock, large meshes
    PeriphBegin(0);
    PollQueries();
    ArmDraw(2);
    DrawAll(d, c, s, o, rr, 10.0f, c4, d4);
    DisarmDraw();
    RestoreStock();
    c->Flush();
    for (int t = 0; t < 200 && !g_o.qPending.empty(); ++t) {
      Sleep(5);
      PollQueries();
    }
    g_verify = false;
    sprintf_s(what, "periph inst %s: negative control (mesh 10x its boundsphere, beyond the rigorous bound): verify finds %llu draws with samples, "
                    "image differs: %s",
              tag, (unsigned long long)g_verNonZero.load(), c3 != c4 ? "yes" : "no");
    Check(g_verNonZero.load() > 0, what);
    g_latched = 0;
  }
  // table back to d3d11's entry
  void** vt = *reinterpret_cast<void***>(c);
  Check(vt[DcsQv_ContextSlot("DrawIndexedInstancedIndirect")] != reinterpret_cast<void*>(&H_Draw) &&
            vt[DcsQv_ContextSlot("VSSetShaderResources")] != reinterpret_cast<void*>(&H_VsSrv),
        "periph inst: verify arm leaves no hook in the context table");
  g_mask = 0;
  o.Release();
  rr.Release();
  ResetModule();
}

// ---------------------------------------------------------------------------
// 4. DCS's own chain kernels (copies of the 2.9.30 DXBC) build the lists
// ---------------------------------------------------------------------------
void StockChainChecks(ID3D11Device* d, ID3D11DeviceContext* c) {
  const wchar_t* dir = _wgetenv(L"QV_R34_STOCK_DIR");
  if (!dir || !*dir) {
    printf("SKIP periph inst: DCS's chain kernels (set QV_R34_STOCK_DIR to the folder of extracted .dxbc)\n");
    return;
  }
  auto load = [&](const wchar_t* md5) { return cftest::ReadAll(std::wstring(dir) + L"\\" + md5 + L".dxbc"); };
  const std::vector<uint8_t> bClear = load(L"8d918e57ed41b3c3389fee74bb5de7ce"),
                             bCounters = load(L"e04d8a271d158223394c66f6bb4ad4f5"),
                             bOffsets = load(L"7fa44bc571591d74ca45a2e8eb14de88"),
                             bSort = load(L"a183a72a8be34094fb7ce5070d4a3969"),
                             bFill = load(L"7b6634f142de6cb81074d7770667c70a");
  if (bClear.empty() || bCounters.empty() || bOffsets.empty() || bSort.empty() || bFill.empty()) {
    Check(false, "periph inst: DCS's chain kernels (clear, calccounters, calcoffsets, sort, fillindirect) found");
    return;
  }
  ID3D11ComputeShader *csClear = nullptr, *csCounters = nullptr, *csOffsets = nullptr, *csSort = nullptr,
                      *csFill = nullptr;
  d->CreateComputeShader(bClear.data(), bClear.size(), nullptr, &csClear);
  d->CreateComputeShader(bCounters.data(), bCounters.size(), nullptr, &csCounters);
  d->CreateComputeShader(bOffsets.data(), bOffsets.size(), nullptr, &csOffsets);
  d->CreateComputeShader(bSort.data(), bSort.size(), nullptr, &csSort);
  d->CreateComputeShader(bFill.data(), bFill.size(), nullptr, &csFill);
  Scene s;
  BuildScene(s, 41, 20000, true);
  DcsBufs o;
  bool ok = csClear && csCounters && csOffsets && csSort && csFill && MakeDcs(d, s, o);
  // DCS's chain on our synthetic mainBuffer: counts, ranks, offsets, indices, arguments
  const UINT sr = D3D11_BIND_SHADER_RESOURCE, ua = D3D11_BIND_UNORDERED_ACCESS;
  const uint32_t sums0[8] = {s.N, 0, 0, 0, 0, 0, 0, 0};
  ID3D11Buffer* sums = MkBuf(d, 32, sr | ua, 0, 0, sums0);
  ID3D11Buffer* ranks = MkBuf(d, s.mainCap * 4, sr | ua, 0, 0, nullptr);
  std::vector<uint32_t> statics(s.args);  // fillindirect's static table: [1] is replaced by counts[[7]]
  for (uint32_t e = 0; e < s.nEntries; ++e) statics[e * 8 + 1] = 0x7777;
  ID3D11Buffer* stat = MkBuf(d, s.nEntries * 32, sr, 0, 0, statics.data());
  auto uav = [&](ID3D11Buffer* b, UINT elems, bool structured) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_UINT;
    u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = elems;
    ID3D11UnorderedAccessView* v = nullptr;
    d->CreateUnorderedAccessView(b, &u, &v);
    return v;
  };
  ok = ok && sums && ranks && stat;
  if (ok) {
    ID3D11UnorderedAccessView *uCnt = uav(o.cnts, s.nb, false), *uMain = uav(o.mainB, s.mainCap, true),
                              *uRank = uav(ranks, s.mainCap, false), *uOff = uav(o.offs, s.nb, false),
                              *uIdx = uav(o.idx, s.idxCap, false), *uArgs = uav(o.args, s.nEntries * 8, false);
    ID3D11ShaderResourceView *sSums = MkSrv(d, sums, 8, false), *sRank = MkSrv(d, ranks, s.mainCap, false),
                             *sStat = MkSrv(d, stat, s.nEntries * 8, false);
    ID3D11UnorderedAccessView* nu[3] = {};
    ID3D11ShaderResourceView* ns[4] = {};
    auto cb = [&](uint32_t a, uint32_t b) {
      const uint32_t v[4] = {a, b, 0, 0};
      return chainfast::MakeCb(d, v, 16);
    };
    // clear counts
    c->CSSetShader(csClear, nullptr, 0);
    c->CSSetUnorderedAccessViews(0, 1, &uCnt, nullptr);
    c->Dispatch(s.nb, 1, 1);
    c->CSSetUnorderedAccessViews(0, 3, nu, nullptr);
    // calccounters
    ID3D11Buffer* cb1 = cb(s.mainCap, s.nb);
    c->CSSetShader(csCounters, nullptr, 0);
    c->CSSetConstantBuffers(0, 1, &cb1);
    c->CSSetShaderResources(0, 1, &sSums);
    ID3D11UnorderedAccessView* u1[3] = {uMain, uRank, uCnt};
    c->CSSetUnorderedAccessViews(0, 3, u1, nullptr);
    c->Dispatch((s.N >> 7) + 1, 1, 1);
    c->CSSetUnorderedAccessViews(0, 3, nu, nullptr);
    // calcoffsets
    ID3D11Buffer* cb2 = cb((s.nb + 255) / 256, s.nb);
    c->CSSetShader(csOffsets, nullptr, 0);
    c->CSSetConstantBuffers(0, 1, &cb2);
    c->CSSetShaderResources(0, 1, &o.sCnts);
    c->CSSetUnorderedAccessViews(0, 1, &uOff, nullptr);
    c->Dispatch(1, 1, 1);
    c->CSSetUnorderedAccessViews(0, 3, nu, nullptr);
    // sort
    ID3D11Buffer* cb3 = cb(s.mainCap, 0);
    c->CSSetShader(csSort, nullptr, 0);
    c->CSSetConstantBuffers(0, 1, &cb3);
    ID3D11ShaderResourceView* s3[4] = {sSums, o.sOffs, o.sMain, sRank};
    c->CSSetShaderResources(0, 4, s3);
    c->CSSetUnorderedAccessViews(0, 1, &uIdx, nullptr);
    c->Dispatch((s.N + 127) / 128, 1, 1);
    c->CSSetUnorderedAccessViews(0, 3, nu, nullptr);
    c->CSSetShaderResources(0, 4, ns);
    // fillindirect
    ID3D11Buffer* cb4 = cb(s.nEntries, 0);
    c->CSSetShader(csFill, nullptr, 0);
    c->CSSetConstantBuffers(0, 1, &cb4);
    ID3D11ShaderResourceView* s4[2] = {o.sCnts, sStat};
    c->CSSetShaderResources(0, 2, s4);
    c->CSSetUnorderedAccessViews(0, 1, &uArgs, nullptr);
    c->Dispatch((s.nEntries + 127) / 128, 1, 1);
    c->CSSetUnorderedAccessViews(0, 3, nu, nullptr);
    c->CSSetShaderResources(0, 4, ns);
    c->CSSetShader(nullptr, nullptr, 0);
    // DCS's lists become the scene's (the indices' order is the stock kernel's)
    s.counts = Read(d, c, o.cnts);
    s.offsets = Read(d, c, o.offs);
    s.indices = Read(d, c, o.idx);
    s.args = Read(d, c, o.args);
    for (ID3D11Buffer* b : {cb1, cb2, cb3, cb4}) b->Release();
    for (ID3D11UnorderedAccessView* v : {uCnt, uMain, uRank, uOff, uIdx, uArgs}) Rel(v);
    for (ID3D11ShaderResourceView* v : {sSums, sRank, sStat}) Rel(v);
    // DCS's lists are consistent the way the filter assumes
    bool cons = true;
    uint32_t total = 0;
    for (uint32_t b = 0; b < s.nb; ++b) {
      cons = cons && s.offsets[b] == total;
      total += s.counts[b];
    }
    cons = cons && total == s.N;
    for (uint32_t b = 0; b < s.nb && cons; ++b)
      for (uint32_t k = 0; k < s.counts[b]; ++k)
        cons = cons && (s.main[s.indices[s.offsets[b] + k]].w[0] & 0xffffff) == b;
    for (uint32_t e = 0; e < s.nEntries && cons; ++e) {
      const uint32_t b = s.args[e * 8 + 7];
      cons = s.args[e * 8 + 1] == (b < s.nb ? s.counts[b] : 0);
    }
    Check(cons, "periph inst: DCS's chain kernels (copies) give offsets = scan(counts), each slot in its bucket, "
                "InstanceCount = counts[entry[7]]");
    ResetModule();
    SetDeviceForTest(d);
    Adopt(s, o);
    SetRenderSize(0, 476, 468);
    RunOut r;
    OneRun(d, c, s, o, kBitZero, true, r);
    const Struct st = CheckStructure(s, r.idx, r.args, r.flags);
    char what[260];
    sprintf_s(what, "periph inst: on DCS's chain output: stable partition, counts, args (%u kept, %u dropped), no error "
                    "(0x%x), stock back after%s%s",
              st.kept, st.dropped, r.stats.empty() ? 0xffffffffu : r.stats[kSErr], st.first.empty() ? "" : ": ",
              st.first.c_str());
    Check(st.ok && st.dropped > 0 && r.stats[kSErr] == 0 && r.idxAfter == s.indices && r.argsAfter == s.args, what);
  } else {
    Check(false, "periph inst: DCS chain kernels created");
  }
  Rel(sums);
  Rel(ranks);
  Rel(stat);
  for (ID3D11ComputeShader* x : {csClear, csCounters, csOffsets, csSort, csFill})
    if (x) x->Release();
  o.Release();
  ResetModule();
}

// ---------------------------------------------------------------------------
// 5. Pass boundaries with the identification arms on a WARP "DCS" context
// ---------------------------------------------------------------------------
struct FakePass {
  uint8_t b[0x40] = {};
  explicit FakePass(const char* name) {
    const uint64_t n = strlen(name);
    memcpy(b + 8, &n, 8);
    memcpy(b + 0x10, name, n);
  }
};

void BoundaryChecks(ID3D11Device* d, ID3D11DeviceContext* c, const char* tag) {
  ResetCensus();
  char what[300];
  Scene s;
  BuildScene(s, 51, 8000, false, false);  // no instances right at the eye: the core ones stay visible
  DcsBufs o;
  Raster rr;
  if (!MakeDcs(d, s, o) || !MakeRaster(d, rr)) {
    Check(false, "periph inst: boundary setup");
    return;
  }
  ResetModule();
  SetDeviceForTest(d);
  // GazeOut under a test-only name suffix (never Perception's block)
  SetEnvironmentVariableA(::dcsvr_perception::name_suffix_variable, "_qvtest_pif");
  std::wstring gname = std::wstring(::dcsvr_perception::gaze_block_name) + L"_qvtest_pif";
  HANDLE gmap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(::dcsvr_perception::GazeOut),
                                   gname.c_str());
  auto* gz = static_cast<::dcsvr_perception::GazeOut*>(MapViewOfFile(gmap, FILE_MAP_ALL_ACCESS, 0, 0, 0));
  {
    ::dcsvr_perception::GazeOut g{};
    g.viewCount = 4;
    const double rect[4] = {-0.3, 0.38, -0.33, 0.35};
    const float* P0 = &s.tcChain[16 * 4];
    const float* P1 = &s.tcChain[17 * 4];
    for (int e = 0; e < 2; ++e) {
      g.fovTan[2 + e][0] = float((rect[0] - P0[2]) / P0[0]);
      g.fovTan[2 + e][1] = float((rect[1] - P0[2]) / P0[0]);
      g.fovTan[2 + e][2] = float((rect[3] - P1[2]) / P1[1]);
      g.fovTan[2 + e][3] = float((rect[2] - P1[2]) / P1[1]);
    }
    g.seq = 2;
    memcpy(gz, &g, sizeof(g));
  }
  // focus TerrainContext whose projection gives the same rect (focus = NDC affine of the periphery):
  // rect.l = P00 (-1 - F02) / F00 + P02, rect.r = P00 (1 - F02) / F00 + P02, the same for y.
  float tcFocus[136 * 4];
  memcpy(tcFocus, s.tcGb, sizeof(tcFocus));
  {
    const double rect[4] = {-0.3, 0.38, -0.33, 0.35};
    const double P00 = s.tcChain[16 * 4 + 0], P02 = s.tcChain[16 * 4 + 2], P11 = s.tcChain[17 * 4 + 1],
                 P12 = s.tcChain[17 * 4 + 2];
    const double a = (rect[0] - P02) / P00, bb = (rect[1] - P02) / P00;
    const double cc = (rect[2] - P12) / P11, t = (rect[3] - P12) / P11;
    const double F00 = 2 / (bb - a), F02 = 1 - F00 * bb, F11 = 2 / (t - cc), F12 = 1 - F11 * t;
    tcFocus[16 * 4 + 0] = float(F00);
    tcFocus[16 * 4 + 2] = float(F02);
    tcFocus[17 * 4 + 1] = float(F11);
    tcFocus[17 * 4 + 2] = float(F12);
    tcFocus[44 * 4 + 2] = 1176;
    tcFocus[44 * 4 + 3] = 1164;
  }
  g_cfgMask = kBitZero | kBitCore;
  ptiming::g_topTid = GetCurrentThreadId();
  // Apply: compile + boundary callback (ptiming may not be installable here: the callback is called directly)
  CompileAll();
  g_installed = true;
  g_mask = kBitZero | kBitCore;
  OpenGaze();
  // static: plod::KindOf caches by pass address
  static FakePass cas("CascadeShadowPass0"), lights("LightsBuffersPass"), gb("GBufferPass"), dep("DepthResolverPass"),
      hmd("CopyToHMDPass");
  ID3D11ShaderResourceView* objSrv[2] = {o.sObjs, o.sLods};
  D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
  ud.Format = DXGI_FORMAT_UNKNOWN;
  ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  ud.Buffer.NumElements = s.mainCap;
  ID3D11UnorderedAccessView* mainUav = nullptr;
  d->CreateUnorderedAccessView(o.mainB, &ud, &mainUav);
  ID3D11ComputeShader* nop = nullptr;
  {
    std::vector<uint8_t> code;
    std::string err;
    chainfast::Compile("RWStructuredBuffer<uint4> u1 : register(u1); [numthreads(1,1,1)] void main() {}", "nop", code,
                       &err);
    d->CreateComputeShader(code.data(), code.size(), nullptr, &nop);
  }
  std::vector<uint32_t> col, dep0, colStock, depStock;
  DrawAll(d, c, s, o, rr, 1.0f, colStock, depStock);
  int committedFrames = 0, restoredOk = 0, imgFrames = 0, diffOut = 0, diffIn = 0;
  for (int frame = 0; frame < 6; ++frame) {
    OnBoundary(cas.b, true, 0);
    OnBoundary(cas.b, false, 0);
    for (int v = 0; v < 4; ++v) {
      OnBoundary(lights.b, true, 0);
      // the view's chain: a Parse-like dispatch (u1 mainBuffer, t3 objects, t4 LODs)
      c->CSSetShader(nop, nullptr, 0);
      ID3D11UnorderedAccessView* u[2] = {nullptr, mainUav};
      c->CSSetUnorderedAccessViews(0, 2, u, nullptr);
      c->CSSetShaderResources(3, 2, objSrv);
      c->Dispatch(1, 1, 1);
      ID3D11UnorderedAccessView* nu[2] = {};
      ID3D11ShaderResourceView* ns[2] = {};
      c->CSSetUnorderedAccessViews(0, 2, nu, nullptr);
      c->CSSetShaderResources(3, 2, ns);
      OnBoundary(lights.b, false, 0);
      c->UpdateSubresource(o.tc, 0, nullptr, v < 2 ? s.tcChain : tcFocus, 0, 0);
      OnBoundary(gb.b, true, 0);
      const bool periph = v < 2;
      c->UpdateSubresource(o.tc, 0, nullptr, v < 2 ? s.tcGb : tcFocus, 0, 0);
      DrawAll(d, c, s, o, rr, 1.0f, col, dep0);
      if (periph && g_rt.committed) ++committedFrames;  // R45: the commit happens at the first instancer draw
      if (v == 0 && g_rt.committed) {
        // zero + core filter: the image differs from stock only inside the focus rect (the composite shows the
        // focus there)
        ++imgFrames;
        if (getenv("QV_PIF_DEBUG")) {
          std::set<uint32_t> a, b;
          for (size_t i = 0; i < col.size(); i += 4) {
            if (colStock[i + 3]) a.insert(colStock[i]);
            if (col[i + 3]) b.insert(col[i]);
          }
          const std::vector<uint32_t> fl = Read(d, c, g_o.flags.b), ar = Read(d, c, o.args);
          uint32_t dr = 0, sumA = 0, sumS = 0;
          for (uint32_t j = 0; j < s.N; ++j) dr += fl[j] == 0;
          for (uint32_t e = 0; e < s.nEntries; ++e) {
            sumA += ar[e * 8 + 1];
            sumS += s.args[e * 8 + 1];
          }
          printf("     debug: ids visible stock %zu filtered %zu; flags dropped %u; args instances %u (stock %u)\n",
                 a.size(), b.size(), dr, sumA, sumS);
        }
        for (int y = 0; y < 468; ++y)
          for (int x = 0; x < 476; ++x) {
            const double nx = (x + 0.5) / 476 * 2 - 1, ny = 1 - (y + 0.5) / 468 * 2;
            const size_t i = (size_t(y) * 476 + x) * 4;
            const bool same = col[i] == colStock[i] && col[i + 1] == colStock[i + 1] && col[i + 2] == colStock[i + 2];
            const bool inRect = nx > -0.3 && nx < 0.38 && ny > -0.33 && ny < 0.35;
            if (!same) (inRect ? diffIn : diffOut)++;
          }
      }
      if (periph) {
        D3D11_VIEWPORT vp{0, 0, 476, 468, 0, 1};
        c->RSSetViewports(1, &vp);
      } else {
        D3D11_VIEWPORT vp{0, 0, 1176, 1164, 0, 1};
        c->RSSetViewports(1, &vp);
      }
      OnBoundary(gb.b, false, 0);
      OnBoundary(gb.b, true, 0);  // the decal GBufferPass of the run
      OnBoundary(gb.b, false, 0);
      OnBoundary(dep.b, true, 0);
      if (periph && Read(d, c, o.idx) == s.indices && Read(d, c, o.args) == s.args) ++restoredOk;
      OnBoundary(dep.b, false, 0);
      OnBoundary(hmd.b, true, 0);
      OnBoundary(hmd.b, false, 0);
    }
    c->Flush();
    Sleep(30);
  }
  OnBoundary(cas.b, true, 0);  // the readbacks of the last frame
  OnBoundary(cas.b, false, 0);
  sprintf_s(what, "periph inst %s: boundaries, one-shot arms in the context table: draw identified %d, Parse "
                  "identified %d, ready %d, render size learned %.0fx%.0f, chain size learned %d/%d, committed runs %d, stock after every run "
                  "%d/12, no fault %d, latched 0x%x",
            tag, g_drawLearned.load() ? 1 : 0, g_dispatchLearned.load() ? 1 : 0, g_resReady.load() ? 1 : 0, g_rt.W[0],
            g_rt.H[0], g_rt.zwKnown[0] ? 1 : 0, g_rt.zwKnown[1] ? 1 : 0, committedFrames, restoredOk,
            g_fault.load() ? 0 : 1, g_latched.load());
  Check(g_drawLearned.load() && g_dispatchLearned.load() && g_resReady.load() && g_rt.W[0] == 476 &&
            g_rt.zwKnown[0] && g_rt.zwKnown[1] && committedFrames >= 4 && restoredOk == 12 && !g_fault.load() &&
            g_latched.load() == 0,
        what);
  {
    std::lock_guard<std::mutex> lock(g_cenMu);
    sprintf_s(what, "periph inst: census per eye read back (eye 0: %llu runs, core on in %llu, core rect error gaze "
                    "%.2f px / previous %.2f px, error runs %llu)",
              (unsigned long long)g_cen[0].runs, (unsigned long long)g_cen[0].coreOn,
              g_cen[0].dgN ? g_cen[0].dgSum / 1000.0 / g_cen[0].dgN : -1.0,
              g_cen[0].dpN ? g_cen[0].dpSum / 1000.0 / g_cen[0].dpN : -1.0, (unsigned long long)g_cen[0].errRuns);
    Check(g_cen[0].runs >= 3 && g_cen[0].coreOn >= 3 && g_cen[0].dgN >= 3 && g_cen[0].dgSum / 1000.0 / g_cen[0].dgN < 0.1 &&
              g_cen[0].errRuns == 0,
          what);
  }
  sprintf_s(what, "periph inst: zero + core filter on %d frames: image identical to stock outside the focus rect "
                  "(%d px differ outside, %d inside; %llu instances dropped in the core per census, %llu dropped)",
            imgFrames, diffOut, diffIn, (unsigned long long)g_cen[0].coreI, (unsigned long long)g_cen[0].dropI);
  printf("     census eye 0: runs %llu inst %llu zeroI %llu coreI %llu dropI %llu coreOn %llu\n",
         (unsigned long long)g_cen[0].runs, (unsigned long long)g_cen[0].inst, (unsigned long long)g_cen[0].zeroI,
         (unsigned long long)g_cen[0].coreI, (unsigned long long)g_cen[0].dropI, (unsigned long long)g_cen[0].coreOn);
  Check(imgFrames >= 2 && diffOut == 0 && diffIn > 0, what);
  // shutdown: nothing of ours left
  Shutdown();
  void** vt = *reinterpret_cast<void***>(c);
  Check(vt[DcsQv_ContextSlot("DrawIndexedInstancedIndirect")] != reinterpret_cast<void*>(&H_Draw) &&
            vt[DcsQv_ContextSlot("Dispatch")] != reinterpret_cast<void*>(&H_Dispatch) && !g_resReady.load() &&
            !g_installed.load() && g_gazeView.load() == nullptr,
        "periph inst: shutdown: no hook in the table, resources and the gaze mapping released");
  g_cfgMask = 0;
  SetEnvironmentVariableA(::dcsvr_perception::name_suffix_variable, nullptr);
  UnmapViewOfFile(gz);
  CloseHandle(gmap);
  Rel(mainUav);
  Rel(nop);
  o.Release();
  rr.Release();
  ResetModule();
  SetDeviceForTest(nullptr);
}

// ---------------------------------------------------------------------------
// 6. R45: when the camera is read (R44 live: every census run had error bits 0x900, 4.4e12 "triangles").
// DCS uploads each view's TerrainContext (one DEFAULT CB, UpdateSubresource) as the first event INSIDE its
// GBufferPass [V gdd trace]: at the pass begin b11 still holds the previous upload (another view's camera) or
// nothing yet. The filter now snapshots at the run's first instancer draw.
// ---------------------------------------------------------------------------
struct CamOut {
  int committed = 0, restored = 0, imgFrames = 0, imgDiff = 0, lateImgDiff = 0;
  uint64_t runs = 0, errRuns = 0, late = 0, inst = 0, zeroI = 0;
  uint32_t errBits = 0, latched = 0;
  bool fault = false;
};
// A camera with the focus projection (P00 2.3) looking elsewhere: what R44's snapshot held [I from its 322 px
// gaze rect error and 92.5 % zero coverage].
void StaleCamera(const Scene& s, float* out) {
  Scene t = s;
  MakeCamera(t, 0.35 + 0.9, 0.3);
  memcpy(out, t.tcGb, sizeof(t.tcGb));
  out[16 * 4 + 0] = 2.2963f;
  out[17 * 4 + 1] = 2.3254f;
  for (int k = 0; k < 4; ++k) {  // VP rows 0/1 with the focus scale
    out[20 * 4 + k] *= 2.2963f / 0.7807443f;
    out[21 * 4 + k] *= 2.3254f / 0.7906425f;
  }
  out[44 * 4 + 2] = 1176;
  out[44 * 4 + 3] = 1164;
}
// Frames of boundaries + draws as DCS orders them; preCam is what b11 holds at each GBufferPass begin (nullptr =
// the G-buffer's own camera), midCam an upload after the draws inside the pass, latePass a second G-buffer pass
// of the run that draws the lists again.
CamOut CamFrames(ID3D11Device* d, ID3D11DeviceContext* c, const Scene& s, DcsBufs& o, Raster& rr,
                 const std::vector<uint32_t>& colStock, const float* preCam, const float* midCam, bool latePass,
                 int frames) {
  CamOut r;
  ResetModule();
  ResetCensus();
  g_noDraw = g_otherLists = g_noCam = g_lateDraws = g_camOther = g_focusNoDraw = g_detailLogged = 0;
  g_fault = false;
  SetDeviceForTest(d);
  CompileAll();
  g_installed = true;
  g_cfgMask = kBitZero;
  g_mask = kBitZero;
  pifilt::g_census = true;
  ptiming::g_topTid = GetCurrentThreadId();
  static FakePass cas("CascadeShadowPass0"), lights("LightsBuffersPass"), gb("GBufferPass"), dep("DepthResolverPass");
  float tcF[136 * 4];
  memcpy(tcF, s.tcGb, sizeof(tcF));
  tcF[44 * 4 + 2] = 1176;
  tcF[44 * 4 + 3] = 1164;
  ID3D11ShaderResourceView* objSrv[2] = {o.sObjs, o.sLods};
  D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
  ud.Format = DXGI_FORMAT_UNKNOWN;
  ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  ud.Buffer.NumElements = s.mainCap;
  ID3D11UnorderedAccessView* mainUav = nullptr;
  d->CreateUnorderedAccessView(o.mainB, &ud, &mainUav);
  ID3D11ComputeShader* nop = nullptr;
  {
    std::vector<uint8_t> code;
    std::string err;
    chainfast::Compile("RWStructuredBuffer<uint4> u1 : register(u1); [numthreads(1,1,1)] void main() {}", "nop", code,
                       &err);
    d->CreateComputeShader(code.data(), code.size(), nullptr, &nop);
  }
  std::vector<uint32_t> col, dep0;
  for (int frame = 0; frame < frames; ++frame) {
    OnBoundary(cas.b, true, 0);
    OnBoundary(cas.b, false, 0);
    for (int v = 0; v < 4; ++v) {
      OnBoundary(lights.b, true, 0);
      c->CSSetShader(nop, nullptr, 0);
      ID3D11UnorderedAccessView* u[2] = {nullptr, mainUav};
      c->CSSetUnorderedAccessViews(0, 2, u, nullptr);
      c->CSSetShaderResources(3, 2, objSrv);
      c->Dispatch(1, 1, 1);
      ID3D11UnorderedAccessView* nu[2] = {};
      ID3D11ShaderResourceView* ns[2] = {};
      c->CSSetUnorderedAccessViews(0, 2, nu, nullptr);
      c->CSSetShaderResources(3, 2, ns);
      OnBoundary(lights.b, false, 0);
      const float* own = v < 2 ? s.tcGb : tcF;
      c->UpdateSubresource(o.tc, 0, nullptr, preCam ? preCam : own, 0, 0);  // what b11 holds at the pass begin
      OnBoundary(gb.b, true, 0);
      c->UpdateSubresource(o.tc, 0, nullptr, own, 0, 0);  // DCS's own upload, first event inside the pass
      DrawAll(d, c, s, o, rr, 1.0f, col, dep0);
      if (v < 2 && g_rt.committed) {
        ++r.committed;
        ++r.imgFrames;
        if (col != colStock) ++r.imgDiff;
      }
      if (midCam && v < 2) c->UpdateSubresource(o.tc, 0, nullptr, midCam, 0, 0);
      OnBoundary(gb.b, false, 0);
      OnBoundary(gb.b, true, 0);  // the next G-buffer pass of the run (decal; with latePass it draws the lists)
      if (latePass) {
        c->UpdateSubresource(o.tc, 0, nullptr, own, 0, 0);
        DrawAll(d, c, s, o, rr, 1.0f, col, dep0);
        if (v < 2 && col != colStock) ++r.lateImgDiff;
      }
      OnBoundary(gb.b, false, 0);
      OnBoundary(dep.b, true, 0);
      if (v < 2 && Read(d, c, o.idx) == s.indices && Read(d, c, o.args) == s.args) ++r.restored;
      OnBoundary(dep.b, false, 0);
    }
    c->Flush();
    Sleep(30);
  }
  OnBoundary(cas.b, true, 0);
  OnBoundary(cas.b, false, 0);
  {
    std::lock_guard<std::mutex> lock(g_cenMu);
    for (int e = 0; e < 2; ++e) {
      r.runs += g_cen[e].runs;
      r.errRuns += g_cen[e].errRuns;
      r.errBits |= g_cen[e].errBits;
      r.inst += g_cen[e].inst;
      r.zeroI += g_cen[e].zeroI;
    }
  }
  r.late = g_lateDraws.load();
  r.latched = g_latched.load();
  r.fault = g_fault.load();
  Shutdown();
  pifilt::g_census = false;
  g_cfgMask = 0;
  Rel(mainUav);
  Rel(nop);
  ResetModule();
  return r;
}

void CameraTimingChecks(ID3D11Device* d, ID3D11DeviceContext* c, const char* tag) {
  char what[400];
  Scene s;
  BuildScene(s, 71, 6000, false, false);
  DcsBufs o;
  Raster rr;
  if (!MakeDcs(d, s, o) || !MakeRaster(d, rr)) {
    Check(false, "periph inst: camera timing setup");
    return;
  }
  ResetModule();
  SetDeviceForTest(d);
  CompileAll();
  std::vector<uint32_t> colStock, depStock;
  DrawAll(d, c, s, o, rr, 1.0f, colStock, depStock);
  float stale[136 * 4], zeros[136 * 4] = {};
  StaleCamera(s, stale);

  // (a) R44 reproduced with the module's own pieces: a snapshot at the pass begin (the R44 timing), then DCS's
  // upload and draws: the after-run check flags 0x100 and the detail names the stale camera.
  {
    Adopt(s, o);
    SetRenderSize(0, 476, 468);
    pifilt::g_census = true;
    g_mask = 0;
    c->UpdateSubresource(o.tc, 0, nullptr, stale, 0, 0);
    PeriphBegin(0);  // R44: CopyResource(snapshot, b11) at the GBufferPass begin
    c->UpdateSubresource(o.tc, 0, nullptr, s.tcGb, 0, 0);
    g_rt.inRun = true;
    g_rt.runView = 0;
    EndRun();
    const std::vector<uint32_t> st = Read(d, c, g_o.stats[0].b);
    const std::string desc = st.size() >= kSCount ? DescribeErr(st.data(), 476, 468, true, 816, 804, g_rt.range, 16) : "";
    const uint64_t zeroShare = st.size() > kSZeroI && st[kSInst] ? 100ull * st[kSZeroI] / st[kSInst] : 0;
    sprintf_s(what, "periph inst %s: R44 failure reproduced: snapshot at the pass begin (stale focus-like camera) -> "
                    "error bits 0x%x, %llu %% of instances 'zero coverage'; detail: %.200s",
              tag, st.empty() ? 0 : st[kSErr], (unsigned long long)zeroShare, desc.c_str());
    Check(st.size() >= kSCount && (st[kSErr] & kErrVp) && desc.find("0x100") != std::string::npos &&
              desc.find("P00 snapshot 2.29630") != std::string::npos && zeroShare >= 30,
          what);
    pifilt::g_census = false;
    c->UpdateSubresource(o.tc, 0, nullptr, s.tcChain, 0, 0);
    ResetModule();
  }
  // (b) the fix: b11 stale at every pass begin, DCS's upload inside the pass -> no error, commits, exact image
  {
    const CamOut r = CamFrames(d, c, s, o, rr, colStock, stale, nullptr, false, 6);
    sprintf_s(what, "periph inst %s: stale camera at every GBufferPass begin: snapshot at the first instancer draw -> %d "
                    "commits, %llu runs read back, error runs %llu (bits 0x%x), latched 0x%x, filtered image = stock in "
                    "%d/%d, stock after %d/12, fault %d",
              tag, r.committed, (unsigned long long)r.runs, (unsigned long long)r.errRuns, r.errBits, r.latched,
              r.imgFrames - r.imgDiff, r.imgFrames, r.restored, r.fault ? 1 : 0);
    Check(r.committed >= 4 && r.runs >= 6 && r.errRuns == 0 && r.latched == 0 && r.imgDiff == 0 && r.restored == 12 &&
              !r.fault,
          what);
  }
  // (c) uninitialised: b11 all zero at every pass begin
  {
    const CamOut r = CamFrames(d, c, s, o, rr, colStock, zeros, nullptr, false, 6);
    sprintf_s(what, "periph inst %s: zeroed camera CB at every GBufferPass begin -> %d commits, error runs %llu (bits "
                    "0x%x), latched 0x%x, image = stock %d/%d",
              tag, r.committed, (unsigned long long)r.errRuns, r.errBits, r.latched, r.imgFrames - r.imgDiff,
              r.imgFrames);
    Check(r.committed >= 4 && r.errRuns == 0 && r.latched == 0 && r.imgDiff == 0, what);
  }
  // (d) DCS uploads another camera inside the pass after the commit draw: caught (0x100), latched
  {
    const CamOut r = CamFrames(d, c, s, o, rr, colStock, nullptr, stale, false, 6);
    sprintf_s(what, "periph inst %s: camera changed inside the pass after the commit -> error bits 0x%x in %llu runs, "
                    "latched 0x%x",
              tag, r.errBits, (unsigned long long)r.errRuns, r.latched);
    Check((r.errBits & kErrVp) && r.errRuns > 0 && (r.latched & kBits) == kBits, what);
  }
  // (e) the lists drawn again in a later G-buffer pass of the run: counted, and they see stock lists
  {
    const CamOut r = CamFrames(d, c, s, o, rr, colStock, nullptr, nullptr, true, 5);
    sprintf_s(what, "periph inst %s: instancer draws in a later G-buffer pass of the run: %llu counted, image = stock "
                    "in that pass (%d differ), commits %d, error runs %llu",
              tag, (unsigned long long)r.late, r.lateImgDiff, r.committed, (unsigned long long)r.errRuns);
    Check(r.late > 0 && r.lateImgDiff == 0 && r.committed >= 3 && r.errRuns == 0, what);
  }
  // (f) LODs whose gPrimCount is garbage (R44: 4.4e12 triangles per run): weight 0, counted apart
  {
    SetDeviceForTest(d);  // CamFrames' Shutdown released the module's device
    Adopt(s, o);
    SetRenderSize(0, 476, 468);
    std::vector<uint32_t> lods = s.lods;
    std::set<uint32_t> bad;
    for (uint32_t l = 0; l < s.nLod; l += 7) {
      lods[l * 12 + 7] = 0xffffffffu;
      bad.insert(l);
    }
    c->UpdateSubresource(o.lods, 0, nullptr, lods.data(), 0, 0);
    RunOut q;
    OneRun(d, c, s, o, 0, true, q);
    uint64_t tri = 0, nBad = 0;
    for (uint32_t i = 0; i < s.N; ++i) {
      const uint32_t l = s.main[i].w[1] & 0xffffff;
      if (l >= s.nLod) continue;
      if (bad.count(l)) ++nBad;
      else tri += s.lods[l * 12 + 7];
    }
    const uint64_t gTri = q.stats.size() > kSTri + 1 ? (uint64_t(q.stats[kSTri]) | (uint64_t(q.stats[kSTri + 1]) << 32)) : 0;
    const uint64_t argK = q.stats.size() > kSArgTriK + 1
                              ? (uint64_t(q.stats[kSArgTriK]) | (uint64_t(q.stats[kSArgTriK + 1]) << 32))
                              : 0;
    uint64_t argTri = 0;
    for (uint32_t e = 0; e < s.nEntries; ++e) argTri += uint64_t(s.args[e * 8] / 3) * s.args[e * 8 + 1];
    sprintf_s(what, "periph inst %s: implausible gPrimCount: %u instances flagged (CPU %llu), triangles %llu (CPU %llu); "
                    "argument triangles %llu (CPU %llu)",
              tag, q.stats.empty() ? 0 : q.stats[kSDBadPrim], (unsigned long long)nBad, (unsigned long long)gTri,
              (unsigned long long)tri, (unsigned long long)argK, (unsigned long long)argTri);
    Check(!q.stats.empty() && q.stats[kSDBadPrim] == nBad && nBad > 0 && gTri == tri &&
              argK == argTri,
          what);
    c->UpdateSubresource(o.lods, 0, nullptr, s.lods.data(), 0, 0);
    ResetModule();
  }
  o.Release();
  rr.Release();
}

// R45 diagnostics, pure: each error bit is decoded with the values the check saw.
void DescribeChecks() {
  uint32_t s[kSCount] = {};
  auto put = [&](int at, float f) { memcpy(&s[at], &f, 4); };
  s[kSErr] = kErrVp | kErrEye | kErrArgs | kErrSlot;
  s[kSDRows] = 0xff;
  put(kSDMaxD, 1.5f);
  s[kSDRow] = 20 * 4 + 3;
  put(kSDSnapV, 33.95f);
  put(kSDLiveV, 11.71f);
  put(kSDSnapP00, 2.2963f);
  put(kSDLiveP00, 0.7807f);
  s[kSDFocFlags] = 1 | 2 | 8;
  put(kSDFocRel, 0.25f);
  s[kSDFirstSlot] = ~123u;
  s[kSDFirstArg] = ~7u;
  const std::string d = DescribeErr(s, 604, 596, true, 604, 596, 1024, 16);
  const bool ok = d.find("0x100") != std::string::npos && d.find("row 20.w") != std::string::npos &&
                  d.find("snapshot 33.95") != std::string::npos && d.find("P00 snapshot 2.29630 / G-buffer 0.78070") != std::string::npos &&
                  d.find("0x800") != std::string::npos && d.find("row 23 differs 1") != std::string::npos &&
                  d.find("row 22 differs 0") != std::string::npos && d.find("entry 7") != std::string::npos &&
                  d.find("slot 123") != std::string::npos && d.find("0x200") == std::string::npos;
  char what[700];
  sprintf_s(what, "periph inst: error detail decodes each check with its values: %.500s", d.c_str());
  Check(ok, what);
}

// QV_PIF_TIME=1 (hardware only): GPU time of one periphery run at DCS's sizes (census: 524,288 index slots,
// 19,527 buckets, 19,129 argument entries, 14,361 objects) with 170,000 listed instances: the run-begin work
// (copies, classification, scan, partition, arguments, copies back) and the run end (stock copies, check).
void TimeRun(ID3D11Device* d, ID3D11DeviceContext* c) {
  Scene s;
  s.nb = 19527;
  s.nObj = 14361;
  s.nEntries = 19129;
  s.idxCap = s.mainCap = 524288;
  BuildScene(s, 61, 170000, false, false);
  DcsBufs o;
  if (!MakeDcs(d, s, o)) {
    Check(false, "periph inst: timing scene");
    return;
  }
  ResetModule();
  SetDeviceForTest(d);
  Adopt(s, o);
  SetRenderSize(0, 476, 468);
  g_rt.totalEst[0] = s.N;  // as after the first readback: the slot range in use
  D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, ts{D3D11_QUERY_TIMESTAMP, 0};
  std::vector<double> tb, te;
  for (int it = 0; it < 60; ++it) {
    ID3D11Query *qd = nullptr, *q0 = nullptr, *q1 = nullptr, *q2 = nullptr;
    d->CreateQuery(&dj, &qd);
    d->CreateQuery(&ts, &q0);
    d->CreateQuery(&ts, &q1);
    d->CreateQuery(&ts, &q2);
    c->Begin(qd);
    c->End(q0);
    g_mask = kBitZero | kBitCore;
    g_testGaze = true;
    g_testGazeTan[0] = -0.6f;
    g_testGazeTan[1] = 0.3f;
    g_testGazeTan[2] = 0.45f;
    g_testGazeTan[3] = -0.4f;
    PeriphBegin(0);
    c->End(q1);
    g_rt.inRun = true;
    g_rt.runView = 0;
    EndRun();
    c->End(q2);
    c->End(qd);
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{};
    while (c->GetData(qd, &dd, sizeof(dd), 0) == S_FALSE) Sleep(0);
    UINT64 t0 = 0, t1 = 0, t2 = 0;
    c->GetData(q0, &t0, 8, 0);
    c->GetData(q1, &t1, 8, 0);
    c->GetData(q2, &t2, 8, 0);
    if (!dd.Disjoint && it >= 10) {
      tb.push_back((t1 - t0) * 1e6 / dd.Frequency);
      te.push_back((t2 - t1) * 1e6 / dd.Frequency);
    }
    qd->Release();
    q0->Release();
    q1->Release();
    q2->Release();
  }
  g_testGaze = false;
  g_mask = 0;
  std::sort(tb.begin(), tb.end());
  std::sort(te.begin(), te.end());
  char what[200];
  sprintf_s(what, "periph inst: GPU time of one periphery run at DCS's sizes: begin %.1f us, end %.1f us (medians of %zu)",
            tb.empty() ? -1.0 : tb[tb.size() / 2], te.empty() ? -1.0 : te[te.size() / 2], tb.size());
  Check(!tb.empty(), what);
  o.Release();
  ResetModule();
}

void RunOn(D3D_DRIVER_TYPE type, const char* tag, bool all) {
  ID3D11Device* d = nullptr;
  ID3D11DeviceContext* c = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, type, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &d, nullptr, &c))) {
    printf("SKIP periph inst: no %s device\n", tag);
    return;
  }
  GpuChecks(d, c, tag);
  RasterChecks(d, c, tag);
  if (all) {
    StockChainChecks(d, c);
    BoundaryChecks(d, c, tag);
    CameraTimingChecks(d, c, tag);
  }
  if (type == D3D_DRIVER_TYPE_HARDWARE && getenv("QV_PIF_TIME")) TimeRun(d, c);
  SetDeviceForTest(nullptr);
  c->Release();
  d->Release();
}

void Run() {
  PureChecks();
  DescribeChecks();
  RunOn(D3D_DRIVER_TYPE_WARP, "WARP", true);
  if (getenv("QV_PIF_HW")) RunOn(D3D_DRIVER_TYPE_HARDWARE, "GPU", true);
}

}  // namespace piftest
