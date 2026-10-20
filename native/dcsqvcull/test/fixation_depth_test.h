// Offline tests for fixation_depth.h (Depth boost's fixation depth): shared
// struct layout, the seq guard (odd = busy, torn reads impossible under a
// concurrent writer), gProj parsing (reverse-Z / forward, infinite / finite,
// LH / RH), gaze tangents <-> texels, the window plan (1.5° ring covered at
// focus resolution), histogram bins, cone/ring classification, the FOV view
// match, and the full pipeline on a WARP device: fake GBufferPass runs for
// four views with synthetic depth textures (R32G8X24 array slice, R24G8, R32),
// DCS's cPerView bound with *1 offsets, the published DepthIn for near, far,
// edge and sky gazes, the eye retarget, and DCS's CS/OM/VS state and depth
// contents bit-identical across our dispatch.
#pragma once

namespace fdtest {

using namespace fixdepth;

// Logical P[r][c] (row-vector convention) of a perspective projection with
// the given FOV tangents; reverse-Z infinite unless far > 0.
void MakeProj(float P[16], double l, double r, double u, double d, double n, double f, bool reverse, bool rh = false) {
  for (int i = 0; i < 16; ++i) P[i] = 0;
  const double sx = 2.0 / (r - l), sy = 2.0 / (u - d);
  const double ox = -(r + l) / (r - l), oy = -(u + d) / (u - d);
  const double s = rh ? -1.0 : 1.0;  // P23
  P[0] = static_cast<float>(sx);
  P[5] = static_cast<float>(sy);
  P[8] = static_cast<float>(ox * s);
  P[9] = static_cast<float>(oy * s);
  P[11] = static_cast<float>(s);
  // depth = (z * P22 + P32) / w, w = z * s
  double a, b;  // depth = a + b / w
  if (reverse) {
    if (f > 0) {
      a = n / (n - f);
      b = -f * a;
    } else {
      a = 0;
      b = n;
    }
  } else {
    if (f > 0) {
      a = f / (f - n);
      b = -n * a;
    } else {
      a = 1;
      b = -n;
    }
  }
  P[10] = static_cast<float>(a * s);  // z * P22 = a * w
  P[14] = static_cast<float>(b);
}

double ProjDepth(const float P[16], double tx, double ty, double w, bool rh = false) {
  const double z = rh ? -w : w;
  const double x = tx * w, y = ty * w;
  const double cz = x * P[2] + y * P[6] + z * P[10] + P[14];
  const double cw = x * P[3] + y * P[7] + z * P[11] + P[15];
  return cz / cw;
}

bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

void TestLayoutAndSeq() {
  Check(sizeof(GazeOut) == 128 && sizeof(DepthIn) == 96, "fixdepth: GazeOut 128 / DepthIn 96 bytes (pack 8)");
  alignas(8) uint8_t buf[sizeof(DepthIn)] = {};
  DepthIn w{};
  w.medianM = 3.5f;
  w.view = 2;
  SeqWrite(buf, w);
  DepthIn r{};
  Check(SeqRead(buf, r) && r.seq == 2 && r.medianM == 3.5f && r.view == 2, "fixdepth: seq write/read, seq even 2");
  *reinterpret_cast<uint64_t*>(buf) = 3;  // writer busy
  Check(!SeqRead(buf, r, 8), "fixdepth: odd seq is never read");
  *reinterpret_cast<uint64_t*>(buf) = 5;  // a writer died mid-write
  SeqWrite(buf, w);
  Check(*reinterpret_cast<uint64_t*>(buf) == 8, "fixdepth: writer recovers from an odd seq (6 -> 8)");
  // Torn reads: a writer thread puts the same value into every field.
  alignas(8) static uint8_t shared[sizeof(GazeOut)] = {};
  std::atomic<bool> stop{false};
  std::thread wr([&] {
    GazeOut g{};
    for (uint32_t i = 1; !stop.load(); ++i) {
      const float f = static_cast<float>(i);
      g.displayTime = i;
      g.flags = i;
      g.viewCount = i;
      for (auto& v : g.gazeTan) v[0] = v[1] = f;
      for (auto& v : g.fovTan) v[0] = v[1] = v[2] = v[3] = f;
      g.vergenceM = f;
      SeqWrite(shared, g);
    }
  });
  int ok = 0, torn = 0;
  for (int i = 0; i < 200000; ++i) {
    GazeOut g{};
    if (!SeqRead(shared, g)) continue;
    ++ok;
    const float f = static_cast<float>(g.flags);
    bool same = g.viewCount == g.flags && static_cast<uint32_t>(g.displayTime) == g.flags && g.vergenceM == f;
    for (auto& v : g.gazeTan) same = same && v[0] == f && v[1] == f;
    for (auto& v : g.fovTan) same = same && v[0] == f && v[3] == f;
    if (!same || (g.seq & 1)) ++torn;
  }
  stop = true;
  wr.join();
  Check(ok > 1000 && torn == 0, "fixdepth: seq guard, no torn read under a concurrent writer");
  // DepthGrid: layout (also static_asserts) and a 9 KB seqlock round trip.
  Check(sizeof(DepthGrid) == 9264 && offsetof(DepthGrid, fovTan) == 32 && offsetof(DepthGrid, nearestM) == 48 &&
            offsetof(DepthGrid, typicalM) == 4144 && offsetof(DepthGrid, nearShare) == 8240,
        "fixdepth grid: DepthGrid 9264 bytes, offsets 32/48/4144/8240");
  {
    static DepthGrid a{}, b{};
    alignas(8) static uint8_t gbuf[sizeof(DepthGrid)] = {};
    for (int k = 0; k < kGridN * kGridN; ++k) {
      a.nearestM[k] = static_cast<float>(k);
      a.typicalM[k] = static_cast<float>(2 * k);
      a.nearShare[k] = static_cast<uint8_t>(k);
    }
    a.flags = 9;
    a.w = a.h = 32;
    SeqWrite(gbuf, a);
    const bool rd = SeqRead(gbuf, b);
    Check(rd && b.seq == 2 && b.flags == 9 && b.nearestM[1023] == 1023.0f && b.typicalM[5] == 10.0f &&
              b.nearShare[300] == static_cast<uint8_t>(300),
          "fixdepth grid: seqlock write/read of the whole grid");
  }
  // BuildGrid from the shader's words: packing, flags, FOV from gProj.
  {
    std::vector<uint32_t> wds(kGridWords, 0);
    float P[16];
    MakeProj(P, -1.5, 1.0, 1.2, -1.3, 0.02, 0, true);
    memcpy(wds.data() + 3 * kGridN * kGridN, P, sizeof(P));
    const float n5 = 0.46f, t5 = 0.5f;
    memcpy(&wds[5], &n5, 4);
    memcpy(&wds[1024 + 5], &t5, 4);
    wds[2048 + 5] = 200 | 512;
    wds[2048 + 6] = 0 | 256 | 512;
    DepthGrid g = BuildGrid(wds.data(), 77, 0, true);
    Check(g.nearestM[5] == n5 && g.typicalM[5] == t5 && g.nearShare[5] == 200 && g.nearestM[7] == 0 &&
              g.flags == (kDepthValid | kDepthReverseZ | kDepthWater | kDepthWaterPass) && g.displayTime == 77 &&
              Near(g.fovTan[0], -1.5, 1e-5) && Near(g.fovTan[3], -1.3, 1e-5) && g.w == 32,
          "fixdepth grid: words -> DepthGrid (near share, water bit, flags, FOV)");
    wds[2048 + 5] = wds[2048 + 6] = 0;
    Check(!(BuildGrid(wds.data(), 0, 0, false).flags & kDepthValid), "fixdepth grid: no cell with samples -> not valid");
  }
}

void TestProjection() {
  float P[16];
  // Reverse-Z infinite (DCS-like), asymmetric FOV.
  MakeProj(P, -0.65, 0.55, 0.6, -0.62, 0.05, 0, true);
  Proj p = ParseProj(P);
  Check(p.ok && p.reverse && p.infinite, "fixdepth: reverse-Z infinite detected");
  bool ok = true;
  for (double w : {0.06, 0.8, 1.5, 3.0, 200.0, 15000.0}) ok = ok && Near(ForwardDist(p, ProjDepth(P, 0.1, -0.2, w)) / w, 1, 1e-4);
  Check(ok, "fixdepth: reverse-Z infinite depth -> metres");
  Check(std::isinf(ForwardDist(p, 0.0)), "fixdepth: reverse-Z depth 0 = sky (infinite)");
  // Reverse-Z finite.
  MakeProj(P, -0.6, 0.6, 0.6, -0.6, 0.1, 50000, true);
  p = ParseProj(P);
  ok = p.ok && p.reverse && !p.infinite;
  for (double w : {0.2, 2.0, 100.0, 40000.0}) ok = ok && Near(ForwardDist(p, ProjDepth(P, 0, 0, w)) / w, 1, 1e-3);
  Check(ok, "fixdepth: reverse-Z finite detected, depth -> metres");
  // Forward finite and forward infinite.
  MakeProj(P, -0.6, 0.6, 0.6, -0.6, 0.1, 1000, false);
  p = ParseProj(P);
  // Forward finite: the clear value 1.0 is the far plane itself (1000 m).
  ok = p.ok && !p.reverse && !p.infinite && Near(ForwardDist(p, ProjDepth(P, 0, 0, 5.0)), 5.0, 1e-3) &&
       Near(ForwardDist(p, 1.0), 1000.0, 1.0);
  MakeProj(P, -0.6, 0.6, 0.6, -0.6, 0.1, 0, false);
  p = ParseProj(P);
  ok = ok && p.ok && !p.reverse && p.infinite && Near(ForwardDist(p, ProjDepth(P, 0, 0, 7.0)), 7.0, 1e-3);
  Check(ok, "fixdepth: forward-Z finite / infinite detected, depth -> metres");
  // Right-handed (P23 = -1).
  MakeProj(P, -0.7, 0.5, 0.6, -0.6, 0.05, 0, true, true);
  p = ParseProj(P);
  double nx, ny, tx, ty;
  TanToNdc(p, 0.3, -0.2, nx, ny);
  const double w = 4.0;
  const double cx = 0.3 * w * P[0] + (-0.2 * w) * P[4] + (-w) * P[8], cw = -w * P[11];
  ok = p.ok && p.reverse && Near(ForwardDist(p, ProjDepth(P, 0.3, -0.2, w, true)), w, 1e-4) && Near(nx, cx / cw, 1e-6);
  Check(ok, "fixdepth: right-handed projection (P23 = -1)");
  // Orthographic / garbage rejected.
  float O[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0.001f, 0, 0, 0, 0.5f, 1};
  Check(!ParseProj(O).ok, "fixdepth: orthographic projection rejected");
  // Tangents <-> NDC <-> pixels, DCS FOV.
  MakeProj(P, -0.65, 0.55, 0.6, -0.62, 0.05, 0, true);
  p = ParseProj(P);
  double fov[4];
  DcsFov(p, fov);
  Check(Near(fov[0], -0.65, 1e-6) && Near(fov[1], 0.55, 1e-6) && Near(fov[2], 0.6, 1e-6) && Near(fov[3], -0.62, 1e-6),
        "fixdepth: DCS FOV tangents from gProj");
  Viewport vp{10, 20, 2028, 2004, 0, 1};
  double px, py;
  TanToPixel(p, vp, -0.65, 0.6, px, py);
  ok = Near(px, 10, 1e-3) && Near(py, 20, 1e-3);
  TanToPixel(p, vp, 0.55, -0.62, px, py);
  ok = ok && Near(px, 2038, 1e-3) && Near(py, 2024, 1e-3);
  TanToPixel(p, vp, 0.12, -0.31, px, py);
  const int ix = static_cast<int>(std::floor(px)), iy = static_cast<int>(std::floor(py));
  TexelToTan(p, vp, ix, iy, tx, ty);
  const double tpp = 1.2 / 2028;  // tangent per pixel
  ok = ok && std::fabs(tx - 0.12) <= tpp && std::fabs(ty + 0.31) <= 1.22 / 2004;
  Check(ok, "fixdepth: gaze tangents -> viewport pixel -> texel centre tangent (round trip)");
}

void TestPlanAndHist() {
  // 2028 px over ±31.5°: about 1655 px per tangent.
  const double t = std::tan(31.5 * kDeg);
  const double ppt = 2028 / (2 * t);
  Plan pl = PlanWindow(ppt, ppt, 0, 0);
  const double need = ppt * std::tan(1.5 * kDeg);
  printf("     plan at 2028 px: radius %.1f px, G %d, stride %d\n", pl.radiusPx, pl.G, pl.stride);
  Check(pl.stride == 1 && pl.half * pl.stride >= need && pl.G <= kMaxG && pl.G > 65,
        "fixdepth: window at focus resolution covers the 1.5° ring (more than 65 x 65)");
  Plan off = PlanWindow(ppt, ppt, 0.5, 0.3);
  Check(off.half * off.stride >= pl.half * pl.stride, "fixdepth: off-axis gaze widens the window");
  Plan big = PlanWindow(8000, 8000, 0, 0);
  Check(big.G <= kMaxG && big.stride >= 2 && big.half * big.stride >= 8000 * std::tan(1.5 * kDeg),
        "fixdepth: very high resolution strides the grid, G <= 127");
  Check(HistBin(0.1) == 0 && HistBin(0.2) == 0 && HistBin(0.21) == 0 && HistBin(20000) == 15 && HistBin(1e9) == 15 &&
            HistBin(INFINITY) == 15 && HistBin(NAN) == 0,
        "fixdepth: histogram clamps at 0.2 m and 20 km");
  bool mono = true;
  int prev = 0;
  const double step = std::pow(1e5, 1.0 / 16);
  for (int b = 0; b < 16; ++b) {
    const double mid = 0.2 * std::pow(step, b + 0.5);
    mono = mono && HistBin(mid) == b && HistBin(mid) >= prev;
    prev = HistBin(mid);
  }
  Check(mono, "fixdepth: 16 log bins, bin b holds 0.2 m * 10^(5 (b + 0.5) / 16)");
}

// Synthetic scene, as range (metres) by view tangent: sky above ty = 0.4,
// a panel at 0.8 m below ty = -0.2, terrain at 200 m between.
double SceneRange(double tx, double ty) {
  (void)tx;
  if (ty > 0.4) return INFINITY;
  if (ty < -0.2) return 0.8;
  return 200.0;
}
float SceneDepth(const float P[16], double tx, double ty) {
  const double r = SceneRange(tx, ty);
  if (std::isinf(r)) return 0.0f;  // reverse-Z sky
  const double w = r / std::sqrt(1 + tx * tx + ty * ty);
  return static_cast<float>(ProjDepth(P, tx, ty, w));
}

void TestSummary() {
  float P[16];
  MakeProj(P, -0.6, 0.6, 0.6, -0.6, 0.05, 0, true);
  const Proj p = ParseProj(P);
  Viewport vp{0, 0, 1024, 1024, 0, 1};
  auto run = [&](double gx, double gy) {
    const Plan pl = PlanWindow(1024 / 1.2, 1024 / 1.2, gx, gy);
    double px, py;
    TanToPixel(p, vp, gx, gy, px, py);
    Window w;
    memcpy(w.P, P, sizeof(P));
    w.cx = static_cast<int>(std::floor(px));
    w.cy = static_cast<int>(std::floor(py));
    w.G = pl.G;
    w.half = pl.half;
    w.stride = pl.stride;
    w.vp = vp;
    w.gx = gx;
    w.gy = gy;
    std::vector<float> s(static_cast<size_t>(pl.G) * pl.G);
    for (int j = 0; j < pl.G; ++j)
      for (int i = 0; i < pl.G; ++i) {
        const int ix = w.cx + (i - pl.half) * pl.stride, iy = w.cy + (j - pl.half) * pl.stride;
        double tx, ty;
        TexelToTan(p, vp, ix, iy, tx, ty);
        s[j * pl.G + i] = (ix < 0 || iy < 0 || ix >= 1024 || iy >= 1024) ? -1.0f : SceneDepth(P, tx, ty);
      }
    w.samples = s.data();
    return Summarize(w);
  };
  Summary s = run(0.0, -0.45);
  Check(s.valid && s.nearShare == 1 && s.farShare == 0 && Near(s.medianM, 0.8, 0.005) && Near(s.nearestM, 0.8, 0.005) &&
            s.edgeNearShare == 1 && s.reverse && s.infinite,
        "fixdepth: gaze on the panel: cone all near, median 0.8 m");
  int b = HistBin(0.8), sum = 0;
  for (int k = 0; k < 16; ++k) sum += s.hist[k];
  Check(s.hist[b] == sum && sum == s.cone, "fixdepth: histogram holds the cone in the 0.8 m bin");
  s = run(0.0, 0.1);
  Check(s.valid && s.farShare == 1 && Near(s.farMedianM, 200, 1.0) && Near(s.medianM, 200, 1.0),
        "fixdepth: gaze at the terrain: cone all far, far median 200 m");
  s = run(0.0, -0.2);
  Check(s.valid && s.nearShare > 0.35 && s.nearShare < 0.65 && s.farShare > 0.35 && s.edgeNearShare > 0.3 &&
            s.edgeFarShare > 0.3,
        "fixdepth: gaze on the panel edge: cone and ring mixed");
  // 1.25° below the edge: cone clean (near), ring sees the edge.
  const double gy = std::tan(std::atan(-0.2) - 1.25 * kDeg);
  s = run(0.0, gy);
  Check(s.valid && s.nearShare == 1 && s.edgeFarShare > 0.05 && s.edgeNearShare > 0.5,
        "fixdepth: edge inside the ring only: cone near, ring mixed");
  s = run(0.0, 0.5);
  Check(s.valid && s.farShare == 1 && s.medianM == 20000 && s.hist[15] == s.cone, "fixdepth: sky = 20 km, top bin");
  s = run(0.0, -0.595);  // window half outside the viewport (bottom edge at -0.6)
  Check(s.valid && s.cone > 0 && s.nearShare == 1, "fixdepth: samples outside the viewport are skipped");
  Window bad;
  Check(!Summarize(bad).valid, "fixdepth: no samples -> not valid");
}

void TestFallbackMath() {
  const float focus[4] = {-0.478f, 0.393f, 0.43f, -0.43f};  // measured left focus (DLSS)
  Check(ConeInside(focus, 0, 0, 1.0) && ConeInside(focus, 0.37, 0, 1.0) && !ConeInside(focus, 0.385, 0, 1.0) &&
            !ConeInside(focus, 0, 0.44, 1.0) && !ConeInside(focus, 0, -0.43, 1.0),
        "fixdepth: cone-inside-focus test at the FOV edges");
  const float bad[4] = {0.5f, -0.5f, 0.4f, -0.4f};
  Check(!ConeInside(bad, 0, 0, 1.0), "fixdepth: an empty FOV contains nothing");
  // Measured densities: focus 1176 px over 0.871 tan, periphery 476 px over 2.56 tan.
  const double cf = ConeDegFor(1176 / 0.871), cp = ConeDegFor(476 / 2.56);
  const double rp = std::tan(cp * kDeg) * (476 / 2.56);
  printf("     cone: focus %.2f deg, periphery %.2f deg (%.1f px radius)\n", cf, cp, rp);
  Check(cf == 1.0 && cp > 1.0 && 3.14159 * rp * rp >= 39.9, "fixdepth: cone widened only where 1° holds < 40 texels");
  Check(IsWaterStencil(2 << 3) && IsWaterStencil((2 << 3) | 4) && IsWaterStencil(6 << 3) && !IsWaterStencil(5 << 3) &&
            !IsWaterStencil(0) && !IsWaterStencil(1 << 3),
        "fixdepth: stencil composition WATER / UNDERWATER recognised (bits 3-5)");
  // Summarize counts water in the cone from the stencil plane.
  float P[16];
  MakeProj(P, -0.6, 0.6, 0.6, -0.6, 0.05, 0, true);
  const Proj p = ParseProj(P);
  Viewport vp{0, 0, 256, 256, 0, 1};
  const Plan pl = PlanWindow(256 / 1.2, 256 / 1.2, 0, 0);
  std::vector<float> dep(static_cast<size_t>(pl.G) * pl.G), st(dep.size());
  double px, py;
  TanToPixel(p, vp, 0, 0, px, py);
  Window w;
  memcpy(w.P, P, sizeof(P));
  w.cx = static_cast<int>(px);
  w.cy = static_cast<int>(py);
  w.G = pl.G;
  w.half = pl.half;
  w.stride = pl.stride;
  w.vp = vp;
  for (int j = 0; j < pl.G; ++j)
    for (int i = 0; i < pl.G; ++i) {
      dep[j * pl.G + i] = static_cast<float>(ProjDepth(P, 0, 0, 50.0));
      st[j * pl.G + i] = static_cast<float>(i < pl.half ? (kStencilWater << 3) : (1u << 3));  // left half water
    }
  w.samples = dep.data();
  w.stencil = st.data();
  Summary sm = Summarize(w);
  Check(sm.valid && sm.water > sm.cone / 4 && sm.water < sm.cone, "fixdepth: water samples counted in the cone");
  w.stencil = nullptr;
  Check(Summarize(w).water == 0, "fixdepth: no stencil plane -> no water");
}

// Test gaze writer: it creates and writes GazeOut only while nobody else
// does; once the layer's seq appears our writes stop and its records are read.
void TestTestWriter() {
  wchar_t gname[64], dname[64];
  swprintf_s(gname, L"Local\\FdTwGaze_%lu", GetCurrentProcessId());
  swprintf_s(dname, L"Local\\FdTwDepth_%lu", GetCurrentProcessId());
  const std::wstring og = g_gazeName, od = g_depthName;
  g_gazeName = gname;
  g_depthName = dname;
  g_testGaze = true;
  g_testGx = 0.25f;
  g_testGy = -0.3f;
  OpenMappings();
  GazeOut g{};
  bool ours = g_gazeOwned;
  for (int i = 0; i < 3; ++i) ours = ours && ReadGaze(g) && g.gazeTan[2][0] == 0.25f && g.viewCount == 4;
  Check(ours && !g_testWriterYielded.load(), "fixdepth test gaze: no layer -> our block, our gaze");
  // The layer starts: it opens the same block and writes its own record.
  HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, gname);
  void* lv = h ? MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GazeOut)) : nullptr;
  GazeOut layer{};
  layer.displayTime = 555;
  layer.flags = kGazeValid;
  layer.viewCount = 4;
  layer.gazeTan[2][0] = 0.1f;
  if (lv) SeqWrite(lv, layer);
  bool theirs = lv != nullptr;
  for (int i = 0; i < 3; ++i) theirs = theirs && ReadGaze(g) && g.displayTime == 555 && g.gazeTan[2][0] == 0.1f;
  Check(theirs && g_testWriterYielded.load(), "fixdepth test gaze: a seq we did not write stops our writes for good");
  layer.displayTime = 556;
  if (lv) SeqWrite(lv, layer);
  Check(ReadGaze(g) && g.displayTime == 556, "fixdepth test gaze: the layer's later records are read unchanged");
  if (lv) UnmapViewOfFile(lv);
  if (h) CloseHandle(h);
  CloseMappings();
  Check(!g_testWriterYielded.load() && g_testWriterSeq == 0, "fixdepth test gaze: closing resets the writer state");
  g_testGaze = false;
  g_gazeName = og;
  g_depthName = od;
}

void TestMatch() {
  GazeOut g{};
  g.viewCount = 4;
  const float fl[4][4] = {{-1.4f, 1.1f, 1.2f, -1.3f},
                          {-1.1f, 1.4f, 1.2f, -1.3f},
                          {-0.65f, 0.55f, 0.6f, -0.62f},
                          {-0.55f, 0.65f, 0.6f, -0.62f}};
  memcpy(g.fovTan, fl, sizeof(fl));
  double dcs[4] = {-0.551, 0.649, 0.6, -0.62};
  Match m = MatchFov(g, dcs);
  Check(m.best == 3 && m.decisive && m.convention == 0, "fixdepth: FOV match finds the right focus view");
  for (auto& v : g.fovTan) {
    v[0] = -v[0];
    v[3] = -v[3];
  }
  m = MatchFov(g, dcs);
  Check(m.best == 3 && m.decisive && m.convention == 1, "fixdepth: FOV match with left/down as magnitudes");
  double sym[4] = {-0.6, 0.6, 0.6, -0.6};
  GazeOut s{};
  s.viewCount = 4;
  for (int v = 2; v < 4; ++v) {
    s.fovTan[v][0] = -0.6f;
    s.fovTan[v][1] = 0.6f;
    s.fovTan[v][2] = 0.6f;
    s.fovTan[v][3] = -0.6f;
  }
  m = MatchFov(s, sym);
  Check(m.best >= 2 && !m.decisive, "fixdepth: identical focus FOVs are not decisive");
  double off[4] = {-3, 3, 3, -3};
  Check(MatchFov(s, off).best == -1, "fixdepth: nothing within tolerance -> no match");
}

// ---------------------------------------------------------------------------
// Pipeline on a device
// ---------------------------------------------------------------------------

struct FakePass {
  void* vt = nullptr;
  uint64_t len = 0;
  char name[40] = {};
  uint8_t pad[0x30] = {};
  uint8_t* node = nullptr;  // +0x68: a SimplePassData pass's resource node (none: no item vector)
};
FakePass MakePass(const char* n) {
  FakePass p;
  p.len = strlen(n);
  strcpy_s(p.name, n);
  return p;
}

struct View {
  int w = 0, h = 0;
  ID3D11Texture2D* tex = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11Buffer* cb = nullptr;
  UINT cbFirst = 0, cbNum = 0;  // 0/0 = plain VSSetConstantBuffers
  float P[16] = {};
  double eyeX = 0;
};

template <class T>
void Rel(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

// Depth texture of `fmt` (typeless) with the scene drawn through P, filled
// through a copy (depth-stencil resources take no initial data on every driver).
bool MakeDepth(ID3D11Device* dev, ID3D11DeviceContext* ctx, View& v, DXGI_FORMAT fmt, UINT slices, UINT slice) {
  D3D11_TEXTURE2D_DESC d{};
  d.Width = v.w;
  d.Height = v.h;
  d.MipLevels = 1;
  d.ArraySize = slices;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(dev->CreateTexture2D(&d, nullptr, &v.tex))) return false;
  const Proj p = ParseProj(v.P);
  const Viewport vp{0, 0, static_cast<double>(v.w), static_cast<double>(v.h), 0, 1};
  const UINT bpp = fmt == DXGI_FORMAT_R32G8X24_TYPELESS ? 8 : 4;
  std::vector<uint8_t> px(static_cast<size_t>(v.w) * v.h * bpp * slices, 0);
  for (UINT s = 0; s < slices; ++s)
    for (int y = 0; y < v.h; ++y)
      for (int x = 0; x < v.w; ++x) {
        double tx, ty;
        TexelToTan(p, vp, x, y, tx, ty);
        const float dep = s == slice ? SceneDepth(v.P, tx, ty) : 0.37f;  // other slice: junk
        uint8_t* o = px.data() + ((static_cast<size_t>(s) * v.h + y) * v.w + x) * bpp;
        if (fmt == DXGI_FORMAT_R24G8_TYPELESS) {
          const uint32_t u = static_cast<uint32_t>(std::lround(std::clamp(dep, 0.0f, 1.0f) * 16777215.0)) | (40u << 24);
          memcpy(o, &u, 4);
        } else {
          memcpy(o, &dep, 4);
          if (bpp == 8) o[4] = 40;  // stencil
        }
      }
  D3D11_TEXTURE2D_DESC sd = d;
  sd.BindFlags = 0;
  std::vector<D3D11_SUBRESOURCE_DATA> init(slices);
  for (UINT s = 0; s < slices; ++s)
    init[s] = {px.data() + static_cast<size_t>(s) * v.w * v.h * bpp, static_cast<UINT>(v.w * bpp), 0};
  ID3D11Texture2D* src = nullptr;
  if (FAILED(dev->CreateTexture2D(&sd, init.data(), &src))) return false;
  ctx->CopyResource(v.tex, src);
  src->Release();
  D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
  dd.Format = fmt == DXGI_FORMAT_R32G8X24_TYPELESS ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT
              : fmt == DXGI_FORMAT_R24G8_TYPELESS  ? DXGI_FORMAT_D24_UNORM_S8_UINT
                                                   : DXGI_FORMAT_D32_FLOAT;
  if (slices > 1) {
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
    dd.Texture2DArray.FirstArraySlice = slice;
    dd.Texture2DArray.ArraySize = 1;
  } else {
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
  }
  return SUCCEEDED(dev->CreateDepthStencilView(v.tex, &dd, &v.dsv));
}

// DCS's cPerView: gViewInv at 192 (right axis row 0, position row 3), gProj at
// 256, column-major as DCS's shaders read it. With offsets the struct starts
// at constant `first`.
bool MakeCb(ID3D11Device* dev, View& v, UINT first) {
  std::vector<float> f((first * 16 + 512) / 4 + 64, 0.0f);
  float* base = f.data() + first * 4;
  auto put = [&](int byteOff, const float m[16]) {
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) base[byteOff / 4 + c * 4 + r] = m[r * 4 + c];
  };
  const float viewInv[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, static_cast<float>(v.eyeX), 1.5f, 0, 1};
  put(192, viewInv);
  put(256, v.P);
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = static_cast<UINT>(f.size() * 4 + 15) / 16 * 16;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  f.resize(d.ByteWidth / 4);
  D3D11_SUBRESOURCE_DATA s{f.data(), 0, 0};
  v.cbFirst = first;
  v.cbNum = first ? 32 : 0;
  return SUCCEEDED(dev->CreateBuffer(&d, &s, &v.cb));
}

// Every binding fixation_depth.h may touch, plus neighbours it must not.
struct Snap {
  void* cs = nullptr;
  void* srv[2] = {};
  void* uav[2] = {};
  void* cb[3] = {};
  UINT first[3] = {}, num[3] = {};
  void* rtv[8] = {};
  void* dsv = nullptr;
  void* vsCb7 = nullptr;
  UINT vsFirst = 0, vsNum = 0;
  D3D11_VIEWPORT vp{};
  UINT nvp = 0;
  bool operator==(const Snap& o) const {
    return cs == o.cs && !memcmp(srv, o.srv, sizeof(srv)) && !memcmp(uav, o.uav, sizeof(uav)) &&
           !memcmp(cb, o.cb, sizeof(cb)) && !memcmp(first, o.first, sizeof(first)) && !memcmp(num, o.num, sizeof(num)) &&
           !memcmp(rtv, o.rtv, sizeof(rtv)) && dsv == o.dsv && vsCb7 == o.vsCb7 && vsFirst == o.vsFirst &&
           vsNum == o.vsNum && nvp == o.nvp && !memcmp(&vp, &o.vp, sizeof(vp));
  }
};
Snap TakeSnap(ID3D11DeviceContext1* c) {
  Snap s;
  ID3D11ComputeShader* cs = nullptr;
  c->CSGetShader(&cs, nullptr, nullptr);
  s.cs = cs;
  ID3D11ShaderResourceView* srv[2] = {};
  c->CSGetShaderResources(0, 2, srv);
  ID3D11UnorderedAccessView* uav[2] = {};
  c->CSGetUnorderedAccessViews(0, 2, uav);
  ID3D11Buffer* cb[3] = {};
  c->CSGetConstantBuffers1(0, 3, cb, s.first, s.num);
  ID3D11RenderTargetView* rtv[8] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  c->OMGetRenderTargets(8, rtv, &dsv);
  ID3D11Buffer* v7 = nullptr;
  c->VSGetConstantBuffers1(7, 1, &v7, &s.vsFirst, &s.vsNum);
  s.nvp = 1;
  c->RSGetViewports(&s.nvp, &s.vp);
  auto keep = [](auto* p) {
    if (p) p->Release();
    return static_cast<void*>(p);
  };
  s.cs = keep(cs);
  for (int i = 0; i < 2; ++i) s.srv[i] = keep(srv[i]), s.uav[i] = keep(uav[i]);
  for (int i = 0; i < 3; ++i) s.cb[i] = keep(cb[i]);
  for (int i = 0; i < 8; ++i) s.rtv[i] = keep(rtv[i]);
  s.dsv = keep(dsv);
  s.vsCb7 = keep(v7);
  return s;
}

std::vector<uint8_t> ReadTex(ID3D11Device* dev, ID3D11DeviceContext* c, ID3D11Texture2D* t) {
  D3D11_TEXTURE2D_DESC d{};
  t->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* st = nullptr;
  std::vector<uint8_t> out;
  if (FAILED(dev->CreateTexture2D(&d, nullptr, &st))) return out;
  c->CopyResource(st, t);
  const bool wide = d.Format == DXGI_FORMAT_R32G8X24_TYPELESS || d.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS ||
                    d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
  const UINT bpp = wide ? 8 : 4;
  for (UINT s = 0; s < d.ArraySize; ++s) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(c->Map(st, s, D3D11_MAP_READ, 0, &m))) break;
    const uint8_t* b = static_cast<const uint8_t*>(m.pData);
    for (UINT y = 0; y < d.Height; ++y) out.insert(out.end(), b + y * m.RowPitch, b + y * m.RowPitch + d.Width * bpp);
    c->Unmap(st, s);
  }
  st->Release();
  return out;
}

void TestPipeline() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &imm))) {
    printf("SKIP fixdepth pipeline: no WARP 11.1 device\n");
    return;
  }
  ID3D11DeviceContext1* c = nullptr;
  imm->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c));
  Check(CompileShaders(), "fixdepth: compute shader variants compile (cs_5_0)");
  if (!c || !g_blobsReady.load()) {
    Rel(c), Rel(imm), Rel(dev);
    return;
  }
  // Views: periphery L/R 160x150 (R32), focus L 1024x1000 (R32G8X24 array,
  // slice 1), focus R 1024x1000 (R24G8). Mirrored focus FOVs.
  View v[4];
  const double fovs[4][4] = {{-1.4, 1.1, 1.2, -1.3}, {-1.1, 1.4, 1.2, -1.3}, {-0.65, 0.55, 0.6, -0.62},
                             {-0.55, 0.65, 0.6, -0.62}};
  bool made = true;
  for (int i = 0; i < 4; ++i) {
    v[i].w = i < 2 ? 160 : 1024;
    v[i].h = i < 2 ? 150 : 1000;
    v[i].eyeX = (i & 1) ? 0.032 : -0.032;
    MakeProj(v[i].P, fovs[i][0], fovs[i][1], fovs[i][2], fovs[i][3], 0.05, 0, true);
    const DXGI_FORMAT f = i == 2 ? DXGI_FORMAT_R32G8X24_TYPELESS
                          : (i == 3 || i == 0) ? DXGI_FORMAT_R24G8_TYPELESS
                                               : DXGI_FORMAT_R32_TYPELESS;
    made = made && MakeDepth(dev, c, v[i], f, i == 2 ? 2 : 1, i == 2 ? 1 : 0) && MakeCb(dev, v[i], i == 3 ? 16 : 0);
  }
  // DCS-like CS / OM state that must survive: a CS, SRVs, UAVs, CBs (slot 1 with offsets), an RTV.
  ID3D11ComputeShader* dcsCs = nullptr;
  dev->CreateComputeShader(g_blob[0].data(), g_blob[0].size(), nullptr, &dcsCs);
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = 4096;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  ID3D11Buffer *b0 = nullptr, *b1 = nullptr, *cbA = nullptr, *cbB = nullptr;
  dev->CreateBuffer(&bd, nullptr, &b0);
  dev->CreateBuffer(&bd, nullptr, &b1);
  D3D11_BUFFER_DESC cd{};
  cd.ByteWidth = 1024;
  cd.Usage = D3D11_USAGE_DEFAULT;
  cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  dev->CreateBuffer(&cd, nullptr, &cbA);
  dev->CreateBuffer(&cd, nullptr, &cbB);
  D3D11_SHADER_RESOURCE_VIEW_DESC srd{};
  srd.Format = DXGI_FORMAT_R32_TYPELESS;
  srd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  srd.BufferEx.NumElements = 1024;
  srd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  ID3D11ShaderResourceView* dcsSrv = nullptr;
  dev->CreateShaderResourceView(b0, &srd, &dcsSrv);
  D3D11_UNORDERED_ACCESS_VIEW_DESC uad{};
  uad.Format = DXGI_FORMAT_R32_TYPELESS;
  uad.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uad.Buffer.NumElements = 1024;
  uad.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  ID3D11UnorderedAccessView* dcsUav = nullptr;
  dev->CreateUnorderedAccessView(b1, &uad, &dcsUav);
  D3D11_TEXTURE2D_DESC rd{};
  rd.Width = 1024;
  rd.Height = 1000;
  rd.MipLevels = rd.ArraySize = 1;
  rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  rd.SampleDesc.Count = 1;
  rd.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D* rtTex = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  dev->CreateTexture2D(&rd, nullptr, &rtTex);
  if (rtTex) dev->CreateRenderTargetView(rtTex, nullptr, &rtv);
  made = made && dcsCs && dcsSrv && dcsUav && rtv && cbA && cbB;
  Check(made, "fixdepth: synthetic views (R32G8X24 array slice, R24G8, R32) and DCS-like state created");
  if (!made) return;

  // Module set up as Install does, minus the execute hook (no GraphicsCore).
  wchar_t gname[64], dname[64];
  swprintf_s(gname, L"Local\\FdTestGaze_%lu", GetCurrentProcessId());
  swprintf_s(dname, L"Local\\FdTestDepth_%lu", GetCurrentProcessId());
  g_gazeName = gname;
  g_depthName = dname;
  wchar_t grname[64];
  swprintf_s(grname, L"Local\\FdTestGrid_%lu", GetCurrentProcessId());
  g_gridName = grname;
  g_coneOn = true;
  g_gridOn = true;
  HANDLE gm = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, gname);
  void* gview = gm ? MapViewOfFile(gm, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GazeOut)) : nullptr;
  ID3D11Device* oldDev = shadowinst::g_device;
  shadowinst::g_device = dev;
  ptiming::g_topTid = GetCurrentThreadId();
  g_fault = false;
  g_workerStop = false;
  g_targetK = 0;
  g_worker = std::thread(WorkerLoop);
  OpenMappings();
  g_installed = true;
  Check(gview && g_gazeView.load() && g_depthView.load() && g_gridView.load() && !g_gazeOwned,
        "fixdepth: mappings opened (GazeOut read-only, DepthIn, DepthGrid)");

  FakePass casc = MakePass("CascadeShadowPass0"), lights = MakePass("LightsBuffersPass"),
           gbuf = MakePass("GBufferPass"), other = MakePass("DeferredLightingPass"),
           hmdPass = MakePass("CopyToHMDPass"), seaPass = MakePass("WaterSeaPass");
  GazeOut g{};
  g.flags = kGazeValid;
  g.viewCount = 4;
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 4; ++k) g.fovTan[i][k] = static_cast<float>(fovs[i][k]);
  int64_t dt = 1000;
  bool stateSame = true, sampledOnce = false;
  int stateChecks = 0;
  auto setDcsState = [&](int i) {
    c->CSSetShader(dcsCs, nullptr, 0);
    c->CSSetShaderResources(0, 1, &dcsSrv);
    const UINT cnt = 7;
    c->CSSetUnorderedAccessViews(0, 1, &dcsUav, &cnt);
    ID3D11Buffer* cbs[2] = {cbA, cbB};
    UINT first[2] = {0, 16}, num[2] = {16, 32};
    c->CSSetConstantBuffers1(0, 2, cbs, first, num);
    ID3D11RenderTargetView* rt = i >= 2 ? rtv : nullptr;  // the RTV is focus-sized
    c->OMSetRenderTargets(1, &rt, v[i].dsv);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(v[i].w), static_cast<float>(v[i].h), 0, 1};
    c->RSSetViewports(1, &vp);
    if (v[i].cbNum) {
      c->VSSetConstantBuffers1(7, 1, &v[i].cb, &v[i].cbFirst, &v[i].cbNum);
    } else {
      c->VSSetConstantBuffers(7, 1, &v[i].cb);
    }
  };
  // DCS's default frame buffer, bound between passes (as measured live).
  View def;
  def.w = 64;
  def.h = 64;
  MakeProj(def.P, -1, 1, 1, -1, 0.05, 0, true);
  Check(MakeDepth(dev, c, def, DXGI_FORMAT_R32_TYPELESS, 1, 0), "fixdepth: default frame buffer created");
  auto bindDefault = [&] {
    ID3D11RenderTargetView* none = nullptr;
    c->OMSetRenderTargets(1, &none, def.dsv);
    D3D11_VIEWPORT vp{0, 0, 64, 64, 0, 1};
    c->RSSetViewports(1, &vp);
  };
  bool hmdFrames = false;
  float waterDepth = -1;  // >= 0: a WaterSeaPass writes this depth and the WATER stencil over the focus views
  // Every boundary call: DCS's bindings must be the same before and after
  // whenever our dispatch ran inside it.
  auto ob = [&](FakePass* p, bool begin) {
    const uint64_t d0 = g_dispatches.load() + g_gridDispatches.load();
    const Snap before = TakeSnap(c);
    OnBoundary(p, begin, 0);
    if (g_dispatches.load() + g_gridDispatches.load() != d0) {
      sampledOnce = true;
      ++stateChecks;
      if (!(before == TakeSnap(c))) stateSame = false;
    }
  };
  auto frame = [&](double gx, double gy, int order[4]) {
    g.displayTime = ++dt;
    for (int vv : {0, 2}) {  // the same tangents in the left periphery and focus frames (test simplification)
      g.gazeTan[vv][0] = static_cast<float>(gx);
      g.gazeTan[vv][1] = static_cast<float>(gy);
    }
    SeqWrite(gview, g);
    ob(&casc, true);
    ob(&casc, false);
    for (int k = 0; k < 4; ++k) {
      const int i = order[k];
      ob(&lights, true);
      ob(&lights, false);
      bindDefault();
      for (int pass = 0; pass < 3; ++pass) {  // cockpit, main, decal
        ob(&gbuf, true);
        if (pass == 0) bindDefault();  // the cockpit pass binds the default one first
        setDcsState(i);                // pushFrameBuffer: the view's DSV and viewport, b7
        // popFrameBuffer: the default frame buffer again; the R24G8 view
        // keeps its own (the fallback and the OM-conflict path).
        if (i != 3) bindDefault();
        ob(&gbuf, false);
      }
      ob(&other, true);
      ob(&other, false);
      if (hmdFrames && waterDepth >= 0) {
        ob(&seaPass, true);
        if (i >= 2 || i == 0) {  // water over the focus views and the left periphery: depth and stencil WATER
          ID3D11RenderTargetView* none = nullptr;
          c->OMSetRenderTargets(1, &none, v[i].dsv);
          c->ClearDepthStencilView(v[i].dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, waterDepth,
                                   static_cast<UINT8>(kStencilWater << 3));
          bindDefault();
        }
        ob(&seaPass, false);
      }
      if (hmdFrames) {
        ob(&hmdPass, true);
        ob(&hmdPass, false);
      }
    }
    // DCS presents every frame, and frames take time: the GPU has finished the frame's copy before the next frame
    // maps it. R60: waited for (event query, 30 s deadline) instead of a 4 ms sleep, which WARP on a loaded CPU
    // overran (the copy still drawing at the DO_NOT_WAIT map, the ring full: a group's result published late).
    gpttest::WaitGpuIdle(dev, c);
  };
  auto latest = [&](DepthIn& d) {
    // R60: until the worker has finished every job queued so far (a deadline, not a spin count), not a fixed 20 ms
    // after the queue emptied: a worker preempted on a loaded machine published after the read.
    for (const ULONGLONG until = GetTickCount64() + 30000;
         g_jobsDone.load(std::memory_order_acquire) < g_jobsQueued.load(std::memory_order_acquire) &&
         GetTickCount64() < until;)
      Sleep(1);
    return SeqRead(g_depthView.load(), d);
  };

  std::vector<uint8_t> depthBefore = ReadTex(dev, c, v[2].tex), depthBefore3 = ReadTex(dev, c, v[3].tex);
  int order[4] = {0, 1, 2, 3};
  DepthIn d{};
  for (int f = 0; f < 6; ++f) frame(0.0, -0.45, order);
  Check(latest(d) && (d.flags & kDepthValid) && d.view == 2 && (d.flags & kDepthFocus) && (d.flags & kDepthReverseZ),
        "fixdepth: pipeline publishes valid, view 2 (left focus), focus and reverse-Z flags");
  printf("     near: flags %u near %.3f far %.3f median %.3f nearest %.3f latency %.1f; dispatches %llu, map late %llu, "
         "ring full %llu, published %llu (valid %llu)\n",
         d.flags, d.nearShare, d.farShare, d.medianM, d.nearestM, d.latencyFrames,
         static_cast<unsigned long long>(g_dispatches.load()), static_cast<unsigned long long>(g_mapLate.load()),
         static_cast<unsigned long long>(g_ringFull.load()), static_cast<unsigned long long>(g_published.load()),
         static_cast<unsigned long long>(g_publishedValid.load()));
  Check(d.nearShare == 1 && Near(d.medianM, 0.8, 0.01) && d.coneDeg == 1.0f && d.latencyFrames >= 2 &&
            d.displayTime > 1000,
        "fixdepth: panel gaze -> near 1, median 0.8 m (R32G8X24 array slice 1), latency >= 2 frames");
  for (int f = 0; f < 4; ++f) frame(0.05, 0.1, order);
  Check(latest(d) && (d.flags & kDepthValid) && d.farShare == 1 && Near(d.farMedianM, 200, 2.0),
        "fixdepth: terrain gaze -> far 1, far median 200 m");
  for (int f = 0; f < 4; ++f) frame(0.0, -0.2, order);
  Check(latest(d) && (d.flags & kDepthValid) && d.nearShare > 0.3 && d.farShare > 0.3 && d.edgeNearShare > 0.3 &&
            d.edgeFarShare > 0.3,
        "fixdepth: edge gaze -> mixed cone and ring");
  for (int f = 0; f < 4; ++f) frame(0.0, 0.55, order);
  Check(latest(d) && (d.flags & kDepthValid) && d.farShare == 1 && d.medianM == 20000 && d.histogram[15] > 0,
        "fixdepth: sky gaze -> far, 20 km");
  Check(sampledOnce && stateChecks >= 10 && stateSame,
        "fixdepth: DCS's CS shader/SRV/UAV/CB(+offsets), OM targets, VS b7, viewport identical across the dispatch");
  Check(ReadTex(dev, c, v[2].tex) == depthBefore && ReadTex(dev, c, v[3].tex) == depthBefore3,
        "fixdepth: depth targets bit-identical after sampling");

  // Eye retarget: the first large run renders the right eye.
  int swapped[4] = {0, 1, 3, 2};
  const uint64_t rt0 = g_retarget.load(), mm0 = g_mismatch.load();
  for (int f = 0; f < 12; ++f) frame(0.0, -0.45, swapped);
  Check(g_mismatch.load() >= mm0 + 3 && g_retarget.load() == rt0 + 1 && g_targetK.load() == 1,
        "fixdepth: a run matching GazeOut view 3 is rejected; after 3 the target moves to the other eye's run");
  Check(latest(d) && (d.flags & kDepthValid) && d.view == 2 && d.nearShare == 1 && Near(d.medianM, 0.8, 0.01),
        "fixdepth: after the retarget the left focus (second large run) is sampled, valid again");

  Check(g_omUnbinds.load() > 0, "fixdepth: a DSV still bound on the sampled texture is unbound and put back");

  // Verify mode: every run in turn; the view map and per-run medians.
  g_verify = 1;
  for (auto& r : g_rank) r = RankInfo{};
  for (int f = 0; f < 16; ++f) frame(0.0, -0.45, swapped);
  g_verify = 0;
  DepthIn dv{};
  latest(dv);
  {
    std::lock_guard<std::mutex> lock(g_statMutex);
    const RankInfo* r = g_rank;
    printf("     verify: ranks -> views %d %d %d %d, camera x %.3f %.3f %.3f %.3f, medians %.3f %.3f %.3f %.3f\n",
           r[0].best, r[1].best, r[2].best, r[3].best, r[0].camX, r[1].camX, r[2].camX, r[3].camX, r[0].median,
           r[1].median, r[2].median, r[3].median);
    Check(r[0].best == 0 && r[1].best == 1 && r[2].best == 3 && r[3].best == 2 && r[0].w == 160 && r[2].w == 1024,
          "fixdepth verify: run ranks map to GazeOut views 0, 1, 3, 2 by FOV");
    Check(r[2].camX > 0 && r[3].camX < 0 && r[0].camX < 0 && r[1].camX > 0,
          "fixdepth verify: camera x (gViewInv) tells left from right");
    Check(Near(r[2].median, 0.8, 0.01) && Near(r[3].median, 0.8, 0.01) && Near(r[0].median, 0.8, 0.02),
          "fixdepth verify: R24G8 (focus R, periphery L) and R32G8X24 array targets read 0.8 m on the panel");
  }
  g_targetK = 1;

  // HMD mode (each view ends with a CopyToHMDPass, as in DCS): the target is
  // HMD view 2 whatever the sizes; verify maps HMD views 0-3 to GazeOut 0-3.
  hmdFrames = true;
  g_hmdOffset = 0;
  for (int f = 0; f < 6; ++f) frame(0.0, 0.1, order);
  Check(latest(d) && (d.flags & kDepthValid) && d.view == 2 && d.farShare == 1 && Near(d.farMedianM, 200, 2.0),
        "fixdepth: HMD mode samples HMD view 2 (first run after the second CopyToHMDPass)");
  Check(!(d.flags & (kDepthWater | kDepthWaterPass | kDepthPeriphery)) && d.coneDeg == 1.0f,
        "fixdepth: no water pass: bit4 clear, no water, focus (no periphery bit), cone 1°");

  // Depth grid of the left periphery (HMD view 0) against a CPU reference
  // over the same synthetic scene (SceneRange per texel: panel, terrain, sky).
  {
    DepthGrid gr{};
    const bool got = SeqRead(g_gridView.load(), gr);
    const Proj p0 = ParseProj(v[0].P);
    const Viewport vp0{0, 0, static_cast<double>(v[0].w), static_cast<double>(v[0].h), 0, 1};
    int bad = 0, cells = 0, skyCells = 0, panelCells = 0, terrainCells = 0;
    double worstN = 0, worstT = 0;
    for (int gy = 0; gy < kGridN; ++gy)
      for (int gx = 0; gx < kGridN; ++gx) {
        const int x0 = static_cast<int>(std::floor(gx * v[0].w / 32.0)), x1 = static_cast<int>(std::floor((gx + 1) * v[0].w / 32.0));
        const int y0 = static_cast<int>(std::floor(gy * v[0].h / 32.0)), y1 = static_cast<int>(std::floor((gy + 1) * v[0].h / 32.0));
        double mn = 1e30, sl = 0;
        int n = 0, nn = 0;
        for (int y = y0; y < y1; ++y)
          for (int x = x0; x < x1; ++x) {
            double tx, ty;
            TexelToTan(p0, vp0, x, y, tx, ty);
            double r = SceneRange(tx, ty);
            if (!(r < 20000)) r = 20000;
            mn = std::min(mn, r);
            sl += std::log(std::max(r, 0.01));
            nn += r < 1.5;
            ++n;
          }
        const int ci = gy * kGridN + gx;
        if (!n) {
          bad += gr.nearestM[ci] != 0;
          continue;
        }
        ++cells;
        const double typ = std::exp(sl / n);
        const int share = static_cast<int>(std::floor(255.0 * nn / n + 0.5));
        const double eN = std::fabs(gr.nearestM[ci] - mn) / mn, eT = std::fabs(gr.typicalM[ci] - typ) / typ;
        worstN = std::max(worstN, eN);
        worstT = std::max(worstT, eT);
        if (eN > 0.01 || eT > 0.01 || std::abs(gr.nearShare[ci] - share) > 1) ++bad;
        skyCells += mn == 20000;
        panelCells += gr.nearShare[ci] == 255;
        terrainCells += Near(gr.typicalM[ci], 200, 2);
      }
    printf("     grid: %d cells, %d off (worst nearest %.4f, typical %.4f), sky %d, panel %d, terrain %d, flags 0x%x, "
           "fov %.3f %.3f %.3f %.3f\n",
           cells, bad, worstN, worstT, skyCells, panelCells, terrainCells, gr.flags, gr.fovTan[0], gr.fovTan[1],
           gr.fovTan[2], gr.fovTan[3]);
    Check(got && cells > 0 && bad == 0 && skyCells > 0 && panelCells > 0 && terrainCells > 0,
          "fixdepth grid: nearest, typical (log-mean) and near share per cell match the CPU reference (sky, panel, terrain)");
    Check((gr.flags & kDepthValid) && (gr.flags & kDepthReverseZ) && !(gr.flags & (kDepthWater | kDepthWaterPass)) &&
              gr.view == 0 && gr.w == 32 && gr.h == 32 && Near(gr.fovTan[0], -1.4, 1e-4) && Near(gr.fovTan[1], 1.1, 1e-4) &&
              Near(gr.fovTan[2], 1.2, 1e-4) && Near(gr.fovTan[3], -1.3, 1e-4) && gr.displayTime > 1000,
          "fixdepth grid: flags, view 0, 32 x 32, DCS's FOV tangents of the periphery, displayTime");
  }

  // Periphery fallback: the cone around (0, 0.59) is not inside the left
  // focus view (up 0.6): the left periphery (view 0) is sampled, cone widened.
  const uint64_t pf0 = g_periphFrames.load();
  for (int f = 0; f < 6; ++f) frame(0.0, 0.59, order);
  Check(latest(d) && (d.flags & kDepthValid) && (d.flags & kDepthPeriphery) && !(d.flags & kDepthFocus) &&
            d.view == 0 && g_periphFrames.load() > pf0,
        "fixdepth: gaze cone outside the focus view -> periphery view 0 sampled, bit5 set, bit2 clear");
  printf("     periphery: cone %.2f deg, near %.2f far %.2f median %.1f\n", d.coneDeg, d.nearShare, d.farShare,
         d.medianM);
  Check(d.coneDeg > 1.5f && d.farShare == 1 && d.medianM == 20000,
        "fixdepth: periphery cone widened to its texel density (>= 40 texels), sky seen");
  for (int f = 0; f < 6; ++f) frame(0.0, 0.3, order);
  Check(latest(d) && (d.flags & kDepthFocus) && !(d.flags & kDepthPeriphery) && d.view == 2,
        "fixdepth: gaze back inside the focus view -> focus again");

  // Water: a WaterSeaPass writes water at 12 m (forward) over the focus views:
  // sampled after it, flags water present and water pass seen.
  {
    float P[16];
    memcpy(P, v[2].P, sizeof(P));
    waterDepth = static_cast<float>(ProjDepth(P, 0, 0, 12.0));
  }
  for (int f = 0; f < 6; ++f) frame(0.0, -0.45, order);
  latest(d);
  printf("     water: flags 0x%x view %u near %.2f far %.2f median %.2f\n", d.flags, d.view, d.nearShare, d.farShare,
         d.medianM);
  Check(latest(d) && (d.flags & kDepthValid) && (d.flags & kDepthWater) && (d.flags & kDepthWaterPass) &&
            d.farShare == 1 && Near(d.medianM, 12 * std::sqrt(1 + 0.45 * 0.45), 0.1),
        "fixdepth: depth read after WaterSeaPass: water at 12 m forward (13.2 m along the gaze) replaces the panel, bits 3 and 4 set");
  {
    DepthGrid gr{};
    SeqRead(g_gridView.load(), gr);
    float mn = 1e30f, mx = 0;
    for (int c2 = 0; c2 < kGridN * kGridN; ++c2)
      if (gr.nearestM[c2] > 0) mn = std::min(mn, gr.nearestM[c2]), mx = std::max(mx, gr.typicalM[c2]);
    printf("     grid on water: flags 0x%x nearest %.2f .. typical max %.1f\n", gr.flags, mn, mx);
    Check((gr.flags & kDepthValid) && (gr.flags & kDepthWater) && (gr.flags & kDepthWaterPass) && Near(mn, 12, 0.1) &&
              mx > 12 && mx < 20000,
          "fixdepth grid: after the water pass the whole grid is water (12 m forward), bits 3 and 4 set");
  }
  waterDepth = -1;
  // The water pass changed the depth targets: put the scene back.
  for (int i : {0, 2, 3}) {
    Rel(v[i].dsv), Rel(v[i].tex);
    MakeDepth(dev, c, v[i], i == 2 ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS, i == 2 ? 2 : 1,
              i == 2 ? 1 : 0);
  }
  g_verify = 1;
  for (auto& r : g_rank) r = RankInfo{};
  for (int f = 0; f < 12; ++f) frame(0.0, -0.45, order);
  g_verify = 0;
  latest(dv);
  {
    std::lock_guard<std::mutex> lock(g_statMutex);
    const RankInfo* r = g_rank;
    Check(r[0].best == 0 && r[1].best == 1 && r[2].best == 2 && r[3].best == 3,
          "fixdepth verify (HMD mode): HMD views 0-3 match GazeOut views 0-3");
  }
  Check(g_stateChecks.load() > 0 && g_stateDiffs.load() == 0, "fixdepth verify: live state check, no difference");
  hmdFrames = false;

  // No gaze (stale): frames publish valid = 0.
  for (int f = 0; f < 40; ++f) {
    OnBoundary(&casc, true, 0);
    OnBoundary(&casc, false, 0);
    for (int k = 0; k < 4; ++k) {
      setDcsState(swapped[k]);
      OnBoundary(&gbuf, true, 0);
      OnBoundary(&gbuf, false, 0);
      OnBoundary(&other, true, 0);
      OnBoundary(&other, false, 0);
    }
    gpttest::WaitGpuIdle(dev, c);  // R60: as frame()
  }
  Check(latest(d) && !(d.flags & kDepthValid) && g_staleGaze.load() > 0, "fixdepth: a stale GazeOut publishes valid = 0");

  {
    // R48: an install from the config thread in the middle of a frame (Depth boost off and on again while flying)
    // with the previous install's counts still there (here: 1 HMD view ended, 4 last frame). Until the next frame
    // start no pass may be taken as HMD view n (cockpit ipd's guard: lastHmdCount >= 2 and hmdOrd <= 3), as after
    // the first install.
    g_s.hmdOrd = 1;
    g_s.lastHmdCount = 4;
    g_cpSynced = true;
#ifndef R48_PREFIX
    UnsyncViewCount();  // what Install does now
#else
    g_cpSynced = false;  // what Install did before R48
#endif
    const bool partialOff = g_s.lastHmdCount < 2;
    ++g_s.hmdOrd;  // a CopyToHMDPass end in the partial frame
    EndFrame();
    const bool synced = g_s.hmdOrd == 0 && g_cpSynced;
    g_s.hmdOrd = 4;  // a whole frame: four CopyToHMDPass ends
    EndFrame();
    Check(partialOff && synced && g_s.lastHmdCount == 4,
          "R48 fixdepth: after a mid-frame install no view index until a frame start; a whole frame counts again");
  }
  // R58: a tone-map draw in flight (ToneDrawPre ran, DrawPost to come) while the config thread uninstalls: the view
  // its DrawPost restores must survive until the context hooks drained (it was cleared before the drain).
  bool drawViewKept = false;
  {
    g_cpDrawView = 2;
    std::atomic<bool> inHook{false};
    std::thread draw([&] {
      // R60: the uninstall's drain is bounded (200 x Sleep(1)); this draw must see g_rearmOk drop and read within
      // it. At normal priority with Sleep(0) a loaded machine could keep it off the CPU past the drain (the view
      // then read as -1). Above normal and spinning, it reacts at once; the deadline only guards a hang.
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
      HookBusy busy;
      inHook = true;
      const ULONGLONG until = GetTickCount64() + 60000;
      while (g_rearmOk.load() && GetTickCount64() < until) YieldProcessor();  // the uninstall reached the drain
      drawViewKept = *static_cast<volatile int*>(&g_cpDrawView) == 2;  // what this draw's DrawPost would read
    });
    while (!inHook.load()) Sleep(0);
    // Hot-reload safe teardown.
    Uninstall();
    draw.join();
  }
  Check(drawViewKept && g_cpDrawView == -1,
        "R58 fixdepth: an in-flight tone-map draw keeps its view for the restore until the uninstall drained the hooks");
  Check(!g_installed.load() && !g_depthView.load() && !g_gazeView.load() && !g_gridView.load() && !g_s.dev &&
            !g_worker.joinable() &&
            !ptiming::g_depthBoundary.load(),
        "fixdepth: uninstall releases D3D objects, joins the worker, closes the mappings");
  const ULONG refs = (dev->AddRef(), dev->Release());
  printf("     device references after uninstall: %lu\n", refs);
  shadowinst::g_device = oldDev;
  for (View& x : v) {
    Rel(x.dsv), Rel(x.tex), Rel(x.cb);
  }
  Rel(def.dsv), Rel(def.tex);
  ID3D11ShaderResourceView* nullSrv = nullptr;
  ID3D11UnorderedAccessView* nullUav = nullptr;
  c->CSSetShaderResources(0, 1, &nullSrv);
  c->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
  c->ClearState();
  Rel(dcsCs), Rel(dcsSrv), Rel(dcsUav), Rel(rtv), Rel(rtTex), Rel(b0), Rel(b1), Rel(cbA), Rel(cbB);
  if (gview) UnmapViewOfFile(gview);
  if (gm) CloseHandle(gm);
  g_gazeName.clear();  // R79: no overrides (the suffixed default names)
  g_depthName.clear();
  g_gridName.clear();
  g_gridOn = false;
  Rel(c), Rel(imm);
  const ULONG last = dev->Release();
  Check(last == 0, "fixdepth: no D3D reference leaked (device released to 0)");
}

void Run() {
  printf("-- fixation_depth.h --\n");
  TestLayoutAndSeq();
  TestProjection();
  TestPlanAndHist();
  TestSummary();
  TestMatch();
  TestFallbackMath();
  TestTestWriter();
  TestPipeline();
}

}  // namespace fdtest
