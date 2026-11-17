// Runtime probe, analysis side. Reads the cases written by probe_run (caseNNN.txt + caseNNN.png) and writes
// results.csv and report.md (in Italian, for the user) into the same folder.
//   probe_analyze <outdir>
#include "common.h"
#include "scene_pattern.h"
#include <algorithm>
#include <array>
#include <functional>

// Thresholds. The ones marked "eye" are proposals until the user's in-headset calibration replaces them.
static const double kPosMaxFocusPx = 1.0, kPosMeanFocusPx = 0.5;        // layer position error
static const double kRingRel = 0.01, kRingRelDark = 0.02;               // eye: corona luminance deviation (>= 10 %, 3 %)
static const double kRingCodes = 1.5;                                   // eye: at 1 %, in 8-bit codes
static const double kPatchCodes = 3.0;                                  // sentinel patches, 8-bit codes
static const double kReprojFocusPx = 1.0;                               // reprojection coherence up to 2 degrees
static const double kEdgeRatio = 1.10;                                  // edge rise width against the composed reference
static const char* kModelNames[6] = {"lineare, non premoltiplicata", "lineare, premoltiplicata", "opaca (alfa ignorata)", "in gamma, non premoltiplicata", "in gamma, premoltiplicata", "premoltiplica in lineare, fonde in gamma"};

struct Blob { double x = 0, y = 0, w = 0; int area = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
static std::vector<Blob> Blobs(const Image& im, int rx0, int rx1, std::function<double(const uint8_t*)> weight) {
  int W = rx1 - rx0, H = im.h; std::vector<float> wt(size_t(W) * H); std::vector<uint8_t> seen(size_t(W) * H, 0);
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) wt[size_t(y) * W + x] = float(weight(im.at(rx0 + x, y)));
  std::vector<Blob> out; std::vector<int> stack;
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
    size_t i = size_t(y) * W + x; if (seen[i] || wt[i] < 0.35f) continue;
    Blob b; b.x0 = b.x1 = x; b.y0 = b.y1 = y; stack.clear(); stack.push_back(int(i)); seen[i] = 1;
    while (!stack.empty()) {
      int k = stack.back(); stack.pop_back(); int kx = k % W, ky = k / W; double w = wt[k];
      b.x += kx * w; b.y += ky * w; b.w += w; ++b.area; b.x0 = std::min(b.x0, kx); b.x1 = std::max(b.x1, kx); b.y0 = std::min(b.y0, ky); b.y1 = std::max(b.y1, ky);
      const int nx[4] = {kx - 1, kx + 1, kx, kx}, ny[4] = {ky, ky, ky - 1, ky + 1};
      for (int n = 0; n < 4; ++n) { if (nx[n] < 0 || ny[n] < 0 || nx[n] >= W || ny[n] >= H) continue; size_t j = size_t(ny[n]) * W + nx[n]; if (!seen[j] && wt[j] >= 0.12f) { seen[j] = 1; stack.push_back(int(j)); } }
    }
    if (b.area >= 6 && b.w > 0) { b.x = b.x / b.w + rx0; b.y /= b.w; b.x0 += rx0; b.x1 += rx0; out.push_back(b); }
  }
  return out;
}
static double GreenW(const uint8_t* p) { return (p[1] - std::max(p[0], p[2])) / 255.0; }
static double RedW(const uint8_t* p) { return (p[0] - std::max(p[1], p[2])) / 255.0; }
static double WhiteW(const uint8_t* p) { return std::min(p[0], std::min(p[1], p[2])) / 255.0; }
static double Median(std::vector<double> v) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }

// Mirror pixel = c0 + c1 * tanX + c2 * tanY, per axis, fitted on the green lattice of layer A.
struct Affine { double ax[3] = {0, 0, 0}, ay[3] = {0, 0, 0}; bool ok = false; double rms = 0; int n = 0;
  void fwd(double tx, double ty, double& px, double& py) const { px = ax[0] + ax[1] * tx + ax[2] * ty; py = ay[0] + ay[1] * tx + ay[2] * ty; }
  void inv(double px, double py, double& tx, double& ty) const { double a = ax[1], b = ax[2], c = ay[1], d = ay[2], det = a * d - b * c, x = px - ax[0], y = py - ay[0]; tx = (d * x - b * y) / det; ty = (-c * x + a * y) / det; } };
static bool Solve3(double m[3][4]) {
  for (int i = 0; i < 3; ++i) { int p = i; for (int r = i + 1; r < 3; ++r) if (fabs(m[r][i]) > fabs(m[p][i])) p = r; if (fabs(m[p][i]) < 1e-12) return false;
    for (int c = 0; c < 4; ++c) std::swap(m[i][c], m[p][c]);
    for (int r = 0; r < 3; ++r) if (r != i) { double f = m[r][i] / m[i][i]; for (int c = i; c < 4; ++c) m[r][c] -= f * m[i][c]; } }
  for (int i = 0; i < 3; ++i) m[i][3] /= m[i][i]; return true;
}
static double NearestSpacing(const std::vector<Blob>& b) {
  std::vector<double> d;
  for (size_t i = 0; i < b.size(); ++i) { double best = 1e9; for (size_t j = 0; j < b.size(); ++j) if (i != j) best = std::min(best, hypot(b[i].x - b[j].x, b[i].y - b[j].y)); if (best < 1e8) d.push_back(best); }
  return Median(d);
}
// Median distance to the nearest whole dot along one axis (the neighbour in the same row, or in the same column).
static double AxisSpacing(const std::vector<Blob>& b, double medianArea, bool vertical) {
  std::vector<double> d;
  for (size_t i = 0; i < b.size(); ++i) { if (b[i].area < 0.6 * medianArea || b[i].area > 1.6 * medianArea) continue; double best = 1e9;
    for (size_t j = 0; j < b.size(); ++j) { if (i == j || b[j].area < 0.6 * medianArea || b[j].area > 1.6 * medianArea) continue; double along = fabs(vertical ? b[i].y - b[j].y : b[i].x - b[j].x), across = fabs(vertical ? b[i].x - b[j].x : b[i].y - b[j].y);
      if (across < 0.25 * along) best = std::min(best, along); }
    if (best < 1e8) d.push_back(best); }
  return Median(d);
}
static Affine FitLattice(const Image& im, int eye, double step) {
  Affine A; int half = im.w / 2, x0 = eye * half, x1 = x0 + half;
  auto green = Blobs(im, x0, x1, GreenW); auto white = Blobs(im, x0, x1, [](const uint8_t* p) { return WhiteW(p) > 0.5 ? WhiteW(p) : 0.0; });
  if (green.size() < 12 || white.empty()) return A;
  double sp = NearestSpacing(green); if (sp < 4) return A;
  Blob anchor = white[0]; for (auto& w : white) if (w.area > anchor.area) anchor = w;
  // Start from the anchor and the measured spacing, then grow: each pass assigns lattice indices with the current fit
  // and refits on a wider neighbourhood, so a small spacing error cannot shift the indices far from the anchor.
  A.ax[0] = anchor.x; A.ax[1] = sp / step; A.ax[2] = 0; A.ay[0] = anchor.y; A.ay[1] = 0; A.ay[2] = -sp / step;
  std::vector<double> areas; for (auto& g : green) areas.push_back(g.area); double ma = Median(areas);
  std::vector<std::array<double, 4>> pts; bool seedPerAxis = false;
  for (double reach : {4.0, 8.0, 16.0, 1e9}) {
    double sx[3][4] = {}, sy[3][4] = {}; pts.clear();
    for (auto& g : green) {
      if (g.area < 0.6 * ma || g.area > 1.6 * ma || hypot(g.x - anchor.x, g.y - anchor.y) > reach * sp) continue;
      double tx, ty; A.inv(g.x, g.y, tx, ty); tx = floor(tx / step + 0.5) * step; ty = floor(ty / step + 0.5) * step;
      double px, py; A.fwd(tx, ty, px, py); if (hypot(px - g.x, py - g.y) > 0.35 * sp) continue;
      double v[3] = {1, tx, ty};
      for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) { sx[r][c] += v[r] * v[c]; sy[r][c] += v[r] * v[c]; } sx[r][3] += v[r] * g.x; sy[r][3] += v[r] * g.y; }
      pts.push_back({tx, ty, g.x, g.y});
    }
    // Quad chain: the focus image is opaque, so around the anchor there are no green dots at all (the nearest whole
    // ones are 8 to 12 steps away, past the fade). A pass that finds too few points is skipped, not fatal: the seed
    // (anchor + measured spacing) is kept for the next, wider one.
    // The seed is then all there is to carry the indices across that hole, and the Pimax undistorted mirror is not
    // isotropic (about 587 px per unit of tangent across, 683 down): the one spacing above, used for both axes, puts
    // the rows 16 % off and the indices wrong after three steps. So the seed is redone with one spacing per axis.
    if (pts.size() < 8) {
      if (reach >= 1e8) return A;
      if (!seedPerAxis) { seedPerAxis = true; double sxp = AxisSpacing(green, ma, false), syp = AxisSpacing(green, ma, true);
        if (sxp >= 4 && syp >= 4) { A.ax[0] = anchor.x; A.ax[1] = sxp / step; A.ax[2] = 0; A.ay[0] = anchor.y; A.ay[1] = 0; A.ay[2] = -syp / step; } }
      continue;
    }
    if (!Solve3(sx) || !Solve3(sy)) return A;
    for (int i = 0; i < 3; ++i) { A.ax[i] = sx[i][3]; A.ay[i] = sy[i][3]; }
  }
  double s2 = 0; for (auto& p : pts) { double px, py; A.fwd(p[0], p[1], px, py); s2 += (px - p[2]) * (px - p[2]) + (py - p[3]) * (py - p[3]); }
  A.n = int(pts.size()); A.rms = sqrt(s2 / pts.size()); A.ok = A.rms < sp * 0.1; return A;
}
static Fov GetFov(const Case& c, const char* name, int e) { std::string p = std::string(name) + std::to_string(e); return {Num(c, (p + "l").c_str()), Num(c, (p + "r").c_str()), Num(c, (p + "u").c_str()), Num(c, (p + "d").c_str())}; }
static double Lum(const uint8_t* p) { return 0.2126 * Dec(p[0]) + 0.7152 * Dec(p[1]) + 0.0722 * Dec(p[2]); }

struct Row { std::string id, kind, tag, variant, mirror; std::map<std::string, double> v; std::string note; bool quad = false, scene = false; };
// chain=quad: the probe submitted four views and the layers under it made the composition (and the fade).
static bool IsQuad(const Case& c) { return Str(c, "chain") == "quad"; }
static std::vector<Row> g_rows;
static std::string VariantName(const Case& c) { int p = int(Num(c, "premult"));
  if (IsQuad(c)) return std::string("catena a quattro viste, ") + (Num(c, "format") == 29 ? "sRGB" : "UNORM"); return std::string(Num(c, "format") == 29 ? "sRGB" : "UNORM") + (Num(c, "array") ? " array" : " separati") + (p == 1 ? ", premolt. lineare" : (p == 2 ? ", premolt. in gamma" : ", non premolt.")); }

static Affine g_cal[2]; // mirror geometry from the first centre lattice (the undistorted mirror does not move)

// Position of layer B's red dots against where layer A's lattice says they should be (focus pixels).
static void DoLattice(const Case& c, const Image& im, Row& r) {
  double step = Num(c, "step"), sumAbs = 0, maxAbs = 0, mx = 0, my = 0, floorSum = 0; int n = 0; bool quad = IsQuad(c);
  for (int e = 0; e < 2; ++e) {
    Affine A = FitLattice(im, e, step); if (!A.ok) { r.note += "calibrazione fallita occhio " + std::to_string(e) + "; "; continue; }
    Fov fb = GetFov(c, "fovB", e); double scale = (Num(c, "bw") / (fb.r - fb.l)) / fabs(A.ax[1]);
    floorSum += A.rms * scale;
    int half = im.w / 2; auto red = Blobs(im, e * half, (e + 1) * half, RedW); std::vector<double> areas; for (auto& b : red) areas.push_back(b.area); double ma = Median(areas);
    for (auto& b : red) {
      if (b.area < 0.75 * ma || b.area > 1.4 * ma) continue;
      double tx, ty; A.inv(b.x, b.y, tx, ty);
      double ex = (floor(tx / step - 0.5 + 0.5) + 0.5) * step, ey = (floor(ty / step - 0.5 + 0.5) + 0.5) * step;
      if (ex - 0.012 < fb.l || ex + 0.012 > fb.r || ey + 0.012 > fb.u || ey - 0.012 < fb.d) continue;
      // Quad chain: the focus image is opaque and the layer fades it, so only the dots in the part the fade leaves
      // whole are measured (the dot's real place, from the mirror's geometry: the lattice may be a moving one).
      if (quad && g_cal[e].ok) { double qx, qy; g_cal[e].inv(b.x, b.y, qx, qy); if (FocusRadius((qx - (fb.l + fb.r) / 2) / ((fb.r - fb.l) / 2), (qy - (fb.u + fb.d) / 2) / ((fb.u - fb.d) / 2)) > 1 - 2 * kSmoothing - 0.05) continue; }
      // The mirror's rows are not as far apart as its columns. The quad chain uses the vertical scale for dy; the
      // direct mode keeps the horizontal one for both, as its earlier results did (its dy reads 16 % too large).
      double scaleY = quad ? (Num(c, "bh") / (fb.u - fb.d)) / fabs(A.ay[2]) : scale;
      double px, py; A.fwd(ex, ey, px, py); double dx = (b.x - px) * scale, dy = (b.y - py) * scaleY, d = hypot(dx, dy);
      sumAbs += d; maxAbs = std::max(maxAbs, d); mx += dx; my += dy; ++n;
    }
  }
  if (n) { r.v["pos_mean"] = sumAbs / n; r.v["pos_max"] = maxAbs; r.v["pos_dx"] = mx / n; r.v["pos_dy"] = my / n; r.v["pos_n"] = n; r.v["pos_floor"] = floorSum / 2; }
  else r.note += "nessun punto rosso misurabile; ";
}

// Motion (quad chain, --motion): the lattice moves right by step / phases per submitted frame. The fit of the green
// lattice gives where its origin is in this image; against the mirror's geometry that is the content's shift, so its
// phase in application frames. lag = frames between the last frame submitted and what the image shows; a real frame has
// a whole lag, a frame interpolated at 2X a lag ending in .5 (at 3X in .33 or .67). The distance from the nearest of
// those is the position error on the expected trajectory. The red dots against the green ones (DoLattice) are the seam.
static void DoMotion(const Case& c, const Image& im, Row& r) {
  const double kTau = 6.283185307179586; double step = Num(c, "step"), M = Num(c, "phases"), sc = 0, ss = 0, sy = 0, res = 0; int n = 0;
  if (M < 1 || step <= 0) { r.note += "caso di movimento senza passo o fasi; "; return; }
  for (int e = 0; e < 2; ++e) {
    if (!g_cal[e].ok) continue;
    Affine A = FitLattice(im, e, step); if (!A.ok) { r.note += "reticolo non leggibile occhio " + std::to_string(e) + "; "; continue; }
    double tx, ty; g_cal[e].inv(A.ax[0], A.ay[0], tx, ty); double p = tx / step; p -= floor(p);
    sc += cos(kTau * p); ss += sin(kTau * p); ty /= step; ty -= floor(ty + 0.5); sy += ty * step; res += A.rms; ++n;
    r.v[e ? "phase_r" : "phase_l"] = p * M;
  }
  if (!n) { if (!g_cal[0].ok && !g_cal[1].ok) r.note += "manca la calibrazione dal reticolo fermo; "; return; }
  double phase = atan2(ss, sc) / kTau; phase -= floor(phase); phase *= M;
  double lag = fmod(Num(c, "submitted") - phase, M); if (lag < 0) lag += M; double frac = lag - floor(lag);
  auto nearest = [&](int parts) { double best = 1; for (int k = 0; k <= parts; ++k) best = std::min(best, fabs(frac - double(k) / parts)); return best; };
  Fov fb = GetFov(c, "fovB", 0); double focusPerTan = Num(c, "bw") / (fb.r - fb.l), framePx = Num(c, "vtan") * focusPerTan;
  double d1 = nearest(1), d2 = nearest(2), d3 = nearest(3);
  r.v["phase"] = phase; r.v["lag"] = lag; r.v["frac"] = frac; r.v["frame_px"] = framePx;
  r.v["traj_err_real"] = d1 * framePx; r.v["traj_err_2x"] = d2 * framePx; r.v["traj_err_3x"] = d3 * framePx;
  // 0 a real frame, 2 half way (2X), 3 at a third (3X), 9 none of them (within 0.12 of a frame).
  r.v["class"] = d1 < 0.12 ? 0 : (fabs(frac - 0.5) < 0.12 ? 2 : (d3 < 0.12 ? 3 : 9));
  r.v["shift_y"] = sy / n * focusPerTan; r.v["fit_rms"] = res / n; r.v["seq"] = Num(c, "seq"); r.v["buf"] = Num(c, "buf"); r.v["t_ms"] = Num(c, "t_ms"); r.v["submitted"] = Num(c, "submitted");
  if (n == 2) r.v["phase_lr"] = fabs(r.v["phase_l"] - r.v["phase_r"]) > M / 2 ? M - fabs(r.v["phase_l"] - r.v["phase_r"]) : fabs(r.v["phase_l"] - r.v["phase_r"]);
  DoLattice(c, im, r);
}

// ---- scene pattern (quad chain, --motion-pattern scene; scene_pattern.h) ----
// A textured world that does not repeat, with markers, instead of the lattice; the probe sweeps it there and back
// (frame n shows phase tri(n) = 0 .. phases-1 .. 0 ..). Per image: the markers (coloured discs with G = 0, found with
// max(R, B) - G) are located and matched to where the seed says they are, the median along the motion gives the
// content's phase, the phase gives the type (real, interpolated, off the trajectory) and the nominal place of every
// marker; the error of each marker against that place is reported per region (periphery, fade band, focus interior).
// In the fade band a marker shows red from the focus layer and blue from the periphery layer: the offset between the
// two parts is the seam. A whole-image number (SceneQuality) compares the image with a real one shifted to its phase.
static double MarkW(const uint8_t* p) { return (std::max(p[0], p[2]) - p[1]) / 255.0; }
static std::string SceneGroup(const Case& c) { return Str(c, "seed") + "|" + Str(c, "phases") + "|" + Str(c, "vtan") + "|" + Str(c, "vtan_y"); }
static const ScenePattern& GetScene(const Case& c) {
  static std::map<std::string, ScenePattern> cache; std::string key = Str(c, "seed");
  auto it = cache.find(key); if (it != cache.end()) return it->second;
  ScenePattern& sp = cache[key]; sp.Build(uint32_t(strtoul(key.c_str(), nullptr, 10))); return sp;
}
// 0 periphery, 1 fade band (with a margin on both sides), 2 the part of the focus the fade leaves whole.
static int FocusRegion(const Fov& fb, double tx, double ty, double* alpha = nullptr) {
  double nx = (tx - (fb.l + fb.r) / 2) / ((fb.r - fb.l) / 2), ny = (ty - (fb.u + fb.d) / 2) / ((fb.u - fb.d) / 2);
  if (alpha) *alpha = FocusAlpha(nx, ny);
  double rr = FocusRadius(nx, ny); return rr < 1 - 2 * kSmoothing - 0.05 ? 2 : (rr <= 1.05 ? 1 : 0);
}
static int g_sceneDomain = 0; // how the two layers were blended in the band: 0 in linear light, 1 on encoded values
struct SceneFrame { size_t row; Image im; Case c; std::string group; double phase; bool real; };
static std::vector<SceneFrame> g_sceneFrames;
static const char* kRegionKey[3] = {"per", "band", "foc"};

static void DoScene(const Case& c, const Image& im, Row& r, bool still) {
  const double kPi = 3.14159265358979; double N = Num(c, "phases"), vx = Num(c, "vtan"), vy = Num(c, "vtan_y"), vmag = hypot(vx, vy); int fg = int(Num(c, "fg"));
  if (N < 1 || vmag <= 0) { r.note += "caso di scena senza passo o fasi; "; return; }
  if (!g_cal[0].ok && !g_cal[1].ok) { r.note += "manca la calibrazione dal reticolo fermo; "; return; }
  const ScenePattern& sp = GetScene(c); const double ux = vx / vmag, uy = vy / vmag; int half = im.w / 2; // (ux, uy): along the motion
  static double dec[256]; static bool decInit = false; if (!decInit) { for (int i = 0; i < 256; ++i) dec[i] = SrgbToLinear(i / 255.0); decInit = true; }
  // 1. candidate discs, and how the band was blended: at the middle of a disc that both layers show, red + blue is 1
  //    in linear light for a linear-light blend (the composed path) and 1 on the encoded values for the runtime's.
  std::vector<Blob> cand[2]; int voteLin = 0, voteEnc = 0;
  for (int e = 0; e < 2; ++e) { if (!g_cal[e].ok) continue; const Affine& A = g_cal[e]; Fov fb = GetFov(c, "fovB", e);
    double areaPx = kPi * kMkDisc * kMkDisc * fabs(A.ax[1] * A.ay[2] - A.ax[2] * A.ay[1]);
    for (auto& b : Blobs(im, e * half, (e + 1) * half, MarkW)) { if (b.area < 0.35 * areaPx || b.area > 2.5 * areaPx) continue; cand[e].push_back(b);
      double tx, ty, a; A.inv(b.x, b.y, tx, ty); FocusRegion(fb, tx, ty, &a); int cx = int(b.x + 0.5), cy = int(b.y + 0.5);
      if (a > 0.2 && a < 0.8 && cx > e * half && cx < (e + 1) * half - 1 && cy > 0 && cy < im.h - 1) { double rs = 0, bs = 0; for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) { rs += im.at(cx + i, cy + j)[0]; bs += im.at(cx + i, cy + j)[2]; }
        double sl = SrgbToLinear(rs / 9 / 255) + SrgbToLinear(bs / 9 / 255), se = (rs + bs) / 9 / 255; ++(fabs(sl - 1) < fabs(se - 1) ? voteLin : voteEnc); } } }
  if (voteLin + voteEnc) g_sceneDomain = voteEnc > voteLin ? 1 : 0;
  const int dom = g_sceneDomain;
  // 2. each disc again, pixel by pixel in linear light. The red part divided by the fade's alpha (the model of
  //    common.h) is the focus layer's disc and the blue part divided by 1 - alpha the periphery layer's: the division
  //    is done where the blend was (on the encoded values for the runtime's blend, then decoded). Their weighted sum
  //    is the disc's coverage whatever the alpha is there: its centroid is the marker, its second moments along and
  //    across the motion the elongation (a doubled marker is longer along the motion). In the band the centroids of
  //    the two layers' discs give the seam.
  struct Det { int e; double tx, ty, elong; bool seam; double sx, sy; };
  std::vector<Det> dets;
  for (int e = 0; e < 2; ++e) { if (!g_cal[e].ok) continue; const Affine& A = g_cal[e]; Fov fb = GetFov(c, "fovB", e);
    double det = A.ax[1] * A.ay[2] - A.ax[2] * A.ay[1], i11 = A.ay[2] / det, i12 = -A.ax[2] / det, i21 = -A.ay[1] / det, i22 = A.ax[1] / det;
    double fx = Num(c, "bw") / (fb.r - fb.l), fy = Num(c, "bh") / (fb.u - fb.d), rx = kMkRing * fabs(A.ax[1]) + 2, ry = kMkRing * fabs(A.ay[2]) + 2;
    for (auto& b : cand[e]) {
      double t0x, t0y, ac; A.inv(b.x, b.y, t0x, t0y); FocusRegion(fb, t0x, t0y, &ac); bool band = ac > 0.10 && ac < 0.90;
      int x0 = std::max(e * half, int(b.x - rx)), x1 = std::min((e + 1) * half - 1, int(b.x + rx) + 1), y0 = std::max(0, int(b.y - ry)), y1 = std::min(im.h - 1, int(b.y + ry) + 1);
      double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0, rw = 0, rxs = 0, rys = 0, bw = 0, bxs = 0, bys = 0;
      for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
        double qx = x - b.x, qy = y - b.y, dtx = i11 * qx + i12 * qy, dty = i21 * qx + i22 * qy; if (dtx * dtx + dty * dty > kMkRing * kMkRing) continue;
        const uint8_t* p = im.at(x, y); double a = 0.5, wr, wb, ur, ub; if (band || dom) FocusRegion(fb, t0x + dtx, t0y + dty, &a);
        if (dom && a > 0.02 && a < 0.98) { ur = SrgbToLinear(std::min(1.0, std::max(0, p[0] - p[1]) / 255.0 / a)); ub = SrgbToLinear(std::min(1.0, std::max(0, p[2] - p[1]) / 255.0 / (1 - a))); wr = a * ur; wb = (1 - a) * ub; }
        else { double g = dec[p[1]], ac2 = a < 0.05 ? 0.05 : (a > 0.95 ? 0.95 : a); wr = std::max(0.0, dec[p[0]] - g); wb = std::max(0.0, dec[p[2]] - g); ur = wr / ac2; ub = wb / (1 - ac2); }
        double w = wr + wb; if (w <= 0) continue;
        sw += w; sx += w * qx; sy += w * qy; sxx += w * qx * qx; sxy += w * qx * qy; syy += w * qy * qy;
        if (band) { rw += ur; rxs += ur * qx; rys += ur * qy; bw += ub; bxs += ub * qx; bys += ub * qy; } }
      if (sw <= 0) continue;
      Det d{}; d.e = e; double cx = sx / sw, cy = sy / sw, vxx = sxx / sw - cx * cx, vxy = sxy / sw - cx * cy, vyy = syy / sw - cy * cy;
      A.inv(b.x + cx, b.y + cy, d.tx, d.ty);
      double l1 = ux * i11 + uy * i21, l2 = ux * i12 + uy * i22, m1 = -uy * i11 + ux * i21, m2 = -uy * i12 + ux * i22;
      double vu = l1 * l1 * vxx + 2 * l1 * l2 * vxy + l2 * l2 * vyy, vw = m1 * m1 * vxx + 2 * m1 * m2 * vxy + m2 * m2 * vyy; d.elong = vw > 0 && vu > 0 ? sqrt(vu / vw) : 0;
      if (band && rw > 0 && bw > 0 && std::min(rw, bw) > 0.3 * std::max(rw, bw)) { double px = rxs / rw - bxs / bw, py = rys / rw - bys / bw; d.seam = true; d.sx = (i11 * px + i12 * py) * fx; d.sy = (i21 * px + i22 * py) * fy; }
      dets.push_back(d); } }
  if (dets.size() < 4) { r.note += "marcatori non trovati (" + std::to_string(dets.size()) + "); "; return; }
  // 3. phase. First a vote: every disc against every marker it could be (same line along the motion, a shift
  //    between -1 and phases frames) says a phase.
  int NB = int((N + 2) * 8 + 0.5); std::vector<int> hist(NB, 0);
  for (auto& d : dets) for (auto& m : sp.markers) { double sx = d.tx - m.x, sy = d.ty - m.y; if (fabs(-sx * uy + sy * ux) >= 0.03) continue; double ph = (sx * ux + sy * uy) / vmag + 1; if (ph >= 0 && ph < N + 2) ++hist[std::min(NB - 1, int(ph * 8))]; }
  int bestBin = 0, bestVotes = -1; for (int i = 0; i < NB; ++i) { int v = (i ? hist[i - 1] : 0) + hist[i] + (i + 1 < NB ? hist[i + 1] : 0); if (v > bestVotes) { bestVotes = v; bestBin = i; } }
  // Then each disc to the nearest marker place at that phase (search radius 0.4 of the markers' least distance).
  struct Match { int det, m; double du, dw; };
  int dup = 0;
  auto match = [&](double phi) { std::vector<Match> out; dup = 0;
    for (size_t i = 0; i < dets.size(); ++i) { const Det& d = dets[i]; double best = 1e9; Match bm{};
      for (size_t mi = 0; mi < sp.markers.size(); ++mi) { double sx = d.tx - sp.markers[mi].x - phi * vx, sy = d.ty - sp.markers[mi].y - phi * vy, dist = hypot(sx, sy); if (dist < best) { best = dist; bm = {int(i), int(mi), sx * ux + sy * uy, -sx * uy + sy * ux}; } }
      if (best > 0.4 * kMkSpacing) continue;
      bool placed = false; for (auto& o : out) if (dets[o.det].e == d.e && o.m == bm.m) { ++dup; placed = true; if (best < hypot(o.du, o.dw)) o = bm; break; }
      if (!placed) out.push_back(bm); }
    return out; };
  double phi0 = (bestBin + 0.5) / 8 - 1; std::vector<Match> ms = match(phi0);
  if (ms.size() < 4) { r.note += "marcatori non riconosciuti sulla traiettoria; "; return; }
  { std::vector<double> all, eye[2]; for (auto& m : ms) { all.push_back(m.du); eye[dets[m.det].e].push_back(m.du); }
    for (int e = 0; e < 2; ++e) if (eye[e].size() >= 3) r.v[e ? "phase_r" : "phase_l"] = phi0 + Median(eye[e]) / vmag;
    phi0 += Median(all) / vmag; }
  const double phi = phi0; ms = match(phi);
  if (r.v.count("phase_l") && r.v.count("phase_r")) r.v["phase_lr"] = fabs(r.v["phase_l"] - r.v["phase_r"]);
  // 4. type, and the nominal place. Every submitted frame shows a whole phase, so the fraction of the phase read is
  //    where the image sits between two real frames: 0 real, .5 half way (2X), thirds (3X). The nominal place is the
  //    nearest slot the frame generation in use can show (fg = frames per application frame, from the case; 0 = not
  //    known: whole, half, thirds as in the lattice analysis). delta = phase read - nominal phase.
  Fov fb0 = GetFov(c, "fovB", 0); double framePx = vmag * Num(c, "bw") / (fb0.r - fb0.l), delta = 0; int cls = 0;
  if (still) { delta = phi; r.v["lag"] = 0; r.v["frac"] = 0; }
  else {
    double frac = phi - floor(phi), slot = 0;
    auto nearest = [&](int parts) { double best = 1; for (int k = 0; k <= parts; ++k) best = std::min(best, fabs(frac - double(k) / parts)); return best; };
    double d1 = nearest(1), d2 = nearest(2), d3 = nearest(3);
    if (fg >= 1) { slot = floor(frac * fg + 0.5) / fg; cls = fabs(frac - slot) >= 0.12 ? 9 : (slot == 0 || slot == 1 ? 0 : (fg == 2 ? 2 : 3)); }
    else if (d1 < 0.12) { slot = floor(frac + 0.5); cls = 0; }
    else if (fabs(frac - 0.5) < 0.12) { slot = 0.5; cls = 2; }
    else { slot = frac < 0.5 ? 1.0 / 3 : 2.0 / 3; cls = d3 < 0.12 ? 3 : 9; }
    delta = frac - slot;
    // lag (frames between the last frame submitted and what the image shows): the sweep passes every phase twice per
    // round, going and coming back; the smaller of the two lags is reported. Only for information.
    double T = 2 * (N - 1), sub = Num(c, "submitted"), l1 = T > 0 ? fmod(sub - phi, T) : 0, l2 = T > 0 ? fmod(sub - (T - phi), T) : 0; if (l1 < 0) l1 += T; if (l2 < 0) l2 += T;
    r.v["lag"] = std::min(l1, l2); r.v["frac"] = frac; r.v["traj_err_real"] = d1 * framePx; r.v["traj_err_2x"] = d2 * framePx; r.v["traj_err_3x"] = d3 * framePx;
    r.v["turn"] = phi < 1 || phi > N - 2 ? 1 : 0; // within one frame of where the sweep turns back
    r.v["seq"] = Num(c, "seq"); r.v["buf"] = Num(c, "buf"); r.v["t_ms"] = Num(c, "t_ms"); r.v["submitted"] = sub;
  }
  r.v["phase"] = phi; r.v["class"] = cls; r.v["frame_px"] = framePx; r.v["traj_err"] = fabs(delta) * framePx; r.v["fg"] = fg; r.v["blend"] = dom;
  // 5. the markers this image should show (their nominal places inside the eye's FOV and the mirror, off the borders),
  //    by region; then every matched disc against its nominal place.
  struct Exp { int e, m, region; };
  std::vector<Exp> exps; int nExp[3] = {0, 0, 0}, nFound[3] = {0, 0, 0}; std::vector<double> errs[3], coh, elong, seamAbs, seamX, seamY;
  for (int e = 0; e < 2; ++e) { if (!g_cal[e].ok) continue; Fov fa = GetFov(c, "fovA", e), fb = GetFov(c, "fovB", e);
    for (size_t mi = 0; mi < sp.markers.size(); ++mi) { double tx = sp.markers[mi].x + (phi - delta) * vx, ty = sp.markers[mi].y + (phi - delta) * vy, px, py;
      if (tx < fa.l + 0.03 || tx > fa.r - 0.03 || ty > fa.u - 0.03 || ty < fa.d + 0.03) continue; g_cal[e].fwd(tx, ty, px, py);
      if (px < e * half + 12 || px > (e + 1) * half - 13 || py < 12 || py > im.h - 13) continue;
      int region = FocusRegion(fb, tx, ty); exps.push_back({e, int(mi), region}); ++nExp[region]; } }
  for (auto& m : ms) { const Det& d = dets[m.det]; const Exp* x = nullptr; for (auto& q : exps) if (q.e == d.e && q.m == m.m) { x = &q; break; }
    if (!x) continue; ++nFound[x->region]; Fov fb = GetFov(c, "fovB", d.e); double fx = Num(c, "bw") / (fb.r - fb.l), fy = Num(c, "bh") / (fb.u - fb.d);
    double eu = m.du + delta * vmag; errs[x->region].push_back(hypot((eu * ux - m.dw * uy) * fx, (eu * uy + m.dw * ux) * fy));
    coh.push_back(hypot((m.du * ux - m.dw * uy) * fx, (m.du * uy + m.dw * ux) * fy)); if (d.elong > 0) elong.push_back(d.elong);
    if (d.seam) { seamAbs.push_back(hypot(d.sx, d.sy)); seamX.push_back(d.sx); seamY.push_back(d.sy); } }
  for (int g = 0; g < 3; ++g) { std::string p = std::string("mk_") + kRegionKey[g]; r.v[p + "_exp"] = nExp[g]; r.v[p + "_n"] = nFound[g];
    if (!errs[g].empty()) { r.v[p + "_med"] = Median(errs[g]); r.v[p + "_max"] = *std::max_element(errs[g].begin(), errs[g].end()); } }
  r.v["mk_exp"] = nExp[0] + nExp[1] + nExp[2]; r.v["mk_n"] = nFound[0] + nFound[1] + nFound[2]; r.v["mk_dup"] = dup; r.v["mk_coh"] = Median(coh); r.v["mk_elong"] = Median(elong);
  r.v["seam_n"] = double(seamAbs.size()); if (!seamAbs.empty()) { r.v["seam_med"] = Median(seamAbs); r.v["seam_max"] = *std::max_element(seamAbs.begin(), seamAbs.end()); r.v["seam_dx"] = Median(seamX); r.v["seam_dy"] = Median(seamY); }
  g_sceneFrames.push_back({g_rows.size(), im, c, SceneGroup(c), phi, still || cls == 0});
}

// Whole-image quality of the scene images: each against a reference made from a REAL image of the same run (the one
// nearest in phase, the still case included) shifted by the measured phase difference (Catmull-Rom). Mean absolute
// error in 8-bit codes and PSNR over R, G, B; the fade band (either end of the shift inside it) apart from the rest.
// A real image scored against another real one is the floor of the method (mirror sampling, the shift's filter).
static void SceneQuality() {
  for (auto& f : g_sceneFrames) {
    const SceneFrame* ref = nullptr; double shift = 1e9, vx = Num(f.c, "vtan"), vy = Num(f.c, "vtan_y");
    for (auto& g : g_sceneFrames) { if (&g == &f || !g.real || g.group != f.group || g.im.w != f.im.w || g.im.h != f.im.h) continue; double d = f.phase - g.phase; if (fabs(d) < fabs(shift)) { shift = d; ref = &g; } }
    Row& r = g_rows[f.row]; if (!ref) { r.note += "nessuna immagine reale di riferimento per la qualita; "; continue; }
    const Image& I = f.im; const Image& J = ref->im; int half = I.w / 2, H = I.h; double sum[3] = {0, 0, 0}, sq[3] = {0, 0, 0}, cnt[3] = {0, 0, 0};
    for (int e = 0; e < 2; ++e) { if (!g_cal[e].ok) continue; const Affine& A = g_cal[e]; Fov fa = GetFov(f.c, "fovA", e), fb = GetFov(f.c, "fovB", e);
      std::vector<int8_t> reg(size_t(half) * H);
      SceneRows(H, [&](int y) { for (int x = 0; x < half; ++x) { double tx, ty; A.inv(e * half + x, y, tx, ty); reg[size_t(y) * half + x] = (tx < fa.l + 0.03 || tx > fa.r - 0.03 || ty > fa.u - 0.03 || ty < fa.d + 0.03) ? int8_t(-1) : int8_t(FocusRegion(fb, tx, ty)); } });
      // A world point at p in the reference is at p + s in this image: the reference is read at p - s.
      double sxp = (A.ax[1] * vx + A.ax[2] * vy) * shift, syp = (A.ay[1] * vx + A.ay[2] * vy) * shift;
      int jx = int(floor(-sxp)), jy = int(floor(-syp)), nx = int(floor(-sxp + 0.5)), ny = int(floor(-syp + 0.5)); double ax = -sxp - jx, ay = -syp - jy;
      auto cr = [](double t, double w[4]) { w[0] = ((-t + 2) * t - 1) * t / 2; w[1] = ((3 * t - 5) * t * t + 2) / 2; w[2] = ((-3 * t + 4) * t + 1) * t / 2; w[3] = (t - 1) * t * t / 2; };
      double wx[4], wy[4]; cr(ax, wx); cr(ay, wy);
      std::vector<std::array<double, 9>> acc(H, std::array<double, 9>{});
      SceneRows(H, [&](int y) { int by = y + jy; if (by < 1 || by + 2 >= H || y + ny < 0 || y + ny >= H) return; auto& a = acc[y];
        for (int x = 3; x < half - 3; ++x) { int bx = x + jx; if (bx < 1 || bx + 2 >= half || x + nx < 0 || x + nx >= half) continue;
          int rp = reg[size_t(y) * half + x], rq = reg[size_t(y + ny) * half + x + nx]; if (rp < 0 || rq < 0) continue; int g = (rp == 1 || rq == 1) ? 1 : (rp == rq ? rp : -1); if (g < 0) continue;
          const uint8_t* p = I.at(e * half + x, y);
          for (int ch = 0; ch < 3; ++ch) { double v = 0; for (int j = 0; j < 4; ++j) { const uint8_t* q = J.at(e * half + bx - 1, by - 1 + j); v += wy[j] * (wx[0] * q[ch] + wx[1] * q[4 + ch] + wx[2] * q[8 + ch] + wx[3] * q[12 + ch]); }
            v = v < 0 ? 0 : (v > 255 ? 255 : v); double d = p[ch] - v; a[g] += fabs(d); a[3 + g] += d * d; a[6 + g] += 1; } } });
      for (auto& a : acc) for (int g = 0; g < 3; ++g) { sum[g] += a[g]; sq[g] += a[3 + g]; cnt[g] += a[6 + g]; } }
    auto psnr = [](double s, double n) { double mse = n > 0 ? s / n : 0; return mse > 1e-9 ? std::min(99.0, 10 * log10(255.0 * 255.0 / mse)) : 99.0; };
    for (int g = 0; g < 3; ++g) if (cnt[g] > 0) { r.v[std::string("q_mae_") + kRegionKey[g]] = sum[g] / cnt[g]; r.v[std::string("q_psnr_") + kRegionKey[g]] = psnr(sq[g], cnt[g]); }
    if (cnt[0] + cnt[2] > 0) { r.v["q_mae"] = (sum[0] + sum[2]) / (cnt[0] + cnt[2]); r.v["q_psnr"] = psnr(sq[0] + sq[2], cnt[0] + cnt[2]); }
    r.v["q_ref"] = atof(g_rows[ref->row].id.c_str() + 4); r.v["q_shift"] = shift;
  }
}

// Ring profile of luminance around layer B's fade.
static bool RingProfile(const Case& c, const Image& im, int e, std::vector<double>& mean, double& yIn, double& yOut) {
  if (!g_cal[e].ok) return false;
  Fov fb = GetFov(c, "fovB", e); double cx = (fb.l + fb.r) / 2, cy = (fb.u + fb.d) / 2, hx = (fb.r - fb.l) / 2, hy = (fb.u - fb.d) / 2;
  const int NB = 66; std::vector<double> sum(NB, 0); std::vector<int> cnt(NB, 0); int half = im.w / 2;
  double px0, py0, px1, py1; g_cal[e].fwd(cx - 1.3 * hx, cy + 1.3 * hy, px0, py0); g_cal[e].fwd(cx + 1.3 * hx, cy - 1.3 * hy, px1, py1);
  int xa = std::max(e * half + 2, int(std::min(px0, px1))), xb = std::min((e + 1) * half - 3, int(std::max(px0, px1))), ya = std::max(2, int(std::min(py0, py1))), yb = std::min(im.h - 3, int(std::max(py0, py1)));
  // Quad chain: the frame-generation layer draws its frame-rate overlay (a coloured number and box) over the fade band.
  // The grey cases have no colour of their own, so the box around every coloured pixel is left out of the profile.
  int ox0 = 1 << 30, oy0 = 1 << 30, ox1 = -1, oy1 = -1;
  if (IsQuad(c)) for (int y = ya; y <= yb; ++y) for (int x = xa; x <= xb; ++x) { const uint8_t* p = im.at(x, y); int hi = std::max(p[0], std::max(p[1], p[2])), lo = std::min(p[0], std::min(p[1], p[2]));
    if (hi - lo > 40) { ox0 = std::min(ox0, x); ox1 = std::max(ox1, x); oy0 = std::min(oy0, y); oy1 = std::max(oy1, y); } }
  for (int y = ya; y <= yb; ++y) for (int x = xa; x <= xb; ++x) {
    if (x >= ox0 - 6 && x <= ox1 + 6 && y >= oy0 - 6 && y <= oy1 + 6) continue;
    double tx, ty; g_cal[e].inv(x, y, tx, ty); double nx = (tx - cx) / hx, ny = (ty - cy) / hy, rr = (fabs(nx) >= 1.3 || fabs(ny) >= 1.3) ? 9 : FocusRadius(nx, ny);
    int b = int(rr / 0.02); if (b < 0 || b >= NB) continue; sum[b] += Lum(im.at(x, y)); ++cnt[b];
  }
  mean.assign(NB, -1); for (int b = 0; b < NB; ++b) if (cnt[b] > 20) mean[b] = sum[b] / cnt[b];
  double si = 0, so = 0; int ni = 0, no = 0;
  for (int b = 0; b < 25; ++b) if (mean[b] >= 0) { si += mean[b]; ++ni; }
  for (int b = 54; b < 62; ++b) if (mean[b] >= 0) { so += mean[b]; ++no; }
  if (!ni || !no) return false; yIn = si / ni; yOut = so / no; return true;
}
static void DoGray(const Case& c, const Image& im, Row& r) {
  double worst = 0, worstCodes = 0, inOut = 0; int n = 0;
  for (int e = 0; e < 2; ++e) { std::vector<double> m; double yi, yo; if (!RingProfile(c, im, e, m, yi, yo) || yo <= 0) continue;
    for (int b = 0; b < 52; ++b) if (m[b] >= 0) { worst = std::max(worst, fabs(m[b] / yo - 1)); worstCodes = std::max(worstCodes, fabs(LinearToSrgb(m[b]) - LinearToSrgb(yo)) * 255); }
    inOut += yi / yo - 1; ++n; }
  if (n) { r.v["ring_rel"] = worst; r.v["ring_codes"] = worstCodes; r.v["in_out"] = inOut / n; r.v["level"] = Num(c, "la"); } else r.note += "profilo non misurabile; ";
}
static void DoTwo(const Case& c, const Image& im, Row& r) {
  double sl = 0, sg = 0; int n = 0;
  for (int e = 0; e < 2; ++e) { std::vector<double> m; double yi, yo; if (!RingProfile(c, im, e, m, yi, yo)) continue;
    for (int b = 31; b < 49; ++b) if (m[b] >= 0) { double rr = (b + 0.5) * 0.02, t = (rr - 0.6) / 0.4; t = t < 0 ? 0 : (t > 1 ? 1 : t); double a = 1 - t * t * (3 - 2 * t);
      double lin = a * yi + (1 - a) * yo, gam = SrgbToLinear(a * LinearToSrgb(yi) + (1 - a) * LinearToSrgb(yo));
      sl += pow(LinearToSrgb(m[b]) - LinearToSrgb(lin), 2); sg += pow(LinearToSrgb(m[b]) - LinearToSrgb(gam), 2); ++n; } }
  if (n) { r.v["rms_linear"] = sqrt(sl / n) * 255; r.v["rms_gamma"] = sqrt(sg / n) * 255; r.v["la"] = Num(c, "la"); r.v["lb"] = Num(c, "lb"); } else r.note += "profilo non misurabile; ";
}
static void MeanPatch(const Case& c, const Image& im, int e, double nx, double ny, double hs, double out[3]) {
  Fov fb = GetFov(c, "fovB", e); double cx = (fb.l + fb.r) / 2, cy = (fb.u + fb.d) / 2, hx = (fb.r - fb.l) / 2, hy = (fb.u - fb.d) / 2, s[3] = {0, 0, 0}; int n = 0;
  double px0, py0, px1, py1; g_cal[e].fwd(cx + (nx - hs) * hx, cy + (ny + hs) * hy, px0, py0); g_cal[e].fwd(cx + (nx + hs) * hx, cy + (ny - hs) * hy, px1, py1);
  for (int y = int(std::min(py0, py1)); y <= int(std::max(py0, py1)); ++y) for (int x = int(std::min(px0, px1)); x <= int(std::max(px0, px1)); ++x) {
    if (x < 0 || y < 0 || x >= im.w || y >= im.h) continue; const uint8_t* p = im.at(x, y); for (int k = 0; k < 3; ++k) s[k] += p[k]; ++n; }
  for (int k = 0; k < 3; ++k) out[k] = n ? s[k] / n : -1;
}
static void DoSentinel(const Case& c, const Image& im, Row& r) {
  struct P { double x, y; uint8_t rgb[3]; double a; const char* name; };
  const P ps[4] = {{-0.5, 0.5, {255, 0, 255}, 0, "p1"}, {0.5, 0.5, {128, 128, 128}, 0, "p2"}, {-0.5, -0.5, {0, 0, 255}, 1, "p3"}, {0.5, -0.5, {255, 255, 0}, 0.5019, "p4"}};
  // Blend models, all compared on the 8-bit values the mirror shows: 0 unpremultiplied over in linear light, 1 premultiplied
  // over in linear light, 2 opaque, 3 unpremultiplied over on encoded values, 4 premultiplied over on encoded values,
  // 5 colour premultiplied in linear light then blended on encoded values.
  double err[6] = {0, 0, 0, 0, 0, 0}; int n = 0;
  for (int e = 0; e < 2; ++e) { if (!g_cal[e].ok) continue;
    double am[3]; MeanPatch(c, im, e, 0, 1.14, 0.05, am); if (am[0] < 0) continue;
    for (auto& p : ps) { double m[3]; MeanPatch(c, im, e, p.x, p.y, 0.09, m); if (m[0] < 0) continue;
      for (int k = 0; k < 3; ++k) { double A = Dec(uint8_t(am[k] + 0.5)), B = Dec(p.rgb[k]), ae = am[k] / 255.0, be = p.rgb[k] / 255.0;
        double model[6] = {LinearToSrgb(p.a * B + (1 - p.a) * A), LinearToSrgb(B + (1 - p.a) * A), be, p.a * be + (1 - p.a) * ae, std::min(1.0, be + (1 - p.a) * ae), std::min(1.0, LinearToSrgb(p.a * B) + (1 - p.a) * ae)};
        for (int mdl = 0; mdl < 6; ++mdl) err[mdl] = std::max(err[mdl], fabs(model[mdl] * 255 - m[k])); }
      ++n; } }
  if (!n) { r.note += "toppe non misurabili; "; return; }
  int best = 0; for (int i = 1; i < 6; ++i) if (err[i] < err[best]) best = i;
  for (int i = 0; i < 6; ++i) r.v["err" + std::to_string(i)] = err[i];
  r.v["blend"] = Num(c, "blend"); r.v["best"] = best; r.v["best_err"] = err[best];
}
// Rising edges (dark to bright) along the middle rows of one eye: first = layer A's edge, second = layer B's.
static void DoEdge(const Case&, const Image& im, Row& r) {
  double rise[2] = {0, 0}; int cnt[2] = {0, 0}; int half = im.w / 2;
  for (int e = 0; e < 2; ++e) {
    std::vector<std::array<double, 2>> pts[2]; int y0 = int(im.h * 0.40), y1 = int(im.h * 0.60);
    for (int y = y0; y < y1; ++y) { std::vector<double> row(half); for (int x = 0; x < half; ++x) row[x] = im.at(e * half + x, y)[1] / 255.0;
      int found = 0; double lastX = -100;
      for (int x = 8; x < half - 8 && found < 2; ++x) { double lo = (row[x - 6] + row[x - 5] + row[x - 4]) / 3, hi = (row[x + 4] + row[x + 5] + row[x + 6]) / 3;
        if (lo < 0.30 && hi > 0.70 && row[x] < 0.55 && row[x + 1] >= 0.55 && x - lastX > 40) { double sub = x + (0.55 - row[x]) / (row[x + 1] - row[x] + 1e-9); pts[found].push_back({double(y), sub}); lastX = x; ++found; } } }
    for (int k = 0; k < 2; ++k) { auto& p = pts[k]; if (p.size() < 40) continue;
      double sy = 0, sx = 0, syy = 0, sxy = 0; for (auto& q : p) { sy += q[0]; sx += q[1]; syy += q[0] * q[0]; sxy += q[0] * q[1]; } double n = double(p.size()), b = (n * sxy - sy * sx) / (n * syy - sy * sy), a = (sx - b * sy) / n, nrm = sqrt(1 + b * b);
      const int NB = 240; std::vector<double> s(NB, 0); std::vector<int> c(NB, 0);
      for (int y = y0; y < y1; ++y) for (int x = int(a + b * y) - 14; x <= int(a + b * y) + 14; ++x) { if (x < 0 || x >= half) continue; double d = (x - (a + b * y)) / nrm; int bin = int((d + 12) / 0.1); if (bin < 0 || bin >= NB) continue; s[bin] += im.at(e * half + x, y)[1] / 255.0; ++c[bin]; }
      std::vector<double> esf(NB, -1); for (int i = 0; i < NB; ++i) if (c[i]) esf[i] = Dec(uint8_t(s[i] / c[i] * 255 + 0.5));
      double lo = 0, hi = 0; int nl = 0, nh = 0; for (int i = 0; i < 30; ++i) if (esf[i] >= 0) { lo += esf[i]; ++nl; } for (int i = NB - 30; i < NB; ++i) if (esf[i] >= 0) { hi += esf[i]; ++nh; }
      if (!nl || !nh) continue; lo /= nl; hi /= nh; double t10 = lo + 0.1 * (hi - lo), t90 = lo + 0.9 * (hi - lo), x10 = -1, x90 = -1;
      for (int i = 1; i < NB; ++i) { if (esf[i] < 0 || esf[i - 1] < 0) continue; if (x10 < 0 && esf[i - 1] < t10 && esf[i] >= t10) x10 = i * 0.1; if (x90 < 0 && esf[i - 1] < t90 && esf[i] >= t90) x90 = i * 0.1; }
      if (x10 >= 0 && x90 > x10) { rise[k] += x90 - x10; ++cnt[k]; } } }
  if (cnt[0]) r.v["rise_a"] = rise[0] / cnt[0]; if (cnt[1]) r.v["rise_b"] = rise[1] / cnt[1];
  if (!cnt[0] && !cnt[1]) r.note += "bordi non trovati; ";
}
// Reprojection coherence on the distorted mirror: displacement of B's dots against the displacement of A's dots around them.
static void DoReproj(const Case& c, const Image& im, const Image& rest, Row& r) {
  double step = Num(c, "step"), sum = 0, mx = 0, common = 0, expected = 0, floorSum = 0, spacing = 0; int n = 0, floorN = 0, half = im.w / 2;
  for (int e = 0; e < 2; ++e) {
    auto g0 = Blobs(rest, e * half, (e + 1) * half, GreenW), g1 = Blobs(im, e * half, (e + 1) * half, GreenW), r0 = Blobs(rest, e * half, (e + 1) * half, RedW), r1 = Blobs(im, e * half, (e + 1) * half, RedW);
    if (g0.size() < 8 || r0.size() < 4) continue; double sp = NearestSpacing(g0), ppt = sp / step; Fov fb = GetFov(c, "fovB", e); double scale = (Num(c, "bw") / (fb.r - fb.l)) / ppt;
    // The common shift first, by voting over all green pairs closer than a spacing; then each dot is matched near its predicted place.
    std::map<std::pair<int, int>, int> votes; for (auto& a : g0) for (auto& b2 : g1) { double dx = b2.x - a.x, dy = b2.y - a.y; if (hypot(dx, dy) < 0.9 * sp) ++votes[{int(floor(dx / 3 + 0.5)), int(floor(dy / 3 + 0.5))}]; }
    double gx = 0, gy = 0; int bestVotes = 0; for (auto& v : votes) if (v.second > bestVotes) { bestVotes = v.second; gx = v.first.first * 3.0; gy = v.first.second * 3.0; }
    std::vector<double> ra; for (auto& b2 : r0) ra.push_back(b2.area); double rma = Median(ra);
    auto match = [&](const Blob& b, const std::vector<Blob>& set, double& dx, double& dy, double px, double py) { double best = 1e9; for (auto& s : set) { double d = hypot(s.x - (b.x + px), s.y - (b.y + py)); if (d < best) { best = d; dx = s.x - b.x; dy = s.y - b.y; } } return best < 0.3 * sp; };
    std::vector<std::array<double, 4>> gd; for (auto& g : g0) { double dx, dy; if (match(g, g1, dx, dy, gx, gy)) gd.push_back({g.x, g.y, dx, dy}); }
    if (gd.size() < 6) continue; std::vector<double> mags; for (auto& g : gd) mags.push_back(hypot(g[2], g[3])); common += Median(mags) / ppt; expected += tan(std::max(fabs(Num(c, "yawA")), fabs(Num(c, "pitchA"))) * 3.14159265 / 180);
    for (auto& b : r0) { double dx, dy; if (b.area < 0.75 * rma || b.area > 1.4 * rma) continue;
      // B may move differently from A (the control case): look for it around A's shift and around no shift at all.
      if (!match(b, r1, dx, dy, gx, gy) && !match(b, r1, dx, dy, 0, 0)) { double bx = 0, by = 0, bb = 1e9; for (auto& s2 : r1) { double d = hypot(s2.x - b.x, s2.y - b.y); if (d < bb) { bb = d; bx = s2.x - b.x; by = s2.y - b.y; } } if (bb > 0.9 * sp) continue; dx = bx; dy = by; }
      std::vector<std::pair<double, size_t>> nearest; for (size_t i = 0; i < gd.size(); ++i) nearest.push_back({hypot(gd[i][0] - b.x, gd[i][1] - b.y), i}); std::sort(nearest.begin(), nearest.end());
      double wx = 0, wy = 0, ws = 0; for (size_t k = 0; k < 4 && k < nearest.size(); ++k) { double w = 1 / (nearest[k].first + 1); wx += w * gd[nearest[k].second][2]; wy += w * gd[nearest[k].second][3]; ws += w; }
      double d = hypot(dx - wx / ws, dy - wy / ws) * scale; sum += d; mx = std::max(mx, d); ++n; }
    // The same estimate applied to layer A alone (each green dot against its four nearest other green dots, taken near the
    // focus area): what the method reads when there is no incoherence at all.
    double cxm = 0, cym = 0; for (auto& b : r0) { cxm += b.x; cym += b.y; } cxm /= r0.size(); cym /= r0.size();
    for (size_t i = 0; i < gd.size(); ++i) { if (hypot(gd[i][0] - cxm, gd[i][1] - cym) > 4 * sp) continue;
      std::vector<std::pair<double, size_t>> nb; for (size_t j = 0; j < gd.size(); ++j) if (j != i) nb.push_back({hypot(gd[j][0] - gd[i][0], gd[j][1] - gd[i][1]), j}); std::sort(nb.begin(), nb.end());
      double wx = 0, wy = 0, ws = 0; for (size_t k = 0; k < 4 && k < nb.size(); ++k) { double w = 1 / (nb[k].first + 1); wx += w * gd[nb[k].second][2]; wy += w * gd[nb[k].second][3]; ws += w; }
      floorSum += hypot(gd[i][2] - wx / ws, gd[i][3] - wy / ws) * scale; ++floorN; }
    spacing += sp;
  }
  if (n) { r.v["coh_mean"] = sum / n; r.v["coh_max"] = mx; r.v["shift_tan"] = common / 2; r.v["shift_expected"] = expected / 2; r.v["coh_n"] = n; r.v["coh_floor"] = floorN ? floorSum / floorN : -1; r.v["spacing_px"] = spacing / 2; } else r.note += "punti non accoppiabili; ";
}

struct Load { double fpsA = 0, missed = 0, discard = 0, gpu = 0; int n = 0; };
static int StampSeconds(const std::string& s) { int Y, M, D, h, m; double sec; if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%lf", &Y, &M, &D, &h, &m, &sec) != 6) return -1; return ((D * 24 + h) * 60 + m) * 60 + int(sec); }
static Load ServerLoad(const std::string& logPath, int pid, const std::string& begin, const std::string& end) {
  Load L; FILE* f = fopen(logPath.c_str(), "r"); if (!f) return L; int b = StampSeconds(begin), e = StampSeconds(end); char line[1024], key[32]; snprintf(key, sizeof(key), " %d ", pid);
  while (fgets(line, sizeof(line), f)) { if (!strstr(line, "rendering fps:(a:") || !strstr(line, key)) continue; int t = StampSeconds(std::string(line + 1, 21)); if (t < b + 2 || t > e - 1) continue;
    double fa = 0, ma = 0, da = 0, g = 0; const char* p = strstr(line, "fps:(a:"); if (p) fa = atof(p + 7); p = strstr(line, "Missed:(a:"); if (p) ma = atof(p + 10); p = strstr(line, "Discard:(a:"); if (p) da = atof(p + 11); p = strstr(line, "GPU: "); if (p) g = atof(p + 5);
    L.fpsA += fa; L.missed += ma; L.discard += da; L.gpu += g; ++L.n; }
  fclose(f); if (L.n) { L.fpsA /= L.n; L.missed /= L.n; L.discard /= L.n; L.gpu /= L.n; } return L;
}

// --selftest: writes synthetic "mirror" images of the quad chain into <outdir> (CPU only: no runtime, no GPU, no
// headset) and then analyses them like a real run. It checks the chain-mode arithmetic: a still lattice, a lattice
// with the focus dots 0.5 focus pixels off, and moving lattices shown at known places of the trajectory; then the
// scene pattern (SelfTestScene), whose readings are checked (SelfTestCheck: exit code 1 if one is wrong).
// Scene pattern: a still image, real frames, a clean half step, a half step whose focus layer sits 1.5 focus pixels to
// the right of the periphery layer (blended on encoded values, as the runtime blends two layers), a half step carrying
// 30 % of the real frame before it, and three images of the diagonal motion. Same mirror geometry as the lattice
// images above (800 px per unit of tangent), a focus FOV in the proportions of the real one.
static void SelfTestScene(const std::string& dir, int& no) {
  const int EW = 1440, H = 1440, BW = 2712, BH = 2410, M = 32; const double ppt = 800, vx = 0.0071;
  const Fov eye{-0.9, 0.9, 0.9, -0.9}, fbs[2] = {{-0.50, 0.42, 0.40, -0.40}, {-0.42, 0.50, 0.40, -0.40}}; const double focusPerTan = BW / (fbs[0].r - fbs[0].l);
  struct T { const char* kind; const char* tag; double shown; long long submitted; double seamPx, ghost; bool encoded, diag; };
  const T tests[] = {{"scene", "still", 0, 0, 0, 0, false, false}, {"motion", "real3", 3, 5, 0, 0, false, false}, {"motion", "real4", 4, 6, 0, 0, false, false},
                     {"motion", "half", 3.5, 5, 0, 0, false, false}, {"motion", "half_seam1.5", 4.5, 6, 1.5, 0, true, false}, {"motion", "half_ghost30", 5.5, 7, 0, 0.3, false, false},
                     {"motion", "diag_real2", 2, 4, 0, 0, false, true}, {"motion", "diag_half", 2.5, 4, 0, 0, false, true}, {"motion", "diag_real3", 3, 5, 0, 0, false, true}};
  const uint8_t* lut = SceneEncLut(); int seq = 0; ScenePattern sp; sp.Build(4242);
  for (const T& t : tests) {
    double vy = t.diag ? 0.6 * vx : 0, seam = t.seamPx / focusPerTan; Image im(2 * EW, H);
    for (int e = 0; e < 2; ++e) SceneRows(H, [&](int y) { const Fov& fb = fbs[e]; for (int x = 0; x < EW; ++x) {
      double tx = (x + 0.5 - EW / 2.0) / ppt, ty = -(y + 0.5 - H / 2.0) / ppt, a = FocusAlpha((tx - (fb.l + fb.r) / 2) / ((fb.r - fb.l) / 2), (ty - (fb.u + fb.d) / 2) / ((fb.u - fb.d) / 2)), per[3] = {0, 0, 0}, foc[3] = {0, 0, 0};
      auto layer = [&](bool focus, double out[3]) { double ox = focus ? seam : 0; sp.Eval(tx - t.shown * vx - ox, ty - t.shown * vy, ppt, focus, out);
        if (t.ghost > 0) { double g[3]; sp.Eval(tx - (t.shown - 0.5) * vx - ox, ty - (t.shown - 0.5) * vy, ppt, focus, g); for (int k = 0; k < 3; ++k) out[k] = (1 - t.ghost) * out[k] + t.ghost * g[k]; } };
      if (a < 1) layer(false, per); if (a > 0) layer(true, foc);
      uint8_t* o = im.at(e * EW + x, y);
      for (int k = 0; k < 3; ++k) { double v = (t.encoded && a > 0 && a < 1) ? SrgbToLinear(a * LinearToSrgb(foc[k]) + (1 - a) * LinearToSrgb(per[k])) : a * foc[k] + (1 - a) * per[k]; v = v < 0 ? 0 : (v > 1 ? 1 : v); o[k] = lut[int(v * 4096 + 0.5)]; }
      o[3] = 255; } });
    char name[32]; snprintf(name, sizeof(name), "case%03d", no++);
    Case c; c["id"] = name; c["kind"] = t.kind; c["tag"] = t.tag; c["mirror"] = "undistorted"; c["chain"] = "quad"; c["format"] = "29"; c["premult"] = "1"; c["layers"] = "1"; c["tracked"] = "1";
    c["scale"] = "1"; c["step"] = "0.06"; c["bw"] = std::to_string(BW); c["bh"] = std::to_string(BH); c["aw"] = "2712"; c["ah"] = "2678"; c["focus_drift"] = "0";
    c["pattern"] = "scene"; c["seed"] = "4242"; c["phases"] = std::to_string(M); c["vtan"] = F(vx); c["vtan_y"] = F(vy); c["fg"] = "2"; c["sweep"] = "pingpong";
    for (int e = 0; e < 2; ++e) for (const char* nm : {"eye", "fovA", "fovB"}) { const Fov& f = !strcmp(nm, "fovB") ? fbs[e] : eye; std::string p = std::string(nm) + std::to_string(e); c[p + "l"] = F(f.l); c[p + "r"] = F(f.r); c[p + "u"] = F(f.u); c[p + "d"] = F(f.d); }
    if (!strcmp(t.kind, "motion")) { c["submitted"] = std::to_string(t.submitted); c["seq"] = std::to_string(seq); c["buf"] = std::to_string(seq % 2); c["t_ms"] = F(seq * 11.1); ++seq; }
    SavePng(Wide(dir + "\\" + name + ".png"), im); SaveCase(dir + "\\" + name + ".txt", c);
  }
}
// What the scene analysis must read from the images above. Returns the number of failures.
static int SelfTestCheck() {
  auto get = [](const Row& r, const char* k, double def) { auto i = r.v.find(k); return i == r.v.end() ? def : i->second; };
  int fails = 0, seen = 0; double halfMae = -1; for (auto& r : g_rows) if (r.scene && r.tag == "half") halfMae = get(r, "q_mae", -1);
  printf("\nscene self-test:\n");
  for (auto& r : g_rows) { if (!r.scene) continue; ++seen;
    int cls = int(get(r, "class", -1)); double frac = get(r, "frac", -1), worst = std::max(get(r, "mk_per_max", 0), std::max(get(r, "mk_band_max", 0), get(r, "mk_foc_max", 0)));
    bool all = get(r, "mk_n", 0) == get(r, "mk_exp", -1) && get(r, "mk_n", 0) > 0, ok = false; const char* want = "";
    if (r.tag == "still") { want = "still, phase 0, all markers within 0.6 px"; ok = cls == 0 && get(r, "traj_err", 9) < 0.3 && all && worst < 0.6; }
    else if (r.tag.find("real") != std::string::npos) { want = "real, all markers within 0.6 px"; ok = cls == 0 && all && worst < 0.6; }
    else if (r.tag == "half" || r.tag == "diag_half") { want = "half step, all markers within 0.6 px, seam below 0.4 px"; ok = cls == 2 && fabs(frac - 0.5) < 0.03 && all && worst < 0.6 && get(r, "seam_n", 0) > 0 && get(r, "seam_med", 9) < 0.4; }
    else if (r.tag == "half_seam1.5") { want = "half step, seam +1.5 px in x, encoded blend"; ok = cls == 2 && fabs(get(r, "seam_dx", 9) - 1.5) < 0.35 && fabs(get(r, "seam_dy", 9)) < 0.35 && get(r, "blend", -1) == 1; }
    else if (r.tag == "half_ghost30") { want = "not a clean half step at 2X, error at least twice the clean half step's"; ok = cls == 9 && halfMae > 0 && get(r, "q_mae", 0) > 2 * halfMae; }
    if (!ok) ++fails;
    printf("  %s %-13s %-4s type %d lag %.2f | markers %2.0f/%2.0f worst %.2f px (per %.2f band %.2f foc %.2f) dup %.0f elong %.3f | seam n %.0f med %.2f dx %+.2f dy %+.2f blend %s | mae %.2f band %.2f psnr %.1f (ref case %.0f, %+.2f frames) | expected: %s\n",
           r.id.c_str(), r.tag.c_str(), ok ? "ok" : "FAIL", cls, get(r, "lag", -1), get(r, "mk_n", 0), get(r, "mk_exp", 0), worst, get(r, "mk_per_med", -1), get(r, "mk_band_med", -1), get(r, "mk_foc_med", -1), get(r, "mk_dup", 0), get(r, "mk_elong", 0),
           get(r, "seam_n", 0), get(r, "seam_med", -1), get(r, "seam_dx", 0), get(r, "seam_dy", 0), get(r, "blend", 0) ? "encoded" : "linear", get(r, "q_mae", -1), get(r, "q_mae_band", -1), get(r, "q_psnr", -1), get(r, "q_ref", -1), get(r, "q_shift", 0), want); }
  if (seen != 9) { ++fails; printf("  %d scene images analysed, 9 expected\n", seen); }
  printf("scene self-test: %s\n", fails ? "FAILED" : "passed"); return fails;
}
static void SelfTestImages(const std::string& dir) {
  const int EW = 1440, H = 1440, BW = 2028, BH = 2004, M = 8; const double ppt = 800, step = 0.06, rad = 0.009 * ppt;
  const Fov eye{-0.9, 0.9, 0.9, -0.9}, fb{-0.30, 0.30, 0.27, -0.27}; const double focusPerTan = BW / (fb.r - fb.l);
  struct T { const char* kind; const char* tag; double shown; long long submitted; double seamPx; };
  // shown = the content phase the image holds (frames); submitted = the last frame the probe had sent.
  const T tests[] = {{"lattice", "centre", 0, 0, 0}, {"lattice", "seam+0.5", 0, 0, 0.5}, {"motion", "m00", 2.0, 4, 0}, {"motion", "m01", 2.5, 5, 0},
                     {"motion", "m02", 3.0, 5, 0}, {"motion", "m03", 7.5, 9, 0}, {"motion", "m04", 5.0 + 1.0 / 3, 7, 0}, {"motion", "m05", 6.2, 8, 0.5}};
  int no = 0;
  for (const T& t : tests) {
    double sx = t.shown / M * step, seam = t.seamPx / focusPerTan; Image im(2 * EW, H);
    auto dot = [&](double tx, double ty, double off) { double ix = floor(tx / step - off + 0.5), iy = floor(ty / step - off + 0.5), dx = tx - (ix + off) * step, dy = ty - (iy + off) * step, cov = rad - sqrt(dx * dx + dy * dy) * ppt + 0.5; return cov < 0 ? 0.0 : (cov > 1 ? 1.0 : cov); };
    for (int e = 0; e < 2; ++e) for (int y = 0; y < H; ++y) for (int x = 0; x < EW; ++x) {
      double tx = (x + 0.5 - EW / 2.0) / ppt, ty = -(y + 0.5 - H / 2.0) / ppt, px = tx - sx;
      bool anchor = floor(px / step + 0.5) == 0 && floor(ty / step + 0.5) == 0; double g = dot(px, ty, 0), col[3] = {anchor ? g : 0, g, anchor ? g : 0};
      double nx = (tx - (fb.l + fb.r) / 2) / ((fb.r - fb.l) / 2), ny = (ty - (fb.u + fb.d) / 2) / ((fb.u - fb.d) / 2), a = FocusAlpha(nx, ny);
      if (a > 0) { double rd = dot(px - seam, ty, 0.5), wd = anchor ? g : 0, foc[3] = {std::max(rd, wd), wd, wd}; for (int k = 0; k < 3; ++k) col[k] = a * foc[k] + (1 - a) * col[k]; }
      uint8_t* o = im.at(e * EW + x, y); for (int k = 0; k < 3; ++k) o[k] = Enc(col[k]); o[3] = 255; }
    char name[32]; snprintf(name, sizeof(name), "case%03d", no++);
    Case c; c["id"] = name; c["kind"] = t.kind; c["tag"] = t.tag; c["mirror"] = "undistorted"; c["chain"] = "quad"; c["format"] = "29"; c["premult"] = "1"; c["layers"] = "1"; c["tracked"] = "1";
    c["scale"] = "1"; c["step"] = F(step); c["bw"] = std::to_string(BW); c["bh"] = std::to_string(BH); c["aw"] = "1040"; c["ah"] = "1028"; c["focus_drift"] = "0";
    for (int e = 0; e < 2; ++e) for (const char* nm : {"eye", "fovA", "fovB"}) { const Fov& f = !strcmp(nm, "fovB") ? fb : eye; std::string p = std::string(nm) + std::to_string(e); c[p + "l"] = F(f.l); c[p + "r"] = F(f.r); c[p + "u"] = F(f.u); c[p + "d"] = F(f.d); }
    if (!strcmp(t.kind, "motion")) { c["phases"] = std::to_string(M); c["vtan"] = F(step / M); c["submitted"] = std::to_string(t.submitted); c["seq"] = std::to_string(no - 3); c["buf"] = std::to_string(no % 2); c["t_ms"] = F((no - 3) * 11.1); }
    SavePng(Wide(dir + "\\" + name + ".png"), im); SaveCase(dir + "\\" + name + ".txt", c);
  }
  SelfTestScene(dir, no);
  Case run; run["runtime"] = "selftest (synthetic images, nothing was captured)"; run["chain"] = "quad"; run["layers"] = "selftest"; run["exit"] = "0"; run["tracked"] = "1"; run["api"] = "D3D11";
  SaveCase(dir + "\\run.txt", run);
}

int main(int argc, char** argv) {
  if (argc < 2) { printf("usage: probe_analyze <outdir>\n       probe_analyze --selftest <new outdir>   (synthetic quad-chain images, CPU only; see CHAIN.md)\n"); return 2; }
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool selftest = false;
  if (!strcmp(argv[1], "--selftest")) { if (argc < 3) return 2; CreateDirectoryA(argv[2], nullptr); SelfTestImages(argv[2]); argv[1] = argv[2]; selftest = true; }
  std::string dir = argv[1];
  Case run; LoadCase(dir + "\\run.txt", run);
  std::vector<Case> cases; for (int i = 0; i < 2000; ++i) { char n[32]; snprintf(n, sizeof(n), "\\case%03d.txt", i); Case c; if (!LoadCase(dir + n, c)) break; cases.push_back(c); }
  printf("%zu cases\n", cases.size());
  Image rest; bool haveRest = false;
  for (auto& c : cases) {
    Image im; if (!LoadPng(Wide(dir + "\\" + Str(c, "id") + ".png"), im)) continue;
    Row r; r.id = Str(c, "id"); r.kind = Str(c, "kind"); r.tag = Str(c, "tag"); r.variant = VariantName(c); r.mirror = Str(c, "mirror"); r.quad = IsQuad(c); if (r.quad && Num(c, "focus_drift") > 0.002) r.note += "il FOV del focus si e mosso dopo la generazione delle immagini (tracciamento oculare attivo?); "; r.v["unstable"] = Num(c, "unstable"); r.v["reference"] = Num(c, "reference"); r.v["scale"] = Num(c, "scale", 1);
    if (Num(c, "tracked") < 1) r.note += "visore non tracciato; ";
    bool undist = r.mirror == "undistorted";
    if (r.kind == "lattice" && undist) {
      if (!g_cal[0].ok && Num(c, "scale", 1) == 1) for (int e = 0; e < 2; ++e) g_cal[e] = FitLattice(im, e, Num(c, "step"));
      DoLattice(c, im, r);
    } else if (r.kind == "gray") DoGray(c, im, r);
    else if (r.kind == "two") DoTwo(c, im, r);
    else if (r.kind == "sentinel") DoSentinel(c, im, r);
    else if (r.kind == "edge") DoEdge(c, im, r);
    else if (r.kind == "scene" || (r.kind == "motion" && Str(c, "pattern") == "scene")) { r.scene = true; DoScene(c, im, r, r.kind == "scene"); }
    else if (r.kind == "motion") DoMotion(c, im, r);
    else if (r.kind == "lattice" && !undist) { if (r.tag == "rest") { rest = im; haveRest = true; } else if (haveRest) DoReproj(c, im, rest, r); }
    g_rows.push_back(r);
  }
  if (!g_sceneFrames.empty()) SceneQuality();
  FILE* csv = fopen((dir + "\\results.csv").c_str(), "w");
  if (csv) { fprintf(csv, "id,kind,tag,variant,mirror,metric,value,note\n"); for (auto& r : g_rows) for (auto& kv : r.v) fprintf(csv, "%s,%s,%s,%s,%s,%s,%.6g,%s\n", r.id.c_str(), r.kind.c_str(), r.tag.c_str(), r.variant.c_str(), r.mirror.c_str(), kv.first.c_str(), kv.second, r.note.c_str()); fclose(csv); }

  // ---- report ----
  FILE* f = fopen((dir + "\\report.md").c_str(), "w"); if (!f) return 1;
  auto has = [](const Row& r, const char* k) { return r.v.count(k) > 0; };
  int exitCode = int(Num(run, "exit", -1));
  fprintf(f, "# Sonda del runtime: rapporto\n\n");
  fprintf(f, "- API grafica: %s\n", Str(run, "api").empty() ? "D3D11" : Str(run, "api").c_str());
  if (Str(run, "chain") == "quad") fprintf(f, "- **Modalita catena**: un layer di proiezione con quattro viste (periferia sinistra e destra, focus sinistro e destro); composizione e sfumatura le fanno i layer sotto la sonda. Dimensioni consigliate delle quattro viste: %s\n- Layer caricati (dal piu vicino all'applicazione): %s\n",
                                    Str(run, "recommended_quad").c_str(), Str(run, "layers").empty() ? "nessuno (quad views del runtime)" : Str(run, "layers").c_str());
  else if (!Str(run, "layers").empty()) fprintf(f, "- Layer caricati (dal piu vicino all'applicazione): %s\n", Str(run, "layers").c_str());
  fprintf(f, "- Runtime: `%s`\n- Inizio: %s, fine: %s\n- Casi catturati: %zu\n- Sessione: stato %s, visore tracciato: %s\n- Immagini per swapchain: %s; dimensione consigliata: %s\n",
          Str(run, "runtime").c_str(), Str(run, "start").c_str(), Str(run, "end").c_str(), cases.size(), Str(run, "state").c_str(), Num(run, "tracked") ? "si" : "NO", Str(run, "images_per_swapchain").c_str(), Str(run, "recommended").c_str());
  fprintf(f, "- Chiavi di specchio prima / dopo la prova: `%s` / `%s`\n", Str(run, "mirror_keys_before").c_str(), Str(run, "mirror_keys_after").c_str());
  fprintf(f, "- `XR_EXT_frame_synthesis`: %s\n\n", Str(run, "extensions").find("XR_EXT_frame_synthesis") != std::string::npos ? "presente" : "assente");
  if (exitCode == 3) fprintf(f, "**NON GIUDICABILE: il visore non era tracciato.** Il compositore Pimax non mostra nessuna applicazione in quello stato. Illuminare la stanza e ripetere.\n\n");
  if (exitCode == 2) fprintf(f, "**NON GIUDICABILE: la sessione OpenXR non e partita o si e interrotta.**\n\n");
  if (exitCode == 4) fprintf(f, "**PARZIALE: lo specchio del compositore non ha dato immagini per tutti i casi.**\n\n");
  fprintf(f, "Calibrazione dello specchio non distorto: occhio sinistro %s (residuo %.3f px su %d punti, %.0f px per unita di tangente), occhio destro %s (residuo %.3f px).\n\n",
          g_cal[0].ok ? "ok" : "FALLITA", g_cal[0].rms, g_cal[0].n, fabs(g_cal[0].ax[1]), g_cal[1].ok ? "ok" : "FALLITA", g_cal[1].rms);

  // per-variant summary
  std::vector<std::string> variants; for (auto& r : g_rows) if (std::find(variants.begin(), variants.end(), r.variant) == variants.end()) variants.push_back(r.variant);
  fprintf(f, "## Esito per variante\n\n| Variante | Posizione: media / massimo (px del focus) | Corona: deviazione massima | Fusione | Alfa | Esito |\n|---|---|---|---|---|---|\n");
  bool anyPass = false; std::vector<std::string> doubts;
  for (auto& v : variants) {
    double pm = 0, px = 0, ring = 0; int np = 0; bool ringFail = false, ringSeen = false, posSeen = false, alphaOk = false, alphaSeen = false; double rl = 0, rg = 0; int nt = 0; std::string alphaTxt = "non misurata";
    for (auto& r : g_rows) { if (r.variant != v || r.v.count("reference") && r.v.at("reference") > 0) continue;
      if (r.kind == "lattice" && r.mirror == "undistorted" && has(r, "pos_max") && r.v.at("scale") == 1) { pm += r.v.at("pos_mean"); px = std::max(px, r.v.at("pos_max")); ++np; posSeen = true; if (r.v.at("pos_max") > 0.8 * kPosMaxFocusPx) doubts.push_back(r.id + " (posizione, " + r.tag + ")"); }
      if (r.kind == "gray" && has(r, "ring_rel")) { ringSeen = true; double lvl = r.v.at("level"), rel = r.v.at("ring_rel"); ring = std::max(ring, rel);
        // A deviation is a failure only above both the relative threshold and one and a half 8-bit codes: nothing can be flatter than the quantisation.
        bool fail = r.v.at("ring_codes") > kRingCodes && (lvl >= 0.09 ? rel > kRingRel : (lvl >= 0.025 ? rel > kRingRelDark : true)); if (fail) { ringFail = true; doubts.push_back(r.id + " (corona al " + F(lvl * 100) + " %)"); } }
      if (r.kind == "two" && has(r, "rms_linear")) { rl += r.v.at("rms_linear"); rg += r.v.at("rms_gamma"); ++nt; }
      if (r.kind == "sentinel" && has(r, "best") && r.v.at("blend") > 0) { alphaSeen = true; int best = int(r.v.at("best")); double e = r.v.at("best_err");
        alphaOk = best != 2 && e <= kPatchCodes; alphaTxt = std::string(kModelNames[best]) + ", errore " + F(floor(e * 10 + 0.5) / 10) + "/255"; if (!alphaOk) doubts.push_back(r.id + " (alfa)"); } }
    bool posOk = posSeen && px <= kPosMaxFocusPx && pm / std::max(np, 1) <= kPosMeanFocusPx; bool quadVariant = false; for (auto& r : g_rows) if (r.variant == v && r.quad) quadVariant = true;
    // Quad chain: no sentinel case (the layer owns the alpha), so the verdict is position and corona only.
    if (quadVariant && !alphaSeen) alphaTxt = "non applicabile (la fa il layer)";
    bool pass = posOk && ringSeen && !ringFail && (quadVariant || (alphaSeen && alphaOk)); anyPass = anyPass || pass;
    std::string fus = nt ? (rl < rg ? "luce lineare" : "spazio gamma") : "non misurata";
    fprintf(f, "| %s | %s | %s | %s | %s | %s |\n", v.c_str(), posSeen ? (F(floor(pm / np * 100 + 0.5) / 100) + " / " + F(floor(px * 100 + 0.5) / 100)).c_str() : "non misurata",
            ringSeen ? (F(floor(ring * 1000 + 0.5) / 10) + " %").c_str() : "non misurata", nt ? (fus + " (scarto " + F(floor(rl / nt * 10 + 0.5) / 10) + " contro " + F(floor(rg / nt * 10 + 0.5) / 10) + ")").c_str() : fus.c_str(), alphaTxt.c_str(),
            pass ? "**passa**" : (posSeen || ringSeen ? "**non passa**" : "non giudicabile"));
  }
  fprintf(f, "\nSoglie: posizione massimo %.1f e media %.1f px del focus; corona %.0f %% (%.0f %% al 3 %%, %.1f codici all'1 %%); alfa %.0f/255. Le soglie della corona sono proposte: vanno tarate con l'occhio dell'utente.\n\n", kPosMaxFocusPx, kPosMeanFocusPx, kRingRel * 100, kRingRelDark * 100, kRingCodes, kPatchCodes);

  fprintf(f, "## Posizione del focus lungo il percorso (prima variante)\n\n| Caso | Posizione | Media | Massimo | Spostamento medio x, y | Punti | Rumore della misura |\n|---|---|---|---|---|---|---|\n");
  for (auto& r : g_rows) if (r.kind == "lattice" && r.mirror == "undistorted" && r.variant == (variants.empty() ? "" : variants[0]) && has(r, "pos_max"))
    fprintf(f, "| %s | %s%s | %.2f | %.2f | %+.2f, %+.2f | %.0f | %.2f |\n", r.id.c_str(), r.tag.c_str(), r.v.at("scale") != 1 ? " (modello in scala)" : "", r.v.at("pos_mean"), r.v.at("pos_max"), r.v.at("pos_dx"), r.v.at("pos_dy"), r.v.at("pos_n"), r.v.at("pos_floor"));
  fprintf(f, "\nValori in pixel del focus. Il rumore e il residuo della calibrazione sui punti del layer di periferia, nella stessa unita.\n\n");

  fprintf(f, "## Corona: due layer contro il riferimento composto\n\n| Caso | Variante | Livello | Deviazione massima | In codici a 8 bit | Dentro contro fuori |\n|---|---|---|---|---|---|\n");
  for (auto& r : g_rows) if (r.kind == "gray" && has(r, "ring_rel")) fprintf(f, "| %s | %s | %.0f %% | %.2f %% | %.1f | %+.2f %% |\n", r.id.c_str(), r.v.at("reference") > 0 ? "riferimento a un layer" : r.variant.c_str(), r.v.at("level") * 100, r.v.at("ring_rel") * 100, r.v.at("ring_codes"), r.v.at("in_out") * 100);
  fprintf(f, "\n## Tipo di fusione (due livelli diversi)\n\n| Caso | Variante | Periferia / focus | Scarto dal modello lineare | Scarto dal modello gamma |\n|---|---|---|---|---|\n");
  for (auto& r : g_rows) if (r.kind == "two" && has(r, "rms_linear")) fprintf(f, "| %s | %s | %.0f %% / %.0f %% | %.1f | %.1f |\n", r.id.c_str(), r.v.at("reference") > 0 ? "riferimento a un layer" : r.variant.c_str(), r.v.at("la") * 100, r.v.at("lb") * 100, r.v.at("rms_linear"), r.v.at("rms_gamma"));
  fprintf(f, "\nScarti in codici a 8 bit lungo la corona.\n\n");

  fprintf(f, "## Alfa: toppe sentinella\n\nErrore massimo (in codici a 8 bit) di ciascun modello di fusione rispetto a cio che mostra lo specchio; il modello con l'errore minore e il comportamento del runtime.\n\n| Caso | Variante | Lineare non premolt. | Lineare premolt. | Opaco | Gamma non premolt. | Gamma premolt. | Premolt. lineare poi gamma | Comportamento |\n|---|---|---|---|---|---|---|---|---|\n");
  for (auto& r : g_rows) if (r.kind == "sentinel" && has(r, "best"))
    fprintf(f, "| %s | %s%s | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f | %s |\n", r.id.c_str(), r.variant.c_str(), r.v.at("blend") > 0 ? "" : ", senza flag di fusione", r.v.at("err0"), r.v.at("err1"), r.v.at("err2"), r.v.at("err3"), r.v.at("err4"), r.v.at("err5"), kModelNames[int(r.v.at("best"))]);

  fprintf(f, "\n## Riproiezione (specchio distorto, posa finta)\n\n| Caso | Prova | Spostamento comune misurato / atteso (tangente) | Incoerenza media | massima | Fondo del metodo | Punti |\n|---|---|---|---|---|---|---|\n");
  bool reprojOk = true, reprojSeen = false;
  for (auto& r : g_rows) if (r.kind == "lattice" && r.mirror == "screen" && has(r, "coh_max")) { reprojSeen = true;
    fprintf(f, "| %s | %s | %.4f / %.4f | %.2f | %.2f | %.2f | %.0f |\n", r.id.c_str(), r.tag.c_str(), r.v.at("shift_tan"), r.v.at("shift_expected"), r.v.at("coh_mean"), r.v.at("coh_max"), r.v.at("coh_floor"), r.v.at("coh_n"));
    if (r.tag != "only_b_yaw2" && r.v.at("coh_mean") - std::max(0.0, r.v.at("coh_floor")) > kReprojFocusPx) { reprojOk = false; doubts.push_back(r.id + " (riproiezione " + r.tag + ")"); } }
  fprintf(f, "\nIncoerenza = differenza fra lo spostamento dei punti del focus e quello dei punti di periferia vicini, in pixel del focus. Nel caso `only_b_yaw2` ruota solo il focus: l'incoerenza deve essere grande, ed e il controllo che la misura vede l'errore. Il fondo del metodo e la stessa stima applicata ai soli punti di periferia: cio che la misura legge quando l'incoerenza e nulla. Soglia: incoerenza media meno fondo <= %.1f px.\n\n", kReprojFocusPx);

  fprintf(f, "## Nitidezza su modello in scala (FOV ingranditi di 2,6 volte)\n\n| Caso | Specchio | Configurazione | Salita 10-90 %% bordo di periferia (px) | bordo del focus (px) |\n|---|---|---|---|---|\n");
  std::map<std::string, std::pair<const Row*, const Row*>> edgePairs;
  for (auto& r : g_rows) if (r.kind == "edge") { fprintf(f, "| %s | %s | %s | %s | %s |\n", r.id.c_str(), r.mirror == "screen" ? "distorto" : "non distorto", r.v.at("reference") > 0 ? "riferimento composto" : (r.quad ? "catena a quattro viste, scala 1" : "due layer"), has(r, "rise_a") ? F(floor(r.v.at("rise_a") * 100 + 0.5) / 100).c_str() : "-", has(r, "rise_b") ? F(floor(r.v.at("rise_b") * 100 + 0.5) / 100).c_str() : "-");
    auto& p = edgePairs[r.mirror]; (r.v.at("reference") > 0 ? p.second : p.first) = &r; }
  bool edgeOk = true, edgeSeen = false;
  for (auto& kv : edgePairs) if (kv.second.first && kv.second.second) for (const char* k : {"rise_a", "rise_b"}) if (has(*kv.second.first, k) && has(*kv.second.second, k)) { edgeSeen = true; double ratio = kv.second.first->v.at(k) / kv.second.second->v.at(k);
    fprintf(f, "\n- Specchio %s, %s: due layer / riferimento = %.2f%s", kv.first == "screen" ? "distorto" : "non distorto", !strcmp(k, "rise_a") ? "bordo di periferia" : "bordo del focus", ratio, ratio > kEdgeRatio ? " **(piu morbido del riferimento)**" : ""); if (ratio > kEdgeRatio) edgeOk = false; }
  fprintf(f, "\n\nVale se lo specchio campiona i layer con lo stesso filtro del pannello: e un'indicazione, non una misura sul pannello.\n\n");

  // ---- motion (quad chain) ----
  { int nm = 0, cls[4] = {0, 0, 0, 0}; double worst2 = 0, seamMax = 0, seamSum = 0; int seamN = 0;
    for (auto& r : g_rows) if (r.kind == "motion" && !r.scene) ++nm;
    if (nm) {
      fprintf(f, "## Movimento: traiettoria dei punti e giunzione, immagine per immagine\n\n| Caso | Buffer | Tempo (ms) | Ultimo frame inviato | Fase letta | Ritardo (frame) | Tipo | Errore sulla traiettoria 2X / 3X (px del focus) | Giunzione rosso-verde: media / massimo, x, y (px del focus) | Residuo del reticolo (px dello specchio) | Sinistro contro destro (frame) |\n|---|---|---|---|---|---|---|---|---|---|---|\n");
      for (auto& r : g_rows) if (r.kind == "motion" && !r.scene) {
        if (!has(r, "lag")) { fprintf(f, "| %s | | | | | | non leggibile | | | | |\n", r.id.c_str()); continue; }
        int k = int(r.v.at("class")); ++cls[k == 0 ? 0 : (k == 2 ? 1 : (k == 3 ? 2 : 3))]; worst2 = std::max(worst2, r.v.at("traj_err_2x"));
        std::string seam = "-"; if (has(r, "pos_max")) { char b[96]; snprintf(b, sizeof(b), "%.2f / %.2f, %+.2f, %+.2f", r.v.at("pos_mean"), r.v.at("pos_max"), r.v.at("pos_dx"), r.v.at("pos_dy")); seam = b; seamMax = std::max(seamMax, r.v.at("pos_max")); seamSum += r.v.at("pos_mean"); ++seamN; }
        fprintf(f, "| %s | %.0f | %.1f | %.0f | %.2f | %.2f | %s | %.2f / %.2f | %s | %.2f | %s |\n", r.id.c_str(), r.v.at("buf"), r.v.at("t_ms"), r.v.at("submitted"), r.v.at("phase"), r.v.at("lag"),
                k == 0 ? "reale" : (k == 2 ? "interpolato (meta)" : (k == 3 ? "interpolato (terzo)" : "**fuori traiettoria**")), r.v.at("traj_err_2x"), r.v.at("traj_err_3x"), seam.c_str(), r.v.at("fit_rms"), has(r, "phase_lr") ? F(floor(r.v.at("phase_lr") * 100 + 0.5) / 100).c_str() : "-");
      }
      double framePx = 0; for (auto& r : g_rows) if (r.kind == "motion" && !r.scene && has(r, "frame_px")) { framePx = r.v.at("frame_px"); break; }
      fprintf(f, "\nIl contenuto si sposta di %.1f px del focus a ogni frame inviato. Ritardo = frame fra l'ultimo inviato e quello che l'immagine mostra: intero per un frame reale, con parte .5 per uno interpolato a 2X (.33 o .67 a 3X). L'errore e la distanza dalla posizione attesa piu vicina. Immagini: %d reali, %d a meta, %d a un terzo, %d fuori traiettoria (su %d). Errore massimo sulla traiettoria a 2X: %.2f px. Giunzione: media %.2f, massimo %.2f px del focus.\n\n",
              framePx, cls[0], cls[1], cls[2], cls[3], nm, worst2, seamN ? seamSum / seamN : 0.0, seamMax);
      if (cls[1] + cls[2] == 0) fprintf(f, "**Nessuna immagine interpolata vista**: o la generazione di frame non era attiva, o lo specchio (scritto a meta della frequenza del visore) cade sempre sui frame reali. In quel caso questa tabella giudica solo i frame reali.\n\n");
      fprintf(f, "Un terzo non distingue da solo 3X da un 2X con errore: si legge insieme alla configurazione della generazione di frame usata.\n\n");
    } }

  // ---- scene pattern (quad chain) ----
  { std::vector<const Row*> sr; for (auto& r : g_rows) if (r.scene) sr.push_back(&r);
    if (!sr.empty()) {
      auto num = [](const Row& r, const std::string& k, double def) { auto i = r.v.find(k); return i == r.v.end() ? def : i->second; };
      // 0 real (the still image included), 1 interpolated, 2 off the trajectory, -1 not readable
      auto group = [&](const Row& r) { if (!r.v.count("class")) return -1; int k = int(r.v.at("class")); return k == 0 ? 0 : (k == 9 ? 2 : 1); };
      auto typeName = [&](const Row& r) -> std::string { if (!r.v.count("class")) return "non leggibile"; if (r.kind == "scene") return "fermo"; int k = int(r.v.at("class")); return k == 0 ? "reale" : (k == 2 ? "interpolato (meta)" : (k == 3 ? "interpolato (terzo)" : "**fuori traiettoria**")); };
      auto reg = [&](const Row& r, const char* g) -> std::string { std::string p = std::string("mk_") + g; char b[96];
        if (num(r, p + "_n", 0) > 0) snprintf(b, sizeof(b), "%.0f/%.0f: %.2f / %.2f", num(r, p + "_n", 0), num(r, p + "_exp", 0), num(r, p + "_med", 0), num(r, p + "_max", 0)); else snprintf(b, sizeof(b), "%.0f/%.0f", num(r, p + "_n", 0), num(r, p + "_exp", 0)); return b; };
      double framePx = 0, fgSeen = 0; for (auto* r : sr) if (r->v.count("frame_px")) { framePx = r->v.at("frame_px"); fgSeen = num(*r, "fg", 0); break; }
      fprintf(f, "## Movimento con la scena: marcatori, immagine per immagine\n\nScena non periodica con marcatori (`--motion-pattern scene`). Il contenuto si sposta di %.1f px del focus a ogni frame inviato. Generazione di frame dichiarata: %s.\n\n", framePx, fgSeen >= 1 ? (F(fgSeen) + "X").c_str() : "non dichiarata");
      fprintf(f, "| Caso | Buffer | Tempo (ms) | Ultimo frame inviato | Fase letta | Ritardo (frame) | Tipo | Errore sulla traiettoria (px del focus) | Periferia: trovati/attesi: mediana / massimo | Fascia della sfumatura | Interno del focus | Doppi | Allungamento | Sinistro contro destro (frame) |\n|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
      for (auto* r : sr) {
        if (!r->v.count("class")) { fprintf(f, "| %s | | | | | | non leggibile | | | | | | | |\n", r->id.c_str()); continue; }
        bool still = r->kind == "scene";
        fprintf(f, "| %s | %s | %s | %s | %.2f | %s | %s | %.2f | %s | %s | %s | %.0f | %.3f | %s |\n", r->id.c_str(), still ? "-" : F(num(*r, "buf", 0)).c_str(), still ? "-" : F(floor(num(*r, "t_ms", 0) * 10 + 0.5) / 10).c_str(), still ? "-" : F(num(*r, "submitted", 0)).c_str(),
                num(*r, "phase", 0), still ? "-" : F(floor(num(*r, "lag", 0) * 100 + 0.5) / 100).c_str(), typeName(*r).c_str(), num(*r, "traj_err", 0), reg(*r, "per").c_str(), reg(*r, "band").c_str(), reg(*r, "foc").c_str(), num(*r, "mk_dup", 0), num(*r, "mk_elong", 0),
                r->v.count("phase_lr") ? F(floor(r->v.at("phase_lr") * 100 + 0.5) / 100).c_str() : "-");
      }
      fprintf(f, "\nErrore sulla traiettoria = distanza della fase letta (mediana dei marcatori) dal posto nominale piu vicino: frame intero, meta a 2X, terzi a 3X; oltre 0,12 frame l'immagine e fuori traiettoria. Per regione: marcatori trovati su quelli attesi, poi mediana e massimo della distanza di ciascun marcatore dal suo posto nominale, in px del focus. Doppi = dischi in piu attorno a un marcatore gia trovato. Allungamento = estensione del disco lungo il moto diviso quella di traverso (circa 1 per un disco pulito; cresce se il disco e sdoppiato o strisciato).\n\n");
      fprintf(f, "## Movimento con la scena: giunzione e qualita dell'immagine\n\n| Caso | Tipo | Giunzione: marcatori, mediana / massimo (px del focus) | Giunzione x, y | Fusione nella fascia | Errore medio fuori dalla fascia (codici a 8 bit) | PSNR (dB) | Errore medio: periferia / interno del focus / fascia | PSNR della fascia | Riferimento: caso, spostamento (frame) |\n|---|---|---|---|---|---|---|---|---|---|\n");
      for (auto* r : sr) { if (!r->v.count("class")) continue;
        std::string seam = "0", sxy = "-", q = "-", qp = "-", qr = "-", qb = "-", ref = "-"; char b[96];
        if (num(*r, "seam_n", 0) > 0) { snprintf(b, sizeof(b), "%.0f: %.2f / %.2f", num(*r, "seam_n", 0), num(*r, "seam_med", 0), num(*r, "seam_max", 0)); seam = b; snprintf(b, sizeof(b), "%+.2f, %+.2f", num(*r, "seam_dx", 0), num(*r, "seam_dy", 0)); sxy = b; }
        if (r->v.count("q_mae")) { snprintf(b, sizeof(b), "%.2f", r->v.at("q_mae")); q = b; snprintf(b, sizeof(b), "%.1f", num(*r, "q_psnr", 0)); qp = b; snprintf(b, sizeof(b), "%.2f / %.2f / %.2f", num(*r, "q_mae_per", 0), num(*r, "q_mae_foc", 0), num(*r, "q_mae_band", 0)); qr = b;
          snprintf(b, sizeof(b), "%.1f", num(*r, "q_psnr_band", 0)); qb = b; snprintf(b, sizeof(b), "case%03.0f, %+.2f", num(*r, "q_ref", 0), num(*r, "q_shift", 0)); ref = b; }
        fprintf(f, "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n", r->id.c_str(), typeName(*r).c_str(), seam.c_str(), sxy.c_str(), num(*r, "blend", 0) ? "valori codificati" : "luce lineare", q.c_str(), qp.c_str(), qr.c_str(), qb.c_str(), ref.c_str()); }
      fprintf(f, "\nGiunzione = nella fascia ogni marcatore e rosso dal layer del focus e blu dal layer di periferia: e lo scarto fra i due dischi (rosso meno blu; x verso destra, y verso l'alto), sui marcatori dove la sfumatura mostra tutti e due (alfa fra 0,10 e 0,90). Qualita = l'immagine contro un'immagine reale della stessa prova spostata della differenza di fase misurata: errore assoluto medio in codici a 8 bit e PSNR su R, G, B; la fascia e contata a parte. Le righe dei frame reali (reale contro reale) sono il fondo del metodo.\n\n");
      const char* gname[3] = {"Reali (compreso il fermo)", "Interpolati", "Fuori traiettoria"}; int unread = 0; for (auto* r : sr) if (group(*r) < 0) ++unread;
      fprintf(f, "| Gruppo | Immagini | Marcatori trovati/attesi | Periferia: mediana / massimo | Fascia: mediana / massimo | Interno del focus: mediana / massimo | Giunzione: mediana / massimo | Errore medio fuori dalla fascia | Errore medio nella fascia | PSNR |\n|---|---|---|---|---|---|---|---|---|---|\n");
      for (int g = 0; g < 3; ++g) { int n = 0; double found = 0, expd = 0, mx[3] = {0, 0, 0}, seamMax = 0; std::vector<double> med[3], seam, mae, maeBand, psnr;
        for (auto* r : sr) { if (group(*r) != g) continue; ++n; found += num(*r, "mk_n", 0); expd += num(*r, "mk_exp", 0);
          for (int k = 0; k < 3; ++k) { std::string p = std::string("mk_") + kRegionKey[k]; if (r->v.count(p + "_med")) { med[k].push_back(r->v.at(p + "_med")); mx[k] = std::max(mx[k], r->v.at(p + "_max")); } }
          if (r->v.count("seam_med")) { seam.push_back(r->v.at("seam_med")); seamMax = std::max(seamMax, r->v.at("seam_max")); }
          if (r->v.count("q_mae")) { mae.push_back(r->v.at("q_mae")); psnr.push_back(r->v.at("q_psnr")); } if (r->v.count("q_mae_band")) maeBand.push_back(r->v.at("q_mae_band")); }
        if (!n) { fprintf(f, "| %s | 0 | | | | | | | | |\n", gname[g]); continue; }
        auto mm = [&](std::vector<double>& v, double m) -> std::string { if (v.empty()) return "-"; char b[64]; snprintf(b, sizeof(b), "%.2f / %.2f", Median(v), m); return b; };
        auto md = [&](std::vector<double>& v, const char* fmt) -> std::string { if (v.empty()) return "-"; char b[64]; snprintf(b, sizeof(b), fmt, Median(v)); return b; };
        fprintf(f, "| %s | %d | %.0f/%.0f | %s | %s | %s | %s | %s | %s | %s |\n", gname[g], n, found, expd, mm(med[0], mx[0]).c_str(), mm(med[1], mx[1]).c_str(), mm(med[2], mx[2]).c_str(), mm(seam, seamMax).c_str(), md(mae, "%.2f").c_str(), md(maeBand, "%.2f").c_str(), md(psnr, "%.1f").c_str()); }
      fprintf(f, "\nMediane sulle immagini del gruppo (delle mediane per immagine), massimi su tutte.%s Per confrontare \"composto\" e \"due layer\" si leggono le righe degli interpolati delle due prove una accanto all'altra, ciascuna contro la propria riga dei reali.\n\n", unread ? " Alcune immagini non sono leggibili: vedere in fondo." : "");
      int interp = 0; for (auto* r : sr) if (group(*r) == 1) ++interp;
      if (!interp) fprintf(f, "**Nessuna immagine interpolata riconosciuta** in questa prova: o la generazione di frame non era attiva, o lo specchio e caduto solo sui frame reali, o le immagini generate sono finite fuori traiettoria.\n\n");
    } }

  fprintf(f, "## Carico del compositore e memoria\n\n| Configurazione | fps dell'app | Frame persi/s | Scartati/s | GPU del sistema | Secondi letti |\n|---|---|---|---|---|---|\n");
  Load loads[2]; const char* tags[2] = {"reference", "two_layer"}; const char* labels[2] = {"Riferimento: un layer 5424x5356", "Due layer 1040x1028 + 2028x2004"};
  for (int i = 0; i < 2; ++i) { Case t; if (!LoadCase(dir + "\\timing_" + tags[i] + ".txt", t)) continue; loads[i] = ServerLoad(dir + "\\pvr_srv_log.txt", int(Num(run, "pid")), Str(t, "begin"), Str(t, "end"));
    if (loads[i].n) fprintf(f, "| %s | %.1f | %.2f | %.2f | %.0f %% | %d |\n", labels[i], loads[i].fpsA, loads[i].missed, loads[i].discard, loads[i].gpu, loads[i].n); else fprintf(f, "| %s | log del server non letto | | | | 0 |\n", labels[i]); }
  bool loadOk = !(loads[0].n && loads[1].n) || (loads[1].missed <= loads[0].missed + 0.2 && loads[1].discard <= loads[0].discard + 0.2);
  fprintf(f, "\nIl riferimento comprende la copia di due immagini da 116 MB per frame fatta dalla sonda stessa. La VRAM non e misurata in questa versione: gli swapchain appartengono al processo del compositore.\n\n");

  fprintf(f, "## Esito\n\n");
  if (exitCode == 2 || exitCode == 3 || cases.empty()) fprintf(f, "**NON GIUDICABILE.**\n");
  else if (!anyPass) fprintf(f, "**NO-GO per le varianti provate**: nessuna supera insieme posizione, corona e alfa. Vedere i casi dubbi.\n");
  else fprintf(f, "**GO condizionato**: almeno una variante supera posizione, corona e alfa%s%s%s. Resta la conferma dell'utente nel visore.\n", reprojSeen ? (reprojOk ? "; riproiezione coerente" : "; **riproiezione NON coerente**") : "; riproiezione non misurata", edgeSeen ? (edgeOk ? "; nitidezza in scala pari al riferimento" : "; nitidezza in scala inferiore al riferimento") : "", loadOk ? "" : "; **piu frame persi del riferimento**");
  fprintf(f, "\n### Casi dubbi o oltre soglia\n\n");
  if (doubts.empty()) fprintf(f, "Nessuno.\n"); else for (auto& d : doubts) fprintf(f, "- `%s.png`: %s\n", d.substr(0, d.find(' ')).c_str(), d.c_str());
  fprintf(f, "\n### Che cosa questa prova non ha potuto giudicare\n\n");
  for (auto& r : g_rows) if (!r.note.empty()) fprintf(f, "- `%s` (%s %s): %s\n", r.id.c_str(), r.kind.c_str(), r.tag.c_str(), r.note.c_str());
  if (loads[0].n == 0) fprintf(f, "- Carico del compositore: log del server Pimax non letto.\n");
  fprintf(f, "- Memoria video: non misurata.\n- Tutto cio che sta dopo lo specchio: lenti e pannello. La nitidezza e misurata solo su modello in scala.\n- Movimento reale della testa, comfort e visione stereo.\n- Soglie della corona non ancora tarate con l'occhio dell'utente.\n");
  fclose(f);
  printf("report written: %s\\report.md\n", dir.c_str());
  if (selftest) return SelfTestCheck() ? 1 : 0;
  return 0;
}
